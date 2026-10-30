#include "pch.h"
#include "ui/Hud.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "ui/Theme.h"

namespace
{
	constexpr float kFlashSeconds = 1.8f;

	const Rgba kPanel{12, 15, 21, 205};
	const Rgba kText{236, 240, 245, 255};
	const Rgba kDim{150, 160, 172, 255};
	const Rgba kFail{240, 80, 90, 255};

	Rgba accent()
	{
		const auto& a = theme::palette().accent;
		return {static_cast<uint8_t>(a.x * 255), static_cast<uint8_t>(a.y * 255), static_cast<uint8_t>(a.z * 255), 255};
	}

	void centered(Canvas& c, float cx, float y, const std::string& s, float scale, Rgba col)
	{
		float w = 0, h = 0;
		c.textSize(s, scale, w, h);
		c.text(cx - w * 0.5f, y, s, scale, col);
	}

	std::string upper(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
		return s;
	}

	// The scale `text` is drawn at so it is no wider than `maxWidth` (never bigger than `scale`).
	float fitScale(Canvas& c, const std::string& text, float scale, float maxWidth)
	{
		float w = 0, h = 0;
		c.textSize(text, scale, w, h);
		return w > maxWidth && w > 0.f ? scale * maxWidth / w : scale;
	}

	// `text` broken into lines no wider than `maxWidth` at `scale` (at word breaks), at most `maxLines` (the last one
	// ends in "..." when there is more).
	std::vector<std::string> wrapLines(Canvas& c, const std::string& text, float scale, float maxWidth, size_t maxLines)
	{
		std::vector<std::string> lines;
		std::string line, word;
		const auto width = [&](const std::string& s) {
			float w = 0, h = 0;
			c.textSize(s, scale, w, h);
			return w;
		};
		size_t i = 0;
		while (i <= text.size())
		{
			if (i == text.size() || text[i] == ' ')
			{
				const std::string next = line.empty() ? word : line + " " + word;
				if (!line.empty() && width(next) > maxWidth)
				{
					lines.push_back(line);
					line = word;
				}
				else
					line = next;
				word.clear();
			}
			else
				word += text[i];
			++i;
		}
		if (!line.empty())
			lines.push_back(line);
		if (lines.size() > maxLines)
		{
			lines.resize(maxLines);
			lines.back() += "...";
		}
		return lines;
	}
}

namespace
{
	std::string fmtSeconds(double seconds)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.2f s", seconds);
		return buf;
	}
}

