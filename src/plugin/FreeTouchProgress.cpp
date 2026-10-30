#include "pch.h"
#include "plugin/FreeTouch.h"

#include <ctime>
#include <fstream>
#include <set>

// The Progress page's data (MMR over time, records, milestones, the journal) and replay takeover (Phase 6).
// Game thread unless noted. Only src/game/ calls the SDK.

namespace
{
	std::string nowText(const char* format)
	{
		const std::time_t t = std::time(nullptr);
		std::tm tm{};
		localtime_s(&tm, &t);
		char buf[64];
		std::strftime(buf, sizeof(buf), format, &tm);
		return buf;
	}

	struct PlaylistDef
	{
		const char* key;
		const char* label;
		int id;
	};
	const PlaylistDef kPlaylists[] = {{"1v1", "Duel", 10}, {"2v2", "Doubles", 11}, {"3v3", "Standard", 13}};

	// Categories that are not training (no trends, no weak spots).
	bool notTraining(const std::string& category)
	{
		return category == "captured" || category == "dev" || category == "replay" || category.empty();
	}

	std::string pretty(std::string id)
	{
		std::replace(id.begin(), id.end(), '_', ' ');
		if (!id.empty())
			id[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(id[0])));
		return id;
	}
}

// ---------------------------------------------------------------- storage

std::filesystem::path FreeTouch::progressFile()
{
	return game_->dataFolder() / "freetouch" / "settings" / "progress.json";
}

void FreeTouch::loadProgress()
{
	std::ifstream in(progressFile());
	if (!in)
		return;
	try
	{
		progress_.fromJson(nlohmann::json::parse(in));
	}
	catch (const std::exception&)
	{
		LOG("Free Touch: settings/progress.json could not be read; starting a new one");
	}
}

void FreeTouch::saveProgress()
{
	if (!progress_.dirty())
		return;
	std::error_code ec;
	std::filesystem::create_directories(progressFile().parent_path(), ec);
	const auto tmp = std::filesystem::path(progressFile().string() + ".tmp");
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << progress_.toJson().dump(1);
		if (!out)
			return;
	}
	std::filesystem::rename(tmp, progressFile(), ec);
	if (!ec)
		progress_.clearDirty();
}

// ---------------------------------------------------------------- MMR and counters

void FreeTouch::readMmrNow(const char* why, bool quiet)
{
	// From the game's own cached ratings (no network call by the plugin): one sample per playlist per day.
	const std::string day = today_.empty() ? nowText("%Y-%m-%d") : today_;
	std::vector<std::pair<const char*, MmrReading>> reads;
	nlohmann::json log = nlohmann::json::object();
	for (const auto& p : kPlaylists)
	{
		const MmrReading r = game_->readMmr(p.id);
		reads.emplace_back(p.key, r);
		if (r.synced && r.mmr > 0.f)
			log[p.key] = {{"mmr", std::round(r.mmr)}, {"tier", r.tier}, {"division", r.division}};
	}
	if (quiet && (log == lastMmrRead_ || log.empty()))
		return;
	lastMmrRead_ = log;
	for (const auto& [key, r] : reads)
	{
		mmrSynced_[key] = r.synced && r.mmr > 0.f;
		if (r.synced && r.mmr > 0.f)
			progress_.recordMmr(key, day, r.mmr, r.tier, r.division, r.matches, false);
	}
	eventLog_.write("mmr_read", {{"why", why}, {"day", day}, {"playlists", log}});
	saveProgress();
	refreshProgress(true);
}

void FreeTouch::pollMmr()
{
	// The game's "MMR changed" notice did not come after Lambourne's matches (2026-10-18: the +47 in 2v2 showed only
	// after a restart), so the cached ratings are re-read every 30 s outside online matches; nothing is written unless
	// a number changed.
	if (unloading_)
		return;
	if (!game_->inOnlineGame())
		readMmrNow("changed", true);
	game_->runAfter(30.f, [this] { pollMmr(); });
}

