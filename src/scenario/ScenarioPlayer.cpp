#include "pch.h"
#include "scenario/ScenarioPlayer.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "content/ContentLibrary.h"
#include "core/Rng.h"
#include "data/EventLog.h"
#include "data/Capture.h"
#include "data/LeadIn.h"
#include "data/ShotShare.h"
#include "data/StateJson.h"
#include "scenario/BallPredict.h"
#include "scenario/GoalGuard.h"

namespace
{
	constexpr double kTicksPerSecond = 120.0;
}

ScenarioPlayer::ScenarioPlayer(GameAdapter& game, const ContentLibrary& content, EventLog& log)
	: game_(game), content_(content), log_(log)
{
}

uint32_t ScenarioPlayer::freshSeed()
{
	// Wall clock mixed with a counter: different every press, and only ever used through the logged seed.
	const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	Rng rng(static_cast<uint64_t>(now) ^ (++seedCounter_ * 0x9E3779B97F4A7C15ull));
	uint32_t seed = static_cast<uint32_t>(rng.nextU64());
	return seed == 0 ? 1u : seed;
}

bool ScenarioPlayer::play(const std::string& scenarioId, const std::string& difficulty, std::optional<uint32_t> seed)
{
	if (!content_.find(ContentLibrary::Kind::Scenario, scenarioId))
	{
		LOG("Free Touch: no scenario '{}' (ft_content_list shows the ids)", scenarioId);
		log_.write("rep_refused", {{"exercise", scenarioId}, {"reason", "unknown_scenario"}});
		return false;
	}
	difficulty_ = difficulty;
	streak_ = 0;
	frame_ = 1;
	frameThrow_ = 1;
	carryPins_ = false;
	freshRun_ = true;
	// Loops, trials and runs start from the first situation (Glade levels keep their progress).
	if (const auto* item = content_.find(ContentLibrary::Kind::Scenario, scenarioId))
	{
		const std::string prog = item->data.value("progression", "");
		if (prog == "loop" || prog == "trial" || prog == "run")
			level_[scenarioId] = 0;
	}
	// Starting something new cuts off any "forward" history, like a browser.
	if (historyIndex_ + 1 < static_cast<int>(history_.size()))
		history_.resize(historyIndex_ + 1);
	history_.push_back({scenarioId, seed.value_or(freshSeed())});
	historyIndex_ = static_cast<int>(history_.size()) - 1;
	return apply(history_.back(), "play");
}

bool ScenarioPlayer::retry()
{
	if (historyIndex_ < 0)
		return false;
	frameThrow_ = 1; // bowling: a retry restarts the frame
	carryPins_ = false;
	streak_ = 0;     // a retry is a fresh try at it: the streak starts over (Lambourne 2026-10-20)
	return apply(history_[historyIndex_], "retry");
}

bool ScenarioPlayer::next(bool keepCar)
{
	if (historyIndex_ < 0)
		return false;
	// In chained drills (Freeplay+, the circle chain) skipping is a chain too: live play carries on.
	if (!keepCar && current_ && onSuccess() == "chain")
		keepCar = true;
	// Levels: D-pad right (no result yet) skips to the next level; after a result the level was already set.
	if (current_ && !hasResult_ && (progression() == "levels" || progression() == "run" || progression() == "loop" || progression() == "trial"))
	{
		if (const auto* item = content_.find(ContentLibrary::Kind::Scenario, current_->id))
			level_[current_->id] = (level_[current_->id] + 1) % std::max<int>(1, static_cast<int>(levelIds(*item).size()));
	}
	const std::string id = history_[historyIndex_].scenarioId;
	if (historyIndex_ + 1 < static_cast<int>(history_.size()))
		history_.resize(historyIndex_ + 1);
	if (bowling() && hasResult_ && !frameDone_)
	{
		// Next throw of the frame: the same setup, with the pins that are down staying down.
		carryDown_.clear();
		for (const auto& o : objects_.objects())
		{
			if (o.type == ObjectType::Pin && !o.active)
				carryDown_.push_back(o.id);
		}
		carryPins_ = true;
		++frameThrow_;
		history_.push_back({id, history_[historyIndex_].seed});
		historyIndex_ = static_cast<int>(history_.size()) - 1;
		return apply(history_.back(), "throw");
	}
	if (bowling())
	{
		++frame_;
		frameThrow_ = 1;
		carryPins_ = false;
	}
	history_.push_back({id, keepCar ? chainSeed(id) : freshSeed()});
	historyIndex_ = static_cast<int>(history_.size()) - 1;
	// A chain can keep the live ball too (Speed Lab), unless it was exploded or is held in a goal.
	const bool keepBall = keepCar && current_ && current_->resolved.value("chainKeepsBall", false) && !ballExploded_ && !goalHeld_;
	return apply(history_.back(), keepCar ? "chain" : "next", keepCar, keepBall);
}

uint32_t ScenarioPlayer::chainSeed(const std::string& scenarioId)
{
	// A chained rep keeps the car where it is: pick a seed whose first objective is far enough away from it.
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, scenarioId);
	const Snapshot snap = game_.snapshot();
	const CarState* car = snap.localCar();
	uint32_t seed = freshSeed();
	if (!item || !car)
		return seed;
	const double minDistance = item->data.value("chainMinDistance", 2000.0);
	for (int attempt = 0; attempt < 40; ++attempt)
	{
		const ResolvedScenario r = scenario::resolve(item->data, seed, difficulty_, mirrorPolicy_, choiceFilter(scenarioId));
		std::vector<std::string> warnings;
		const auto objs = r.resolved.contains("objects") ? objects::fromJson(r.resolved["objects"], warnings)
			: std::vector<WorldObject>{};
		const auto first = std::find_if(objs.begin(), objs.end(), [](const WorldObject& o) { return objects::isGoal(o); });
		if (first == objs.end())
			return seed;
		const float dx = first->location.x - car->location.x, dy = first->location.y - car->location.y;
		if (std::sqrt(dx * dx + dy * dy) >= minDistance)
			return seed;
		seed = freshSeed();
	}
	return seed;
}

bool ScenarioPlayer::previous()
{
	if (historyIndex_ <= 0)
	{
		LOG("Free Touch: no earlier rep");
		return false;
	}
	--historyIndex_;
	return apply(history_[historyIndex_], "previous");
}

void ScenarioPlayer::waitForCar(const Entry& entry, const std::string& reason, int generation, int triesLeft)
{
	game_.runAfter(0.2f, [this, entry, reason, generation, triesLeft] {
		if (generation != applyGeneration_)
			return;
		if (game_.localCarReady())
			apply(entry, reason);
		else if (triesLeft > 0)
			waitForCar(entry, reason, generation, triesLeft - 1);
		else
			log_.write("rep_refused", {{"exercise", entry.scenarioId}, {"reason", "car_never_respawned"}});
	});
}

void ScenarioPlayer::stop()
{
	++applyGeneration_; // cancels a rep waiting for the car
	const bool wasActive = current_.has_value();
	endRep("stop");
	current_.reset();
	objects_.clear();
	leadInPhase_ = LeadInPhase::Off;
	countdownUntilTick_ = 0;
	showBall();
	if (ballExploded_)
	{
		game_.respawnBall();
		ballExploded_ = false;
	}
	if (wasActive && activeListener_)
		activeListener_(false);
}

void ScenarioPlayer::showBall()
{
	if (!ballHidden_)
		return;
	game_.setBallHidden(false);
	ballHidden_ = false;
	log_.write("ball_shown");
}

void ScenarioPlayer::endRep(const std::string& reason)
{
	if (!current_)
		return;
	log_.write("rep_end", {
		{"exercise", current_->id},
		{"rep", rep_},
		{"seed", current_->seed},
		{"reason", reason},
		{"seconds", (lastTick_ - repStartTick_) / kTicksPerSecond},
		{"touches", touches_},
		{"goal", goalSide_ == 0 ? "none" : goalSide_ > 0 ? "orange" : "blue"},
	});
}

