#include "pch.h"
#include "scenario/ScenarioLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "core/Rng.h"
#include "scenario/BallPredict.h"

using nlohmann::json;

namespace
{
	constexpr double kHalfGravity = 325.0; // |g| / 2, g = -650 uu/s^2
	constexpr double kPi = 3.14159265358979323846;

	bool isRange(const json& v)
	{
		return v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number();
	}

	// Step 1: draw every range, depth-first in key order (nlohmann::json objects iterate sorted by key).
	void resolveRanges(json& node, Rng& rng, const std::string& key)
	{
		if (node.is_array())
		{
			// Lists of 2D points and gap fractions are data, not ranges.
			if (key == "points" || key == "gaps")
				return;
			if (key != "grid" && isRange(node))
			{
				node = rng.range(node[0].get<double>(), node[1].get<double>());
				return;
			}
			for (auto& element : node)
				resolveRanges(element, rng, "");
		}
		else if (node.is_object())
		{
			for (auto& [k, v] : node.items())
				resolveRanges(v, rng, k);
		}
	}

	// Sets root.<dotted path> = value. Array segments must exist; object keys are created.
	bool setPath(json& root, const std::string& path, const json& value)
	{
		json* cur = &root;
		size_t start = 0;
		while (true)
		{
			const size_t dot = path.find('.', start);
			const std::string seg = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
			const bool last = dot == std::string::npos;
			if (cur->is_array())
			{
				char* end = nullptr;
				const long idx = std::strtol(seg.c_str(), &end, 10);
				if (seg.empty() || *end != '\0' || idx < 0 || static_cast<size_t>(idx) >= cur->size())
					return false;
				cur = &(*cur)[static_cast<size_t>(idx)];
			}
			else if (cur->is_object())
			{
				if (!last && !cur->contains(seg))
					return false;
				cur = &(*cur)[seg];
			}
			else
			{
				return false;
			}
			if (last)
			{
				*cur = value;
				return true;
			}
			start = dot + 1;
		}
	}

	Vec3 readVec3(const json& v, const Vec3& fallback = {})
	{
		if (!v.is_array() || v.size() != 3)
			return fallback;
		return {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
	}

	int wrapRot(long long r)
	{
		r %= 65536;
		if (r > 32767) r -= 65536;
		if (r < -32768) r += 65536;
		return static_cast<int>(r);
	}

	Rot readRot(const json& v)
	{
		if (!v.is_array() || v.size() != 3)
			return {};
		return {wrapRot(std::llround(v[0].get<double>())), wrapRot(std::llround(v[1].get<double>())),
			wrapRot(std::llround(v[2].get<double>()))};
	}

	void negateX(json& v)
	{
		if (v.is_array() && !v.empty() && v[0].is_number())
			v[0] = -v[0].get<double>();
	}

	// Mirror across the x = 0 plane. Spin is an axial vector: its y and z flip, x stays.
	void mirrorSpin(json& v)
	{
		if (v.is_array() && v.size() == 3)
		{
			v[1] = -v[1].get<double>();
			v[2] = -v[2].get<double>();
		}
	}

	void mirrorRotation(json& v)
	{
		if (v.is_array() && v.size() == 3)
		{
			v[1] = wrapRot(32768 - std::llround(v[1].get<double>())); // yaw
			v[2] = -v[2].get<double>();                                 // roll
		}
	}

	void mirrorBody(json& body)
	{
		if (!body.is_object())
			return;
		if (body.contains("location")) negateX(body["location"]);
		if (body.contains("velocity")) negateX(body["velocity"]);
		if (body.contains("normal")) negateX(body["normal"]);
		if (body.contains("rotation")) mirrorRotation(body["rotation"]);
		if (body.contains("faceBallOffset") && body["faceBallOffset"].is_number())
			body["faceBallOffset"] = -body["faceBallOffset"].get<double>();
		if (body.contains("angularVelocity")) mirrorSpin(body["angularVelocity"]);
		if (body.contains("aimAt") && body["aimAt"].contains("point")) negateX(body["aimAt"]["point"]);
		if (body.contains("points"))
		{
			for (auto& p : body["points"])
				negateX(p);
		}
	}
}

namespace
{
	// A flat area of the arena that objects can be laid out on in a grid. Cells run along x (columns) and z (rows) on
	// walls, along x and y on the ceiling. `centre` is where the objects' middle plane sits.
	struct Surface
	{
		Vec3 centre;
		Vec3 normal;      // facing the field
		float width = 0.f, height = 0.f;
	};

	bool surfaceNamed(const std::string& name, Surface& s)
	{
		// Goals: the mouth (1786 x 643) just in front of the goal line, so everything happens before a goal counts.
		if (name == "orange_goal") s = {{0.f, 5100.f, 321.5f}, {0.f, -1.f, 0.f}, 1786.f, 643.f};
		else if (name == "blue_goal") s = {{0.f, -5100.f, 321.5f}, {0.f, 1.f, 0.f}, 1786.f, 643.f};
		// Backboards: the back wall above the goal.
		else if (name == "orange_backboard") s = {{0.f, 5110.f, 1250.f}, {0.f, -1.f, 0.f}, 2600.f, 900.f};
		else if (name == "blue_backboard") s = {{0.f, -5110.f, 1250.f}, {0.f, 1.f, 0.f}, 2600.f, 900.f};
		else if (name == "ceiling") s = {{0.f, 0.f, 2034.f}, {0.f, 0.f, -1.f}, 2400.f, 2400.f};
		else return false;
		return true;
	}

	json vecJson(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

	void expandSurfaceGrids(json& objects, Rng& rng)
	{
		json out = json::array();
		for (auto& o : objects)
		{
			Surface s;
			const bool gridOk = o.contains("grid") && o["grid"].is_array() && o["grid"].size() == 2
				&& o["grid"][0].is_number_integer() && o["grid"][1].is_number_integer();
			if (!o.contains("surface") || !gridOk || !surfaceNamed(o["surface"].get<std::string>(), s))
			{
				out.push_back(o);
				continue;
			}
			const int cols = std::max(1, o["grid"][0].get<int>());
			const int rows = std::max(1, o["grid"][1].get<int>());
			const float cw = s.width / cols, ch = s.height / rows;
			const bool ceiling = std::fabs(s.normal.z) > 0.5f;
			auto cellCentre = [&](int k) {
				const float u = -s.width * 0.5f + (k % cols + 0.5f) * cw;
				const float v = -s.height * 0.5f + (k / cols + 0.5f) * ch;
				return ceiling ? Vec3(s.centre.x + u, s.centre.y + v, s.centre.z) : Vec3(s.centre.x + u, s.centre.y, s.centre.z + v);
			};
			const int cells = cols * rows;
			const std::string type = o.value("type", "");
			const std::string id = o.value("id", type);
			json base = o;
			for (const char* k : {"surface", "grid", "cell", "blockRest"})
				base.erase(k);

			if (type == "target")
			{
				int pick = o.value("cell", -1);
				if (pick < 0 || pick >= cells)
					pick = std::min(cells - 1, static_cast<int>(rng.next01() * cells));
				constexpr float kBlockerDepth = 40.f;
				json t = base;
				t["id"] = id;
				t["location"] = vecJson(cellCentre(pick) + s.normal * (kBlockerDepth * 0.5f)); // level with the blockers' front
				t["normal"] = vecJson(s.normal);
				t["size"] = json::array({cw - 8.f, ch - 8.f, 0.f});
				t["cell"] = pick;
				out.push_back(t);
				if (o.value("blockRest", true))
				{
					for (int k = 0; k < cells; ++k)
					{
						if (k == pick)
							continue;
						out.push_back({{"type", "blocker"}, {"id", id + "_block_" + std::to_string(k)},
							{"location", vecJson(cellCentre(k))}, {"normal", vecJson(s.normal)},
							{"size", json::array({cw, ch, kBlockerDepth})}, {"color", "blue"}});
					}
				}
			}
			else if (type == "panel")
			{
				for (int k = 0; k < cells; ++k)
				{
					json p = base;
					p["id"] = id + "_" + std::to_string(k);
					p["location"] = vecJson(cellCentre(k) + s.normal * 2.f);
					p["normal"] = vecJson(s.normal);
					p["size"] = json::array({cw - 12.f, ch - 12.f, 20.f});
					out.push_back(p);
				}
			}
			else
			{
				base["location"] = vecJson(s.centre);
				base["normal"] = vecJson(s.normal);
				out.push_back(base);
			}
		}
		objects = std::move(out);
	}
}

namespace
{
	Vec3 unit(const Vec3& v)
	{
		const float len = v.length();
		return len > 1e-3f ? v * (1.f / len) : Vec3(0.f, 1.f, 0.f);
	}

