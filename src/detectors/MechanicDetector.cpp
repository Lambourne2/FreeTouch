#include "pch.h"
#include "detectors/MechanicDetector.h"

#include <algorithm>
#include <cmath>

#include "scenario/BallPredict.h"
#include "scenario/GoalGuard.h"

namespace
{
	constexpr float kBallRadius = 92.75f;
	constexpr uint64_t kTicksPerSecond = 120;
	constexpr uint64_t kJudgeDelayTicks = 8;   // a touch's hit shows in the ball 2 ticks later; a pinch builds for a few more
	constexpr float kCeilingCarZ = 1900.f;     // a car this high with its wheels on something is on the ceiling

	nlohmann::json vec(const Vec3& v)
	{
		auto r = [](float f) { return std::round(f * 10.f) / 10.f; };
		return nlohmann::json::array({r(v.x), r(v.y), r(v.z)});
	}
	float round2(float f) { return std::round(f * 100.f) / 100.f; }
}

void MechanicDetector::orientation(const Rot& r, Vec3& forward, Vec3& up)
{
	// Unreal rotator -> the car's nose and roof directions (as RLBot / RLUtilities convert them).
	const float k = 3.14159265f / 32768.f;
	const float cp = std::cos(r.pitch * k), sp = std::sin(r.pitch * k);
	const float cy = std::cos(r.yaw * k), sy = std::sin(r.yaw * k);
	const float cr = std::cos(r.roll * k), sr = std::sin(r.roll * k);
	forward = Vec3(cp * cy, cp * sy, sp);
	up = Vec3(-cr * cy * sp - sr * sy, -cr * sy * sp + sr * cy, cp * cr);
}

float MechanicDetector::pitchStep(const Rot& from, const Rot& to)
{
	// The new nose against the old roof: sin of the turn about the car's own side axis. Physical vectors, so it does not
	// care how Unreal writes a rotation past straight up or down.
	Vec3 f0, u0, f1, u1;
	orientation(from, f0, u0);
	orientation(to, f1, u1);
	return std::asin(std::clamp(f1.dot(u0), -1.f, 1.f));
}

bool MechanicDetector::CarSample::onRealSurface() const
{
	if (realKnown)
		return real;
	return onWall || (onGround && (location.z < 120.f || nearSurface(location, 220.f)));
}

bool MechanicDetector::realSurface(const CarState& me) const
{
	// 2026-10-19: resets near a wall or the ceiling (the wall pop's first touches) were read as landings by distance
	// alone; the wheel counts tell the ball from the wall (3 wheels touching, 0 of them the world, on a logged reset).
	// And the game says "on a wall" when the car sits tilted on the ball (Lambourne's HORSE run, 1.0.0.187: four
	// resets, all four wheels on the ball 100 uu from its centre, 0 world wheels, logged as wall landings), so a wall
	// counts only with world wheels or the car actually at a wall.
	if (me.onWall)
		return (wheelCountsWork_ && me.worldWheelContacts > 0) || nearSurface(me.location, 250.f);
	if (!me.onGround)
		return false;
	if (me.location.z < 60.f || me.location.z > kCeilingCarZ)
		return true;
	if (wheelCountsWork_)
		return me.worldWheelContacts > 0;
	return me.location.z < 120.f || nearSurface(me.location, 220.f);
}

void MechanicDetector::logFlight(const char* why)
{
	if (flightTouches_ > 0 && !flightLog_.empty())
		emit("flight", "FLIGHT", tick_, 0.f,
			{{"why", why}, {"seconds", round2((tick_ - leftSurfaceTick_) / 120.f)}, {"touches", flightTouches_},
				{"resets", flightResets_}, {"wheelCountsWork", wheelCountsWork_}, {"changes", flightLog_}},
			false);
	flightLog_ = nlohmann::json::array();
	flightTouches_ = 0;
}

nlohmann::json MechanicDetector::trace(double seconds) const
{
	nlohmann::json rows = nlohmann::json::array();
	const size_t want = static_cast<size_t>(std::clamp(seconds, 0.1, 3.0) * 120.0);
	const size_t from = trace_.size() > want ? trace_.size() - want : 0;
	auto r1 = [](float f) { return std::round(f * 10.f) / 10.f; };
	for (size_t i = from; i < trace_.size(); ++i)
	{
		const auto& t = trace_[i];
		const auto& c = t.car;
		rows.push_back({c.tick, r1(c.location.x), r1(c.location.y), r1(c.location.z), r1(c.velocity.x), r1(c.velocity.y), r1(c.velocity.z),
			c.rotation.pitch, c.rotation.yaw, c.rotation.roll, c.onGround ? 1 : 0, c.onWall ? 1 : 0, c.hasFlip ? 1 : 0, c.jumped ? 1 : 0,
			c.doubleJumped ? 1 : 0, r1(t.ball.location.x), r1(t.ball.location.y), r1(t.ball.location.z), r1(t.ball.velocity.x),
			r1(t.ball.velocity.y), r1(t.ball.velocity.z)});
	}
	return rows;
}