bool ScenarioPlayer::apply(const Entry& entry, const std::string& reason, bool keepCar, bool keepBall)
{
	// Freeplay guard: exercises only start in offline freeplay.
	if (!game_.inFreeplay())
	{
		LOG("Free Touch: exercises only run in freeplay");
		log_.write("rep_refused", {{"exercise", entry.scenarioId}, {"reason", "not_in_freeplay"}});
		return false;
	}
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, entry.scenarioId);
	if (!item)
	{
		LOG("Free Touch: scenario '{}' is no longer loaded", entry.scenarioId);
		log_.write("rep_refused", {{"exercise", entry.scenarioId}, {"reason", "unknown_scenario"}});
		return false;
	}

	// Demolished (e.g. by a goal explosion in their own net): wait for the respawn, then start the rep. Any newer
	// apply or stop cancels the wait.
	const int generation = ++applyGeneration_;
	if (!game_.localCarReady())
	{
		log_.write("rep_waiting_for_car", {{"exercise", entry.scenarioId}, {"reason", reason}});
		waitForCar(entry, reason, generation, kCarWaitTries);
		return true;
	}
	// Skipped straight after a goal: a respawn sent during the explosion's first second is ignored by the game (the
	// ball stays gone), so the next rep starts once that second is over.
	constexpr double kExplosionHold = 1.15;
	if (ballExploded_ && lastTick_ < explodedAtTick_ + static_cast<uint64_t>(kExplosionHold * kTicksPerSecond))
	{
		const float wait = static_cast<float>((explodedAtTick_ + static_cast<uint64_t>(kExplosionHold * kTicksPerSecond) - lastTick_) / kTicksPerSecond);
		log_.write("rep_waiting_for_explosion", {{"exercise", entry.scenarioId}, {"reason", reason}, {"seconds", wait}});
		game_.runAfter(wait, [this, entry, reason, keepCar, keepBall, generation] {
			if (generation == applyGeneration_)
				apply(entry, reason, keepCar, keepBall);
		});
		return true;
	}

	endRep(reason);
	const bool wasActive = current_.has_value();

	modeNewRun(item->data, entry.scenarioId, reason);
	std::vector<std::string> filter = choiceFilter(entry.scenarioId);
	const std::string prog = item->data.value("progression", "");
	if (prog == "levels" || prog == "run" || prog == "loop" || prog == "trial" || prog == "horse" || prog == "combo")
	{
		// Levels / runs: this scenario's current level only.
		const auto ids = levelIds(*item);
		// A rep from the history (previous, retry) replays its own level; a new one records the level it plays.
		if (entry.level >= 0)
			level_[entry.scenarioId] = entry.level;
		else if (historyIndex_ >= 0 && history_[historyIndex_].level < 0 && history_[historyIndex_].scenarioId == entry.scenarioId
			&& history_[historyIndex_].seed == entry.seed)
			history_[historyIndex_].level = level_[entry.scenarioId];
		if (!ids.empty())
			filter = {ids[std::clamp(level_[entry.scenarioId], 0, static_cast<int>(ids.size()) - 1)]};
	}
	ResolvedScenario r = scenario::resolve(item->data, entry.seed, difficulty_, mirrorPolicy_, filter);
	addPackDefender(r);
	addPackTarget(r);
	addShotExtras(r);
	if (packStyleTargets_)
		targetsAsPackTiles(r);
	setupZones(r);
	placeNearCar(r, keepCar);
	if (r.resolved.contains("match") && (reason == "play" || matchOver_ || matchScenario_ != r.id))
	{
		// A new match: 0 - 0, a full clock.
		matchScenario_ = r.id;
		scoreYou_ = scoreBot_ = 0;
		matchSecondsLeft_ = r.resolved["match"].value("minutes", 3.0) * 60.0;
		overtimeSeconds_ = 0.0;
		overtime_ = matchOver_ = clockZeroLogged_ = false;
		log_.write("match_start", {{"exercise", r.id}, {"minutes", matchSecondsLeft_ / 60.0}, {"difficulty", difficulty_}});
	}
	const bool freeze = r.ballFreezeSeconds > 0.0 || r.ballHidden;
	BallTarget ball = r.ball;
	if (freeze)
		ball.velocity = ball.angularVelocity = {};

	if (ballExploded_)
	{
		game_.respawnBall(); // a fresh ball; the exploded one ignores moves
		ballExploded_ = false;
		log_.write("ball_respawned", {{"exercise", r.id}});
	}
	// Show a hidden ball before placing it: un-hiding snaps it back to where it was parked (keeping the new
	// velocity), so it is placed again on the next tick too (One Tank: a save right after a ballless situation).
	const bool wasHidden = ballHidden_;
	if (!r.ballHidden)
		showBall();
	const bool ballOk = keepBall || game_.setBall(ball); // a chain may leave the live ball alone
	if (r.ballHidden && !ballHidden_)
		ballHidden_ = game_.setBallHidden(true);
	reapplyBallTick_ = wasHidden && !r.ballHidden && !keepBall ? lastTick_ + 1 : 0;
	// One tank: in a loop only the very first rep sets the boost; in a trial only the start of an attempt.
	const int levelNow = level_[entry.scenarioId];
	if ((prog == "loop" && reason != "play") || (prog == "trial" && !(levelNow == 0 && (freshRun_ || reason == "play"))))
		r.player.boost.reset();
	if (prog == "trial" && levelNow == 0 && (freshRun_ || reason == "play"))
	{
		trialStartTick_ = lastTick_;
		freshRun_ = false;
	}
	const bool carOk = keepCar || game_.setLocalCar(r.player); // a chained rep leaves the car as it is

	// A freeplay reset put back (onRoundReset's fallback) is the same rep, not a new one.
	if (reason != "game_reset")
		++rep_;
	recent_.clear(); // an undone reset goes back within this rep, never into the one before
	repStartTick_ = lastTick_;
	touches_ = 0;
	goalHeld_ = false;
	goalLetThrough_ = false;
	realGoalPending_ = false;
	goalSide_ = 0;
	goalCheckFromTick_ = lastTick_ + 3;
	hasResult_ = false;
	resultNewBest_ = false;
	lastTouchByBot_ = false;
	prevBall_.reset();
	saveWatchFromTick_ = 0;
	outTicks_ = 0;
	keeperWatchFromTick_ = 0;
	keeperOutTicks_ = 0;
	lastPlayerTouchTick_ = lastBotTouchTick_ = 0;
	firstPlayerTouchTick_ = firstBotTouchTick_ = 0;
	ctl_ = Control{};
	shadow_ = Shadow{};
	coach_ = SpeedFlipCoach{};
	coachReported_ = false;
	coachLine_.clear();
	keeperContested_ = false;
	resetModeRep();
	outcome_ = 0;
	resultLabel_.clear();
	resultDetail_.clear();
	resultKmh_ = 0.f;
	freezeUntilTick_ = r.ballHidden ? UINT64_MAX
		: freeze && !keepBall ? lastTick_ + static_cast<uint64_t>(r.ballFreezeSeconds * kTicksPerSecond) : 0;
	released_ = !freeze || keepBall;
	// Saved Shots: replay the seconds before the capture, then freeze on it until the player presses something.
	leadIn_ = r.resolved.contains("leadIn") && !keepBall && !keepCar ? leadin::fromJson(r.resolved["leadIn"])
		: leadin::Track{};
	leadInPhase_ = leadIn_.frames.size() >= 2 ? LeadInPhase::Playing : LeadInPhase::Off;
	leadInStartTick_ = lastTick_;
	// Training packs: frozen on the shot's start until the player's first input (any button or stick).
	// "readFirst" (HORSE, the combo ladders, every Daily Challenge shot): the same, with the objective left on screen to
	// read and a fresh press needed (a held throttle does not start it).
	readFirst_ = r.resolved.value("readFirst", false) || forceReadFirst_;
	waitForInput_ = leadInPhase_ == LeadInPhase::Off && (r.resolved.value("waitForInput", false) || readFirst_) && !keepBall && !keepCar;
	if (waitForInput_)
	{
		leadInPhase_ = LeadInPhase::Waiting;
		leadInArmed_ = false;
	}
	timeUpTick_ = 0;
	if (leadInPhase_ == LeadInPhase::Playing)
	{
		const leadin::Frame first = leadIn_.frames.front();
		game_.setBall(first.ball);
		game_.poseLocalCar(first.car);
	}
	// A countdown before the play starts (startCountdownSeconds): everything frozen at its start, then released.
	const double countdown = r.resolved.value("startCountdownSeconds", 0.0);
	countdownUntilTick_ = countdown > 0.0 && !keepBall && !keepCar && leadInPhase_ == LeadInPhase::Off
		? lastTick_ + static_cast<uint64_t>(countdown * kTicksPerSecond) : 0;
	countdownGiveUpTick_ = countdownUntilTick_ != 0 ? countdownUntilTick_ + static_cast<uint64_t>(10.0 * kTicksPerSecond) : 0;
	countdownWaitLogged_ = false;

	using statejson::toJson;
	nlohmann::ordered_json fields = {
		{"exercise", r.id},
		{"rep", rep_},
		{"seed", r.seed},
		{"difficulty", r.difficulty},
		{"reason", reason},
		{"mirrored", r.mirrored},
		{"ball", {{"location", toJson(r.ball.location)}, {"velocity", toJson(r.ball.velocity)},
			{"angularVelocity", toJson(r.ball.angularVelocity)}, {"freezeSeconds", r.ballFreezeSeconds},
			{"hidden", r.ballHidden}}},
		{"player", {{"location", toJson(r.player.location)}, {"rotation", toJson(r.player.rotation)},
			{"velocity", toJson(r.player.velocity)},
			{"boost", r.player.boost ? nlohmann::ordered_json(*r.player.boost) : nlohmann::ordered_json(nullptr)}}},
		{"timeLimitSeconds", r.timeLimitSeconds},
		{"applied", ballOk && carOk},
		{"keptCar", keepCar},
		{"keptBall", keepBall},
	};
	if (r.aimAtUsed)
		fields["aimAtReachable"] = r.aimAtReachable;
	if (leadInPhase_ != LeadInPhase::Off)
		fields["leadInSeconds"] = leadIn_.seconds();
	if (r.resolved.contains("situation"))
	{
		const auto& sit = r.resolved["situation"];
		fields["play"] = sit.value("variant", sit.value("carry", sit.value("type", std::string())));
	}
	std::vector<std::string> objectWarnings;
	objects_.setObjects(r.resolved.contains("objects") ? objects::fromJson(r.resolved["objects"], objectWarnings)
		: std::vector<WorldObject>{});
	if (carryPins_)
	{
		for (const auto& pinId : carryDown_)
			objects_.setHit(pinId);
		carryPins_ = false;
	}
	else if (reason != "throw")
	{
		frameThrow_ = 1;
	}
	frameDone_ = false;
	pendingTouchTick_ = 0;
	touchStreak_ = 0;
	dribbleSeconds_ = 0.0;
	dribbleLostTick_ = 0;
	pinsDownAtStart_ = pinsDown();
	lastPinHitTick_ = 0;
	slowSinceTick_ = 0;
	if (!objects_.empty())
		fields["objects"] = objects_.objects().size();
	if (r.resolved.contains("bowling"))
		fields["bowling"] = {{"frame", frame_}, {"throw", frameThrow_}, {"pinsDownAtStart", pinsDownAtStart_}};
	r.warnings.insert(r.warnings.end(), objectWarnings.begin(), objectWarnings.end());
	if (!r.warnings.empty())
		fields["warnings"] = r.warnings;
	log_.write("rep_start", fields);

	LOG("Free Touch: rep {} {} ({}) seed {}{}{}", rep_, r.id, r.difficulty, r.seed, r.mirrored ? " mirrored" : "",
		reason == "play" ? "" : " [" + reason + "]");
	for (const auto& w : r.warnings)
		LOG("  warning: {}", w);

	current_ = std::move(r);
	ghostRun_.clear();
	padBoost_ = -1.f;
	ghostLabel_.clear();
	ghostBest_ = ghost::Run{};
	if (ghostOn())
	{
		// A friend's ghost picked on the Races page, for this race; else your own best.
		const std::string key = capture::sanitizeId(bestKey());
		if (!ghostOverrideKey_.empty() && ghostOverrideKey_ != key)
			clearGhostOverride();
		if (!ghostOverride_.frames.empty())
		{
			ghostBest_ = ghostOverride_;
			ghostLabel_ = ghostOverrideLabel_;
		}
		else if (ghostLoad_)
			ghostBest_ = ghostLoad_(bestKey());
	}
	if (repStartListener_)
		repStartListener_(*current_);

	// Where did things really end up? (Moves land a physics step later; 0.1 s is several steps.) Shows when the
	// game overrides a rep, e.g. with its own kickoff reset.
	const int checkRep = rep_;
	game_.runAfter(0.1f, [this, checkRep] {
		if (!current_ || rep_ != checkRep)
			return;
		const Snapshot s = game_.snapshot();
		nlohmann::ordered_json f = {{"exercise", current_->id}, {"rep", checkRep}};
		if (s.ball)
		{
			f["ball"] = statejson::toJson(s.ball->location);
			f["ballOffFromStart"] = statejson::round2((s.ball->location - current_->ball.location).length());
		}
		if (const CarState* car = s.localCar())
		{
			f["car"] = statejson::toJson(car->location);
			f["carOffFromStart"] = statejson::round2((car->location - current_->player.location).length());
		}
		log_.write("rep_check", f);
	});

	if (!wasActive && activeListener_)
		activeListener_(true);
	return ballOk && carOk;
}

