#pragma once

#include <algorithm>
#include <cmath>

#include "game/GameState.h"

// Catches a goal on the last physics tick before the game would count it (the whole ball over the line), so every
// save up to that moment (pinch off the post, clearance off the line) still counts, but freeplay's own goal
// sequence (explosion, slow motion, replay, kickoff reset, which swallows the next reps) never starts. No SDK calls.
namespace goalguard
{
	// Soccar: goal lines at y = +/-5120, mouth 1786 wide and 643 tall. In front of the line, a ball (radius 92.75) can
	// only enter with its centre at |x| < 893 - 92.75 and z < 643 - 92.75; outside that it hits a post or the bar.
	constexpr float kGoalLineY = 5120.f;
	constexpr float kBallRadius = 92.75f;
	constexpr float kMouthHalfWidth = 893.f;
	constexpr float kMouthHeight = 643.f;
	constexpr float kTickSeconds = 1.f / 120.f;
	// Two ticks ahead (it was 1.05, "the latest safe tick"): moves land a tick late, and real goals slipped through
	// (Lambourne 2026-10-20: 22 in his logs, each with freeplay's slow motion and replay).
	constexpr float kLookaheadTicks = 2.f;

	struct Catch
	{
		bool caught = false;
		int side = 0;   // +1 = orange net (y > 0), -1 = blue net
		Vec3 holdAt;    // where the ball is: at or over the line, not yet fully
	};

	inline Catch check(const BallState& ball)
	{
		Catch c;
		const Vec3& p = ball.location;
		const Vec3& v = ball.velocity;
		const int side = p.y >= 0.f ? 1 : -1;
		const float towardGoal = side * v.y;
		// A ball already more than half in (centre past the line, under the bar) and not being cleared out is caught: one
		// sitting there and knocked the rest of the way in (a bot touching it on the line) scored within a tick. A ball
		// moving out quickly is a clearance off the line and still counts as a save.
		if (side * p.y >= kGoalLineY && side * p.y < kGoalLineY + kBallRadius && p.z < kMouthHeight
			&& std::fabs(p.x) < kMouthHalfWidth && towardGoal > -300.f)
		{
			c.caught = true;
			c.side = side;
			c.holdAt = p;
			return c;
		}
		if (towardGoal <= 0.f)
			return c;
		// The mouth check only applies in front of the line: a ball whose centre is past the line can only be inside
		// the goal (a diagonal shot drifts sideways after entering).
		if (side * p.y < kGoalLineY
			&& (std::fabs(p.x) > kMouthHalfWidth - kBallRadius + 10.f || p.z > kMouthHeight - kBallRadius + 10.f))
			return c;
		const float fullyIn = kGoalLineY + kBallRadius;
		const float centreSoon = side * p.y + towardGoal * kTickSeconds * kLookaheadTicks;
		if (centreSoon < fullyIn - 2.f)
			return c;
		c.caught = true;
		c.side = side;
		c.holdAt = p;
		return c;
	}
}