	// A circle placed flat on a random arena surface from "placeOn" (floor, wall, ceiling, back_wall), away from the
	// curved edges, with its normal pointing into the arena.
	void placeOnSurface(json& o, Rng& rng)
	{
		const json& choices = o["placeOn"];
		const std::string where = choices[std::min(choices.size() - 1, static_cast<size_t>(rng.next01() * choices.size()))];
		const float side = rng.coin() ? 1.f : -1.f;
		const float a = static_cast<float>(rng.next01()), b = static_cast<float>(rng.next01());
		auto lerp = [](float lo, float hi, float t) { return lo + (hi - lo) * t; };
		Vec3 at, n;
		if (where == "wall")
		{
			at = {side * (4096.f - 15.f), lerp(-3600.f, 3600.f, a), lerp(350.f, 1650.f, b)};
			n = {-side, 0.f, 0.f};
		}
		else if (where == "ceiling")
		{
			at = {lerp(-3000.f, 3000.f, a), lerp(-3800.f, 3800.f, b), 2044.f - 15.f};
			n = {0.f, 0.f, -1.f};
		}
		else if (where == "back_wall")
		{
			at = {lerp(-2700.f, 2700.f, a), side * (5120.f - 15.f), lerp(850.f, 1650.f, b)}; // above the goal
			n = {0.f, -side, 0.f};
		}
		else
		{
			at = {lerp(-3400.f, 3400.f, a), lerp(-4300.f, 4300.f, b), 15.f};
			n = {0.f, 0.f, 1.f};
		}
		o["location"] = vecJson(at);
		o["normal"] = vecJson(n);
		o["surface"] = where; // for the log; objects::fromJson ignores it
		o.erase("placeOn");
	}

	// A ring course that flows around the arena from the player's start: floor arches, rings along a side wall, rings
	// on the ceiling and air rings (off a wall or the ceiling). Steps stay short (1000-1500 uu), sideways moves and
	// height changes are gradual, the route only goes up a wall it is already near, and it U-turns at either end in a
	// two-ring arc. Every ring faces along the path through it (from the previous ring toward the next), kept square
	// to its surface, so the course reads as one flowing line.
	void generateRingCourse(const json& spec, const json& player, json& out, Rng& rng)
	{
		const int count = std::clamp(static_cast<int>(std::lround(spec.value("count", 7.0))), 2, 20);
		const float radius = spec.value("radius", 320.f);
		const std::string id = spec.value("id", std::string("ring"));
		Vec3 start(0.f, -4300.f, 17.f);
		if (player.contains("location") && player["location"].is_array())
			start = {player["location"][0].get<float>(), player["location"][1].get<float>(), player["location"][2].get<float>()};
		float dir = start.y < 0.f ? 1.f : -1.f;   // along the field, toward the far end
		float side = start.x >= 0.f ? 1.f : -1.f; // the side wall the route leans to
		constexpr float kEnd = 3800.f, kWallX = 4096.f - 30.f, kCeilingZ = 2044.f - 30.f;
		auto lerp = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(rng.next01()); };

		// Next surface after each surface: floor, wall, ceiling, air.
		auto pick = [&](const std::string& from) {
			const float r = static_cast<float>(rng.next01());
			if (from == "floor") return r < 0.35f ? "floor" : r < 0.8f ? "wall" : "air";
			if (from == "wall") return r < 0.3f ? "wall" : r < 0.7f ? "ceiling" : "air";
			if (from == "ceiling") return r < 0.35f ? "ceiling" : r < 0.8f ? "air" : "wall";
			return r < 0.55f ? "floor" : r < 0.75f ? "air" : "wall"; // after an air ring
		};

		struct Ring { Vec3 at; std::string type; float radius; };
		std::vector<Ring> rings;
		Vec3 prev = start;
		std::string prevType = "floor";
		bool wantWall = false; // the last floor ring swung toward a wall: go up it next
		while (static_cast<int>(rings.size()) < count)
		{
			const float y = prev.y + dir * lerp(1000.f, 1500.f);
			if (std::fabs(y) > kEnd && prevType != "floor")
			{
				// Off a wall, the ceiling or out of the air: come down to the floor, stepping toward the middle (the start
				// of the turn), then turn.
				const float inward = prev.x >= 0.f ? -1.f : 1.f;
				const Vec3 at(std::clamp(prev.x + inward * lerp(1200.f, 1600.f), -2700.f, 2700.f),
					std::clamp(prev.y + dir * 400.f, -3900.f, 3900.f), radius * 0.4f);
				rings.push_back({at, "floor", radius});
				prev = at;
				prevType = "floor";
				wantWall = false;
				continue;
			}
			if (std::fabs(y) > kEnd)
			{
				// U-turn arc: swing toward the middle at the end of the field, then head back down the other half. Near
				// the middle, keep swinging the way the route is already moving sideways (no zig-zag).
				const float lastDx = rings.size() >= 2 ? prev.x - rings[rings.size() - 2].at.x : prev.x - start.x;
				float inward = std::fabs(lastDx) > 600.f ? (lastDx >= 0.f ? 1.f : -1.f) : (prev.x >= 0.f ? -1.f : 1.f);
				if (std::fabs(prev.x + inward * 1300.f) > 2900.f)
					inward = -inward; // no room that way: swing the other way
				const float endY = std::clamp(prev.y + dir * 500.f, -3900.f, 3900.f);
				const Vec3 a(std::clamp(prev.x + inward * lerp(1300.f, 1800.f), -2700.f, 2700.f), endY, radius * 0.4f);
				rings.push_back({a, "floor", radius});
				dir = -dir;
				side = inward;
				if (static_cast<int>(rings.size()) < count)
					rings.push_back({{std::clamp(a.x + inward * lerp(800.f, 1200.f), -2700.f, 2700.f),
						endY + dir * lerp(900.f, 1200.f), radius * 0.4f}, "floor", radius});
				prev = rings.back().at;
				prevType = "floor";
				wantWall = false;
				continue;
			}
			std::string type = rings.empty() ? "floor" : wantWall ? "wall" : pick(prevType);
			wantWall = false;
			// Only go up a wall that is near: otherwise swing toward it on the floor, and go up it next.
			if (type == "wall" && prevType != "wall" && prevType != "ceiling" && std::fabs(prev.x) < 2000.f)
			{
				const float toward = std::fabs(prev.x) < 300.f ? side : (prev.x > 0.f ? 1.f : -1.f);
				const Vec3 at(std::clamp(prev.x + toward * lerp(1300.f, 1800.f), -2900.f, 2900.f), y, radius * 0.4f);
				rings.push_back({at, "floor", radius});
				prev = at;
				prevType = "floor";
				wantWall = std::fabs(at.x) >= 2000.f;
				continue;
			}
			if (type == "wall" && prevType != "wall" && prevType != "ceiling")
				side = prev.x >= 0.f ? 1.f : -1.f;
			Vec3 at;
			float r = radius;
			if (type == "wall")
			{
				const float z = prevType == "wall" ? std::clamp(prev.z + lerp(-350.f, 350.f), 500.f, 1500.f)
					: prevType == "ceiling" ? lerp(1200.f, 1500.f) : lerp(500.f, 900.f);
				at = {side * kWallX, y, z};
			}
			else if (type == "ceiling")
			{
				const float x = prevType == "ceiling" ? std::clamp(prev.x + lerp(-400.f, 400.f), -3200.f, 3200.f)
					: side * lerp(2700.f, 3200.f); // just off the wall you came up
				at = {x, y, kCeilingZ};
			}
			else if (type == "air")
			{
				r = radius + 40.f; // a little more room in the air
				if (prevType == "wall" || prevType == "ceiling")
					at = {side * lerp(2500.f, 3200.f), y, prevType == "ceiling" ? lerp(1000.f, 1450.f) : lerp(600.f, 1200.f)};
				else
					at = {std::clamp(prev.x + lerp(-600.f, 600.f), -2800.f, 2800.f), y, lerp(500.f, 1000.f)};
			}
			else
			{
				// A floor arch, drifting gently; toward the route's wall when it is heading for it.
				float x = std::clamp(prev.x * 0.9f + lerp(-600.f, 600.f), -2800.f, 2800.f);
				if (prevType == "floor" && std::fabs(prev.x) < 2000.f && rng.coin())
					x = std::clamp(prev.x + side * lerp(700.f, 1300.f), -2800.f, 2800.f);
				at = {x, y, radius * 0.4f};
			}
			rings.push_back({at, type, r});
			prev = at;
			prevType = type;
		}