void Hud::draw(Canvas& canvas, const ScenarioPlayer::Status& status, const SessionRunner::Status& session)
{
	// A real goal's slow motion and replay: nothing on top of it (the next shot's card covered the replay, Lambourne's
	// friend 2026-10-23). The result's flash is dropped rather than shown afterwards.
	if (status.goalReplay)
	{
		lastSeq_ = status.resultSeq;
		flashing_ = false;
		return;
	}
	if (status.resultSeq != lastSeq_)
	{
		lastSeq_ = status.resultSeq;
		if (status.hasResult)
		{
			flashing_ = true;
			flashStart_ = std::chrono::steady_clock::now();
			flash_ = status;
		}
	}
	if (!status.active)
	{
		flashing_ = false;
		return;
	}
	const float k = canvas.height() / 1080.f; // layout is designed at 1080p
	drawStrip(canvas, status, k);
	if (status.map.show)
		drawPlayMap(canvas, status.map, k);
	float lineY = 16.f * k + 64.f * k + 6.f * k; // first line under the strip
	if (session.active)
	{
		// Session line under the strip: name, time left, exercise n of m.
		char line[160];
		const int left = static_cast<int>(session.secondsLeft + 0.999);
		if (!session.line.empty())
			snprintf(line, sizeof(line), "%s", session.line.c_str()); // the Daily Challenge
		else
			snprintf(line, sizeof(line), "%s   |   %d:%02d LEFT   |   EXERCISE %d / %d", upper(session.name).c_str(),
				left / 60, left % 60, session.exerciseIndex, session.exerciseCount);
		float w = 0, h = 0;
		canvas.textSize(line, 1.05f * k, w, h);
		const float x = (canvas.width() - w) * 0.5f, y = 16.f * k + 64.f * k + 6.f * k;
		canvas.fillRect(x - 14.f * k, y - 4.f * k, w + 28.f * k, h + 8.f * k, Rgba{12, 15, 21, 170});
		canvas.text(x, y, line, 1.05f * k, accent());
		lineY = y + h + 12.f * k;
		if (!session.marks.empty())
		{
			// A square per shot under the line: green a fast hit, yellow a hit, dark a miss, outlined still to play.
			const float sq = 16.f * k, gap = 6.f * k;
			const float total = session.marks.size() * sq + (session.marks.size() - 1) * gap;
			float sx = (canvas.width() - total) * 0.5f;
			const float sy = lineY - 4.f * k;
			for (size_t i = 0; i < session.marks.size(); ++i)
			{
				const int m = session.marks[i];
				const Rgba fill = m == 1 ? Rgba{83, 196, 108, 255} : m == 2 ? Rgba{226, 186, 64, 255} : m == 3 ? Rgba{58, 62, 72, 255}
					: Rgba{12, 15, 21, 150};
				canvas.fillRect(sx, sy, sq, sq, fill);
				if (m == 0)
				{
					canvas.fillRect(sx, sy, sq, 1.5f * k, Rgba{200, 205, 215, 160});
					canvas.fillRect(sx, sy + sq - 1.5f * k, sq, 1.5f * k, Rgba{200, 205, 215, 160});
					canvas.fillRect(sx, sy, 1.5f * k, sq, Rgba{200, 205, 215, 160});
					canvas.fillRect(sx + sq - 1.5f * k, sy, 1.5f * k, sq, Rgba{200, 205, 215, 160});
				}
				sx += sq + gap;
			}
			lineY = sy + sq + 12.f * k;
		}
	}
	drawObjective(canvas, status, k, lineY);
	if (status.holding)
		drawReadCard(canvas, status, k);
	if (gameSpeed_ < 0.999f)
	{
		// Slow motion pill, right of the strip.
		char buf[32];
		snprintf(buf, sizeof(buf), "SLOW %d%%", static_cast<int>(gameSpeed_ * 100.f + 0.5f));
		float w = 0, h = 0;
		canvas.textSize(buf, 1.3f * k, w, h);
		const float x = (canvas.width() + 820.f * k) * 0.5f + 14.f * k, y = 16.f * k + 18.f * k;
		canvas.fillRect(x - 10.f * k, y - 6.f * k, w + 20.f * k, h + 12.f * k, kPanel);
		canvas.fillRect(x - 10.f * k, y + h + 3.f * k, w + 20.f * k, 3.f * k, Rgba{255, 196, 70, 255});
		canvas.text(x, y, buf, 1.3f * k, Rgba{255, 196, 70, 255});
	}
	drawFlash(canvas, k);
}

