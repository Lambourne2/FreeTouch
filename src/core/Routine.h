#pragma once

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

// The Routine page: the player's day of Rocket League as sections (by default Warm-up, Play, After) of ordered steps,
// and each day's log of it. Most of it happens outside Free Touch (replay review, ranked games, training packs); Free
// Touch runs the drill blocks and keeps the checklist, tallies and notes. Saved in settings/routine.json and
// settings/journal.json on this PC only. New players start from a template (content/routines/*.json). No SDK calls.
namespace routine
{
	struct Drill
	{
		std::string id;          // scenario id
		int reps = 5;
		std::string difficulty;  // "" = the block's
	};

	struct Item
	{
		std::string name;        // training pack / workshop map name
		std::string code;        // training pack code (Play brings the pack into Free Touch the first time, then plays it)
	};

	struct Section
	{
		std::string id;          // "w1", "w2", ...
		std::string title;       // "Warm-up", "Play", "After"
		bool warmup = false;     // Home's Warm-up button runs this section's drill blocks
	};

	struct Step
	{
		std::string id;          // stable key for the day logs ("s1", "s2", ...)
		std::string section;     // its section's id
		std::string type;        // drills | freeplay | packs | workshop | task | notes
		std::string title;
		std::string detail;      // a line under the title (a link is shown with a Copy button)
		int minutes = 0;         // planned time (0 = not set); sections show their total
		bool optional = false;
		int count = 0;           // task: a tally to reach (3 games); 0 = a single tick
		bool notes = false;      // task: a notes box
		std::string notesHint;   // e.g. "Mistakes -> fixes"
		std::vector<Item> items; // packs / workshop
		// drills
		std::vector<Drill> drills;
		std::string difficulty = "medium";
		bool shuffle = false;
		std::string airRoll = "free";
		// freeplay (Freeplay+ for `minutes`): its objectives (empty = the Freeplay+ page's) and boost ("" = the Freeplay+
		// page's, unlimited, standard)
		std::vector<std::string> objectives;
		std::string boost;
	};

	struct Routine
	{
		std::string name = "My routine";
		bool focus = true;       // a "Focus for today" box at the top
		std::vector<Section> sections;
		std::vector<Step> steps; // in order; each belongs to one section
		bool empty() const { return sections.empty() && steps.empty(); }
	};

	// One step of the routine as it was on a day (the journal shows it; the routine may change later).
	struct PlanStep
	{
		std::string id, section, title, type;
		bool optional = false;
		int count = 0;
	};

	struct DayLog
	{
		std::string focus;
		std::map<std::string, bool> done;        // step id -> ticked
		std::map<std::string, int> tally;        // step id -> count so far
		std::map<std::string, std::string> notes; // step id -> text
		std::vector<PlanStep> plan;              // the routine that day (saved with the day; empty in older logs)
	};

	// A starting point offered to new players (content/routines/<id>.json).
	struct Template
	{
		std::string id;
		std::string name;
		std::string description;
		Routine routine;
	};

	inline const char* kTypes[] = {"drills", "freeplay", "packs", "workshop", "task", "notes"};
	inline constexpr const char* kFreeplayScenario = "speed_lab.objectives";
	inline int freeplayMinutes(const Step& s) { return s.minutes > 0 ? s.minutes : 10; }

	inline std::string newStepId(const Routine& r)
	{
		for (int n = 1;; ++n)
		{
			const std::string id = "s" + std::to_string(n);
			if (std::none_of(r.steps.begin(), r.steps.end(), [&](const Step& s) { return s.id == id; }))
				return id;
		}
	}

	inline std::string newSectionId(const Routine& r)
	{
		for (int n = 1;; ++n)
		{
			const std::string id = "w" + std::to_string(n);
			if (std::none_of(r.sections.begin(), r.sections.end(), [&](const Section& s) { return s.id == id; }))
				return id;
		}
	}

