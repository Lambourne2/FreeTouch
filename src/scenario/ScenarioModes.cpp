#include "pch.h"
#include "scenario/ScenarioPlayer.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "data/EventLog.h"
#include "data/StateJson.h"

// The mechanics modes on top of the scenario player (Phase 4): mechanic chains, the lit zone on the net, HORSE,
// Build Your Combo and Style Rush. Game thread. No SDK calls.

namespace
{
	constexpr double kTicks = 120.0;

	uint32_t nextRand(uint32_t& s)
	{
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return s;
	}

	std::string upper(std::string s)
	{
		for (auto& ch : s)
			ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		return s;
	}

	std::string lower(std::string s)
	{
		for (auto& ch : s)
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		return s;
	}

	std::string clock(double seconds)
	{
		const int t = static_cast<int>(std::ceil(std::max(0.0, seconds)));
		char buf[16];
		snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
		return buf;
	}

	// Mechanics a Style Rush bonus can call.
	const char* kBonusPool[] = {"flip_reset", "air_dribble", "double_touch", "flick", "ceiling_shot", "pinch", "wall_touch"};
}

// ---------------------------------------------------------------- chains

std::vector<std::string> ScenarioPlayer::chainTokens() const
{
	std::vector<std::string> out;
	if (!current_)
		return out;
	const nlohmann::json& s = current_->resolved["success"];
	const std::string type = s.value("type", "");
	if ((type == "mechanic" || type == "detector_chain") && s.contains("chain"))
		for (const auto& t : s["chain"])
			out.push_back(t.get<std::string>());
	if (type == "detector" && s.contains("detector"))
		out.push_back(s["detector"].get<std::string>());
	return out;
}

bool ScenarioPlayer::tokenMatches(const std::string& token, const RepMechanic& m)
{
	const size_t colon = token.find(':');
	std::string id = token.substr(0, colon);
	if (id == "backboard_touch")
		id = "backboard";
	if (m.id != id)
		return false;
	if (colon == std::string::npos)
		return true;
	return lower(m.label).find(lower(token.substr(colon + 1))) != std::string::npos;
}

std::string ScenarioPlayer::tokenName(const std::string& token)
{
	const size_t colon = token.find(':');
	std::string id = token.substr(0, colon);
	if (id == "backboard_touch" || id == "backboard")
		return "BACKBOARD";
	std::replace(id.begin(), id.end(), '_', ' ');
	std::string name = upper(id);
	if (colon == std::string::npos)
		return name;
	const std::string word = upper(token.substr(colon + 1));
	return !word.empty() && word[0] == 'X' ? name + " " + word : word + " " + name;
}

bool ScenarioPlayer::mechanicSuccess() const
{
	const std::string t = successType();
	return t == "mechanic" || t == "detector" || t == "detector_chain";
}

void ScenarioPlayer::resetModeRep()
{
	repMechanics_.clear();
	chainStep_ = 0;
	pendingGoal_ = PendingGoal{};
	rushPot_ = 0;
	rushKinds_.clear();
	rushDeadTicks_ = 0;
	rushLastTouchTick_ = lastTick_;
}

void ScenarioPlayer::onMechanic(const MechanicEvent& e)
{
	if (!current_ || hasResult_ || countdownUntilTick_ != 0 || leadInPhase_ != LeadInPhase::Off || e.id == "touch")
		return;
	RepMechanic m{e.id, e.label, e.tick, e.points};
	repMechanics_.push_back(m);

	if (isRush() && !rushOver_ && e.show && e.points > 0 && e.id != "goal")
	{
		const bool bonus = !rushBonus_.empty() && lastTick_ < rushBonusUntil_ && tokenMatches(rushBonus_, m);
		const int pts = e.points * (bonus ? 3 : 1);
		rushPot_ += pts;
		if (std::find(rushKinds_.begin(), rushKinds_.end(), e.id) == rushKinds_.end())
			rushKinds_.push_back(e.id);
		log_.write("rush_mechanic", {{"rep", rep_}, {"id", e.id}, {"points", pts}, {"bonus", bonus}, {"pot", rushPot_},
			{"multiplier", static_cast<int>(rushKinds_.size())}});
	}

	const auto tokens = chainTokens();
	if (chainStep_ < static_cast<int>(tokens.size()) && tokenMatches(tokens[chainStep_], m))
	{
		++chainStep_;
		log_.write("chain_step", {{"exercise", current_->id}, {"rep", rep_}, {"step", chainStep_}, {"of", tokens.size()},
			{"mechanic", e.id}, {"label", e.label}});
		const nlohmann::json& s = current_->resolved["success"];
		const bool requireGoal = s.value("requireGoal", successType() == "mechanic");
		if (mechanicSuccess() && chainStep_ == static_cast<int>(tokens.size()) && !requireGoal)
			setResult(+1, s.value("label", std::string("LANDED")), e.kmh);
	}
}

