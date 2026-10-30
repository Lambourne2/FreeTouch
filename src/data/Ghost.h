#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// Ghosts (Phase 5): a race run recorded as the car's pose over time, kept for the personal best and drawn as a
// see-through car on later runs so the player races their own best. settings/ghosts/<exercise|difficulty>.json.
// No SDK calls.
namespace ghost
{
	struct Frame
	{
		float t = 0.f;   // seconds since the run started
		Vec3 location;
		Rot rotation;
	};

	struct Run
	{
		std::vector<Frame> frames;
		float seconds() const { return frames.empty() ? 0.f : frames.back().t; }
		void clear() { frames.clear(); }
		void add(float t, const Vec3& location, const Rot& rotation) { frames.push_back({t, location, rotation}); }
	};

	inline int lerpAngle(int a, int b, float u)
	{
		int d = (b - a) % 65536;
		if (d > 32768) d -= 65536;
		if (d < -32768) d += 65536;
		int r = a + static_cast<int>(std::lround(d * u));
		if (r >= 32768) r -= 65536;
		if (r < -32768) r += 65536;
		return r;
	}

	// The pose `t` seconds into the run (held at its ends).
	inline Frame sample(const Run& run, float t)
	{
		if (run.frames.empty())
			return {};
		if (t <= run.frames.front().t)
			return run.frames.front();
		if (t >= run.frames.back().t)
			return run.frames.back();
		const auto it = std::lower_bound(run.frames.begin(), run.frames.end(), t, [](const Frame& f, float v) { return f.t < v; });
		const Frame& b = *it;
		const Frame& a = *(it - 1);
		const float u = b.t > a.t ? (t - a.t) / (b.t - a.t) : 1.f;
		Frame f;
		f.t = t;
		f.location = a.location + (b.location - a.location) * u;
		f.rotation = {lerpAngle(a.rotation.pitch, b.rotation.pitch, u), lerpAngle(a.rotation.yaw, b.rotation.yaw, u),
			lerpAngle(a.rotation.roll, b.rotation.roll, u)};
		return f;
	}

	// The car's nose, right side and roof directions from its rotation (RLBot / RLUtilities' conversion).
	inline void basis(const Rot& r, Vec3& forward, Vec3& right, Vec3& up)
	{
		const float k = 3.14159265f / 32768.f;
		const float cp = std::cos(r.pitch * k), sp = std::sin(r.pitch * k);
		const float cy = std::cos(r.yaw * k), sy = std::sin(r.yaw * k);
		const float cr = std::cos(r.roll * k), sr = std::sin(r.roll * k);
		forward = Vec3(cp * cy, cp * sy, sp);
		right = Vec3(cy * sp * sr - cr * sy, sy * sp * sr + cr * cy, -cp * sr);
		up = Vec3(-cr * cy * sp - sr * sy, -cr * sy * sp + sr * cy, cp * cr);
	}

	// The 8 corners of an Octane-sized box (118 x 84 x 36 uu, its centre a little above the car's origin).
	inline std::vector<Vec3> boxCorners(const Frame& f)
	{
		Vec3 fw, rt, up;
		basis(f.rotation, fw, rt, up);
		const Vec3 c = f.location + up * 4.f + fw * 14.f;
		std::vector<Vec3> out;
		for (int i = 0; i < 8; ++i)
		{
			const float sx = (i & 1) ? 59.f : -59.f, sy = (i & 2) ? 42.f : -42.f, sz = (i & 4) ? 18.f : -18.f;
			out.push_back(c + fw * sx + rt * sy + up * sz);
		}
		return out;
	}

	inline nlohmann::json toJson(const Run& run)
	{
		nlohmann::json rows = nlohmann::json::array();
		auto r1 = [](float v) { return std::round(v * 10.f) / 10.f; };
		for (const auto& f : run.frames)
			rows.push_back({std::round(f.t * 1000.f) / 1000.f, r1(f.location.x), r1(f.location.y), r1(f.location.z), f.rotation.pitch,
				f.rotation.yaw, f.rotation.roll});
		return {{"version", 1}, {"seconds", run.seconds()}, {"frames", rows}};
	}

	inline Run fromJson(const nlohmann::json& j)
	{
		Run run;
		if (!j.is_object() || !j.contains("frames") || !j["frames"].is_array())
			return run;
		for (const auto& row : j["frames"])
		{
			if (!row.is_array() || row.size() != 7)
				continue;
			run.add(row[0].get<float>(), {row[1].get<float>(), row[2].get<float>(), row[3].get<float>()},
				{row[4].get<int>(), row[5].get<int>(), row[6].get<int>()});
		}
		return run;
	}
}
