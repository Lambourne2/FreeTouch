#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/Canvas.h"
#include "content/ContentLibrary.h"
#include "data/BestTimes.h"
#include "data/Ghost.h"
#include "data/LeadIn.h"
#include "detectors/MechanicDetector.h"
#include "detectors/SpeedFlipCoach.h"
#include "game/GameAdapter.h"
#include "objects/ObjectSystem.h"
#include "scenario/ScenarioLoader.h"


class EventLog;

// Applies scenarios through the GameAdapter and keeps the rep history for retry / next / previous.
// Every apply is a new rep (with its own rep number) and is logged with its seed so it can be reproduced.
// Game thread only. No SDK calls.
class ScenarioPlayer
{
public:
	ScenarioPlayer(GameAdapter& game, const ContentLibrary& content, EventLog& log);

	// New rep of `scenarioId` with a fresh seed. `seed` forces one (for reproducing a logged rep).
	bool play(const std::string& scenarioId, const std::string& difficulty, std::optional<uint32_t> seed = {});
	bool retry();     // same scenario, same seed
	bool next(bool keepCar = false); // same scenario, new seed; keepCar: leave the car as it is (chained drills)
	bool previous();  // the rep before this one in the history (same seed it had)
	void stop();      // end the current rep; nothing is applied until the next play (shows a hidden ball again)

	// Called with true when an exercise starts and false when it stops (exercise binds are swapped in meanwhile).
	void setActiveListener(std::function<void(bool)> listener) { activeListener_ = std::move(listener); }
	bool active() const { return current_.has_value(); }
	// Unlimited boost on (true) or off (false) for one scenario, whatever its own player.boostLock says (Freeplay+).
	void setBoostOverride(const std::string& scenarioId, bool unlimited) { boostOverride_[scenarioId] = unlimited; }
	// Goals in the opponent's net play the player's goal explosion (default on). Conceded goals never explode.
	void setGoalExplosion(bool on) { goalExplosion_ = on; }
	// "Goal replays" (Settings): goals go in for real and the game's replay plays; the drill moves on after it. Off:
	// every goal is caught at the line and exploded, straight on to the next rep.
	void setGoalReplays(bool on) { goalReplays_ = on; }
	// Training packs: a defender in the orange net on every pack ("" none, or psyonix / necto / nexto).
	void setPackDefender(const std::string& type) { packDefender_ = type; }
	// Training packs: only part of the net counts ("" whole net, "tile" one lit tile per shot, "moving" a lit tile
	// that moves).
	// size: easy (4 tiles) | medium (6) | hard (8) | impossible (a hole just bigger than the ball); tile: the chosen
	// cell for "pick" (row-major from the bottom left as the game's x runs, see zoneAt).
	void setPackTarget(const std::string& target, const std::string& size, int tile)
	{
		packTarget_ = target;
		packTileSize_ = size;
		packTile_ = tile;
	}
	// Session air-roll choice: force mirroring on/off for scenarios that allow it (nullopt = random per rep).
	void setMirrorPolicy(std::optional<bool> mirror) { mirrorPolicy_ = mirror; }
	// Scenarios with "onSuccess" start their next rep themselves after a success. A session sets this to decide
	// (it may rotate to another exercise instead); without it the player just goes to its own next rep.
	void setAdvanceRequest(std::function<void(bool keepCar)> request) { advanceRequest_ = std::move(request); }
	// Which one_of choices (by id) a scenario may pick from, e.g. the objectives ticked in the Freeplay+ picker.
	void setChoiceFilter(const std::string& scenarioId, std::vector<std::string> enabled) { choiceFilters_[scenarioId] = std::move(enabled); }
	// The Daily Challenge plays every scenario with all its choices (the same reps for every player), whatever was picked.
	void setIgnoreChoiceFilters(bool ignore) { ignoreChoiceFilters_ = ignore; }
	// Every rep waits, frozen on its setup with the objective shown, until a fresh press (the Daily Challenge).
	void setForceReadFirst(bool on) { forceReadFirst_ = on; }
	// Target grids in the orange net drawn as the training packs' tiles (grey, the open one lit) (the Daily Challenge).
	void setPackStyleTargets(bool on) { packStyleTargets_ = on; }
	// Personal bests for timed scenarios ("timed": true); not owned.
	void setBestTimes(BestTimes* bests) { bests_ = bests; }
	// Ghosts ("ghost": true races): where the best run is loaded from and saved to (by best key).
	// Races: someone else's run as the ghost for the race whose ghost key (sanitized best key) is `key`, with their
	// name over it, until another race is played or it is cleared (Phase 8: race a friend's ghost).
	void setGhostOverride(const std::string& key, ghost::Run run, const std::string& label)
	{
		ghostOverrideKey_ = key;
		ghostOverride_ = std::move(run);
		ghostOverrideLabel_ = label;
	}
	void clearGhostOverride() { ghostOverrideKey_.clear(); ghostOverride_.clear(); ghostOverrideLabel_.clear(); }
	void setGhostStore(std::function<ghost::Run(const std::string& key)> load, std::function<void(const std::string& key, const ghost::Run&)> save)
	{
		ghostLoad_ = std::move(load);
		ghostSave_ = std::move(save);
	}
	// Called when a rep has been applied (bots are set up from it).
	void setRepStartListener(std::function<void(const ResolvedScenario&)> listener) { repStartListener_ = std::move(listener); }
	// The start countdown only ends once this says yes (the bots are placed), so nobody gets a head start.
	void setStartGate(std::function<bool()> gate) { startGate_ = std::move(gate); }
	// The tick the start countdown ends on (0 = no countdown running); the bots are held until then.
	// Bots are held at their starts until this tick (the start countdown); while a rep waits for the player's first
	// input ("waitForInput") they are held until it comes.
	uint64_t countdownEndTick() const
	{
		return waitForInput_ && leadInPhase_ == LeadInPhase::Waiting ? UINT64_MAX : countdownUntilTick_;
	}
	// Seconds into the lead-up while it plays (its end while it waits for the player); -1 when there is none running.
	double leadInTime() const
	{
		if (leadInPhase_ == LeadInPhase::Off)
			return -1.0;
		return leadInPhase_ == LeadInPhase::Waiting ? leadIn_.seconds() : std::min(leadIn_.seconds(), (lastTick_ - leadInStartTick_) / 120.0);
	}

