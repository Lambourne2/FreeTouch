#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/Canvas.h"
#include "game/GameState.h"
#include "objects/WorldObject.h"

// Tests the ball and the player's car against the rep's world objects every physics tick, using the movement since
// the previous tick (so a fast ball cannot pass through a thin target), and draws them. Game thread. No SDK calls.
class ObjectSystem
{
public:
	struct Hit
	{
		std::string id;
		ObjectType type = ObjectType::Unknown;
		std::string by;        // "ball" or "car"
		Vec3 at;               // where it happened
		float ballSpeed = 0.f;
		bool inOrder = true;   // false: a course ring / checkpoint passed out of order (it does not count)
	};

	struct TickResult
	{
		std::vector<Hit> hits;
		std::optional<BallTarget> ballBounce; // a blocker sent the ball back: put it here
		std::string leftLane;                 // id of a lane the car just left ("" if none)
	};

	void setObjects(std::vector<WorldObject> objects);
	void clear();
	bool empty() const { return objects_.empty(); }

	TickResult tick(const Snapshot& snap);
	void draw(Canvas& canvas, double time) const;

	const std::vector<WorldObject>& objects() const { return objects_; }
	// Marks an object as already hit (pins and panels also disappear), e.g. pins still down from the last throw.
	void setHit(const std::string& id);
	bool allHit() const;      // every object that can be hit (not blockers or lanes) has been
	int nextInOrder() const;  // lowest course order not passed yet, or -1 when the course is done / has no order
	// Where to park a hidden ball so ball cam points at what to do next: just above the next ring in order (clear of
	// the car), the middle of a circle, the centre of a target. Empty when nothing is left.
	// `ahead`: in an ordered course, mark that many objectives past the next one (capped at the last one).
	std::optional<Vec3> objectiveMarker(int ahead = 0) const;

private:
	void testObject(WorldObject& o, const BallState* ball, const CarState* car, TickResult& out);

	std::vector<WorldObject> objects_;
	std::optional<Vec3> prevBall_;
	std::optional<Vec3> prevCar_;
	bool onCourse_ = false;        // the car has been inside a lane (and has not left the lanes since)
	std::vector<uint64_t> lastBlockerTick_;
	uint64_t tick_ = 0;
};