void Hud::drawObjective(Canvas& c, const ScenarioPlayer::Status& s, float k, float lineY)
{
	if (s.objective.empty())
		return;
	const std::string text = upper(s.objective);
	if (s.rep != objectiveRep_)
	{
		// A prompt (waiting for the player) gets no big announcement, and none pops up once the shot starts either.
		objectiveRep_ = s.rep;
		bannerText_ = s.prompt || s.quietObjective ? std::string() : text;
		bannerStart_ = std::chrono::steady_clock::now();
	}
	if (s.prompt)
	{
		if (s.holding)
			return; // the read card says it (with the exercise and what counts)
		// Small and still under the strip, so rapid resets are not noisy.
		float w = 0, h = 0;
		c.textSize(text, 1.05f * k, w, h);
		const float x = (c.width() - w) * 0.5f;
		c.fillRect(x - 12.f * k, lineY - 3.f * k, w + 24.f * k, h + 6.f * k, kPanel);
		c.text(x, lineY, text, 1.05f * k, kDim);
		return;
	}

	// 1. Standing line under the strip.
	{
		// A match shows its score and clock there instead of an objective.
		const std::string line = (s.match || s.plainObjective ? "" : "OBJECTIVE:  ") + text + (s.objectiveProgress.empty() ? "" : "   " + s.objectiveProgress);
		const float scale = fitScale(c, line, 1.6f * k, c.width() * 0.92f); // long objectives used to run off the screen
		float w = 0, h = 0;
		c.textSize(line, scale, w, h);
		const float x = (c.width() - w) * 0.5f;
		c.fillRect(x - 18.f * k, lineY - 5.f * k, w + 36.f * k, h + 10.f * k, kPanel);
		c.fillRect(x - 18.f * k, lineY + h + 2.f * k, w + 36.f * k, 3.f * k, accent());
		c.text(x, lineY, line, scale, s.hasResult ? kDim : accent());
	}

	// 2. Big announcement when it changes: a couple of seconds, or for as long as the rep waits to be started.
	if (s.waitingForInput)
		bannerStart_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(500); // held at full strength
	const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - bannerStart_).count();
	if (t < 2.2f && !bannerText_.empty() && !s.quietObjective)
	{
		const float alpha = t < 0.15f ? t / 0.15f : t > 1.7f ? 1.f - (t - 1.7f) / 0.5f : 1.f;
		const float pop = t < 0.15f ? 0.8f + 0.2f * (t / 0.15f) : 1.f;
		// As big as fits: a long objective (a HORSE combo) at full size ran off both sides of the screen.
		const float scale = fitScale(c, bannerText_, 3.4f * k, c.width() * 0.9f);
		centered(c, c.width() * 0.5f, c.height() * 0.40f, bannerText_, scale * pop, accent().withAlpha(alpha));
	}

	// 3. A tag on the objective in the world, or an arrow at the screen edge pointing the way to it.
	if (!s.objectiveAt || s.hasResult)
		return;
	float x = 0, y = 0;
	const bool inFront = c.project(*s.objectiveAt + Vec3(0.f, 0.f, 120.f), x, y);
	const float margin = 70.f * k;
	const bool onScreen = inFront && x > margin && x < c.width() - margin && y > margin && y < c.height() - margin;
	const std::string tag = upper(s.objectiveTag);
	float tw = 0, th = 0;
	c.textSize(tag, 1.4f * k, tw, th);
	if (onScreen)
	{
		c.fillRect(x - tw * 0.5f - 10.f * k, y - th - 14.f * k, tw + 20.f * k, th + 8.f * k, kPanel);
		c.text(x - tw * 0.5f, y - th - 10.f * k, tag, 1.4f * k, accent());
		// a small pointer down to the objective
		c.line(x - 10.f * k, y - 6.f * k, x, y + 6.f * k, 3.f * k, accent());
		c.line(x + 10.f * k, y - 6.f * k, x, y + 6.f * k, 3.f * k, accent());
		return;
	}
	// Off screen: push the direction from the centre out to the screen edge.
	const float cx = c.width() * 0.5f, cy = c.height() * 0.5f;
	float dx = x - cx, dy = y - cy;
	if (std::fabs(dx) < 1e-3f && std::fabs(dy) < 1e-3f)
		dy = 1.f;
	const float sx = (cx - margin) / std::max(std::fabs(dx), 1e-3f), sy = (cy - margin) / std::max(std::fabs(dy), 1e-3f);
	const float scale = std::min(sx, sy);
	const float ax = cx + dx * scale, ay = cy + dy * scale;
	const float len = std::sqrt(dx * dx + dy * dy);
	const float ux = dx / len, uy = dy / len;     // pointing out toward the objective
	const float px = -uy, py = ux;                // across
	const float size = 26.f * k;
	const Rgba col = accent();
	c.line(ax + ux * size, ay + uy * size, ax - ux * size * 0.2f + px * size, ay - uy * size * 0.2f + py * size, 5.f * k, col);
	c.line(ax + ux * size, ay + uy * size, ax - ux * size * 0.2f - px * size, ay - uy * size * 0.2f - py * size, 5.f * k, col);
	const float lx = std::clamp(ax - ux * size * 1.6f - tw * 0.5f, 8.f * k, c.width() - tw - 8.f * k);
	const float ly = std::clamp(ay - uy * size * 1.6f - th * 0.5f, 8.f * k, c.height() - th - 8.f * k);
	c.fillRect(lx - 8.f * k, ly - 4.f * k, tw + 16.f * k, th + 8.f * k, kPanel);
	c.text(lx, ly, tag, 1.4f * k, col);
}

