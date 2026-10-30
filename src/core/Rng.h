#pragma once

#include <cstdint>

// Seeded RNG (SplitMix64). The same seed gives the same sequence on every build and machine, so a rep can be
// reproduced from the seed in the event log. Do not swap in <random> distributions: their output is not portable.
class Rng
{
public:
	explicit Rng(uint64_t seed) : state_(seed) {}

	uint64_t nextU64()
	{
		uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		return z ^ (z >> 31);
	}

	// Uniform in [0, 1).
	double next01() { return static_cast<double>(nextU64() >> 11) * (1.0 / 9007199254740992.0); }

	// Uniform in [lo, hi] (order of lo and hi does not matter).
	double range(double lo, double hi) { return lo + (hi - lo) * next01(); }

	bool coin() { return (nextU64() >> 63) != 0; }

private:
	uint64_t state_;
};