		// Facing along the path through each ring, square to its surface.
		for (size_t i = 0; i < rings.size(); ++i)
		{
			const Vec3 from = i == 0 ? start : rings[i - 1].at;
			const Vec3 in = unit(rings[i].at - from);
			const Vec3 outDir = i + 1 < rings.size() ? unit(rings[i + 1].at - rings[i].at) : in;
			Vec3 n = in + outDir;
			const std::string& type = rings[i].type;
			if (type == "wall")
				n.x = 0.f;             // in the wall's plane
			else if (type == "floor" || type == "ceiling")
				n.z = 0.f;             // upright
			if (n.length() < 1e-3f)
				n = Vec3(0.f, in.y >= 0.f ? 1.f : -1.f, 0.f);
			n = unit(n);
			out.push_back({{"type", "ring"}, {"id", id + "_" + std::to_string(i + 1)}, {"order", static_cast<int>(i)},
				{"location", vecJson(rings[i].at)}, {"normal", vecJson(n)}, {"radius", rings[i].radius},
				{"triggeredBy", "car"}, {"surface", type}, {"color", type == "air" ? "orange" : "accent"}});
		}
	}

	// "one_of": replace it with the objects of one random choice and remember the choice's label.
	void expandOneOf(json& doc, Rng& rng, const std::vector<std::string>& enabled, const std::string& difficulty)
	{
		json out = json::array();
		for (auto& o : doc["objects"])
		{
			if (o.value("type", "") != "one_of" || !o.contains("choices") || o["choices"].empty())
			{
				out.push_back(o);
				continue;
			}
			// Only the choices for this difficulty ("difficulties": [...], none = every difficulty), and of those the
			// enabled ones (by id) if any of them are; with nothing left, the difficulty's choices, then all.
			const auto forDifficulty = [&](const json& c) {
				if (!c.contains("difficulties") || !c["difficulties"].is_array())
					return true;
				for (const auto& d : c["difficulties"])
					if (d.is_string() && d.get<std::string>() == difficulty)
						return true;
				return false;
			};
			std::vector<const json*> pool;
			for (const auto& c : o["choices"])
			{
				if (forDifficulty(c) && (enabled.empty() || std::find(enabled.begin(), enabled.end(), c.value("id", "")) != enabled.end()))
					pool.push_back(&c);
			}
			if (pool.empty())
			{
				for (const auto& c : o["choices"])
					if (forDifficulty(c))
						pool.push_back(&c);
			}
			if (pool.empty())
			{
				for (const auto& c : o["choices"])
					pool.push_back(&c);
			}
			const json& pick = *pool[std::min(pool.size() - 1, static_cast<size_t>(rng.next01() * pool.size()))];
			for (const auto& inner : pick["objects"])
				out.push_back(inner);
			// A choice can bring its own ball and car (e.g. one pinch setup each) and its own rules and clock.
			for (const char* key : {"ball", "player", "success", "fail", "timeLimitSeconds", "guide"})
			{
				if (pick.contains(key))
					doc[key] = pick[key];
			}
			if (pick.contains("id"))
				doc["objectiveId"] = pick["id"];
			if (pick.contains("label"))
				doc["objectiveLabel"] = pick["label"];
			if (pick.contains("tag"))
				doc["objectiveTag"] = pick["tag"];
		}
		doc["objects"] = std::move(out);
	}

	// A Glade course: control points smoothed into a lane on the floor (Catmull-Rom), with gaps to jump, gates along
	// it in order and a finish circle (for the car, or for the ball). The car starts at the first point facing along
	// the path; with a ball finish the ball starts on the path a little ahead of the car.
	void generateGladePath(const json& spec, json& doc, json& out)
	{
		std::vector<Vec3> ctrl;
		for (const auto& p : spec.value("points", json::array()))
			ctrl.push_back(Vec3(p[0].get<float>(), p[1].get<float>(), 0.f));
		if (ctrl.size() < 2)
			return;
		// Dense centre line.
		std::vector<Vec3> line;
		for (size_t i = 0; i + 1 < ctrl.size(); ++i)
		{
			const Vec3 p0 = ctrl[i == 0 ? 0 : i - 1], p1 = ctrl[i], p2 = ctrl[i + 1], p3 = ctrl[std::min(i + 2, ctrl.size() - 1)];
			for (int k = 0; k < 12; ++k)
			{
				const float t = k / 12.f, t2 = t * t, t3 = t2 * t;
				line.push_back((p1 * 2.f + (p2 - p0) * t + (p0 * 2.f - p1 * 5.f + p2 * 4.f - p3) * t2
					+ (p1 * 3.f - p0 - p2 * 3.f + p3) * t3) * 0.5f);
			}
		}
		line.push_back(ctrl.back());
		std::vector<float> at(line.size(), 0.f); // distance along the line
		for (size_t i = 1; i < line.size(); ++i)
			at[i] = at[i - 1] + (line[i] - line[i - 1]).length();
		const float total = at.back();
		auto pointAt = [&](float s, Vec3& tangent) {
			size_t i = 1;
			while (i + 1 < line.size() && at[i] < s)
				++i;
			const float seg = std::max(at[i] - at[i - 1], 1e-3f);
			tangent = unit(line[i] - line[i - 1]);
			return line[i - 1] + (line[i] - line[i - 1]) * std::clamp((s - at[i - 1]) / seg, 0.f, 1.f);
		};
		std::vector<std::pair<float, float>> gaps;
		for (const auto& g : spec.value("gaps", json::array()))
			gaps.push_back({g[0].get<float>() * total, g[1].get<float>() * total});
		auto inGap = [&](float s) {
			for (const auto& [a, b] : gaps)
				if (s > a && s < b)
					return true;
			return false;
		};

		const float width = spec.value("width", 600.f);
		const std::string color = spec.value("color", std::string("white"));
		// Lanes: the centre line split at the gaps.
		json lane = json::array();
		int laneCount = 0;
		auto flush = [&]() {
			if (lane.size() >= 2)
				out.push_back({{"type", "lane"}, {"id", "lane_" + std::to_string(++laneCount)}, {"points", lane},
					{"width", width}, {"color", color}});
			lane = json::array();
		};
		for (size_t i = 0; i < line.size(); ++i)
		{
			if (inGap(at[i]))
			{
				flush();
				continue;
			}
			lane.push_back(vecJson(line[i]));
		}
		flush();

		// Gates in order (not in gaps), then the finish.
		const int gates = std::max(0, static_cast<int>(spec.value("gates", 3)));
		int order = 0;
		for (int g = 1; g <= gates; ++g)
		{
			const float s = total * g / (gates + 1);
			if (inGap(s))
				continue;
			Vec3 tangent;
			const Vec3 p = pointAt(s, tangent);
			out.push_back({{"type", "checkpoint"}, {"id", "gate_" + std::to_string(order + 1)}, {"order", order},
				{"location", vecJson(Vec3(p.x, p.y, 0.f))}, {"normal", vecJson(Vec3(tangent.x, tangent.y, 0.f))},
				{"radius", width * 0.5f + 80.f}, {"triggeredBy", "car"}, {"color", "orange"}});
			++order;
		}
		const bool ballFinish = spec.value("finish", std::string("car")) == "ball";
		out.push_back({{"type", "circle"}, {"id", "finish"}, {"order", order}, {"location", vecJson(Vec3(line.back().x, line.back().y, 15.f))},
			{"radius", ballFinish ? 420.f : width * 0.6f + 100.f}, {"triggeredBy", ballFinish ? "ball" : "car"},
			{"color", ballFinish ? "green" : "accent"}});

		// Start: the car at the first point facing along the path; a ball finish puts the ball just ahead.
		Vec3 tangent;
		pointAt(1.f, tangent);
		const double yaw = std::atan2(tangent.y, tangent.x);
		json& player = doc["player"];
		player["location"] = vecJson(Vec3(line.front().x, line.front().y, 17.f));
		player["rotation"] = json::array({0, static_cast<int>(std::lround(yaw * 32768.0 / 3.14159265358979323846)), 0});
		player.erase("faceBall");
		if (ballFinish)
		{
			const Vec3 b = pointAt(std::min(600.f, total * 0.2f), tangent);
			doc["ball"]["location"] = vecJson(Vec3(b.x, b.y, 93.f));
			doc["ball"]["hidden"] = false;
			doc["ball"].erase("markObjective");
		}
	}