	void onTick(const Snapshot& snap);
	void onTouch(const TouchEvent& touch);
	// A mechanic the detector recognised (Phase 4): mechanics drills, HORSE, combos and Style Rush judge from these.
	void onMechanic(const MechanicEvent& e);
	// This scenario shows the mechanics feed whatever the developer setting (the mechanics modes).
	bool wantsMechanicsFeed() const { return current_ && current_->resolved.value("showMechanics", false); }
	// Draws the rep's world objects (draw listener, game thread).
	void drawObjects(Canvas& canvas, const Snapshot& snap) const;
	// Freeplay did its own kickoff reset (e.g. a "reset" input on the same button as retry): put the rep back.
	void onRoundReset();
	// The game scored a real goal (its "goal scored" event): wait for its replay and the reset after it.
	void onRealGoal();

	struct Status
	{
		bool active = false;
		std::string scenarioId;
		std::string scenarioName;
		std::string difficulty;
		int rep = 0;
		uint32_t seed = 0;
		bool mirrored = false;
		double repSeconds = 0.0;
		double timeLimitSeconds = 0.0;
		bool clockCountsDown = false; // "clockCountsDown": the HUD shows the time left (training packs)
		int touches = 0;
		int goalSide = 0;   // +1 orange net, -1 blue net, 0 no goal this rep

		// This rep's result, judged from the scenario's success/fail rules (once per rep).
		bool hasResult = false;
		int outcome = 0;           // +1 success, -1 fail, 0 neutral
		std::string resultLabel;   // GOAL, CONCEDED, SAVED, TIME, WIDE, OWN GOAL
		std::string resultDetail;  // a line under the result (kickoffs: who got there first, whose ball)
		int countdown = 0;         // the start countdown's whole seconds left (3, 2, 1); 0 when none is running

