#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "data/LeadIn.h"
#include "third_party/nlohmann/json.hpp"

// Shot Bank: the player fills a rack of five shots by scoring, each recorded with its build-up, then saves them in their own net in a
// shuffled order, a car playing back everything they did. The ball follows the recording all the way to the goal line,
// so every bounce happens as it did (a shot off the bar flown again from a saved speed lands somewhere else); the
// player touching it sets it free there and then. No SDK calls.
namespace shotbank
{
	constexpr auto kShootId = "modes.shot_bank";
	constexpr auto kDefendPrefix = "shot_bank.defend.";
	constexpr int kShots = 5;
	constexpr double kKeepSeconds = 12.0; // the most of a build-up that is kept
	constexpr float kMinKmh = 40.f;       // softer goals don't bank: a slow roller would be a free save
	constexpr float kStillSpeed = 100.f;  // uu/s: below this, for ball and car, nothing is happening yet
	constexpr double kStillKeepSeconds = 0.5;

	struct Shot
	{
		leadin::Track track;
		float kmh = 0.f; // at the goal line
	};

	inline size_t buildUpStart(const leadin::Track& t)
	{
		size_t first = 0;
		while (first + 1 < t.frames.size() && t.frames[first].ball.velocity.length() < kStillSpeed
			&& t.frames[first].car.velocity.length() < kStillSpeed)
			++first;
		return first - std::min(first, static_cast<size_t>(kStillKeepSeconds * t.hz));
	}

	// The recording up to the goal, less any time spent sitting still at its start. Empty (no frames) when there isn't
	// enough of it to replay.
	inline Shot make(leadin::Track track, float kmh)
	{
		Shot s;
		s.kmh = kmh;
		if (track.frames.size() < 2)
			return s;
		const size_t first = buildUpStart(track);
		track.frames.erase(track.frames.begin(), track.frames.begin() + static_cast<std::ptrdiff_t>(first));
		s.track = std::move(track);
		return s;
	}

	// The five slots. One is picked at a time (the D-pad moves it, wrapping round) and scoring fills it; a filled slot
	// is replaced, so a shot can be redone without touching the others. Past the last slot is the Defend tile, there
	// only once all five are in.
	class Rack
	{
	public:
		static constexpr int kDefendTile = kShots;

		const Shot& slot(int i) const { return slots_[i]; }
		void setSlot(int i, Shot shot) { slots_[i] = std::move(shot); }
		bool filled(int i) const { return i >= 0 && i < kShots && !slots_[i].track.frames.empty(); }
		int count() const
		{
			int n = 0;
			for (int i = 0; i < kShots; ++i)
				n += filled(i);
			return n;
		}
		bool full() const { return count() == kShots; }
		int selected() const { return selected_; }
		bool onDefend() const { return selected_ == kDefendTile; }
		void select(int i) { selected_ = std::clamp(i, 0, full() ? kDefendTile : kShots - 1); }
		void selectFirstEmpty()
		{
			selected_ = 0;
			while (selected_ < kShots - 1 && filled(selected_))
				++selected_;
			if (full())
				selected_ = 0;
		}
		void step(int delta)
		{
			const int tiles = full() ? kShots + 1 : kShots;
			selected_ = ((selected_ + delta) % tiles + tiles) % tiles;
		}
		// Fills the picked slot and moves on: to the next empty one, or the Defend tile once all are in. True when it
		// replaced a shot.
		bool store(Shot shot)
		{
			if (onDefend())
				return false;
			const bool replaced = filled(selected_);
			slots_[selected_] = std::move(shot);
			if (full())
				selected_ = kDefendTile;
			else
				selectFirstEmpty();
			return replaced;
		}
		void clearSlot(int i)
		{
			if (i >= 0 && i < kShots)
				slots_[i] = Shot{};
			if (onDefend())
				selectFirstEmpty();
		}
		void clear()
		{
			for (auto& s : slots_)
				s = Shot{};
			selected_ = 0;
		}