// ---------------------------------------------------------------- runs and setups

void ScenarioPlayer::modeNewRun(const nlohmann::json& data, const std::string& scenarioId, const std::string& reason)
{
	const std::string prog = data.value("progression", "");
	if (prog == "horse" && (reason == "play" || horseOver_ || horseScenario_ != scenarioId))
	{
		horseScenario_ = scenarioId;
		horseLetters_ = horseMade_ = 0;
		horseOver_ = false;
		level_[scenarioId] = 0;
		log_.write("horse_start", {{"exercise", scenarioId}, {"word", data.value("word", std::string("HORSE"))}});
	}
	if (prog == "combo" && reason == "play")
		level_[scenarioId] = 0;
	if (data.contains("rush") && (reason == "play" || rushOver_ || rushScenario_ != scenarioId))
	{
		rushScenario_ = scenarioId;
		rushSecondsLeft_ = data["rush"].value("minutes", 2.0) * 60.0;
		rushScore_ = rushPlays_ = rushBestPlay_ = 0;
		rushOver_ = false;
		rushBonus_.clear();
		rushBonusUntil_ = 0;
		rushNextBonus_ = lastTick_ + static_cast<uint64_t>(data["rush"].value("firstBonusSeconds", 15.0) * kTicks);
		rushRng_ = static_cast<uint32_t>(lastTick_ * 2654435761u) | 1u;
		log_.write("rush_start", {{"exercise", scenarioId}, {"minutes", rushSecondsLeft_ / 60.0}});
	}
}

void ScenarioPlayer::setupZones(ResolvedScenario& r)
{
	zones_ = Zones{};
	if (!r.resolved.contains("objects"))
		return;
	nlohmann::json kept = nlohmann::json::array();
	for (const auto& o : r.resolved["objects"])
	{
		if (o.value("type", "") != "zone_cycle")
		{
			kept.push_back(o);
			continue;
		}
		zones_.on = true;
		if (o.contains("grid") && o["grid"].size() == 2)
		{
			zones_.cols = std::clamp(o["grid"][0].get<int>(), 1, 5);
			zones_.rows = std::clamp(o["grid"][1].get<int>(), 1, 4);
		}
		zones_.cycleSeconds = std::max(0.3, o.value("cycleSeconds", 1.0));
		zones_.side = o.value("surface", std::string("orange_goal")) == "blue_goal" ? -1 : 1;
		zones_.rng = r.seed | 1u;
		zones_.lit = static_cast<int>(nextRand(zones_.rng) % (zones_.cols * zones_.rows));
		zones_.nextTick = lastTick_ + static_cast<uint64_t>(zones_.cycleSeconds * kTicks);
		zones_.blocking = o.value("blocking", false);
		zones_.hole = std::max(0.f, o.value("hole", 0.f));
		if (o.contains("lit") && o["lit"].is_number_integer())
		{
			zones_.fixed = true;
			zones_.lit = std::clamp(o["lit"].get<int>(), 0, zones_.cols * zones_.rows - 1);
			zones_.nextTick = UINT64_MAX;
		}
		if (o.contains("open") && o["open"].is_array())
		{
			// Several open tiles (a Saved Shot's net): fixed, the rest of the net closed.
			for (const auto& c : o["open"])
				if (c.is_number_integer() && c.get<int>() >= 0 && c.get<int>() < zones_.cols * zones_.rows)
					zones_.open |= 1u << c.get<int>();
			if (zones_.open != 0)
			{
				zones_.fixed = true;
				zones_.nextTick = UINT64_MAX;
				for (int c = 0; c < zones_.cols * zones_.rows; ++c)
					if (zones_.open & (1u << c))
					{
						zones_.lit = c;
						break;
					}
			}
		}
	}
	r.resolved["objects"] = kept;
}