void FreeTouch::countResult(const ScenarioPlayer::Status& s)
{
	// Lifetime counters for the milestones (results only; mechanics are counted in onMechanics).
	if (s.scenarioId == "vs_bot.match" && s.resultLabel == "YOU WIN")
	{
		progress_.bump("match_wins");
		progress_.bump("match_wins_" + defender_);
	}
	if (s.scenarioId == "vs_bot.kickoff" && s.outcome > 0)
		progress_.bump("kickoffs_won");
	if (s.scenarioId == "vs_bot.fifty" && s.outcome > 0)
		progress_.bump("fifties_won");
	if (s.resultLabel == "LADDER CLEARED")
		progress_.bump("ladders_cleared");
	if (s.resultLabel == "IN THE ZONE")
		progress_.bump("zone_goals");
	if (s.scenarioId.rfind("saves.", 0) == 0 && s.outcome > 0)
		progress_.bump("saves");
}

// ---------------------------------------------------------------- the page's view

void FreeTouch::refreshProgress(bool force)
{
	const std::pair<int, int> version{history_.version(), progress_.version()};
	if (!force && version == progressBuiltFor_ && tick_ < progressBuiltTick_ + 600)
		return;
	progressBuiltFor_ = version;
	progressBuiltTick_ = tick_;
	const std::string today = today_.empty() ? nowText("%Y-%m-%d") : today_;
	const int todayNum = TrainingHistory::dayNumber(today);
	auto v = std::make_shared<ProgressView>();
	v->goalMinutes = dailyGoalMinutes_;

	// Days: the last 84 for the charts; totals and the best streak over everything.
	for (int i = 83; i >= 0; --i)
	{
		const std::string d = TrainingHistory::shiftDay(today, -i);
		const auto day = history_.day(d);
		v->days.push_back({d, static_cast<float>(day.seconds / 60.0), day.reps, day.successes});
	}
	int run = 0, lastNum = INT_MIN;
	for (const auto& [d, day] : history_.allDays())
	{
		v->totalMinutes += day.seconds / 60.0;
		v->totalReps += day.reps;
		v->totalSuccesses += day.successes;
		if (!history_.trained(d))
			continue;
		++v->daysTrained;
		const int n = TrainingHistory::dayNumber(d);
		run = n == lastNum + 1 ? run + 1 : 1;
		lastNum = n;
		v->bestStreak = std::max(v->bestStreak, run);
	}
	v->streak = history_.streak(today);
	v->bestStreak = std::max(v->bestStreak, v->streak);

	// MMR per playlist.
	for (const auto& p : kPlaylists)
	{
		ProgressView::Playlist pl;
		pl.key = p.key;
		pl.label = p.label;
		pl.synced = mmrSynced_[p.key];
		const auto& samples = progress_.mmr(p.key);
		for (const auto& sm : samples)
		{
			const int n = TrainingHistory::dayNumber(sm.day);
			if (n == INT_MIN)
				continue;
			pl.samples.push_back({n - todayNum, sm.mmr});
			pl.peak = std::max(pl.peak, sm.mmr);
		}
		if (!samples.empty())
		{
			const auto& last = samples.back();
			pl.mmr = last.mmr;
			pl.tier = last.tier;
			pl.division = last.division;
			pl.matches = last.matches;
			pl.rank = last.tier > 0 ? ProgressLog::rankName(last.tier, last.division) : "";
			// Change this week: against the latest sample from 7 or more days before the last one (else the first).
			const int lastNum = TrainingHistory::dayNumber(last.day);
			const ProgressLog::MmrSample* base = nullptr;
			for (const auto& sm : samples)
				if (TrainingHistory::dayNumber(sm.day) <= lastNum - 7)
					base = &sm;
			if (!base && samples.size() >= 2)
				base = &samples.front();
			if (base && base != &last)
			{
				pl.hasDelta = true;
				pl.delta = last.mmr - base->mmr;
			}
		}
		v->playlists.push_back(pl);
	}

	// Categories: all time, this week and last week.
	std::map<std::string, ProgressView::Category> cats;
	for (const auto& [id, e] : history_.exercises())
	{
		if (notTraining(e.category) || e.reps == 0)
			continue;
		auto& c = cats[e.category];
		c.id = e.category;
		c.reps += e.reps;
		c.wins += e.successes;
	}
	for (auto& [id, c] : cats)
	{
		for (int i = 0; i < 14; ++i)
		{
			const auto d = history_.categoryDay(TrainingHistory::shiftDay(today, -i), id);
			if (i < 7)
			{
				c.reps7 += d.reps;
				c.wins7 += d.successes;
			}
			else
			{
				c.repsPrev7 += d.reps;
				c.winsPrev7 += d.successes;
			}
		}
		v->categories.push_back(c);
	}
	std::sort(v->categories.begin(), v->categories.end(), [](const auto& a, const auto& b) {
		return a.reps7 != b.reps7 ? a.reps7 > b.reps7 : a.reps > b.reps;
	});

	// Weak spots: the lowest success among drills played 6+ times (outcomes that mean something).
	std::vector<ProgressView::WeakSpot> weak;
	const long long nowSec = static_cast<long long>(std::time(nullptr));
	for (const auto& [id, e] : history_.exercises())
	{
		if (notTraining(e.category) || e.category == "speed_lab" || e.reps < 6)
			continue;
		const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
		if (!item)
			continue;
		ProgressView::WeakSpot w;
		w.id = id;
		w.name = item->name;
		w.category = e.category;
		w.reps = e.reps;
		w.success = static_cast<float>(e.successes) / e.reps;
		const long long ago = e.lastPlayed > 0 ? (nowSec - e.lastPlayed) / 86400 : -1;
		char buf[96];
		snprintf(buf, sizeof(buf), "%d%% over %d reps%s", static_cast<int>(w.success * 100.f + 0.5f), e.reps,
			ago <= 0 ? ", played today" : ago == 1 ? ", last played yesterday" : (", " + std::to_string(ago) + " days ago").c_str());
		w.why = buf;
		if (w.success < 0.6f)
			weak.push_back(w);
	}
	std::sort(weak.begin(), weak.end(), [](const auto& a, const auto& b) { return a.success < b.success; });
	if (weak.size() > 3)
		weak.resize(3);
	v->weakSpots = weak;

	// Records: high scores, then best times.
	int horseBest = 0, rushBest = 0;
	for (const auto& [key, value] : bests_.all())
	{
		ProgressView::Record r;
		if (key.rfind("max:", 0) == 0)
		{
			// max:<mode>|<exercise>|<difficulty>
			const std::string rest = key.substr(4);
			const auto p1 = rest.find('|'), p2 = rest.rfind('|');
			const std::string mode = rest.substr(0, p1);
			const std::string diff = p2 != std::string::npos && p2 > p1 ? rest.substr(p2 + 1) : "";
			r.highScore = true;
			if (mode == "horse")
			{
				r.title = "HORSE";
				r.value = std::to_string(static_cast<int>(value)) + " shots";
				horseBest = std::max(horseBest, static_cast<int>(value));
			}
			else if (mode == "rush")
			{
				r.title = "Style Rush";
				r.value = std::to_string(static_cast<int>(value)) + " pts";
				rushBest = std::max(rushBest, static_cast<int>(value));
			}
			else
			{
				r.title = pretty(mode);
				r.value = std::to_string(static_cast<int>(value));
			}
			r.sub = "Best, " + diff;
		}
		else
		{
			// <exercise>|<difficulty>[|<objective>]
			const auto p1 = key.find('|');
			const std::string id = key.substr(0, p1);
			const std::string rest = p1 == std::string::npos ? "" : key.substr(p1 + 1);
			const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
			if (!item || notTraining(item->category))
				continue;
			r.title = item->name;
			r.exerciseId = id;
			std::string sub = rest;
			std::replace(sub.begin(), sub.end(), '|', ' ');
			r.sub = "Best time, " + sub;
			char buf[32];
			snprintf(buf, sizeof(buf), "%.2f s", value);
			r.value = buf;
		}
		v->records.push_back(r);
	}
	std::stable_sort(v->records.begin(), v->records.end(), [](const auto& a, const auto& b) { return a.highScore > b.highScore; });
	if (v->records.size() > 12)
		v->records.resize(12);

	// Mechanics landed, most first.
	for (const auto& [key, count] : progress_.counters())
		if (key.rfind("mech:", 0) == 0 && count > 0)
			v->mechanics.push_back({pretty(key.substr(5)), count});
	std::sort(v->mechanics.begin(), v->mechanics.end(), [](const auto& a, const auto& b) { return a.count > b.count; });

	// Milestones.
	std::set<std::string> catsPlayed;
	for (const auto& [id, e] : history_.exercises())
		if (!notTraining(e.category) && e.reps > 0)
			catsPlayed.insert(e.category);
	int goalDays = 0;
	for (const auto& [d, day] : history_.allDays())
		goalDays += day.seconds / 60.0 >= dailyGoalMinutes_;
	float bestClimb = 0.f;
	for (const auto& pl : v->playlists)
		if (pl.hasDelta)
			bestClimb = std::max(bestClimb, pl.delta);
	struct Def
	{
		const char* id;
		const char* title;
		const char* about;
		double value;
		double target;
		int tier;
		const char* unit;
	};
	const double hours = v->totalMinutes / 60.0;
	const Def defs[] = {
		{"reps_100", "Warmed up", "Play 100 reps", static_cast<double>(v->totalReps), 100, 1, "reps"},
		{"reps_1000", "Grinder", "Play 1,000 reps", static_cast<double>(v->totalReps), 1000, 2, "reps"},
		{"reps_5000", "No days off", "Play 5,000 reps", static_cast<double>(v->totalReps), 5000, 3, "reps"},
		{"hours_1", "First hour", "Train for an hour in total", hours, 1, 1, "h"},
		{"hours_10", "Ten hours in", "Train for 10 hours in total", hours, 10, 2, "h"},
		{"hours_50", "Dedicated", "Train for 50 hours in total", hours, 50, 3, "h"},
		{"streak_3", "On a roll", "Train 3 days in a row", static_cast<double>(v->bestStreak), 3, 1, "days"},
		{"streak_7", "Week streak", "Train 7 days in a row", static_cast<double>(v->bestStreak), 7, 2, "days"},
		{"streak_30", "Unstoppable", "Train 30 days in a row", static_cast<double>(v->bestStreak), 30, 3, "days"},
		{"goal_1", "Goal getter", "Hit your daily goal", static_cast<double>(goalDays), 1, 1, "days"},
		{"goal_20", "Consistent", "Hit your daily goal on 20 days", static_cast<double>(goalDays), 20, 3, "days"},
		{"explorer", "Explorer", "Play 8 different categories", static_cast<double>(catsPlayed.size()), 8, 2, "categories"},
		{"kickoffs_25", "Kickoff king", "Win 25 kickoffs against the bot", static_cast<double>(progress_.counter("kickoffs_won")), 25, 2, "won"},
		{"fifties_25", "Fifty-fifty", "Win 25 50s against the bot", static_cast<double>(progress_.counter("fifties_won")), 25, 2, "won"},
		{"match_win", "Beat the bot", "Win a 1v1 match against the bot", static_cast<double>(progress_.counter("match_wins")), 1, 1, "won"},
		{"match_nexto", "Nexto slayer", "Win a 1v1 match against Nexto", static_cast<double>(progress_.counter("match_wins_nexto")), 1, 3, "won"},
		{"horse_5", "Called it", "Make 5 shots in one game of HORSE", static_cast<double>(horseBest), 5, 1, "shots"},
		{"horse_10", "HORSE master", "Make 10 shots in one game of HORSE", static_cast<double>(horseBest), 10, 3, "shots"},
		{"rush_300", "Stylish", "Score 300 in Style Rush", static_cast<double>(rushBest), 300, 1, "pts"},
		{"rush_1000", "Freestyler", "Score 1,000 in Style Rush", static_cast<double>(rushBest), 1000, 3, "pts"},
		{"ladder_1", "Combo builder", "Clear a Build Your Combo ladder", static_cast<double>(progress_.counter("ladders_cleared")), 1, 2, "ladders"},
		{"zone_25", "Sniper", "Score 25 goals into the lit zone", static_cast<double>(progress_.counter("zone_goals")), 25, 2, "goals"},
		{"air_dribble_25", "Air dribbler", "Land 25 air dribbles", static_cast<double>(progress_.counter("mech:air_dribble")), 25, 2, "landed"},
		{"flip_reset_10", "Reset hunter", "Land 10 flip resets", static_cast<double>(progress_.counter("mech:flip_reset")), 10, 3, "landed"},
		{"double_tap_10", "Double trouble", "Land 10 double taps", static_cast<double>(progress_.counter("mech:double_touch")), 10, 2, "landed"},
		{"ceiling_5", "Ceiling fan", "Land 5 ceiling shots", static_cast<double>(progress_.counter("mech:ceiling_shot")), 5, 2, "landed"},
		{"saves_100", "Brick wall", "Make 100 saves", static_cast<double>(progress_.counter("saves")), 100, 2, "saves"},
		{"takeover_1", "Director's cut", "Take over a moment from a replay", static_cast<double>(progress_.counter("takeovers")), 1, 1, "moments"},
		{"climb_25", "Climbing", "Gain 25 MMR in a week in any playlist", static_cast<double>(bestClimb), 25, 2, "MMR"},
	};
	for (const auto& d : defs)
	{
		ProgressView::Milestone ms;
		ms.id = d.id;
		ms.title = d.title;
		ms.about = d.about;
		ms.tier = d.tier;
		ms.progress = static_cast<float>(std::clamp(d.value / d.target, 0.0, 1.0));
		ms.done = d.value >= d.target;
		char buf[64];
		if (std::string(d.unit) == "h")
			snprintf(buf, sizeof(buf), "%.1f / %.0f h", std::min(d.value, d.target), d.target);
		else
			snprintf(buf, sizeof(buf), "%.0f / %.0f %s", std::min(d.value, d.target), d.target, d.unit);
		ms.progressText = buf;
		ms.unlockedOn = progress_.unlockedOn(d.id);
		v->milestones.push_back(ms);
	}
	checkMilestones(*v);
	for (auto& ms : v->milestones)
		ms.unlockedOn = progress_.unlockedOn(ms.id);

	// Journal: every day with training or a routine log, newest first (60 days at most).
	std::set<std::string> days;
	for (const auto& [d, day] : history_.allDays())
		if (day.seconds >= 30.0 || day.reps > 0)
			days.insert(d);
	for (const auto& [d, log] : journal_)
		days.insert(d);
	for (auto it = days.rbegin(); it != days.rend() && v->journal.size() < 60; ++it)
	{
		ProgressView::JournalDay j;
		j.day = *it;
		const auto day = history_.day(*it);
		j.minutes = static_cast<float>(day.seconds / 60.0);
		j.reps = day.reps;
		j.wins = day.successes;
		const auto jl = journal_.find(*it);
		if (jl != journal_.end())
		{
			const routine::DayLog& log = jl->second;
			j.focus = log.focus;
			for (const auto& [step, done] : log.done)
				j.ticked += done;
			// The routine that day: its saved plan, else (logs from before plans were saved) the routine as it is now,
			// when the day has anything logged against it.
			std::vector<routine::PlanStep> plan = log.plan;
			if (plan.empty() && routine::hasEntries(log))
			{
				plan = routine::planOf(routine_);
				j.planIsCurrent = !plan.empty();
			}
			std::set<std::string> placed;
			for (const auto& p : plan)
			{
				ProgressView::JournalDay::Step st;
				st.section = p.section;
				st.title = p.title;
				st.optional = p.optional;
				const auto done = log.done.find(p.id);
				st.done = done != log.done.end() && done->second;
				const auto tally = log.tally.find(p.id);
				const int count = tally != log.tally.end() ? tally->second : 0;
				if (p.count > 0)
				{
					st.tally = std::to_string(count) + " / " + std::to_string(p.count);
					st.done = st.done || count >= p.count;
				}
				const auto note = log.notes.find(p.id);
				if (note != log.notes.end())
					st.note = note->second;
				placed.insert(p.id);
				j.steps.push_back(std::move(st));
			}
			for (const auto& [step, text] : log.notes)
				if (!text.empty() && !placed.count(step))
					j.notes.push_back(text);
		}
		for (const auto& [category, d] : history_.categoriesOn(*it))
			if (d.reps > 0 && !notTraining(category))
				j.trained.push_back({category, d.reps, d.successes});
		std::sort(j.trained.begin(), j.trained.end(), [](const auto& a, const auto& b) { return a.reps > b.reps; });
		std::string mmr;
		for (const auto& p : kPlaylists)
			for (const auto& sm : progress_.mmr(p.key))
				if (sm.day == *it)
				{
					char buf[48];
					snprintf(buf, sizeof(buf), "%s%s %.0f", mmr.empty() ? "" : "  ", p.key, sm.mmr);
					mmr += buf;
				}
		j.mmr = mmr;
		if (j.minutes < 0.5f && j.reps == 0 && j.ticked == 0 && j.notes.empty() && j.focus.empty() && j.mmr.empty())
			continue;
		v->journal.push_back(j);
	}
	progressPub_ = v;
}