	private:
		Shot slots_[kShots];
		int selected_ = 0;
	};

	// rack.json: {"slots": [null or {"kmh", "boost": "0110...", "track": a lead-in track}]}
	inline nlohmann::ordered_json toJson(const Rack& rack)
	{
		nlohmann::ordered_json slots = nlohmann::ordered_json::array();
		for (int i = 0; i < kShots; ++i)
		{
			if (!rack.filled(i))
			{
				slots.push_back(nullptr);
				continue;
			}
			const Shot& s = rack.slot(i);
			std::string boost;
			for (const auto& f : s.track.frames)
				boost += f.boost ? '1' : '0';
			slots.push_back({{"kmh", leadin::round1(s.kmh)}, {"boost", boost}, {"track", leadin::toJson(s.track)}});
		}
		return {{"version", 1}, {"slots", std::move(slots)}};
	}

	template <class Json>
	void fromJson(const Json& doc, Rack& rack)
	{
		rack.clear();
		if (!doc.is_object() || !doc.contains("slots") || !doc["slots"].is_array())
			return;
		int i = 0;
		for (const auto& j : doc["slots"])
		{
			if (i >= kShots)
				break;
			const int at = i++;
			if (!j.is_object() || !j.contains("track"))
				continue;
			Shot s;
			s.kmh = j.value("kmh", 0.f);
			s.track = leadin::fromJson(j["track"]);
			const std::string boost = j.value("boost", std::string());
			for (size_t f = 0; f < s.track.frames.size() && f < boost.size(); ++f)
				s.track.frames[f].boost = boost[f] == '1';
			if (s.track.frames.size() >= 2)
				rack.setSlot(at, std::move(s));
		}
		rack.selectFirstEmpty();
	}

	// A bank sent to friends: 30 frames a second in whole numbers, without what a replay doesn't use. One row of
	// [ball location, velocity, spin x 100, car location, rotation, velocity, boosting]. {"v": 1, "hz": 30, "slots": [...]}
	constexpr int kShareValues = 19;
	constexpr size_t kMaxShareFrames = 480;
	constexpr size_t kMaxShareBytes = 300000; // the service's limit

	inline nlohmann::json shareDoc(const Rack& rack)
	{
		nlohmann::json slots = nlohmann::json::array();
		for (int i = 0; i < kShots; ++i)
		{
			const Shot& s = rack.slot(i);
			const auto every = static_cast<size_t>(std::max(1.0, std::round(s.track.hz / 30.0)));
			nlohmann::json rows = nlohmann::json::array();
			auto add = [&](const leadin::Frame& f) {
				auto whole = [](float v) { return static_cast<int>(std::lround(v)); };
				rows.push_back({whole(f.ball.location.x), whole(f.ball.location.y), whole(f.ball.location.z), whole(f.ball.velocity.x),
					whole(f.ball.velocity.y), whole(f.ball.velocity.z), whole(f.ball.angularVelocity.x * 100.f),
					whole(f.ball.angularVelocity.y * 100.f), whole(f.ball.angularVelocity.z * 100.f), whole(f.car.location.x),
					whole(f.car.location.y), whole(f.car.location.z), f.car.rotation.pitch, f.car.rotation.yaw, f.car.rotation.roll,
					whole(f.car.velocity.x), whole(f.car.velocity.y), whole(f.car.velocity.z), f.boost ? 1 : 0});
			};
			for (size_t f = 0; f < s.track.frames.size(); f += every)
				add(s.track.frames[f]);
			if (!s.track.frames.empty() && (s.track.frames.size() - 1) % every != 0)
				add(s.track.frames.back()); // the goal line
			slots.push_back({{"kmh", std::lround(s.kmh)}, {"f", std::move(rows)}});
		}
		return {{"v", 1}, {"hz", 30.0}, {"slots", std::move(slots)}};
	}

