#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "data/TrainingHistory.h"
#include "third_party/nlohmann/json.hpp"

// The Daily Challenge: the same ten shots for every Free Touch player each day, like Wordle. The local date alone
// seeds the pick (no network): one drill per slot of content/daily/daily.json and each shot's seed, so everyone
// with the same content gets the same shots. One counted attempt per day; a hit is 100 points plus up to 50 for doing
// it fast; the result is shared as text with a grid of squares. Saved in settings/daily.json. No SDK calls.
namespace daily
{
	struct Slot
	{
		std::string name;                 // "Shooting", "Saves", ...
		std::vector<std::string> drills;  // scenario ids this slot may pick
	};

	struct Pool
	{
		int version = 1;                  // bump when the slots change, so a day's shots are not mixed across versions
		std::string epoch = "2026-10-19"; // Daily #1
		std::string difficulty = "medium";
		std::vector<Slot> slots;

		static Pool fromJson(const nlohmann::json& j)
		{
			Pool p;
			p.version = j.value("version", 1);
			p.epoch = j.value("epoch", p.epoch);
			p.difficulty = j.value("difficulty", p.difficulty);
			for (const auto& s : j.value("slots", nlohmann::json::array()))
			{
				Slot slot;
				slot.name = s.value("name", "");
				for (const auto& d : s.value("drills", nlohmann::json::array()))
					if (d.is_string())
						slot.drills.push_back(d.get<std::string>());
				if (!slot.drills.empty())
					p.slots.push_back(std::move(slot));
			}
			return p;
		}
	};

	struct Shot
	{
		std::string slot;       // the slot's name
		std::string scenarioId;
		std::string difficulty;
		uint32_t seed = 0;
	};

	struct Challenge
	{
		std::string date;       // "YYYY-MM-DD"
		int number = 0;         // Daily #n (1 on the epoch day)
		std::vector<Shot> shots;
	};

	// Day number: 1 on the pool's epoch, counting up (0 or less before it).
	inline int dayNumber(const Pool& pool, const std::string& date)
	{
		const int d = TrainingHistory::dayNumber(date), e = TrainingHistory::dayNumber(pool.epoch);
		if (d == INT_MIN || e == INT_MIN)
			return 0;
		return d - e + 1;
	}

	// A fixed generator (the same on every PC and compiler): FNV-1a for the day's seed, then splitmix64.
	inline uint64_t fnv1a(const std::string& s)
	{
		uint64_t h = 1469598103934665603ull;
		for (unsigned char c : s)
		{
			h ^= c;
			h *= 1099511628211ull;
		}
		return h;
	}
	struct Rng
	{
		uint64_t state;
		uint64_t next()
		{
			uint64_t z = (state += 0x9E3779B97F4A7C15ull);
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			return z ^ (z >> 31);
		}
	};

	// The day's shots: one per slot, in the slots' order (they climb from shooting to mechanics).
	inline Challenge make(const Pool& pool, const std::string& date)
	{
		Challenge c;
		c.date = date;
		c.number = dayNumber(pool, date);
		Rng rng{fnv1a("freetouch-daily|" + std::to_string(pool.version) + "|" + date)};
		for (const auto& slot : pool.slots)
		{
			const uint64_t pick = rng.next();
			const uint64_t seed = rng.next();
			c.shots.push_back({slot.name, slot.drills[static_cast<size_t>(pick % slot.drills.size())], pool.difficulty,
				static_cast<uint32_t>(seed & 0xFFFFFFFFu)});
		}
		return c;
	}

	// How one shot went. Mark: what its square shows.
	enum class Mark { Pending = 0, Fast = 1, Hit = 2, Miss = 3 };

	struct ShotResult
	{
		Mark mark = Mark::Pending;
		int points = 0;
		double seconds = 0.0;   // the rep's time when it was judged
		std::string label;      // GOAL, SAVED, TIME, SKIPPED, ...
	};