void ScenarioPlayer::placeNearCar(ResolvedScenario& r, bool keepCar)
{
	// "ball": {"nearCar": {...}}: a ball fed in front of the car (where it is now when the car is kept): `ahead`,
	// `side`, `height`, coming back toward the car at `toward` and rising at `up`.
	if (!r.resolved.contains("ball") || !r.resolved["ball"].contains("nearCar"))
		return;
	const nlohmann::json& n = r.resolved["ball"]["nearCar"];
	Vec3 base = r.player.location;
	int yawUnits = r.player.rotation.yaw;
	if (keepCar)
	{
		const Snapshot snap = game_.snapshot();
		if (const CarState* me = snap.localCar())
		{
			base = me->location;
			yawUnits = me->rotation.yaw;
		}
	}
	const float yaw = yawUnits * 3.14159265f / 32768.f;
	const Vec3 fwd(std::cos(yaw), std::sin(yaw), 0.f), side(-std::sin(yaw), std::cos(yaw), 0.f);
	Vec3 p = base + fwd * n.value("ahead", 1200.f) + side * n.value("side", 0.f);
	// Inside the field, out of the nets, clear of the corner walls.
	p.x = std::clamp(p.x, -3700.f, 3700.f);
	p.y = std::clamp(p.y, -4500.f, 4500.f);
	const float over = std::fabs(p.x) + std::fabs(p.y) - 7300.f;
	if (over > 0.f)
	{
		p.x -= std::copysign(over * 0.5f, p.x);
		p.y -= std::copysign(over * 0.5f, p.y);
	}
	p.z = std::clamp(n.value("height", 600.f), 93.f, 1800.f);
	// Toward the car (from wherever the clamp left the ball), plus the rise.
	Vec3 back = base - p;
	back.z = 0.f;
	const float len = std::max(1.f, back.length());
	r.ball.location = p;
	r.ball.velocity = back * (n.value("toward", 300.f) / len) + Vec3(0.f, 0.f, n.value("up", 0.f));
	r.ball.angularVelocity = {};
}

// ---------------------------------------------------------------- every tick

void ScenarioPlayer::tickModes(const Snapshot& snap)
{
	// The lit zone moves on.
	if (zones_.on && !zones_.fixed && snap.tick >= zones_.nextTick)
	{
		const int cells = zones_.cols * zones_.rows;
		if (cells > 1)
		{
			int next = zones_.lit;
			while (next == zones_.lit)
				next = static_cast<int>(nextRand(zones_.rng) % cells);
			zones_.lit = next;
		}
		const uint64_t step = static_cast<uint64_t>(zones_.cycleSeconds * kTicks);
		zones_.nextTick = zones_.nextTick + step > snap.tick ? zones_.nextTick + step : snap.tick + step;
	}

	if (pendingGoal_.on && snap.tick >= pendingGoal_.judgeTick)
		judgeModeGoal();
	if (hasResult_)
		return;

	// "ball_hits_ground": after the player's first touch the ball must not land.
	if (failHas("ball_hits_ground") && touches_ > 0 && snap.ball && snap.ball->location.z < 100.f
		&& snap.tick > lastPlayerTouchTick_ + 10)
	{
		log_.write("ball_dropped", {{"exercise", current_->id}, {"rep", rep_}, {"ballLocation", statejson::toJson(snap.ball->location)}});
		resultDetail_ = repMechanics_.empty() ? "" : "LANDED " + std::to_string(chainStep_) + " OF " + std::to_string(chainTokens().size());
		setResult(-1, "DROPPED", 0.f);
		return;
	}

	if (!isRush() || rushOver_ || !released_ || snap.paused || ballExploded_ || goalHeld_)
		return;
	// Style Rush: the clock, bonus call-outs, dead balls.
	rushSecondsLeft_ -= 1.0 / kTicks;
	if (rushSecondsLeft_ <= 0.0)
	{
		endRush();
		return;
	}
	const nlohmann::json& rush = current_->resolved["rush"];
	if (!rushBonus_.empty() && snap.tick >= rushBonusUntil_)
		rushBonus_.clear();
	if (snap.tick >= rushNextBonus_)
	{
		rushBonus_ = kBonusPool[nextRand(rushRng_) % std::size(kBonusPool)];
		rushBonusUntil_ = snap.tick + static_cast<uint64_t>(rush.value("bonusSeconds", 15.0) * kTicks);
		rushNextBonus_ = snap.tick + static_cast<uint64_t>(rush.value("bonusEverySeconds", 25.0) * kTicks);
		log_.write("rush_bonus", {{"mechanic", rushBonus_}});
	}
	if (snap.ball)
	{
		const bool onFloor = snap.ball->location.z < 110.f && std::fabs(snap.ball->velocity.z) < 250.f;
		rushDeadTicks_ = onFloor ? rushDeadTicks_ + 1 : 0;
	}
	const double sinceTouch = (snap.tick - std::max(lastPlayerTouchTick_, rushLastTouchTick_)) / kTicks;
	// Dead: after a touch, the ball lying on the floor for a second; before any touch, a while to get to it.
	if ((touches_ > 0 && rushDeadTicks_ > 120 && sinceTouch > 1.0) || sinceTouch > rush.value("idleSeconds", 7.0))
		endRushPlay("dead ball", false);
}