bool MechanicDetector::nearSurface(const Vec3& p, float pad)
{
	return std::fabs(p.x) > 4096.f - pad || std::fabs(p.y) > 5120.f - pad || p.z > 2044.f - pad
		|| std::fabs(p.x) + std::fabs(p.y) > 8064.f - pad * 1.4142f;
}

int MechanicDetector::pointsFor(const std::string& id)
{
	if (id == "aerial_touch") return 1;
	if (id == "wall_touch") return 1;
	if (id == "ceiling_touch") return 2;
	if (id == "flick") return 3;
	if (id == "air_dribble") return 3;
	if (id == "redirect") return 3;
	if (id == "backboard_read") return 3;
	if (id == "power_shot") return 2;
	if (id == "pinch") return 4;
	if (id == "double_touch") return 5;
	if (id == "ceiling_shot") return 5;
	if (id == "flip_reset") return 6;
	if (id == "double_reset") return 10;
	if (id == "goal") return 2;
	return 0;
}

void MechanicDetector::emit(const std::string& id, const std::string& label, uint64_t tick, float kmh, nlohmann::json values,
	bool show, int points)
{
	MechanicEvent e;
	e.id = id;
	e.label = label;
	e.tick = tick;
	e.points = points >= 0 ? points : pointsFor(id);
	e.show = show;
	e.kmh = kmh;
	e.values = std::move(values);
	if (show && e.points >= 3)
		recent_.push_back({tick, id, label, e.points});
	out_.push_back(std::move(e));
}

std::string MechanicDetector::resetName(int n)
{
	static const char* kNames[] = {"FLIP", "DOUBLE", "TRIPLE", "QUAD", "PENTA", "HEXA", "HEPTA", "OCTA", "NONA", "DECA"};
	if (n >= 1 && n <= 10)
		return std::string(kNames[n - 1]) + " RESET";
	return "RESET x" + std::to_string(n);
}

void MechanicDetector::reportReset(uint64_t tick, nlohmann::json values)
{
	// Each reset in one flight (no floor, wall or ceiling in between) counts up: the feed shows FLIP RESET, then
	// DOUBLE, TRIPLE, QUAD, PENTA ... RESET (Lambourne, 2026-10-19), worth 5 points a reset from the double on. Every
	// one is also a flip_reset (logged, and it ticks a flip_reset step of a chain), shown only as the first.
	++flightResets_;
	values["resetsThisFlight"] = flightResets_;
	emit("flip_reset", "FLIP RESET", tick, 0.f, values, flightResets_ == 1);
	if (flightResets_ >= 2)
		emit("double_reset", resetName(flightResets_), tick, 0.f, values, true, 5 * flightResets_);
	lastResetTick_ = tick;
}

std::vector<MechanicEvent> MechanicDetector::take()
{
	std::vector<MechanicEvent> events;
	events.swap(out_);
	return events;
}

void MechanicDetector::reset()
{
	pending_.clear();
	car_.reset();
	ball_.reset();
	airborne_ = false;
	flightResets_ = flightAerialTouches_ = 0;
	ceilingLeftTick_ = 0;
	onCeiling_ = false;
	airChain_ = 0;
	ballBouncedSinceTouch_ = backboardSinceTouch_ = false;
	dribbleTicks_ = 0;
	flick_ = FlickCandidate{};
	flagsBackTick_ = 0;
	dodgeStartTick_ = 0;
	airDodge_ = AirDodge{};
	lastRawLocalTouchTick_ = 0;
	lastFlickTick_ = 0;
	flightLog_ = nlohmann::json::array();
	flightTouches_ = 0;
	recent_.clear();
	goalSeen_ = false;
	// The last rep's touches are not part of this play (a ball thrown off the backboard is not a double touch).
	lastLocalTouchTick_ = 0;
	lastTouchLocal_ = false;
}

void MechanicDetector::onTouch(const TouchEvent& e)
{
	// The goal-replay copy of a car re-fires the scoring touch: only touches by simulated cars count.
	if (!e.simulated)
		return;
	lastTouchTick_ = e.tick;
	lastTouchLocal_ = e.isLocal;
	if (!e.isLocal)
	{
		// Someone else touched it: the player's chain (air dribble, double touch) is broken.
		ballBouncedSinceTouch_ = true;
		airChain_ = 0;
		return;
	}
	lastRawLocalTouchTick_ = e.tick;
	lastRawLocalTouchBallVel_ = e.ballVelocity;
	if (airborne_)
		++flightTouches_;
	if (airDodge_.on && e.tick <= airDodge_.start + 78)
	{
		if (airDodge_.touches == 0)
		{
			airDodge_.firstTouch = e.tick;
			if (!airDodge_.haveBaseline)
			{
				airDodge_.baseline = e.ballVelocity;
				airDodge_.haveBaseline = true;
			}
		}
		++airDodge_.touches;
		airDodge_.lastTouch = e.tick;
	}
	PendingTouch p;
	p.touch = e;
	if (car_)
		p.car = *car_;
	else
		p.car.location = e.carLocation;
	p.ballSpeedBefore = e.ballVelocity.length();
	p.before = carHistory_.size() >= 3 ? carHistory_[carHistory_.size() - 3] : p.car;
	for (size_t i = carHistory_.size() >= 5 ? carHistory_.size() - 4 : 1; i < carHistory_.size(); ++i)
		p.pitchTurn += pitchStep(carHistory_[i - 1].rotation, carHistory_[i].rotation);
	p.dodged = p.car.doubleJumped || !p.car.hasFlip;
	p.bouncedBefore = ballBouncedSinceTouch_;
	p.backboardBefore = backboardSinceTouch_;
	p.backboardTick = backboardTick_;
	ballBouncedSinceTouch_ = backboardSinceTouch_ = false; // from here on: since this touch
	pending_.push_back(p);
}

