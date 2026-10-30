#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "data/Capture.h"
#include "third_party/nlohmann/json.hpp"

// Saved Shots, shared: a shot as a small .ftshot file a friend drops into Discord (the file goes on the clipboard),
// and reading one back as a Saved Shot of theirs. Also the shot's extras (a defender in the net, the net's open
// tiles). Files only: no network. No SDK calls.
namespace shotshare
{
	using json = nlohmann::json;

	constexpr const char* kExtension = ".ftshot";
	constexpr int kFormat = 1;
	constexpr uintmax_t kMaxFileBytes = 4u << 20; // a 15 s recording at 60 Hz is about 200 KB
	// What a player added to a saved shot ("extras" in its JSON), the same choices as the Training packs page:
	// a defender in the orange net, and a net target (Lambourne 2026-10-29: "like the training packs one: easy with 4
	// tiles, medium with 6, hard with 8, and impossible, just bigger than the ball").
	struct Extras
	{
		std::string defender;        // "" none, "psyonix", "necto" or "nexto": in the orange net, strength by difficulty
		std::string target;          // "" the whole net, "pick" (open tiles), "random" (one a rep), "moving" (it moves)
		std::string size = "medium"; // easy (4 tiles), medium (6), hard (8), impossible (a hole just bigger than the ball)
		uint32_t open = 0;           // pick: the open tiles of the size's grid (bit per cell; the hole: one)

		bool operator==(const Extras& o) const
		{
			return defender == o.defender && target == o.target && (target.empty() || size == o.size) && open == o.open;
		}
		bool operator!=(const Extras& o) const { return !(*this == o); }
	};

	// The orange net's grid for a tile size: 2 rows of 2, 3 or 4 (cell = row * cols + column, column 0 at x = -893);
	// impossible is a hole at one of the 8 cells' centres.
	inline int colsFor(const std::string& size) { return size == "easy" ? 2 : size == "hard" || size == "impossible" ? 4 : 3; }
	constexpr int kRows = 2;
	inline uint32_t allOpen(const std::string& size) { return (1u << (colsFor(size) * kRows)) - 1; }

	inline bool validDefender(const std::string& who) { return who == "psyonix" || who == "necto" || who == "nexto"; }
	inline bool validTarget(const std::string& t) { return t == "pick" || t == "random" || t == "moving"; }
	inline bool validSize(const std::string& s) { return s == "easy" || s == "medium" || s == "hard" || s == "impossible"; }

	inline int openCount(uint32_t open)
	{
		int n = 0;
		for (int c = 0; c < 32; ++c)
			n += (open >> c) & 1u;
		return n;
	}

	// The lowest open cell (the hole's), or -1.
	inline int firstOpen(uint32_t open)
	{
		for (int c = 0; c < 32; ++c)
			if (open & (1u << c))
				return c;
		return -1;
	}

	// Made consistent: picked tiles inside the grid, every tile open is the whole net, nothing open is the whole net,
	// the hole is one cell.
	inline Extras normalized(Extras e)
	{
		if (!validDefender(e.defender))
			e.defender.clear();
		if (!validSize(e.size))
			e.size = "medium";
		if (!validTarget(e.target))
			e.target.clear();
		if (e.target != "pick")
			e.open = 0;
		else
		{
			e.open &= allOpen(e.size);
			if (e.size == "impossible" && e.open != 0)
				e.open = 1u << firstOpen(e.open);
			if (e.open == 0 || (e.size != "impossible" && e.open == allOpen(e.size)))
			{
				e.target.clear();
				e.open = 0;
			}
		}
		if (e.target.empty())
			e.size = "medium";
		return e;
	}

	inline Extras extrasOf(const json& doc)
	{
		Extras e;
		if (!doc.is_object() || !doc.contains("extras") || !doc["extras"].is_object())
			return e;
		const json& x = doc["extras"];
		e.defender = x.value("defender", "");
		e.target = x.value("target", "");
		e.size = x.value("tileSize", "medium");
		if (x.contains("openTiles") && x["openTiles"].is_array())
		{
			for (const auto& c : x["openTiles"])
				if (c.is_number_integer() && c.get<int>() >= 0 && c.get<int>() < 8)
					e.open |= 1u << c.get<int>();
			if (e.target.empty() && e.open != 0)
				e.target = "pick"; // saved before net targets (2026-10-29): open tiles on the 3 x 2 grid
		}
		return normalized(e);
	}

	// Writes them into a shot's JSON (no "extras" at all when there is nothing).
	inline void setExtras(json& doc, const Extras& in)
	{
		const Extras e = normalized(in);
		json x = json::object();
		if (!e.defender.empty())
			x["defender"] = e.defender;
		if (!e.target.empty())
		{
			x["target"] = e.target;
			x["tileSize"] = e.size;
		}
		if (e.target == "pick")
		{
			json tiles = json::array();
			for (int c = 0; c < 8; ++c)
				if (e.open & (1u << c))
					tiles.push_back(c);
			x["openTiles"] = tiles;
		}
		if (x.empty())
			doc.erase("extras");
		else
			doc["extras"] = x;
	}

	// A short tag for the shot's card ("Hard: 2 of 8 open", "Moving hole"), "" for the whole net.
	inline std::string targetLabel(const Extras& e)
	{
		if (e.target.empty())
			return "";
		const bool hole = e.size == "impossible";
		if (e.target == "moving")
			return hole ? "Moving hole" : "Moving tile (" + std::to_string(colsFor(e.size) * kRows) + ")";
		if (e.target == "random")
			return hole ? "Random hole" : "Random tile (" + std::to_string(colsFor(e.size) * kRows) + ")";
		if (hole)
			return "Hole";
		return std::to_string(openCount(e.open)) + " of " + std::to_string(colsFor(e.size) * kRows) + " open";
	}

