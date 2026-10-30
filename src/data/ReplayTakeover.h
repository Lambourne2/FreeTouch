#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "data/Capture.h"
#include "data/LeadIn.h"
#include "data/StateJson.h"
#include "game/GameState.h"
#include "scenario/BallPredict.h"
#include "third_party/nlohmann/json.hpp"

// Replay takeover (Phase 6): the moment of a replay becomes a freeplay drill. While a replay plays, the last couple of
// seconds of the ball and every car are kept (Recorder); "take over" turns the latest frame into a scenario with
// smoothed velocities, the chosen player as you (the field turned round when they were orange, so you always attack
// the orange net), their opponents as bots, and the right rules: a save when the ball is on its way into your net,
// a goal otherwise. No SDK calls.
namespace takeover
{
	struct Frame
	{
		double time = 0.0;  // replay seconds
		std::optional<BallState> ball;
		std::vector<CarState> cars;
	};

	// Keeps the last `seconds` of replay frames (one per new replay time; paused or repeated times are skipped). A jump
	// back in time (scrubbing, a new replay) starts over.
	class Recorder
	{
	public:
		void onFrame(const Snapshot& s, double replayTime)
		{
			if (!frames_.empty() && replayTime < frames_.back().time - 0.01)
				frames_.clear();
			if (!frames_.empty() && std::fabs(replayTime - frames_.back().time) < 1e-4)
				return; // paused, or the same frame drawn again
			if (!frames_.empty() && replayTime - frames_.back().time > 1.0)
				frames_.clear(); // skipped ahead
			frames_.push_back({replayTime, s.ball, s.cars});
			while (!frames_.empty() && frames_.back().time - frames_.front().time > kKeepSeconds)
				frames_.pop_front();
		}
		void clear() { frames_.clear(); }
		const std::deque<Frame>& frames() const { return frames_; }

	private:
		static constexpr double kKeepSeconds = 2.5;
		std::deque<Frame> frames_;
	};

	// Velocity smoothing: replays store positions at their own rate and their reported velocities can be stale or
	// zero. The reported velocity averaged over the last ~0.1 s, blended with the velocity the positions show over that
	// window; positions only when nothing was reported.
	inline Vec3 smoothVelocity(const std::vector<std::pair<double, std::pair<Vec3, Vec3>>>& samples) // time, (location, velocity)
	{
		if (samples.empty())
			return {};
		const double tEnd = samples.back().first;
		Vec3 sum;
		int n = 0;
		size_t first = samples.size() - 1;
		for (size_t i = samples.size(); i-- > 0;)
		{
			if (tEnd - samples[i].first > 0.1)
				break;
			sum = sum + samples[i].second.second;
			++n;
			first = i;
		}
		const Vec3 reported = n > 0 ? sum * (1.f / n) : samples.back().second.second;
		const double span = tEnd - samples[first].first;
		if (span < 0.02)
			return reported;
		const Vec3 moved = (samples.back().second.first - samples[first].second.first) * static_cast<float>(1.0 / span);
		if (reported.length() < 1.f)
			return moved;
		// Big disagreement (a bounce or a touch inside the window): trust the latest report.
		if ((reported - moved).length() > std::max(400.f, 0.5f * reported.length()))
			return samples.back().second.second;
		return reported * 0.6f + moved * 0.4f;
	}

	struct Options
	{
		uint64_t playerId = 0;       // which car you take over (CarState::id)
		int others = 2;              // the other players as bots: 2 everyone (teammates on your side), 1 opponents, 0 none
		bool leadIn = true;          // replay the moments before it (everyone moving), then wait for you; else a 3-2-1
		std::string replayName;
		std::string map;
		std::string stamp;           // local time, for the description
	};

	inline int wrapYaw(int yaw)
	{
		yaw %= 65536;
		if (yaw > 32767)
			yaw -= 65536;
		if (yaw < -32768)
			yaw += 65536;
		return yaw;
	}