	inline Step makeStep(const Routine& r, const std::string& type, const std::string& section)
	{
		Step s;
		s.id = newStepId(r);
		s.section = section;
		s.type = type;
		if (type == "freeplay")
			s.minutes = 10;
		s.title = type == "drills" ? "Free Touch drills" : type == "freeplay" ? "Freeplay+" : type == "packs" ? "Training packs"
			: type == "workshop" ? "Workshop map" : type == "notes" ? "Notes on the day" : "New task";
		return s;
	}

	inline Section makeSection(const Routine& r, const std::string& title)
	{
		return {newSectionId(r), title, false};
	}

	// The section Home's Warm-up button runs: the one marked, else the first.
	inline const Section* warmupSection(const Routine& r)
	{
		for (const auto& s : r.sections)
			if (s.warmup)
				return &s;
		return r.sections.empty() ? nullptr : &r.sections.front();
	}

	// A step counts as done when ticked, or (a tally) when the count is reached.
	inline bool isDone(const Step& s, const DayLog& d)
	{
		const auto t = d.done.find(s.id);
		if (t != d.done.end() && t->second)
			return true;
		if (s.count > 0)
		{
			const auto c = d.tally.find(s.id);
			return c != d.tally.end() && c->second >= s.count;
		}
		return false;
	}

	// Steps done out of the steps that count (optional ones only count once done), in one section or ("") all.
	inline std::pair<int, int> progress(const Routine& r, const DayLog& d, const std::string& section = "")
	{
		int done = 0, total = 0;
		for (const auto& s : r.steps)
		{
			if (!section.empty() && s.section != section)
				continue;
			const bool ok = isDone(s, d);
			if (!s.optional || ok)
				++total;
			if (ok)
				++done;
		}
		return {done, total};
	}

	inline int plannedMinutes(const Routine& r, const std::string& section)
	{
		int total = 0;
		for (const auto& s : r.steps)
			if (s.section == section)
				total += s.minutes;
		return total;
	}

	// The drill block as a session ("routine.<step id>"): the list once, in order (or shuffled), each for its reps.
	inline std::string sessionId(const Step& s) { return "routine." + s.id; }
	inline nlohmann::json toSession(const Routine& r, const Step& s)
	{
		if (s.type == "freeplay")
		{
			// Freeplay+ for the step's minutes: it ends on time (and ticks the step).
			return {
				{"id", sessionId(s)},
				{"name", s.title.empty() ? std::string("Freeplay+") : s.title},
				{"description", "Freeplay+ from your routine. Edit it on the Routine page; Free Touch rewrites it."},
				{"category", "speed_lab"},
				{"durationMinutes", freeplayMinutes(s)},
				{"difficulty", s.difficulty},
				{"airRoll", "free"},
				{"order", "sequential"},
				{"exercises", nlohmann::json::array({{{"ref", kFreeplayScenario}}})},
			};
		}
		nlohmann::json exercises = nlohmann::json::array();
		for (const auto& d : s.drills)
		{
			nlohmann::json e = {{"ref", d.id}, {"reps", std::max(1, d.reps)}};
			if (d.difficulty == "easy" || d.difficulty == "medium" || d.difficulty == "hard")
				e["difficulty"] = d.difficulty;
			exercises.push_back(std::move(e));
		}
		return {
			{"id", sessionId(s)},
			{"name", s.title.empty() ? r.name : s.title},
			{"description", "A drill block of your routine. Edit it on the Routine page; Free Touch rewrites it."},
			{"category", "quick_play"},
			{"durationMinutes", 120},
			{"difficulty", s.difficulty},
			{"airRoll", s.airRoll},
			{"order", s.shuffle ? "shuffle" : "sequential"},
			{"endAfterList", true},
			{"exercises", exercises},
		};
	}

