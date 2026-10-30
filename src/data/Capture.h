#pragma once

#include <cctype>
#include <cmath>
#include <string>

#include "data/StateJson.h"
#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// ft_capture: the current moment as a scenario JSON that puts the ball and the player's car back exactly as they
// are, so a moment from freeplay becomes a drill. Other cars are only mentioned in the notes. No SDK calls.
namespace capture
{
	using json = nlohmann::ordered_json;

	// A content id part from free text: lowercase letters, digits, '_', '-' and '.'; anything else becomes '_'.
	inline std::string sanitizeId(const std::string& text)
	{
		std::string out;
		for (const char ch : text)
		{
			const auto c = static_cast<unsigned char>(ch);
			if (std::isalnum(c))
				out += static_cast<char>(std::tolower(c));
			else if (ch == '_' || ch == '-' || ch == '.')
				out += ch;
			else
				out += '_';
		}
		while (!out.empty() && out.front() == '.')
			out.erase(out.begin());
		while (!out.empty() && out.back() == '.')
			out.pop_back();
		return out;
	}

	inline double round1(float v) { return std::round(static_cast<double>(v) * 10.0) / 10.0; }
	inline json vec1(const Vec3& v) { return json::array({round1(v.x), round1(v.y), round1(v.z)}); }

	// The scenario "captured.<name>" for this snapshot, or null if there is no ball or no local car.
	// `stamp` is a readable local time for the description, e.g. "2026-10-12 05:10:31".
	inline json scenarioFromSnapshot(const Snapshot& s, const std::string& name, const std::string& stamp)
	{
		const CarState* me = s.localCar();
		if (!s.ball || !me)
			return nullptr;

		json doc;
		doc["id"] = "captured." + name;
		doc["name"] = "Captured: " + name;
		doc["category"] = "captured";
		doc["description"] = "Captured in freeplay on " + stamp + ". Ball and car start exactly as they were.";
		doc["tags"] = json::array({"captured"});
		if (s.cars.size() > 1)
		{
			doc["notes"] = std::to_string(s.cars.size() - 1)
				+ " other car(s) were on the field; they are not part of the capture (add bots by hand).";
		}
		doc["source"] = {{"kind", "captured"}, {"label", stamp}};
		doc["allowMirror"] = false;
		doc["timeLimitSeconds"] = 10;
		doc["ball"] = {
			{"location", vec1(s.ball->location)},
			{"velocity", vec1(s.ball->velocity)},
			{"angularVelocity", vec1(s.ball->angularVelocity)},
		};
		doc["player"] = {
			{"location", vec1(me->location)},
			{"rotation", statejson::toJson(me->rotation)},
			{"velocity", vec1(me->velocity)},
			{"angularVelocity", vec1(me->angularVelocity)},
			{"boost", std::round(me->boost)},
		};
		doc["success"] = {{"type", "goal"}};
		doc["fail"] = json::array({{{"type", "timeout"}, {"seconds", 10}}});
		return doc;
	}
}
