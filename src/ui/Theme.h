#pragma once

#include <string>
#include <vector>

#include "IMGUI/imgui.h"

// Free Touch look: dark, rounded, one accent colour. Pushed around the dashboard window only, so BakkesMod's own
// windows keep their style. Render thread.
namespace theme
{
	struct Palette
	{
		ImVec4 accent{0.17f, 0.85f, 0.77f, 1.f};
		ImVec4 bg{0.047f, 0.055f, 0.078f, 0.98f};    // deep navy rather than flat grey
		ImVec4 rail{0.063f, 0.071f, 0.102f, 1.f};
		ImVec4 card{0.094f, 0.106f, 0.145f, 1.f};
		ImVec4 cardHover{0.125f, 0.141f, 0.192f, 1.f};
		ImVec4 text{0.95f, 0.96f, 0.98f, 1.f};
		ImVec4 textDim{0.56f, 0.61f, 0.70f, 1.f};
		ImVec4 pitch{0.07f, 0.16f, 0.13f, 1.f};
		ImVec4 pitchLine{0.22f, 0.36f, 0.30f, 1.f};
		ImVec4 blue{0.25f, 0.55f, 1.f, 1.f};
		ImVec4 orange{1.f, 0.55f, 0.15f, 1.f};
	};

	Palette& palette();
	void setAccent(const ImVec4& accent);

	ImU32 u32(const ImVec4& c, float alphaScale = 1.f);
	ImVec4 mix(const ImVec4& a, const ImVec4& b, float t);

	// Push / pop the window style. Push before ImGui::Begin, pop after ImGui::End.
	void push();
	void pop();

	// Fonts (polish pass, 2026-10-19): Barlow (content/fonts/, SIL Open Font License) in a few real sizes, so big text
	// is drawn from a big font instead of a stretched small one. The plugin loads them through BakkesMod's GUI manager,
	// the preview tool straight from the files; without them everything falls back to ImGui's own font as before.
	struct FontSpec
	{
		const char* name;  // e.g. "ft_body"
		const char* file;  // in content/fonts/
		float size;        // pixels
	};
	const std::vector<FontSpec>& fontSpecs(); // the body font first
	void setFont(const std::string& name, ImFont* font);
	bool hasFonts();       // the body font is loaded
	ImFont* bodyFont();    // null until loaded
	// The loaded font for text `px` pixels high: the smallest at least that big (so it is drawn at or below its own size,
	// sharp), else the biggest. ImGui's current font when none are loaded.
	ImFont* fontAt(float px);
	// ImGui::SetWindowFontScale, drawing with the font for that size; 1 goes back to the body font.
	void fontScale(float scale);
}