	// Finds a bank already uploaded, so sending it to another friend doesn't store it twice (FNV-1a).
	inline std::string fingerprint(const std::string& text)
	{
		uint64_t h = 14695981039346656037ull;
		for (const unsigned char c : text)
			h = (h ^ c) * 1099511628211ull;
		char out[17];
		snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
		return out;
	}

	// A bank from another player. It only ever moves the ball and a car, and everything is held to what the game could
	// really show; anything that isn't a whole bank of the right shape is refused.
	template <class Json>
	bool fromShare(const Json& doc, Rack& rack)
	{
		rack.clear();
		if (!doc.is_object() || doc.value("v", 0) != 1 || !doc.contains("slots") || !doc["slots"].is_array() || doc["slots"].size() != kShots)
			return false;
		const double hz = doc.value("hz", 0.0);
		if (hz < 10.0 || hz > 120.0)
			return false;
		auto within = [](float v, float limit) { return std::clamp(v, -limit, limit); };
		auto bounded = [&](float x, float y, float z, float lx, float ly, float lz, float lowZ) {
			return Vec3(within(x, lx), within(y, ly), std::clamp(z, lowZ, lz));
		};
		auto speed = [&](float x, float y, float z, float limit) { return Vec3(within(x, limit), within(y, limit), within(z, limit)); };
		int at = 0;
		for (const auto& j : doc["slots"])
		{
			if (!j.is_object() || !j.contains("f") || !j["f"].is_array() || j["f"].size() < 2 || j["f"].size() > kMaxShareFrames)
				return false;
			Shot s;
			s.kmh = std::clamp(j.value("kmh", 0.f), 0.f, 400.f);
			s.track.hz = hz;
			for (const auto& row : j["f"])
			{
				if (!row.is_array() || row.size() != kShareValues)
					return false;
				float n[kShareValues];
				for (int i = 0; i < kShareValues; ++i)
				{
					if (!row[i].is_number())
						return false;
					n[i] = row[i].template get<float>();
				}
				leadin::Frame f;
				f.ball.location = bounded(n[0], n[1], n[2], 4200.f, 6100.f, 2100.f, 0.f);
				f.ball.velocity = speed(n[3], n[4], n[5], 6100.f);
				f.ball.angularVelocity = speed(n[6] / 100.f, n[7] / 100.f, n[8] / 100.f, 6.5f);
				f.car.location = bounded(n[9], n[10], n[11], 4200.f, 6100.f, 2100.f, 0.f);
				f.car.rotation = {static_cast<int>(within(n[12], 32768.f)), static_cast<int>(within(n[13], 32768.f)), static_cast<int>(within(n[14], 32768.f))};
				f.car.velocity = speed(n[15], n[16], n[17], 2400.f);
				f.boost = n[18] > 0.5f;
				s.track.frames.push_back(f);
			}
			rack.setSlot(at++, std::move(s));
		}
		rack.selectFirstEmpty();
		return rack.full();
	}

	inline int wrapRot(int r)
	{
		r %= 65536;
		if (r >= 32768) r -= 65536;
		if (r < -32768) r += 65536;
		return r;
	}

	// Half a turn round the field's centre: a shot at the orange net comes at the blue one.
	inline Vec3 turned(const Vec3& v) { return {-v.x, -v.y, v.z}; }
	// Across x = 0. Spin is an axial vector: its y and z flip, x stays.
	inline Vec3 mirrored(const Vec3& v) { return {-v.x, v.y, v.z}; }
	inline Vec3 mirroredSpin(const Vec3& v) { return {v.x, -v.y, -v.z}; }

	inline void turn(leadin::Frame& f)
	{
		f.ball.location = turned(f.ball.location);
		f.ball.velocity = turned(f.ball.velocity);
		f.ball.angularVelocity = turned(f.ball.angularVelocity);
		f.car.location = turned(f.car.location);
		f.car.velocity = turned(f.car.velocity);
		f.car.angularVelocity = turned(f.car.angularVelocity);
		f.car.rotation.yaw = wrapRot(f.car.rotation.yaw + 32768);
	}

