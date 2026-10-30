#include "pch.h"
#include "ui/BindingsScreen.h"

#include <algorithm>
#include <cmath>

#include "core/Controls.h"
#include "ui/Theme.h"

namespace
{
	// Lambourne 2026-10-29: "make it look nicer; don't use a controller, but draw button shapes". Each action is a tile:
	// the button drawn as itself (a D-pad with its arm lit, a stick, a shoulder, a face button, a keyboard key), what it
	// does in a few words, and Change.
	struct Look
	{
		const char* title;
		const char* help;
	};

	Look lookOf(const std::string& id)
	{
		if (id == "retry") return {"Reset the shot", "The same rep again, from its start"};
		if (id == "next") return {"Next shot", "A new rep"};
		if (id == "previous") return {"Previous shot", "Back to the rep before"};
		if (id == "back") return {"Back to the menu", "Double tap: stops the drill"};
		if (id == "slowmo") return {"Slow motion", "Practice speed on / off"};
		if (id == "clutch") return {"Clutch", "Hold for even slower"};
		return {"", ""};
	}

	float scaleNow()
	{
		const ImFont* body = theme::bodyFont();
		return std::max(0.75f, ImGui::GetFontSize() / (body ? body->FontSize : 13.f));
	}

	void centeredText(ImDrawList* dl, ImVec2 c, ImU32 col, const char* text, float scale)
	{
		const float fs = ImGui::GetFontSize() * scale;
		const ImVec2 ts = theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, 0.f, text);
		dl->AddText(theme::fontAt(fs), fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col, text);
	}

	// The button as itself, centred at `c` in a box `s` wide.
	void drawGlyph(ImDrawList* dl, const std::string& key, ImVec2 c, float s, bool live)
	{
		const theme::Palette& pal = theme::palette();
		const ImU32 lit = theme::u32(pal.accent);
		const ImU32 body = theme::u32(theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.16f));
		const ImU32 edge = theme::u32(pal.textDim, 0.55f);
		const ImU32 ink = IM_COL32(12, 16, 22, 255);
		const float pulse = live ? 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.f) : 1.f;
		if (key.empty())
		{
			// Not set: a dashed ring.
			for (int i = 0; i < 12; i += 2)
			{
				const float a0 = i * 3.14159265f / 6.f, a1 = (i + 1) * 3.14159265f / 6.f;
				dl->PathArcTo(c, s * 0.3f, a0, a1, 6);
				dl->PathStroke(edge, false, 2.f);
			}
			return;
		}
		if (key.rfind("XboxTypeS_DPad_", 0) == 0)
		{
			// A D-pad: the cross, with the pressed arm lit.
			const float k = s * 0.13f;
			const std::string dir = key.substr(15);
			const auto arm = [&](float dx, float dy, bool on) {
				const ImVec2 a(c.x + dx * 2.f * k - k, c.y + dy * 2.f * k - k), b(a.x + 2.f * k, a.y + 2.f * k);
				dl->AddRectFilled(a, b, on ? theme::u32(pal.accent, pulse) : body, 3.f);
			};
			arm(0, 0, false);
			arm(0, -1, dir == "Up");
			arm(0, 1, dir == "Down");
			arm(-1, 0, dir == "Left");
			arm(1, 0, dir == "Right");
			// A small arrow on the lit arm.
			const float ax = dir == "Left" ? -1.f : dir == "Right" ? 1.f : 0.f, ay = dir == "Up" ? -1.f : dir == "Down" ? 1.f : 0.f;
			const ImVec2 tip(c.x + ax * 2.6f * k, c.y + ay * 2.6f * k), base(c.x + ax * 1.6f * k, c.y + ay * 1.6f * k);
			const ImVec2 side(-ay * 0.55f * k, ax * 0.55f * k);
			dl->AddTriangleFilled(tip, ImVec2(base.x + side.x, base.y + side.y), ImVec2(base.x - side.x, base.y - side.y), ink);
			return;
		}
		if (key == "XboxTypeS_LeftThumbStick" || key == "XboxTypeS_RightThumbStick")
		{
			// A stick seen from above, pressed in: rings and its letter.
			dl->AddCircleFilled(c, s * 0.34f, body, 28);
			dl->AddCircle(c, s * 0.34f, theme::u32(pal.accent, pulse), 28, 3.f);
			dl->AddCircle(c, s * 0.22f, edge, 24, 1.5f);
			centeredText(dl, c, theme::u32(pal.text), key[9] == 'L' ? "L3" : "R3", 1.f);
			return;
		}
		if (key == "XboxTypeS_LeftShoulder" || key == "XboxTypeS_RightShoulder")
		{
			const ImVec2 a(c.x - s * 0.36f, c.y - s * 0.16f), b(c.x + s * 0.36f, c.y + s * 0.16f);
			dl->AddRectFilled(a, b, body, s * 0.16f);
			dl->AddRect(a, b, theme::u32(pal.accent, pulse), s * 0.16f, ImDrawCornerFlags_All, 2.5f);
			centeredText(dl, c, theme::u32(pal.text), key[9] == 'L' ? "LB" : "RB", 0.95f);
			return;
		}
		if (key == "XboxTypeS_LeftTrigger" || key == "XboxTypeS_RightTrigger")
		{
			// A trigger: rounded at the top, flat below.
			const ImVec2 a(c.x - s * 0.24f, c.y - s * 0.3f), b(c.x + s * 0.24f, c.y + s * 0.3f);
			dl->AddRectFilled(a, b, body, s * 0.2f, ImDrawCornerFlags_Top);
			dl->AddRect(a, b, theme::u32(pal.accent, pulse), s * 0.2f, ImDrawCornerFlags_Top, 2.5f);
			centeredText(dl, ImVec2(c.x, c.y + s * 0.05f), theme::u32(pal.text), key[9] == 'L' ? "LT" : "RT", 0.95f);
			return;
		}
		if (key == "XboxTypeS_A" || key == "XboxTypeS_B" || key == "XboxTypeS_X" || key == "XboxTypeS_Y")
		{
			// A face button in its colour.
			const char letter = key.back();
			const ImU32 fill = letter == 'A' ? IM_COL32(83, 196, 108, 255) : letter == 'B' ? IM_COL32(232, 84, 84, 255)
				: letter == 'X' ? IM_COL32(70, 140, 235, 255) : IM_COL32(236, 196, 64, 255);
			dl->AddCircleFilled(c, s * 0.3f, fill, 28);
			if (live)
				dl->AddCircle(c, s * 0.36f, theme::u32(pal.accent, pulse), 28, 2.f);
			const char txt[2] = {letter, 0};
			centeredText(dl, c, IM_COL32(255, 255, 255, 255), txt, 1.15f);
			return;
		}
		if (key == "XboxTypeS_Back" || key == "XboxTypeS_Start")
		{
			// The small middle buttons: Select (two squares) and Start (three lines).
			const ImVec2 a(c.x - s * 0.26f, c.y - s * 0.18f), b(c.x + s * 0.26f, c.y + s * 0.18f);
			dl->AddRectFilled(a, b, body, s * 0.18f);
			dl->AddRect(a, b, theme::u32(pal.accent, pulse), s * 0.18f, ImDrawCornerFlags_All, 2.f);
			const float g = s * 0.06f;
			if (key == "XboxTypeS_Back")
			{
				dl->AddRect(ImVec2(c.x - 2.2f * g, c.y - 1.4f * g), ImVec2(c.x + 0.4f * g, c.y + 0.9f * g), theme::u32(pal.text), 1.f, 0, 1.5f);
				dl->AddRect(ImVec2(c.x - 0.6f * g, c.y - 0.6f * g), ImVec2(c.x + 2.0f * g, c.y + 1.6f * g), theme::u32(pal.text), 1.f, 0, 1.5f);
			}
			else
				for (int i = -1; i <= 1; ++i)
					dl->AddLine(ImVec2(c.x - 1.8f * g, c.y + i * g), ImVec2(c.x + 1.8f * g, c.y + i * g), theme::u32(pal.text), 1.5f);
			return;
		}
		// A keyboard key: a keycap with its name, the text shrinking to fit.
		const std::string name = Controls::displayName(key);
		float scale = 0.95f;
		while (scale > 0.55f && theme::fontAt(ImGui::GetFontSize() * scale)->CalcTextSizeA(ImGui::GetFontSize() * scale, FLT_MAX, 0.f, name.c_str()).x
			> s * 0.62f)
			scale -= 0.05f;
		const ImVec2 a(c.x - s * 0.38f, c.y - s * 0.32f), b(c.x + s * 0.38f, c.y + s * 0.32f);
		dl->AddRectFilled(ImVec2(a.x, a.y + 4.f), ImVec2(b.x, b.y + 4.f), IM_COL32(0, 0, 0, 90), 8.f); // its side
		dl->AddRectFilled(a, b, body, 8.f);
		dl->AddRect(a, b, live ? theme::u32(pal.accent, pulse) : edge, 8.f, ImDrawCornerFlags_All, live ? 2.5f : 1.5f);
		centeredText(dl, ImVec2(c.x, c.y - 1.f), theme::u32(pal.text), name.c_str(), scale);
	}

	bool tileButton(const char* label, ImVec2 size, bool primary)
	{
		const theme::Palette& pal = theme::palette();
		ImGui::PushStyleColor(ImGuiCol_Button, primary ? pal.accent : theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.08f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? theme::mix(pal.accent, ImVec4(1, 1, 1, 1), 0.15f) : theme::mix(pal.card, pal.accent, 0.3f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? pal.accent : theme::mix(pal.card, pal.accent, 0.45f));
		ImGui::PushStyleColor(ImGuiCol_Text, primary ? ImVec4(0.05f, 0.07f, 0.09f, 1.f) : pal.text);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size.y * 0.5f);
		const bool pressed = ImGui::Button(label, size);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(4);
		return pressed;
	}
}