	void expandGenerated(json& doc, Rng& rng)
	{
		json out = json::array();
		for (auto& o : doc["objects"])
		{
			if (o.value("type", "") == "ring_course")
			{
				generateRingCourse(o, doc.value("player", json::object()), out, rng);
				continue;
			}
			if (o.value("type", "") == "glade_path")
			{
				generateGladePath(o, doc, out);
				continue;
			}
			if (o.value("type", "") == "pin_rack")
			{
				// The bowling triangle: the head pin at 'location', rows extending along 'normal' (on the floor).
				const Vec3 head = o.contains("location") ? readVec3(o["location"]) : Vec3(0.f, 0.f, 0.f);
				Vec3 facing = o.contains("normal") ? readVec3(o["normal"]) : Vec3(0.f, 1.f, 0.f);
				facing.z = 0.f;
				facing = unit(facing);
				const Vec3 across(-facing.y, facing.x, 0.f);
				const int rows = std::clamp(o.value("rows", 4), 1, 6);
				const float spacing = o.value("spacing", 300.f);
				const std::string id = o.value("id", std::string("pin"));
				for (int r = 0; r < rows; ++r)
				{
					for (int i = 0; i <= r; ++i)
					{
						const Vec3 at = head + facing * (r * spacing * 0.866f) + across * ((i - r * 0.5f) * spacing);
						out.push_back({{"type", "pin"}, {"id", id + "_" + std::to_string(r + 1) + "_" + std::to_string(i + 1)},
							{"location", vecJson(Vec3(at.x, at.y, 0.f))}, {"triggeredBy", "ball"}, {"color", "white"}});
					}
				}
				continue;
			}
			if (o.contains("placeOn") && o["placeOn"].is_array() && !o["placeOn"].empty())
				placeOnSurface(o, rng);
			out.push_back(o);
		}
		doc["objects"] = std::move(out);
	}
}

Vec3 scenario::aimVelocity(const Vec3& from, const Vec3& to, double speed, bool& reachable)
{
	// p(t) = from + v t + (0, 0, -325 t^2) passes `to` at time t: v = (d + (0, 0, 325 t^2)) / t.
	// |v|^2 = |d|^2 / u + 650 dz + 325^2 u with u = t^2, so 325^2 u^2 + (650 dz - s^2) u + |d|^2 = 0.
	const Vec3 d = to - from;
	const double a = kHalfGravity * kHalfGravity;
	const double b = 2.0 * kHalfGravity * d.z - speed * speed;
	const double c = static_cast<double>(d.dot(d));
	const double disc = b * b - 4.0 * a * c;

	double u = -1.0;
	reachable = disc >= 0.0;
	if (reachable)
	{
		const double r1 = (-b - std::sqrt(disc)) / (2.0 * a); // smaller u: flatter, faster arc
		const double r2 = (-b + std::sqrt(disc)) / (2.0 * a);
		u = r1 > 0.0 ? r1 : r2;
		reachable = u > 0.0;
	}
	if (!reachable)
		u = std::sqrt(c / a); // the lowest-speed arc that reaches the point
	if (u <= 0.0)
		return {};

	const double t = std::sqrt(u);
	return {static_cast<float>(d.x / t), static_cast<float>(d.y / t), static_cast<float>(d.z / t + kHalfGravity * t)};
}

Vec3 scenario::aimVelocityOffSideWall(const Vec3& from, const Vec3& to, double speed, bool& reachable)
{
	constexpr float kWall = ballpredict::kHalfWidth - ballpredict::kRadius; // ball centre at the side wall
	const float side = from.x < 0.f ? -1.f : 1.f;
	Vec3 behind = to;
	behind.x = side * (kWall + (kWall - side * to.x) / ballpredict::kBounce);
	Vec3 v = aimVelocity(from, behind, speed, reachable);

	// The reflection ignores drag and the speed the wall takes along it, so fly the shot with BallPredict's model
	// and move the reflected point until the ball crosses `to`'s y within a few uu of `to`. A ground bounce just
	// before the line can stop it settling, so the closest shot tried wins.
	const float dir = to.y < from.y ? -1.f : 1.f;
	Vec3 best = v;
	bool bestReachable = reachable;
	float bestError = 1e9f;
	for (int i = 0; i < 40; ++i)
	{
		BallState b{from, v, {}};
		bool crossed = false;
		for (int t = 0; t < 120 * 8 && !crossed; ++t)
		{
			ballpredict::step(b, 1.f / 120.f, false);
			crossed = (b.location.y - to.y) * dir >= 0.f;
		}
		if (!crossed)
			break;
		const float ex = b.location.x - to.x, ez = b.location.z - to.z;
		if (std::hypot(ex, ez) < bestError)
		{
			bestError = std::hypot(ex, ez);
			best = v;
			bestReachable = reachable;
		}
		if (bestError < 3.f)
			break;
		const float gain = i < 10 ? 0.8f : 0.4f;
		behind.x += ex * gain / ballpredict::kBounce; // the hit x moves 0.6x the other way on both walls
		behind.z -= ez * gain;
		v = aimVelocity(from, behind, speed, reachable);
	}
	reachable = bestReachable;
	return best;
}

std::string scenario::pendingFeature(const json& data)
{
	auto conditionTypes = [&]() {
		std::vector<std::string> types;
		if (data.contains("success"))
			types.push_back(data["success"].value("type", ""));
		if (data.contains("fail"))
		{
			for (const auto& f : data["fail"])
				types.push_back(f.value("type", ""));
		}
		return types;
	}();
	auto uses = [&](std::initializer_list<const char*> names) {
		return std::any_of(conditionTypes.begin(), conditionTypes.end(), [&](const std::string& t) {
			return std::any_of(names.begin(), names.end(), [&](const char* n) { return t == n; });
		});
	};

	if (data.contains("bots"))
	{
		for (const auto& bot : data["bots"])
		{
			const std::string type = bot.value("type", "");
			if (type == "psyonix" || type == "necto" || type == "nexto" || (type == "puppet" && bot.value("behavior", "") == "keeper"))
				continue;
			return type == "puppet" ? "puppet bots (Phase 3)" : type == "nexto" ? "Nexto (Phase 7)" : "bots (Phase 6)";
		}
	}
	if (uses({"ball_touched_by_bot"}))
		return "detectors (Phase 4)";
	return "";
}

namespace
{
	// "situation": {"type": "attack", ...}: an attacking play toward the orange net, all ranges already drawn.
	//  - the ball at (x, y), rolling toward the net along the player's approach line (approachAngle degrees off the
	//    straight line to the net) at ballSpeed, with an optional hop;
	//  - the player ballLead behind it on that line, already driving at playerSpeed, facing it;
	//  - the first bot (the defender) defenderGap goal-side of the ball on the ball-to-net line (defenderLateral to
	//    its side), facing its own net and backing up at defenderSpeed: a shadowing defender, which still has to turn.
	// Mirrored reps play the same thing on the other side (x, the approach angle and the defender's side flip).
	// bounceChance: that share of reps bounce the ball (bounceHop up, bounceLeadExtra further ahead) instead of rolling.
	// Keeps a floor spawn inside the field: the side and back walls, and the 45-degree corner walls (|x| + |y| = 8064
	// on a standard Soccar field), `margin` uu in from all of them.
	void keepInField(double& x, double& y, double margin)
	{
		x = std::clamp(x, -4096.0 + margin, 4096.0 - margin);
		y = std::clamp(y, -5120.0 + margin, 5120.0 - margin);
		const double limit = 8064.0 - margin * 1.4142;
		const double over = std::fabs(x) + std::fabs(y) - limit;
		if (over > 0.0)
		{
			x -= std::copysign(over * 0.5, x);
			y -= std::copysign(over * 0.5, y);
		}
	}

	// A bounce that is obvious from the first frame: instead of leaving the floor going up, the ball starts in the air
	// already falling, timed so it comes off its first bounce (restitution 0.6) going up at `hop`. Sets the start
	// height and vertical speed; hop 0 leaves a rolling ball.
	void dropIn(double hop, double& z, double& vz)
	{
		if (hop <= 1.0)
		{
			z = 92.75;
			vz = 0.0;
			return;
		}
		const double impact = hop / 0.6;          // speed into the floor
		vz = -0.7 * impact;                      // already on its way down
		const double fall = (impact * impact - vz * vz) / (2.0 * 650.0);
		z = std::min(92.75 + fall, 1400.0);
	}

