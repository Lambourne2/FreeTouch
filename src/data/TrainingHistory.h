#pragma once

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

// What the player has trained, for Home: minutes and reps per day (today, the day streak, the last 7 days), and per
// exercise the last time it was played and how it went (Jump back in, Try next). Kept in settings/history.json on this
// PC only. Days are local dates "YYYY-MM-DD" handed in by the caller. No SDK calls.
class TrainingHistory
{
public:
	struct Day
	{
		double seconds = 0.0;
		int reps = 0;
		int successes = 0;
	};
	struct Exercise
	{
		std::string category;
		long long lastPlayed = 0; // unix seconds
		int reps = 0;
		int successes = 0;
		std::string lastLabel;    // GOAL, SAVED, TIME, ...
		int lastOutcome = 0;
	};

	static constexpr double kStreakSeconds = 60.0; // a day counts toward the streak after a minute of training

	// "YYYY-MM-DD" -> the day before / after (`offset` days), or "" if `day` is not a date.
	static std::string shiftDay(const std::string& day, int offset)
	{
		int y = 0, m = 0, d = 0;
		if (std::sscanf(day.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
			return {};
		using namespace std::chrono;
		const year_month_day ymd{year{y}, month{static_cast<unsigned>(m)}, std::chrono::day{static_cast<unsigned>(d)}};
		if (!ymd.ok())
			return {};
		const year_month_day out{sys_days{ymd} + days{offset}};
		char buf[16];
		std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", static_cast<int>(out.year()), static_cast<unsigned>(out.month()),
			static_cast<unsigned>(out.day()));
		return buf;
	}

	// Days since 1970-01-01 for "YYYY-MM-DD" (INT_MIN if it is not a date): differences between days.
	static int dayNumber(const std::string& day)
	{
		int y = 0, m = 0, d = 0;
		if (std::sscanf(day.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
			return INT_MIN;
		using namespace std::chrono;
		const year_month_day ymd{year{y}, month{static_cast<unsigned>(m)}, std::chrono::day{static_cast<unsigned>(d)}};
		if (!ymd.ok())
			return INT_MIN;
		return static_cast<int>(sys_days{ymd}.time_since_epoch().count());
	}

	void addSeconds(const std::string& day, double seconds)
	{
		days_[day].seconds += seconds;
		dirty_ = true;
	}

	// A rep started (it shows under Jump back in even before a result).
	void onPlayed(const std::string& id, const std::string& category, long long now)
	{
		auto& e = exercises_[id];
		e.category = category;
		e.lastPlayed = now;
		dirty_ = true;
		++version_;
	}

	void onResult(const std::string& day, const std::string& id, const std::string& category, long long now,
		int outcome, const std::string& label)
	{
		Day& d = days_[day];
		++d.reps;
		if (outcome > 0)
			++d.successes;
		// Per category too (the Progress page's trends).
		if (!category.empty())
		{
			Day& c = categoryDays_[day][category];
			++c.reps;
			if (outcome > 0)
				++c.successes;
		}
		auto& e = exercises_[id];
		e.category = category;
		e.lastPlayed = now;
		++e.reps;
		if (outcome > 0)
			++e.successes;
		e.lastLabel = label;
		e.lastOutcome = outcome;
		dirty_ = true;
		++version_;
	}

	Day day(const std::string& day) const
	{
		const auto it = days_.find(day);
		return it == days_.end() ? Day{} : it->second;
	}

	// Days in a row with training, ending today (or yesterday, while today has not started yet).
	int streak(const std::string& today) const
	{
		std::string d = trained(today) ? today : shiftDay(today, -1);
		int n = 0;
		while (!d.empty() && trained(d))
		{
			++n;
			d = shiftDay(d, -1);
		}
		return n;
	}

	bool trained(const std::string& day) const { return this->day(day).seconds >= kStreakSeconds; }

	// Most recently played first.
	std::vector<std::string> recent(size_t count) const
	{
		std::vector<std::pair<long long, std::string>> all;
		for (const auto& [id, e] : exercises_)
			if (e.lastPlayed > 0)
				all.push_back({e.lastPlayed, id});
		std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::vector<std::string> out;
		for (size_t i = 0; i < all.size() && i < count; ++i)
			out.push_back(all[i].second);
		return out;
	}

	const Exercise* exercise(const std::string& id) const
	{
		const auto it = exercises_.find(id);
		return it == exercises_.end() ? nullptr : &it->second;
	}

	// The last time anything in `category` was played (0 = never).
	long long categoryLastPlayed(const std::string& category) const
	{
		long long last = 0;
		for (const auto& [id, e] : exercises_)
			if (e.category == category)
				last = std::max(last, e.lastPlayed);
		return last;
	}

	struct Suggestion
	{
		std::string id;
		std::string category;
		int daysAgo = -1; // since the category was last played; -1 = never
	};
	// Try next: from the categories given (id -> its playable exercise ids, in display order), the one left alone
	// longest (never played first); in it, the exercise played least recently.
	Suggestion suggest(const std::vector<std::pair<std::string, std::vector<std::string>>>& categories, long long now) const
	{
		Suggestion best;
		long long bestLast = -1;
		for (const auto& [cat, ids] : categories)
		{
			if (ids.empty())
				continue;
			const long long last = categoryLastPlayed(cat);
			if (bestLast >= 0 && last >= bestLast)
				continue;
			bestLast = last;
			std::string pick = ids.front();
			long long pickLast = LLONG_MAX;
			for (const auto& id : ids)
			{
				const Exercise* e = exercise(id);
				const long long l = e ? e->lastPlayed : 0;
				if (l < pickLast)
				{
					pickLast = l;
					pick = id;
				}
			}
			best.id = pick;
			best.category = cat;
			best.daysAgo = last == 0 ? -1 : static_cast<int>(std::max(0LL, now - last) / 86400);
		}
		return best;
	}

	// Every day trained (or with reps), and each exercise, for the Progress page.
	const std::map<std::string, Day>& allDays() const { return days_; }
	const std::map<std::string, Exercise>& exercises() const { return exercises_; }
	// Every category played on `day` (reps and successes; seconds unused).
	std::map<std::string, Day> categoriesOn(const std::string& day) const
	{
		const auto d = categoryDays_.find(day);
		return d == categoryDays_.end() ? std::map<std::string, Day>{} : d->second;
	}
	// Reps and successes in `category` on `day` (seconds unused).
	Day categoryDay(const std::string& day, const std::string& category) const
	{
		const auto d = categoryDays_.find(day);
		if (d == categoryDays_.end())
			return {};
		const auto c = d->second.find(category);
		return c == d->second.end() ? Day{} : c->second;
	}

	bool dirty() const { return dirty_; }
	void clearDirty() { dirty_ = false; }
	int version() const { return version_; } // changes with every rep result / play

	nlohmann::json toJson() const
	{
		nlohmann::json j;
		j["version"] = 1;
		auto& days = j["days"] = nlohmann::json::object();
		for (const auto& [day, d] : days_)
			days[day] = {{"seconds", std::round(d.seconds * 10.0) / 10.0}, {"reps", d.reps}, {"successes", d.successes}};
		auto& ex = j["exercises"] = nlohmann::json::object();
		for (const auto& [id, e] : exercises_)
			ex[id] = {{"category", e.category}, {"lastPlayed", e.lastPlayed}, {"reps", e.reps}, {"successes", e.successes},
				{"lastLabel", e.lastLabel}, {"lastOutcome", e.lastOutcome}};
		auto& cd = j["categoryDays"] = nlohmann::json::object();
		for (const auto& [day, cats] : categoryDays_)
			for (const auto& [cat, c] : cats)
				cd[day][cat] = {c.reps, c.successes};
		return j;
	}

	void fromJson(const nlohmann::json& j)
	{
		days_.clear();
		exercises_.clear();
		categoryDays_.clear();
		if (j.contains("categoryDays") && j["categoryDays"].is_object())
			for (const auto& [day, cats] : j["categoryDays"].items())
				for (const auto& [cat, c] : cats.items())
					if (c.is_array() && c.size() == 2)
						categoryDays_[day][cat] = {0.0, c[0].get<int>(), c[1].get<int>()};
		if (j.contains("days") && j["days"].is_object())
			for (const auto& [day, d] : j["days"].items())
				days_[day] = {d.value("seconds", 0.0), d.value("reps", 0), d.value("successes", 0)};
		if (j.contains("exercises") && j["exercises"].is_object())
			for (const auto& [id, e] : j["exercises"].items())
				exercises_[id] = {e.value("category", ""), e.value("lastPlayed", 0LL), e.value("reps", 0),
					e.value("successes", 0), e.value("lastLabel", ""), e.value("lastOutcome", 0)};
		dirty_ = false;
		++version_;
	}

private:
	std::map<std::string, Day> days_;
	std::map<std::string, Exercise> exercises_;
	std::map<std::string, std::map<std::string, Day>> categoryDays_; // day -> category -> reps / successes
	bool dirty_ = false;
	int version_ = 0;
};