	// The scenario for the latest frame, or null with `error` set.
	inline nlohmann::ordered_json scenario(const std::deque<Frame>& frames, const Options& o, std::string& error)
	{
		using json = nlohmann::ordered_json;
		if (frames.empty() || !frames.back().ball)
		{
			error = "no replay frames yet (let the replay play for a moment)";
			return nullptr;
		}
		const Frame& now = frames.back();
		const auto me = std::find_if(now.cars.begin(), now.cars.end(), [&](const CarState& c) { return c.id == o.playerId; });
		if (me == now.cars.end())
		{
			error = "that player has no car right now (demolished?)";
			return nullptr;
		}
		// Orange: turn the field round (180 degrees about z), so the drill is played as blue attacking +y.
		const bool turn = me->team == 1;
		auto pos = [&](Vec3 v) { return turn ? Vec3(-v.x, -v.y, v.z) : v; };
		auto rot = [&](Rot r) {
			if (turn)
				r.yaw = wrapYaw(r.yaw + 32768);
			return r;
		};
		auto history = [&](auto pick) {
			std::vector<std::pair<double, std::pair<Vec3, Vec3>>> out;
			for (const auto& f : frames)
				if (const auto lv = pick(f))
					out.push_back({f.time, *lv});
			return out;
		};
		const Vec3 ballVel = smoothVelocity(history([](const Frame& f) -> std::optional<std::pair<Vec3, Vec3>> {
			if (!f.ball)
				return std::nullopt;
			return std::make_pair(f.ball->location, f.ball->velocity);
		}));
		auto carVel = [&](uint64_t id) {
			return smoothVelocity(history([id](const Frame& f) -> std::optional<std::pair<Vec3, Vec3>> {
				for (const auto& c : f.cars)
					if (c.id == id)
						return std::make_pair(c.location, c.velocity);
				return std::nullopt;
			}));
		};
		// Replays do not always carry boost: when every car reads 0, assume a third of a tank.
		const bool boostKnown = std::any_of(now.cars.begin(), now.cars.end(), [](const CarState& c) { return c.boost > 0.5f; });
		auto boostOf = [&](const CarState& c) { return boostKnown ? std::round(c.boost) : 33.0f; };

		BallState ball{pos(now.ball->location), pos(ballVel), pos(now.ball->angularVelocity)};
		// Defending or attacking: is the ball on its way into your net (blue, after the turn)? The flight prediction, or
		// (for a ball rolling along the floor, which the prediction slows too fast) heading at your net: toward it fast
		// enough to get there within 4 s, lined up with the goal give or take a post's width.
		bool save = ballpredict::timeToGoal(ball, -1) >= 0.f;
		if (!save && ball.velocity.y < -400.f)
		{
			const float t = (ball.location.y + 5120.f) / -ball.velocity.y;
			const float xAtLine = ball.location.x + ball.velocity.x * t;
			save = t < 4.f && std::fabs(xAtLine) < 1500.f;
		}

		const int secs = static_cast<int>(now.time);
		char clock[16];
		snprintf(clock, sizeof(clock), "%d:%02d", secs / 60, secs % 60);
		const std::string who = me->name.empty() ? "Player" : me->name;
		const std::string tag = capture::sanitizeId(o.replayName.empty() ? "replay" : o.replayName).substr(0, 40);
		char idTime[16];
		snprintf(idTime, sizeof(idTime), "%dm%02ds", secs / 60, secs % 60);

		json doc;
		doc["id"] = "replay." + tag + "_" + idTime + "_" + capture::sanitizeId(who).substr(0, 20);
		doc["name"] = who + " at " + clock + (o.replayName.empty() ? std::string() : " (" + o.replayName + ")");
		doc["category"] = "replay";
		int teammates = 0, opponents = 0;
		for (const auto& c : now.cars)
		{
			if (c.id == me->id)
				continue;
			(c.team == me->team ? teammates : opponents) += 1;
		}
		std::string about = std::string(save ? "Save it: " : "Finish it: ") + "the moment " + who + " had at " + clock
			+ (o.replayName.empty() ? "" : " in " + o.replayName) + ", from your replay. You are " + who + ".";
		if (o.others >= 2 && teammates + opponents > 0)
			about += " Everyone else comes along as bots, teammates on your side.";
		else if (o.others == 1 && opponents > 0)
			about += opponents == 1 ? " Their opponent is a bot." : " Their opponents are bots.";
		if (o.leadIn)
			about += " It replays the moments before it, then waits for you to press something.";
		if (turn)
			about += " The field is turned round so you attack the orange net.";
		doc["description"] = about;
		doc["tags"] = json::array({"replay", "takeover", save ? "save" : "attack"});
		std::vector<std::string> players;
		for (const auto& c : now.cars)
			players.push_back(c.name);
		doc["source"] = {{"kind", "replay_takeover"}, {"replayId", o.replayName}, {"time", std::round(now.time * 100.0) / 100.0},
			{"players", players}, {"label", o.stamp}};
		if (!o.map.empty())
			doc["arena"] = o.map;
		doc["allowMirror"] = false;
		doc["timeLimitSeconds"] = 10;
		doc["ball"] = {{"location", capture::vec1(ball.location)}, {"velocity", capture::vec1(ball.velocity)},
			{"angularVelocity", capture::vec1(ball.angularVelocity)}};
		doc["player"] = {{"location", capture::vec1(pos(me->location))}, {"rotation", statejson::toJson(rot(me->rotation))},
			{"velocity", capture::vec1(pos(carVel(me->id)))}, {"angularVelocity", capture::vec1(pos(me->angularVelocity))},
			{"boost", boostOf(*me)}};
		// The lead-up: the frames before the moment resampled to 60 Hz (the replay's own rate varies), turned like the rest.
		const double t1 = now.time, t0 = std::max(frames.front().time, t1 - 2.5);
		const bool lead = o.leadIn && t1 - t0 >= 0.4 && frames.size() >= 4;
		std::vector<double> times;
		if (lead)
			for (double t = t0; t < t1 + 1e-6; t += 1.0 / 60.0)
				times.push_back(std::min(t, t1));
		// Where `id` (0 = the ball) was at time `t`, between the two frames around it.
		auto at = [&](uint64_t id, double t, Vec3& loc, Vec3& vel, Rot& r) {
			size_t j = 1;
			while (j + 1 < frames.size() && frames[j].time < t)
				++j;
			const Frame& fa = frames[j - 1];
			const Frame& fb = frames[std::min(j, frames.size() - 1)];
			const float u = fb.time > fa.time ? static_cast<float>(std::clamp((t - fa.time) / (fb.time - fa.time), 0.0, 1.0)) : 1.f;
			auto find = [&](const Frame& f, Vec3& l, Vec3& v, Rot& ro) {
				if (id == 0)
				{
					if (!f.ball)
						return false;
					l = f.ball->location;
					v = f.ball->velocity;
					return true;
				}
				for (const auto& c : f.cars)
					if (c.id == id)
					{
						l = c.location;
						v = c.velocity;
						ro = c.rotation;
						return true;
					}
				return false;
			};
			Vec3 la, va, lb, vb;
			Rot ra, rb;
			const bool ha = find(fa, la, va, ra), hb = find(fb, lb, vb, rb);
			if (!ha && !hb)
				return false;
			if (!ha) { la = lb; va = vb; ra = rb; }
			if (!hb) { lb = la; vb = va; rb = ra; }
			loc = pos(leadin::lerp(la, lb, u));
			vel = pos(leadin::lerp(va, vb, u));
			r = rot(Rot{leadin::lerpAngle(ra.pitch, rb.pitch, u), leadin::lerpAngle(ra.yaw, rb.yaw, u), leadin::lerpAngle(ra.roll, rb.roll, u)});
			return true;
		};
		if (lead)
		{
			leadin::Track track;
			for (const double t : times)
			{
				leadin::Frame f;
				Rot unused;
				at(0, t, f.ball.location, f.ball.velocity, unused);
				at(me->id, t, f.car.location, f.car.velocity, f.car.rotation);
				track.frames.push_back(f);
			}
			// The last frame is the moment itself, with the smoothed velocities.
			track.frames.back().ball = BallTarget{ball.location, ball.velocity, ball.angularVelocity};
			track.frames.back().car.location = pos(me->location);
			track.frames.back().car.rotation = rot(me->rotation);
			track.frames.back().car.velocity = pos(carVel(me->id));
			doc["leadIn"] = leadin::toJson(track);
		}

		json bots = json::array();
		for (const auto& c : now.cars)
		{
			if (c.id == me->id || o.others == 0 || (o.others == 1 && c.team == me->team))
				continue;
			json bot = {{"type", "psyonix"}, {"team", c.team == me->team ? 0 : 1}, {"location", capture::vec1(pos(c.location))},
				{"rotation", capture::vec1(Vec3(static_cast<float>(rot(c.rotation).pitch), static_cast<float>(rot(c.rotation).yaw),
					static_cast<float>(rot(c.rotation).roll)))},
				{"velocity", capture::vec1(pos(carVel(c.id)))}, {"boost", boostOf(c)}, {"params", {{"skill", 1.0}, {"beta", 1.0}}}};
			if (!c.name.empty())
				bot["label"] = c.name;
			if (lead)
			{
				// Its own lead-up: [x, y, z, pitch, yaw, roll, vx, vy, vz] at 60 Hz, ending on its start.
				json rows = json::array();
				for (const double t : times)
				{
					Vec3 l, v;
					Rot r;
					if (!at(c.id, t, l, v, r))
					{
						l = pos(c.location);
						r = rot(c.rotation);
					}
					rows.push_back({capture::round1(l.x), capture::round1(l.y), capture::round1(l.z), r.pitch, r.yaw, r.roll,
						capture::round1(v.x), capture::round1(v.y), capture::round1(v.z)});
				}
				bot["track"] = {{"hz", 60}, {"frames", rows}};
			}
			bots.push_back(bot);
		}
		if (!bots.empty())
			doc["bots"] = bots;
		doc["success"] = {{"type", save ? "save" : "goal"}};
		doc["fail"] = json::array({{{"type", "conceded"}}, {{"type", "timeout"}, {"seconds", 10}}});
		if (!lead)
			doc["startCountdownSeconds"] = 3;
		doc["showPlayMap"] = true;
		doc["onSuccess"] = "next";
		doc["onFail"] = "next";
		doc["advanceDelaySeconds"] = 2;
		doc["notes"] = "Made by replay takeover. Velocities smoothed over the last 0.1 s of the replay.";
		return doc;
	}
}
