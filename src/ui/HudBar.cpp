#include "pch.h"
#include "ui/HudBar.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hudbar
{
	namespace
	{
		const Rgba kText{236, 240, 245, 255};
		const Rgba kDim{150, 160, 172, 255};

		struct Info { const char* id; const char* name; const char* about; };
		constexpr Info kInfo[kStyles] = {
			{"classic", "Classic", "The full strip: every number in its own column."},
			{"scoreboard", "Scoreboard", "A match scoreboard: the clock in the middle, the rep and streak either side."},
			{"neon", "Neon", "See-through, with glowing lines and corner brackets."},
			{"pill", "Pill", "One slim line. All the numbers, very little screen."},
			{"minimal", "Minimal", "Just the clock and a small line, with no panel at all."},
			{"side", "Side card", "A card at the left, so the top of the screen stays clear."},
		};

		uint8_t faded(int alpha, float opacity) { return static_cast<uint8_t>(std::clamp(alpha * opacity, 0.f, 255.f)); }

		// The scale `text` is drawn at so it is no wider than `maxWidth`.
		float fit(Canvas& c, const std::string& text, float scale, float maxWidth)
		{
			float w = 0, h = 0;
			c.textSize(text, scale, w, h);
			return w > maxWidth && w > 0.f ? scale * maxWidth / w : scale;
		}

		float widthOf(Canvas& c, const std::string& text, float scale)
		{
			float w = 0, h = 0;
			c.textSize(text, scale, w, h);
			return w;
		}

		void centred(Canvas& c, float cx, float y, const std::string& text, float scale, Rgba color, bool shadow = true)
		{
			c.text(cx - widthOf(c, text, scale) * 0.5f, y, text, scale, color, shadow);
		}

		// A column of the strip: its label, and the value kept inside it (smaller first, cut if still too long).
		void column(Canvas& c, float x, float y, float width, const Cell& cell, float z, Rgba labelColor)
		{
			c.text(x, y + 9.f * z, cell.label, 0.85f * z, labelColor, false);
			std::string value = cell.value;
			const float scale = std::max(1.f * z, fit(c, value, 1.45f * z, width - 8.f * z));
			while (value.size() > 4 && widthOf(c, value, scale) > width - 8.f * z)
				value = value.substr(0, value.size() - 4) + "...";
			c.text(x, y + 26.f * z + (1.45f * z - scale) * 8.f, value, scale, cell.color);
		}

		// The columns shown, with their shares of the strip's width.
		struct Column { const Cell* cell; float share; };
		std::vector<Column> columns(const Bar& bar, const Options& o)
		{
			std::vector<Column> out;
			if (o.showName)
				out.push_back({&bar.exercise, 2.6f});
			out.push_back({&bar.rep, 0.8f});
			out.push_back({&bar.time, 1.3f});
			out.push_back({&bar.stat, 0.9f});
			if (o.showLevel)
				out.push_back({&bar.level, 1.f});
			return out;
		}

		float strip(Canvas& c, const Bar& bar, const Options& o, float z, float y, float h, Rgba labelColor, float& x)
		{
			const auto cols = columns(bar, o);
			float total = 0.f;
			for (const auto& col : cols)
				total += col.share;
			const float w = std::min(820.f * z * total / 6.6f, c.width() - 16.f * z);
			x = (c.width() - w) * 0.5f;
			const float pad = 18.f * z, usable = w - 2.f * pad;
			float cx = x + pad;
			for (size_t i = 0; i < cols.size(); ++i)
			{
				const float cw = usable * cols[i].share / total;
				column(c, cx, y, cw, *cols[i].cell, z, labelColor);
				if (i + 1 < cols.size())
					c.fillRect(cx + cw - 10.f * z, y + 12.f * z, 1.f, h - 24.f * z, Rgba{255, 255, 255, 30});
				cx += cw;
			}
			return w;
		}

		Placed classic(Canvas& c, const Bar& bar, const Options& o, float z, Rgba accent)
		{
			const float y = 16.f * z, h = 64.f * z;
			const auto cols = columns(bar, o);
			float total = 0.f;
			for (const auto& col : cols)
				total += col.share;
			const float w = std::min(820.f * z * total / 6.6f, c.width() - 16.f * z);
			const float x = (c.width() - w) * 0.5f;
			c.fillRect(x, y, w, h, Rgba{12, 15, 21, faded(205, o.opacity)});
			c.fillRect(x, y + h - 3.f * z, w, 3.f * z, accent);
			float unused = 0.f;
			strip(c, bar, o, z, y, h, kDim, unused);
			return {y + h + 6.f * z, x + w + 14.f * z, y + 18.f * z};
		}

		Placed scoreboard(Canvas& c, const Bar& bar, const Options& o, float z, Rgba accent)
		{
			// Blue and orange either side of the clock, like the game's own scoreboard.
			const float mid = c.width() * 0.5f, y = 14.f * z, h = 62.f * z;
			const float clockW = 196.f * z, sideW = 148.f * z, gap = 4.f * z;
			const float clockX = mid - clockW * 0.5f;
			const float leftX = clockX - gap - sideW, rightX = clockX + clockW + gap;
			c.fillRect(leftX, y, sideW, h, Rgba{26, 74, 150, faded(225, o.opacity)});
			c.fillRect(rightX, y, sideW, h, Rgba{176, 92, 24, faded(225, o.opacity)});
			c.fillRect(clockX, y, clockW, h, Rgba{10, 13, 20, faded(235, o.opacity)});
			c.fillRect(clockX, y + h - 3.f * z, clockW, 3.f * z, accent);
			const Rgba white{255, 255, 255, 255}, soft{255, 255, 255, 190};
			centred(c, leftX + sideW * 0.5f, y + 8.f * z, bar.rep.label, 0.85f * z, soft, false);
			centred(c, leftX + sideW * 0.5f, y + 24.f * z, bar.rep.value, fit(c, bar.rep.value, 1.9f * z, sideW - 16.f * z), white);
			centred(c, rightX + sideW * 0.5f, y + 8.f * z, bar.stat.label, 0.85f * z, soft, false);
			centred(c, rightX + sideW * 0.5f, y + 24.f * z, bar.stat.value, fit(c, bar.stat.value, 1.9f * z, sideW - 16.f * z), white);
			const float clockScale = fit(c, bar.time.value, 2.5f * z, clockW - 20.f * z);
			float tw = 0, th = 0;
			c.textSize(bar.time.value, clockScale, tw, th);
			c.text(mid - tw * 0.5f, y + (h - 3.f * z - th) * 0.5f, bar.time.value, clockScale, bar.time.color);
			float under = y + h;
			// The drill and its level in a thin line underneath.
			std::string line = o.showName ? bar.exercise.value : "";
			if (o.showLevel)
				line += (line.empty() ? "" : "   |   ") + bar.level.value;
			if (!line.empty())
			{
				const float fullW = sideW * 2.f + clockW + gap * 2.f;
				const float s = fit(c, line, 0.95f * z, fullW - 24.f * z);
				float lw = 0, lh = 0;
				c.textSize(line, s, lw, lh);
				c.fillRect(leftX, under, fullW, lh + 10.f * z, Rgba{10, 13, 20, faded(190, o.opacity)});
				c.text(mid - lw * 0.5f, under + 5.f * z, line, s, kDim, false);
				under += lh + 10.f * z;
			}
			return {under + 6.f * z, rightX + sideW + 14.f * z, y + 18.f * z};
		}

		Placed neon(Canvas& c, const Bar& bar, const Options& o, float z, Rgba accent)
		{
			const float y = 16.f * z, h = 64.f * z;
			const auto cols = columns(bar, o);
			float total = 0.f;
			for (const auto& col : cols)
				total += col.share;
			const float w = std::min(820.f * z * total / 6.6f, c.width() - 16.f * z);
			const float x = (c.width() - w) * 0.5f;
			c.fillRect(x, y, w, h, Rgba{6, 10, 18, faded(90, o.opacity)});
			// The glow: wide faint lines under a bright thin one.
			for (const float edge : {y, y + h})
			{
				c.fillRect(x, edge - 3.f * z, w, 6.f * z, accent.withAlpha(0.10f));
				c.fillRect(x, edge - 1.5f * z, w, 3.f * z, accent.withAlpha(0.22f));
				c.fillRect(x, edge - 0.5f * z, w, std::max(1.f, 1.f * z), accent);
			}
			// Corner brackets.
			const float t = std::max(1.f, 2.f * z), arm = 16.f * z;
			for (const float bx : {x - 3.f * z - t, x + w + 3.f * z})
				c.fillRect(bx, y - 3.f * z, t, h + 6.f * z, accent.withAlpha(0.85f));
			c.fillRect(x - 3.f * z - t, y - 3.f * z, arm, t, accent);
			c.fillRect(x + w + 3.f * z + t - arm, y - 3.f * z, arm, t, accent);
			c.fillRect(x - 3.f * z - t, y + h + 3.f * z - t, arm, t, accent);
			c.fillRect(x + w + 3.f * z + t - arm, y + h + 3.f * z - t, arm, t, accent);
			float unused = 0.f;
			strip(c, bar, o, z, y, h, accent, unused);
			return {y + h + 10.f * z, x + w + 18.f * z, y + 18.f * z};
		}

		Placed pill(Canvas& c, const Bar& bar, const Options& o, float z, Rgba accent)
		{
			// "REP 12   4.9 s   STREAK 3   MEDIUM": label dim, value bright, a small dot between them.
			const float y = 16.f * z, h = 38.f * z, labelS = 0.9f * z, valueS = 1.2f * z;
			struct Piece { std::string label, value; Rgba color; };
			std::vector<Piece> pieces;
			if (o.showName)
				pieces.push_back({"", bar.exercise.value, bar.exercise.color});
			pieces.push_back({bar.rep.label, bar.rep.value, bar.rep.color});
			pieces.push_back({"", bar.time.value, bar.time.color});
			pieces.push_back({bar.stat.label, bar.stat.value, bar.stat.color});
			if (o.showLevel)
				pieces.push_back({"", bar.level.value, bar.level.color});
			const float dotGap = 16.f * z, inner = 7.f * z;
			const auto measure = [&](float shrink) {
				float total = 0.f;
				for (const auto& p : pieces)
					total += (p.label.empty() ? 0.f : widthOf(c, p.label, labelS * shrink) + inner) + widthOf(c, p.value, valueS * shrink);
				return total + (pieces.size() - 1) * dotGap;
			};
			const float maxW = std::min(1000.f * z, c.width() - 60.f * z);
			const float shrink = std::min(1.f, maxW / std::max(measure(1.f), 1.f));
			const float contentW = measure(shrink);
			const float w = contentW + 44.f * z, x = (c.width() - w) * 0.5f;
			c.fillRect(x, y, w, h, Rgba{12, 15, 21, faded(205, o.opacity)});
			c.fillRect(x, y, 4.f * z, h, accent);
			c.fillRect(x + w - 4.f * z, y, 4.f * z, h, accent);
			float cx = x + 22.f * z;
			for (size_t i = 0; i < pieces.size(); ++i)
			{
				const auto& p = pieces[i];
				float vw = 0, vh = 0;
				c.textSize(p.value, valueS * shrink, vw, vh);
				const float ty = y + (h - vh) * 0.5f;
				if (!p.label.empty())
				{
					c.text(cx, ty + 2.f * z, p.label, labelS * shrink, kDim, false);
					cx += widthOf(c, p.label, labelS * shrink) + inner;
				}
				c.text(cx, ty, p.value, valueS * shrink, p.color);
				cx += vw;
				if (i + 1 < pieces.size())
				{
					c.fillRect(cx + dotGap * 0.5f - 2.f * z, y + h * 0.5f - 2.f * z, 4.f * z, 4.f * z, accent.withAlpha(0.8f));
					cx += dotGap;
				}
			}
			return {y + h + 6.f * z, x + w + 14.f * z, y + 4.f * z};
		}

		Placed minimal(Canvas& c, const Bar& bar, const Options& o, float z, Rgba)
		{
			// No panel: the shadows keep the text readable.
			const float mid = c.width() * 0.5f;
			float y = 14.f * z;
			if (o.showName)
			{
				const float s = fit(c, bar.exercise.value, 1.f * z, c.width() * 0.7f);
				centred(c, mid, y, bar.exercise.value, s, kDim);
				y += 20.f * z;
			}
			float tw = 0, th = 0;
			const float clockS = 2.6f * z;
			c.textSize(bar.time.value, clockS, tw, th);
			c.text(mid - tw * 0.5f, y, bar.time.value, clockS, bar.time.color);
			y += th + 2.f * z;
			std::string line = bar.rep.label + " " + bar.rep.value + "     " + bar.stat.label + " " + bar.stat.value;
			if (o.showLevel)
				line += "     " + bar.level.value;
			centred(c, mid, y, line, 1.05f * z, kText);
			float lw = 0, lh = 0;
			c.textSize(line, 1.05f * z, lw, lh);
			return {y + lh + 8.f * z, mid + std::max(tw, lw) * 0.5f + 18.f * z, y - 26.f * z};
		}

		Placed side(Canvas& c, const Bar& bar, const Options& o, float z, Rgba accent)
		{
			const float x = 24.f * z, y = 150.f * z, w = 250.f * z, pad = 16.f * z, rowH = 30.f * z;
			std::vector<const Cell*> rows = {&bar.rep, &bar.time, &bar.stat};
			if (o.showLevel)
				rows.push_back(&bar.level);
			const float titleH = o.showName ? 34.f * z : 0.f;
			const float h = pad * 2.f + titleH + rows.size() * rowH - 6.f * z;
			c.fillRect(x, y, w, h, Rgba{12, 15, 21, faded(205, o.opacity)});
			c.fillRect(x, y, 4.f * z, h, accent);
			float cy = y + pad;
			if (o.showName)
			{
				c.text(x + pad, cy, bar.exercise.value, fit(c, bar.exercise.value, 1.2f * z, w - pad * 2.f), accent);
				cy += titleH;
			}
			for (const Cell* row : rows)
			{
				c.text(x + pad, cy + 3.f * z, row->label, 0.9f * z, kDim, false);
				const float s = fit(c, row->value, 1.35f * z, w * 0.55f);
				c.text(x + w - pad - widthOf(c, row->value, s), cy, row->value, s, row->color);
				cy += rowH;
			}
			// The top stays clear: lines start where the strip's would.
			return {16.f * z, x + w + 14.f * z, y};
		}
	}

	const char* styleId(int style) { return kInfo[std::clamp(style, 0, kStyles - 1)].id; }
	const char* styleName(int style) { return kInfo[std::clamp(style, 0, kStyles - 1)].name; }
	const char* styleAbout(int style) { return kInfo[std::clamp(style, 0, kStyles - 1)].about; }

	int styleFromId(const std::string& id)
	{
		for (int i = 0; i < kStyles; ++i)
			if (id == kInfo[i].id)
				return i;
		return Classic;
	}

	Placed draw(Canvas& c, const Bar& bar, const Options& options, float k, Rgba accent)
	{
		const float z = k * options.size;
		switch (options.style)
		{
		case Scoreboard: return scoreboard(c, bar, options, z, accent);
		case Neon: return neon(c, bar, options, z, accent);
		case Pill: return pill(c, bar, options, z, accent);
		case Minimal: return minimal(c, bar, options, z, accent);
		case Side: return side(c, bar, options, z, accent);
		default: return classic(c, bar, options, z, accent);
		}
	}
}
