#pragma once

#include <array>
#include <cmath>
#include <map>
#include <vector>

#include "core/BoostPads.h"
#include "game/GameState.h"

// Necto's observation (a C++ port of Necto's necto_obs.py as its RLBot bot runs it), for EarlPolicy: the bot's own
// row plus its last action as the query, and every entity (ball, cars, 34 boost pads) as keys, each 24 values, in the
// bot's team frame (orange sees the field turned around). Stateful like the original: boost and demo timers advance
// by the tick skip on every build. The action heads turn back into controls with parseAction(). No SDK calls.
namespace necto
{
	constexpr int kEntityDim = 24;
	constexpr int kQueryDim = 32; // 24 + the previous action (8)
	constexpr int kPads = boostpads::kCount;

	inline const auto& padLocations() { return boostpads::locations(); }

	struct Car
	{
		uint64_t id = 0;
		int team = 0;          // 0 blue, 1 orange
		Vec3 location, velocity;
		Vec3 forward, up;      // unit vectors (see orientation())
		Vec3 angularVelocity;  // rad/s
		float boost = 0.f;     // 0..1
		bool onGround = false;
		bool hasFlip = false;
	};

	struct World
	{
		BallState ball;
		std::vector<Car> cars;          // the bot first, then its teammates, then opponents (as Necto's bot orders them)
		std::array<bool, kPads> padAvailable{}; // true = the pad can be picked up
	};

	// Forward and up from an Unreal rotator (65536 = a full turn), RLGym's euler_to_rotation on radians.
	inline void orientation(const Rot& r, Vec3& forward, Vec3& up)
	{
		constexpr float k = 3.14159265358979f / 32768.f;
		const float p = r.pitch * k, y = r.yaw * k, ro = r.roll * k;
		const float cp = std::cos(p), sp = std::sin(p), cy = std::cos(y), sy = std::sin(y), cr = std::cos(ro), sr = std::sin(ro);
		forward = {cp * cy, cp * sy, sp};
		up = {-cr * cy * sp - sr * sy, -cr * sy * sp + sr * cy, cp * cr};
	}

	struct Obs
	{
		std::vector<float> q;   // kQueryDim
		std::vector<float> kv;  // entities x kEntityDim
		int entities = 0;
	};

	class ObsBuilder
	{
	public:
		explicit ObsBuilder(int tickSkip = 8) : tickSkip_(tickSkip) {}
		void reset()
		{
			demo_.clear();
			boost_.fill(0.f);
		}

		// `previousAction`: the 8 controls the bot used last (throttle, steer, pitch, yaw, roll, jump, boost, handbrake).
		Obs build(const World& w, const std::array<float, 8>& previousAction)
		{
			const int players = static_cast<int>(w.cars.size());
			const int n = 1 + players + kPads;
			std::vector<float> qkv(static_cast<size_t>(n) * kEntityDim, 0.f);
			auto row = [&](int i) { return qkv.data() + static_cast<size_t>(i) * kEntityDim; };
			auto put3 = [](float* r, int at, const Vec3& v) { r[at] = v.x; r[at + 1] = v.y; r[at + 2] = v.z; };

			float* b = row(0);
			b[3] = 1.f;
			put3(b, 5, w.ball.location);
			put3(b, 8, w.ball.velocity);
			put3(b, 17, w.ball.angularVelocity);
			for (int i = 0; i < players; ++i)
			{
				const Car& c = w.cars[i];
				float* r = row(1 + i);
				r[c.team == 0 ? 1 : 2] = 1.f; // "teammate" = blue, "opponent" = orange (swapped below for orange)
				put3(r, 5, c.location);
				put3(r, 8, c.velocity);
				put3(r, 11, c.forward);
				put3(r, 14, c.up);
				put3(r, 17, c.angularVelocity);
				r[20] = c.boost;
				r[22] = c.onGround ? 1.f : 0.f;
				r[23] = c.hasFlip ? 1.f : 0.f;
				float& demo = demo_[c.id];
				demo = demo <= 0.f ? 3.f : std::max(demo - tickSkip_ / 120.f, 0.f);
				r[21] = demo / 10.f;
			}
			const auto& pads = padLocations();
			for (int p = 0; p < kPads; ++p)
			{
				float* r = row(1 + players + p);
				r[4] = 1.f;
				r[5] = pads[p][0];
				r[6] = pads[p][1];
				r[7] = pads[p][2];
				const bool big = pads[p][2] > 72.f;
				r[20] = 0.12f + 0.88f * (big ? 1.f : 0.f);
				const float avail = w.padAvailable[p] ? 1.f : 0.f;
				if (avail == 1.f && boost_[p] == 0.f)
					boost_[p] = big ? 1.0f : 0.4f;
				boost_[p] *= avail;
				r[21] = boost_[p];
				boost_[p] = std::max(0.f, boost_[p] - tickSkip_ / 1200.f);
			}
			// Normalise: positions and velocities / 2300, spin / 5.5.
			for (int i = 0; i < n; ++i)
			{
				float* r = row(i);
				for (int k = 5; k < 11; ++k)
					r[k] /= 2300.f;
				for (int k = 17; k < 20; ++k)
					r[k] /= 5.5f;
			}

			const int main = 1; // the bot is the first car
			row(main)[0] = 1.f;
			if (players > 0 && w.cars[0].team == 1)
			{
				// Orange: swap the team flags, turn x and y around.
				for (int i = 0; i < n; ++i)
				{
					float* r = row(i);
					std::swap(r[1], r[2]);
					for (int k = 5; k < 20; k += 3)
					{
						r[k] = -r[k];
						r[k + 1] = -r[k + 1];
					}
				}
			}
			Obs o;
			o.entities = n;
			o.q.assign(row(main), row(main) + kEntityDim);
			o.q.insert(o.q.end(), previousAction.begin(), previousAction.end());
			// Keys relative to the bot (position and velocity).
			for (int i = 0; i < n; ++i)
			{
				float* r = row(i);
				for (int k = 5; k < 11; ++k)
					r[k] -= o.q[k];
			}
			o.kv = std::move(qkv);
			return o;
		}

	private:
		int tickSkip_;
		std::map<uint64_t, float> demo_;
		std::array<float, kPads> boost_{};
	};

	// The five heads (throttle 3, steer 3, jump 2, boost 2, handbrake 2) at their best choice -> the 8 controls.
	inline std::array<float, 8> parseAction(const std::vector<float>& logits)
	{
		static const int sizes[5] = {3, 3, 2, 2, 2};
		int pick[5] = {};
		size_t at = 0;
		for (int h = 0; h < 5; ++h)
		{
			int best = 0;
			for (int k = 1; k < sizes[h]; ++k)
				if (at + k < logits.size() && logits[at + k] > logits[at + best])
					best = k;
			pick[h] = best;
			at += sizes[h];
		}
		const float throttle = static_cast<float>(pick[0] - 1), steer = static_cast<float>(pick[1] - 1);
		const float handbrake = static_cast<float>(pick[4]);
		return {throttle, steer, throttle, steer * (1.f - handbrake), steer * handbrake,
			static_cast<float>(pick[2]), static_cast<float>(pick[3]), handbrake};
	}
}
