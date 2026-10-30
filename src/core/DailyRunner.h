#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "core/DailyChallenge.h"
#include "core/SessionRunner.h"
#include "scenario/ScenarioPlayer.h"

class EventLog;

// Plays the Daily Challenge over the scenario player: today's shots in order, each with its seed, one rep each.
// A shot's first judged rep is its result; a retry, a skip to another rep, or stopping before the result counts as a
// miss. Stopping keeps your place: start() continues at the next shot. Game thread only. No SDK calls.
class DailyRunner
{
public:
	DailyRunner(ScenarioPlayer& player, EventLog& log) : player_(player), log_(log) {}

	void setPool(daily::Pool pool) { pool_ = std::move(pool); challengeDate_.clear(); }
	const daily::Pool& pool() const { return pool_; }

	// Today's challenge (made once per date).
	const daily::Challenge& challenge(const std::string& today);

	// Starts (or continues) today's attempt; false with a reason when there is nothing to play.
	bool start(const std::string& today, std::string& error);
	bool active() const { return active_; }
	// Ends the run here (the shot being played counts as a miss when it had no result yet).
	void stop(const std::string& reason);

	void onTick(uint64_t tick, const ScenarioPlayer::Status& status);
	void onResult(const ScenarioPlayer::Status& status);
	void onPlayerStopped();

	// The HUD line while it runs: DAILY #n | SHOT i / 10 | points, and the squares.
	SessionRunner::Status hudStatus() const;

	// Every day's attempt, by date (settings/daily.json).
	const std::map<std::string, daily::Attempt>& attempts() const { return attempts_; }
	void loadAttempts(const nlohmann::json& doc);
	// The service already has this player's score for `date` (posted from this PC or another, before a lost file or
	// a reinstall): that day is finished here too, with the posted squares; a run of it going on stops. True if
	// anything changed.
	bool lockFromService(const std::string& date, int number, const std::string& marks, int points);
	nlohmann::json attemptsJson() const;

	// Called when an attempt changes (save it, refresh the page) and when it is finished.
	void setChangedListener(std::function<void()> f) { changed_ = std::move(f); }
	void setFinishedListener(std::function<void(const daily::Attempt&)> f) { finished_ = std::move(f); }

private:
	void playShot();
	void record(const daily::ShotResult& r, const std::string& why);
	daily::Attempt& attempt() { return attempts_[runDate_]; }

	ScenarioPlayer& player_;
	EventLog& log_;
	daily::Pool pool_;
	std::string challengeDate_;
	daily::Challenge challenge_;
	std::map<std::string, daily::Attempt> attempts_;

	bool active_ = false;
	std::string runDate_;          // the day the run is for (a run that crosses midnight stays on its day)
	daily::Challenge run_;         // that day's shots
	size_t shot_ = 0;              // the shot being played
	bool awaitingStart_ = false;   // play() asked for; the rep is not on the field yet
	int shotRep_ = -1;             // the player's rep number for this shot once it started
	bool shotDone_ = false;        // this shot has its result; waiting to move on
	uint64_t lastTick_ = 0;
	uint64_t nextAtTick_ = 0;      // when to start the next shot (a pause to see the result)

	std::function<void()> changed_;
	std::function<void(const daily::Attempt&)> finished_;
};