void ScenarioPlayer::onTick(const Snapshot& snap)
{
	lastTick_ = snap.tick;
	if (!current_)
		return;
	// The last few ticks of ball and car, to undo a freeplay reset (onRoundReset).
	if (snap.ball && snap.localCar())
	{
		recent_.push_back({snap.tick, *snap.ball, *snap.localCar()});
		while (recent_.size() > 16)
			recent_.pop_front();
	}
	if (leadInPhase_ != LeadInPhase::Off)
	{
		tickLeadIn(snap); // nothing is judged until the shot is live
		return;
	}
	if (countdownUntilTick_ != 0)
	{
		tickCountdown(snap);
		return;
	}

	// Saves: after the player's touch, judge every tick. A ball that is no longer heading into the defended net for
	// a few ticks in a row is a save (this also catches a pinch off the post the moment it bounces out); a ball that
	// still goes in is caught on its last tick below and is CONCEDED.
	const bool saveRep = successType() == "save";
	// The player's own net comes from their team (blue in freeplay), not their start: drills can start them anywhere.
	if (const CarState* me = snap.localCar())
		defendedNet_ = me->team == 1 ? 1 : -1;
	const int defended = defendedNet_;
	float goesInAfter = -2.f; // -2 = not predicted this tick
	if (saveRep && !hasResult_ && snap.ball && touches_ > 0 && snap.tick >= saveWatchFromTick_)
	{
		goesInAfter = ballpredict::timeToGoal(*snap.ball, defended);
		outTicks_ = goesInAfter < 0.f ? outTicks_ + 1 : 0;
		if (outTicks_ >= kSaveConfirmTicks)
		{
			log_.write("save_judged", {
				{"exercise", current_->id},
				{"rep", rep_},
				{"touches", touches_},
				{"ballLocation", statejson::toJson(snap.ball->location)},
				{"ballVelocity", statejson::toJson(snap.ball->velocity)},
			});
			setResult(+1, "SAVED", 0.f);
		}
	}

	// Keeper saves (scenarios that fail on "save"): after the bot's touch (the last one; see onTouch for 50s), a ball
	// that is no longer heading into the net the player attacks, for a quarter of a second in a row, was saved.
	if (!hasResult_ && keeperWatchFromTick_ != 0 && snap.tick >= keeperWatchFromTick_ && snap.ball && failHas("save"))
	{
		const float goesIn = ballpredict::timeToGoal(*snap.ball, -defended);
		keeperOutTicks_ = goesIn < 0.f ? keeperOutTicks_ + 1 : 0;
		if (keeperOutTicks_ >= kKeeperConfirmTicks)
		{
			log_.write("keeper_save_judged", {
				{"exercise", current_->id},
				{"rep", rep_},
				{"contested", keeperContested_},
				{"ballLocation", statejson::toJson(snap.ball->location)},
				{"ballVelocity", statejson::toJson(snap.ball->velocity)},
			});
			setResult(-1, "KEEPER SAVE", 0.f);
		}
	}

	// Ghost recording: the car's pose 30 times a second while the run is on.
	if (ghostOn() && !hasResult_ && snap.tick >= repStartTick_ && (snap.tick - repStartTick_) % 4 == 0)
	{
		if (const CarState* me = snap.localCar())
			ghostRun_.add(static_cast<float>((snap.tick - repStartTick_) / kTicksPerSecond), me->location, me->rotation);
	}
	if (!hasResult_ && isMatch())
		tickMatch(snap);
	tickModes(snap);
	if (!hasResult_ && successType() == "kickoff")
		judgeKickoff(snap);
	if (!hasResult_ && successType() == "fifty")
		judgeFifty(snap);
	if (!hasResult_ && successType() == "possession")
		judgeControl(snap);
	if (!hasResult_ && successType() == "shadow")
		judgeShadow(snap);
	tickCoach(snap);

	// Time limit: judge the rep once when the clock runs out. A save rep whose ball is still heading in waits for
	// the ball (it ends as a goal or as a save).
	if (!hasResult_ && current_->timeLimitSeconds > 0.0
		&& (lastTick_ - repStartTick_) / kTicksPerSecond >= current_->timeLimitSeconds)
	{
		if (saveRep)
		{
			if (goesInAfter == -2.f && snap.ball)
				goesInAfter = ballpredict::timeToGoal(*snap.ball, defended);
			if (goesInAfter < 0.f)
				setResult(touches_ > 0 ? +1 : 0, touches_ > 0 ? "SAVED" : "WIDE", 0.f);
		}
		else if (bowling())
		{
			finishThrow();
		}
		else if (current_->resolved.value("timeUpWaitsForGround", false) && snap.ball)
		{
			// Like the game's training: when time is up the shot plays on until the ball touches the ground (or 4 s).
			if (timeUpTick_ == 0)
			{
				timeUpTick_ = snap.tick;
				log_.write("time_up_waiting", {{"exercise", current_->id}, {"rep", rep_}, {"ballZ", snap.ball->location.z}});
			}
			if (snap.ball->location.z < 110.f || snap.tick > timeUpTick_ + static_cast<uint64_t>(4.0 * kTicksPerSecond))
				setResult(-1, "TIME", 0.f);
		}
		else
		{
			setResult(-1, "TIME", 0.f);
		}
	}

	logWallBounce(snap);
	if (!objects_.empty() && snap.tick >= goalCheckFromTick_) // the first ticks still show the previous rep
		judgeObjects(snap);
	if (reapplyBallTick_ != 0 && snap.tick >= reapplyBallTick_)
	{
		reapplyBallTick_ = 0;
		if (!freezeUntilTick_ || snap.tick >= freezeUntilTick_)
			game_.setBall(current_->ball);
		log_.write("ball_reapplied", {{"exercise", current_->id}, {"rep", rep_}});
	}
	if (snap.tick >= goalCheckFromTick_)
		judgeSkill(snap);
	// Unlimited boost (rings): top the tank up whenever it drops.
	bool boostLock = current_->resolved["player"].value("boostLock", false);
	if (const auto o = boostOverride_.find(current_->id); o != boostOverride_.end())
		boostLock = o->second;
	if (boostLock && snap.tick >= goalCheckFromTick_)
	{
		if (const CarState* me = snap.localCar(); me && me->boost < 99.f)
			game_.setLocalBoost(100.f);
	}
	// No pads: once the run is live, any rise in the tank is a pickup and is taken back.
	if (!boostLock && current_->resolved["player"].value("noPads", false) && countdownUntilTick_ == 0
		&& snap.tick >= goalCheckFromTick_ && !hasResult_)
	{
		if (const CarState* me = snap.localCar())
		{
			if (padBoost_ >= 0.f && me->boost > padBoost_ + 0.5f)
			{
				game_.setLocalBoost(padBoost_);
				log_.write("pad_refused", {{"exercise", current_->id}, {"rep", rep_}, {"boost", me->boost}, {"kept", padBoost_}});
			}
			else
			{
				padBoost_ = me->boost;
			}
		}
	}

	if (goalHeld_)
	{
		game_.setBall({goalHoldAt_, {}, {}});
		return;
	}
	if (snap.tick >= goalCheckFromTick_)
		blockZoneMiss(snap);
	if (released_ && !ballHidden_ && !ballExploded_ && !goalLetThrough_ && snap.ball && snap.tick >= goalCheckFromTick_)
	{
		// One catch point for every goal: the last tick before the whole ball is over the line.
		const auto c = goalguard::check(*snap.ball);
		if (c.caught)
		{
			goalSide_ = c.side;
			goalHoldAt_ = c.holdAt;
			const bool attackingNet = c.side != defended;
			// "Goal replays" on: the goal goes in for real (the game's explosion, slow motion and replay); the rep is
			// judged now and moves on after freeplay's reset that follows the replay (onRoundReset).
			if (goalReplays_)
			{
				goalLetThrough_ = true;
				realGoalPending_ = true;
			}
			// The explosion: the player's own for their goals, the default one (no scorer) for goals against them.
			else if (goalExplosion_ && game_.explodeBall(c.side, attackingNet))
			{
				ballExploded_ = true;
				explodedAtTick_ = snap.tick;
			}
			else
			{
				goalHeld_ = true;
				game_.setBall({goalHoldAt_, {}, {}});
			}
			const float speed = snap.ball->velocity.length();
			log_.write("goal_intercepted", {
				{"exercise", current_->id},
				{"rep", rep_},
				{"net", c.side > 0 ? "orange" : "blue"},
				{"ballSpeed", statejson::round2(speed)},
				{"ballKmh", statejson::round2(speed * 0.036f)},
				{"ballLocation", statejson::toJson(snap.ball->location)},
				{"exploded", ballExploded_},
				{"letThrough", goalLetThrough_},
				{"touches", touches_},
			});
			LOG("Free Touch: GOAL ({} net) at {:.0f} km/h, rep {}", c.side > 0 ? "orange" : "blue", speed * 0.036f, rep_);
			const std::string success = successType();
			if (modeGoal(c.side, snap.ball->location, speed * 0.036f))
				return;
			if (isMatch() && !hasResult_)
			{
				(attackingNet ? scoreYou_ : scoreBot_) += 1;
				log_.write("match_goal", {{"exercise", current_->id}, {"rep", rep_}, {"by", attackingNet ? "you" : "bot"},
					{"you", scoreYou_}, {"bot", scoreBot_}, {"clock", statejson::round2(static_cast<float>(matchSecondsLeft_))},
					{"overtime", overtime_}});
				const int firstTo = current_->resolved["match"].value("firstTo", 0);
				if (overtime_ || (firstTo > 0 && std::max(scoreYou_, scoreBot_) >= firstTo))
				{
					endMatch(overtime_ ? "overtime goal" : "first to");
					return;
				}
				{
					char buf[32];
					const int left = static_cast<int>(std::ceil(matchSecondsLeft_));
					snprintf(buf, sizeof(buf), "%d:%02d LEFT", left / 60, left % 60);
					resultDetail_ = scoreText() + "   |   " + buf;
				}
				setResult(attackingNet ? +1 : -1, attackingNet ? "GOAL" : "CONCEDED", speed * 0.036f);
				return;
			}
			if (attackingNet)
			{
				if (success == "goal" || success == "goal_in_zone" || success == "kickoff" || success == "fifty" || success == "possession")
					setResult(+1, "GOAL", speed * 0.036f);
				else
					setResult(0, "GOAL", speed * 0.036f);
			}
			else if (saveRep || failHas("conceded") || lastTouchByBot_)
				setResult(-1, "CONCEDED", speed * 0.036f);
			else
				setResult(0, "OWN GOAL", speed * 0.036f);
			return;
		}
	}
	if (released_)
		return;

	if (snap.tick < freezeUntilTick_)
	{
		// Hold the ball where the scenario put it (a moved ball would otherwise fall or drift).
		BallTarget hold = current_->ball;
		hold.velocity = hold.angularVelocity = {};
		// A hidden ball can mark the current objective instead, so ball cam points the way (next ring, the circle).
		if (ballHidden_ && current_->resolved["ball"].value("markObjective", false))
		{
			// Smart ball cam: point at the next objective while it is far; inside 1500 uu slide toward the one
			// after (markAhead), so the camera turns into the route before the car passes through.
			const int ahead = current_->resolved["ball"].value("markAhead", 0);
			const auto near = objects_.objectiveMarker(0);
			const auto far = ahead > 0 ? objects_.objectiveMarker(ahead) : near;
			const CarState* me = snap.localCar();
			if (near && far && me)
			{
				const float d = (*near - me->location).length();
				const float t = std::clamp((1500.f - d) / 1000.f, 0.f, 1.f);
				hold.location = *near + (*far - *near) * t;
			}
			else if (near)
			{
				hold.location = *near;
			}
		}
		game_.setBall(hold);
		return;
	}
	game_.setBall(current_->ball);
	released_ = true;
	log_.write("ball_released", {{"exercise", current_->id}, {"rep", rep_}});
}

void ScenarioPlayer::tickCountdown(const Snapshot& snap)
{
	CarTarget car = current_->player;
	car.boost.reset();
	if (snap.tick >= countdownUntilTick_ && startGate_ && !startGate_() && snap.tick < countdownGiveUpTick_)
	{
		// The count is over but a bot is not in place yet (it is still spawning): hold everyone a little longer.
		if (!countdownWaitLogged_)
		{
			countdownWaitLogged_ = true;
			log_.write("countdown_waiting_for_bots", {{"exercise", current_->id}, {"rep", rep_}});
		}
		countdownUntilTick_ = snap.tick + 1;
	}
	if (snap.tick < countdownUntilTick_)
	{
		// Frozen at the start until the count ends. Holding boost while frozen would empty the tank: keep it topped
		// back up to the start amount, but only once the teleport has landed (it travels with a full tank so it
		// cannot pick up pads on the way; setLocalCar drops it to the start amount after that).
		game_.setBall({current_->ball.location, {}, {}});
		car.velocity = car.angularVelocity = {};
		game_.poseLocalCar(car);
		if (current_->player.boost && snap.tick > repStartTick_ + static_cast<uint64_t>(0.25 * kTicksPerSecond))
			game_.setLocalBoost(*current_->player.boost);
		return;
	}
	// Go: everything moves as the play was set up, and the rep (clock, touches, goals) starts now. The car is NOT posed
	// again: the countdown already holds it on its spot, and a last teleport (which lands a tick late, with zero speed)
	// ate the first moment of the player's held throttle and boost (kickoffs felt slow off the line, Lambourne
	// 2026-10-20). A car that starts moving (velocity in the setup) still gets it.
	game_.setBall(current_->ball);
	if (current_->player.velocity.length() > 1.f)
		game_.poseLocalCar(car);
	if (current_->player.boost)
		game_.setLocalBoost(*current_->player.boost);
	countdownUntilTick_ = 0;
	repStartTick_ = snap.tick;
	goalCheckFromTick_ = snap.tick + 3;
	touches_ = 0;
	touchStreak_ = 0;
	lastTouchByBot_ = false;
	prevBall_.reset();
	saveWatchFromTick_ = 0;
	outTicks_ = 0;
	keeperWatchFromTick_ = 0;
	lastPlayerTouchTick_ = lastBotTouchTick_ = 0;
	firstPlayerTouchTick_ = firstBotTouchTick_ = 0;
	ctl_ = Control{};
	shadow_ = Shadow{};
	coach_ = SpeedFlipCoach{};
	coachReported_ = false;
	coachLine_.clear();
	resetModeRep();
	ghostRun_.clear();
	padBoost_ = -1.f;
	log_.write("countdown_done", {{"exercise", current_->id}, {"rep", rep_}, {"waitedForBots", countdownWaitLogged_}});
}

void ScenarioPlayer::judgeKickoff(const Snapshot& snap)
{
	// A kickoff is judged `judgeSeconds` after the first touch (anyone's): by then the ball has come out of the 50.
	const uint64_t first = firstPlayerTouchTick_ && firstBotTouchTick_ ? std::min(firstPlayerTouchTick_, firstBotTouchTick_)
		: std::max(firstPlayerTouchTick_, firstBotTouchTick_);
	if (first == 0 || !snap.ball || countdownUntilTick_ != 0)
		return;
	const nlohmann::json& success = current_->resolved["success"];
	const double judge = success.value("judgeSeconds", 2.5);
	if (snap.tick < first + static_cast<uint64_t>(judge * kTicksPerSecond))
		return;

	// Where the ball is heading, in the player's attacking direction (+ = toward the net they attack).
	const float dir = defendedNet_ < 0 ? 1.f : -1.f;
	const BallState& b = *snap.ball;
	const float ahead = dir * (b.location.y + b.velocity.y * static_cast<float>(success.value("lookAheadSeconds", 0.7)));
	const float margin = static_cast<float>(success.value("winMargin", 1100.0));
	// Who is nearer the ball (straight-line distance, so a car already facing it and moving counts the same).
	const CarState* me = snap.localCar();
	const CarState* bot = nullptr;
	for (const auto& c : snap.cars)
		if (c.isBot && (!bot || (c.location - b.location).length() < (bot->location - b.location).length()))
			bot = &c;
	const float myDist = me ? (me->location - b.location).length() : 1e9f;
	const float botDist = bot ? (bot->location - b.location).length() : 1e9f;
	const float closer = static_cast<float>(success.value("possessionMargin", 350.0));
	const int possession = myDist + closer < botDist ? 1 : botDist + closer < myDist ? -1 : 0;

	int outcome = 0;
	std::string label;
	if (ahead > margin)
	{
		outcome = 1;
		label = "WON";
	}
	else if (ahead < -margin)
	{
		outcome = -1;
		label = "LOST";
	}
	else
	{
		// The ball stayed in the middle: whoever gets it first has the kickoff.
		outcome = possession;
		label = possession > 0 ? "WON" : possession < 0 ? "LOST" : "50 / 50";
	}

	auto secs = [&](uint64_t tick) {
		char buf[24];
		snprintf(buf, sizeof(buf), "%.2f S", (tick - repStartTick_) / kTicksPerSecond);
		return std::string(buf);
	};
	std::string detail;
	if (firstPlayerTouchTick_ && (!firstBotTouchTick_ || firstPlayerTouchTick_ <= firstBotTouchTick_))
		detail = "FIRST TOUCH YOU " + secs(firstPlayerTouchTick_);
	else if (firstBotTouchTick_)
		detail = "FIRST TOUCH BOT " + secs(firstBotTouchTick_) + (firstPlayerTouchTick_ ? "   |   YOU " + secs(firstPlayerTouchTick_) : "");
	detail += possession > 0 ? "   |   YOUR BALL" : possession < 0 ? "   |   ITS BALL" : "";
	if (ahead > margin)
		detail += "   |   INTO ITS HALF";
	else if (ahead < -margin)
		detail += "   |   INTO YOUR HALF";

	log_.write("kickoff_judged", {
		{"exercise", current_->id},
		{"rep", rep_},
		{"play", current_->resolved.contains("situation") ? current_->resolved["situation"].value("variant", "") : ""},
		{"label", label},
		{"outcome", outcome},
		{"playerFirstTouch", firstPlayerTouchTick_ ? (firstPlayerTouchTick_ - repStartTick_) / kTicksPerSecond : -1.0},
		{"botFirstTouch", firstBotTouchTick_ ? (firstBotTouchTick_ - repStartTick_) / kTicksPerSecond : -1.0},
		{"ballAhead", statejson::round2(ahead)},
		{"playerDistance", statejson::round2(myDist)},
		{"botDistance", statejson::round2(botDist)},
		{"possession", possession},
		{"ballLocation", statejson::toJson(b.location)},
		{"ballVelocity", statejson::toJson(b.velocity)},
	});
	resultDetail_ = detail;
	setResult(outcome, label, 0.f);
}

