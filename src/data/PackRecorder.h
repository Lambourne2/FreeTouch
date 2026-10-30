#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "data/Capture.h"
#include "data/StateJson.h"
#include "game/GameState.h"
#include "third_party/nlohmann/json.hpp"

// Training pack recorder: while the player plays a custom training pack, each shot's start is read while the game
// holds the ball and car frozen in place (the ball's location stays put, though the game already gives it its launch
// velocity), and the shot's time limit from the round clock. When the shot starts, the launch velocity is taken from
// the frozen ball, or from its first moving tick. Every recorded shot goes into one scenario per pack that plays like
// the game's training: shots in order, frozen until the first input with the ball's path drawn, the pack's clock
// counting down, and time up waiting for the ball to land. No SDK calls.
namespace packs
{
	using json = nlohmann::ordered_json;

	constexpr const char* kRecorderTag = "pack-recorder-2"; // files written by this version (older ones are not merged)
	constexpr double kDefaultSeconds = 10.0;                // a shot with no time limit in the pack

	struct Shot
	{
		int round = 0;
		Vec3 ballLocation, ballVelocity, ballSpin;
		Vec3 carLocation;
		Rot carRotation;
		double seconds = 0.0; // the shot's time limit in the pack (0 = none found)
	};

	class Recorder
	{
	public:
		// One training tick. True when a shot has just been recorded (last() is it).
		bool feed(const Snapshot& s, const TrainingInfo& t)
		{
			const CarState* me = s.localCar();
			if (!t.active || !s.ball || !me)
				return false;
			const std::string key = t.code.empty() ? t.name : t.code;
			if (key != packKey_ || t.round != round_)
			{
				packKey_ = key;
				round_ = t.round;
				phase_ = Phase::Waiting;
				stillTicks_ = 0;
				haveStart_ = false;
				havePrev_ = false;
			}
			const Vec3 ball = s.ball->location, car = me->location;
			// How far each moved since the last tick; a jump of more than a car length is a teleport (the game
			// placing the next shot, or the old ball leaving the net), not a launch.
			const float ballStep = havePrev_ ? (ball - prevBall_).length() : 0.f;
			const float carStep = havePrev_ ? (car - prevCar_).length() : 0.f;
			const bool teleport = havePrev_ && (ballStep > 400.f || carStep > 400.f);
			prevBall_ = ball;
			prevCar_ = car;
			const bool firstTick = !havePrev_;
			havePrev_ = true;
			const bool placed = ball.z > 50.f || std::fabs(ball.x) + std::fabs(ball.y) > 1.f; // (0, 0, 0): not placed yet
			const bool held = !firstTick && !teleport && ballStep < 0.5f && carStep < 0.5f;

			switch (phase_)
			{
			case Phase::Waiting:
				if (teleport || !placed)
				{
					stillTicks_ = 0;
					haveStart_ = false;
					return false;
				}
				if (held)
				{
					// Frozen on the shot's start: read it once it has held still for a few ticks.
					if (++stillTicks_ >= 4)
					{
						pending_ = {};
						pending_.round = t.round;
						pending_.ballLocation = ball;
						// The frozen ball reads still; the game's own trajectory for the shot has its launch velocity.
						pending_.ballVelocity = t.trajectoryVelocity.length() > 1.f ? t.trajectoryVelocity : s.ball->velocity;
						pending_.ballSpin = s.ball->angularVelocity;
						pending_.carLocation = car;
						pending_.carRotation = me->rotation;
						frozenSeconds_ = haveStart_ ? std::max(frozenSeconds_, t.secondsLeft) : t.secondsLeft;
						pending_.seconds = frozenSeconds_;
						haveStart_ = true;
					}
					return false;
				}
				if (!haveStart_)
				{
					stillTicks_ = 0;
					return false;
				}
				// It moved a little: the shot has started.
				phase_ = Phase::Launched;
				launchTicks_ = 0;
				return launched(s, ballStep);
			case Phase::Launched:
				return launched(s, ballStep);
			case Phase::Done:
				// Placed again (the shot reset, or the game placing it after a step): read it afresh.
				if (teleport)
				{
					phase_ = Phase::Waiting;
					stillTicks_ = 0;
					haveStart_ = false;
				}
				return false;
			}
			return false;
		}

		const Shot& last() const { return last_; }
		// The shot as the game holds it frozen (its launch velocity included), once it has held still `minTicks`.
		bool frozen(Shot& out, int minTicks) const
		{
			if (phase_ != Phase::Waiting || !haveStart_ || stillTicks_ < minTicks)
				return false;
			out = pending_;
			return true;
		}
		int round() const { return round_; }

	private:
		enum class Phase { Waiting, Launched, Done };

		bool launched(const Snapshot& s, float ballStep)
		{
			++launchTicks_;
			const bool ballMoving = ballStep >= 0.5f;
			// The frozen ball already carried its launch velocity; a ball frozen with none (a still ball, or a game
			// that sets it at the start) gets it from its first moving tick.
			if (pending_.ballVelocity.length() < 5.f)
			{
				if (!ballMoving && launchTicks_ < 10)
					return false;
				if (ballMoving)
				{
					pending_.ballVelocity = s.ball->velocity + Vec3(0.f, 0.f, 650.f * launchTicks_ / 120.f);
					pending_.ballSpin = s.ball->angularVelocity;
				}
			}
			phase_ = Phase::Done;
			last_ = pending_;
			return true;
		}

		std::string packKey_;
		int round_ = -1;
		Phase phase_ = Phase::Waiting;
		int stillTicks_ = 0;
		bool haveStart_ = false;
		bool havePrev_ = false;
		Vec3 prevBall_, prevCar_;
		double frozenSeconds_ = 0.0;
		int launchTicks_ = 0;
		Shot pending_, last_;
	};

