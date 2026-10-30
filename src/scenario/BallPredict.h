#pragma once

#include <algorithm>
#include <cmath>

#include "game/GameState.h"

// Simple ball flight prediction (no SDK): gravity, air drag, and bounces off the floor, ceiling, side walls and back
// walls (outside the goal mouths). Corners and ramps are ignored, so it is approximate; it is meant for questions
// like "is this ball still going into the net?", not for exact positions. No SDK calls.
namespace ballpredict
{
	constexpr float kGravity = -650.f;
	constexpr float kRadius = 92.75f;
	constexpr float kDrag = 0.0305f;        // per second
	constexpr float kBounce = 0.6f;         // restitution normal to a surface
	constexpr float kGroundFriction = 0.97f; // horizontal speed kept per ground bounce
	constexpr float kWallFriction = 0.285f; // walls and ceiling: the bounce also takes up to 28.5% of the speed along
	constexpr float kWallGrip = 2.0f;       // the wall (RLBot's ball bounce model, no spin): min(1, 2 |v normal| / |v along|)
	constexpr float kHalfWidth = 4096.f, kHalfLength = 5120.f, kCeiling = 2044.f;
	constexpr float kMouthHalfWidth = 893.f, kMouthHeight = 643.f;

	// Bounce off a wall or the ceiling whose normal is along `axis` (0 x, 1 y, 2 z).
	inline void wallBounce(Vec3& v, int axis)
	{
		float* c[3] = {&v.x, &v.y, &v.z};
		const float normal = *c[axis];
		const float along = std::sqrt(std::max(0.f, v.dot(v) - normal * normal));
		const float keep = along > 1e-3f ? 1.f - std::min(1.f, kWallGrip * std::fabs(normal) / along) * kWallFriction : 1.f;
		for (int i = 0; i < 3; ++i)
			*c[i] = i == axis ? -normal * kBounce : *c[i] * keep;
	}

	inline bool inMouth(const Vec3& p)
	{
		return std::fabs(p.x) < kMouthHalfWidth - kRadius + 10.f && p.z < kMouthHeight - kRadius + 10.f;
	}

	// One physics step. `backWalls` false lets the ball through the back walls (for aiming at a point on the line).
	inline void step(BallState& b, float dt, bool backWalls = true)
	{
		b.velocity.z += kGravity * dt;
		b.velocity = b.velocity * (1.f - kDrag * dt);
		b.location = b.location + b.velocity * dt;

		Vec3& p = b.location;
		Vec3& v = b.velocity;
		if (p.z < kRadius && v.z < 0.f)
		{
			p.z = kRadius;
			v.z = -v.z * kBounce;
			v.x *= kGroundFriction;
			v.y *= kGroundFriction;
		}
		if (p.z > kCeiling - kRadius && v.z > 0.f)
		{
			p.z = kCeiling - kRadius;
			wallBounce(v, 2);
		}
		if (std::fabs(p.x) > kHalfWidth - kRadius && p.x * v.x > 0.f)
		{
			p.x = std::copysign(kHalfWidth - kRadius, p.x);
			wallBounce(v, 0);
		}
		if (backWalls && std::fabs(p.y) > kHalfLength - kRadius && p.y * v.y > 0.f && !inMouth(p))
		{
			p.y = std::copysign(kHalfLength - kRadius, p.y);
			wallBounce(v, 1);
		}
	}

	// Seconds until the ball's centre crosses the goal line inside the mouth of the net on `side` (+1 orange, -1 blue;
	// from there it is inside the goal and goes in), or a negative
	// value if it does not get there within `horizon` seconds (8 s: slow lobs from midfield take over 4 s).
	inline float timeToGoal(BallState b, int side, float horizon = 8.f)
	{
		const float dt = 1.f / 120.f;
		for (float t = 0.f; t < horizon; t += dt)
		{
			step(b, dt);
			if (inMouth(b.location) && side * b.location.y > kHalfLength)
				return t;
		}
		return -1.f;
	}
}