		// Match labs ("match": {"minutes": 3}): the score, the clock and whether it is in overtime.
		bool match = false;
		int scoreYou = 0, scoreBot = 0;
		double matchSecondsLeft = 0.0; // overtime: seconds played in overtime
		bool overtime = false;
		bool matchOver = false;

		// Mechanics modes: the score of a Style Rush run (shown big), whether the feed is forced on.
		bool showMechanics = false;
		bool ghost = false;          // racing a ghost of the best run
		double ghostSeconds = 0.0;   // that run's time
		bool plainObjective = false; // the objective line is a score / clock, without "OBJECTIVE:"
		bool quietObjective = false; // no big announcement when a rep starts (Style Rush: every ball is a rep)
		bool prompt = false;         // the objective is a short prompt ("Press any button to play"): small, no banner
		bool waitingForInput = false; // frozen on the setup with the objective up, until a press starts it ("readFirst")
		// While the rep is held (waiting for a press, or its start countdown): the HUD's read card shows the full name,
		// what counts (from the success rule) and the first sentence of the description.
		bool holding = false;
		bool goalReplay = false; // a real goal's slow motion / replay is playing: the HUD stays out of the way
		std::string goalText;  // "SCORE", "SAVE IT", "AIR DRIBBLE > GOAL", ...
		std::string about;     // the description's first sentence

		// "showPlayMap": a top-down map of the play during the start countdown (where everyone is, the teammate).
		struct PlayMap
		{
			bool show = false;
			Vec3 ball, player;
			int playerYaw = 0;
			std::vector<Vec3> bots;
			std::vector<int> botYaws;
			bool hasMate = false;
			Vec3 mate;
			float mateRadius = 0.f;
			bool hasContact = false;
			Vec3 contact;
		} map;
		float resultKmh = 0.f;
		int resultSeq = 0;         // increases with every result (the HUD flashes on change)
		int streak = 0;            // successes in a row

		// Timed scenarios: the run's time, the personal best for this exercise + difficulty, and whether the run set it.
		bool timed = false;
		bool selfAdvancing = false; // the scenario starts its next rep itself after this result
		float tank = -1.f;          // the car's boost when the scenario shows it (One Tank); -1 = not shown

		// "Hit them all" drills (all_objects_hit, not bowling): objects hit so far out of how many.
		int objectsHit = 0;
		int objectsTotal = 0;

		// Bowling: pins down this frame out of the rack, and which throw of the frame this is.
		int pinsDown = 0;
		int pinsTotal = 0;
		int frame = 0;
		int frameThrow = 0;
		int frameThrows = 0;

		// Labs with a changing objective (one_of with a label): what to do, a short tag and where it is.
		std::string objective;
		std::string objectiveTag;
		std::string objectiveProgress; // e.g. "2 / 4" (keep it up), "1.5 / 3.0 S" (dribble)
		std::optional<Vec3> objectiveAt;
		double resultSeconds = 0.0;
		double bestSeconds = 0.0;  // 0 = no best yet
		bool newBest = false;
	};
	Status status() const;
	// Called once per judged rep, with the status right after judging.
	void setResultListener(std::function<void(const Status&)> listener) { resultListener_ = std::move(listener); }

private:
	struct Entry
	{
		std::string scenarioId;
		uint32_t seed = 0;
		int level = -1; // levels / loops: the level this rep played (set when applied), so previous and retry replay it
	};

	bool apply(const Entry& entry, const std::string& reason, bool keepCar = false, bool keepBall = false);
	void waitForCar(const Entry& entry, const std::string& reason, int generation, int triesLeft);
	int applyGeneration_ = 0;
	static constexpr int kCarWaitTries = 40; // 8 s in 0.2 s steps
	void endRep(const std::string& reason);
	uint32_t freshSeed();
	void showBall();

	GameAdapter& game_;
	const ContentLibrary& content_;
	EventLog& log_;

	std::vector<Entry> history_;
	int historyIndex_ = -1;
	std::string difficulty_ = "medium";

