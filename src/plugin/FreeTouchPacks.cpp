#include "pch.h"
#include "plugin/FreeTouch.h"

#include <ctime>
#include <fstream>

// Training packs: while the player plays a custom training pack (the game's own mode), each shot is recorded and
// the pack is saved as one Free Touch drill in content/captured/packs/ (category "packs", Training packs on the
// Exercises page). Nothing is changed in training: the shots are only read. Game thread. Only src/game/ calls the SDK.

namespace
{
	std::string packStamp()
	{
		const std::time_t t = std::time(nullptr);
		std::tm tm{};
		localtime_s(&tm, &t);
		char buf[32];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
		return buf;
	}
}

void FreeTouch::onTrainingTick(const Snapshot& s, const TrainingInfo& t)
{
	if (!packImport_.on && !cvarManager->getCvar("ft_record_packs").getBoolValue())
		return;
	// One line per shot reached, with how things stand on its first tick, for debugging.
	if (const std::string key = t.code + "|" + t.name + "|" + std::to_string(t.round); key != packRoundLogged_)
	{
		packRoundLogged_ = key;
		const CarState* me = s.localCar();
		eventLog_.write("pack_round", {{"pack", t.name}, {"code", t.code}, {"creator", t.creator}, {"trainingType", t.type},
			{"round", t.round + 1}, {"rounds", t.rounds}, {"secondsLeft", t.secondsLeft},
			{"ball", s.ball ? statejson::toJson(s.ball->location) : nlohmann::ordered_json(nullptr)},
			{"ballSpeed", s.ball ? s.ball->velocity.length() : -1.f}, {"carSpeed", me ? me->velocity.length() : -1.f}, {"paused", s.paused}});
	}
	trainingNow_ = t;
	if (++trainingTicks_ % 30 == 0)
		publishStatus(); // the Packs page shows the pack being played (every 30 frames)
	const bool launched = packRecorder_.feed(s, t);
	if (packImport_.on)
	{
		tickPackImport(s, t);
		return;
	}
	if (!launched)
		return;
	const packs::Shot& shot = packRecorder_.last();
	const int number = shot.round + 1;
	const std::string id = packs::packId(t);
	if (id != packChoicesKey_)
		savePackChoices(t); // loads the saved shots first (writes nothing new yet)
	packChoices_[number] = packs::shotChoice(shot, number, t.rounds);
	savePackChoices(t);
	const auto path = contentRoot() / "captured" / "packs" / (id.substr(6) + ".json");
	eventLog_.write("pack_shot", {{"pack", t.name}, {"code", t.code}, {"round", number}, {"rounds", t.rounds},
		{"recorded", packChoices_.size()}, {"seconds", shot.seconds}, {"ball", statejson::toJson(shot.ballLocation)},
		{"ballVelocity", statejson::toJson(shot.ballVelocity)}, {"car", statejson::toJson(shot.carLocation)},
		{"yaw", shot.carRotation.yaw}, {"file", path.string()}});
	char buf[160];
	snprintf(buf, sizeof(buf), "Free Touch saved shot %d  (%zu of %d from this pack)", number, packChoices_.size(),
		std::max(t.rounds, static_cast<int>(packChoices_.size())));
	hud_.showHint(buf, 2.2f);
}

void FreeTouch::savePackChoices(const TrainingInfo& t)
{
	// The shots saved before (an earlier session) stay; new ones are added or replaced. A first call for a pack only
	// loads them.
	const auto dir = contentRoot() / "captured" / "packs";
	const std::string id = packs::packId(t);
	const auto path = dir / (id.substr(6) + ".json"); // without "packs."
	if (id != packChoicesKey_)
	{
		packChoicesKey_ = id;
		packChoices_.clear();
		std::ifstream in(path);
		if (in)
		{
			try
			{
				packChoices_ = packs::savedChoices(nlohmann::ordered_json::parse(in));
			}
			catch (const std::exception&)
			{
			}
		}
		return;
	}
	auto doc = packs::scenario(t, packChoices_, packStamp());
	if (doc.is_null())
		return;
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::ofstream out(path, std::ios::trunc);
	out << doc.dump(2) << "\n";
	if (!out)
	{
		LOG("Free Touch: could not write the training pack file {}", path.string());
		eventLog_.write("pack_write_failed", {{"path", path.string()}});
		return;
	}
	packsDirty_ = true;
}

