#pragma once

#include <string>
#include <vector>

#include "core/Canvas.h"
#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// World objects: things Free Touch draws in the arena and tests the ball and car against every tick (the game knows
// nothing about them). Built from a resolved scenario's "objects". No SDK calls.
enum class ObjectType { Target, Blocker, Ring, Pin, Checkpoint, Lane, Panel, Circle, Unknown };
enum class Trigger { Ball, Car, CarWithBall };

struct WorldObject
{
	std::string id;
	ObjectType type = ObjectType::Unknown;
	Vec3 location;            // centre; pins and ground circles: the point on the floor
	Vec3 normal{0, 0, 1};     // facing side of flat objects (target, ring, panel, checkpoint) and of a blocker's front
	Vec3 size{200, 200, 40};  // blocker / panel: width, height, depth
	float radius = 150.f;     // target, ring, checkpoint, circle; pin: its body radius
	bool rect = false;        // target given a size: a rectangular pocket (size.x by size.y) instead of a disc
	float height = 0.f;       // pin height
	std::vector<Vec3> points; // lane centre line
	float width = 400.f;      // lane width
	Trigger trigger = Trigger::Ball;
	float withinUu = 350.f;   // car_with_ball: how close the ball must be to the car when it passes
	int order = -1;           // rings / checkpoints in a course: 0, 1, 2, ...
	Rgba color{43, 217, 196, 255};

	bool active = true;       // pins and panels disappear when hit
	bool hit = false;
};

namespace objects
{
	const char* typeName(ObjectType type);

	// Blockers bounce the ball and lanes only fence the car: neither is something to "hit" for all_objects_hit.
	inline bool isGoal(const WorldObject& o) { return o.type != ObjectType::Blocker && o.type != ObjectType::Lane; }

	// From a resolved scenario's "objects" array (ranges already drawn). Unknown types are skipped with a warning.
	std::vector<WorldObject> fromJson(const nlohmann::json& objects, std::vector<std::string>& warnings);
}