void Hud::drawReadCard(Canvas& c, const ScenarioPlayer::Status& s, float k)
{
	// While a rep is held (waiting for a press, or counting down): the exercise's full name, what counts and the first
	// sentence of its description, under the big objective banner (Lambourne 2026-10-21: "I can't read the full
	// exercise text; especially during the daily it feels unfair").
	const float cardW = std::min(900.f * k, c.width() * 0.8f);
	const float pad = 22.f * k;
	const float inner = cardW - 2.f * pad;
	const std::vector<std::string> name = wrapLines(c, s.scenarioName, 1.3f * k, inner, 2);
	const float goalScale = s.goalText.empty() ? 0.f : fitScale(c, s.goalText, 2.f * k, inner);
	const std::vector<std::string> about = s.about.empty() ? std::vector<std::string>{} : wrapLines(c, s.about, 1.05f * k, inner, 3);
	const bool press = s.waitingForInput || s.prompt;
	float lw = 0, nameH = 0, goalH = 0, aboutH = 0, pressH = 0;
	c.textSize("Ag", 1.3f * k, lw, nameH);
	if (goalScale > 0.f)
		c.textSize("Ag", goalScale, lw, goalH);
	c.textSize("Ag", 1.05f * k, lw, aboutH);
	c.textSize("Ag", 1.05f * k, lw, pressH);
	const float gap = 8.f * k;
	float height = pad * 2.f + name.size() * nameH;
	if (goalScale > 0.f)
		height += gap + goalH;
	if (!about.empty())
		height += gap + about.size() * aboutH;
	if (press)
		height += gap * 2.f + pressH;
	const float x = (c.width() - cardW) * 0.5f;
	const float y = std::min(c.height() * 0.53f, c.height() - height - 120.f * k);
	c.fillRect(x, y, cardW, height, kPanel);
	c.fillRect(x, y, 4.f * k, height, accent());
	float ty = y + pad;
	for (const auto& line : name)
	{
		centered(c, c.width() * 0.5f, ty, line, 1.3f * k, kText);
		ty += nameH;
	}
	if (goalScale > 0.f)
	{
		ty += gap;
		centered(c, c.width() * 0.5f, ty, s.goalText, goalScale, accent());
		ty += goalH;
	}
	if (!about.empty())
	{
		ty += gap;
		for (const auto& line : about)
		{
			centered(c, c.width() * 0.5f, ty, line, 1.05f * k, kDim);
			ty += aboutH;
		}
	}
	if (press)
	{
		// A soft pulse: noticed, not shouted.
		ty += gap * 2.f;
		const float pulse = 0.65f + 0.35f * std::sin(std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count() * 4.f);
		centered(c, c.width() * 0.5f, ty, "PRESS ANY BUTTON TO START", 1.05f * k, Rgba{235, 240, 248, static_cast<uint8_t>(255 * pulse)});
	}
}