int ScenarioPlayer::zoneAt(const Vec3& p) const
{
	const float w = 1786.f / zones_.cols, h = 643.f / zones_.rows;
	const int c = std::clamp(static_cast<int>((p.x + 893.f) / w), 0, zones_.cols - 1);
	const int r = std::clamp(static_cast<int>(p.z / h), 0, zones_.rows - 1);
	return r * zones_.cols + c;
}

Vec3 ScenarioPlayer::zoneCentre(int cell) const
{
	const float w = 1786.f / zones_.cols, h = 643.f / zones_.rows;
	const int c = cell % zones_.cols, r = cell / zones_.cols;
	return Vec3(-893.f + (c + 0.5f) * w, zones_.side * 5120.f, (r + 0.5f) * h);
}

bool ScenarioPlayer::zoneHit(const Vec3& p) const
{
	if (zones_.hole > 0.f)
	{
		// Through the hole only if the whole ball fits (its centre within the hole's radius less the ball's).
		const Vec3 c = zoneCentre(zones_.lit);
		const float dx = p.x - c.x, dz = p.z - c.z;
		return std::sqrt(dx * dx + dz * dz) <= zones_.hole - 92.75f;
	}
	if (zones_.open != 0)
		return (zones_.open >> zoneAt(p)) & 1u;
	return zoneAt(p) == zones_.lit;
}

void ScenarioPlayer::blockZoneMiss(const Snapshot& snap)
{
	// The net is closed outside the lit tile (or the hole): a ball reaching its face there bounces back off it.
	if (!zones_.on || !zones_.blocking || !snap.ball || !released_ || ballExploded_ || goalHeld_)
		return;
	const BallState& b = *snap.ball;
	const float face = zones_.side * 5120.f;
	const bool heading = b.velocity.y * zones_.side > 0.f;
	const bool atFace = std::fabs(b.location.y - face) <= 92.75f + std::fabs(b.velocity.y) / kTicks + 2.f
		&& (b.location.y - face) * zones_.side < 0.f;
	const bool inMouth = std::fabs(b.location.x) < 893.f && b.location.z < 643.f;
	if (!heading || !atFace || !inMouth || zoneHit(b.location))
		return;
	BallTarget bounce;
	bounce.location = Vec3(b.location.x, face - zones_.side * (92.75f + 4.f), b.location.z);
	bounce.velocity = Vec3(b.velocity.x * 0.85f, -b.velocity.y * 0.6f, b.velocity.z * 0.85f);
	bounce.angularVelocity = b.angularVelocity;
	game_.setBall(bounce);
	log_.write("zone_miss", {{"exercise", current_ ? current_->id : ""}, {"rep", rep_}, {"cell", zoneAt(b.location)},
		{"lit", zones_.lit}, {"hole", zones_.hole}, {"at", statejson::toJson(b.location)}});
	if (!hasResult_)
		setResult(-1, "MISS", b.velocity.length() * 0.036f);
}

