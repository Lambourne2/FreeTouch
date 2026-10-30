#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "bots/policy/EarlPolicy.h"
#include "bots/policy/NectoObs.h"
#include "bots/policy/NextoObs.h"
#include "core/Rng.h"
#include "game/GameState.h"

// A neural-network bot (Necto or Nexto): every `tickSkip` ticks it builds the observation
// from the snapshot, runs the policy and keeps those controls until the next decision, like the bot does in RLBot.
// The car is a spawned Psyonix bot whose inputs are overwritten every tick (SetVehicleInput params, Phase 0).
// content/bots/<id>/bot.json holds its settings, including sign conventions that can be fixed in game without a
// rebuild. No SDK calls.
struct PolicyBotConfig
{
	std::string name = "Necto";
	std::string obs = "necto"; // necto (five action heads) | nexto (one pick from its 90-action table)
	int heads = 4;
	int tickSkip = 8;
	float rollSign = 1.f;            // flip if the bot drives as if upside down
	Vec3 angularVelocitySign{1, 1, 1};
	// Kickoffs, as the bot's own RLBot agent does them: "speedflip" plays Nexto's scripted speedflip while the ball sits
	// untouched at the centre; stochasticKickoffs makes the network play looser (beta 0.5) until the ball is touched.
	std::string kickoff;
	bool stochasticKickoffs = false;
	// Difficulty knobs (bot.json "difficulty": {"easy": {...}, "medium": {...}, "hard": {...}}): `beta` (lower plays
	// looser; a scenario's own beta can only lower it), `reactionSeconds` (it sees the ball and the other cars this
	// late; its own car it knows at once) and `maxSpeed` (no boost on the ground above this, uu/s; 0 = no cap).
	struct Knobs
	{
		float beta = 1.f;
		float reactionSeconds = 0.f;
		float maxSpeed = 0.f;
	};
	std::map<std::string, Knobs> difficulty;
	Knobs knobsFor(const std::string& level) const
	{
		const auto it = difficulty.find(level);
		return it == difficulty.end() ? Knobs{} : it->second;
	}

	static bool load(const std::filesystem::path& file, PolicyBotConfig& out, std::string& error);
};

class PolicyBot
{
public:
	PolicyBot(std::shared_ptr<const EarlPolicy> policy, PolicyBotConfig config)
		: policy_(std::move(policy)), config_(std::move(config)), obs_(config_.tickSkip) {}

	// A new rep: fresh timers, no last action. `beta` 1 = always the best action (hardest); lower plays looser
	// (0.5 samples by the network's own odds), as in Necto's bot. `seed` makes a rep repeatable.
	void reset(float beta, uint64_t seed);
	// Off: no scripted kickoff this rep, the network plays it (easier).
	void setScriptedKickoff(bool on) { scriptedKickoff_ = on; }
	// The difficulty's reaction time and ground speed cap (see PolicyBotConfig::Knobs), for this rep.
	void setKnobs(float reactionSeconds, float maxSpeed)
	{
		reactionTicks_ = static_cast<int>(std::lround(std::clamp(reactionSeconds, 0.f, 1.f) * 120.f));
		maxSpeed_ = std::max(0.f, maxSpeed);
	}

	// Game thread, every tick: the controls for the bot's car `carId` (nullopt when that car is not in the snapshot).
	std::optional<CarInput> tick(const Snapshot& s, uint64_t carId);
	// Nexto's scripted kickoff (bot.py KICKOFF_CONTROLS), one action per tick; empty past its end.
	static std::optional<std::array<float, 8>> speedflipAction(int tick);
	bool kickedOff() const { return kickoffLogged_; }

	const std::array<float, 8>& action() const { return action_; }
	const PolicyBotConfig& config() const { return config_; }
	double lastInferenceMs() const { return lastMs_; }
	// Since the last call: ticks driven, decisions made, their average and slowest time (2026-10-19: hard Nexto felt
	// a little slow to react at times; logged as bot_timing so a slow PC would show).
	struct Timing
	{
		int ticks = 0, decisions = 0;
		double avgMs = 0.0, maxMs = 0.0;
	};
	Timing takeTiming()
	{
		Timing t{timingTicks_, timingDecisions_, timingDecisions_ ? timingMsSum_ / timingDecisions_ : 0.0, timingMsMax_};
		timingTicks_ = timingDecisions_ = 0;
		timingMsSum_ = timingMsMax_ = 0.0;
		return t;
	}

private:
	std::shared_ptr<const EarlPolicy> policy_;
	PolicyBotConfig config_;
	necto::ObsBuilder obs_;
	std::array<float, 8> action_{};
	int ticksToNext_ = 0;
	float beta_ = 1.f;
	Rng rng_{1};
	double lastMs_ = 0.0;
	bool scriptedKickoff_ = true;
	int kickoffIndex_ = -1;     // ticks into the scripted kickoff; -1 not at a kickoff, -2 not the kickoff taker
	bool kickoffLogged_ = false;
	int reactionTicks_ = 0;
	float maxSpeed_ = 0.f;
	int timingTicks_ = 0, timingDecisions_ = 0;
	double timingMsSum_ = 0.0, timingMsMax_ = 0.0;
	// What it saw: the ball and every car, one entry a tick (the last reactionTicks_ + 1).
	struct Seen
	{
		BallState ball;
		std::vector<CarState> cars;
	};
	std::deque<Seen> seen_;
};
