#pragma once

#include <string>

#include "core/Canvas.h"

// The bar at the top of the screen during a drill, in the player's chosen style (Settings > Drill bar). It takes plain
// text and colours and draws through Canvas, so the same code draws it in the game and as the live preview in Settings.
// No SDK.
namespace hudbar
{
	enum Style { Classic, Scoreboard, Neon, Pill, Minimal, Side, kStyles };

	const char* styleId(int style);   // what ui.json keeps
	const char* styleName(int style); // what Settings shows
	const char* styleAbout(int style);
	int styleFromId(const std::string& id); // Classic if it isn't one

	struct Options
	{
		int style = Classic;
		float size = 1.f;    // 0.85, 1 or 1.2
		float opacity = 1.f; // of the panels behind the text: 1, 0.7 or 0.4
		bool showName = true;
		bool showLevel = true;
	};

	struct Cell
	{
		std::string label, value;
		Rgba color;
	};

	struct Bar
	{
		Cell exercise, rep, time, stat, level; // stat: the streak, best time, pins, tank... (the drill decides)
	};

	// Where the rest of the HUD goes: lines stack under `underY`; the slow-motion pill sits at (sideX, sideY).
	struct Placed
	{
		float underY = 0.f;
		float sideX = 0.f, sideY = 0.f;
	};

	// `k`: pixels per design pixel (the layout is designed at 1080p). `accent`: the theme's colour.
	Placed draw(Canvas& c, const Bar& bar, const Options& options, float k, Rgba accent);
}
