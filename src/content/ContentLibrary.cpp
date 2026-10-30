#include "pch.h"
#include "content/ContentLibrary.h"

#include <algorithm>
#include <fstream>

#include "content/JsonSchema.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace
{
	struct Folder
	{
		ContentLibrary::Kind kind;
		const char* dir;
		const char* schema;
	};

	const Folder kFolders[] = {
		{ContentLibrary::Kind::Scenario, "scenarios", "scenario.schema.json"},
		{ContentLibrary::Kind::Scenario, "captured", "scenario.schema.json"}, // ft_capture's moments
		{ContentLibrary::Kind::Session, "sessions", "session.schema.json"},
		{ContentLibrary::Kind::Combo, "combos", "combo.schema.json"},
		{ContentLibrary::Kind::Horse, "horse", "horse.schema.json"},
	};

	bool readJson(const fs::path& file, json& out, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
		{
			error = "cannot open";
			return false;
		}
		try
		{
			out = json::parse(in, nullptr, true, false);
			return true;
		}
		catch (const json::parse_error& e)
		{
			error = std::string("not valid JSON: ") + e.what();
			return false;
		}
	}

	std::string relPath(const fs::path& file, const fs::path& root)
	{
		std::error_code ec;
		auto rel = fs::relative(file, root, ec);
		return (ec ? file : rel).generic_string();
	}
}

const char* ContentLibrary::kindName(Kind kind)
{
	switch (kind)
	{
	case Kind::Scenario: return "scenario";
	case Kind::Session: return "session";
	case Kind::Combo: return "combo";
	case Kind::Horse: return "horse";
	}
	return "?";
}

std::map<std::string, ContentLibrary::Item>& ContentLibrary::table(Kind kind)
{
	switch (kind)
	{
	case Kind::Session: return sessions_;
	case Kind::Combo: return combos_;
	case Kind::Horse: return horse_;
	default: return scenarios_;
	}
}

const std::map<std::string, ContentLibrary::Item>& ContentLibrary::table(Kind kind) const
{
	return const_cast<ContentLibrary*>(this)->table(kind);
}

ContentLibrary::Report ContentLibrary::load(const fs::path& root)
{
	Report report;
	report.root = root;
	root_ = root;

	std::map<std::string, Item> loaded[4];

	for (const Folder& folder : kFolders)
	{
		const fs::path schemaFile = root / "schema" / folder.schema;
		json schemaDoc;
		std::string error;
		JsonSchema schema;
		if (!readJson(schemaFile, schemaDoc, error) || !schema.load(std::move(schemaDoc), error))
		{
			report.errors.push_back("schema/" + std::string(folder.schema) + ": " + error + "; nothing loaded");
			return report; // keep the previous library
		}

		const fs::path dir = root / folder.dir;
		std::error_code ec;
		if (!fs::is_directory(dir, ec))
			continue;

		std::vector<fs::path> files;
		for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
		{
			if (entry.is_regular_file() && entry.path().extension() == ".json")
				files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());

		auto& table = loaded[static_cast<int>(folder.kind)];
		for (const auto& file : files)
		{
			++report.filesChecked;
			const std::string rel = relPath(file, root);
			json doc;
			if (!readJson(file, doc, error))
			{
				report.errors.push_back(rel + ": " + error);
				continue;
			}
			std::vector<std::string> problems;
			schema.validate(doc, problems);
			if (!problems.empty())
			{
				for (const auto& p : problems)
					report.errors.push_back(rel + ": " + p);
				continue;
			}

			Item item;
			item.kind = folder.kind;
			item.id = doc["id"].get<std::string>();
			item.name = doc.value("name", item.id);
			item.category = doc.value("category", "");
			item.file = file;
			item.data = std::move(doc);

			if (const auto it = table.find(item.id); it != table.end())
			{
				report.errors.push_back(rel + ": duplicate id '" + item.id + "' (already in "
					+ relPath(it->second.file, root) + ")");
				continue;
			}
			table.emplace(item.id, std::move(item));
		}
	}

	scenarios_ = std::move(loaded[static_cast<int>(Kind::Scenario)]);
	sessions_ = std::move(loaded[static_cast<int>(Kind::Session)]);
	combos_ = std::move(loaded[static_cast<int>(Kind::Combo)]);
	horse_ = std::move(loaded[static_cast<int>(Kind::Horse)]);
	report.scenarios = static_cast<int>(scenarios_.size());
	report.sessions = static_cast<int>(sessions_.size());
	report.combos = static_cast<int>(combos_.size());
	report.horse = static_cast<int>(horse_.size());

	// Cross-references: sessions, combo steps and HORSE shots point at scenarios.
	for (const auto& [id, s] : sessions_)
	{
		for (const auto& ex : s.data["exercises"])
			checkRef("session " + id, ex["ref"].get<std::string>(), report.warnings);
	}
	for (const auto& [id, c] : combos_)
	{
		for (const auto& step : c.data["steps"])
			checkRef("combo " + id, step["setup"].get<std::string>(), report.warnings);
	}
	for (const auto& [id, h] : horse_)
	{
		for (const auto& shot : h.data["shots"])
			checkRef("horse " + id, shot["setup"].get<std::string>(), report.warnings);
	}
	return report;
}

bool ContentLibrary::add(Kind kind, json doc, const fs::path& file, std::string& error)
{
	const Folder* folder = nullptr;
	for (const Folder& f : kFolders)
	{
		if (f.kind == kind)
		{
			folder = &f;
			break;
		}
	}
	json schemaDoc;
	JsonSchema schema;
	if (!folder || !readJson(root_ / "schema" / folder->schema, schemaDoc, error) || !schema.load(std::move(schemaDoc), error))
	{
		error = "no schema: " + error;
		return false;
	}
	std::vector<std::string> problems;
	schema.validate(doc, problems);
	if (!problems.empty())
	{
		error = problems.front();
		return false;
	}
	Item item;
	item.kind = kind;
	item.id = doc["id"].get<std::string>();
	item.name = doc.value("name", item.id);
	item.category = doc.value("category", "");
	item.file = file;
	item.data = std::move(doc);
	auto& t = table(kind);
	if (t.count(item.id))
	{
		error = "duplicate id '" + item.id + "'";
		return false;
	}
	t.emplace(item.id, std::move(item));
	return true;
}

void ContentLibrary::checkRef(const std::string& owner, const std::string& ref, std::vector<std::string>& warnings) const
{
	if (ref.rfind("mode:", 0) == 0)
		return; // modes are code, not content
	if (matchScenarios(ref).empty())
		warnings.push_back(owner + ": '" + ref + "' matches no scenario");
}

const ContentLibrary::Item* ContentLibrary::find(Kind kind, const std::string& id) const
{
	const auto& t = table(kind);
	const auto it = t.find(id);
	return it == t.end() ? nullptr : &it->second;
}

std::vector<const ContentLibrary::Item*> ContentLibrary::matchScenarios(const std::string& ref) const
{
	std::vector<const Item*> out;
	if (!ref.empty() && ref.back() == '*')
	{
		const std::string prefix = ref.substr(0, ref.size() - 1);
		for (auto it = scenarios_.lower_bound(prefix); it != scenarios_.end() && it->first.rfind(prefix, 0) == 0; ++it)
			out.push_back(&it->second);
	}
	else if (const auto it = scenarios_.find(ref); it != scenarios_.end())
	{
		out.push_back(&it->second);
	}
	return out;
}

std::vector<const ContentLibrary::Item*> ContentLibrary::all(Kind kind) const
{
	std::vector<const Item*> out;
	for (const auto& [id, item] : table(kind))
		out.push_back(&item);
	return out;
}
