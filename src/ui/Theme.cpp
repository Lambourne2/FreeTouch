#include "pch.h"
#include "ui/Theme.h"

#include <cmath>
#include <map>

namespace
{
	int g_colors = 0;
	int g_vars = 0;

	void color(ImGuiCol idx, const ImVec4& c)
	{
		ImGui::PushStyleColor(idx, c);
		++g_colors;
	}

	void var(ImGuiStyleVar idx, float v)
	{
		ImGui::PushStyleVar(idx, v);
		++g_vars;
	}

	void var(ImGuiStyleVar idx, const ImVec2& v)
	{
		ImGui::PushStyleVar(idx, v);
		++g_vars;
	}
}

theme::Palette& theme::palette()
{
	static Palette p;
	return p;
}

void theme::setAccent(const ImVec4& accent)
{
	palette().accent = accent;
}

ImU32 theme::u32(const ImVec4& c, float alphaScale)
{
	return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alphaScale));
}

ImVec4 theme::mix(const ImVec4& a, const ImVec4& b, float t)
{
	return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

void theme::push()
{
	const Palette& p = palette();
	g_colors = g_vars = 0;

	color(ImGuiCol_WindowBg, p.bg);
	color(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	color(ImGuiCol_Text, p.text);
	color(ImGuiCol_TextDisabled, p.textDim);
	color(ImGuiCol_Border, ImVec4(1, 1, 1, 0.06f));
	color(ImGuiCol_TitleBg, p.rail);
	color(ImGuiCol_TitleBgActive, p.rail);
	color(ImGuiCol_TitleBgCollapsed, p.rail);
	// Inputs, slider tracks and combos get their own fill so they show on a card without a border.
	color(ImGuiCol_FrameBg, mix(p.card, ImVec4(1, 1, 1, 1), 0.06f));
	color(ImGuiCol_FrameBgHovered, mix(p.card, p.accent, 0.18f));
	color(ImGuiCol_FrameBgActive, mix(p.card, p.accent, 0.25f));
	color(ImGuiCol_Button, mix(p.card, ImVec4(1, 1, 1, 1), 0.07f)); // visible on a card (it was the card's own colour)
	color(ImGuiCol_ButtonHovered, mix(p.card, p.accent, 0.35f));
	color(ImGuiCol_ButtonActive, mix(p.card, p.accent, 0.6f));
	color(ImGuiCol_Header, mix(p.card, p.accent, 0.25f));
	color(ImGuiCol_HeaderHovered, mix(p.card, p.accent, 0.35f));
	color(ImGuiCol_HeaderActive, mix(p.card, p.accent, 0.5f));
	color(ImGuiCol_CheckMark, p.accent);
	color(ImGuiCol_SliderGrab, p.accent);
	color(ImGuiCol_SliderGrabActive, p.accent);
	color(ImGuiCol_Separator, ImVec4(1, 1, 1, 0.08f));
	color(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
	color(ImGuiCol_ScrollbarGrab, p.card);
	color(ImGuiCol_ScrollbarGrabHovered, p.cardHover);
	color(ImGuiCol_PopupBg, p.rail);

	var(ImGuiStyleVar_WindowRounding, 10.f);
	var(ImGuiStyleVar_ChildRounding, 10.f);
	var(ImGuiStyleVar_FrameRounding, 6.f);
	var(ImGuiStyleVar_GrabRounding, 6.f);
	var(ImGuiStyleVar_PopupRounding, 6.f);
	var(ImGuiStyleVar_ScrollbarRounding, 6.f);
	var(ImGuiStyleVar_ScrollbarSize, 10.f);
	var(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
	var(ImGuiStyleVar_FramePadding, ImVec2(12.f, 7.f));
	var(ImGuiStyleVar_ItemSpacing, ImVec2(10.f, 10.f));
	var(ImGuiStyleVar_WindowBorderSize, 0.f);
}

void theme::pop()
{
	ImGui::PopStyleVar(g_vars);
	ImGui::PopStyleColor(g_colors);
	g_colors = g_vars = 0;
}

namespace
{
	std::map<std::string, ImFont*>& loadedFonts()
	{
		static std::map<std::string, ImFont*> fonts;
		return fonts;
	}
	int g_scaledFonts = 0; // fonts pushed by fontScale
}

const std::vector<theme::FontSpec>& theme::fontSpecs()
{
	static const std::vector<FontSpec> specs = {
		{"ft_body", "Barlow-Medium.ttf", 17.f},
		{"ft_small", "Barlow-Medium.ttf", 14.f},
		{"ft_semibold21", "Barlow-SemiBold.ttf", 21.f},
		{"ft_bold26", "Barlow-Bold.ttf", 26.f},
		{"ft_bold34", "Barlow-Bold.ttf", 34.f},
		{"ft_bold46", "Barlow-Bold.ttf", 46.f},
		{"ft_bold64", "Barlow-Bold.ttf", 64.f},
	};
	return specs;
}

void theme::setFont(const std::string& name, ImFont* font)
{
	if (font)
		loadedFonts()[name] = font;
}

bool theme::hasFonts()
{
	return bodyFont() != nullptr;
}

ImFont* theme::bodyFont()
{
	const auto it = loadedFonts().find("ft_body");
	return it == loadedFonts().end() ? nullptr : it->second;
}

ImFont* theme::fontAt(float px)
{
	ImFont* best = nullptr;
	ImFont* biggest = nullptr;
	for (const auto& [name, font] : loadedFonts())
	{
		if (!biggest || font->FontSize > biggest->FontSize)
			biggest = font;
		if (font->FontSize >= px * 0.98f && (!best || font->FontSize < best->FontSize))
			best = font;
	}
	if (best)
		return best;
	return biggest ? biggest : ImGui::GetFont();
}

void theme::fontScale(float scale)
{
	while (g_scaledFonts > 0)
	{
		ImGui::PopFont();
		--g_scaledFonts;
	}
	ImFont* body = bodyFont();
	if (!body || std::fabs(scale - 1.f) < 0.001f)
	{
		ImGui::SetWindowFontScale(scale);
		return;
	}
	const float px = body->FontSize * scale;
	ImFont* font = fontAt(px);
	ImGui::PushFont(font);
	++g_scaledFonts;
	ImGui::SetWindowFontScale(px / font->FontSize);
}
