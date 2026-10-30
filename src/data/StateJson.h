#pragma once

#include <cmath>

#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// JSON forms of the plain game-state types, for the event log and dumps. Values are rounded to keep logs readable.
namespace statejson
{
	using json = nlohmann::ordered_json;

	inline double round2(float v) { return std::round(static_cast<double>(v) * 100.0) / 100.0; }

	inline json toJson(const Vec3& v) { return json::array({round2(v.x), round2(v.y), round2(v.z)}); }
	inline json toJson(const Rot& r) { return json::array({r.pitch, r.yaw, r.roll}); }

	inline json toJson(const BallState& b)
	{
		return {
			{"location", toJson(b.location)},
			{"velocity", toJson(b.velocity)},
			{"angularVelocity", toJson(b.angularVelocity)},
			{"speed", round2(b.velocity.length())},
		};
	}

	inline json toJson(const CarState& c)
	{
		return {
			{"id", c.id},
			{"name", c.name},
			{"team", c.team},
			{"local", c.isLocal},
			{"bot", c.isBot},
			{"location", toJson(c.location)},
			{"velocity", toJson(c.velocity)},
			{"rotation", toJson(c.rotation)},
			{"angularVelocity", toJson(c.angularVelocity)},
			{"boost", round2(c.boost)},
			{"hasFlip", c.hasFlip},
			{"onGround", c.onGround},
			{"onWall", c.onWall},
			{"jumped", c.jumped},
			{"doubleJumped", c.doubleJumped},
		};
	}

	inline json toJson(const Snapshot& s)
	{
		json cars = json::array();
		for (const auto& c : s.cars)
			cars.push_back(toJson(c));
		return {
			{"tick", s.tick},
			{"time", std::round(s.time * 1000.0) / 1000.0},
			{"inFreeplay", s.inFreeplay},
			{"inReplay", s.inReplay},
			{"paused", s.paused},
			{"ball", s.ball ? toJson(*s.ball) : json(nullptr)},
			{"cars", cars},
		};
	}
}