	// "situation": {"type": "defend", ...}: the first bot attacks the blue net with the ball, the player shadows.
	//  - the ball at (x, y); the attacker heads at the blue net (approachAngle off the line) at attackerSpeed with the
	//    ball on its roof (dribbleChance, so it can flick), bouncing ahead (bounceChance: bounceHop, bounceLead) or
	//    rolling just ahead (rollLead);
	//  - the player defenderGap goal-side of the ball on the ball-to-net line and defenderLateral to one side (random),
	//    not closer to the goal line than y -4100, mostly across the field and angled playerAngle degrees toward its
	//    net, moving at playerSpeed: shadowing, ready to cut across.
	void applyDefend(nlohmann::json& doc, bool mirrored, Rng& rng)
	{
		const nlohmann::json& s = doc["situation"];
		auto num = [&](const char* key, double fallback) { return s.contains(key) && s[key].is_number() ? s[key].get<double>() : fallback; };
		const double flip = mirrored ? -1.0 : 1.0;
		const double bx = std::clamp(flip * num("x", 0.0), -3300.0, 3300.0);
		const double by = std::clamp(num("y", 0.0), -3000.0, 2500.0);
		const double toNet = std::atan2(-5120.0 - by, 0.0 - bx);
		const double heading = toNet - flip * num("approachAngle", 0.0) * kPi / 180.0;
		const double hx = std::cos(heading), hy = std::sin(heading);
		const double speed = num("attackerSpeed", 1300.0);

		// How the attacker has the ball.
		const double roll = rng.next01();
		const double dribble = num("dribbleChance", 0.3), bounce = num("bounceChance", 0.4), wall = num("wallChance", 0.0);
		double carX, carY, ballZ = 92.75, ballVz = 0.0, ballSpeed = speed;
		double ballX = bx, ballY = by, ballVx = hx * speed, ballVy = hy * speed;
		bool directional = true; // ball velocity along the attacker's heading
		std::string carry;
		if (roll < wall)
		{
			// Off the side wall: the ball runs along the wall on x's side toward the blue net, the attacker chasing on
			// the floor beside it.
			carry = "wall";
			const double side = bx >= 0.0 ? 1.0 : -1.0;
			ballX = side * (4096.0 - 93.0);
			ballZ = num("wallHeight", 300.0);
			ballVx = side * 40.0;
			ballVy = -speed * 0.95;
			ballVz = num("wallHop", 150.0);
			directional = false;
			carX = side * (4096.0 - num("wallOffset", 450.0));
			carY = std::min(by + num("rollLead", 350.0) + 150.0, 4800.0);
		}
		else if (roll < wall + dribble)
		{
			carry = "dribble"; // on its roof, a little forward of centre
			carX = bx - hx * 20.0;
			carY = by - hy * 20.0;
			ballZ = 150.0;
		}
		else if (roll < wall + dribble + bounce)
		{
			carry = "bounce"; // dropping in ahead of the attacker
			const double lead = num("bounceLead", 550.0);
			carX = bx - hx * lead;
			carY = by - hy * lead;
			dropIn(num("bounceHop", 400.0), ballZ, ballVz);
			ballSpeed = speed * 0.85;
		}
		else
		{
			carry = "roll";
			const double lead = num("rollLead", 350.0);
			carX = bx - hx * lead;
			carY = by - hy * lead;
			ballSpeed = speed * 0.92;
		}
		doc["situation"]["carry"] = carry; // for the event log
		if (directional)
		{
			ballVx = hx * ballSpeed;
			ballVy = hy * ballSpeed;
		}
		doc["ball"]["location"] = {ballX, ballY, ballZ};
		doc["ball"]["velocity"] = {ballVx, ballVy, ballVz};
		doc["ball"].erase("aimAt");
		if (doc.contains("bots") && !doc["bots"].empty())
		{
			nlohmann::json& a = doc["bots"][0];
			double cx = carX, cy = carY;
			keepInField(cx, cy, 150.0);
			// Facing where it is going: its heading, or for a wall play the ball's run down the wall.
			const double face = directional ? heading : std::atan2(ballY - cy, (ballX - (ballX > 0 ? 300.0 : -300.0)) - cx);
			a["location"] = {cx, cy, 17.0};
			a["rotation"] = {0, static_cast<int>(std::lround(face * 32768.0 / kPi)), 0};
			a["velocity"] = {std::cos(face) * speed, std::sin(face) * speed, 0.0};
			a.erase("randomSide");
		}

		// The player: goal-side of the ball, to one side of its lane, shadowing.
		const double laneX = std::clamp(ballX, -3600.0, 3600.0);
		const double toNetBall = std::atan2(-5120.0 - ballY, 0.0 - laneX);
		const double nx = std::cos(toNetBall), ny = std::sin(toNetBall);
		const double gap = num("defenderGap", 2000.0);
		const double side = (rng.coin() ? 1.0 : -1.0) * num("defenderLateral", 600.0);
		double px = laneX + nx * gap - ny * side, py = ballY + ny * gap + nx * side;
		px = std::clamp(px, -3700.0, 3700.0);
		py = std::clamp(py, -4100.0, 4000.0);
		keepInField(px, py, 200.0);
		// Mostly along the goal line, toward the ball's lane, angled down toward its own net.
		const double across = (px > laneX + nx * gap) ? -1.0 : 1.0; // back toward the lane
		const double angle = num("playerAngle", 30.0) * kPi / 180.0;
		const double dirX = across * std::cos(angle), dirY = -std::sin(angle);
		const double pSpeed = num("playerSpeed", 800.0);
		nlohmann::json& p = doc["player"];
		p["location"] = {px, py, 17.0};
		p["rotation"] = {0, static_cast<int>(std::lround(std::atan2(dirY, dirX) * 32768.0 / kPi)), 0};
		p["velocity"] = {dirX * pSpeed, dirY * pSpeed, 0.0};
		p.erase("faceBall");
		p.erase("forwardSpeed");
		p.erase("relativeTo");
	}

	// "variants": [{"name", "weight", ...}]: one is picked per rep (by weight) and its values replace the base ones;
	// "defenderGapExtra" in a variant adds to the (difficulty's) defenderGap. "onWall": the ball rolls along the side
	// wall on the x side (x = its distance... only the sign of x is used), ballHeight up the wall, the player on the
	// floor wallOffset from the wall, ballLead behind it.
	// Picks one of situation.variants by weight (one draw) and copies its values over the base ones in `s`. Variants
	// with an "id" can be limited to `enabled` (the exercise page's picker); if none match, all are used.
	void pickVariant(nlohmann::json& s, nlohmann::json& written, Rng& rng, const std::vector<std::string>& enabled)
	{
		if (!s.contains("variants") || !s["variants"].is_array() || s["variants"].empty())
			return;
		std::vector<const nlohmann::json*> pool;
		for (const auto& v : s["variants"])
			if (!enabled.empty() && v.contains("id") && std::find(enabled.begin(), enabled.end(), v["id"].get<std::string>()) != enabled.end())
				pool.push_back(&v);
		if (pool.empty())
			for (const auto& v : s["variants"])
				pool.push_back(&v);
		double total = 0.0;
		for (const auto* v : pool)
			total += std::max(0.0, v->value("weight", 1.0));
		double pick = rng.next01() * total;
		const nlohmann::json* chosen = pool.back();
		for (const auto* v : pool)
			if ((pick -= std::max(0.0, v->value("weight", 1.0))) <= 0.0)
			{
				chosen = v;
				break;
			}
		const nlohmann::json variant = *chosen;
		for (const auto& [key, value] : variant.items())
			if (key != "weight" && key != "name" && key != "id")
				s[key] = value;
		written["variant"] = variant.value("name", std::string("?"));
	}

	// "situation": {"type": "kickoff", ...}: the player on a kickoff spot (x, y, yaw in degrees; blue side), the first
	// bot on the opposite spot (through the centre, as in a 1v1 kickoff), the ball still at the centre. Mirrored reps
	// take the spot on the other side (left / right).
	void applyKickoff(nlohmann::json& doc, bool mirrored, Rng& rng, const std::vector<std::string>& enabled)
	{
		nlohmann::json s = doc["situation"];
		pickVariant(s, doc["situation"], rng, enabled);
		auto num = [&](const char* key, double fallback) { return s.contains(key) && s[key].is_number() ? s[key].get<double>() : fallback; };
		const double flip = mirrored ? -1.0 : 1.0;
		const double x = flip * num("x", 0.0), y = num("y", -4608.0);
		double yaw = num("yaw", 90.0) * kPi / 180.0;
		if (mirrored)
			yaw = kPi - yaw;
		auto units = [](double radians) {
			double u = radians * 32768.0 / kPi;
			while (u > 32768.0) u -= 65536.0;
			while (u < -32768.0) u += 65536.0;
			return static_cast<int>(std::lround(u));
		};
		doc["ball"]["location"] = {0.0, 0.0, 92.75};
		doc["ball"]["velocity"] = {0.0, 0.0, 0.0};
		doc["ball"].erase("aimAt");
		nlohmann::json& p = doc["player"];
		p["location"] = {x, y, 17.0};
		p["rotation"] = {0, units(yaw), 0};
		p["velocity"] = {0.0, 0.0, 0.0};
		p.erase("faceBall");
		p.erase("forwardSpeed");
		p.erase("relativeTo");
		if (doc.contains("bots") && !doc["bots"].empty())
		{
			nlohmann::json& b = doc["bots"][0];
			b["location"] = {-x, -y, 17.0};
			b["rotation"] = {0, units(yaw + kPi), 0};
			b["velocity"] = {0.0, 0.0, 0.0};
			b.erase("randomSide");
		}
	}

