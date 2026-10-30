#include "pch.h"
#include "ui/Dashboard.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>

#include "IMGUI/imgui_internal.h"
#include "IMGUI/imgui_stdlib.h"
#include "core/DailyChallenge.h"
#include "ui/BindingsScreen.h"
#include "ui/Icons.h"
#include "ui/TutorialScenes.h"
#include "ui/Theme.h"
#include "data/PackRecorder.h"

using icons::Icon;

namespace
{
	// Controller navigation: the dashboard being drawn, so every button, tile, chip and checkbox can register with
	// its focus list right after ImGui draws it (zone 0 = the left rail, 1 = the page).
	Dashboard* g_nav = nullptr;
	using Fx = Dashboard::Feedback;
	// Registers the item for controller navigation, then the hover tick / click sound and the press flash.
	bool navAfterItem(int zone = 1, bool clicked = false, Fx fx = Fx::Click)
	{
		const bool selected = g_nav && g_nav->navHook(zone);
		if (g_nav)
			g_nav->itemFeedback(clicked || selected, fx);
		return selected || clicked;
	}
	// A text button in the dashboard's own style (it was ImGui's flat grey block): laid out exactly like ImGui::Button
	// (its size, padding and text baseline), drawn like bigButton's dark glass one, rounded, with a faint frame that
	// turns accent on hover. A colour pushed for ImGuiCol_Button (the red Stop, an accent Play) fills it instead.
	bool navButton(const char* label, const ImVec2& size = ImVec2(0, 0), Fx fx = Fx::Click)
	{
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (window->SkipItems)
			return false;
		const ImGuiStyle& style = ImGui::GetStyle();
		const ImGuiID id = window->GetID(label);
		const char* textEnd = ImGui::FindRenderedTextEnd(label);
		const ImVec2 ts = ImGui::CalcTextSize(label, textEnd, true);
		const ImVec2 pos = window->DC.CursorPos;
		const ImVec2 sz = ImGui::CalcItemSize(size, ts.x + style.FramePadding.x * 2.f, ts.y + style.FramePadding.y * 2.f);
		const ImRect bb(pos, ImVec2(pos.x + sz.x, pos.y + sz.y));
		ImGui::ItemSize(sz, style.FramePadding.y);
		if (!ImGui::ItemAdd(bb, id))
			return navAfterItem(1, false, fx);
		bool hovered = false, held = false;
		const bool clicked = ImGui::ButtonBehavior(bb, id, &hovered, &held);

		const theme::Palette& pal = theme::palette();
		ImDrawList* dl = window->DrawList;
		const float rounding = std::max(style.FrameRounding, 9.f);
		const ImVec4 fill = ImGui::GetStyleColorVec4(ImGuiCol_Button);
		const ImVec4 themeFill = theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.07f);
		const bool custom = std::fabs(fill.x - themeFill.x) + std::fabs(fill.y - themeFill.y) + std::fabs(fill.z - themeFill.z)
			+ std::fabs(fill.w - themeFill.w) > 0.01f;
		const float dip = held ? 1.f : 0.f;
		const ImVec2 a(bb.Min.x + dip, bb.Min.y + dip), b(bb.Max.x - dip, bb.Max.y - dip);
		if (custom)
		{
			const ImVec4 f = held ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : hovered ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered) : fill;
			dl->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(f), rounding);
			dl->AddRect(a, b, IM_COL32(255, 255, 255, hovered ? 70 : 35), rounding, ImDrawCornerFlags_All, 1.f);
		}
		else
		{
			dl->AddRectFilled(a, b, held ? theme::u32(theme::mix(pal.card, pal.accent, 0.3f)) : IM_COL32(8, 11, 20, hovered ? 220 : 175), rounding);
			dl->AddRect(a, b, hovered ? theme::u32(pal.accent, 0.9f) : IM_COL32(255, 255, 255, 50), rounding, ImDrawCornerFlags_All, 1.2f);
		}
		const ImVec2 textAt(bb.Min.x + (sz.x - ts.x) * style.ButtonTextAlign.x, bb.Min.y + (sz.y - ts.y) * style.ButtonTextAlign.y);
		dl->AddText(ImVec2(textAt.x + dip, textAt.y + dip), ImGui::GetColorU32(ImGuiCol_Text), label, textEnd);
		return navAfterItem(1, clicked, fx);
	}
	bool navInvisible(const char* id, const ImVec2& size, int zone = 1, Fx fx = Fx::Click)
	{
		const bool clicked = ImGui::InvisibleButton(id, size);
		return navAfterItem(zone, clicked, fx);
	}
	bool navCheckbox(const char* label, bool* value)
	{
		bool changed = ImGui::Checkbox(label, value);
		const bool selected = g_nav && g_nav->navHook(1);
		if (selected)
		{
			*value = !*value;
			changed = true;
		}
		if (g_nav)
			g_nav->itemFeedback(changed, Fx::Toggle);
		return changed;
	}

	// SameLine when an item `width` wide still fits on this line (else it starts the next line).
	void sameLineIfFits(float width, float spacing = 10.f)
	{
		const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		if (ImGui::GetItemRectMax().x + spacing + width <= right)
			ImGui::SameLine(0.f, spacing);
	}
	float buttonWidth(const char* label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.f; }

	// 0..1 eased (fast start, soft landing).
	float easeOut(float t)
	{
		t = std::clamp(t, 0.f, 1.f);
		return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
	}

	// The UI's scale against the size it was laid out for (the 17 px body font; 13 px for ImGui's own): BakkesMod's GUI
	// scale makes text bigger, so hand-placed sizes and offsets are multiplied by this to grow with it.
	float uiScale()
	{
		const ImFont* body = theme::bodyFont();
		return std::max(0.75f, ImGui::GetFontSize() / (body ? body->FontSize : 13.f));
	}

	const char* kDifficulties[] = {"easy", "medium", "hard"};
	// Shown only with Settings > Developer tools: captured moments and the object gallery.
	bool isDevCategory(const std::string& category) { return category == "captured" || category == "dev"; }

	// `text` cut to fit `maxWidth` pixels, ending in "..." when it was cut.
	std::string fitText(const std::string& text, float maxWidth)
	{
		if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
			return text;
		std::string out = text;
		while (!out.empty() && ImGui::CalcTextSize((out + "...").c_str()).x > maxWidth)
			out.pop_back();
		return out + "...";
	}

	// Wrapped text that stops after `maxLines` (the last one ends in "..." when there is more). Returns the height used.
	float clampedText(ImDrawList* dl, float size, ImVec2 at, ImU32 col, const std::string& text, float wrapW, int maxLines)
	{
		ImFont* font = theme::fontAt(size);
		const float lineH = size * 1.2f;
		const char* p = text.c_str();
		const char* end = p + text.size();
		int line = 0;
		while (p < end && line < maxLines)
		{
			const char* brk = font->CalcWordWrapPositionA(size / font->FontSize, p, end, wrapW);
			if (brk == p)
				brk = p + 1; // a word wider than the line
			std::string piece(p, brk);
			const bool more = brk < end;
			if (line == maxLines - 1 && more)
			{
				while (!piece.empty() && font->CalcTextSizeA(size, FLT_MAX, 0.f, (piece + "...").c_str()).x > wrapW)
					piece.pop_back();
				while (!piece.empty() && piece.back() == ' ')
					piece.pop_back();
				piece += "...";
			}
			dl->AddText(font, size, ImVec2(at.x, at.y + line * lineH), col, piece.c_str());
			p = brk;
			while (p < end && (*p == ' ' || *p == '\n'))
				++p;
			++line;
		}
		return line * lineH;
	}
	const char* kDifficultyLabels[] = {"Easy", "Medium", "Hard"};

	std::string categoryLabel(const std::string& id)
	{
		static const std::map<std::string, std::string> names = {
			{"basics", "Basics"}, {"saves", "Saves"}, {"pro_saves", "Pro saves"}, {"mechanics", "Mechanics"},
			{"horse", "HORSE"}, {"pinch_bowling", "Pinch bowling"}, {"combo", "Combos"},
			{"backboard_breakout", "Backboard breakout"}, {"movement", "Movement"}, {"first_touch", "First touch"},
			{"speed_lab", "Freeplay+"}, {"glade", "The Glade"}, {"defense", "Defense"}, {"fifties", "50s"},
			{"races", "Races"}, {"speed_flip", "Speed flip"}, {"replay", "Replays"}, {"packs", "Training packs"}, {"captured", "Captured"}, {"dev", "Dev"},
			{"vs_bot", "1v1 vs bot"}, {"__favourites", "Favourites"},
		};
		const auto it = names.find(id);
		if (it != names.end())
			return it->second;
		std::string s = id;
		std::replace(s.begin(), s.end(), '_', ' ');
		if (!s.empty())
			s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
		return s;
	}

	ImVec2 add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }

	// Top-down mini pitch: blue goal on the left, orange on the right, +x up (Unreal is left-handed).
	void drawPreview(ImDrawList* dl, ImVec2 a, ImVec2 b, const ExercisePreview& p)
	{
		const theme::Palette& pal = theme::palette();
		dl->AddRectFilled(a, b, theme::u32(pal.pitch), 8.f);

		const float margin = 14.f;
		const float scale = std::min((b.x - a.x - 2 * margin) / (10240.f + 2 * 880.f), (b.y - a.y - 2 * margin) / 8192.f);
		const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
		auto P = [&](const Vec3& w) { return ImVec2(c.x + w.y * scale, c.y - w.x * scale); };

		const ImU32 line = theme::u32(pal.pitchLine);
		dl->AddRect(P({4096, -5120, 0}), P({-4096, 5120, 0}), line, 4.f, ImDrawCornerFlags_All, 1.5f);
		dl->AddLine(P({4096, 0, 0}), P({-4096, 0, 0}), line, 1.f);
		dl->AddCircle(c, 1000.f * scale, line, 24, 1.f);
		dl->AddRectFilled(P({893, -6000, 0}), P({-893, -5120, 0}), theme::u32(pal.blue, 0.75f), 2.f);
		dl->AddRectFilled(P({893, 5120, 0}), P({-893, 6000, 0}), theme::u32(pal.orange, 0.75f), 2.f);

		for (size_t i = 0; i < p.objects.size(); ++i)
		{
			const float r = std::max(3.f, (i < p.objectRadii.size() ? p.objectRadii[i] : 150.f) * scale);
			dl->AddCircle(P(p.objects[i]), r, theme::u32(pal.accent), 20, 2.f);
		}
		for (const auto& bot : p.bots)
			dl->AddCircleFilled(P(bot), 4.f, theme::u32(pal.orange), 12);

		if (p.ballVisible)
		{
			const ImVec2 ball = P(p.ball);
			const Vec3 ahead = p.ball + p.ballVelocity * 0.6f;
			if (p.ballVelocity.length() > 50.f)
				dl->AddLine(ball, P(ahead), IM_COL32(255, 255, 255, 150), 2.f);
			dl->AddCircleFilled(ball, 4.5f, IM_COL32(245, 245, 245, 255), 16);
		}

		// Car: a small arrow pointing along its yaw.
		const float yaw = p.carYaw * 3.14159265f / 32768.f;
		const ImVec2 car = P(p.car);
		const ImVec2 fwd(std::sin(yaw), -std::cos(yaw));
		const ImVec2 side(-fwd.y, fwd.x);
		dl->AddTriangleFilled(ImVec2(car.x + fwd.x * 8, car.y + fwd.y * 8),
			ImVec2(car.x - fwd.x * 5 + side.x * 5, car.y - fwd.y * 5 + side.y * 5),
			ImVec2(car.x - fwd.x * 5 - side.x * 5, car.y - fwd.y * 5 - side.y * 5), theme::u32(pal.accent));
	}

	// Record page: a recording on the mini pitch (as drawPreview): the ball's path, bright up to the start (it replays)
	// and dim after (you play it); the start's ball (gold ring) and your car there (an arrow).
	void drawTakeMap(ImDrawList* dl, ImVec2 a, ImVec2 b, const RecordTake& t, float startSeconds)
	{
		const theme::Palette& pal = theme::palette();
		dl->AddRectFilled(a, b, theme::u32(pal.pitch), 10.f);
		const float margin = 12.f;
		const float scale = std::min((b.x - a.x - 2 * margin) / (10240.f + 2 * 880.f), (b.y - a.y - 2 * margin) / 8192.f);
		const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
		auto P = [&](const Vec3& w) { return ImVec2(c.x + w.y * scale, c.y - w.x * scale); };
		const ImU32 line = theme::u32(pal.pitchLine);
		dl->AddRect(P({4096, -5120, 0}), P({-4096, 5120, 0}), line, 4.f, ImDrawCornerFlags_All, 1.5f);
		dl->AddLine(P({4096, 0, 0}), P({-4096, 0, 0}), line, 1.f);
		dl->AddCircle(c, 1000.f * scale, line, 24, 1.f);
		dl->AddRectFilled(P({893, -6000, 0}), P({-893, -5120, 0}), theme::u32(pal.blue, 0.75f), 2.f);
		dl->AddRectFilled(P({893, 5120, 0}), P({-893, 6000, 0}), theme::u32(pal.orange, 0.75f), 2.f);
		if (t.ball.size() < 2)
			return;
		const size_t start = std::min(t.ball.size() - 1, static_cast<size_t>(std::lround(startSeconds * t.hz)));
		// Your car's path, faint, then the ball's.
		for (size_t i = 1; i < t.car.size(); ++i)
			dl->AddLine(P(t.car[i - 1]), P(t.car[i]), theme::u32(pal.accent, i <= start ? 0.35f : 0.15f), 1.5f);
		for (size_t i = 1; i < t.ball.size(); ++i)
			dl->AddLine(P(t.ball[i - 1]), P(t.ball[i]), i <= start ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 90),
				i <= start ? 3.f : 2.f);
		const ImU32 gold = IM_COL32(255, 200, 80, 255);
		const ImVec2 ball = P(t.ball[start]);
		dl->AddCircle(ball, 9.f, gold, 20, 2.5f);
		dl->AddCircleFilled(ball, 4.f, IM_COL32(245, 245, 245, 255), 12);
		const float yaw = t.carYaw[start] * 3.14159265f / 32768.f;
		const ImVec2 car = P(t.car[start]);
		const ImVec2 fwd(std::sin(yaw), -std::cos(yaw));
		const ImVec2 side(-fwd.y, fwd.x);
		dl->AddTriangleFilled(ImVec2(car.x + fwd.x * 10, car.y + fwd.y * 10),
			ImVec2(car.x - fwd.x * 6 + side.x * 6, car.y - fwd.y * 6 + side.y * 6),
			ImVec2(car.x - fwd.x * 6 - side.x * 6, car.y - fwd.y * 6 - side.y * 6), gold);
	}

	// Draws an image filling a..b, cropped to keep its aspect ("cover"). False if it is not loaded yet.
	bool drawCover(ImDrawList* dl, const ImageInfo& img, ImVec2 a, ImVec2 b, float rounding, ImU32 tint = IM_COL32_WHITE,
		float zoom = 0.f)
	{
		if (!img.texture || img.width <= 0.f || img.height <= 0.f || b.x <= a.x || b.y <= a.y)
			return false;
		const float boxAspect = (b.x - a.x) / (b.y - a.y);
		const float imgAspect = img.width / img.height;
		ImVec2 uv0(0.f, 0.f), uv1(1.f, 1.f);
		if (imgAspect > boxAspect)
		{
			const float w = boxAspect / imgAspect;
			uv0.x = (1.f - w) * 0.5f;
			uv1.x = uv0.x + w;
		}
		else
		{
			const float h = imgAspect / boxAspect;
			uv0.y = (1.f - h) * 0.5f;
			uv1.y = uv0.y + h;
		}
		// zoom: crop a little more around the centre (0.05 = 5% closer)
		const ImVec2 c((uv0.x + uv1.x) * 0.5f, (uv0.y + uv1.y) * 0.5f);
		const float k = 1.f - zoom;
		uv0 = ImVec2(c.x + (uv0.x - c.x) * k, c.y + (uv0.y - c.y) * k);
		uv1 = ImVec2(c.x + (uv1.x - c.x) * k, c.y + (uv1.y - c.y) * k);
		dl->AddImageRounded(img.texture, a, b, uv0, uv1, tint, rounding);
		return true;
	}

	// Rocket League-style tile pieces (playlist tiles): a dark fade at the bottom for the title, a thin frame, small
	// dark pills that read on any art, and a big title with a drop shadow.
	// A four-corner gradient in a rounded rectangle (ImGui's AddRectFilledMultiColor is square only: over rounded art its
	// dark corners showed as black square corners). Fills the rounded shape, then colours each vertex by where it sits,
	// keeping the anti-aliased edge's fade.
	void roundedGradient(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 tl, ImU32 tr, ImU32 br, ImU32 bl, float rounding,
		ImDrawCornerFlags corners = ImDrawCornerFlags_All)
	{
		if (b.x - a.x < 1.f || b.y - a.y < 1.f)
			return;
		const int start = dl->VtxBuffer.Size;
		dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, corners);
		const ImVec4 c[4] = {ImGui::ColorConvertU32ToFloat4(tl), ImGui::ColorConvertU32ToFloat4(tr), ImGui::ColorConvertU32ToFloat4(br),
			ImGui::ColorConvertU32ToFloat4(bl)};
		const auto lerp = [](const ImVec4& p, const ImVec4& q, float t) {
			return ImVec4(p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t, p.w + (q.w - p.w) * t);
		};
		for (int i = start; i < dl->VtxBuffer.Size; ++i)
		{
			ImDrawVert& v = dl->VtxBuffer[i];
			const float tx = std::clamp((v.pos.x - a.x) / (b.x - a.x), 0.f, 1.f);
			const float ty = std::clamp((v.pos.y - a.y) / (b.y - a.y), 0.f, 1.f);
			ImVec4 col = lerp(lerp(c[0], c[1], tx), lerp(c[3], c[2], tx), ty);
			col.w *= static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.f; // the edge fringe
			v.col = ImGui::ColorConvertFloat4ToU32(col);
		}
	}

	void tileFade(ImDrawList* dl, ImVec2 a, ImVec2 b, float portion, float rounding = 10.f)
	{
		const ImU32 clear = IM_COL32(4, 6, 12, 0);
		const float top = b.y - (b.y - a.y) * portion;
		roundedGradient(dl, ImVec2(a.x, top), b, clear, clear, IM_COL32(4, 6, 12, 200), IM_COL32(4, 6, 12, 200), rounding,
			ImDrawCornerFlags_Bot);
		// Corner shade behind the title: darkest in the bottom-left corner, fading diagonally toward the art.
		const ImVec2 cornerTop(a.x, a.y + (b.y - a.y) * 0.2f);
		const ImVec2 cornerEnd(a.x + (b.x - a.x) * 0.8f, b.y);
		roundedGradient(dl, cornerTop, cornerEnd, clear, clear, clear, IM_COL32(4, 6, 12, 215), rounding, ImDrawCornerFlags_BotLeft);
	}

	// Text with a soft shadow (several faint offsets, like a small blur) so it reads on bright art.
	void shadowedText(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 color, const char* text, float wrap = 0.f)
	{
		static const ImVec2 offsets[] = {{-1.f, 0.f}, {1.f, 0.f}, {0.f, -1.f}, {0.f, 1.f}, {1.5f, 1.5f}, {2.f, 2.5f}, {0.f, 2.5f}};
		for (const ImVec2& o : offsets)
			dl->AddText(theme::fontAt(size), size, ImVec2(at.x + o.x, at.y + o.y), IM_COL32(0, 0, 0, 90), text, nullptr, wrap);
		dl->AddText(theme::fontAt(size), size, at, color, text, nullptr, wrap);
	}

	// `hover` 0..1 (animated): the frame fades from blue to the accent and thickens, with a soft outer glow.
	void tileFrame(ImDrawList* dl, ImVec2 a, ImVec2 b, float hover, bool selected)
	{
		const theme::Palette& pal = theme::palette();
		const float t = selected ? 1.f : hover;
		if (t > 0.01f)
			dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), theme::u32(pal.accent, 0.25f * t), 12.f,
				ImDrawCornerFlags_All, 4.f);
		dl->AddRect(a, b, theme::u32(theme::mix(theme::mix(pal.blue, ImVec4(0, 0, 0, 0), 0.45f), pal.accent, t)), 10.f,
			ImDrawCornerFlags_All, 1.5f + 1.5f * t);
	}

	// Returns the pill's width.
	float darkPill(ImDrawList* dl, ImVec2 pos, const std::string& text, ImU32 textColor = IM_COL32(240, 244, 250, 255))
	{
		const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
		dl->AddRectFilled(pos, ImVec2(pos.x + ts.x + 16, pos.y + ts.y + 6), IM_COL32(6, 9, 16, 190), 20.f);
		dl->AddText(ImVec2(pos.x + 8, pos.y + 3), textColor, text.c_str());
		return ts.x + 16;
	}

	// "Shoot on a defender: popped ball" -> kicker "SHOOT ON A DEFENDER", title "Popped ball". Captures named by
	// time ("Captured: 20261012_230316") read "Oct 12, 23:03:16".
	struct TileName { std::string kicker, title; };
	TileName splitName(const std::string& name)
	{
		auto trim = [](std::string s) {
			s.erase(0, s.find_first_not_of(' '));
			s.erase(s.find_last_not_of(' ') + 1);
			return s;
		};
		TileName n;
		const size_t colon = name.find(':');
		if (colon == std::string::npos)
		{
			n.title = name;
			return n;
		}
		n.kicker = trim(name.substr(0, colon));
		for (auto& ch : n.kicker)
			ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		n.title = trim(name.substr(colon + 1));
		if (!n.title.empty())
			n.title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n.title[0])));
		// yyyymmdd_hhmmss[_n]
		const std::string& t = n.title;
		const bool stamp = t.size() >= 15 && t[8] == '_' && std::all_of(t.begin(), t.begin() + 8, ::isdigit)
			&& std::all_of(t.begin() + 9, t.begin() + 15, ::isdigit);
		if (stamp)
		{
			static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
			const int month = std::clamp(std::stoi(t.substr(4, 2)), 1, 12);
			n.title = std::string(kMonths[month - 1]) + " " + std::to_string(std::stoi(t.substr(6, 2))) + ", "
				+ t.substr(9, 2) + ":" + t.substr(11, 2) + ":" + t.substr(13, 2) + t.substr(15);
		}
		return n;
	}

	// Kicker (small caps line) over the title, bottom-left, with a drop shadow; the title wraps to two lines at most.
	void tileTitle(ImDrawList* dl, ImVec2 a, ImVec2 b, const std::string& name, float scale)
	{
		const TileName n = splitName(name);
		ImFont* font = ImGui::GetFont();
		const float size = ImGui::GetFontSize() * scale;
		const float wrap = b.x - a.x - 32.f;
		const ImVec2 ts = theme::fontAt(size)->CalcTextSizeA(size, FLT_MAX, wrap, n.title.c_str());
		ImVec2 at(a.x + 16.f, b.y - 14.f - std::min(ts.y, size * 2.3f));
		shadowedText(dl, font, size, at, IM_COL32(250, 252, 255, 255), n.title.c_str(), wrap);
		if (!n.kicker.empty())
		{
			const float ks = ImGui::GetFontSize() * 0.78f;
			shadowedText(dl, font, ks, ImVec2(at.x, at.y - ks - 3.f), IM_COL32(215, 226, 242, 240), n.kicker.c_str());
		}
	}

	// Moves `v` toward `target` at `speed` per second (hover animations).
	float approach(float v, float target, float speed)
	{
		const float step = ImGui::GetIO().DeltaTime * speed;
		return v < target ? std::min(target, v + step) : std::max(target, v - step);
	}

	void pill(ImDrawList* dl, ImVec2 pos, const std::string& text, const ImVec4& color)
	{
		const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
		dl->AddRectFilled(pos, ImVec2(pos.x + ts.x + 16, pos.y + ts.y + 6), theme::u32(color, 0.18f), 20.f);
		dl->AddText(ImVec2(pos.x + 8, pos.y + 3), theme::u32(color), text.c_str());
	}

	// A selectable chip; returns true when clicked.
	bool chip(const char* label, bool selected)
	{
		// Selected: the accent, solid, with dark text (it was a half-mixed teal that the dark text barely showed on).
		const theme::Palette& pal = theme::palette();
		ImGui::PushStyleColor(ImGuiCol_Button, selected ? pal.accent : theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.07f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? theme::mix(pal.accent, ImVec4(1, 1, 1, 1), 0.15f)
			: theme::mix(pal.card, pal.accent, 0.3f));
		ImGui::PushStyleColor(ImGuiCol_Text, selected ? ImVec4(0.04f, 0.06f, 0.08f, 1.f) : pal.text);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.f);
		bool clicked = ImGui::Button(label);
		clicked = navAfterItem(1, clicked);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);
		return clicked;
	}
}

void Dashboard::setModel(std::shared_ptr<const DashboardModel> model)
{
	std::lock_guard lock(mutex_);
	model_ = std::move(model);
}

void Dashboard::previewPage(const std::string& page, const std::string& detailId)
{
	static const std::map<std::string, Page> pages = {{"home", Page::Home}, {"daily", Page::Daily}, {"freeplay", Page::FreeplayPlus},
		{"sessions", Page::Sessions}, {"exercises", Page::Exercises}, {"routine", Page::QuickPlay}, {"saved", Page::SavedShots},
		{"record", Page::Record}, {"races", Page::Races}, {"packs", Page::Packs}, {"replays", Page::Replays},
		{"progress", Page::Stats}, {"friends", Page::Friends}, {"board", Page::Board}, {"settings", Page::Settings}};
	if (const auto it = pages.find(page); it != pages.end())
		page_ = it->second;
	detailId_ = detailId;
	if (detailId.rfind("toast:", 0) == 0)
	{
		toast(detailId.substr(6)); // the preview's confirmation message
		detailId_.clear();
	}
	if (detailId.rfind("tutorial:", 0) == 0)
	{
		// The preview's tutorial: "tutorial:<id>:<step>[:<seconds into its animation>]".
		const std::string rest = detailId.substr(9);
		const size_t colon = rest.find(':');
		openTutorial(rest.substr(0, colon));
		tutorial_.step = colon == std::string::npos ? 0 : std::atoi(rest.c_str() + colon + 1);
		if (const size_t at = colon == std::string::npos ? std::string::npos : rest.find(':', colon + 1); at != std::string::npos)
			tutorial_.stepAt -= std::atof(rest.c_str() + at + 1);
		detailId_.clear();
	}
	if (detailId.rfind("raceboard:", 0) == 0)
	{
		raceBoard_.requested = true; // the preview's race board
		raceBoard_.exerciseId = detailId.substr(10);
		raceBoard_.race = "races.half_flip_medium";
		raceBoard_.name = "Half flip";
		detailId_.clear();
	}
	if (detailId.rfind("setup:", 0) == 0)
	{
		shotshare::Extras x;
		x.defender = "nexto";
		x.target = "pick";
		x.size = "hard";
		x.open = (1u << 7) | (1u << 6);
		openPlaySetup(false, detailId.substr(6), "", "Recorded: my wall pop", x); // the preview's Play setup
		detailId_.clear();
	}
	if ((page_ == Page::SavedShots || page_ == Page::Record) && detailId.rfind("edit:", 0) == 0)
	{
		editingShot_ = detailId.substr(5); // the preview's saved-shot editor
		snprintf(renameBuf_, sizeof(renameBuf_), "%s", "my wall pop");
		detailId_.clear();
	}
	if (page_ == Page::QuickPlay && detailId == "edit")
	{
		routineEditing_ = true; // the preview's Routine editor
		detailId_.clear();
	}
	if (page_ == Page::Stats && detailId.rfind("tab:", 0) == 0)
	{
		progressTab_ = std::atoi(detailId.c_str() + 4); // the preview's Progress tabs ("tab:4:<day>" opens a journal day)
		if (const size_t day = detailId.find(':', 4); day != std::string::npos)
			journalOpen_ = detailId.substr(day + 1);
		detailId_.clear();
	}
	pageAt_ = ImGui::GetTime();
}

void Dashboard::setStatus(const DashboardStatus& status)
{
	std::lock_guard lock(mutex_);
	status_ = status;
}

void Dashboard::pageTitle(const char* title, const char* subtitle)
{
	// Under tabs (drawn before the page) the open tab names the page: only its line of description. A one-page place
	// keeps its title, with the ? button on its row.
	if (hubTabs(hubOf(page_)).size() <= 1)
	{
		drawHubTabs();
		theme::fontScale(1.55f);
		ImGui::TextUnformatted(title);
		theme::fontScale(1.f);
	}
	if (subtitle && *subtitle)
		ImGui::TextDisabled("%s", subtitle);
	ImGui::Dummy(ImVec2(0, 6));
}

void Dashboard::queueNav(const std::string& action)
{
	std::lock_guard lock(navMutex_);
	navQueue_.push_back(action);
}

bool Dashboard::navHook(int zone)
{
	const int idx = navSeen_[zone]++;
	{
		const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
		navRects_[zone].push_back(ImVec4(mn.x, mn.y, mx.x, mx.y));
	}
	if (!navActive_ || zone != navZone_ || idx != navIndex_[zone])
		return false;
	// The focused item: a softly pulsing accent outline, scrolled into view after a move, activated by select.
	const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRect(ImVec2(a.x - 5, a.y - 5), ImVec2(b.x + 5, b.y + 5), theme::u32(theme::palette().accent, 0.2f * pulse), 10.f,
		ImDrawCornerFlags_All, 4.f);
	dl->AddRect(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), theme::u32(theme::palette().accent, 0.75f + 0.25f * pulse),
		8.f, ImDrawCornerFlags_All, 2.5f);
	if (navScroll_)
	{
		ImGui::SetScrollHereY(0.5f);
		navScroll_ = false;
	}
	if (navSelect_)
	{
		navSelect_ = false;
		return true;
	}
	return false;
}

void Dashboard::sound(const char* name)
{
	if (cb_.sound)
		cb_.sound(name);
}

void Dashboard::itemFeedback(bool activated, Feedback fx)
{
	// Hover tick when the mouse comes onto a new item.
	if (ImGui::IsItemHovered())
	{
		hoverSeen_ = true;
		const ImGuiID id = ImGui::GetItemID();
		if (id != hoveredId_)
		{
			hoveredId_ = id;
			sound("ui_hover");
		}
	}
	if (!activated)
		return;
	flashMin_ = ImGui::GetItemRectMin();
	flashMax_ = ImGui::GetItemRectMax();
	flashAt_ = ImGui::GetTime();
	switch (fx)
	{
	case Feedback::Click: sound("ui_click"); break;
	case Feedback::Confirm: sound("ui_confirm"); break;
	case Feedback::Toggle: sound("ui_toggle"); break;
	case Feedback::Silent: break;
	}
}

int Dashboard::navSpatial(int zone, const std::string& dir) const
{
	// A 2D move: the nearest item whose centre lies that way, preferring ones in line with the focused item (rows
	// for left/right, columns for up/down) over ones closer but off to the side.
	const auto& rects = navRectsPrev_[zone];
	const int cur = navIndex_[zone];
	if (cur < 0 || cur >= static_cast<int>(rects.size()))
		return -1;
	const ImVec4 c = rects[cur];
	const float cx = (c.x + c.z) * 0.5f, cy = (c.y + c.w) * 0.5f;
	const bool vertical = dir == "up" || dir == "down";
	const float sign = dir == "down" || dir == "right" ? 1.f : -1.f;
	int best = -1;
	float bestScore = 1e30f;
	for (int i = 0; i < static_cast<int>(rects.size()); ++i)
	{
		if (i == cur)
			continue;
		const ImVec4 r = rects[i];
		const float rx = (r.x + r.z) * 0.5f, ry = (r.y + r.w) * 0.5f;
		// How far it is that way (edge to edge), and how far off to the side (0 when they overlap across).
		float along, side, centreOff;
		if (vertical)
		{
			if ((ry - cy) * sign < 4.f)
				continue;
			along = std::max(0.f, sign > 0 ? r.y - c.w : c.y - r.w);
			side = std::max(0.f, std::max(r.x - c.z, c.x - r.z));
			centreOff = std::fabs(rx - cx);
		}
		else
		{
			if ((rx - cx) * sign < 4.f)
				continue;
			along = std::max(0.f, sign > 0 ? r.x - c.z : c.x - r.z);
			side = std::max(0.f, std::max(r.y - c.w, c.y - r.w));
			centreOff = std::fabs(ry - cy);
		}
		const float score = along + side * 4.f + centreOff * 0.15f;
		if (score < bestScore)
		{
			bestScore = score;
			best = i;
		}
	}
	return best;
}

void Dashboard::applyNav()
{
	std::vector<std::string> actions;
	{
		std::lock_guard lock(navMutex_);
		actions.swap(navQueue_);
	}
	if (ImGui::GetIO().MouseDelta.x != 0.f || ImGui::GetIO().MouseDelta.y != 0.f)
		navActive_ = false; // the mouse took over
	for (const auto& act : actions)
	{
		if (!navActive_ && act != "close" && act != "back")
		{
			navActive_ = true; // the first press only shows where the focus is
			sound("ui_hover");
			continue;
		}
		if (act == "up" || act == "down" || act == "left" || act == "right")
			sound("ui_hover");
		// The rail is a list; the page moves in 2D (left off its left edge goes to the rail).
		const bool spatial = navZone_ == 1 && !navRectsPrev_[1].empty();
		if (act == "up" || act == "down")
		{
			if (spatial)
			{
				if (const int to = navSpatial(1, act); to >= 0)
					navIndex_[1] = to;
			}
			else
			{
				navIndex_[navZone_] += act == "down" ? 1 : -1;
			}
			navScroll_ = true;
		}
		else if (act == "left")
		{
			if (navZone_ == 1)
			{
				if (const int to = spatial ? navSpatial(1, act) : -1; to >= 0)
				{
					navIndex_[1] = to;
					navScroll_ = true;
				}
				else
				{
					navZone_ = 0;
				}
			}
		}
		else if (act == "right")
		{
			if (navZone_ == 0)
				navZone_ = 1;
			else if (const int to = spatial ? navSpatial(1, act) : -1; to >= 0)
				navIndex_[1] = to;
			navScroll_ = true;
		}
		else if (act == "select") navSelect_ = true;
		else if (act == "back")
		{
			sound("ui_back");
			if (page_ == Page::Exercises && !detailId_.empty()) { detailId_.clear(); pageAt_ = ImGui::GetTime(); navIndex_[1] = 0; }
			else if (page_ != Page::Home) { page_ = Page::Home; navZone_ = 0; navIndex_[0] = 0; }
			else closeRequested_ = true;
		}
		else if (act == "close") closeRequested_ = true;
		for (int z = 0; z < 2; ++z)
			navIndex_[z] = std::clamp(navIndex_[z], 0, std::max(0, navCount_[z] - 1));
	}
}

void Dashboard::render(bool* open)
{
	g_nav = this;
	navSeen_[0] = navSeen_[1] = 0;
	navRects_[0].clear();
	navRects_[1].clear();
	hoverSeen_ = false;
	const double now = ImGui::GetTime();
	// Opened again (no frame drawn for a while): fade in and replay the Home entrance.
	if (ImGui::GetFrameCount() - lastFrame_ > 2)
	{
		openedAt_ = pageAt_ = homeAt_ = now;
		hoveredId_ = 0;
	}
	lastFrame_ = ImGui::GetFrameCount();
	applyNav();
	if (page_ != shownPage_)
	{
		if (shownPage_ != Page::Home || page_ != Page::Home)
			sound("ui_page");
		shownPage_ = page_;
		pageAt_ = now;
		if (page_ == Page::Home)
			homeAt_ = now;
	}
	std::shared_ptr<const DashboardModel> model;
	DashboardStatus status;
	{
		std::lock_guard lock(mutex_);
		model = model_;
		status = status_;
	}
	devTools_ = status.devTools;
	friendRequests_ = status.friends.incoming;
	hubLast_[static_cast<int>(hubOf(page_))] = page_;
	dailyWaiting_ = status.daily && status.daily->available && !status.daily->done && !status.daily->running;
	sharedWaiting_ = 0;
	for (const auto& i : status.shots.inbox)
		sharedWaiting_ += !(status.haveKeys && status.haveKeys->count(i.key));
	if (status.inbox)
		for (const auto& f : *status.inbox)
			sharedWaiting_ += !f.imported;
	if (page_ == Page::Friends && lastPage_ != Page::Friends && cb_.refreshFriends)
		cb_.refreshFriends(); // a fresh list each time the page opens
	if (page_ == Page::Board && lastPage_ != Page::Board && cb_.refreshShotBoard)
		cb_.refreshShotBoard(status.shots.sort);
	if (page_ == Page::Races && lastPage_ != Page::Races && cb_.refreshRaceLeaders)
		cb_.refreshRaceLeaders();
	if (page_ == Page::Daily && lastPage_ != Page::Daily && cb_.refreshDailyBoard && status.daily)
		cb_.refreshDailyBoard(status.daily->date, status.boards.daily.scope);
	lastPage_ = page_;
	// Results of sharing online, as a toast.
	if (status.shots.message != shotsMessage_ || status.shots.error != shotsError_)
	{
		if (!status.shots.message.empty() && status.shots.message != shotsMessage_)
			toast(status.shots.message);
		else if (!status.shots.error.empty() && status.shots.error != shotsError_ && page_ != Page::Board)
			toast(status.shots.error);
		shotsMessage_ = status.shots.message;
		shotsError_ = status.shots.error;
	}
	if (status.noticeSeq != seenNoticeSeq_)
	{
		// Not the one there before the window first drew.
		if (seenNoticeSeq_ >= 0 && !status.notice.empty())
			toast(status.notice);
		seenNoticeSeq_ = status.noticeSeq;
	}
	if (!devTools_ && page_ == Page::SavedShots)
		page_ = Page::Home;
	// A daily just finished: the menu opens on its result (not on the first status after loading).
	if (status.daily)
	{
		if (seenDailySeq_ < 0)
			seenDailySeq_ = status.daily->finishedSeq;
		else if (status.daily->finishedSeq != seenDailySeq_)
		{
			seenDailySeq_ = status.daily->finishedSeq;
			page_ = Page::Daily;
			detailId_.clear();
		}
	}
	static const DashboardModel kEmpty;
	const DashboardModel& m = model ? *model : kEmpty;

	ImGui::SetNextWindowSize(ImVec2(1120.f, 700.f), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(900.f, 560.f), ImVec2(FLT_MAX, FLT_MAX));
	theme::push();
	ImFont* bodyFont = theme::bodyFont(); // Barlow once loaded (else ImGui's own font, as before)
	if (bodyFont)
		ImGui::PushFont(bodyFont);
	if (ImGui::Begin("Free Touch###freetouch_dashboard", open,
		ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		renderRail();
		ImGui::SameLine(0.f, 0.f);

		ImGui::BeginGroup();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.f, 24.f));
		// The scrollbar's room is always kept: one that comes and goes changes the width, so the cards' sizes, so
		// whether it is needed, and the page flickered every frame at that height.
		// The Exercises grid scrolls in its own list under a fixed toolbar, so this page does not scroll itself.
		const bool ownScroll = page_ == Page::Exercises && detailId_.empty();
		ImGui::BeginChild("content", ImVec2(0.f, -footerHeight()), false, ImGuiWindowFlags_AlwaysUseWindowPadding
			| (ownScroll ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_AlwaysVerticalScrollbar));
		ImGui::PopStyleVar();
		ImGui::PushTextWrapPos(0.f); // text wraps at the page's edge at any window size
		// Page entrance: slides up a little while it fades in (the fade is drawn over it below).
		const float pageIn = easeOut(static_cast<float>((now - pageAt_) / 0.3));
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 18.f * (1.f - pageIn));
		const ImVec2 contentMin = ImGui::GetWindowPos();
		const ImVec2 contentMax = add(contentMin, ImGui::GetWindowSize());
		// A place with several pages: its tabs on every one of them (Freeplay+ and a drill's own page have no page
		// title, so the tabs can't live in pageTitle alone: Lambourne 2026-10-29, "the tabs go away on Freeplay+").
		if (hubTabs(hubOf(page_)).size() > 1)
			drawHubTabs();
		switch (page_)
		{
		case Page::Home: renderHome(m, status); break;
		case Page::FreeplayPlus: renderFreeplayPlus(m); break;
		case Page::Sessions: renderSessions(m, status); break;
		case Page::Exercises: renderExercises(m, status); break;
		case Page::QuickPlay: renderWarmup(m, status); break;
		case Page::SavedShots: renderSavedShots(m, status); break;
		case Page::Record: renderRecord(m, status); break;
		case Page::Races: renderRaces(m, status); break;
		case Page::Daily: renderDaily(m, status); break;
		case Page::Packs: renderPacks(m, status); break;
		case Page::Replays: renderReplays(m, status); break;
		case Page::Stats: renderProgress(m, status); break;
		case Page::Friends: renderFriends(status); break;
		case Page::Board: renderBoard(status); break;
		case Page::Settings: renderSettings(m, status); break;
		}
		ImGui::PopTextWrapPos();
		ImGui::EndChild();
		renderStatusBar(m, status);
		ImGui::EndGroup();
		renderPlaySetup(status);
		renderRaceBoard(status);
		renderTutorial(m, status);

		// Over everything (nested lists included), only for a moment: the page fade, the window fade-in on open, and
		// the flash of the last pressed item.
		ImDrawList* fg = ImGui::GetForegroundDrawList();
		const ImVec4 bg = theme::palette().bg;
		if (pageIn < 1.f)
			fg->AddRectFilled(contentMin, contentMax, theme::u32(ImVec4(bg.x, bg.y, bg.z, 1.f), 1.f - pageIn));
		const float openIn = easeOut(static_cast<float>((now - openedAt_) / 0.22));
		if (openIn < 1.f)
		{
			const ImVec2 wp = ImGui::GetWindowPos();
			fg->AddRectFilled(wp, add(wp, ImGui::GetWindowSize()), theme::u32(ImVec4(bg.x, bg.y, bg.z, 1.f), 1.f - openIn), 8.f);
		}
		const float flash = static_cast<float>((now - flashAt_) / 0.4);
		if (flash < 1.f)
		{
			const float grow = 2.f + 10.f * easeOut(flash);
			fg->AddRect(ImVec2(flashMin_.x - grow, flashMin_.y - grow), ImVec2(flashMax_.x + grow, flashMax_.y + grow),
				theme::u32(theme::palette().accent, 0.8f * (1.f - flash)), 12.f, ImDrawCornerFlags_All, 2.f);
			if (flash < 0.3f)
				fg->AddRectFilled(flashMin_, flashMax_, IM_COL32(255, 255, 255, static_cast<int>(40 * (1.f - flash / 0.3f))), 10.f);
		}
		// A short confirmation ("Added to your warm-up") centred above the footer: rises in, holds, fades out.
		const float toastAge = static_cast<float>(now - toastAt_);
		if (!toast_.empty() && toastAge < 2.4f)
		{
			const theme::Palette& pal = theme::palette();
			const float u = uiScale();
			const float in = easeOut(std::min(1.f, toastAge / 0.18f));
			const float alpha = in * std::clamp((2.4f - toastAge) / 0.35f, 0.f, 1.f);
			const ImVec2 ts = ImGui::CalcTextSize(toast_.c_str());
			const float dot = 7.f * u;
			const ImVec2 size(ts.x + dot + 44.f * u, ts.y + 20.f * u);
			const ImVec2 at((contentMin.x + contentMax.x - size.x) * 0.5f, contentMax.y - size.y - 18.f * u + 10.f * u * (1.f - in));
			const ImVec2 end(at.x + size.x, at.y + size.y);
			fg->AddRectFilled(ImVec2(at.x, at.y + 3.f * u), ImVec2(end.x, end.y + 3.f * u), IM_COL32(0, 0, 0, static_cast<int>(90 * alpha)), size.y * 0.5f);
			fg->AddRectFilled(at, end, theme::u32(theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.06f), 0.97f * alpha), size.y * 0.5f);
			fg->AddRect(at, end, theme::u32(pal.accent, 0.55f * alpha), size.y * 0.5f, ImDrawCornerFlags_All, 1.2f);
			fg->AddCircleFilled(ImVec2(at.x + 18.f * u + dot * 0.5f, at.y + size.y * 0.5f), dot * 0.5f, theme::u32(pal.accent, alpha), 12);
			fg->AddText(ImVec2(at.x + 26.f * u + dot, at.y + (size.y - ts.y) * 0.5f), theme::u32(pal.text, alpha), toast_.c_str());
		}
	}
	theme::fontScale(1.f); // no scaled font left pushed
	ImGui::End();
	if (bodyFont)
		ImGui::PopFont();
	if (!hoverSeen_)
		hoveredId_ = 0;
	theme::pop();
	navCount_[0] = navSeen_[0];
	navCount_[1] = navSeen_[1];
	navRectsPrev_[0].swap(navRects_[0]);
	navRectsPrev_[1].swap(navRects_[1]);
	g_nav = nullptr;

	if (closeRequested_)
	{
		closeRequested_ = false;
		if (open)
			*open = false;
	}
}

Dashboard::Hub Dashboard::hubOf(Page page)
{
	switch (page)
	{
	case Page::Home: return Hub::Home;
	case Page::Exercises: case Page::Sessions: case Page::FreeplayPlus: case Page::QuickPlay: return Hub::Train;
	case Page::Daily: case Page::Races: case Page::Board: return Hub::Compete;
	case Page::Record: case Page::Packs: case Page::Replays: case Page::SavedShots: return Hub::Create;
	case Page::Friends: return Hub::Friends;
	case Page::Stats: return Hub::Progress;
	case Page::Settings: return Hub::Settings;
	}
	return Hub::Home;
}

const char* Dashboard::hubKey(Hub hub)
{
	switch (hub)
	{
	case Hub::Home: return "home";
	case Hub::Train: return "train";
	case Hub::Compete: return "compete";
	case Hub::Create: return "create";
	case Hub::Friends: return "friends";
	case Hub::Progress: return "progress";
	case Hub::Settings: return "settings";
	}
	return "home";
}

std::vector<Dashboard::HubTab> Dashboard::hubTabs(Hub hub) const
{
	switch (hub)
	{
	case Hub::Train:
		return {{Page::Exercises, "Exercises"}, {Page::Sessions, "Sessions & Labs"}, {Page::FreeplayPlus, "Freeplay+"},
			{Page::QuickPlay, "Routine"}};
	case Hub::Compete:
		return {{Page::Daily, "Daily"}, {Page::Races, "Races"}, {Page::Board, "Shot Board"}};
	case Hub::Create:
	{
		std::vector<HubTab> tabs = {{Page::Record, "Record"}, {Page::Packs, "Training Packs"}, {Page::Replays, "Replays"}};
		if (devTools_)
			tabs.push_back({Page::SavedShots, "Saved Shots"});
		return tabs;
	}
	case Hub::Home: return {{Page::Home, "Home"}};
	case Hub::Friends: return {{Page::Friends, "Friends"}};
	case Hub::Progress: return {{Page::Stats, "Progress"}};
	case Hub::Settings: return {{Page::Settings, "Settings"}};
	}
	return {};
}

void Dashboard::renderRail()
{
	// The side menu: seven places instead of fourteen pages (Lambourne 2026-10-29). Each row is the place's name with
	// what's in it underneath, and a dot or count when something waits there; the place opens on the page last used.
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	struct Item { Hub hub; Icon icon; const char* label; const char* caption; };
	struct Group { const char* title; std::vector<Item> items; };
	static const std::vector<Group> groups = {
		{"", {{Hub::Home, Icon::Home, "Home", "Today at a glance"},
			{Hub::Train, Icon::Exercises, "Train", "Drills and routine"},
			{Hub::Compete, Icon::Trophy, "Compete", "Daily and races"},
			{Hub::Create, Icon::Record, "Create", "Your own drills"}}},
		{"YOU", {{Hub::Friends, Icon::Friends, "Friends", "Requests and activity"},
			{Hub::Progress, Icon::Stats, "Progress", "Rank, records, journal"},
			{Hub::Settings, Icon::Settings, "Settings", "Account and controls"}}},
	};
	float widest = ImGui::CalcTextSize("offline training").x;
	for (const auto& g : groups)
		for (const auto& item : g.items)
			widest = std::max(widest, ImGui::CalcTextSize(item.label).x);
	const float railW = std::round(std::max(220.f, widest + 120.f * u));
	ImGui::BeginChild("rail", ImVec2(railW, 0.f), false, ImGuiWindowFlags_NoScrollbar);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 wp = ImGui::GetWindowPos();
	const ImVec2 ws = ImGui::GetWindowSize();
	dl->AddRectFilled(wp, add(wp, ws), theme::u32(pal.rail), 10.f, ImDrawCornerFlags_Left);

	const float padX = 22.f * u;
	ImGui::SetCursorPos(ImVec2(padX, 24.f * u));
	theme::fontScale(1.5f);
	ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
	ImGui::TextUnformatted("FREE TOUCH");
	ImGui::PopStyleColor();
	theme::fontScale(1.f);
	ImGui::SetCursorPosX(padX);
	ImGui::TextDisabled("training");

	const float fs = ImGui::GetFontSize();
	const float cs = fs * 0.74f; // the caption's size
	const float labelH = fs * 0.72f + 1.f * u;
	int rows = 0;
	for (const auto& g : groups)
		rows += static_cast<int>(g.items.size());
	// Rows tighten (the caption goes first) when the window is too short for them all at a large GUI scale.
	const float room = ws.y - ImGui::GetCursorPosY() - 10.f * u;
	float groupGap = 18.f * u, rowH = std::max(50.f * u, fs + cs + 18.f * u);
	bool captions = true;
	auto need = [&] { return groups.size() * (groupGap + labelH) + rows * (rowH + 4.f * u); };
	if (need() > room)
	{
		groupGap = 8.f * u;
		if (need() > room)
		{
			captions = false;
			rowH = std::max(30.f * u, fs + 14.f * u);
		}
	}
	const float sideGap = 10.f * u;
	const Hub current = hubOf(page_);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.f, 4.f * u));
	for (const auto& g : groups)
	{
		ImGui::Dummy(ImVec2(0, groupGap));
		if (*g.title)
		{
			ImGui::SetCursorPosX(padX);
			const ImVec2 lp = ImGui::GetCursorScreenPos();
			const float ls = fs * 0.72f;
			dl->AddText(theme::fontAt(ls), ls, lp, theme::u32(pal.textDim, 0.65f), g.title);
			ImGui::Dummy(ImVec2(0, ls + 1.f * u));
		}
		for (const auto& item : g.items)
		{
			ImGui::SetCursorPosX(sideGap);
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const ImVec2 size(railW - 2.f * sideGap, rowH);
			ImGui::PushID(item.label);
			if (navInvisible("nav", size, 0, Fx::Silent))
			{
				// The page last used in that place (else its first).
				const auto last = hubLast_.find(static_cast<int>(item.hub));
				page_ = last != hubLast_.end() ? last->second : hubTabs(item.hub).front().page;
				navZone_ = 1;
				navIndex_[1] = 0;
			}
			const bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();
			const bool selected = current == item.hub;
			if (selected || hovered)
				dl->AddRectFilled(pos, add(pos, size), theme::u32(selected ? theme::mix(pal.rail, pal.accent, 0.18f) : pal.cardHover), 10.f);
			if (selected)
				dl->AddRectFilled(ImVec2(pos.x, pos.y + 9.f * u), ImVec2(pos.x + 4.f, pos.y + size.y - 9.f * u), theme::u32(pal.accent), 2.f);
			const ImU32 col = theme::u32(selected ? pal.accent : pal.textDim);
			icons::draw(dl, item.icon, ImVec2(pos.x + 24.f * u, pos.y + size.y * 0.5f), 18.f * u, col);
			const float tx = pos.x + 48.f * u;
			const float textTop = captions ? pos.y + (size.y - fs - cs - 2.f * u) * 0.5f : pos.y + (size.y - fs) * 0.5f;
			dl->AddText(ImVec2(tx, textTop), theme::u32(selected ? pal.text : theme::mix(pal.text, pal.textDim, 0.3f)), item.label);
			if (captions)
				dl->AddText(theme::fontAt(cs), cs, ImVec2(tx, textTop + fs + 2.f * u), theme::u32(pal.textDim, selected ? 0.9f : 0.7f),
					fitText(item.caption, size.x - (tx - pos.x) - 8.f * u).c_str());
			// Something waiting there: a count (friend requests, shots shared with you) or a dot (today's daily).
			const int count = item.hub == Hub::Friends ? friendRequests_ : item.hub == Hub::Create ? sharedWaiting_ : 0;
			const float bx = tx + ImGui::CalcTextSize(item.label).x + 8.f * u, by = textTop + fs * 0.5f;
			if (count > 0)
			{
				const std::string n = std::to_string(count);
				const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
				dl->AddRectFilled(ImVec2(bx, by - ts.y * 0.5f - 2.f), ImVec2(bx + ts.x + 12.f, by + ts.y * 0.5f + 2.f), theme::u32(pal.accent), 20.f);
				dl->AddText(ImVec2(bx + 6.f, by - ts.y * 0.5f), IM_COL32(10, 14, 20, 255), n.c_str());
			}
			else if (item.hub == Hub::Compete && dailyWaiting_)
				dl->AddCircleFilled(ImVec2(bx + 4.f * u, by), 4.f * u, theme::u32(pal.accent), 12);
		}
	}
	ImGui::PopStyleVar();
	ImGui::EndChild();
}

void Dashboard::drawHubTabs()
{
	// The place's pages as tabs along the top (an underline under the open one), and the ? button that brings back
	// the place's tips. One-page places only get the ? button.
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const Hub hub = hubOf(page_);
	const auto tabs = hubTabs(hub);
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	const float h = 38.f * u;
	// The ? button, top right.
	{
		const float r = 15.f * u;
		const ImVec2 c(top.x + width - r - 2.f, top.y + h * 0.5f);
		ImGui::SetCursorScreenPos(ImVec2(c.x - r, c.y - r));
		const bool clicked = navInvisible("##hub_help", ImVec2(2.f * r, 2.f * r), 1, Fx::Click);
		const bool hovered = ImGui::IsItemHovered();
		dl->AddCircleFilled(c, r, theme::u32(hovered ? theme::mix(pal.card, pal.accent, 0.35f) : pal.card), 24);
		dl->AddCircle(c, r, theme::u32(pal.accent, hovered ? 1.f : 0.5f), 24, 1.5f);
		const ImVec2 qs = ImGui::CalcTextSize("?");
		dl->AddText(ImVec2(c.x - qs.x * 0.5f, c.y - qs.y * 0.5f), theme::u32(pal.text), "?");
		if (hovered)
			ImGui::SetTooltip("How this works");
		if (clicked)
			openTutorial(hubKey(hub));
	}
	if (tabs.size() > 1)
	{
		float x = top.x;
		const float fs = ImGui::GetFontSize() * 1.05f;
		for (const auto& t : tabs)
		{
			const ImVec2 ts = theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, 0.f, t.label);
			const float w = ts.x + 28.f * u;
			ImGui::SetCursorScreenPos(ImVec2(x, top.y));
			ImGui::PushID(t.label);
			const bool clicked = navInvisible("##hub_tab", ImVec2(w, h), 1, Fx::Click);
			const bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();
			const bool sel = page_ == t.page;
			if (hovered && !sel)
				dl->AddRectFilled(ImVec2(x, top.y + 2.f), ImVec2(x + w, top.y + h - 2.f), theme::u32(pal.cardHover), 8.f);
			dl->AddText(theme::fontAt(fs), fs, ImVec2(x + 14.f * u, top.y + (h - ts.y) * 0.5f - 1.f),
				theme::u32(sel ? pal.text : pal.textDim), t.label);
			if (sel)
				dl->AddRectFilled(ImVec2(x + 10.f * u, top.y + h - 3.f * u), ImVec2(x + w - 10.f * u, top.y + h), theme::u32(pal.accent), 2.f);
			if (clicked)
			{
				// Another tab, or this one again from a drill's own page: back to its list.
				detailId_.clear();
				if (!sel)
				{
					page_ = t.page;
					pageAt_ = ImGui::GetTime();
				}
			}
			x += w + 4.f * u;
		}
		dl->AddLine(ImVec2(top.x, top.y + h), ImVec2(top.x + width - 40.f * u, top.y + h), theme::u32(pal.textDim, 0.15f), 1.f);
		ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + h + 14.f * u));
	}
	else
		ImGui::SetCursorScreenPos(top); // the title goes on the ? button's row
}

namespace
{
	const ExerciseCard* findCard(const DashboardModel& m, const std::string& id)
	{
		const auto it = std::find_if(m.exercises.begin(), m.exercises.end(), [&](const ExerciseCard& c) { return c.id == id; });
		return it == m.exercises.end() ? nullptr : &*it;
	}

	// A big rounded button drawn by hand (primary: accent fill and dark text; else a dark glass one), with an optional
	// icon, a hover glow and a press dip. `fx`: its sound.
	bool bigButton(const char* id, const char* label, ImVec2 size, bool primary, Icon icon, bool withIcon, Fx fx)
	{
		const theme::Palette& pal = theme::palette();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float fs = ImGui::GetFontSize() * 1.1f;
		const ImVec2 ts = theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
		const float iu = uiScale();
		const float iconW = withIcon ? 22.f * iu : 0.f;
		// Never smaller than its label: sizes were picked at the default GUI scale, and a bigger scale grows the text.
		size.x = std::max(size.x, ts.x + iconW + 22.f * iu);
		size.y = std::max(size.y, fs * 1.6f);
		const bool clicked = navInvisible(id, size, 1, fx);
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const float dip = held ? 1.f : 0.f;
		const ImVec2 a(pos.x + dip, pos.y + dip), b(pos.x + size.x - dip, pos.y + size.y - dip);
		if (primary)
		{
			if (hovered)
				dl->AddRectFilled(ImVec2(a.x - 4, a.y - 4), ImVec2(b.x + 4, b.y + 4), theme::u32(pal.accent, 0.22f), 14.f);
			const ImVec4 top = theme::mix(pal.accent, ImVec4(1, 1, 1, 1), hovered ? 0.28f : 0.14f);
			const ImVec4 bottom = theme::mix(pal.accent, ImVec4(0, 0, 0, 1), 0.12f);
			dl->AddRectFilled(a, b, theme::u32(bottom), 11.f);
			roundedGradient(dl, ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, (a.y + b.y) * 0.5f), theme::u32(top), theme::u32(top),
				theme::u32(bottom), theme::u32(bottom), 9.f, ImDrawCornerFlags_Top);
			dl->AddRect(a, b, IM_COL32(255, 255, 255, hovered ? 90 : 50), 11.f, ImDrawCornerFlags_All, 1.f);
		}
		else
		{
			dl->AddRectFilled(a, b, IM_COL32(8, 11, 20, hovered ? 220 : 175), 11.f);
			dl->AddRect(a, b, hovered ? theme::u32(pal.accent, 0.9f) : IM_COL32(255, 255, 255, 60), 11.f,
				ImDrawCornerFlags_All, 1.2f);
		}
		const ImU32 ink = primary ? IM_COL32(8, 12, 18, 255) : theme::u32(pal.text);
		const float x = (a.x + b.x - ts.x - iconW) * 0.5f;
		if (withIcon)
			icons::draw(dl, icon, ImVec2(x + 7.f * iu, (a.y + b.y) * 0.5f), 15.f * iu, ink);
		dl->AddText(theme::fontAt(fs), fs, ImVec2(x + iconW, (a.y + b.y - ts.y) * 0.5f), ink, label);
		return clicked;
	}

	// Big number with a small caps label under it, left-aligned at `at`.
	void statText(ImDrawList* dl, ImVec2 at, const char* value, const char* label, ImU32 valueColor)
	{
		const theme::Palette& pal = theme::palette();
		ImFont* font = ImGui::GetFont();
		const float big = ImGui::GetFontSize() * 2.0f;
		dl->AddText(theme::fontAt(big), big, at, valueColor, value);
		dl->AddText(theme::fontAt(ImGui::GetFontSize() * 0.78f), ImGui::GetFontSize() * 0.78f, ImVec2(at.x + 1, at.y + big + 2), theme::u32(pal.textDim), label);
	}

	// The card behind a Today cell (`hover` 0..1 tints it toward the accent).
	void cellCard(ImDrawList* dl, ImVec2 a, ImVec2 b, float hover)
	{
		const theme::Palette& pal = theme::palette();
		dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.04f + 0.08f * hover)), 12.f);
		dl->AddRect(a, b, theme::u32(theme::mix(ImVec4(1, 1, 1, 0.06f), pal.accent, hover)), 12.f, ImDrawCornerFlags_All,
			1.f + hover);
	}
}

void Dashboard::renderHome(const DashboardModel& m, const DashboardStatus& s)
{
	if (s.firstRun)
		renderFirstRun();
	if (s.lastSummary)
		renderSummary(*s.lastSummary);
	renderHero(m, s);
	if (s.daily && s.daily->available)
		renderDailyCard(*s.daily);
	renderToday(m, s);
	renderHomeTiles(m);
	renderRecent(m, s);
}

void Dashboard::renderHero(const DashboardModel& m, const DashboardStatus& s)
{
	// The big banner: what is running, else what to continue (the last exercise), else Freeplay+.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float now = static_cast<float>(ImGui::GetTime());

	std::string id;
	std::string kicker;
	if (s.exerciseActive)
	{
		id = s.exerciseId;
		kicker = s.sessionActive ? "SESSION RUNNING" : "NOW PLAYING";
	}
	else if (!s.lastExerciseId.empty() || !s.home.recent.empty())
	{
		id = !s.lastExerciseId.empty() ? s.lastExerciseId : s.home.recent.front().id;
		kicker = "CONTINUE";
	}
	else
	{
		id = m.freeplayId;
		kicker = "START HERE";
	}
	const ExerciseCard* card = findCard(m, id);
	const std::string name = s.exerciseActive ? s.exerciseName : card ? card->name : std::string("Freeplay+");
	const TileName parts = splitName(name);
	if (!parts.kicker.empty())
		kicker += "  /  " + parts.kicker;

	std::string sub;
	char buf[160];
	if (s.exerciseActive)
	{
		snprintf(buf, sizeof(buf), "Rep %d  |  %s", s.rep, s.difficulty.c_str());
		sub = buf;
		if (s.sessionActive)
		{
			snprintf(buf, sizeof(buf), "  |  %s %d:%02d left, exercise %d of %d", s.sessionName.c_str(),
				static_cast<int>(s.sessionSecondsLeft) / 60, static_cast<int>(s.sessionSecondsLeft) % 60,
				s.sessionExerciseIndex, s.sessionExerciseCount);
			sub += buf;
		}
	}
	else
	{
		const auto rec = std::find_if(s.home.recent.begin(), s.home.recent.end(), [&](const HomeRecent& r) { return r.id == id; });
		if (rec != s.home.recent.end() && rec->reps > 0)
		{
			snprintf(buf, sizeof(buf), "%d %s  |  %d%% success  |  last: %s", rec->reps, rec->reps == 1 ? "rep" : "reps",
				static_cast<int>(100.f * rec->successes / rec->reps + 0.5f), rec->lastLabel.empty() ? "-" : rec->lastLabel.c_str());
			sub = buf;
		}
		else if (card)
		{
			sub = card->description;
		}
	}

	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float hu = uiScale(); // taller with BakkesMod's GUI scale, so the bigger text and buttons still fit
	const ImVec2 size(width, std::clamp(width * 0.25f, 196.f * hu, 260.f * hu));
	const ImVec2 a = pos, b = add(pos, size);

	// Art with a slow push-in, darkened on the left for the text.
	const ImageInfo art = card && card->image >= 0 && cb_.image ? cb_.image(card->image) : ImageInfo{};
	const float drift = 0.04f + 0.035f * (0.5f + 0.5f * std::sin(now * 0.22f));
	if (!drawCover(dl, art, a, b, 14.f, IM_COL32(235, 235, 235, 255), drift))
	{
		dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.22f)), 14.f);
		icons::draw(dl, Icon::Play, ImVec2(b.x - size.y * 0.55f, a.y + size.y * 0.5f), size.y * 0.5f, theme::u32(pal.accent, 0.25f));
	}
	const ImU32 shade = IM_COL32(5, 8, 15, 235), clear = IM_COL32(5, 8, 15, 0);
	roundedGradient(dl, a, ImVec2(a.x + width * 0.72f, b.y), shade, clear, clear, shade, 14.f, ImDrawCornerFlags_Left);
	roundedGradient(dl, ImVec2(a.x, b.y - size.y * 0.35f), b, clear, clear, IM_COL32(5, 8, 15, 150), IM_COL32(5, 8, 15, 150), 14.f,
		ImDrawCornerFlags_Bot);
	dl->AddRect(a, b, theme::u32(pal.accent, 0.3f), 14.f, ImDrawCornerFlags_All, 1.2f);
	// A light sweep across the banner every few seconds.
	const float sweep = std::fmod(now, 7.f) / 1.4f;
	if (sweep < 1.f)
	{
		const float x = a.x + (width + 300.f) * sweep - 150.f;
		const ImU32 c0 = IM_COL32(255, 255, 255, 0), c1 = IM_COL32(255, 255, 255, 18);
		dl->PushClipRect(a, b, true);
		dl->AddRectFilledMultiColor(ImVec2(x - 120.f, a.y), ImVec2(x, b.y), c0, c1, c1, c0);
		dl->AddRectFilledMultiColor(ImVec2(x, a.y), ImVec2(x + 120.f, b.y), c1, c0, c0, c1);
		dl->PopClipRect();
	}

	// Text: kicker, big title, one line under it.
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const float textX = a.x + 30.f * hu;
	const float wrap = width * 0.58f;
	float y = a.y + 26.f * hu;
	if (s.exerciseActive)
	{
		const float dot = 0.5f + 0.5f * std::sin(now * 4.f);
		dl->AddCircleFilled(ImVec2(textX + 5.f, y + fs * 0.42f), 5.f, theme::u32(pal.accent, 0.5f + 0.5f * dot), 16);
		shadowedText(dl, font, fs * 0.9f, ImVec2(textX + 16.f, y), theme::u32(pal.accent), kicker.c_str());
	}
	else
	{
		shadowedText(dl, font, fs * 0.9f, ImVec2(textX, y), theme::u32(pal.accent), kicker.c_str());
	}
	y += fs * 1.35f;
	const float titleSize = fs * 2.15f;
	const std::string title = fitText(parts.title, wrap / 2.15f); // measured at the normal size, drawn larger
	shadowedText(dl, font, titleSize, ImVec2(textX, y), IM_COL32(250, 252, 255, 255), title.c_str());
	y += titleSize + 4.f;
	if (!sub.empty())
	{
		const std::string line = fitText(sub, wrap);
		shadowedText(dl, font, fs, ImVec2(textX, y), IM_COL32(205, 214, 228, 235), line.c_str());
	}
	if (!s.inFreeplay && !s.exerciseActive)
	{
		const char* note = s.startPending ? "Loading Free Play..." : "Play loads Free Play";
		const ImVec2 ts = ImGui::CalcTextSize(note);
		darkPill(dl, ImVec2(b.x - ts.x - 34.f, a.y + 16.f), note, theme::u32(pal.textDim));
	}

	// Buttons along the bottom (sized with the text, so BakkesMod's GUI scale keeps them in proportion).
	const float u = uiScale();
	const float btnH = 46.f * u;
	ImGui::SetCursorScreenPos(ImVec2(textX, b.y - 26.f * u - btnH));
	if (s.exerciseActive)
	{
		if (bigButton("hero_stop", s.sessionActive ? "End session" : "Stop", ImVec2(170.f * u, btnH), false, Icon::Stop, true, Fx::Click)
			&& cb_.stop)
			cb_.stop();
	}
	else
	{
		if (bigButton("hero_play", "Play", ImVec2(150.f * u, btnH), true, Icon::Play, true, Fx::Confirm) && cb_.play && !id.empty())
		{
			cb_.play(id, kDifficulties[difficulty_]);
			closeRequested_ = true;
		}
		ImGui::SameLine(0.f, 10.f * u);
		if (bigButton("hero_warmup", "Warm-up", ImVec2(150.f * u, btnH), false, Icon::QuickPlay, true, Fx::Confirm) && cb_.startQuickPlay)
		{
			cb_.startQuickPlay();
			closeRequested_ = true;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Starts your warm-up's Free Touch drills (edit them on the Routine page).");
		// Difficulty for Play, right of the buttons, centred on them.
		ImGui::SameLine(0.f, 18.f * u);
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (btnH - ImGui::GetFrameHeight()) * 0.5f);
		for (int d = 0; d < 3; ++d)
		{
			if (d > 0)
				ImGui::SameLine(0.f, 4.f);
			ImGui::PushID(300 + d);
			if (chip(kDifficultyLabels[d], difficulty_ == d))
				difficulty_ = d;
			ImGui::PopID();
		}
	}
	ImGui::SetCursorScreenPos(ImVec2(pos.x, b.y + 16.f));
}

void Dashboard::renderToday(const DashboardModel& m, const DashboardStatus& s)
{
	// Four cells: minutes today against the goal, the day streak, reps today, and a suggestion.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const double now = ImGui::GetTime();
	const HomeStats& h = s.home;
	const float count = easeOut(static_cast<float>((now - homeAt_) / 0.9)); // numbers count up on entering Home
	const float width = ImGui::GetContentRegionAvail().x;
	const float u = uiScale();
	const float gap = 12.f * u;
	const int perRow = width >= 780.f * u ? 4 : 2;
	const float cellW = (width - (perRow - 1) * gap) / perRow;
	const float cellH = 96.f * u;
	const ImVec2 row = ImGui::GetCursorScreenPos();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	char buf[96];

	for (int i = 0; i < 4; ++i)
	{
		const float rise = easeOut(static_cast<float>((now - pageAt_ - 0.06 * i) / 0.35));
		const ImVec2 slot(row.x + (i % perRow) * (cellW + gap), row.y + (i / perRow) * (cellH + gap));
		ImVec2 a(slot.x, slot.y + 12.f * u * (1.f - rise));
		ImVec2 b(a.x + cellW, a.y + cellH);
		float hover = 0.f;
		if (i == 3)
		{
			// Try next is a button.
			ImGui::SetCursorScreenPos(slot);
			const ExerciseCard* next = findCard(m, h.nextId);
			if (navInvisible("try_next", ImVec2(cellW, cellH), 1, Fx::Confirm) && next && cb_.play)
			{
				cb_.play(next->id, kDifficulties[difficulty_]);
				closeRequested_ = true;
			}
			float& hv = hoverAnim_["home.try_next"];
			hv = approach(hv, ImGui::IsItemHovered() ? 1.f : 0.f, 7.f);
			hover = hv;
			a.y -= 3.f * hover;
			b.y -= 3.f * hover;
			cellCard(dl, a, b, hover);
			dl->AddText(theme::fontAt(fs * 0.78f), fs * 0.78f, ImVec2(a.x + 16.f * u, a.y + 14.f * u), theme::u32(pal.accent), "TRY NEXT");
			if (next)
			{
				const std::string n = fitText(next->name, cellW - 70.f * u);
				dl->AddText(theme::fontAt(fs * 1.05f), fs * 1.05f, ImVec2(a.x + 16.f * u, a.y + 36.f * u), theme::u32(pal.text), n.c_str());
				const std::string cat = categoryLabel(h.nextCategory);
				const std::string reason = h.nextDaysAgo < 0 ? "New: you haven't tried " + cat
					: h.nextDaysAgo >= 2 ? cat + ": not trained in " + std::to_string(h.nextDaysAgo) + " days"
					: h.nextDaysAgo == 1 ? cat + ": last trained yesterday" : cat + ": keep it fresh";
				const std::string why = fitText(reason, cellW - 70.f * u);
				dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(a.x + 16.f * u, a.y + 62.f * u), theme::u32(pal.textDim), why.c_str());
				const ImVec2 c(b.x - 30.f * u, (a.y + b.y) * 0.5f);
				dl->AddCircleFilled(c, (15.f + 2.f * hover) * u, theme::u32(pal.accent, 0.25f + 0.75f * hover), 24);
				icons::draw(dl, Icon::Play, ImVec2(c.x + 1.5f * u, c.y), 13.f * u, hover > 0.5f ? IM_COL32(8, 12, 18, 255) : theme::u32(pal.accent));
			}
			else
			{
				dl->AddText(theme::fontAt(fs), fs, ImVec2(a.x + 16.f * u, a.y + 40.f * u), theme::u32(pal.textDim), "Play anything to get going");
			}
		}
		else
		{
			cellCard(dl, a, b, 0.f);
		}

		if (i == 0)
		{
			// Ring: minutes today against the daily goal.
			const float goal = static_cast<float>(std::max(1.0, h.goalMinutes));
			const float mins = static_cast<float>(h.todayMinutes) * count;
			const float frac = std::clamp(mins / goal, 0.f, 1.f);
			const ImVec2 c(a.x + 46.f * u, (a.y + b.y) * 0.5f);
			const float r = 28.f * u;
			dl->AddCircle(c, r, theme::u32(pal.textDim, 0.25f), 48, 6.f * u);
			if (frac > 0.001f)
			{
				const float a0 = -1.5707963f;
				dl->PathArcTo(c, r, a0, a0 + 6.2831853f * frac, 48);
				dl->PathStroke(theme::u32(pal.accent), false, 6.f * u);
			}
			snprintf(buf, sizeof(buf), "%d", static_cast<int>(mins));
			const ImVec2 ts = theme::fontAt(fs * 1.3f)->CalcTextSizeA(fs * 1.3f, FLT_MAX, 0.f, buf);
			dl->AddText(theme::fontAt(fs * 1.3f), fs * 1.3f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), theme::u32(pal.text), buf);
			dl->AddText(theme::fontAt(fs * 0.78f), fs * 0.78f, ImVec2(a.x + 88.f * u, a.y + 28.f * u), theme::u32(pal.textDim), "MIN TODAY");
			const bool done = h.todayMinutes >= h.goalMinutes;
			snprintf(buf, sizeof(buf), done ? "Goal done!" : "Goal %d", static_cast<int>(h.goalMinutes));
			dl->AddText(theme::fontAt(fs), fs, ImVec2(a.x + 88.f * u, a.y + 48.f * u), theme::u32(done ? pal.accent : pal.text), buf);
		}
		else if (i == 1)
		{
			// Streak, and the last 7 days as dots (today last, ringed).
			snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(h.streakDays * count)));
			statText(dl, ImVec2(a.x + 18.f * u, a.y + 12.f * u), buf, "DAY STREAK",
				theme::u32(h.streakDays > 0 ? pal.orange : pal.text));
			for (int d = 0; d < 7; ++d)
			{
				const ImVec2 c(b.x - (18.f + (6 - d) * 15.f) * u, b.y - 22.f * u);
				if (h.week[d])
					dl->AddCircleFilled(c, 5.f * u, theme::u32(pal.orange), 12);
				else
					dl->AddCircle(c, 5.f * u, theme::u32(pal.textDim, 0.5f), 12, 1.2f);
				if (d == 6)
					dl->AddCircle(c, 8.f * u, theme::u32(pal.text, 0.5f), 16, 1.f);
			}
		}
		else if (i == 2)
		{
			// Reps today and how many were successes.
			snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(h.repsToday * count)));
			statText(dl, ImVec2(a.x + 18.f * u, a.y + 12.f * u), buf, "REPS TODAY", theme::u32(pal.text));
			const float pct = h.repsToday > 0 ? static_cast<float>(h.successesToday) / h.repsToday : 0.f;
			const float barX0 = a.x + 18.f * u, barX1 = b.x - 18.f * u;
			const float barY = b.y - 16.f * u;
			dl->AddRectFilled(ImVec2(barX0, barY), ImVec2(barX1, barY + 6.f * u), theme::u32(pal.textDim, 0.2f), 3.f);
			if (pct > 0.f)
				dl->AddRectFilled(ImVec2(barX0, barY), ImVec2(barX0 + (barX1 - barX0) * pct * count, barY + 6.f * u),
					theme::u32(pal.accent), 3.f);
			snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(pct * 100.f * count + 0.5f));
			const ImVec2 ps = theme::fontAt(fs * 1.1f)->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0.f, buf);
			dl->AddText(theme::fontAt(fs * 1.1f), fs * 1.1f, ImVec2(b.x - 18.f * u - ps.x, a.y + 16.f * u), theme::u32(pal.accent), buf);
			dl->AddText(theme::fontAt(fs * 0.72f), fs * 0.72f, ImVec2(b.x - 18.f * u - theme::fontAt(fs * 0.72f)->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0.f, "SUCCESS").x,
				a.y + 18.f * u + ps.y), theme::u32(pal.textDim), "SUCCESS");
		}
		// Entrance fade.
		if (rise < 1.f)
		{
			const ImVec4 bg = pal.bg;
			dl->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), theme::u32(ImVec4(bg.x, bg.y, bg.z, 1.f), 1.f - rise), 12.f);
		}
	}
	ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + (4 / perRow) * (cellH + gap) - gap + 18.f * u));
}

void Dashboard::renderHomeTiles(const DashboardModel& m)
{
	// The four ways in, as art tiles like Rocket League's playlists.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const double now = ImGui::GetTime();
	struct Tile { Page page; Icon icon; const char* name; const char* key; };
	static const Tile tiles[] = {
		{Page::Exercises, Icon::Exercises, "Every drill: Exercises", "home.exercises"},
		{Page::Sessions, Icon::Sessions, "Timed playlists: Sessions & Labs", "home.sessions"},
		{Page::QuickPlay, Icon::QuickPlay, "Your day: Routine", "home.quick_play"},
		{Page::Settings, Icon::Settings, "Controls and look: Settings", "home.settings"},
	};
	const float width = ImGui::GetContentRegionAvail().x;
	const float gap = 14.f;
	const int perRow = (width - 3 * gap) / 4.f >= 170.f ? 4 : 2;
	const float tileW = (width - (perRow - 1) * gap) / perRow;
	const float tileH = std::clamp(tileW * 0.5f, 110.f, 180.f);
	const ImVec2 row = ImGui::GetCursorScreenPos();
	for (int i = 0; i < 4; ++i)
	{
		const ImVec2 pos(row.x + (i % perRow) * (tileW + gap), row.y + (i / perRow) * (tileH + gap));
		ImGui::SetCursorScreenPos(pos);
		ImGui::PushID(i);
		if (navInvisible("tile", ImVec2(tileW, tileH), 1, Fx::Silent))
			page_ = tiles[i].page;
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		float& h = hoverAnim_[tiles[i].key];
		h = approach(h, hovered ? 1.f : 0.f, 7.f);
		const float rise = easeOut(static_cast<float>((now - pageAt_ - 0.15 - 0.06 * i) / 0.35));
		const float lift = 4.f * h + 14.f * (1.f - rise);
		const ImVec2 a(pos.x, pos.y - lift), b(pos.x + tileW, pos.y + tileH - lift);
		const int light = static_cast<int>(215.f + 40.f * h);
		const ImageInfo art = m.homeImages[i] >= 0 && cb_.image ? cb_.image(m.homeImages[i]) : ImageInfo{};
		if (!drawCover(dl, art, a, b, 10.f, IM_COL32(light, light, light, 255), 0.07f * h))
		{
			dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.12f + 0.1f * h)), 10.f);
			icons::draw(dl, tiles[i].icon, ImVec2(b.x - 50.f, a.y + tileH * 0.4f), 48.f, theme::u32(pal.accent, 0.4f));
		}
		tileFade(dl, a, b, 0.6f);
		tileTitle(dl, a, b, tiles[i].name, 1.25f);
		tileFrame(dl, a, b, h, false);
		if (rise < 1.f)
		{
			const ImVec4 bg = pal.bg;
			dl->AddRectFilled(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), theme::u32(ImVec4(bg.x, bg.y, bg.z, 1.f), 1.f - rise), 12.f);
		}
	}
	ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + (4 / perRow) * (tileH + gap) - gap + 22.f));
}

void Dashboard::renderRecent(const DashboardModel& m, const DashboardStatus& s)
{
	// Jump back in: the last exercises played, with how they went.
	if (s.home.recent.empty())
	{
		ImGui::TextDisabled("%d exercises and %d sessions ready.", static_cast<int>(m.exercises.size()),
			static_cast<int>(m.sessions.size()));
		return;
	}
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const double now = ImGui::GetTime();
	ImGui::PushStyleColor(ImGuiCol_Text, pal.textDim);
	ImGui::TextUnformatted("JUMP BACK IN");
	ImGui::PopStyleColor();
	ImGui::Dummy(ImVec2(0, 2));
	const float width = ImGui::GetContentRegionAvail().x;
	const float gap = 14.f;
	const int perRow = (width - 3 * gap) / 4.f >= 170.f ? 4 : 2;
	const float cardW = (width - (perRow - 1) * gap) / perRow;
	const float cardH = std::clamp(cardW * 0.5f, 100.f, 150.f);
	const ImVec2 row = ImGui::GetCursorScreenPos();
	int shown = 0;
	for (const auto& r : s.home.recent)
	{
		const ExerciseCard* card = findCard(m, r.id);
		if (!card || shown >= 4)
			continue;
		const ImVec2 pos(row.x + (shown % perRow) * (cardW + gap), row.y + (shown / perRow) * (cardH + gap));
		ImGui::SetCursorScreenPos(pos);
		ImGui::PushID(r.id.c_str());
		if (navInvisible("recent", ImVec2(cardW, cardH), 1, Fx::Confirm) && cb_.play)
		{
			cb_.play(r.id, kDifficulties[difficulty_]);
			closeRequested_ = true;
		}
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		float& h = hoverAnim_["recent." + r.id];
		h = approach(h, hovered ? 1.f : 0.f, 7.f);
		const float rise = easeOut(static_cast<float>((now - pageAt_ - 0.3 - 0.06 * shown) / 0.35));
		const float lift = 3.f * h + 12.f * (1.f - rise);
		const ImVec2 a(pos.x, pos.y - lift), b(pos.x + cardW, pos.y + cardH - lift);
		const int light = static_cast<int>(200.f + 50.f * h);
		const ImageInfo art = card->image >= 0 && cb_.image ? cb_.image(card->image) : ImageInfo{};
		if (!drawCover(dl, art, a, b, 10.f, IM_COL32(light, light, light, 255), 0.06f * h))
			drawPreview(dl, a, b, card->preview);
		tileFade(dl, a, b, 0.7f);
		tileTitle(dl, a, b, card->name, 1.0f);
		if (r.reps > 0)
		{
			// Pills stay inside the card, clear of the play button: the last result only when it fits.
			const float maxRight = b.x - 46.f;
			char buf[64];
			const int pct = static_cast<int>(100.f * r.successes / r.reps + 0.5f);
			snprintf(buf, sizeof(buf), "%d %s  |  %d%%", r.reps, r.reps == 1 ? "rep" : "reps", pct);
			if (a.x + 8.f + ImGui::CalcTextSize(buf).x + 16.f > maxRight)
				snprintf(buf, sizeof(buf), "%d%%", pct);
			const float pw = darkPill(dl, ImVec2(a.x + 8.f, a.y + 8.f), buf);
			if (!r.lastLabel.empty() && a.x + 14.f + pw + ImGui::CalcTextSize(r.lastLabel.c_str()).x + 16.f <= maxRight)
				darkPill(dl, ImVec2(a.x + 14.f + pw, a.y + 8.f), r.lastLabel,
					r.lastOutcome > 0 ? theme::u32(pal.accent) : r.lastOutcome < 0 ? IM_COL32(255, 120, 120, 255) : theme::u32(pal.textDim));
		}
		if (h > 0.01f)
		{
			const ImVec2 c(b.x - 26.f, a.y + 26.f);
			dl->AddCircleFilled(c, 13.f + 3.f * h, theme::u32(pal.accent, 0.95f * h), 24);
			icons::draw(dl, Icon::Play, ImVec2(c.x + 1.5f, c.y), 12.f * h, IM_COL32(10, 14, 18, static_cast<int>(255 * h)));
		}
		tileFrame(dl, a, b, h, false);
		if (rise < 1.f)
		{
			const ImVec4 bg = pal.bg;
			dl->AddRectFilled(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), theme::u32(ImVec4(bg.x, bg.y, bg.z, 1.f), 1.f - rise), 12.f);
		}
		++shown;
	}
	const int rowsShown = (shown + perRow - 1) / perRow;
	ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + rowsShown * (cardH + gap) - gap + 12.f));
	ImGui::Dummy(ImVec2(0, 4));
}

void Dashboard::renderSessions(const DashboardModel& m, const DashboardStatus& st)
{
	const theme::Palette& pal = theme::palette();
	pageTitle("Sessions & Labs", "Timed playlists. Pick one, set time, difficulty and air roll, then Start.");
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float width = ImGui::GetContentRegionAvail().x;

	if (st.sessionActive)
	{
		ImGui::TextColored(pal.accent, "%s is running (%d:%02d left).", st.sessionName.c_str(),
			static_cast<int>(st.sessionSecondsLeft) / 60, static_cast<int>(st.sessionSecondsLeft) % 60);
		ImGui::SameLine();
		if (navButton("End session") && cb_.stop)
			cb_.stop();
		ImGui::Dummy(ImVec2(0, 6));
	}

	// Setup panel for the selected session.
	const auto sel = std::find_if(m.sessions.begin(), m.sessions.end(),
		[&](const SessionCard& c) { return c.id == selectedSession_; });
	if (sel != m.sessions.end())
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const bool narrow = width < 760.f;
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 22, pos.y + 16));
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(pos.x + width - (narrow ? 22.f : 220.f) - ImGui::GetWindowPos().x); // window-relative
		theme::fontScale(1.25f);
		ImGui::TextUnformatted(sel->name.c_str());
		theme::fontScale(1.f);
		ImGui::PushItemWidth(260.f);
		ImGui::SliderFloat("Minutes", &sessionMinutes_, 1.f, 60.f, "%.0f min");
		ImGui::PopItemWidth();
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Difficulty");
		for (int d = 0; d < 3; ++d)
		{
			ImGui::SameLine(0.f, 6.f);
			ImGui::PushID(100 + d);
			if (chip(kDifficultyLabels[d], sessionDifficulty_ == d))
				sessionDifficulty_ = d;
			ImGui::PopID();
		}
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Air roll  ");
		static const char* kAirRoll[] = {"Left", "Right", "Free"};
		for (int a = 0; a < 3; ++a)
		{
			ImGui::SameLine(0.f, 6.f);
			ImGui::PushID(200 + a);
			if (chip(kAirRoll[a], sessionAirRoll_ == a))
				sessionAirRoll_ = a;
			ImGui::PopID();
		}
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		const float groupBottom = ImGui::GetItemRectMax().y;

		ImGui::SetCursorScreenPos(narrow ? ImVec2(pos.x + 22, groupBottom + 12.f) : ImVec2(pos.x + width - 200, pos.y + 22));
		if (bigButton("start_session", "Start session", ImVec2(178, 44), true, Icon::Play, true, Fx::Confirm) && cb_.startSession)
		{
			SessionSettings settings;
			settings.sessionId = sel->id;
			settings.minutes = std::round(sessionMinutes_);
			settings.difficulty = kDifficulties[sessionDifficulty_];
			static const char* kAirRollIds[] = {"left", "right", "free"};
			settings.airRoll = kAirRollIds[sessionAirRoll_];
			cb_.startSession(settings);
			closeRequested_ = true;
		}
		const ImVec2 size(width, std::max(groupBottom, ImGui::GetItemRectMax().y) - pos.y + 16.f);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(theme::mix(pal.card, pal.accent, 0.10f)), 12.f);
		dl->AddRect(pos, add(pos, size), theme::u32(pal.accent, 0.6f), 12.f, ImDrawCornerFlags_All, 1.5f);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 16));
	}

	const int perRow = width >= 640.f ? 2 : 1;
	const float cardW = (width - 16.f * (perRow - 1)) / perRow;
	int i = 0;
	for (const auto& s : m.sessions)
	{
		if (i % perRow != 0)
			ImGui::SameLine(0.f, 16.f);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(cardW, std::clamp(cardW * 0.42f, 150.f, 220.f));
		ImGui::PushID(s.id.c_str());
		if (navInvisible("session", size))
		{
			selectedSession_ = s.id;
			sessionMinutes_ = static_cast<float>(s.durationMinutes);
			sessionDifficulty_ = s.difficulty == "easy" ? 0 : s.difficulty == "hard" ? 2 : 1;
			sessionAirRoll_ = 2;
		}
		const bool hovered = ImGui::IsItemHovered();
		ImGui::PopID();
		const bool selected = s.id == selectedSession_;
		float& h = hoverAnim_[s.id];
		h = approach(h, hovered ? 1.f : 0.f, 7.f);
		const ImVec2 a(pos.x, pos.y - 3.f * h), b(pos.x + size.x, pos.y + size.y - 3.f * h);
		// Art fills the tile; without art, an accent-tinted tile with a large faint icon.
		const int light = static_cast<int>(215.f + 40.f * h);
		const ImageInfo art = s.image >= 0 && cb_.image ? cb_.image(s.image) : ImageInfo{};
		if (!drawCover(dl, art, a, b, 10.f, IM_COL32(light, light, light, 255), 0.05f * h))
		{
			dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.18f + 0.1f * h)), 10.f);
			icons::draw(dl, Icon::Sessions, ImVec2(b.x - 70.f, a.y + size.y * 0.45f), 64.f, theme::u32(pal.accent, 0.35f));
		}
		tileFade(dl, a, b, 0.6f);
		tileTitle(dl, a, b, s.name, 1.35f);
		char meta[96];
		snprintf(meta, sizeof(meta), "%.0f min  |  %s", s.durationMinutes, s.difficulty.c_str());
		darkPill(dl, ImVec2(a.x + 10.f, a.y + 10.f), meta);
		if (selected)
		{
			const ImVec2 ts = ImGui::CalcTextSize("Selected");
			darkPill(dl, ImVec2(b.x - 10.f - ts.x - 16.f, a.y + 10.f), "Selected", theme::u32(pal.accent));
		}
		tileFrame(dl, a, b, h, selected);
		if (hovered && !s.description.empty())
		{
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(340.f);
			ImGui::TextUnformatted(s.description.c_str());
			ImGui::TextDisabled("%d exercise group%s. Click to set it up.", s.exerciseRefs, s.exerciseRefs == 1 ? "" : "s");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		++i;
	}
	if (m.sessions.empty())
		ImGui::TextDisabled("No sessions loaded.");
}

void Dashboard::renderSummary(const SessionSummary& sum)
{
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float width = ImGui::GetContentRegionAvail().x;
	const float rowH = ImGui::GetTextLineHeightWithSpacing() + 6.f;
	const ImVec2 pos = ImGui::GetCursorScreenPos();

	// The card is drawn behind the content once its height is known (channel 0), so the text always fits.
	dl->ChannelsSplit(2);
	dl->ChannelsSetCurrent(1);
	ImGui::SetCursorScreenPos(ImVec2(pos.x + 22, pos.y + 16));
	ImGui::BeginGroup();
	ImGui::TextColored(pal.accent, "SESSION COMPLETE%s", sum.endReason == "time" || sum.endReason == "done" ? "" : " (ended early)");
	theme::fontScale(1.3f);
	ImGui::TextUnformatted(sum.name.c_str());
	theme::fontScale(1.f);
	const int played = static_cast<int>(sum.playedSeconds);
	const int rate = sum.reps > 0 ? static_cast<int>(100.f * sum.successes / sum.reps + 0.5f) : 0;
	std::string level = sum.difficulty;
	if (!level.empty())
		level[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(level[0])));
	ImGui::TextDisabled("%s  |  air roll %s", level.c_str(), sum.airRoll.c_str());
	ImGui::Dummy(ImVec2(0, 6));

	// The headline numbers in a row (it was one long line of them).
	const float fs = ImGui::GetFontSize();
	const float inner = width - 44.f;
	{
		char time[16], reps[16], success[16], streak[16], fastest[24];
		snprintf(time, sizeof(time), "%d:%02d", played / 60, played % 60);
		snprintf(reps, sizeof(reps), "%d", sum.reps);
		snprintf(success, sizeof(success), "%d%%", rate);
		snprintf(streak, sizeof(streak), "%d", sum.bestStreak);
		if (sum.fastestKmh > 0.f)
			snprintf(fastest, sizeof(fastest), "%.0f km/h", sum.fastestKmh);
		else
			snprintf(fastest, sizeof(fastest), "-");
		const char* values[] = {time, reps, success, streak, fastest};
		const char* labels[] = {"PLAYED", "REPS", "SUCCESS", "BEST STREAK", "FASTEST GOAL"};
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float big = fs * 1.7f, small = fs * 0.75f;
		const float colW = inner / 5.f;
		for (int i = 0; i < 5; ++i)
		{
			const float x = at.x + colW * i;
			dl->AddText(theme::fontAt(big), big, ImVec2(x, at.y), theme::u32(i == 2 ? pal.accent : pal.text), values[i]);
			dl->AddText(theme::fontAt(small), small, ImVec2(x + 1.f, at.y + big * 1.15f), theme::u32(pal.textDim), labels[i]);
		}
		ImGui::Dummy(ImVec2(inner, big * 1.15f + small * 1.4f + 10.f));
	}

	// Per exercise: reps, successes (and their share), fails, best streak, fastest goal; faint lines between rows.
	const float cols[] = {0.f, 0.46f, 0.56f, 0.70f, 0.79f, 0.89f};
	const char* heads[] = {"EXERCISE", "REPS", "SUCCESS", "FAIL", "STREAK", "FASTEST"};
	ImVec2 row = ImGui::GetCursorScreenPos();
	const float hs = fs * 0.75f;
	for (int c = 0; c < 6; ++c)
		dl->AddText(theme::fontAt(hs), hs, ImVec2(row.x + inner * cols[c], row.y), theme::u32(pal.textDim), heads[c]);
	row.y += hs * 1.6f;
	for (const auto& e : sum.exercises)
	{
		dl->AddLine(ImVec2(row.x, row.y - 3.f), ImVec2(row.x + inner, row.y - 3.f), IM_COL32(255, 255, 255, 14), 1.f);
		char buf[6][96];
		snprintf(buf[0], 96, "%s", fitText(e.name, inner * (cols[1] - 0.02f)).c_str());
		snprintf(buf[1], 96, "%d", e.reps);
		if (e.reps > 0)
			snprintf(buf[2], 96, "%d  (%d%%)", e.successes, static_cast<int>(100.f * e.successes / e.reps + 0.5f));
		else
			snprintf(buf[2], 96, "%d", e.successes);
		snprintf(buf[3], 96, "%d", e.fails);
		snprintf(buf[4], 96, "%d", e.bestStreak);
		if (e.fastestKmh > 0.f)
			snprintf(buf[5], 96, "%.0f km/h", e.fastestKmh);
		else
			snprintf(buf[5], 96, "-");
		for (int c = 0; c < 6; ++c)
			dl->AddText(ImVec2(row.x + inner * cols[c], row.y + 3.f),
				theme::u32(c == 2 && e.successes > 0 ? pal.accent : c == 0 ? pal.text : theme::mix(pal.textDim, pal.text, 0.5f)), buf[c]);
		row.y += rowH;
	}
	ImGui::SetCursorScreenPos(ImVec2(pos.x + 22, row.y));
	ImGui::Dummy(ImVec2(inner, 0.f));
	ImGui::EndGroup();

	ImGui::SetCursorScreenPos(ImVec2(pos.x + width - 150, pos.y + 18));
	if (navButton("Dismiss", ImVec2(128, 34)) && cb_.dismissSummary)
		cb_.dismissSummary();

	const ImVec2 size(width, std::max(row.y, ImGui::GetItemRectMax().y) - pos.y + 14.f);
	dl->ChannelsSetCurrent(0);
	dl->AddRectFilled(pos, add(pos, size), theme::u32(pal.card), 12.f);
	dl->AddRect(pos, add(pos, size), theme::u32(pal.accent, 0.7f), 12.f, ImDrawCornerFlags_All, 1.5f);
	dl->ChannelsMerge();
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 18));
}

namespace
{
	std::string lower(std::string s)
	{
		for (auto& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	// Every word of `query` appears in the exercise's name, category, description or tags.
	bool matches(const ExerciseCard& c, const std::string& query)
	{
		std::string hay = lower(c.name + " " + categoryLabel(c.category) + " " + c.description);
		for (const auto& t : c.tags)
			hay += " " + lower(t);
		size_t pos = 0;
		const std::string q = lower(query);
		while (pos < q.size())
		{
			const size_t end = std::min(q.find(' ', pos), q.size());
			if (end > pos && hay.find(q.substr(pos, end - pos)) == std::string::npos)
				return false;
			pos = end + 1;
		}
		return true;
	}

	// "12 reps | 75%" (and the last result when there is room) along the top of a tile, kept inside `maxRight`.
	void statsPills(ImDrawList* dl, ImVec2 at, float maxRight, const ExerciseStatsView& v)
	{
		const theme::Palette& pal = theme::palette();
		if (v.reps <= 0)
			return;
		char buf[64];
		snprintf(buf, sizeof(buf), "%d %s  |  %d%%", v.reps, v.reps == 1 ? "rep" : "reps", static_cast<int>(100.f * v.successes / v.reps + 0.5f));
		if (at.x + ImGui::CalcTextSize(buf).x + 16.f > maxRight)
			snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(100.f * v.successes / v.reps + 0.5f));
		const float pw = darkPill(dl, at, buf);
		if (!v.lastLabel.empty() && at.x + pw + 6.f + ImGui::CalcTextSize(v.lastLabel.c_str()).x + 16.f <= maxRight)
			darkPill(dl, ImVec2(at.x + pw + 6.f, at.y), v.lastLabel,
				v.lastOutcome > 0 ? theme::u32(pal.accent) : v.lastOutcome < 0 ? IM_COL32(255, 120, 120, 255) : theme::u32(pal.textDim));
	}

	std::string agoText(long long when)
	{
		if (when <= 0)
			return "never";
		const long long secs = std::max(0LL, static_cast<long long>(std::time(nullptr)) - when);
		if (secs < 3600)
			return "just now";
		if (secs < 86400)
			return std::to_string(secs / 3600) + " h ago";
		const long long days = secs / 86400;
		return days == 1 ? "yesterday" : std::to_string(days) + " days ago";
	}

	// A row of options with a knob that slides to the chosen one (`anim` follows it). Returns the choice.
	int segmented(const char* id, const char* const* labels, int count, int current, float width, float& anim, bool enabled = true)
	{
		const theme::Palette& pal = theme::palette();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float h = std::max(40.f, ImGui::GetFontSize() * 1.7f); // room for the label at a big GUI scale
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float segW = width / count;
		anim = approach(anim, static_cast<float>(current), 9.f);
		dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + h), IM_COL32(6, 9, 16, 200), 12.f);
		dl->AddRect(pos, ImVec2(pos.x + width, pos.y + h), IM_COL32(255, 255, 255, 25), 12.f);
		const ImVec2 k0(pos.x + 3.f + segW * anim, pos.y + 3.f), k1(k0.x + segW - 6.f, pos.y + h - 3.f);
		if (enabled)
		{
			dl->AddRectFilled(k0, k1, theme::u32(pal.accent), 10.f);
			roundedGradient(dl, k0, ImVec2(k1.x, (k0.y + k1.y) * 0.5f), IM_COL32(255, 255, 255, 40), IM_COL32(255, 255, 255, 40),
				IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0), 10.f, ImDrawCornerFlags_Top);
		}
		int chosen = current;
		for (int i = 0; i < count; ++i)
		{
			ImGui::SetCursorScreenPos(ImVec2(pos.x + segW * i, pos.y));
			ImGui::PushID(id);
			ImGui::PushID(i);
			if (navInvisible("seg", ImVec2(segW, h), 1, Fx::Toggle) && enabled)
				chosen = i;
			const bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();
			ImGui::PopID();
			// A label wider than its segment shrinks (down to 75%), then is shortened, so labels never overlap.
			ImFont* font = ImGui::GetFont();
			const float room = segW - 14.f;
			std::string text = labels[i];
			float size = ImGui::GetFontSize();
			ImVec2 ts = theme::fontAt(size)->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
			if (ts.x > room)
			{
				size = std::max(size * 0.75f, size * room / ts.x);
				ts = theme::fontAt(size)->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
				std::string base = text;
				while (ts.x > room && base.size() > 1)
				{
					base.pop_back();
					text = base + "..";
					ts = theme::fontAt(size)->CalcTextSizeA(size, FLT_MAX, 0.f, text.c_str());
				}
			}
			const float near = 1.f - std::min(1.f, std::fabs(anim - i)); // the knob is under this label
			const ImU32 ink = !enabled ? theme::u32(pal.textDim, 0.6f)
				: near > 0.5f ? IM_COL32(8, 12, 18, 255) : hovered ? theme::u32(pal.text) : theme::u32(pal.textDim);
			dl->AddText(theme::fontAt(size), size, ImVec2(pos.x + segW * i + (segW - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), ink, text.c_str());
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h + 8.f));
		ImGui::Dummy(ImVec2(width, 0.f));
		return chosen;
	}

	// Menus that open under the button just drawn (BeginPopup alone opens them wherever the mouse is), with roomier
	// rows and rounded corners. A button in the window's right half gets its menu right-aligned under it, so the menu
	// stays inside the window. Pair beginMenuBelow with endMenuBelow.
	void pushMenuStyle()
	{
		const float u = uiScale();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * u, 10.f * u));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 12.f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f * u, 7.f * u));
		ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
	}
	void popMenuStyle() { ImGui::PopStyleVar(4); }

	bool beginMenuBelow(const char* id)
	{
		const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
		const float mid = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f;
		const bool right = (lo.x + hi.x) * 0.5f > mid;
		ImGui::SetNextWindowPos(ImVec2(right ? hi.x : lo.x, hi.y + 6.f), ImGuiCond_Appearing, ImVec2(right ? 1.f : 0.f, 0.f));
		pushMenuStyle();
		if (ImGui::BeginPopup(id))
			return true;
		popMenuStyle();
		return false;
	}
	void endMenuBelow()
	{
		ImGui::EndPopup();
		popMenuStyle();
	}

	void sectionLabel(const char* text)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().accent);
		ImGui::TextUnformatted(text);
		ImGui::PopStyleColor();
	}

	const char* kSortNames[] = {"Recommended", "Name A-Z", "Recently played", "Most played", "Favourites first", "Recently updated"};
	constexpr int kSortCount = 6;
	const char* kFavourites = "__favourites"; // the category menu's Favourites entry
}

int Dashboard::exerciseCard(const ExerciseCard& card, float width, const ExerciseStatsView* stats, bool favorite)
{
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 size(width, std::clamp(width * 0.5625f, 150.f, 240.f)); // 16:9 like the art
	const ImVec2 end = add(pos, size);

	ImGui::PushID(card.id.c_str());
	const bool clicked = navInvisible("card", size, 1, Fx::Click);
	const bool hovered = ImGui::IsItemHovered();
	const bool doubleClicked = hovered && ImGui::IsMouseDoubleClicked(0);
	ImGui::PopID();
	float& h = hoverAnim_[card.id];
	h = approach(h, hovered ? 1.f : 0.f, 7.f);

	// Lifted a little while hovered. Art (or the drawn mini-pitch) fills the tile; setup-only drills are dimmed.
	const ImVec2 a(pos.x, pos.y - 3.f * h), b(end.x, end.y - 3.f * h);
	const bool setupOnly = !card.pending.empty();
	const int light = static_cast<int>((setupOnly ? 120.f : 215.f) + 40.f * h);
	const ImageInfo art = card.image >= 0 && cb_.image ? cb_.image(card.image) : ImageInfo{};
	if (!drawCover(dl, art, a, b, 10.f, IM_COL32(light, light, light, 255), 0.06f * h))
		drawPreview(dl, a, b, card.preview);
	if (setupOnly)
		dl->AddRectFilled(a, b, IM_COL32(6, 9, 16, static_cast<int>(110 * (1.f - h))), 10.f);
	tileFade(dl, a, b, 0.6f);
	tileTitle(dl, a, b, card.name, 1.2f);

	if (setupOnly)
		darkPill(dl, ImVec2(a.x + 10.f, a.y + 10.f), "Setup only", theme::u32(pal.textDim));
	else if (stats)
		statsPills(dl, ImVec2(a.x + 10.f, a.y + 10.f), b.x - 88.f, *stats);
	if (h > 0.01f)
	{
		// Opens the exercise's page: a chevron in a circle, top right.
		const ImVec2 c(b.x - 26.f, a.y + 26.f);
		dl->AddCircleFilled(c, 13.f + 3.f * h, theme::u32(pal.accent, 0.95f * h), 24);
		const ImU32 ink = IM_COL32(10, 14, 18, static_cast<int>(255 * h));
		dl->AddLine(ImVec2(c.x - 3.f, c.y - 6.f), ImVec2(c.x + 3.f, c.y), ink, 2.5f);
		dl->AddLine(ImVec2(c.x + 3.f, c.y), ImVec2(c.x - 3.f, c.y + 6.f), ink, 2.5f);
	}
	// Favourite star, left of the chevron: gold when starred, an outline on hover; a click on it toggles it.
	const ImVec2 sc(b.x - 62.f, a.y + 26.f);
	bool starHit = false;
	if (favorite || h > 0.01f)
	{
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const bool overStar = hovered && (mouse.x - sc.x) * (mouse.x - sc.x) + (mouse.y - sc.y) * (mouse.y - sc.y) < 15.f * 15.f;
		starHit = clicked && overStar;
		dl->AddCircleFilled(sc, 13.f, IM_COL32(6, 9, 16, static_cast<int>(170 * std::max(h, favorite ? 1.f : 0.f))), 24);
		if (favorite)
			icons::star(dl, sc, 9.f + (overStar ? 1.5f : 0.f), IM_COL32(255, 205, 80, 255), true);
		else
			icons::star(dl, sc, 9.f + (overStar ? 1.5f : 0.f), overStar ? IM_COL32(255, 205, 80, 255) : IM_COL32(255, 255, 255, static_cast<int>(200 * h)), false);
	}
	tileFrame(dl, a, b, h, false);

	if (hovered && !card.description.empty())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(340.f);
		ImGui::TextUnformatted(card.description.c_str());
		ImGui::TextDisabled("Click for details, double-click to start.");
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
	return starHit ? 3 : doubleClicked ? 2 : clicked ? 1 : 0;
}

void Dashboard::renderExercises(const DashboardModel& m, const DashboardStatus& s)
{
	const theme::Palette& pal = theme::palette();
	if (!detailId_.empty())
	{
		const ExerciseCard* card = findCard(m, detailId_);
		if (card)
		{
			renderExerciseDetail(m, s, *card);
			return;
		}
		detailId_.clear();
	}
	pageTitle("Exercises", nullptr);
	if (!devTools_ && isDevCategory(category_))
		category_.clear();

	// Toolbar: search, category, sort.
	const float width = ImGui::GetContentRegionAvail().x;
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f, 9.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
	ImGui::PushItemWidth(std::min(380.f, width * 0.42f));
	ImGui::InputTextWithHint("##search", "Search: wall, ceiling, saves, rings...", search_, sizeof(search_));
	ImGui::PopItemWidth();
	const std::string catLabel = (category_.empty() ? std::string("All categories") : categoryLabel(category_)) + "  v";
	sameLineIfFits(buttonWidth(catLabel.c_str()));
	if (navButton(catLabel.c_str()))
		ImGui::OpenPopup("category_menu");
	// The menus open right under their button (they opened wherever the mouse was).
	const ImVec2 catMenuAt(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 6.f);
	const std::string sortLabel = std::string("Sort: ") + kSortNames[sort_] + "  v";
	sameLineIfFits(buttonWidth(sortLabel.c_str()));
	if (navButton(sortLabel.c_str()))
		ImGui::OpenPopup("sort_menu");
	const ImVec2 sortMenuAt(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 6.f);
	ImGui::PopStyleVar(2);
	// Sections per category (with any sort), or one list.
	sameLineIfFits(140.f, 14.f);
	navCheckbox("By category", &groupByCategory_);

	auto count = [&](const std::string& cat) {
		return static_cast<int>(std::count_if(m.exercises.begin(), m.exercises.end(),
			[&](const ExerciseCard& c) { return c.category == cat; }));
	};
	const size_t favCount = s.favorites ? std::count_if(m.exercises.begin(), m.exercises.end(),
		[&](const ExerciseCard& c) { return s.favorites->count(c.id) > 0; }) : 0;
	// Menu rows: roomy, the name on the left and (for categories) its count dim on the right.
	const float u = uiScale();
	const auto menuRow = [&](const char* label, int n, bool selected) {
		const float rowW = 280.f * u;
		ImGui::PushStyleColor(ImGuiCol_Header, theme::mix(pal.card, pal.accent, 0.35f));
		const bool hit = ImGui::Selectable(label, selected, 0, ImVec2(rowW, ImGui::GetFontSize() * 1.5f));
		ImGui::PopStyleColor();
		if (n >= 0)
		{
			const std::string c = std::to_string(n);
			const ImVec2 r = ImGui::GetItemRectMax();
			ImGui::GetWindowDrawList()->AddText(ImVec2(r.x - ImGui::CalcTextSize(c.c_str()).x - 8.f * u,
				ImGui::GetItemRectMin().y + (r.y - ImGui::GetItemRectMin().y - ImGui::GetFontSize()) * 0.5f), theme::u32(pal.textDim), c.c_str());
		}
		return hit;
	};
	const auto menuStyle = [&]() {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f * u, 10.f * u));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 12.f);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.f * u, 2.f * u));
		ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
	};
	menuStyle();
	ImGui::SetNextWindowPos(catMenuAt, ImGuiCond_Appearing);
	if (ImGui::BeginPopup("category_menu"))
	{
		if (menuRow("All categories", -1, category_.empty()))
			category_.clear();
		if (menuRow("Favourites", static_cast<int>(favCount), category_ == kFavourites))
		{
			category_ = kFavourites;
			sound("ui_click");
		}
		ImGui::Separator();
		for (const auto& cat : m.categories)
		{
			if (!devTools_ && isDevCategory(cat))
				continue;
			ImGui::PushID(cat.c_str());
			if (menuRow(categoryLabel(cat).c_str(), count(cat), category_ == cat))
			{
				category_ = cat;
				sound("ui_click");
			}
			ImGui::PopID();
		}
		ImGui::EndPopup();
	}
	ImGui::SetNextWindowPos(sortMenuAt, ImGuiCond_Appearing);
	if (ImGui::BeginPopup("sort_menu"))
	{
		for (int i = 0; i < kSortCount; ++i)
			if (menuRow(kSortNames[i], -1, sort_ == i))
			{
				sort_ = i;
				sound("ui_click");
			}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(4);
	if (!category_.empty() || search_[0])
	{
		sameLineIfFits(buttonWidth("Clear"));
		if (navButton("Clear"))
		{
			category_.clear();
			search_[0] = 0;
		}
	}
	ImGui::Dummy(ImVec2(0, 8));

	// What to show, in order.
	const auto* stats = s.stats.get();
	auto statOf = [&](const std::string& id) -> const ExerciseStatsView* {
		if (!stats)
			return nullptr;
		const auto it = stats->find(id);
		return it == stats->end() ? nullptr : &it->second;
	};
	std::vector<const ExerciseCard*> list;
	for (const auto& card : m.exercises)
	{
		if (!devTools_ && isDevCategory(card.category))
			continue;
		const bool fav = s.favorites && s.favorites->count(card.id) > 0;
		if (category_ == kFavourites ? !fav : !category_.empty() && card.category != category_)
			continue;
		if (search_[0] && !matches(card, search_))
			continue;
		list.push_back(&card);
	}
	if (sort_ == 4)
		std::stable_sort(list.begin(), list.end(), [&](const ExerciseCard* a, const ExerciseCard* b) {
			return (s.favorites && s.favorites->count(a->id)) > (s.favorites && s.favorites->count(b->id));
		});
	else if (sort_ == 5)
		std::stable_sort(list.begin(), list.end(), [](const ExerciseCard* a, const ExerciseCard* b) { return a->updated > b->updated; });
	if (sort_ == 1)
		std::stable_sort(list.begin(), list.end(), [](const ExerciseCard* a, const ExerciseCard* b) { return lower(a->name) < lower(b->name); });
	else if (sort_ == 2 || sort_ == 3)
		std::stable_sort(list.begin(), list.end(), [&](const ExerciseCard* a, const ExerciseCard* b) {
			const auto* sa = statOf(a->id);
			const auto* sb = statOf(b->id);
			const long long va = sa ? (sort_ == 2 ? sa->lastPlayed : sa->reps) : 0;
			const long long vb = sb ? (sort_ == 2 ? sb->lastPlayed : sb->reps) : 0;
			return va > vb;
		});
	// By category (when not already filtered to one): one section per category, in the categories' order, each sorted
	// by the chosen sort.
	const bool grouped = groupByCategory_ && category_.empty();
	if (grouped)
	{
		auto rank = [&](const std::string& cat) {
			const auto it = std::find(m.categories.begin(), m.categories.end(), cat);
			return static_cast<int>(it - m.categories.begin());
		};
		std::stable_sort(list.begin(), list.end(), [&](const ExerciseCard* a, const ExerciseCard* b) {
			return rank(a->category) < rank(b->category);
		});
	}
	auto shownIn = [&](const std::string& cat) {
		return static_cast<int>(std::count_if(list.begin(), list.end(), [&](const ExerciseCard* c) { return c->category == cat; }));
	};

	// The cards scroll on their own under the toolbar, so search, category and sort stay in reach.
	ImGui::BeginChild("exercise_grid", ImVec2(0.f, 0.f), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);
	const float avail = ImGui::GetContentRegionAvail().x;
	const float gap = 16.f;
	const int columns = std::max(1, static_cast<int>((avail + gap) / (250.f + gap)));
	const float cardW = (avail - gap * (columns - 1)) / columns;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	int shown = 0;
	std::string section;
	for (const ExerciseCard* card : list)
	{
		if (grouped && card->category != section)
		{
			section = card->category;
			if (shown > 0)
				ImGui::Dummy(ImVec2(0, 10));
			shown = 0;
			// Header: name, count, and a thin line to the edge.
			const ImVec2 p = ImGui::GetCursorScreenPos();
			std::string head = categoryLabel(section);
			for (auto& ch : head)
				ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
			ImGui::PushStyleColor(ImGuiCol_Text, pal.text);
			ImGui::TextUnformatted(head.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine(0.f, 8.f);
			ImGui::TextDisabled("%d", shownIn(section));
			const float lineY = p.y + ImGui::GetFontSize() * 0.55f;
			const float lineX = ImGui::GetItemRectMax().x + 12.f;
			dl->AddLine(ImVec2(lineX, lineY), ImVec2(p.x + avail, lineY), IM_COL32(255, 255, 255, 22), 1.f);
			ImGui::Dummy(ImVec2(0, 2));
		}
		if (shown % columns != 0)
			ImGui::SameLine(0.f, gap);
		const int action = exerciseCard(*card, cardW, statOf(card->id), s.favorites && s.favorites->count(card->id) > 0);
		if (action == 3 && cb_.favoriteToggled)
		{
			cb_.favoriteToggled(card->id);
			sound("ui_toggle");
		}
		else if (action == 1)
		{
			detailId_ = card->id;
			pageAt_ = ImGui::GetTime();
			sound("ui_page");
		}
		else if (action == 2 && cb_.play)
		{
			sound("ui_confirm");
			cb_.play(card->id, kDifficulties[difficulty_]);
			closeRequested_ = true;
		}
		++shown;
	}
	if (list.empty())
	{
		if (category_ == kFavourites && !search_[0])
			ImGui::TextDisabled("No favourites yet: hover a card and click its star, or press Favourite on an exercise's page.");
		else
			ImGui::TextDisabled(search_[0] ? "Nothing matches \"%s\"." : "No exercises here yet.", search_);
	}
	ImGui::Dummy(ImVec2(0, 8));
	ImGui::EndChild();
}

void Dashboard::packOptions(const DashboardStatus& s, float inner)
{
		// Training packs: an optional defender in the net (one setting for every pack).
		sectionLabel("DEFENDER");
		static const char* kWho[] = {"None", "Psyonix", "Necto", "Nexto"};
		static const char* kIds[] = {"", "psyonix", "necto", "nexto"};
		const int cur = s.packDefender == "nexto" ? 3 : s.packDefender == "necto" ? 2 : s.packDefender == "psyonix" ? 1 : 0;
		const int pick = segmented("pack_defender", kWho, 4, cur, inner, packDefenderAnim_);
		const bool available = pick < 2 || (pick == 2 && s.nectoAvailable) || (pick == 3 && s.nextoAvailable);
		if (pick != cur && available && cb_.packDefenderChanged)
			cb_.packDefenderChanged(kIds[pick]);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
		if (pick != cur && !available)
			ImGui::TextDisabled("%s is not installed (content/bots/%s/policy.bin).", kWho[pick], kIds[pick]);
		else
			ImGui::TextDisabled("%s", cur == 0 ? "Just you and the shots, like the game's training."
				: "Waits in the orange net, frozen until you start the shot. Stronger on hard. Applies to every pack.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 4));
		sectionLabel("NET TARGET");
		static const char* kTargets[] = {"Whole net", "Pick", "Random", "Moving"};
		static const char* kTargetIds[] = {"", "pick", "tile", "moving"};
		const int tcur = s.packTarget == "moving" ? 3 : s.packTarget == "tile" ? 2 : s.packTarget == "pick" ? 1 : 0;
		const int tpick = segmented("pack_target", kTargets, 4, tcur, inner, packTargetAnim_);
		if (tpick != tcur && cb_.packTargetChanged)
			cb_.packTargetChanged(kTargetIds[tpick], s.packTileSize, s.packTile);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
		ImGui::TextDisabled("%s", tcur == 0 ? "Any goal counts."
			: tcur == 1 ? "Only your tile is open: the rest of the net is closed and a ball that hits it bounces off (MISS)."
			: tcur == 2 ? "A random tile is open each shot; the rest of the net bounces the ball off (MISS)."
			: "The open tile keeps moving (faster on hard); the rest of the net bounces the ball off (MISS).");
		ImGui::PopTextWrapPos();
		if (tcur != 0)
		{
			ImGui::Dummy(ImVec2(0, 2));
			sectionLabel("TILE SIZE");
			static const char* kSizes[] = {"Easy  4", "Medium  6", "Hard  8", "Impossible"};
			static const char* kSizeIds[] = {"easy", "medium", "hard", "impossible"};
			const int scur = s.packTileSize == "easy" ? 0 : s.packTileSize == "hard" ? 2 : s.packTileSize == "impossible" ? 3 : 1;
			const int spick = segmented("pack_size", kSizes, 4, scur, inner, packSizeAnim_);
			if (spick != scur && cb_.packTargetChanged)
				cb_.packTargetChanged(s.packTarget, kSizeIds[spick], s.packTile);
			if (scur == 3)
				ImGui::TextDisabled("A hole only just bigger than the ball.");
			if (tcur == 1)
				packTilePicker(s, inner);
		}
		ImGui::Dummy(ImVec2(0, 4));
	}

void Dashboard::packTilePicker(const DashboardStatus& s, float width)
{
	// The orange net as the player sees it from the field: the game's x runs right to left, so screen column j is game
	// column cols - 1 - j; the top row on screen is the game's top row.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const bool hole = s.packTileSize == "impossible";
	const int cols = s.packTileSize == "easy" ? 2 : s.packTileSize == "hard" || hole ? 4 : 3, rows = 2;
	const int chosen = std::clamp(s.packTile, 0, cols * rows - 1);
	const float w = width, h = std::max(70.f, width * 643.f / 1786.f);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(6, 9, 16, 210), 6.f);
	const float cw = w / cols, ch = h / rows;
	for (int r = 0; r < rows; ++r)
	{
		for (int j = 0; j < cols; ++j)
		{
			const int cell = (rows - 1 - r) * cols + (cols - 1 - j);
			const ImVec2 a(p.x + j * cw, p.y + r * ch), b(a.x + cw, a.y + ch);
			ImGui::SetCursorScreenPos(a);
			ImGui::PushID("pack_tile");
			ImGui::PushID(cell);
			const bool clicked = navInvisible("tile", ImVec2(cw, ch), 1, Fx::Toggle);
			const bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();
			ImGui::PopID();
			if (clicked && cell != chosen && cb_.packTargetChanged)
				cb_.packTargetChanged(s.packTarget, s.packTileSize, cell);
			const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
			if (hole)
			{
				const float rr = std::min(cw, ch) * 0.22f;
				dl->AddCircle(c, rr, cell == chosen ? theme::u32(pal.accent) : IM_COL32(255, 255, 255, hovered ? 120 : 45), 32,
					cell == chosen ? 3.f : 1.5f);
				if (cell == chosen)
					dl->AddCircleFilled(c, rr - 4.f, theme::u32(pal.accent, 0.35f), 32);
			}
			else
			{
				const ImVec2 a2(a.x + 3.f, a.y + 3.f), b2(b.x - 3.f, b.y - 3.f);
				if (cell == chosen)
					dl->AddRectFilled(a2, b2, theme::u32(pal.accent, 0.55f), 4.f);
				dl->AddRect(a2, b2, cell == chosen ? theme::u32(pal.accent) : IM_COL32(255, 255, 255, hovered ? 120 : 45), 4.f, 0,
					cell == chosen ? 2.5f : 1.f);
			}
		}
	}
	// The posts and crossbar.
	dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x, p.y), IM_COL32(255, 255, 255, 200), 3.f);
	dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 200), 3.f);
	dl->AddLine(ImVec2(p.x + w, p.y), ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 200), 3.f);
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 6.f));
	ImGui::TextDisabled("Click the %s to aim for (the net as you see it from the field).", hole ? "spot for the hole" : "tile");
}

int Dashboard::opponentPicker(const char* id, const DashboardStatus& s, float width)
{
	// Who plays the defender in shooting drills (one setting: every shooting drill follows it).
	static const char* kWho[] = {"Psyonix bot", "Necto", "Nexto"};
	static const char* kIds[] = {"psyonix", "necto", "nexto"};
	const int cur = s.defender == "nexto" ? 2 : s.defender == "necto" ? 1 : 0;
	const int pick = segmented(id, kWho, 3, cur, width, defenderAnim_);
	const bool available = pick == 0 || (pick == 1 && s.nectoAvailable) || (pick == 2 && s.nextoAvailable);
	if (pick != cur && available && cb_.defenderChanged)
		cb_.defenderChanged(kIds[pick]);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
	if (pick != cur && !available)
		ImGui::TextDisabled("%s is not installed (content/bots/%s/policy.bin).", kWho[pick], kIds[pick]);
	else
		ImGui::TextDisabled("%s", cur == 2 ? "Nexto: the strongest RLGym bot. Applies to every shooting drill."
			: cur == 1 ? "Necto: the RLGym community's neural-network bot. Applies to every shooting drill."
			: "The game's own bot, by difficulty (rookie / pro / all-star). Applies to every shooting drill.");
	ImGui::PopTextWrapPos();
	return cur;
}

void Dashboard::renderExerciseDetail(const DashboardModel& m, const DashboardStatus& s, const ExerciseCard& card)
{
	// One exercise: what it is, how you have done, and how to start it.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float now = static_cast<float>(ImGui::GetTime());
	ExerciseStatsView st;
	if (s.stats)
	{
		const auto it = s.stats->find(card.id);
		if (it != s.stats->end())
			st = it->second;
	}

	if (navButton("<  Exercises"))
	{
		detailId_.clear();
		pageAt_ = ImGui::GetTime();
		sound("ui_back");
		return;
	}
	ImGui::Dummy(ImVec2(0, 4));

	// Banner.
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 size(width, std::clamp(width * 0.24f, 180.f, 240.f));
	const ImVec2 a = pos, b = add(pos, size);
	const ImageInfo art = card.image >= 0 && cb_.image ? cb_.image(card.image) : ImageInfo{};
	const float drift = 0.04f + 0.035f * (0.5f + 0.5f * std::sin(now * 0.22f));
	if (!drawCover(dl, art, a, b, 14.f, IM_COL32(235, 235, 235, 255), drift))
		drawPreview(dl, a, b, card.preview);
	const ImU32 shade = IM_COL32(5, 8, 15, 235), clear = IM_COL32(5, 8, 15, 0);
	roundedGradient(dl, a, ImVec2(a.x + width * 0.7f, b.y), shade, clear, clear, shade, 14.f, ImDrawCornerFlags_Left);
	dl->AddRect(a, b, theme::u32(pal.accent, 0.3f), 14.f, ImDrawCornerFlags_All, 1.2f);
	const TileName parts = splitName(card.name);
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	std::string kicker = categoryLabel(card.category);
	if (!parts.kicker.empty() && lower(parts.kicker) != lower(kicker))
		kicker += "  /  " + parts.kicker;
	for (auto& ch : kicker)
		ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
	shadowedText(dl, font, fs * 0.9f, ImVec2(a.x + 28.f, a.y + 26.f), theme::u32(pal.accent), kicker.c_str());
	const std::string title = fitText(parts.title, width * 0.6f / 2.1f);
	shadowedText(dl, font, fs * 2.1f, ImVec2(a.x + 28.f, a.y + 26.f + fs * 1.35f), IM_COL32(250, 252, 255, 255), title.c_str());
	float px = a.x + 28.f;
	const float py = b.y - 40.f;
	char buf[96];
	if (card.timeLimitSeconds > 0.0)
	{
		snprintf(buf, sizeof(buf), "%.0f s a rep", card.timeLimitSeconds);
		px += darkPill(dl, ImVec2(px, py), buf) + 6.f;
	}
	if (!card.opponent.empty())
	{
		const std::string who = card.opponent != "psyonix" ? card.opponent : s.defender;
		px += darkPill(dl, ImVec2(px, py), who == "nexto" ? "Vs Nexto" : who == "necto" ? "Vs Necto" : "Vs Psyonix bot",
			theme::u32(pal.accent)) + 6.f;
	}
	if (card.allowMirror)
		px += darkPill(dl, ImVec2(px, py), "Both sides") + 6.f;
	if (!card.hasDifficulty)
		px += darkPill(dl, ImVec2(px, py), "One difficulty") + 6.f;
	if (!card.pending.empty())
		darkPill(dl, ImVec2(px, py), "Setup only", theme::u32(pal.textDim));
	ImGui::SetCursorScreenPos(ImVec2(pos.x, b.y + 20.f));

	// Two columns: about and stats on the left, options and Start on the right.
	const bool stacked = width < 760.f;
	const float rightW = stacked ? width : std::clamp(width * 0.4f, 300.f, 420.f);
	const float leftW = stacked ? width : width - rightW - 28.f;
	const ImVec2 colTop = ImGui::GetCursorScreenPos();

	ImGui::BeginGroup();
	sectionLabel("ABOUT");
	ImGui::PushTextWrapPos(colTop.x + leftW - ImGui::GetWindowPos().x); // window-relative (it ran under the options card)
	ImGui::TextUnformatted(card.description.empty() ? "No description yet." : card.description.c_str());
	if (!card.pending.empty())
		ImGui::TextDisabled("Setup only: needs %s, so it is not judged yet and sessions skip it.", card.pending.c_str());
	ImGui::PopTextWrapPos();
	if (!card.tags.empty())
	{
		ImGui::Dummy(ImVec2(0, 4));
		ImVec2 tp = ImGui::GetCursorScreenPos();
		const float x0 = tp.x;
		for (const auto& t : card.tags)
		{
			const float w = ImGui::CalcTextSize(t.c_str()).x + 16.f;
			if (tp.x + w > x0 + leftW)
			{
				tp.x = x0;
				tp.y += ImGui::GetFontSize() + 12.f;
			}
			pill(dl, tp, t, pal.textDim);
			tp.x += w + 6.f;
		}
		ImGui::SetCursorScreenPos(ImVec2(x0, tp.y + ImGui::GetFontSize() + 12.f));
	}
	ImGui::Dummy(ImVec2(leftW, 10));
	sectionLabel("YOUR STATS");
	{
		// Four figures in a row.
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float cw = leftW / 4.f;
		const std::string pct = st.reps > 0 ? std::to_string(static_cast<int>(100.f * st.successes / st.reps + 0.5f)) + "%" : "-";
		const std::string reps = std::to_string(st.reps);
		const std::string last = st.lastLabel.empty() ? "-" : st.lastLabel;
		const std::string when = agoText(st.lastPlayed);
		const std::pair<const std::string*, const char*> figs[] = {{&reps, "REPS"}, {&pct, "SUCCESS"}, {&last, "LAST RESULT"},
			{&when, "LAST PLAYED"}};
		for (int i = 0; i < 4; ++i)
		{
			const ImVec2 at(p.x + cw * i, p.y + 4.f);
			const std::string v = fitText(*figs[i].first, (cw - 10.f) / 1.4f);
			dl->AddText(theme::fontAt(fs * 1.4f), fs * 1.4f, at, theme::u32(i == 1 && st.reps > 0 ? pal.accent : pal.text), v.c_str());
			dl->AddText(theme::fontAt(fs * 0.75f), fs * 0.75f, ImVec2(at.x, at.y + fs * 1.4f + 4.f), theme::u32(pal.textDim), figs[i].second);
		}
		ImGui::Dummy(ImVec2(leftW, fs * 2.4f + 10.f));
		if (st.best[0] > 0.0 || st.best[1] > 0.0 || st.best[2] > 0.0)
		{
			std::string bests = "Best times: ";
			for (int d = 0; d < 3; ++d)
			{
				if (st.best[d] <= 0.0)
					continue;
				snprintf(buf, sizeof(buf), "%s %.2f s   ", kDifficultyLabels[d], st.best[d]);
				bests += buf;
			}
			ImGui::TextColored(pal.accent, "%s", bests.c_str());
		}
	}
	ImGui::EndGroup();
	const float leftBottom = ImGui::GetItemRectMax().y;

	// Options card on the right (below, when stacked).
	ImGui::SetCursorScreenPos(stacked ? ImVec2(colTop.x, leftBottom + 16.f) : ImVec2(colTop.x + leftW + 28.f, colTop.y));
	const ImVec2 cardPos = ImGui::GetCursorScreenPos();
	dl->ChannelsSplit(2);
	dl->ChannelsSetCurrent(1);
	ImGui::SetCursorScreenPos(ImVec2(cardPos.x + 20.f, cardPos.y + 18.f));
	ImGui::BeginGroup();
	const float inner = rightW - 40.f;
	if (card.category == "packs")
	{
		packOptions(s, inner);
	}
	else if (card.opponent == "psyonix")
	{
		// Who defends: the game's bot or Necto (this is the Defender setting, so every shooting drill follows it).
		sectionLabel("OPPONENT");
		opponentPicker("opponent", s, inner);
		ImGui::Dummy(ImVec2(0, 4));
	}
	else if (card.opponent == "necto" || card.opponent == "nexto")
	{
		sectionLabel("OPPONENT");
		ImGui::TextUnformatted(card.opponent == "nexto" ? "Nexto" : "Necto");
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
		ImGui::TextDisabled("%s", card.opponent == "nexto" ? "The strongest RLGym bot. Easy plays looser; hard always plays its best."
			: "The RLGym community's neural-network bot. Easy plays looser; hard always plays its best.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 4));
	}
	sectionLabel("DIFFICULTY");
	difficulty_ = segmented("difficulty", kDifficultyLabels, 3, difficulty_, inner, diffAnim_, card.hasDifficulty);
	if (!card.hasDifficulty)
		ImGui::TextDisabled("This one is the same at every difficulty.");
	if (!card.variants.empty())
	{
		// e.g. Kickoff spot: Any / Diagonal / Off-centre / Back centre.
		ImGui::Dummy(ImVec2(0, 4));
		std::string title = card.variantLabel.empty() ? "Play" : card.variantLabel;
		for (auto& ch : title)
			ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		sectionLabel(title.c_str());
		std::vector<std::string> names = {"Any"};
		for (const auto& v : card.variants)
		{
			std::string n = v.second;
			if (!n.empty())
				n[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(n[0])));
			names.push_back(n);
		}
		std::vector<const char*> labels;
		for (const auto& n : names)
			labels.push_back(n.c_str());
		int& pick = variantPick_[card.id];
		pick = std::clamp(pick, 0, static_cast<int>(labels.size()) - 1);
		float need = 0.f;
		for (const auto* l : labels)
			need += ImGui::CalcTextSize(l).x + 2.f * ImGui::GetStyle().FramePadding.x + 6.f;
		if (need <= inner)
		{
			pick = segmented("variant", labels.data(), static_cast<int>(labels.size()), pick, inner, variantAnim_);
		}
		else
		{
			// Too many to fit one bar (they were cut to "Drop..", "Off a.."): chips that wrap, full names.
			const float x0 = ImGui::GetCursorPosX();
			for (int i = 0; i < static_cast<int>(labels.size()); ++i)
			{
				const float w = ImGui::CalcTextSize(labels[i]).x + 2.f * ImGui::GetStyle().FramePadding.x;
				if (i > 0)
				{
					ImGui::SameLine(0.f, 6.f);
					if (ImGui::GetCursorPosX() + w > x0 + inner)
					{
						ImGui::NewLine();
						ImGui::SetCursorPosX(x0);
					}
				}
				ImGui::PushID(i);
				if (chip(labels[i], pick == i))
					pick = i;
				ImGui::PopID();
			}
		}
	}
	if (card.allowMirror)
	{
		ImGui::Dummy(ImVec2(0, 4));
		sectionLabel(card.variants.empty() ? "SIDE" : "SIDE (LEFT / RIGHT)");
		static const char* kSides[] = {"Random", "Normal", "Mirrored"};
		side_ = segmented("side", kSides, 3, side_, inner, sideAnim_);
	}
	ImGui::Dummy(ImVec2(0, 8));
	const bool isFreeplay = card.id == m.freeplayId;
	const bool running = s.exerciseActive && s.exerciseId == card.id;
	if (running)
	{
		// Playing this one now: a big Stop, and Start becomes Restart (with the options above).
		ImGui::TextColored(pal.accent, "%s", s.sessionActive ? "Playing now, in a session" : "Playing now");
		if (bigButton("detail_stop", s.sessionActive ? "End session" : "Stop", ImVec2(inner, 52.f), false, Icon::Stop, true, Fx::Click)
			&& cb_.stop)
			cb_.stop();
		ImGui::Dummy(ImVec2(0, 2));
	}
	const char* startLabel = !card.pending.empty() ? "Open setup" : running ? "Restart" : "Start";
	if (bigButton("detail_start", startLabel, ImVec2(inner, running ? 44.f : 52.f), !running, Icon::Play, true, Fx::Confirm))
	{
		const std::optional<bool> mirror = card.allowMirror && side_ > 0 ? std::optional<bool>(side_ == 2) : std::nullopt;
		const int vp = variantPick_.count(card.id) ? variantPick_[card.id] : 0;
		const std::string variant = vp > 0 && vp <= static_cast<int>(card.variants.size()) ? card.variants[vp - 1].first : "";
		if (cb_.playWith)
			cb_.playWith(card.id, kDifficulties[difficulty_], mirror, variant);
		else if (cb_.play)
			cb_.play(card.id, kDifficulties[difficulty_]);
		closeRequested_ = true;
	}
	if (!s.inFreeplay)
		ImGui::TextDisabled(s.startPending ? "Loading Free Play..." : "Loads Free Play first.");
	ImGui::Dummy(ImVec2(0, 2));
	bool inQuickPlay = false;
	if (s.routine)
		for (const auto& step : s.routine->steps)
			for (const auto& d : step.drills)
				inQuickPlay = inQuickPlay || d.id == card.id;
	const float half = (inner - 8.f) * 0.5f;
	if (bigButton("detail_qp", inQuickPlay ? "In your warm-up" : "+ Warm-up", ImVec2(isFreeplay ? half : inner, 40.f), false,
		Icon::QuickPlay, false, Fx::Click) && !inQuickPlay && cb_.addToQuickPlay)
	{
		cb_.addToQuickPlay(card.id);
		toast("Added to your warm-up");
	}
	if (isFreeplay)
	{
		ImGui::SameLine(0.f, 8.f);
		if (bigButton("detail_fpp", "Objectives", ImVec2(half, 40.f), false, Icon::Settings, false, Fx::Click))
			page_ = Page::FreeplayPlus;
	}
	ImGui::Dummy(ImVec2(0, 2));
	{
		const bool fav = s.favorites && s.favorites->count(card.id) > 0;
		const ImVec2 at = ImGui::GetCursorScreenPos();
		if (bigButton("detail_fav", fav ? "Favourite" : "Add to favourites", ImVec2(inner, 40.f), false, Icon::Star, false, Fx::Toggle)
			&& cb_.favoriteToggled)
		{
			cb_.favoriteToggled(card.id);
			toast(fav ? "Removed from favourites" : "Added to favourites");
		}
		if (fav)
			icons::star(ImGui::GetWindowDrawList(), ImVec2(at.x + inner - 22.f, at.y + 20.f), 8.f, IM_COL32(255, 205, 80, 255), true);
	}
	ImGui::Dummy(ImVec2(0, 2));
	ImGui::TextDisabled("Tip: double-click a card to start right away.");
	ImGui::EndGroup();
	const ImVec2 cardEnd(cardPos.x + rightW, ImGui::GetItemRectMax().y + 18.f);
	dl->ChannelsSetCurrent(0);
	dl->AddRectFilled(cardPos, cardEnd, theme::u32(theme::mix(pal.card, pal.accent, 0.05f)), 14.f);
	dl->AddRect(cardPos, cardEnd, IM_COL32(255, 255, 255, 18), 14.f);
	dl->ChannelsMerge();

	ImGui::SetCursorScreenPos(ImVec2(colTop.x, std::max(leftBottom, cardEnd.y) + 16.f));
	ImGui::Dummy(ImVec2(0, 4));
}

namespace
{
	const char* stepKicker(const std::string& type)
	{
		return type == "drills" ? "FREE TOUCH DRILLS" : type == "freeplay" ? "FREEPLAY+" : type == "packs" ? "TRAINING PACKS" : type == "workshop" ? "WORKSHOP MAPS"
			: type == "notes" ? "NOTES" : "TASK";
	}

	bool isLink(const std::string& s) { return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0; }

	// A small square button with a drawn glyph (chevrons, a "more" menu, minus / plus), a hover background and a
	// tooltip. Disabled ones are drawn faint and do nothing.
	enum class Glyph { Up, Down, More, Minus, Plus, Close };
	bool iconButton(const char* id, Glyph g, const char* tip, bool enabled = true, float size = 28.f)
	{
		const theme::Palette& pal = theme::palette();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const bool clicked = navInvisible(id, ImVec2(size, size), 1, Fx::Click) && enabled;
		const bool hovered = ImGui::IsItemHovered() && enabled;
		if (hovered)
			dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), theme::u32(pal.cardHover), 7.f);
		const ImU32 ink = !enabled ? theme::u32(pal.textDim, 0.3f) : hovered ? theme::u32(pal.text) : theme::u32(pal.textDim);
		const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
		const float k = size * 0.18f;
		switch (g)
		{
		case Glyph::Up:
			dl->AddLine(ImVec2(c.x - k, c.y + k * 0.5f), ImVec2(c.x, c.y - k * 0.5f), ink, 2.f);
			dl->AddLine(ImVec2(c.x, c.y - k * 0.5f), ImVec2(c.x + k, c.y + k * 0.5f), ink, 2.f);
			break;
		case Glyph::Down:
			dl->AddLine(ImVec2(c.x - k, c.y - k * 0.5f), ImVec2(c.x, c.y + k * 0.5f), ink, 2.f);
			dl->AddLine(ImVec2(c.x, c.y + k * 0.5f), ImVec2(c.x + k, c.y - k * 0.5f), ink, 2.f);
			break;
		case Glyph::More:
			for (int d = -1; d <= 1; ++d)
				dl->AddCircleFilled(ImVec2(c.x + d * k * 1.1f, c.y), 2.f, ink, 8);
			break;
		case Glyph::Minus:
			dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), ink, 2.f);
			break;
		case Glyph::Plus:
			dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x + k, c.y), ink, 2.f);
			dl->AddLine(ImVec2(c.x, c.y - k), ImVec2(c.x, c.y + k), ink, 2.f);
			break;
		case Glyph::Close:
			dl->AddLine(ImVec2(c.x - k * 0.8f, c.y - k * 0.8f), ImVec2(c.x + k * 0.8f, c.y + k * 0.8f), ink, 2.f);
			dl->AddLine(ImVec2(c.x - k * 0.8f, c.y + k * 0.8f), ImVec2(c.x + k * 0.8f, c.y - k * 0.8f), ink, 2.f);
			break;
		}
		if (hovered && tip)
			ImGui::SetTooltip("%s", tip);
		return clicked;
	}

	// "Label   -  value  +": a labelled number with minus / plus buttons. True when it changed.
	bool stepper(const char* id, const char* label, int& value, int step, int lo, int hi, const char* (*format)(int, char*, size_t))
	{
		ImGui::PushID(id);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", label);
		ImGui::SameLine(0.f, 10.f);
		bool changed = false;
		if (iconButton("minus", Glyph::Minus, nullptr, value > lo, 26.f))
		{
			value = std::max(lo, value - step);
			changed = true;
		}
		ImGui::SameLine(0.f, 4.f);
		char buf[32];
		const char* text = format(value, buf, sizeof(buf));
		const float w = 74.f;
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const ImVec2 ts = ImGui::CalcTextSize(text);
		ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (26.f - ts.y) * 0.5f),
			theme::u32(value == lo ? theme::palette().textDim : theme::palette().text), text);
		ImGui::Dummy(ImVec2(w, 26.f));
		ImGui::SameLine(0.f, 4.f);
		if (iconButton("plus", Glyph::Plus, nullptr, value < hi, 26.f))
		{
			value = std::min(hi, value + step);
			changed = true;
		}
		ImGui::PopID();
		return changed;
	}
	const char* minutesText(int v, char* buf, size_t n)
	{
		if (v <= 0)
			return "not set";
		snprintf(buf, n, "%d min", v);
		return buf;
	}
	const char* tallyText(int v, char* buf, size_t n)
	{
		if (v <= 0)
			return "off";
		snprintf(buf, n, "%d", v);
		return buf;
	}

	// A compact button for rows and editors.
	bool smallButton(const char* label, Fx fx = Fx::Click)
	{
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.f, 4.f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.f);
		const bool clicked = navButton(label, ImVec2(0, 0), fx);
		ImGui::PopStyleVar(2);
		return clicked;
	}

	std::string capitalized(std::string s)
	{
		if (!s.empty())
			s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
		return s;
	}

	// Dim hint text drawn over an empty text box (ImGui 1.75 has no hint for multi-line boxes).
	void boxHint(const std::string& value, const char* hint)
	{
		if (!value.empty() || ImGui::IsItemActive() || !hint || !*hint)
			return;
		const ImVec2 a = ImGui::GetItemRectMin();
		ImGui::GetWindowDrawList()->AddText(ImVec2(a.x + 8.f, a.y + 6.f), theme::u32(theme::palette().textDim, 0.7f), hint);
	}

	// "2026-10-13" -> "Tuesday, Oct 13".
	std::string longDate(const std::string& day)
	{
		int y = 0, mo = 0, d = 0;
		if (std::sscanf(day.c_str(), "%d-%d-%d", &y, &mo, &d) != 3)
			return day;
		using namespace std::chrono;
		const year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)}, std::chrono::day{static_cast<unsigned>(d)}};
		if (!ymd.ok())
			return day;
		static const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
		static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		return std::string(kDays[weekday{sys_days{ymd}}.c_encoding()]) + ", " + kMonths[mo - 1] + " " + std::to_string(d);
	}
}

namespace
{
	void heading(const char* title, float width, const char* right); // a section heading with a rule (defined below)
}

float Dashboard::blockMinutes(const DashboardModel& m, const routine::Step& st) const
{
	// A rep takes about two thirds of its time limit (8 s with none), plus the pause before the next one.
	double seconds = 0.0;
	for (const auto& d : st.drills)
	{
		const ExerciseCard* card = findCard(m, d.id);
		const double rep = card && card->timeLimitSeconds > 0.0 ? card->timeLimitSeconds * 0.65 : 8.0;
		seconds += d.reps * (rep + 1.8);
	}
	return static_cast<float>(seconds / 60.0);
}

void Dashboard::renderBlockEditor(const DashboardModel& m, const DashboardStatus& s, routine::Routine& r, routine::Step& st,
	const std::function<void()>& commit)
{
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const float width = ImGui::GetContentRegionAvail().x;
	auto inBlock = [&](const std::string& id) {
		return std::any_of(st.drills.begin(), st.drills.end(), [&](const routine::Drill& d) { return d.id == id; });
	};
	auto playable = [&](const ExerciseCard& c) { return c.pending.empty() && !isDevCategory(c.category) && c.category != "speed_lab"; };

	// Header: back, the block's name, how long it is.
	if (smallButton("< Back to your routine"))
	{
		commit();
		blockEditId_.clear();
		return;
	}
	ImGui::Dummy(ImVec2(0, 4));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f, 9.f));
	ImGui::PushItemWidth(std::min(420.f, width * 0.6f));
	theme::fontScale(1.3f);
	ImGui::InputTextWithHint("##block_title", "Block name", &st.title);
	if (ImGui::IsItemDeactivatedAfterEdit())
		commit();
	theme::fontScale(1.f);
	ImGui::PopItemWidth();
	ImGui::PopStyleVar();
	{
		char buf[96];
		snprintf(buf, sizeof(buf), "%d drill%s  |  about %.0f min", static_cast<int>(st.drills.size()), st.drills.size() == 1 ? "" : "s",
			std::max(1.f, std::round(blockMinutes(m, st))));
		ImGui::SameLine(0.f, 16.f);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", st.drills.empty() ? "No drills yet" : buf);
	}
	ImGui::Dummy(ImVec2(0, 6));

	// Settings: order, difficulty, air roll.
	{
		const float col = std::min(260.f, (width - 40.f) / 3.f);
		const ImVec2 at = ImGui::GetCursorScreenPos();
		ImGui::BeginGroup();
		sectionLabel("ORDER");
		static const char* kOrder[] = {"In order", "Shuffle"};
		const int o = segmented("block_order", kOrder, 2, st.shuffle ? 1 : 0, col, blockOrderAnim_);
		if (o != (st.shuffle ? 1 : 0)) { st.shuffle = o == 1; commit(); }
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(at.x + col + 20.f, at.y));
		ImGui::BeginGroup();
		sectionLabel("DIFFICULTY");
		const int dcur = st.difficulty == "easy" ? 0 : st.difficulty == "hard" ? 2 : 1;
		const int dp = segmented("block_diff", kDifficultyLabels, 3, dcur, col, blockDiffAnim_);
		if (dp != dcur) { st.difficulty = kDifficulties[dp]; commit(); }
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(at.x + (col + 20.f) * 2.f, at.y));
		ImGui::BeginGroup();
		sectionLabel("AIR ROLL");
		static const char* kRoll[] = {"Left", "Right", "Free"};
		static const char* kRollIds[] = {"left", "right", "free"};
		const int rcur = st.airRoll == "left" ? 0 : st.airRoll == "right" ? 1 : 2;
		const int rp = segmented("block_roll", kRoll, 3, rcur, col, blockRollAnim_);
		if (rp != rcur) { st.airRoll = kRollIds[rp]; commit(); }
		ImGui::EndGroup();
	}

	// One-click fills.
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Quick fill:");
		auto addAll = [&](const std::vector<const ExerciseCard*>& cards) {
			int added = 0;
			for (const ExerciseCard* c : cards)
			{
				if (added >= 6 || inBlock(c->id))
					continue;
				st.drills.push_back({c->id, 5, ""});
				++added;
			}
			if (added > 0)
			{
				commit();
				sound("ui_confirm");
			}
		};
		ImGui::SameLine(0.f, 8.f);
		if (chip("My favourites", false))
		{
			std::vector<const ExerciseCard*> cards;
			for (const auto& c : m.exercises)
				if (playable(c) && s.favorites && s.favorites->count(c.id))
					cards.push_back(&c);
			addAll(cards);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Adds up to 6 of your starred drills.");
		ImGui::SameLine(0.f, 6.f);
		if (chip("My weak spots", false))
		{
			std::vector<std::pair<float, const ExerciseCard*>> weak;
			for (const auto& c : m.exercises)
			{
				if (!playable(c) || !s.stats)
					continue;
				const auto it = s.stats->find(c.id);
				if (it != s.stats->end() && it->second.reps >= 4)
					weak.push_back({static_cast<float>(it->second.successes) / it->second.reps, &c});
			}
			std::sort(weak.begin(), weak.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			std::vector<const ExerciseCard*> cards;
			for (const auto& w : weak)
				if (w.first < 0.6f)
					cards.push_back(w.second);
			addAll(cards);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Adds the drills where most of your reps fail (up to 6).");
		ImGui::SameLine(0.f, 6.f);
		if (chip("Random from...", false))
			ImGui::OpenPopup("block_random");
		if (beginMenuBelow("block_random"))
		{
			for (const auto& cat : m.categories)
			{
				if (isDevCategory(cat) || cat == "speed_lab" || cat == "captured")
					continue;
				if (ImGui::Selectable(categoryLabel(cat).c_str()))
				{
					std::vector<const ExerciseCard*> cards;
					for (const auto& c : m.exercises)
						if (playable(c) && c.category == cat)
							cards.push_back(&c);
					// A shuffle that changes each time it is pressed.
					static unsigned seed = 1u;
					seed = seed * 1103515245u + 12345u + static_cast<unsigned>(ImGui::GetFrameCount());
					for (size_t k = cards.size(); k > 1; --k)
					{
						seed = seed * 1103515245u + 12345u;
						std::swap(cards[k - 1], cards[(seed >> 16) % k]);
					}
					addAll(cards);
				}
			}
			endMenuBelow();
		}
		std::vector<const routine::Step*> others;
		for (const auto& o : r.steps)
			if (o.type == "drills" && o.id != st.id && !o.drills.empty())
				others.push_back(&o);
		if (!others.empty())
		{
			ImGui::SameLine(0.f, 6.f);
			if (chip("Copy from...", false))
				ImGui::OpenPopup("block_copy");
			if (beginMenuBelow("block_copy"))
			{
				ImGui::TextDisabled("Replace this block's drills with:");
				for (const routine::Step* o : others)
				{
					const std::string label = (o->title.empty() ? std::string("Drills") : o->title) + "  (" + std::to_string(o->drills.size()) + ")##" + o->id;
					if (ImGui::Selectable(label.c_str()))
					{
						st.drills = o->drills;
						commit();
					}
				}
				endMenuBelow();
			}
		}
		if (!st.drills.empty())
		{
			ImGui::SameLine(0.f, 14.f);
			if (chip("Clear", false))
				ImGui::OpenPopup("block_clear");
			if (beginMenuBelow("block_clear"))
			{
				ImGui::TextUnformatted("Remove every drill from this block?");
				if (ImGui::MenuItem("Clear it"))
				{
					st.drills.clear();
					commit();
				}
				ImGui::MenuItem("Keep them");
				endMenuBelow();
			}
		}
	}
	ImGui::Dummy(ImVec2(0, 8));

	// Two columns: the block (left), the browser (right); stacked when narrow.
	const bool stacked = width < 900.f;
	const float leftW = stacked ? width : width * 0.5f - 10.f;
	const float rightW = stacked ? width : width - leftW - 20.f;
	const ImVec2 colTop = ImGui::GetCursorScreenPos();

	// ---- In this block ----
	ImGui::BeginGroup();
	{
		char head[48];
		snprintf(head, sizeof(head), "%d", static_cast<int>(st.drills.size()));
		heading("IN THIS BLOCK", leftW, st.drills.empty() ? nullptr : head);
		if (st.drills.empty())
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + leftW);
			ImGui::TextDisabled("Empty. Click drills on the right to add them, or use a quick fill above.");
			ImGui::PopTextWrapPos();
		}
		int moveFrom = -1, moveTo = -1, removeAt = -1;
		const float rowH = 74.f;
		for (size_t j = 0; j < st.drills.size(); ++j)
		{
			routine::Drill& d = st.drills[j];
			const ExerciseCard* card = findCard(m, d.id);
			ImGui::PushID(static_cast<int>(j));
			const ImVec2 a = ImGui::GetCursorScreenPos();
			const ImVec2 b(a.x + leftW, a.y + rowH);
			dl->AddRectFilled(a, b, theme::u32(pal.card), 10.f);
			dl->AddRect(a, b, IM_COL32(255, 255, 255, 14), 10.f);
			// Drag handle (mouse: drag a row onto another to move it there).
			ImGui::SetCursorScreenPos(ImVec2(a.x + 4.f, a.y + 8.f));
			ImGui::InvisibleButton("grip", ImVec2(18.f, rowH - 16.f));
			const bool gripHover = ImGui::IsItemHovered() || ImGui::IsItemActive();
			for (int gy = 0; gy < 3; ++gy)
				for (int gx = 0; gx < 2; ++gx)
					dl->AddCircleFilled(ImVec2(a.x + 9.f + gx * 7.f, a.y + rowH * 0.5f - 8.f + gy * 8.f), 2.f,
						gripHover ? theme::u32(pal.text) : theme::u32(pal.textDim, 0.6f), 8);
			if (gripHover)
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
			{
				const int from = static_cast<int>(j);
				ImGui::SetDragDropPayload("FT_BLOCK_DRILL", &from, sizeof(int));
				ImGui::TextUnformatted(card ? card->name.c_str() : d.id.c_str());
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTargetCustom(ImRect(a, b), ImGui::GetID("drop")))
			{
				if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("FT_BLOCK_DRILL"))
				{
					moveFrom = *static_cast<const int*>(p->Data);
					moveTo = static_cast<int>(j);
				}
				ImGui::EndDragDropTarget();
			}
			// Thumbnail, number, name and category.
			const ImVec2 ta(a.x + 28.f, a.y + 9.f), tb(ta.x + 100.f, ta.y + 56.f);
			const ImageInfo art = card && card->image >= 0 && cb_.image ? cb_.image(card->image) : ImageInfo{};
			if (!drawCover(dl, art, ta, tb, 6.f))
			{
				if (card)
					drawPreview(dl, ta, tb, card->preview);
				else
					dl->AddRectFilled(ta, tb, theme::u32(pal.bg), 6.f);
			}
			char num[8];
			snprintf(num, sizeof(num), "%d", static_cast<int>(j + 1));
			darkPill(dl, ImVec2(ta.x + 4.f, ta.y + 4.f), num);
			const float infoX = tb.x + 12.f;
			// The controls' room: the wider of the difficulty chips and the reps / arrows row, plus a margin inside the card.
			static const char* kDiff[] = {"Block", "Easy", "Med", "Hard"};
			static const char* kDiffIds[] = {"", "easy", "medium", "hard"};
			float chipsW = 3.f * 3.f;
			for (const char* label : kDiff)
				chipsW += buttonWidth(label);
			const float repsW = 26.f + 2.f + ImGui::CalcTextSize("50 reps").x + 2.f + 26.f + 10.f + 26.f * 3.f + 4.f;
			const float controlsW = std::max(chipsW, repsW) + 14.f;
			const float nameW = std::max(80.f, b.x - infoX - controlsW);
			dl->AddText(theme::fontAt(fs), fs, ImVec2(infoX, a.y + 14.f), theme::u32(card ? pal.text : pal.textDim),
				fitText(card ? splitName(card->name).title : d.id + " (missing)", nameW).c_str());
			dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(infoX, a.y + 18.f + fs), theme::u32(pal.textDim),
				fitText(card ? categoryLabel(card->category) : std::string("Not loaded"), nameW).c_str());
			// Reps.
			const float cx = b.x - controlsW;
			ImGui::SetCursorScreenPos(ImVec2(cx, a.y + 8.f));
			if (iconButton("less", Glyph::Minus, "Fewer reps", d.reps > 1, 26.f)) { --d.reps; commit(); }
			ImGui::SameLine(0.f, 2.f);
			ImGui::AlignTextToFramePadding();
			ImGui::Text("%2d reps", d.reps);
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("more", Glyph::Plus, "More reps", d.reps < 50, 26.f)) { ++d.reps; commit(); }
			// Order and remove.
			ImGui::SameLine(0.f, 10.f);
			if (iconButton("up", Glyph::Up, "Play earlier", j > 0, 26.f)) { moveFrom = static_cast<int>(j); moveTo = static_cast<int>(j) - 1; }
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("down", Glyph::Down, "Play later", j + 1 < st.drills.size(), 26.f)) { moveFrom = static_cast<int>(j); moveTo = static_cast<int>(j) + 1; }
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("remove", Glyph::Close, "Remove from the block", true, 26.f))
				removeAt = static_cast<int>(j);
			// Difficulty: the block's, or its own.
			ImGui::SetCursorScreenPos(ImVec2(cx, a.y + 40.f));
			for (int k = 0; k < 4; ++k)
			{
				if (k > 0)
					ImGui::SameLine(0.f, 3.f);
				ImGui::PushID(k);
				if (chip(kDiff[k], d.difficulty == kDiffIds[k])) { d.difficulty = kDiffIds[k]; commit(); }
				ImGui::PopID();
			}
			ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 8.f));
			ImGui::PopID();
		}
		if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo && moveTo < static_cast<int>(st.drills.size()))
		{
			const routine::Drill moved = st.drills[moveFrom];
			st.drills.erase(st.drills.begin() + moveFrom);
			st.drills.insert(st.drills.begin() + moveTo, moved);
			commit();
		}
		if (removeAt >= 0)
		{
			st.drills.erase(st.drills.begin() + removeAt);
			commit();
		}
		if (st.drills.size() > 1)
			ImGui::TextDisabled("Drag a row by its dots to move it, or use the arrows.");
	}
	ImGui::Dummy(ImVec2(leftW, 0.f));
	ImGui::EndGroup();
	const float leftBottom = ImGui::GetItemRectMax().y;

	// ---- Add drills ----
	ImGui::SetCursorScreenPos(stacked ? ImVec2(colTop.x, leftBottom + 18.f) : ImVec2(colTop.x + leftW + 20.f, colTop.y));
	ImGui::BeginGroup();
	heading("ADD DRILLS", rightW, nullptr);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 8.f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
	ImGui::PushItemWidth(rightW * 0.55f);
	ImGui::InputTextWithHint("##block_search", "Search drills", blockSearch_, sizeof(blockSearch_));
	ImGui::PopItemWidth();
	ImGui::SameLine(0.f, 8.f);
	const std::string catLabel = (blockCategory_.empty() ? std::string("All") : categoryLabel(blockCategory_)) + "  v";
	if (navButton(catLabel.c_str()))
		ImGui::OpenPopup("block_cat");
	ImGui::PopStyleVar(2);
	if (beginMenuBelow("block_cat"))
	{
		if (ImGui::Selectable("All", blockCategory_.empty()))
			blockCategory_.clear();
		if (ImGui::Selectable("Favourites", blockCategory_ == "__favourites"))
			blockCategory_ = "__favourites";
		ImGui::Separator();
		for (const auto& cat : m.categories)
		{
			if (isDevCategory(cat) || cat == "speed_lab")
				continue;
			if (ImGui::Selectable(categoryLabel(cat).c_str(), blockCategory_ == cat))
				blockCategory_ = cat;
		}
		endMenuBelow();
	}
	ImGui::Dummy(ImVec2(0, 4));
	{
		const float gap = 10.f;
		const int cols = std::max(2, static_cast<int>((rightW + gap) / (170.f + gap)));
		const float cw = (rightW - gap * (cols - 1)) / cols, ch = cw * 0.56f;
		int shown = 0;
		for (const auto& c : m.exercises)
		{
			if (!playable(c))
				continue;
			if (blockCategory_ == "__favourites" ? !(s.favorites && s.favorites->count(c.id)) : !blockCategory_.empty() && c.category != blockCategory_)
				continue;
			if (blockSearch_[0] && !matches(c, blockSearch_))
				continue;
			if (shown % cols != 0)
				ImGui::SameLine(0.f, gap);
			ImGui::PushID(c.id.c_str());
			const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + ch);
			const bool added = inBlock(c.id);
			const bool clicked = navInvisible("add", ImVec2(cw, ch), 1, added ? Fx::Click : Fx::Confirm);
			const bool hovered = ImGui::IsItemHovered();
			const ImageInfo art = c.image >= 0 && cb_.image ? cb_.image(c.image) : ImageInfo{};
			if (!drawCover(dl, art, a, b, 8.f, IM_COL32(hovered ? 235 : 200, hovered ? 235 : 200, hovered ? 235 : 200, 255)))
				drawPreview(dl, a, b, c.preview);
			tileFade(dl, a, b, 0.7f, 8.f);
			dl->AddText(theme::fontAt(fs * 0.9f), fs * 0.9f, ImVec2(a.x + 8.f, b.y - fs * 0.9f - 8.f), IM_COL32(245, 248, 252, 255),
				fitText(splitName(c.name).title, cw - 16.f).c_str());
			if (added)
			{
				// Already in: a tick (a click takes it out again).
				const ImVec2 k(b.x - 16.f, a.y + 16.f);
				dl->AddCircleFilled(k, 11.f, theme::u32(pal.accent), 20);
				dl->AddLine(ImVec2(k.x - 5.f, k.y), ImVec2(k.x - 1.f, k.y + 4.f), IM_COL32(10, 14, 18, 255), 2.5f);
				dl->AddLine(ImVec2(k.x - 1.f, k.y + 4.f), ImVec2(k.x + 6.f, k.y - 4.f), IM_COL32(10, 14, 18, 255), 2.5f);
				dl->AddRect(a, b, theme::u32(pal.accent, 0.9f), 8.f, ImDrawCornerFlags_All, 2.f);
			}
			else if (hovered)
			{
				const ImVec2 k(b.x - 16.f, a.y + 16.f);
				dl->AddCircleFilled(k, 11.f, IM_COL32(6, 9, 16, 200), 20);
				dl->AddLine(ImVec2(k.x - 5.f, k.y), ImVec2(k.x + 5.f, k.y), IM_COL32(255, 255, 255, 230), 2.f);
				dl->AddLine(ImVec2(k.x, k.y - 5.f), ImVec2(k.x, k.y + 5.f), IM_COL32(255, 255, 255, 230), 2.f);
			}
			if (hovered)
				ImGui::SetTooltip("%s\n%s", c.name.c_str(), added ? "In this block: click to take it out." : "Click to add it to this block.");
			if (clicked)
			{
				if (added)
					st.drills.erase(std::remove_if(st.drills.begin(), st.drills.end(), [&](const routine::Drill& d) { return d.id == c.id; }),
						st.drills.end());
				else
					st.drills.push_back({c.id, 5, ""});
				commit();
			}
			ImGui::PopID();
			++shown;
		}
		if (shown == 0)
			ImGui::TextDisabled(blockCategory_ == "__favourites" ? "No favourites yet: star drills on the Exercises page." : "Nothing matches.");
	}
	ImGui::Dummy(ImVec2(rightW, 0.f));
	ImGui::EndGroup();
	const float rightBottom = ImGui::GetItemRectMax().y;
	ImGui::SetCursorScreenPos(ImVec2(colTop.x, std::max(leftBottom, rightBottom) + 12.f));
	ImGui::Dummy(ImVec2(0, 4));
}

namespace
{
	struct StepKind
	{
		const char* id;
		const char* name;
		const char* about;
		Icon icon;
	};
	const StepKind kStepKinds[] = {
		{"drills", "Free Touch drills", "Drills played back to back in Free Play, ticked when you finish them.", Icon::Exercises},
		{"freeplay", "Freeplay+", "Timed live freeplay with objectives popping up (5 to 10 minutes makes a great warm-up).", Icon::Play},
		{"packs", "Training packs", "Your packs, played in Free Touch with a defender or a net target.", Icon::Packs},
		{"workshop", "Workshop maps", "Maps to play outside Free Touch, as a checklist.", Icon::Replays},
		{"task", "Task", "Anything else (ranked games, replay review), with a counter and notes if you like.", Icon::Stats},
		{"notes", "Notes", "A notes box for the day.", Icon::Sessions},
	};
	const StepKind& stepKind(const std::string& id)
	{
		for (const auto& k : kStepKinds)
			if (id == k.id)
				return k;
		return kStepKinds[4];
	}
}

std::string Dashboard::stepSummary(const DashboardModel& m, const routine::Step& st) const
{
	char buf[160];
	if (st.type == "drills")
	{
		if (st.drills.empty())
			return "No drills yet";
		snprintf(buf, sizeof(buf), "%d drill%s  |  %s  |  %s  |  about %.0f min", static_cast<int>(st.drills.size()),
			st.drills.size() == 1 ? "" : "s", capitalized(st.difficulty).c_str(), st.shuffle ? "shuffled" : "in order",
			std::max(1.f, std::round(blockMinutes(m, st))));
		return buf;
	}
	if (st.type == "freeplay")
	{
		snprintf(buf, sizeof(buf), "%d min  |  %s  |  %s  |  %s", routine::freeplayMinutes(st), capitalized(st.difficulty).c_str(),
			st.objectives.empty() ? "your Freeplay+ objectives" : (std::to_string(st.objectives.size()) + " objectives").c_str(),
			st.boost.empty() ? "boost as on Freeplay+" : st.boost == "unlimited" ? "unlimited boost" : "standard boost");
		return buf;
	}
	if (st.type == "packs" || st.type == "workshop")
	{
		if (st.items.empty())
			return st.type == "packs" ? "No packs yet" : "No maps yet";
		std::string names;
		for (const auto& it : st.items)
		{
			if (!names.empty())
				names += ", ";
			names += it.name.empty() ? (it.code.empty() ? std::string("unnamed") : it.code) : it.name;
		}
		return std::to_string(st.items.size()) + (st.type == "packs" ? (st.items.size() == 1 ? " pack: " : " packs: ")
			: (st.items.size() == 1 ? " map: " : " maps: ")) + names;
	}
	if (st.type == "task")
	{
		std::string out = st.count > 0 ? "Count to " + std::to_string(st.count) : std::string("A tick");
		if (st.notes)
			out += "  |  notes box";
		if (st.optional)
			out += "  |  optional";
		return out;
	}
	return "A notes box for the day";
}

void Dashboard::renderRoutineEditor(const DashboardModel& m, const DashboardStatus& s, routine::Routine& r, const std::function<void()>& commit)
{
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const float width = ImGui::GetContentRegionAvail().x;
	(void)s;

	auto openStep = [&](const std::string& id) {
		blockEditId_ = id;
		blockSearch_[0] = 0;
		pageAt_ = ImGui::GetTime();
		sound("ui_page");
	};
	// Changes are collected, then made once after drawing (the lists are being walked).
	int dragFrom = -1, dropBefore = -1;     // a step dragged onto another (it goes in front of it)
	std::string dropSection;                // or onto a section (it goes to its end)
	int duplicateAt = -1, removeAt = -1, moveAt = -1;
	std::string moveToSection;
	int sectionMove = -1, sectionBy = 0, sectionRemove = -1;
	std::string addType, addSection;

	// 1. The routine itself.
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		ImGui::SetCursorScreenPos(ImVec2(p.x + 18.f, p.y + 14.f));
		ImGui::BeginGroup();
		sectionLabel("ROUTINE");
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 8.f));
		ImGui::PushItemWidth(std::min(360.f, width * 0.45f));
		ImGui::InputTextWithHint("##routine_name", "Routine name", &r.name);
		if (ImGui::IsItemDeactivatedAfterEdit())
			commit();
		ImGui::PopItemWidth();
		ImGui::PopStyleVar();
		sameLineIfFits(260.f, 18.f);
		ImGui::AlignTextToFramePadding();
		if (navCheckbox("\"Focus for today\" box", &r.focus))
			commit();
		sameLineIfFits(buttonWidth("Start from a template"), 18.f);
		if (navButton("Start from a template"))
			pickingTemplate_ = true;
		ImGui::EndGroup();
		const ImVec2 e(p.x + width, ImGui::GetItemRectMax().y + 14.f);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(p, e, theme::u32(pal.card), 12.f);
		dl->AddRect(p, e, IM_COL32(255, 255, 255, 14), 12.f);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(p.x, e.y + 16.f));
	}

	// 2. Sections.
	for (size_t si = 0; si < r.sections.size(); ++si)
	{
		routine::Section& sec = r.sections[si];
		ImGui::PushID(sec.id.c_str());
		const ImVec2 sp = ImGui::GetCursorScreenPos();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		ImGui::SetCursorScreenPos(ImVec2(sp.x + 16.f, sp.y + 14.f));
		ImGui::BeginGroup();
		const float inner = width - 32.f;
		// Header: name, warm-up, totals; order and delete on the right.
		{
			const ImVec2 hp = ImGui::GetCursorScreenPos();
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.f, 6.f));
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
			ImGui::PushItemWidth(std::min(280.f, inner * 0.4f));
			theme::fontScale(1.15f);
			ImGui::InputTextWithHint("##section", "Section name", &sec.title);
			if (ImGui::IsItemDeactivatedAfterEdit())
				commit();
			theme::fontScale(1.f);
			ImGui::PopItemWidth();
			ImGui::PopStyleColor();
			ImGui::PopStyleVar();
			if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
				ImGui::SetTooltip("Click to rename");
			ImGui::SameLine(0.f, 10.f);
			const bool warm = routine::warmupSection(r) == &sec;
			if (chip(warm ? "Warm-up section" : "Use as warm-up", warm) && !warm)
			{
				for (auto& o : r.sections)
					o.warmup = &o == &sec;
				commit();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Home's Warm-up button plays this section's Free Touch drills.");
			const int steps = static_cast<int>(std::count_if(r.steps.begin(), r.steps.end(), [&](const routine::Step& x) { return x.section == sec.id; }));
			const int mins = routine::plannedMinutes(r, sec.id);
			char info[64];
			if (mins > 0)
				snprintf(info, sizeof(info), "%d step%s  |  about %d min", steps, steps == 1 ? "" : "s", mins);
			else
				snprintf(info, sizeof(info), "%d step%s", steps, steps == 1 ? "" : "s");
			ImGui::SameLine(0.f, 12.f);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", info);
			ImGui::SetCursorScreenPos(ImVec2(hp.x + inner - 92.f, hp.y + 2.f));
			if (iconButton("sec_up", Glyph::Up, "Move this section up", si > 0)) { sectionMove = static_cast<int>(si); sectionBy = -1; }
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("sec_down", Glyph::Down, "Move this section down", si + 1 < r.sections.size())) { sectionMove = static_cast<int>(si); sectionBy = 1; }
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("sec_delete", Glyph::Close, "Delete this section", r.sections.size() > 1))
				ImGui::OpenPopup("section_delete");
			if (beginMenuBelow("section_delete"))
			{
				ImGui::TextUnformatted(steps > 0 ? "Delete this section and its steps?" : "Delete this section?");
				if (ImGui::MenuItem("Delete"))
					sectionRemove = static_cast<int>(si);
				ImGui::MenuItem("Keep it");
				endMenuBelow();
			}
			// Dropping a step on the header puts it at the end of this section.
			if (ImGui::BeginDragDropTargetCustom(ImRect(hp, ImVec2(hp.x + inner, hp.y + 36.f)), ImGui::GetID("section_drop")))
			{
				if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FT_STEP"))
				{
					dragFrom = *static_cast<const int*>(pl->Data);
					dropSection = sec.id;
				}
				ImGui::EndDragDropTarget();
			}
			ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + 40.f));
		}

		// Step rows.
		int number = 0;
		for (size_t i = 0; i < r.steps.size(); ++i)
		{
			routine::Step& st = r.steps[i];
			if (st.section != sec.id)
				continue;
			++number;
			ImGui::PushID(st.id.c_str());
			const StepKind& kind = stepKind(st.type);
			const float rowH = std::max(58.f, 24.f + fs * 2.35f); // the title and its summary line at any GUI scale
			const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + inner, a.y + rowH);
			float& hv = hoverAnim_["rstep." + st.id];
			hv = approach(hv, ImGui::IsMouseHoveringRect(a, b) ? 1.f : 0.f, 10.f);
			dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.bg, pal.card, 0.6f + 0.4f * hv)), 10.f);
			dl->AddRect(a, b, theme::u32(theme::mix(ImVec4(1, 1, 1, 0.06f), pal.accent, 0.5f * hv)), 10.f);
			// Grip (drag the row by it).
			ImGui::SetCursorScreenPos(ImVec2(a.x + 4.f, a.y + 6.f));
			ImGui::InvisibleButton("grip", ImVec2(18.f, rowH - 12.f));
			const bool gripHover = ImGui::IsItemHovered() || ImGui::IsItemActive();
			if (gripHover)
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			for (int gy = 0; gy < 3; ++gy)
				for (int gx = 0; gx < 2; ++gx)
					dl->AddCircleFilled(ImVec2(a.x + 10.f + gx * 6.f, a.y + rowH * 0.5f - 7.f + gy * 7.f), 1.8f,
						gripHover ? theme::u32(pal.text) : theme::u32(pal.textDim, 0.55f), 8);
			if (ImGui::BeginDragDropSource())
			{
				const int from = static_cast<int>(i);
				ImGui::SetDragDropPayload("FT_STEP", &from, sizeof(int));
				ImGui::TextUnformatted(st.title.empty() ? kind.name : st.title.c_str());
				ImGui::EndDragDropSource();
			}
			if (ImGui::BeginDragDropTargetCustom(ImRect(a, b), ImGui::GetID("step_drop")))
			{
				if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("FT_STEP"))
				{
					dragFrom = *static_cast<const int*>(pl->Data);
					dropBefore = static_cast<int>(i);
				}
				ImGui::EndDragDropTarget();
			}
			// Type icon in a circle, then the title and its one-line summary; the row opens the step's page.
			const ImVec2 ic(a.x + 46.f, a.y + rowH * 0.5f);
			dl->AddCircleFilled(ic, 17.f, theme::u32(theme::mix(pal.card, pal.accent, 0.25f + 0.2f * hv)), 24);
			icons::draw(dl, kind.icon, ic, 18.f, theme::u32(pal.text));
			const float editH = std::max(34.f, fs * 1.1f * 1.6f);
			const float editW = std::max(96.f, theme::fontAt(fs * 1.1f)->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0.f, "Edit").x + 40.f);
			const float controlsW = editW + 54.f;
			const float tx = a.x + 74.f, tw = b.x - tx - controlsW - 10.f;
			ImGui::SetCursorScreenPos(ImVec2(tx, a.y + 4.f));
			if (navInvisible("open", ImVec2(tw, rowH - 8.f), 1, Fx::Click))
				openStep(st.id);
			if (ImGui::IsItemHovered())
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			char num[8];
			snprintf(num, sizeof(num), "%d.", number);
			std::string title = st.title.empty() ? kind.name : st.title;
			dl->AddText(theme::fontAt(fs * 1.05f), fs * 1.05f, ImVec2(tx, a.y + 9.f), theme::u32(pal.textDim), num);
			const float nx = tx + theme::fontAt(fs * 1.05f)->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0.f, num).x + 6.f;
			dl->AddText(theme::fontAt(fs * 1.05f), fs * 1.05f, ImVec2(nx, a.y + 9.f), theme::u32(pal.text), fitText(title, tw - (nx - tx)).c_str());
			std::string summary = stepSummary(m, st);
			if (st.minutes > 0)
				summary = std::to_string(st.minutes) + " min  |  " + summary;
			dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(tx, a.y + 13.f + fs * 1.05f), theme::u32(pal.textDim), fitText(summary, tw).c_str());
			// Right: Edit, and a menu for the rest.
			ImGui::SetCursorScreenPos(ImVec2(b.x - controlsW + 6.f, a.y + (rowH - editH) * 0.5f));
			if (bigButton("edit", "Edit", ImVec2(editW, editH), false, Icon::Settings, false, Fx::Click))
				openStep(st.id);
			ImGui::SameLine(0.f, 6.f);
			ImGui::SetCursorScreenPos(ImVec2(b.x - 40.f, a.y + (rowH - 28.f) * 0.5f));
			if (iconButton("more", Glyph::More, "Duplicate, move, delete"))
			{
				confirmStep_.clear();
				ImGui::OpenPopup("step_menu");
			}
			if (beginMenuBelow("step_menu"))
			{
				if (confirmStep_ == st.id)
				{
					ImGui::TextUnformatted("Delete this step?");
					if (ImGui::MenuItem("Delete"))
						removeAt = static_cast<int>(i);
					if (ImGui::MenuItem("Keep it"))
						confirmStep_.clear();
				}
				else
				{
					if (ImGui::MenuItem("Edit..."))
						openStep(st.id);
					if (ImGui::MenuItem("Duplicate"))
						duplicateAt = static_cast<int>(i);
					if (r.sections.size() > 1 && ImGui::BeginMenu("Move to section"))
					{
						for (const auto& o : r.sections)
							if (o.id != sec.id && ImGui::MenuItem((o.title.empty() ? std::string("Section") : o.title).c_str()))
							{
								moveAt = static_cast<int>(i);
								moveToSection = o.id;
							}
						ImGui::EndMenu();
					}
					ImGui::Separator();
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.5f, 1.f));
					if (ImGui::Selectable("Delete...", false, ImGuiSelectableFlags_DontClosePopups))
						confirmStep_ = st.id;
					ImGui::PopStyleColor();
				}
				endMenuBelow();
			}
			ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 6.f));
			ImGui::PopID();
		}
		if (number == 0)
		{
			ImGui::TextDisabled("No steps yet.");
			ImGui::Dummy(ImVec2(0, 2));
		}
		// Add a step: the types with what each is for.
		if (smallButton("+ Add step"))
			ImGui::OpenPopup("add_step");
		if (beginMenuBelow("add_step"))
		{
			ImGui::TextDisabled("Add to %s", sec.title.empty() ? "this section" : sec.title.c_str());
			ImGui::Separator();
			for (const auto& k : kStepKinds)
			{
				ImGui::PushID(k.id);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const bool picked = ImGui::Selectable("##kind", false, 0, ImVec2(380.f, fs * 2.6f));
				ImDrawList* pdl = ImGui::GetWindowDrawList();
				pdl->AddCircleFilled(ImVec2(p.x + 18.f, p.y + fs * 1.3f), 15.f, theme::u32(theme::mix(pal.card, pal.accent, 0.3f)), 20);
				icons::draw(pdl, k.icon, ImVec2(p.x + 18.f, p.y + fs * 1.3f), 16.f, theme::u32(pal.text));
				pdl->AddText(ImVec2(p.x + 44.f, p.y + 3.f), theme::u32(pal.text), k.name);
				pdl->AddText(theme::fontAt(fs * 0.82f), fs * 0.82f, ImVec2(p.x + 44.f, p.y + 5.f + fs), theme::u32(pal.textDim), k.about);
				if (picked)
				{
					addType = k.id;
					addSection = sec.id;
				}
				ImGui::PopID();
			}
			endMenuBelow();
		}
		ImGui::EndGroup();
		const ImVec2 e(sp.x + width, ImGui::GetItemRectMax().y + 14.f);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(sp, e, theme::u32(pal.card), 12.f);
		dl->AddRectFilled(sp, ImVec2(sp.x + 4.f, e.y), theme::u32(pal.accent, routine::warmupSection(r) == &sec ? 0.9f : 0.25f), 12.f,
			ImDrawCornerFlags_Left);
		dl->AddRect(sp, e, IM_COL32(255, 255, 255, 14), 12.f);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(sp.x, e.y + 14.f));
		ImGui::PopID();
	}
	if (navButton("+ Add section"))
	{
		r.sections.push_back(routine::makeSection(r, "New section"));
		commit();
	}
	ImGui::Dummy(ImVec2(0, 4));
	ImGui::TextDisabled("Drag steps by their dots (onto another section's name to move them there). Click a step to edit it.");
	ImGui::Dummy(ImVec2(0, 8));

	// Apply what was asked for.
	auto endOf = [&](const std::string& section) {
		size_t at = r.steps.size();
		for (size_t k = 0; k < r.steps.size(); ++k)
			if (r.steps[k].section == section)
				at = k + 1;
		return at;
	};
	if (dragFrom >= 0 && dragFrom < static_cast<int>(r.steps.size()) && (dropBefore >= 0 || !dropSection.empty()) && dragFrom != dropBefore)
	{
		routine::Step moved = r.steps[dragFrom];
		const std::string targetSection = dropBefore >= 0 ? r.steps[dropBefore].section : dropSection;
		const std::string targetId = dropBefore >= 0 ? r.steps[dropBefore].id : std::string();
		r.steps.erase(r.steps.begin() + dragFrom);
		moved.section = targetSection;
		size_t at = endOf(targetSection);
		if (!targetId.empty())
		{
			for (size_t k = 0; k < r.steps.size(); ++k)
				if (r.steps[k].id == targetId)
					at = dragFrom <= static_cast<int>(k) ? k + 1 : k; // dragged down: after it; up: in front of it
		}
		r.steps.insert(r.steps.begin() + std::min(at, r.steps.size()), moved);
		commit();
	}
	else if (moveAt >= 0 && !moveToSection.empty())
	{
		routine::Step moved = r.steps[moveAt];
		moved.section = moveToSection;
		r.steps.erase(r.steps.begin() + moveAt);
		r.steps.insert(r.steps.begin() + std::min(endOf(moveToSection), r.steps.size()), moved);
		commit();
	}
	else if (duplicateAt >= 0)
	{
		routine::Step copy = r.steps[duplicateAt];
		copy.id = routine::newStepId(r);
		copy.title += " (copy)";
		r.steps.insert(r.steps.begin() + duplicateAt + 1, copy);
		commit();
	}
	else if (removeAt >= 0)
	{
		r.steps.erase(r.steps.begin() + removeAt);
		confirmStep_.clear();
		commit();
	}
	else if (sectionMove >= 0)
	{
		std::swap(r.sections[sectionMove], r.sections[sectionMove + sectionBy]);
		commit();
	}
	else if (sectionRemove >= 0)
	{
		const std::string gone = r.sections[sectionRemove].id;
		r.steps.erase(std::remove_if(r.steps.begin(), r.steps.end(), [&](const routine::Step& x) { return x.section == gone; }), r.steps.end());
		r.sections.erase(r.sections.begin() + sectionRemove);
		commit();
	}
	else if (!addType.empty())
	{
		routine::Step step = routine::makeStep(r, addType, addSection);
		const std::string id = step.id;
		r.steps.insert(r.steps.begin() + std::min(endOf(addSection), r.steps.size()), step);
		commit();
		openStep(id); // straight into the new step's page
	}
}

void Dashboard::renderStepEditor(const DashboardModel& m, const DashboardStatus& s, routine::Routine& r, routine::Step& st,
	const std::function<void()>& commit)
{
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const float width = ImGui::GetContentRegionAvail().x;
	const StepKind& kind = stepKind(st.type);
	auto edited = [&] {
		if (ImGui::IsItemDeactivatedAfterEdit())
			commit();
	};

	if (smallButton("< Back to your routine"))
	{
		commit();
		blockEditId_.clear();
		return;
	}
	ImGui::Dummy(ImVec2(0, 4));
	// Title with the type's icon.
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		dl->AddCircleFilled(ImVec2(p.x + 20.f, p.y + 22.f), 20.f, theme::u32(theme::mix(pal.card, pal.accent, 0.35f)), 24);
		icons::draw(dl, kind.icon, ImVec2(p.x + 20.f, p.y + 22.f), 20.f, theme::u32(pal.text));
		ImGui::SetCursorScreenPos(ImVec2(p.x + 52.f, p.y));
		ImGui::BeginGroup();
		std::string kicker = kind.name;
		for (auto& c : kicker)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		ImGui::TextColored(pal.accent, "%s", kicker.c_str());
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.f, 7.f));
		ImGui::PushItemWidth(std::min(460.f, width - 60.f));
		theme::fontScale(1.25f);
		ImGui::InputTextWithHint("##step_title", "Step name", &st.title);
		edited();
		theme::fontScale(1.f);
		ImGui::PopItemWidth();
		ImGui::PopStyleVar();
		ImGui::EndGroup();
	}
	ImGui::Dummy(ImVec2(0, 6));

	// The basics, in one card: note or link, planned time, section, optional.
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		ImGui::SetCursorScreenPos(ImVec2(p.x + 18.f, p.y + 14.f));
		ImGui::BeginGroup();
		const float inner = width - 36.f;
		sectionLabel("NOTE OR LINK");
		ImGui::PushItemWidth(inner);
		ImGui::InputTextWithHint("##detail", "Shown under the step's name; a link gets a Copy button (optional)", &st.detail);
		edited();
		ImGui::PopItemWidth();
		ImGui::Dummy(ImVec2(0, 4));
		if (st.type != "freeplay" && stepper("minutes", "Planned time", st.minutes, 5, 0, 180, minutesText))
			commit();
		if (r.sections.size() > 1)
		{
			if (st.type != "freeplay")
				sameLineIfFits(260.f, 24.f);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("Section");
			ImGui::SameLine(0.f, 8.f);
			std::string current = "Section";
			for (const auto& o : r.sections)
				if (o.id == st.section)
					current = o.title.empty() ? "Section" : o.title;
			ImGui::PushItemWidth(180.f);
			if (ImGui::BeginCombo("##section", current.c_str()))
			{
				for (const auto& o : r.sections)
					if (ImGui::Selectable((o.title.empty() ? std::string("Section") : o.title).c_str(), o.id == st.section) && o.id != st.section)
					{
						// To the end of that section.
						routine::Step moved = st;
						moved.section = o.id;
						const std::string id = st.id;
						r.steps.erase(std::remove_if(r.steps.begin(), r.steps.end(), [&](const routine::Step& x) { return x.id == id; }), r.steps.end());
						size_t at = r.steps.size();
						for (size_t k = 0; k < r.steps.size(); ++k)
							if (r.steps[k].section == o.id)
								at = k + 1;
						r.steps.insert(r.steps.begin() + at, moved);
						commit();
						ImGui::EndCombo();
						ImGui::PopItemWidth();
						ImGui::EndGroup();
						dl->ChannelsMerge();
						return; // st moved: draw again next frame
					}
				ImGui::EndCombo();
			}
			ImGui::PopItemWidth();
		}
		if (st.type != "notes")
		{
			sameLineIfFits(240.f, 24.f);
			if (navCheckbox("Optional (only counts when done)", &st.optional))
				commit();
		}
		ImGui::EndGroup();
		const ImVec2 e(p.x + width, ImGui::GetItemRectMax().y + 14.f);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(p, e, theme::u32(pal.card), 12.f);
		dl->AddRect(p, e, IM_COL32(255, 255, 255, 14), 12.f);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(p.x, e.y + 16.f));
	}

	if (st.type == "freeplay")
	{
		// Length, difficulty, boost, and which objectives come up.
		heading("FREEPLAY+", width, nullptr);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("Length");
		static const int kLengths[] = {3, 5, 10, 15, 20};
		for (int len : kLengths)
		{
			ImGui::SameLine(0.f, 6.f);
			ImGui::PushID(len);
			char label[16];
			snprintf(label, sizeof(label), "%d min", len);
			if (chip(label, routine::freeplayMinutes(st) == len)) { st.minutes = len; commit(); }
			ImGui::PopID();
		}
		ImGui::SameLine(0.f, 14.f);
		if (stepper("fp_minutes", "or", st.minutes, 1, 1, 60, minutesText))
			commit();
		const float col = std::min(300.f, (width - 20.f) / 2.f);
		const ImVec2 at = ImGui::GetCursorScreenPos();
		ImGui::BeginGroup();
		sectionLabel("DIFFICULTY");
		const int dcur = st.difficulty == "easy" ? 0 : st.difficulty == "hard" ? 2 : 1;
		const int dp = segmented("fp_diff", kDifficultyLabels, 3, dcur, col, blockDiffAnim_);
		if (dp != dcur) { st.difficulty = kDifficulties[dp]; commit(); }
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(at.x + col + 20.f, at.y));
		ImGui::BeginGroup();
		sectionLabel("BOOST");
		static const char* kBoost[] = {"As on Freeplay+", "Unlimited", "Standard"};
		static const char* kBoostIds[] = {"", "unlimited", "standard"};
		const int bcur = st.boost == "unlimited" ? 1 : st.boost == "standard" ? 2 : 0;
		const int bp = segmented("fp_boost", kBoost, 3, bcur, col, blockRollAnim_);
		if (bp != bcur) { st.boost = kBoostIds[bp]; commit(); }
		ImGui::EndGroup();
		ImGui::Dummy(ImVec2(0, 4));
		// Objectives: the Freeplay+ page's, or this step's own picks (grouped like the Freeplay+ page).
		sectionLabel("OBJECTIVES");
		bool own = !st.objectives.empty();
		if (navCheckbox("Pick this step's own objectives", &own))
		{
			if (own)
			{
				st.objectives = m.freeplayEnabled;
				if (st.objectives.empty())
					for (const auto& c : m.freeplayChoices)
						st.objectives.push_back(c.id);
			}
			else
			{
				st.objectives.clear();
			}
			commit();
		}
		if (!own)
		{
			ImGui::TextDisabled("Uses the objectives you picked on the Freeplay+ page (%s).",
				m.freeplayEnabled.empty() ? "all of them" : (std::to_string(m.freeplayEnabled.size()) + " on").c_str());
		}
		else
		{
			if (smallButton("All"))
			{
				st.objectives.clear();
				for (const auto& c : m.freeplayChoices)
					st.objectives.push_back(c.id);
				commit();
			}
			ImGui::SameLine(0.f, 6.f);
			if (smallButton("Beginner"))
			{
				st.objectives.clear();
				for (const auto& c : m.freeplayChoices)
					if (c.beginner)
						st.objectives.push_back(c.id);
				if (st.objectives.empty() && !m.freeplayChoices.empty())
					st.objectives.push_back(m.freeplayChoices.front().id);
				commit();
			}
			std::string group;
			for (const auto& c : m.freeplayChoices)
			{
				if (c.group != group)
				{
					group = c.group;
					ImGui::Dummy(ImVec2(0, 2));
					ImGui::TextDisabled("%s", group.empty() ? "Other" : group.c_str());
				}
				else
				{
					sameLineIfFits(buttonWidth(c.label.c_str()) + 10.f, 6.f);
				}
				const bool on = std::find(st.objectives.begin(), st.objectives.end(), c.id) != st.objectives.end();
				ImGui::PushID(c.id.c_str());
				if (chip(c.label.c_str(), on))
				{
					if (on && st.objectives.size() > 1)
						st.objectives.erase(std::remove(st.objectives.begin(), st.objectives.end(), c.id), st.objectives.end());
					else if (!on)
						st.objectives.push_back(c.id);
					commit();
				}
				if (ImGui::IsItemHovered() && !c.about.empty())
					ImGui::SetTooltip("%s", c.about.c_str());
				ImGui::PopID();
			}
		}
		ImGui::Dummy(ImVec2(0, 8));
		return;
	}
	if (st.type == "task" || st.type == "notes")
	{
		heading(st.type == "task" ? "TRACKING" : "NOTES BOX", width, nullptr);
		if (st.type == "task")
		{
			if (stepper("tally", "Count to", st.count, 1, 0, 20, tallyText))
				commit();
			ImGui::SameLine(0.f, 12.f);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", st.count > 0 ? "A counter: tap + as you go (e.g. 3 ranked games); done when it is reached."
				: "Off: a single tick when it is done.");
			if (navCheckbox("A notes box for this step", &st.notes))
				commit();
		}
		if (st.type == "notes" || st.notes)
		{
			ImGui::PushItemWidth(std::min(width, 460.f));
			ImGui::InputTextWithHint("##hint", st.type == "notes" ? "Prompt, e.g. How did today go?" : "Prompt, e.g. Mistakes -> fixes", &st.notesHint);
			edited();
			ImGui::PopItemWidth();
			ImGui::TextDisabled("The prompt shows in the empty box; what you write is kept for each day in your Journal.");
		}
		ImGui::Dummy(ImVec2(0, 8));
		return;
	}

	// Packs and workshop maps: a list, plus (packs) the ones already in Free Touch to add with a click.
	const bool packs = st.type == "packs";
	auto importedPack = [&](const std::string& code) -> const ExerciseCard* {
		const std::string want = packs::normalizeCode(code);
		if (want.empty())
			return nullptr;
		for (const auto& c : m.exercises)
			if (c.category == "packs" && packs::codeFromId(c.id) == want)
				return &c;
		return nullptr;
	};
	{
		char count[16];
		snprintf(count, sizeof(count), "%d", static_cast<int>(st.items.size()));
		heading(packs ? "PACKS IN THIS STEP" : "MAPS", width, st.items.empty() ? nullptr : count);
		int removeAt = -1, moveFrom = -1, moveTo = -1;
		for (size_t j = 0; j < st.items.size(); ++j)
		{
			routine::Item& it = st.items[j];
			ImGui::PushID(static_cast<int>(j));
			const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + 62.f);
			dl->AddRectFilled(a, b, theme::u32(pal.card), 10.f);
			dl->AddRect(a, b, IM_COL32(255, 255, 255, 14), 10.f);
			const ExerciseCard* have = packs ? importedPack(it.code) : nullptr;
			// Picture (packs: the pack's art when it is in Free Touch).
			const ImVec2 ta(a.x + 10.f, a.y + 8.f), tb(ta.x + 82.f, ta.y + 46.f);
			const ImageInfo art = have && have->image >= 0 && cb_.image ? cb_.image(have->image) : ImageInfo{};
			if (!drawCover(dl, art, ta, tb, 6.f))
			{
				dl->AddRectFilled(ta, tb, theme::u32(theme::mix(pal.bg, pal.accent, 0.15f)), 6.f);
				icons::draw(dl, packs ? Icon::Packs : Icon::Replays, ImVec2((ta.x + tb.x) * 0.5f, (ta.y + tb.y) * 0.5f), 22.f, theme::u32(pal.textDim));
			}
			ImGui::SetCursorScreenPos(ImVec2(tb.x + 12.f, a.y + 8.f));
			ImGui::PushItemWidth(std::min(260.f, width * 0.3f));
			ImGui::InputTextWithHint("##name", packs ? "Pack name" : "Map name", &it.name);
			edited();
			ImGui::PopItemWidth();
			if (packs)
			{
				ImGui::SameLine(0.f, 8.f);
				ImGui::PushItemWidth(210.f);
				ImGui::InputTextWithHint("##code", "XXXX-XXXX-XXXX-XXXX", &it.code, ImGuiInputTextFlags_CharsUppercase);
				if (ImGui::IsItemDeactivatedAfterEdit())
				{
					if (const std::string n = packs::normalizeCode(it.code); !n.empty())
						it.code = n;
					commit();
				}
				ImGui::PopItemWidth();
				// What Play will do.
				ImGui::SetCursorScreenPos(ImVec2(tb.x + 12.f, a.y + 38.f));
				if (have)
					ImGui::TextColored(pal.accent, "In Free Touch: Play starts it");
				else if (!packs::normalizeCode(it.code).empty())
					ImGui::TextDisabled("Not in Free Touch yet: Get & play reads its shots first");
				else if (!it.code.empty())
					ImGui::TextColored(ImVec4(1.f, 0.55f, 0.5f, 1.f), "That code doesn't look right (16 letters and numbers)");
				else
					ImGui::TextDisabled("Add its code to play it in Free Touch (or just keep it as a reminder)");
			}
			ImGui::SetCursorScreenPos(ImVec2(b.x - 96.f, a.y + 17.f));
			if (iconButton("up", Glyph::Up, "Earlier", j > 0)) { moveFrom = static_cast<int>(j); moveTo = static_cast<int>(j) - 1; }
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("down", Glyph::Down, "Later", j + 1 < st.items.size())) { moveFrom = static_cast<int>(j); moveTo = static_cast<int>(j) + 1; }
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("remove", Glyph::Close, "Remove"))
				removeAt = static_cast<int>(j);
			ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 6.f));
			ImGui::PopID();
		}
		if (moveFrom >= 0)
		{
			std::swap(st.items[moveFrom], st.items[moveTo]);
			commit();
		}
		if (removeAt >= 0)
		{
			st.items.erase(st.items.begin() + removeAt);
			commit();
		}
		if (smallButton(packs ? "+ Add a pack by its code" : "+ Add a map"))
		{
			st.items.push_back({});
			commit();
		}
	}
	if (packs)
	{
		ImGui::Dummy(ImVec2(0, 10));
		std::vector<const ExerciseCard*> mine;
		for (const auto& c : m.exercises)
			if (c.category == "packs" && !packs::codeFromId(c.id).empty())
				mine.push_back(&c);
		heading("YOUR PACKS", width, mine.empty() ? nullptr : "click to add");
		if (mine.empty())
			ImGui::TextDisabled("Packs you bring into Free Touch (Training Packs page) show here.");
		const float gap = 10.f;
		const int cols = std::max(2, static_cast<int>((width + gap) / (200.f + gap)));
		const float cw = (width - gap * (cols - 1)) / cols, ch = cw * 0.5f;
		int shown = 0;
		for (const ExerciseCard* c : mine)
		{
			if (shown % cols != 0)
				ImGui::SameLine(0.f, gap);
			ImGui::PushID(c->id.c_str());
			const std::string code = packs::codeFromId(c->id);
			const bool added = std::any_of(st.items.begin(), st.items.end(), [&](const routine::Item& x) { return packs::normalizeCode(x.code) == code; });
			const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + cw, a.y + ch);
			const bool clicked = navInvisible("pack", ImVec2(cw, ch), 1, Fx::Confirm);
			const bool hovered = ImGui::IsItemHovered();
			const ImageInfo art = c->image >= 0 && cb_.image ? cb_.image(c->image) : ImageInfo{};
			if (!drawCover(dl, art, a, b, 8.f, IM_COL32(hovered ? 235 : 190, hovered ? 235 : 190, hovered ? 235 : 190, 255)))
				dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.1f)), 8.f);
			tileFade(dl, a, b, 0.75f, 8.f);
			const std::string name = c->name.rfind("Pack: ", 0) == 0 ? c->name.substr(6) : c->name;
			dl->AddText(theme::fontAt(fs * 0.9f), fs * 0.9f, ImVec2(a.x + 8.f, b.y - fs * 0.9f - 8.f), IM_COL32(245, 248, 252, 255), fitText(name, cw - 16.f).c_str());
			if (added)
			{
				const ImVec2 k(b.x - 16.f, a.y + 16.f);
				dl->AddCircleFilled(k, 11.f, theme::u32(pal.accent), 20);
				dl->AddLine(ImVec2(k.x - 5.f, k.y), ImVec2(k.x - 1.f, k.y + 4.f), IM_COL32(10, 14, 18, 255), 2.5f);
				dl->AddLine(ImVec2(k.x - 1.f, k.y + 4.f), ImVec2(k.x + 6.f, k.y - 4.f), IM_COL32(10, 14, 18, 255), 2.5f);
				dl->AddRect(a, b, theme::u32(pal.accent, 0.9f), 8.f, ImDrawCornerFlags_All, 2.f);
			}
			if (clicked)
			{
				if (added)
					st.items.erase(std::remove_if(st.items.begin(), st.items.end(), [&](const routine::Item& x) { return packs::normalizeCode(x.code) == code; }),
						st.items.end());
				else
					st.items.push_back({name, code});
				commit();
			}
			ImGui::PopID();
			++shown;
		}
		(void)s;
	}
	ImGui::Dummy(ImVec2(0, 8));
}

void Dashboard::renderWarmup(const DashboardModel& m, const DashboardStatus& s)
{
	const theme::Palette& pal = theme::palette();
	const double now = ImGui::GetTime();
	if (!s.routine || !s.today)
	{
		pageTitle("Warm-up", nullptr);
		ImGui::TextDisabled("Loading your routine...");
		return;
	}
	// Drafts: what is on screen. They follow the saved copies except while being edited (and just after a change,
	// until the saved copy catches up).
	if (!routineEditing_ && blockEditId_.empty() && now - routineChangedAt_ > 0.6)
		routineDraft_ = *s.routine;
	if (!dayTextActive_ && now - dayChangedAt_ > 0.6)
		dayDraft_ = *s.today;
	bool textActive = false;
	routine::Routine& r = routineDraft_;
	routine::DayLog& day = dayDraft_;
	auto commitRoutine = [&] {
		routineChangedAt_ = now;
		if (cb_.saveRoutine)
			cb_.saveRoutine(r);
	};
	auto commitDay = [&] {
		dayChangedAt_ = now;
		if (cb_.updateDay)
			cb_.updateDay(day);
	};
	auto trackText = [&](bool commitRoutineOnEdit) {
		textActive = textActive || ImGui::IsItemActive();
		if (ImGui::IsItemDeactivatedAfterEdit())
		{
			if (commitRoutineOnEdit)
				commitRoutine();
			else
				commitDay();
		}
	};

	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const float width = ImGui::GetContentRegionAvail().x;

	if (r.empty() || pickingTemplate_)
	{
		renderRoutineTemplates(m, !r.empty());
		return;
	}
	// A drill block open in its own editor.
	if (!blockEditId_.empty())
	{
		for (auto& st : r.steps)
			if (st.id == blockEditId_)
			{
				if (st.type == "drills")
					renderBlockEditor(m, s, r, st, commitRoutine);
				else
					renderStepEditor(m, s, r, st, commitRoutine);
				return;
			}
		blockEditId_.clear();
	}

	// Header: title and date on the left; progress and Edit on the right.
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float barW = std::clamp(width * 0.25f, 120.f, 200.f);
		const float bx = p.x + width - barW - 150.f;
		// Title and date stay left of the progress bar (left of Edit while editing, where the bar is hidden).
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (routineEditing_ ? width - 150.f : bx - p.x - 12.f));
		theme::fontScale(1.55f);
		ImGui::TextUnformatted(routineEditing_ ? "Edit your routine" : r.name.c_str());
		theme::fontScale(1.f);
		ImGui::TextDisabled("%s", routineEditing_ ? "Sections, steps, drills, packs and tasks: all yours. Changes save as you go."
			: longDate(s.todayDate).c_str());
		ImGui::PopTextWrapPos();
		const ImVec2 below = ImGui::GetCursorScreenPos();
		const auto [done, total] = routine::progress(r, day);
		char buf[64];
		const float frac = total > 0 ? static_cast<float>(done) / total : 0.f;
		float& shown = hoverAnim_["warmup.progress"];
		shown = approach(shown, frac, 2.f);
		if (!routineEditing_)
		{
			snprintf(buf, sizeof(buf), "%d of %d done", done, total);
			dl->AddText(ImVec2(bx, p.y + 4.f), theme::u32(done >= total && total > 0 ? pal.accent : pal.text), buf);
			dl->AddRectFilled(ImVec2(bx, p.y + 30.f), ImVec2(bx + barW, p.y + 38.f), theme::u32(pal.textDim, 0.2f), 4.f);
			if (shown > 0.f)
				dl->AddRectFilled(ImVec2(bx, p.y + 30.f), ImVec2(bx + barW * shown, p.y + 38.f), theme::u32(pal.accent), 4.f);
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x + width - 134.f, p.y + 4.f));
		if (bigButton("warmup_edit", routineEditing_ ? "Done" : "Edit", ImVec2(134.f, 40.f), routineEditing_, Icon::Settings,
			false, Fx::Click))
		{
			if (routineEditing_)
			{
				commitRoutine();
				toast("Routine saved");
			}
			routineEditing_ = !routineEditing_;
			confirmStep_.clear();
		}
		ImGui::SetCursorScreenPos(ImVec2(below.x, std::max(below.y, p.y + 48.f) + 8.f));
	}

	// Edit mode: the outline of sections and steps (each step opens its own page).
	if (routineEditing_)
	{
		renderRoutineEditor(m, s, r, commitRoutine);
		dayTextActive_ = textActive;
		return;
	}
	if (routineEditing_)
	{
		ImGui::PushItemWidth(std::min(320.f, width * 0.5f));
		ImGui::InputTextWithHint("Routine name", "My routine", &r.name);
		trackText(true);
		ImGui::PopItemWidth();
		if (navCheckbox("\"Focus for today\" box at the top", &r.focus))
			commitRoutine();
		sameLineIfFits(buttonWidth("+ Add section"), 18.f);
		if (navButton("+ Add section"))
		{
			r.sections.push_back(routine::makeSection(r, "New section"));
			commitRoutine();
		}
		sameLineIfFits(buttonWidth("Start from a template"));
		if (navButton("Start from a template"))
			pickingTemplate_ = true;
		ImGui::Dummy(ImVec2(0, 8));
	}

	// Focus for today.
	if (r.focus)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		ImGui::SetCursorScreenPos(ImVec2(p.x + 18.f, p.y + 14.f));
		ImGui::BeginGroup();
		ImGui::TextColored(pal.accent, "FOCUS FOR TODAY");
		ImGui::InputTextMultiline("##focus", &day.focus, ImVec2(width - 36.f, 50.f));
		trackText(false);
		boxHint(day.focus, "One thing to work on today, e.g. \"first touch off the wall\"");
		ImGui::EndGroup();
		const ImVec2 e(p.x + width, ImGui::GetItemRectMax().y + 14.f);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(p, e, theme::u32(theme::mix(pal.card, pal.accent, 0.1f)), 12.f);
		dl->AddRectFilled(p, ImVec2(p.x + 4.f, e.y), theme::u32(pal.accent), 12.f, ImDrawCornerFlags_Left);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(p.x, e.y + 14.f));
	}

	// Sections, each with its steps.
	int moveFrom = -1, moveTo = -1, removeAt = -1, duplicateAt = -1;
	int sectionMove = -1, sectionBy = 0, sectionRemove = -1;
	std::string moveSection;  // a step moved to another section
	for (size_t si = 0; si < r.sections.size(); ++si)
	{
	routine::Section& sec = r.sections[si];
	ImGui::PushID(sec.id.c_str());
	{
		// Section header: name, Warm-up tag, progress and planned time (edit: rename, warm-up choice, order, remove).
		ImGui::Dummy(ImVec2(0, 4));
		const ImVec2 hp = ImGui::GetCursorScreenPos();
		const auto [sd, stotal] = routine::progress(r, day, sec.id);
		const int mins = routine::plannedMinutes(r, sec.id);
		if (routineEditing_)
		{
			ImGui::PushItemWidth(std::min(240.f, width * 0.35f));
			ImGui::InputTextWithHint("##section", "Section name", &sec.title);
			trackText(true);
			ImGui::PopItemWidth();
			bool warm = routine::warmupSection(r) == &sec;
			sameLineIfFits(160.f, 14.f);
			if (navCheckbox("Warm-up section", &warm) && warm)
			{
				for (auto& o : r.sections)
					o.warmup = &o == &sec;
				commitRoutine();
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Home's Warm-up button runs this section's Free Touch drills.");
			sameLineIfFits(100.f, 14.f);
			if (iconButton("sec_up", Glyph::Up, "Move section up", si > 0)) { sectionMove = static_cast<int>(si); sectionBy = -1; }
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("sec_down", Glyph::Down, "Move section down", si + 1 < r.sections.size())) { sectionMove = static_cast<int>(si); sectionBy = 1; }
			ImGui::SameLine(0.f, 2.f);
			if (iconButton("sec_more", Glyph::More, "Delete section", r.sections.size() > 1))
				ImGui::OpenPopup("section_menu");
			if (beginMenuBelow("section_menu"))
			{
				const int inside = static_cast<int>(std::count_if(r.steps.begin(), r.steps.end(), [&](const routine::Step& x) { return x.section == sec.id; }));
				ImGui::TextUnformatted(inside > 0 ? "Delete this section and its steps?" : "Delete this section?");
				if (ImGui::MenuItem("Delete"))
					sectionRemove = static_cast<int>(si);
				ImGui::MenuItem("Keep it");
				endMenuBelow();
			}
		}
		else
		{
			std::string head = sec.title.empty() ? std::string("SECTION") : sec.title;
			for (auto& c : head)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			dl->AddText(theme::fontAt(fs * 1.05f), fs * 1.05f, hp, theme::u32(pal.accent), head.c_str());
			float x = hp.x + theme::fontAt(fs * 1.05f)->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0.f, head.c_str()).x + 12.f;
			// The tag for the section Home's Warm-up button runs (not when the section is already called that: it read
			// "WARM-UP  Warm-up").
			if (routine::warmupSection(r) == &sec && r.sections.size() > 1 && routine::lowerCopy(sec.title) != "warm-up")
				x += darkPill(dl, ImVec2(x, hp.y - 2.f), "Home's Warm-up", theme::u32(pal.accent)) + 10.f;
			char info[64];
			if (mins > 0)
				snprintf(info, sizeof(info), "%d of %d done  |  about %d min", sd, stotal, mins);
			else
				snprintf(info, sizeof(info), "%d of %d done", sd, stotal);
			dl->AddText(ImVec2(x, hp.y + 1.f), theme::u32(sd >= stotal && stotal > 0 ? pal.accent : pal.textDim), info);
			ImGui::Dummy(ImVec2(width, fs * 1.4f));
		}
		ImGui::Dummy(ImVec2(0, 2));
	}
	int number = 0;
	for (size_t i = 0; i < r.steps.size(); ++i)
	{
		routine::Step& st = r.steps[i];
		if (st.section != sec.id)
			continue;
		++number;
		ImGui::PushID(st.id.c_str());
		const bool done = routine::isDone(st, day);
		const ImVec2 cp = ImGui::GetCursorScreenPos();
		const float right = cp.x + width - 18.f;
		const float controlsW = routineEditing_ ? 34.f : st.type == "drills" || st.type == "freeplay" ? 140.f : st.count > 0 ? 150.f : 0.f;
		const float tx = cp.x + 64.f;
		const float textW = right - tx - controlsW - 12.f;
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);

		// Edit: the step's place in its section (up / down). Otherwise the tick circle with its number.
		auto neighbour = [&](int dir) {
			for (int k = static_cast<int>(i) + dir; k >= 0 && k < static_cast<int>(r.steps.size()); k += dir)
				if (r.steps[k].section == sec.id)
					return k;
			return -1;
		};
		const bool editRow = routineEditing_;
		if (editRow)
		{
			ImGui::SetCursorScreenPos(ImVec2(cp.x + 16.f, cp.y + 10.f));
			if (iconButton("up", Glyph::Up, "Move up", neighbour(-1) >= 0)) { moveFrom = static_cast<int>(i); moveTo = neighbour(-1); }
			const std::string n = std::to_string(number);
			const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
			dl->AddText(ImVec2(cp.x + 30.f - ts.x * 0.5f, cp.y + 40.f), theme::u32(pal.textDim), n.c_str());
			ImGui::SetCursorScreenPos(ImVec2(cp.x + 16.f, cp.y + 40.f + ts.y + 2.f));
			if (iconButton("down", Glyph::Down, "Move down", neighbour(1) >= 0)) { moveFrom = static_cast<int>(i); moveTo = neighbour(1); }
		}
		ImGui::SetCursorScreenPos(ImVec2(cp.x + 14.f, cp.y + 14.f));
		if (!editRow && navInvisible("tick", ImVec2(34.f, 34.f), 1, Fx::Toggle))
		{
			day.done[st.id] = !done;
			if (done && st.count > 0)
				day.tally[st.id] = std::min(day.tally[st.id], st.count - 1); // unticking a full tally steps it back
			commitDay();
		}
		const bool tickHover = !editRow && ImGui::IsItemHovered();
		const ImVec2 c(cp.x + 31.f, cp.y + 31.f);
		float& tickAnim = hoverAnim_["tick." + st.id];
		tickAnim = approach(tickAnim, done ? 1.f : 0.f, 6.f);
		if (!editRow && tickAnim > 0.01f)
		{
			dl->AddCircleFilled(c, 15.f * (0.6f + 0.4f * easeOut(tickAnim)), theme::u32(pal.accent, tickAnim), 28);
			const ImU32 ink = IM_COL32(8, 12, 18, static_cast<int>(255 * tickAnim));
			dl->AddLine(ImVec2(c.x - 6.f, c.y + 0.5f), ImVec2(c.x - 1.5f, c.y + 5.f), ink, 2.6f);
			dl->AddLine(ImVec2(c.x - 1.5f, c.y + 5.f), ImVec2(c.x + 6.5f, c.y - 5.f), ink, 2.6f);
		}
		if (!editRow && tickAnim < 0.99f)
		{
			dl->AddCircle(c, 15.f, theme::u32(tickHover ? pal.accent : pal.textDim, 1.f - tickAnim), 28, 1.8f);
			const std::string n = std::to_string(number);
			const ImVec2 ts = ImGui::CalcTextSize(n.c_str());
			dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), theme::u32(pal.text, 1.f - tickAnim), n.c_str());
		}

		// Text column.
		ImGui::SetCursorScreenPos(ImVec2(tx, cp.y + 14.f));
		ImGui::BeginGroup();
		ImGui::PushTextWrapPos(tx + textW - ImGui::GetWindowPos().x); // window-relative
		{
			std::string kicker = stepKicker(st.type);
			if (st.optional)
				kicker += "   OPTIONAL";
			dl->AddText(theme::fontAt(fs * 0.75f), fs * 0.75f, ImGui::GetCursorScreenPos(), theme::u32(pal.textDim), kicker.c_str());
			ImGui::Dummy(ImVec2(0.f, fs * 0.8f));
		}
		if (routineEditing_)
		{
			ImGui::PushItemWidth(std::min(textW, 420.f));
			ImGui::InputTextWithHint("##title", "Step name", &st.title);
			trackText(true);
			ImGui::InputTextWithHint("##detail", "A note or a link to copy (optional)", &st.detail);
			trackText(true);
			ImGui::PopItemWidth();
			if (stepper("minutes", "Planned time", st.minutes, 5, 0, 180, minutesText))
				commitRoutine();
		}
		else
		{
			const std::string title = fitText(st.title.empty() ? "Untitled step" : st.title, textW / 1.15f);
			const ImVec2 tp = ImGui::GetCursorScreenPos();
			dl->AddText(theme::fontAt(fs * 1.15f), fs * 1.15f, tp, theme::u32(done ? pal.textDim : pal.text), title.c_str());
			if (done)
			{
				const float tw = theme::fontAt(fs * 1.15f)->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0.f, title.c_str()).x;
				dl->AddLine(ImVec2(tp.x, tp.y + fs * 0.62f), ImVec2(tp.x + tw * std::min(1.f, tickAnim), tp.y + fs * 0.62f),
					theme::u32(pal.textDim), 1.5f);
			}
			ImGui::Dummy(ImVec2(0.f, fs * 1.2f));
			if (!st.detail.empty())
			{
				const std::string d = fitText(st.detail, textW - (isLink(st.detail) ? 100.f : 0.f));
				ImGui::TextDisabled("%s", d.c_str());
				if (isLink(st.detail))
				{
					ImGui::SameLine(0.f, 8.f);
					if (smallButton("Copy link"))
						ImGui::SetClipboardText(st.detail.c_str());
				}
			}
		}

		// The step's own content.
		if (st.type == "drills")
		{
			char buf[120];
			snprintf(buf, sizeof(buf), "%d drill%s  |  %s  |  %s  |  about %.0f min", static_cast<int>(st.drills.size()),
				st.drills.size() == 1 ? "" : "s", st.shuffle ? "shuffled" : "in order", capitalized(st.difficulty).c_str(),
				std::max(1.f, std::round(blockMinutes(m, st))));
			ImGui::TextDisabled("%s", st.drills.empty() ? "No drills yet" : buf);
			// The drills as small pictures with their reps, wrapping.
			ImVec2 pp = ImGui::GetCursorScreenPos();
			const float x0 = pp.x, tw = 92.f, th = 52.f;
			for (const auto& d : st.drills)
			{
				const ExerciseCard* card = findCard(m, d.id);
				if (pp.x + tw > x0 + textW && pp.x > x0)
				{
					pp.x = x0;
					pp.y += th + 6.f;
				}
				const ImVec2 ta = pp, tb(pp.x + tw, pp.y + th);
				const ImageInfo art = card && card->image >= 0 && cb_.image ? cb_.image(card->image) : ImageInfo{};
				if (!drawCover(dl, art, ta, tb, 6.f, IM_COL32(210, 210, 210, 255)))
				{
					if (card)
						drawPreview(dl, ta, tb, card->preview);
					else
						dl->AddRectFilled(ta, tb, theme::u32(pal.bg), 6.f);
				}
				tileFade(dl, ta, tb, 0.75f, 6.f);
				char reps[16];
				snprintf(reps, sizeof(reps), "%dx", d.reps);
				dl->AddText(theme::fontAt(fs * 0.8f), fs * 0.8f, ImVec2(ta.x + 6.f, tb.y - fs * 0.8f - 4.f), IM_COL32(245, 248, 252, 255), reps);
				if (ImGui::IsMouseHoveringRect(ta, tb))
					ImGui::SetTooltip("%s\n%d reps%s", card ? card->name.c_str() : d.id.c_str(), d.reps,
						d.difficulty.empty() ? "" : (", " + d.difficulty).c_str());
				pp.x += tw + 6.f;
			}
			ImGui::SetCursorScreenPos(ImVec2(x0, (st.drills.empty() ? pp.y : pp.y + th + 8.f)));
			if (smallButton(st.drills.empty() ? "+ Add drills" : "Edit drills", Fx::Click))
			{
				blockEditId_ = st.id;
				blockSearch_[0] = 0;
				pageAt_ = ImGui::GetTime();
				sound("ui_page");
			}
		}
		else if (st.type == "freeplay")
		{
			ImGui::TextDisabled("%s", fitText(stepSummary(m, st), textW).c_str());
			if (smallButton("Change"))
			{
				blockEditId_ = st.id;
				pageAt_ = ImGui::GetTime();
				sound("ui_page");
			}
		}
		else if (st.type == "packs" || st.type == "workshop")
		{
			const bool packs = st.type == "packs";
			int itemRemove = -1;
			// A pack already brought into Free Touch (by its code), to play it here with the defender and net options.
			auto importedPack = [&](const std::string& code) -> const ExerciseCard* {
				const std::string want = packs::normalizeCode(code);
				if (want.empty())
					return nullptr;
				for (const auto& c : m.exercises)
					if (c.category == "packs" && packs::codeFromId(c.id) == want)
						return &c;
				return nullptr;
			};
			for (size_t j = 0; j < st.items.size(); ++j)
			{
				routine::Item& it = st.items[j];
				ImGui::PushID(static_cast<int>(j));
				if (routineEditing_)
				{
					ImGui::PushItemWidth(220.f);
					ImGui::InputTextWithHint("##name", packs ? "Pack name" : "Map name", &it.name);
					trackText(true);
					ImGui::PopItemWidth();
					if (packs)
					{
						ImGui::SameLine(0.f, 6.f);
						ImGui::PushItemWidth(200.f);
						ImGui::InputTextWithHint("##code", "XXXX-XXXX-XXXX-XXXX", &it.code);
						trackText(true);
						ImGui::PopItemWidth();
					}
					ImGui::SameLine(0.f, 6.f);
					if (iconButton("i_remove", Glyph::Close, packs ? "Remove this pack" : "Remove this map", true, 26.f))
						itemRemove = static_cast<int>(j);
				}
				else
				{
					// Play it in Free Touch: straight away if it is in, else it is brought in first (custom training
					// opens, every shot is read, then Free Play starts it).
					const ExerciseCard* have = packs ? importedPack(it.code) : nullptr;
					const bool codeOk = packs && !packs::normalizeCode(it.code).empty();
					if (packs && (have || codeOk))
					{
						if (bigButton("pack_play", have ? "Play" : "Get & play", ImVec2(have ? 92.f : 120.f, 34.f), true, Icon::Play, false, Fx::Confirm))
						{
							if (have && cb_.play)
							{
								cb_.play(have->id, st.difficulty.empty() ? "medium" : st.difficulty);
								closeRequested_ = true;
							}
							else if (!have && cb_.importPack && !s.packImporting)
							{
								cb_.importPack(it.code);
							}
						}
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", have ? "Play it in Free Touch (defender and net target from the Training Packs page)."
								: "Not in Free Touch yet: it opens in custom training, every shot is read in a few seconds, then it starts here.");
						ImGui::SameLine(0.f, 10.f);
					}
					ImGui::AlignTextToFramePadding();
					const std::string name = !it.name.empty() ? it.name : have ? (have->name.rfind("Pack: ", 0) == 0 ? have->name.substr(6) : have->name)
						: (packs ? "Training pack" : "Workshop map");
					ImGui::TextUnformatted(name.c_str());
					if (packs && !it.code.empty())
					{
						ImGui::SameLine(0.f, 12.f);
						ImGui::TextColored(pal.accent, "%s", it.code.c_str());
						ImGui::SameLine(0.f, 8.f);
						if (smallButton("Copy code"))
							ImGui::SetClipboardText(it.code.c_str());
					}
				}
				ImGui::PopID();
			}
			if (itemRemove >= 0)
			{
				st.items.erase(st.items.begin() + itemRemove);
				commitRoutine();
			}
			if (routineEditing_)
			{
				if (smallButton(packs ? "+ Add pack" : "+ Add map"))
				{
					st.items.push_back({});
					commitRoutine();
				}
				if (packs)
				{
					// Or one already in Free Touch (Training Packs page).
					std::vector<const ExerciseCard*> mine;
					for (const auto& c : m.exercises)
						if (c.category == "packs" && !packs::codeFromId(c.id).empty())
							mine.push_back(&c);
					if (!mine.empty())
					{
						ImGui::SameLine(0.f, 6.f);
						if (smallButton("+ From my packs"))
							ImGui::OpenPopup("my_packs");
						if (beginMenuBelow("my_packs"))
						{
							for (const ExerciseCard* c : mine)
							{
								const std::string code = packs::codeFromId(c->id);
								const std::string name = c->name.rfind("Pack: ", 0) == 0 ? c->name.substr(6) : c->name;
								if (ImGui::Selectable((name + "##" + c->id).c_str()))
								{
									st.items.push_back({name, code});
									commitRoutine();
								}
							}
							endMenuBelow();
						}
					}
					ImGui::SameLine(0.f, 10.f);
					ImGui::TextDisabled("With a code, Play brings it into Free Touch (defender, net target) the first time.");
				}
			}
			else if (st.items.empty())
			{
				ImGui::TextDisabled(packs ? "No packs yet: press Edit to add codes." : "No maps yet: press Edit to add them.");
			}
		}
		else if (st.type == "task" && routineEditing_)
		{
			if (stepper("tally", "Count to", st.count, 1, 0, 20, tallyText))
				commitRoutine();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("A counter for this step, e.g. 3 games. Off: a single tick.");
			if (navCheckbox("Optional (only counts when done)", &st.optional))
				commitRoutine();
			sameLineIfFits(160.f, 18.f);
			if (navCheckbox("Notes box", &st.notes))
				commitRoutine();
			if (st.notes)
			{
				ImGui::PushItemWidth(std::min(textW, 420.f));
				ImGui::InputTextWithHint("##hint", "Notes prompt, e.g. Mistakes -> fixes", &st.notesHint);
				trackText(true);
				ImGui::PopItemWidth();
			}
		}
		// Notes: a box for today (tasks with notes and notes steps).
		if (!routineEditing_ && (st.type == "notes" || (st.type == "task" && st.notes)))
		{
			std::string& text = day.notes[st.id];
			ImGui::InputTextMultiline("##notes", &text, ImVec2(textW, st.type == "notes" ? 80.f : 58.f));
			trackText(false);
			boxHint(text, st.type == "notes" ? "How did today go?" : st.notesHint.empty() ? "Notes" : st.notesHint.c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		float bottom = std::max(ImGui::GetItemRectMax().y, cp.y + 62.f);

		// Controls on the right.
		if (routineEditing_)
		{
			// Everything else about the step in one menu: copy, move to another section, delete (asks first).
			ImGui::SetCursorScreenPos(ImVec2(right - 30.f, cp.y + 10.f));
			if (iconButton("more", Glyph::More, "More: copy, move, delete"))
			{
				confirmStep_.clear();
				ImGui::OpenPopup("step_menu");
			}
			if (beginMenuBelow("step_menu"))
			{
				if (confirmStep_ == st.id)
				{
					ImGui::TextUnformatted("Delete this step?");
					if (ImGui::MenuItem("Delete"))
					{
						removeAt = static_cast<int>(i);
						ImGui::CloseCurrentPopup();
					}
					if (ImGui::MenuItem("Keep it"))
						confirmStep_.clear();
				}
				else
				{
					if (ImGui::MenuItem("Duplicate"))
						duplicateAt = static_cast<int>(i);
					if (r.sections.size() > 1 && ImGui::BeginMenu("Move to section"))
					{
						for (const auto& o : r.sections)
							if (o.id != sec.id && ImGui::MenuItem((o.title.empty() ? std::string("Section") : o.title).c_str()))
							{
								moveFrom = static_cast<int>(i);
								moveSection = o.id;
							}
						ImGui::EndMenu();
					}
					ImGui::Separator();
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.5f, 0.5f, 1.f));
					if (ImGui::Selectable("Delete step...", false, ImGuiSelectableFlags_DontClosePopups))
						confirmStep_ = st.id;
					ImGui::PopStyleColor();
				}
				endMenuBelow();
			}
		}
		else if (st.type == "drills" || st.type == "freeplay")
		{
			ImGui::SetCursorScreenPos(ImVec2(right - controlsW, cp.y + 16.f));
			if (bigButton("start", done ? "Again" : "Start", ImVec2(controlsW, 44.f), !done, Icon::Play, true, Fx::Confirm)
				&& (st.type == "freeplay" || !st.drills.empty()) && cb_.startRoutineStep)
			{
				cb_.startRoutineStep(st.id);
				closeRequested_ = true;
			}
			if (!s.inFreeplay)
			{
				const char* note = s.startPending ? "Loading Free Play..." : "Loads Free Play";
				const ImVec2 ts = ImGui::CalcTextSize(note);
				dl->AddText(ImVec2(right - ts.x, cp.y + 66.f), theme::u32(pal.textDim), note);
				bottom = std::max(bottom, cp.y + 66.f + ts.y);
			}
		}
		else if (st.count > 0)
		{
			// Tally: - 1 / 3 +
			int& tally = day.tally[st.id];
			ImGui::SetCursorScreenPos(ImVec2(right - controlsW, cp.y + 18.f));
			if (smallButton("-") && tally > 0)
			{
				--tally;
				commitDay();
			}
			char buf[32];
			snprintf(buf, sizeof(buf), "%d / %d", std::min(tally, 99), st.count);
			const ImVec2 ts = theme::fontAt(fs * 1.3f)->CalcTextSizeA(fs * 1.3f, FLT_MAX, 0.f, buf);
			const float mid = right - controlsW * 0.5f;
			dl->AddText(theme::fontAt(fs * 1.3f), fs * 1.3f, ImVec2(mid - ts.x * 0.5f, cp.y + 16.f), theme::u32(tally >= st.count ? pal.accent : pal.text), buf);
			ImGui::SetCursorScreenPos(ImVec2(right - 30.f, cp.y + 18.f));
			if (smallButton("+", Fx::Confirm))
			{
				++tally;
				commitDay();
			}
		}

		const ImVec2 e(cp.x + width, bottom + 14.f);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(cp, e, theme::u32(done && !routineEditing_ ? theme::mix(pal.card, pal.accent, 0.05f) : pal.card), 12.f);
		if (done && !routineEditing_)
			dl->AddRectFilled(cp, ImVec2(cp.x + 4.f, e.y), theme::u32(pal.accent, 0.8f), 12.f, ImDrawCornerFlags_Left);
		dl->AddRect(cp, e, IM_COL32(255, 255, 255, 14), 12.f);
		dl->ChannelsMerge();
		ImGui::SetCursorScreenPos(ImVec2(cp.x, e.y + 10.f));
		ImGui::PopID();
	}
	if (routineEditing_)
	{
		// Add a step to this section.
		static const std::pair<const char*, const char*> kAdd[] = {{"drills", "+ Drills"}, {"packs", "+ Training packs"},
			{"workshop", "+ Workshop maps"}, {"task", "+ Task"}, {"notes", "+ Notes"}};
		for (size_t k = 0; k < std::size(kAdd); ++k)
		{
			if (k > 0)
				sameLineIfFits(buttonWidth(kAdd[k].second), 6.f);
			if (smallButton(kAdd[k].second))
			{
				// After the section's last step.
				auto at = r.steps.end();
				for (auto it = r.steps.begin(); it != r.steps.end(); ++it)
					if (it->section == sec.id)
						at = it + 1;
				if (at == r.steps.end())
					r.steps.push_back(routine::makeStep(r, kAdd[k].first, sec.id));
				else
					r.steps.insert(at, routine::makeStep(r, kAdd[k].first, sec.id));
				commitRoutine();
				break; // the list changed
			}
		}
		ImGui::Dummy(ImVec2(0, 6));
	}
	else if (number == 0)
	{
		ImGui::TextDisabled("Nothing here yet: press Edit to add steps.");
	}
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::PopID();
	}
	if (moveFrom >= 0 && !moveSection.empty())
	{
		// To the end of the other section.
		routine::Step moved = r.steps[moveFrom];
		moved.section = moveSection;
		r.steps.erase(r.steps.begin() + moveFrom);
		auto at = r.steps.end();
		for (auto it = r.steps.begin(); it != r.steps.end(); ++it)
			if (it->section == moveSection)
				at = it + 1;
		r.steps.insert(at, moved);
		commitRoutine();
	}
	else if (moveFrom >= 0 && moveTo >= 0)
	{
		std::swap(r.steps[moveFrom], r.steps[moveTo]);
		commitRoutine();
	}
	if (duplicateAt >= 0)
	{
		routine::Step copy = r.steps[duplicateAt];
		copy.id = routine::newStepId(r);
		copy.title += " (copy)";
		r.steps.insert(r.steps.begin() + duplicateAt + 1, copy);
		commitRoutine();
	}
	if (removeAt >= 0)
	{
		r.steps.erase(r.steps.begin() + removeAt);
		confirmStep_.clear();
		commitRoutine();
	}
	if (sectionMove >= 0)
	{
		std::swap(r.sections[sectionMove], r.sections[sectionMove + sectionBy]);
		commitRoutine();
	}
	if (sectionRemove >= 0)
	{
		const std::string gone = r.sections[sectionRemove].id;
		r.steps.erase(std::remove_if(r.steps.begin(), r.steps.end(), [&](const routine::Step& x) { return x.section == gone; }),
			r.steps.end());
		r.sections.erase(r.sections.begin() + sectionRemove);
		confirmStep_.clear();
		commitRoutine();
	}

	if (routineEditing_)
	{
		ImGui::TextDisabled("Tip: tasks can count games (Tally) and have a notes box. Drill blocks run in Free Touch and tick "
			"themselves when you finish them.");
	}
	else
	{
		ImGui::TextDisabled("Tick steps as you go: Free Touch drill blocks tick themselves when you finish them. "
			"Notes are kept per day.");
	}
	ImGui::Dummy(ImVec2(0, 8));
	dayTextActive_ = textActive;
}

void Dashboard::renderRoutineTemplates(const DashboardModel& m, bool replacing)
{
	// Starting points (content/routines/*.json): pick one, then make it yours on the Routine page.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const float width = ImGui::GetContentRegionAvail().x;
	pageTitle(replacing ? "Start from a template" : "Build your routine", nullptr);
	ImGui::TextDisabled("%s", replacing
		? "This replaces your current routine. Today's ticks and notes are kept."
		: "Your day of Rocket League as a checklist: a warm-up, your games and what comes after. Pick a starting point; "
		  "you can change everything after.");
	if (replacing)
	{
		if (navButton("Cancel"))
			pickingTemplate_ = false;
	}
	ImGui::Dummy(ImVec2(0, 8));
	if (m.routineTemplates.empty())
	{
		ImGui::TextDisabled("No templates found (content/routines/).");
		return;
	}
	const float gap = 14.f;
	const int columns = width >= 760.f ? 3 : width >= 480.f ? 2 : 1;
	const float cw = (width - gap * (columns - 1)) / columns;
	const float pad = 18.f;
	auto sectionsLine = [](const routine::Routine& r) {
		std::string line;
		for (const auto& s : r.sections)
			line += (line.empty() ? "" : "  >  ") + s.title;
		return line;
	};
	auto statsLine = [](const routine::Routine& r) {
		int minutes = 0;
		for (const auto& s : r.steps)
			minutes += s.minutes;
		std::string line = std::to_string(r.steps.size()) + (r.steps.size() == 1 ? " step" : " steps");
		if (minutes > 0)
			line += "  |  about " + std::to_string(minutes) + " min";
		return line;
	};
	auto cardHeight = [&](const routine::Template& t) {
		float h = pad + fs * 1.25f + 8.f;
		h += theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, cw - 2 * pad, t.description.c_str()).y + 12.f;
		h += theme::fontAt(fs * 0.85f)->CalcTextSizeA(fs * 0.85f, FLT_MAX, cw - 2 * pad, sectionsLine(t.routine).c_str()).y + 4.f;
		h += fs + 14.f + 44.f + pad;
		return h;
	};
	ImVec2 row = ImGui::GetCursorScreenPos();
	const auto& list = m.routineTemplates;
	for (size_t first = 0; first < list.size(); first += columns)
	{
		const size_t last = std::min(list.size(), first + columns);
		float rowH = 0.f;
		for (size_t k = first; k < last; ++k)
			rowH = std::max(rowH, cardHeight(list[k]));
		for (size_t k = first; k < last; ++k)
		{
			const routine::Template& t = list[k];
			const ImVec2 p(row.x + (k - first) * (cw + gap), row.y);
			const ImVec2 e(p.x + cw, p.y + rowH);
			dl->AddRectFilled(p, e, theme::u32(pal.card), 14.f);
			dl->AddRect(p, e, IM_COL32(255, 255, 255, 18), 14.f);
			float y = p.y + pad;
			dl->AddText(theme::fontAt(fs * 1.25f), fs * 1.25f, ImVec2(p.x + pad, y), theme::u32(pal.text), t.name.c_str());
			y += fs * 1.25f + 8.f;
			dl->AddText(theme::fontAt(fs), fs, ImVec2(p.x + pad, y), theme::u32(pal.textDim), t.description.c_str(), nullptr, cw - 2 * pad);
			y += theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, cw - 2 * pad, t.description.c_str()).y + 12.f;
			const std::string sections = sectionsLine(t.routine);
			dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(p.x + pad, y), theme::u32(pal.accent), sections.c_str(), nullptr, cw - 2 * pad);
			y += theme::fontAt(fs * 0.85f)->CalcTextSizeA(fs * 0.85f, FLT_MAX, cw - 2 * pad, sections.c_str()).y + 4.f;
			const std::string stats = statsLine(t.routine);
			dl->AddText(ImVec2(p.x + pad, y), theme::u32(pal.textDim), stats.c_str());
			ImGui::SetCursorScreenPos(ImVec2(p.x + pad, e.y - pad - 44.f));
			ImGui::PushID(t.id.c_str());
			if (bigButton("use", "Use this", ImVec2(cw - 2 * pad, 44.f), true, Icon::Play, false, Fx::Confirm) && cb_.saveRoutine)
			{
				routineDraft_ = routine::fromTemplate(t.routine);
				routineChangedAt_ = ImGui::GetTime();
				cb_.saveRoutine(routineDraft_);
				toast("Routine set up from \"" + t.name + "\"");
				pickingTemplate_ = false;
				routineEditing_ = false;
			}
			ImGui::PopID();
		}
		row.y += rowH + gap;
	}
	ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 4.f));
	ImGui::TextDisabled("Templates live in content/routines/ as JSON: add your own there.");
}

void Dashboard::renderFreeplayPlus(const DashboardModel& m)
{
	const theme::Palette& pal = theme::palette();
	if (m.freeplayChoices.empty())
	{
		pageTitle("Freeplay+", nullptr);
		ImGui::TextDisabled("Freeplay+ is not loaded (content/scenarios/speed_lab/objectives.json).");
		return;
	}
	auto isOn = [&](const std::string& id) {
		return m.freeplayEnabled.empty() || std::find(m.freeplayEnabled.begin(), m.freeplayEnabled.end(), id) != m.freeplayEnabled.end();
	};
	std::vector<std::string> on;
	for (const auto& c : m.freeplayChoices)
		if (isOn(c.id))
			on.push_back(c.id);
	auto save = [&](std::vector<std::string> ids) {
		if (!ids.empty() && cb_.saveFreeplayPlus)
			cb_.saveFreeplayPlus(ids);
	};
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const float width = ImGui::GetContentRegionAvail().x;
	const float now = static_cast<float>(ImGui::GetTime());

	// Banner: what it is, how many objectives are on, Start.
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, std::clamp(width * 0.22f, 170.f, 220.f));
		const ImVec2 a = pos, b = add(pos, size);
		const ExerciseCard* card = findCard(m, m.freeplayId);
		const ImageInfo art = card && card->image >= 0 && cb_.image ? cb_.image(card->image) : ImageInfo{};
		const float drift = 0.04f + 0.035f * (0.5f + 0.5f * std::sin(now * 0.22f));
		if (!drawCover(dl, art, a, b, 14.f, IM_COL32(235, 235, 235, 255), drift))
			dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.22f)), 14.f);
		const ImU32 shade = IM_COL32(5, 8, 15, 235), clear = IM_COL32(5, 8, 15, 0);
		roundedGradient(dl, a, ImVec2(a.x + width * 0.7f, b.y), shade, clear, clear, shade, 14.f, ImDrawCornerFlags_Left);
		dl->AddRect(a, b, theme::u32(pal.accent, 0.3f), 14.f, ImDrawCornerFlags_All, 1.2f);
		shadowedText(dl, font, fs * 0.9f, ImVec2(a.x + 28.f, a.y + 24.f), theme::u32(pal.accent), "FREEPLAY WITH INTENT");
		shadowedText(dl, font, fs * 2.1f, ImVec2(a.x + 28.f, a.y + 24.f + fs * 1.3f), IM_COL32(250, 252, 255, 255), "Freeplay+");
		shadowedText(dl, font, fs, ImVec2(a.x + 28.f, a.y + 24.f + fs * 3.6f), IM_COL32(205, 214, 228, 235),
			"Objectives come one at a time. The ball and your car never reset.");
		char count[64];
		snprintf(count, sizeof(count), "%d of %d objectives on", static_cast<int>(on.size()), static_cast<int>(m.freeplayChoices.size()));
		const ImVec2 cs = ImGui::CalcTextSize(count);
		darkPill(dl, ImVec2(b.x - cs.x - 34.f, a.y + 16.f), count, theme::u32(pal.accent));
		ImGui::SetCursorScreenPos(ImVec2(a.x + 28.f, b.y - 22.f - 46.f));
		if (bigButton("fpp_start", "Start", ImVec2(160, 46), true, Icon::Play, true, Fx::Confirm) && cb_.play)
		{
			cb_.play(m.freeplayId, kDifficulties[difficulty_]);
			closeRequested_ = true;
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x, b.y + 16.f));
	}

	// Options: difficulty and boost, side by side (stacked in a narrow window).
	{
		const float gap = 16.f;
		const bool stacked = width < 640.f;
		const float cw = stacked ? width : (width - gap) * 0.5f;
		const ImVec2 row = ImGui::GetCursorScreenPos();
		float bottom = row.y;
		for (int k = 0; k < 2; ++k)
		{
			const ImVec2 p(stacked ? row.x : row.x + k * (cw + gap), stacked && k == 1 ? bottom + gap : row.y);
			dl->ChannelsSplit(2);
			dl->ChannelsSetCurrent(1);
			ImGui::SetCursorScreenPos(ImVec2(p.x + 18.f, p.y + 14.f));
			ImGui::BeginGroup();
			if (k == 0)
			{
				ImGui::TextColored(pal.accent, "DIFFICULTY");
				difficulty_ = segmented("fpp_diff", kDifficultyLabels, 3, difficulty_, cw - 36.f, diffAnim_);
				ImGui::TextDisabled("%s", difficulty_ == 0 ? "Bigger objectives and a guide line from the ball."
					: difficulty_ == 2 ? "Smaller objectives, higher touches, less time." : "The standard sizes.");
			}
			else
			{
				ImGui::TextColored(pal.accent, "BOOST");
				static const char* kBoost[] = {"Unlimited", "Standard"};
				const int cur = m.freeplayUnlimitedBoost ? 0 : 1;
				const int pick = segmented("fpp_boost", kBoost, 2, cur, cw - 36.f, boostAnim_);
				if (pick != cur && cb_.freeplayBoostChanged)
					cb_.freeplayBoostChanged(pick == 0);
				ImGui::TextDisabled("%s", cur == 0 ? "Your tank refills all the time." : "Your tank works as normal: boost pads refill it.");
			}
			ImGui::EndGroup();
			const ImVec2 e(p.x + cw, ImGui::GetItemRectMax().y + 14.f);
			dl->ChannelsSetCurrent(0);
			dl->AddRectFilled(p, e, theme::u32(pal.card), 12.f);
			dl->AddRect(p, e, IM_COL32(255, 255, 255, 14), 12.f);
			dl->ChannelsMerge();
			bottom = std::max(bottom, e.y);
		}
		ImGui::SetCursorScreenPos(ImVec2(row.x, bottom + 18.f));
	}

	// Presets: one tile each; the one matching what is on is lit.
	std::vector<std::string> groups;
	for (const auto& c : m.freeplayChoices)
		if (std::find(groups.begin(), groups.end(), c.group) == groups.end())
			groups.push_back(c.group);
	{
		ImGui::TextColored(pal.accent, "PRESETS");
		std::vector<std::pair<std::string, std::vector<std::string>>> presets;
		std::vector<std::string> all, beginner;
		for (const auto& c : m.freeplayChoices)
		{
			all.push_back(c.id);
			if (c.beginner)
				beginner.push_back(c.id);
		}
		presets.push_back({"Everything", all});
		presets.push_back({"Beginner", beginner});
		for (const auto& g : groups)
		{
			std::vector<std::string> ids;
			for (const auto& c : m.freeplayChoices)
				if (c.group == g)
					ids.push_back(c.id);
			presets.push_back({g, ids});
		}
		const float gap = 10.f;
		const int n = static_cast<int>(presets.size());
		const int perRow = std::clamp(static_cast<int>((width + gap) / (140.f + gap)), 2, n);
		const int rows = (n + perRow - 1) / perRow;
		const float tw = (width - gap * (perRow - 1)) / perRow;
		const ImVec2 row = ImGui::GetCursorScreenPos();
		auto sameSet = [&](std::vector<std::string> a) {
			std::vector<std::string> b = on;
			std::sort(a.begin(), a.end());
			std::sort(b.begin(), b.end());
			return a == b;
		};
		for (int k = 0; k < n; ++k)
		{
			const ImVec2 p(row.x + (k % perRow) * (tw + gap), row.y + (k / perRow) * (62.f + gap));
			ImGui::SetCursorScreenPos(p);
			ImGui::PushID(600 + k);
			if (navInvisible("preset", ImVec2(tw, 62.f), 1, Fx::Toggle))
				save(presets[k].second);
			const bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();
			const bool lit = sameSet(presets[k].second);
			float& h = hoverAnim_["fpp.preset." + presets[k].first];
			h = approach(h, hovered ? 1.f : 0.f, 8.f);
			const ImVec2 e(p.x + tw, p.y + 62.f);
			dl->AddRectFilled(p, e, theme::u32(lit ? theme::mix(pal.card, pal.accent, 0.3f) : theme::mix(pal.card, pal.cardHover, h)), 10.f);
			dl->AddRect(p, e, lit ? theme::u32(pal.accent) : theme::u32(theme::mix(ImVec4(1, 1, 1, 0.06f), pal.accent, h)), 10.f,
				ImDrawCornerFlags_All, lit ? 2.f : 1.f);
			const std::string name = fitText(presets[k].first, tw - 20.f);
			dl->AddText(ImVec2(p.x + 12.f, p.y + 12.f), theme::u32(pal.text), name.c_str());
			char sub[32];
			snprintf(sub, sizeof(sub), "%d objective%s", static_cast<int>(presets[k].second.size()),
				presets[k].second.size() == 1 ? "" : "s");
			dl->AddText(theme::fontAt(fs * 0.82f), fs * 0.82f, ImVec2(p.x + 12.f, p.y + 36.f), theme::u32(lit ? pal.accent : pal.textDim), sub);
		}
		ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + rows * (62.f + gap) - gap + 20.f));
	}

	// Objectives: a switch card each, by group.
	const float gap = 12.f;
	const int columns = std::max(1, static_cast<int>((width + gap) / (230.f + gap)));
	const float cw = (width - gap * (columns - 1)) / columns;
	for (const auto& g : groups)
	{
		int total = 0, lit = 0;
		for (const auto& c : m.freeplayChoices)
			if (c.group == g)
			{
				++total;
				lit += isOn(c.id) ? 1 : 0;
			}
		std::string head = g;
		for (auto& c : head)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		ImGui::TextColored(pal.accent, "%s", head.c_str());
		ImGui::SameLine(0.f, 8.f);
		ImGui::TextDisabled("%d of %d on", lit, total);
		ImGui::SameLine(0.f, 14.f);
		ImGui::PushID(g.c_str());
		if (smallButton("All on"))
		{
			std::vector<std::string> ids = on;
			for (const auto& c : m.freeplayChoices)
				if (c.group == g && !isOn(c.id))
					ids.push_back(c.id);
			save(ids);
		}
		ImGui::SameLine(0.f, 4.f);
		if (smallButton("All off"))
		{
			std::vector<std::string> ids;
			for (const auto& id : on)
			{
				const auto it = std::find_if(m.freeplayChoices.begin(), m.freeplayChoices.end(), [&](const FreeplayChoice& c) { return c.id == id; });
				if (it != m.freeplayChoices.end() && it->group != g)
					ids.push_back(id);
			}
			save(ids); // ignored when it would switch off everything
		}
		ImGui::PopID();
		ImGui::Dummy(ImVec2(0, 2));
		// Cards in rows; each row as tall as its tallest card (titles and descriptions wrap, nothing is cut).
		std::vector<const FreeplayChoice*> cards;
		for (const auto& c : m.freeplayChoices)
			if (c.group == g)
				cards.push_back(&c);
		const float pad = 14.f;
		const float switchW = 40.f;
		auto cardHeight = [&](const FreeplayChoice& c) {
			const std::string title = capitalized(c.label);
			float h = pad + theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, cw - 2 * pad - switchW - 8.f, title.c_str()).y + 6.f;
			if (!c.about.empty())
				h += theme::fontAt(fs * 0.85f)->CalcTextSizeA(fs * 0.85f, FLT_MAX, cw - 2 * pad, c.about.c_str()).y;
			if (c.beginner)
				h += 8.f + fs * 0.75f + 6.f;
			return std::max(h + pad, 80.f);
		};
		ImVec2 row = ImGui::GetCursorScreenPos();
		for (size_t first = 0; first < cards.size(); first += columns)
		{
			const size_t last = std::min(cards.size(), first + columns);
			float rowH = 0.f;
			for (size_t k = first; k < last; ++k)
				rowH = std::max(rowH, cardHeight(*cards[k]));
			for (size_t k = first; k < last; ++k)
			{
				const FreeplayChoice& c = *cards[k];
				const bool active = isOn(c.id);
				const ImVec2 p(row.x + (k - first) * (cw + gap), row.y);
				ImGui::SetCursorScreenPos(p);
				ImGui::PushID(c.id.c_str());
				if (navInvisible("obj", ImVec2(cw, rowH), 1, Fx::Toggle))
				{
					std::vector<std::string> ids = on;
					if (active)
						ids.erase(std::remove(ids.begin(), ids.end(), c.id), ids.end());
					else
						ids.push_back(c.id);
					save(ids); // never empty: the last one cannot be switched off
				}
				const bool hovered = ImGui::IsItemHovered();
				ImGui::PopID();
				float& h = hoverAnim_["fpp.obj." + c.id];
				h = approach(h, hovered ? 1.f : 0.f, 8.f);
				float& sw = hoverAnim_["fpp.sw." + c.id];
				sw = approach(sw, active ? 1.f : 0.f, 7.f);
				const ImVec2 e(p.x + cw, p.y + rowH);
				dl->AddRectFilled(p, e, theme::u32(theme::mix(theme::mix(pal.card, pal.cardHover, h), pal.accent, 0.1f * sw)), 12.f);
				dl->AddRect(p, e, theme::u32(theme::mix(ImVec4(1, 1, 1, 0.06f), pal.accent, 0.6f * sw + 0.3f * h)), 12.f,
					ImDrawCornerFlags_All, 1.f + sw);
				// Switch in the top-right corner; the title wraps beside it.
				const ImVec2 s0(e.x - pad - 36.f, p.y + pad), s1(e.x - pad, p.y + pad + 20.f);
				dl->AddRectFilled(s0, s1, theme::u32(theme::mix(ImVec4(0.25f, 0.28f, 0.35f, 1.f), pal.accent, sw)), 10.f);
				dl->AddCircleFilled(ImVec2(s0.x + 10.f + 16.f * sw, (s0.y + s1.y) * 0.5f), 8.f, IM_COL32(245, 248, 252, 255), 20);
				const std::string title = capitalized(c.label);
				const float titleWrap = cw - 2 * pad - switchW - 8.f;
				dl->AddText(theme::fontAt(fs), fs, ImVec2(p.x + pad, p.y + pad), theme::u32(active ? pal.text : pal.textDim), title.c_str(),
					nullptr, titleWrap);
				float y = p.y + pad + theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, titleWrap, title.c_str()).y + 6.f;
				if (!c.about.empty())
				{
					dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(p.x + pad, y), theme::u32(pal.textDim, active ? 1.f : 0.7f), c.about.c_str(),
						nullptr, cw - 2 * pad);
					y += theme::fontAt(fs * 0.85f)->CalcTextSizeA(fs * 0.85f, FLT_MAX, cw - 2 * pad, c.about.c_str()).y;
				}
				if (c.beginner)
				{
					// A small tag along the bottom, clear of everything else.
					const char* tag = "BEGINNER";
					const ImVec2 ts = theme::fontAt(fs * 0.75f)->CalcTextSizeA(fs * 0.75f, FLT_MAX, 0.f, tag);
					const ImVec2 t0(p.x + pad, e.y - pad - ts.y - 4.f);
					dl->AddRect(t0, ImVec2(t0.x + ts.x + 12.f, t0.y + ts.y + 4.f), theme::u32(pal.accent, 0.7f), 8.f);
					dl->AddText(theme::fontAt(fs * 0.75f), fs * 0.75f, ImVec2(t0.x + 6.f, t0.y + 2.f), theme::u32(pal.accent), tag);
				}
			}
			row.y += rowH + gap;
		}
		ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 10.f));
	}
	ImGui::TextDisabled("D-pad right skips an objective. Your choices are saved and used whenever Freeplay+ runs.");
	ImGui::Dummy(ImVec2(0, 8));
}

void Dashboard::renderFirstRun()
{
	// First time here: three steps to get going.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	// Content first (channel 1), then the card behind it (channel 0) sized to what was drawn.
	dl->ChannelsSplit(2);
	dl->ChannelsSetCurrent(1);
	ImGui::SetCursorScreenPos(ImVec2(pos.x + 20, pos.y + 16));
	ImGui::BeginGroup();
	ImGui::TextColored(pal.accent, "MAKE IT YOURS");
	ImGui::TextUnformatted("Three steps to get going. Works with the mouse or the controller (D-pad, A, B).");
	ImGui::Dummy(ImVec2(0, 6));
	const bool twoRows = width < 820.f;
	const float bw = twoRows ? (width - 40.f - 12.f) / 2.f : (width - 40.f - 3 * 12.f) / 4.f;
	if (navButton("1  Set up your controls", ImVec2(bw, 44)))
		page_ = Page::Settings;
	ImGui::SameLine(0.f, 12.f);
	if (navButton("2  Try Freeplay+", ImVec2(bw, 44)))
		page_ = Page::FreeplayPlus;
	if (!twoRows)
		ImGui::SameLine(0.f, 12.f);
	if (navButton("3  Back to the field", ImVec2(bw, 44)))
		closeRequested_ = true;
	ImGui::SameLine(0.f, 12.f);
	if (navButton("Got it", ImVec2(bw, 44)) && cb_.dismissFirstRun)
		cb_.dismissFirstRun();
	ImGui::EndGroup();
	const ImVec2 size(width, ImGui::GetItemRectMax().y - pos.y + 18.f);
	dl->ChannelsSetCurrent(0);
	dl->AddRectFilled(pos, add(pos, size), theme::u32(theme::mix(pal.card, pal.accent, 0.12f)), 12.f);
	dl->AddRect(pos, add(pos, size), theme::u32(pal.accent, 0.8f), 12.f, ImDrawCornerFlags_All, 1.5f);
	dl->ChannelsMerge();
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 16));
}

void Dashboard::renderSavedShots(const DashboardModel& m, const DashboardStatus& s)
{
	pageTitle("Saved Shots", "Moments you saved in freeplay and recordings from the Record page. Star them, play them, share "
		"them, or add them to your warm-up.");
	renderInbox(s);
	if (m.savedShots.empty())
	{
		ImGui::TextDisabled("Nothing saved yet. Record a play on the Record page, or set up a moment in freeplay and run "
			"ft_capture in the console. It shows up here.");
		return;
	}
	renderShotList(m, s);
}

void Dashboard::renderInbox(const DashboardStatus& s)
{
	// Shots friends shared (.ftshot files Discord saved in Downloads), to add with one click.
	// Lambourne 2026-10-29: "I don't see the importer": the card is always there, with Browse for a file anywhere.
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float width = ImGui::GetContentRegionAvail().x;
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
		ImGui::TextUnformatted("SHARED WITH YOU");
		ImGui::PopStyleColor();
		const float bw = 150.f * u, gap = 8.f * u;
		ImGui::SetCursorScreenPos(ImVec2(pos.x + width - 2.f * bw - gap, pos.y - 4.f * u));
		if (navButton("Check Downloads", ImVec2(bw, 30.f * u)) && cb_.scanInbox)
		{
			cb_.scanInbox();
			toast("Looked in Downloads");
		}
		ImGui::SameLine(0.f, gap);
		if (navButton("Browse...", ImVec2(bw, 30.f * u)) && cb_.browseSharedShot)
			cb_.browseSharedShot();
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Pick a .ftshot file from anywhere on your PC");
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + 34.f * u));
	}
	// Shots friends sent through the service: first, newest first.
	const bool anyOnline = !s.shots.inbox.empty();
	for (const auto& i : s.shots.inbox)
	{
		ImGui::PushID(i.delivery.c_str());
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 56.f * u);
		const bool have = s.haveKeys && s.haveKeys->count(i.key) > 0;
		dl->AddRectFilled(pos, add(pos, size), theme::u32(theme::mix(pal.card, pal.accent, have ? 0.0f : 0.10f)), 10.f);
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.f * u, pos.y + 8.f * u));
		ImGui::BeginGroup();
		const float textW = std::max(60.f, size.x - 290.f * u);
		ImGui::TextUnformatted(fitText(i.name, textW).c_str());
		ImGui::TextDisabled("%s", fitText("sent by " + i.from + ", " + Friends::ago(i.ageSeconds), textW).c_str());
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - 260.f * u, pos.y + 11.f * u));
		if (bigButton("add_online", have ? "Play" : "Add it", ImVec2(134.f * u, 34.f * u), true, Icon::Play, false, Fx::Confirm))
		{
			if (have)
				openPlaySetup(true, i.shotId, i.delivery, i.name, {});
			else if (cb_.addOnlineShot)
				cb_.addOnlineShot(i.shotId, i.delivery, false, false, {}, "");
		}
		ImGui::SameLine(0.f, 8.f * u);
		if (navButton("Dismiss", ImVec2(100.f * u, 34.f * u)) && cb_.dismissOnlineShot)
			cb_.dismissOnlineShot(i.delivery);
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.f * u));
		ImGui::PopID();
	}
	if (!s.inbox || s.inbox->empty())
	{
		if (anyOnline)
		{
			ImGui::Dummy(ImVec2(0, 6.f * u));
			return;
		}
		const char* hint = "Got a shot from a friend? Download the .ftshot file from Discord and it shows up here (Free Touch "
			"looks in your Downloads folder), or press Browse to pick it.";
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float wrap = width - 32.f * u;
		const ImVec2 size(width, ImGui::CalcTextSize(hint, nullptr, false, wrap).y + 28.f * u);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(pal.card), 10.f);
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.f * u, pos.y + 14.f * u));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap); // window-local
		ImGui::TextDisabled("%s", hint);
		ImGui::PopTextWrapPos();
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 14.f * u));
		return;
	}
	for (const auto& f : *s.inbox)
	{
		ImGui::PushID(f.path.c_str());
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 56.f * u);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(theme::mix(pal.card, pal.accent, f.imported ? 0.0f : 0.10f)), 10.f);
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.f * u, pos.y + 8.f * u));
		ImGui::BeginGroup();
		const float textW = std::max(60.f, size.x - 190.f * u);
		const std::string name = fitText(f.name, textW);
		ImGui::TextUnformatted(name.c_str());
		const std::string from = fitText("from " + f.by + (f.on.empty() ? "" : ", " + f.on), textW);
		ImGui::TextDisabled("%s", from.c_str());
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - 150.f * u, pos.y + 11.f * u));
		if (f.imported)
		{
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f); // already a Saved Shot
			ImGui::Button("Added", ImVec2(134.f * u, 34.f * u));
			ImGui::PopStyleVar();
		}
		else if (bigButton("import", "Add it", ImVec2(134.f * u, 34.f * u), true, Icon::Play, false, Fx::Confirm) && cb_.importSharedShot)
		{
			cb_.importSharedShot(f.path);
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.f * u));
		ImGui::PopID();
	}
	ImGui::Dummy(ImVec2(0, 6.f * u));
}

uint32_t Dashboard::netTilePicker(uint32_t open, int cols, bool hole, float width)
{
	// The orange net as the player sees it from the field (the game's x runs right to left, so screen column j is the
	// game's column cols - 1 - j), 2 rows: a click opens or closes a tile (closed: the packs' grey blocker), or, for
	// the hole, moves it there. The last open tile can't be closed; 0 shows every tile open.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const int rows = 2;
	const uint32_t all = (1u << (cols * rows)) - 1;
	const uint32_t shown = open == 0 ? (hole ? 1u : all) : open;
	const float w = width, h = std::max(70.f, width * 643.f / 1786.f);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(6, 9, 16, 210), 6.f);
	const float cw = w / cols, ch = h / rows;
	uint32_t next = shown;
	for (int r = 0; r < rows; ++r)
	{
		for (int j = 0; j < cols; ++j)
		{
			const int cell = (rows - 1 - r) * cols + (cols - 1 - j);
			const bool isOpen = (shown >> cell) & 1u;
			const ImVec2 a(p.x + j * cw, p.y + r * ch), b(a.x + cw, a.y + ch);
			ImGui::SetCursorScreenPos(a);
			ImGui::PushID("net_tile");
			ImGui::PushID(cell);
			const bool clicked = navInvisible("tile", ImVec2(cw, ch), 1, Fx::Toggle);
			const bool hovered = ImGui::IsItemHovered();
			ImGui::PopID();
			ImGui::PopID();
			if (clicked)
			{
				if (hole)
					next = 1u << cell;
				else if (const uint32_t flipped = shown ^ (1u << cell); flipped != 0)
					next = flipped;
			}
			const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
			if (hole)
			{
				// A ring where the hole is (the rest of the net is closed); faint rings on the other spots.
				const float rr = std::min(cw, ch) * 0.24f;
				dl->AddCircle(c, rr, isOpen ? IM_COL32(255, 205, 60, 255) : IM_COL32(255, 255, 255, hovered ? 120 : 45), 32,
					isOpen ? 3.f : 1.5f);
				if (isOpen)
					dl->AddCircleFilled(c, rr - 4.f, IM_COL32(255, 205, 60, 110), 32);
				continue;
			}
			const ImVec2 a2(a.x + 3.f, a.y + 3.f), b2(b.x - 3.f, b.y - 3.f);
			if (isOpen)
			{
				// Gold, as the open tiles are drawn on the net in game.
				dl->AddRectFilled(a2, b2, IM_COL32(255, 205, 60, hovered ? 130 : 95), 4.f);
				dl->AddRect(a2, b2, IM_COL32(255, 205, 60, 255), 4.f, 0, 2.5f);
			}
			else
			{
				dl->AddRectFilled(a2, b2, IM_COL32(120, 126, 136, hovered ? 200 : 170), 4.f);
				for (int k = 1; k < 4; ++k) // hatching: closed
				{
					const float t = k / 4.f;
					dl->AddLine(ImVec2(a2.x + (b2.x - a2.x) * t, a2.y), ImVec2(a2.x, a2.y + (b2.y - a2.y) * t), IM_COL32(70, 74, 82, 200), 1.5f);
					dl->AddLine(ImVec2(b2.x, a2.y + (b2.y - a2.y) * t), ImVec2(a2.x + (b2.x - a2.x) * t, b2.y), IM_COL32(70, 74, 82, 200), 1.5f);
				}
			}
		}
	}
	if (hole)
	{
		// The rest is closed: a faint grey over the mouth.
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::u32(pal.textDim, 0.08f), 6.f);
	}
	// The posts and crossbar.
	dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x, p.y), IM_COL32(255, 255, 255, 200), 3.f);
	dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 200), 3.f);
	dl->AddLine(ImVec2(p.x + w, p.y), ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 200), 3.f);
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 6.f));
	return next;
}

void Dashboard::renderShotEditor(const SavedShot& shot, const DashboardStatus& s, float width)
{
	// Renaming a saved shot (its defender and net are picked when it's played: renderPlaySetup).
	(void)s;
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float u = uiScale();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float pad = 18.f * u;
	const ImVec2 size(width, ImGui::GetFrameHeight() + 2.f * pad);
	dl->AddRectFilled(pos, add(pos, size), theme::u32(theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.035f)), 10.f);
	dl->AddRect(pos, add(pos, size), theme::u32(pal.accent, 0.35f), 10.f, ImDrawCornerFlags_All, 1.f);
	ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, pos.y + pad));
	ImGui::PushItemWidth(std::min(420.f * u, width - 260.f * u));
	const bool enter = ImGui::InputText("##name", renameBuf_, sizeof(renameBuf_), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::PopItemWidth();
	ImGui::SameLine(0.f, 8.f * u);
	if ((navButton("Save", ImVec2(90.f * u, 0.f)) || enter) && shot.name != renameBuf_ && renameBuf_[0] && cb_.renameShot)
	{
		cb_.renameShot(shot.id, renameBuf_);
		toast("Renamed");
		editingShot_.clear();
	}
	ImGui::SameLine(0.f, 8.f * u);
	if (navButton("Cancel", ImVec2(90.f * u, 0.f)))
		editingShot_.clear();
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 10.f * u));
}

void Dashboard::openPlaySetup(bool online, const std::string& id, const std::string& delivery, const std::string& name,
	const shotshare::Extras& extras)
{
	playSetup_ = PlaySetup{};
	playSetup_.requested = true;
	playSetup_.online = online;
	playSetup_.id = id;
	playSetup_.delivery = delivery;
	playSetup_.name = name;
	playSetup_.extras = extras;
	playSetup_.start = extras;
}

void Dashboard::renderPlaySetup(const DashboardStatus& s)
{
	// Lambourne 2026-10-29: pressing Play on a saved, shared or Shot Board shot first asks for its setup, like the
	// Training packs page: difficulty, a defender in the orange net, and which parts of the net are open. A saved shot
	// keeps what was picked for next time.
	if (playSetup_.requested)
	{
		ImGui::OpenPopup("play_setup");
		playSetup_.requested = false;
	}
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
	ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	// Two columns (the player's side, the net) when there's room, else one; never taller than the window (it scrolls).
	const float popW = std::min(ws.x - 80.f * u, 920.f * u);
	ImGui::SetNextWindowSizeConstraints(ImVec2(popW, 0.f), ImVec2(popW, ws.y - 40.f * u));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.03f));
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.f, 0.f, 0.f, 0.55f));
	ImGui::PushStyleColor(ImGuiCol_Border, theme::mix(pal.card, pal.accent, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.f * u);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.f * u, 22.f * u));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	if (ImGui::BeginPopupModal("play_setup", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
	{
		PlaySetup& p = playSetup_;
		const float full = ImGui::GetContentRegionAvail().x;
		const bool twoCol = full >= 700.f * u;
		const float gapX = 28.f * u;
		const float inner = twoCol ? (full - gapX) * 0.5f : full; // a column's width
		theme::fontScale(1.35f);
		ImGui::TextUnformatted(fitText(p.name, full).c_str());
		theme::fontScale(1.f);
		ImGui::TextDisabled("%s", p.online ? "Set it up, then play. It's added to your drills too." : "Set it up, then play. It keeps this setup.");
		ImGui::Dummy(ImVec2(0, 8.f * u));

		const ImVec2 colTop = ImGui::GetCursorScreenPos();
		ImGui::BeginGroup();
		sectionLabel("DIFFICULTY");
		difficulty_ = segmented("setup_diff", kDifficultyLabels, 3, difficulty_, inner, setupDiffAnim_);
		ImGui::Dummy(ImVec2(0, 6.f * u));

		shotshare::Extras& x = p.extras;
		sectionLabel("DEFENDER");
		static const char* kWho[] = {"None", "Psyonix", "Necto", "Nexto"};
		static const char* kIds[] = {"", "psyonix", "necto", "nexto"};
		const int cur = x.defender == "nexto" ? 3 : x.defender == "necto" ? 2 : x.defender == "psyonix" ? 1 : 0;
		const int pick = segmented("setup_defender", kWho, 4, cur, inner, shotDefenderAnim_);
		const bool available = pick < 2 || (pick == 2 && s.nectoAvailable) || (pick == 3 && s.nextoAvailable);
		if (pick != cur && available)
			x.defender = kIds[pick];
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
		if (pick != cur && !available)
			ImGui::TextDisabled("%s is not installed (content/bots/%s/policy.bin).", kWho[pick], kIds[pick]);
		else
			ImGui::TextDisabled("%s", x.defender.empty() ? "Just you and the ball."
				: "Waits in the orange net and comes out to save it. Stronger on hard; its save ends the rep.");
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		const float leftBottom = ImGui::GetItemRectMax().y;
		if (twoCol)
			ImGui::SetCursorScreenPos(ImVec2(colTop.x + inner + gapX, colTop.y));
		else
			ImGui::Dummy(ImVec2(0, 6.f * u));
		ImGui::BeginGroup();

		// The net target, as on the Training packs page: the whole net, picked tiles, a random one each rep, or one
		// that moves; tiles of 4, 6 or 8, or a hole only just bigger than the ball.
		sectionLabel("NET TARGET");
		static const char* kTargets[] = {"Whole net", "Pick", "Random", "Moving"};
		static const char* kTargetIds[] = {"", "pick", "random", "moving"};
		const int tcur = x.target == "moving" ? 3 : x.target == "random" ? 2 : x.target == "pick" ? 1 : 0;
		const int tpick = segmented("setup_target", kTargets, 4, tcur, inner, setupTargetAnim_);
		if (tpick != tcur)
		{
			x.target = kTargetIds[tpick];
			if (x.target == "pick" && x.open == 0)
				x.open = 1u << (shotshare::colsFor(x.size) * 2 - 1); // the top corner on the left, as you see it
		}
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
		ImGui::TextDisabled("%s", x.target.empty() ? "Any goal counts."
			: x.target == "pick" ? "Only the gold part is open: the rest of the net is closed and a ball that hits it bounces off (MISS)."
			: x.target == "random" ? "A random tile is open each rep; the rest of the net bounces the ball off (MISS)."
			: "The open tile keeps moving (faster on hard); the rest of the net bounces the ball off (MISS).");
		ImGui::PopTextWrapPos();
		if (!x.target.empty())
		{
			ImGui::Dummy(ImVec2(0, 4.f * u));
			sectionLabel("TILE SIZE");
			static const char* kSizes[] = {"Easy  4", "Medium  6", "Hard  8", "Impossible"};
			static const char* kSizeIds[] = {"easy", "medium", "hard", "impossible"};
			const int scur = x.size == "easy" ? 0 : x.size == "hard" ? 2 : x.size == "impossible" ? 3 : 1;
			const int spick = segmented("setup_size", kSizes, 4, scur, inner, setupSizeAnim_);
			if (spick != scur)
			{
				x.size = kSizeIds[spick];
				x.open = 1u << (shotshare::colsFor(x.size) * 2 - 1); // a new grid: start from one tile
			}
			if (x.size == "impossible")
				ImGui::TextDisabled("A hole only just bigger than the ball.");
			if (x.target == "pick")
			{
				const float netW = std::min(inner, 380.f * u);
				const ImVec2 netAt = ImGui::GetCursorScreenPos();
				ImGui::SetCursorScreenPos(ImVec2(netAt.x + (inner - netW) * 0.5f, netAt.y + 4.f * u));
				x.open = netTilePicker(x.open, shotshare::colsFor(x.size), x.size == "impossible", netW);
				ImGui::SetCursorScreenPos(ImVec2(netAt.x, ImGui::GetCursorScreenPos().y));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
				ImGui::TextDisabled("%s", x.size == "impossible" ? "Click where the hole goes (the net as you see it from the field)."
					: "Click tiles to open or close them (the net as you see it from the field).");
				ImGui::PopTextWrapPos();
			}
		}
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(colTop.x, std::max(leftBottom, ImGui::GetItemRectMax().y) + 16.f * u));
		const float inner2 = full; // the buttons span the whole width

		const float bw = 150.f * u;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + inner2 - 2.f * bw - 10.f * u);
		if (navButton("Cancel", ImVec2(bw, 40.f * u)) || ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)))
		{
			p.open = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine(0.f, 10.f * u);
		if (bigButton("setup_play", "Play", ImVec2(bw, 40.f * u), true, Icon::Play, true, Fx::Confirm))
		{
			const std::string diff = kDifficulties[difficulty_];
			if (p.online)
			{
				if (cb_.addOnlineShot)
					cb_.addOnlineShot(p.id, p.delivery, true, true, shotshare::normalized(p.extras), diff);
			}
			else
			{
				if (shotshare::normalized(p.extras) != shotshare::normalized(p.start) && cb_.shotExtrasChanged)
					cb_.shotExtrasChanged(p.id, shotshare::normalized(p.extras)); // kept for next time
				if (cb_.play)
					cb_.play(p.id, diff);
			}
			closeRequested_ = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(3);
}

void Dashboard::renderShotList(const DashboardModel& m, const DashboardStatus& s)
{
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float width = ImGui::GetContentRegionAvail().x;
	const float u = uiScale(); // sizes follow the GUI scale
	constexpr float kButtons = 580.f; // Star, Play, Edit, Share, + Warm-up, Delete
	for (const auto& shot : m.savedShots)
	{
		ImGui::PushID(shot.id.c_str());
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const bool compact = width < 860.f * u; // the buttons go under the name
		const ImVec2 size(width, (compact ? 104.f : 64.f) * u);
		const bool editing = editingShot_ == shot.id;
		dl->AddRectFilled(pos, add(pos, size), theme::u32(editing ? theme::mix(pal.card, pal.accent, 0.08f) : pal.card), 10.f);
		if (shot.favorite)
			dl->AddRectFilled(pos, ImVec2(pos.x + 5.f, pos.y + size.y), theme::u32(pal.accent), 10.f, ImDrawCornerFlags_Left);
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 16.f * u, pos.y + 10.f * u));
		ImGui::BeginGroup();
		{
			// Name (with what was added to it as tags) and description stop short of the buttons on the right.
			std::vector<std::string> tags;
			const shotshare::Extras& x = shot.extras;
			if (!x.defender.empty())
				tags.push_back(std::string("vs ") + (x.defender == "psyonix" ? "Psyonix" : x.defender == "necto" ? "Necto" : "Nexto"));
			if (const std::string net = shotshare::targetLabel(x); !net.empty())
				tags.push_back(net);
			if (!shot.sharedBy.empty())
				tags.push_back("from " + shot.sharedBy);
			float tagsW = 0.f;
			for (const auto& t : tags)
				tagsW += ImGui::CalcTextSize(t.c_str()).x + 16.f + 6.f * u;
			const float textW = compact ? size.x - 32.f : std::max(60.f, size.x - kButtons * u - 32.f * u);
			const std::string name = fitText(shot.name, std::max(60.f, textW - tagsW));
			const std::string about = fitText(shot.description, textW);
			ImGui::TextUnformatted(name.c_str());
			float tx = ImGui::GetItemRectMax().x + 10.f * u;
			const float ty = ImGui::GetItemRectMin().y - 2.f;
			for (const auto& t : tags)
			{
				pill(dl, ImVec2(tx, ty), t, t.rfind("from ", 0) == 0 ? pal.textDim : pal.accent);
				tx += ImGui::CalcTextSize(t.c_str()).x + 16.f + 6.f * u;
			}
			ImGui::TextDisabled("%s", about.c_str());
			if (ImGui::IsItemHovered() && about != shot.description)
				ImGui::SetTooltip("%s", shot.description.c_str());
		}
		ImGui::EndGroup();

		// Actions on the right.
		ImGui::SetCursorScreenPos(compact ? ImVec2(pos.x + 16.f * u, pos.y + 58.f * u) : ImVec2(pos.x + size.x - kButtons * u, pos.y + 15.f * u));
		if (navButton(shot.favorite ? "Starred" : "Star", ImVec2(80.f * u, 34.f * u)) && cb_.favoriteShot)
		{
			cb_.favoriteShot(shot.id, !shot.favorite);
			toast(shot.favorite ? "Unstarred" : "Starred");
		}
		ImGui::SameLine(0.f, 8.f * u);
		// The same bright Play as everywhere else (it was a muted teal the dark text barely showed on).
		if (bigButton("play", "Play", ImVec2(80.f * u, 34.f * u), true, Icon::Play, false, Fx::Confirm))
			openPlaySetup(false, shot.id, "", shot.name, shot.extras);
		ImGui::SameLine(0.f, 8.f * u);
		if (editing)
			ImGui::PushStyleColor(ImGuiCol_Button, theme::mix(pal.card, pal.accent, 0.35f));
		if (navButton(editing ? "Done" : "Rename", ImVec2(80.f * u, 34.f * u)))
		{
			if (editing)
				editingShot_.clear();
			else
			{
				editingShot_ = shot.id;
				snprintf(renameBuf_, sizeof(renameBuf_), "%s", shot.name.c_str());
			}
		}
		if (editing)
			ImGui::PopStyleColor();
		ImGui::SameLine(0.f, 8.f * u);
		if (navButton("Share", ImVec2(90.f * u, 34.f * u)))
			ImGui::OpenPopup("share_menu");
		if (beginMenuBelow("share_menu"))
		{
			// As a file (Discord), or online: the Shot Board, or straight to friends (their Record page).
			if (ImGui::Selectable("Copy as a file (paste in Discord)") && cb_.shareShot)
				cb_.shareShot(shot.id); // its toast comes back from the game thread (copied, or why not)
			if (s.account.state != Account::State::SignedIn)
				ImGui::TextDisabled("Pick a name (Settings > Account) to share online");
			else
			{
				if (ImGui::Selectable("Post on the Shot Board") && cb_.shareShotOnline)
					cb_.shareShotOnline(shot.id, true, {}, "");
				std::vector<const Friends::Entry*> friends;
				for (const auto& e : s.friends.list)
					if (e.state == "friend")
						friends.push_back(&e);
				ImGui::Separator();
				if (friends.empty())
					ImGui::TextDisabled("Add friends (Friends page) to send them shots");
				else
				{
					ImGui::TextDisabled("Send to a friend");
					for (const auto* e : friends)
					{
						ImGui::PushID(e->id.c_str());
						if (ImGui::Selectable(e->name.c_str()) && cb_.shareShotOnline)
							cb_.shareShotOnline(shot.id, false, {e->id}, e->name);
						ImGui::PopID();
					}
					if (friends.size() > 1 && ImGui::Selectable("All my friends") && cb_.shareShotOnline)
					{
						std::vector<std::string> ids;
						for (const auto* e : friends)
							ids.push_back(e->id);
						cb_.shareShotOnline(shot.id, false, ids, "all your friends");
					}
				}
			}
			endMenuBelow();
		}
		ImGui::SameLine(0.f, 8.f * u);
		if (navButton("+ Warm-up", ImVec2(120.f * u, 34.f * u)) && cb_.addShotToQuickPlay)
		{
			cb_.addShotToQuickPlay(shot.id);
			toast("Added to your warm-up");
		}
		ImGui::SameLine(0.f, 8.f * u);
		const bool confirming = confirmDelete_ == shot.id;
		ImGui::PushStyleColor(ImGuiCol_Button, confirming ? ImVec4(0.75f, 0.2f, 0.25f, 1.f) : theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.07f)); // a button, not plain text
		if (navButton(confirming ? "Sure?" : "Delete", ImVec2(90.f * u, 34.f * u)))
		{
			if (confirming && cb_.deleteShot)
			{
				cb_.deleteShot(shot.id);
				toast("Deleted");
				confirmDelete_.clear();
			}
			else
			{
				confirmDelete_ = shot.id;
			}
		}
		ImGui::PopStyleColor();
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.f * u));
		if (editing)
			renderShotEditor(shot, s, width);
		ImGui::PopID();
	}
}

void Dashboard::renderRecord(const DashboardModel& m, const DashboardStatus& s)
{
	// Record a feed, pick where the drill starts, save it (Lambourne, 2026-10-19: "so many words": the page shows what
	// to do with a big button, three steps, the recording on a map and a timeline to drag).
	const theme::Palette& pal = theme::palette();
	pageTitle("Record", "Play a feed, then pick where your drill starts.");
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const float width = ImGui::GetContentRegionAvail().x;
	const double now = ImGui::GetTime();
	const ImVec4 red(0.95f, 0.27f, 0.30f, 1.f);
	const ImU32 gold = IM_COL32(255, 200, 80, 255);
	const bool hasTake = s.takeSeconds > 0.f && !s.recording && s.take && s.take->ball.size() >= 2;
	const int stage = s.recording ? 1 : hasTake ? 2 : 0;
	const float u = uiScale(); // BakkesMod's GUI scale: the hand-placed card grows with the text

	// The record card: the button, what is happening, the three steps.
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 150.f * u);
		ImGui::Dummy(size);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(stage == 1 ? theme::mix(pal.card, red, 0.12f) : pal.card), 14.f);
		const ImVec2 c(pos.x + 82.f * u, pos.y + size.y * 0.5f);
		const float r = 46.f * u;
		const bool can = s.recording || s.inFreeplay;
		ImGui::SetCursorScreenPos(ImVec2(c.x - r - 6.f, c.y - r - 6.f));
		const bool pressed = navInvisible("##record", ImVec2(2.f * r + 12.f, 2.f * r + 12.f));
		const bool hovered = ImGui::IsItemHovered();
		dl->AddCircle(c, r + 6.f, IM_COL32(255, 255, 255, hovered && can ? 70 : 35), 48, 3.f);
		if (s.recording)
		{
			// Progress round the button to the 15 s limit, and a pulsing stop square.
			const float frac = std::clamp(s.recordSeconds / 15.f, 0.f, 1.f);
			const float a0 = -1.5707963f;
			dl->PathArcTo(c, r + 6.f, a0, a0 + frac * 6.2831853f, 64);
			dl->PathStroke(theme::u32(red), false, 5.f);
			const float pulse = 0.75f + 0.25f * static_cast<float>(std::sin(now * 5.0));
			dl->AddRectFilled(ImVec2(c.x - 17.f * u, c.y - 17.f * u), ImVec2(c.x + 17.f * u, c.y + 17.f * u), theme::u32(red, pulse), 6.f);
		}
		else
		{
			dl->AddCircleFilled(c, (hovered && can ? 32.f : 30.f) * u, theme::u32(red, can ? 1.f : 0.3f), 40);
		}
		if (pressed)
		{
			if (s.recording && cb_.stopRecording)
				cb_.stopRecording();
			else if (can && cb_.startRecording)
			{
				cb_.startRecording();
				recordStart_ = 0.f;
				closeRequested_ = true; // play the feed; open the window again to stop
			}
		}

		const float tx = pos.x + 168.f * u;
		char title[64];
		const char* sub;
		if (stage == 1)
		{
			snprintf(title, sizeof(title), "Recording  %.1f s", s.recordSeconds);
			sub = "Play it out, then open this window and stop.";
		}
		else if (stage == 2)
		{
			snprintf(title, sizeof(title), "Got it: %.1f s", s.takeSeconds);
			sub = "Drag the timeline to where your drill starts.";
		}
		else if (!s.inFreeplay)
		{
			snprintf(title, sizeof(title), "Go to freeplay");
			sub = "Recording works in freeplay.";
		}
		else
		{
			snprintf(title, sizeof(title), "Record a feed");
			sub = "Press, set the ball up, play the moment out.";
		}
		dl->AddText(theme::fontAt(fs * 1.6f), fs * 1.6f, ImVec2(tx, pos.y + 22.f * u), theme::u32(pal.text), title);
		dl->AddText(ImVec2(tx, pos.y + 22.f * u + fs * 1.6f + 4.f * u), theme::u32(pal.textDim), sub);
		// The three steps, the current one lit.
		static const char* kSteps[] = {"1  RECORD", "2  PICK THE START", "3  SAVE"};
		float x = tx;
		const int current = stage == 2 ? 1 : 0;
		for (int i = 0; i < 3; ++i)
		{
			const ImVec4 col = i == current ? pal.accent : i < current ? theme::mix(pal.textDim, pal.accent, 0.5f) : pal.textDim;
			pill(dl, ImVec2(x, pos.y + 22.f * u + fs * 2.6f + 14.f * u), kSteps[i], col);
			x += ImGui::CalcTextSize(kSteps[i]).x + 16.f + 10.f;
		}
		const char* hint = "Select button: ft_select_command ft_record";
		const ImVec2 hs = ImGui::CalcTextSize(hint);
		if (x + 30.f + hs.x < pos.x + size.x - 18.f)
			dl->AddText(ImVec2(pos.x + size.x - 18.f - hs.x, pos.y + size.y - 18.f * u - hs.y), theme::u32(pal.textDim, 0.6f), hint);
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 14.f));
	}

	// The recording: on the map, and the timeline to pick the start.
	if (hasTake)
	{
		const RecordTake& t = *s.take;
		const float dur = std::max(0.1f, t.seconds());
		recordStart_ = std::clamp(recordStart_, 0.f, dur);
		const float before = recordStart_;
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 286.f * u);
		ImGui::Dummy(size);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(pal.card), 14.f);
		const ImVec2 mapA(pos.x + 18.f, pos.y + 18.f), mapB(pos.x + 18.f + 360.f * u, pos.y + size.y - 18.f);
		drawTakeMap(dl, mapA, mapB, t, recordStart_);

		const float rx = mapB.x + 26.f, rw = pos.x + size.x - 22.f - rx;
		dl->AddText(ImVec2(rx, pos.y + 18.f * u), theme::u32(pal.textDim), "YOUR DRILL STARTS AT");
		char buf[48];
		snprintf(buf, sizeof(buf), "%.1f s", recordStart_);
		dl->AddText(theme::fontAt(fs * 2.f), fs * 2.f, ImVec2(rx, pos.y + 18.f * u + fs * 1.1f), gold, buf);

		// Timeline: what replays first (the lead-in) up to the handle, what you play after it.
		const ImVec2 ta(rx, pos.y + 18.f * u + fs * 3.4f), tb(rx + rw, pos.y + 18.f * u + fs * 3.4f + 46.f * u);
		const float hx = ta.x + (tb.x - ta.x) * (recordStart_ / dur);
		dl->AddRectFilled(ta, tb, theme::u32(pal.bg), 10.f);
		if (recordStart_ > 0.f && recordLeadIn_)
			dl->AddRectFilled(ta, ImVec2(hx, tb.y), theme::u32(pal.accent, 0.35f), 10.f,
				hx >= tb.x - 1.f ? ImDrawCornerFlags_All : ImDrawCornerFlags_Left);
		for (int sec = 1; sec < static_cast<int>(dur); ++sec)
		{
			const float sx = ta.x + (tb.x - ta.x) * (sec / dur);
			dl->AddLine(ImVec2(sx, tb.y - 8.f), ImVec2(sx, tb.y), theme::u32(pal.textDim, 0.6f), 1.f);
		}
		if (hx - ta.x > 90.f && recordLeadIn_)
			dl->AddText(ImVec2(ta.x + 10.f, ta.y + 14.f), theme::u32(pal.text, 0.9f), "REPLAYS");
		if (tb.x - hx > 90.f)
			dl->AddText(ImVec2(hx + 12.f, ta.y + 14.f), theme::u32(pal.textDim), "YOU PLAY");
		dl->AddLine(ImVec2(hx, ta.y - 6.f), ImVec2(hx, tb.y + 6.f), gold, 3.f);
		dl->AddCircleFilled(ImVec2(hx, ta.y - 6.f), 7.f, gold, 16);
		ImGui::SetCursorScreenPos(ImVec2(ta.x, ta.y - 10.f));
		ImGui::InvisibleButton("##timeline", ImVec2(tb.x - ta.x, tb.y - ta.y + 20.f));
		if (ImGui::IsItemActive())
			recordStart_ = std::clamp((ImGui::GetIO().MousePos.x - ta.x) / (tb.x - ta.x) * dur, 0.f, dur);
		if (ImGui::IsItemHovered())
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
		dl->AddText(ImVec2(ta.x, tb.y + 6.f), theme::u32(pal.textDim, 0.7f), "0 s");
		snprintf(buf, sizeof(buf), "%.1f s", dur);
		dl->AddText(ImVec2(tb.x - ImGui::CalcTextSize(buf).x, tb.y + 6.f), theme::u32(pal.textDim, 0.7f), buf);

		// Fine steps (and the way to do it with a controller).
		ImGui::SetCursorScreenPos(ImVec2(rx, tb.y + fs * 1.5f));
		for (const auto& [label, step] : {std::pair<const char*, float>{"-0.5 s", -0.5f}, {"-0.1 s", -0.1f}, {"+0.1 s", 0.1f}, {"+0.5 s", 0.5f}})
		{
			if (navButton(label, ImVec2(70.f * u, ImGui::GetFrameHeight())))
				recordStart_ = std::clamp(recordStart_ + step, 0.f, dur);
			ImGui::SameLine(0.f, 6.f);
		}
		ImGui::NewLine();
		if (recordStart_ != before && cb_.previewRecording)
			cb_.previewRecording(recordStart_);

		// Name it, choose the lead-in, save.
		ImGui::SetCursorScreenPos(ImVec2(rx, tb.y + fs * 1.5f + ImGui::GetFrameHeight() + 10.f * u));
		ImGui::PushItemWidth(std::min(260.f * u, rw));
		ImGui::InputTextWithHint("##recname", "Name it (e.g. my wall pop)", recordName_, sizeof(recordName_));
		ImGui::PopItemWidth();
		ImGui::SameLine(0.f, 14.f);
		navCheckbox("Replay the feed first", &recordLeadIn_);
		ImGui::SetCursorScreenPos(ImVec2(rx, tb.y + fs * 1.5f + 2.f * ImGui::GetFrameHeight() + 20.f * u));
		if (bigButton("save_drill", "Save as drill", ImVec2(170.f * u, 38.f * u), true, Icon::Play, false, Fx::Confirm) && cb_.saveRecording)
		{
			cb_.saveRecording(recordStart_, recordLeadIn_, recordName_);
			toast("Saved: it's under Your drills");
			recordName_[0] = 0;
			recordStart_ = 0.f;
		}
		ImGui::SameLine(0.f, 8.f);
		if (navButton("Discard", ImVec2(110.f * u, 38.f * u)) && cb_.discardRecording)
			cb_.discardRecording();
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 14.f));
		ImGui::Dummy(ImVec2(0, 0));
	}

	// Drills friends shared, then yours.
	renderInbox(s);
	ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
	ImGui::TextUnformatted("YOUR DRILLS");
	ImGui::PopStyleColor();
	if (m.savedShots.empty())
		ImGui::TextDisabled("Saved drills show up here.");
	else
		renderShotList(m, s);
}

void Dashboard::renderComingSoon(const char* title, const char* phase, const char* what)
{
	const theme::Palette& pal = theme::palette();
	pageTitle(title, nullptr);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const ImVec2 size(ImGui::GetContentRegionAvail().x, 160.f);
	ImGui::Dummy(size);
	dl->AddRectFilled(pos, add(pos, size), theme::u32(pal.card), 12.f);
	pill(dl, ImVec2(pos.x + 22, pos.y + 22), std::string("Coming ") + phase, pal.accent);
	dl->AddText(theme::fontAt(ImGui::GetFontSize()), ImGui::GetFontSize(), ImVec2(pos.x + 22, pos.y + 62), theme::u32(pal.text), what,
		nullptr, size.x - 44.f);
}

bool Dashboard::switchToggle(const char* id, bool* value)
{
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	const ImVec2 size(46.f * u, 26.f * u);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	// A click is shown at once and held for a moment, until the game thread has applied it and the status caught up.
	const std::string key = std::string("switch.") + id;
	const double now = ImGui::GetTime();
	auto pending = switchPending_.find(key);
	if (pending != switchPending_.end())
	{
		if (pending->second.second < now || pending->second.first == *value)
			switchPending_.erase(pending);
		else
			*value = pending->second.first;
	}
	const bool clicked = navInvisible(id, size, 1, Fx::Toggle);
	if (clicked)
	{
		*value = !*value;
		switchPending_[key] = {*value, now + 1.0};
	}
	const bool hovered = ImGui::IsItemHovered();
	float& on = hoverAnim_[key];
	on = approach(on, *value ? 1.f : 0.f, 10.f);
	const ImVec4 off(0.25f, 0.28f, 0.35f, 1.f);
	const ImVec4 track = theme::mix(off, pal.accent, on);
	dl->AddRectFilled(p, add(p, size), theme::u32(hovered ? theme::mix(track, ImVec4(1, 1, 1, 1), 0.12f) : track), size.y * 0.5f);
	const float r = size.y * 0.5f - 3.f * u;
	dl->AddCircleFilled(ImVec2(p.x + size.y * 0.5f + (size.x - size.y) * on, p.y + size.y * 0.5f), r, IM_COL32(245, 248, 252, 255), 24);
	return clicked;
}

void Dashboard::settingsCardBegin(const char* id, const char* title)
{
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	cardId_ = id;
	const float width = std::min(ImGui::GetContentRegionAvail().x, 980.f * u);
	const float pad = 20.f * u;
	cardStart_ = ImGui::GetCursorScreenPos();
	cardInner_ = width - 2.f * pad;
	const auto it = cardHeight_.find(id);
	if (it != cardHeight_.end())
		dl->AddRectFilled(cardStart_, ImVec2(cardStart_.x + width, cardStart_.y + it->second), theme::u32(pal.card), 14.f);
	ImGui::SetCursorScreenPos(ImVec2(cardStart_.x + pad, cardStart_.y + pad * 0.85f));
	ImGui::BeginGroup();
	ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
	ImGui::TextUnformatted(title);
	ImGui::PopStyleColor();
	ImGui::Dummy(ImVec2(0.f, 2.f * u));
	firstRow_ = true;
}

void Dashboard::settingsCardEnd()
{
	const float u = uiScale();
	ImGui::EndGroup();
	const float bottom = ImGui::GetItemRectMax().y + 18.f * u;
	cardHeight_[cardId_] = bottom - cardStart_.y;
	ImGui::SetCursorScreenPos(ImVec2(cardStart_.x, bottom + 14.f * u));
	ImGui::Dummy(ImVec2(0.f, 0.f));
}

void Dashboard::settingRow(const char* label, const char* help)
{
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	if (!firstRow_)
	{
		// A faint line between rows.
		dl->AddLine(ImVec2(p.x, p.y + 2.f * u), ImVec2(p.x + cardInner_, p.y + 2.f * u), IM_COL32(255, 255, 255, 14), 1.f);
		p.y += 12.f * u;
		ImGui::SetCursorScreenPos(p);
	}
	firstRow_ = false;
	rowTop_ = p.y;
	const float ctrlW = std::min(380.f * u, cardInner_ * 0.5f);
	const float leftW = cardInner_ - ctrlW - 24.f * u;
	rowCtrlX_ = p.x + cardInner_ - ctrlW;
	ImGui::BeginGroup();
	ImGui::PushTextWrapPos(p.x - ImGui::GetWindowPos().x + leftW); // window-relative
	ImGui::TextUnformatted(label);
	if (help && *help)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, pal.textDim);
		theme::fontScale(0.9f);
		ImGui::TextUnformatted(help);
		theme::fontScale(1.f);
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();
	ImGui::EndGroup();
	rowLeftBottom_ = ImGui::GetItemRectMax().y;
	ImGui::SetCursorScreenPos(ImVec2(rowCtrlX_, rowTop_));
	ImGui::BeginGroup();
	ImGui::PushItemWidth(ctrlW);
}

void Dashboard::settingRowEnd()
{
	ImGui::PopItemWidth();
	ImGui::EndGroup();
	const float bottom = std::max(rowLeftBottom_, ImGui::GetItemRectMax().y);
	ImGui::SetCursorScreenPos(ImVec2(cardStart_.x + 20.f * uiScale(), bottom + 10.f * uiScale()));
}

void Dashboard::renderBoard(const DashboardStatus& s)
{
	// Phase 8: the public Shot Board (Lambourne 2026-10-29: share shots online). Posted from a drill's Share menu.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float u = uiScale();
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec4 red(0.95f, 0.45f, 0.45f, 1.f);
	const OnlineShots::View& v = s.shots;
	pageTitle("Shot Board", "Shots players posted: play one, like it, or add it to your drills. Post yours from a drill's "
		"Share button (Record page).");
	if (s.account.state != Account::State::SignedIn)
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 96.f * u);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(pal.card), 12.f);
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 20.f * u, pos.y + 16.f * u));
		ImGui::BeginGroup();
		ImGui::TextUnformatted(s.account.state == Account::State::Offline ? "Can't reach the Free Touch service right now."
			: s.account.state == Account::State::Working ? "Signing in..." : "The Shot Board needs a Free Touch account: just a name.");
		ImGui::Dummy(ImVec2(0, 4.f * u));
		if (s.account.state == Account::State::SignedOut && navButton("Pick a name in Settings", ImVec2(0, 32.f * u)))
			page_ = Page::Settings;
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 14.f * u));
		return;
	}
	static const char* kSorts[] = {"Newest", "Top this week", "Mine"};
	static const char* kSortIds[] = {"new", "top", "mine"};
	const int cur = v.sort == "top" ? 1 : v.sort == "mine" ? 2 : 0;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	const int pick = segmented("board_sort", kSorts, 3, cur, std::min(width - 130.f * u, 460.f * u), boardSortAnim_);
	if (pick != cur && cb_.refreshShotBoard)
		cb_.refreshShotBoard(kSortIds[pick]);
	const ImVec2 below = ImGui::GetCursorScreenPos();
	const float rw = 100.f * u;
	ImGui::SetCursorScreenPos(ImVec2(top.x + width - rw, top.y)); // Refresh on the tabs' row, at the right
	if (navButton(v.busy ? "..." : "Refresh", ImVec2(rw, 0.f)) && !v.busy && cb_.refreshShotBoard)
		cb_.refreshShotBoard(v.sort);
	ImGui::SetCursorScreenPos(below);
	if (!v.error.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, red);
		ImGui::TextUnformatted(v.error.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::Dummy(ImVec2(0, 6.f * u));
	if (!v.boardLoaded)
	{
		ImGui::TextDisabled("Loading...");
		return;
	}
	if (v.board.empty())
	{
		ImGui::TextDisabled("%s", cur == 2 ? "Nothing of yours online yet. Use Share on one of your drills (Record page)."
			: "Nothing posted yet. Be the first: Share > Post on the Shot Board on one of your drills.");
		return;
	}
	for (const auto& b : v.board)
	{
		ImGui::PushID(b.id.c_str());
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 62.f * u);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(pal.card), 10.f);
		const bool have = s.haveKeys && s.haveKeys->count(b.key) > 0;
		const float buttonsW = b.mine ? 230.f * u : 420.f * u;
		const float textW = std::max(60.f, size.x - buttonsW - 40.f * u);
		const float tx = pos.x + 18.f * u;
		dl->AddText(ImVec2(tx, pos.y + 10.f * u), theme::u32(pal.text), fitText(b.name, textW).c_str());
		std::string line = (b.mine ? std::string(b.isPublic ? "yours, on the board" : "yours, friends only") : "by " + b.owner)
			+ "   " + Friends::ago(b.ageSeconds) + "   |   " + std::to_string(b.likes) + (b.likes == 1 ? " like" : " likes")
			+ "   " + std::to_string(b.playsWeek) + (b.playsWeek == 1 ? " player" : " players") + " this week";
		dl->AddText(ImVec2(tx, pos.y + 12.f * u + ImGui::GetFontSize()), theme::u32(pal.textDim), fitText(line, textW).c_str());
		ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - buttonsW - 12.f * u, pos.y + 14.f * u));
		if (b.mine)
		{
			if (navButton(b.isPublic ? "Make private" : "Post publicly", ImVec2(130.f * u, 34.f * u)) && cb_.setShotPublic)
				cb_.setShotPublic(b.id, !b.isPublic);
			ImGui::SameLine(0.f, 8.f * u);
			const bool confirming = confirmBoard_ == b.id;
			ImGui::PushStyleColor(ImGuiCol_Button, confirming ? ImVec4(0.75f, 0.2f, 0.25f, 1.f) : theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.07f));
			if (navButton(confirming ? "Sure?" : "Delete", ImVec2(90.f * u, 34.f * u)))
			{
				if (confirming && cb_.deleteOnlineShot)
				{
					cb_.deleteOnlineShot(b.id);
					toast("Deleted from the service");
					confirmBoard_.clear();
				}
				else
					confirmBoard_ = b.id;
			}
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Takes it off the service (friends you sent it to lose it too). Your own drill stays.");
		}
		else
		{
			if (bigButton("play", "Play", ImVec2(90.f * u, 34.f * u), true, Icon::Play, false, Fx::Confirm))
				openPlaySetup(true, b.id, "", b.name, {});
			ImGui::SameLine(0.f, 8.f * u);
			if (have)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
				ImGui::Button("Added", ImVec2(90.f * u, 34.f * u));
				ImGui::PopStyleVar();
			}
			else if (navButton("Add", ImVec2(90.f * u, 34.f * u)) && cb_.addOnlineShot)
				cb_.addOnlineShot(b.id, "", false, false, {}, "");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Adds it to your drills (Record page) to play any time");
			ImGui::SameLine(0.f, 8.f * u);
			if (b.liked)
				ImGui::PushStyleColor(ImGuiCol_Button, theme::mix(pal.card, pal.accent, 0.45f));
			if (navButton(b.liked ? "Liked" : "Like", ImVec2(90.f * u, 34.f * u)) && cb_.likeShot)
				cb_.likeShot(b.id, !b.liked);
			if (b.liked)
				ImGui::PopStyleColor();
			ImGui::SameLine(0.f, 8.f * u);
			const bool confirming = confirmBoard_ == b.id;
			ImGui::PushStyleColor(ImGuiCol_Button, confirming ? ImVec4(0.75f, 0.2f, 0.25f, 1.f) : theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.07f));
			if (navButton(confirming ? "Sure?" : "Report", ImVec2(90.f * u, 34.f * u)))
			{
				if (confirming && cb_.reportShot)
				{
					cb_.reportShot(b.id);
					confirmBoard_.clear();
				}
				else
					confirmBoard_ = b.id;
			}
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Something wrong with it? Three reports take a shot off the board.");
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.f * u));
		ImGui::PopID();
	}
}

void Dashboard::renderFriends(const DashboardStatus& s)
{
	// Phase 8: friends by friend code or name, requests, and what friends have been training (Lambourne 2026-10-29).
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float u = uiScale();
	const float width = ImGui::GetContentRegionAvail().x;
	const ImVec4 red(0.95f, 0.45f, 0.45f, 1.f);
	const ImVec4 green(0.35f, 0.85f, 0.45f, 1.f);
	const Friends::View& f = s.friends;
	pageTitle("Friends", "Add friends with their friend code and see what they've been training.");
	if (s.account.state != Account::State::SignedIn)
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 96.f * u);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(pal.card), 12.f);
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 20.f * u, pos.y + 16.f * u));
		ImGui::BeginGroup();
		ImGui::TextUnformatted(s.account.state == Account::State::Offline ? "Can't reach the Free Touch service right now."
			: s.account.state == Account::State::Working ? "Signing in..." : "Friends need a Free Touch account: just a name.");
		ImGui::Dummy(ImVec2(0, 4.f * u));
		if (s.account.state == Account::State::SignedOut && navButton("Pick a name in Settings", ImVec2(0, 32.f * u)))
			page_ = Page::Settings;
		ImGui::EndGroup();
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 14.f * u));
		return;
	}

	// Add a friend, and your own code to give out.
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 74.f * u);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(pal.card), 12.f);
		ImGui::SetCursorScreenPos(ImVec2(pos.x + 20.f * u, pos.y + 20.f * u));
		ImGui::PushItemWidth(std::min(300.f * u, width * 0.35f));
		const bool enter = ImGui::InputTextWithHint("##friend_in", "Friend code or name", friendInput_, sizeof(friendInput_),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::PopItemWidth();
		ImGui::SameLine(0.f, 8.f * u);
		if ((bigButton("friend_add", f.busy ? "..." : "Add friend", ImVec2(130.f * u, 0.f), true, Icon::Friends, false, Fx::Confirm) || enter)
			&& friendInput_[0] && !f.busy && cb_.addFriend)
		{
			cb_.addFriend(friendInput_);
			friendInput_[0] = 0;
		}
		// Your code on the right.
		const std::string mine = "Your code  " + s.account.friendCode;
		const float copyW = 70.f * u;
		const float mx = pos.x + size.x - 20.f * u - copyW - 10.f * u - ImGui::CalcTextSize(mine.c_str()).x;
		if (mx > ImGui::GetItemRectMax().x + 30.f * u)
		{
			dl->AddText(ImVec2(mx, pos.y + (size.y - ImGui::GetFontSize()) * 0.5f), theme::u32(pal.textDim), mine.c_str());
			ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - 20.f * u - copyW, pos.y + 20.f * u));
			if (navButton("Copy##mycode", ImVec2(copyW, 0.f)) && cb_.copyText)
				toast(cb_.copyText(s.account.friendCode) ? "Friend code copied" : "Couldn't use the clipboard");
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.f * u));
	}
	if (!f.message.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
		ImGui::TextUnformatted(f.message.c_str());
		ImGui::PopStyleColor();
	}
	if (!f.error.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, red);
		ImGui::TextUnformatted(f.error.c_str());
		ImGui::PopStyleColor();
	}
	// What friends see of you.
	{
		bool share = s.shareActivity;
		if (switchToggle("share_activity", &share) && cb_.shareActivityChanged)
		{
			cb_.shareActivityChanged(share);
			toast(share ? "Friends see what you train" : "Your training is private");
		}
		ImGui::SameLine(0.f, 10.f * u);
		ImGui::TextDisabled("%s", share ? "Friends see your minutes this week, streak, last drill and when you're on."
			: "Friends don't see your training.");
		ImGui::SameLine();
		const float rw = 100.f * u;
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - rw));
		if (navButton(f.busy ? "..." : "Refresh", ImVec2(rw, 0.f)) && !f.busy && cb_.refreshFriends)
			cb_.refreshFriends();
	}
	ImGui::Dummy(ImVec2(0, 6.f * u));

	// One row: a name (with an on-now dot), a line under it, the buttons on the right.
	const auto row = [&](const Friends::Entry& e, const std::string& line, const std::string& stats) {
		ImGui::PushID(e.id.c_str());
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImVec2 size(width, 62.f * u);
		dl->AddRectFilled(pos, add(pos, size), theme::u32(e.state == "incoming" ? theme::mix(pal.card, pal.accent, 0.10f) : pal.card), 10.f);
		const float dotX = pos.x + 22.f * u, midY = pos.y + 21.f * u;
		if (e.state == "friend")
			dl->AddCircleFilled(ImVec2(dotX, midY), 5.f * u, theme::u32(e.online ? green : theme::mix(pal.textDim, pal.card, 0.4f)), 12);
		const float tx = pos.x + 38.f * u;
		const float buttonsW = e.state == "incoming" ? 200.f * u : 120.f * u;
		dl->AddText(ImVec2(tx, pos.y + 10.f * u), theme::u32(pal.text), e.name.c_str());
		const float statsW = std::max(0.f, size.x - (tx - pos.x) - buttonsW - 40.f * u);
		dl->AddText(ImVec2(tx, pos.y + 12.f * u + ImGui::GetFontSize()), theme::u32(e.online ? green : pal.textDim),
			fitText(line + (stats.empty() ? "" : "   |   " + stats), statsW).c_str());
		ImGui::SetCursorScreenPos(ImVec2(pos.x + size.x - buttonsW - 14.f * u, pos.y + 14.f * u));
		if (e.state == "incoming")
		{
			if (bigButton("accept", "Accept", ImVec2(96.f * u, 34.f * u), true, Icon::Friends, false, Fx::Confirm) && cb_.respondFriend)
				cb_.respondFriend(e.id, true);
			ImGui::SameLine(0.f, 8.f * u);
			if (navButton("Decline", ImVec2(96.f * u, 34.f * u)) && cb_.respondFriend)
				cb_.respondFriend(e.id, false);
		}
		else
		{
			const bool confirming = confirmRemoveFriend_ == e.id;
			ImGui::PushStyleColor(ImGuiCol_Button, confirming ? ImVec4(0.75f, 0.2f, 0.25f, 1.f) : theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.07f));
			if (navButton(confirming ? "Sure?" : e.state == "sent" ? "Cancel" : "Remove", ImVec2(110.f * u, 34.f * u)))
			{
				if (confirming && cb_.removeFriend)
				{
					cb_.removeFriend(e.id);
					confirmRemoveFriend_.clear();
				}
				else
					confirmRemoveFriend_ = e.id;
			}
			ImGui::PopStyleColor();
		}
		ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.f * u));
		ImGui::PopID();
	};
	const auto section = [&](const char* title) {
		ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
		ImGui::TextUnformatted(title);
		ImGui::PopStyleColor();
	};

	if (f.incoming > 0)
	{
		section("REQUESTS");
		for (const auto& e : f.list)
			if (e.state == "incoming")
				row(e, "wants to be friends (" + e.friendCode + ")", "");
		ImGui::Dummy(ImVec2(0, 4.f * u));
	}
	const int friends = static_cast<int>(std::count_if(f.list.begin(), f.list.end(), [](const Friends::Entry& e) { return e.state == "friend"; }));
	section(("FRIENDS  " + std::to_string(friends)).c_str());
	if (friends == 0)
		ImGui::TextDisabled("%s", !f.loaded ? "Loading..." : "No friends yet. Give your code to a friend, or add theirs above.");
	for (const auto& e : f.list)
	{
		if (e.state != "friend")
			continue;
		const std::string line = e.online ? "On now" : e.seenAgo >= 0 ? "On " + Friends::ago(e.seenAgo) : "";
		std::string stats;
		if (e.weekMinutes >= 0)
		{
			stats = std::to_string(e.weekMinutes) + " min this week";
			if (e.streakDays > 0)
				stats += "   " + std::to_string(e.streakDays) + "-day streak";
			if (!e.lastDrill.empty())
				stats += "   Last: " + e.lastDrill + (e.trainedAgo >= 0 ? " (" + Friends::ago(e.trainedAgo) + ")" : "");
		}
		else
			stats = "doesn't share their training";
		row(e, line, stats);
	}
	bool anySent = false;
	for (const auto& e : f.list)
	{
		if (e.state != "sent")
			continue;
		if (!anySent)
		{
			ImGui::Dummy(ImVec2(0, 4.f * u));
			section("SENT");
			anySent = true;
		}
		row(e, "waiting for them to accept (" + e.friendCode + ")", "");
	}
}

void Dashboard::renderUpdatesCard(const DashboardStatus& s)
{
	// Auto updates (2026-10-29): this version, the newest, what's new, and install now / automatically.
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	const UpdateView& v = s.update;
	settingsCardBegin("updates", "UPDATES");
	if (!v.selfUpdate)
	{
		// The BakkesPlugins build: BakkesMod installs new versions.
		settingRow(("Version " + v.current).c_str(), "BakkesMod keeps Free Touch up to date (BakkesPlugins).");
		ImGui::Dummy(ImVec2(0, 0));
		settingRowEnd();
	}
	else
	{
		std::string state;
		switch (v.state)
		{
		case UpdateState::Idle: state = "Checks a little after the game starts."; break;
		case UpdateState::Checking: state = "Checking..."; break;
		case UpdateState::UpToDate: state = "Up to date."; break;
		case UpdateState::Available: state = "Free Touch " + v.latest + " is available."; break;
		case UpdateState::Downloading:
			state = "Downloading " + v.latest + ": " + std::to_string(v.done) + " of " + std::to_string(v.total) + " files...";
			break;
		case UpdateState::Installed: state = v.latest + " is installed. Press F9 (or restart the game) to load it."; break;
		case UpdateState::Failed: state = v.error; break;
		}
		if (!v.justUpdated.empty() && v.state != UpdateState::Available && v.state != UpdateState::Installed)
			state = "Updated to " + v.justUpdated + ".";
		settingRow(("Version " + v.current).c_str(), state.c_str());
		if (v.state == UpdateState::Available)
		{
			if (navButton("Update now", ImVec2(150.f * u, 0.f)) && cb_.installUpdate)
				cb_.installUpdate();
		}
		else if (v.state != UpdateState::Checking && v.state != UpdateState::Downloading && v.state != UpdateState::Installed)
		{
			if (navButton("Check now", ImVec2(150.f * u, 0.f)) && cb_.checkUpdate)
			{
				cb_.checkUpdate();
				toast("Checking for updates");
			}
		}
		else
			ImGui::TextDisabled("%s", v.state == UpdateState::Installed ? "Ready" : "...");
		settingRowEnd();
		if (!v.notes.empty() && (v.state == UpdateState::Available || v.state == UpdateState::Downloading
			|| v.state == UpdateState::Installed || !v.justUpdated.empty()))
		{
			settingRow("What's new", v.notes.c_str());
			ImGui::Dummy(ImVec2(0, 0));
			settingRowEnd();
		}
		settingRow("Install updates automatically", "New versions download and install by themselves; they load the next time "
			"the game starts (or F9). Your drills, packs and settings are never touched.");
		bool on = v.autoInstall;
		if (switchToggle("auto_update", &on) && cb_.autoUpdateChanged)
			cb_.autoUpdateChanged(on);
		settingRowEnd();
	}
	(void)pal;
	settingsCardEnd();
}

void Dashboard::renderAccountCard(const DashboardStatus& s)
{
	// Phase 8 (Lambourne 2026-10-29): an optional account for friends, shared shots and leaderboards. A name is all it
	// takes; this PC keeps the sign-in.
	const float u = uiScale();
	const Account::View& a = s.account;
	const ImVec4 red(0.95f, 0.45f, 0.45f, 1.f);
	if (a.name != accountNameFrom_)
	{
		accountNameFrom_ = a.name;
		snprintf(accountName_, sizeof(accountName_), "%s", a.name.c_str());
	}
	settingsCardBegin("account", "ACCOUNT");
	if (a.state == Account::State::Working)
	{
		settingRow("Account", "Talking to the Free Touch service...");
		ImGui::TextDisabled("Connecting...");
		settingRowEnd();
	}
	else if (a.state == Account::State::SignedOut)
	{
		settingRow("Pick a name", "Optional: for friends, sending shots and leaderboards. No email or password; this PC keeps "
			"your sign-in. Everything else works without it.");
		ImGui::PushItemWidth(std::max(120.f, ImGui::CalcItemWidth() - 110.f * u));
		const bool enter = ImGui::InputTextWithHint("##account_name", "3 to 20 characters", accountName_, sizeof(accountName_),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::PopItemWidth();
		ImGui::SameLine(0.f, 8.f * u);
		if ((bigButton("account_create", "Create", ImVec2(100.f * u, 0.f), true, Icon::Play, false, Fx::Confirm) || enter)
			&& accountName_[0] && cb_.createAccount)
			cb_.createAccount(accountName_);
		settingRowEnd();

		settingRow("Have a recovery code?", "Bring your account to this PC: same name, same friend code. It signs out the PC "
			"it was on.");
		ImGui::PushItemWidth(std::max(120.f, ImGui::CalcItemWidth() - 110.f * u));
		const bool go = ImGui::InputTextWithHint("##recovery_in", "XXXXX-XXXXX-XXXXX-XXXXX", recoveryInput_, sizeof(recoveryInput_),
			ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::PopItemWidth();
		ImGui::SameLine(0.f, 8.f * u);
		if ((navButton("Restore", ImVec2(100.f * u, 0.f)) || go) && recoveryInput_[0] && cb_.restoreAccount)
		{
			cb_.restoreAccount(recoveryInput_);
			recoveryInput_[0] = 0;
		}
		settingRowEnd();
	}
	else
	{
		const bool offline = a.state == Account::State::Offline;
		settingRow("Name", offline ? "Offline: shown from last time. Your training works as usual."
			: "What your friends see. Names are unique.");
		ImGui::PushItemWidth(std::max(120.f, ImGui::CalcItemWidth() - 110.f * u));
		const bool enter = ImGui::InputText("##account_name", accountName_, sizeof(accountName_), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::PopItemWidth();
		ImGui::SameLine(0.f, 8.f * u);
		if (offline)
		{
			if (navButton("Try again", ImVec2(100.f * u, 0.f)) && cb_.retryAccount)
				cb_.retryAccount();
		}
		else if ((navButton("Rename", ImVec2(100.f * u, 0.f)) || enter) && a.name != accountName_ && cb_.renameAccount)
			cb_.renameAccount(accountName_);
		settingRowEnd();

		settingRow("Friend code", "Give it to a friend so they can add you (friends come next).");
		ImGui::TextUnformatted(a.friendCode.empty() ? "-" : a.friendCode.c_str());
		ImGui::SameLine(0.f, 12.f * u);
		if (!a.friendCode.empty() && navButton("Copy", ImVec2(80.f * u, 0.f)) && cb_.copyText)
			toast(cb_.copyText(a.friendCode) ? "Friend code copied" : "Couldn't use the clipboard");
		settingRowEnd();

		settingRow("Recovery code", "Moves your account to another PC, or back after a reinstall. Keep it somewhere safe "
			"(a note, or a DM to yourself); anyone with it can take the account.");
		if (a.recoveryCode.empty())
		{
			if (navButton("Make one", ImVec2(120.f * u, 0.f)) && cb_.makeRecoveryCode)
				cb_.makeRecoveryCode();
		}
		else
		{
			ImGui::TextUnformatted(showRecovery_ ? a.recoveryCode.c_str() : "*****-*****-*****-*****");
			ImGui::SameLine(0.f, 12.f * u);
			if (navButton(showRecovery_ ? "Hide" : "Show", ImVec2(70.f * u, 0.f)))
				showRecovery_ = !showRecovery_;
			ImGui::SameLine(0.f, 6.f * u);
			if (navButton("Copy##recovery", ImVec2(70.f * u, 0.f)) && cb_.copyText)
				toast(cb_.copyText(a.recoveryCode) ? "Recovery code copied" : "Couldn't use the clipboard");
			ImGui::SameLine(0.f, 6.f * u);
			if (navButton(confirmNewCode_ ? "Sure? New" : "New", ImVec2(90.f * u, 0.f)))
			{
				if (confirmNewCode_ && cb_.makeRecoveryCode)
				{
					cb_.makeRecoveryCode(); // the old one stops working
					confirmNewCode_ = false;
				}
				else
					confirmNewCode_ = true;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("A new code; the old one stops working");
		}
		settingRowEnd();

		settingRow("Sign out", confirmSignOut_ ? (a.recoveryCode.empty()
				? "You have no recovery code: signing out loses this account for good."
				: "You can come back with your recovery code. Copy it first!")
			: "Forget this account on this PC.");
		ImGui::PushStyleColor(ImGuiCol_Button, confirmSignOut_ ? ImVec4(0.75f, 0.2f, 0.25f, 1.f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
		if (navButton(confirmSignOut_ ? "Sure? Sign out" : "Sign out", ImVec2(150.f * u, 0.f)))
		{
			if (confirmSignOut_ && cb_.signOutAccount)
			{
				cb_.signOutAccount();
				toast("Signed out");
				confirmSignOut_ = false;
			}
			else
				confirmSignOut_ = true;
		}
		ImGui::PopStyleColor();
		if (confirmSignOut_)
		{
			ImGui::SameLine(0.f, 8.f * u);
			if (navButton("Keep it", ImVec2(100.f * u, 0.f)))
				confirmSignOut_ = false;
		}
		settingRowEnd();
	}
	if (!a.error.empty())
	{
		ImGui::PushStyleColor(ImGuiCol_Text, red);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(ImGui::GetContentRegionAvail().x, 900.f * u));
		ImGui::TextUnformatted(a.error.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
	}
	settingsCardEnd();
}

void Dashboard::renderSettings(const DashboardModel& m, const DashboardStatus& s)
{
	// Settings (polish pass): section cards; each setting a row with its name, one short line of help and its control on
	// the right. The settings themselves are unchanged.
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	pageTitle("Settings", "How Free Touch plays, sounds and looks.");

	renderAccountCard(s);
	renderUpdatesCard(s);
	settingsCardBegin("controls", "CONTROLS");
	if (bindings_)
		bindings_->render(cardInner_);
	settingRow("Refresh controls", "If a button stops doing the right thing (e.g. after changing controls in Rocket League).");
	if (navButton("Refresh controls") && cb_.refreshControls)
	{
		cb_.refreshControls();
		toast("Controls refreshed");
	}
	settingRowEnd();

	// The controller's Select button: one of Free Touch's actions (ft_select_command). Its own bind comes back when
	// Free Touch unloads.
	settingRow("Select button", "What the controller's Select (Back / View) button does while Free Touch is loaded.");
	{
		struct Action { const char* command; const char* label; bool dev; };
		static const Action kActions[] = {
			{"ft_menu", "Open the menu", false}, {"ft_retry", "Reset the shot", false}, {"ft_next", "Next shot", false},
			{"ft_prev", "Previous shot", false}, {"ft_stop", "Stop", false}, {"ft_slowmo", "Slow motion", false},
			{"ft_capture", "Save this moment", false}, {"ft_record", "Record", false}, {"ft_warmup", "Warm-up", false},
			{"", "Nothing", false}, {"ft_mark", "Mark a mechanic", true}};
		const float right = rowCtrlX_ + ImGui::CalcItemWidth();
		const float padX = ImGui::GetStyle().FramePadding.x * 2.f;
		bool first = true;
		for (const auto& a : kActions)
		{
			if (a.dev && !s.devTools && s.selectCommand != a.command)
				continue;
			if (!first && ImGui::GetItemRectMax().x + 6.f * uiScale() + ImGui::CalcTextSize(a.label).x + padX <= right)
				ImGui::SameLine(0.f, 6.f * uiScale());
			first = false;
			ImGui::PushID(a.label);
			if (chip(a.label, s.selectCommand == a.command) && cb_.selectCommandChanged && s.selectCommand != a.command)
			{
				cb_.selectCommandChanged(a.command);
				toast(std::string("Select: ") + a.label);
			}
			ImGui::PopID();
		}
	}
	settingRowEnd();
	settingsCardEnd();

	settingsCardBegin("gameplay", "GAMEPLAY");
	{
		settingRow("Goal explosions", "Your own explosion when you score. Off: the ball just stops there.");
		bool explosion = s.goalExplosion;
		if (switchToggle("explosion", &explosion) && cb_.goalExplosionChanged)
			cb_.goalExplosionChanged(explosion);
		settingRowEnd();

		settingRow("Goal replays", "On: goals go in for real and you watch the game's replay; the drill carries on after it. "
			"Off: the goal explodes and the next rep starts straight away. Every exercise.");
		bool replays = s.goalReplays;
		if (switchToggle("replays", &replays) && cb_.goalReplaysChanged)
			cb_.goalReplaysChanged(replays);
		settingRowEnd();

		settingRow("Opponent in shooting drills", nullptr); // the picker says what each one is
		opponentPicker("defender", s, ImGui::CalcItemWidth());
		settingRowEnd();

		// Arena for Free Play when something is started from Rocket League's main menu.
		static const std::pair<const char*, const char*> kArenas[] = {{"Stadium_P", "DFH Stadium"},
			{"EuroStadium_P", "Mannfield"}, {"cs_p", "Champions Field"}, {"Park_P", "Beckwith Park"},
			{"TrainStation_P", "Urban Central"}, {"UtopiaStadium_P", "Utopia Coliseum"}};
		// Chips that wrap inside the control column (it was a combo whose list was hard to read).
		settingRow("Free Play arena", "Where Free Play loads when you start something from the main menu.");
		{
			const float right = rowCtrlX_ + ImGui::CalcItemWidth();
			const float padX = ImGui::GetStyle().FramePadding.x * 2.f;
			for (size_t i = 0; i < std::size(kArenas); ++i)
			{
				const auto& [map, name] = kArenas[i];
				if (i > 0 && ImGui::GetItemRectMax().x + 6.f * u + ImGui::CalcTextSize(name).x + padX <= right)
					ImGui::SameLine(0.f, 6.f * u);
				ImGui::PushID(map);
				if (chip(name, s.freeplayMap == map) && cb_.freeplayMapChanged)
					cb_.freeplayMapChanged(map);
				ImGui::PopID();
			}
		}
		settingRowEnd();
	}
	settingsCardEnd();

	settingsCardBegin("practice", "PRACTICE SPEED");
	{
		PracticeSpeedSettings p = s.practice;
		bool changed = false;
		settingRow("Slow motion in drills", "The slow-motion button (D-pad down) slows the game; back to normal when the drill ends.");
		changed |= switchToggle("slowmo", &p.enabled);
		settingRowEnd();

		settingRow("Slow-motion button", "Press once to toggle, or slow only while held.");
		if (chip("Toggle", !p.hold)) { p.hold = false; changed = true; }
		ImGui::SameLine(0.f, 6.f);
		if (chip("Hold", p.hold)) { p.hold = true; changed = true; }
		settingRowEnd();

		settingRow("Practice speed", nullptr);
		int practice = static_cast<int>(p.practice * 100.f + 0.5f);
		if (ImGui::SliderInt("##practice", &practice, 30, 95, "%d%% of normal"))
		{
			p.practice = practice / 100.f;
			changed = true;
		}
		settingRowEnd();

		settingRow("Clutch speed", "Hold the clutch button for slower still (set it under Controls).");
		int clutch = static_cast<int>(p.clutch * 100.f + 0.5f);
		if (ImGui::SliderInt("##clutch", &clutch, 25, 90, "%d%% of practice speed"))
		{
			p.clutch = clutch / 100.f;
			changed = true;
		}
		settingRowEnd();
		if (changed && cb_.practiceChanged)
			cb_.practiceChanged(p);
	}
	settingsCardEnd();

	settingsCardBegin("sound", "SOUND AND DAILY GOAL");
	{
		// Drafts while a slider is held (the status copy only catches up after the change is saved).
		if (!sliderHeld_ && ImGui::GetTime() - sliderReleasedAt_ > 0.5)
		{
			menuDraft_ = std::round(s.menuVolume * 100.f);
			trainingDraft_ = std::round(s.trainingVolume * 100.f);
			goalDraft_ = static_cast<float>(s.home.goalMinutes);
		}
		bool held = false;
		settingRow("Menu sounds", "Hover, clicks and pages.");
		ImGui::SliderFloat("##menuvol", &menuDraft_, 0.f, 100.f, "%.0f%%");
		held |= ImGui::IsItemActive();
		const bool menuDone = ImGui::IsItemDeactivatedAfterEdit();
		settingRowEnd();
		settingRow("Training sounds", "Rep start, success and fail, rings and pins, new bests.");
		ImGui::SliderFloat("##trainvol", &trainingDraft_, 0.f, 100.f, "%.0f%%");
		held |= ImGui::IsItemActive();
		const bool trainingDone = ImGui::IsItemDeactivatedAfterEdit();
		settingRowEnd();
		if ((menuDone || trainingDone) && cb_.volumesChanged)
			cb_.volumesChanged(menuDraft_ / 100.f, trainingDraft_ / 100.f, true);

		settingRow("Daily goal", "Fills the ring on Home. A minute of training a day keeps your streak.");
		ImGui::SliderFloat("##goal", &goalDraft_, 5.f, 120.f, "%.0f min a day");
		held |= ImGui::IsItemActive();
		if (ImGui::IsItemDeactivatedAfterEdit() && cb_.dailyGoalChanged)
		{
			cb_.dailyGoalChanged(std::round(goalDraft_));
			toast("Daily goal: " + std::to_string(static_cast<int>(std::round(goalDraft_))) + " min a day");
		}
		settingRowEnd();
		if (sliderHeld_ && !held)
			sliderReleasedAt_ = ImGui::GetTime();
		sliderHeld_ = held;
	}
	settingsCardEnd();

	settingsCardBegin("look", "LOOK");
	{
		settingRow("Accent colour", "Buttons, highlights and the menu's details. Drag along the bar for any other colour.");
		static const ImVec4 presets[] = {
			{0.17f, 0.85f, 0.77f, 1.f}, {0.30f, 0.62f, 1.f, 1.f}, {1.f, 0.55f, 0.15f, 1.f},
			{0.95f, 0.30f, 0.45f, 1.f}, {0.62f, 0.45f, 1.f, 1.f}, {0.55f, 0.88f, 0.30f, 1.f},
		};
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const auto close = [](const ImVec4& a, const ImVec4& b) {
			return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) < 0.03f;
		};
		// Round swatches; the one in use has a ring.
		const float d = 30.f * u;
		for (int i = 0; i < 6; ++i)
		{
			if (i > 0)
				ImGui::SameLine(0.f, 10.f * u);
			ImGui::PushID(i);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const bool clicked = navInvisible("##preset", ImVec2(d, d), 1, Fx::Click);
			const ImVec2 c(p.x + d * 0.5f, p.y + d * 0.5f);
			const bool selected = close(presets[i], pal.accent);
			dl->AddCircleFilled(c, d * 0.5f - (selected ? 4.f * u : 0.f) - (ImGui::IsItemHovered() && !selected ? 1.f * u : 0.f),
				theme::u32(presets[i]), 32);
			if (selected)
				dl->AddCircle(c, d * 0.5f, theme::u32(pal.text), 32, 2.f * u);
			if (clicked)
			{
				theme::setAccent(presets[i]);
				if (cb_.accentChanged)
					cb_.accentChanged(presets[i]);
			}
			ImGui::PopID();
		}
		// Any other colour: a hue bar under the swatches (it was a colour picker pop-up full of numbers).
		{
			ImGui::Dummy(ImVec2(0.f, 4.f * u));
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const ImVec2 size(ImGui::CalcItemWidth(), 14.f * u);
			ImGui::InvisibleButton("##hue", ImVec2(size.x, size.y + 6.f * u));
			const float barY = p.y + 3.f * u;
			static const ImU32 stops[] = {IM_COL32(255, 0, 0, 255), IM_COL32(255, 255, 0, 255), IM_COL32(0, 255, 0, 255),
				IM_COL32(0, 255, 255, 255), IM_COL32(0, 0, 255, 255), IM_COL32(255, 0, 255, 255), IM_COL32(255, 0, 0, 255)};
			const float seg = size.x / 6.f;
			for (int i = 0; i < 6; ++i)
				dl->AddRectFilledMultiColor(ImVec2(p.x + seg * i, barY), ImVec2(p.x + seg * (i + 1), barY + size.y), stops[i], stops[i + 1],
					stops[i + 1], stops[i]);
			float h = 0.f, sat = 0.f, val = 0.f;
			ImGui::ColorConvertRGBtoHSV(pal.accent.x, pal.accent.y, pal.accent.z, h, sat, val);
			if (ImGui::IsItemActive())
			{
				h = std::clamp((ImGui::GetIO().MousePos.x - p.x) / size.x, 0.f, 0.999f);
				ImVec4 picked(0.f, 0.f, 0.f, 1.f);
				ImGui::ColorConvertHSVtoRGB(h, 0.78f, 0.95f, picked.x, picked.y, picked.z);
				theme::setAccent(picked);
			}
			if (ImGui::IsItemDeactivated() && cb_.accentChanged)
				cb_.accentChanged(theme::palette().accent);
			const ImVec2 knob(p.x + h * size.x, barY + size.y * 0.5f);
			dl->AddCircleFilled(knob, size.y * 0.5f + 3.f * u, theme::u32(pal.accent), 24);
			dl->AddCircle(knob, size.y * 0.5f + 3.f * u, IM_COL32(245, 248, 252, 255), 24, 2.f * u);
		}
		settingRowEnd();
	}
	settingsCardEnd();

	settingsCardBegin("advanced", "ADVANCED");
	{
		settingRow("Tips", "The short tutorial each place shows the first time it opens (the ? button shows it any time).");
		if (navButton("Show the tips again", ImVec2(0.f, 0.f)) && cb_.resetTutorials)
		{
			cb_.resetTutorials();
			tutorialsShown_.clear();
			toast("Tips will show again");
		}
		settingRowEnd();

		settingRow("Developer tools", "Saved Shots, the Captured and Dev categories, the mechanics feed.");
		bool dev = s.devTools;
		if (switchToggle("devtools", &dev) && cb_.devToolsChanged)
			cb_.devToolsChanged(dev);
		settingRowEnd();
		if (s.devTools)
		{
			settingRow("Mechanics feed", "Names what you just did on the left of the screen, with a chime for the big ones.");
			bool feed = s.mechanicsFeed;
			if (switchToggle("feed", &feed) && cb_.mechanicsFeedChanged)
				cb_.mechanicsFeedChanged(feed);
			settingRowEnd();
		}
		char buf[160];
		snprintf(buf, sizeof(buf), "%d exercises, %d sessions%s", static_cast<int>(m.exercises.size()), static_cast<int>(m.sessions.size()),
			m.contentErrors > 0 ? ", with errors" : m.contentWarnings > 0 ? ", with warnings" : "");
		settingRow("Content", buf);
		if (navButton("Reload content") && cb_.reloadContent)
		{
			cb_.reloadContent();
			toast("Content reloaded");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s\n%d error(s), %d warning(s) in the last load", m.contentRoot.c_str(), m.contentErrors, m.contentWarnings);
		settingRowEnd();
	}
	settingsCardEnd();

	ImGui::TextDisabled("Free Touch v%s. Offline freeplay only.", version_.c_str());
}

float Dashboard::footerHeight() const
{
	// The status bar's room under the page: its line plus space above and below, growing with the GUI scale (a fixed
	// 36 px left the controller hints touching the window's bottom edge at a big scale).
	return std::max(36.f, ImGui::GetTextLineHeight() + 30.f * uiScale());
}

void Dashboard::renderStatusBar(const DashboardModel& m, const DashboardStatus& s)
{
	const theme::Palette& pal = theme::palette();
	// Centred in the room footerHeight() keeps (the cursor starts an item spacing below the page).
	const float room = footerHeight() - ImGui::GetStyle().ItemSpacing.y;
	ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.f, (room - ImGui::GetTextLineHeight()) * 0.5f - 2.f));
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 30.f);
	ImGui::TextDisabled("v%s", version_.c_str());
	ImGui::SameLine(0.f, 18.f);
	if (s.exerciseActive)
	{
		// Always reachable: Stop, on every page.
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f, 3.f));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.25f, 0.85f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.26f, 0.3f, 1.f));
		if (navButton(s.sessionActive ? "End session" : "Stop exercise") && cb_.stop)
			cb_.stop();
		ImGui::PopStyleColor(2);
		ImGui::PopStyleVar();
		ImGui::SameLine(0.f, 12.f);
		ImGui::TextDisabled("Rep %d  |  %s  |  %s", s.rep, s.exerciseName.c_str(), s.difficulty.c_str());
	}
	else
		ImGui::TextDisabled("%s", s.inFreeplay ? "In freeplay" : "Not in freeplay");
	if (m.contentErrors > 0)
	{
		ImGui::SameLine(0.f, 18.f);
		ImGui::TextColored(pal.orange, "%d content error(s): see Settings", m.contentErrors);
	}

	// Controller hints on the right (the D-pad, A, B and Start drive the menu while it is open).
	struct Hint { const char* key; const char* what; };
	static const Hint hints[] = {{"A", "Select"}, {"B", "Back"}, {"D-pad", "Move"}, {"Start", "Close"}};
	const float u = uiScale();
	const float ks = ImGui::GetFontSize() * 0.78f;
	ImFont* keyFont = theme::fontAt(ks * 1.05f);
	float total = 0.f;
	for (const auto& h : hints)
		total += keyFont->CalcTextSizeA(ks, FLT_MAX, 0.f, h.key).x + 12.f * u + 6.f * u + ImGui::CalcTextSize(h.what).x + 18.f * u;
	const float right = ImGui::GetWindowContentRegionMax().x - 20.f * u;
	ImGui::SameLine(0.f, 0.f);
	const float x0 = std::max(ImGui::GetCursorPosX() + 24.f * u, right - total);
	if (x0 + total <= right + 1.f)
	{
		ImGui::SetCursorPosX(x0);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();
		const float lineH = ImGui::GetTextLineHeight();
		for (const auto& h : hints)
		{
			const ImVec2 kt = keyFont->CalcTextSizeA(ks, FLT_MAX, 0.f, h.key);
			const ImVec2 ka(p.x, p.y + (lineH - kt.y) * 0.5f - 2.f * u);
			const ImVec2 kb(ka.x + kt.x + 12.f * u, ka.y + kt.y + 4.f * u);
			dl->AddRectFilled(ka, kb, theme::u32(theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.10f)), 5.f);
			dl->AddText(keyFont, ks, ImVec2(ka.x + 6.f * u, ka.y + 2.f * u), theme::u32(pal.text), h.key);
			p.x = kb.x + 6.f * u;
			dl->AddText(p, theme::u32(pal.textDim), h.what);
			p.x += ImGui::CalcTextSize(h.what).x + 18.f * u;
		}
		ImGui::Dummy(ImVec2(total, lineH));
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Progress page

namespace
{
	// Rank group colours: unranked, bronze, silver, gold, platinum, diamond, champion, grand champion, supersonic legend.
	ImU32 rankColor(int tier, float alpha = 1.f)
	{
		static const ImVec4 kColors[] = {
			{0.55f, 0.58f, 0.64f, 1.f}, {0.80f, 0.52f, 0.30f, 1.f}, {0.75f, 0.78f, 0.84f, 1.f}, {0.98f, 0.80f, 0.30f, 1.f},
			{0.40f, 0.85f, 0.85f, 1.f}, {0.35f, 0.60f, 1.00f, 1.f}, {0.70f, 0.45f, 1.00f, 1.f}, {1.00f, 0.35f, 0.35f, 1.f},
			{0.97f, 0.97f, 1.00f, 1.f}};
		const int group = tier <= 0 ? 0 : tier >= 22 ? 8 : 1 + (tier - 1) / 3;
		return theme::u32(kColors[group], alpha);
	}

	const char* kRankGroups[] = {"Bronze", "Silver", "Gold", "Platinum", "Diamond", "Champion", "Grand Champion", "SSL"};

	// Approximate MMR where each rank group starts (it moves a little every season): Duel, then Doubles / Standard.
	float rankGroupFloor(int group, bool duel)
	{
		static const float kTeam[] = {0.f, 290.f, 470.f, 655.f, 835.f, 1075.f, 1435.f, 1860.f};
		static const float kDuel[] = {0.f, 270.f, 450.f, 630.f, 810.f, 990.f, 1170.f, 1360.f};
		return (duel ? kDuel : kTeam)[std::clamp(group, 0, 7)];
	}

	const ImU32 kGold = IM_COL32(255, 205, 80, 255);
	ImU32 medalColor(int tier)
	{
		return tier >= 3 ? kGold : tier == 2 ? IM_COL32(205, 214, 228, 255) : IM_COL32(222, 146, 90, 255);
	}

	std::string hoursText(double minutes)
	{
		char buf[32];
		if (minutes < 60.0)
			snprintf(buf, sizeof(buf), "%.0f min", minutes);
		else
			snprintf(buf, sizeof(buf), "%.0fh %02.0fm", std::floor(minutes / 60.0), std::floor(std::fmod(minutes, 60.0)));
		return buf;
	}

	// "Tue 14 Oct" from "2026-10-14".
	std::string niceDay(const std::string& day)
	{
		int y = 0, m = 0, d = 0;
		if (std::sscanf(day.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
			return day;
		static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		static const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
		int yy = y, mm = m;
		if (mm < 3)
		{
			mm += 12;
			--yy;
		}
		const int k = yy % 100, j = yy / 100;
		const int h = (d + 13 * (mm + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7; // 0 = Saturday
		char buf[32];
		snprintf(buf, sizeof(buf), "%s %d %s", kDays[(h + 6) % 7], d, kMonths[std::clamp(m, 1, 12) - 1]);
		return buf;
	}

	// A small up / down arrow with a number: "+12" in the accent, "-8" in red.
	void trendText(ImDrawList* dl, ImVec2 at, float delta, const char* unit, float size)
	{
		const theme::Palette& pal = theme::palette();
		if (std::fabs(delta) < 0.5f)
		{
			dl->AddText(theme::fontAt(size), size, at, theme::u32(pal.textDim), "steady");
			return;
		}
		const bool up = delta > 0.f;
		const ImU32 col = up ? theme::u32(pal.accent) : IM_COL32(255, 110, 110, 255);
		const float s = size * 0.38f;
		const ImVec2 c(at.x + s, at.y + size * 0.52f);
		if (up)
			dl->AddTriangleFilled(ImVec2(c.x - s, c.y + s * 0.6f), ImVec2(c.x + s, c.y + s * 0.6f), ImVec2(c.x, c.y - s * 0.7f), col);
		else
			dl->AddTriangleFilled(ImVec2(c.x - s, c.y - s * 0.6f), ImVec2(c.x + s, c.y - s * 0.6f), ImVec2(c.x, c.y + s * 0.7f), col);
		char buf[32];
		snprintf(buf, sizeof(buf), "%+.0f%s", delta, unit);
		dl->AddText(theme::fontAt(size), size, ImVec2(at.x + s * 2.f + 5.f, at.y), col, buf);
	}

	// A section heading: a short accent bar, the title, and a hairline running to the right edge.
	void heading(const char* title, float width, const char* right = nullptr)
	{
		const theme::Palette& pal = theme::palette();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize() * 0.82f;
		const ImVec2 at = ImGui::GetCursorScreenPos();
		dl->AddRectFilled(ImVec2(at.x, at.y + 2.f), ImVec2(at.x + 3.f, at.y + fs + 1.f), theme::u32(pal.accent), 2.f);
		dl->AddText(theme::fontAt(fs), fs, ImVec2(at.x + 11.f, at.y), theme::u32(pal.text, 0.92f), title);
		float x = at.x + 11.f + theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, 0.f, title).x + 12.f;
		float endX = at.x + width;
		if (right)
		{
			const float rw = theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, 0.f, right).x;
			dl->AddText(theme::fontAt(fs), fs, ImVec2(endX - rw, at.y), theme::u32(pal.textDim), right);
			endX -= rw + 12.f;
		}
		if (endX > x)
			dl->AddLine(ImVec2(x, at.y + fs * 0.6f), ImVec2(endX, at.y + fs * 0.6f), IM_COL32(255, 255, 255, 18), 1.f);
		ImGui::Dummy(ImVec2(width, fs + 10.f));
	}

	// A speech-bubble label above a point (chart hover).
	void bubble(ImDrawList* dl, ImVec2 tip, const std::string& top, const std::string& bottom)
	{
		const theme::Palette& pal = theme::palette();
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize();
		const ImVec2 ts = theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, 0.f, top.c_str());
		const ImVec2 bs = bottom.empty() ? ImVec2(0, 0) : theme::fontAt(fs * 0.8f)->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0.f, bottom.c_str());
		const float w = std::max(ts.x, bs.x) + 20.f, h = ts.y + (bottom.empty() ? 0.f : bs.y + 2.f) + 12.f;
		const ImVec2 a(tip.x - w * 0.5f, tip.y - h - 9.f), b(a.x + w, a.y + h);
		dl->AddRectFilled(ImVec2(a.x + 1.f, a.y + 3.f), ImVec2(b.x + 1.f, b.y + 3.f), IM_COL32(0, 0, 0, 80), 8.f);
		dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.18f)), 8.f);
		dl->AddRect(a, b, theme::u32(pal.accent, 0.5f), 8.f);
		dl->AddTriangleFilled(ImVec2(tip.x - 6.f, b.y), ImVec2(tip.x + 6.f, b.y), ImVec2(tip.x, b.y + 6.f),
			theme::u32(theme::mix(pal.card, pal.accent, 0.18f)));
		dl->AddText(theme::fontAt(fs), fs, ImVec2(tip.x - ts.x * 0.5f, a.y + 6.f), theme::u32(pal.text), top.c_str());
		if (!bottom.empty())
			dl->AddText(theme::fontAt(fs * 0.8f), fs * 0.8f, ImVec2(tip.x - bs.x * 0.5f, a.y + 8.f + ts.y), theme::u32(pal.textDim), bottom.c_str());
	}

	// Small line icons for the headline numbers.
	enum class StatGlyph { Clock, Flame, Target, Calendar };
	void glyph(ImDrawList* dl, StatGlyph g, ImVec2 c, float r, ImU32 col)
	{
		switch (g)
		{
		case StatGlyph::Clock:
			dl->AddCircle(c, r, col, 24, 1.8f);
			dl->AddLine(c, ImVec2(c.x, c.y - r * 0.6f), col, 1.8f);
			dl->AddLine(c, ImVec2(c.x + r * 0.45f, c.y + r * 0.2f), col, 1.8f);
			break;
		case StatGlyph::Flame:
		{
			const ImVec2 pts[] = {{c.x, c.y - r}, {c.x + r * 0.7f, c.y + r * 0.1f}, {c.x + r * 0.45f, c.y + r * 0.85f},
				{c.x - r * 0.45f, c.y + r * 0.85f}, {c.x - r * 0.7f, c.y + r * 0.1f}};
			dl->AddPolyline(pts, 5, col, true, 1.8f);
			dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.35f), r * 0.28f, col, 12);
			break;
		}
		case StatGlyph::Target:
			dl->AddCircle(c, r, col, 24, 1.8f);
			dl->AddCircle(c, r * 0.55f, col, 20, 1.8f);
			dl->AddCircleFilled(c, r * 0.18f, col, 10);
			break;
		case StatGlyph::Calendar:
			dl->AddRect(ImVec2(c.x - r, c.y - r * 0.8f), ImVec2(c.x + r, c.y + r * 0.9f), col, 3.f, ImDrawCornerFlags_All, 1.8f);
			dl->AddLine(ImVec2(c.x - r, c.y - r * 0.3f), ImVec2(c.x + r, c.y - r * 0.3f), col, 1.8f);
			for (int i = 0; i < 3; ++i)
				dl->AddCircleFilled(ImVec2(c.x - r * 0.5f + i * r * 0.5f, c.y + r * 0.3f), 1.8f, col, 6);
			break;
		}
	}

	// A card that sizes itself to what is drawn inside it (draw list channels: the card goes behind). `hover` (0..1,
	// animated by the caller) lifts its border toward the accent.
	struct CardScope
	{
		ImDrawList* dl;
		ImVec2 pos;
		float width;
		float pad;
		float* hover;
		CardScope(float w, float padding = 18.f, float* hoverAnim = nullptr) : dl(ImGui::GetWindowDrawList()), width(w), pad(padding),
			hover(hoverAnim)
		{
			pos = ImGui::GetCursorScreenPos();
			dl->ChannelsSplit(2);
			dl->ChannelsSetCurrent(1);
			ImGui::SetCursorScreenPos(ImVec2(pos.x + pad, pos.y + pad));
			ImGui::BeginGroup();
		}
		float inner() const { return width - 2.f * pad; }
		~CardScope()
		{
			ImGui::EndGroup();
			const ImVec2 end(pos.x + width, ImGui::GetItemRectMax().y + pad);
			float h = 0.f;
			if (hover)
			{
				const bool in = ImGui::IsMouseHoveringRect(pos, end) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
				*hover = approach(*hover, in ? 1.f : 0.f, 8.f);
				h = *hover;
			}
			dl->ChannelsSetCurrent(0);
			const theme::Palette& pal = theme::palette();
			dl->AddRectFilled(pos, end, theme::u32(theme::mix(pal.card, pal.accent, 0.03f + 0.03f * h)), 14.f);
			// A faint light along the top edge.
			dl->AddRectFilledMultiColor(ImVec2(pos.x + 14.f, pos.y + 1.f), ImVec2(end.x - 14.f, pos.y + 22.f), IM_COL32(255, 255, 255, 7),
				IM_COL32(255, 255, 255, 7), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
			dl->AddRect(pos, end, theme::u32(theme::mix(ImVec4(1, 1, 1, 0.065f), pal.accent, 0.45f * h)), 14.f, ImDrawCornerFlags_All, 1.f);
			dl->ChannelsMerge();
			ImGui::SetCursorScreenPos(ImVec2(pos.x, end.y));
			ImGui::Dummy(ImVec2(width, 0.f));
		}
	};
}

void Dashboard::renderProgress(const DashboardModel& m, const DashboardStatus& s)
{
	const theme::Palette& pal = theme::palette();
	const double now = ImGui::GetTime();
	if (progressOpenedAt_ < pageAt_)
	{
		progressOpenedAt_ = now;
		tabAt_ = now;
		if (cb_.progressOpened)
			cb_.progressOpened(); // read the MMR now
	}
	pageTitle("Progress", "What you have trained on this PC, your rank over time, milestones and your journal.");
	const ProgressView* p = s.progress.get();
	if (!p)
	{
		ImGui::TextDisabled("Loading...");
		return;
	}
	const float width = ImGui::GetContentRegionAvail().x;
	static const char* kTabs[] = {"Overview", "Rank", "Training", "Milestones", "Journal"};
	const int tab = segmented("progress_tab", kTabs, 5, progressTab_, std::min(width, 660.f), progressTabAnim_);
	if (tab != progressTab_)
	{
		progressTab_ = tab;
		tabAt_ = now;
	}
	ImGui::Dummy(ImVec2(0, 8));
	const ImVec2 contentTop = ImGui::GetCursorScreenPos();
	switch (progressTab_)
	{
	case 0: renderProgressOverview(m, s, *p); break;
	case 1: renderProgressRank(*p, false); break;
	case 2: renderProgressTraining(m, *p); break;
	case 3: renderProgressMilestones(*p, false); break;
	case 4: renderProgressJournal(*p); break;
	}
	ImGui::Dummy(ImVec2(0, 12));
	// Switching tabs: the new one fades up from the background.
	const float in = easeOut(static_cast<float>((now - tabAt_) / 0.28));
	if (in < 1.f)
	{
		const ImVec4 bg = pal.bg;
		const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
		ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(contentTop.x - 4.f, std::max(contentTop.y, wp.y)),
			ImVec2(contentTop.x + width + 4.f, wp.y + ws.y), theme::u32(ImVec4(bg.x, bg.y, bg.z, 1.f), 1.f - in));
	}
}

void Dashboard::renderProgressHero(const ProgressView& p)
{
	// Four headline numbers that count up: time trained, the streak, reps and success, days trained.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const double now = ImGui::GetTime();
	const float count = easeOut(static_cast<float>((now - tabAt_) / 1.0));
	const float width = ImGui::GetContentRegionAvail().x;
	const float gap = 12.f;
	const int perRow = width >= 780.f ? 4 : 2;
	const float cellW = (width - (perRow - 1) * gap) / perRow, cellH = 108.f;
	const ImVec2 row = ImGui::GetCursorScreenPos();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	char buf[64];
	static const StatGlyph kGlyphs[] = {StatGlyph::Clock, StatGlyph::Flame, StatGlyph::Target, StatGlyph::Calendar};
	for (int i = 0; i < 4; ++i)
	{
		const float rise = easeOut(static_cast<float>((now - tabAt_ - 0.07 * i) / 0.4));
		const ImVec2 slot(row.x + (i % perRow) * (cellW + gap), row.y + (i / perRow) * (cellH + gap));
		float& hv = hoverAnim_["progress.hero" + std::to_string(i)];
		hv = approach(hv, ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + cellW, slot.y + cellH)) ? 1.f : 0.f, 8.f);
		ImVec2 a(slot.x, slot.y + 14.f * (1.f - rise) - 3.f * hv);
		ImVec2 b(a.x + cellW, a.y + cellH);
		cellCard(dl, a, b, hv * 0.6f);
		const ImU32 tint = i == 1 && p.streak > 0 ? theme::u32(pal.orange) : theme::u32(pal.accent);
		dl->AddCircleFilled(ImVec2(b.x - 30.f, a.y + 30.f), 17.f, (tint & 0x00FFFFFF) | 0x26000000, 24);
		glyph(dl, kGlyphs[i], ImVec2(b.x - 30.f, a.y + 30.f), 9.f, tint);
		if (i == 0)
		{
			const std::string t = hoursText(p.totalMinutes * count);
			statText(dl, ImVec2(a.x + 18, a.y + 16), t.c_str(), "TIME TRAINED", theme::u32(pal.text));
		}
		else if (i == 1)
		{
			snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(p.streak * count)));
			statText(dl, ImVec2(a.x + 18, a.y + 16), buf, "DAY STREAK", theme::u32(p.streak > 0 ? pal.orange : pal.text));
			snprintf(buf, sizeof(buf), "best %d", p.bestStreak);
			const ImVec2 ts = theme::fontAt(fs * 0.82f)->CalcTextSizeA(fs * 0.82f, FLT_MAX, 0.f, buf);
			dl->AddText(theme::fontAt(fs * 0.82f), fs * 0.82f, ImVec2(b.x - 16.f - ts.x, b.y - 26.f), theme::u32(pal.textDim), buf);
		}
		else if (i == 2)
		{
			snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(p.totalReps * count)));
			statText(dl, ImVec2(a.x + 18, a.y + 16), buf, "REPS", theme::u32(pal.text));
			const float pct = p.totalReps > 0 ? static_cast<float>(p.totalSuccesses) / p.totalReps : 0.f;
			snprintf(buf, sizeof(buf), "%d%% success", static_cast<int>(pct * 100.f * count + 0.5f));
			const ImVec2 ts = theme::fontAt(fs * 0.82f)->CalcTextSizeA(fs * 0.82f, FLT_MAX, 0.f, buf);
			dl->AddText(theme::fontAt(fs * 0.82f), fs * 0.82f, ImVec2(b.x - 16.f - ts.x, b.y - 26.f), theme::u32(pal.accent), buf);
		}
		else
		{
			snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(p.daysTrained * count)));
			statText(dl, ImVec2(a.x + 18, a.y + 16), buf, "DAYS TRAINED", theme::u32(pal.text));
			// The last 14 days as a tiny bar sparkline.
			const int n = std::min<int>(14, static_cast<int>(p.days.size()));
			float mx = 1.f;
			for (int d = 0; d < n; ++d)
				mx = std::max(mx, p.days[p.days.size() - n + d].minutes);
			const float bw = 4.f, bx0 = b.x - 16.f - n * (bw + 2.f);
			for (int d = 0; d < n; ++d)
			{
				const float v = p.days[p.days.size() - n + d].minutes / mx * count;
				const float h = std::max(2.f, 18.f * v);
				dl->AddRectFilled(ImVec2(bx0 + d * (bw + 2.f), b.y - 14.f - h), ImVec2(bx0 + d * (bw + 2.f) + bw, b.y - 14.f),
					theme::u32(v > 0.01f ? pal.accent : pal.textDim, v > 0.01f ? 0.9f : 0.25f), 1.5f);
			}
		}
		if (rise < 1.f)
		{
			const ImVec4 bg = pal.bg;
			dl->AddRectFilled(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), theme::u32(ImVec4(bg.x, bg.y, bg.z, 1.f), 1.f - rise), 12.f);
		}
	}
	ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + ((3 + perRow) / perRow) * (cellH + gap) - gap + 18.f));
}

void Dashboard::renderProgressOverview(const DashboardModel& m, const DashboardStatus& s, const ProgressView& p)
{
	const theme::Palette& pal = theme::palette();
	renderProgressHero(p);
	const float width = ImGui::GetContentRegionAvail().x;
	const bool two = width >= 860.f;
	const float colW = two ? (width - 16.f) * 0.5f : width;
	const ImVec2 top = ImGui::GetCursorScreenPos();

	// Left: rank at a glance (every playlist) and the last two weeks.
	ImGui::BeginGroup();
	{
		CardScope card(colW, 18.f, &hoverAnim_["progress.rankcard"]);
		heading("RANK", card.inner(), "this week");
		bool any = false;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize();
		for (const auto& pl : p.playlists)
		{
			if (pl.mmr <= 0.f)
				continue;
			any = true;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			// A rank-coloured chip with the playlist, the number, the rank name, the week's change.
			dl->AddRectFilled(ImVec2(at.x, at.y + 4.f), ImVec2(at.x + 4.f, at.y + fs * 2.6f), rankColor(pl.tier), 2.f);
			dl->AddText(theme::fontAt(fs * 0.8f), fs * 0.8f, ImVec2(at.x + 14.f, at.y + 2.f), theme::u32(pal.textDim), pl.label.c_str());
			char buf[32];
			snprintf(buf, sizeof(buf), "%.0f", pl.mmr);
			dl->AddText(theme::fontAt(fs * 1.45f), fs * 1.45f, ImVec2(at.x + 14.f, at.y + fs * 0.95f), theme::u32(pal.text), buf);
			const float numW = theme::fontAt(fs * 1.45f)->CalcTextSizeA(fs * 1.45f, FLT_MAX, 0.f, buf).x;
			if (pl.tier > 0)
				dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(at.x + 24.f + numW, at.y + fs * 1.4f), rankColor(pl.tier),
					fitText(pl.rank, card.inner() - numW - 140.f).c_str());
			if (pl.hasDelta)
				trendText(dl, ImVec2(at.x + card.inner() - 70.f, at.y + fs * 1.2f), pl.delta, "", fs * 0.95f);
			ImGui::Dummy(ImVec2(card.inner(), fs * 2.9f));
		}
		if (!any)
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card.inner());
			ImGui::TextDisabled("No rank yet. Play a ranked match and Free Touch saves your MMR once a day from the game's own "
				"numbers (nothing goes online), or type it in on the Rank tab.");
			ImGui::PopTextWrapPos();
			ImGui::Dummy(ImVec2(0, 4));
		}
		if (navButton("Open Rank  >"))
		{
			progressTab_ = 1;
			tabAt_ = ImGui::GetTime();
		}
	}
	ImGui::Dummy(ImVec2(0, 14));
	{
		CardScope card(colW, 18.f, &hoverAnim_["progress.dayscard"]);
		heading("LAST 14 DAYS", card.inner(), "minutes a day");
		drawMinuteBars(p, 14, card.inner(), 128.f);
	}
	ImGui::EndGroup();
	const float leftBottom = ImGui::GetItemRectMax().y;

	// Right: what to work on, and the next milestones.
	ImGui::SetCursorScreenPos(two ? ImVec2(top.x + colW + 16.f, top.y) : ImVec2(top.x, leftBottom + 14.f));
	ImGui::BeginGroup();
	{
		CardScope card(colW, 18.f, &hoverAnim_["progress.weakcard"]);
		heading("WORK ON THIS", card.inner(), "lowest success");
		renderWeakSpots(m, s, p, card.inner());
	}
	ImGui::Dummy(ImVec2(0, 14));
	{
		CardScope card(colW, 18.f, &hoverAnim_["progress.nextcard"]);
		int done = 0;
		for (const auto& ms : p.milestones)
			done += ms.done;
		char right[32];
		snprintf(right, sizeof(right), "%d / %d done", done, static_cast<int>(p.milestones.size()));
		heading("NEXT MILESTONES", card.inner(), right);
		std::vector<const ProgressView::Milestone*> next;
		for (const auto& ms : p.milestones)
			if (!ms.done)
				next.push_back(&ms);
		std::sort(next.begin(), next.end(), [](auto* a, auto* b) { return a->progress > b->progress; });
		if (next.size() > 3)
			next.resize(3);
		for (const auto* ms : next)
			milestoneRow(*ms, card.inner());
		if (navButton("All milestones  >"))
		{
			progressTab_ = 3;
			tabAt_ = ImGui::GetTime();
		}
	}
	ImGui::EndGroup();
	const float rightBottom = ImGui::GetItemRectMax().y;
	ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(leftBottom, rightBottom) + 8.f));
	ImGui::Dummy(ImVec2(width, 0.f));
}

void Dashboard::renderWeakSpots(const DashboardModel& m, const DashboardStatus& s, const ProgressView& p, float width)
{
	const theme::Palette& pal = theme::palette();
	if (p.weakSpots.empty())
	{
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
		ImGui::TextDisabled("Nothing stands out yet. Play a few drills (6+ reps each) and the ones you find hardest show up here, "
			"one click from practice.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 4));
		return;
	}
	const double now = ImGui::GetTime();
	int index = 0;
	for (const auto& w : p.weakSpots)
	{
		ImGui::PushID(w.id.c_str());
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 at = ImGui::GetCursorScreenPos();
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize();
		const float u = uiScale();
		const float rowH = 56.f * u;
		float& hv = hoverAnim_["progress.weak." + w.id];
		hv = approach(hv, ImGui::IsMouseHoveringRect(at, ImVec2(at.x + width, at.y + rowH)) ? 1.f : 0.f, 9.f);
		if (hv > 0.01f)
			dl->AddRectFilled(ImVec2(at.x - 8.f, at.y - 2.f), ImVec2(at.x + width + 8.f, at.y + rowH - 4.f), theme::u32(pal.accent, 0.05f * hv), 10.f);
		// Success ring, filling in on entering.
		const float grow = easeOut(static_cast<float>((now - tabAt_ - 0.1 * index) / 0.7));
		const ImVec2 c(at.x + 22.f * u, at.y + 24.f * u);
		dl->AddCircle(c, 19.f * u, theme::u32(pal.textDim, 0.2f), 32, 4.f);
		if (w.success > 0.005f)
		{
			dl->PathArcTo(c, 19.f * u, -1.5707963f, -1.5707963f + 6.2831853f * w.success * grow, 32);
			dl->PathStroke(IM_COL32(255, 150, 90, 255), false, 4.f);
		}
		char buf[16];
		snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(w.success * 100.f * grow + 0.5f));
		const ImVec2 ts = theme::fontAt(fs * 0.75f)->CalcTextSizeA(fs * 0.75f, FLT_MAX, 0.f, buf);
		dl->AddText(theme::fontAt(fs * 0.75f), fs * 0.75f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), theme::u32(pal.text), buf);
		// The buttons at the right, sized to their text: Practice, then + (or a tick once it is in the warm-up).
		const float bfs = ImGui::GetFontSize() * 1.1f;
		const float btnH = std::max(32.f * u, bfs * 1.6f);
		const float practiceW = theme::fontAt(bfs)->CalcTextSizeA(bfs, FLT_MAX, 0.f, "Practice").x + 24.f * u;
		const float addW = btnH;
		const float buttonsW = practiceW + 6.f * u + addW;
		const float textX = 54.f * u;
		const float textW = width - textX - buttonsW - 12.f * u;
		dl->AddText(theme::fontAt(fs), fs, ImVec2(at.x + textX, at.y + 6.f * u), theme::u32(pal.text), fitText(w.name, textW).c_str());
		dl->AddText(theme::fontAt(fs * 0.82f), fs * 0.82f, ImVec2(at.x + textX, at.y + 8.f * u + fs * 1.2f), theme::u32(pal.textDim), fitText(w.why, textW).c_str());
		ImGui::SetCursorScreenPos(ImVec2(at.x + width - buttonsW, at.y + (rowH - 4.f * u - btnH) * 0.5f));
		if (bigButton("practice", "Practice", ImVec2(practiceW, btnH), true, Icon::Play, false, Fx::Confirm) && cb_.play)
		{
			cb_.play(w.id, kDifficulties[difficulty_]);
			closeRequested_ = true;
		}
		ImGui::SameLine(0.f, 6.f * u);
		bool inWarmup = false;
		if (s.routine)
			for (const auto& step : s.routine->steps)
				for (const auto& d : step.drills)
					inWarmup = inWarmup || d.id == w.id;
		if (bigButton("warmup", inWarmup ? "" : "+", ImVec2(addW, btnH), false, Icon::QuickPlay, false, Fx::Click) && !inWarmup
			&& cb_.addToQuickPlay)
		{
			cb_.addToQuickPlay(w.id);
			toast("Added to your warm-up");
		}
		if (inWarmup)
		{
			// A tick (the font has no check mark).
			const ImVec2 mid((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
				(ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
			const float k = btnH * 0.18f;
			const ImVec2 pts[] = {ImVec2(mid.x - k, mid.y), ImVec2(mid.x - k * 0.3f, mid.y + k * 0.75f), ImVec2(mid.x + k * 1.1f, mid.y - k * 0.8f)};
			dl->AddPolyline(pts, 3, theme::u32(pal.accent), false, 2.5f * u);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(inWarmup ? "Already in your warm-up" : "Add to your warm-up");
		ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rowH));
		ImGui::Dummy(ImVec2(width, 0.f));
		ImGui::PopID();
		++index;
	}
	(void)m;
}

void Dashboard::milestoneRow(const ProgressView::Milestone& ms, float width)
{
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const ImU32 medal = medalColor(ms.tier);
	const float grow = easeOut(static_cast<float>((ImGui::GetTime() - tabAt_) / 0.8));
	const ImVec2 c(at.x + 18.f, at.y + 18.f);
	dl->AddCircleFilled(c, 15.f, theme::u32(pal.textDim, 0.14f), 24);
	if (ms.progress > 0.005f)
	{
		dl->PathArcTo(c, 15.f, -1.5707963f, -1.5707963f + 6.2831853f * ms.progress * grow, 24);
		dl->PathStroke(medal, false, 3.f);
	}
	dl->AddText(theme::fontAt(fs), fs, ImVec2(at.x + 44.f, at.y + 1.f), theme::u32(pal.text), fitText(ms.title, width - 160.f).c_str());
	dl->AddText(theme::fontAt(fs * 0.8f), fs * 0.8f, ImVec2(at.x + 44.f, at.y + 3.f + fs * 1.15f), theme::u32(pal.textDim), fitText(ms.about, width - 160.f).c_str());
	const ImVec2 rs = theme::fontAt(fs * 0.85f)->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0.f, ms.progressText.c_str());
	dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(at.x + width - rs.x, at.y + 8.f), theme::u32(pal.textDim), ms.progressText.c_str());
	ImGui::Dummy(ImVec2(width, 46.f));
}

void Dashboard::drawMinuteBars(const ProgressView& p, int count, float width, float height)
{
	// Minutes per day (oldest left, today right), the daily goal as a dashed line; hover a day for its numbers.
	const theme::Palette& pal = theme::palette();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const int n = std::min<int>(count, static_cast<int>(p.days.size()));
	const ImVec2 at = ImGui::GetCursorScreenPos();
	const double now = ImGui::GetTime();
	float mx = static_cast<float>(p.goalMinutes) * 1.25f;
	for (int i = 0; i < n; ++i)
		mx = std::max(mx, p.days[p.days.size() - n + i].minutes);
	const float top = 26.f; // room for the hover bubble
	const float chartH = height - fs * 1.4f - top;
	const float base = at.y + top + chartH;
	const float slot = width / std::max(1, n);
	const float bw = std::max(3.f, std::min(slot * 0.62f, 26.f));
	const float goalY = base - chartH * static_cast<float>(p.goalMinutes) / mx;
	for (float x = at.x; x < at.x + width; x += 10.f)
		dl->AddLine(ImVec2(x, goalY), ImVec2(std::min(x + 5.f, at.x + width), goalY), theme::u32(pal.accent, 0.3f), 1.f);
	int hovered = -1;
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	const bool inChart = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && mouse.y >= at.y && mouse.y <= at.y + height;
	for (int i = 0; i < n; ++i)
		if (inChart && mouse.x >= at.x + i * slot && mouse.x < at.x + (i + 1) * slot)
			hovered = i;
	for (int i = 0; i < n; ++i)
	{
		const auto& d = p.days[p.days.size() - n + i];
		const float grow = easeOut(static_cast<float>((now - tabAt_ - 0.015 * i) / 0.55));
		const float x0 = at.x + i * slot + (slot - bw) * 0.5f;
		const float h = std::max(d.minutes > 0.f ? 3.f : 1.5f, chartH * d.minutes / mx * grow);
		const bool goal = d.minutes >= p.goalMinutes;
		const bool hot = i == hovered;
		ImU32 col = d.minutes <= 0.f ? theme::u32(pal.textDim, 0.22f) : goal ? theme::u32(pal.accent)
			: theme::u32(theme::mix(pal.textDim, pal.accent, 0.45f));
		if (hovered >= 0 && !hot && d.minutes > 0.f)
			col = (col & 0x00FFFFFF) | 0x99000000; // the others dim a little
		const float r = std::min(4.f, bw * 0.4f);
		dl->AddRectFilled(ImVec2(x0, base - h), ImVec2(x0 + bw, base), col, r, ImDrawCornerFlags_Top);
		if (hot)
			dl->AddRect(ImVec2(x0 - 1.5f, base - h - 1.5f), ImVec2(x0 + bw + 1.5f, base + 0.5f), theme::u32(pal.text, 0.8f), r + 1.f,
				ImDrawCornerFlags_Top, 1.5f);
	}
	dl->AddLine(ImVec2(at.x, base + 0.5f), ImVec2(at.x + width, base + 0.5f), IM_COL32(255, 255, 255, 22), 1.f);
	{
		// The goal line's label, drawn over the bars on a small backing (it sat under today's bar).
		const float gs = fs * 0.7f;
		const ImVec2 gt = theme::fontAt(gs)->CalcTextSizeA(gs, FLT_MAX, 0.f, "goal");
		const ImVec2 ga(at.x + width - gt.x - 6.f, goalY - gt.y * 0.5f);
		dl->AddRectFilled(ImVec2(ga.x - 4.f, ga.y - 1.f), ImVec2(ga.x + gt.x + 4.f, ga.y + gt.y + 1.f), theme::u32(pal.card, 0.92f), 4.f);
		dl->AddText(theme::fontAt(gs), gs, ga, theme::u32(pal.accent, 0.8f), "goal");
	}
	auto label = [&](int i, const char* text, ImU32 col) {
		const ImVec2 ts = theme::fontAt(fs * 0.75f)->CalcTextSizeA(fs * 0.75f, FLT_MAX, 0.f, text);
		float x = at.x + i * slot + slot * 0.5f - ts.x * 0.5f;
		x = std::clamp(x, at.x, at.x + width - ts.x);
		dl->AddText(theme::fontAt(fs * 0.75f), fs * 0.75f, ImVec2(x, base + 5.f), col, text);
	};
	if (n > 0)
	{
		label(0, niceDay(p.days[p.days.size() - n].day).c_str(), theme::u32(pal.textDim));
		label(n - 1, "Today", theme::u32(pal.text, 0.85f));
	}
	if (hovered >= 0)
	{
		const auto& d = p.days[p.days.size() - n + hovered];
		const float h = std::max(1.5f, chartH * d.minutes / mx);
		char top[48], bottom[64];
		snprintf(top, sizeof(top), "%s", hoursText(d.minutes).c_str());
		snprintf(bottom, sizeof(bottom), "%s  |  %d reps", niceDay(d.day).c_str(), d.reps);
		const float tipX = std::clamp(at.x + hovered * slot + slot * 0.5f, at.x + 60.f, at.x + width - 60.f);
		bubble(ImGui::GetForegroundDrawList(), ImVec2(tipX, base - h - 2.f), top, bottom);
	}
	ImGui::Dummy(ImVec2(width, height));
}

void Dashboard::renderProgressRank(const ProgressView& p, bool compact)
{
	const theme::Palette& pal = theme::palette();
	const float width = ImGui::GetContentRegionAvail().x;
	static const char* kPlaylists[] = {"1v1 Duel", "2v2 Doubles", "3v3 Standard"};
	static const char* kRanges[] = {"30 days", "90 days", "All"};
	// Both pickers on one row when they fit (side by side, top-aligned), sized to their labels at any GUI scale.
	const float u = uiScale();
	const float playlistW = std::min(width, 420.f * u), rangeW = std::min(width, 260.f * u);
	const ImVec2 row = ImGui::GetCursorScreenPos();
	const int playlist = segmented("mmr_playlist", kPlaylists, 3, mmrPlaylist_, playlistW, mmrPlaylistAnim_);
	if (playlistW + 16.f * u + rangeW <= width)
		ImGui::SetCursorScreenPos(ImVec2(row.x + playlistW + 16.f * u, row.y));
	const int range = segmented("mmr_range", kRanges, 3, mmrRange_, rangeW, mmrRangeAnim_);
	if (playlist != mmrPlaylist_ || range != mmrRange_)
	{
		mmrPlaylist_ = playlist;
		mmrRange_ = range;
		chartAt_ = ImGui::GetTime(); // the line draws itself again
	}
	if (p.playlists.size() < 3)
		return;
	ImGui::Dummy(ImVec2(0, 4));
	const auto& pl = p.playlists[std::clamp(mmrPlaylist_, 0, 2)];
	CardScope card(width, 22.f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const ImVec2 head = ImGui::GetCursorScreenPos();
	const double now = ImGui::GetTime();
	const float chartIn = easeOut(static_cast<float>((now - std::max(tabAt_, chartAt_)) / 0.9));
	char buf[96];
	if (pl.mmr > 0.f)
	{
		snprintf(buf, sizeof(buf), "%.0f", pl.mmr * (0.85f + 0.15f * chartIn));
		dl->AddText(theme::fontAt(fs * 2.8f), fs * 2.8f, head, theme::u32(pal.text), buf);
		const float numW = theme::fontAt(fs * 2.8f)->CalcTextSizeA(fs * 2.8f, FLT_MAX, 0.f, "0000").x;
		if (pl.tier > 0)
		{
			dl->AddCircleFilled(ImVec2(head.x + numW + 22.f, head.y + fs * 0.95f), 8.f, rankColor(pl.tier), 20);
			dl->AddCircle(ImVec2(head.x + numW + 22.f, head.y + fs * 0.95f), 12.f, rankColor(pl.tier, 0.35f), 24, 1.5f);
			dl->AddText(theme::fontAt(fs * 1.15f), fs * 1.15f, ImVec2(head.x + numW + 42.f, head.y + fs * 0.35f), rankColor(pl.tier), pl.rank.c_str());
		}
		if (pl.hasDelta)
			trendText(dl, ImVec2(head.x + numW + 42.f, head.y + fs * 1.7f), pl.delta, " this week", fs * 0.95f);
		snprintf(buf, sizeof(buf), "Peak %.0f", pl.peak);
		const ImVec2 ts = theme::fontAt(fs * 0.9f)->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.f, buf);
		dl->AddText(theme::fontAt(fs * 0.9f), fs * 0.9f, ImVec2(head.x + card.inner() - ts.x, head.y + 6.f), theme::u32(pal.textDim), buf);
		const char* src = pl.synced ? "From the game, today" : "Saved earlier";
		const ImVec2 ss = theme::fontAt(fs * 0.78f)->CalcTextSizeA(fs * 0.78f, FLT_MAX, 0.f, src);
		dl->AddText(theme::fontAt(fs * 0.78f), fs * 0.78f, ImVec2(head.x + card.inner() - ss.x, head.y + 8.f + ts.y), theme::u32(pal.textDim, 0.75f), src);
	}
	else
	{
		dl->AddText(theme::fontAt(fs * 1.25f), fs * 1.25f, ImVec2(head.x, head.y + 8.f), theme::u32(pal.textDim), "No MMR for this playlist yet");
	}
	ImGui::Dummy(ImVec2(card.inner(), fs * 3.6f));

	// The chart: MMR over the range, a soft fill under the line, approximate rank-group lines behind it, a crosshair.
	const int span = mmrRange_ == 0 ? 30 : mmrRange_ == 1 ? 90 : 0;
	std::vector<std::pair<int, float>> pts;
	for (const auto& sm : pl.samples)
		if (span == 0 || sm.first >= -span)
			pts.push_back(sm);
	const float chartH = compact ? 160.f : 260.f;
	const ImVec2 a = ImGui::GetCursorScreenPos();
	const ImVec2 b(a.x + card.inner(), a.y + chartH);
	dl->AddRectFilled(a, b, IM_COL32(5, 8, 14, 110), 12.f);
	if (pts.empty())
	{
		const char* msg = "Your MMR shows up here once it is saved: play a ranked match, or type it in below.";
		const ImVec2 ts = theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, 0.f, msg);
		dl->AddText(theme::fontAt(fs), fs, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), theme::u32(pal.textDim), msg);
	}
	else
	{
		float lo = pts.front().second, hi = lo;
		for (const auto& q : pts)
		{
			lo = std::min(lo, q.second);
			hi = std::max(hi, q.second);
		}
		const float padV = std::max(30.f, (hi - lo) * 0.18f);
		lo -= padV;
		hi += padV;
		const int firstDay = span ? -span : std::min(-7, pts.front().first);
		const float left = a.x + 52.f, right = b.x - 20.f, topY = a.y + 22.f, bottomY = b.y - 26.f;
		auto X = [&](float day) { return left + (right - left) * (day - firstDay) / std::max(1.f, static_cast<float>(-firstDay)); };
		auto Y = [&](float v) { return bottomY - (bottomY - topY) * (v - lo) / std::max(1.f, hi - lo); };
		// Horizontal guides with values.
		for (int k = 0; k <= 3; ++k)
		{
			const float v = lo + (hi - lo) * k / 3.f;
			dl->AddLine(ImVec2(left, Y(v)), ImVec2(right, Y(v)), IM_COL32(255, 255, 255, 10), 1.f);
			snprintf(buf, sizeof(buf), "%.0f", v);
			const ImVec2 ts = theme::fontAt(fs * 0.72f)->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0.f, buf);
			dl->AddText(theme::fontAt(fs * 0.72f), fs * 0.72f, ImVec2(left - 10.f - ts.x, Y(v) - ts.y * 0.5f), theme::u32(pal.textDim, 0.8f), buf);
		}
		// Rank group floors inside the range.
		const bool duel = pl.key == "1v1";
		for (int g = 1; g < 8; ++g)
		{
			const float f = rankGroupFloor(g, duel);
			if (f <= lo || f >= hi)
				continue;
			const float y = Y(f);
			const int tier = std::min(22, 1 + g * 3);
			for (float x = left; x < right; x += 9.f)
				dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + 4.f, right), y), rankColor(tier, 0.4f), 1.f);
			dl->AddText(theme::fontAt(fs * 0.72f), fs * 0.72f, ImVec2(left + 6.f, y - fs * 0.85f), rankColor(tier, 0.75f), kRankGroups[g]);
		}
		// The line draws itself from the left (smoothly, part of a segment at a time), with a fill under it.
		std::vector<ImVec2> line;
		for (const auto& q : pts)
			line.push_back(ImVec2(X(static_cast<float>(q.first)), Y(q.second)));
		const float progress = chartIn * (line.size() > 1 ? line.size() - 1 : 1);
		std::vector<ImVec2> drawn;
		for (size_t i = 0; i < line.size(); ++i)
		{
			if (i <= progress)
				drawn.push_back(line[i]);
			else
			{
				const float u = progress - std::floor(progress);
				if (i > 0 && i == static_cast<size_t>(std::floor(progress)) + 1)
					drawn.push_back(ImVec2(line[i - 1].x + (line[i].x - line[i - 1].x) * u, line[i - 1].y + (line[i].y - line[i - 1].y) * u));
				break;
			}
		}
		for (size_t i = 1; i < drawn.size(); ++i)
		{
			const ImVec2 q[4] = {drawn[i - 1], drawn[i], ImVec2(drawn[i].x, bottomY), ImVec2(drawn[i - 1].x, bottomY)};
			dl->AddQuadFilled(q[0], q[1], q[2], q[3], theme::u32(pal.accent, 0.10f));
		}
		if (drawn.size() >= 2)
			dl->AddPolyline(drawn.data(), static_cast<int>(drawn.size()), theme::u32(pal.accent), false, 3.f);
		for (size_t i = 0; i < drawn.size() && i < line.size(); ++i)
		{
			if (drawn[i].x != line[i].x)
				break;
			const bool last = i + 1 == line.size();
			if (last)
			{
				const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 3.f);
				dl->AddCircle(line[i], 9.f + 3.f * pulse, theme::u32(pal.accent, 0.35f * (1.f - pulse)), 20, 2.f);
			}
			dl->AddCircleFilled(line[i], last ? 5.5f : 3.f, theme::u32(last ? pal.text : pal.accent), 16);
		}
		// Crosshair: the nearest day under the mouse.
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && mouse.x >= left - 10.f && mouse.x <= right + 10.f
			&& mouse.y >= a.y && mouse.y <= b.y && chartIn > 0.95f)
		{
			size_t best = 0;
			for (size_t i = 1; i < line.size(); ++i)
				if (std::fabs(line[i].x - mouse.x) < std::fabs(line[best].x - mouse.x))
					best = i;
			const ImVec2 q = line[best];
			for (float y = topY; y < bottomY; y += 8.f)
				dl->AddLine(ImVec2(q.x, y), ImVec2(q.x, std::min(y + 4.f, bottomY)), theme::u32(pal.text, 0.25f), 1.f);
			dl->AddCircle(q, 8.f, theme::u32(pal.text, 0.8f), 16, 2.f);
			const int ago = -pts[best].first;
			char top[32], bottom[48];
			snprintf(top, sizeof(top), "%.0f MMR", pts[best].second);
			if (ago == 0)
				snprintf(bottom, sizeof(bottom), "today");
			else if (ago == 1)
				snprintf(bottom, sizeof(bottom), "yesterday");
			else
				snprintf(bottom, sizeof(bottom), "%d days ago", ago);
			bubble(ImGui::GetForegroundDrawList(), ImVec2(std::clamp(q.x, left + 50.f, right - 50.f), q.y - 6.f), top, bottom);
		}
		snprintf(buf, sizeof(buf), "%d days ago", -firstDay);
		dl->AddText(theme::fontAt(fs * 0.72f), fs * 0.72f, ImVec2(left, bottomY + 6.f), theme::u32(pal.textDim), buf);
		dl->AddText(theme::fontAt(fs * 0.72f), fs * 0.72f, ImVec2(right - theme::fontAt(fs * 0.72f)->CalcTextSizeA(fs * 0.72f, FLT_MAX, 0.f, "today").x, bottomY + 6.f),
			theme::u32(pal.textDim), "today");
	}
	ImGui::Dummy(ImVec2(card.inner(), chartH + 10.f));
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card.inner());
	ImGui::TextDisabled("Saved once a day from the game's own numbers when you are signed in (Free Touch makes no internet calls). "
		"Rank lines are approximate; they move a little every season.");
	ImGui::PopTextWrapPos();
	if (!compact)
	{
		// Typing it in (when the game has not got it, or to fill in a day).
		ImGui::Dummy(ImVec2(0, 6));
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Add today's MMR");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110.f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.f);
		ImGui::InputTextWithHint("##mmr_in", "e.g. 1150", mmrInput_, sizeof(mmrInput_), ImGuiInputTextFlags_CharsDecimal);
		ImGui::PopStyleVar();
		ImGui::SameLine();
		const float typed = static_cast<float>(std::atof(mmrInput_));
		const bool ok = typed > 0.f && typed < 4000.f;
		if (navButton("Save") && ok && cb_.addMmr)
		{
			cb_.addMmr(pl.key, typed);
			toast("MMR saved");
			mmrInput_[0] = 0;
			chartAt_ = ImGui::GetTime();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("for %s", kPlaylists[std::clamp(mmrPlaylist_, 0, 2)]);
	}
}

void Dashboard::renderProgressTraining(const DashboardModel& m, const ProgressView& p)
{
	const theme::Palette& pal = theme::palette();
	const float width = ImGui::GetContentRegionAvail().x;
	const double now = ImGui::GetTime();
	{
		CardScope card(width, 20.f, &hoverAnim_["progress.bars28"]);
		double sum = 0.0;
		for (size_t i = p.days.size() >= 28 ? p.days.size() - 28 : 0; i < p.days.size(); ++i)
			sum += p.days[i].minutes;
		const std::string right = hoursText(sum) + " in 4 weeks";
		heading("MINUTES PER DAY", card.inner(), right.c_str());
		drawMinuteBars(p, 28, card.inner(), 168.f);
	}
	ImGui::Dummy(ImVec2(0, 14));
	{
		// Streak calendar: 12 weeks, a column per week, brighter for more minutes; goal days ringed.
		CardScope card(width, 20.f, &hoverAnim_["progress.calendar"]);
		heading("STREAK CALENDAR", card.inner(), "12 weeks");
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const int weeks = 12;
		const float cell = std::clamp((card.inner() - 150.f) / weeks - 5.f, 10.f, 24.f);
		const int n = std::min<int>(weeks * 7, static_cast<int>(p.days.size()));
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		int hover = -1;
		for (int i = 0; i < n; ++i)
		{
			const int col = i / 7, row = i % 7;
			const auto& d = p.days[p.days.size() - n + i];
			const float pop = easeOut(static_cast<float>((now - tabAt_ - 0.02 * col) / 0.35));
			const ImVec2 c0(at.x + col * (cell + 5.f), at.y + row * (cell + 5.f));
			const float shrink = (1.f - pop) * cell * 0.5f;
			const ImVec2 q0(c0.x + shrink, c0.y + shrink), q1(c0.x + cell - shrink, c0.y + cell - shrink);
			const float t = d.minutes <= 0.f ? 0.f : std::clamp(0.28f + 0.72f * d.minutes / static_cast<float>(p.goalMinutes * 1.5), 0.28f, 1.f);
			dl->AddRectFilled(q0, q1, t > 0.f ? theme::u32(pal.accent, t) : theme::u32(pal.textDim, 0.12f), 4.f);
			if (d.minutes >= p.goalMinutes && pop > 0.9f)
				dl->AddRect(q0, q1, theme::u32(pal.text, 0.55f), 4.f, ImDrawCornerFlags_All, 1.4f);
			if (i == n - 1)
				dl->AddRect(ImVec2(c0.x - 2.5f, c0.y - 2.5f), ImVec2(c0.x + cell + 2.5f, c0.y + cell + 2.5f), theme::u32(pal.orange), 5.f,
					ImDrawCornerFlags_All, 1.5f);
			if (mouse.x >= c0.x && mouse.x < c0.x + cell && mouse.y >= c0.y && mouse.y < c0.y + cell)
				hover = i;
		}
		const float gridW = weeks * (cell + 5.f);
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize();
		const float lx = at.x + gridW + 18.f;
		// Legend: "Less [] [] [] [] More" on one row, then the goal and today marks, each a swatch beside its word.
		const float ls = fs * 0.8f;
		ImFont* lf = theme::fontAt(ls);
		const float sq = std::round(ls * 0.75f);
		const float lineH = ls * 1.6f;
		const auto swatchY = [&](float y) { return y + (ls - sq) * 0.5f + 1.f; };
		float x = lx;
		dl->AddText(lf, ls, ImVec2(x, at.y), theme::u32(pal.textDim), "Less");
		x += lf->CalcTextSizeA(ls, FLT_MAX, 0.f, "Less").x + 8.f;
		for (int k = 0; k < 4; ++k)
		{
			dl->AddRectFilled(ImVec2(x, swatchY(at.y)), ImVec2(x + sq, swatchY(at.y) + sq),
				k == 0 ? theme::u32(pal.textDim, 0.12f) : theme::u32(pal.accent, 0.28f + 0.24f * k), 3.f);
			x += sq + 4.f;
		}
		dl->AddText(lf, ls, ImVec2(x + 4.f, at.y), theme::u32(pal.textDim), "More");
		const float y1 = at.y + lineH, y2 = at.y + lineH * 2.f;
		dl->AddRect(ImVec2(lx, swatchY(y1)), ImVec2(lx + sq, swatchY(y1) + sq), theme::u32(pal.text, 0.55f), 3.f, ImDrawCornerFlags_All, 1.4f);
		dl->AddText(lf, ls, ImVec2(lx + sq + 8.f, y1), theme::u32(pal.textDim), "goal done");
		dl->AddRect(ImVec2(lx, swatchY(y2)), ImVec2(lx + sq, swatchY(y2) + sq), theme::u32(pal.orange), 3.f, ImDrawCornerFlags_All, 1.5f);
		dl->AddText(lf, ls, ImVec2(lx + sq + 8.f, y2), theme::u32(pal.textDim), "today");
		if (hover >= 0)
		{
			const auto& d = p.days[p.days.size() - n + hover];
			const int col = hover / 7, row = hover % 7;
			char bottom[64];
			snprintf(bottom, sizeof(bottom), "%s  |  %d reps", niceDay(d.day).c_str(), d.reps);
			bubble(ImGui::GetForegroundDrawList(), ImVec2(at.x + col * (cell + 5.f) + cell * 0.5f, at.y + row * (cell + 5.f) - 2.f),
				hoursText(d.minutes), bottom);
		}
		ImGui::Dummy(ImVec2(card.inner(), 7 * (cell + 5.f)));
	}
	ImGui::Dummy(ImVec2(0, 14));

	// Categories: this week against last week.
	heading("BY CATEGORY", width, "this week vs last week");
	{
		const float gap = 12.f;
		const int perRow = width >= 900.f ? 3 : width >= 560.f ? 2 : 1;
		const float cw = (width - gap * (perRow - 1)) / perRow;
		const ImVec2 row = ImGui::GetCursorScreenPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize();
		const float chH = std::max(96.f, 44.f + fs * 3.5f); // the three lines and the bar at any GUI scale
		int i = 0;
		for (const auto& c : p.categories)
		{
			const float rise = easeOut(static_cast<float>((now - tabAt_ - 0.04 * i) / 0.4));
			const ImVec2 slot(row.x + (i % perRow) * (cw + gap), row.y + (i / perRow) * (chH + gap));
			float& hv = hoverAnim_["progress.cat." + c.id];
			hv = approach(hv, ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + cw, slot.y + chH)) ? 1.f : 0.f, 9.f);
			const ImVec2 a(slot.x, slot.y + 10.f * (1.f - rise) - 2.f * hv), b(a.x + cw, a.y + chH);
			cellCard(dl, a, b, hv * 0.6f);
			dl->AddText(theme::fontAt(fs), fs, ImVec2(a.x + 16.f, a.y + 13.f), theme::u32(pal.text), fitText(categoryLabel(c.id), cw - 120.f).c_str());
			char buf[64];
			const float now7 = c.reps7 > 0 ? 100.f * c.wins7 / c.reps7 : 0.f;
			const float prev7 = c.repsPrev7 > 0 ? 100.f * c.winsPrev7 / c.repsPrev7 : 0.f;
			// A thin success bar for this week.
			const float barX0 = a.x + 16.f, barX1 = b.x - 16.f, barY = a.y + 18.f + fs * 2.45f;
			dl->AddRectFilled(ImVec2(barX0, barY), ImVec2(barX1, barY + 5.f), theme::u32(pal.textDim, 0.16f), 3.f);
			if (c.reps7 > 0)
				dl->AddRectFilled(ImVec2(barX0, barY), ImVec2(barX0 + (barX1 - barX0) * now7 / 100.f * rise, barY + 5.f), theme::u32(pal.accent), 3.f);
			snprintf(buf, sizeof(buf), c.reps7 > 0 ? "%d reps  |  %.0f%%" : "Not played this week", c.reps7, now7);
			dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(a.x + 16.f, a.y + 12.f + fs * 1.3f), theme::u32(c.reps7 > 0 ? pal.text : pal.textDim, 0.9f), buf);
			snprintf(buf, sizeof(buf), "All time %d reps, %.0f%%", c.reps, c.reps > 0 ? 100.f * c.wins / c.reps : 0.f);
			dl->AddText(theme::fontAt(fs * 0.76f), fs * 0.76f, ImVec2(a.x + 16.f, barY + 12.f), theme::u32(pal.textDim), buf);
			if (c.reps7 > 0 && c.repsPrev7 > 0)
			{
				// Right-aligned inside the card (the arrow plus "+12 pts").
				const float ts = fs * 0.85f;
				const float trendW = theme::fontAt(ts)->CalcTextSizeA(ts, FLT_MAX, 0.f, "+00 pts").x + ts * 1.1f;
				trendText(dl, ImVec2(b.x - 16.f - trendW, a.y + 14.f), now7 - prev7, " pts", ts);
			}
			++i;
		}
		const int rows = (i + perRow - 1) / perRow;
		ImGui::Dummy(ImVec2(width, rows * (chH + gap)));
		if (p.categories.empty())
			ImGui::TextDisabled("Play some drills and your categories show up here.");
	}
	ImGui::Dummy(ImVec2(0, 8));

	// Records and lifetime mechanics side by side (stacked when narrow).
	const bool two = width >= 860.f;
	const float colW = two ? (width - 16.f) * 0.5f : width;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	ImGui::BeginGroup();
	{
		CardScope card(colW, 20.f, &hoverAnim_["progress.records"]);
		heading("RECORDS", card.inner());
		if (p.records.empty())
			ImGui::TextDisabled("Timed drills, HORSE and Style Rush keep your bests here.");
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize();
		int i = 0;
		for (const auto& r : p.records)
		{
			const ImVec2 at = ImGui::GetCursorScreenPos();
			if (i > 0)
				dl->AddLine(ImVec2(at.x, at.y - 4.f), ImVec2(at.x + card.inner(), at.y - 4.f), IM_COL32(255, 255, 255, 12), 1.f);
			if (r.highScore)
			{
				// A small star for the high scores.
				const ImVec2 c(at.x + 8.f, at.y + fs * 0.6f);
				for (int k = 0; k < 5; ++k)
				{
					const float a0 = -1.5708f + k * 1.2566f, a1 = a0 + 0.6283f;
					dl->AddTriangleFilled(c, ImVec2(c.x + std::cos(a0) * 8.f, c.y + std::sin(a0) * 8.f),
						ImVec2(c.x + std::cos(a1) * 3.5f, c.y + std::sin(a1) * 3.5f), kGold);
					dl->AddTriangleFilled(c, ImVec2(c.x + std::cos(a1) * 3.5f, c.y + std::sin(a1) * 3.5f),
						ImVec2(c.x + std::cos(a0 + 1.2566f) * 8.f, c.y + std::sin(a0 + 1.2566f) * 8.f), kGold);
				}
			}
			const float tx = at.x + (r.highScore ? 24.f : 0.f);
			dl->AddText(theme::fontAt(fs), fs, ImVec2(tx, at.y), theme::u32(pal.text), fitText(r.title, card.inner() - 130.f).c_str());
			dl->AddText(theme::fontAt(fs * 0.78f), fs * 0.78f, ImVec2(tx, at.y + fs * 1.15f), theme::u32(pal.textDim), r.sub.c_str());
			const ImVec2 vs = theme::fontAt(fs * 1.2f)->CalcTextSizeA(fs * 1.2f, FLT_MAX, 0.f, r.value.c_str());
			dl->AddText(theme::fontAt(fs * 1.2f), fs * 1.2f, ImVec2(at.x + card.inner() - vs.x, at.y + 2.f), r.highScore ? kGold : theme::u32(pal.accent), r.value.c_str());
			ImGui::Dummy(ImVec2(card.inner(), fs * 2.7f));
			++i;
		}
	}
	ImGui::EndGroup();
	const float leftBottom = ImGui::GetItemRectMax().y;
	ImGui::SetCursorScreenPos(two ? ImVec2(top.x + colW + 16.f, top.y) : ImVec2(top.x, leftBottom + 14.f));
	ImGui::BeginGroup();
	{
		CardScope card(colW, 20.f, &hoverAnim_["progress.mechanics"]);
		heading("MECHANICS LANDED", card.inner(), "all time");
		if (p.mechanics.empty())
			ImGui::TextDisabled("Air dribbles, flip resets, double taps and more are counted as you land them.");
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize();
		ImVec2 at = ImGui::GetCursorScreenPos();
		const float x0 = at.x;
		int k = 0;
		for (const auto& mc : p.mechanics)
		{
			char num[16];
			snprintf(num, sizeof(num), "%d", mc.count);
			const ImVec2 ls = theme::fontAt(fs * 0.9f)->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.f, mc.label.c_str());
			const ImVec2 ns = theme::fontAt(fs * 0.9f)->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.f, num);
			const float w = ls.x + ns.x + 34.f;
			if (at.x + w > x0 + card.inner())
			{
				at.x = x0;
				at.y += fs * 2.0f;
			}
			const float rise = easeOut(static_cast<float>((now - tabAt_ - 0.03 * k) / 0.35));
			const ImU32 fill = theme::u32(pal.accent, 0.13f * rise);
			dl->AddRectFilled(at, ImVec2(at.x + w, at.y + fs * 1.6f), fill, 20.f);
			dl->AddText(theme::fontAt(fs * 0.9f), fs * 0.9f, ImVec2(at.x + 12.f, at.y + fs * 0.3f), theme::u32(pal.text, rise), mc.label.c_str());
			dl->AddText(theme::fontAt(fs * 0.9f), fs * 0.9f, ImVec2(at.x + 22.f + ls.x, at.y + fs * 0.3f), theme::u32(pal.accent, rise), num);
			at.x += w + 8.f;
			++k;
		}
		ImGui::SetCursorScreenPos(ImVec2(x0, at.y + (p.mechanics.empty() ? 0.f : fs * 2.1f)));
		ImGui::TextDisabled("The detectors are still being tuned, so counts are approximate.");
	}
	ImGui::EndGroup();
	const float rightBottom = ImGui::GetItemRectMax().y;
	ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(leftBottom, rightBottom) + 8.f));
	ImGui::Dummy(ImVec2(width, 0.f));
	(void)m;
}

void Dashboard::renderProgressMilestones(const ProgressView& p, bool)
{
	const theme::Palette& pal = theme::palette();
	const float width = ImGui::GetContentRegionAvail().x;
	const double now = ImGui::GetTime();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	int done = 0, gold = 0, silver = 0, bronze = 0;
	for (const auto& ms : p.milestones)
		if (ms.done)
		{
			++done;
			(ms.tier >= 3 ? gold : ms.tier == 2 ? silver : bronze) += 1;
		}
	{
		// The tally: a bar through them all and a count of each medal.
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 at = ImGui::GetCursorScreenPos();
		const float frac = p.milestones.empty() ? 0.f : static_cast<float>(done) / p.milestones.size();
		const float grow = easeOut(static_cast<float>((now - tabAt_) / 0.9));
		char buf[64];
		snprintf(buf, sizeof(buf), "%d of %d", done, static_cast<int>(p.milestones.size()));
		dl->AddText(theme::fontAt(fs * 1.6f), fs * 1.6f, at, theme::u32(pal.text), buf);
		float x = at.x + theme::fontAt(fs * 1.6f)->CalcTextSizeA(fs * 1.6f, FLT_MAX, 0.f, buf).x + 24.f;
		const std::pair<int, int> medals[] = {{3, gold}, {2, silver}, {1, bronze}};
		for (const auto& [tier, n] : medals)
		{
			dl->AddCircleFilled(ImVec2(x + 8.f, at.y + fs * 0.85f), 8.f, medalColor(tier), 20);
			snprintf(buf, sizeof(buf), "%d", n);
			dl->AddText(theme::fontAt(fs), fs, ImVec2(x + 22.f, at.y + fs * 0.3f), theme::u32(pal.text, 0.9f), buf);
			x += 52.f;
		}
		const float barY = at.y + fs * 2.2f;
		dl->AddRectFilled(ImVec2(at.x, barY), ImVec2(at.x + width, barY + 8.f), theme::u32(pal.textDim, 0.16f), 4.f);
		roundedGradient(dl, ImVec2(at.x, barY), ImVec2(at.x + std::max(8.f, width * frac * grow), barY + 8.f),
			IM_COL32(222, 146, 90, 255), kGold, kGold, IM_COL32(222, 146, 90, 255), 4.f);
		ImGui::Dummy(ImVec2(width, fs * 2.2f + 22.f));
	}
	const float gap = 12.f;
	const int perRow = width >= 900.f ? 3 : width >= 560.f ? 2 : 1;
	const float cw = (width - gap * (perRow - 1)) / perRow;
	const float chH = 116.f;
	const ImVec2 row = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// Done first (newest first), then the closest to done.
	std::vector<const ProgressView::Milestone*> order;
	for (const auto& ms : p.milestones)
		order.push_back(&ms);
	std::stable_sort(order.begin(), order.end(), [](auto* a, auto* b) {
		if (a->done != b->done)
			return a->done;
		if (a->done)
			return a->unlockedOn > b->unlockedOn;
		return a->progress > b->progress;
	});
	const std::string newest = order.empty() || !order.front()->done ? "" : order.front()->unlockedOn;
	int i = 0;
	for (const auto* ms : order)
	{
		const float rise = easeOut(static_cast<float>((now - tabAt_ - 0.025 * i) / 0.4));
		const ImVec2 slot(row.x + (i % perRow) * (cw + gap), row.y + (i / perRow) * (chH + gap));
		float& hv = hoverAnim_["progress.ms." + ms->id];
		hv = approach(hv, ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + cw, slot.y + chH)) ? 1.f : 0.f, 9.f);
		const ImVec2 a(slot.x, slot.y + 10.f * (1.f - rise) - 3.f * hv);
		const ImVec2 b(a.x + cw, a.y + chH);
		const ImU32 medal = medalColor(ms->tier);
		const ImVec4 medalV = ImGui::ColorConvertU32ToFloat4(medal);
		dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, ms->done ? medalV : pal.accent, ms->done ? 0.07f + 0.04f * hv : 0.03f * hv)), 12.f);
		dl->AddRect(a, b, ms->done ? ((medal & 0x00FFFFFF) | (static_cast<ImU32>(90 + 80 * hv) << 24))
			: theme::u32(theme::mix(ImVec4(1, 1, 1, 0.055f), pal.accent, 0.4f * hv)), 12.f, ImDrawCornerFlags_All, ms->done ? 1.5f : 1.f);
		// The medal.
		const ImVec2 c(a.x + 42.f, a.y + chH * 0.5f);
		if (ms->done)
		{
			const float glow = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 1.6f + i);
			dl->AddCircleFilled(c, 31.f, (medal & 0x00FFFFFF) | (static_cast<ImU32>(18 + 22 * glow) << 24), 32);
			dl->AddCircleFilled(c, 25.f, medal, 32);
			dl->AddCircle(c, 25.f, IM_COL32(255, 255, 255, 110), 32, 1.5f);
			dl->AddCircle(c, 19.f, IM_COL32(0, 0, 0, 40), 32, 1.5f);
			// A light sweep across the medal every few seconds.
			const float sweep = std::fmod(static_cast<float>(now) * 0.35f + i * 0.13f, 1.f) * 4.f - 1.f;
			if (sweep > -1.f && sweep < 1.f)
			{
				const float sx = c.x + sweep * 25.f;
				const float half = std::sqrt(std::max(0.f, 625.f - (sx - c.x) * (sx - c.x)));
				dl->AddLine(ImVec2(sx - 4.f, c.y + half - 2.f), ImVec2(sx + 4.f, c.y - half + 2.f), IM_COL32(255, 255, 255, 120), 4.f);
			}
			dl->AddLine(ImVec2(c.x - 10.f, c.y), ImVec2(c.x - 2.5f, c.y + 8.f), IM_COL32(25, 25, 35, 255), 3.5f);
			dl->AddLine(ImVec2(c.x - 2.5f, c.y + 8.f), ImVec2(c.x + 11.f, c.y - 8.f), IM_COL32(25, 25, 35, 255), 3.5f);
		}
		else
		{
			dl->AddCircleFilled(c, 25.f, theme::u32(pal.textDim, 0.1f), 32);
			dl->AddCircle(c, 25.f, theme::u32(pal.textDim, 0.25f), 32, 1.5f);
			if (ms->progress > 0.005f)
			{
				dl->PathArcTo(c, 25.f, -1.5707963f, -1.5707963f + 6.2831853f * ms->progress * rise, 40);
				dl->PathStroke(medal, false, 4.f);
			}
			char pct[8];
			snprintf(pct, sizeof(pct), "%d%%", static_cast<int>(ms->progress * 100.f * rise));
			const ImVec2 ts = theme::fontAt(fs * 0.8f)->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0.f, pct);
			dl->AddText(theme::fontAt(fs * 0.8f), fs * 0.8f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), theme::u32(pal.textDim), pct);
		}
		const float tx = a.x + 84.f, tw = cw - 98.f;
		dl->AddText(theme::fontAt(fs * 1.05f), fs * 1.05f, ImVec2(tx, a.y + 18.f), theme::u32(pal.text, ms->done ? 1.f : 0.85f), fitText(ms->title, tw - 44.f).c_str());
		dl->AddText(theme::fontAt(fs * 0.84f), fs * 0.84f, ImVec2(tx, a.y + 20.f + fs * 1.35f), theme::u32(pal.textDim), fitText(ms->about, tw).c_str());
		const std::string foot = ms->done ? "Unlocked " + niceDay(ms->unlockedOn) : ms->progressText;
		dl->AddText(theme::fontAt(fs * 0.84f), fs * 0.84f, ImVec2(tx, b.y - fs * 1.7f), ms->done ? medal : theme::u32(pal.accent, 0.9f), fitText(foot, tw).c_str());
		if (ms->done && !newest.empty() && ms->unlockedOn == newest)
		{
			// The newest one(s) get a tag.
			const char* tag = "NEW";
			const ImVec2 ts = theme::fontAt(fs * 0.7f)->CalcTextSizeA(fs * 0.7f, FLT_MAX, 0.f, tag);
			const ImVec2 t0(b.x - ts.x - 22.f, a.y + 14.f);
			dl->AddRectFilled(t0, ImVec2(t0.x + ts.x + 12.f, t0.y + ts.y + 6.f), kGold, 10.f);
			dl->AddText(theme::fontAt(fs * 0.7f), fs * 0.7f, ImVec2(t0.x + 6.f, t0.y + 3.f), IM_COL32(25, 25, 35, 255), tag);
		}
		++i;
	}
	const int rows = (i + perRow - 1) / perRow;
	ImGui::Dummy(ImVec2(width, rows * (chH + gap)));
}

void Dashboard::renderProgressJournal(const ProgressView& p)
{
	const theme::Palette& pal = theme::palette();
	const float width = ImGui::GetContentRegionAvail().x;
	const double now = ImGui::GetTime();
	if (p.journal.empty())
	{
		ImGui::TextDisabled("Your training days show up here: minutes, reps, what you ticked off in your routine, its notes, "
			"and your MMR that day.");
		return;
	}
	int index = 0;
	std::string lastMonth;
	for (const auto& d : p.journal)
	{
		// A small month divider when the month changes.
		const std::string month = d.day.size() >= 7 ? d.day.substr(0, 7) : "";
		if (month != lastMonth)
		{
			lastMonth = month;
			static const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September",
				"October", "November", "December"};
			int y = 0, mo = 0;
			std::sscanf(month.c_str(), "%d-%d", &y, &mo);
			char title[48];
			snprintf(title, sizeof(title), "%s %d", kMonths[std::clamp(mo, 1, 12) - 1], y);
			if (index > 0)
				ImGui::Dummy(ImVec2(0, 6));
			heading(title, width);
		}
		ImGui::PushID(d.day.c_str());
		const bool open = journalOpen_ == d.day;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float rise = easeOut(static_cast<float>((now - tabAt_ - 0.03 * std::min(index, 12)) / 0.35));
		const ImVec2 slot = ImGui::GetCursorScreenPos();
		const ImVec2 at(slot.x, slot.y + 8.f * (1.f - rise));
		ImFont* font = ImGui::GetFont();
		const float fs = ImGui::GetFontSize();
		const bool hasMore = !d.notes.empty() || !d.focus.empty() || !d.steps.empty() || !d.trained.empty();
		const float rowH = std::max(58.f, 18.f + fs * 2.45f); // both lines fit at any GUI scale
		if (navInvisible("day", ImVec2(width, rowH), 1, hasMore ? Fx::Toggle : Fx::Silent) && hasMore)
		{
			journalOpen_ = open ? "" : d.day;
			journalOpenAt_ = now;
		}
		float& hv = hoverAnim_["progress.day." + d.day];
		hv = approach(hv, ImGui::IsItemHovered() ? 1.f : 0.f, 10.f);
		dl->AddRectFilled(at, ImVec2(at.x + width, at.y + rowH), theme::u32(theme::mix(pal.card, pal.cardHover, hv)), 10.f);
		if (open)
			dl->AddRect(at, ImVec2(at.x + width, at.y + rowH), theme::u32(pal.accent, 0.4f), 10.f);
		// The day's training as a short bar at the left edge (longer for more minutes, accent when the goal was done).
		const float frac = std::clamp(d.minutes / static_cast<float>(std::max(1.0, p.goalMinutes)), 0.f, 1.f);
		dl->AddRectFilled(ImVec2(at.x + 6.f, at.y + rowH - 8.f - (rowH - 16.f) * frac), ImVec2(at.x + 9.f, at.y + rowH - 8.f),
			frac >= 1.f ? theme::u32(pal.accent) : theme::u32(pal.textDim, 0.6f), 2.f);
		dl->AddText(theme::fontAt(fs * 1.05f), fs * 1.05f, ImVec2(at.x + 20.f, at.y + 10.f), theme::u32(pal.text), niceDay(d.day).c_str());
		char buf[96];
		snprintf(buf, sizeof(buf), "%s   |   %d reps   |   %d%% success", hoursText(d.minutes).c_str(), d.reps,
			d.reps > 0 ? 100 * d.wins / d.reps : 0);
		dl->AddText(theme::fontAt(fs * 0.82f), fs * 0.82f, ImVec2(at.x + 20.f, at.y + 12.f + fs * 1.2f), theme::u32(pal.textDim), buf);
		float x = at.x + std::max(width * 0.45f, 300.f);
		if (d.ticked > 0)
		{
			snprintf(buf, sizeof(buf), "%d routine step%s", d.ticked, d.ticked == 1 ? "" : "s");
			pill(dl, ImVec2(x, at.y + (rowH - fs) * 0.5f - 2.f), buf, pal.accent);
			x += ImGui::CalcTextSize(buf).x + 26.f;
		}
		if (!d.mmr.empty())
			pill(dl, ImVec2(x, at.y + (rowH - fs) * 0.5f - 2.f), d.mmr, ImVec4(0.7f, 0.45f, 1.f, 1.f));
		if (hasMore)
		{
			// A chevron that turns as it opens.
			const float turn = open ? easeOut(static_cast<float>((now - journalOpenAt_) / 0.2)) : 1.f - easeOut(static_cast<float>((now - journalOpenAt_) / 0.2));
			const ImVec2 c(at.x + width - 24.f, at.y + rowH * 0.5f);
			const float k = 1.f - 2.f * std::clamp(turn, 0.f, 1.f); // turn is already how open it is: v closed, ^ open
			dl->AddLine(ImVec2(c.x - 6.f, c.y - 3.f * k), ImVec2(c.x, c.y + 3.f * k), theme::u32(pal.textDim), 2.f);
			dl->AddLine(ImVec2(c.x, c.y + 3.f * k), ImVec2(c.x + 6.f, c.y - 3.f * k), theme::u32(pal.textDim), 2.f);
		}
		if (open)
		{
			const float fade = easeOut(static_cast<float>((now - journalOpenAt_) / 0.25));
			// The day opened: its focus, the routine as it was that day (ticks, tallies, each step's notes), then what was
			// trained. Drawn on a panel under the row (background behind, measured after the content).
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
			const float u = uiScale();
			const float pad = 18.f * u;
			const ImVec2 panelTop(slot.x, ImGui::GetCursorScreenPos().y + 4.f * u);
			dl->ChannelsSplit(2);
			dl->ChannelsSetCurrent(1);
			const float left = slot.x + pad;
			const float wrapAt = slot.x + width - pad - ImGui::GetWindowPos().x; // window-relative
			ImGui::SetCursorScreenPos(ImVec2(left, panelTop.y + pad * 0.8f));
			ImGui::PushTextWrapPos(wrapAt);
			const auto caps = [&](const char* text, const ImVec4& col) {
				ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y));
				ImGui::PushStyleColor(ImGuiCol_Text, col);
				theme::fontScale(0.82f);
				ImGui::TextUnformatted(text);
				theme::fontScale(1.f);
				ImGui::PopStyleColor();
			};
			if (!d.focus.empty())
			{
				caps("FOCUS", pal.accent);
				ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y));
				ImGui::TextUnformatted(d.focus.c_str());
				ImGui::Dummy(ImVec2(0, 8.f * u));
			}
			std::string section = "\x01";
			for (const auto& st : d.steps)
			{
				if (st.section != section)
				{
					section = st.section;
					std::string head = section.empty() ? std::string("ROUTINE") : section;
					for (auto& ch : head)
						ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
					ImGui::Dummy(ImVec2(0, 2.f * u));
					caps(head.c_str(), pal.textDim);
				}
				// A tick circle, the step, its tally on the right, its note under it.
				const ImVec2 at(left, ImGui::GetCursorScreenPos().y + 2.f * u);
				const float r = fs * 0.42f;
				const ImVec2 c(at.x + r, at.y + fs * 0.55f);
				if (st.done)
				{
					dl->AddCircleFilled(c, r, theme::u32(pal.accent), 20);
					const ImVec2 tick[] = {ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x - r * 0.1f, c.y + r * 0.35f), ImVec2(c.x + r * 0.5f, c.y - r * 0.35f)};
					dl->AddPolyline(tick, 3, IM_COL32(8, 12, 18, 255), false, 2.f * u);
				}
				else
				{
					dl->AddCircle(c, r - 0.5f, theme::u32(pal.textDim, 0.7f), 20, 1.5f * u);
				}
				const float textX = at.x + r * 2.f + 10.f * u;
				ImGui::SetCursorScreenPos(ImVec2(textX, at.y));
				ImGui::PushStyleColor(ImGuiCol_Text, st.done ? pal.text : pal.textDim);
				ImGui::TextUnformatted(st.title.empty() ? "(untitled step)" : st.title.c_str());
				ImGui::PopStyleColor();
				const float rowBottom = ImGui::GetItemRectMax().y;
				if (st.optional)
				{
					ImGui::SameLine(0.f, 8.f * u);
					ImGui::PushStyleColor(ImGuiCol_Text, pal.textDim);
					theme::fontScale(0.8f);
					ImGui::TextUnformatted("optional");
					theme::fontScale(1.f);
					ImGui::PopStyleColor();
				}
				if (!st.tally.empty())
				{
					const ImVec2 ts = ImGui::CalcTextSize(st.tally.c_str());
					dl->AddText(ImVec2(slot.x + width - pad - ts.x, at.y), theme::u32(st.done ? pal.accent : pal.textDim), st.tally.c_str());
				}
				ImGui::SetCursorScreenPos(ImVec2(textX, rowBottom + 2.f * u));
				if (!st.note.empty())
				{
					ImGui::PushStyleColor(ImGuiCol_Text, theme::mix(pal.textDim, pal.text, 0.35f));
					ImGui::TextWrapped("%s", st.note.c_str());
					ImGui::PopStyleColor();
					ImGui::SetCursorScreenPos(ImVec2(textX, ImGui::GetCursorScreenPos().y + 2.f * u));
				}
				ImGui::Dummy(ImVec2(0, 3.f * u));
			}
			if (!d.notes.empty())
			{
				ImGui::Dummy(ImVec2(0, 2.f * u));
				caps("NOTES", pal.textDim);
				for (const auto& n : d.notes)
				{
					ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y));
					ImGui::TextWrapped("%s", n.c_str());
				}
			}
			if (!d.trained.empty())
			{
				ImGui::Dummy(ImVec2(0, 6.f * u));
				caps("TRAINED", pal.textDim);
				// Chips: "Mechanics  40 reps  55%", wrapping across the panel.
				float x = left;
				float y = ImGui::GetCursorScreenPos().y + 2.f * u;
				const float chipH = fs * 1.55f;
				for (const auto& t : d.trained)
				{
					char buf[96];
					snprintf(buf, sizeof(buf), "%s   %d reps   %d%%", categoryLabel(t.category).c_str(), t.reps, t.reps > 0 ? 100 * t.wins / t.reps : 0);
					const float w = ImGui::CalcTextSize(buf).x + 24.f * u;
					if (x > left && x + w > slot.x + width - pad)
					{
						x = left;
						y += chipH + 6.f * u;
					}
					dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + chipH), theme::u32(pal.textDim, 0.12f), chipH * 0.5f);
					dl->AddText(ImVec2(x + 12.f * u, y + (chipH - fs) * 0.5f), theme::u32(pal.text), buf);
					x += w + 6.f * u;
				}
				ImGui::SetCursorScreenPos(ImVec2(left, y + chipH + 4.f * u));
			}
			if (d.planIsCurrent)
			{
				ImGui::Dummy(ImVec2(0, 4.f * u));
				ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y));
				ImGui::PushStyleColor(ImGuiCol_Text, pal.textDim);
				theme::fontScale(0.82f);
				ImGui::TextWrapped("Logged before Free Touch kept each day's routine: these are your routine's steps as they are now.");
				theme::fontScale(1.f);
				ImGui::PopStyleColor();
			}
			ImGui::PopTextWrapPos();
			const float panelBottom = ImGui::GetCursorScreenPos().y + pad * 0.6f;
			dl->ChannelsSetCurrent(0);
			dl->AddRectFilled(panelTop, ImVec2(slot.x + width, panelBottom), theme::u32(pal.card, 0.75f), 10.f);
			dl->AddRect(panelTop, ImVec2(slot.x + width, panelBottom), theme::u32(pal.accent, 0.18f), 10.f);
			dl->ChannelsMerge();
			ImGui::SetCursorScreenPos(ImVec2(slot.x, panelBottom));
			ImGui::Dummy(ImVec2(width, 0.f));
			ImGui::PopStyleVar();
			ImGui::Dummy(ImVec2(0, 8));
		}
		ImGui::Dummy(ImVec2(0, 5));
		ImGui::PopID();
		++index;
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Replays page: replay takeover

void Dashboard::renderReplays(const DashboardModel& m, const DashboardStatus& s)
{
	const theme::Palette& pal = theme::palette();
	pageTitle("Replays", "Take over any moment from your replays and play it out in Free Play.");
	const float width = ImGui::GetContentRegionAvail().x;
	const ReplayView* r = s.replay.get();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	const double now = ImGui::GetTime();

	if (r && r->active)
	{
		CardScope card(width, 22.f);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		// A live dot: you are watching.
		const ImVec2 hp = ImGui::GetCursorScreenPos();
		const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) * 4.f);
		dl->AddCircleFilled(ImVec2(hp.x + 5.f, hp.y + fs * 0.45f), 4.f, IM_COL32(255, 80, 80, 255), 12);
		dl->AddCircle(ImVec2(hp.x + 5.f, hp.y + fs * 0.45f), 6.f + 3.f * pulse, IM_COL32(255, 80, 80, static_cast<int>(120 * (1.f - pulse))), 16, 1.5f);
		ImGui::SetCursorScreenPos(ImVec2(hp.x + 16.f, hp.y));
		heading("NOW WATCHING", card.inner() - 16.f);
		const int secs = static_cast<int>(r->time);
		char buf[160];
		snprintf(buf, sizeof(buf), "%s", r->name.empty() ? "Replay" : r->name.c_str());
		theme::fontScale(1.35f);
		ImGui::TextUnformatted(buf);
		theme::fontScale(1.f);
		ImGui::TextDisabled("%d:%02d%s%s", secs / 60, secs % 60, r->map.empty() ? "" : "   |   ", r->map.c_str());
		ImGui::Dummy(ImVec2(0, 8));

		// Who you will be: the camera's player unless you pick another (team colours, blue first).
		heading("PLAY AS", card.inner());
		uint64_t pick = replayPick_;
		if (std::none_of(r->players.begin(), r->players.end(), [&](const ReplayView::Player& p) { return p.id == pick; }))
			pick = r->focusId;
		for (int team = 0; team < 2; ++team)
		{
			bool first = true;
			for (const auto& pl : r->players)
			{
				if (pl.team != team)
					continue;
				if (!first)
					ImGui::SameLine(0.f, 8.f);
				first = false;
				ImGui::PushID(static_cast<int>(pl.id & 0x7fffffff));
				const ImVec4 teamCol = team == 0 ? pal.blue : pal.orange;
				const bool sel = pl.id == pick;
				ImGui::PushStyleColor(ImGuiCol_Button, sel ? teamCol : theme::mix(pal.card, teamCol, 0.16f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::mix(pal.card, teamCol, sel ? 0.9f : 0.45f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, teamCol);
				ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(0.04f, 0.05f, 0.08f, 1.f) : pal.text);
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 20.f);
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.f, 8.f));
				const std::string label = (pl.name.empty() ? std::string("Player") : pl.name) + (pl.id == r->focusId ? "   camera" : "");
				if (navButton(label.c_str()))
					replayPick_ = pl.id;
				ImGui::PopStyleVar(2);
				ImGui::PopStyleColor(4);
				ImGui::PopID();
			}
		}
		if (r->players.empty())
			ImGui::TextDisabled("No cars in this part of the replay.");
		ImGui::Dummy(ImVec2(0, 8));

		// Who comes along, and how it starts.
		const float half = std::min((card.inner() - 20.f) * 0.5f, 360.f);
		const ImVec2 optTop = ImGui::GetCursorScreenPos();
		ImGui::BeginGroup();
		heading("EVERYONE ELSE", half);
		static const char* kOthers[] = {"All as bots", "Opponents", "Nobody"};
		replayOthers_ = segmented("replay_others", kOthers, 3, replayOthers_, half, replayOthersAnim_);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + half);
		ImGui::TextDisabled("%s", replayOthers_ == 0 ? "Teammates on your side, opponents against you (your Opponent setting picks the bot)."
			: replayOthers_ == 1 ? "Only the other team, as bots." : "Just you and the ball.");
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		const bool side = card.inner() >= 2.f * half + 20.f;
		ImGui::SetCursorScreenPos(side ? ImVec2(optTop.x + half + 20.f, optTop.y) : ImVec2(optTop.x, ImGui::GetItemRectMax().y + 10.f));
		ImGui::BeginGroup();
		heading("START", half);
		static const char* kStart[] = {"Watch the lead-up", "3-2-1"};
		replayStart_ = segmented("replay_start", kStart, 2, replayStart_, half, replayStartAnim_);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + half);
		ImGui::TextDisabled("%s", replayStart_ == 0 ? "Everyone replays the two seconds before it, then it waits for you: press anything to play."
			: "Straight in after a countdown, with a map of where everyone is.");
		ImGui::PopTextWrapPos();
		ImGui::EndGroup();
		ImGui::Dummy(ImVec2(0, 12));

		const bool ready = pick != 0;
		if (bigButton("takeover", "Take over this moment", ImVec2(std::min(card.inner(), 380.f), 54.f), ready, Icon::Play, true, Fx::Confirm)
			&& ready && cb_.takeover)
		{
			cb_.takeover(pick, replayOthers_ == 0 ? 2 : replayOthers_ == 1 ? 1 : 0, replayStart_ == 0);
			closeRequested_ = true;
		}
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + card.inner());
		ImGui::TextDisabled("Pause on the moment first. R3 on the controller does the same as the camera's player, with everyone "
			"and the lead-up. %s", r->framesBuffered >= 3 ? "" : "Let the replay play a moment before pausing, so the speeds and the lead-up can be recorded.");
		if (!r->message.empty())
			ImGui::TextColored(pal.accent, "%s", r->message.c_str());
		ImGui::PopTextWrapPos();
	}
	else
	{
		// How it works, in three steps.
		CardScope card(width, 22.f);
		heading("HOW IT WORKS", card.inner());
		static const std::pair<const char*, const char*> kSteps[] = {
			{"Open a replay", "From Rocket League's Match History or saved Replays, with Free Touch loaded."},
			{"Pause on the moment", "Press R3, or open Free Touch here to pick who you are and who comes along."},
			{"Play it out", "Everyone replays the lead-up, then it's yours: score it or save it, as often as you like."},
		};
		const float gap = 14.f;
		const int perRow = card.inner() >= 700.f ? 3 : 1;
		const float cw = (card.inner() - gap * (perRow - 1)) / perRow;
		const ImVec2 row = ImGui::GetCursorScreenPos();
		const float h = std::max(104.f, 40.f + fs * 4.6f); // the title and three lines of text at any GUI scale
		ImDrawList* dl = ImGui::GetWindowDrawList();
		for (int i = 0; i < 3; ++i)
		{
			const float rise = easeOut(static_cast<float>((now - pageAt_ - 0.08 * i) / 0.4));
			const ImVec2 a(row.x + (i % perRow) * (cw + gap), row.y + (i / perRow) * (h + gap) + 10.f * (1.f - rise));
			dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + h), IM_COL32(6, 9, 16, 120), 12.f);
			const ImVec2 c(a.x + 30.f, a.y + 32.f);
			dl->AddCircleFilled(c, 17.f, theme::u32(pal.accent), 24);
			char n[4];
			snprintf(n, sizeof(n), "%d", i + 1);
			const ImVec2 ts = theme::fontAt(fs * 1.1f)->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0.f, n);
			dl->AddText(theme::fontAt(fs * 1.1f), fs * 1.1f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(8, 12, 18, 255), n);
			dl->AddText(theme::fontAt(fs * 1.05f), fs * 1.05f, ImVec2(a.x + 60.f, a.y + 20.f), theme::u32(pal.text), kSteps[i].first);
			dl->AddText(theme::fontAt(fs * 0.85f), fs * 0.85f, ImVec2(a.x + 60.f, a.y + 24.f + fs * 1.3f), theme::u32(pal.textDim), kSteps[i].second, nullptr,
				cw - 76.f);
		}
		ImGui::Dummy(ImVec2(card.inner(), ((3 + perRow - 1) / perRow) * (h + gap) - gap));
		if (r && !r->message.empty())
			ImGui::TextColored(pal.accent, "%s", r->message.c_str());
	}
	ImGui::Dummy(ImVec2(0, 14));

	// Saved moments.
	char countText[32];
	snprintf(countText, sizeof(countText), "%d saved", static_cast<int>(m.replayMoments.size()));
	heading("YOUR REPLAY MOMENTS", width, m.replayMoments.empty() ? nullptr : countText);
	if (m.replayMoments.empty())
	{
		ImGui::TextDisabled("Every moment you take over is saved here (and under Exercises > Replays) to play again.");
		return;
	}
	const float gap = 12.f;
	const int perRow = width >= 900.f ? 2 : 1;
	const float cw = (width - gap * (perRow - 1)) / perRow;
	int i = 0;
	const ImVec2 row = ImGui::GetCursorScreenPos();
	const float h = std::max(104.f, 34.f + fs * 3.6f); // the tag, the name and the arena at any GUI scale
	const float bs = fs * 1.1f;
	const float btnH = std::max(36.f, bs * 1.6f);
	const float playW = std::max(86.f, theme::fontAt(bs)->CalcTextSizeA(bs, FLT_MAX, 0.f, "Play").x + 28.f);
	const float delW = std::max(66.f, theme::fontAt(bs)->CalcTextSizeA(bs, FLT_MAX, 0.f, "Delete").x + 24.f);
	for (const auto& mo : m.replayMoments)
	{
		ImGui::PushID(mo.id.c_str());
		const ImVec2 slot(row.x + (i % perRow) * (cw + gap), row.y + (i / perRow) * (h + gap));
		float& hv = hoverAnim_["replay." + mo.id];
		hv = approach(hv, ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + cw, slot.y + h)) ? 1.f : 0.f, 9.f);
		const ImVec2 a(slot.x, slot.y - 2.f * hv), b(a.x + cw, a.y + h);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		cellCard(dl, a, b, hv * 0.6f);
		const ImVec4 tagCol = mo.save ? pal.blue : pal.orange;
		dl->AddRectFilled(ImVec2(a.x, a.y + 14.f), ImVec2(a.x + 4.f, b.y - 14.f), theme::u32(tagCol), 2.f);
		pill(dl, ImVec2(a.x + 18.f, a.y + 14.f), mo.save ? "SAVE IT" : "FINISH IT", tagCol);
		const float textW = cw - 18.f - playW - delW - 6.f - 32.f;
		dl->AddText(theme::fontAt(fs * 1.05f), fs * 1.05f, ImVec2(a.x + 18.f, a.y + 20.f + fs * 1.3f), theme::u32(pal.text), fitText(mo.name, textW).c_str());
		if (!mo.arena.empty())
			dl->AddText(theme::fontAt(fs * 0.8f), fs * 0.8f, ImVec2(a.x + 18.f, a.y + 22.f + fs * 2.55f), theme::u32(pal.textDim), mo.arena.c_str());
		ImGui::SetCursorScreenPos(ImVec2(b.x - 16.f - delW - 6.f - playW, a.y + (h - btnH) * 0.5f));
		if (bigButton("play", "Play", ImVec2(playW, btnH), true, Icon::Play, false, Fx::Confirm) && cb_.play)
		{
			cb_.play(mo.id, "medium");
			closeRequested_ = true;
		}
		ImGui::SameLine(0.f, 6.f);
		const bool asking = replayDeleteAsk_ == mo.id;
		if (bigButton("del", asking ? "Sure?" : "Delete", ImVec2(delW, btnH), false, Icon::Stop, false, Fx::Click))
		{
			if (asking && cb_.deleteReplayMoment)
			{
				cb_.deleteReplayMoment(mo.id);
				toast("Moment deleted");
				replayDeleteAsk_.clear();
			}
			else
				replayDeleteAsk_ = mo.id;
		}
		ImGui::PopID();
		++i;
	}
	const int rows = (i + perRow - 1) / perRow;
	ImGui::SetCursorScreenPos(row);
	ImGui::Dummy(ImVec2(width, rows * (h + gap)));
}

// ---------------------------------------------------------------------------------------------------------------
// Races page: movement races against your own ghost

void Dashboard::renderPacks(const DashboardModel& m, const DashboardStatus& s)
{
	const theme::Palette& pal = theme::palette();
	pageTitle("Training Packs", "Bring in your favourite training packs, then play them with a bot in goal or only part of the net open.");
	const float width = ImGui::GetContentRegionAvail().x;
	const double now = ImGui::GetTime();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	ImDrawList* dl = ImGui::GetWindowDrawList();

	// 1. Import: a code, or the pack the player is in right now; and options, side by side (stacked when narrow).
	const bool stacked = width < 820.f;
	const float leftW = stacked ? width : width * 0.52f, rightW = stacked ? width : width - leftW - 20.f;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	auto card = [&](ImVec2 at, float w, const std::function<void(float)>& body) {
		dl->ChannelsSplit(2);
		dl->ChannelsSetCurrent(1);
		ImGui::SetCursorScreenPos(ImVec2(at.x + 20.f, at.y + 18.f));
		ImGui::BeginGroup();
		body(w - 40.f);
		ImGui::EndGroup();
		const ImVec2 end(at.x + w, ImGui::GetItemRectMax().y + 18.f);
		dl->ChannelsSetCurrent(0);
		dl->AddRectFilled(at, end, theme::u32(theme::mix(pal.card, pal.accent, 0.05f)), 14.f);
		dl->AddRect(at, end, IM_COL32(255, 255, 255, 18), 14.f);
		dl->ChannelsMerge();
		return end.y;
	};
	const float leftEnd = card(top, leftW, [&](float inner) {
		sectionLabel("ADD A PACK");
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
		ImGui::TextDisabled("Type or paste its code (from the pack's page in the game, or any pack site). Free Touch opens it in custom "
			"training, reads every shot by itself in a few seconds, then starts it here in Free Play.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 2));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.f, 10.f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.f);
		ImGui::PushItemWidth(inner - 128.f);
		const bool enter = ImGui::InputTextWithHint("##pack_code", "42BF-686D-E047-574B", packCode_, sizeof(packCode_),
			ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::PopItemWidth();
		ImGui::PopStyleVar(2);
		ImGui::SameLine(0.f, 8.f);
		if ((bigButton("pack_import", s.packImporting ? "Reading..." : "Import", ImVec2(120.f, 42.f), !s.packImporting, Icon::Packs, false, Fx::Confirm)
			|| enter) && !s.packImporting && cb_.importPack && packCode_[0])
			cb_.importPack(packCode_);
		if (!s.trainingPackName.empty())
		{
			ImGui::Dummy(ImVec2(0, 4));
			const std::string label = "Import \"" + s.trainingPackName + "\" (the pack you're in)";
			if (bigButton("pack_import_here", fitText(label, inner - 40.f).c_str(), ImVec2(inner, 40.f), false, Icon::Packs, false, Fx::Confirm)
				&& !s.packImporting && cb_.importPack)
				cb_.importPack("");
		}
		if (!s.packImportMessage.empty())
		{
			ImGui::Dummy(ImVec2(0, 2));
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
			ImGui::TextColored(pal.accent, "%s", s.packImportMessage.c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::Dummy(ImVec2(0, 2));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
		ImGui::TextDisabled("Playing any pack in custom training also adds its shots as you go.");
		ImGui::PopTextWrapPos();
	});
	const ImVec2 rightAt = stacked ? ImVec2(top.x, leftEnd + 16.f) : ImVec2(top.x + leftW + 20.f, top.y);
	const float rightEnd = card(rightAt, rightW, [&](float inner) { packOptions(s, inner); });
	ImGui::SetCursorScreenPos(ImVec2(top.x, std::max(leftEnd, rightEnd) + 22.f));

	// 2. Your packs.
	std::vector<const ExerciseCard*> list;
	for (const auto& c : m.exercises)
		if (c.category == "packs")
			list.push_back(&c);
	std::stable_sort(list.begin(), list.end(), [](const ExerciseCard* a, const ExerciseCard* b) { return a->updated > b->updated; });
	char count[32];
	snprintf(count, sizeof(count), "%d", static_cast<int>(list.size()));
	heading("YOUR PACKS", width, list.empty() ? nullptr : count);
	if (list.empty())
	{
		ImGui::TextDisabled("No packs yet: add one above.");
		return;
	}
	const float gap = 14.f;
	const int perRow = width >= 980.f ? 3 : width >= 620.f ? 2 : 1;
	const float cw = (width - gap * (perRow - 1)) / perRow;
	const float ch = std::max(176.f, fs * 9.f); // title, two lines, the buttons at any GUI scale
	const ImVec2 row = ImGui::GetCursorScreenPos();
	int i = 0;
	for (const ExerciseCard* c : list)
	{
		ImGui::PushID(c->id.c_str());
		const float rise = easeOut(static_cast<float>((now - pageAt_ - 0.04 * i) / 0.4));
		const ImVec2 slot(row.x + (i % perRow) * (cw + gap), row.y + (i / perRow) * (ch + gap));
		float& hv = hoverAnim_["pack." + c->id];
		hv = approach(hv, ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + cw, slot.y + ch)) ? 1.f : 0.f, 9.f);
		const ImVec2 a(slot.x, slot.y + 12.f * (1.f - rise) - 3.f * hv), b(a.x + cw, a.y + ch);
		const ImageInfo art = c->image >= 0 && cb_.image ? cb_.image(c->image) : ImageInfo{};
		if (!drawCover(dl, art, a, b, 14.f, IM_COL32(130, 130, 130, 255), 0.03f * hv))
			dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.06f)), 14.f);
		roundedGradient(dl, a, b, IM_COL32(6, 9, 16, 235), IM_COL32(6, 9, 16, 160), IM_COL32(6, 9, 16, 160), IM_COL32(6, 9, 16, 235), 14.f);
		dl->AddRect(a, b, theme::u32(theme::mix(ImVec4(1, 1, 1, 0.08f), pal.accent, 0.6f * hv)), 14.f, ImDrawCornerFlags_All, 1.f + hv);
		std::string title = c->name.rfind("Pack: ", 0) == 0 ? c->name.substr(6) : c->name;
		shadowedText(dl, font, fs * 1.35f, ImVec2(a.x + 18.f, a.y + 16.f), IM_COL32(250, 252, 255, 255), fitText(title, cw - 40.f).c_str());
		// Buttons along the bottom, each sized to its label; the description gets the lines above them.
		const float bs = fs * 1.1f;
		const float btnH = std::max(36.f, bs * 1.6f);
		const auto btnW = [&](const char* label, float min) {
			return std::max(min, theme::fontAt(bs)->CalcTextSizeA(bs, FLT_MAX, 0.f, label).x + 28.f);
		};
		const float btnY = b.y - 16.f - btnH;
		{
			const float ds = fs * 0.82f;
			const float top = a.y + 22.f + fs * 1.35f;
			const int lines = std::max(1, static_cast<int>((btnY - 8.f - top) / (ds * 1.2f)));
			clampedText(dl, ds, ImVec2(a.x + 18.f, top), theme::u32(pal.textDim), c->description, cw - 36.f, lines);
		}
		const std::string code = packs::codeFromId(c->id);
		float bx = a.x + 18.f;
		ImGui::SetCursorScreenPos(ImVec2(bx, btnY));
		const float playW = btnW("Play", 96.f);
		if (bigButton("play", "Play", ImVec2(playW, btnH), true, Icon::Play, false, Fx::Confirm) && cb_.play)
		{
			cb_.play(c->id, kDifficulties[difficulty_]);
			closeRequested_ = true;
		}
		bx += playW + 8.f;
		if (!code.empty())
		{
			ImGui::SetCursorScreenPos(ImVec2(bx, btnY));
			const float rereadW = btnW("Re-read", 96.f);
			if (bigButton("reimport", "Re-read", ImVec2(rereadW, btnH), false, Icon::Packs, false, Fx::Click) && !s.packImporting && cb_.importPack)
				cb_.importPack(code);
			bx += rereadW + 8.f;
		}
		const bool confirming = confirmPackDelete_ == c->id;
		const float removeW = btnW("Remove", 86.f);
		ImGui::SetCursorScreenPos(ImVec2(b.x - 18.f - removeW, btnY));
		if (bigButton("delete", confirming ? "Sure?" : "Remove", ImVec2(removeW, btnH), false, Icon::Stop, false, Fx::Click))
		{
			if (confirming && cb_.deletePack)
			{
				cb_.deletePack(c->id);
				toast("Pack removed");
				confirmPackDelete_.clear();
			}
			else
			{
				confirmPackDelete_ = c->id;
			}
		}
		ImGui::PopID();
		++i;
	}
	const int rows = (i + perRow - 1) / perRow;
	ImGui::SetCursorScreenPos(row);
	ImGui::Dummy(ImVec2(width, rows * (ch + gap)));
	ImGui::Dummy(ImVec2(0, 6));
}

void Dashboard::openTutorial(const std::string& id)
{
	tutorial_ = TutorialOpen{};
	tutorial_.requested = true;
	tutorial_.id = id;
	tutorial_.stepAt = ImGui::GetTime();
	tutorialsShown_.insert(id);
	if (cb_.tutorialSeen)
		cb_.tutorialSeen(id); // shown once by itself; the ? button brings it back
}

void Dashboard::renderTutorial(const DashboardModel& m, const DashboardStatus& s)
{
	// Lambourne 2026-10-29: "short tutorial popups with visuals would be PERFECT". The first time a place opens (once
	// the plugin knows which were shown before), its tips; any time from the ? button.
	const char* here = hubKey(hubOf(page_));
	const auto tut = std::find_if(m.tutorials.begin(), m.tutorials.end(), [&](const Tutorial& t) { return t.id == here; });
	if (s.tutorialsSeen && tut != m.tutorials.end() && !s.tutorialsSeen->count(here) && !tutorialsShown_.count(here)
		&& !tutorial_.requested && !playSetup_.requested && !raceBoard_.requested && !ImGui::IsPopupOpen("tutorial")
		&& !ImGui::IsPopupOpen("play_setup") && !ImGui::IsPopupOpen("race_board"))
		openTutorial(here);
	if (tutorial_.requested)
	{
		ImGui::OpenPopup("tutorial");
		tutorial_.requested = false;
	}
	const auto open = std::find_if(m.tutorials.begin(), m.tutorials.end(), [&](const Tutorial& t) { return t.id == tutorial_.id; });
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
	const float popW = std::min(ws.x - 80.f * u, 700.f * u);
	ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(popW, 0.f), ImVec2(popW, ws.y - 40.f * u));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.03f));
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.f, 0.f, 0.f, 0.55f));
	ImGui::PushStyleColor(ImGuiCol_Border, theme::mix(pal.card, pal.accent, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 16.f * u);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.f * u, 20.f * u));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	if (ImGui::BeginPopupModal("tutorial", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (open == m.tutorials.end() || open->steps.empty())
			ImGui::CloseCurrentPopup();
		else
		{
			const Tutorial& t = *open;
			const int count = static_cast<int>(t.steps.size());
			tutorial_.step = std::clamp(tutorial_.step, 0, count - 1);
			const Tutorial::Step& st = t.steps[tutorial_.step];
			const float inner = ImGui::GetContentRegionAvail().x;
			ImDrawList* dl = ImGui::GetWindowDrawList();
			// Kicker: the place and the step.
			char kicker[96];
			snprintf(kicker, sizeof(kicker), "%s   %d / %d", t.title.c_str(), tutorial_.step + 1, count);
			ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
			ImGui::TextUnformatted(kicker);
			ImGui::PopStyleColor();
			ImGui::Dummy(ImVec2(0, 4.f * u));
			// The drawing (a quick fade in on each step).
			const ImVec2 sa = ImGui::GetCursorScreenPos();
			const float sceneH = std::min(240.f * u, (ws.y - 40.f * u) * 0.42f);
			const ImVec2 sb(sa.x + inner, sa.y + sceneH);
			const float age = static_cast<float>(ImGui::GetTime() - tutorial_.stepAt);
			tutorial::drawScene(dl, st.scene, sa, sb, age, u);
			if (age < 0.25f)
				dl->AddRectFilled(sa, sb, theme::u32(theme::mix(pal.bg, pal.card, 0.5f), 1.f - age / 0.25f), 12.f);
			ImGui::Dummy(ImVec2(inner, sceneH + 10.f * u));
			theme::fontScale(1.35f);
			ImGui::TextUnformatted(st.title.c_str());
			theme::fontScale(1.f);
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
			ImGui::PushStyleColor(ImGuiCol_Text, theme::mix(pal.text, pal.textDim, 0.25f));
			ImGui::TextUnformatted(st.text.c_str());
			ImGui::PopStyleColor();
			ImGui::PopTextWrapPos();
			ImGui::Dummy(ImVec2(0, 12.f * u));
			// Footer: the step dots, Skip, Back, Next / Got it.
			const ImVec2 fp = ImGui::GetCursorScreenPos();
			const float bh = 38.f * u;
			for (int i = 0; i < count; ++i)
			{
				const ImVec2 dc(fp.x + 8.f * u + i * 18.f * u, fp.y + bh * 0.5f);
				dl->AddCircleFilled(dc, i == tutorial_.step ? 5.f * u : 3.5f * u, theme::u32(i == tutorial_.step ? pal.accent : pal.textDim,
					i == tutorial_.step ? 1.f : 0.5f), 12);
			}
			const bool last = tutorial_.step == count - 1;
			const float nw = 140.f * u, gap = 10.f * u;
			float x = fp.x + inner - nw;
			ImGui::SetCursorScreenPos(ImVec2(x, fp.y));
			if (bigButton("tut_next", last ? "Got it" : "Next", ImVec2(nw, bh), true, Icon::Play, false, Fx::Confirm)
				|| ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Enter)))
			{
				if (last)
					ImGui::CloseCurrentPopup();
				else
				{
					++tutorial_.step;
					tutorial_.stepAt = ImGui::GetTime();
				}
			}
			if (tutorial_.step > 0)
			{
				x -= 110.f * u + gap;
				ImGui::SetCursorScreenPos(ImVec2(x, fp.y));
				if (navButton("Back", ImVec2(110.f * u, bh)))
				{
					--tutorial_.step;
					tutorial_.stepAt = ImGui::GetTime();
				}
			}
			if (!last)
			{
				x -= 100.f * u + gap;
				ImGui::SetCursorScreenPos(ImVec2(x, fp.y));
				if (navButton("Skip", ImVec2(100.f * u, bh)) || ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)))
					ImGui::CloseCurrentPopup();
			}
			else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)))
				ImGui::CloseCurrentPopup();
			ImGui::SetCursorScreenPos(ImVec2(fp.x, fp.y + bh));
			ImGui::Dummy(ImVec2(inner, 2.f * u));
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(3);
}

void Dashboard::renderRaceBoard(const DashboardStatus& s)
{
	// Phase 8: a race's times for you and your friends, or everyone, with their ghosts to race.
	if (raceBoard_.requested)
	{
		ImGui::OpenPopup("race_board");
		raceBoard_.requested = false;
	}
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	const float fs = ImGui::GetFontSize();
	const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
	const float popW = std::min(ws.x - 80.f * u, 760.f * u);
	ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + ws.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSizeConstraints(ImVec2(popW, 0.f), ImVec2(popW, ws.y - 40.f * u));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.03f));
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.f, 0.f, 0.f, 0.55f));
	ImGui::PushStyleColor(ImGuiCol_Border, theme::mix(pal.card, pal.accent, 0.5f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.f * u);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26.f * u, 22.f * u));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
	if (ImGui::BeginPopupModal("race_board", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
	{
		const Leaderboards::RaceBoard& b = s.boards.race;
		const float inner = ImGui::GetContentRegionAvail().x;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		theme::fontScale(1.35f);
		ImGui::TextUnformatted(fitText(raceBoard_.name, inner).c_str());
		theme::fontScale(1.f);
		ImGui::TextDisabled("Race anyone's ghost: their run as an orange car with their name over it.");
		ImGui::Dummy(ImVec2(0, 6.f * u));
		static const char* kScopes[] = {"Friends", "Everyone"};
		const int cur = b.scope == "all" ? 1 : 0;
		const int pick = segmented("race_scope", kScopes, 2, cur, std::min(inner, 320.f * u), raceScopeAnim_);
		if (pick != cur && cb_.refreshRaceBoard)
			cb_.refreshRaceBoard(raceBoard_.race, pick == 1 ? "all" : "friends");
		ImGui::Dummy(ImVec2(0, 6.f * u));
		if (!b.error.empty())
			ImGui::TextDisabled("%s", b.error.c_str());
		if (!b.loaded || b.race != raceBoard_.race)
			ImGui::TextDisabled("Loading...");
		else if (b.rows.empty())
			ImGui::TextDisabled("%s", b.scope == "all" ? "No times yet. Set the first one!" : "No times from you or your friends yet.");
		else
		{
			const float rowH = std::max(40.f * u, fs * 2.0f);
			double mine = 0.0;
			for (const auto& r : b.rows)
				if (r.me)
					mine = r.seconds;
			for (const auto& r : b.rows)
			{
				ImGui::PushID(r.player.c_str());
				const ImVec2 a = ImGui::GetCursorScreenPos(), e(a.x + inner, a.y + rowH);
				dl->AddRectFilled(a, e, theme::u32(r.me ? theme::mix(pal.card, pal.accent, 0.16f) : theme::mix(pal.bg, pal.card, 0.6f)), 10.f);
				char rank[16];
				snprintf(rank, sizeof(rank), "#%d", r.rank);
				dl->AddText(ImVec2(a.x + 14.f * u, a.y + (rowH - fs) * 0.5f),
					r.rank == 1 ? IM_COL32(255, 205, 60, 255) : theme::u32(r.me ? pal.accent : pal.textDim), rank);
				dl->AddText(ImVec2(a.x + 64.f * u, a.y + (rowH - fs) * 0.5f), theme::u32(pal.text),
					fitText(r.name + (r.me ? "  (you)" : ""), inner * 0.4f).c_str());
				char t[48];
				if (mine > 0.0 && !r.me)
					snprintf(t, sizeof(t), "%.2f s  (%+.2f)", r.seconds, r.seconds - mine);
				else
					snprintf(t, sizeof(t), "%.2f s", r.seconds);
				const float bw = 130.f * u;
				dl->AddText(ImVec2(a.x + inner * 0.48f, a.y + (rowH - fs) * 0.5f), theme::u32(pal.text), t);
				if (!r.me && r.hasGhost)
				{
					ImGui::SetCursorScreenPos(ImVec2(e.x - bw - 8.f * u, a.y + (rowH - 32.f * u) * 0.5f));
					if (bigButton("race_ghost", "Race ghost", ImVec2(bw, 32.f * u), true, Icon::Play, false, Fx::Confirm) && cb_.raceGhost)
					{
						cb_.raceGhost(raceBoard_.exerciseId, raceBoard_.race, r.player, r.name);
						closeRequested_ = true;
						ImGui::CloseCurrentPopup();
					}
				}
				ImGui::SetCursorScreenPos(ImVec2(a.x, e.y + 6.f * u));
				ImGui::Dummy(ImVec2(inner, 0.f));
				ImGui::PopID();
			}
			ImGui::TextDisabled("%d %s", b.players, b.players == 1 ? "player" : "players");
		}
		ImGui::Dummy(ImVec2(0, 10.f * u));
		const float cw = 150.f * u;
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + inner - cw);
		if (navButton("Close", ImVec2(cw, 38.f * u)) || ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(3);
	ImGui::PopStyleColor(3);
}

void Dashboard::renderRaces(const DashboardModel& m, const DashboardStatus& s)
{
	const theme::Palette& pal = theme::palette();
	pageTitle("Races", "Beat your best and that run becomes your ghost: a see-through car to race every time after.");
	const float width = ImGui::GetContentRegionAvail().x;
	const double now = ImGui::GetTime();
	ImFont* font = ImGui::GetFont();
	const float fs = ImGui::GetFontSize();
	std::vector<const ExerciseCard*> races;
	for (const auto& c : m.exercises)
		if (c.category == "races")
			races.push_back(&c);
	if (races.empty())
	{
		ImGui::TextDisabled("No races loaded (content/scenarios/races/).");
		return;
	}
	const float gap = 14.f;
	const int perRow = width >= 980.f ? 3 : width >= 620.f ? 2 : 1;
	const float cw = (width - gap * (perRow - 1)) / perRow;
	const float ch = std::max(196.f, fs * 10.5f); // title, three lines of description, the time and the button
	const ImVec2 row = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	int i = 0;
	for (const ExerciseCard* card : races)
	{
		ImGui::PushID(card->id.c_str());
		const float rise = easeOut(static_cast<float>((now - pageAt_ - 0.05 * i) / 0.4));
		const ImVec2 slot(row.x + (i % perRow) * (cw + gap), row.y + (i / perRow) * (ch + gap));
		float& hv = hoverAnim_["race." + card->id];
		hv = approach(hv, ImGui::IsMouseHoveringRect(slot, ImVec2(slot.x + cw, slot.y + ch)) ? 1.f : 0.f, 9.f);
		const ImVec2 a(slot.x, slot.y + 12.f * (1.f - rise) - 3.f * hv), b(a.x + cw, a.y + ch);
		// Art (the card's image, else the category art) darkened behind the text.
		const ImageInfo art = card->image >= 0 && cb_.image ? cb_.image(card->image) : ImageInfo{};
		if (!drawCover(dl, art, a, b, 14.f, IM_COL32(150, 150, 150, 255), 0.03f * hv))
			dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.card, pal.accent, 0.06f)), 14.f);
		roundedGradient(dl, a, b, IM_COL32(6, 9, 16, 230), IM_COL32(6, 9, 16, 150), IM_COL32(6, 9, 16, 150), IM_COL32(6, 9, 16, 230), 14.f);
		dl->AddRect(a, b, theme::u32(theme::mix(ImVec4(1, 1, 1, 0.08f), pal.accent, 0.6f * hv)), 14.f, ImDrawCornerFlags_All, 1.f + hv);
		std::string title = card->name;
		if (title.rfind("Race: ", 0) == 0)
			title = title.substr(6);
		if (!title.empty())
			title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
		shadowedText(dl, font, fs * 1.4f, ImVec2(a.x + 18.f, a.y + 16.f), IM_COL32(250, 252, 255, 255), fitText(title, cw - 40.f).c_str());
		// The description, cut to the lines that fit above the time (the full text is on hover).
		{
			const float ds = fs * 0.84f;
			const float top = a.y + 22.f + fs * 1.4f;
			const float bottom = b.y - fs * 3.2f - 6.f;
			const int lines = std::max(1, static_cast<int>((bottom - top) / (ds * 1.2f)));
			clampedText(dl, ds, ImVec2(a.x + 18.f, top), theme::u32(pal.textDim), card->description, cw - 36.f, lines);
		}
		// Best time (medium: races have one difficulty) and whether a ghost is waiting.
		double best = 0.0;
		if (s.stats)
		{
			const auto it = s.stats->find(card->id);
			if (it != s.stats->end())
				best = it->second.best[1];
		}
		char buf[48];
		// The fastest of you and your friends, from the service.
		std::string friendsLine;
		const auto keyIt = m.raceKeys.find(card->id);
		const std::string raceKey = keyIt != m.raceKeys.end() ? keyIt->second : std::string();
		// The small line under the time stops short of the buttons (Race, and Board when signed in).
		const bool online = s.account.state == Account::State::SignedIn && !raceKey.empty();
		const float raceW = std::max(92.f, theme::fontAt(fs * 1.1f)->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0.f, "Race").x + 40.f);
		const float boardW = std::max(80.f, ImGui::CalcTextSize("Board").x + 30.f);
		const float smallW = std::max(40.f, (b.x - 18.f - raceW - (online ? 8.f + boardW : 0.f)) - (a.x + 20.f) - 10.f);
		const auto small = [&](const std::string& text) {
			const float sz = fs * 0.78f;
			std::string t = text;
			while (t.size() > 3 && theme::fontAt(sz)->CalcTextSizeA(sz, FLT_MAX, 0.f, t.c_str()).x > smallW)
				t = t.substr(0, t.size() - 4) + "...";
			dl->AddText(theme::fontAt(sz), sz, ImVec2(a.x + 20.f, b.y - fs * 0.95f - 6.f), theme::u32(pal.textDim), t.c_str());
		};
		if (const auto lead = s.boards.raceLeaders.find(raceKey); lead != s.boards.raceLeaders.end())
		{
			char lb[96];
			if (lead->second.me)
				snprintf(lb, sizeof(lb), "YOU LEAD YOUR FRIENDS");
			else
				snprintf(lb, sizeof(lb), "BEST: %s %.2f", lead->second.name.c_str(), lead->second.seconds);
			friendsLine = lb;
		}
		if (best > 0.0)
		{
			snprintf(buf, sizeof(buf), "%.2f s", best);
			dl->AddText(theme::fontAt(fs * 1.9f), fs * 1.9f, ImVec2(a.x + 18.f, b.y - fs * 2.9f), theme::u32(pal.accent), buf);
			small(friendsLine.empty() ? "YOUR BEST  |  GHOST READY" : friendsLine);
		}
		else
		{
			dl->AddText(theme::fontAt(fs * 1.1f), fs * 1.1f, ImVec2(a.x + 18.f, b.y - fs * 2.4f), theme::u32(pal.text, 0.8f), "No time yet");
			small(friendsLine.empty() ? "FIRST RUN SETS THE GHOST" : friendsLine);
		}
		{
			// Bottom-right, sized to its label.
			const float bs = fs * 1.1f;
			const ImVec2 bsz(std::max(92.f, theme::fontAt(bs)->CalcTextSizeA(bs, FLT_MAX, 0.f, "Race").x + 40.f), std::max(36.f, bs * 1.6f));
			ImGui::SetCursorScreenPos(ImVec2(b.x - 18.f - bsz.x, b.y - 14.f - bsz.y));
			if (bigButton("race", "Race", bsz, true, Icon::Play, false, Fx::Confirm) && cb_.play)
			{
				cb_.play(card->id, "medium");
				closeRequested_ = true;
			}
			// The race's board (signed in): times for friends or everyone, and their ghosts to race.
			if (online)
			{
				const ImVec2 bbs(boardW, bsz.y);
				ImGui::SetCursorScreenPos(ImVec2(b.x - 18.f - bsz.x - 8.f - bbs.x, b.y - 14.f - bsz.y));
				if (navButton("Board", bbs) && cb_.refreshRaceBoard)
				{
					raceBoard_.requested = true;
					raceBoard_.exerciseId = card->id;
					raceBoard_.race = raceKey;
					raceBoard_.name = title;
					cb_.refreshRaceBoard(raceKey, s.boards.race.scope);
				}
			}
		}
		if (ImGui::IsMouseHoveringRect(a, b) && !card->description.empty())
		{
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.f);
			ImGui::TextUnformatted(card->description.c_str());
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		ImGui::PopID();
		++i;
	}
	const int rows = (i + perRow - 1) / perRow;
	ImGui::SetCursorScreenPos(row);
	ImGui::Dummy(ImVec2(width, rows * (ch + gap)));
	ImGui::Dummy(ImVec2(0, 6));
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
	ImGui::TextDisabled("Each race starts after a 2-second countdown and goes again by itself. The ghost is your best run, drawn as a "
		"light-blue outline with a trail; it starts with you.");
	ImGui::PopTextWrapPos();
}

// ---------------------------------------------------------------------------------------------------------------
// Daily Challenge page: today's ten shots, the same for every player; one attempt; the result to share

namespace
{
	// A shot's square: green a fast hit, yellow a hit, dark a miss, an outline still to play.
	ImU32 dailyMarkColor(int mark, float alpha = 1.f)
	{
		switch (mark)
		{
		case 1: return IM_COL32(83, 196, 108, static_cast<int>(255 * alpha));
		case 2: return IM_COL32(226, 186, 64, static_cast<int>(255 * alpha));
		case 3: return IM_COL32(58, 62, 72, static_cast<int>(255 * alpha));
		default: return IM_COL32(0, 0, 0, 0);
		}
	}

	// The squares in rows of `perRow`, each `size` wide.
	void dailyGrid(ImDrawList* dl, ImVec2 at, const std::vector<int>& marks, float size, float gap, int perRow, ImU32 outline)
	{
		for (size_t i = 0; i < marks.size(); ++i)
		{
			const int col = static_cast<int>(i) % perRow, row = static_cast<int>(i) / perRow;
			const ImVec2 a(at.x + col * (size + gap), at.y + row * (size + gap)), b(a.x + size, a.y + size);
			const float r = size * 0.18f;
			if (marks[i] == 0)
				dl->AddRect(a, b, outline, r, ImDrawCornerFlags_All, std::max(1.5f, size * 0.06f));
			else
				dl->AddRectFilled(a, b, dailyMarkColor(marks[i]), r);
		}
	}

	// "First touch lab: high lob" -> "High lob": the slot already says what kind of shot it is.
	std::string dailyShotName(const std::string& name)
	{
		for (const char* prefix : {"First touch lab: ", "Mechanics lab: ", "Movement: ", "Saves: ", "Target shooting: ",
				 "Air dribble setup: "})
			if (name.rfind(prefix, 0) == 0)
			{
				std::string rest = name.substr(std::char_traits<char>::length(prefix));
				if (!rest.empty())
					rest[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(rest[0])));
				return rest;
			}
		return name;
	}
}

void Dashboard::renderDailyCard(const DailyView& d)
{
	// Home: a slim card for today's daily: its number and state, the squares so far, and one button.
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	const float width = ImGui::GetContentRegionAvail().x;
	const float fs = ImGui::GetFontSize();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 a = ImGui::GetCursorScreenPos();
	const float h = std::max(76.f * u, fs * 3.4f);
	const ImVec2 b(a.x + width, a.y + h);
	float& hv = hoverAnim_["home.daily"];
	hv = approach(hv, ImGui::IsMouseHoveringRect(a, b) ? 1.f : 0.f, 9.f);
	cellCard(dl, a, b, hv * 0.6f);
	dl->AddRectFilled(ImVec2(a.x, a.y + 12.f * u), ImVec2(a.x + 4.f * u, b.y - 12.f * u), theme::u32(pal.accent), 2.f);

	char kicker[48];
	snprintf(kicker, sizeof(kicker), "DAILY CHALLENGE #%d", d.number);
	const float ks = fs * 0.8f;
	const float textTop = a.y + (h - ks * 1.3f - fs * 1.1f) * 0.5f;
	dl->AddText(theme::fontAt(ks), ks, ImVec2(a.x + 20.f * u, textTop), theme::u32(pal.accent), kicker);
	const int count = static_cast<int>(d.shots.size());
	std::string line;
	if (d.running)
		line = "Playing now: shot " + std::to_string(std::min(d.played + 1, count)) + " of " + std::to_string(count);
	else if (d.done)
		line = std::to_string(d.hits) + "/" + std::to_string(count) + "  |  " + daily::groupThousands(d.points) + " points";
	else if (d.played > 0)
		line = "Shot " + std::to_string(d.played + 1) + " of " + std::to_string(count) + " next  |  " + daily::groupThousands(d.points) + " so far";
	else
		line = "Today's ten shots, the same for everyone. One attempt counts.";

	// The button at the right, the squares left of it (when there is room), the line at the left.
	const char* label = d.done ? "See result" : d.played > 0 ? "Continue" : "Play";
	const float bs = fs * 1.1f;
	const ImVec2 bsz(std::max(130.f * u, theme::fontAt(bs)->CalcTextSizeA(bs, FLT_MAX, 0.f, label).x + 60.f * u), std::max(40.f * u, bs * 1.7f));
	std::vector<int> marks;
	for (const auto& s : d.shots)
		marks.push_back(s.mark);
	const float sq = std::round(16.f * u), gap = std::round(5.f * u);
	const float gridW = marks.empty() ? 0.f : marks.size() * sq + (marks.size() - 1) * gap;
	const float gridX = b.x - 18.f * u - bsz.x - 24.f * u - gridW;
	const float lineRoom = gridX - 24.f * u - (a.x + 20.f * u);
	const bool showGrid = lineRoom > 220.f * u;
	if (showGrid)
		dailyGrid(dl, ImVec2(gridX, a.y + (h - sq) * 0.5f), marks, sq, gap, 10, theme::u32(pal.textDim, 0.5f));
	const float textW = showGrid ? lineRoom : b.x - 18.f * u - bsz.x - 24.f * u - (a.x + 20.f * u);
	dl->AddText(theme::fontAt(fs * 1.1f), fs * 1.1f, ImVec2(a.x + 20.f * u, textTop + ks * 1.3f), theme::u32(pal.text),
		fitText(line, textW).c_str());
	ImGui::SetCursorScreenPos(ImVec2(b.x - 18.f * u - bsz.x, a.y + (h - bsz.y) * 0.5f));
	if (d.running)
	{
		const float ts = ImGui::CalcTextSize("Playing now").x;
		dl->AddText(ImVec2(b.x - 18.f * u - ts, a.y + (h - fs) * 0.5f), theme::u32(pal.textDim), "Playing now");
	}
	else if (bigButton("daily_card", label, bsz, !d.done, d.done ? Icon::Calendar : Icon::Play, true, Fx::Confirm))
	{
		if (d.done)
			page_ = Page::Daily;
		else if (cb_.startDaily)
		{
			cb_.startDaily();
			closeRequested_ = true;
		}
	}
	ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 14.f * u));
	ImGui::Dummy(ImVec2(width, 0.f));
}

void Dashboard::renderDailyBoard(const DashboardStatus& s, const DailyView& d, float width)
{
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	const float fs = ImGui::GetFontSize();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const Leaderboards::Board& b = s.boards.daily;
	ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
	ImGui::TextUnformatted("LEADERBOARD");
	ImGui::PopStyleColor();
	if (s.account.state != Account::State::SignedIn)
	{
		ImGui::TextDisabled("%s", s.account.state == Account::State::Offline ? "Can't reach the Free Touch service right now."
			: "Pick a name (Settings > Account) to see how friends and everyone did today.");
		if (s.account.state == Account::State::SignedOut && navButton("Pick a name in Settings", ImVec2(0, 30.f * u)))
			page_ = Page::Settings;
		return;
	}
	static const char* kScopes[] = {"Friends", "Everyone"};
	const int cur = b.scope == "all" ? 1 : 0;
	const ImVec2 top = ImGui::GetCursorScreenPos();
	const int pick = segmented("daily_scope", kScopes, 2, cur, std::min(width - 130.f * u, 320.f * u), dailyBoardAnim_);
	if (pick != cur && cb_.refreshDailyBoard)
		cb_.refreshDailyBoard(d.date, pick == 1 ? "all" : "friends");
	const ImVec2 below = ImGui::GetCursorScreenPos();
	const float rw = 100.f * u;
	ImGui::SetCursorScreenPos(ImVec2(top.x + width - rw, top.y));
	if (navButton(b.busy ? "..." : "Refresh##daily", ImVec2(rw, 0.f)) && !b.busy && cb_.refreshDailyBoard)
		cb_.refreshDailyBoard(d.date, b.scope);
	ImGui::SetCursorScreenPos(below);
	if (!d.done)
		ImGui::TextDisabled("Finish today's daily to get on the board. One attempt counts.");
	if (!b.error.empty())
		ImGui::TextDisabled("%s", b.error.c_str());
	if (!b.loaded || b.day != d.date)
	{
		ImGui::TextDisabled("Loading...");
		return;
	}
	if (b.rows.empty())
	{
		ImGui::TextDisabled("%s", b.scope == "all" ? "Nobody has finished today's daily yet." : "None of your friends have finished today's daily yet.");
		return;
	}
	ImGui::Dummy(ImVec2(0, 4.f * u));
	const float rowH = std::max(40.f * u, fs * 2.0f);
	int lastRank = 0;
	for (const auto& r : b.rows)
	{
		if (lastRank != 0 && r.rank > lastRank + 1 && b.scope == "all")
		{
			ImGui::TextDisabled("...");
		}
		lastRank = r.rank;
		const ImVec2 a = ImGui::GetCursorScreenPos(), e(a.x + width, a.y + rowH);
		dl->AddRectFilled(a, e, theme::u32(r.me ? theme::mix(pal.card, pal.accent, 0.16f) : theme::mix(pal.bg, pal.card, 0.6f)), 10.f);
		char rank[16];
		snprintf(rank, sizeof(rank), "#%d", r.rank);
		const ImU32 rankCol = r.rank == 1 ? IM_COL32(255, 205, 60, 255) : theme::u32(r.me ? pal.accent : pal.textDim);
		dl->AddText(ImVec2(a.x + 16.f * u, a.y + (rowH - fs) * 0.5f), rankCol, rank);
		const std::string name = r.name + (r.me ? "  (you)" : "");
		dl->AddText(ImVec2(a.x + 70.f * u, a.y + (rowH - fs) * 0.5f), theme::u32(pal.text), fitText(name, width * 0.42f - 80.f * u).c_str());
		std::vector<int> marks;
		for (const char c : r.marks)
			marks.push_back(c - '0');
		marks.resize(std::max<size_t>(marks.size(), d.shots.size()), 0);
		const float sq = std::round(14.f * u), gap = std::round(4.f * u);
		dailyGrid(dl, ImVec2(a.x + width * 0.42f, a.y + (rowH - sq) * 0.5f), marks, sq, gap, 10, theme::u32(pal.textDim, 0.35f));
		char right[48];
		snprintf(right, sizeof(right), "%d/%d   %s", r.hits, static_cast<int>(d.shots.size()), daily::groupThousands(r.points).c_str());
		const ImVec2 rs = ImGui::CalcTextSize(right);
		dl->AddText(ImVec2(e.x - 16.f * u - rs.x, a.y + (rowH - fs) * 0.5f), theme::u32(pal.text), right);
		ImGui::SetCursorScreenPos(ImVec2(a.x, e.y + 6.f * u));
		ImGui::Dummy(ImVec2(width, 0.f));
	}
	if (b.players > 0)
		ImGui::TextDisabled("%d %s today", b.players, b.players == 1 ? "player" : "players");
}

void Dashboard::renderDaily(const DashboardModel& m, const DashboardStatus& s)
{
	(void)m;
	const theme::Palette& pal = theme::palette();
	const float u = uiScale();
	const float fs = ImGui::GetFontSize();
	const double now = ImGui::GetTime();
	pageTitle("Daily Challenge", "The same ten shots for every Free Touch player today. One attempt counts.");
	const DailyView* d = s.daily.get();
	if (!d || !d->available)
	{
		ImGui::TextDisabled("%s", d && d->number < 1 ? "The first Daily Challenge is on 19 October."
			: "No daily challenge loaded (content/daily/daily.json).");
		return;
	}
	const float width = ImGui::GetContentRegionAvail().x;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const int count = static_cast<int>(d->shots.size());

	// Today: the number and date, the state, the squares, the streak, and Start / Continue / Copy result.
	{
		CardScope card(width, 24.f * u);
		const float inner = card.inner();
		const ImVec2 top = ImGui::GetCursorScreenPos();
		const float sideW = 180.f * u; // the streak column at the right
		ImGui::PushTextWrapPos(top.x - ImGui::GetWindowPos().x + inner - sideW - 20.f * u); // window-relative
		char kicker[48];
		snprintf(kicker, sizeof(kicker), "DAILY #%d", d->number);
		ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
		ImGui::TextUnformatted(kicker);
		ImGui::PopStyleColor();
		theme::fontScale(1.6f);
		ImGui::TextUnformatted(niceDay(d->date).c_str());
		theme::fontScale(1.f);
		if (d->done)
		{
			// The score, big, with the hits under it.
			theme::fontScale(2.4f);
			ImGui::PushStyleColor(ImGuiCol_Text, pal.accent);
			ImGui::TextUnformatted(daily::groupThousands(d->points).c_str());
			ImGui::PopStyleColor();
			theme::fontScale(1.f);
			ImGui::TextDisabled("points  |  %d of %d shots", d->hits, count);
		}
		else if (d->running)
			ImGui::TextDisabled("Playing now: shot %d of %d.", std::min(d->played + 1, count), count);
		else if (d->played > 0)
			ImGui::TextDisabled("%d of %d played, %s points so far. Continue picks up at shot %d.", d->played, count,
				daily::groupThousands(d->points).c_str(), d->played + 1);
		else
			ImGui::TextDisabled("Ten shots, one rep each. Each waits for you: read it, then press any button and the clock "
				"starts. A hit is 100 points, up to 150 if it's quick. A retry, a skip or stopping mid-shot is a miss.");
		ImGui::PopTextWrapPos();
		ImGui::Dummy(ImVec2(0, 10.f * u));

		// The squares, big; they grow in as the page opens.
		std::vector<int> marks;
		for (const auto& sh : d->shots)
			marks.push_back(sh.mark);
		const float sq = std::round(std::max(30.f * u, fs * 1.6f)), gap = std::round(8.f * u);
		const ImVec2 gridAt = ImGui::GetCursorScreenPos();
		const float grow = easeOut(static_cast<float>((now - pageAt_) / 0.5));
		const float shown = sq * (0.85f + 0.15f * grow);
		dailyGrid(dl, ImVec2(gridAt.x + (sq - shown) * 0.5f, gridAt.y + (sq - shown) * 0.5f), marks, shown, gap + (sq - shown), 10,
			theme::u32(pal.textDim, 0.45f));
		ImGui::Dummy(ImVec2(count * sq + (count - 1) * gap, sq));
		ImGui::Dummy(ImVec2(0, 14.f * u));

		// The button, and a line beside it.
		const float bs = fs * 1.1f;
		const float btnH = std::max(46.f * u, bs * 1.7f);
		const auto besideButton = [&](const char* text) {
			ImGui::SameLine(0.f, 16.f * u);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (btnH - fs) * 0.5f);
			ImGui::TextDisabled("%s", text);
		};
		if (d->done)
		{
			if (bigButton("daily_copy", "Copy result", ImVec2(200.f * u, btnH), true, Icon::Calendar, true, Fx::Confirm))
			{
				// Through Windows' clipboard: ImGui's drops the green and yellow squares (characters past U+FFFF).
				const bool copied = cb_.copyText ? cb_.copyText(d->share) : (ImGui::SetClipboardText(d->share.c_str()), true);
				toast(copied ? "Copied: paste it to your friends" : "Couldn't reach the clipboard: try again");
			}
			char next[64];
			snprintf(next, sizeof(next), "Daily #%d is tomorrow.", d->number + 1);
			besideButton(next);
		}
		else if (d->running)
			ImGui::TextDisabled("Playing now. Stopping the exercise pauses it; the shot you're on counts as a miss.");
		else
		{
			if (bigButton("daily_start", d->played > 0 ? "Continue" : "Start", ImVec2(180.f * u, btnH), true, Icon::Play, true, Fx::Confirm)
				&& cb_.startDaily)
			{
				cb_.startDaily();
				closeRequested_ = true;
			}
			besideButton("Loads Free Play if you're not in it.");
		}

		// Right column: the streak and the best day.
		{
			const float colX = top.x + inner - sideW;
			const float big = fs * 2.6f;
			char num[16];
			snprintf(num, sizeof(num), "%d", d->streak);
			const ImVec2 ns = theme::fontAt(big)->CalcTextSizeA(big, FLT_MAX, 0.f, num);
			dl->AddText(theme::fontAt(big), big, ImVec2(colX + (sideW - ns.x) * 0.5f, top.y), theme::u32(d->streak > 0 ? pal.orange : pal.textDim), num);
			const char* lbl = "DAY STREAK";
			const float ls = fs * 0.78f;
			const ImVec2 lsz = theme::fontAt(ls)->CalcTextSizeA(ls, FLT_MAX, 0.f, lbl);
			dl->AddText(theme::fontAt(ls), ls, ImVec2(colX + (sideW - lsz.x) * 0.5f, top.y + big * 1.1f), theme::u32(pal.textDim), lbl);
			if (d->bestPoints > 0)
			{
				const std::string best = "Best " + daily::groupThousands(d->bestPoints);
				const ImVec2 bsz2 = ImGui::CalcTextSize(best.c_str());
				dl->AddText(ImVec2(colX + (sideW - bsz2.x) * 0.5f, top.y + big * 1.1f + ls * 1.7f), theme::u32(pal.textDim), best.c_str());
			}
		}
	}
	ImGui::Dummy(ImVec2(0, 16.f * u));

	// The shots: each drill shows once played (or once the day is done), with its result; afterwards, practise any.
	heading("TODAY'S SHOTS", width, d->done ? "practice doesn't count" : nullptr);
	{
		const float rowH = std::max(50.f * u, fs * 2.6f);
		for (size_t i = 0; i < d->shots.size(); ++i)
		{
			const auto& sh = d->shots[i];
			ImGui::PushID(static_cast<int>(i));
			const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + rowH);
			const bool revealed = d->done || sh.mark != 0;
			dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.bg, pal.card, 0.75f)), 10.f);
			// The number in its square's colour.
			const float sq = std::round(rowH * 0.56f);
			const ImVec2 na(a.x + 12.f * u, a.y + (rowH - sq) * 0.5f), nb(na.x + sq, na.y + sq);
			if (sh.mark == 0)
				dl->AddRect(na, nb, theme::u32(pal.textDim, 0.5f), sq * 0.18f, ImDrawCornerFlags_All, 1.5f);
			else
				dl->AddRectFilled(na, nb, dailyMarkColor(sh.mark), sq * 0.18f);
			char n[4];
			snprintf(n, sizeof(n), "%d", static_cast<int>(i + 1));
			const ImVec2 ts = ImGui::CalcTextSize(n);
			dl->AddText(ImVec2(na.x + (sq - ts.x) * 0.5f, na.y + (sq - ts.y) * 0.5f),
				sh.mark == 1 || sh.mark == 2 ? IM_COL32(12, 16, 22, 255) : theme::u32(pal.text), n);
			// The slot, and the drill once it is revealed.
			const float tx = nb.x + 14.f * u;
			const float ss = fs * 0.75f;
			const float textTop = a.y + (rowH - ss * 1.25f - fs) * 0.5f;
			std::string slot = sh.slot;
			for (auto& c : slot)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			dl->AddText(theme::fontAt(ss), ss, ImVec2(tx, textTop), theme::u32(pal.textDim), slot.c_str());
			const std::string name = revealed ? dailyShotName(sh.name) : std::string("?");
			dl->AddText(ImVec2(tx, textTop + ss * 1.25f), theme::u32(revealed ? pal.text : pal.textDim), fitText(name, width * 0.48f).c_str());
			// Right: the result, and Practice once the day is done.
			float right = b.x - 14.f * u;
			if (d->done && cb_.practiceDailyShot)
			{
				const float bw = std::max(110.f * u, ImGui::CalcTextSize("Practice").x + 40.f * u), bh = std::max(32.f * u, fs * 1.5f);
				ImGui::SetCursorScreenPos(ImVec2(right - bw, a.y + (rowH - bh) * 0.5f));
				if (navButton("Practice", ImVec2(bw, bh)))
				{
					cb_.practiceDailyShot(sh.scenarioId, sh.difficulty, sh.seed);
					closeRequested_ = true;
				}
				right -= bw + 16.f * u;
			}
			if (sh.mark != 0)
			{
				char res[64];
				if (sh.points > 0)
					snprintf(res, sizeof(res), "%s  +%d", sh.label.c_str(), sh.points);
				else
					snprintf(res, sizeof(res), "%s", sh.label.empty() ? "MISS" : sh.label.c_str());
				const ImVec2 rs = ImGui::CalcTextSize(res);
				dl->AddText(ImVec2(right - rs.x, a.y + (rowH - rs.y) * 0.5f), sh.points > 0 ? dailyMarkColor(sh.mark) : theme::u32(pal.textDim), res);
			}
			ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 6.f * u));
			ImGui::Dummy(ImVec2(width, 0.f));
			ImGui::PopID();
		}
	}

	// The leaderboard: today's scores for friends or everyone (Phase 8).
	ImGui::Dummy(ImVec2(0, 12.f * u));
	renderDailyBoard(s, *d, width);

	// Past days: number and date, the squares, hits and points.
	if (!d->history.empty())
	{
		ImGui::Dummy(ImVec2(0, 12.f * u));
		heading("PAST DAYS", width, nullptr);
		const float rowH = std::max(44.f * u, fs * 2.2f);
		for (const auto& day : d->history)
		{
			const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + width, a.y + rowH);
			dl->AddRectFilled(a, b, theme::u32(theme::mix(pal.bg, pal.card, 0.6f)), 10.f);
			char left[64];
			snprintf(left, sizeof(left), "#%d   %s", day.number, niceDay(day.date).c_str());
			dl->AddText(ImVec2(a.x + 16.f * u, a.y + (rowH - fs) * 0.5f), theme::u32(pal.text), left);
			std::vector<int> marks = day.marks;
			marks.resize(static_cast<size_t>(std::max(day.shots, static_cast<int>(marks.size()))), 0);
			const float sq = std::round(14.f * u), gap = std::round(4.f * u);
			dailyGrid(dl, ImVec2(a.x + width * 0.42f, a.y + (rowH - sq) * 0.5f), marks, sq, gap, 10, theme::u32(pal.textDim, 0.35f));
			char right[64];
			if (day.done)
				snprintf(right, sizeof(right), "%d/%d   %s", day.hits, day.shots, daily::groupThousands(day.points).c_str());
			else
				snprintf(right, sizeof(right), "unfinished   %s", daily::groupThousands(day.points).c_str());
			const ImVec2 rs = ImGui::CalcTextSize(right);
			dl->AddText(ImVec2(b.x - 16.f * u - rs.x, a.y + (rowH - fs) * 0.5f), theme::u32(day.done ? pal.text : pal.textDim), right);
			ImGui::SetCursorScreenPos(ImVec2(a.x, b.y + 6.f * u));
			ImGui::Dummy(ImVec2(width, 0.f));
		}
	}
}