	// A hit: 100 points, plus up to 50 for speed (all 50 inside the first 40% of the shot's time, none at the end).
	// A fast hit (25+ bonus) is a green square, a hit a yellow one; a miss is dark and scores nothing.
	inline ShotResult score(bool success, double seconds, double timeLimit, const std::string& label)
	{
		ShotResult r;
		r.seconds = seconds;
		r.label = label;
		if (!success)
		{
			r.mark = Mark::Miss;
			return r;
		}
		int bonus = 0;
		if (timeLimit > 0.0)
		{
			const double frac = std::clamp(seconds / timeLimit, 0.0, 1.0);
			bonus = static_cast<int>(std::lround(50.0 * std::clamp((1.0 - frac) / 0.6, 0.0, 1.0)));
		}
		r.points = 100 + bonus;
		r.mark = bonus >= 25 ? Mark::Fast : Mark::Hit;
		return r;
	}

	// A day's attempt: the challenge's shots and the results so far (one per shot played).
	struct Attempt
	{
		std::string date;
		int number = 0;
		std::vector<ShotResult> results;
		bool done = false;

		int hits() const
		{
			return static_cast<int>(std::count_if(results.begin(), results.end(), [](const ShotResult& r) { return r.mark == Mark::Fast || r.mark == Mark::Hit; }));
		}
		int points() const
		{
			int total = 0;
			for (const auto& r : results)
				total += r.points;
			return total;
		}
	};

	inline std::string groupThousands(int n)
	{
		std::string s = std::to_string(std::abs(n));
		for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
			s.insert(static_cast<size_t>(i), ",");
		return n < 0 ? "-" + s : s;
	}

	// The text to paste to friends, e.g.
	//   Free Touch Daily #12  7/10  1,040
	//   [green][green][dark][yellow][green]
	//   [dark][green][green][yellow][dark]
	// (squares as emoji, five to a row; shots not played yet are white).
	inline std::string shareText(const Attempt& a, int shotCount)
	{
		static const char* kGreen = "\xF0\x9F\x9F\xA9";  // U+1F7E9
		static const char* kYellow = "\xF0\x9F\x9F\xA8"; // U+1F7E8
		static const char* kDark = "\xE2\xAC\x9B";       // U+2B1B
		static const char* kWhite = "\xE2\xAC\x9C";      // U+2B1C
		std::string out = "Free Touch Daily #" + std::to_string(a.number) + "  " + std::to_string(a.hits()) + "/" +
			std::to_string(shotCount) + "  " + groupThousands(a.points());
		for (int i = 0; i < shotCount; ++i)
		{
			if (i % 5 == 0)
				out += "\n";
			const Mark m = i < static_cast<int>(a.results.size()) ? a.results[static_cast<size_t>(i)].mark : Mark::Pending;
			out += m == Mark::Fast ? kGreen : m == Mark::Hit ? kYellow : m == Mark::Miss ? kDark : kWhite;
		}
		return out;
	}

	// settings/daily.json: every day's attempt by date.
	inline nlohmann::json toJson(const Attempt& a)
	{
		nlohmann::json shots = nlohmann::json::array();
		for (const auto& r : a.results)
			shots.push_back({{"mark", static_cast<int>(r.mark)}, {"points", r.points}, {"seconds", std::round(r.seconds * 100.0) / 100.0},
				{"label", r.label}});
		return {{"number", a.number}, {"done", a.done}, {"shots", shots}};
	}

	inline Attempt attemptFromJson(const std::string& date, const nlohmann::json& j)
	{
		Attempt a;
		a.date = date;
		a.number = j.value("number", 0);
		a.done = j.value("done", false);
		for (const auto& s : j.value("shots", nlohmann::json::array()))
		{
			ShotResult r;
			r.mark = static_cast<Mark>(std::clamp(s.value("mark", 3), 0, 3));
			r.points = s.value("points", 0);
			r.seconds = s.value("seconds", 0.0);
			r.label = s.value("label", "");
			a.results.push_back(std::move(r));
		}
		return a;
	}

	// Days in a row with a finished daily, ending today (or yesterday, while today's is not finished yet).
	inline int streak(const std::map<std::string, Attempt>& days, const std::string& today)
	{
		const auto finished = [&](const std::string& d) {
			const auto it = days.find(d);
			return it != days.end() && it->second.done;
		};
		std::string d = finished(today) ? today : TrainingHistory::shiftDay(today, -1);
		int n = 0;
		while (!d.empty() && finished(d))
		{
			++n;
			d = TrainingHistory::shiftDay(d, -1);
		}
		return n;
	}
}
