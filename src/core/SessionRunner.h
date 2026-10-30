#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/Rng.h"
#include "scenario/ScenarioPlayer.h"

class ContentLibrary;
class EventLog;

struct SessionSettings
{
	std::string sessionId;
	double minutes = 10.0;
	std::string difficulty = "medium"; // easy | medium | hard (adaptive plays as medium for now)
	std::string airRoll = "free";      // left = scenarios as authored, right = mirrored, free = random
};

struct ExerciseStats
{
	std::string id;
	std::string name;
	int reps = 0;        // judged + skipped
	int successes = 0;
	int fails = 0;
	int neutrals = 0;
	int skipped = 0;
	int streak = 0;
	int bestStreak = 0;
	float fastestKmh = 0.f;  // fastest successful goal
};

struct SessionSummary
{
	std::string sessionId;
	std::string name;
	std::string difficulty;
	std::string airRoll;
	std::string endReason;   // time | done (a warm-up's list finished) | stopped
	double plannedSeconds = 0.0;
	double playedSeconds = 0.0;
	int reps = 0, successes = 0, fails = 0, bestStreak = 0;
	float fastestKmh = 0.f;
	std::vector<ExerciseStats> exercises; // in the order they were first played
};

// Runs a session: a timed playlist over the scenario player. Rotates exercises (every N reps or by time share),
// starts the next rep by itself after a judged rep, ends on time with a summary. Game thread. No SDK calls.
class SessionRunner
{
public:
	SessionRunner(ScenarioPlayer& player, const ContentLibrary& content, EventLog& log);

	bool start(const SessionSettings& settings, std::string& error);
	void finish(const std::string& reason);   // time | stopped
	bool active() const { return active_; }

	void next();                               // D-pad right in a session: skip to the next rep
	// A self-advancing drill (onSuccess "next"/"chain") finished a rep: continue it, or rotate when due.
	void continueAfterSuccess(bool keepCar) { if (active_) advance(keepCar); }
	void onTick(const Snapshot& snap);
	void onResult(const ScenarioPlayer::Status& status);
	void onPlayerStopped();                    // the exercise was stopped from outside (ft_stop, D-pad up)

	struct Status
	{
		bool active = false;
		std::string name;
		double secondsLeft = 0.0;
		int exerciseIndex = 0;   // 1-based position in the rotation
		int exerciseCount = 0;
		// The Daily Challenge uses the same line: its own text, and a square per shot (0 to play, 1 fast hit, 2 hit,
		// 3 miss; see daily::Mark). Empty for sessions.
		std::string line;
		std::vector<int> marks;
	};
	Status status() const;
	std::shared_ptr<const SessionSummary> lastSummary() const { return summary_; }
	void clearSummary() { summary_.reset(); }

	void setFinishedListener(std::function<void(const SessionSummary&)> listener) { finished_ = std::move(listener); }

private:
	struct Slot
	{
		std::string scenarioId;
		double weight = 1.0;
		int reps = 0;           // rotate after this many reps (0: the session's repsPerExercise / time share)
		std::string difficulty; // "" = the session's
	};

	void advance(bool keepCar = false); // next rep, rotating exercise when due (keepCar: a chained drill continues)
	void playSlot(int index);
	int pickNextSlot();
	double elapsedSeconds() const;
	ExerciseStats& statsFor(const std::string& id);

	ScenarioPlayer& player_;
	const ContentLibrary& content_;
	EventLog& log_;

	bool active_ = false;
	bool finishing_ = false;
	SessionSettings settings_;
	std::string name_;
	std::string order_;
	int repsPerExercise_ = 0;  // 0 = rotate by time share
	bool endAfterList_ = false; // warm-ups: play the list once, in order, then finish ("done")
	std::vector<Slot> slots_;
	std::vector<int> sequence_; // slot order for sequential/shuffle
	int sequencePos_ = 0;
	int current_ = -1;          // slot index
	int repsOnCurrent_ = 0;
	bool currentJudged_ = false;

	uint64_t startTick_ = 0;
	uint64_t lastTick_ = 0;
	uint64_t exerciseStartTick_ = 0;
	uint64_t autoAdvanceTick_ = 0; // 0 = none
	int judgedRep_ = 0;            // the player's rep number that was judged (auto-advance only from that rep)
	Rng rng_{1};

	std::vector<std::string> statsOrder_;
	std::map<std::string, ExerciseStats> stats_;
	std::shared_ptr<const SessionSummary> summary_;
	std::function<void(const SessionSummary&)> finished_;
};