void FreeTouch::startPackImport(const std::string& codeText)
{
	// "" imports the pack the player is in; a code opens that pack in custom training first.
	const std::string code = codeText.empty() ? std::string() : packs::normalizeCode(codeText);
	if (!codeText.empty() && code.empty())
	{
		packImport_.message = "That doesn't look like a pack code (16 letters and numbers, like 42BF-686D-E047-574B).";
		publishStatus();
		return;
	}
	if (code.empty() && !(trainingNow_.active && game_->inCustomTraining()))
	{
		packImport_.message = "Open a pack in custom training first, or type its code.";
		publishStatus();
		return;
	}
	const int startMethod = packImport_.startMethod; // the start call that worked last time goes first
	packImport_ = PackImport{};
	packImport_.startMethod = startMethod;
	packImport_.on = true;
	packImport_.code = code;
	packImport_.since = std::chrono::steady_clock::now();
	packImport_.message = code.empty() ? "Reading the shots of the pack you're in..." : "Opening " + code + " in custom training...";
	eventLog_.write("pack_import_start", {{"code", code.empty() ? "current" : code}, {"inTraining", trainingNow_.active}});
	if (!code.empty() && !(trainingNow_.active && trainingNow_.code == code))
		cvarManager->executeCommand("load_training " + code);
	if (isWindowOpen_)
		cvarManager->executeCommand("togglemenu " + GetMenuName());
	publishStatus();
}