void Hud::drawStrip(Canvas& c, const ScenarioPlayer::Status& s, float k)
{
	const float w = 820.f * k, h = 64.f * k;
	const float x = (c.width() - w) * 0.5f, y = 16.f * k;
	c.fillRect(x, y, w, h, kPanel);
	c.fillRect(x, y + h - 3.f * k, w, 3.f * k, accent());

	char time[48];
	if (s.timeLimitSeconds > 0.0 && s.clockCountsDown)
		snprintf(time, sizeof(time), "%.1f s left", std::max(0.0, s.timeLimitSeconds - s.repSeconds));
	else if (s.timeLimitSeconds > 0.0)
		snprintf(time, sizeof(time), "%.1f / %.0f s", s.repSeconds, s.timeLimitSeconds);
	else
		snprintf(time, sizeof(time), "%.1f s", s.repSeconds);

	struct Cell { const char* label; std::string value; Rgba color; float share; };
	const Rgba timeColor = !s.hasResult ? kText : s.outcome > 0 ? accent() : s.outcome < 0 ? kFail : kDim;
	const Cell cells[] = {
		{"EXERCISE", s.scenarioName, kText, 2.6f},
		{"REP", std::to_string(s.rep), kText, 0.8f},
		{"TIME", time, timeColor, 1.3f},
		// Bowling shows the pins down; timed drills show the personal best instead of the streak.
		s.tank >= 0.f ? Cell{"TANK", std::to_string(static_cast<int>(s.tank + 0.5f)), s.tank < 20.f ? kFail : kText, 0.9f}
		: s.pinsTotal > 0 ? Cell{"PINS", std::to_string(s.pinsDown) + " / " + std::to_string(s.pinsTotal) + "   T" +
			std::to_string(s.frameThrow) + "/" + std::to_string(s.frameThrows), s.pinsDown == s.pinsTotal ? accent() : kText, 0.9f}
		: s.objectsTotal > 0 ? Cell{"BROKEN", std::to_string(s.objectsHit) + " / " + std::to_string(s.objectsTotal),
			s.objectsHit == s.objectsTotal ? accent() : kText, 0.9f}
		: s.timed ? Cell{"BEST", s.bestSeconds > 0.0 ? fmtSeconds(s.bestSeconds) : std::string("-"), s.newBest ? accent() : kText, 0.9f}
			: Cell{"STREAK", std::to_string(s.streak), s.streak > 0 ? accent() : kText, 0.9f},
		{"LEVEL", upper(s.difficulty), kText, 1.0f},
	};
	float total = 0.f;
	for (const auto& cell : cells)
		total += cell.share;

	const float pad = 18.f * k;
	float cx = x + pad;
	const float usable = w - 2 * pad;
	for (size_t i = 0; i < std::size(cells); ++i)
	{
		const auto& cell = cells[i];
		const float cw = usable * cell.share / total;
		c.text(cx, y + 9.f * k, cell.label, 0.85f * k, kDim, false);
		std::string value = cell.value;
		// Keep long exercise names inside their cell: smaller first (down to 70%), cut only if still too long.
		const float scale = std::max(1.f * k, fitScale(c, value, 1.45f * k, cw - 8.f * k));
		float tw = 0, th = 0;
		c.textSize(value, scale, tw, th);
		while (value.size() > 4 && tw > cw - 8.f * k)
		{
			value = value.substr(0, value.size() - 4) + "...";
			c.textSize(value, scale, tw, th);
		}
		c.text(cx, y + 26.f * k + (1.45f * k - scale) * 8.f, value, scale, cell.color);
		if (i + 1 < std::size(cells))
			c.fillRect(cx + cw - 10.f * k, y + 12.f * k, 1.f, h - 24.f * k, Rgba{255, 255, 255, 30});
		cx += cw;
	}
}

void Hud::pushMechanic(const std::string& label, float kmh, int points, bool force)
{
	if (!feedOn_ && !force)
		return;
	feed_.push_back({label, kmh, points, std::chrono::steady_clock::now()});
	while (feed_.size() > 5)
		feed_.pop_front();
}