std::string ScenarioPlayer::scoreText() const
{
	return "YOU " + std::to_string(scoreYou_) + " - " + std::to_string(scoreBot_) + " BOT";
}

void ScenarioPlayer::tickMatch(const Snapshot& snap)
{
	// The clock runs while the ball is in play: not in the countdown (returned before this) and not after a goal.
	if (!released_ || goalHeld_ || ballExploded_ || snap.paused)
		return;
	const double dt = 1.0 / kTicksPerSecond;
	if (overtime_)
	{
		overtimeSeconds_ += dt;
		return;
	}
	matchSecondsLeft_ = std::max(0.0, matchSecondsLeft_ - dt);
	if (matchSecondsLeft_ > 0.0)
		return;
	if (!clockZeroLogged_)
	{
		clockZeroLogged_ = true;
		log_.write("match_clock_zero", {{"exercise", current_->id}, {"you", scoreYou_}, {"bot", scoreBot_}});
	}
	if (scoreYou_ == scoreBot_)
	{
		// Tied at 0:00: overtime, the next goal wins.
		overtime_ = true;
		log_.write("match_overtime", {{"exercise", current_->id}, {"score", scoreYou_}});
		return;
	}
	// As in the game: at 0:00 the match ends when the ball next touches the floor.
	if (snap.ball && snap.ball->location.z < 110.f)
		endMatch("time");
}

void ScenarioPlayer::endMatch(const char* why)
{
	matchOver_ = true;
	const bool won = scoreYou_ > scoreBot_;
	log_.write("match_end", {{"exercise", current_->id}, {"you", scoreYou_}, {"bot", scoreBot_}, {"why", why},
		{"overtime", overtime_}, {"overtimeSeconds", statejson::round2(static_cast<float>(overtimeSeconds_))}});
	resultDetail_ = scoreText() + (overtime_ ? "   |   IN OVERTIME" : "") + "   |   NEW MATCH IN 6 S";
	setResult(won ? +1 : -1, won ? "YOU WIN" : "YOU LOSE", 0.f);
}

std::string ScenarioPlayer::firstTouchText() const
{
	auto secs = [&](uint64_t tick) {
		char buf[24];
		snprintf(buf, sizeof(buf), "%.2f S", (tick - repStartTick_) / kTicksPerSecond);
		return std::string(buf);
	};
	if (firstPlayerTouchTick_ && (!firstBotTouchTick_ || firstPlayerTouchTick_ <= firstBotTouchTick_))
		return "FIRST TOUCH YOU " + secs(firstPlayerTouchTick_) + (firstBotTouchTick_ ? "   |   BOT " + secs(firstBotTouchTick_) : "");
	if (firstBotTouchTick_)
		return "FIRST TOUCH BOT " + secs(firstBotTouchTick_) + (firstPlayerTouchTick_ ? "   |   YOU " + secs(firstPlayerTouchTick_) : "");
	return "";
}

namespace
{
	// A rough time (s) for a car to reach `at`: its distance, which way it faces, how fast it is already going that way
	// and whether it is in the air (straight-line distance alone called 50s lost that the player was about to collect).
	float etaToBall(const CarState* c, const Vec3& at)
	{
		if (!c)
			return 99.f;
		const float dx = at.x - c->location.x, dy = at.y - c->location.y;
		const float d = std::max(1.f, std::hypot(dx, dy));
		const float yaw = c->rotation.yaw * 3.14159265f / 32768.f;
		const float facing = (std::cos(yaw) * dx + std::sin(yaw) * dy) / d;
		const float towards = (c->velocity.x * dx + c->velocity.y * dy) / d;
		float t = d / 1400.f - std::clamp(towards, 0.f, 2300.f) / 2300.f * 0.25f;
		t += facing < 0.f ? 0.55f : facing < 0.6f ? 0.2f : 0.f;
		t += c->onGround ? 0.f : 0.25f;
		return std::max(0.f, t);
	}
}

void ScenarioPlayer::judgeFifty(const Snapshot& snap)
{
	const uint64_t first = firstPlayerTouchTick_ && firstBotTouchTick_ ? std::min(firstPlayerTouchTick_, firstBotTouchTick_)
		: std::max(firstPlayerTouchTick_, firstBotTouchTick_);
	if (first == 0 || !snap.ball || countdownUntilTick_ != 0)
		return;
	const nlohmann::json& success = current_->resolved["success"];
	const nlohmann::json& sit = current_->resolved["situation"];
	const std::string aim = sit.value("aim", "corner");
	const BallState& b = *snap.ball;
	const float dir = defendedNet_ < 0 ? 1.f : -1.f;
	const float contactY = sit.contains("contact") ? sit["contact"][1].get<float>() : 0.f;

	// Back to a teammate: the ball reaching the circle (low enough for them to play it) is the result at once.
	if (aim == "mate" && firstPlayerTouchTick_ && sit.contains("mate"))
	{
		const float mx = sit["mate"][0].get<float>(), my = sit["mate"][1].get<float>();
		const float radius = static_cast<float>(success.value("mateRadius", 750.0));
		if (std::hypot(b.location.x - mx, b.location.y - my) < radius && b.location.z < 700.f)
		{
			log_.write("fifty_judged", {{"exercise", current_->id}, {"rep", rep_}, {"aim", aim}, {"label", "TO YOUR MATE"},
				{"outcome", 1}, {"ballLocation", statejson::toJson(b.location)}});
			resultDetail_ = firstTouchText();
			setResult(+1, "TO YOUR MATE", 0.f);
			return;
		}
	}
	const double window = aim == "mate" ? success.value("mateSeconds", 3.0) : success.value("judgeSeconds", 1.6);
	if (snap.tick < first + static_cast<uint64_t>(window * kTicksPerSecond))
		return;

	const float look = static_cast<float>(success.value("lookAheadSeconds", 0.7));
	const Vec3 proj(b.location.x + b.velocity.x * look, b.location.y + b.velocity.y * look, b.location.z);
	const float ahead = dir * (proj.y - contactY);  // forward from the 50
	const float depth = dir * proj.y;               // + their half, - yours
	const bool danger = ballpredict::timeToGoal(b, defendedNet_) >= 0.f
		|| (std::fabs(proj.x) < 1800.f && depth < -2600.f); // in front of your net
	// Who gets the ball next: a rough time for each car to reach where the ball is going, from its distance, which way
	// it faces, how fast it is already going that way and whether it is in the air (straight-line distance alone
	// called 50s lost that the player was about to collect).
	const CarState* me = snap.localCar();
	const CarState* bot = nullptr;
	for (const auto& c : snap.cars)
		if (c.isBot && (!bot || (c.location - b.location).length() < (bot->location - b.location).length()))
			bot = &c;
	const float myEta = etaToBall(me, proj), botEta = etaToBall(bot, proj);
	const int possession = myEta + 0.2f < botEta ? 1 : botEta + 0.2f < myEta ? -1 : 0;

	// Getting the ball next is winning the 50; the aim decides the label (and the corner / safe placements count even
	// when the bot gets to it, since that is the point of them).
	int outcome = 0;
	std::string label;
	const bool corner = std::fabs(proj.x) > 2300.f && depth > 2000.f;
	const bool safePlace = std::fabs(proj.x) > 2300.f || ahead > 700.f;
	if (!firstPlayerTouchTick_)
	{
		outcome = -1;
		label = "BEATEN";
	}
	else if (danger && possession <= 0)
	{
		outcome = -1;
		label = "DANGER";
	}
	else if (danger)
	{
		outcome = 0;
		label = "YOUR BALL, CLOSE CALL";
	}
	else if (aim == "corner" && corner && ahead > 700.f)
	{
		outcome = 1;
		label = "INTO THE CORNER";
	}
	else if (aim == "safe" && safePlace)
	{
		outcome = 1;
		label = "SAFE";
	}
	else if (possession > 0)
	{
		outcome = aim == "mate" ? 0 : 1;
		label = "YOUR BALL";
	}
	else if (possession < 0)
	{
		outcome = -1;
		label = aim == "mate" ? "MISSED YOUR MATE" : ahead < -700.f ? "LOST IT" : "ITS BALL";
	}
	else
	{
		outcome = aim == "mate" ? -1 : 0;
		label = aim == "mate" ? "MISSED YOUR MATE" : ahead > 700.f ? "FORWARD" : "50 / 50";
	}

	log_.write("fifty_judged", {
		{"exercise", current_->id},
		{"rep", rep_},
		{"aim", aim},
		{"play", sit.value("variant", "")},
		{"carry", sit.value("carry", "")},
		{"label", label},
		{"outcome", outcome},
		{"playerFirstTouch", firstPlayerTouchTick_ ? (firstPlayerTouchTick_ - repStartTick_) / kTicksPerSecond : -1.0},
		{"botFirstTouch", firstBotTouchTick_ ? (firstBotTouchTick_ - repStartTick_) / kTicksPerSecond : -1.0},
		{"ahead", statejson::round2(ahead)},
		{"projected", statejson::toJson(proj)},
		{"danger", danger},
		{"possession", possession},
		{"playerEta", statejson::round2(myEta)},
		{"botEta", statejson::round2(botEta)},
		{"ballLocation", statejson::toJson(b.location)},
		{"ballVelocity", statejson::toJson(b.velocity)},
	});
	resultDetail_ = firstTouchText();
	setResult(outcome, label, 0.f);
}

void ScenarioPlayer::judgeControl(const Snapshot& snap)
{
	if (!snap.ball || countdownUntilTick_ != 0)
		return;
	const nlohmann::json& success = current_->resolved["success"];
	const BallState& b = *snap.ball;
	const float dir = defendedNet_ < 0 ? 1.f : -1.f; // toward the net you attack
	const CarState* me = snap.localCar();
	const CarState* bot = nullptr;
	for (const auto& c : snap.cars)
		if (c.isBot && (!bot || (c.location - b.location).length() < (bot->location - b.location).length()))
			bot = &c;
	auto finish = [&](int outcome, const char* label, const char* why) {
		nlohmann::json botAt, carAt;
		if (bot)
			botAt = statejson::toJson(bot->location);
		if (me)
			carAt = statejson::toJson(me->location);
		std::string detail;
		char buf[96];
		if (firstPlayerTouchTick_)
		{
			snprintf(buf, sizeof(buf), "FIRST TOUCH %.0f KM/H   |   KEPT IT %.1f S",
				std::max(0.f, ctl_.firstTouchKmh), ctl_.possessionSeconds);
			detail = buf;
		}
		log_.write("control_judged", {{"exercise", current_->id}, {"rep", rep_}, {"label", label}, {"why", why},
			{"outcome", outcome}, {"play", current_->resolved["situation"].value("variant", "")},
			{"firstTouchSeconds", firstPlayerTouchTick_ ? (firstPlayerTouchTick_ - repStartTick_) / kTicksPerSecond : -1.0},
			{"firstTouchKmh", statejson::round2(ctl_.firstTouchKmh)}, {"possessionSeconds", statejson::round2(ctl_.possessionSeconds)},
			{"botTouches", ctl_.botTouches}, {"ballLocation", statejson::toJson(b.location)},
			{"botLocation", botAt}, {"carLocation", carAt}});
		resultDetail_ = detail;
		setResult(outcome, label, 0.f);
	};

	// The bot got to it first.
	if (!firstPlayerTouchTick_)
	{
		if (firstBotTouchTick_)
			finish(-1, "BEATEN TO IT", "the bot touched it before you");
		return;
	}
	// How soft the first touch was: the ball's speed 0.1 s after it.
	if (ctl_.firstTouchKmh < 0.f && snap.tick >= firstPlayerTouchTick_ + 12)
		ctl_.firstTouchKmh = b.velocity.length() * 0.036f;
	ctl_.possessionSeconds = static_cast<float>((snap.tick - firstPlayerTouchTick_) / kTicksPerSecond);

	// Who has it: who gets to where the ball is going first (as in the 50s judge).
	const float look = static_cast<float>(success.value("lookAheadSeconds", 0.4));
	const Vec3 proj(b.location.x + b.velocity.x * look, b.location.y + b.velocity.y * look, b.location.z);
	const float myEta = etaToBall(me, proj), botEta = etaToBall(bot, proj);
	// Yours whatever the distances say while you are on it: your last touch recent and none of the bot's since (a
	// dribble or an air dribble at their net with the bot in goal was called a heavy touch, Lambourne 2026-10-19), or a
	// ball you sent on target (it ends as a goal, or the bot saves it).
	const bool yoursLast = lastPlayerTouchTick_ > lastBotTouchTick_;
	const bool onIt = yoursLast
		&& snap.tick < lastPlayerTouchTick_ + static_cast<uint64_t>(success.value("controlSeconds", 0.75) * kTicksPerSecond);
	const bool onTarget = yoursLast && ballpredict::timeToGoal(b, -defendedNet_) >= 0.f;
	if (onTarget)
		ctl_.onTargetTick = snap.tick;
	const bool botHasIt = !onIt && !onTarget && botEta + 0.15f < myEta;
	if (!botHasIt)
		ctl_.lostSince = 0;
	else if (ctl_.lostSince == 0)
		ctl_.lostSince = snap.tick;
	const uint64_t lostTicks = static_cast<uint64_t>(success.value("lostSeconds", 0.5) * kTicksPerSecond);
	if (ctl_.lostSince != 0 && snap.tick >= ctl_.lostSince + lostTicks)
	{
		const bool saved = ctl_.onTargetTick != 0 && lastBotTouchTick_ != 0 && lastBotTouchTick_ <= ctl_.onTargetTick + 6;
		finish(-1, saved ? "SAVED" : ctl_.botTouches > 0 ? "LOST IT" : "HEAVY TOUCH",
			saved ? "the bot saved your shot" : ctl_.botTouches > 0 ? "the bot took it" : "the ball ran away from you to the bot");
		return;
	}
	// Past the bot with it: the ball beyond it toward their net, you beyond it too and near the ball.
	if (bot && me && !botHasIt)
	{
		const float ballPast = dir * (b.location.y - bot->location.y);
		const float mePast = dir * (me->location.y - bot->location.y);
		if (ballPast > static_cast<float>(success.value("pastMargin", 600.0)) && mePast > 0.f
			&& (me->location - b.location).length() < 1200.f)
		{
			finish(+1, "PAST HIM", "you and the ball got past the bot");
			return;
		}
	}
	// Kept it long enough.
	if (!botHasIt && ctl_.possessionSeconds >= static_cast<float>(success.value("keepSeconds", 4.0)))
		finish(+1, "KEPT IT", "possession for keepSeconds");
}