	// "situation": {"type": "fifty", ...}: you and the first bot drive at a loose ball and meet it at the same moment.
	// The meeting point is (x, y) after meetSeconds; you come up-field (playerAngle degrees off +y), the bot down-field
	// (botAngle off -y); the ball rolls across at ballSpeed or drops in bouncing (bounceChance, bounceHop). "aim" is
	// what the 50 is for: "corner" (forward into their corner), "safe" (safely to the side / corner, in your half),
	// "mate" (back to a teammate: a circle mateBack behind the 50 and mateSide toward the middle).
	void applyFifty(nlohmann::json& doc, bool mirrored, Rng& rng, const std::vector<std::string>& enabled)
	{
		nlohmann::json s = doc["situation"];
		pickVariant(s, doc["situation"], rng, enabled);
		auto num = [&](const char* key, double fallback) { return s.contains(key) && s[key].is_number() ? s[key].get<double>() : fallback; };
		const double flip = mirrored ? -1.0 : 1.0;
		const std::string aim = s.value("aim", "corner");
		const double cx = flip * num("x", 2000.0), cy = num("y", 0.0);
		const double t = std::max(0.5, num("meetSeconds", 1.1));
		const double reach = 150.0; // car centre to ball centre at contact

		// The ball: rolling across the field (toward the middle or the wall, by coin), or dropping in to bounce.
		const double across = rng.coin() ? 1.0 : -1.0;
		const double ballSpeed = num("ballSpeed", 300.0);
		const double bvx = across * ballSpeed, bvy = num("ballDriftY", 0.0);
		double bx = cx - bvx * t, by = cy - bvy * t;
		keepInField(bx, by, 250.0);
		double bz = 92.75, bvz = 0.0;
		const bool bounce = rng.next01() < num("bounceChance", 0.0);
		if (bounce)
			dropIn(num("bounceHop", 350.0), bz, bvz);
		doc["ball"]["location"] = {bx, by, bz};
		doc["ball"]["velocity"] = {bvx, bvy, bvz};
		doc["ball"].erase("aimAt");

		// You: driving up-field into the meeting point.
		const double ph = kPi / 2.0 - flip * num("playerAngle", 0.0) * kPi / 180.0;
		const double ps = num("playerSpeed", 1400.0);
		double px = cx - std::cos(ph) * std::max(300.0, ps * t - reach), py = cy - std::sin(ph) * std::max(300.0, ps * t - reach);
		keepInField(px, py, 150.0);
		const double pFace = std::atan2(cy - py, cx - px);
		nlohmann::json& p = doc["player"];
		p["location"] = {px, py, 17.0};
		p["rotation"] = {0, static_cast<int>(std::lround(pFace * 32768.0 / kPi)), 0};
		p["velocity"] = {std::cos(pFace) * ps, std::sin(pFace) * ps, 0.0};
		p.erase("faceBall");
		p.erase("forwardSpeed");
		p.erase("relativeTo");

		// The bot: driving down-field into the same point at the same moment.
		if (doc.contains("bots") && !doc["bots"].empty())
		{
			const double bh = -kPi / 2.0 + flip * num("botAngle", 0.0) * kPi / 180.0;
			const double bs = num("botSpeed", 1400.0);
			double ox = cx - std::cos(bh) * std::max(300.0, bs * t - reach), oy = cy - std::sin(bh) * std::max(300.0, bs * t - reach);
			keepInField(ox, oy, 150.0);
			const double oFace = std::atan2(cy - oy, cx - ox);
			nlohmann::json& b = doc["bots"][0];
			b["location"] = {ox, oy, 17.0};
			b["rotation"] = {0, static_cast<int>(std::lround(oFace * 32768.0 / kPi)), 0};
			b["velocity"] = {std::cos(oFace) * bs, std::sin(oFace) * bs, 0.0};
			b.erase("randomSide");
		}

		nlohmann::json& w = doc["situation"];
		w["aim"] = aim;
		w["contact"] = {cx, cy};
		w["carry"] = bounce ? "bounce" : "roll";
		if (aim == "mate")
		{
			// Your teammate: behind the 50, toward the middle of the field.
			const double toMiddle = cx > 0.0 ? -1.0 : 1.0;
			double mx = cx + toMiddle * num("mateSide", 900.0), my = cy - num("mateBack", 2200.0);
			keepInField(mx, my, 500.0);
			w["mate"] = {mx, my};
			if (!doc.contains("objects"))
				doc["objects"] = nlohmann::json::array();
			doc["objects"].push_back({{"type", "circle"}, {"id", "mate"}, {"radius", num("mateRadius", 650.0)},
				{"triggeredBy", "ball"}, {"color", "green"}, {"location", {mx, my, 0.0}}});
		}
		doc["objectiveLabel"] = aim == "mate" ? "Win the 50 back to your teammate (the circle)"
			: aim == "safe" ? "Win the 50 safely: to the side or the corner, never the middle"
			: "Win the 50 forward, into their corner";
		doc["objectiveTag"] = aim == "mate" ? "TEAMMATE" : aim == "safe" ? "SAFE 50" : "INTO THE CORNER";
	}

	// "situation": {"type": "control", ...} (Ball Control, Phase 3): an awkward ball reaches you at (x, y) after
	// meetSeconds while the first bot comes at it from their side, arriving botLate seconds after you. You drive
	// up-field into the point (playerAngle off +y, playerSpeed). The ball by "kind": lob (dropping in from their side to
	// land on you), bounce (it lands just before you and comes up at you), spin (a skipping roller with heavy spin),
	// wall (coming down the side wall at the point; x's sign picks the wall), over (a pass from behind over your head).
	void applyControl(nlohmann::json& doc, bool mirrored, Rng& rng, const std::vector<std::string>& enabled)
	{
		nlohmann::json s = doc["situation"];
		pickVariant(s, doc["situation"], rng, enabled);
		auto num = [&](const char* key, double fallback) { return s.contains(key) && s[key].is_number() ? s[key].get<double>() : fallback; };
		const double flip = mirrored ? -1.0 : 1.0;
		const std::string kind = s.value("kind", "bounce");
		const double t = std::max(0.6, num("meetSeconds", 1.3));
		double cx = flip * num("x", 0.0), cy = num("y", -1500.0);
		const double wallSide = cx >= 0.0 ? 1.0 : -1.0;
		if (kind == "wall")
			cx = wallSide * (4096.0 - num("wallOffset", 500.0));
		keepInField(cx, cy, 400.0);

		// You: up-field into the point, a little short of it (you receive it, not drive through it).
		const double ph = kPi / 2.0 - flip * num("playerAngle", 0.0) * kPi / 180.0;
		const double ps = num("playerSpeed", 1200.0);
		double px = cx - std::cos(ph) * std::max(300.0, ps * t - 200.0), py = cy - std::sin(ph) * std::max(300.0, ps * t - 200.0);
		keepInField(px, py, 150.0);
		const double pFace = std::atan2(cy - py, cx - px);
		nlohmann::json& p = doc["player"];
		p["location"] = {px, py, 17.0};
		p["rotation"] = {0, static_cast<int>(std::lround(pFace * 32768.0 / kPi)), 0};
		p["velocity"] = {std::cos(pFace) * ps, std::sin(pFace) * ps, 0.0};
		p.erase("faceBall");
		p.erase("forwardSpeed");
		p.erase("relativeTo");

		// The ball.
		const double bs = num("ballSpeed", 900.0);
		const double g = 650.0;
		// The vertical speed that brings a ball from height h down to the floor (z 93) after `secs`.
		auto landIn = [&](double h, double secs) { return (92.75 - h + 0.5 * g * secs * secs) / secs; };
		double bx = cx, by = cy, bz = 92.75, bvx = 0.0, bvy = 0.0, bvz = 0.0;
		Vec3 spin;
		if (kind == "wall")
		{
			// High on the side wall above the point, pressed into it, sliding down and back toward you.
			bx = wallSide * (4096.0 - 93.0);
			bz = num("wallHeight", 800.0);
			by = cy + bs * 0.5 * t;
			bvx = wallSide * 40.0;
			bvy = -bs * 0.5;
			bvz = -num("ballHop", 150.0);
		}
		else
		{
			// From their side toward you (ballAngle off straight down-field), or from behind you ("over").
			const bool over = kind == "over";
			const double heading = (over ? kPi / 2.0 : -kPi / 2.0) + flip * num("ballAngle", 0.0) * kPi / 180.0;
			const double hx = std::cos(heading), hy = std::sin(heading);
			bx = cx - hx * bs * t;
			by = cy - hy * bs * t;
			bvx = hx * bs;
			bvy = hy * bs;
			if (kind == "lob" || over)
			{
				bz = num("lobHeight", over ? 900.0 : 700.0);
				bvz = landIn(bz, t); // lands at the point as you get there
			}
			else if (kind == "bounce")
			{
				// Lands 0.35 s before you get there and comes up at you off the floor.
				bz = num("lobHeight", 500.0);
				bvz = landIn(bz, std::max(0.3, t - 0.35));
			}
			else
			{
				// spin: skipping along with heavy spin about its side axis (top or back spin by coin).
				double z = 92.75, vz = 0.0;
				dropIn(num("ballHop", 250.0), z, vz);
				bz = z;
				bvz = vz;
				const double w = std::min(6.0, num("spin", 6.0)) * (rng.coin() ? 1.0 : -1.0);
				spin = Vec3(static_cast<float>(-hy * w), static_cast<float>(hx * w), 0.f);
			}
		}
		if (kind == "wall")
			by = std::clamp(by, -4500.0, 4500.0); // it stays on the wall
		else
			keepInField(bx, by, 150.0);
		doc["ball"]["location"] = {bx, by, std::clamp(bz, 92.75, 1900.0)};
		doc["ball"]["velocity"] = {bvx, bvy, bvz};
		doc["ball"]["angularVelocity"] = {spin.x, spin.y, spin.z};
		doc["ball"].erase("aimAt");

		// The bot: down-field into the point from their side, there botLate seconds after you.
		if (doc.contains("bots") && !doc["bots"].empty())
		{
			const double bh = -kPi / 2.0 + flip * num("botAngle", 0.0) * kPi / 180.0;
			const double bsp = num("botSpeed", 1300.0);
			const double dist = bsp * (t + num("botLate", 0.8));
			double ox = cx - std::cos(bh) * dist + flip * num("botLateral", 0.0), oy = cy - std::sin(bh) * dist;
			keepInField(ox, oy, 150.0);
			const double oFace = std::atan2(cy - oy, cx - ox);
			nlohmann::json& b = doc["bots"][0];
			b["location"] = {ox, oy, 17.0};
			b["rotation"] = {0, static_cast<int>(std::lround(oFace * 32768.0 / kPi)), 0};
			b["velocity"] = {std::cos(oFace) * bsp, std::sin(oFace) * bsp, 0.0};
			b.erase("randomSide");
		}
		nlohmann::json& w = doc["situation"];
		w["carry"] = kind;
		w["contact"] = {cx, cy};
		doc["objectiveLabel"] = "Control it with the bot on you: keep it, get past it, or score";
		doc["objectiveTag"] = "BALL CONTROL";
	}