	inline void mirror(leadin::Frame& f)
	{
		f.ball.location = mirrored(f.ball.location);
		f.ball.velocity = mirrored(f.ball.velocity);
		f.ball.angularVelocity = mirroredSpin(f.ball.angularVelocity);
		f.car.location = mirrored(f.car.location);
		f.car.velocity = mirrored(f.car.velocity);
		f.car.angularVelocity = mirroredSpin(f.car.angularVelocity);
		f.car.rotation.yaw = wrapRot(32768 - f.car.rotation.yaw);
		f.car.rotation.roll = -f.car.rotation.roll;
	}

	inline nlohmann::ordered_json vec1(const Vec3& v)
	{
		return nlohmann::ordered_json::array({leadin::round1(v.x), leadin::round1(v.y), leadin::round1(v.z)});
	}
	inline nlohmann::ordered_json vec2(const Vec3& v)
	{
		return nlohmann::ordered_json::array({leadin::round2(v.x), leadin::round2(v.y), leadin::round2(v.z)});
	}

	// The scenario that plays `shot` at the player's net: turned round, mirrored if asked, and the car on a track.
	inline nlohmann::ordered_json toScenario(Shot shot, const std::string& id, int number, int total, bool mirrorIt,
		const std::string& carName = "FT Past You")
	{
		for (auto& f : shot.track.frames)
		{
			turn(f);
			if (mirrorIt)
				mirror(f);
		}
		nlohmann::ordered_json rows = nlohmann::ordered_json::array();
		for (const auto& f : shot.track.frames)
			rows.push_back({leadin::round1(f.car.location.x), leadin::round1(f.car.location.y), leadin::round1(f.car.location.z),
				f.car.rotation.pitch, f.car.rotation.yaw, f.car.rotation.roll, leadin::round1(f.car.velocity.x),
				leadin::round1(f.car.velocity.y), leadin::round1(f.car.velocity.z), f.boost ? 1 : 0});

		const leadin::Frame& start = shot.track.frames.front();
		const leadin::Frame& free = shot.track.frames.back();
		nlohmann::ordered_json doc;
		doc["id"] = id;
		doc["name"] = "Shot Bank: shot " + std::to_string(number) + " of " + std::to_string(total);
		doc["category"] = "captured";
		doc["description"] = "One of your own shots coming back at you, build-up and all.";
		doc["tags"] = nlohmann::ordered_json::array({"shot bank"});
		doc["allowMirror"] = false;
		doc["timeLimitSeconds"] = 4;
		doc["shotReplay"] = true;
		doc["ball"] = {{"location", vec1(free.ball.location)}, {"velocity", vec1(free.ball.velocity)},
			{"angularVelocity", vec2(free.ball.angularVelocity)}};
		doc["player"] = {{"location", nlohmann::ordered_json::array({0, -4900, 17})},
			{"rotation", nlohmann::ordered_json::array({0, 16384, 0})}, {"boost", 100}};
		doc["leadIn"] = leadin::toJson(shot.track);
		doc["bots"] = nlohmann::ordered_json::array({{{"type", "psyonix"}, {"team", 1}, {"replay", true}, {"keepType", true}, {"name", carName},
			{"location", vec1(start.car.location)},
			{"rotation", nlohmann::ordered_json::array({start.car.rotation.pitch, start.car.rotation.yaw, start.car.rotation.roll})},
			{"velocity", vec1(start.car.velocity)}, {"boost", 100},
			{"track", {{"hz", shot.track.hz}, {"frames", std::move(rows)}}}}});
		doc["success"] = {{"type", "save"}};
		doc["fail"] = nlohmann::ordered_json::array({{{"type", "conceded"}}});
		doc["onSuccess"] = "next";
		doc["onFail"] = "next";
		doc["advanceDelaySeconds"] = 2;
		return doc;
	}
}
