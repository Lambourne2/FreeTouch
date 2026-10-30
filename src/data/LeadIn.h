#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// Saved Shots lead-in: the seconds before a capture, replayed before the shot freezes and waits for the player.
// The recorder keeps the last few seconds of ball and car; a capture stores them in the scenario as
//   "leadIn": {"hz": 60, "frames": [[ball loc 3, ball vel 3, ball spin 3, car loc 3, car rot 3, car vel 3, car spin 3], ...]}
// No SDK calls.
namespace leadin
{
	struct Frame
	{
		BallTarget ball;
		CarTarget car; // no boost
	};

	struct Track
	{
		double hz = 60.0;
		std::vector<Frame> frames;
		double seconds() const { return frames.size() < 2 ? 0.0 : (frames.size() - 1) / hz; }
	};

	constexpr int kValues = 21;

	inline double round1(float v) { return std::round(static_cast<double>(v) * 10.0) / 10.0; }
	inline double round2(float v) { return std::round(static_cast<double>(v) * 100.0) / 100.0; }

	inline nlohmann::ordered_json toJson(const Track& t)
	{
		nlohmann::ordered_json frames = nlohmann::ordered_json::array();
		for (const auto& f : t.frames)
		{
			const Vec3* v[] = {&f.ball.location, &f.ball.velocity, &f.ball.angularVelocity, &f.car.location};
			nlohmann::ordered_json row = nlohmann::ordered_json::array();
			for (int i = 0; i < 4; ++i)
			{
				const bool spin = i == 2;
				for (float c : {v[i]->x, v[i]->y, v[i]->z})
					row.push_back(spin ? round2(c) : round1(c));
			}
			row.push_back(f.car.rotation.pitch);
			row.push_back(f.car.rotation.yaw);
			row.push_back(f.car.rotation.roll);
			for (float c : {f.car.velocity.x, f.car.velocity.y, f.car.velocity.z})
				row.push_back(round1(c));
			for (float c : {f.car.angularVelocity.x, f.car.angularVelocity.y, f.car.angularVelocity.z})
				row.push_back(round2(c));
			frames.push_back(std::move(row));
		}
		return {{"hz", t.hz}, {"frames", std::move(frames)}};
	}

	// Empty when `j` is missing or malformed (rows of the wrong length are skipped).
	template <class Json>
	Track fromJson(const Json& j)
	{
		Track t;
		if (!j.is_object() || !j.contains("frames") || !j["frames"].is_array())
			return t;
		t.hz = std::max(1.0, j.value("hz", 60.0));
		for (const auto& row : j["frames"])
		{
			if (!row.is_array() || row.size() != kValues)
				continue;
			float n[kValues];
			for (int i = 0; i < kValues; ++i)
				n[i] = row[i].is_number() ? row[i].template get<float>() : 0.f;
			Frame f;
			f.ball = {{n[0], n[1], n[2]}, {n[3], n[4], n[5]}, {n[6], n[7], n[8]}};
			f.car.location = {n[9], n[10], n[11]};
			f.car.rotation = {static_cast<int>(n[12]), static_cast<int>(n[13]), static_cast<int>(n[14])};
			f.car.velocity = {n[15], n[16], n[17]};
			f.car.angularVelocity = {n[18], n[19], n[20]};
			t.frames.push_back(f);
		}
		return t;
	}

	inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
	// Rotation units wrap at 65536: take the short way round.
	inline int lerpAngle(int a, int b, float t)
	{
		int d = (b - a) % 65536;
		if (d > 32768) d -= 65536;
		if (d < -32768) d += 65536;
		int r = (a + static_cast<int>(std::lround(d * t))) % 65536; // back into [-32768, 32768)
		if (r >= 32768) r -= 65536;
		if (r < -32768) r += 65536;
		return r;
	}

	// The pose `seconds` into the track (clamped to its ends), interpolated between frames.
	inline Frame sample(const Track& t, double seconds)
	{
		if (t.frames.empty())
			return {};
		const double pos = std::clamp(seconds * t.hz, 0.0, static_cast<double>(t.frames.size() - 1));
		const size_t i = std::min(static_cast<size_t>(pos), t.frames.size() - 1);
		const size_t j = std::min(i + 1, t.frames.size() - 1);
		const float u = static_cast<float>(pos - i);
		const Frame& a = t.frames[i];
		const Frame& b = t.frames[j];
		Frame f;
		f.ball = {lerp(a.ball.location, b.ball.location, u), lerp(a.ball.velocity, b.ball.velocity, u),
			lerp(a.ball.angularVelocity, b.ball.angularVelocity, u)};
		f.car.location = lerp(a.car.location, b.car.location, u);
		f.car.rotation = {lerpAngle(a.car.rotation.pitch, b.car.rotation.pitch, u),
			lerpAngle(a.car.rotation.yaw, b.car.rotation.yaw, u), lerpAngle(a.car.rotation.roll, b.car.rotation.roll, u)};
		f.car.velocity = lerp(a.car.velocity, b.car.velocity, u);
		f.car.angularVelocity = lerp(a.car.angularVelocity, b.car.angularVelocity, u);
		return f;
	}

	// The player is pressing something (sticks past a small dead zone, or any button).
	inline bool anyInput(const CarInput& in)
	{
		constexpr float dz = 0.2f;
		return std::fabs(in.throttle) > dz || std::fabs(in.steer) > dz || std::fabs(in.pitch) > dz
			|| std::fabs(in.yaw) > dz || std::fabs(in.roll) > dz || in.jump || in.boost || in.handbrake;
	}

	// The last `keepSeconds` of ball and local car, sampled at `hz` from the 120 Hz tick. A jump in position no
	// physics could make (a reset, a teleport, a new rep) starts the recording over, so a lead-in never shows one.
	class Recorder
	{
	public:
		explicit Recorder(double keepSeconds = 4.0, double hz = 60.0) : keep_(keepSeconds), hz_(hz) {}

		void onTick(const Snapshot& s)
		{
			const CarState* me = s.localCar();
			if (!s.ball || !me || !s.inFreeplay)
			{
				frames_.clear();
				return;
			}
			const auto every = static_cast<uint64_t>(std::max(1.0, std::round(120.0 / hz_)));
			if (s.tick % every != 0)
				return;
			Frame f;
			f.ball = {s.ball->location, s.ball->velocity, s.ball->angularVelocity};
			f.car.location = me->location;
			f.car.rotation = me->rotation;
			f.car.velocity = me->velocity;
			f.car.angularVelocity = me->angularVelocity;
			if (!frames_.empty() && (teleported(frames_.back().ball.location, f.ball.location, f.ball.velocity, every)
				|| teleported(frames_.back().car.location, f.car.location, f.car.velocity, every)
				|| s.tick != lastTick_ + every))
				frames_.clear();
			lastTick_ = s.tick;
			frames_.push_back(f);
			const auto max = static_cast<size_t>(keep_ * hz_) + 1;
			while (frames_.size() > max)
				frames_.pop_front();
		}

		void clear() { frames_.clear(); }

		Track track() const
		{
			Track t;
			t.hz = hz_;
			t.frames.assign(frames_.begin(), frames_.end());
			return t;
		}

	private:
		// Farther than the speed (plus a generous margin for bounces and boost) could carry it in `ticks`.
		static bool teleported(const Vec3& from, const Vec3& to, const Vec3& velocity, uint64_t ticks)
		{
			const float dt = ticks / 120.f;
			return (to - from).length() > (velocity.length() + 2500.f) * dt + 60.f;
		}

		double keep_;
		double hz_;
		uint64_t lastTick_ = 0;
		std::deque<Frame> frames_;
	};
}