void ScenarioPlayer::judgeShadow(const Snapshot& snap)
{
	const CarState* me = snap.localCar();
	if (!snap.ball || !me || countdownUntilTick_ != 0)
		return;
	const BallState& b = *snap.ball;
	const CarState* bot = nullptr;
	for (const auto& c : snap.cars)
		if (c.isBot && (!bot || (c.location - b.location).length() < (bot->location - b.location).length()))
			bot = &c;
	const Vec3 net(0.f, static_cast<float>(defendedNet_) * 5120.f, 0.f);
	const Vec3 ballFlat(b.location.x, b.location.y, 0.f), meFlat(me->location.x, me->location.y, 0.f);
	const float ballToNet = (ballFlat - net).length(), meToNet = (meFlat - net).length();
	const bool headingIn = ballpredict::timeToGoal(b, defendedNet_) >= 0.f;
	const float botGap = bot ? (bot->location - b.location).length() : 9999.f;
	const bool botControls = botGap < 260.f && b.location.z < 300.f && bot && bot->onGround;
	const bool timeUp = current_->timeLimitSeconds > 0.0
		&& (lastTick_ - repStartTick_) / kTicksPerSecond >= current_->timeLimitSeconds - 0.02;

	auto finish = [&](int outcome, const char* label, const char* why) {
		const float pct = shadow_.ticks ? 100.f * shadow_.goalSideTicks / shadow_.ticks : 0.f;
		char buf[96];
		snprintf(buf, sizeof(buf), "GOAL-SIDE %.0f%%%s", pct, !shadow_.commitTick ? ""
			: shadow_.commitLoose ? "   |   CHALLENGED A LOOSE BALL" : "   |   CHALLENGED ITS CONTROL");
		log_.write("shadow_judged", {{"exercise", current_->id}, {"rep", rep_}, {"label", label}, {"why", why},
			{"outcome", outcome}, {"play", current_->resolved["situation"].value("carry", "")},
			{"goalSidePercent", statejson::round2(pct)},
			{"commitSeconds", shadow_.commitTick ? (shadow_.commitTick - repStartTick_) / kTicksPerSecond : -1.0},
			{"commitLoose", shadow_.commitLoose}, {"shotBefore", shadow_.shotBefore}, {"botTouches", ctl_.botTouches},
			{"ballLocation", statejson::toJson(b.location)}, {"carLocation", statejson::toJson(me->location)}});
		resultDetail_ = buf;
		setResult(outcome, label, 0.f);
	};

	if (!shadow_.commitTick)
	{
		// Goal-side: between the ball and your net, near the line from the net to the ball.
		const Vec3 nb = ballFlat - net, np = meFlat - net;
		const float len = std::max(1.f, nb.length());
		const float along = (np.x * nb.x + np.y * nb.y) / len;
		const float lateral = std::fabs(np.x * nb.y - np.y * nb.x) / len;
		++shadow_.ticks;
		if (along < len - 150.f && lateral < 900.f)
			++shadow_.goalSideTicks;
		if (firstPlayerTouchTick_ != 0)
		{
			// Your commit: your first touch. Loose ball, or into its control (the bot's state just before).
			shadow_.commitTick = snap.tick;
			shadow_.commitLoose = !ctl_.botHadControl;
			shadow_.shotBefore = ctl_.wasHeadingIn; // before your touch only the bot had touched it
		}
		ctl_.botHadControl = botControls;
		ctl_.wasHeadingIn = headingIn;
	}

	// Past you: the ball nearer your net than you (and not on its way in: then the goal decides) for a quarter second.
	const bool past = ballToNet < meToNet - 250.f && !headingIn;
	if (!past)
		shadow_.pastSince = 0;
	else if (shadow_.pastSince == 0)
		shadow_.pastSince = snap.tick;
	if (shadow_.pastSince != 0 && snap.tick >= shadow_.pastSince + 30)
	{
		if (!shadow_.commitTick && lastTouchByBot_ && botGap > 800.f)
			finish(0, "SHOT WIDE", "its shot missed");
		else if (!shadow_.commitTick)
			finish(-1, "BEATEN", "it got past you before you touched it");
		else
			finish(-1, shadow_.commitLoose ? "BEATEN" : "DOVE IN",
				shadow_.commitLoose ? "it got past you after your challenge" : "you challenged its control and it went past you");
		return;
	}
	if (!shadow_.commitTick)
	{
		if (timeUp)
		{
			const float pct = shadow_.ticks ? 100.f * shadow_.goalSideTicks / shadow_.ticks : 0.f;
			finish(pct >= 60.f ? +1 : 0, pct >= 60.f ? "HELD HIM UP" : "TIME", "time up before you touched it");
		}
		return;
	}
	// After your touch: 1.5 s to see where it goes (a ball still heading in is left to the goal check).
	if (snap.tick >= shadow_.commitTick + static_cast<uint64_t>(1.5 * kTicksPerSecond) || timeUp)
	{
		if (headingIn && !timeUp)
			return;
		if (shadow_.shotBefore)
			finish(+1, "SAVED", "you stopped its shot");
		else
			finish(+1, shadow_.commitLoose ? "GOOD CHALLENGE" : "WON IT",
				shadow_.commitLoose ? "you challenged a loose ball and cleared it" : "you took it off its control");
	}
}

void ScenarioPlayer::tickCoach(const Snapshot& snap)
{
	if (!current_ || coachReported_ || current_->resolved.value("coach", "") != "speed_flip")
		return;
	if (!coach_.on())
	{
		// From the release (after the countdown and any freeze).
		if (countdownUntilTick_ == 0 && snap.tick >= repStartTick_ && (freezeUntilTick_ == 0 || snap.tick >= freezeUntilTick_))
			coach_.start(snap.tick);
		return;
	}
	if (const CarState* me = snap.localCar())
		coach_.tick(snap.tick, *me);
	if (coach_.done() || hasResult_)
		reportCoach();
}

void ScenarioPlayer::reportCoach()
{
	if (!coach_.on() || coachReported_)
		return;
	coachReported_ = true;
	const SpeedFlipCoach::Report r = coach_.report();
	nlohmann::json j = r.toJson();
	j["exercise"] = current_->id;
	j["rep"] = rep_;
	log_.write("speed_flip_coach", j);
	char buf[160];
	if (r.landed)
		snprintf(buf, sizeof(buf), "SPEED FLIP: %s   |   LANDED AT %.0f KM/H", r.tip.c_str(), r.landKmh);
	else
		snprintf(buf, sizeof(buf), "SPEED FLIP: %s", r.tip.c_str());
	coachLine_ = buf;
	if (hasResult_ && resultDetail_.find(coachLine_) == std::string::npos)
		resultDetail_ = resultDetail_.empty() ? coachLine_ : resultDetail_ + "   |   " + coachLine_;
}

nlohmann::json ScenarioPlayer::defenderBot(const std::string& who) const
{
	// In the orange net, facing out, so it defends the shot instead of racing the player to the ball; held with
	// everything else until the player's first input (or through a Saved Shot's lead-in). Its strength follows the
	// difficulty.
	const int d = difficulty_ == "easy" ? 0 : difficulty_ == "hard" ? 2 : 1;
	static const float kSkill[] = {0.3f, 0.6f, 1.0f}; // Psyonix: rookie / pro / all-star
	static const float kBeta[] = {0.55f, 0.8f, 1.0f};  // networks: looser on easy, best play on hard
	return {{"type", who}, {"team", 1}, {"keepType", true},
		{"location", {0, 4950, 17}}, {"rotation", {0, -16384, 0}}, {"velocity", {0, 0, 0}}, {"boost", 100},
		{"params", {{"skill", kSkill[d]}, {"beta", kBeta[d]}, {"scriptedKickoff", false}}}};
}

void ScenarioPlayer::addPackDefender(ResolvedScenario& r) const
{
	if (packDefender_.empty() || r.category != "packs")
		return;
	if (!r.resolved.contains("bots") || !r.resolved["bots"].is_array())
		r.resolved["bots"] = nlohmann::json::array();
	r.resolved["bots"].push_back(defenderBot(packDefender_));
}

void ScenarioPlayer::addShotExtras(ResolvedScenario& r) const
{
	// Lambourne 2026-10-29: "bot defenders for recorded shots, and block parts of the net like in training packs".
	const shotshare::Extras e = shotshare::extrasOf(r.resolved);
	if (!e.defender.empty())
	{
		if (!r.resolved.contains("bots") || !r.resolved["bots"].is_array())
			r.resolved["bots"] = nlohmann::json::array();
		r.resolved["bots"].push_back(defenderBot(e.defender));
		// Its save ends the rep (KEEPER SAVE) instead of the clock running out.
		if (!r.resolved.contains("fail") || !r.resolved["fail"].is_array())
			r.resolved["fail"] = nlohmann::json::array();
		const auto& fails = r.resolved["fail"];
		if (std::none_of(fails.begin(), fails.end(), [](const nlohmann::json& f) { return f.value("type", "") == "save"; }))
			r.resolved["fail"].push_back({{"type", "save"}});
	}
	if (!e.target.empty())
	{
		// The net target, as addPackTarget: picked tiles (or the hole), a random one each rep, or one that moves
		// (faster on hard); the rest of the net closed.
		const int d = difficulty_ == "easy" ? 0 : difficulty_ == "hard" ? 2 : 1;
		static const double kCycle[] = {2.2, 1.6, 1.1};
		const bool hole = e.size == "impossible";
		nlohmann::json zone = {{"type", "zone_cycle"}, {"id", "shot_net"}, {"surface", "orange_goal"},
			{"grid", {shotshare::colsFor(e.size), shotshare::kRows}}, {"cycleSeconds", e.target == "moving" ? kCycle[d] : 100000.0},
			{"blocking", true}};
		if (hole)
			zone["hole"] = 104.0; // the ball is 92.75: about 11 uu to spare
		if (e.target == "pick")
		{
			if (hole)
				zone["lit"] = shotshare::firstOpen(e.open);
			else
			{
				nlohmann::json open = nlohmann::json::array();
				for (int c = 0; c < shotshare::colsFor(e.size) * shotshare::kRows; ++c)
					if (e.open & (1u << c))
						open.push_back(c);
				zone["open"] = open;
			}
		}
		if (!r.resolved.contains("objects") || !r.resolved["objects"].is_array())
			r.resolved["objects"] = nlohmann::json::array();
		r.resolved["objects"].push_back(zone);
		r.resolved["success"] = {{"type", "goal_in_zone"}, {"label", hole ? "THROUGH THE HOLE" : "IN THE TILE"}};
	}
}

void ScenarioPlayer::addPackTarget(ResolvedScenario& r) const
{
	// Lit tiles in the orange net (the mechanics lab's zones): a goal counts only in the lit one. Grids and the
	// moving tile's speed follow the difficulty.
	if (packTarget_.empty() || r.category != "packs")
		return;
	// Tile size: 4, 6 or 8 tiles, or a hole just bigger than the ball at one of the 8 tiles' centres. The moving
	// tile's speed follows the difficulty.
	const int d = difficulty_ == "easy" ? 0 : difficulty_ == "hard" ? 2 : 1;
	static const double kCycle[] = {2.2, 1.6, 1.1};
	const int cols = packTileSize_ == "easy" ? 2 : packTileSize_ == "hard" || packTileSize_ == "impossible" ? 4 : 3;
	nlohmann::json zone = {{"type", "zone_cycle"}, {"id", "pack_target"}, {"surface", "orange_goal"}, {"grid", {cols, 2}},
		{"cycleSeconds", packTarget_ == "moving" ? kCycle[d] : 100000.0}, {"color", "accent"}, {"blocking", true}};
	if (packTileSize_ == "impossible")
		zone["hole"] = 104.0; // the ball is 92.75: about 11 uu to spare
	if (packTarget_ == "pick")
		zone["lit"] = std::clamp(packTile_, 0, cols * 2 - 1);
	if (!r.resolved.contains("objects") || !r.resolved["objects"].is_array())
		r.resolved["objects"] = nlohmann::json::array();
	r.resolved["objects"].push_back(zone);
	r.resolved["success"] = {{"type", "goal_in_zone"}, {"label", packTileSize_ == "impossible" ? "THROUGH THE HOLE" : "IN THE TILE"}};
}

