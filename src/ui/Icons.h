#pragma once

#include "IMGUI/imgui.h"

// Small vector icons drawn with ImDrawList (no image files needed). Render thread.
namespace icons
{
	enum class Icon { Home, Sessions, Exercises, QuickPlay, Races, Replays, Stats, Settings, Play, Stop, Star, Packs, Bookmark, Calendar, Friends, Trophy, Record, Heart };

	// A five-pointed star of outer radius `r`: filled, or as an outline.
	void star(ImDrawList* dl, ImVec2 center, float r, ImU32 color, bool filled, float thickness = 1.6f);

	// Draws `icon` centred at `center`, fitting a box of `size` pixels.
	void draw(ImDrawList* dl, Icon icon, ImVec2 center, float size, ImU32 color);
}