void MechanicDetector::onTick(const Snapshot& s)
{
	tick_ = s.tick;
	const CarState* me = s.localCar();
	if (me)
	{
		attackSide_ = me->team == 1 ? -1 : 1;
		if (me->onGround && me->location.z < 30.f && me->worldWheelContacts >= 3)
			wheelCountsWork_ = true;
		watchCar(*me, s);
		watchAirDodge(*me, s);
	}
	watchBall(s);

	// Touches whose hit has landed in the ball (the fastest the ball got meanwhile is kept, for pinches).
	std::vector<PendingTouch> keep;
	for (auto& p : pending_)
	{
		if (me)
		{
			CarSample now;
			now.location = me->location;
			now.onGround = me->onGround;
			now.onWall = me->onWall;
			now.hasFlip = me->hasFlip;
			now.jumped = me->jumped;
			now.doubleJumped = me->doubleJumped;
			now.realKnown = true;
			now.real = realSurface(*me);
			if (now.onRealSurface())
				p.leftAir = true;
			else if (now.fresh())
				p.regained = true;
			if (me->doubleJumped || !me->hasFlip)
				p.dodged = true;
			if (car_)
				p.pitchTurn += pitchStep(car_->rotation, me->rotation);
		}
		if (s.ball && s.tick >= p.touch.tick + 2 && s.ball->velocity.length() > p.peakSpeed)
		{
			p.peakSpeed = s.ball->velocity.length();
			p.peakVelocity = s.ball->velocity;
		}
		if (s.tick < p.touch.tick + kJudgeDelayTicks)
			keep.push_back(p);
		else
			judgeTouch(p, s);
	}
	pending_ = std::move(keep);

	if (flick_.on && (s.tick > flick_.tick + 24 || (me && car_ && car_->onRealSurface())))
	{
		emit("flick", flick_.type, flick_.tick, flick_.kmh, flick_.values);
		lastFlickTick_ = flick_.tick;
		flick_ = FlickCandidate{};
		dribbleTicks_ = 0;
	}

	while (!recent_.empty() && s.tick > recent_.front().tick + 4 * kTicksPerSecond)
		recent_.pop_front();

	// A goal off the player's last touch, named after the best mechanic of the play.
	if (s.ball)
	{
		if (std::fabs(s.ball->location.y) < 4000.f)
			goalSeen_ = false;
		const auto c = goalguard::check(*s.ball);
		if (c.caught && !goalSeen_ && c.side == attackSide_ && lastTouchLocal_ && lastLocalTouchTick_ != 0)
		{
			goalSeen_ = true;
			std::string best, bestLabel;
			int bestPoints = 0;
			for (const auto& r : recent_)
				if (r.points >= bestPoints)
				{
					best = r.id;
					bestLabel = r.label;
					bestPoints = r.points;
				}
			std::string label = "GOAL";
			if (best == "double_reset")
			{
				label = bestLabel + " GOAL"; // TRIPLE RESET GOAL
			}
			else if (!best.empty())
			{
				std::string name = best;
				std::replace(name.begin(), name.end(), '_', ' ');
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
				label = name + " GOAL";
			}
			const float kmh = s.ball->velocity.length() * 0.036f;
			emit("goal", label, s.tick, kmh, {{"ballLocation", vec(s.ball->location)}, {"kmh", round2(kmh)}, {"best", best}});
		}
	}
	if (me)
	{
		CarSample c;
		c.tick = s.tick;
		c.location = me->location;
		c.velocity = me->velocity;
		c.angularVelocity = me->angularVelocity;
		c.rotation = me->rotation;
		c.onGround = me->onGround;
		c.onWall = me->onWall;
		c.hasFlip = me->hasFlip;
		c.jumped = me->jumped;
		c.doubleJumped = me->doubleJumped;
		c.wheels = me->wheelContacts;
		c.worldWheels = me->worldWheelContacts;
		c.realKnown = true;
		c.real = realSurface(*me);
		car_ = c;
		carHistory_.push_back(c);
		while (carHistory_.size() > 16)
			carHistory_.pop_front();
		trace_.push_back({c, s.ball ? *s.ball : BallState{}});
		while (trace_.size() > 360)
			trace_.pop_front();
	}
	if (s.ball)
		ball_ = *s.ball;
}

