#include "pch.h"
#include "core/DailyRunner.h"

#include <algorithm>
#include <cctype>

#include "data/EventLog.h"

namespace
{
	constexpr uint64_t kPauseTicks = 300; // 2.5 s to see a shot's result before the next one
}

const daily::Challenge& DailyRunner::challenge(const std::string& today)
{
	if (challengeDate_ != today)
	{
		challenge_ = daily::make(pool_, today);
		challengeDate_ = today;
	}
	return challenge_;
}

bool DailyRunner::start(const std::string& today, std::string& error)
{
	if (active_)
	{
		error = "already running";
		return false;
	}
	if (pool_.slots.empty())
	{
		error = "no daily challenge loaded (content/daily/daily.json)";
		return false;
	}
	run_ = daily::make(pool_, today);
	if (run_.number < 1)
	{
		error = "the Daily Challenge starts on " + pool_.epoch;
		return false;
	}
	runDate_ = today;
	daily::Attempt& a = attempt();
	a.date = today;
	a.number = run_.number;
	if (a.done || a.results.size() >= run_.shots.size())
	{
		error = "today's challenge is done";
		return false;
	}
	shot_ = a.results.size();
	active_ = true;
	log_.write("daily_start", {{"number", run_.number}, {"date", today}, {"shot", shot_ + 1}, {"continued", shot_ > 0}});
	playShot();
	return active_;
}

void DailyRunner::playShot()
{
	const daily::Shot& s = run_.shots[shot_];
	// Every player's reps: all of the scenario's choices, the mirror from the seed.
	player_.setIgnoreChoiceFilters(true);
	player_.setForceReadFirst(true); // frozen until a press: read the shot first, the clock starts then
	player_.setPackStyleTargets(true); // target nets as the packs' grey tiles with the lit opening
	player_.setMirrorPolicy(std::nullopt);
	awaitingStart_ = true;
	shotDone_ = false;
	shotRep_ = -1;
	nextAtTick_ = 0;
	log_.write("daily_shot_start", {{"number", run_.number}, {"shot", shot_ + 1}, {"scenario", s.scenarioId}, {"seed", s.seed},
		{"difficulty", s.difficulty}});
	if (!player_.play(s.scenarioId, s.difficulty, s.seed))
		stop("could not start " + s.scenarioId);
	if (changed_)
		changed_();
}

void DailyRunner::record(const daily::ShotResult& r, const std::string& why)
{
	daily::Attempt& a = attempt();
	a.results.push_back(r);
	shotDone_ = true;
	nextAtTick_ = lastTick_ + kPauseTicks;
	log_.write("daily_shot", {{"number", run_.number}, {"shot", shot_ + 1}, {"scenario", run_.shots[shot_].scenarioId},
		{"mark", static_cast<int>(r.mark)}, {"points", r.points}, {"seconds", r.seconds}, {"label", r.label}, {"why", why},
		{"total", a.points()}});
	if (changed_)
		changed_();
}

void DailyRunner::onTick(uint64_t tick, const ScenarioPlayer::Status& status)
{
	lastTick_ = tick;
	if (!active_)
		return;
	const daily::Shot& s = run_.shots[shot_];
	if (awaitingStart_)
	{
		// The shot's rep is on the field once the player shows its scenario and seed.
		if (status.active && status.scenarioId == s.scenarioId && status.seed == s.seed)
		{
			awaitingStart_ = false;
			shotRep_ = status.rep;
		}
		return;
	}
	if (!shotDone_)
	{
		// A retry or a skip starts another rep before this one had its result: one rep per shot, so it is a miss.
		if (status.active && status.rep != shotRep_)
			record(daily::score(false, status.repSeconds, status.timeLimitSeconds, "SKIPPED"), "another rep started");
		return;
	}
	if (status.goalReplay)
	{
		// A real goal's replay is playing: the next shot (or the end of the daily, which opens the menu) waits until a
		// second after it is over.
		if (nextAtTick_ != 0)
			nextAtTick_ = tick + 120;
		return;
	}
	if (nextAtTick_ != 0 && tick >= nextAtTick_)
	{
		nextAtTick_ = 0;
		++shot_;
		if (shot_ < run_.shots.size())
		{
			playShot();
			return;
		}
		// Done: the attempt is final.
		daily::Attempt& a = attempt();
		a.done = true;
		active_ = false;
		player_.setIgnoreChoiceFilters(false);
		player_.setForceReadFirst(false);
		player_.setPackStyleTargets(false);
		log_.write("daily_done", {{"number", run_.number}, {"date", runDate_}, {"hits", a.hits()}, {"points", a.points()}});
		if (changed_)
			changed_();
		if (finished_)
			finished_(a);
		player_.stop();
	}
}

