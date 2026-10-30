#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// The speed flip coach (Phase 5): watches one speed flip attempt from the start of the run (the car's state each tick,
// no inputs needed) and gives one tip, the biggest problem first. Plain C++, no SDK; unit-tested on made-up runs.
//
// A speed flip, as Nexto's scripted kickoff does it (bot.py KICKOFF_CONTROLS): boost from the start, jump at about
// 0.5 s, flip diagonally (about 45 degrees off straight ahead) a few ticks after the jump, cancel it at once by pulling
// back so the nose stays level, roll to land flat on the wheels, boosting all the way.
//
// What it measures:
//  - jump: the jump flag coming on (seconds from the start);
//  - flip: doubleJumped coming on (the dodge), ticks after the jump;
//  - angle: the dodge's spin in its first ticks, roll against pitch (0 = a front flip, 90 = a side flip);
//  - cancel: the nose's lowest and highest angle from the dodge until landing (a front flip that is not cancelled
//    dives nose down);
//  - landing: wheels back on the floor, how long after the dodge and how flat (roof up);
//  - boost: whether the tank went down between the dodge and the landing (boosting through the flip).
class SpeedFlipCoach
{
public:
	struct Report
	{
		bool jumped = false, flipped = false, landed = false;
		float jumpSeconds = -1.f;
		int flipDelayTicks = -1;
		float flipAngle = -1.f;          // degrees off straight ahead
		float noseLowest = 0.f, noseHighest = 0.f; // degrees, from the dodge to the landing
		float landSeconds = -1.f;        // after the dodge
		float landFlatness = 0.f;        // the roof's up component at landing (1 = flat)
		float landKmh = 0.f;
		bool boostedThrough = false;
		std::string tip;                 // the one thing to fix, or "CLEAN SPEED FLIP"
		bool clean = false;

		nlohmann::json toJson() const
		{
			auto r = [](float f) { return std::round(f * 100.f) / 100.f; };
			return {{"jumped", jumped}, {"flipped", flipped}, {"landed", landed}, {"jumpSeconds", r(jumpSeconds)},
				{"flipDelayTicks", flipDelayTicks}, {"flipAngle", r(flipAngle)}, {"noseLowest", r(noseLowest)},
				{"noseHighest", r(noseHighest)}, {"landSeconds", r(landSeconds)}, {"landFlatness", r(landFlatness)},
				{"landKmh", r(landKmh)}, {"boostedThrough", boostedThrough}, {"tip", tip}, {"clean", clean}};
		}
	};

	// The run starts (the car released).
	void start(uint64_t tick)
	{
		*this = SpeedFlipCoach{};
		on_ = true;
		startTick_ = tick;
	}
	bool on() const { return on_; }
	// Watching is over: landed after the flip, or long enough after it, or nothing happened in 2.5 s.
	bool done() const { return done_; }

	void tick(uint64_t tick, const CarState& car)
	{
		if (!on_ || done_ || tick < startTick_)
			return;
		Vec3 fwd, up;
		orientation(car.rotation, fwd, up);
		if (!r_.jumped && car.jumped && !car.onGround)
		{
			r_.jumped = true;
			jumpTick_ = tick;
			r_.jumpSeconds = static_cast<float>(tick - startTick_) / 120.f;
		}
		if (r_.jumped && !r_.flipped && car.doubleJumped)
		{
			r_.flipped = true;
			dodgeTick_ = tick;
			r_.flipDelayTicks = static_cast<int>(tick - jumpTick_);
			boostAtDodge_ = car.boost;
			r_.noseLowest = r_.noseHighest = noseDegrees(fwd);
		}
		if (r_.flipped)
		{
			// The dodge's spin over its first 4 ticks (before the cancel takes it out): roll against pitch.
			if (tick <= dodgeTick_ + 4)
			{
				const Vec3 right(fwd.y * up.z - fwd.z * up.y, fwd.z * up.x - fwd.x * up.z, fwd.x * up.y - fwd.y * up.x);
				rollRate_ = std::max(rollRate_, std::fabs(car.angularVelocity.dot(fwd)));
				pitchRate_ = std::max(pitchRate_, std::fabs(car.angularVelocity.dot(right)));
				if (rollRate_ + pitchRate_ > 0.5f)
					r_.flipAngle = std::atan2(rollRate_, pitchRate_) * 57.2958f;
			}
			if (!car.onGround)
			{
				r_.noseLowest = std::min(r_.noseLowest, noseDegrees(fwd));
				r_.noseHighest = std::max(r_.noseHighest, noseDegrees(fwd));
			}
			if (car.onGround && car.location.z < 60.f && tick > dodgeTick_ + 6)
			{
				r_.landed = true;
				r_.landSeconds = static_cast<float>(tick - dodgeTick_) / 120.f;
				r_.landFlatness = up.z;
				r_.landKmh = car.velocity.length() * 0.036f;
				r_.boostedThrough = car.boost < boostAtDodge_ - 2.f;
				finish();
				return;
			}
			if (tick > dodgeTick_ + 180)
			{
				r_.boostedThrough = car.boost < boostAtDodge_ - 2.f;
				finish();
				return;
			}
		}
		if (tick > startTick_ + 300 && !r_.flipped)
			finish();
	}

