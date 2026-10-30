#pragma once

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "data/SafeFile.h"
#include "third_party/nlohmann/json.hpp"

// Personal bests for timed drills (seconds, lower is better), per exercise and difficulty, kept in
// <data>/freetouch/settings/bests.json so they survive restarts. Game thread. No SDK calls.
class BestTimes
{
public:
	void load(const std::filesystem::path& file)
	{
		file_ = file;
		best_.clear();
		std::ifstream in(file);
		if (!in)
			return;
		try
		{
			const nlohmann::json doc = nlohmann::json::parse(in); // keep it alive: items() only refers to it
			for (auto& [key, value] : doc.items())
			{
				if (value.is_number())
					best_[key] = value.get<double>();
			}
		}
		catch (const std::exception&)
		{
		}
	}

	static std::string key(const std::string& exerciseId, const std::string& difficulty)
	{
		return exerciseId + "|" + difficulty;
	}

	// The id records are kept under: a scenario whose layout changed bumps "recordVersion", so its old best (and
	// ghost) no longer count. Version 1 (or none) keeps the plain id, so existing records stay.
	static std::string recordId(const std::string& exerciseId, const nlohmann::json& scenario)
	{
		const int v = scenario.is_object() ? scenario.value("recordVersion", 1) : 1;
		return v > 1 ? exerciseId + "@v" + std::to_string(v) : exerciseId;
	}

	// 0 if there is no best yet.
	double best(const std::string& key) const
	{
		const auto it = best_.find(key);
		return it == best_.end() ? 0.0 : it->second;
	}

	// Every record, keyed "exercise|difficulty[|objective]" (times) or "max:mode|exercise|difficulty" (high scores).
	const std::map<std::string, double>& all() const { return best_; }

	// High scores (higher is better: Style Rush points, HORSE shots made), in the same file under "max:" keys.
	double bestMax(const std::string& key) const { return best("max:" + key); }
	bool recordMax(const std::string& key, double value)
	{
		const double old = bestMax(key);
		if (value <= old)
			return false;
		best_["max:" + key] = value;
		save();
		return true;
	}

	// Records a time; returns true (and saves) when it is a new best.
	bool record(const std::string& key, double seconds)
	{
		const double old = best(key);
		if (old > 0.0 && seconds >= old)
			return false;
		best_[key] = seconds;
		save();
		return true;
	}

private:
	void save() const
	{
		if (file_.empty())
			return;
		std::error_code ec;
		std::filesystem::create_directories(file_.parent_path(), ec);
		nlohmann::json doc = nlohmann::json::object();
		for (const auto& [k, v] : best_)
			doc[k] = std::round(v * 1000.0) / 1000.0;
		safefile::write(file_, doc.dump(2)); // never half written
	}

	std::filesystem::path file_;
	std::map<std::string, double> best_;
};