void FreeTouch::checkMilestones(const ProgressView& view)
{
	// Newly reached milestones get a moment on screen (the first check after loading only records what was already
	// reached, quietly).
	const std::string today = today_.empty() ? nowText("%Y-%m-%d") : today_;
	bool any = false;
	for (const auto& ms : view.milestones)
	{
		if (!ms.done || !progress_.unlock(ms.id, today))
			continue;
		any = true;
		eventLog_.write("milestone", {{"id", ms.id}, {"title", ms.title}, {"quiet", !milestonesPrimed_}});
		if (milestonesPrimed_)
		{
			hud_.showMilestone(ms.title, ms.tier);
			playTrainingSound("new_best");
		}
	}
	milestonesPrimed_ = true;
	if (any)
		saveProgress();
}

// ---------------------------------------------------------------- replay takeover

void FreeTouch::onReplayFrame(const Snapshot& s, const ReplayInfo& info)
{
	// Every rendered frame of a replay: keep the last moments for velocity smoothing, take R3 for takeover.
	replayRec_.onFrame(s, info.time);
	replayInfo_ = info;
	if (!replayBindOn_ && settings_)
	{
		replayBindOn_ = true;
		settings_->setBind("XboxTypeS_RightThumbStick", "ft_takeover");
		eventLog_.write("replay_entered", {{"replay", info.name}, {"map", info.map}});
	}
	if (++replayFrames_ % 15 == 0)
		publishReplay();
}

