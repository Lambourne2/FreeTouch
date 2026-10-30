#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "game/GameState.h"

// 2D drawing on the game screen, without SDK types. The GameAdapter hands one to draw listeners once per frame
// (game thread). Coordinates are screen pixels from the top-left.
struct Rgba
{
	uint8_t r = 255, g = 255, b = 255, a = 255;

	Rgba withAlpha(float alpha) const
	{
		return {r, g, b, static_cast<uint8_t>(alpha < 0.f ? 0 : alpha > 1.f ? 255 : alpha * 255.f)};
	}
};

// A sphere that hides world lines behind it (the ball, cars): the canvas has no depth buffer, so this fakes it.
struct Occluder
{
	Vec3 centre;
	float radius = 0.f;
};

class Canvas
{
public:
	virtual ~Canvas() = default;

	virtual float width() const = 0;
	virtual float height() const = 0;

	virtual void fillRect(float x, float y, float w, float h, Rgba color) = 0;
	virtual void line(float x1, float y1, float x2, float y2, float thickness, Rgba color) = 0;
	virtual void text(float x, float y, const std::string& text, float scale, Rgba color, bool shadow = true) = 0;
	virtual void textSize(const std::string& text, float scale, float& w, float& h) = 0;

	// A line between two world points (uu): clipped at a near plane in front of the camera and to the screen, and
	// thinner with distance (maxWidth px up close, 1 px far away). Draws nothing without a camera.
	virtual void line3d(const Vec3& a, const Vec3& b, Rgba color, float maxWidth = 4.f, float minWidth = 1.f) = 0;

	// Screen position of a world point. True when it is in front of the camera; for a point behind, x/y is where it
	// would be if it were mirrored in front (so an edge arrow toward it points the way to turn). False without a camera.
	virtual bool project(const Vec3& world, float& x, float& y) = 0;
	// Spheres that hide the parts of later line3d lines behind them, until the next call (this frame only).
	virtual void setOccluders(const std::vector<Occluder>& spheres) = 0;
	// Distance from the camera along its view direction (negative: behind it). 0 without a camera.
	virtual float depthOf(const Vec3& p) = 0;
};