void ScenarioPlayer::drawZones(Canvas& canvas) const
{
	if (!zones_.on || !current_)
		return;
	const float y = zones_.side * 5100.f;
	const float w = 1786.f / zones_.cols, h = 643.f / zones_.rows;
	if (zones_.hole > 0.f)
	{
		// A ring the size of the hole, a faint one around it, and the mouth's outline (the rest is closed).
		const Vec3 c = zoneCentre(zones_.lit);
		const Rgba col = hasResult_ ? Rgba{255, 255, 255, 90} : Rgba{255, 205, 60, 255};
		for (int k = 0; k < 40; ++k)
		{
			const float a0 = k * 6.2831853f / 40.f, a1 = (k + 1) * 6.2831853f / 40.f;
			canvas.line3d(Vec3(c.x + std::cos(a0) * zones_.hole, y, c.z + std::sin(a0) * zones_.hole),
				Vec3(c.x + std::cos(a1) * zones_.hole, y, c.z + std::sin(a1) * zones_.hole), col, 6.f, 2.f);
			canvas.line3d(Vec3(c.x + std::cos(a0) * (zones_.hole + 40.f), y, c.z + std::sin(a0) * (zones_.hole + 40.f)),
				Vec3(c.x + std::cos(a1) * (zones_.hole + 40.f), y, c.z + std::sin(a1) * (zones_.hole + 40.f)), col.withAlpha(0.3f), 3.f, 1.f);
		}
		const Rgba edge{255, 255, 255, 50};
		canvas.line3d(Vec3(-887.f, y, 6.f), Vec3(887.f, y, 6.f), edge, 2.f, 1.f);
		canvas.line3d(Vec3(887.f, y, 6.f), Vec3(887.f, y, 637.f), edge, 2.f, 1.f);
		canvas.line3d(Vec3(887.f, y, 637.f), Vec3(-887.f, y, 637.f), edge, 2.f, 1.f);
		canvas.line3d(Vec3(-887.f, y, 637.f), Vec3(-887.f, y, 6.f), edge, 2.f, 1.f);
		return;
	}
	// How long the current zone has left (it flickers in its last fifth, so the switch can be read).
	const double left = (zones_.nextTick > lastTick_ ? zones_.nextTick - lastTick_ : 0) / kTicks;
	const bool flicker = left < zones_.cycleSeconds * 0.2 && (lastTick_ / 6) % 2 == 0;
	for (int i = 0; i < zones_.cols * zones_.rows; ++i)
	{
		const int c = i % zones_.cols, r = i / zones_.cols;
		const float x0 = -893.f + c * w, x1 = x0 + w, z0 = r * h + 4.f, z1 = z0 + h - 8.f;
		const Vec3 a(x0 + 6.f, y, z0), b(x1 - 6.f, y, z0), cc(x1 - 6.f, y, z1), d(x0 + 6.f, y, z1);
		const bool lit = zones_.open != 0 ? ((zones_.open >> i) & 1u) != 0 : i == zones_.lit;
		if (lit && !hasResult_)
		{
			const Rgba col = flicker ? Rgba{255, 255, 255, 230} : Rgba{255, 205, 60, 255};
			canvas.line3d(a, b, col, 7.f, 2.f);
			canvas.line3d(b, cc, col, 7.f, 2.f);
			canvas.line3d(cc, d, col, 7.f, 2.f);
			canvas.line3d(d, a, col, 7.f, 2.f);
			// Hatching across the lit cell.
			for (int k = 1; k < 6; ++k)
			{
				const float t = k / 6.f;
				canvas.line3d(Vec3(x0 + 6.f + (w - 12.f) * t, y, z0), Vec3(x0 + 6.f, y, z0 + (z1 - z0) * t), col.withAlpha(0.6f), 3.f, 1.f);
				canvas.line3d(Vec3(x1 - 6.f, y, z0 + (z1 - z0) * t), Vec3(x0 + 6.f + (w - 12.f) * t, y, z1), col.withAlpha(0.6f), 3.f, 1.f);
			}
		}
		else
		{
			const Rgba col{255, 255, 255, 55};
			canvas.line3d(a, b, col, 2.f, 1.f);
			canvas.line3d(b, cc, col, 2.f, 1.f);
			canvas.line3d(cc, d, col, 2.f, 1.f);
			canvas.line3d(d, a, col, 2.f, 1.f);
		}
	}
}