	// The report so far (finishing it if it was not done: e.g. the run ended first).
	Report report()
	{
		if (on_ && !done_)
			finish();
		return r_;
	}

private:
	static void orientation(const Rot& r, Vec3& forward, Vec3& up)
	{
		// Unreal rotator -> nose and roof directions (as RLBot converts them; MechanicDetector does the same).
		const float k = 3.14159265f / 32768.f;
		const float cp = std::cos(r.pitch * k), sp = std::sin(r.pitch * k);
		const float cy = std::cos(r.yaw * k), sy = std::sin(r.yaw * k);
		const float cr = std::cos(r.roll * k), sr = std::sin(r.roll * k);
		forward = Vec3(cp * cy, cp * sy, sp);
		up = Vec3(-cr * cy * sp - sr * sy, -cr * sy * sp + sr * cy, cp * cr);
	}
	static float noseDegrees(const Vec3& fwd) { return std::asin(std::clamp(fwd.z, -1.f, 1.f)) * 57.2958f; }

	void finish()
	{
		done_ = true;
		// One tip, the biggest problem first.
		char buf[96];
		if (!r_.jumped)
			r_.tip = "BOOST, THEN JUMP AT ABOUT HALF A SECOND";
		else if (!r_.flipped)
			r_.tip = "FLIP DIAGONALLY RIGHT AFTER THE JUMP";
		else if (r_.jumpSeconds > 0.75f)
			snprintf(buf, sizeof(buf), "JUMP SOONER (%.2f S IN, TRY 0.5)", r_.jumpSeconds), r_.tip = buf;
		else if (r_.jumpSeconds < 0.3f)
			snprintf(buf, sizeof(buf), "JUMP A BIT LATER (%.2f S IN, TRY 0.5): BUILD SPEED FIRST", r_.jumpSeconds), r_.tip = buf;
		else if (r_.flipDelayTicks > 16)
			snprintf(buf, sizeof(buf), "FLIP SOONER AFTER THE JUMP (%.2f S)", r_.flipDelayTicks / 120.f), r_.tip = buf;
		else if (r_.flipAngle >= 0.f && r_.flipAngle < 22.f)
			snprintf(buf, sizeof(buf), "FLIP MORE DIAGONALLY (%.0f DEG, AIM FOR 45)", r_.flipAngle), r_.tip = buf;
		else if (r_.flipAngle > 72.f)
			snprintf(buf, sizeof(buf), "FLIP MORE FORWARD (%.0f DEG, AIM FOR 45)", r_.flipAngle), r_.tip = buf;
		else if (r_.noseLowest < -30.f)
			snprintf(buf, sizeof(buf), "CANCEL THE FLIP: PULL BACK RIGHT AWAY (THE NOSE DIPPED %.0f DEG)", -r_.noseLowest), r_.tip = buf;
		else if (r_.noseHighest > 35.f)
			snprintf(buf, sizeof(buf), "PULL BACK LESS (THE NOSE CAME UP %.0f DEG)", r_.noseHighest), r_.tip = buf;
		else if (!r_.landed || r_.landSeconds > 1.1f || r_.landFlatness < 0.8f)
			r_.tip = "AIR ROLL TO LAND FLAT ON YOUR WHEELS, SOONER";
		else if (!r_.boostedThrough)
			r_.tip = "KEEP BOOSTING THROUGH THE FLIP";
		else
		{
			r_.tip = "CLEAN SPEED FLIP";
			r_.clean = true;
		}
	}

	bool on_ = false, done_ = false;
	uint64_t startTick_ = 0, jumpTick_ = 0, dodgeTick_ = 0;
	float boostAtDodge_ = 0.f;
	float rollRate_ = 0.f, pitchRate_ = 0.f;
	Report r_;
};