	inline nlohmann::json toJson(const Routine& r)
	{
		nlohmann::json sections = nlohmann::json::array();
		for (const auto& s : r.sections)
			sections.push_back({{"id", s.id}, {"title", s.title}, {"warmup", s.warmup}});
		nlohmann::json steps = nlohmann::json::array();
		for (const auto& s : r.steps)
		{
			nlohmann::json j = {{"id", s.id}, {"section", s.section}, {"type", s.type}, {"title", s.title}};
			if (!s.detail.empty()) j["detail"] = s.detail;
			if (s.minutes > 0) j["minutes"] = s.minutes;
			if (s.optional) j["optional"] = true;
			if (s.count > 0) j["count"] = s.count;
			if (s.notes) j["notes"] = true;
			if (!s.notesHint.empty()) j["notesHint"] = s.notesHint;
			if (!s.items.empty())
			{
				auto& items = j["items"] = nlohmann::json::array();
				for (const auto& it : s.items)
					items.push_back({{"name", it.name}, {"code", it.code}});
			}
			if (s.type == "drills")
			{
				auto& drills = j["drills"] = nlohmann::json::array();
				for (const auto& d : s.drills)
					drills.push_back({{"id", d.id}, {"reps", d.reps}, {"difficulty", d.difficulty}});
				j["difficulty"] = s.difficulty;
				j["shuffle"] = s.shuffle;
				j["airRoll"] = s.airRoll;
			}
			if (s.type == "freeplay")
			{
				j["difficulty"] = s.difficulty;
				j["objectives"] = s.objectives;
				if (!s.boost.empty())
					j["boost"] = s.boost;
			}
			steps.push_back(std::move(j));
		}
		return {{"version", 2}, {"name", r.name}, {"focus", r.focus}, {"sections", sections}, {"steps", steps}};
	}