	inline double shotSeconds(double recorded)
	{
		// The round clock while frozen is the shot's limit; nothing sensible read means no limit in the pack.
		return recorded >= 1.0 && recorded <= 60.0 ? std::round(recorded * 10.0) / 10.0 : kDefaultSeconds;
	}

	// One shot as a one_of choice (custom training's striker shots are at the orange net, +y, as Free Touch's are).
	inline json shotChoice(const Shot& sh, int number, int total)
	{
		const double seconds = shotSeconds(sh.seconds);
		json c;
		c["id"] = "shot_" + std::to_string(number);
		c["label"] = "shot " + std::to_string(number) + " of " + std::to_string(total);
		c["objects"] = json::array();
		c["ball"] = {{"location", capture::vec1(sh.ballLocation)}, {"velocity", capture::vec1(sh.ballVelocity)},
			{"angularVelocity", capture::vec1(sh.ballSpin)}};
		c["player"] = {{"location", capture::vec1(sh.carLocation)}, {"rotation", statejson::toJson(sh.carRotation)},
			{"velocity", json::array({0, 0, 0})}, {"boost", 100}, {"boostLock", true}};
		c["timeLimitSeconds"] = seconds;
		c["success"] = {{"type", "goal"}};
		c["fail"] = json::array({{{"type", "timeout"}, {"seconds", seconds}}});
		return c;
	}

	// A pack code as the game writes it (XXXX-XXXX-XXXX-XXXX, hex), from what a player typed or pasted; "" if it is not one.
	inline std::string normalizeCode(const std::string& text)
	{
		std::string hex;
		for (const char ch : text)
		{
			const auto c = static_cast<unsigned char>(ch);
			if (std::isxdigit(c))
				hex += static_cast<char>(std::toupper(c));
			else if (ch != '-' && ch != ' ' && ch != '\t')
				return "";
		}
		if (hex.size() != 16)
			return "";
		return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4);
	}

	// The code of a pack drill made from a shared pack ("packs.42bf-686d-e047-574b"), or "".
	inline std::string codeFromId(const std::string& id)
	{
		return id.rfind("packs.", 0) == 0 ? normalizeCode(id.substr(6)) : std::string();
	}

	inline std::string packId(const TrainingInfo& t)
	{
		const std::string base = !t.code.empty() ? t.code : !t.name.empty() ? t.name : "unnamed";
		return "packs." + capture::sanitizeId(base);
	}

	// Shot number (1-based) of a choice id "shot_7", or 0.
	inline int choiceNumber(const json& choice)
	{
		const std::string id = choice.value("id", "");
		return id.rfind("shot_", 0) == 0 ? std::atoi(id.c_str() + 5) : 0;
	}

	// The choices already saved for a pack by this recorder version (by shot number), to add new shots to.
	inline std::map<int, json> savedChoices(const json& doc)
	{
		std::map<int, json> out;
		if (!doc.is_object())
			return out;
		const auto tags = doc.value("tags", json::array());
		if (std::find(tags.begin(), tags.end(), kRecorderTag) == tags.end())
			return out;
		for (const auto& o : doc.value("objects", json::array()))
		{
			for (const auto& c : o.value("choices", json::array()))
			{
				if (const int n = choiceNumber(c); n > 0)
					out[n] = c;
			}
		}
		return out;
	}

	// The pack's scenario from its choices (by shot number), or null if there are none. Labels are renumbered to the
	// pack's shot count.
	inline json scenario(const TrainingInfo& t, std::map<int, json> choices, const std::string& stamp)
	{
		if (choices.empty())
			return nullptr;
		const int total = std::max(t.rounds, choices.rbegin()->first);
		json list = json::array();
		for (auto& [n, c] : choices)
		{
			c["label"] = "shot " + std::to_string(n) + " of " + std::to_string(total);
			list.push_back(c);
		}
		const std::string name = t.name.empty() ? "Training pack" : t.name;
		json doc;
		doc["id"] = packId(t);
		doc["name"] = "Pack: " + name;
		doc["category"] = "packs";
		std::string about = "From the training pack \"" + name + "\"";
		if (!t.creator.empty())
			about += " by " + t.creator;
		if (!t.code.empty())
			about += " (" + t.code + ")";
		about += ": " + std::to_string(choices.size()) + " of " + std::to_string(total)
			+ " shots, in order. Each waits for your first input, with the ball's path shown; the pack's clock counts down.";
		doc["description"] = about;
		doc["tags"] = json::array({"training pack", kRecorderTag});
		doc["source"] = {{"kind", "captured"}, {"label", "training pack " + (t.code.empty() ? name : t.code) + ", " + stamp}};
		doc["allowMirror"] = false;
		doc["progression"] = "loop";
		doc["waitForInput"] = true;
		doc["showTrajectory"] = true;
		doc["clockCountsDown"] = true;
		doc["timeUpWaitsForGround"] = true;
		doc["timeLimitSeconds"] = list[0]["timeLimitSeconds"];
		doc["ball"] = list[0]["ball"];
		doc["player"] = list[0]["player"];
		doc["success"] = list[0]["success"];
		doc["fail"] = list[0]["fail"];
		doc["objects"] = json::array({{{"type", "one_of"}, {"id", "shot"}, {"choices", list}}});
		doc["onSuccess"] = "next";
		doc["onFail"] = "next";
		doc["advanceDelaySeconds"] = 1.2;
		doc["notes"] = "Play the pack in custom training to add or update its shots (game training type " + std::to_string(t.type) + ").";
		return doc;
	}
}
