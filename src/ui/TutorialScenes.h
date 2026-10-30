#pragma once

#include <string>

#include "IMGUI/imgui.h"

// The drawings above the tutorials' text (content/tutorials/tutorials.json "scene"): small animated diagrams drawn
// with ImGui shapes, no image files. `t` is seconds since the step showed; `u` the GUI scale.
namespace tutorial
{
	void drawScene(ImDrawList* dl, const std::string& scene, ImVec2 a, ImVec2 b, float t, float u);
}