void MechanicDetector::watchCar(const CarState& me, const Snapshot& s)
{
	// The game reports "on the ground" whenever the wheels touch something, the ball included. Only the floor, the
	// walls and the ceiling are landings; wheels on the ball in mid-air are what a flip reset is.
	const bool nearBall = s.ball && (me.location - s.ball->location).length() < 330.f;
	const bool realSurface = this->realSurface(me);
	// Also: 3+ wheels touching something that is not the world (the game's wheel counts).
	const bool wheelsOffWorld = me.wheelContacts >= 3 && me.worldWheelContacts >= 0
		&& me.wheelContacts - me.worldWheelContacts >= 3;
	const bool wheelsOnBall = (me.onGround || wheelsOffWorld) && !realSurface && nearBall;
	// With working wheel counts, wheels down on nothing of the world are on the ball or a car: the flight goes on.
	const bool surface = realSurface || (me.onGround && !wheelsOnBall && !wheelCountsWork_);

	// The flight log: what changed this tick (flags, "on the ground", wheel counts), while in the air.
	if (airborne_ && car_ && flightLog_.size() < 60
		&& (car_->hasFlip != me.hasFlip || car_->jumped != me.jumped || car_->doubleJumped != me.doubleJumped
			|| car_->onGround != me.onGround || car_->wheels != me.wheelContacts || car_->worldWheels != me.worldWheelContacts))
	{
		std::string flags;
		if (me.jumped) flags += 'j';
		if (me.doubleJumped) flags += 'd';
		if (me.hasFlip) flags += 'h';
		if (me.onGround) flags += 'g';
		flightLog_.push_back({{"t", round2((s.tick - leftSurfaceTick_) / 120.f)}, {"flags", flags},
			{"wheels", {me.wheelContacts, me.worldWheelContacts}}, {"z", std::round(me.location.z)},
			{"ballDistance", s.ball ? std::round((me.location - s.ball->location).length()) : -1.f},
			{"real", realSurface}});
	}
	if (wheelsOnBall)
	{
		// Wheels on the ball: still in the air, the flight goes on. Once per contact.
		if (!wheelsOnBall_ && airborne_ && s.tick > leftSurfaceTick_ + 15 && me.location.z > 120.f && s.tick > lastResetTick_ + 30)
		{
			const float d = (me.location - s.ball->location).length();
			nlohmann::json v = {{"carLocation", vec(me.location)}, {"ballLocation", vec(s.ball->location)},
				{"distance", round2(d)}, {"secondsInAir", round2((s.tick - leftSurfaceTick_) / 120.f)},
				{"how", me.onGround ? "wheels on the ball" : "wheel contacts"},
				{"hasFlip", me.hasFlip}, {"jumped", me.jumped}, {"doubleJumped", me.doubleJumped},
				{"wheelContacts", me.wheelContacts}, {"worldWheelContacts", me.worldWheelContacts}};
			reportReset(s.tick, v);
		}
		wheelsOnBall_ = true;
	}
	else
	{
		wheelsOnBall_ = false;
	}
	if (wheelsOnBall)
	{
		// (handled above)
	}
	else if (surface)
	{
		if (me.location.z > kCeilingCarZ)
		{
			if (airborne_)
				logFlight("ceiling");
			onCeiling_ = true;
		}
		else
		{
			// Landed on the floor or a wall: the flight and its chains are over.
			if (airborne_)
				logFlight(me.onWall ? "wall" : "floor");
			if (airborne_ || onCeiling_)
			{
				flightResets_ = flightAerialTouches_ = 0;
				ceilingLeftTick_ = 0;
				airChain_ = 0;
			}
			onCeiling_ = false;
		}
		airborne_ = false;
	}
	else if (!airborne_)
	{
		airborne_ = true;
		leftSurfaceTick_ = s.tick;
		flightResets_ = 0;
		flightTouches_ = 0;
		std::string flags;
		if (me.jumped) flags += 'j';
		if (me.doubleJumped) flags += 'd';
		if (me.hasFlip) flags += 'h';
		flightLog_ = nlohmann::json::array({{{"t", 0}, {"flags", flags}, {"left", me.location.z > kCeilingCarZ - 100.f ? "ceiling"
			: me.location.z > 200.f ? "wall" : "floor"}, {"z", std::round(me.location.z)}}});
		if (onCeiling_)
		{
			ceilingLeftTick_ = s.tick; // dropped off the ceiling with a flip: a ceiling shot is on
			onCeiling_ = false;
		}
	}

	// Flip reset by the flags (Lambourne's rule, 2026-10-19): in the air with the jump used, the flip used or its time
	// run out, then a fresh flip (has flip, jump flags clear) with no floor, wall or ceiling: only the ball or a car can
	// give it back, so no distance is needed (it is logged). Confirmed 3 ticks later with the car still off every
	// surface (the flags can come back a tick before a landing shows).
	// Not covered: leaving a wall or the ceiling without jumping keeps the flip, so wheels on the ball then change no
	// flag (only "on the ground" or the wheel counts can show that contact).
	if (flagsBackTick_ != 0)
	{
		if (realSurface || !airborne_)
		{
			flagsBackTick_ = 0; // it was a landing
		}
		else if (s.tick >= flagsBackTick_ + 3)
		{
			if (flagsBackTick_ > lastResetTick_ + 30)
			{
				reportReset(flagsBackTick_, flagsBackValues_);
			}
			flagsBackTick_ = 0;
		}
	}
	const bool freshNow = me.hasFlip && !me.jumped && !me.doubleJumped;
	const bool spentBefore = car_ && (car_->jumped || car_->doubleJumped || !car_->hasFlip);
	if (car_ && airborne_ && !realSurface && spentBefore && freshNow && me.location.z > 60.f && s.tick > leftSurfaceTick_ + 10
		&& !nearSurface(me.location, 40.f))
	{
		flagsBackTick_ = s.tick;
		const float d = s.ball ? (me.location - s.ball->location).length() : -1.f;
		flagsBackValues_ = {{"carLocation", vec(me.location)}, {"ballLocation", s.ball ? vec(s.ball->location) : nlohmann::json()},
			{"distance", round2(d)}, {"secondsInAir", round2((s.tick - leftSurfaceTick_) / 120.f)}, {"how", "flip flags back"},
			{"flagsBefore", {{"hasFlip", car_->hasFlip}, {"jumped", car_->jumped}, {"doubleJumped", car_->doubleJumped}}},
			{"wheelContacts", me.wheelContacts}, {"worldWheelContacts", me.worldWheelContacts}};
	}
	if (car_ && !car_->doubleJumped && me.doubleJumped)
		dodgeStartTick_ = s.tick;

	// Dribble: the ball sitting on the roof of the car (on the floor) - what a flick comes off.
	if (s.ball)
	{
		const Vec3 rel = s.ball->location - me.location;
		const bool onRoof = me.onGround && me.location.z < 80.f && rel.z > 90.f && rel.z < 320.f && std::hypot(rel.x, rel.y) < 210.f;
		if (onRoof)
		{
			++dribbleTicks_;
			dribbleLastTick_ = s.tick;
		}
		else if (s.tick > dribbleLastTick_ + 200)
		{
			dribbleTicks_ = 0;
		}
	}
}