	std::optional<ResolvedScenario> current_;
	int rep_ = 0;
	uint64_t repStartTick_ = 0;
	uint64_t lastTick_ = 0;
	uint64_t freezeUntilTick_ = 0; // hold the ball still until this tick, then release it
	bool released_ = true;
	std::map<std::string, bool> boostOverride_;
	// Saved Shots lead-in: replaying the seconds before the capture, then frozen until the player presses something.
	enum class LeadInPhase { Off, Playing, Waiting };
	LeadInPhase leadInPhase_ = LeadInPhase::Off;
	leadin::Track leadIn_;
	uint64_t leadInStartTick_ = 0;
	bool leadInArmed_ = false; // saw no input since the freeze began
	std::string packDefender_;
	std::string packTarget_;
	std::string packTileSize_ = "medium";
	int packTile_ = 0;
	void addPackDefender(ResolvedScenario& r) const;
	void addPackTarget(ResolvedScenario& r) const;
	// Saved Shots' "extras": a defender in the orange net and the net's open tiles (the rest closed).
	void addShotExtras(ResolvedScenario& r) const;
	nlohmann::json defenderBot(const std::string& who) const; // in the orange net, facing out, by difficulty
	void targetsAsPackTiles(ResolvedScenario& r) const;
	std::string goalText() const; // the success rule in a few words, for the HUD's read card
	bool packStyleTargets_ = false;
	bool waitForInput_ = false; // "waitForInput": this rep started frozen (no lead-in) until the player's first input
	bool readFirst_ = false;    // "readFirst" (or forced): that wait shows the objective and needs a fresh press
	bool forceReadFirst_ = false;
	uint64_t timeUpTick_ = 0;   // "timeUpWaitsForGround": the clock ran out here; the rep ends when the ball lands
	void drawTrajectory(Canvas& canvas) const;
	void tickLeadIn(const Snapshot& snap);
	uint64_t countdownUntilTick_ = 0; // startCountdownSeconds: frozen at the start until this tick
	void tickCountdown(const Snapshot& snap);
	std::function<bool()> startGate_;
	uint64_t countdownGiveUpTick_ = 0; // stop waiting for the bots here and start anyway
	bool countdownWaitLogged_ = false;
	// Kickoffs ("success": {"type": "kickoff"}): judged a moment after the first touch, from where the ball is going
	// and who is nearer to it.
	void judgeKickoff(const Snapshot& snap);
	// 50s ("success": {"type": "fifty"}): judged by where the ball goes after the first touch, against the rep's aim
	// (situation.aim: corner / safe / mate).
	void judgeFifty(const Snapshot& snap);
	std::string firstTouchText() const;
	// Ball Control ("success": {"type": "possession"}): an awkward ball and the bot pressuring. From your first touch
	// you must keep it (you are the one who gets to the ball next) for keepSeconds, get past the bot with it, or
	// score; the bot having it for half a second in a row (or getting there first) is losing it.
	void judgeControl(const Snapshot& snap);
	struct Control
	{
		uint64_t lostSince = 0;     // the bot has had the ball (gets to it next) since this tick
		uint64_t softTick = 0;      // first touch + 0.1 s: when the first touch's ball speed is read
		float firstTouchKmh = -1.f; // the ball's speed just after your first touch (a soft first touch is control)
		int botTouches = 0;
		float possessionSeconds = 0.f;
		uint64_t onTargetTick = 0;  // the last tick your ball was heading into their net
		bool botHadControl = false; // shadow: last tick, the bot had the ball under control (close, low, on its wheels)
		bool wasHeadingIn = false;  // shadow: last tick, the ball was heading into your net
	} ctl_;
	// Shadow defense ("success": {"type": "shadow"}, Match Skills): the bot attacks with the ball. Before you commit
	// (your first touch, or getting within 300 uu of the ball) it counts how much of the time you were goal-side; your
	// commit is a challenge on a loose ball (the bot more than 260 uu from it, or in the air) or a dive into its
	// control. Then: cleared (WON IT / GOOD CHALLENGE), a shot SAVED, BEATEN / DOVE IN (it got past you), CONCEDED, or
	// at time up still in front and no goal: HELD HIM UP.
	void judgeShadow(const Snapshot& snap);
	// "coach": "speed_flip" (Phase 5): the run is watched from its start and one tip is added to the result line.
	void tickCoach(const Snapshot& snap);
	void reportCoach();
	SpeedFlipCoach coach_;
	bool coachReported_ = false;
	std::string coachLine_;   // "SPEED FLIP: <tip>   |   LANDED AT ... KM/H", added to the rep's result line
	struct Shadow
	{
		int ticks = 0, goalSideTicks = 0;
		uint64_t commitTick = 0;
		bool commitLoose = false;     // the bot did not have the ball under control when you committed
		bool shotBefore = false;      // the bot's shot was heading in when you touched it
		uint64_t pastSince = 0;       // the ball has been goal-side of you since this tick
	} shadow_;
	// Match labs: every rep is a kickoff; goals add to the score; the clock runs while the ball is live; a tie at
	// 0:00 goes to overtime (next goal wins); otherwise the match ends when the ball next touches the floor.
	bool isMatch() const { return current_ && current_->resolved.contains("match"); }
	void tickMatch(const Snapshot& snap);
	void endMatch(const char* why);
	std::string scoreText() const;
	std::string matchScenario_;
	int scoreYou_ = 0, scoreBot_ = 0;
	double matchSecondsLeft_ = 0.0;
	double overtimeSeconds_ = 0.0;
	bool overtime_ = false;
	bool matchOver_ = false;
	bool clockZeroLogged_ = false;

