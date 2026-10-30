#pragma once

#include <array>
#include <cmath>
#include <vector>

#include "bots/policy/NectoObs.h"

// Nexto's observation (a C++ port of Nexto's nexto_obs.py as its RLBot bot runs it). Same 24 values per entity as
// Necto's, but: cars first, then the ball, then the 34 pads; pads give "available" (no timers); and every entity is
// turned into the bot's frame (positions relative to it, x / y rotated by its heading). Uses necto::World as input.
// The action is one of Nexto's 90 table rows (EarlPolicy::actionTable()). No SDK calls.
namespace nexto
{
	constexpr int kEntityDim = 24;
	constexpr int kQueryDim = 32;

	inline necto::Obs build(const necto::World& w, const std::array<float, 8>& previousAction)
	{
		const int players = static_cast<int>(w.cars.size());
		const int n = players + 1 + necto::kPads;
		std::vector<float> kv(static_cast<size_t>(n) * kEntityDim, 0.f);
		auto row = [&](int i) { return kv.data() + static_cast<size_t>(i) * kEntityDim; };
		auto put3 = [](float* r, int at, const Vec3& v) { r[at] = v.x; r[at + 1] = v.y; r[at + 2] = v.z; };

		for (int i = 0; i < players; ++i)
		{
			const necto::Car& c = w.cars[i];
			float* r = row(i);
			r[1] = c.team == 0 ? 1.f : 0.f; // "mate" = blue, "opponent" = orange (swapped below for orange)
			r[2] = c.team == 0 ? 0.f : 1.f;
			put3(r, 5, c.location);
			put3(r, 8, c.velocity);
			put3(r, 11, c.forward);
			put3(r, 14, c.up);
			put3(r, 17, c.angularVelocity);
			r[20] = c.boost;
			r[21] = 0.f; // demoed
			r[22] = c.onGround ? 1.f : 0.f;
			r[23] = c.hasFlip ? 1.f : 0.f;
		}
		row(0)[0] = 1.f; // the bot itself
		float* b = row(players);
		b[3] = 1.f;
		put3(b, 5, w.ball.location);
		put3(b, 8, w.ball.velocity);
		put3(b, 17, w.ball.angularVelocity);
		const auto& pads = necto::padLocations();
		for (int p = 0; p < necto::kPads; ++p)
		{
			float* r = row(players + 1 + p);
			r[4] = 1.f;
			r[5] = pads[p][0];
			r[6] = pads[p][1];
			r[7] = pads[p][2];
			r[20] = pads[p][2] > 72.f ? 1.f : 0.12f;
			r[21] = w.padAvailable[p] ? 1.f : 0.f;
		}
		if (players > 0 && w.cars[0].team == 1)
		{
			for (int i = 0; i < n; ++i)
			{
				float* r = row(i);
				for (int k = 5; k < 20; k += 3)
				{
					r[k] = -r[k];
					r[k + 1] = -r[k + 1];
				}
				std::swap(r[1], r[2]);
			}
		}
		for (int i = 0; i < n; ++i)
		{
			float* r = row(i);
			for (int k = 5; k < 11; ++k)
				r[k] /= 2300.f;
			for (int k = 17; k < 20; ++k)
				r[k] /= 5.5f;
		}

		necto::Obs o;
		o.entities = n;
		o.q.assign(row(0), row(0) + kEntityDim);
		o.q.insert(o.q.end(), previousAction.begin(), previousAction.end());
		// Relative to the bot: positions minus its own, then x / y of every vector turned by its heading.
		const float theta = std::atan2(o.q[11], o.q[12]);
		const float ct = std::cos(theta), st = std::sin(theta);
		for (int i = 0; i < n; ++i)
		{
			float* r = row(i);
			for (int k = 5; k < 8; ++k)
				r[k] -= o.q[k];
			for (int k = 5; k < 20; k += 3)
			{
				const float x = r[k], y = r[k + 1];
				r[k] = ct * x - st * y;
				r[k + 1] = st * x + ct * y;
			}
		}
		o.kv = std::move(kv);
		return o;
	}

	// Row `index` of Nexto's 90-action table -> the 8 controls (throttle, steer, pitch, yaw, roll, jump, boost,
	// handbrake).
	inline std::array<float, 8> tableAction(const std::vector<float>& table, int index)
	{
		std::array<float, 8> a{};
		if (index >= 0 && static_cast<size_t>(index + 1) * 8 <= table.size())
			for (int k = 0; k < 8; ++k)
				a[k] = table[static_cast<size_t>(index) * 8 + k];
		return a;
	}
}
