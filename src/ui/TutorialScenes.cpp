#include "pch.h"
#include "ui/TutorialScenes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/Icons.h"
#include "ui/Theme.h"

namespace
{
	using icons::Icon;
	constexpr float kPi = 3.14159265f;
	const ImU32 kGold = IM_COL32(255, 205, 60, 255);
	const ImU32 kGhost = IM_COL32(255, 170, 90, 255);
	const ImU32 kGreen = IM_COL32(83, 196, 108, 255);
	const ImU32 kYellow = IM_COL32(226, 186, 64, 255);
	const ImU32 kMiss = IM_COL32(58, 62, 72, 255);

	ImVec2 lerp(ImVec2 a, ImVec2 b, float k) { return ImVec2(a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k); }
	float ease(float x)
	{
		x = std::clamp(x, 0.f, 1.f);
		return 1.f - (1.f - x) * (1.f - x) * (1.f - x);
	}
	// 0..1 that ramps up every `period` seconds (a loop for the scene).
	float loop(float t, float period) { return std::fmod(t, period) / period; }
	ImU32 col(const ImVec4& c, float a = 1.f) { return theme::u32(c, a); }

	void text(ImDrawList* dl, ImVec2 at, ImU32 c, const char* s, float scale = 1.f)
	{
		const float fs = ImGui::GetFontSize() * scale;
		dl->AddText(theme::fontAt(fs), fs, at, c, s);
	}
	void textCentered(ImDrawList* dl, ImVec2 c, ImU32 colr, const char* s, float scale = 1.f)
	{
		const float fs = ImGui::GetFontSize() * scale;
		const ImVec2 ts = theme::fontAt(fs)->CalcTextSizeA(fs, FLT_MAX, 0.f, s);
		dl->AddText(theme::fontAt(fs), fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), colr, s);
	}

	// A car from above: a rounded body with a lighter nose, facing `angle` (radians).
	void car(ImDrawList* dl, ImVec2 c, float size, float angle, ImU32 body, ImU32 nose)
	{
		const float ca = std::cos(angle), sa = std::sin(angle);
		const auto p = [&](float x, float y) { return ImVec2(c.x + (x * ca - y * sa) * size, c.y + (x * sa + y * ca) * size); };
		const ImVec2 q[] = {p(-1.f, -0.55f), p(0.85f, -0.55f), p(1.05f, -0.3f), p(1.05f, 0.3f), p(0.85f, 0.55f), p(-1.f, 0.55f)};
		dl->AddConvexPolyFilled(q, 6, body);
		dl->AddLine(p(0.55f, -0.45f), p(0.55f, 0.45f), nose, std::max(1.5f, size * 0.12f));
	}

	void ball(ImDrawList* dl, ImVec2 c, float r)
	{
		dl->AddCircleFilled(c, r, IM_COL32(235, 238, 245, 255), 20);
		dl->AddCircle(c, r, IM_COL32(120, 130, 150, 255), 20, 1.5f);
	}

	void panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float r = 10.f)
	{
		const theme::Palette& pal = theme::palette();
		dl->AddRectFilled(a, b, col(theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.05f)), r);
		dl->AddRect(a, b, col(pal.textDim, 0.25f), r, ImDrawCornerFlags_All, 1.f);
	}

	// A small menu with seven places; the lit one moves down the list.
	void hubs(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		static const struct { Icon icon; const char* label; const char* caption; } kItems[] = {
			{Icon::Home, "Home", "Today at a glance"}, {Icon::Exercises, "Train", "Drills and routine"},
			{Icon::Trophy, "Compete", "Daily and races"}, {Icon::Record, "Create", "Your own drills"},
			{Icon::Friends, "Friends", "Requests and activity"}, {Icon::Stats, "Progress", "Rank, records, journal"},
			{Icon::Settings, "Settings", "Account and controls"}};
		const int n = 7;
		const int lit = static_cast<int>(t / 1.1f) % n;
		const float railW = (b.x - a.x) * 0.38f;
		const ImVec2 ra(a.x + 16.f * u, a.y + 12.f * u), rb(a.x + 16.f * u + railW, b.y - 12.f * u);
		panel(dl, ra, rb);
		const float rowH = (rb.y - ra.y - 12.f * u) / n;
		for (int i = 0; i < n; ++i)
		{
			const ImVec2 p(ra.x + 6.f * u, ra.y + 6.f * u + i * rowH);
			if (i == lit)
				dl->AddRectFilled(p, ImVec2(rb.x - 6.f * u, p.y + rowH - 2.f), col(theme::mix(pal.card, pal.accent, 0.3f)), 6.f);
			icons::draw(dl, kItems[i].icon, ImVec2(p.x + 14.f * u, p.y + rowH * 0.5f), 12.f * u, col(i == lit ? pal.accent : pal.textDim));
			text(dl, ImVec2(p.x + 30.f * u, p.y + rowH * 0.5f - ImGui::GetFontSize() * 0.42f), col(i == lit ? pal.text : pal.textDim), kItems[i].label, 0.85f);
		}
		// The lit place, large, with what's in it.
		const ImVec2 c((rb.x + b.x) * 0.5f, (a.y + b.y) * 0.5f - 14.f * u);
		const float pop = ease(loop(t, 1.1f) * 4.f);
		icons::draw(dl, kItems[lit].icon, c, (54.f + 6.f * pop) * u, col(pal.accent));
		textCentered(dl, ImVec2(c.x, c.y + 50.f * u), col(pal.text), kItems[lit].label, 1.3f);
		textCentered(dl, ImVec2(c.x, c.y + 76.f * u), col(pal.textDim), kItems[lit].caption, 0.9f);
	}

	// A page with tabs along the top: the underline slides from tab to tab; the ? button pulses.
	void tabs(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 pa(a.x + 24.f * u, a.y + 18.f * u), pb(b.x - 24.f * u, b.y - 14.f * u);
		panel(dl, pa, pb);
		static const char* kTabs[] = {"Exercises", "Sessions", "Freeplay+", "Routine"};
		const int cur = static_cast<int>(t / 1.3f) % 4;
		const float slide = ease(loop(t, 1.3f) * 3.f);
		float x = pa.x + 18.f * u;
		float xs[5];
		for (int i = 0; i < 4; ++i)
		{
			xs[i] = x;
			const ImVec2 ts = ImGui::CalcTextSize(kTabs[i]);
			text(dl, ImVec2(x + 8.f * u, pa.y + 14.f * u), col(i == cur ? pal.text : pal.textDim), kTabs[i]);
			x += ts.x + 30.f * u;
		}
		xs[4] = x;
		const int prev = (cur + 3) % 4;
		const float fromX = xs[prev], toX = xs[cur];
		const float fromW = xs[prev + 1] - xs[prev] - 14.f * u, toW = xs[cur + 1] - xs[cur] - 14.f * u;
		const float ux = fromX + (toX - fromX) * slide, uw = fromW + (toW - fromW) * slide;
		const float lineY = pa.y + 14.f * u + ImGui::GetFontSize() + 8.f * u;
		dl->AddLine(ImVec2(pa.x + 14.f * u, lineY + 2.f), ImVec2(pb.x - 50.f * u, lineY + 2.f), col(pal.textDim, 0.2f), 1.f);
		dl->AddRectFilled(ImVec2(ux + 4.f * u, lineY), ImVec2(ux + uw, lineY + 3.f * u), col(pal.accent), 2.f);
		// The ? button.
		const ImVec2 q(pb.x - 26.f * u, pa.y + 24.f * u);
		const float pulse = 0.5f + 0.5f * std::sin(t * 4.f);
		dl->AddCircle(q, (13.f + 3.f * pulse) * u, col(pal.accent, 0.3f + 0.4f * pulse), 24, 2.f);
		dl->AddCircleFilled(q, 12.f * u, col(pal.card), 24);
		dl->AddCircle(q, 12.f * u, col(pal.accent), 24, 1.5f);
		textCentered(dl, q, col(pal.text), "?");
		// Page content: a few cards.
		for (int i = 0; i < 3; ++i)
		{
			const float cx = pa.x + 18.f * u + i * ((pb.x - pa.x - 36.f * u) / 3.f);
			const ImVec2 ca(cx, lineY + 18.f * u), cb(cx + (pb.x - pa.x - 36.f * u) / 3.f - 12.f * u, pb.y - 14.f * u);
			dl->AddRectFilled(ca, cb, col(theme::mix(pal.card, pal.accent, i == cur % 3 ? 0.12f : 0.04f)), 8.f);
			dl->AddRectFilled(ImVec2(ca.x + 10.f * u, ca.y + 12.f * u), ImVec2(cb.x - 30.f * u, ca.y + 20.f * u), col(pal.textDim, 0.5f), 3.f);
			dl->AddRectFilled(ImVec2(ca.x + 10.f * u, ca.y + 28.f * u), ImVec2(cb.x - 50.f * u, ca.y + 34.f * u), col(pal.textDim, 0.25f), 3.f);
		}
	}

	// A controller: R3, the D-pad's left / right and down light up in turn with what they do.
	void controller(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f + 6.f * u);
		const float s = std::min(b.x - a.x, (b.y - a.y) * 1.9f) * 0.21f;
		const ImU32 body = col(theme::mix(pal.card, ImVec4(1, 1, 1, 1), 0.12f));
		const ImU32 edge = col(pal.textDim, 0.6f);
		// Body: two grips and the middle.
		dl->AddCircleFilled(ImVec2(c.x - s * 0.95f, c.y + s * 0.15f), s * 0.62f, body, 32);
		dl->AddCircleFilled(ImVec2(c.x + s * 0.95f, c.y + s * 0.15f), s * 0.62f, body, 32);
		dl->AddRectFilled(ImVec2(c.x - s * 1.1f, c.y - s * 0.55f), ImVec2(c.x + s * 1.1f, c.y + s * 0.45f), body, s * 0.35f);
		const int lit = static_cast<int>(t / 1.4f) % 3; // 0: R3, 1: D-pad left / right, 2: D-pad down
		const float pulse = 0.6f + 0.4f * std::sin(t * 6.f);
		// D-pad (left side).
		const ImVec2 dp(c.x - s * 0.95f, c.y - s * 0.05f);
		const float k = s * 0.13f;
		const auto pad = [&](ImVec2 o, bool on) {
			dl->AddRectFilled(ImVec2(dp.x + o.x - k, dp.y + o.y - k), ImVec2(dp.x + o.x + k, dp.y + o.y + k), on ? col(pal.accent, pulse) : edge, 3.f);
		};
		pad(ImVec2(0, -2.f * k), false);
		pad(ImVec2(-2.f * k, 0), lit == 1);
		pad(ImVec2(2.f * k, 0), lit == 1);
		pad(ImVec2(0, 2.f * k), lit == 2);
		dl->AddRectFilled(ImVec2(dp.x - k, dp.y - k), ImVec2(dp.x + k, dp.y + k), edge, 2.f);
		// Sticks: left, and the right one (R3) lower right.
		dl->AddCircleFilled(ImVec2(c.x - s * 0.45f, c.y + s * 0.3f), s * 0.2f, edge, 20);
		const ImVec2 r3(c.x + s * 0.45f, c.y + s * 0.3f);
		dl->AddCircleFilled(r3, s * 0.2f, lit == 0 ? col(pal.accent, pulse) : edge, 20);
		// Face buttons.
		for (int i = 0; i < 4; ++i)
		{
			const float ang = i * kPi * 0.5f;
			dl->AddCircleFilled(ImVec2(c.x + s * 0.95f + std::cos(ang) * s * 0.22f, c.y - s * 0.05f + std::sin(ang) * s * 0.22f), s * 0.09f, edge, 12);
		}
		// What the lit control does.
		static const char* kWhat[] = {"R3: reset the shot", "D-pad left / right: previous / next", "D-pad down: slow motion"};
		textCentered(dl, ImVec2(c.x, b.y - 22.f * u), col(pal.text), kWhat[lit], 1.05f);
		const ImVec2 from = lit == 0 ? r3 : lit == 1 ? ImVec2(dp.x + 2.f * k, dp.y) : ImVec2(dp.x, dp.y + 2.f * k);
		dl->AddLine(from, ImVec2(c.x, b.y - 34.f * u), col(pal.accent, 0.6f), 1.5f);
	}

	// Easy, medium and hard lit in turn over a drill card with its Play button.
	void difficulty(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 ca(a.x + 60.f * u, a.y + 20.f * u), cb(b.x - 60.f * u, b.y - 20.f * u);
		panel(dl, ca, cb, 12.f);
		text(dl, ImVec2(ca.x + 18.f * u, ca.y + 16.f * u), col(pal.text), "Aerial reads", 1.3f);
		static const char* kD[] = {"Easy", "Medium", "Hard"};
		const int cur = static_cast<int>(t / 1.2f) % 3;
		const float segW = (cb.x - ca.x - 36.f * u) / 3.f;
		const float y = ca.y + 60.f * u;
		dl->AddRectFilled(ImVec2(ca.x + 18.f * u, y), ImVec2(cb.x - 18.f * u, y + 34.f * u), col(pal.bg), 17.f * u);
		for (int i = 0; i < 3; ++i)
		{
			const ImVec2 sa(ca.x + 18.f * u + i * segW, y), sb(sa.x + segW, y + 34.f * u);
			if (i == cur)
				dl->AddRectFilled(ImVec2(sa.x + 3.f, sa.y + 3.f), ImVec2(sb.x - 3.f, sb.y - 3.f), col(pal.accent), 15.f * u);
			textCentered(dl, ImVec2((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f), i == cur ? IM_COL32(10, 14, 20, 255) : col(pal.text), kD[i]);
		}
		// Play and a star.
		const ImVec2 pa(cb.x - 140.f * u, cb.y - 52.f * u), pb(cb.x - 18.f * u, cb.y - 16.f * u);
		const float press = 1.f - 0.06f * std::max(0.f, std::sin(t * 3.f));
		dl->AddRectFilled(lerp(pa, pb, (1.f - press) * 0.5f), lerp(pb, pa, (1.f - press) * 0.5f), col(pal.accent), 10.f);
		textCentered(dl, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), IM_COL32(10, 14, 20, 255), "Play");
		icons::star(dl, ImVec2(ca.x + 34.f * u, cb.y - 34.f * u), 12.f * u, kGold, std::fmod(t, 2.4f) > 1.2f);
	}

	// A field from above: a car loops round while targets pop up and fade.
	void freeplay(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 fa(a.x + 40.f * u, a.y + 14.f * u), fb(b.x - 40.f * u, b.y - 14.f * u);
		dl->AddRectFilled(fa, fb, IM_COL32(28, 52, 40, 255), 24.f * u);
		dl->AddRect(fa, fb, IM_COL32(220, 230, 240, 120), 24.f * u, ImDrawCornerFlags_All, 2.f);
		const ImVec2 c((fa.x + fb.x) * 0.5f, (fa.y + fb.y) * 0.5f);
		dl->AddLine(ImVec2(c.x, fa.y), ImVec2(c.x, fb.y), IM_COL32(220, 230, 240, 90), 1.5f);
		dl->AddCircle(c, (fb.y - fa.y) * 0.16f, IM_COL32(220, 230, 240, 90), 32, 1.5f);
		const float gh = (fb.y - fa.y) * 0.3f;
		dl->AddRectFilled(ImVec2(fa.x - 8.f * u, c.y - gh * 0.5f), ImVec2(fa.x, c.y + gh * 0.5f), IM_COL32(70, 130, 230, 255), 3.f);
		dl->AddRectFilled(ImVec2(fb.x, c.y - gh * 0.5f), ImVec2(fb.x + 8.f * u, c.y + gh * 0.5f), IM_COL32(240, 140, 50, 255), 3.f);
		// Targets: a ring, then another, each growing in and fading.
		static const ImVec2 kSpots[] = {{0.25f, 0.3f}, {0.7f, 0.25f}, {0.6f, 0.72f}, {0.3f, 0.7f}, {0.82f, 0.5f}};
		for (int i = 0; i < 5; ++i)
		{
			const float age = std::fmod(t - i * 0.6f + 30.f, 3.f);
			if (age > 2.f)
				continue;
			const ImVec2 p(fa.x + (fb.x - fa.x) * kSpots[i].x, fa.y + (fb.y - fa.y) * kSpots[i].y);
			const float grow = ease(age * 2.f);
			const float fade = std::clamp(2.f - age, 0.f, 1.f);
			dl->AddCircle(p, 18.f * u * grow, IM_COL32(255, 205, 60, static_cast<int>(255 * fade)), 24, 3.f);
		}
		const float ang = t * 1.1f;
		const ImVec2 cp(c.x + std::cos(ang) * (fb.x - fa.x) * 0.3f, c.y + std::sin(ang) * (fb.y - fa.y) * 0.3f);
		car(dl, cp, 14.f * u, ang + kPi * 0.5f, IM_COL32(70, 130, 230, 255), IM_COL32(220, 235, 255, 255));
		ball(dl, ImVec2(c.x + std::cos(ang + 0.5f) * (fb.x - fa.x) * 0.3f, c.y + std::sin(ang + 0.5f) * (fb.y - fa.y) * 0.3f), 7.f * u);
	}

	// A routine: blocks ticked off one after another.
	void routine(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		static const char* kBlocks[] = {"Warm-up: first touches", "Shooting: 10 reps", "Aerials: 8 reps", "Cool-down: free play"};
		const float rowH = (b.y - a.y - 30.f * u) / 4.f;
		const int done = static_cast<int>(t / 0.9f) % 6;
		for (int i = 0; i < 4; ++i)
		{
			const ImVec2 ra(a.x + 90.f * u, a.y + 15.f * u + i * rowH), rb(b.x - 90.f * u, ra.y + rowH - 8.f * u);
			const bool tick = i < done;
			dl->AddRectFilled(ra, rb, col(theme::mix(pal.card, pal.accent, tick ? 0.18f : 0.05f)), 8.f);
			dl->AddRectFilled(ra, ImVec2(ra.x + 5.f * u, rb.y), col(tick ? pal.accent : pal.textDim, tick ? 1.f : 0.4f), 3.f);
			text(dl, ImVec2(ra.x + 18.f * u, (ra.y + rb.y) * 0.5f - ImGui::GetFontSize() * 0.5f), col(tick ? pal.text : pal.textDim), kBlocks[i]);
			const ImVec2 cc(rb.x - 22.f * u, (ra.y + rb.y) * 0.5f);
			dl->AddCircle(cc, 10.f * u, col(pal.textDim, 0.6f), 16, 1.5f);
			if (tick)
			{
				dl->AddCircleFilled(cc, 10.f * u, col(pal.accent), 16);
				dl->AddLine(ImVec2(cc.x - 5.f * u, cc.y), ImVec2(cc.x - 1.f * u, cc.y + 4.f * u), IM_COL32(10, 14, 20, 255), 2.f);
				dl->AddLine(ImVec2(cc.x - 1.f * u, cc.y + 4.f * u), ImVec2(cc.x + 6.f * u, cc.y - 4.f * u), IM_COL32(10, 14, 20, 255), 2.f);
			}
		}
	}

	// Ten squares filling in, then the day's score with a lock: one attempt counts.
	void daily(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		static const ImU32 kMarks[] = {kGreen, kYellow, kGreen, kMiss, kGreen, kGreen, kYellow, kMiss, kGreen, kGreen};
		const float sq = std::min(34.f * u, (b.x - a.x - 120.f * u) / 10.f - 8.f * u);
		const float total = 10 * sq + 9 * 8.f * u;
		const float x0 = (a.x + b.x - total) * 0.5f, y0 = a.y + (b.y - a.y) * 0.32f;
		const float phase = std::fmod(t, 6.f);
		const int shown = std::min(10, static_cast<int>(phase / 0.35f));
		for (int i = 0; i < 10; ++i)
		{
			const ImVec2 p(x0 + i * (sq + 8.f * u), y0);
			if (i < shown)
			{
				const float pop = ease((phase - i * 0.35f) * 4.f);
				const float d = sq * 0.5f * (1.f - pop);
				dl->AddRectFilled(ImVec2(p.x + d, p.y + d), ImVec2(p.x + sq - d, p.y + sq - d), kMarks[i], 5.f);
			}
			else
				dl->AddRect(p, ImVec2(p.x + sq, p.y + sq), col(pal.textDim, 0.4f), 5.f, ImDrawCornerFlags_All, 1.5f);
		}
		textCentered(dl, ImVec2((a.x + b.x) * 0.5f, a.y + (b.y - a.y) * 0.15f), col(pal.textDim), "DAILY #11", 0.9f);
		if (shown == 10)
		{
			const float in = ease((phase - 3.6f) * 2.f);
			char buf[32];
			snprintf(buf, sizeof(buf), "7/10   1,040");
			textCentered(dl, ImVec2((a.x + b.x) * 0.5f, y0 + sq + 34.f * u), col(pal.text, in), buf, 1.5f);
			// A lock.
			const ImVec2 lc((a.x + b.x) * 0.5f, y0 + sq + 80.f * u);
			dl->AddRectFilled(ImVec2(lc.x - 12.f * u, lc.y - 4.f * u), ImVec2(lc.x + 12.f * u, lc.y + 14.f * u), col(pal.accent, in), 3.f);
			dl->PathArcTo(ImVec2(lc.x, lc.y - 4.f * u), 8.f * u, kPi, 2.f * kPi, 12);
			dl->PathStroke(col(pal.accent, in), false, 3.f * u);
			textCentered(dl, ImVec2(lc.x, lc.y + 30.f * u), col(pal.textDim, in), "locked in for today", 0.85f);
		}
	}

	// A race: you and an orange ghost side by side to the finish line.
	void ghost(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 la(a.x + 40.f * u, a.y + 40.f * u), lb(b.x - 40.f * u, b.y - 40.f * u);
		dl->AddRectFilled(la, lb, IM_COL32(40, 44, 54, 255), 10.f);
		const float midY = (la.y + lb.y) * 0.5f;
		dl->AddLine(ImVec2(la.x + 10.f * u, midY), ImVec2(lb.x - 10.f * u, midY), IM_COL32(220, 230, 240, 60), 2.f);
		// The finish: a checkered strip.
		const float fx = lb.x - 50.f * u;
		for (int i = 0; i < 8; ++i)
			for (int j = 0; j < 2; ++j)
				if ((i + j) % 2 == 0)
				{
					const float h = (lb.y - la.y) / 8.f;
					dl->AddRectFilled(ImVec2(fx + j * 8.f * u, la.y + i * h), ImVec2(fx + (j + 1) * 8.f * u, la.y + (i + 1) * h), IM_COL32(230, 235, 245, 200));
				}
		const float run = loop(t, 3.4f);
		const float span = fx - la.x - 40.f * u;
		const float ghostX = la.x + 30.f * u + span * ease(run * 1.25f);
		const float youX = la.x + 30.f * u + span * ease(run * 1.18f);
		// The ghost: an outline with a trail and its name.
		const float gy = midY - (lb.y - la.y) * 0.22f, yy = midY + (lb.y - la.y) * 0.22f;
		for (int k = 1; k <= 8; ++k)
			dl->AddCircleFilled(ImVec2(ghostX - k * 9.f * u, gy), 3.f * u, IM_COL32(255, 170, 90, 120 - k * 14), 8);
		car(dl, ImVec2(ghostX, gy), 16.f * u, 0.f, IM_COL32(255, 170, 90, 70), kGhost);
		dl->AddRect(ImVec2(ghostX - 16.f * u, gy - 9.f * u), ImVec2(ghostX + 17.f * u, gy + 9.f * u), kGhost, 5.f, ImDrawCornerFlags_All, 1.5f);
		textCentered(dl, ImVec2(ghostX, gy - 22.f * u), kGhost, "AMY 2.31", 0.8f);
		car(dl, ImVec2(youX, yy), 16.f * u, 0.f, IM_COL32(70, 130, 230, 255), IM_COL32(220, 235, 255, 255));
		textCentered(dl, ImVec2(youX, yy + 22.f * u), col(pal.text), "YOU", 0.8f);
	}

	// A leaderboard: rows slide in, #1 gold, your row lit.
	void board(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		static const struct { const char* name; const char* value; bool me; } kRows[] = {
			{"Amy", "1,240", false}, {"You", "1,040", true}, {"Kuxir", "980", false}, {"bob_rl", "610", false}};
		const float rowH = (b.y - a.y - 24.f * u) / 4.f;
		const float phase = std::fmod(t, 5.f);
		for (int i = 0; i < 4; ++i)
		{
			const float in = ease((phase - i * 0.25f) * 2.f);
			const float off = (1.f - in) * 60.f * u;
			const ImVec2 ra(a.x + 70.f * u + off, a.y + 12.f * u + i * rowH), rb(b.x - 70.f * u + off, ra.y + rowH - 8.f * u);
			dl->AddRectFilled(ra, rb, col(kRows[i].me ? theme::mix(pal.card, pal.accent, 0.2f) : theme::mix(pal.bg, pal.card, 0.7f), in), 8.f);
			char rank[8];
			snprintf(rank, sizeof(rank), "#%d", i + 1);
			const float ty = (ra.y + rb.y) * 0.5f - ImGui::GetFontSize() * 0.5f;
			text(dl, ImVec2(ra.x + 14.f * u, ty), i == 0 ? kGold : col(kRows[i].me ? pal.accent : pal.textDim, in), rank);
			text(dl, ImVec2(ra.x + 60.f * u, ty), col(pal.text, in), kRows[i].name);
			const ImVec2 vs = ImGui::CalcTextSize(kRows[i].value);
			text(dl, ImVec2(rb.x - 14.f * u - vs.x, ty), col(pal.text, in), kRows[i].value);
		}
	}

	// Any training pack: its code typed in, its shots become your drills, then a defender in the net and the tiles you
	// must score in (Lambourne 2026-10-29: "a big mention that you can download ANY training pack").
	void packs(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const float phase = std::fmod(t, 8.f); // the finished picture holds a couple of seconds
		const float w = b.x - a.x, h = b.y - a.y;
		const float midY = a.y + h * 0.48f;
		// 1. The code, typed in, and Import.
		const char* code = "A503-264C-A7EB-D282";
		const int typed = std::min(19, static_cast<int>(phase / 0.07f));
		const ImVec2 ia(a.x + 18.f * u, midY - 40.f * u), ib(a.x + w * 0.3f, midY - 10.f * u);
		dl->AddRectFilled(ia, ib, col(pal.bg), 6.f);
		dl->AddRect(ia, ib, col(pal.accent, phase < 1.6f ? 0.9f : 0.35f), 6.f, ImDrawCornerFlags_All, 1.5f);
		const std::string shown(code, typed);
		text(dl, ImVec2(ia.x + 8.f * u, (ia.y + ib.y) * 0.5f - ImGui::GetFontSize() * 0.42f), col(pal.text), shown.c_str(), 0.85f);
		const float caretX = ia.x + 8.f * u + ImGui::CalcTextSize(shown.c_str()).x * 0.85f + 2.f;
		if (phase < 1.6f && std::fmod(t, 0.6f) < 0.35f)
			dl->AddLine(ImVec2(caretX, ia.y + 6.f * u), ImVec2(caretX, ib.y - 6.f * u), col(pal.text), 1.5f);
		const ImVec2 pa(ia.x, ib.y + 8.f * u), pb(ib.x, ib.y + 36.f * u);
		const bool pressed = phase > 1.5f && phase < 1.9f;
		dl->AddRectFilled(pa, pb, col(pal.accent, pressed ? 1.f : 0.85f), 14.f * u);
		textCentered(dl, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f), IM_COL32(10, 14, 20, 255), "Import", 0.9f);
		textCentered(dl, ImVec2((ia.x + ib.x) * 0.5f, ia.y - 14.f * u), col(pal.textDim), "ANY TRAINING PACK", 0.75f);
		// 2. Its shots fan out as drills.
		const float fan = ease((phase - 1.8f) * 1.3f);
		const ImVec2 dc(a.x + w * 0.47f, midY);
		for (int i = 4; i >= 0; --i)
		{
			const float off = i * 9.f * u * fan;
			const ImVec2 ca(dc.x - 46.f * u + off, dc.y - 34.f * u - off * 0.6f), cb(dc.x + 46.f * u + off, dc.y + 34.f * u - off * 0.6f);
			dl->AddRectFilled(ca, cb, col(theme::mix(pal.card, pal.accent, i == 0 ? 0.25f : 0.08f), phase > 1.8f ? 1.f : 0.25f), 8.f);
			dl->AddRect(ca, cb, col(pal.textDim, 0.35f), 8.f, ImDrawCornerFlags_All, 1.f);
			if (i == 0 && phase > 1.8f)
			{
				icons::draw(dl, Icon::Packs, ImVec2(ca.x + 18.f * u, ca.y + 18.f * u), 16.f * u, col(pal.accent));
				text(dl, ImVec2(ca.x + 10.f * u, cb.y - ImGui::GetFontSize() - 6.f * u), col(pal.text), "Shot 1 of 10", 0.8f);
			}
		}
		if (phase > 1.8f)
			textCentered(dl, ImVec2(dc.x + 18.f * u, dc.y + 58.f * u), col(pal.textDim, fan), "10 drills", 0.85f);
		// Arrows between the steps.
		const auto arrow = [&](float x0, float x1, float on) {
			dl->AddLine(ImVec2(x0, midY), ImVec2(x1 - 6.f * u, midY), col(pal.accent, 0.25f + 0.75f * on), 2.f);
			dl->AddTriangleFilled(ImVec2(x1, midY), ImVec2(x1 - 8.f * u, midY - 5.f * u), ImVec2(x1 - 8.f * u, midY + 5.f * u),
				col(pal.accent, 0.25f + 0.75f * on));
		};
		arrow(ib.x + 8.f * u, dc.x - 56.f * u, fan);
		const float setup = ease((phase - 3.0f) * 1.5f);
		arrow(dc.x + 90.f * u, a.x + w * 0.66f, setup);
		// 3. The net: two gold tiles open, a defender slides in, the ball goes into a gold one.
		const float nw = w * 0.3f, nh = nw * 643.f / 1786.f * 1.25f;
		const ImVec2 np(b.x - nw - 18.f * u, midY - nh * 0.5f - 10.f * u);
		const int cols = 4;
		const float cw = nw / cols, ch = nh / 2.f;
		dl->AddRectFilled(np, ImVec2(np.x + nw, np.y + nh), IM_COL32(6, 9, 16, 210), 5.f);
		for (int r = 0; r < 2; ++r)
			for (int j = 0; j < cols; ++j)
			{
				const bool open = (r == 0 && j == 0) || (r == 0 && j == 3);
				const ImVec2 ca(np.x + j * cw + 2.f, np.y + r * ch + 2.f), cb(np.x + (j + 1) * cw - 2.f, np.y + (r + 1) * ch - 2.f);
				if (open && setup > 0.f)
				{
					dl->AddRectFilled(ca, cb, IM_COL32(255, 205, 60, static_cast<int>(95 * setup)), 3.f);
					dl->AddRect(ca, cb, IM_COL32(255, 205, 60, static_cast<int>(255 * setup)), 3.f, ImDrawCornerFlags_All, 2.f);
				}
				else
					dl->AddRectFilled(ca, cb, IM_COL32(120, 126, 136, static_cast<int>(60 + 110 * setup)), 3.f);
			}
		dl->AddLine(ImVec2(np.x, np.y + nh), np, IM_COL32(255, 255, 255, 200), 2.5f);
		dl->AddLine(np, ImVec2(np.x + nw, np.y), IM_COL32(255, 255, 255, 200), 2.5f);
		dl->AddLine(ImVec2(np.x + nw, np.y), ImVec2(np.x + nw, np.y + nh), IM_COL32(255, 255, 255, 200), 2.5f);
		// The defender: an orange car sliding in under the bar.
		const float slide = ease((phase - 3.3f) * 1.4f);
		const ImVec2 carAt(np.x + nw * (1.3f - 0.8f * slide), np.y + nh + 18.f * u);
		if (phase > 3.3f)
		{
			car(dl, carAt, 18.f * u, kPi, IM_COL32(240, 140, 50, 255), IM_COL32(255, 230, 200, 255));
			textCentered(dl, ImVec2(carAt.x, carAt.y + 24.f * u), kGhost, "DEFENDER", 0.75f);
		}
		// The ball into the top-left gold tile.
		const float shot = (phase - 4.6f) / 1.0f;
		if (shot > 0.f)
		{
			const ImVec2 to(np.x + 0.5f * cw, np.y + 0.5f * ch);
			const ImVec2 from(np.x + nw * 0.5f, b.y - 6.f * u);
			if (shot < 1.f)
				ball(dl, lerp(from, to, ease(shot)), (12.f - 5.f * ease(shot)) * u);
			else
				textCentered(dl, to, IM_COL32(255, 255, 255, 255), "GOAL", 0.85f); // in the middle of its tile
		}
		textCentered(dl, ImVec2(np.x + nw * 0.5f, np.y - 14.f * u), col(pal.textDim), "YOUR SETUP", 0.75f);
	}

	// Recording: the ball's path over a timeline; the handle picks where the drill starts.
	void record(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 ta(a.x + 60.f * u, b.y - 60.f * u), tb(b.x - 60.f * u, b.y - 30.f * u);
		const float handle = 0.25f + 0.4f * (0.5f + 0.5f * std::sin(t * 1.2f));
		const float hx = ta.x + (tb.x - ta.x) * handle;
		// The path above: a few bounces.
		ImVec2 prev;
		for (int i = 0; i <= 60; ++i)
		{
			const float k = i / 60.f;
			const float x = ta.x + (tb.x - ta.x) * k;
			const float y = ta.y - 20.f * u - std::fabs(std::sin(k * kPi * 2.2f)) * (ta.y - a.y - 50.f * u) * (1.f - k * 0.35f);
			const ImVec2 p(x, y);
			if (i > 0)
				dl->AddLine(prev, p, k <= handle ? IM_COL32(255, 255, 255, 230) : IM_COL32(255, 255, 255, 90), k <= handle ? 3.f : 2.f);
			prev = p;
			if (std::fabs(k - handle) < 1.f / 120.f)
			{
				dl->AddCircle(p, 13.f * u, kGold, 24, 2.5f);
				ball(dl, p, 7.f * u);
			}
		}
		dl->AddRectFilled(ta, tb, col(pal.bg), 8.f);
		dl->AddRectFilled(ta, ImVec2(hx, tb.y), col(pal.accent, 0.35f), 8.f, ImDrawCornerFlags_Left);
		dl->AddLine(ImVec2(hx, ta.y - 6.f * u), ImVec2(hx, tb.y + 6.f * u), kGold, 3.f);
		dl->AddCircleFilled(ImVec2(hx, ta.y - 6.f * u), 6.f * u, kGold, 12);
		text(dl, ImVec2(ta.x + 10.f * u, ta.y + 7.f * u), col(pal.text, 0.9f), "REPLAYS", 0.8f);
		text(dl, ImVec2(hx + 10.f * u, ta.y + 7.f * u), col(pal.textDim), "YOU PLAY", 0.8f);
		// The record button, pulsing.
		const float pulse = 0.5f + 0.5f * std::sin(t * 4.f);
		dl->AddCircle(ImVec2(a.x + 30.f * u, a.y + 30.f * u), 14.f * u, IM_COL32(240, 70, 76, static_cast<int>(120 + 120 * pulse)), 24, 2.f);
		dl->AddCircleFilled(ImVec2(a.x + 30.f * u, a.y + 30.f * u), 9.f * u, IM_COL32(240, 70, 76, 255), 20);
	}

	// The net from the field: gold tiles open, grey ones closed; a ball goes into a gold one.
	void net(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const float w = std::min(b.x - a.x - 120.f * u, (b.y - a.y - 40.f * u) * 1786.f / 643.f);
		const float h = w * 643.f / 1786.f;
		const ImVec2 p((a.x + b.x - w) * 0.5f, a.y + 16.f * u);
		const int cols = 4, rows = 2;
		const int phase = static_cast<int>(t / 2.4f) % 3;
		const unsigned openMask = phase == 0 ? 0b10000001u : phase == 1 ? 0b00000110u : 0b11000000u;
		const float cw = w / cols, ch = h / rows;
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(6, 9, 16, 210), 6.f);
		int target = 0;
		for (int i = 0; i < cols * rows; ++i)
			if (openMask & (1u << i))
				target = i;
		for (int r = 0; r < rows; ++r)
			for (int j = 0; j < cols; ++j)
			{
				const int cell = r * cols + j;
				const ImVec2 ca(p.x + j * cw + 3.f, p.y + r * ch + 3.f), cb(p.x + (j + 1) * cw - 3.f, p.y + (r + 1) * ch - 3.f);
				if (openMask & (1u << cell))
				{
					dl->AddRectFilled(ca, cb, IM_COL32(255, 205, 60, 95), 4.f);
					dl->AddRect(ca, cb, kGold, 4.f, ImDrawCornerFlags_All, 2.5f);
				}
				else
					dl->AddRectFilled(ca, cb, IM_COL32(120, 126, 136, 170), 4.f);
			}
		dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x, p.y), IM_COL32(255, 255, 255, 200), 3.f);
		dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), IM_COL32(255, 255, 255, 200), 3.f);
		dl->AddLine(ImVec2(p.x + w, p.y), ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 200), 3.f);
		// The ball flies from below into the target tile and shrinks into the net.
		const float k = loop(t, 2.4f);
		const ImVec2 to(p.x + (target % cols + 0.5f) * cw, p.y + (target / cols + 0.5f) * ch);
		const ImVec2 from((a.x + b.x) * 0.5f, b.y - 10.f * u);
		const float f = ease(k * 1.4f);
		if (k < 0.8f)
			ball(dl, lerp(from, to, f), (16.f - 8.f * f) * u);
		else
			textCentered(dl, ImVec2(to.x, to.y), IM_COL32(255, 255, 255, 255), "GOAL", 0.9f);
	}

	// A drill card and its three ways out: a file for Discord, a friend, the Shot Board.
	void share(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 c(a.x + (b.x - a.x) * 0.25f, (a.y + b.y) * 0.5f);
		panel(dl, ImVec2(c.x - 80.f * u, c.y - 34.f * u), ImVec2(c.x + 80.f * u, c.y + 34.f * u), 10.f);
		text(dl, ImVec2(c.x - 66.f * u, c.y - 22.f * u), col(pal.text), "my wall pop");
		text(dl, ImVec2(c.x - 66.f * u, c.y + 2.f * u), col(pal.accent), "vs Nexto", 0.8f);
		static const struct { const char* label; Icon icon; } kOut[] = {{"Discord (file)", Icon::Bookmark}, {"A friend", Icon::Friends}, {"Shot Board", Icon::Star}};
		const int lit = static_cast<int>(t / 1.3f) % 3;
		for (int i = 0; i < 3; ++i)
		{
			const ImVec2 o(a.x + (b.x - a.x) * 0.72f, a.y + (b.y - a.y) * (0.2f + 0.3f * i));
			const bool on = i == lit;
			dl->AddLine(ImVec2(c.x + 80.f * u, c.y), ImVec2(o.x - 30.f * u, o.y), col(on ? pal.accent : pal.textDim, on ? 1.f : 0.3f), on ? 2.5f : 1.5f);
			if (on)
			{
				const float k = loop(t, 1.3f);
				dl->AddCircleFilled(lerp(ImVec2(c.x + 80.f * u, c.y), ImVec2(o.x - 30.f * u, o.y), ease(k * 1.4f)), 5.f * u, col(pal.accent), 12);
			}
			dl->AddCircleFilled(o, 22.f * u, col(theme::mix(pal.card, pal.accent, on ? 0.35f : 0.08f)), 24);
			if (kOut[i].icon == Icon::Star)
				icons::star(dl, o, 11.f * u, on ? kGold : col(pal.textDim), true);
			else
				icons::draw(dl, kOut[i].icon, o, 20.f * u, col(on ? pal.text : pal.textDim));
			text(dl, ImVec2(o.x + 30.f * u, o.y - ImGui::GetFontSize() * 0.5f), col(on ? pal.text : pal.textDim), kOut[i].label);
		}
	}

	// Two players: a friend code travels across, then they're linked.
	void friends(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 l(a.x + (b.x - a.x) * 0.25f, (a.y + b.y) * 0.5f - 10.f * u), r(a.x + (b.x - a.x) * 0.75f, l.y);
		const auto person = [&](ImVec2 c, ImU32 colr, const char* name) {
			dl->AddCircleFilled(ImVec2(c.x, c.y - 18.f * u), 15.f * u, colr, 24);
			dl->PathArcTo(ImVec2(c.x, c.y + 26.f * u), 26.f * u, kPi, 2.f * kPi, 20);
			dl->PathFillConvex(colr);
			textCentered(dl, ImVec2(c.x, c.y + 44.f * u), col(pal.text), name);
		};
		const float k = loop(t, 3.f);
		const bool linked = k > 0.62f;
		dl->AddLine(l, r, col(linked ? pal.accent : pal.textDim, linked ? 1.f : 0.25f), linked ? 3.f : 1.5f);
		person(l, IM_COL32(70, 130, 230, 255), "You");
		person(r, IM_COL32(240, 140, 50, 255), "Amy");
		if (!linked)
		{
			const ImVec2 p = lerp(ImVec2(l.x + 30.f * u, l.y - 40.f * u), ImVec2(r.x - 30.f * u, r.y - 40.f * u), ease(k / 0.6f));
			const char* code = "7F3A9C21";
			const ImVec2 ts = ImGui::CalcTextSize(code);
			dl->AddRectFilled(ImVec2(p.x - ts.x * 0.5f - 10.f * u, p.y - ts.y * 0.5f - 6.f * u), ImVec2(p.x + ts.x * 0.5f + 10.f * u, p.y + ts.y * 0.5f + 6.f * u),
				col(pal.accent), 10.f);
			textCentered(dl, p, IM_COL32(10, 14, 20, 255), code);
		}
		else
		{
			const ImVec2 m((l.x + r.x) * 0.5f, l.y);
			dl->AddCircleFilled(m, 14.f * u, col(pal.accent), 20);
			dl->AddLine(ImVec2(m.x - 6.f * u, m.y), ImVec2(m.x - 1.f * u, m.y + 5.f * u), IM_COL32(10, 14, 20, 255), 2.5f);
			dl->AddLine(ImVec2(m.x - 1.f * u, m.y + 5.f * u), ImVec2(m.x + 7.f * u, m.y - 5.f * u), IM_COL32(10, 14, 20, 255), 2.5f);
		}
	}

	// Bars growing week by week, and a line over them.
	void progress(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		static const float kH[] = {0.25f, 0.35f, 0.3f, 0.5f, 0.55f, 0.7f, 0.85f};
		const float grow = ease(std::fmod(t, 4.f) * 0.8f);
		const float bw = (b.x - a.x - 160.f * u) / 7.f;
		const float base = b.y - 26.f * u;
		ImVec2 prev;
		for (int i = 0; i < 7; ++i)
		{
			const float x = a.x + 80.f * u + i * bw;
			const float h = (base - a.y - 30.f * u) * kH[i] * grow;
			dl->AddRectFilled(ImVec2(x + 6.f * u, base - h), ImVec2(x + bw - 6.f * u, base), col(pal.accent, 0.35f + 0.08f * i), 6.f);
			const ImVec2 p(x + bw * 0.5f, base - h - 10.f * u);
			if (i > 0)
				dl->AddLine(prev, p, kGold, 2.5f);
			dl->AddCircleFilled(p, 4.f * u, kGold, 12);
			prev = p;
		}
		dl->AddLine(ImVec2(a.x + 70.f * u, base), ImVec2(b.x - 70.f * u, base), col(pal.textDim, 0.4f), 1.5f);
	}

	// A gear turning and switches flipping.
	void settings(ImDrawList* dl, ImVec2 a, ImVec2 b, float t, float u)
	{
		const theme::Palette& pal = theme::palette();
		const ImVec2 g(a.x + (b.x - a.x) * 0.28f, (a.y + b.y) * 0.5f);
		const float rr = std::min(b.y - a.y, b.x - a.x) * 0.22f;
		for (int i = 0; i < 8; ++i)
		{
			const float ang = t * 0.8f + i * kPi / 4.f;
			const ImVec2 d(std::cos(ang), std::sin(ang));
			dl->AddLine(ImVec2(g.x + d.x * rr * 0.8f, g.y + d.y * rr * 0.8f), ImVec2(g.x + d.x * rr * 1.18f, g.y + d.y * rr * 1.18f),
				col(pal.accent), rr * 0.28f);
		}
		dl->AddCircleFilled(g, rr * 0.9f, col(pal.accent), 32);
		dl->AddCircleFilled(g, rr * 0.38f, col(pal.bg), 24);
		static const char* kRows[] = {"Account", "Controls", "Sounds"};
		for (int i = 0; i < 3; ++i)
		{
			const ImVec2 p(a.x + (b.x - a.x) * 0.52f, a.y + (b.y - a.y) * (0.25f + 0.25f * i));
			text(dl, ImVec2(p.x, p.y - ImGui::GetFontSize() * 0.5f), col(pal.text), kRows[i]);
			const bool on = std::fmod(t + i * 0.7f, 2.4f) > 1.2f;
			const ImVec2 s0(b.x - 110.f * u, p.y - 11.f * u), s1(b.x - 66.f * u, p.y + 11.f * u);
			dl->AddRectFilled(s0, s1, col(on ? pal.accent : pal.textDim, on ? 1.f : 0.4f), 11.f * u);
			dl->AddCircleFilled(ImVec2(on ? s1.x - 11.f * u : s0.x + 11.f * u, p.y), 8.f * u, IM_COL32(245, 248, 255, 255), 16);
		}
	}
}