void FreeTouch::tickPackImport(const Snapshot& s, const TrainingInfo& t)
{
	PackImport& pi = packImport_;
	// Waiting for the game to open the pack.
	if (!pi.started)
	{
		const bool ours = t.active && (pi.code.empty() || t.code == pi.code);
		if (!ours || t.rounds <= 0)
		{
			if (std::chrono::steady_clock::now() - pi.since > std::chrono::seconds(60))
				finishPackImport(t, "the pack did not open (is the code right, and can the game reach its training packs?)");
			return;
		}
		pi.started = true;
		eventLog_.write("pack_import_open", {{"pack", t.name}, {"code", t.code}, {"rounds", t.rounds}, {"round", t.round + 1}});
		// A fresh read: the pack is written again from what is read now (no leftovers from an earlier read).
		packChoicesKey_ = packs::packId(t);
		packChoices_.clear();
	}
	if (std::chrono::steady_clock::now() - pi.since > std::chrono::seconds(40 + 6 * std::max(1, t.rounds)))
	{
		finishPackImport(t, "it took too long");
		return;
	}
	const CarState* me = s.localCar();
	if (!s.ball || !me)
		return;
	int good = 0;
	for (const auto& [r, q] : pi.quality)
		good += q >= 1 ? 1 : 0;
	char buf[220];
	snprintf(buf, sizeof(buf), "Free Touch is reading \"%s\": %d of %d shots.%s", t.name.c_str(), good, t.rounds,
		pi.askNext ? " Press the game's next-shot button to go on." : pi.askTap ? " Tap throttle once to start it." : " Hands off the controller.");
	hud_.showHint(buf, 0.3f);

	using Phase = PackImport::Phase;
	// Frame bookkeeping: a ball not where its last position and speed would put it was placed (teleported) this frame.
	const auto nowClock = std::chrono::steady_clock::now();
	if (pi.haveLast)
		pi.frameSeconds = std::clamp(std::chrono::duration<float>(nowClock - pi.lastFrame).count(), 1.f / 360.f, 0.1f);
	const bool placedBall = s.ball->location.z > 50.f || std::fabs(s.ball->location.x) + std::fabs(s.ball->location.y) > 1.f;
	// (A bounce can put a ball up to two frames of travel off its straight line, so only more than that counts.)
	const Vec3 expected = pi.lastBall + pi.lastVel * pi.frameSeconds;
	const float slack = std::max(40.f, 2.5f * pi.lastVel.length() * pi.frameSeconds);
	const bool firstSight = !pi.haveLast;
	const bool teleported = firstSight || (placedBall && (s.ball->location - expected).length() > slack);
	pi.lastBall = s.ball->location;
	pi.lastVel = s.ball->velocity;
	pi.lastFrame = nowClock;
	pi.haveLast = true;
	const bool moving = s.ball->velocity.length() > 5.f;
	if (t.round != pi.round)
	{
		pi.round = t.round;
		pi.roundTick = s.tick;
		pi.phase = Phase::Wait;
	}

	auto shotHere = [&]() {
		packs::Shot sh;
		sh.round = t.round;
		sh.ballLocation = s.ball->location;
		sh.ballVelocity = s.ball->velocity;
		sh.ballSpin = s.ball->angularVelocity;
		sh.carLocation = me->location;
		sh.carRotation = me->rotation;
		sh.seconds = t.secondsLeft;
		return sh;
	};
	// Filed under the game's shot number; a better reading replaces a weaker one.
	auto record = [&](const packs::Shot& sh, int quality, const char* how) {
		const int round = sh.round;
		const auto have = pi.quality.find(round);
		if (have != pi.quality.end() && have->second >= quality)
			return;
		pi.quality[round] = quality;
		packChoices_[round + 1] = packs::shotChoice(sh, round + 1, t.rounds);
		eventLog_.write("pack_import_shot", {{"round", round + 1}, {"rounds", t.rounds}, {"how", how}, {"quality", quality},
			{"seconds", sh.seconds}, {"ball", statejson::toJson(sh.ballLocation)}, {"ballVelocity", statejson::toJson(sh.ballVelocity)},
			{"car", statejson::toJson(sh.carLocation)}, {"yaw", sh.carRotation.yaw}, {"startMethod", pi.startMethod}});
	};
	auto stepOn = [&]() {
		int best = 0, any = 0;
		for (int r = 0; r < t.rounds; ++r)
		{
			const auto q = pi.quality.find(r);
			any += q != pi.quality.end() ? 1 : 0;
			best += q != pi.quality.end() && q->second >= 2 ? 1 : 0;
		}
		// Done when every shot has a launch reading; after two laps a weaker reading will do; after four, what there is.
		if (best >= t.rounds || (any >= t.rounds && pi.steps > t.rounds * 2) || pi.steps > t.rounds * 4)
		{
			finishPackImport(t, any == 0 ? "no shots could be read" : "");
			return;
		}
		pi.stepping = true;
		pi.stepFrom = t.round;
		pi.stepTick = s.tick;
		pi.tries = 0;
		pi.askNext = false;
		++pi.steps;
		pi.method = pi.method < 10 ? pi.method : 0;
		game_->trainingNextRound(pi.method);
	};
	const auto known = pi.quality.find(t.round);
	const bool haveGood = known != pi.quality.end() && known->second >= 2;

	switch (pi.phase)
	{
	case Phase::Wait:
	{
		// Placed flying (stepped while the last one was live): read it on this first frame. Only right after the shot
		// changed or a step was asked, so nothing mid-flight is taken for a new shot.
		const bool recent = firstSight || pi.stepping || s.tick <= pi.roundTick + 30;
		if (placedBall && teleported && moving && recent)
		{
			pi.stepping = false;
			if (!haveGood)
				record(shotHere(), 2, "launched_on_placing");
			stepOn();
			return;
		}
		// Placed frozen: its exact start, then start it (unless this shot is already read: on to the next).
		packs::Shot sh;
		if (packRecorder_.round() == t.round && packRecorder_.frozen(sh, 8))
		{
			pi.stepping = false;
			if (haveGood)
			{
				stepOn();
				return;
			}
			pi.shot = sh;
			pi.phase = Phase::Launch;
			pi.phaseTick = s.tick;
			pi.tries = 0;
			pi.askTap = false;
			game_->trainingNextRound(pi.startMethod);
			eventLog_.write("pack_import_start_try", {{"round", t.round + 1}, {"method", pi.startMethod}});
			return;
		}
		// A shot that is flying but was never seen being placed (read late, so weaker: a later lap replaces it).
		if (!pi.stepping && moving && placedBall && s.tick > pi.roundTick + 90)
		{
			record(shotHere(), 1, "late");
			stepOn();
			return;
		}
		// A step asked for and nothing new: try the other ways on, then ask for the game's button.
		if (pi.stepping && !pi.askNext && t.round == pi.stepFrom && s.tick > pi.stepTick + 60)
		{
			++pi.tries;
			if (pi.tries >= 5)
			{
				pi.askNext = true;
				eventLog_.write("pack_import_ask_next", {{"round", t.round + 1}});
				return;
			}
			static const int kStep[] = {0, 1, 2, 10, 11};
			pi.method = kStep[pi.tries % 5];
			pi.stepTick = s.tick;
			game_->trainingNextRound(pi.method);
			eventLog_.write("pack_import_step_try", {{"from", pi.stepFrom + 1}, {"method", pi.method}, {"tries", pi.tries}});
		}
		return;
	}
	case Phase::Launch:
	{
		// Its first moving frame gives the launch (the start is the exact frozen one).
		packs::Shot& sh = pi.shot;
		const float ballMoved = (s.ball->location - sh.ballLocation).length();
		const float carMoved = (me->location - sh.carLocation).length();
		if (ballMoved > 0.5f && moving)
		{
			sh.ballVelocity = s.ball->velocity;
			sh.ballSpin = s.ball->angularVelocity;
			record(sh, 2, pi.askTap ? "player_tap" : "started");
			if (pi.askTap)
				pi.startMethod = 23; // none worked: from the top next time
			pi.askTap = false;
			pi.phase = Phase::Wait;
			stepOn();
			return;
		}
		if (carMoved > 3.f && s.tick > pi.phaseTick + 40)
		{
			sh.ballVelocity = {}; // the car is going and the ball is not: a still ball (weaker: a later lap may replace it)
			record(sh, 1, "still_ball");
			pi.askTap = false;
			pi.phase = Phase::Wait;
			stepOn();
			return;
		}
		if (pi.askTap || s.tick < pi.phaseTick + 40)
			return;
		static const int kStart[] = {23, 24, 25, 20, 21, 22};
		++pi.tries;
		if (pi.tries >= 6)
		{
			pi.askTap = true;
			eventLog_.write("pack_import_tap_needed", {{"round", t.round + 1}});
			return;
		}
		int at = 0;
		while (at < 6 && kStart[at] != pi.startMethod)
			++at;
		pi.startMethod = kStart[(at + 1) % 6];
		pi.phaseTick = s.tick;
		game_->trainingNextRound(pi.startMethod);
		eventLog_.write("pack_import_start_try", {{"round", t.round + 1}, {"method", pi.startMethod}});
		return;
	}
	}
}

