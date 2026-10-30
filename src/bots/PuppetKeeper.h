#pragma once

#include <deque>

#include "game/GameState.h"

// Puppet keeper: a car that slides along a line in front of its goal toward where the ball will cross that line
// (or shadows the ball's x when it is not coming). It sees the ball `reaction` seconds late, moves at most
// `maxSpeed` sideways and aims `error` uu off (a fixed offset per rep, from the rep's seed). It is driven by setting
// its state every tick (Phase 0: holds within 0.08 uu, contacts look normal). No SDK calls.
struct KeeperParams
{
	float reaction = 0.3f;   // s
	float maxSpeed = 1400.f; // uu/s
	float error = 120.f;     // uu
};

class PuppetKeeper
{
public:
	// `start` is its line (x is where it begins, y and z stay fixed); `side` is the net it defends (+1 orange, -1 blue).
	void reset(const Vec3& start, int side, const KeeperParams& params, float errorOffset);

	// One physics tick: returns where the keeper is this tick (location, velocity, rotation).
	CarTarget step(const BallState& ball, float dt = 1.f / 120.f);

	float x() const { return x_; }
	float targetX() const { return targetX_; }

private:
	Vec3 start_;
	int side_ = 1;
	KeeperParams params_;
	float errorOffset_ = 0.f;
	float x_ = 0.f;
	float targetX_ = 0.f;
	std::deque<BallState> history_;
};
