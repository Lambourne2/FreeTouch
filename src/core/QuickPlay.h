#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

// Quick Play: the player's own warm-up, a session JSON saved in <data>/freetouch/settings/quick_play.json (outside
// the content folder, which every build mirrors over). Until it is saved, the warm-up is content's
// quick_play.default. No SDK calls.
struct QuickPlayConfig
{
	std::vector<std::string> exercises; // scenario ids, played in this order (or shuffled)
	double minutes = 15.0;
	std::string difficulty = "medium";  // easy | medium | hard
	std::string airRoll = "free";       // left | right | free
	bool shuffle = false;
	int repsPerExercise = 6;
};

namespace quickplay
{
	constexpr const char* kSessionId = "quick_play.mine";
	constexpr const char* kDefaultSessionId = "quick_play.default";

	inline nlohmann::json toSession(const QuickPlayConfig& c)
	{
		nlohmann::json exercises = nlohmann::json::array();
		for (const auto& id : c.exercises)
			exercises.push_back({{"ref", id}});
		return {
			{"id", kSessionId},
			{"name", "My warm-up"},
			{"description", "Your Quick Play warm-up. Edit it on the Quick Play page; Free Touch rewrites this file."},
			{"category", "quick_play"},
			{"durationMinutes", std::clamp(c.minutes, 1.0, 120.0)},
			{"difficulty", c.difficulty},
			{"airRoll", c.airRoll},
			{"order", c.shuffle ? "shuffle" : "sequential"},
			{"repsPerExercise", std::max(1, c.repsPerExercise)},
			{"exercises", exercises},
		};
	}

	// The enabled plain scenario refs of a session (wildcards and modes are left out: Quick Play lists exercises).
	inline QuickPlayConfig fromSession(const nlohmann::json& s)
	{
		QuickPlayConfig c;
		c.minutes = s.value("durationMinutes", c.minutes);
		c.difficulty = s.value("difficulty", c.difficulty);
		if (c.difficulty != "easy" && c.difficulty != "hard")
			c.difficulty = "medium"; // also "adaptive", which plays as medium for now
		c.airRoll = s.value("airRoll", c.airRoll);
		c.shuffle = s.value("order", "sequential") != "sequential";
		c.repsPerExercise = s.value("repsPerExercise", c.repsPerExercise);
		if (s.contains("exercises"))
		{
			for (const auto& ex : s["exercises"])
			{
				const std::string ref = ex.value("ref", "");
				if (ex.value("enabled", true) && !ref.empty() && ref.back() != '*' && ref.rfind("mode:", 0) != 0)
					c.exercises.push_back(ref);
			}
		}
		return c;
	}
}