// ---------------------------------------------------------------- goals and results

bool ScenarioPlayer::modeGoal(int side, const Vec3& at, float kmh)
{
	if (!current_ || hasResult_ || pendingGoal_.on)
		return false;
	const bool zoneRule = successType() == "goal_in_zone" || (mechanicSuccess() && current_->resolved["success"].value("zone", false));
	if (!isRush() && !mechanicSuccess() && !zoneRule)
		return false;
	// Judged a moment later, so a mechanic on the scoring touch (a touch is judged 8 ticks after it, a flick once its
	// dodge's touches are over, up to 24 more) still counts.
	pendingGoal_.on = true;
	pendingGoal_.judgeTick = lastTick_ + 36;
	pendingGoal_.attacking = side != defendedNet_;
	pendingGoal_.at = at;
	pendingGoal_.kmh = kmh;
	pendingGoal_.zone = zones_.on && zoneRule ? (zoneHit(at) ? 1 : 0) : -1;
	log_.write("mode_goal", {{"exercise", current_->id}, {"rep", rep_}, {"attacking", pendingGoal_.attacking},
		{"zone", pendingGoal_.zone}, {"cell", zones_.on ? zoneAt(at) : -1}, {"lit", zones_.on ? zones_.lit : -1},
		{"chainStep", chainStep_}});
	return true;
}

void ScenarioPlayer::judgeModeGoal()
{
	pendingGoal_.on = false;
	if (hasResult_ || !current_)
		return;
	const float kmh = pendingGoal_.kmh;
	if (isRush())
	{
		if (pendingGoal_.attacking)
			endRushPlay("goal", true);
		else
		{
			rushPot_ = 0;
			endRushPlay("own goal", false);
		}
		return;
	}
	if (!pendingGoal_.attacking)
	{
		setResult(-1, "OWN GOAL", kmh);
		return;
	}
	const auto tokens = chainTokens();
	if (chainStep_ < static_cast<int>(tokens.size()))
	{
		resultDetail_ = chainStep_ > 0 ? "LANDED " + std::to_string(chainStep_) + " OF " + std::to_string(tokens.size()) : "";
		setResult(-1, "NO " + tokenName(tokens[chainStep_]), kmh);
		return;
	}
	if (pendingGoal_.zone == 0)
	{
		setResult(-1, "WRONG ZONE", kmh);
		return;
	}
	const nlohmann::json& s = current_->resolved["success"];
	setResult(+1, s.value("label", std::string(pendingGoal_.zone == 1 ? "IN THE ZONE" : "GOAL")), kmh);
}

void ScenarioPlayer::endRushPlay(const char* why, bool goal)
{
	const int mult = std::max<int>(1, static_cast<int>(rushKinds_.size()));
	const int gained = goal ? (rushPot_ > 0 ? rushPot_ * mult * 2 : 5) : rushPot_;
	rushScore_ += gained;
	++rushPlays_;
	rushBestPlay_ = std::max(rushBestPlay_, gained);
	log_.write("rush_play", {{"exercise", current_->id}, {"rep", rep_}, {"why", why}, {"pot", rushPot_}, {"multiplier", mult},
		{"gained", gained}, {"score", rushScore_}});
	const std::string score = "SCORE " + std::to_string(rushScore_);
	std::string label, detail;
	if (goal)
	{
		label = "+" + std::to_string(gained);
		detail = (rushPot_ > 0 ? (mult > 1 ? "x" + std::to_string(mult) + " COMBO  x2 GOAL" : "x2 GOAL") : "GOAL") + std::string("   |   ") + score;
	}
	else if (rushPot_ > 0)
	{
		label = "+" + std::to_string(gained);
		detail = "BANKED   |   " + score + "   |   FINISH WITH A GOAL TO MULTIPLY";
	}
	else
	{
		label = std::string(why) == "own goal" ? "OWN GOAL" : "NEXT BALL";
		detail = score;
	}
	resultDetail_ = detail;
	setResult(goal ? +1 : 0, label, 0.f);
}