void MechanicDetector::watchAirDodge(const CarState& me, const Snapshot& s)
{
	const bool onSurface = realSurface(me);
	if (!airDodge_.on)
	{
		// A dodge starting in the air with the ball near.
		if (car_ && !car_->doubleJumped && me.doubleJumped && !onSurface && me.location.z > 250.f && s.ball
			&& (me.location - s.ball->location).length() < 700.f)
		{
			airDodge_ = AirDodge{};
			airDodge_.on = true;
			airDodge_.start = s.tick;
			airDodge_.carZ = me.location.z;
			float dx = car_->velocity.x, dy = car_->velocity.y;
			if (std::hypot(dx, dy) < 300.f)
			{
				const float yaw = car_->rotation.yaw * 3.14159265f / 32768.f;
				dx = std::cos(yaw);
				dy = std::sin(yaw);
			}
			const float dl = std::max(1e-3f, std::hypot(dx, dy));
			airDodge_.travel = Vec3(dx / dl, dy / dl, 0.f);
			if (lastRawLocalTouchTick_ != 0 && s.tick <= lastRawLocalTouchTick_ + 12)
			{
				// Touch, then the backflip: that touch is part of it.
				airDodge_.baseline = lastRawLocalTouchBallVel_;
				airDodge_.touches = 1;
				airDodge_.firstTouch = airDodge_.lastTouch = lastRawLocalTouchTick_;
			}
			else
			{
				airDodge_.baseline = s.ball->velocity;
			}
			airDodge_.haveBaseline = true;
		}
		return;
	}
	if (car_ && s.tick <= airDodge_.start + 40)
		airDodge_.pitchTurn += pitchStep(car_->rotation, me.rotation);
	const bool ballAtNet = s.ball && std::fabs(s.ball->location.y) > 4900.f;
	const bool due = airDodge_.touches > 0
		&& (s.tick >= std::max(airDodge_.lastTouch + 12, airDodge_.start + 50) || ballAtNet);
	if (!(due || onSurface || s.tick > airDodge_.start + 90))
		return;
	if (airDodge_.touches > 0 && s.ball && lastFlickTick_ + 12 < airDodge_.start)
	{
		const Vec3& v = s.ball->velocity;
		const Vec3 d = v - airDodge_.baseline;
		const Vec3& t = airDodge_.travel;
		const float gainForward = d.x * t.x + d.y * t.y;
		const float gainAtNet = d.y * static_cast<float>(attackSide_);
		const float gain = std::max(gainForward, gainAtNet);
		const float goingForward = std::max(v.x * t.x + v.y * t.y, v.y * static_cast<float>(attackSide_));
		const float kmh = v.length() * 0.036f;
		const bool musty = airDodge_.pitchTurn > 0.35f && gain > 250.f && goingForward > 600.f && v.length() > 900.f;
		nlohmann::json values = {{"pitchTurnDegrees", round2(airDodge_.pitchTurn * 57.2958f)},
			{"gainForwardKmh", round2(gainForward * 0.036f)}, {"gainAtNetKmh", round2(gainAtNet * 0.036f)},
			{"forwardKmh", round2(goingForward * 0.036f)}, {"kmh", round2(kmh)}, {"touches", airDodge_.touches},
			{"firstTouchSecondsFromDodge", round2((static_cast<float>(airDodge_.firstTouch) - airDodge_.start) / 120.f)},
			{"lastTouchSecondsFromDodge", round2((static_cast<float>(airDodge_.lastTouch) - airDodge_.start) / 120.f)},
			{"carZ", round2(airDodge_.carZ)}, {"ballLocation", vec(s.ball->location)}, {"type", "AIR MUSTY FLICK"}};
		if (musty)
			emit("flick", "AIR MUSTY FLICK", airDodge_.firstTouch, kmh, values);
		else
			emit("musty_check", "NOT A MUSTY", airDodge_.firstTouch, kmh, values, false); // logged for tuning
	}
	airDodge_ = AirDodge{};
}