	inline std::string lowerCopy(std::string s)
	{
		for (auto& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	// Also reads version 1 files (no sections): those are split at the first step about 2v2s (Lambourne's routine:
	// everything before the 2v2s is the warm-up) into Warm-up / Play / After, else all go into Warm-up.
	inline Routine fromJson(const nlohmann::json& j)
	{
		Routine r;
		r.name = j.value("name", r.name);
		if (r.name == "My warm-up")
			r.name = "My routine";
		r.focus = j.value("focus", true);
		for (const auto& js : j.value("sections", nlohmann::json::array()))
		{
			Section s{js.value("id", ""), js.value("title", "Section"), js.value("warmup", false)};
			if (s.id.empty() || std::any_of(r.sections.begin(), r.sections.end(), [&](const Section& o) { return o.id == s.id; }))
				s.id = newSectionId(r);
			r.sections.push_back(std::move(s));
		}
		for (const auto& js : j.value("steps", nlohmann::json::array()))
		{
			Step s;
			s.id = js.value("id", "");
			s.section = js.value("section", "");
			s.type = js.value("type", "task");
			if (std::find(std::begin(kTypes), std::end(kTypes), s.type) == std::end(kTypes))
				s.type = "task";
			s.title = js.value("title", "");
			s.detail = js.value("detail", "");
			s.minutes = std::max(0, js.value("minutes", 0));
			s.optional = js.value("optional", false);
			s.count = std::max(0, js.value("count", 0));
			s.notes = js.value("notes", false);
			s.notesHint = js.value("notesHint", "");
			for (const auto& it : js.value("items", nlohmann::json::array()))
				s.items.push_back({it.value("name", ""), it.value("code", "")});
			for (const auto& d : js.value("drills", nlohmann::json::array()))
				s.drills.push_back({d.value("id", ""), std::max(1, d.value("reps", 5)), d.value("difficulty", "")});
			s.difficulty = js.value("difficulty", "medium");
			s.shuffle = js.value("shuffle", false);
			s.airRoll = js.value("airRoll", "free");
			for (const auto& o : js.value("objectives", nlohmann::json::array()))
				if (o.is_string())
					s.objectives.push_back(o.get<std::string>());
			s.boost = js.value("boost", "");
			if (s.id.empty() || std::any_of(r.steps.begin(), r.steps.end(), [&](const Step& o) { return o.id == s.id; }))
				s.id = newStepId(r);
			r.steps.push_back(std::move(s));
		}

		if (r.sections.empty() && !r.steps.empty())
		{
			// Version 1: one list.
			r.sections = {{"w1", "Warm-up", true}, {"w2", "Play", false}, {"w3", "After", false}};
			const auto split = std::find_if(r.steps.begin(), r.steps.end(),
				[](const Step& s) { return lowerCopy(s.title).find("2v2") != std::string::npos; });
			const std::string* where = &r.sections[0].id;
			for (auto it = r.steps.begin(); it != r.steps.end(); ++it)
			{
				if (it == split)
					where = &r.sections[1].id;
				else if (split != r.steps.end() && it > split)
					where = &r.sections[2].id;
				it->section = *where;
			}
			r.sections.erase(std::remove_if(r.sections.begin() + 1, r.sections.end(), [&](const Section& sec) {
				return std::none_of(r.steps.begin(), r.steps.end(), [&](const Step& s) { return s.section == sec.id; });
			}), r.sections.end());
		}
		// Every step in a known section.
		for (auto& s : r.steps)
		{
			if (std::none_of(r.sections.begin(), r.sections.end(), [&](const Section& sec) { return sec.id == s.section; }))
			{
				if (r.sections.empty())
					r.sections.push_back({"w1", "Warm-up", true});
				s.section = r.sections.front().id;
			}
		}
		return r;
	}

	// A template as a fresh routine: new ids, so nothing carries over from an older routine's day logs.
	inline Routine fromTemplate(const Routine& t)
	{
		Routine r;
		r.name = t.name;
		r.focus = t.focus;
		std::map<std::string, std::string> sectionIds;
		for (const auto& s : t.sections)
		{
			Section n = s;
			n.id = newSectionId(r);
			sectionIds[s.id] = n.id;
			r.sections.push_back(n);
		}
		for (const auto& s : t.steps)
		{
			Step n = s;
			n.id = newStepId(r);
			n.section = sectionIds[s.section];
			r.steps.push_back(n);
		}
		return r;
	}

	// The routine's steps as a day's plan: section titles instead of ids, so it reads the same after the routine changes.
	inline std::vector<PlanStep> planOf(const Routine& r)
	{
		std::vector<PlanStep> plan;
		for (const auto& sec : r.sections)
			for (const auto& s : r.steps)
				if (s.section == sec.id)
					plan.push_back({s.id, sec.title, s.title, s.type, s.optional, s.count});
		return plan;
	}

	// Anything worth keeping for the day (a plan alone is not).
	inline bool hasEntries(const DayLog& d)
	{
		return !d.focus.empty() || !d.done.empty() || !d.tally.empty()
			|| std::any_of(d.notes.begin(), d.notes.end(), [](const auto& n) { return !n.second.empty(); });
	}

	inline nlohmann::json toJson(const DayLog& d)
	{
		nlohmann::json j = nlohmann::json::object();
		if (!d.focus.empty()) j["focus"] = d.focus;
		if (!d.done.empty()) j["done"] = d.done;
		if (!d.tally.empty()) j["tally"] = d.tally;
		if (!d.notes.empty()) j["notes"] = d.notes;
		if (!d.plan.empty() && hasEntries(d))
		{
			auto& plan = j["plan"] = nlohmann::json::array();
			for (const auto& p : d.plan)
			{
				nlohmann::json s = {{"id", p.id}, {"section", p.section}, {"title", p.title}, {"type", p.type}};
				if (p.optional) s["optional"] = true;
				if (p.count > 0) s["count"] = p.count;
				plan.push_back(std::move(s));
			}
		}
		return j;
	}

	inline DayLog dayFromJson(const nlohmann::json& j)
	{
		DayLog d;
		d.focus = j.value("focus", "");
		if (j.contains("done") && j["done"].is_object())
			d.done = j["done"].get<std::map<std::string, bool>>();
		if (j.contains("tally") && j["tally"].is_object())
			d.tally = j["tally"].get<std::map<std::string, int>>();
		if (j.contains("notes") && j["notes"].is_object())
			d.notes = j["notes"].get<std::map<std::string, std::string>>();
		for (const auto& p : j.value("plan", nlohmann::json::array()))
			if (p.is_object())
				d.plan.push_back({p.value("id", ""), p.value("section", ""), p.value("title", ""), p.value("type", "task"),
					p.value("optional", false), std::max(0, p.value("count", 0))});
		return d;
	}
}