void ScenarioPlayer::endRush()
{
	rushOver_ = true;
	rushScore_ += rushPot_; // a play still going is banked
	const std::string key = "rush|" + current_->id + "|" + current_->difficulty;
	const double best = bests_ ? bests_->bestMax(key) : 0.0;
	const bool newBest = bests_ && bests_->recordMax(key, rushScore_);
	log_.write("rush_end", {{"exercise", current_->id}, {"score", rushScore_}, {"plays", rushPlays_}, {"bestPlay", rushBestPlay_},
		{"previousBest", best}, {"newBest", newBest}});
	resultDetail_ = (newBest ? std::string("NEW BEST") : "BEST " + std::to_string(static_cast<int>(best))) + "   |   BEST PLAY "
		+ std::to_string(rushBestPlay_) + "   |   NEW RUN IN 6 S";
	rushNewBest_ = newBest;
	setResult(+1, std::to_string(rushScore_) + " PTS", 0.f);
}

std::string ScenarioPlayer::horseWord() const
{
	return current_ ? upper(current_->resolved.value("word", std::string("HORSE"))) : "HORSE";
}

void ScenarioPlayer::modeAfterResult(int outcome, std::string& label)
{
	if (!current_)
		return;
	const std::string prog = progression();
	if (isRush() && rushOver_)
		resultNewBest_ = rushNewBest_;
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, current_->id);
	const int levels = item ? std::max<int>(1, static_cast<int>(levelIds(*item).size())) : 1;
	int& level = level_[current_->id];
	auto stepLabel = [&](int i) {
		std::string l;
		if (item)
			for (const auto& obj : item->data.value("objects", nlohmann::json::array()))
			{
				const auto choices = obj.value("choices", nlohmann::json::array());
				if (i >= 0 && i < static_cast<int>(choices.size()))
					l = choices[i].value("label", "");
			}
		return upper(l);
	};

	if (prog == "horse")
	{
		const std::string word = horseWord();
		auto letters = [&] {
			std::string out;
			for (size_t i = 0; i < word.size(); ++i)
				out += (i ? " " : "") + std::string(1, static_cast<int>(i) < horseLetters_ ? word[i] : '_');
			return out;
		};
		// The next call: the next shot up the list; past the end, a random one from the hardest third.
		auto advance = [&] {
			if (level + 1 < levels)
				++level;
			else
			{
				const int from = std::max(0, levels - std::max(1, levels / 3));
				uint32_t s = static_cast<uint32_t>(lastTick_ | 1u);
				level = from + static_cast<int>(nextRand(s) % static_cast<uint32_t>(levels - from));
			}
		};
		if (outcome > 0)
		{
			++horseMade_;
			resultDetail_ = "MADE " + std::to_string(horseMade_) + "   |   " + letters();
			label = "MADE IT";
		}
		else
		{
			const std::string why = label;
			++horseLetters_;
			if (horseLetters_ >= static_cast<int>(word.size()))
			{
				horseOver_ = true;
				const std::string key = "horse|" + current_->id + "|" + current_->difficulty;
				const double best = bests_ ? bests_->bestMax(key) : 0.0;
				const bool newBest = bests_ && bests_->recordMax(key, horseMade_);
				std::string spelled;
				for (size_t i = 0; i < word.size(); ++i)
					spelled += (i ? "-" : "") + std::string(1, word[i]);
				label = spelled;
				resultDetail_ = "MADE " + std::to_string(horseMade_) + " SHOTS   |   " + (newBest ? std::string("NEW BEST")
					: "BEST " + std::to_string(static_cast<int>(best))) + "   |   NEW GAME IN 6 S";
				resultNewBest_ = newBest;
				log_.write("horse_end", {{"exercise", current_->id}, {"made", horseMade_}, {"newBest", newBest}});
			}
			else
			{
				label = "LETTER " + std::string(1, word[horseLetters_ - 1]);
				resultDetail_ = why + "   |   " + letters() + "   |   MADE " + std::to_string(horseMade_);
			}
		}
		log_.write("horse_shot", {{"exercise", current_->id}, {"shot", current_->resolved.value("objectiveId", "")},
			{"made", outcome > 0}, {"letters", horseLetters_}, {"total", horseMade_}});
		if (!horseOver_)
			advance();
	}
	else if (prog == "combo")
	{
		if (outcome > 0)
		{
			if (level >= levels - 1)
			{
				label = "LADDER CLEARED";
				resultDetail_ = "ALL " + std::to_string(levels) + " STEPS   |   GO AGAIN AT THE TOP";
			}
			else
			{
				++level;
				label = "STEP " + std::to_string(level + 1) + " UNLOCKED";
				resultDetail_ = "NEXT: " + stepLabel(level);
			}
		}
		else
		{
			const std::string why = label;
			if (current_->resolved.value("dropOnMiss", false) && level > 0)
				--level;
			resultDetail_ = why + "   |   AGAIN: " + stepLabel(level);
		}
		log_.write("combo_step", {{"exercise", current_->id}, {"landed", outcome > 0}, {"level", level}, {"of", levels}});
	}
}

