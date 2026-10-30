#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <memory>

#include "core/Account.h"
#include "data/ShotShare.h"
#include "core/Friends.h"
#include "core/OnlineShots.h"
#include "core/Leaderboards.h"
#include "core/UpdateStatus.h"
#include "core/PracticeSpeed.h"
#include "core/QuickPlay.h"
#include "core/Routine.h"
#include "core/SessionRunner.h"
#include "game/GameState.h"

// Read-only data the dashboard draws. Built on the game thread (after each content load) and handed to the render
// thread as a whole, so the render thread never touches ContentLibrary or the game. No SDK, no ImGui.
struct ExercisePreview
{
	bool ballVisible = true;
	Vec3 ball;
	Vec3 ballVelocity;
	Vec3 car;
	int carYaw = 0;
	std::vector<Vec3> bots;
	std::vector<Vec3> objects;
	std::vector<float> objectRadii;
};

struct ExerciseCard
{
	std::string id;
	std::string name;
	std::string category;      // content category id, e.g. "first_touch"
	std::string description;
	std::vector<std::string> tags;
	double timeLimitSeconds = 0.0;
	bool allowMirror = false;
	bool hasDifficulty = false; // the scenario defines easy/hard overrides, or setups only on some difficulties
	std::string pending;        // what it still needs, e.g. "objects (Phase 2)": a free setup, not judged; sessions skip it
	int image = -1;             // GameAdapter image handle (images/exercises/<id> or images/categories/<category>)
	ExercisePreview preview;    // seed 1, medium
	std::string opponent;       // "psyonix" (the Opponent setting picks who plays it), "necto" / "nexto" (always), "" none
	long long updated = 0;      // unix seconds its file was last written (new and changed drills sort first by it)
	// Situation variants the page can pick from (situation.variantLabel + variants with an "id"), e.g. kickoff spots.
	std::string variantLabel;
	std::vector<std::pair<std::string, std::string>> variants; // id, name
};

struct SessionCard
{
	std::string id;
	std::string name;
	std::string category;
	std::string description;
	double durationMinutes = 10.0;
	std::string difficulty;
	int exerciseRefs = 0;
	int image = -1;             // images/sessions/<id> or images/categories/<category>
};

// A captured moment (content/captured/), for the Saved Shots page.
struct SavedShot
{
	std::string id;
	std::string name;
	std::string description;
	bool favorite = false;
	shotshare::Extras extras; // its defender and net target (picked when it's played)
	std::string sharedBy;   // a friend's shot (imported from a .ftshot file): their name
};

// A shared shot (.ftshot) waiting in Downloads (or freetouch/inbox) to be imported.
struct SharedShotFile
{
	std::string path; // UTF-8
	std::string name, by, on;
	bool imported = false; // a Saved Shot with the same fingerprint is already there
};

// One Freeplay+ objective the player can switch on or off.
struct FreeplayChoice
{
	std::string id;
	std::string label;
	std::string group;
	std::string about;
	bool beginner = false;
};

// A replay moment saved by replay takeover (content/captured/replays/).
struct ReplayMoment
{
	std::string id;
	std::string name;
	std::string description;
	std::string arena;
	bool save = false;          // a save (the ball was heading into your net) rather than a finish
};

// A short tutorial (content/tutorials/tutorials.json): shown the first time a place in the menu opens, and from its
// ? button. Each step is a drawing (scene) with a title and a line or two.
struct Tutorial
{
	struct Step
	{
		std::string scene, title, text;
	};
	std::string id; // the place: home, train, compete, create, friends, progress, settings
	std::string title;
	std::vector<Step> steps;
};

struct DashboardModel
{
	std::vector<SavedShot> savedShots;          // favourites first, then newest
	std::map<std::string, std::string> raceKeys; // race exercise id -> its ghost key (its boards on the service)
	std::vector<Tutorial> tutorials;            // content/tutorials/tutorials.json
	std::vector<ReplayMoment> replayMoments;    // newest first
	std::string freeplayId;                    // the Freeplay+ scenario
	std::vector<FreeplayChoice> freeplayChoices;
	std::vector<std::string> freeplayEnabled;  // ids switched on (empty = all)
	bool freeplayUnlimitedBoost = true;
	std::vector<routine::Template> routineTemplates; // starting points for the Routine page
	std::vector<ExerciseCard> exercises;
	std::vector<SessionCard> sessions;
	std::vector<std::string> categories; // in display order, only those with exercises
	QuickPlayConfig quickPlay;           // the saved warm-up, or the default one until it is saved
	bool quickPlaySaved = false;
	int contentErrors = 0;
	int contentWarnings = 0;
	std::string contentRoot;
	// Home tiles' art (images/home/<exercises|sessions|quick_play|settings>), image handles or -1.
	int homeImages[4] = {-1, -1, -1, -1};
};

