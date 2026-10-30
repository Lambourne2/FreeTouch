#include "pch.h"
#include "content/JsonSchema.h"

#include <cmath>
#include <regex>
#include <set>

using nlohmann::json;

namespace
{
	const std::set<std::string> kIgnored = {"$schema", "$id", "title", "description", "default", "$defs", "examples"};
	const std::set<std::string> kSupported = {"type", "enum", "pattern", "required", "properties",
		"additionalProperties", "items", "minItems", "maxItems", "minimum", "maximum", "oneOf", "$ref"};

	std::string pathOrRoot(const std::string& path) { return path.empty() ? "/" : path; }

	std::string escapePointer(const std::string& key)
	{
		std::string out;
		for (char c : key)
		{
			if (c == '~') out += "~0";
			else if (c == '/') out += "~1";
			else out += c;
		}
		return out;
	}

	std::string typeName(const json& v)
	{
		if (v.is_object()) return "object";
		if (v.is_array()) return "array";
		if (v.is_string()) return "string";
		if (v.is_boolean()) return "boolean";
		if (v.is_null()) return "null";
		if (v.is_number_integer() || v.is_number_unsigned()) return "integer";
		return "number";
	}

	bool isType(const json& v, const std::string& type)
	{
		if (type == "object") return v.is_object();
		if (type == "array") return v.is_array();
		if (type == "string") return v.is_string();
		if (type == "boolean") return v.is_boolean();
		if (type == "null") return v.is_null();
		if (type == "number") return v.is_number();
		if (type == "integer")
		{
			if (v.is_number_integer() || v.is_number_unsigned()) return true;
			if (v.is_number_float()) { const double d = v.get<double>(); return std::floor(d) == d; }
		}
		return false;
	}

	std::string shortDump(const json& v)
	{
		std::string s = v.dump();
		return s.size() > 60 ? s.substr(0, 57) + "..." : s;
	}
}

bool JsonSchema::load(json schema, std::string& error)
{
	root_ = std::move(schema);
	return check(root_, "#", error);
}

const json* JsonSchema::resolve(const std::string& ref) const
{
	if (ref.empty() || ref[0] != '#')
		return nullptr;
	try
	{
		const auto pointer = json::json_pointer(ref.substr(1));
		return root_.contains(pointer) ? &root_.at(pointer) : nullptr;
	}
	catch (const std::exception&)
	{
		return nullptr;
	}
}

bool JsonSchema::check(const json& node, const std::string& where, std::string& error) const
{
	if (!node.is_object())
	{
		error = where + ": a schema must be an object";
		return false;
	}
	for (const auto& [key, value] : node.items())
	{
		if (kIgnored.contains(key))
			continue;
		if (!kSupported.contains(key))
		{
			error = where + ": unsupported schema keyword '" + key + "'";
			return false;
		}
	}
	if (node.contains("$ref"))
	{
		if (!node["$ref"].is_string() || !resolve(node["$ref"].get<std::string>()))
		{
			error = where + ": $ref " + shortDump(node["$ref"]) + " does not resolve";
			return false;
		}
	}
	if (node.contains("pattern"))
	{
		try { std::regex re(node["pattern"].get<std::string>()); }
		catch (const std::exception&)
		{
			error = where + ": bad pattern";
			return false;
		}
	}
	if (node.contains("properties"))
	{
		for (const auto& [key, sub] : node["properties"].items())
		{
			if (!check(sub, where + "/properties/" + key, error))
				return false;
		}
	}
	if (node.contains("items") && !check(node["items"], where + "/items", error))
		return false;
	if (node.contains("additionalProperties") && node["additionalProperties"].is_object()
		&& !check(node["additionalProperties"], where + "/additionalProperties", error))
		return false;
	if (node.contains("oneOf"))
	{
		int i = 0;
		for (const auto& sub : node["oneOf"])
		{
			if (!check(sub, where + "/oneOf/" + std::to_string(i++), error))
				return false;
		}
	}
	if (node.contains("$defs"))
	{
		for (const auto& [key, sub] : node["$defs"].items())
		{
			if (!check(sub, where + "/$defs/" + key, error))
				return false;
		}
	}
	return true;
}