void MechanicDetector::watchBall(const Snapshot& s)
{
	if (!s.ball || !ball_ || s.tick < lastTouchTick_ + 3)
		return;
	const Vec3& p = s.ball->location;
	const Vec3& v = s.ball->velocity;
	const Vec3& pv = ball_->velocity;
	const float r = kBallRadius + 40.f;
	auto bounce = [&](const char* surface) {
		ballBouncedSinceTouch_ = true;
		(void)surface;
	};
	// Floor: rolling on it, or coming off it.
	if (p.z < kBallRadius + 15.f || (p.z < r + 20.f && pv.z < -80.f && v.z > pv.z + 80.f))
		bounce("floor");
	// Side walls.
	if (std::fabs(p.x) > 4096.f - r && pv.x * p.x > 0.f && std::fabs(pv.x) > 80.f && v.x * p.x < 0.f)
		bounce("side wall");
	// Ceiling.
	if (p.z > 2044.f - r && pv.z > 80.f && v.z < 0.f)
		bounce("ceiling");
	// Back walls: above the crossbar it is the backboard.
	if (std::fabs(p.y) > 5120.f - r && pv.y * p.y > 0.f && std::fabs(pv.y) > 80.f && v.y * p.y < 0.f)
	{
		if (p.z > 650.f)
		{
			backboardSinceTouch_ = true;
			backboardTick_ = s.tick;
			emit("backboard", "BACKBOARD", s.tick, v.length() * 0.036f,
				{{"ballLocation", vec(p)}, {"net", p.y > 0.f ? "orange" : "blue"}}, false);
		}
		else
		{
			bounce("back wall");
		}
	}
}

