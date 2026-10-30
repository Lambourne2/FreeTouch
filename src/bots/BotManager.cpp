#include "pch.h"
#include "bots/BotManager.h"

#include <algorithm>

#include "core/Rng.h"
#include "data/EventLog.h"
#include "data/StateJson.h"
#include "game/GameAdapter.h"
#include "scenario/ScenarioLoader.h"

namespace
{
	constexpr auto kBotPrefix = "FT ";
	constexpr int kRespawnRequestTicks = 600; // ask again if a bot has not shown up within 5 s

	Vec3 vec(const nlohmann::json& v)
	{
		return v.is_array() && v.size() == 3 ? Vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>()) : Vec3();
	}
}

void BotManager::onRepStart(const ResolvedScenario& rep)
{
	std::vector<Keeper> wanted;
	repLeadIn_ = rep.resolved.contains("leadIn");
	int index = 0;
	if (rep.resolved.contains("bots"))
	{
		for (const auto& bot : rep.resolved["bots"])
		{
			++index;
			std::string type = bot.value("type", "");
			const std::string behavior = bot.value("behavior", "none");
			if (type == "psyonix" && (defender_ == "necto" || defender_ == "nexto") && !bot.value("keepType", false))
				type = defender_; // the Defender / Opponent setting
			if (type == "necto" || type == "nexto" || type == "psyonix")
			{
				Keeper k;
				k.puppet = false;
				if (type != "psyonix" && makeBrain_)
				{
					k.brain = makeBrain_(type);
					if (!k.brain)
						log_.write("bot_model_missing", {{"exercise", rep.id}, {"bot", type}, {"fallback", "psyonix"}});
				}
				const std::string base = !k.brain ? "FT Defender" : type == "nexto" ? "FT Nexto" : "FT Necto";
				k.name = base + (wanted.empty() ? std::string() : " " + std::to_string(wanted.size() + 1));
				if (k.brain)
				{
					// Difficulty: the bot's own knobs for this level (bot.json), the scenario's params on top. Beta 1
					// always plays its best action, lower plays looser (the scenario can only lower it); reaction time
					// and a ground speed cap make the easier levels beatable (medium and hard were the same Nexto).
					const nlohmann::json params = bot.value("params", nlohmann::json::object());
					const PolicyBotConfig::Knobs knobs = k.brain->config().knobsFor(rep.difficulty);
					const float beta = std::min(params.value("beta", 1.f), knobs.beta);
					const float reaction = params.value("reactionSeconds", knobs.reactionSeconds);
					const float maxSpeed = params.value("maxSpeed", knobs.maxSpeed);
					k.brain->reset(beta, static_cast<uint64_t>(rep.seed) * 2654435761ull + static_cast<uint64_t>(index));
					k.brain->setKnobs(reaction, maxSpeed);
					k.brain->setScriptedKickoff(params.value("scriptedKickoff", true));
					log_.write("bot_difficulty", {{"exercise", rep.id}, {"bot", type}, {"difficulty", rep.difficulty},
						{"beta", beta}, {"reactionSeconds", reaction}, {"maxSpeed", maxSpeed}});
				}
				k.skill = bot.contains("params") ? bot["params"].value("skill", 0.5f) : 0.5f;
				k.holdSeconds = bot.contains("params") ? std::max(0.f, bot["params"].value("holdSeconds", 0.f)) : 0.f;
				k.freezeSeconds = std::max(0.f, rep.resolved.value("startCountdownSeconds", 0.f));
				if (repLeadIn_ || rep.resolved.value("waitForInput", false))
					k.freezeSeconds = std::max(k.freezeSeconds, 0.01f); // held through the lead-up / until the player starts
				k.team = bot.value("team", 1);
				if (bot.contains("track") && bot["track"].contains("frames"))
				{
					k.trackHz = std::max(1.0, bot["track"].value("hz", 60.0));
					for (const auto& row : bot["track"]["frames"])
					{
						if (!row.is_array() || row.size() != 9)
							continue;
						std::array<float, 9> f{};
						for (int i = 0; i < 9; ++i)
							f[i] = row[i].get<float>();
						k.track.push_back(f);
					}
				}
				k.start.location = vec(bot.value("location", nlohmann::json::array({0, 5000, 17})));
				const Vec3 r = vec(bot.value("rotation", nlohmann::json::array({0, -16384, 0})));
				k.start.rotation = {static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.z)};
				k.start.velocity = vec(bot.value("velocity", nlohmann::json::array({0, 0, 0})));
				if (bot.contains("boost") && bot["boost"].is_number())
					k.start.boost = bot["boost"].get<float>();
				const float readBack = game_.setBotSkill(k.skill);
				log_.write("defender_setup", {{"exercise", rep.id}, {"name", k.name}, {"skill", k.skill},
					{"skillReadBack", readBack}, {"start", statejson::toJson(k.start.location)},
					{"yaw", k.start.rotation.yaw}, {"velocity", statejson::toJson(k.start.velocity)}});
				wanted.push_back(std::move(k));
				continue;
			}
			if (type != "puppet" || behavior != "keeper")
			{
				log_.write("bot_unsupported", {{"exercise", rep.id}, {"botType", type}, {"behavior", behavior}});
				continue;
			}
			Keeper k;
			k.name = wanted.empty() ? "FT Keeper" : "FT Keeper " + std::to_string(wanted.size() + 1);
			KeeperParams params;
			if (bot.contains("params"))
			{
				const auto& p = bot["params"];
				params.reaction = p.value("reaction", params.reaction);
				params.maxSpeed = p.value("maxSpeed", params.maxSpeed);
				params.error = p.value("error", params.error);
			}
			const Vec3 start = vec(bot.value("location", nlohmann::json::array({0, 5000, 17})));
			// Error offset from the rep's seed, so a retry gets the same keeper.
			Rng rng(static_cast<uint64_t>(rep.seed) * 0x9E3779B97F4A7C15ull + static_cast<uint64_t>(index));
			const float offset = static_cast<float>(rng.range(-params.error, params.error));
			k.logic.reset(start, start.y >= 0.f ? 1 : -1, params, offset);
			log_.write("keeper_setup", {
				{"exercise", rep.id},
				{"name", k.name},
				{"start", statejson::toJson(start)},
				{"reaction", params.reaction},
				{"maxSpeed", params.maxSpeed},
				{"error", params.error},
				{"errorOffset", statejson::round2(offset)},
			});
			wanted.push_back(std::move(k));
		}
	}

	// Keep bots that are still wanted (same name), so a new rep does not respawn them.
	for (auto& k : wanted)
	{
		for (const auto& old : keepers_)
		{
			if (old.name == k.name)
			{
				k.spawnRequested = old.spawnRequested;
				k.carId = old.carId;
				k.bodyChecked = old.bodyChecked;
				k.teamChecked = old.teamChecked && old.team == k.team;
				k.respawns = old.respawns;
			}
		}
	}
	// A bot that is no longer wanted goes, also when another takes its place (switching Psyonix to Necto kept the
	// count the same and left the old bot in the game, Lambourne 2026-10-20). Removing clears them all; the wanted
	// ones spawn again.
	bool anyGone = false;
	for (const auto& old : keepers_)
		anyGone = anyGone || std::none_of(wanted.begin(), wanted.end(), [&](const Keeper& k) { return k.name == old.name; });
	if (anyGone || (wanted.empty() && haveBots_))
	{
		const int removed = game_.removeBots(kBotPrefix);
		log_.write("bots_removed", {{"count", removed}, {"why", wanted.empty() ? "not in this rep" : "replaced by another bot"}});
		for (auto& k : wanted)
		{
			k.spawnRequested = false;
			k.carId = 0;
		}
		haveBots_ = false;
	}
	keepers_ = std::move(wanted);
}