void JsonSchema::validate(const json& instance, std::vector<std::string>& errors) const
{
	validateNode(root_, instance, "", errors);
}

void JsonSchema::validateNode(const json& schema, const json& value, const std::string& path,
	std::vector<std::string>& errors) const
{
	if (schema.contains("$ref"))
	{
		if (const json* target = resolve(schema["$ref"].get<std::string>()))
			validateNode(*target, value, path, errors);
	}

	if (schema.contains("type"))
	{
		const json& t = schema["type"];
		bool ok = false;
		if (t.is_string())
			ok = isType(value, t.get<std::string>());
		else
			for (const auto& one : t) ok = ok || isType(value, one.get<std::string>());
		if (!ok)
		{
			errors.push_back(pathOrRoot(path) + ": expected " + (t.is_string() ? t.get<std::string>() : t.dump())
				+ ", got " + typeName(value));
			return; // the remaining keywords would only repeat this
		}
	}

	if (schema.contains("enum"))
	{
		bool found = false;
		for (const auto& option : schema["enum"])
			found = found || option == value;
		if (!found)
			errors.push_back(pathOrRoot(path) + ": " + shortDump(value) + " is not one of " + shortDump(schema["enum"]));
	}

	if (value.is_string() && schema.contains("pattern"))
	{
		const std::regex re(schema["pattern"].get<std::string>());
		if (!std::regex_search(value.get<std::string>(), re))
			errors.push_back(pathOrRoot(path) + ": " + shortDump(value) + " does not match " + schema["pattern"].get<std::string>());
	}

	if (value.is_number())
	{
		const double v = value.get<double>();
		if (schema.contains("minimum") && v < schema["minimum"].get<double>())
			errors.push_back(pathOrRoot(path) + ": " + shortDump(value) + " is below the minimum " + schema["minimum"].dump());
		if (schema.contains("maximum") && v > schema["maximum"].get<double>())
			errors.push_back(pathOrRoot(path) + ": " + shortDump(value) + " is above the maximum " + schema["maximum"].dump());
	}

	if (value.is_array())
	{
		if (schema.contains("minItems") && value.size() < schema["minItems"].get<size_t>())
			errors.push_back(pathOrRoot(path) + ": needs at least " + schema["minItems"].dump() + " items, has " + std::to_string(value.size()));
		if (schema.contains("maxItems") && value.size() > schema["maxItems"].get<size_t>())
			errors.push_back(pathOrRoot(path) + ": allows at most " + schema["maxItems"].dump() + " items, has " + std::to_string(value.size()));
		if (schema.contains("items"))
		{
			for (size_t i = 0; i < value.size(); ++i)
				validateNode(schema["items"], value[i], path + "/" + std::to_string(i), errors);
		}
	}

	if (value.is_object())
	{
		if (schema.contains("required"))
		{
			for (const auto& key : schema["required"])
			{
				if (!value.contains(key.get<std::string>()))
					errors.push_back(pathOrRoot(path) + ": missing required '" + key.get<std::string>() + "'");
			}
		}
		const json* props = schema.contains("properties") ? &schema["properties"] : nullptr;
		for (const auto& [key, sub] : value.items())
		{
			const std::string subPath = path + "/" + escapePointer(key);
			if (props && props->contains(key))
			{
				validateNode((*props)[key], sub, subPath, errors);
			}
			else if (schema.contains("additionalProperties"))
			{
				const json& extra = schema["additionalProperties"];
				if (extra.is_boolean() && !extra.get<bool>())
					errors.push_back(subPath + ": unknown property '" + key + "'");
				else if (extra.is_object())
					validateNode(extra, sub, subPath, errors);
			}
		}
	}

	if (schema.contains("oneOf"))
	{
		int matches = 0;
		for (const auto& option : schema["oneOf"])
		{
			std::vector<std::string> optionErrors;
			validateNode(option, value, path, optionErrors);
			if (optionErrors.empty())
				++matches;
		}
		if (matches != 1)
		{
			errors.push_back(pathOrRoot(path) + ": " + shortDump(value) + (matches == 0
				? " does not match any allowed form (for a number field: a number or a [min, max] pair)"
				: " matches more than one allowed form"));
		}
	}
}