	// The fields a shared shot keeps. Everything else (its id, bots, file references, favourite) is left behind.
	inline const char* const kSharedFields[] = {"name", "description", "allowMirror", "timeLimitSeconds", "ball", "player",
		"success", "fail", "leadIn", "extras", "waitForInput", "readFirst", "showTrajectory", "trajectorySeconds"};

	// A short fingerprint of the shot itself (its start, lead-in and extras), so the same shot shared twice is
	// recognised as already imported.
	inline std::string keyOf(const json& scenario)
	{
		json core = json::object();
		for (const char* f : {"ball", "player", "leadIn", "extras"})
			if (scenario.contains(f))
				core[f] = scenario[f];
		uint64_t h = 1469598103934665603ull;
		for (const unsigned char c : core.dump())
		{
			h ^= c;
			h *= 1099511628211ull;
		}
		char buf[17];
		snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
		return buf;
	}

	// The .ftshot file's JSON for a saved shot: who shared it, when, with which version, and the shot.
	inline json shareDoc(const json& scenario, const std::string& by, const std::string& date, const std::string& version)
	{
		json shot = json::object();
		for (const char* f : kSharedFields)
			if (scenario.contains(f))
				shot[f] = scenario[f];
		std::string name = shot.value("name", "Shared shot");
		if (name.rfind("Recorded: ", 0) == 0)
			name = name.substr(10);
		shot["name"] = name;
		json doc = json::object();
		doc["freeTouchShot"] = kFormat;
		doc["name"] = name;
		doc["by"] = by;
		doc["on"] = date;
		doc["version"] = version;
		doc["key"] = keyOf(shot);
		doc["shot"] = shot;
		return doc;
	}

	struct Shared
	{
		std::string name, by, on, key;
		json shot; // the shared fields only
	};

	// A .ftshot file's text, checked: the format, a ball and a car to place. False (and why) if it is not one.
	inline bool readShare(const std::string& text, Shared& out, std::string& error)
	{
		json doc;
		try
		{
			doc = json::parse(text);
		}
		catch (const std::exception&)
		{
			error = "not a Free Touch shot (unreadable)";
			return false;
		}
		if (!doc.is_object() || doc.value("freeTouchShot", 0) != kFormat || !doc.contains("shot") || !doc["shot"].is_object())
		{
			error = "not a Free Touch shot";
			return false;
		}
		const json& in = doc["shot"];
		const auto body = [&](const char* f) {
			return in.contains(f) && in[f].is_object() && in[f].contains("location") && in[f]["location"].is_array()
				&& in[f]["location"].size() == 3;
		};
		if (!body("ball") || !body("player"))
		{
			error = "the shot has no ball or car to place";
			return false;
		}
		out = Shared{};
		for (const char* f : kSharedFields)
			if (in.contains(f))
				out.shot[f] = in[f];
		const auto text1 = [&](const char* f, const char* fallback) {
			return doc.contains(f) && doc[f].is_string() && !doc[f].get<std::string>().empty() ? doc[f].get<std::string>() : std::string(fallback);
		};
		out.name = text1("name", "Shared shot").substr(0, 60);
		out.by = text1("by", "a friend").substr(0, 40);
		out.on = text1("on", "").substr(0, 20);
		out.key = keyOf(out.shot);
		return true;
	}

	// The Saved Shot made from a shared one: id "captured.<file name>", the sharer named in its description.
	inline json importedScenario(const Shared& s, const std::string& fileName, const std::string& date, const std::string& onlineId = "")
	{
		json doc = s.shot;
		doc["id"] = "captured." + fileName;
		doc["name"] = s.name;
		doc["category"] = "captured";
		doc["tags"] = json::array({"captured", "shared"});
		std::string about = "Shared by " + s.by + (s.on.empty() ? "" : " on " + s.on) + ".";
		const std::string was = s.shot.value("description", "");
		if (!was.empty())
			about += " " + was;
		doc["description"] = about;
		doc["source"] = {{"kind", "captured"}, {"label", "shared by " + s.by + ", imported " + date}};
		doc["shared"] = {{"by", s.by}, {"on", s.on}, {"key", s.key}};
		if (!onlineId.empty())
			doc["shared"]["id"] = onlineId; // from the Free Touch service: its plays are counted there
		if (!doc.contains("success"))
			doc["success"] = {{"type", "goal"}};
		if (!doc.contains("timeLimitSeconds"))
			doc["timeLimitSeconds"] = 10;
		return doc;
	}

	// A file name for it in content/captured/ ("shared_" + the name).
	inline std::string importFileName(const Shared& s)
	{
		std::string base = capture::sanitizeId(s.name);
		if (base.empty())
			base = "shot";
		return "shared_" + base.substr(0, 40);
	}

	// A file name for the .ftshot that is shared (the shot's name, readable in Discord).
	inline std::string shareFileName(const std::string& name)
	{
		std::string out;
		for (const char ch : name)
		{
			const auto c = static_cast<unsigned char>(ch);
			if (std::isalnum(c) || ch == '-' || ch == '_')
				out += ch;
			else if (ch == ' ')
				out += '_';
		}
		if (out.empty())
			out = "shot";
		return out.substr(0, 48) + kExtension;
	}
}
