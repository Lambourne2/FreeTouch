#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "bots/PolicyBot.h"
#include "bots/PuppetKeeper.h"
#include "game/GameState.h"

class EventLog;
class GameAdapter;
struct ResolvedScenario;

// Sets up the bots a rep asks for, drives them and removes them when the exercise stops:
//  - "psyonix": a normal Psyonix bot (skill from params.skill); placed at its start once per rep, then it plays.
//  - "puppet" + "keeper": a scripted keeper whose state is set every tick (looks robotic; for scripted drills).
//  - "necto": a Psyonix bot car driven by the Necto network (PolicyBot), placed at its start once per rep. With the
//    Defender setting on Necto, "psyonix" bots play as Necto too.
//  Free Touch's bots are named "FT ..." so only they are ever removed. Game thread. No SDK calls.
class BotManager
{
public:
	BotManager(GameAdapter& game, EventLog& log) : game_(game), log_(log) {}

	void onRepStart(const ResolvedScenario& rep);
	// The network bots: id ("necto") -> a fresh PolicyBot, or null when its model is not there.
	void setBrainFactory(std::function<std::shared_ptr<PolicyBot>(const std::string& id)> factory) { makeBrain_ = std::move(factory); }
	// "psyonix" (as the scenario says), "necto" or "nexto": who defends in drills with a Psyonix defender.
	void setDefender(const std::string& defender) { defender_ = defender; }
	void onTick(const Snapshot& snap);
	void stop();
	// The rep's start countdown ends on this tick (0 = none running): bots with a countdown stay frozen until then,
	// released on the same tick as the player's car.
	void setHeldUntil(uint64_t tick) { heldUntil_ = tick; }
	// The rep's lead-up (replay takeover, Saved Shots): seconds into it, -1 when none is running. Bots follow their
	// "track" meanwhile (or wait at their start) and are released with the player.
	void setLeadInTime(double seconds) { leadInT_ = seconds; }
	// Every bot of this rep is in the game and at its start (the countdown waits for this).
	bool ready() const;

private:
	struct Keeper
	{
		std::string name;
		bool puppet = true;      // false = Psyonix bot that plays on its own (or driven by `brain`)
		std::shared_ptr<PolicyBot> brain; // a network bot's driver (Necto)
		bool brainLogged = false;
		bool speedflipLogged = false;
		float holdSeconds = 0.f;   // reaction time: coasts with no inputs this long after being placed
		float freezeSeconds = 0.f; // the rep's start countdown: held at its start (no speed) this long
		bool unfrozen = false;
		bool bodyChecked = false;  // its car is an Octane (the game sometimes gives a spawned bot another body)
		int respawns = 0;
		uint64_t placedTick = 0;
		uint64_t goTick = 0;       // when the countdown released it
		bool released = true;
		float skill = 0.5f;
		int team = 1;              // wanted team (0 = the player's side)
		bool teamChecked = false;
		std::vector<std::array<float, 9>> track; // lead-up: x y z pitch yaw roll vx vy vz at trackHz
		double trackHz = 60.0;
		CarTarget start;         // where a Psyonix bot is put at the start of each rep
		bool placed = false;
		PuppetKeeper logic;
		bool spawnRequested = false;
		bool ready = false;
		uint64_t carId = 0;
		int ticksWaiting = 0;
	};

	GameAdapter& game_;
	EventLog& log_;
	std::vector<Keeper> keepers_;
	bool haveBots_ = false;
	std::function<std::shared_ptr<PolicyBot>(const std::string&)> makeBrain_;
	std::string defender_ = "psyonix";
	uint64_t heldUntil_ = 0;
	double leadInT_ = -1.0;
	bool repLeadIn_ = false;   // this rep has a lead-up (bots wait for it)
};
