#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// One mechanic the player just did, with the values that triggered it (they go to the event log).
struct MechanicEvent
{
	std::string id;        // touch, aerial_touch, wall_touch, ceiling_touch, backboard, double_touch, air_dribble,
	                       // flip_reset, double_reset, flick, pinch, ceiling_shot, redirect, backboard_read, power_shot,
	                       // shot, goal; pinch_check / musty_check (logged only: a near miss, for tuning)
	std::string label;     // what the feed shows, e.g. "AIR DRIBBLE x3"
	uint64_t tick = 0;
	int points = 0;        // for scoring games (harder = more)
	bool show = true;      // false: logged only (plain touches, shots)
	float kmh = 0.f;       // the ball's speed after the touch, when it matters
	nlohmann::json values; // why it fired
};

// Recognises the local player's mechanics from the snapshots and touch events (Phase 4). Plain C++, no SDK: it is
// unit-tested on made-up tick sequences (tests/scenario_tests.cpp). Game thread.
//
// What it watches:
//  - every local touch, judged 2 ticks later (the hit shows in the ball by then), with the car as it was at the touch;
//  - the car's surface contact (floor, wall, ceiling) and HasFlip while airborne (a flip reset gives the flip back);
//  - the ball's own bounces (floor, side walls, back wall / backboard, ceiling) from its velocity flipping.
class MechanicDetector
{
public:
	void onTouch(const TouchEvent& e);
	void onTick(const Snapshot& s);
	// The events since the last call, oldest first.
	std::vector<MechanicEvent> take();
	// A new rep or a teleport: forget the current play (air chains, dribbles, ceiling).
	void reset();

	// Points per mechanic (the scoring games use them).
	static int pointsFor(const std::string& id);

	// The last `seconds` (up to 3) of the local car and the ball, one row per tick, for tuning: ft_mark writes it to the
	// log when a mechanic was missed or misnamed. Rows: tick, car x y z, vx vy vz, pitch yaw roll, onGround onWall
	// hasFlip jumped doubleJumped, ball x y z, vx vy vz.
	nlohmann::json trace(double seconds) const;

private:
	struct CarSample
	{
		uint64_t tick = 0;
		Vec3 location, velocity, angularVelocity;
		Rot rotation;
		bool onGround = false, onWall = false, hasFlip = false, jumped = false, doubleJumped = false;
		int wheels = -1, worldWheels = -1;  // the game's wheel contact counts (all / the world's)
		bool realKnown = false, real = false; // realSurface() worked out when the sample was taken
		bool airborne() const { return !onGround && !onWall; }
		// On the floor, a wall or the ceiling (the game's "on the ground" is also true with the wheels on the ball).
		bool onRealSurface() const;
		// Has used its jump / flip (what a flip reset gives back).
		bool spent() const { return jumped || doubleJumped || !hasFlip; }
		bool fresh() const { return hasFlip && !jumped && !doubleJumped; }
	};
	struct PendingTouch
	{
		TouchEvent touch;
		CarSample car;           // the local car at the touch
		float ballSpeedBefore = 0.f;
		float peakSpeed = 0.f;   // the ball's fastest over the ticks after the touch (a pinch builds over a few)
		// What the ball did between the previous local touch and this one (taken at the touch: judging waits).
		bool bouncedBefore = false, backboardBefore = false;
		uint64_t backboardTick = 0;
		CarSample before;        // the car a few ticks before the touch (its flip state going in)
		bool regained = false;   // the flip came back over the ticks after the touch, still off any surface
		bool leftAir = false;    // ...or the car landed meanwhile (then it is not a reset)
		// The dodge: the flags can say so a few ticks after the touch (the touch lands as the flip starts).
		bool dodged = false;
		// How far the nose turned toward the roof (radians; + = nose up, a backflip) from 4 ticks before the touch to
		// judging. A musty is a backflip into the ball.
		float pitchTurn = 0.f;
		Vec3 peakVelocity;
	};
	// The car's nose and roof directions from its rotation.
	static void orientation(const Rot& r, Vec3& forward, Vec3& up);
	// The nose's turn toward the roof from one rotation to the next (radians; + = nose up).
	static float pitchStep(const Rot& from, const Rot& to);
	// A wall, the back walls or the ceiling within `pad` of this point (the floor is checked separately).
	static bool nearSurface(const Vec3& p, float pad);

	// The car's wheels are on the floor, a wall or the ceiling (not the ball or a car). Once the game's wheel counts
	// have read right on the floor (wheelCountsWork_), they decide it; before that, the car's distance to a surface.
	bool realSurface(const CarState& me) const;
	bool wheelCountsWork_ = false;
	// The current flight, logged when it ends if the car touched the ball in it (for tuning resets): every change of
	// the flip flags, "on the ground" or the wheel counts.
	nlohmann::json flightLog_ = nlohmann::json::array();
	int flightTouches_ = 0;
	void logFlight(const char* why);