void ScenarioPlayer::modeStatus(Status& s) const
{
	if (!current_)
		return;
	s.showMechanics = wantsMechanicsFeed();
	const std::string prog = progression();
	const auto tokens = chainTokens();
	if (!tokens.empty() && !isRush())
	{
		// The chain, ticked off as it lands: "AIR DRIBBLE OK  >  DOUBLE TOUCH  >  GOAL IN THE LIT ZONE".
		std::string line;
		for (size_t i = 0; i < tokens.size(); ++i)
			line += (i ? "  >  " : "") + tokenName(tokens[i]) + (static_cast<int>(i) < chainStep_ ? " OK" : "");
		const nlohmann::json& sc = current_->resolved["success"];
		const bool goal = sc.value("requireGoal", successType() == "mechanic");
		if (goal)
			line += std::string("  >  ") + (sc.value("zone", false) ? "GOAL IN THE LIT ZONE" : "GOAL");
		s.objectiveProgress = line;
		if (s.objective.empty())
		{
			// A single-mechanic drill: name it ("FLICK INTO THE LIT ZONE").
			std::string what;
			for (size_t i = 0; i < tokens.size(); ++i)
				what += (i ? " + " : "") + tokenName(tokens[i]);
			s.objective = what + (!goal ? "" : sc.value("zone", false) ? " INTO THE LIT ZONE" : " GOAL");
			s.plainObjective = true;
		}
	}
	if (prog == "horse")
	{
		const std::string word = horseWord();
		std::string letters;
		for (size_t i = 0; i < word.size(); ++i)
			letters += (i ? " " : "") + std::string(1, static_cast<int>(i) < horseLetters_ ? word[i] : '_');
		s.objective = "CALLED: " + upper(current_->resolved.value("objectiveLabel", ""));
		s.objectiveProgress = letters + "   MADE " + std::to_string(horseMade_)
			+ (s.objectiveProgress.empty() ? "" : "   |   " + s.objectiveProgress);
		s.plainObjective = true;
	}
	if (prog == "combo")
	{
		const auto* item = content_.find(ContentLibrary::Kind::Scenario, current_->id);
		const int levels = item ? static_cast<int>(levelIds(*item).size()) : 1;
		const auto it = level_.find(current_->id);
		const int level = it == level_.end() ? 0 : it->second;
		s.objective = "STEP " + std::to_string(level + 1) + " / " + std::to_string(levels) + ": "
			+ upper(current_->resolved.value("objectiveLabel", ""));
		s.plainObjective = true;
	}
	if (isRush())
	{
		const int mult = std::max<int>(1, static_cast<int>(rushKinds_.size()));
		s.objective = "SCORE " + std::to_string(rushScore_) + "   " + clock(rushSecondsLeft_);
		std::string pot = rushPot_ > 0 ? "PLAY " + std::to_string(rushPot_) + (mult > 1 ? "  x" + std::to_string(mult) : "") : "";
		if (!rushBonus_.empty() && lastTick_ < rushBonusUntil_)
			pot += (pot.empty() ? "" : "   |   ") + std::string("BONUS: ") + tokenName(rushBonus_) + " x3  "
				+ std::to_string(static_cast<int>(std::ceil((rushBonusUntil_ - lastTick_) / kTicks))) + "S";
		s.objectiveProgress = pot;
		s.objectiveTag.clear();
		s.objectiveAt.reset();
		s.plainObjective = true;
		s.quietObjective = true;
	}
}