void BindingsScreen::render(float width)
{
	const theme::Palette& pal = theme::palette();
	const float u = scaleNow();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const auto keys = controls_.bindings();
	const auto capture = controls_.capture();
	if (width <= 0.f)
		width = ImGui::GetContentRegionAvail().x;
	const bool active = cb_.exerciseBindsActive && cb_.exerciseBindsActive();

	// Heading: what these are, and whether they're on now.
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const char* chip = active ? "ON NOW" : "ONLY DURING DRILLS";
		const ImVec2 cs = ImGui::CalcTextSize(chip);
		const ImVec2 ca(p.x + width - cs.x - 22.f * u, p.y), cb(p.x + width, p.y + cs.y + 8.f * u);
		dl->AddRectFilled(ca, cb, theme::u32(active ? pal.accent : pal.textDim, active ? 0.25f : 0.12f), (cb.y - ca.y) * 0.5f);
		if (active)
			dl->AddCircleFilled(ImVec2(ca.x + 9.f * u, (ca.y + cb.y) * 0.5f), 3.5f * u, theme::u32(pal.accent), 10);
		dl->AddText(ImVec2(ca.x + 11.f * u + (active ? 4.f * u : 0.f), ca.y + 4.f * u), theme::u32(active ? pal.accent : pal.textDim), chip);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - cs.x - 40.f * u);
		ImGui::TextUnformatted("Drill buttons");
		ImGui::TextDisabled("Free Touch only uses these while a drill runs; your own binds come back when it stops.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 6.f * u));
	}

	// The tiles: two columns when there's room.
	const auto& actions = Controls::actions();
	const int cols = width >= 640.f * u ? 2 : 1;
	const float gap = 12.f * u;
	const float tileW = (width - gap * (cols - 1)) / cols;
	const float tileH = 76.f * u;
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	for (size_t i = 0; i < actions.size(); ++i)
	{
		const auto& action = actions[i];
		ImGui::PushID(action.id.c_str());
		const ImVec2 a(origin.x + (i % cols) * (tileW + gap), origin.y + (i / cols) * (tileH + gap)), b(a.x + tileW, a.y + tileH);
		const auto it = keys.find(action.id);
		const std::string key = it == keys.end() ? "" : it->second;
		const bool waiting = capture.capturing && capture.actionId == action.id;
		const bool hovered = ImGui::IsMouseHoveringRect(a, b);
		dl->AddRectFilled(a, b, theme::u32(waiting ? theme::mix(pal.card, pal.accent, 0.14f)
			: theme::mix(pal.card, ImVec4(1, 1, 1, 1), hovered ? 0.05f : 0.025f)), 12.f);
		if (waiting)
			dl->AddRect(a, b, theme::u32(pal.accent, 0.8f), 12.f, ImDrawCornerFlags_All, 1.5f);
		// The button's picture.
		const float gs = tileH - 16.f * u;
		const ImVec2 ga(a.x + 8.f * u, a.y + 8.f * u), gb(ga.x + gs, ga.y + gs);
		dl->AddRectFilled(ga, gb, theme::u32(pal.bg, 0.85f), 10.f);
		drawGlyph(dl, waiting ? std::string() : key, ImVec2((ga.x + gb.x) * 0.5f, (ga.y + gb.y) * 0.5f), gs, waiting || active);
		if (waiting)
		{
			// Waiting for a press: a pulsing ring.
			const float t = static_cast<float>(ImGui::GetTime());
			dl->AddCircle(ImVec2((ga.x + gb.x) * 0.5f, (ga.y + gb.y) * 0.5f), gs * (0.25f + 0.1f * std::fmod(t * 1.5f, 1.f)),
				theme::u32(pal.accent, 1.f - std::fmod(t * 1.5f, 1.f)), 28, 2.5f);
		}
		// What it does, and the button's name.
		const Look look = lookOf(action.id);
		const float tx = gb.x + 14.f * u;
		const float bw = 96.f * u, bh = 30.f * u;
		const float textW = b.x - tx - bw - 20.f * u;
		const float fs = ImGui::GetFontSize();
		dl->AddText(ImVec2(tx, a.y + 12.f * u), theme::u32(pal.text), look.title);
		const std::string sub = waiting ? std::string("Press a button or key") : std::string(look.help);
		const float ss = fs * 0.85f;
		dl->PushClipRect(ImVec2(tx, a.y), ImVec2(tx + std::max(20.f, textW), b.y), true);
		dl->AddText(theme::fontAt(ss), ss, ImVec2(tx, a.y + 14.f * u + fs), theme::u32(waiting ? pal.accent : pal.textDim), sub.c_str());
		const std::string keyName = key.empty() ? "Not set" : Controls::displayName(key);
		dl->AddText(theme::fontAt(ss), ss, ImVec2(tx, a.y + 16.f * u + fs + ss), theme::u32(pal.textDim, 0.75f), keyName.c_str());
		dl->PopClipRect();
		// Change / Cancel, and Default when it isn't.
		ImGui::SetCursorScreenPos(ImVec2(b.x - bw - 12.f * u, a.y + (tileH - bh) * 0.5f - (key != action.defaultKey && !waiting ? 10.f * u : 0.f)));
		if (waiting)
		{
			if (tileButton("Cancel", ImVec2(bw, bh), false) && cb_.cancelCapture)
				cb_.cancelCapture();
		}
		else if (!capture.capturing)
		{
			if (tileButton(key.empty() ? "Set" : "Change", ImVec2(bw, bh), false) && cb_.rebind)
				cb_.rebind(action.id);
			if (key != action.defaultKey)
			{
				ImGui::SetCursorScreenPos(ImVec2(b.x - bw - 12.f * u, ImGui::GetItemRectMax().y + 2.f * u));
				ImGui::PushStyleColor(ImGuiCol_Text, pal.textDim);
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::mix(pal.card, pal.accent, 0.2f));
				if (ImGui::Button("Default", ImVec2(bw, 20.f * u)) && cb_.resetToDefault)
					cb_.resetToDefault(action.id);
				ImGui::PopStyleColor(3);
			}
		}
		ImGui::PopID();
	}
	const int rows = static_cast<int>((actions.size() + cols - 1) / cols);
	ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (tileH + gap)));
	if (!capture.message.empty() && !capture.capturing)
	{
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
		ImGui::TextDisabled("%s", capture.message.c_str());
		ImGui::PopTextWrapPos();
	}
	ImGui::Dummy(ImVec2(width, 4.f * u));
}