void FreeTouch::finishPackImport(const TrainingInfo& t, const std::string& why)
{
	packImport_.on = false;
	if (!why.empty())
	{
		packImport_.message = "Import stopped: " + why + ".";
		hud_.showHint("Free Touch: import stopped (" + why + ")", 4.f);
		eventLog_.write("pack_import_failed", {{"code", packImport_.code}, {"why", why}, {"read", packChoices_.size()}});
		if (!packChoices_.empty())
			savePackChoices(t);
		publishStatus();
		return;
	}
	savePackChoices(t);
	std::string missing;
	for (int r = 0; r < t.rounds; ++r)
		if (!packImport_.quality.count(r))
			missing += (missing.empty() ? "" : ", ") + std::to_string(r + 1);
	// A pack whose frozen balls all read still would play wrong: say so (playing its shots records them properly).
	int still = 0;
	for (const auto& [n, c] : packChoices_)
	{
		const auto& v = c["ball"]["velocity"];
		if (std::fabs(v[0].get<double>()) + std::fabs(v[1].get<double>()) + std::fabs(v[2].get<double>()) < 5.0)
			++still;
	}
	const std::string id = packs::packId(t);
	char buf[200];
	snprintf(buf, sizeof(buf), "Imported %zu of %d shots from \"%s\".%s", packChoices_.size(), t.rounds, t.name.c_str(),
		still == static_cast<int>(packChoices_.size()) ? " Every ball read as still: play a few shots in training to record them properly." : "");
	packImport_.message = buf;
	if (!missing.empty())
		packImport_.message += " Not read: shot " + missing + " (Re-read to try again).";
	eventLog_.write("pack_import_done", {{"pack", t.name}, {"code", t.code}, {"shots", packChoices_.size()}, {"rounds", t.rounds},
		{"missing", missing}, {"steps", packImport_.steps}, {"stillBalls", still},
		{"method", packImport_.method}, {"id", id}});
	hud_.showHint(std::string(buf) + " Loading Free Play...", 4.f);
	// Straight into it: Free Play loads and the pack starts (with the defender and net options).
	startInFreeplay([this, id] {
		reloadPacksIfDirty();
		startExercise(id, "medium", {});
	}, id);
	publishStatus();
}

void FreeTouch::deletePack(const std::string& id)
{
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
	if (!item || item->category != "packs")
		return;
	const auto dir = contentRoot() / "captured" / "packs";
	std::error_code ec;
	const auto rel = std::filesystem::relative(item->file, dir, ec);
	if (ec || rel.empty() || rel.native()[0] == '.')
		return; // only files inside captured/packs
	std::filesystem::remove(item->file, ec);
	eventLog_.write("pack_deleted", {{"id", id}, {"ok", !ec}});
	packChoicesKey_.clear();
	reloadContent();
	publishStatus();
}

void FreeTouch::reloadPacksIfDirty()
{
	if (!packsDirty_)
		return;
	packsDirty_ = false;
	reloadContent();
}