	void applyAttack(nlohmann::json& doc, bool mirrored, Rng& rng)
	{
		nlohmann::json s = doc["situation"];
		pickVariant(s, doc["situation"], rng, {});
		auto num = [&](const char* key, double fallback) { return s.contains(key) && s[key].is_number() ? s[key].get<double>() : fallback; };
		const double flip = mirrored ? -1.0 : 1.0;
		const bool onWall = num("onWall", 0.0) > 0.5;
		const double wallSide = flip * num("x", 0.0) >= 0.0 ? 1.0 : -1.0;
		const double bx = onWall ? wallSide * (4096.0 - 93.0) : std::clamp(flip * num("x", 0.0), -3300.0, 3300.0);
		const double by = std::clamp(num("y", 500.0), -4300.0, 3200.0);
		const double toNet = std::atan2(5120.0 - by, 0.0 - bx);
		const bool bounce = rng.next01() < num("bounceChance", 0.0);
		const double lead = num("ballLead", 600.0) + (bounce ? num("bounceLeadExtra", 0.0) : 0.0);
		const double ballSpeed = num("ballSpeed", 900.0);
		const double hop = bounce ? num("bounceHop", 400.0) : num("ballHop", 0.0);
		const double ballZ = std::max(92.75, num("ballHeight", 92.75));
		const double playerSpeed = num("playerSpeed", 1400.0);
		double px, py, approach;
		if (onWall)
		{
			// Up the side wall: the ball runs along it (pressed into it a little so it stays on), the player chases on
			// the floor beside it.
			doc["ball"]["location"] = {bx, by, ballZ};
			doc["ball"]["velocity"] = {wallSide * 40.0, ballSpeed, hop};
			px = wallSide * (4096.0 - num("wallOffset", 500.0));
			py = std::clamp(by - lead, -4900.0, 4900.0);
			approach = std::atan2(by - py, (bx - wallSide * 250.0) - px);
		}
		else
		{
			// "heading": the play's direction in degrees from +x (mirrored reps flip it), instead of toward the net.
			approach = s.contains("heading") && s["heading"].is_number()
				? (mirrored ? kPi - num("heading", 0.0) * kPi / 180.0 : num("heading", 0.0) * kPi / 180.0)
				: toNet - flip * num("approachAngle", 0.0) * kPi / 180.0;
			const double ax = std::cos(approach), ay = std::sin(approach);
			double z = ballZ, vz = 0.0;
			if (hop > 1.0)
				dropIn(hop, z, vz); // a bounce starts in the air, falling
			doc["ball"]["location"] = {bx, by, z};
			doc["ball"]["velocity"] = {ax * ballSpeed, ay * ballSpeed, vz};
			px = bx - ax * lead;
			py = by - ay * lead;
		}
		keepInField(px, py, 150.0);
		doc["ball"].erase("aimAt");

		nlohmann::json& p = doc["player"];
		p["location"] = {px, py, 17.0};
		p["rotation"] = {0, static_cast<int>(std::lround(approach * 32768.0 / kPi)), 0};
		p["velocity"] = {std::cos(approach) * playerSpeed, std::sin(approach) * playerSpeed, 0.0};
		p.erase("faceBall");
		p.erase("forwardSpeed");
		p.erase("relativeTo");

		if (doc.contains("bots") && !doc["bots"].empty())
		{
			nlohmann::json& d = doc["bots"][0];
			const double nx = std::cos(toNet), ny = std::sin(toNet);
			const double gap = num("defenderGap", 2200.0) + num("defenderGapExtra", 0.0), side = -flip * num("defenderLateral", 0.0);
			double dx = bx + nx * gap - ny * side, dy = by + ny * gap + nx * side;
			dy = std::min(dy, 4700.0); // never inside its net
			keepInField(dx, dy, 200.0);
			const double back = num("defenderSpeed", 900.0);
			d["location"] = {dx, dy, 17.0};
			d["rotation"] = {0, static_cast<int>(std::lround(toNet * 32768.0 / kPi)), 0}; // facing its own net
			d["velocity"] = {nx * back, ny * back, 0.0};
			d.erase("randomSide");
		}
	}
}

ResolvedScenario scenario::resolve(const json& data, uint32_t seed, const std::string& difficulty,
	std::optional<bool> forceMirror, const std::vector<std::string>& enabledChoices)
{
	ResolvedScenario r;
	r.seed = seed;
	r.difficulty = difficulty;
	r.id = data.value("id", "");
	r.name = data.value("name", r.id);
	r.category = data.value("category", "");

	Rng rng(seed);
	json doc = data;
	json overrides = json::object();
	if (doc.contains("difficulty"))
	{
		if (doc["difficulty"].contains(difficulty))
			overrides = doc["difficulty"][difficulty];
		doc.erase("difficulty");
	}

	// 1. ranges
	resolveRanges(doc, rng, "");

	// 2. difficulty overrides
	for (auto& [path, value] : overrides.items())
	{
		json v = value;
		// The last path segment is the key, so "objects.0.grid": [2, 1] stays a grid and is not drawn as a range.
		resolveRanges(v, rng, path.substr(path.rfind('.') == std::string::npos ? 0 : path.rfind('.') + 1));
		if (!setPath(doc, path, v))
			r.warnings.push_back("difficulty '" + difficulty + "': path '" + path + "' not found");
	}

	// 2b. one_of: pick the rep's objective set (and its ball / car) before mirroring, so they flip together.
	if (doc.contains("objects"))
		expandOneOf(doc, rng, enabledChoices, difficulty);

	// 3. mirror
	const bool coin = rng.coin();
	if (doc.value("allowMirror", false) && forceMirror.value_or(coin))
	{
		r.mirrored = true;
		mirrorBody(doc["ball"]);
		mirrorBody(doc["player"]);
		for (const char* list : {"bots", "objects"})
		{
			if (doc.contains(list))
			{
				for (auto& item : doc[list])
					mirrorBody(item);
			}
		}
	}

	// 3c (below, after 3b): objects on a surface grid are expanded into cells.
	// 3b. bots with randomSide: their own coin per bot, in order (after the scenario mirror, so seeds stay stable).
	if (doc.contains("bots"))
	{
		for (auto& bot : doc["bots"])
		{
			if (bot.value("randomSide", false) && rng.coin())
				mirrorBody(bot);
		}
	}

	// 3c. surface grids (last, so they never change the draws above): a target with "surface" + "grid" becomes one
	// pocket target in a random cell plus blue blockers on every other cell; a panel becomes one panel per cell.
	if (doc.contains("objects"))
	{
		expandSurfaceGrids(doc["objects"], rng);
		expandGenerated(doc, rng);
	}
	if (doc.contains("objectiveLabel"))
		r.name += ": " + doc["objectiveLabel"].get<std::string>();

	// 3d. a whole play generated at once (ball, player and defender that fit together).
	if (doc.contains("situation") && doc["situation"].value("type", "") == "attack")
		applyAttack(doc, r.mirrored, rng);
	if (doc.contains("situation") && doc["situation"].value("type", "") == "defend")
		applyDefend(doc, r.mirrored, rng);
	if (doc.contains("situation") && doc["situation"].value("type", "") == "kickoff")
		applyKickoff(doc, r.mirrored, rng, enabledChoices);
	if (doc.contains("situation") && doc["situation"].value("type", "") == "fifty")
		applyFifty(doc, r.mirrored, rng, enabledChoices);
	if (doc.contains("situation") && doc["situation"].value("type", "") == "control")
		applyControl(doc, r.mirrored, rng, enabledChoices);

	// 4. typed ball and player
	const json& ball = doc["ball"];
	r.ball.location = readVec3(ball["location"]);
	r.ball.velocity = readVec3(ball.value("velocity", json()), {});
	r.ball.angularVelocity = readVec3(ball.value("angularVelocity", json()), {});
	r.ballFreezeSeconds = ball.value("freezeSeconds", 0.0);
	r.ballHidden = ball.value("hidden", false);
	if (r.ballHidden)
		r.ball.velocity = r.ball.angularVelocity = {};
	if (ball.contains("aimAt"))
	{
		r.aimAtUsed = true;
		const Vec3 point = readVec3(ball["aimAt"]["point"]);
		const double speed = ball["aimAt"]["speed"].get<double>();
		r.ball.velocity = ball["aimAt"].value("via", "direct") == "sideWall"
			? aimVelocityOffSideWall(r.ball.location, point, speed, r.aimAtReachable)
			: aimVelocity(r.ball.location, point, speed, r.aimAtReachable);
		if (!r.aimAtReachable)
			r.warnings.push_back("aimAt speed too low to reach the point; used the slowest arc that does");
	}

	const json& player = doc["player"];
	r.player.location = readVec3(player["location"]);
	const std::string relativeTo = player.value("relativeTo", "world");
	if (relativeTo == "ball")
	{
		r.player.location.x += r.ball.location.x;
		r.player.location.y += r.ball.location.y;
	}
	else if (relativeTo == "landingToGoal")
	{
		// Offset from where the ball first comes down (gravity only), in a frame whose +y points from there at the
		// orange goal's centre and +x to its right: [0, -1500] is 1500 straight back on the line to the net, however
		// the ball drifts (mirrors cleanly: x flips with it).
		const Vec3& b = r.ball.location;
		const Vec3& v = r.ball.velocity;
		const double fall = std::max(0.0, static_cast<double>(b.z) - 92.75);
		const double t = (v.z + std::sqrt(static_cast<double>(v.z) * v.z + 4.0 * kHalfGravity * fall)) / (2.0 * kHalfGravity);
		const double lx = b.x + v.x * t, ly = b.y + v.y * t;
		const double a = std::atan2(5120.0 - ly, 0.0 - lx);
		const double ox = r.player.location.x, oy = r.player.location.y;
		r.player.location.x = static_cast<float>(lx + ox * std::sin(a) + oy * std::cos(a));
		r.player.location.y = static_cast<float>(ly - ox * std::cos(a) + oy * std::sin(a));
	}
	r.player.rotation = readRot(player["rotation"]);
	r.player.velocity = readVec3(player.value("velocity", json()), {});
	r.player.angularVelocity = readVec3(player.value("angularVelocity", json()), {});
	if (player.contains("boost"))
		r.player.boost = std::clamp(player["boost"].get<float>(), 0.f, 100.f);
	if (player.value("faceBall", false))
	{
		const Vec3 to = r.ball.location - r.player.location;
		r.player.rotation.yaw = wrapRot(std::llround(std::atan2(to.y, to.x) * 32768.0 / kPi
			+ player.value("faceBallOffset", 0.0)));
	}
	if (player.contains("forwardSpeed"))
	{
		const double yaw = r.player.rotation.yaw * kPi / 32768.0;
		const float speed = player["forwardSpeed"].get<float>();
		r.player.velocity = {static_cast<float>(std::cos(yaw)) * speed, static_cast<float>(std::sin(yaw)) * speed, 0.f};
	}
	if (player.contains("tossAt") && player["tossAt"].contains("targets") && !player["tossAt"]["targets"].empty())
	{
		// Thrown at a wall or corner: start `distance` back from a random target (toward the middle of the field,
		// up to 35 degrees off), already flying at it. The start height comes from location.z.
		const json& toss = player["tossAt"];
		const json& targets = toss["targets"];
		const Vec3 target = readVec3(targets[std::min(targets.size() - 1, static_cast<size_t>(rng.next01() * targets.size()))]);
		const double spread = (rng.next01() * 2.0 - 1.0) * 35.0 * kPi / 180.0;
		Vec3 back = unit(Vec3(-target.x, -target.y, 0.f));
		back = Vec3(static_cast<float>(back.x * std::cos(spread) - back.y * std::sin(spread)),
			static_cast<float>(back.x * std::sin(spread) + back.y * std::cos(spread)), 0.f);
		const float distance = toss.value("distance", 1400.f);
		r.player.location = Vec3(target.x + back.x * distance, target.y + back.y * distance, r.player.location.z);
		r.player.location.x = std::clamp(r.player.location.x, -3800.f, 3800.f);
		r.player.location.y = std::clamp(r.player.location.y, -4800.f, 4800.f);
		r.player.velocity = unit(target - r.player.location) * toss.value("speed", 1100.f);
	}

	if (doc.contains("timeLimitSeconds"))
		r.timeLimitSeconds = doc["timeLimitSeconds"].get<double>();

	r.resolved = std::move(doc);
	return r;
}

bool scenario::targetsAsPackTiles(json& resolved)
{
	// The Daily Challenge (Lambourne 2026-10-20: "the goal blocker from the training packs, grey with an orange
	// opening; I don't like the blue one"): a target grid in the orange net becomes the packs' tiles, the same cell
	// open (lit) and the rest closed. A ball reaching a closed tile bounces off it and is a miss.
	if (!resolved.contains("objects") || !resolved["objects"].is_array())
		return false;
	nlohmann::json kept = nlohmann::json::array();
	std::string targetId;
	int cell = -1, cols = 0, rows = 0;
	for (const auto& o : resolved["objects"])
	{
		const bool netTarget = o.value("type", "") == "target" && o.contains("cell") && o.contains("size") && o.contains("location")
			&& o["location"].is_array() && o["location"].size() == 3 && o["location"][1].get<double>() > 4900.0;
		if (netTarget && targetId.empty())
		{
			targetId = o.value("id", "target");
			cell = o["cell"].get<int>();
			// The grid from the cell size (each cell is the net's width / cols, less 8 uu).
			cols = std::clamp(static_cast<int>(std::lround(1786.0 / (o["size"][0].get<double>() + 8.0))), 1, 5);
			rows = std::clamp(static_cast<int>(std::lround(643.0 / (o["size"][1].get<double>() + 8.0))), 1, 4);
			continue;
		}
		kept.push_back(o);
	}
	if (targetId.empty())
		return false;
	// Its blockers go too (they were "<id>_block_<k>").
	nlohmann::json rest = nlohmann::json::array();
	for (const auto& o : kept)
		if (o.value("id", "").rfind(targetId + "_block_", 0) != 0)
			rest.push_back(o);
	rest.push_back({{"type", "zone_cycle"}, {"id", "pack_target"}, {"surface", "orange_goal"}, {"grid", {cols, rows}},
		{"cycleSeconds", 100000.0}, {"blocking", true}, {"lit", std::clamp(cell, 0, cols * rows - 1)}});
	resolved["objects"] = rest;
	resolved["success"] = {{"type", "goal_in_zone"}, {"label", "IN THE TILE"}};
	// Only the clock is left to fail it (the blockers' hits are gone; a closed tile is a miss by itself).
	nlohmann::json fails = nlohmann::json::array();
	for (const auto& f : resolved.value("fail", nlohmann::json::array()))
		if (f.value("type", "") != "object_hit")
			fails.push_back(f);
	resolved["fail"] = fails;
	return true;
}

bool scenario::hasDifficulty(const json& data)
{
	if (data.contains("difficulty"))
		return true;
	for (const auto& o : data.value("objects", json::array()))
		if (o.value("type", "") == "one_of")
			for (const auto& c : o.value("choices", json::array()))
				if (c.contains("difficulties"))
					return true;
	return false;
}
