#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

// The Progress page's own records, in settings/progress.json on this PC: MMR per playlist over time (read from the
// game's local data, or typed in), lifetime counters (matches won vs the bot, kickoffs won, mechanics landed, replay
// takeovers...) and the day each milestone was reached. No SDK calls.
class ProgressLog
{
public:
	struct MmrSample
	{
		std::string day;    // YYYY-MM-DD
		float mmr = 0.f;
		int tier = 0;       // 0 unranked, 1 Bronze I ... 22 Supersonic Legend
		int division = 0;   // 0..3
		int matches = 0;
		bool manual = false;
	};

	// One sample per playlist per day: a newer reading the same day replaces the older one (the day's last MMR).
	void recordMmr(const std::string& playlist, const std::string& day, float mmr, int tier, int division, int matches,
		bool manual)
	{
		auto& list = mmr_[playlist];
		MmrSample s{day, mmr, tier, division, matches, manual};
		auto it = std::find_if(list.begin(), list.end(), [&](const MmrSample& m) { return m.day == day; });
		if (it != list.end())
		{
			if (it->mmr == mmr && it->tier == tier && it->division == division && it->manual == manual)
				return;
			*it = s;
		}
		else
		{
			list.push_back(s);
			std::sort(list.begin(), list.end(), [](const MmrSample& a, const MmrSample& b) { return a.day < b.day; });
		}
		dirty_ = true;
		++version_;
	}
	const std::vector<MmrSample>& mmr(const std::string& playlist) const
	{
		static const std::vector<MmrSample> kNone;
		const auto it = mmr_.find(playlist);
		return it == mmr_.end() ? kNone : it->second;
	}

	void bump(const std::string& counter, int by = 1)
	{
		counters_[counter] += by;
		dirty_ = true;
		++version_;
	}
	int counter(const std::string& name) const
	{
		const auto it = counters_.find(name);
		return it == counters_.end() ? 0 : it->second;
	}
	const std::map<std::string, int>& counters() const { return counters_; }

	// The first day a milestone was reached; true when it is new.
	bool unlock(const std::string& id, const std::string& day)
	{
		if (unlocked_.count(id))
			return false;
		unlocked_[id] = day;
		dirty_ = true;
		++version_;
		return true;
	}
	std::string unlockedOn(const std::string& id) const
	{
		const auto it = unlocked_.find(id);
		return it == unlocked_.end() ? "" : it->second;
	}

	bool dirty() const { return dirty_; }
	void clearDirty() { dirty_ = false; }
	int version() const { return version_; }

	nlohmann::json toJson() const
	{
		nlohmann::json j;
		j["version"] = 1;
		auto& m = j["mmr"] = nlohmann::json::object();
		for (const auto& [playlist, list] : mmr_)
		{
			auto& arr = m[playlist] = nlohmann::json::array();
			for (const auto& s : list)
				arr.push_back({{"day", s.day}, {"mmr", std::round(s.mmr * 10.f) / 10.f}, {"tier", s.tier}, {"division", s.division},
					{"matches", s.matches}, {"manual", s.manual}});
		}
		j["counters"] = counters_;
		j["milestones"] = unlocked_;
		return j;
	}

	void fromJson(const nlohmann::json& j)
	{
		mmr_.clear();
		counters_.clear();
		unlocked_.clear();
		if (j.contains("mmr") && j["mmr"].is_object())
			for (const auto& [playlist, arr] : j["mmr"].items())
				for (const auto& s : arr)
					mmr_[playlist].push_back({s.value("day", ""), s.value("mmr", 0.f), s.value("tier", 0), s.value("division", 0),
						s.value("matches", 0), s.value("manual", false)});
		if (j.contains("counters") && j["counters"].is_object())
			for (const auto& [k, v] : j["counters"].items())
				if (v.is_number_integer())
					counters_[k] = v.get<int>();
		if (j.contains("milestones") && j["milestones"].is_object())
			for (const auto& [k, v] : j["milestones"].items())
				if (v.is_string())
					unlocked_[k] = v.get<std::string>();
		for (auto& [p, list] : mmr_)
			std::sort(list.begin(), list.end(), [](const MmrSample& a, const MmrSample& b) { return a.day < b.day; });
		dirty_ = false;
		++version_;
	}

	// "Champion II Div III" for a tier (0..22) and division (0..3).
	static std::string rankName(int tier, int division, bool withDivision = true)
	{
		static const char* kGroups[] = {"Bronze", "Silver", "Gold", "Platinum", "Diamond", "Champion", "Grand Champion"};
		static const char* kRoman[] = {"I", "II", "III", "IV"};
		if (tier <= 0)
			return "Unranked";
		if (tier >= 22)
			return "Supersonic Legend";
		const int group = (tier - 1) / 3, sub = (tier - 1) % 3;
		std::string name = std::string(kGroups[std::min(group, 6)]) + " " + kRoman[sub];
		if (withDivision)
			name += std::string(" Div ") + kRoman[std::clamp(division, 0, 3)];
		return name;
	}

private:
	std::map<std::string, std::vector<MmrSample>> mmr_;
	std::map<std::string, int> counters_;
	std::map<std::string, std::string> unlocked_;
	bool dirty_ = false;
	int version_ = 0;
};
