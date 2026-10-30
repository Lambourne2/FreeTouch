#pragma once

#include <string>
#include <vector>

#include "third_party/nlohmann/json.hpp"

// A small JSON Schema validator for the keywords content/schema/*.json uses:
// type, enum, pattern, required, properties, additionalProperties, items, minItems, maxItems, minimum, maximum,
// oneOf, and local $ref ("#/..."). Annotation keywords ($schema, $id, title, description, default, $defs) are
// ignored. Any other keyword makes load() fail, so a schema can never use a rule that is silently skipped.
// No SDK calls.
class JsonSchema
{
public:
	// Returns false and fills `error` if the schema uses an unsupported keyword or a broken $ref.
	bool load(nlohmann::json schema, std::string& error);

	// Appends one message per problem, each starting with the JSON pointer of the offending value.
	void validate(const nlohmann::json& instance, std::vector<std::string>& errors) const;

private:
	bool check(const nlohmann::json& node, const std::string& where, std::string& error) const;
	void validateNode(const nlohmann::json& schema, const nlohmann::json& value, const std::string& path,
		std::vector<std::string>& errors) const;
	const nlohmann::json* resolve(const std::string& ref) const;

	nlohmann::json root_;
};