void BotManager::onTick(const Snapshot& snap)
{
	if (keepers_.empty())
		return;
	for (auto& k : keepers_)
	{
		const CarState* car = nullptr;
		for (const auto& c : snap.cars)
		{
			if (c.isBot && c.name == k.name)
				car = &c;
		}
		if (!car)
		{
			// Not there yet (just spawned, or demolished and waiting to respawn).
			if (!k.spawnRequested || ++k.ticksWaiting > kRespawnRequestTicks)
			{
				game_.spawnBot(k.name);
				k.spawnRequested = true;
				k.ticksWaiting = 0;
				haveBots_ = true;
				log_.write("bot_spawn_requested", {{"name", k.name}});
			}
			k.ready = false;
			continue;
		}
		k.ticksWaiting = 0;
		if (!k.puppet && !k.bodyChecked)
		{
			// Same hitbox as the player's Octane, so touches behave the way the bot (and the drill) expect. The game
			// sometimes gives a spawned bot a preset car (a Breakout, ...) whatever SpawnBot asked for, and
			// GetLoadoutBody still reads 23: set the bot's loadout to the Octane once it is there.
			constexpr int kOctane = 23;
			k.bodyChecked = true;
			const bool set = game_.setBotBody(car->id, kOctane);
			log_.write("bot_body", {{"name", k.name}, {"bodyRead", car->body}, {"loadoutSet", set}});
		}
		if (!k.puppet && !k.teamChecked)
		{
			// A teammate (replay takeover): bots spawn on orange; move it to the player's side once.
			k.teamChecked = true;
			if (car->team != k.team)
			{
				const bool moved = game_.setBotTeam(car->id, k.team);
				k.placed = false; // put it at its start again (a team change can respawn it)
				log_.write("bot_team", {{"name", k.name}, {"from", car->team}, {"to", k.team}, {"called", moved}});
			}
		}
		if (!k.puppet)
		{
			// A Psyonix bot plays on its own: put it at its start once per rep (its id is the player, so a
			// respawn mid-rep does not move it again).
			if (!k.placed || k.carId != car->id)
			{
				k.placed = true;
				k.carId = car->id;
				k.placedTick = snap.tick;
				k.released = k.holdSeconds <= 0.f && k.freezeSeconds <= 0.f;
				k.unfrozen = false;
				// Teleported with a full tank, so it picks up no pads between where it was and its start; a bot held
				// by the countdown keeps the full tank until go, when it gets its start boost (it is not moving then).
				CarTarget place = k.start;
				if (place.boost)
					place.boost = 100.f;
				game_.setCarState(car->id, place);
				log_.write("defender_placed", {{"name", k.name}, {"carId", car->id}, {"from", statejson::toJson(car->location)},
					{"holdSeconds", k.holdSeconds}});
			}
			if (!k.released)
			{
				// Frozen until the player's countdown says go (it waits for this bot to be placed), or through the rep's
				// lead-up, following its own recorded track.
				const bool frozen = k.freezeSeconds > 0.f && !k.unfrozen && (snap.tick < heldUntil_ || leadInT_ >= 0.0
					|| (repLeadIn_ && snap.tick < k.placedTick + 2));
				if (frozen && leadInT_ >= 0.0 && !k.track.empty())
				{
					// Its lead-up, interpolated, moving at its recorded speed.
					const double pos = std::clamp(leadInT_ * k.trackHz, 0.0, static_cast<double>(k.track.size() - 1));
					const size_t i = std::min(static_cast<size_t>(pos), k.track.size() - 1), j = std::min(i + 1, k.track.size() - 1);
					const float u = static_cast<float>(pos - i);
					const auto& a = k.track[i];
					const auto& b = k.track[j];
					auto lerp = [&](int n) { return a[n] + (b[n] - a[n]) * u; };
					auto angle = [&](int n) {
						int d = (static_cast<int>(b[n]) - static_cast<int>(a[n])) % 65536;
						if (d > 32768) d -= 65536;
						if (d < -32768) d += 65536;
						int r = static_cast<int>(a[n]) + static_cast<int>(std::lround(d * u));
						if (r >= 32768) r -= 65536;
						if (r < -32768) r += 65536;
						return r;
					};
					CarTarget pose;
					pose.location = {lerp(0), lerp(1), lerp(2)};
					pose.rotation = {angle(3), angle(4), angle(5)};
					pose.velocity = {lerp(6), lerp(7), lerp(8)};
					pose.boost = k.start.boost ? std::optional<float>(100.f) : std::nullopt;
					game_.setCarState(car->id, pose);
					game_.setBotInput(car->id, CarInput{});
					continue;
				}
				if (frozen)
				{
					// The start countdown: held at its start, still.
					CarTarget hold = k.start;
					hold.velocity = hold.angularVelocity = {};
					if (hold.boost)
						hold.boost = 100.f;
					game_.setCarState(car->id, hold);
					game_.setBotInput(car->id, CarInput{});
					continue;
				}
				if (k.freezeSeconds > 0.f && !k.unfrozen)
				{
					k.unfrozen = true;
					k.goTick = snap.tick;
					game_.setCarState(car->id, k.start); // go: its start speed
					log_.write("bot_go", {{"name", k.name}, {"tick", snap.tick}});
				}
				// Reaction time: no inputs, so it keeps rolling the way it was placed (backing up when shadowing).
				const uint64_t from = k.freezeSeconds > 0.f ? k.goTick : k.placedTick;
				if (snap.tick < from + static_cast<uint64_t>(k.holdSeconds * 120.f))
				{
					game_.setBotInput(car->id, CarInput{});
					continue;
				}
				k.released = true;
				if (!k.brain)
					game_.setBotInput(car->id, std::nullopt); // the Psyonix AI takes over
				log_.write("defender_released", {{"name", k.name}, {"at", statejson::toJson(car->location)}});
			}
			if (k.brain)
			{
				// Necto drives: its controls replace the Psyonix AI's on every input call.
				game_.setBotInput(car->id, k.brain->tick(snap, car->id));
				if (snap.tick % 1200 == 0)
				{
					const auto t = k.brain->takeTiming();
					log_.write("bot_timing", {{"name", k.name}, {"ticks", t.ticks}, {"decisions", t.decisions},
						{"avgMs", statejson::round2(static_cast<float>(t.avgMs))}, {"maxMs", statejson::round2(static_cast<float>(t.maxMs))}});
				}
				if (!k.speedflipLogged && k.brain->kickedOff())
				{
					k.speedflipLogged = true;
					log_.write("bot_speedflip", {{"name", k.name}, {"at", statejson::toJson(car->location)}, {"tick", snap.tick}});
				}
				if (!k.brainLogged)
				{
					k.brainLogged = true;
					const auto& a = k.brain->action();
					log_.write("necto_first_decision", {{"name", k.name}, {"ms", statejson::round2(static_cast<float>(k.brain->lastInferenceMs()))},
						{"action", {a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]}}});
				}
			}
			continue;
		}
		if (!k.ready || k.carId != car->id)
		{
			k.ready = true;
			k.carId = car->id;
			game_.setBotInput(car->id, CarInput{}); // no AI inputs: the puppet only moves where we put it
			log_.write("keeper_ready", {{"name", k.name}, {"carId", car->id}, {"at", statejson::toJson(car->location)}});
		}
		if (!snap.ball)
			continue;
		game_.setCarState(car->id, k.logic.step(*snap.ball));
	}
}

bool BotManager::ready() const
{
	for (const auto& k : keepers_)
		if (k.carId == 0 || (k.puppet ? !k.ready : !k.placed))
			return false;
	return true;
}

void BotManager::stop()
{
	if (keepers_.empty() && !haveBots_)
		return;
	for (const auto& k : keepers_)
	{
		if (k.carId != 0)
			game_.setBotInput(k.carId, std::nullopt);
	}
	const int removed = game_.removeBots(kBotPrefix);
	log_.write("bots_removed", {{"count", removed}, {"why", "exercise stopped"}});
	keepers_.clear();
	haveBots_ = false;
}
