#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

// All JSON content (scenarios, sessions, combo ladders, HORSE shot lists), validated against content/schema/ and
// indexed by id. Files that fail validation are skipped and reported; nothing is half-loaded. No SDK calls.
//
// Layout under the content root:
//   schema/{scenario,session,combo,horse}.schema.json
//   scenarios/**.json   captured/**.json (ft_capture)   sessions/**.json   combos/**.json   horse/**.json
class ContentLibrary
{
public:
	enum class Kind { Scenario, Session, Combo, Horse };

	struct Item
	{
		Kind kind = Kind::Scenario;
		std::string id;
		std::string name;
		std::string category;      // scenarios and sessions
		std::filesystem::path file;
		nlohmann::json data;
	};

	struct Report
	{
		std::filesystem::path root;
		int filesChecked = 0;
		int scenarios = 0, sessions = 0, combos = 0, horse = 0;
		std::vector<std::string> errors;    // file skipped
		std::vector<std::string> warnings;  // loaded, but something looks wrong (e.g. a ref to a missing scenario)
	};

	// Replaces the whole library with what is under `root`. On a schema failure nothing is loaded.
	Report load(const std::filesystem::path& root);

	// Adds one document kept outside the content folder (e.g. the saved Quick Play warm-up), validated against the
	// same schema. Call after load(). Returns false with `error` set if it is invalid or its id is taken.
	bool add(Kind kind, nlohmann::json doc, const std::filesystem::path& file, std::string& error);

	const Item* find(Kind kind, const std::string& id) const;
	// Scenario ids equal to `ref`, or starting with it when `ref` ends in '*'.
	std::vector<const Item*> matchScenarios(const std::string& ref) const;
	std::vector<const Item*> all(Kind kind) const;

	static const char* kindName(Kind kind);

private:
	std::map<std::string, Item>& table(Kind kind);
	const std::map<std::string, Item>& table(Kind kind) const;
	void checkRef(const std::string& owner, const std::string& ref, std::vector<std::string>& warnings) const;

	std::map<std::string, Item> scenarios_, sessions_, combos_, horse_;
	std::filesystem::path root_;
};