void ScenarioPlayer::targetsAsPackTiles(ResolvedScenario& r) const
{
	scenario::targetsAsPackTiles(r.resolved);
}

void ScenarioPlayer::drawTrajectory(Canvas& canvas) const
{
	// "showTrajectory": while the shot waits for the player, or during a start countdown (Ball Control: "you do have
	// an idea of the direction the ball is headed", Lambourne 2026-10-19), the ball's path for the next
	// trajectorySeconds (1.6) as a fading line with a dot every 0.2 s (bounces off the floor and walls included).
	const bool waiting = leadInPhase_ == LeadInPhase::Waiting || countdownUntilTick_ != 0;
	// A "readFirst" wait (HORSE, the combo ladders, every Daily Challenge shot) always shows it, and further ahead.
	if (!current_ || !waiting || !(current_->resolved.value("showTrajectory", false) || readFirst_))
		return;
	BallState b{current_->ball.location, current_->ball.velocity, current_->ball.angularVelocity};
	if (b.velocity.length() < 1.f)
		return;
	constexpr float dt = 1.f / 120.f;
	const double seconds = current_->resolved.value("trajectorySeconds", readFirst_ ? 3.0 : 1.6);
	const int kSteps = std::clamp(static_cast<int>(seconds * 120.0), 24, 480);
	Vec3 prev = b.location;
	for (int i = 1; i <= kSteps; ++i)
	{
		ballpredict::step(b, dt);
		if (i % 4 != 0)
			continue;
		const float fade = 1.f - static_cast<float>(i) / kSteps;
		const auto a = static_cast<uint8_t>(60 + 170 * fade);
		canvas.line3d(prev, b.location, Rgba{255, 255, 255, a}, 4.f, 1.5f);
		if (i % 24 == 0)
			canvas.line3d(b.location - Vec3(0, 0, 14.f), b.location + Vec3(0, 0, 14.f), Rgba{255, 210, 90, a}, 6.f, 2.f);
		prev = b.location;
	}
}

void ScenarioPlayer::drawGhost(Canvas& canvas) const
{
	// The best run as a see-through car moving with the clock (at its start during the countdown), with a short trail.
	if (!ghostOn() || ghostBest_.frames.empty())
		return;
	const float t = countdownUntilTick_ != 0 || lastTick_ < repStartTick_ ? 0.f
		: static_cast<float>((lastTick_ - repStartTick_) / kTicksPerSecond);
	const bool finished = t > ghostBest_.seconds();
	// Light blue for your best, orange for someone else's.
	const Rgba col = ghostLabel_.empty() ? Rgba{150, 230, 255, static_cast<uint8_t>(finished ? 90 : 200)}
		: Rgba{255, 170, 90, static_cast<uint8_t>(finished ? 90 : 210)};
	// Trail: the last half second of its path.
	Vec3 prev = ghost::sample(ghostBest_, std::max(0.f, t - 0.5f)).location;
	for (int k = 1; k <= 10; ++k)
	{
		const Vec3 p = ghost::sample(ghostBest_, std::max(0.f, t - 0.5f + 0.05f * k)).location;
		canvas.line3d(prev + Vec3(0, 0, 8.f), p + Vec3(0, 0, 8.f), col.withAlpha(k / 14.f), 5.f, 1.f);
		prev = p;
	}
	const auto c = ghost::boxCorners(ghost::sample(ghostBest_, t));
	static const int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
	for (const auto& e : kEdges)
		canvas.line3d(c[e[0]], c[e[1]], col, 3.f, 1.f);
	// The nose edge brighter, so its facing reads at a glance.
	canvas.line3d(c[1], c[3], Rgba{255, 255, 255, col.a}, 4.f, 1.5f);
	canvas.line3d(c[5], c[7], Rgba{255, 255, 255, col.a}, 4.f, 1.5f);
	float x = 0.f, y = 0.f;
	const Vec3 top = ghost::sample(ghostBest_, t).location + Vec3(0, 0, 90.f);
	if (canvas.project(top, x, y) && canvas.depthOf(top) > 0.f)
	{
		char buf[64];
		std::string who = ghostLabel_;
		for (auto& ch : who)
			ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		snprintf(buf, sizeof(buf), "%s %.2f", who.empty() ? "BEST" : who.substr(0, 20).c_str(), ghostBest_.seconds());
		float w = 0, h = 0;
		canvas.textSize(buf, 0.9f, w, h);
		canvas.text(x - w * 0.5f, y - h, buf, 0.9f, col);
	}
}

void ScenarioPlayer::tickLeadIn(const Snapshot& snap)
{
	const bool pressing = snap.localInput && leadin::anyInput(*snap.localInput);
	if (leadInPhase_ == LeadInPhase::Playing)
	{
		const double t = (snap.tick - leadInStartTick_) / kTicksPerSecond;
		if (t < leadIn_.seconds())
		{
			const leadin::Frame f = leadin::sample(leadIn_, t);
			game_.setBall(f.ball);
			game_.poseLocalCar(f.car);
			return;
		}
		leadInPhase_ = LeadInPhase::Waiting;
		leadInArmed_ = !pressing; // something still held from the lead-in does not count: let go first
		log_.write("lead_in_waiting", {{"exercise", current_->id}, {"rep", rep_}, {"armed", leadInArmed_}});
	}
	// Armed once nothing is pressed; packs also arm after 0.4 s with a button held (throttle straight into the shot),
	// "readFirst" waits for a real let-go so the shot starts only when the player chooses.
	if (!pressing || (waitForInput_ && !readFirst_ && snap.tick > leadInStartTick_ + static_cast<uint64_t>(0.4 * kTicksPerSecond)))
		leadInArmed_ = true;
	CarTarget car = current_->player;
	car.boost.reset();
	if (!(leadInArmed_ && pressing))
	{
		// Frozen on the captured moment.
		game_.setBall({current_->ball.location, {}, {}});
		car.velocity = car.angularVelocity = {};
		game_.poseLocalCar(car);
		return;
	}
	// Go: the captured velocities carry on from here, and the rep (its clock, touches, goals) starts now.
	game_.setBall(current_->ball);
	game_.poseLocalCar(car);
	leadInPhase_ = LeadInPhase::Off;
	repStartTick_ = snap.tick;
	goalCheckFromTick_ = snap.tick + 3;
	touches_ = 0;
	touchStreak_ = 0;
	lastTouchByBot_ = false;
	prevBall_.reset();
	saveWatchFromTick_ = 0;
	outTicks_ = 0;
	log_.write("lead_in_released", {{"exercise", current_->id}, {"rep", rep_}});
}

int ScenarioPlayer::pinsDown() const
{
	int n = 0;
	for (const auto& o : objects_.objects())
		n += o.type == ObjectType::Pin && !o.active;
	return n;
}

int ScenarioPlayer::pinsTotal() const
{
	int n = 0;
	for (const auto& o : objects_.objects())
		n += o.type == ObjectType::Pin;
	return n;
}

void ScenarioPlayer::judgeBowling(const Snapshot& snap)
{
	const int total = pinsTotal();
	if (total > 0 && pinsDown() == total)
	{
		frameDone_ = true;
		setResult(+1, frameThrow_ == 1 ? "STRIKE" : "SPARE", 0.f);
		return;
	}
	// The pins have settled (no new pin down for a while), or the ball has stopped after being played.
	const double settle = current_->resolved["bowling"].value("settleSeconds", 2.5);
	if (lastPinHitTick_ != 0 && snap.tick > lastPinHitTick_ + static_cast<uint64_t>(settle * kTicksPerSecond))
	{
		finishThrow();
		return;
	}
	if (touches_ > 0 && snap.ball && snap.ball->velocity.length() < 150.f)
	{
		if (slowSinceTick_ == 0)
			slowSinceTick_ = snap.tick;
		else if (snap.tick > slowSinceTick_ + static_cast<uint64_t>(1.5 * kTicksPerSecond))
			finishThrow();
	}
	else
	{
		slowSinceTick_ = 0;
	}
}

void ScenarioPlayer::finishThrow()
{
	if (hasResult_)
		return;
	const int knocked = pinsDown() - pinsDownAtStart_;
	frameDone_ = frameThrow_ >= bowlingThrows();
	const std::string label = knocked == 0 ? "MISS" : knocked == 1 ? "1 PIN" : std::to_string(knocked) + " PINS";
	setResult(knocked == 0 ? -1 : 0, label, 0.f);
}

std::vector<std::string> ScenarioPlayer::levelIds(const ContentLibrary::Item& item) const
{
	std::vector<std::string> ids;
	for (const auto& obj : item.data.value("objects", nlohmann::json::array()))
	{
		for (const auto& c : obj.value("choices", nlohmann::json::array()))
			if (c.contains("id"))
				ids.push_back(c["id"].get<std::string>());
	}
	return ids;
}

std::string ScenarioPlayer::bestKey() const
{
	// Per exercise and difficulty, and per level / objective when the rep has one.
	std::string key = BestTimes::key(BestTimes::recordId(current_->id, current_->resolved), current_->difficulty);
	const std::string objective = current_->resolved.value("objectiveId", "");
	return objective.empty() ? key : key + "|" + objective;
}

const std::vector<std::string>& ScenarioPlayer::choiceFilter(const std::string& scenarioId) const
{
	static const std::vector<std::string> kNone;
	if (ignoreChoiceFilters_)
		return kNone; // the Daily Challenge: every choice, as for everyone else
	const auto it = choiceFilters_.find(scenarioId);
	return it == choiceFilters_.end() ? kNone : it->second;
}

void ScenarioPlayer::judgeSkill(const Snapshot& snap)
{
	if (const CarState* me = snap.localCar())
	{
		lastLocalOnWall_ = me->onWall;
		lastLocalBoost_ = me->boost;
	}
	const std::optional<BallState> prevBall = lastBall_;
	lastBall_ = snap.ball;
	if (hasResult_ || !snap.ball)
		return;
	const nlohmann::json& success = current_->resolved["success"];
	const std::string type = success.value("type", "");

	if (type == "touch" && pendingTouchTick_ != 0 && snap.tick >= pendingTouchTick_)
	{
		// The touch's hit is in the ball now: check what the objective asks for.
		pendingTouchTick_ = 0;
		const float kmh = snap.ball->velocity.length() * 0.036f;
		bool ok = true;
		if (success.contains("minBallHeight")) ok = ok && pendingTouchBallZ_ >= success["minBallHeight"].get<float>();
		if (success.contains("minSpeedKmh")) ok = ok && kmh >= success["minSpeedKmh"].get<float>();
		if (success.value("onWall", false)) ok = ok && pendingTouchOnWall_;
		log_.write("skill_touch", {{"exercise", current_->id}, {"rep", rep_}, {"ballHeight", statejson::round2(pendingTouchBallZ_)},
			{"kmh", statejson::round2(kmh)}, {"onWall", pendingTouchOnWall_}, {"ok", ok}});
		if (ok)
			setResult(+1, "NICE", kmh);
	}
	else if (type == "touch_streak")
	{
		// The ball landing on the floor (moving down at floor height) breaks the streak.
		if (prevBall && snap.ball->location.z < 100.f && prevBall->velocity.z < -80.f && snap.ball->velocity.z > prevBall->velocity.z)
			touchStreak_ = 0;
		if (touchStreak_ >= static_cast<int>(success.value("count", 3.0)))
			setResult(+1, "KEPT IT UP", 0.f);
	}
	else if (type == "dribble")
	{
		// Ball resting on the car: above it and close in plan; brief losses (0.2 s) are forgiven.
		const CarState* me = snap.localCar();
		bool onCar = false;
		if (me)
		{
			const Vec3 d = snap.ball->location - me->location;
			onCar = d.z > 70.f && d.z < 280.f && std::sqrt(d.x * d.x + d.y * d.y) < 170.f;
		}
		if (onCar)
		{
			dribbleSeconds_ += 1.0 / kTicksPerSecond;
			dribbleLostTick_ = 0;
		}
		else if (dribbleLostTick_ == 0)
		{
			dribbleLostTick_ = snap.tick;
		}
		else if (snap.tick > dribbleLostTick_ + static_cast<uint64_t>(0.2 * kTicksPerSecond))
		{
			dribbleSeconds_ = 0.0;
		}
		if (dribbleSeconds_ >= success.value("seconds", 3.0))
			setResult(+1, "DRIBBLED", 0.f);
	}
}

void ScenarioPlayer::drawObjects(Canvas& canvas, const Snapshot& snap) const
{
	if (!current_)
		return;
	objects_.draw(canvas, lastTick_ / kTicksPerSecond);
	drawZones(canvas);
	drawGhost(canvas);
	drawTrajectory(canvas);
	// Beginner aid: a dashed guide from the ball to the objective.
	if (current_->resolved.value("guide", false) && !hasResult_ && snap.ball)
	{
		if (const auto to = objects_.objectiveMarker(0))
		{
			const Vec3 from = snap.ball->location;
			const Vec3 d = *to - from;
			const float len = d.length();
			if (len > 300.f)
			{
				const Vec3 u = d * (1.f / len);
				const float flow = static_cast<float>(std::fmod(lastTick_ / kTicksPerSecond * 500.0, 200.0));
				for (float s = 120.f + flow - 200.f; s < len - 150.f; s += 200.f)
				{
					const float s0 = std::max(s, 120.f), s1 = std::min(s + 110.f, len - 150.f);
					if (s1 > s0)
						canvas.line3d(from + u * s0, from + u * s1, Rgba{255, 214, 90, 255}, 4.f, 2.f);
				}
			}
		}
	}
}