void MechanicDetector::judgeTouch(const PendingTouch& p, const Snapshot& s)
{
	const CarSample& car = p.car;
	const uint64_t tick = p.touch.tick;
	// The ball's speed after the hit: the fastest it got over the ticks after the touch.
	const float speedAfter = std::max(p.peakSpeed, s.ball ? s.ball->velocity.length() : 0.f);
	const Vec3 velAfter = p.peakSpeed > 0.f ? p.peakVelocity : s.ball ? s.ball->velocity : Vec3();
	const float kmh = speedAfter * 0.036f;
	const Vec3 ballAt = p.touch.ballLocation;
	// In the air: off the floor, walls and ceiling. A touch with the wheels on the ball (the game says "on the ground")
	// is still an aerial touch.
	const bool aerial = !car.onRealSurface() && car.location.z > 120.f && ballAt.z > 180.f;
	const bool wall = car.onWall && car.onRealSurface() && car.location.z > 120.f && car.location.z < kCeilingCarZ;
	const bool ceiling = !car.airborne() && car.location.z > kCeilingCarZ;
	const double sinceLast = lastLocalTouchTick_ ? (tick - lastLocalTouchTick_) / 120.0 : 99.0;

	nlohmann::json base = {
		{"carLocation", vec(car.location)},
		{"ballLocation", vec(ballAt)},
		{"kmhAfter", round2(kmh)},
		{"kmhBefore", round2(p.ballSpeedBefore * 0.036f)},
		{"carAirborne", car.airborne()},
		{"carOnWall", car.onWall},
		{"secondsSinceLastTouch", round2(static_cast<float>(sinceLast))},
		{"carFlags", {{"onGround", car.onGround}, {"onWall", car.onWall}, {"hasFlip", car.hasFlip}, {"jumped", car.jumped},
			{"doubleJumped", car.doubleJumped}}},
		{"flagsBefore", {{"hasFlip", p.before.hasFlip}, {"jumped", p.before.jumped}, {"doubleJumped", p.before.doubleJumped}}},
		{"flipBackAfter", p.regained},
		{"carRotation", {car.rotation.pitch, car.rotation.yaw, car.rotation.roll}},
		{"wheels", {car.wheels, car.worldWheels}},
	};
	emit("touch", "TOUCH", tick, kmh, base, false);

	// The bigger mechanics first; a plain aerial / wall touch shows only when nothing bigger fired on this touch.
	bool big = false;

	// Flip reset off this touch: the car went into it with its jump / flip used, touched the ball in the air, and over
	// the next ticks (still in the air) had its flip back. Soft and pop resets included: no "on the ground" needed.
	if (aerial && p.before.spent() && p.regained && !p.leftAir && tick > lastResetTick_ + 30 && tick > leftSurfaceTick_ + 15)
	{
		nlohmann::json v = base;
		v["how"] = "touch";
		reportReset(tick, v);
		big = true;
	}
	if (aerial)
	{
		++flightAerialTouches_;
		if (p.backboardBefore && !p.bouncedBefore && sinceLast < 4.5)
		{
			nlohmann::json v = base;
			v["secondsAfterBackboard"] = round2((tick - p.backboardTick) / 120.f);
			emit("double_touch", "DOUBLE TOUCH", tick, kmh, v);
			big = true;
		}
		if (airChain_ >= 1 && !p.bouncedBefore && !p.backboardBefore && sinceLast < 3.5)
			++airChain_;
		else
			airChain_ = 1;
		if (airChain_ >= 2)
		{
			nlohmann::json v = base;
			v["touches"] = airChain_;
			emit("air_dribble", airChain_ == 2 ? "AIR DRIBBLE" : "AIR DRIBBLE x" + std::to_string(airChain_), tick, kmh, v);
			out_.back().points = 3 + (airChain_ - 2);
			big = true;
		}
		if (ceilingLeftTick_ != 0 && tick < ceilingLeftTick_ + 9 * kTicksPerSecond / 2)
		{
			nlohmann::json v = base;
			v["secondsOffCeiling"] = round2((tick - ceilingLeftTick_) / 120.f);
			emit("ceiling_shot", "CEILING SHOT", tick, kmh, v);
			ceilingLeftTick_ = 0;
			big = true;
		}
	}
	else
	{
		airChain_ = wall ? 1 : 0; // a wall touch can pop the ball into an air dribble
	}

	// Flick: the ball was on the roof a moment ago (or was popped off it and has not landed), the car is in its dodge
	// (the flip used, at the touch or within the ticks after it: the flags lag a flip that starts at the touch), and
	// the touch sent it away. The best touch of the dodge counts (kept in flick_, reported when the dodge's touches are
	// over). The type: a musty is a backflip into the ball sending it forward (the nose turning toward the roof around
	// the touch; Lambourne's marked musties, 2026-10-14, start nose down -90 to -25 degrees); otherwise the angle
	// between the way the car was driving and where the hit sent the ball (front, 45, 90, back).
	// 2026-10-18 misses: a musty touched with the flags still saying "has flip" (the backflip starting), and one 1.05 s
	// after the pop (the old window was 0.8 s).
	// (Air musties are judged over the whole backflip: watchAirDodge.)
	const bool dodging = car.doubleJumped || !car.hasFlip || p.dodged;
	const bool offDribble = dribbleTicks_ >= 24
		&& (tick < dribbleLastTick_ + 96 || (tick < dribbleLastTick_ + 200 && !p.bouncedBefore));
	const float flickGain = speedAfter - p.ballSpeedBefore;
	const bool groundFlick = offDribble && dodging && !car.onRealSurface() && car.location.z < 450.f && speedAfter > 1000.f
		&& flickGain > 200.f;
	if (groundFlick)
	{
		Vec3 fwd, up;
		orientation(car.rotation, fwd, up);
		const float yaw = car.rotation.yaw * 3.14159265f / 32768.f;
		float dx = car.velocity.x, dy = car.velocity.y;
		if (std::hypot(dx, dy) < 300.f)
		{
			dx = std::cos(yaw);
			dy = std::sin(yaw);
		}
		const float dl = std::max(1e-3f, std::hypot(dx, dy));
		dx /= dl;
		dy /= dl;
		const Vec3 hit = velAfter - p.touch.ballVelocity;
		const float hh = std::max(1.f, std::hypot(hit.x, hit.y));
		const float along = (dx * hit.x + dy * hit.y) / hh;
		const float angle = std::acos(std::clamp(along, -1.f, 1.f)) * 180.f / 3.14159265f; // 0 = straight ahead
		const float fh = std::hypot(fwd.x, fwd.y);
		const float noseBack = fh > 0.05f ? (fwd.x * dx + fwd.y * dy) / fh : 0.f; // < 0: the nose points back
		// Which way the nose is turning: the car a few ticks before the touch against the car at it.
		Vec3 fwdBefore, upBefore;
		orientation(p.before.rotation, fwdBefore, upBefore);
		const float noseRise = fwd.z - fwdBefore.z; // > 0: rotating nose-up (a backflip)
		std::string type = "FLICK";
		if (p.pitchTurn > 0.12f && along > 0.2f)
			type = "MUSTY FLICK";
		else if (angle < 25.f)
			type = "FRONT FLICK";
		else if (angle < 65.f)
			type = "45 FLICK";
		else if (angle < 115.f)
			type = "90 FLICK";
		else
			type = "BACK FLICK";
		nlohmann::json v = base;
		v["dribbleSeconds"] = round2(dribbleTicks_ / 120.f);
		v["hitAngle"] = round2(angle);
		v["carForward"] = vec(fwd * 100.f);
		v["carUp"] = vec(up * 100.f);
		v["noseBack"] = round2(noseBack);
		v["noseRise"] = round2(noseRise * 100.f);
		v["pitchTurnDegrees"] = round2(p.pitchTurn * 57.2958f);
		v["dodgeFlagsLate"] = !(car.doubleJumped || !car.hasFlip);
		v["secondsAfterDribble"] = round2((tick - dribbleLastTick_) / 120.f);
		v["type"] = type;
		const float gain = flickGain;
		if (!flick_.on || gain > flick_.gain)
		{
			flick_.on = true;
			flick_.gain = gain;
			flick_.kmh = kmh;
			flick_.type = type;
			flick_.values = v;
		}
		flick_.tick = tick;
		big = true;
	}

	// Pinch: the ball against the floor, a wall or the ceiling with the car driving it INTO that surface. Squeezed
	// between the two it shoots out fast, often along the surface (2026-10-18: the old rule wanted it to leave away from
	// the surface, so wall pinches, which shoot up or along the wall, never counted; and flip shots of a floor ball
	// did). A plain hard hit of a floor ball pushes along the floor, not into it. The goal mouth is not a wall.
	{
		const float pad = kBallRadius + 85.f;
		struct Surface
		{
			Vec3 normal; // pointing into the field
			const char* name;
		};
		Surface nearby[5];
		int nearCount = 0;
		if (ballAt.z < pad)
			nearby[nearCount++] = {{0, 0, 1}, "floor"};
		if (std::fabs(ballAt.x) > 4096.f - pad)
			nearby[nearCount++] = {{ballAt.x > 0 ? -1.f : 1.f, 0, 0}, "side wall"};
		if (std::fabs(ballAt.y) > 5120.f - pad && !(std::fabs(ballAt.x) < 893.f && ballAt.z < 643.f))
			nearby[nearCount++] = {{0, ballAt.y > 0 ? -1.f : 1.f, 0}, ballAt.z > 643.f ? "backboard" : "back wall"};
		if (ballAt.z > 2044.f - pad)
			nearby[nearCount++] = {{0, 0, -1}, "ceiling"};
		if (std::fabs(ballAt.x) + std::fabs(ballAt.y) > 8064.f - pad * 1.4142f)
			nearby[nearCount++] = {Vec3(ballAt.x > 0 ? -0.7071f : 0.7071f, ballAt.y > 0 ? -0.7071f : 0.7071f, 0), "corner"};
		Vec3 toBall = ballAt - p.touch.carLocation;
		toBall = toBall * (1.f / std::max(1.f, toBall.length()));
		const Surface* surface = nullptr;
		float push = -1.f;
		for (int i = 0; i < nearCount; ++i)
		{
			const float into = -toBall.dot(nearby[i].normal);
			if (into > push)
			{
				push = into;
				surface = &nearby[i];
			}
		}
		const float gain = speedAfter - p.ballSpeedBefore;
		if (surface && speedAfter > 2200.f)
		{
			nlohmann::json v = base;
			v["surface"] = surface->name;
			v["pushIntoSurface"] = round2(push);
			v["gainKmh"] = round2(gain * 0.036f);
			v["awayFromSurface"] = round2(velAfter.dot(surface->normal) / std::max(1.f, speedAfter));
			const std::string name = surface->name;
			if (push > 0.3f && speedAfter > 2650.f && gain > 1000.f)
			{
				emit("pinch", name == "floor" ? "PINCH" : name == "corner" ? "CORNER PINCH" : name == "ceiling" ? "CEILING PINCH"
					: "WALL PINCH", tick, kmh, v);
				big = true;
			}
			else
			{
				emit("pinch_check", "NOT A PINCH", tick, kmh, v, false); // logged so a missed pinch can be tuned
			}
		}
	}

	// Backboard read: an aerial touch on a ball coming off the backboard that the player did not put there (that is a
	// double touch). Redirect: an aerial touch turning a ball that came from somewhere else (a cross, a bounce, someone
	// else's touch) by more than 45 degrees.
	if (aerial && !big)
	{
		const Vec3 vin = p.touch.ballVelocity;
		const float speedIn = vin.length();
		const bool notSetUpByPlayer = sinceLast > 2.0 || p.bouncedBefore || p.backboardBefore;
		const float turn = speedIn > 1.f && speedAfter > 1.f
			? std::acos(std::clamp(vin.dot(velAfter) / (speedIn * speedAfter), -1.f, 1.f)) * 57.2958f : 0.f;
		nlohmann::json v = base;
		v["turnDegrees"] = round2(turn);
		v["kmhIn"] = round2(speedIn * 0.036f);
		if (p.backboardBefore && !p.bouncedBefore && sinceLast >= 4.5)
		{
			v["secondsAfterBackboard"] = round2((tick - p.backboardTick) / 120.f);
			emit("backboard_read", "BACKBOARD READ", tick, kmh, v);
			big = true;
		}
		else if (notSetUpByPlayer && speedIn > 1200.f && speedAfter > 1300.f && turn > 45.f)
		{
			emit("redirect", "REDIRECT", tick, kmh, v);
			big = true;
		}
	}

	if (ceiling)
		emit("ceiling_touch", "CEILING TOUCH", tick, kmh, base, !big);
	else if (aerial)
		emit("aerial_touch", "AERIAL", tick, kmh, base, !big);
	else if (wall)
		emit("wall_touch", "WALL TOUCH", tick, kmh, base, !big);

	if (s.ball && ballpredict::timeToGoal(*s.ball, attackSide_) >= 0.f)
	{
		emit("shot", "SHOT", tick, kmh, base, false);
		// Power shot: on target at 110 km/h or more.
		if (kmh >= 110.f)
			emit("power_shot", "POWER SHOT", tick, kmh, base);
	}

	lastLocalTouchTick_ = tick;
}
