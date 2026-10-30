#include "pch.h"
#include "bots/PuppetKeeper.h"

#include <algorithm>
#include <cmath>

namespace
{
	constexpr float kPostLimit = 893.f - 70.f; // keep the car (about 84 wide) inside the posts
	constexpr float kComingSpeed = 150.f;      // uu/s toward the goal before the keeper predicts the crossing
}

void PuppetKeeper::reset(const Vec3& start, int side, const KeeperParams& params, float errorOffset)
{
	start_ = start;
	side_ = side;
	params_ = params;
	errorOffset_ = errorOffset;
	x_ = std::clamp(start.x, -kPostLimit, kPostLimit);
	targetX_ = x_;
	history_.clear();
}

CarTarget PuppetKeeper::step(const BallState& ball, float dt)
{
	// What the keeper "sees": the ball as it was `reaction` seconds ago.
	history_.push_back(ball);
	const size_t delayTicks = static_cast<size_t>(std::max(0.f, params_.reaction) / dt + 0.5f);
	while (history_.size() > delayTicks + 1)
		history_.pop_front();
	if (history_.size() <= delayTicks)
	{
		// Has not reacted yet (start of the rep): hold still.
		CarTarget hold;
		hold.location = {x_, start_.y, start_.z};
		hold.rotation = {0, side_ > 0 ? -16384 : 16384, 0};
		return hold;
	}
	const BallState& seen = history_.front();

	// Where will it cross the keeper's line? Straight-line guess in x (good enough for a goal-line keeper).
	const float towardGoal = side_ * seen.velocity.y;
	const float distance = side_ * (start_.y - seen.location.y);
	float aim = seen.location.x;
	if (towardGoal > kComingSpeed && distance > 0.f)
		aim = seen.location.x + seen.velocity.x * (distance / towardGoal);
	targetX_ = std::clamp(aim + errorOffset_, -kPostLimit, kPostLimit);

	const float maxStep = params_.maxSpeed * dt;
	const float move = std::clamp(targetX_ - x_, -maxStep, maxStep);
	x_ += move;

	CarTarget t;
	t.location = {x_, start_.y, start_.z};
	t.velocity = {move / dt, 0.f, 0.f}; // real velocity, so contacts with the ball behave
	t.rotation = {0, side_ > 0 ? -16384 : 16384, 0}; // facing the field
	return t;
}
