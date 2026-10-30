#include "pch.h"
#include "core/SessionRunner.h"

#include <algorithm>
#include <chrono>

#include "content/ContentLibrary.h"
#include "data/EventLog.h"
#include "data/StateJson.h"
#include "scenario/ScenarioLoader.h"

namespace
{
	constexpr double kTicksPerSecond = 120.0;
	constexpr double kAutoAdvanceSeconds = 1.6;   // the result flash, then the next rep
	constexpr double kMinSecondsPerExercise = 60.0;
}

SessionRunner::SessionRunner(ScenarioPlayer& player, const ContentLibrary& content, EventLog& log)
	: player_(player), content_(content), log_(log)
{
}

double SessionRunner::elapsedSeconds() const
{
	return (lastTick_ - startTick_) / kTicksPerSecond;
}

ExerciseStats& SessionRunner::statsFor(const std::string& id)
{
	auto it = stats_.find(id);
	if (it == stats_.end())
	{
		ExerciseStats s;
		s.id = id;
		const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
		s.name = item ? item->name : id;
		it = stats_.emplace(id, s).first;
		statsOrder_.push_back(id);
	}
	return it->second;
}

bool SessionRunner::start(const SessionSettings& settings, std::string& error)
{
	if (active_)
		finish("stopped");

	const auto* session = content_.find(ContentLibrary::Kind::Session, settings.sessionId);
	if (!session)
	{
		error = "no session '" + settings.sessionId + "'";
		return false;
	}

	settings_ = settings;
	if (settings_.difficulty == "adaptive")
		settings_.difficulty = "medium"; // adaptive tiers come later
	name_ = session->name;
	order_ = session->data.value("order", "shuffle");
	repsPerExercise_ = session->data.value("repsPerExercise", 0);
	endAfterList_ = session->data.value("endAfterList", false);

	slots_.clear();
	nlohmann::ordered_json skipped = nlohmann::ordered_json::array();
	for (const auto& ex : session->data["exercises"])
	{
		if (!ex.value("enabled", true))
			continue;
		const std::string ref = ex["ref"].get<std::string>();
		if (ex.contains("difficulties")
			&& std::find(ex["difficulties"].begin(), ex["difficulties"].end(), settings_.difficulty) == ex["difficulties"].end())
		{
			skipped.push_back({{"ref", ref}, {"reason", "not at " + settings_.difficulty}});
			continue;
		}
		// Scenarios that need a later phase (objects, puppet behaviours, detectors) are free setups only.
		std::vector<const ContentLibrary::Item*> matches;
		for (const auto* m : content_.matchScenarios(ref))
		{
			const std::string pending = scenario::pendingFeature(m->data);
			if (pending.empty())
				matches.push_back(m);
			else
				skipped.push_back({{"ref", m->id}, {"reason", "needs " + pending}});
		}
		if (ref.rfind("mode:", 0) == 0 || matches.empty())
		{
			skipped.push_back({{"ref", ref}, {"reason", "nothing playable"}});
			continue;
		}
		const double weight = ex.value("weight", 1.0) / static_cast<double>(matches.size());
		for (const auto* m : matches)
			slots_.push_back({m->id, weight, ex.value("reps", 0), ex.value("difficulty", "")});
	}
	if (slots_.empty())
	{
		error = "session '" + settings.sessionId + "' has no playable exercises";
		return false;
	}

	rng_ = Rng(static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
	sequence_.clear();
	for (int i = 0; i < static_cast<int>(slots_.size()); ++i)
		sequence_.push_back(i);
	if (order_ == "shuffle")
	{
		for (int i = static_cast<int>(sequence_.size()) - 1; i > 0; --i)
			std::swap(sequence_[i], sequence_[static_cast<int>(rng_.next01() * (i + 1))]);
	}
	sequencePos_ = 0;

	stats_.clear();
	statsOrder_.clear();
	summary_.reset();
	startTick_ = lastTick_;
	active_ = true;

	if (settings_.airRoll == "left")
		player_.setMirrorPolicy(false);
	else if (settings_.airRoll == "right")
		player_.setMirrorPolicy(true);
	else
		player_.setMirrorPolicy(std::nullopt);

	nlohmann::ordered_json playlist = nlohmann::ordered_json::array();
	for (const auto& s : slots_)
		playlist.push_back({{"scenario", s.scenarioId}, {"weight", statejson::round2(static_cast<float>(s.weight))}});
	log_.write("session_start", {
		{"session", settings_.sessionId},
		{"name", name_},
		{"minutes", settings_.minutes},
		{"difficulty", settings_.difficulty},
		{"airRoll", settings_.airRoll},
		{"order", order_},
		{"repsPerExercise", repsPerExercise_},
		{"endAfterList", endAfterList_},
		{"playlist", playlist},
		{"skippedRefs", skipped},
	});
	LOG("Free Touch: session {} ({:.0f} min, {}, air roll {})", name_, settings_.minutes, settings_.difficulty,
		settings_.airRoll);

	playSlot(pickNextSlot());
	if (!player_.status().active)
	{
		error = "could not start the first exercise (are you in freeplay?)";
		finish("stopped");
		return false;
	}
	return true;
}

int SessionRunner::pickNextSlot()
{
	const int n = static_cast<int>(slots_.size());
	if (endAfterList_)
		return sequencePos_ < n ? sequence_[sequencePos_++] : -1; // once through, then done
	if (n == 1)
		return 0;
	if (order_ == "weighted" || order_ == "weak_spots") // weak_spots: weighted until per-exercise history exists
	{
		double total = 0.0;
		for (const auto& s : slots_)
			total += s.weight;
		for (int attempt = 0; attempt < 6; ++attempt)
		{
			double r = rng_.next01() * total;
			int pick = n - 1;
			for (int i = 0; i < n; ++i)
			{
				r -= slots_[i].weight;
				if (r <= 0.0)
				{
					pick = i;
					break;
				}
			}
			if (pick != current_)
				return pick;
		}
		return (current_ + 1) % n;
	}
	if (sequencePos_ >= n)
	{
		sequencePos_ = 0;
		if (order_ == "shuffle")
		{
			for (int i = n - 1; i > 0; --i)
				std::swap(sequence_[i], sequence_[static_cast<int>(rng_.next01() * (i + 1))]);
		}
	}
	return sequence_[sequencePos_++];
}

void SessionRunner::playSlot(int index)
{
	current_ = index;
	repsOnCurrent_ = 0;
	currentJudged_ = false;
	autoAdvanceTick_ = 0;
	exerciseStartTick_ = lastTick_;
	const std::string& id = slots_[index].scenarioId;
	const std::string difficulty = slots_[index].difficulty.empty() ? settings_.difficulty : slots_[index].difficulty;
	statsFor(id);
	log_.write("session_exercise", {{"session", settings_.sessionId}, {"scenario", id}, {"difficulty", difficulty},
		{"elapsed", statejson::round2(static_cast<float>(elapsedSeconds()))}});
	player_.play(id, difficulty);
}

void SessionRunner::advance(bool keepCar)
{
	autoAdvanceTick_ = 0;
	currentJudged_ = false;
	if (elapsedSeconds() >= settings_.minutes * 60.0)
	{
		finish("time");
		return;
	}
	const int repsHere = current_ >= 0 && slots_[current_].reps > 0 ? slots_[current_].reps : repsPerExercise_;
	const bool rotate = (slots_.size() > 1 || endAfterList_) && (repsHere > 0
		? repsOnCurrent_ >= repsHere
		: (lastTick_ - exerciseStartTick_) / kTicksPerSecond
			>= std::max(kMinSecondsPerExercise, settings_.minutes * 60.0 / slots_.size()));
	if (!rotate)
	{
		player_.next(keepCar);
		return;
	}
	const int next = pickNextSlot();
	if (next < 0)
		finish("done");
	else
		playSlot(next);
}

void SessionRunner::next()
{
	if (!active_)
		return;
	if (!currentJudged_ && current_ >= 0)
	{
		ExerciseStats& st = statsFor(slots_[current_].scenarioId);
		++st.reps;
		++st.skipped;
		++repsOnCurrent_;
	}
	advance();
}

void SessionRunner::onResult(const ScenarioPlayer::Status& s)
{
	if (!active_ || current_ < 0)
		return;
	ExerciseStats& st = statsFor(s.scenarioId);
	++st.reps;
	if (s.outcome > 0)
	{
		++st.successes;
		++st.streak;
		st.bestStreak = std::max(st.bestStreak, st.streak);
		st.fastestKmh = std::max(st.fastestKmh, s.resultKmh);
	}
	else if (s.outcome < 0)
	{
		++st.fails;
		st.streak = 0;
	}
	else
	{
		++st.neutrals;
	}
	++repsOnCurrent_;
	currentJudged_ = true;
	judgedRep_ = s.rep;
	// Self-advancing drills (onSuccess) ask through the advance request instead of waiting for the flash.
	autoAdvanceTick_ = s.selfAdvancing ? 0 : lastTick_ + static_cast<uint64_t>(kAutoAdvanceSeconds * kTicksPerSecond);
}

void SessionRunner::onTick(const Snapshot& snap)
{
	lastTick_ = snap.tick;
	if (!active_)
		return;
	if (elapsedSeconds() >= settings_.minutes * 60.0)
	{
		finish("time");
		return;
	}
	if (autoAdvanceTick_ != 0)
	{
		// The player started another rep by hand (retry / previous): do not jump ahead of them.
		if (player_.status().rep != judgedRep_)
		{
			autoAdvanceTick_ = 0;
			currentJudged_ = false;
		}
		else if (lastTick_ >= autoAdvanceTick_)
		{
			advance();
		}
	}
}

void SessionRunner::onPlayerStopped()
{
	if (active_ && !finishing_)
		finish("stopped");
}

void SessionRunner::finish(const std::string& reason)
{
	if (!active_ || finishing_)
		return;
	finishing_ = true;
	active_ = false;
	autoAdvanceTick_ = 0;

	auto summary = std::make_shared<SessionSummary>();
	summary->sessionId = settings_.sessionId;
	summary->name = name_;
	summary->difficulty = settings_.difficulty;
	summary->airRoll = settings_.airRoll;
	summary->endReason = reason;
	summary->plannedSeconds = settings_.minutes * 60.0;
	summary->playedSeconds = std::min(elapsedSeconds(), summary->plannedSeconds);
	nlohmann::ordered_json rows = nlohmann::ordered_json::array();
	for (const auto& id : statsOrder_)
	{
		const ExerciseStats& st = stats_.at(id);
		summary->exercises.push_back(st);
		summary->reps += st.reps;
		summary->successes += st.successes;
		summary->fails += st.fails;
		summary->bestStreak = std::max(summary->bestStreak, st.bestStreak);
		summary->fastestKmh = std::max(summary->fastestKmh, st.fastestKmh);
		rows.push_back({{"scenario", id}, {"reps", st.reps}, {"successes", st.successes}, {"fails", st.fails},
			{"neutral", st.neutrals}, {"skipped", st.skipped}, {"bestStreak", st.bestStreak},
			{"fastestKmh", statejson::round2(st.fastestKmh)}});
	}
	summary_ = summary;

	player_.setMirrorPolicy(std::nullopt);
	player_.stop();

	log_.write("session_end", {
		{"session", settings_.sessionId},
		{"reason", reason},
		{"plannedSeconds", summary->plannedSeconds},
		{"playedSeconds", statejson::round2(static_cast<float>(summary->playedSeconds))},
		{"reps", summary->reps},
		{"successes", summary->successes},
		{"fails", summary->fails},
		{"bestStreak", summary->bestStreak},
		{"fastestKmh", statejson::round2(summary->fastestKmh)},
		{"exercises", rows},
	});
	LOG("Free Touch: session {} ended ({}): {} reps, {} successes", name_, reason, summary->reps, summary->successes);
	finishing_ = false;
	if (finished_)
		finished_(*summary);
}

SessionRunner::Status SessionRunner::status() const
{
	Status s;
	if (!active_)
		return s;
	s.active = true;
	s.name = name_;
	s.secondsLeft = std::max(0.0, settings_.minutes * 60.0 - elapsedSeconds());
	s.exerciseCount = static_cast<int>(slots_.size());
	if (current_ >= 0)
	{
		const auto it = std::find(sequence_.begin(), sequence_.end(), current_);
		s.exerciseIndex = static_cast<int>(it - sequence_.begin()) + 1;
	}
	return s;
}