	void judgeTouch(const PendingTouch& p, const Snapshot& s);
	void watchBall(const Snapshot& s);
	void watchCar(const CarState& car, const Snapshot& s);
	void emit(const std::string& id, const std::string& label, uint64_t tick, float kmh, nlohmann::json values, bool show = true,
		int points = -1);
	// One more flip reset in this flight: FLIP RESET, then DOUBLE / TRIPLE / QUAD / PENTA ... RESET (id double_reset).
	void reportReset(uint64_t tick, nlohmann::json values);
	static std::string resetName(int n);

	std::vector<MechanicEvent> out_;
	std::vector<PendingTouch> pending_;
	std::deque<CarSample> carHistory_;   // the last ticks of the car (flip state before a touch)
	struct TraceRow
	{
		CarSample car;
		BallState ball;
	};
	std::deque<TraceRow> trace_;          // 3 s for ft_mark
	std::optional<CarSample> car_;      // the local car last tick
	std::optional<BallState> ball_;     // the ball last tick
	uint64_t tick_ = 0;
	uint64_t lastTouchTick_ = 0;        // any car's touch (ball bounces right after a touch are not bounces)
	uint64_t lastLocalTouchTick_ = 0;
	bool lastTouchLocal_ = false;
	int attackSide_ = 1;                // +1 the orange net (the player is blue in freeplay)

	// The current flight: from leaving a surface to the next landing.
	bool airborne_ = false;
	uint64_t leftSurfaceTick_ = 0;
	int flightResets_ = 0;
	int flightAerialTouches_ = 0;
	uint64_t ceilingLeftTick_ = 0;      // left the ceiling (and has not landed since): a ceiling shot is on
	uint64_t lastResetTick_ = 0;
	bool onCeiling_ = false;
	bool wheelsOnBall_ = false;         // last tick: "on the ground" in mid-air at the ball, i.e. wheels on the ball
	// Flip reset by the flags (Lambourne, 2026-10-19): in the air with the jump / flip used or run out, then the flip
	// is back without a floor, wall or ceiling. Confirmed 3 ticks later (the flags can come back a tick before a
	// landing shows).
	uint64_t flagsBackTick_ = 0;
	nlohmann::json flagsBackValues_;
	uint64_t dodgeStartTick_ = 0;        // the last dodge (or double jump) started here: doubleJumped came on

	// Air musty, judged over the whole backflip (Lambourne, 2026-10-19: in the air it is a scoop more than a hit: touch
	// then backflip, or backflip then touch, or the ball rolling over the roof, then the ball rushing forward). A dodge
	// in the air with the ball near; the touches from 0.1 s before it to 0.65 s after; the nose turning up over its
	// first 0.33 s; then the ball's gain forward (the way the car was going, or at the net) since before those touches.
	struct AirDodge
	{
		bool on = false;
		uint64_t start = 0;
		float pitchTurn = 0.f;    // radians over the first 40 ticks, + = nose up (a backflip)
		Vec3 travel;              // the car's way at the start (flat, unit)
		Vec3 baseline;            // the ball's velocity before the dodge's first touch
		bool haveBaseline = false;
		int touches = 0;
		uint64_t firstTouch = 0, lastTouch = 0;
		float carZ = 0.f;
	} airDodge_;
	uint64_t lastFlickTick_ = 0;          // the last (ground) flick reported: an air dodge over it is not judged again
	uint64_t lastRawLocalTouchTick_ = 0;  // the last local touch as it happened (lastLocalTouchTick_ is set when judged)
	Vec3 lastRawLocalTouchBallVel_;
	void watchAirDodge(const CarState& me, const Snapshot& s);

	// Air dribble / double touch: aerial touches in this flight, and what the ball hit since the last one.
	int airChain_ = 0;
	bool ballBouncedSinceTouch_ = false;  // floor / side wall / ceiling since the last local touch
	bool backboardSinceTouch_ = false;    // the back wall above the net since the last local touch
	uint64_t backboardTick_ = 0;

	// Dribble (for flicks): ticks the ball has sat on the car's roof recently (kept 1.7 s after the ball leaves the
	// roof: a musty often comes off a pop, a second after the dribble).
	int dribbleTicks_ = 0;
	uint64_t dribbleLastTick_ = 0;
	uint64_t flipStartTick_ = 0;          // the car used its flip (dodge) here
	// A flick is judged on the best touch of the dodge (the jump's pop comes first and is not the flick): the touch
	// with the biggest hit is kept here and reported once the dodge's touches are over.
	struct FlickCandidate
	{
		bool on = false;
		uint64_t tick = 0;
		float gain = 0.f;
		float kmh = 0.f;
		std::string type;
		nlohmann::json values;
	} flick_;

	// The last mechanics, for the goal's label ("FLIP RESET GOAL").
	struct Recent
	{
		uint64_t tick;
		std::string id, label;
		int points;
	};
	std::deque<Recent> recent_;
	bool goalSeen_ = false;
};
