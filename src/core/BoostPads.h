#pragma once

#include <array>

// The 34 boost pads of a standard Soccar field, in RLBot's order (which Necto and Nexto were trained with): x, y, z.
// z > 72 marks a big pad (100 boost, 10 s respawn); the rest are small (12 boost, 4 s). No SDK calls.
namespace boostpads
{
	constexpr int kCount = 34;

	inline const std::array<std::array<float, 3>, kCount>& locations()
	{
		static const std::array<std::array<float, 3>, kCount> pads = {{
			{0.f, -4240.f, 70.f}, {-1792.f, -4184.f, 70.f}, {1792.f, -4184.f, 70.f}, {-3072.f, -4096.f, 73.f},
			{3072.f, -4096.f, 73.f}, {-940.f, -3308.f, 70.f}, {940.f, -3308.f, 70.f}, {0.f, -2816.f, 70.f},
			{-3584.f, -2484.f, 70.f}, {3584.f, -2484.f, 70.f}, {-1788.f, -2300.f, 70.f}, {1788.f, -2300.f, 70.f},
			{-2048.f, -1036.f, 70.f}, {0.f, -1024.f, 70.f}, {2048.f, -1036.f, 70.f}, {-3584.f, 0.f, 73.f},
			{-1024.f, 0.f, 70.f}, {1024.f, 0.f, 70.f}, {3584.f, 0.f, 73.f}, {-2048.f, 1036.f, 70.f},
			{0.f, 1024.f, 70.f}, {2048.f, 1036.f, 70.f}, {-1788.f, 2300.f, 70.f}, {1788.f, 2300.f, 70.f},
			{-3584.f, 2484.f, 70.f}, {3584.f, 2484.f, 70.f}, {0.f, 2816.f, 70.f}, {-940.f, 3310.f, 70.f},
			{940.f, 3308.f, 70.f}, {-3072.f, 4096.f, 73.f}, {3072.f, 4096.f, 73.f}, {-1792.f, 4184.f, 70.f},
			{1792.f, 4184.f, 70.f}, {0.f, 4240.f, 70.f},
		}};
		return pads;
	}

	inline bool isBig(int i) { return locations()[i][2] > 72.f; }

	// The pad nearest to (x, y) within `maxDistance` uu, or -1.
	inline int nearest(float x, float y, float maxDistance = 300.f)
	{
		int best = -1;
		float bestD2 = maxDistance * maxDistance;
		for (int i = 0; i < kCount; ++i)
		{
			const float dx = locations()[i][0] - x, dy = locations()[i][1] - y;
			const float d2 = dx * dx + dy * dy;
			if (d2 < bestD2)
			{
				bestD2 = d2;
				best = i;
			}
		}
		return best;
	}
}