void Hud::drawMilestone(Canvas& c)
{
	if (milestone_.empty())
		return;
	constexpr float kShow = 3.6f;
	const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - milestoneAt_).count();
	if (t > kShow + 0.5f)
	{
		milestone_.clear();
		return;
	}
	const float k = c.height() / 1080.f;
	const float in = std::min(1.f, t / 0.35f);
	const float ease = 1.f - (1.f - in) * (1.f - in) * (1.f - in);
	const float alpha = t > kShow ? std::max(0.f, 1.f - (t - kShow) / 0.5f) : 1.f;
	const Rgba medal = milestoneTier_ >= 3 ? Rgba{255, 205, 80, 255} : milestoneTier_ == 2 ? Rgba{205, 214, 228, 255} : Rgba{222, 146, 90, 255};
	const std::string kicker = "MILESTONE UNLOCKED";
	float kw = 0, kh = 0, tw = 0, th = 0;
	c.textSize(kicker, 0.95f * k, kw, kh);
	c.textSize(milestone_, 1.9f * k, tw, th);
	const float w = std::max(kw, tw) + 90.f * k, h = kh + th + 34.f * k;
	const float x = (c.width() - w) * 0.5f;
	const float y = c.height() * 0.70f + (1.f - ease) * 50.f * k; // below the middle, clear of the strip and results
	c.fillRect(x, y, w, h, Rgba{10, 13, 20, static_cast<unsigned char>(215 * alpha)});
	c.fillRect(x, y, 5.f * k, h, medal.withAlpha(alpha));
	c.fillRect(x, y + h - 3.f * k, w, 3.f * k, medal.withAlpha(0.8f * alpha));
	// A small medal: a filled square with a lighter centre.
	const float m = 26.f * k;
	c.fillRect(x + 24.f * k, y + (h - m) * 0.5f, m, m, medal.withAlpha(alpha));
	c.fillRect(x + 24.f * k + m * 0.3f, y + (h - m) * 0.5f + m * 0.3f, m * 0.4f, m * 0.4f, Rgba{255, 255, 255, static_cast<unsigned char>(120 * alpha)});
	c.text(x + 66.f * k, y + 12.f * k, kicker, 0.95f * k, medal.withAlpha(alpha));
	c.text(x + 66.f * k, y + 16.f * k + kh, milestone_, 1.9f * k, Rgba{245, 247, 252, static_cast<unsigned char>(255 * alpha)});
}

void Hud::drawHint(Canvas& c)
{
	if (hint_.empty())
		return;
	const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - hintAt_).count();
	if (t > hintSeconds_)
	{
		hint_.clear();
		return;
	}
	const float k = c.height() / 1080.f;
	const float alpha = std::min(1.f, std::min(t / 0.12f, (hintSeconds_ - t) / 0.25f));
	float tw = 0, th = 0;
	c.textSize(hint_, 1.15f * k, tw, th);
	const float w = tw + 40.f * k, h = th + 18.f * k;
	const float x = (c.width() - w) * 0.5f, y = c.height() * 0.82f;
	c.fillRect(x, y, w, h, Rgba{10, 13, 20, static_cast<unsigned char>(200 * alpha)});
	c.text(x + 20.f * k, y + 9.f * k, hint_, 1.15f * k, Rgba{235, 240, 250, static_cast<unsigned char>(255 * alpha)});
}

void Hud::drawFeed(Canvas& c)
{
	drawMilestone(c);
	drawHint(c);
	constexpr float kShow = 3.2f, kFade = 0.6f;
	const auto now = std::chrono::steady_clock::now();
	while (!feed_.empty() && std::chrono::duration<float>(now - feed_.front().at).count() > kShow + kFade)
		feed_.pop_front();
	if (feed_.empty())
		return;
	const float k = c.height() / 1080.f;
	// Newest at the bottom, stacked upward from just below the middle of the left edge.
	float y = c.height() * 0.62f;
	const float x = 48.f * k;
	for (auto it = feed_.rbegin(); it != feed_.rend(); ++it)
	{
		const float t = std::chrono::duration<float>(now - it->at).count();
		const float alpha = t < 0.12f ? t / 0.12f : t > kShow ? std::max(0.f, 1.f - (t - kShow) / kFade) : 1.f;
		const float pop = t < 0.12f ? 0.85f + 0.15f * (t / 0.12f) : 1.f;
		const float scale = (it->points >= 5 ? 2.3f : 1.9f) * k * pop;
		// Big mechanics in the accent colour, a gold edge for the hardest ones.
		const Rgba col = it->points >= 6 ? Rgba{255, 210, 90, 255} : it->points >= 3 ? accent() : kText;
		float w = 0, h = 0;
		c.textSize(it->label, scale, w, h);
		c.fillRect(x - 12.f * k, y - 6.f * k, 6.f * k, h + 12.f * k, col.withAlpha(alpha));
		c.text(x + 4.f * k, y, it->label, scale, col.withAlpha(alpha));
		std::string sub;
		if (it->points > 0)
			sub = "+" + std::to_string(it->points);
		if (it->kmh >= 1.f)
		{
			char buf[24];
			snprintf(buf, sizeof(buf), "%s%.0f KM/H", sub.empty() ? "" : "   ", it->kmh);
			sub += buf;
		}
		if (!sub.empty())
			c.text(x + 14.f * k + w, y + h * 0.35f, sub, 1.0f * k, kDim.withAlpha(alpha));
		y -= h + 18.f * k;
	}
}