bool ScenarioPlayer::failsOnObject(const std::string& objectId) const
{
	if (!current_ || !current_->resolved.contains("fail"))
		return false;
	for (const auto& f : current_->resolved["fail"])
	{
		if (f.value("type", "") != "object_hit")
			continue;
		// "target_block_*" matches every generated blocker of a pocket target.
		const std::string want = f.value("objectId", "");
		if (want == objectId || (!want.empty() && want.back() == '*' && objectId.rfind(want.substr(0, want.size() - 1), 0) == 0))
			return true;
	}
	return false;
}

void ScenarioPlayer::judgeObjects(const Snapshot& snap)
{
	const ObjectSystem::TickResult res = objects_.tick(snap);
	if (res.ballBounce && released_ && !ballExploded_ && !goalHeld_)
		game_.setBall(*res.ballBounce);
	for (const auto& h : res.hits)
	{
		log_.write("object_hit", {
			{"exercise", current_->id},
			{"rep", rep_},
			{"object", h.id},
			{"objectType", objects::typeName(h.type)}, // not "type": that is the event name
			{"by", h.by},
			{"at", statejson::toJson(h.at)},
			{"ballSpeed", statejson::round2(h.ballSpeed)},
			{"inOrder", h.inOrder},
		});
	}
	if (!res.leftLane.empty())
		log_.write("lane_left", {{"exercise", current_->id}, {"rep", rep_}, {"lane", res.leftLane}});
	if (bowling())
	{
		for (const auto& h : res.hits)
		{
			if (h.type == ObjectType::Pin)
				lastPinHitTick_ = snap.tick;
		}
		if (!hasResult_)
			judgeBowling(snap);
		return;
	}
	if (hasResult_)
		return;

	// Fails first (a blocker hit or leaving the lane ends the rep even if a target was hit on the same tick).
	for (const auto& h : res.hits)
	{
		if (h.inOrder && failsOnObject(h.id))
		{
			setResult(-1, h.type == ObjectType::Blocker ? "BLOCKED" : "MISS", h.ballSpeed * 0.036f);
			return;
		}
	}
	if (!res.leftLane.empty() && failHas("car_leaves_lane"))
	{
		setResult(-1, "OFF LANE", 0.f);
		return;
	}

	const nlohmann::json& success = current_->resolved["success"];
	const std::string type = success.value("type", "");
	const std::string wanted = success.value("objectId", "");
	for (const auto& h : res.hits)
	{
		if (!h.inOrder || h.type == ObjectType::Blocker)
			continue;
		if ((type == "object_hit" || type == "reach_object") && (wanted.empty() || h.id == wanted))
		{
			setResult(+1, type == "object_hit" ? "HIT" : "REACHED", h.ballSpeed * 0.036f);
			return;
		}
	}
	if (type == "all_objects_hit" && !res.hits.empty() && objects_.allHit())
		setResult(+1, "CLEARED", 0.f);
	else if (type == "objects_in_order" && !res.hits.empty() && objects_.nextInOrder() < 0)
		setResult(+1, "COURSE DONE", 0.f);
}

void ScenarioPlayer::logWallBounce(const Snapshot& snap)
{
	// Side-wall bounces, logged with BallPredict's guess for the speed kept along the wall, so the bank shot model
	// can be checked against the game (a touch near the wall can show up here too: see "touches").
	const std::optional<BallState> prev = prevBall_;
	prevBall_ = released_ ? snap.ball : std::nullopt;
	if (!prev || !snap.ball)
		return;
	const Vec3& in = prev->velocity;
	const Vec3& out = snap.ball->velocity;
	if (std::fabs(snap.ball->location.x) < 3800.f || in.x * out.x >= 0.f || in.x * snap.ball->location.x <= 0.f)
		return;
	Vec3 predicted = in;
	ballpredict::wallBounce(predicted, 0);
	const float alongIn = std::hypot(in.y, in.z);
	const float alongOut = std::hypot(out.y, out.z);
	const float alongPredicted = std::hypot(predicted.y, predicted.z);
	log_.write("ball_wall_bounce", {
		{"exercise", current_->id},
		{"rep", rep_},
		{"touches", touches_},
		{"ballLocation", statejson::toJson(snap.ball->location)},
		{"velocityIn", statejson::toJson(in)},
		{"velocityOut", statejson::toJson(out)},
		{"velocityPredicted", statejson::toJson(predicted)},
		{"normalKept", statejson::round2(std::fabs(in.x) > 1.f ? -out.x / in.x : 0.f)},
		{"alongKept", statejson::round2(alongIn > 1.f ? alongOut / alongIn : 0.f)},
		{"alongKeptPredicted", statejson::round2(alongIn > 1.f ? alongPredicted / alongIn : 0.f)},
	});
}

std::string ScenarioPlayer::goalText() const
{
	// What counts, in a few words (Lambourne 2026-10-21: "I can't read the full exercise text"; daily shots had no
	// line saying what to do).
	if (!current_ || !current_->resolved.contains("success"))
		return "";
	const nlohmann::json& s = current_->resolved["success"];
	const std::string t = s.value("type", "");
	if (t == "goal") return "SCORE";
	if (t == "goal_in_zone") return zones_.hole > 0.f ? "THROUGH THE HOLE"
		: zones_.open != 0 && shotshare::openCount(zones_.open) > 1 ? "SCORE IN A LIT TILE" : "SCORE IN THE LIT TILE";
	if (t == "save") return "SAVE IT";
	if (t == "all_objects_hit") return bowling() ? "KNOCK DOWN ALL THE PINS" : "HIT THEM ALL";
	if (t == "object_hit") return "HIT THE TARGET";
	if (t == "objects_in_order") return "THROUGH THEM IN ORDER";
	if (t == "reach_object") return "REACH THE CIRCLE";
	if (t == "dribble") return "CATCH IT AND KEEP IT ON YOUR CAR";
	if (t == "touch") return "TOUCH THE BALL";
	if (t == "touch_streak") return "KEEP IT UP";
	if (t == "kickoff") return "WIN THE KICKOFF";
	if (t == "fifty") return "WIN THE 50";
	if (t == "possession") return "KEEP THE BALL";
	if (t == "shadow") return "SHADOW AND STOP THE ATTACK";
	if (t == "match") return "WIN THE MATCH";
	if (t == "mechanic" || t == "detector" || t == "detector_chain")
	{
		static const std::map<std::string, std::string> kNames = {{"aerial_touch", "AERIAL"}, {"air_dribble", "AIR DRIBBLE"},
			{"double_touch", "DOUBLE TAP"}, {"flip_reset", "FLIP RESET"}, {"double_reset", "DOUBLE RESET"},
			{"ceiling_shot", "CEILING SHOT"}, {"backboard_read", "BACKBOARD READ"}, {"flick", "FLICK"},
			{"flick:musty", "MUSTY FLICK"}, {"pinch", "PINCH"}, {"power_shot", "POWER SHOT"}, {"redirect", "REDIRECT"},
			{"wall_touch", "OFF THE WALL"}};
		std::string out;
		for (const auto& step : s.value("chain", nlohmann::json::array()))
		{
			if (!step.is_string())
				continue;
			std::string id = step.get<std::string>(), times;
			if (const size_t x = id.find(":x"); x != std::string::npos)
			{
				times = " X" + id.substr(x + 2);
				id = id.substr(0, x);
			}
			const auto it = kNames.find(id);
			std::string name = it != kNames.end() ? it->second : id;
			if (it == kNames.end())
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c == '_' ? ' ' : static_cast<char>(std::toupper(c)); });
			out += (out.empty() ? "" : " > ") + name + times;
		}
		if (out.empty())
			out = "DO THE MECHANIC";
		if (s.value("requireGoal", false))
			out += s.value("zone", false) ? " > GOAL IN THE LIT ZONE" : " > GOAL";
		return out;
	}
	return "";
}

std::string ScenarioPlayer::successType() const
{
	if (!current_ || !current_->resolved.contains("success"))
		return "";
	return current_->resolved["success"].value("type", "");
}

bool ScenarioPlayer::failHas(const std::string& type) const
{
	if (!current_ || !current_->resolved.contains("fail"))
		return false;
	for (const auto& f : current_->resolved["fail"])
	{
		if (f.value("type", "") == type)
			return true;
	}
	return false;
}

void ScenarioPlayer::setResult(int outcome, const std::string& label, float kmh)
{
	if (hasResult_ || !current_)
		return;
	hasResult_ = true;
	outcome_ = outcome;
	resultLabel_ = label;
	resultKmh_ = kmh;
	resultSeconds_ = (lastTick_ - repStartTick_) / kTicksPerSecond;
	resultNewBest_ = outcome > 0 && timed() && bests_ && bests_->record(bestKey(), resultSeconds_);
	// The speed flip coach's tip (it may have finished before the result: the landing comes before the ball).
	reportCoach();
	if (!coachLine_.empty() && resultDetail_.find(coachLine_) == std::string::npos)
		resultDetail_ = resultDetail_.empty() ? coachLine_ : resultDetail_ + "   |   " + coachLine_;
	if (resultNewBest_ && ghostOn() && !ghostRun_.frames.empty())
	{
		// The finish itself as the last frame, then it becomes the ghost to race.
		const Snapshot snap = game_.snapshot();
		if (const CarState* me = snap.localCar())
			ghostRun_.add(static_cast<float>(resultSeconds_), me->location, me->rotation);
		if (ghostOverride_.frames.empty())
			ghostBest_ = ghostRun_; // (a friend's ghost stays up while you race it)
		if (ghostSave_)
			ghostSave_(bestKey(), ghostRun_);
		log_.write("ghost_saved", {{"exercise", current_->id}, {"seconds", resultSeconds_}, {"frames", ghostRun_.frames.size()}});
	}
	// Runs: every situation moves on; the last one shows how many of the run were done.
	if (progression() == "run")
	{
		if (const auto* item = content_.find(ContentLibrary::Kind::Scenario, current_->id))
		{
			const int levels = std::max<int>(1, static_cast<int>(levelIds(*item).size()));
			int& level = level_[current_->id];
			if (level == 0)
				runSuccesses_ = 0;
			runSuccesses_ += outcome > 0;
			if (level == levels - 1)
			{
				resultLabel_ += "  RUN " + std::to_string(runSuccesses_) + "/" + std::to_string(levels);
				log_.write("run_done", {{"exercise", current_->id}, {"done", runSuccesses_}, {"of", levels}});
			}
			level = (level + 1) % levels;
		}
	}
	// Loop: every situation moves on, round and round.
	trialDone_ = false;
	if (progression() == "loop")
	{
		if (const auto* item = content_.find(ContentLibrary::Kind::Scenario, current_->id))
			level_[current_->id] = (level_[current_->id] + 1) % std::max<int>(1, static_cast<int>(levelIds(*item).size()));
	}
	// Trial: a success moves on and a fail repeats; finishing the last one is the attempt's time.
	if (progression() == "trial")
	{
		if (const auto* item = content_.find(ContentLibrary::Kind::Scenario, current_->id))
		{
			const int levels = std::max<int>(1, static_cast<int>(levelIds(*item).size()));
			int& level = level_[current_->id];
			if (outcome > 0 && level == levels - 1)
			{
				trialDone_ = true;
				resultSeconds_ = (lastTick_ - trialStartTick_) / kTicksPerSecond;
				resultNewBest_ = bests_ && bests_->record(BestTimes::key(BestTimes::recordId(current_->id, current_->resolved), current_->difficulty), resultSeconds_);
				resultLabel_ = "ALL DONE";
				log_.write("trial_done", {{"exercise", current_->id}, {"seconds", resultSeconds_}, {"newBest", resultNewBest_}});
				level = 0;
				freshRun_ = true;
			}
			else if (outcome > 0)
			{
				++level;
			}
		}
	}
	lastFailed_ = outcome <= 0;
	modeAfterResult(outcome, resultLabel_);
	// Levels: a success moves on to the next level (after the last, back to the first).
	if (outcome > 0 && progression() == "levels")
	{
		if (const auto* item = content_.find(ContentLibrary::Kind::Scenario, current_->id))
			level_[current_->id] = (level_[current_->id] + 1) % std::max<int>(1, static_cast<int>(levelIds(*item).size()));
	}
	++resultSeq_;
	if (outcome > 0)
		++streak_;
	else if (outcome < 0)
		streak_ = 0;
	log_.write("rep_result", {
		{"exercise", current_->id},
		{"rep", rep_},
		{"seed", current_->seed},
		{"outcome", outcome > 0 ? "success" : outcome < 0 ? "fail" : "neutral"},
		{"label", label},
		{"kmh", statejson::round2(kmh)},
		{"seconds", resultSeconds_},
		{"touches", touches_},
		{"streak", streak_},
		{"newBest", resultNewBest_},
	});
	const bool selfAdvance = afterResult(outcome) != "stay";
	if (resultListener_)
		resultListener_(status());
	if (selfAdvance)
	{
		// (A finished Style Rush starts the next run from the start, with its countdown.)
		const bool keepCar = afterResult(outcome) == "chain" && !(isRush() && rushOver_);
		const int judged = rep_;
		// Next tick (not inside the hit test that judged this rep); a manual retry/next meanwhile wins.
		const float delay = (matchOver_ && isMatch()) || (horseOver_ && progression() == "horse") || (rushOver_ && isRush()) ? 6.f : std::max(0.01f, current_->resolved.value("advanceDelaySeconds", 0.01f));
		game_.runAfter(delay, [this, keepCar, judged] {
			if (!current_ || rep_ != judged)
				return;
			if (realGoalPending_)
				return; // a real goal's replay is playing: freeplay's reset after it moves on (onRoundReset)
			if (advanceRequest_)
				advanceRequest_(keepCar);
			else
				next(keepCar);
		});
	}
}