void DailyRunner::onResult(const ScenarioPlayer::Status& status)
{
	if (!active_ || awaitingStart_ || shotDone_)
		return;
	if (status.rep != shotRep_ || status.scenarioId != run_.shots[shot_].scenarioId)
		return;
	record(daily::score(status.outcome > 0, status.repSeconds, status.timeLimitSeconds, status.resultLabel), "judged");
}

void DailyRunner::stop(const std::string& reason)
{
	if (!active_)
		return;
	// Stopping during a shot that had no result yet costs that shot; between shots it costs nothing.
	if (!awaitingStart_ && !shotDone_)
		record(daily::score(false, 0.0, 0.0, "STOPPED"), reason);
	active_ = false;
	nextAtTick_ = 0;
	player_.setIgnoreChoiceFilters(false);
	player_.setForceReadFirst(false);
	player_.setPackStyleTargets(false);
	daily::Attempt& a = attempt();
	if (a.results.size() >= run_.shots.size())
	{
		// Every shot has its result (stopped in the pause after the last one): the attempt is finished.
		a.done = true;
		log_.write("daily_done", {{"number", run_.number}, {"date", runDate_}, {"hits", a.hits()}, {"points", a.points()}});
		if (changed_)
			changed_();
		if (finished_)
			finished_(a);
		return;
	}
	log_.write("daily_paused", {{"number", run_.number}, {"shotsPlayed", a.results.size()}, {"reason", reason}});
	if (changed_)
		changed_();
}

void DailyRunner::onPlayerStopped()
{
	if (active_)
		stop("exercise stopped");
}

SessionRunner::Status DailyRunner::hudStatus() const
{
	SessionRunner::Status s;
	if (!active_)
		return s;
	s.active = true;
	s.name = "Daily #" + std::to_string(run_.number);
	const auto it = attempts_.find(runDate_);
	const int points = it == attempts_.end() ? 0 : it->second.points();
	char line[160];
	snprintf(line, sizeof(line), "DAILY #%d   |   SHOT %d / %d   |   %s PTS", run_.number,
		static_cast<int>(std::min(shot_ + 1, run_.shots.size())), static_cast<int>(run_.shots.size()), daily::groupThousands(points).c_str());
	s.line = line;
	for (size_t i = 0; i < run_.shots.size(); ++i)
		s.marks.push_back(it != attempts_.end() && i < it->second.results.size() ? static_cast<int>(it->second.results[i].mark) : 0);
	s.exerciseIndex = static_cast<int>(shot_ + 1);
	s.exerciseCount = static_cast<int>(run_.shots.size());
	return s;
}

bool DailyRunner::lockFromService(const std::string& date, int number, const std::string& marks, int points)
{
	daily::Attempt& a = attempts_[date];
	if (a.done)
		return false;
	// The posted attempt replaces whatever is here: its squares, its points (spread over its hits, the rest on the first).
	a.date = date;
	a.number = number;
	a.results.clear();
	int hits = 0;
	for (const char c : marks)
		hits += c == '1' || c == '2';
	int left = points;
	for (const char c : marks)
	{
		daily::ShotResult r;
		r.mark = static_cast<daily::Mark>(std::clamp(c - '0', 0, 3));
		const bool hit = r.mark == daily::Mark::Fast || r.mark == daily::Mark::Hit;
		r.points = hit && hits > 0 ? points / hits : 0;
		r.label = hit ? "HIT" : "MISS";
		left -= r.points;
		a.results.push_back(r);
	}
	for (auto& r : a.results)
		if (r.points > 0)
		{
			r.points += left;
			break;
		}
	a.done = true;
	const bool wasRunning = active_ && runDate_ == date;
	log_.write("daily_locked", {{"date", date}, {"number", number}, {"points", points}, {"marks", marks}, {"stoppedRun", wasRunning}});
	if (wasRunning)
	{
		active_ = false;
		nextAtTick_ = 0;
		player_.setIgnoreChoiceFilters(false);
		player_.setForceReadFirst(false);
		player_.setPackStyleTargets(false);
		player_.stop();
	}
	if (changed_)
		changed_();
	return true;
}

void DailyRunner::loadAttempts(const nlohmann::json& doc)
{
	attempts_.clear();
	for (const auto& [date, a] : doc.value("days", nlohmann::json::object()).items())
		attempts_[date] = daily::attemptFromJson(date, a);
}

nlohmann::json DailyRunner::attemptsJson() const
{
	nlohmann::json days = nlohmann::json::object();
	for (const auto& [date, a] : attempts_)
		if (!a.results.empty() || a.done)
			days[date] = daily::toJson(a);
	return {{"version", 1}, {"days", days}};
}