void Hud::drawPlayMap(Canvas& c, const ScenarioPlayer::Status::PlayMap& m, float k)
{
	// Top-down, your net at the bottom, on the right of the screen. Seen from your net, +x is on the LEFT in Rocket
	// League (Unreal is left-handed), so x is flipped onto the map.
	const float mh = 340.f * k, mw = mh * 8192.f / 10240.f;
	const float x0 = c.width() - mw - 56.f * k, y0 = c.height() * 0.5f - mh * 0.5f;
	auto mx = [&](float wx) { return x0 + (4096.f - wx) / 8192.f * mw; };
	auto my = [&](float wy) { return y0 + (5120.f - wy) / 10240.f * mh; };
	const float s = mw / 8192.f; // uu -> px
	const Rgba lineCol{255, 255, 255, 70};
	c.fillRect(x0 - 14.f * k, y0 - 40.f * k, mw + 28.f * k, mh + 54.f * k, Rgba{8, 11, 18, 175});
	c.text(x0, y0 - 32.f * k, "THE PLAY", 0.9f * k, accent());
	// Field outline with the corner walls (|x| + |y| = 8064).
	const float cut = 1152.f;
	const float px[] = {-4096.f + cut, 4096.f - cut, 4096.f, 4096.f, 4096.f - cut, -4096.f + cut, -4096.f, -4096.f};
	const float py[] = {5120.f, 5120.f, 5120.f - cut, -5120.f + cut, -5120.f, -5120.f, -5120.f + cut, 5120.f - cut};
	for (int i = 0; i < 8; ++i)
		c.line(mx(px[i]), my(py[i]), mx(px[(i + 1) % 8]), my(py[(i + 1) % 8]), 2.f * k, lineCol);
	c.line(mx(-4096.f), my(0.f), mx(4096.f), my(0.f), 1.f * k, lineCol);
	c.fillRect(mx(893.f), my(5120.f) - 3.f * k, 1786.f * s, 6.f * k, Rgba{255, 140, 40, 220});
	c.fillRect(mx(893.f), my(-5120.f) - 3.f * k, 1786.f * s, 6.f * k, Rgba{60, 140, 255, 220});
	auto ring = [&](const Vec3& at, float radius, Rgba col, float width) {
		constexpr int kSeg = 28;
		for (int i = 0; i < kSeg; ++i)
		{
			const float a0 = 6.2831853f * i / kSeg, a1 = 6.2831853f * (i + 1) / kSeg;
			c.line(mx(at.x + std::cos(a0) * radius), my(at.y + std::sin(a0) * radius), mx(at.x + std::cos(a1) * radius),
				my(at.y + std::sin(a1) * radius), width, col);
		}
	};
	auto car = [&](const Vec3& at, int yaw, Rgba col, const char* label) {
		const float a = yaw * 3.14159265f / 32768.f;
		const float cx = mx(at.x), cy = my(at.y), r = 6.f * k;
		c.line(cx, cy, cx - std::cos(a) * 22.f * k, cy - std::sin(a) * 22.f * k, 3.f * k, col); // x flipped
		c.fillRect(cx - r, cy - r, 2.f * r, 2.f * r, col);
		c.text(cx + 10.f * k, cy + 4.f * k, label, 0.75f * k, col);
	};
	ring({0.f, 0.f, 0.f}, 915.f, lineCol, 1.f * k);
	if (m.hasMate)
	{
		const Rgba green{90, 230, 120, 255};
		ring(m.mate, m.mateRadius, green, 3.f * k);
		c.text(mx(m.mate.x) - 18.f * k, my(m.mate.y) + m.mateRadius * s + 4.f * k, "MATE", 0.8f * k, green);
	}
	if (m.hasContact)
	{
		const float cx = mx(m.contact.x), cy = my(m.contact.y), r = 7.f * k;
		const Rgba col{255, 230, 120, 230};
		c.line(cx - r, cy - r, cx + r, cy + r, 2.f * k, col);
		c.line(cx - r, cy + r, cx + r, cy - r, 2.f * k, col);
	}
	for (size_t i = 0; i < m.bots.size(); ++i)
		car(m.bots[i], i < m.botYaws.size() ? m.botYaws[i] : 0, Rgba{255, 120, 60, 255}, "BOT");
	car(m.player, m.playerYaw, accent(), "YOU");
	const float br = 5.f * k;
	c.fillRect(mx(m.ball.x) - br, my(m.ball.y) - br, 2.f * br, 2.f * br, Rgba{245, 245, 245, 255});
}