// One exercise's history for its detail page and sorting (settings/history.json, settings/bests.json).
struct ExerciseStatsView
{
	int reps = 0;
	int successes = 0;
	long long lastPlayed = 0;   // unix seconds, 0 = never
	std::string lastLabel;
	int lastOutcome = 0;
	double best[3] = {0.0, 0.0, 0.0}; // timed exercises: best seconds at easy / medium / hard, 0 = none
};

// Home: today, the streak, recent exercises and a suggestion, from the training history (settings/history.json).
struct HomeRecent
{
	std::string id;
	int reps = 0;
	int successes = 0;
	std::string lastLabel;
	int lastOutcome = 0;
};

struct HomeStats
{
	double todayMinutes = 0.0;
	double goalMinutes = 20.0;
	int streakDays = 0;
	int repsToday = 0;
	int successesToday = 0;
	bool week[7] = {};        // the last 7 days trained (a minute or more), oldest first, today last
	std::vector<HomeRecent> recent; // most recent first, up to 4
	std::string nextId;       // Try next
	std::string nextCategory;
	int nextDaysAgo = -1;     // since that category was last played; -1 = never
};

// The Progress page: everything trained on this PC, MMR over time, records, milestones and the journal. Built by the
// plugin (game thread) when something changes; the page only reads it.
struct ProgressView
{
	struct Playlist
	{
		std::string key;          // 1v1 | 2v2 | 3v3
		std::string label;        // Duel, Doubles, Standard
		bool synced = false;      // read from the game today
		float mmr = 0.f;          // latest (game or typed in); 0 = none
		int tier = 0, division = 0;
		std::string rank;         // "Champion II Div III"
		bool hasDelta = false;
		float delta = 0.f;        // since the sample 7+ days before the latest (or the first one)
		float peak = 0.f;
		int matches = 0;
		std::vector<std::pair<int, float>> samples; // (days before today, negative; mmr), oldest first
	};
	std::vector<Playlist> playlists;

	struct DayBar
	{
		std::string day;
		float minutes = 0.f;
		int reps = 0, successes = 0;
	};
	std::vector<DayBar> days;     // the last 84 days, oldest first, today last
	double goalMinutes = 20.0;
	double totalMinutes = 0.0;
	int totalReps = 0, totalSuccesses = 0, daysTrained = 0, streak = 0, bestStreak = 0;

	struct Category
	{
		std::string id, label;
		int reps7 = 0, wins7 = 0, repsPrev7 = 0, winsPrev7 = 0, reps = 0, wins = 0;
		int image = -1;
	};
	std::vector<Category> categories; // most played first

	struct Record
	{
		std::string title, value, sub, exerciseId;
		bool highScore = false;
	};
	std::vector<Record> records;

	struct WeakSpot
	{
		std::string id, name, category, why;
		int reps = 0;
		float success = 0.f;
	};
	std::vector<WeakSpot> weakSpots;

	struct Mechanic
	{
		std::string label;
		int count = 0;
	};
	std::vector<Mechanic> mechanics; // lifetime, most first

	struct Milestone
	{
		std::string id, title, about, progressText, unlockedOn;
		float progress = 0.f;  // 0..1
		bool done = false;
		int tier = 1;          // 1 bronze, 2 silver, 3 gold (its colour)
	};
	std::vector<Milestone> milestones;

	struct JournalDay
	{
		std::string day, focus;
		float minutes = 0.f;
		int reps = 0, wins = 0, ticked = 0;
		std::vector<std::string> notes; // notes not tied to a step of the plan below
		std::string mmr;          // "2v2 1180 (+12)" when recorded that day
		// The routine that day (its saved plan, or today's routine for older logs), each step with how it went.
		struct Step
		{
			std::string section, title, note, tally; // tally: "2 / 3" for counted steps
			bool done = false, optional = false;
		};
		std::vector<Step> steps;
		bool planIsCurrent = false; // older log: the steps are the routine as it is now, not as it was
		struct Trained
		{
			std::string category; // id; the page names it
			int reps = 0, wins = 0;
		};
		std::vector<Trained> trained; // categories played that day, most reps first
	};
	std::vector<JournalDay> journal; // newest first
};

// The Daily Challenge page: today's shots and how they went, the share text, the streak and past days.
struct DailyView
{
	bool available = false;     // a pool is loaded and today is on or after Daily #1
	std::string date;           // "YYYY-MM-DD"
	int number = 0;             // Daily #n
	struct Shot
	{
		std::string slot, scenarioId, name, difficulty;
		uint32_t seed = 0;
		int mark = 0;           // 0 to play, 1 fast hit, 2 hit, 3 miss
		int points = 0;
		std::string label;
	};
	std::vector<Shot> shots;
	int played = 0, hits = 0, points = 0;
	bool done = false;          // today's attempt is finished
	bool running = false;       // it is being played now
	int streak = 0;             // days in a row with a finished daily
	int bestPoints = 0;         // the best finished day
	std::string share;          // the text to paste (emoji squares)
	struct Day
	{
		std::string date;
		int number = 0, hits = 0, points = 0, shots = 0;
		bool done = false;
		std::vector<int> marks;
	};
	std::vector<Day> history;   // past days with an attempt, newest first (today not included)
	int finishedSeq = 0;        // goes up when an attempt finishes (the page opens to show it)
	std::string message;        // why it could not start, if it could not
};