void FreeTouch::publishReplay()
{
	auto v = std::make_shared<ReplayView>();
	v->active = replayInfo_.active;
	v->name = replayInfo_.name;
	v->map = replayInfo_.map;
	v->time = replayInfo_.time;
	v->focusId = replayInfo_.focusCarId;
	v->framesBuffered = static_cast<int>(replayRec_.frames().size());
	v->message = takeoverMessage_;
	if (!replayRec_.frames().empty())
		for (const auto& c : replayRec_.frames().back().cars)
			v->players.push_back({c.id, c.name, c.team});
	replayPub_ = v;
	publishStatus();
}

void FreeTouch::leaveReplay()
{
	if (!replayBindOn_)
		return;
	replayBindOn_ = false;
	if (settings_)
		settings_->restoreBind("XboxTypeS_RightThumbStick");
	replayRec_.clear();
	replayInfo_ = ReplayInfo{};
	eventLog_.write("replay_left");
	publishReplay();
}

void FreeTouch::takeoverNow(uint64_t playerId, int others, bool leadIn)
{
	if (!game_->inReplay())
	{
		takeoverMessage_ = "Open a replay first.";
		publishReplay();
		return;
	}
	const auto& frames = replayRec_.frames();
	if (playerId == 0)
		playerId = replayInfo_.focusCarId;
	if (playerId == 0 && !frames.empty())
	{
		// Free camera: the first blue player.
		for (const auto& c : frames.back().cars)
			if (c.team == 0)
			{
				playerId = c.id;
				break;
			}
	}
	takeover::Options o;
	o.playerId = playerId;
	o.others = others;
	o.leadIn = leadIn;
	o.replayName = replayInfo_.name;
	o.map = replayInfo_.map;
	o.stamp = nowText("%Y-%m-%d %H:%M:%S");
	std::string error;
	auto doc = takeover::scenario(frames, o, error);
	if (doc.is_null())
	{
		takeoverMessage_ = "Could not take over: " + error;
		eventLog_.write("takeover_refused", {{"reason", error}, {"frames", frames.size()}});
		hud_.pushMechanic("TAKEOVER: " + error, 0.f, 0, true);
		publishReplay();
		return;
	}
	// Saved under captured/replays/ (kept by the content mirror), never overwriting an earlier one.
	const auto dir = contentRoot() / "captured" / "replays";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::string id = doc["id"].get<std::string>();
	std::string file = id;
	for (int n = 2; std::filesystem::exists(dir / (file + ".json")); ++n)
		file = id + "_" + std::to_string(n);
	if (file != id)
	{
		id = file;
		doc["id"] = id;
	}
	{
		std::ofstream out(dir / (file + ".json"), std::ios::trunc);
		out << doc.dump(2) << "\n";
		if (!out)
		{
			takeoverMessage_ = "Could not save the moment.";
			publishReplay();
			return;
		}
	}
	progress_.bump("takeovers");
	saveProgress();
	eventLog_.write("takeover", {{"id", id}, {"replay", o.replayName}, {"map", o.map}, {"time", replayInfo_.time},
		{"player", playerId}, {"others", others}, {"leadIn", doc.contains("leadIn")}, {"bots", doc.contains("bots") ? doc["bots"].size() : 0},
		{"frames", frames.size()}, {"rules", doc["success"]["type"]}});
	LOG("Free Touch: took over {} ({}); loading Free Play", id, doc["name"].get<std::string>());
	takeoverMessage_ = "Saved \"" + doc["name"].get<std::string>() + "\". Loading Free Play...";
	reloadContent();
	const std::string map = o.map;
	startInFreeplay([this, id] { startExercise(id, "medium", {}, std::nullopt); }, id, map);
	publishReplay();
}