void Hud::drawFlash(Canvas& c, float k)
{
	if (!flashing_)
		return;
	const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - flashStart_).count();
	if (t > kFlashSeconds)
	{
		flashing_ = false;
		return;
	}
	// Pop in fast, hold, fade out.
	const float alpha = t < 0.12f ? t / 0.12f : t > 1.1f ? 1.f - (t - 1.1f) / (kFlashSeconds - 1.1f) : 1.f;
	const float pop = t < 0.12f ? 0.85f + 0.15f * (t / 0.12f) : 1.f;

	// Text only (with drop shadow): the dark band behind it was distracting (Lambourne).
	const Rgba color = flash_.outcome > 0 ? accent() : flash_.outcome < 0 ? kFail : kText;
	const float bandY = c.height() * 0.24f;

	centered(c, c.width() * 0.5f, bandY + 18.f * k, flash_.resultLabel, 5.2f * k * pop, color.withAlpha(alpha));

	std::string sub;
	char buf[64];
	if (!flash_.resultDetail.empty())
	{
		sub = flash_.resultDetail;
	}
	else if (flash_.pinsTotal > 0)
	{
		sub = "FRAME " + std::to_string(flash_.frame) + "   |   THROW " + std::to_string(flash_.frameThrow) + " OF "
			+ std::to_string(flash_.frameThrows) + "   |   " + std::to_string(flash_.pinsDown) + " / "
			+ std::to_string(flash_.pinsTotal) + " DOWN";
	}
	else if (flash_.timed && flash_.outcome > 0)
	{
		// "1.84 s  |  NEW BEST" or "1.92 s  |  BEST 1.84 s"
		sub = fmtSeconds(flash_.resultSeconds) + (flash_.newBest ? "   |   NEW BEST"
			: flash_.bestSeconds > 0.0 ? "   |   BEST " + fmtSeconds(flash_.bestSeconds) : "");
	}
	else if (flash_.resultKmh > 0.f)
	{
		snprintf(buf, sizeof(buf), "%.0f km/h", flash_.resultKmh);
		sub = buf;
	}
	if (flash_.outcome > 0 && flash_.streak > 1)
	{
		snprintf(buf, sizeof(buf), "%sSTREAK %d", sub.empty() ? "" : "   |   ", flash_.streak);
		sub += buf;
	}
	if (!sub.empty())
		centered(c, c.width() * 0.5f, bandY + 100.f * k, sub, 1.7f * k, kText.withAlpha(alpha));
}