// The replay being watched, for the Replays page.
struct ReplayView
{
	bool active = false;
	std::string name, map;
	double time = 0.0;
	uint64_t focusId = 0;
	struct Player
	{
		uint64_t id = 0;
		std::string name;
		int team = 0;
	};
	std::vector<Player> players;
	int framesBuffered = 0;   // replay frames kept for velocity smoothing
	std::string message;      // the last takeover's result ("" = none)
};

// Live status for the Home screen and the status bar (copied out under a mutex).
// Record page: the finished recording for the page's map and timeline (30 per second: ball, car, car yaw).
struct RecordTake
{
	float hz = 30.f;
	std::vector<Vec3> ball;
	std::vector<Vec3> car;
	std::vector<int> carYaw;
	float seconds() const { return ball.size() < 2 ? 0.f : (ball.size() - 1) / hz; }
};

struct DashboardStatus
{
	bool exerciseActive = false;
	std::string exerciseId;
	std::string exerciseName;
	std::string difficulty;
	int rep = 0;
	uint32_t seed = 0;
	double repSeconds = 0.0;
	int lastGoalSide = 0;
	bool inFreeplay = false;
	std::string lastExerciseId; // for "Continue"
	bool goalExplosion = true;
	bool goalReplays = false;     // goals go in for real, with the game's replay
	std::string selectCommand;    // ft_select_command: what the controller's Select runs
	PracticeSpeedSettings practice;
	bool firstRun = false;      // show the first-run steps on Home
	std::shared_ptr<const std::set<std::string>> tutorialsSeen; // tutorials already shown (null until loaded)
	bool devTools = false;      // Saved Shots and the Captured / Dev categories
	bool mechanicsFeed = true;  // the on-screen feed of detected mechanics
	bool recording = false;     // Record page: a feed is being recorded
	float recordSeconds = 0.f;  // ...for this long
	float takeSeconds = 0.f;    // a finished recording waiting to be saved (0: none)
	std::shared_ptr<const RecordTake> take; // ...its path
	std::shared_ptr<const std::vector<SharedShotFile>> inbox; // shared shots found in Downloads, newest first
	std::string notice; // a result from the game thread to show as a toast (sharing, importing)
	Account::View account; // Free Touch account (Settings > Account)
	Friends::View friends; // the Friends page
	OnlineShots::View shots; // shots friends sent, the Shot Board
	Leaderboards::View boards; // the Daily page's leaderboard
	UpdateView update;         // Settings > Updates
	std::shared_ptr<const std::set<std::string>> haveKeys; // saved shots' fingerprints: "Added"
	bool shareActivity = true;
	int noticeSeq = 0;  // bumped with every notice
	std::string defender = "psyonix"; // who defends in shooting drills: psyonix | necto
	std::string packDefender;         // training packs: "" none | psyonix | necto | nexto
	std::string packTarget;           // training packs: "" whole net | pick | tile (random) | moving
	std::string packTileSize = "medium"; // easy (4 tiles) | medium (6) | hard (8) | impossible (a hole)
	bool packImporting = false;       // a pack is being read shot by shot
	std::string packImportMessage;    // how the last import went
	std::string trainingPackName;     // the custom training pack being played ("" when not in one)
	std::string trainingPackCode;
	int packTile = 0;                 // the picked tile (game cell index: from the bottom row, game x rising)
	std::shared_ptr<const std::set<std::string>> favorites; // starred exercise ids
	bool nectoAvailable = false;      // content/bots/necto/policy.bin is there
	bool nextoAvailable = false;
	float menuVolume = 0.6f;    // 0..1
	float trainingVolume = 0.7f;
	HomeStats home;
	std::shared_ptr<const std::map<std::string, ExerciseStatsView>> stats; // by exercise id (played ones only)
	bool startPending = false;  // Free Play is loading for something started from the main menu
	std::shared_ptr<const routine::Routine> routine; // the warm-up
	std::shared_ptr<const routine::DayLog> today;    // today's ticks, tallies and notes
	std::string todayDate;      // "YYYY-MM-DD"
	std::string freeplayMap;    // ft_freeplay_map

	bool sessionActive = false;
	std::string sessionName;
	double sessionSecondsLeft = 0.0;
	int sessionExerciseIndex = 0;
	int sessionExerciseCount = 0;
	std::shared_ptr<const SessionSummary> lastSummary; // shown on Home until dismissed
	std::shared_ptr<const ProgressView> progress;
	std::shared_ptr<const ReplayView> replay;
	std::shared_ptr<const DailyView> daily;
};