void tutorial::drawScene(ImDrawList* dl, const std::string& scene, ImVec2 a, ImVec2 b, float t, float u)
{
	const theme::Palette& pal = theme::palette();
	dl->AddRectFilled(a, b, col(theme::mix(pal.bg, pal.card, 0.5f)), 12.f);
	dl->PushClipRect(a, b, true);
	if (scene == "hubs") hubs(dl, a, b, t, u);
	else if (scene == "tabs") tabs(dl, a, b, t, u);
	else if (scene == "controller") controller(dl, a, b, t, u);
	else if (scene == "difficulty") difficulty(dl, a, b, t, u);
	else if (scene == "freeplay") freeplay(dl, a, b, t, u);
	else if (scene == "routine") routine(dl, a, b, t, u);
	else if (scene == "daily") daily(dl, a, b, t, u);
	else if (scene == "ghost") ghost(dl, a, b, t, u);
	else if (scene == "board") board(dl, a, b, t, u);
	else if (scene == "packs") packs(dl, a, b, t, u);
	else if (scene == "record") record(dl, a, b, t, u);
	else if (scene == "net") net(dl, a, b, t, u);
	else if (scene == "share") share(dl, a, b, t, u);
	else if (scene == "friends") friends(dl, a, b, t, u);
	else if (scene == "progress") progress(dl, a, b, t, u);
	else if (scene == "settings") settings(dl, a, b, t, u);
	else
		icons::draw(dl, Icon::Star, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), 60.f * u, col(pal.accent));
	dl->PopClipRect();
}