void FreeTouch::deleteReplayMoment(const std::string& id)
{
	// Only replay moments, and only files under captured/ (never the shipped content).
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
	if (!item || item->category != "replay")
		return;
	const auto file = std::filesystem::path(item->file);
	const auto captured = (contentRoot() / "captured").lexically_normal().string();
	if (file.lexically_normal().string().rfind(captured, 0) != 0)
		return;
	std::error_code ec;
	std::filesystem::remove(file, ec);
	eventLog_.write("replay_moment_deleted", {{"id", id}, {"removed", !ec}});
	reloadContent();
}

void FreeTouch::drawReplayHint(Canvas& c, const Snapshot& s)
{
	// In a replay: how to take the moment over, with who the camera is on.
	if (!s.inReplay)
		return;
	std::string who;
	for (const auto& car : s.cars)
		if (car.id == replayInfo_.focusCarId)
			who = car.name;
	const float k = c.height() / 1080.f;
	const std::string line = who.empty() ? "FREE TOUCH   |   PAUSE ON A MOMENT, THEN R3 (OR FREE TOUCH > REPLAYS) TO PLAY IT OUT"
		: "FREE TOUCH   |   R3: TAKE OVER AS " + who + "   |   FREE TOUCH > REPLAYS TO PICK SOMEONE ELSE";
	float w = 0, h = 0;
	c.textSize(line, 1.05f * k, w, h);
	const float x = (c.width() - w) * 0.5f, y = c.height() - 120.f * k;
	c.fillRect(x - 16.f * k, y - 6.f * k, w + 32.f * k, h + 12.f * k, Rgba{10, 13, 20, 170});
	c.fillRect(x - 16.f * k, y + h + 4.f * k, w + 32.f * k, 2.f * k, Rgba{44, 217, 196, 200});
	c.text(x, y, line, 1.05f * k, Rgba{235, 240, 248, 255});
}
