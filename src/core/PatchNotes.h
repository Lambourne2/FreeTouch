#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

// The patch notes (content/updates/patch_notes.json), newest first: what the menu's Patch notes window shows. By date
// and title, not version number: a release has a different number on BakkesPlugins. No SDK calls.
namespace patchnotes
{
	struct Item
	{
		std::string kind; // new, changed or fixed
		std::string text;
	};

	struct Update
	{
		std::string id; // a new id at the top opens the window once for each player
		std::string date; // YYYY-MM-DD
		std::string title;
		std::vector<Item> items;
	};

	inline std::vector<Update> parse(const nlohmann::json& doc)
	{
		std::vector<Update> out;
		const nlohmann::json updates = doc.value("updates", nlohmann::json::array());
		for (const auto& u : updates)
		{
			Update update;
			update.id = u.value("id", "");
			update.date = u.value("date", "");
			update.title = u.value("title", "");
			const nlohmann::json items = u.value("items", nlohmann::json::array());
			for (const auto& i : items)
				if (const std::string text = i.value("text", ""); !text.empty())
					update.items.push_back({i.value("kind", "new"), text});
			if (!update.id.empty() && !update.items.empty())
				out.push_back(std::move(update));
		}
		return out;
	}

	// Whether the window opens by itself: there's a newest update this player hasn't seen, and they aren't brand new
	// (to a new player every update is old news; the seen mark just moves on).
	inline bool opensByItself(const std::vector<Update>& notes, const std::string& seen, bool firstRun)
	{
		return !notes.empty() && notes.front().id != seen && !firstRun;
	}

	// "3 Nov 2026" from "2026-11-03"; anything else as it is.
	inline std::string niceDate(const std::string& date)
	{
		static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		if (date.size() != 10 || date[4] != '-' || date[7] != '-')
			return date;
		const int month = std::atoi(date.c_str() + 5), day = std::atoi(date.c_str() + 8);
		if (month < 1 || month > 12 || day < 1)
			return date;
		return std::to_string(day) + " " + kMonths[month - 1] + " " + date.substr(0, 4);
	}
}