	// ---- Ghosts (races): this run being recorded, and the best run to race ----
	bool ghostOn() const { return current_ && current_->resolved.value("ghost", false); }
	std::function<ghost::Run(const std::string&)> ghostLoad_;
	std::function<void(const std::string&, const ghost::Run&)> ghostSave_;
	ghost::Run ghostBest_;
	ghost::Run ghostRun_;
	std::string ghostLabel_; // whose ghost is shown ("" = your best)
	std::string ghostOverrideKey_, ghostOverrideLabel_;
	ghost::Run ghostOverride_;
	float padBoost_ = -1.f; // "noPads": the tank the car may have (a rise is a pad pickup, taken back); -1 = not tracking yet
	void drawGhost(Canvas& canvas) const;

	// ---- Mechanics modes (ScenarioModes.cpp) ----
	// "success": {"type": "mechanic", "chain": [...], "requireGoal": bool, "zone": bool}: the chain of mechanics in
	// order within the rep (tokens "id" or "id:word", the word found in the mechanic's label, e.g. "flick:musty",
	// "air_dribble:x3"), then (requireGoal) a goal, (zone) into the lit zone.
	struct RepMechanic
	{
		std::string id, label;
		uint64_t tick = 0;
		int points = 0;
	};
	std::vector<RepMechanic> repMechanics_;
	int chainStep_ = 0;
	std::vector<std::string> chainTokens() const;
	static bool tokenMatches(const std::string& token, const RepMechanic& m);
	static std::string tokenName(const std::string& token);
	bool mechanicSuccess() const;
	void resetModeRep();
	void tickModes(const Snapshot& snap);
	// A goal in a mode that judges goals itself; true when handled. `side` +1 orange net.
	bool modeGoal(int side, const Vec3& at, float kmh);
	void judgeModeGoal();
	struct PendingGoal
	{
		bool on = false;
		uint64_t judgeTick = 0;
		bool attacking = false;
		Vec3 at;
		float kmh = 0.f;
		int zone = -1;
	} pendingGoal_;
	void modeStatus(Status& s) const;
	void modeAfterResult(int outcome, std::string& label);
	// Before a rep is resolved: a fresh HORSE game / Style Rush run / combo ladder when one starts.
	void modeNewRun(const nlohmann::json& data, const std::string& scenarioId, const std::string& reason);
	// After: the lit zone (zone_cycle objects are taken out of the object list) and balls fed near the car.
	void setupZones(ResolvedScenario& r);
	void placeNearCar(ResolvedScenario& r, bool keepCar);