void ScenarioPlayer::onRealGoal()
{
	// The game scored a goal for real: its explosion, slow motion and replay play now, then freeplay resets. Until that
	// reset the exercise waits (no next rep, the HUD hidden, the daily's next shot held back); onRoundReset moves on.
	if (!current_ || realGoalPending_)
		return;
	realGoalPending_ = true;
	goalLetThrough_ = true; // not caught again while it plays
	log_.write("real_goal", {{"exercise", current_->id}, {"rep", rep_}, {"replaysOn", goalReplays_}, {"judged", hasResult_}});
}

void ScenarioPlayer::onRoundReset()
{
	if (!current_ || historyIndex_ < 0)
		return;
	const int resetRep = rep_;
	log_.write("game_round_reset", {{"exercise", current_->id}, {"rep", resetRep}, {"afterRealGoal", realGoalPending_}});
	if (realGoalPending_)
	{
		// "Goal replays": this is the reset after a real goal's replay. Move on as the rep would have: its next rep for
		// a self-advancing drill, else the same rep again.
		realGoalPending_ = false;
		const bool advance = afterResult(outcome_) != "stay";
		game_.runAfter(0.05f, [this, resetRep, advance] {
			if (!current_ || rep_ != resetRep || historyIndex_ < 0)
				return;
			if (advance && advanceRequest_)
				advanceRequest_(false);
			else if (advance)
				next(false);
			else if (!forceReadFirst_) // the daily (which forces read-first) starts its next shot itself
				apply(history_[historyIndex_], "after_goal_replay");
		});
		return;
	}
	// During an exercise freeplay's reset (Epic's default reset button, or R3 on some setups) is undone: the ball and
	// car go back to where they were a moment before it, moving as they were; the rep, its clock and the streak carry
	// on (it used to re-apply the rep as a new one: +1 rep, +2 with a retry on the same button, a skip in the daily).
	// The reset's first event comes 1-2 ticks before this one, so the state from 4 ticks back is from before it.
	const RecentState* before = nullptr;
	for (auto it = recent_.rbegin(); it != recent_.rend(); ++it)
		if (it->tick + 4 <= lastTick_)
		{
			before = &*it;
			break;
		}
	if (before && !ballExploded_)
	{
		const RecentState s = *before;
		log_.write("game_reset_undone", {{"exercise", current_->id}, {"rep", resetRep}, {"fromTick", s.tick},
			{"ball", statejson::toJson(s.ball.location)}, {"car", statejson::toJson(s.car.location)}});
		game_.runAfter(0.05f, [this, resetRep, s] {
			if (!current_ || rep_ != resetRep)
				return;
			game_.setBall({s.ball.location, s.ball.velocity, s.ball.angularVelocity});
			CarTarget car;
			car.location = s.car.location;
			car.rotation = s.car.rotation;
			car.velocity = s.car.velocity;
			car.angularVelocity = s.car.angularVelocity;
			car.boost = s.car.boost;
			game_.poseLocalCar(car);
		});
		return;
	}
	// No state from before it (or the ball was exploded by a goal): put the same rep back, as before.
	game_.runAfter(0.05f, [this, resetRep] {
		if (current_ && rep_ == resetRep && historyIndex_ >= 0)
			apply(history_[historyIndex_], "game_reset");
	});
}

void ScenarioPlayer::onTouch(const TouchEvent& touch)
{
	if (current_ && touch.simulated)
		lastTouchByBot_ = touch.isBot;
	if (current_ && touch.simulated && !touch.isLocal && touch.isBot)
	{
		// The bot touched: watch for a save. A 50 (the player touched in the same moment) is judged only from after
		// both touches have landed in the ball's flight.
		keeperContested_ = lastPlayerTouchTick_ != 0 && touch.tick <= lastPlayerTouchTick_ + kFiftyTicks;
		keeperWatchFromTick_ = touch.tick + (keeperContested_ ? kFiftyTicks : 2);
		keeperOutTicks_ = 0;
		lastBotTouchTick_ = touch.tick;
		if (firstBotTouchTick_ == 0 && touch.tick >= repStartTick_)
			firstBotTouchTick_ = touch.tick;
		if (touch.tick >= repStartTick_)
			++ctl_.botTouches;
	}
	if (current_ && touch.simulated && touch.isLocal)
	{
		lastPlayerTouchTick_ = touch.tick;
		if (firstPlayerTouchTick_ == 0 && touch.tick >= repStartTick_)
			firstPlayerTouchTick_ = touch.tick;
		if (keeperWatchFromTick_ != 0)
		{
			if (touch.tick <= lastBotTouchTick_ + kFiftyTicks)
			{
				keeperContested_ = true; // a 50: wait until it is settled
				keeperWatchFromTick_ = touch.tick + kFiftyTicks;
				keeperOutTicks_ = 0;
			}
			else
			{
				keeperWatchFromTick_ = 0; // the player has it again: no save unless the bot touches it again
			}
		}
		++touches_;
		++touchStreak_;
		pendingTouchTick_ = touch.tick + 2;
		pendingTouchBallZ_ = touch.ballLocation.z;
		pendingTouchOnWall_ = lastLocalOnWall_;
		saveWatchFromTick_ = touch.tick + 2; // the hit is in the ball's velocity two ticks later
		outTicks_ = 0;
	}
}

ScenarioPlayer::Status ScenarioPlayer::status() const
{
	Status s;
	if (!current_)
		return s;
	s.active = true;
	s.scenarioId = current_->id;
	s.scenarioName = current_->name;
	s.difficulty = current_->difficulty;
	s.rep = rep_;
	s.seed = current_->seed;
	s.mirrored = current_->mirrored;
	s.repSeconds = hasResult_ ? resultSeconds_ : (lastTick_ - repStartTick_) / kTicksPerSecond;
	s.timeLimitSeconds = current_->timeLimitSeconds;
	s.clockCountsDown = current_->resolved.value("clockCountsDown", false);
	s.touches = touches_;
	s.goalSide = goalSide_;
	s.hasResult = hasResult_;
	s.outcome = outcome_;
	s.resultLabel = resultLabel_;
	s.resultDetail = resultDetail_;
	s.ghost = ghostOn() && !ghostBest_.frames.empty();
	s.ghostSeconds = ghostBest_.seconds();
	if (isMatch())
	{
		s.match = true;
		s.scoreYou = scoreYou_;
		s.scoreBot = scoreBot_;
		s.overtime = overtime_;
		s.matchOver = matchOver_;
		s.matchSecondsLeft = overtime_ ? overtimeSeconds_ : matchSecondsLeft_;
	}
	s.resultKmh = resultKmh_;
	s.resultSeq = resultSeq_;
	s.streak = streak_;
	s.timed = timed();
	s.selfAdvancing = hasResult_ && afterResult(outcome_) != "stay";
	if (current_->resolved.value("showTank", false))
		s.tank = lastLocalBoost_;
	if (progression() == "trial")
	{
		s.timed = trialDone_;
		s.newBest = trialDone_ && resultNewBest_;
		if (bests_)
			s.bestSeconds = bests_->best(BestTimes::key(BestTimes::recordId(current_->id, current_->resolved), current_->difficulty));
	}
	if (!bowling() && successType() == "all_objects_hit")
	{
		for (const auto& o : objects_.objects())
		{
			if (!objects::isGoal(o))
				continue;
			++s.objectsTotal;
			s.objectsHit += o.hit;
		}
	}
	if (bowling())
	{
		s.pinsDown = pinsDown();
		s.pinsTotal = pinsTotal();
		s.frame = frame_;
		s.frameThrow = frameThrow_;
		s.frameThrows = bowlingThrows();
	}
	s.objective = current_->resolved.value("objectiveLabel", "");
	{
		const nlohmann::json& success = current_->resolved["success"];
		const std::string type = success.value("type", "");
		char buf[48];
		if (type == "touch_streak")
		{
			snprintf(buf, sizeof(buf), "%d / %d", std::min(touchStreak_, static_cast<int>(success.value("count", 3.0))),
				static_cast<int>(success.value("count", 3.0)));
			s.objectiveProgress = buf;
		}
		else if (type == "dribble")
		{
			snprintf(buf, sizeof(buf), "%.1f / %.1f S", dribbleSeconds_, success.value("seconds", 3.0));
			s.objectiveProgress = buf;
		}
		else if (progression() == "trial" && !hasResult_)
		{
			snprintf(buf, sizeof(buf), "TOTAL %.1f S", (lastTick_ - trialStartTick_) / kTicksPerSecond);
			s.objectiveProgress = buf;
		}
		else if (current_->timeLimitSeconds > 0.0 && !s.objective.empty())
		{
			snprintf(buf, sizeof(buf), "%.0f S LEFT", std::max(0.0, current_->timeLimitSeconds - s.repSeconds));
			s.objectiveProgress = buf;
		}
	}
	if (!s.objective.empty())
	{
		s.objectiveTag = current_->resolved.value("objectiveTag", s.objective);
		if (!hasResult_)
			s.objectiveAt = objects_.objectiveMarker(0);
	}
	modeStatus(s);
	if (countdownUntilTick_ != 0 && current_->resolved.value("showPlayMap", false))
	{
		auto& m = s.map;
		m.show = true;
		m.ball = current_->ball.location;
		m.player = current_->player.location;
		m.playerYaw = current_->player.rotation.yaw;
		auto vec = [](const nlohmann::json& v) {
			return v.is_array() && v.size() >= 2 ? Vec3(v[0].get<float>(), v[1].get<float>(), v.size() > 2 ? v[2].get<float>() : 0.f) : Vec3();
		};
		for (const auto& bot : current_->resolved.value("bots", nlohmann::json::array()))
		{
			m.bots.push_back(vec(bot.value("location", nlohmann::json())));
			m.botYaws.push_back(bot.contains("rotation") ? bot["rotation"][1].get<int>() : 0);
		}
		if (current_->resolved.contains("situation"))
		{
			const auto& sit = current_->resolved["situation"];
			if (sit.contains("mate"))
			{
				m.hasMate = true;
				m.mate = vec(sit["mate"]);
				m.mateRadius = static_cast<float>(sit.value("mateRadius", 650.0));
			}
			if (sit.contains("contact"))
			{
				m.hasContact = true;
				m.contact = vec(sit["contact"]);
			}
		}
	}
	if (isMatch())
	{
		// The score on the objective line, the clock beside it.
		char buf[32];
		const int t = static_cast<int>(overtime_ ? overtimeSeconds_ : std::ceil(matchSecondsLeft_));
		snprintf(buf, sizeof(buf), overtime_ ? "OT +%d:%02d" : "%d:%02d", t / 60, t % 60);
		s.objective = scoreText();
		s.objectiveTag.clear();
		s.objectiveAt.reset();
		s.objectiveProgress = buf;
	}
	if (countdownUntilTick_ != 0)
	{
		const double left = (countdownUntilTick_ > lastTick_ ? countdownUntilTick_ - lastTick_ : 0) / kTicksPerSecond;
		s.objective = isMatch() ? "Kickoff   |   " + scoreText() : "Get ready";
		s.countdown = std::max(1, static_cast<int>(std::ceil(left)));
		s.objectiveTag.clear();
		s.objectiveAt.reset();
		s.objectiveProgress = std::to_string(static_cast<int>(std::ceil(left)));
		s.repSeconds = 0.0;
	}
	if (leadInPhase_ == LeadInPhase::Waiting && readFirst_ && !s.objective.empty())
	{
		// Read first: the objective stays up (and its marker in the world); the HUD adds "press any button to start".
		s.waitingForInput = true;
		s.repSeconds = 0.0;
	}
	else if (leadInPhase_ != LeadInPhase::Off)
	{
		s.objective = leadInPhase_ == LeadInPhase::Playing ? "Watch the lead-up" : "Press any button to play";
		s.prompt = true;
		s.objectiveTag.clear();
		s.objectiveAt.reset();
		s.objectiveProgress.clear();
		s.repSeconds = 0.0;
	}
	s.goalReplay = realGoalPending_;
	// Held (waiting for a press, or counting down): the read card's text.
	s.holding = !hasResult_ && (leadInPhase_ == LeadInPhase::Waiting || countdownUntilTick_ != 0);
	if (s.holding)
	{
		s.goalText = goalText();
		// The description's first sentence (up to ~160 characters).
		const std::string d = current_->resolved.value("description", "");
		size_t end = d.find(". ");
		end = end == std::string::npos ? d.size() : end + 1;
		s.about = d.substr(0, std::min<size_t>(end, 160));
		if (end > 160)
			s.about += "...";
	}
	s.resultSeconds = resultSeconds_;
	s.newBest = hasResult_ && resultNewBest_;
	if (s.timed && bests_)
		s.bestSeconds = bests_->best(bestKey());
	return s;
}
