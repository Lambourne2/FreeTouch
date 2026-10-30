#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// One rep of a scenario with every range resolved. No SDK calls.
struct ResolvedScenario
{
	std::string id;
	std::string name;
	std::string category;
	uint32_t seed = 0;
	std::string difficulty;   // easy | medium | hard
	bool mirrored = false;
	double timeLimitSeconds = 0.0; // 0 = none

	BallTarget ball;
	double ballFreezeSeconds = 0.0;
	bool ballHidden = false;  // ballless drill: hidden and held at ball.location for the whole rep
	bool aimAtUsed = false;
	bool aimAtReachable = true; // false: the requested speed cannot reach the point; the slowest arc that can was used

	CarTarget player;

	nlohmann::json resolved;  // the whole scenario after resolution (bots, objects, success, fail for later systems)
	std::vector<std::string> warnings;
};

// JSON -> ResolvedScenario, in this order (fixed, so a seed always gives the same rep):
//   1. every [min, max] range is drawn from the seed, depth-first in key order. A range is an array of exactly two
//      numbers anywhere in the scenario, except under "grid" (an integer pair), "points" and "gaps" (data, not ranges).
//   2. difficulty overrides (dotted paths, e.g. "bots.0.params.reaction"); a range value here is drawn too.
//   3. mirror coin flip when allowMirror: x, yaw, roll and spin are flipped; then one coin per bot with
//      randomSide flips just that bot.
//   3c. objects with "surface" + "grid" are laid out in cells (a pocket target in a random cell plus blockers on
//      the rest, or one panel per cell).
//   4. aimAt is turned into a ball velocity (gravity -650 uu/s^2, no drag; via "sideWall" banks it off the side wall
//      on the ball's side) and faceBall into a yaw. Player extras: relativeTo "ball" (location x/y is an offset
//      from the ball) or "landingToGoal" (from where the ball first lands, in a frame whose +y points from there at the orange goal), faceBallOffset (yaw added to faceBall; mirrored), forwardSpeed (velocity along the yaw).
namespace scenario
{
	// `forceMirror` (a session's air-roll choice) replaces the coin flip for scenarios that allow mirroring; the coin
	// is still drawn, so the rest of the rep stays the same for a seed.
	// `enabledChoices`: one_of choices with an "id" are limited to these (empty = all; if none match, all).
	ResolvedScenario resolve(const nlohmann::json& data, uint32_t seed, const std::string& difficulty,
		std::optional<bool> forceMirror = {}, const std::vector<std::string>& enabledChoices = {});

	// A target grid in the orange net (a "target" with "surface" + "grid", expanded) turned into the training packs'
	// tiles: a blocking zone_cycle with the same cell lit, success a goal in it, the blockers' fails dropped. False when
	// there is no such target. (The Daily Challenge draws its target shots like this.)
	bool targetsAsPackTiles(nlohmann::json& resolved);

	// The difficulty picker applies: the scenario has easy / hard overrides ("difficulty"), or a one_of setup only on
	// some difficulties ("difficulties", e.g. Pinch bowling's ceiling pinches on hard).
	bool hasDifficulty(const nlohmann::json& data);

	// Velocity that sends a ball from `from` through `to` at `speed` (flatter arc). Sets `reachable` to false and
	// returns the lowest-speed arc if `speed` is too slow.
	Vec3 aimVelocity(const Vec3& from, const Vec3& to, double speed, bool& reachable);

	// The same, banked off the side wall on `from`'s side (x sign): starts from `to` reflected behind the wall (its x
	// distance from the wall / 0.6), then corrects for drag and the wall's grip by flying the shot with BallPredict.
	Vec3 aimVelocityOffSideWall(const Vec3& from, const Vec3& to, double speed, bool& reachable);

	// What a scenario needs that Free Touch cannot do yet (e.g. "objects (Phase 2)"), or "" if it plays fully.
	// Such scenarios still load as free setups from the Exercises page, but sessions skip them.
	std::string pendingFeature(const nlohmann::json& data);
}