	// Lit zone on the net (objects type "zone_cycle"; success "goal_in_zone" or a mechanic with "zone": true).
	struct Zones
	{
		bool on = false;
		int cols = 3, rows = 2;
		double cycleSeconds = 1.0;
		int lit = 0;
		uint64_t nextTick = 0;
		uint32_t rng = 1;
		int side = 1; // the net it is on (+1 orange)
		bool fixed = false;    // "lit": the player's chosen tile, it never moves
		bool blocking = false; // "blocking": the rest of the net is closed (the ball bounces off: MISS)
		float hole = 0.f;      // "hole": a round hole of this radius at the lit cell's centre instead of a tile
		uint32_t open = 0;     // "open": these tiles are open (a bit per cell) instead of the one lit tile
	} zones_;
	int zoneAt(const Vec3& p) const;
	// Whether a ball centred at `p` (on the net's face) goes in: in the lit tile, or wholly through the hole.
	bool zoneHit(const Vec3& p) const;
	Vec3 zoneCentre(int cell) const;
	void blockZoneMiss(const Snapshot& snap); // the ball meets the closed part of the net: it bounces off, MISS
	void drawZones(Canvas& canvas) const;

	// HORSE (progression "horse"): a called shot per rep; a miss earns the next letter of the word.
	int horseLetters_ = 0;
	int horseMade_ = 0;
	bool horseOver_ = false;
	std::string horseScenario_;
	std::string horseWord() const;

	// Style Rush (scenario "rush"): a timed run; mechanics fill a pot, distinct ones raise the multiplier, a goal
	// cashes it at double, a dead ball banks it plain.
	bool isRush() const { return current_ && current_->resolved.contains("rush"); }
	std::string rushScenario_;
	double rushSecondsLeft_ = 0.0;
	int rushScore_ = 0;
	int rushPot_ = 0;
	std::vector<std::string> rushKinds_;
	bool rushOver_ = false;
	int rushPlays_ = 0;
	int rushBestPlay_ = 0;
	std::string rushBonus_;          // the mechanic worth triple right now ("" = none)
	uint64_t rushBonusUntil_ = 0, rushNextBonus_ = 0;
	uint64_t rushDeadTicks_ = 0;     // ticks the ball has lain on the floor
	uint64_t rushLastTouchTick_ = 0;
	uint32_t rushRng_ = 7;
	bool rushNewBest_ = false;
	void endRushPlay(const char* why, bool goal);
	void endRush();
	uint64_t firstPlayerTouchTick_ = 0;
	uint64_t firstBotTouchTick_ = 0;
	std::string resultDetail_;
	bool ballHidden_ = false;
	bool goalHeld_ = false;   // a goal was caught at the line; the ball is held there until the next rep
	bool goalReplays_ = false;     // "Goal replays" setting: goals go in for real, with the game's replay
	bool goalLetThrough_ = false;  // this rep's goal was let through (not caught again)
	bool realGoalPending_ = false; // ...and its replay / freeplay reset has not come yet
	bool ballExploded_ = false; // the ball was exploded; respawn it before the next rep
	// The last ticks of ball and local car during a rep: a freeplay reset puts them back (onRoundReset).
	struct RecentState
	{
		uint64_t tick = 0;
		BallState ball;
		CarState car;
	};
	std::deque<RecentState> recent_;
	uint64_t explodedAtTick_ = 0; // when: the game ignores a respawn during the explosion's first second
	bool goalExplosion_ = true;
	std::optional<bool> mirrorPolicy_;
	std::function<void(const Status&)> resultListener_;
	std::function<void(const ResolvedScenario&)> repStartListener_;
	uint64_t keeperWatchFromTick_ = 0; // a bot touched the ball: judge a keeper save from this tick
	int keeperOutTicks_ = 0;
	uint64_t lastPlayerTouchTick_ = 0;
	uint64_t lastBotTouchTick_ = 0;
	bool keeperContested_ = false;     // the watched touch was a 50
	static constexpr int kKeeperConfirmTicks = 30; // 0.25 s off target before it is a keeper save
	static constexpr int kFiftyTicks = 12;         // touches this close together (0.1 s) are a 50
	bool lastTouchByBot_ = false;
	std::optional<BallState> prevBall_; // last tick's ball, to log side-wall bounces
	Vec3 goalHoldAt_;
	int touches_ = 0;
	int goalSide_ = 0;
	uint64_t saveWatchFromTick_ = 0; // save judging starts when the touch's effect has landed (2 ticks after it)
	int defendedNet_ = -1;           // -1 blue, +1 orange: the local car's team (kept while it is demolished)
	int outTicks_ = 0;              // consecutive ticks the ball has not been heading into the defended net
	static constexpr int kSaveConfirmTicks = 3;
	uint64_t goalCheckFromTick_ = 0; // a rep's first ticks still show the previous ball position

	void setResult(int outcome, const std::string& label, float kmh);
	void logWallBounce(const Snapshot& snap);
	void judgeObjects(const Snapshot& snap);
	bool failsOnObject(const std::string& objectId) const;
	ObjectSystem objects_;
	std::string successType() const;
	bool failHas(const std::string& type) const;

	bool hasResult_ = false;
	int outcome_ = 0;
	std::string resultLabel_;
	float resultKmh_ = 0.f;
	double resultSeconds_ = 0.0;
	bool resultNewBest_ = false;
	BestTimes* bests_ = nullptr;
	std::function<void(bool keepCar)> advanceRequest_;
	std::map<std::string, std::vector<std::string>> choiceFilters_;
	bool ignoreChoiceFilters_ = false;
	std::map<std::string, int> level_;   // progression "levels" / "run": the level each scenario is on
	int runSuccesses_ = 0;                // progression "run": successes so far in this run
	uint64_t reapplyBallTick_ = 0;        // place the ball again on this tick (after un-hiding it)
	bool freshRun_ = true;                // loop / trial: the next level-0 rep starts a new run (full tank, clock)
	bool lastFailed_ = false;
	uint64_t trialStartTick_ = 0;         // trial: when the current attempt started
	bool trialDone_ = false;              // trial: this result finished all the situations
	float lastLocalBoost_ = -1.f;
	std::string progression() const { return current_ ? current_->resolved.value("progression", "") : ""; }
	std::vector<std::string> levelIds(const ContentLibrary::Item& item) const;
	std::string bestKey() const;
	const std::vector<std::string>& choiceFilter(const std::string& scenarioId) const;
	// Skill objectives judged from touches and the ball (touch, touch_streak, dribble).
	void judgeSkill(const Snapshot& snap);
	uint64_t pendingTouchTick_ = 0;   // a touch to judge once its hit shows in the ball (2 ticks later)
	float pendingTouchBallZ_ = 0.f;
	bool pendingTouchOnWall_ = false;
	bool lastLocalOnWall_ = false;
	int touchStreak_ = 0;
	double dribbleSeconds_ = 0.0;
	uint64_t dribbleLostTick_ = 0;
	std::optional<BallState> lastBall_;
	std::string onSuccess() const { return current_ ? current_->resolved.value("onSuccess", "stay") : "stay"; }
	// Bowling (scenario "bowling"): frames of throws on one setup; pins knocked down stay down within a frame.
	bool bowling() const { return current_ && current_->resolved.contains("bowling"); }
	int bowlingThrows() const { return bowling() ? current_->resolved["bowling"].value("throws", 2) : 0; }
	void judgeBowling(const Snapshot& snap);
	void finishThrow();
	int pinsDown() const;
	int pinsTotal() const;
	int frame_ = 0;
	int frameThrow_ = 1;
	bool frameDone_ = false;
	bool carryPins_ = false;
	std::vector<std::string> carryDown_;
	int pinsDownAtStart_ = 0;
	uint64_t lastPinHitTick_ = 0;
	uint64_t slowSinceTick_ = 0;
	std::string onFail() const { return current_ ? current_->resolved.value("onFail", "stay") : "stay"; }
	// What the scenario does after this outcome: "stay", "next" or "chain".
	std::string afterResult(int outcome) const { return outcome > 0 ? onSuccess() : onFail(); }
	uint32_t chainSeed(const std::string& scenarioId);
	bool timed() const { return current_ && current_->resolved.value("timed", false); }
	int resultSeq_ = 0;
	int streak_ = 0;
	uint64_t seedCounter_ = 0;
	std::function<void(bool)> activeListener_;
};
