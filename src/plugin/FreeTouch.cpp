#include "pch.h"
#include "plugin/FreeTouch.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#include "data/Capture.h"
#include "core/Account.h"
#include "core/Friends.h"
#include "core/OnlineShots.h"
#include "core/Leaderboards.h"
#if FT_SELF_UPDATE
#include "core/Updater.h"
#endif
#include "data/SafeFile.h"
#include "data/ShotShare.h"
#include "data/StateJson.h"
#include "game/BakkesGameAdapter.h"
#include "scenario/ScenarioLoader.h"
#include "ui/Theme.h"

BAKKESMOD_PLUGIN(FreeTouch, "Free Touch: offline freeplay training", plugin_version, PLUGINTYPE_FREEPLAY)

std::shared_ptr<CVarManagerWrapper> _globalCvarManager;

namespace
{
	constexpr auto kReloadKey = "F9";
	constexpr auto kReloadCommand = "plugin unload freetouch; plugin load freetouch";
	constexpr auto kSelectKey = "XboxTypeS_Back";
	constexpr auto kMenuKey = "F7"; // F6 is the BakkesMod console
	constexpr auto kFreeplayId = "speed_lab.objectives"; // Freeplay+

	// Local time with a strftime format, e.g. "%Y%m%d-%H%M%S" for file names.
	std::string localTime(const char* format)
	{
		const std::time_t now = std::time(nullptr);
		std::tm tm{};
		localtime_s(&tm, &now);
		char buf[64];
		std::strftime(buf, sizeof(buf), format, &tm);
		return buf;
	}

	std::string joinArgs(const std::vector<std::string>& args, size_t first)
	{
		std::string text;
		for (size_t i = first; i < args.size(); ++i)
		{
			if (!text.empty())
				text += ' ';
			text += args[i];
		}
		return text;
	}
}

void FreeTouch::onLoad()
{
	_globalCvarManager = cvarManager;

	cvarManager->setBind(kReloadKey, kReloadCommand);

	game_ = std::make_unique<BakkesGameAdapter>(gameWrapper);
	openEventLog();

	player_ = std::make_unique<ScenarioPlayer>(*game_, content_, eventLog_);
	session_ = std::make_unique<SessionRunner>(*player_, content_, eventLog_);
	daily_ = std::make_unique<DailyRunner>(*player_, eventLog_);
	daily_->setChangedListener([this] {
		saveDaily();
		publishDaily();
		publishStatus();
	});
	daily_->setFinishedListener([this](const daily::Attempt& a) {
		++dailyFinishedSeq_; // the menu opens on the Daily page to show the result
		postDailyScore(a);
		playTrainingSound("new_best");
		publishDaily();
		publishStatus();
		if (!isWindowOpen_ && !unloading_)
			cvarManager->executeCommand("togglemenu " + GetMenuName());
	});
	player_->setResultListener([this](const ScenarioPlayer::Status& s) {
		daily_->onResult(s);
		session_->onResult(s);
		const auto* item = content_.find(ContentLibrary::Kind::Scenario, s.scenarioId);
		history_.onResult(today_, s.scenarioId, item ? item->category : "", static_cast<long long>(std::time(nullptr)),
			s.outcome, s.resultLabel);
		// Result cue: a new best, else a chime that climbs with the streak, or a soft thud.
		if (s.newBest)
			playTrainingSound("new_best");
		else if (s.outcome > 0)
			playTrainingSound("success", std::pow(2.f, std::min(std::max(s.streak - 1, 0), 7) / 12.f));
		else if (s.outcome < 0)
			playTrainingSound("fail");
		lastResultCueTick_ = tick_;
		countResult(s);
	});
	player_->setAdvanceRequest([this](bool keepCar) {
		if (daily_->active())
			return; // the daily moves on to its next shot itself
		if (session_->active())
			session_->continueAfterSuccess(keepCar); // the session may rotate to another exercise instead
		else
			player_->next(keepCar);
	});
	bots_ = std::make_unique<BotManager>(*game_, eventLog_);
	bots_->setBrainFactory([this](const std::string& id) { return makeBrain(id); });
	player_->setStartGate([this] { return bots_->ready(); });
	player_->setRepStartListener([this](const ResolvedScenario& rep) {
		bots_->onRepStart(rep);
		detector_.reset(); // a teleport: the last play is over
		history_.onPlayed(rep.id, rep.category, static_cast<long long>(std::time(nullptr)));
	});

	game_->addRoundResetListener([this] { player_->onRoundReset(); });
	game_->addGameEventListener([this](const std::string& name, uint64_t tick) {
		eventLog_.write("game_event", {{"name", name}, {"tick", tick}});
		// A real goal (let through for "Goal replays", or one that slipped past the catch): the game's slow motion and
		// replay follow, and the exercise waits for them (the HUD hides, the next rep comes after freeplay's reset).
		if (name.size() >= 15 && name.compare(name.size() - 15, 15, "EventGoalScored") == 0)
			player_->onRealGoal();
	});
	game_->addTouchListener([this](const TouchEvent& e) {
		touchLogger_.onTouch(e);
		detector_.onTouch(e);
		player_->onTouch(e);
	});
	game_->addTickListener([this](const Snapshot& s) {
		if (!firstTickLogged_)
		{
			firstTickLogged_ = true;
			eventLog_.write("first_tick", {{"tick", s.tick}, {"cars", s.cars.size()}, {"hasBall", s.ball.has_value()}});
		}
		tick_ = s.tick;
		tickPendingStart(s);
		if (s.tick % 120 == 0 || today_.empty())
		{
			const std::string day = localTime("%Y-%m-%d");
			if (day != today_)
			{
				today_ = day;
				if (routineLoaded_)
					publishRoutine(); // a new day: a fresh checklist
				publishDaily();      // and a new daily
			}
		}
		if (player_->active() && !s.paused)
			history_.addSeconds(today_, 1.0 / 120.0);
		if (s.tick % 3600 == 0 && history_.dirty())
		{
			saveHistory(); // every 30 s while something changed
			saveProgress();
		}
		touchLogger_.onTick(s);
		loadAccountOnce(); // once, and not in an online match (then once out of it)
		if (s.inFreeplay && !s.paused)
		{
			detector_.onTick(s);
			onMechanics();
		}
		leadInRecorder_.onTick(s);
		if (recording_)
		{
			recordRec_.onTick(s);
			if (s.tick >= recordStartTick_ + 15 * 120 || !s.inFreeplay)
				stopRecording(); // 15 s is the most it keeps
			else if ((s.tick - recordStartTick_) % 30 == 0)
				publishStatus(); // the page's clock
		}
		bots_->setHeldUntil(player_->countdownEndTick());
		bots_->setLeadInTime(player_->leadInTime());
		bots_->onTick(s);
		player_->onTick(s);
		session_->onTick(s);
		daily_->onTick(s.tick, daily_->active() ? player_->status() : ScenarioPlayer::Status{});
	});
	game_->start();

	// Select is the quick-test button while Free Touch is loaded: it runs ft_select_command. Its old bind
	// (cpt_do_checkpoint on Lambourne's PC) comes back on unload.
	// The arena Free Touch loads when an exercise is started from the main menu.
	cvarManager->registerCvar("ft_freeplay_map", "Stadium_P", "Map Free Touch loads for Free Play from the main menu");
	auto selectCommand = cvarManager->registerCvar("ft_select_command", "ft_next",
		"Console command the controller's Select button runs while Free Touch is loaded (quick testing)");
	if (selectCommand.getStringValue() == "ft_test_hide_next") // saved value from the removed hide test
		selectCommand.setValue("ft_next");
	if (selectCommand.getStringValue() == "ft_dump; ft_capture") // saved value from the capture test
		selectCommand.setValue("ft_next");
	// While the mechanic detectors are being tuned, Select marks a missed / misnamed mechanic (ft_mark); D-pad right
	// still skips to the next rep in exercises.
	if (selectCommand.getStringValue() == "ft_next")
		selectCommand.setValue("ft_mark");
	cvarManager->registerNotifier("ft_select", [this](std::vector<std::string>) {
		const std::string command = cvarManager->getCvar("ft_select_command").getStringValue();
		eventLog_.write("select_pressed", {{"command", command}});
		size_t start = 0; // several commands may be separated by ';'
		while (start <= command.size())
		{
			const size_t end = std::min(command.find(';', start), command.size());
			std::string one = command.substr(start, end - start);
			one.erase(0, one.find_first_not_of(' '));
			one.erase(one.find_last_not_of(' ') + 1);
			if (!one.empty())
				cvarManager->executeCommand(one);
			start = end + 1;
		}
	}, "Runs ft_select_command (bound to the controller's Select button)", PERMISSION_ALL);
	settings_ = std::make_unique<SettingSwapper>(cvarManager, game_->dataFolder() / "freetouch" / "settings_backup.json");
	if (const int recovered = settings_->recoverFromCrash())
	{
		LOG("Free Touch: restored {} bind(s) left over from a crash", recovered);
		eventLog_.write("binds_recovered", {{"count", recovered}});
	}
	repairGoalCvarsOnce();
	settings_->setBind(kSelectKey, "ft_select");
	eventLog_.write("bind_swapped", {{"key", kSelectKey}, {"original", settings_->originalBinds().at(kSelectKey)}});

	// Exercise binds: on their keys only while an exercise runs.
	controls_ = std::make_unique<ExerciseControls>(*game_, *settings_, eventLog_,
		game_->dataFolder() / "freetouch" / "settings" / "bindings.json");
	controls_->load();
	player_->setActiveListener([this](bool active) {
		controls_->setActive(active);
		exerciseBindsOn_ = active;
		updateFreeplayBindings();
		if (!active)
		{
			saveHistory();
			bots_->stop();
			session_->onPlayerStopped();
			daily_->onPlayerStopped();
			// Out of freeplay (the main menu) no ticks publish, so the window learns of the stop here.
			publishStatus();
		}
	});
	session_->setFinishedListener([this](const SessionSummary& summary) {
		// A Freeplay+ step's own objectives and boost were for its run only.
		if (routineFreeplayOverride_)
		{
			routineFreeplayOverride_ = false;
			player_->setChoiceFilter(kFreeplayId, freeplayEnabled_);
			player_->setBoostOverride(kFreeplayId, freeplayUnlimited_);
		}
		// A warm-up drill block played to the end (or a Freeplay+ step to its time) ticks its step.
		if ((summary.endReason == "done" || summary.endReason == "time") && summary.sessionId.rfind("routine.", 0) == 0)
		{
			journal_[today_].done[summary.sessionId.substr(8)] = true;
			saveJournal();
			publishRoutine();
		}
		publishStatus();
		if (!isWindowOpen_ && !unloading_)
			cvarManager->executeCommand("togglemenu " + GetMenuName()); // back to the dashboard, which shows the summary
	});

	// The bindings screen runs on the render thread: it only queues work for the game thread.
	BindingsScreen::Callbacks screenCallbacks;
	screenCallbacks.rebind = [this](const std::string& id) { game_->runOnGameThread([this, id] { controls_->startCapture(id); }); };
	screenCallbacks.resetToDefault = [this](const std::string& id) { game_->runOnGameThread([this, id] { controls_->resetToDefault(id); }); };
	screenCallbacks.cancelCapture = [this] { game_->runOnGameThread([this] { controls_->cancelCapture("Cancelled."); }); };
	screenCallbacks.exerciseBindsActive = [this] { return controls_->active(); };
	bindingsScreen_ = std::make_unique<BindingsScreen>(controls_->controls(), std::move(screenCallbacks));

	// Dashboard: render thread; every action goes back to the game thread.
	Dashboard::Callbacks dash;
	dash.play = [this](const std::string& id, const std::string& difficulty) {
		game_->runOnGameThread([this, id, difficulty] {
			player_->clearGhostOverride(); // a race played as usual: your own ghost
			startInFreeplay([this, id, difficulty] { startExercise(id, difficulty, {}); }, id);
		});
	};
	dash.stop = [this] {
		game_->runOnGameThread([this] {
			pendingStart_ = nullptr; // a Free Play load still waiting to start it is called off too
			player_->stop();
			publishStatus();
		});
	};
	dash.reloadContent = [this] { game_->runOnGameThread([this] { reloadContent(); }); };
	dash.accentChanged = [this](const ImVec4& accent) { game_->runOnGameThread([this, accent] { saveAccent(accent); }); };
	dash.startSession = [this](const SessionSettings& settings) {
		game_->runOnGameThread([this, settings] {
			startInFreeplay([this, settings] { startSession(settings); }, settings.sessionId);
		});
	};
	dash.saveRoutine = [this](const routine::Routine& r) {
		game_->runOnGameThread([this, r] {
			routine_ = r;
			saveRoutine();
			reloadContent(); // the drill blocks are sessions
			publishRoutine();
		});
	};
	dash.updateDay = [this](const routine::DayLog& day) {
		game_->runOnGameThread([this, day] {
			journal_[today_] = day;
			saveJournal();
			publishRoutine();
		});
	};
	dash.startRoutineStep = [this](const std::string& stepId) {
		game_->runOnGameThread([this, stepId] { startInFreeplay([this, stepId] { startRoutineStep(stepId); }, "warm-up"); });
	};
	dash.freeplayBoostChanged = [this](bool unlimited) {
		game_->runOnGameThread([this, unlimited] { saveFreeplayPlus(freeplayEnabled_, unlimited); });
	};
	dash.startDaily = [this] { game_->runOnGameThread([this] { startDaily(); }); };
	dash.copyText = [this](const std::string& text) {
		const bool ok = game_->copyToClipboard(text); // Windows' clipboard; fine from the render thread
		eventLog_.write("daily_copied", {{"ok", ok}, {"text", text}});
		return ok;
	};
	dash.practiceDailyShot = [this](const std::string& id, const std::string& difficulty, uint32_t seed) {
		game_->runOnGameThread([this, id, difficulty, seed] {
			// Practice (doesn't count): the shot as it was, from its seed.
			startInFreeplay([this, id, difficulty, seed] { startExercise(id, difficulty, seed, {}); }, id);
		});
	};
	dash.startQuickPlay = [this] {
		game_->runOnGameThread([this] { startInFreeplay([this] { startQuickPlay(); }, "warm-up"); });
	};
	dash.playWith = [this](const std::string& id, const std::string& difficulty, std::optional<bool> mirror,
		const std::string& variant) {
		game_->runOnGameThread([this, id, difficulty, mirror, variant] {
			if (id != kFreeplayId)
				player_->setChoiceFilter(id, variant.empty() ? std::vector<std::string>{} : std::vector<std::string>{variant});
			startInFreeplay([this, id, difficulty, mirror] { startExercise(id, difficulty, {}, mirror); }, id);
		});
	};
	dash.image = [this](int handle) { return game_->image(handle); };
	dash.saveFreeplayPlus = [this](const std::vector<std::string>& enabled) {
		game_->runOnGameThread([this, enabled] { saveFreeplayPlus(enabled, freeplayUnlimited_); });
	};
	dash.dismissSummary = [this] { game_->runOnGameThread([this] { session_->clearSummary(); publishStatus(); }); };
	dash.refreshControls = [this] { game_->runOnGameThread([this] { refreshControls(); }); };
	dash.tutorialSeen = [this](const std::string& id) {
		game_->runOnGameThread([this, id] {
			if (tutorialsSeen_.insert(id).second)
			{
				saveUiSettings();
				eventLog_.write("tutorial_seen", {{"id", id}});
				publishStatus();
			}
		});
	};
	dash.resetTutorials = [this] {
		game_->runOnGameThread([this] {
			tutorialsSeen_.clear();
			saveUiSettings();
			publishStatus();
		});
	};
	dash.dismissFirstRun = [this] {
		game_->runOnGameThread([this] {
			firstRunDone_ = true;
			saveUiSettings();
		});
	};
	dash.renameShot = [this](const std::string& id, const std::string& name) {
		game_->runOnGameThread([this, id, name] {
			if (!name.empty())
				editShot(id, [&](nlohmann::json& doc) { doc["name"] = name; });
		});
	};
	dash.favoriteShot = [this](const std::string& id, bool favorite) {
		game_->runOnGameThread([this, id, favorite] { editShot(id, [&](nlohmann::json& doc) { doc["favorite"] = favorite; }); });
	};
	dash.deleteShot = [this](const std::string& id) { game_->runOnGameThread([this, id] { deleteShot(id); }); };
	dash.shotExtrasChanged = [this](const std::string& id, const shotshare::Extras& e) {
		game_->runOnGameThread([this, id, e] {
			if (editShot(id, [&](nlohmann::json& doc) { shotshare::setExtras(doc, e); }))
				eventLog_.write("shot_extras", {{"id", id}, {"defender", e.defender.empty() ? "none" : e.defender},
					{"target", e.target.empty() ? "whole_net" : e.target}, {"size", e.size}, {"openTiles", e.open}});
		});
	};
	dash.shareShot = [this](const std::string& id) { game_->runOnGameThread([this, id] { shareShot(id); }); };
	dash.importSharedShot = [this](const std::string& path) {
		game_->runOnGameThread([this, path] {
			importShared(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size())));
		});
	};
	dash.scanInbox = [this] { game_->runOnGameThread([this] { scanInbox(); }); };
	dash.createAccount = [this](const std::string& name) {
		game_->runOnGameThread([this, name] { account_->create(name, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.renameAccount = [this](const std::string& name) {
		game_->runOnGameThread([this, name] { account_->rename(name, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.signOutAccount = [this] { game_->runOnGameThread([this] { account_->signOut(); }); };
	dash.retryAccount = [this] { game_->runOnGameThread([this] { account_->retry(static_cast<int64_t>(std::time(nullptr))); }); };
	dash.refreshFriends = [this] {
		game_->runOnGameThread([this] {
			friendsAt_ = static_cast<int64_t>(std::time(nullptr));
			friends_->refresh(friendsAt_);
		});
	};
	dash.addFriend = [this](const std::string& who) {
		game_->runOnGameThread([this, who] { friends_->add(who, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.respondFriend = [this](const std::string& id, bool accept) {
		game_->runOnGameThread([this, id, accept] { friends_->respond(id, accept, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.removeFriend = [this](const std::string& id) {
		game_->runOnGameThread([this, id] { friends_->remove(id, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.shareShotOnline = [this](const std::string& id, bool makePublic, const std::vector<std::string>& friendIds,
		const std::string& toNames) {
		game_->runOnGameThread([this, id, makePublic, friendIds, toNames] { shareShotOnline(id, makePublic, friendIds, toNames); });
	};
	dash.addOnlineShot = [this](const std::string& shotId, const std::string& delivery, bool play, bool applyExtras,
		const shotshare::Extras& extras, const std::string& difficulty) {
		game_->runOnGameThread([this, shotId, delivery, play, applyExtras, extras, difficulty] {
			std::optional<shotshare::Extras> setup;
			if (applyExtras)
				setup = extras;
			addOnlineShot(shotId, delivery, play, setup, difficulty);
		});
	};
	dash.dismissOnlineShot = [this](const std::string& delivery) {
		game_->runOnGameThread([this, delivery] { shots_->dismiss(delivery, static_cast<int64_t>(std::time(nullptr))); });
	};
#if FT_SELF_UPDATE
	dash.checkUpdate = [this] { game_->runOnGameThread([this] { if (updater_) updater_->check(false); }); };
	dash.installUpdate = [this] { game_->runOnGameThread([this] { if (updater_) updater_->install(); }); };
	dash.autoUpdateChanged = [this](bool on) {
		game_->runOnGameThread([this, on] {
			autoUpdate_ = on;
			if (updater_)
				updater_->setAutoInstall(on);
			saveUiSettings();
			eventLog_.write("auto_update", {{"on", on}});
		});
	};
#endif
	dash.refreshRaceLeaders = [this] {
		game_->runOnGameThread([this] { boards_->refreshRaceLeaders(static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.refreshRaceBoard = [this](const std::string& race, const std::string& scope) {
		game_->runOnGameThread([this, race, scope] { boards_->refreshRace(race, scope, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.raceGhost = [this](const std::string& exerciseId, const std::string& race, const std::string& player, const std::string& name) {
		game_->runOnGameThread([this, exerciseId, race, player, name] {
			// Someone's ghost from the board: fetched, then the race starts with it (orange, their name over it).
			boards_->fetchGhost(race, player, static_cast<int64_t>(std::time(nullptr)),
				[this, exerciseId, race, name](const nlohmann::json& g, const std::string& error) {
					const ghost::Run run = g.is_object() ? ghost::fromJson(g) : ghost::Run{};
					if (run.frames.size() < 2)
					{
						notice(error.empty() ? "That ghost couldn't be read" : error);
						return;
					}
					eventLog_.write("race_ghost", {{"exercise", exerciseId}, {"race", race}, {"name", name}, {"seconds", run.seconds()}});
					startInFreeplay([this, exerciseId, race, run, name] {
						player_->setGhostOverride(race, run, name);
						startExercise(exerciseId, "medium", {}, {});
					}, exerciseId);
				});
		});
	};
	dash.refreshDailyBoard = [this](const std::string& day, const std::string& scope) {
		game_->runOnGameThread([this, day, scope] { boards_->refreshDaily(day, scope, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.refreshShotBoard = [this](const std::string& sort) {
		game_->runOnGameThread([this, sort] { shots_->refreshBoard(sort, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.likeShot = [this](const std::string& shotId, bool on) {
		game_->runOnGameThread([this, shotId, on] { shots_->like(shotId, on, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.reportShot = [this](const std::string& shotId) {
		game_->runOnGameThread([this, shotId] { shots_->report(shotId, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.setShotPublic = [this](const std::string& shotId, bool on) {
		game_->runOnGameThread([this, shotId, on] { shots_->setPublic(shotId, on, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.deleteOnlineShot = [this](const std::string& shotId) {
		game_->runOnGameThread([this, shotId] { shots_->remove(shotId, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.shareActivityChanged = [this](bool on) {
		game_->runOnGameThread([this, on] {
			account_->setShareActivity(on);
			const int64_t now = static_cast<int64_t>(std::time(nullptr));
			if (on)
				activityAt_ = 0; // sent on the next check
			else
				friends_->stopSharing(now);
		});
	};
	dash.makeRecoveryCode = [this] {
		game_->runOnGameThread([this] { account_->makeRecoveryCode(static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.restoreAccount = [this](const std::string& code) {
		game_->runOnGameThread([this, code] { account_->restore(code, static_cast<int64_t>(std::time(nullptr))); });
	};
	dash.browseSharedShot = [this] {
		// Windows' Open dialog (its own thread), starting in Downloads; the chosen file is added on the game thread.
		game_->pickFile(game_->downloadsFolder(), [this](const std::filesystem::path& file) {
			if (!file.empty())
				game_->runOnGameThread([this, file] { importShared(file); });
		});
	};
	dash.startRecording = [this] { game_->runOnGameThread([this] { startRecording(); }); };
	dash.stopRecording = [this] { game_->runOnGameThread([this] { stopRecording(); }); };
	dash.previewRecording = [this](float at) { game_->runOnGameThread([this, at] { takeStart_ = at; }); };
	dash.saveRecording = [this](float at, bool leadIn, const std::string& name) {
		game_->runOnGameThread([this, at, leadIn, name] { saveRecording(at, leadIn, name); });
	};
	dash.discardRecording = [this] {
		game_->runOnGameThread([this] {
			take_ = leadin::Track{};
			takeView_.reset();
			eventLog_.write("record_discarded", nlohmann::json::object());
			publishStatus();
		});
	};
	dash.addShotToQuickPlay = [this](const std::string& id) { game_->runOnGameThread([this, id] { addToWarmup(id); }); };
	dash.practiceChanged = [this](const PracticeSpeedSettings& p) {
		game_->runOnGameThread([this, p] {
			practice_.setSettings(p);
			saveUiSettings();
			publishStatus(); // the switch on Settings shows the new value
		});
	};
	dash.goalReplaysChanged = [this](bool on) {
		game_->runOnGameThread([this, on] {
			goalReplays_ = on;
			player_->setGoalReplays(on);
			saveUiSettings();
			eventLog_.write("goal_replays", {{"on", on}});
			publishStatus();
		});
	};
	dash.selectCommandChanged = [this](const std::string& command) {
		game_->runOnGameThread([this, command] {
			cvarManager->getCvar("ft_select_command").setValue(command); // what the controller's Select runs
			eventLog_.write("select_command", {{"command", command}});
			publishStatus();
		});
	};
	dash.goalExplosionChanged = [this](bool on) {
		game_->runOnGameThread([this, on] {
			goalExplosion_ = on;
			player_->setGoalExplosion(on);
			saveUiSettings();
			publishStatus();
		});
	};
	dash.sound = [this](const char* name) { playMenuSound(name); };
	dash.volumesChanged = [this](float menu, float training, bool preview) {
		menuVolume_ = std::clamp(menu, 0.f, 1.f);
		trainingVolume_ = std::clamp(training, 0.f, 1.f);
		if (preview)
			playTrainingSound("success");
		game_->runOnGameThread([this] { saveUiSettings(); publishStatus(); });
	};
	dash.addToQuickPlay = [this](const std::string& id) { game_->runOnGameThread([this, id] { addToWarmup(id); }); };
	dash.freeplayMapChanged = [this](const std::string& map) {
		game_->runOnGameThread([this, map] {
			cvarManager->getCvar("ft_freeplay_map").setValue(map);
			publishStatus();
		});
	};
	dash.progressOpened = [this] { game_->runOnGameThread([this] { readMmrNow("progress page"); }); };
	dash.addMmr = [this](const std::string& playlist, float mmr) {
		game_->runOnGameThread([this, playlist, mmr] {
			progress_.recordMmr(playlist, localTime("%Y-%m-%d"), mmr, 0, 0, 0, true);
			eventLog_.write("mmr_typed", {{"playlist", playlist}, {"mmr", mmr}});
			saveProgress();
			refreshProgress(true);
			publishStatus();
		});
	};
	dash.takeover = [this](uint64_t playerId, int others, bool leadIn) {
		game_->runOnGameThread([this, playerId, others, leadIn] { takeoverNow(playerId, others, leadIn); });
	};
	dash.deleteReplayMoment = [this](const std::string& id) { game_->runOnGameThread([this, id] { deleteReplayMoment(id); }); };
	dash.dailyGoalChanged = [this](double minutes) {
		game_->runOnGameThread([this, minutes] {
			dailyGoalMinutes_ = std::clamp(minutes, 5.0, 240.0);
			saveUiSettings();
			refreshHome(true);
			publishStatus();
		});
	};
	dash.importPack = [this](const std::string& code) { game_->runOnGameThread([this, code] { startPackImport(code); }); };
	dash.deletePack = [this](const std::string& id) { game_->runOnGameThread([this, id] { deletePack(id); }); };
	dash.packTargetChanged = [this](const std::string& target, const std::string& size, int tile) {
		game_->runOnGameThread([this, target, size, tile] {
			packTarget_ = target == "tile" || target == "moving" || target == "pick" ? target : "";
			packTileSize_ = size == "easy" || size == "hard" || size == "impossible" ? size : "medium";
			packTile_ = std::clamp(tile, 0, 7);
			player_->setPackTarget(packTarget_, packTileSize_, packTile_);
			saveUiSettings();
			eventLog_.write("pack_target", {{"target", packTarget_.empty() ? "whole_net" : packTarget_}, {"size", packTileSize_},
				{"tile", packTile_}});
			publishStatus();
		});
	};
	dash.favoriteToggled = [this](const std::string& id) {
		game_->runOnGameThread([this, id] {
			const bool on = favorites_.insert(id).second;
			if (!on)
				favorites_.erase(id);
			favoritesPub_ = std::make_shared<std::set<std::string>>(favorites_);
			saveUiSettings();
			eventLog_.write("favorite", {{"exercise", id}, {"on", on}});
			publishStatus();
		});
	};
	dash.packDefenderChanged = [this](const std::string& who) {
		game_->runOnGameThread([this, who] {
			packDefender_ = who == "psyonix" || who == "necto" || who == "nexto" ? who : "";
			player_->setPackDefender(packDefender_);
			saveUiSettings();
			eventLog_.write("pack_defender", {{"who", packDefender_.empty() ? "none" : packDefender_}});
			publishStatus();
		});
	};
	dash.defenderChanged = [this](const std::string& who) {
		game_->runOnGameThread([this, who] {
			defender_ = who == "necto" || who == "nexto" ? who : "psyonix";
			bots_->setDefender(defender_);
			saveUiSettings();
			publishStatus();
		});
	};
	dash.mechanicsFeedChanged = [this](bool on) {
		game_->runOnGameThread([this, on] {
			mechanicsFeed_ = on;
			hud_.setFeedEnabled(feedVisible());
			saveUiSettings();
			publishStatus();
		});
	};
	dash.devToolsChanged = [this](bool on) {
		game_->runOnGameThread([this, on] {
			devTools_ = on;
			hud_.setFeedEnabled(feedVisible());
			saveUiSettings();
			publishStatus();
		});
	};
	dashboard_ = std::make_unique<Dashboard>(std::move(dash), bindingsScreen_.get(), plugin_version);
	bests_.load(game_->dataFolder() / "freetouch" / "settings" / "bests.json");
	player_->setBestTimes(&bests_);
	startAccount(); // signs in on the first tick (or when the menu opens): onLoad is too early (build 228 crashed here)
#if FT_SELF_UPDATE
	startUpdater();
#endif
	// Ghosts of the best race runs: settings/ghosts/<exercise_difficulty>.json.
	auto ghostFile = [this](const std::string& key) {
		return game_->dataFolder() / "freetouch" / "settings" / "ghosts" / (capture::sanitizeId(key) + ".json");
	};
	player_->setGhostStore(
		[ghostFile](const std::string& key) {
			std::ifstream in(ghostFile(key));
			if (!in)
				return ghost::Run{};
			try
			{
				return ghost::fromJson(nlohmann::json::parse(in));
			}
			catch (const std::exception&)
			{
				return ghost::Run{};
			}
		},
		[this, ghostFile](const std::string& key, const ghost::Run& run) {
			std::error_code ec;
			std::filesystem::create_directories(ghostFile(key).parent_path(), ec);
			safefile::write(ghostFile(key), ghost::toJson(run).dump());
			// A new best: onto the race's board with its ghost (Phase 8), if signed in.
			if (boards_)
				boards_->postRace(capture::sanitizeId(key), run.seconds(), ghost::toJson(run), static_cast<int64_t>(std::time(nullptr)));
		});
	loadUiSettings();
	uiLoaded_ = true; // the tips already shown are known: new ones may open by themselves
	loadHistory();
	loadProgress();
	loadFreeplayPlus();
	// MMR: read once the game is up, then whenever the game updates it (after a ranked match).
	game_->addMmrListener([this] { game_->runOnGameThread([this] { readMmrNow("mmr update"); }); });
	game_->runAfter(3.f, [this] { readMmrNow("loaded"); });
	game_->runAfter(33.f, [this] { pollMmr(); });
	game_->runAfter(5.f, [this] { pollOnline(); });
	// Replays: their frames (velocity smoothing for takeover) and R3 while one is open.
	game_->addReplayFrameListener([this](const Snapshot& s, const ReplayInfo& info) { onReplayFrame(s, info); });
	// Training packs: their shots, recorded as the player plays them in custom training.
	cvarManager->registerNotifier("ft_import_pack", [this](std::vector<std::string> args) {
		const std::string code = args.size() > 1 ? args[1] : std::string();
		game_->runOnGameThread([this, code] { startPackImport(code); });
	}, "Import a training pack's shots: ft_import_pack <code> (or none: the pack you're in)", PERMISSION_ALL);
	cvarManager->registerCvar("ft_record_packs", "1", "Record the shots of custom training packs you play as Free Touch drills (Training packs)",
		true, true, 0, true, 1, true);
	game_->addTrainingTickListener([this](const Snapshot& s, const TrainingInfo& t) { onTrainingTick(s, t); });
	settings_->setBind(kMenuKey, "ft_menu");
	game_->addTickListener([this](const Snapshot&) {
		updatePracticeSpeed();
		updateMenuNavBinds();
		reloadPacksIfDirty();
		publishStatus();
	});
	cvarManager->registerNotifier("ft_nav", [this](std::vector<std::string> args) {
		if (args.size() > 1 && dashboard_)
			dashboard_->queueNav(args[1]);
	}, "Controller navigation of the Free Touch window: ft_nav up|down|left|right|select|back|close", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_slowmo", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { practice_.toggle(); });
	}, "Toggle practice slow motion during an exercise", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_clutch", [this](std::vector<std::string>) {
		// Held, not pressed: the clutch is read from the key state every tick.
	}, "Clutch for practice slow motion (hold the bound button)", PERMISSION_ALL);
	game_->addDrawListener([this](Canvas& canvas) {
		const Snapshot s = game_->snapshot();
		if (!s.inReplay && replayBindOn_)
			leaveReplay(); // R3 back to the player's own bind
		drawReplayHint(canvas, s);
		if (s.paused)
		{
			hud_.drawFeed(canvas); // hints only (a training shot waiting frozen counts as paused)
			return;                // keep the pause menu clean
		}
		// The canvas has no depth: the ball and cars hide the world objects behind them.
		std::vector<Occluder> occluders;
		if (s.ball)
			occluders.push_back({s.ball->location, 92.75f});
		for (const auto& car : s.cars)
			occluders.push_back({car.location + Vec3(0.f, 0.f, 20.f), 65.f});
		canvas.setOccluders(occluders);
		player_->drawObjects(canvas, s); // world objects first, so the HUD stays on top
		drawRecording(canvas);
		hud_.draw(canvas, player_->status(), daily_->active() ? daily_->hudStatus() : session_->status());
		hud_.drawFeed(canvas);
	});

	cvarManager->registerNotifier("ft_menu", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { reloadPacksIfDirty(); });
		cvarManager->executeCommand("togglemenu " + GetMenuName());
	}, "Open or close the Free Touch window", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_back", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] {
			// A double tap: the first press only arms it (a stray press mid-play does nothing), the second within
			// half a second stops the exercise and opens the window.
			const auto now = std::chrono::steady_clock::now();
			if (now - lastBackPress_ > std::chrono::milliseconds(500))
			{
				lastBackPress_ = now;
				hud_.showHint("Press again for the Free Touch menu", 0.5f);
				return;
			}
			lastBackPress_ = {};
			hud_.showHint("", 0.f);
			eventLog_.write("back_double_tap");
			player_->stop();
			if (!isWindowOpen_)
				cvarManager->executeCommand("togglemenu " + GetMenuName());
		});
	}, "Stop the exercise and open the Free Touch window", PERMISSION_ALL);

	cvarManager->registerNotifier("ft_log", [this](std::vector<std::string> args) {
		const std::string text = joinArgs(args, 1);
		eventLog_.write("note", {{"text", text}});
		LOG("ft_log: wrote note \"{}\"", text);
	}, "Write a note into the Free Touch event log: ft_log <text>", PERMISSION_ALL);

	cvarManager->registerNotifier("ft_log_path", [this](std::vector<std::string>) {
		LOG("Event log: {}", eventLog_.isOpen() ? eventLog_.path().string() : std::string("(not open)"));
	}, "Print the path of the current Free Touch event log", PERMISSION_ALL);

	cvarManager->registerCvar("ft_content_dir", "",
		"Folder with Free Touch content JSON. Empty = <BakkesMod data>/freetouch/content (the build copies the repo's "
		"content/ there). Point it at the repo's content folder to edit JSON live.", true);

	cvarManager->registerNotifier("ft_reload_content", [this](std::vector<std::string>) {
		reloadContent();
	}, "Reload all Free Touch content JSON (scenarios, sessions, combos, HORSE) without rebuilding", PERMISSION_ALL);

	cvarManager->registerNotifier("ft_content_list", [this](std::vector<std::string>) {
		for (auto kind : {ContentLibrary::Kind::Scenario, ContentLibrary::Kind::Session, ContentLibrary::Kind::Combo,
				 ContentLibrary::Kind::Horse})
		{
			const auto items = content_.all(kind);
			LOG("{} ({}):", ContentLibrary::kindName(kind), items.size());
			for (const auto* item : items)
				LOG("  {}  \"{}\"{}", item->id, item->name, item->category.empty() ? "" : "  [" + item->category + "]");
		}
	}, "List all loaded Free Touch content ids", PERMISSION_ALL);

	reloadContent();

	cvarManager->registerNotifier("ft_play", [this](std::vector<std::string> args) {
		if (args.size() < 2)
		{
			LOG("usage: ft_play <scenario id> [easy|medium|hard] [seed]");
			return;
		}
		const std::string id = args[1];
		std::string difficulty = args.size() > 2 ? args[2] : "medium";
		if (difficulty != "easy" && difficulty != "medium" && difficulty != "hard")
		{
			LOG("ft_play: difficulty must be easy, medium or hard");
			return;
		}
		std::optional<uint32_t> seed;
		if (args.size() > 3)
		{
			try { seed = static_cast<uint32_t>(std::stoul(args[3])); }
			catch (...) { LOG("ft_play: bad seed '{}'", args[3]); return; }
		}
		game_->runOnGameThread([this, id, difficulty, seed] {
			startInFreeplay([this, id, difficulty, seed] { startExercise(id, difficulty, seed); }, id);
		});
	}, "Start a scenario: ft_play <scenario id> [easy|medium|hard] [seed]", PERMISSION_ALL);

	cvarManager->registerNotifier("ft_retry", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { player_->retry(); });
	}, "Retry the current rep (same seed)", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_next", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] {
			if (session_->active())
				session_->next();
			else
				player_->next();
		});
	}, "Next rep of the current scenario (new seed); in a session, the next rep of the session", PERMISSION_ALL);

	cvarManager->registerNotifier("ft_session", [this](std::vector<std::string> args) {
		if (args.size() < 2)
		{
			LOG("usage: ft_session <session id> [minutes] [easy|medium|hard] [left|right|free]");
			return;
		}
		SessionSettings settings;
		settings.sessionId = args[1];
		if (const auto* item = content_.find(ContentLibrary::Kind::Session, settings.sessionId))
		{
			settings.minutes = item->data.value("durationMinutes", 10.0);
			settings.difficulty = item->data.value("difficulty", "medium");
			settings.airRoll = item->data.value("airRoll", "free");
		}
		try
		{
			if (args.size() > 2) settings.minutes = std::stod(args[2]);
		}
		catch (...) {}
		if (args.size() > 3) settings.difficulty = args[3];
		if (args.size() > 4) settings.airRoll = args[4];
		game_->runOnGameThread([this, settings] {
			startInFreeplay([this, settings] { startSession(settings); }, settings.sessionId);
		});
	}, "Start a session: ft_session <session id> [minutes] [easy|medium|hard] [left|right|free]", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_warmup", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { startInFreeplay([this] { startQuickPlay(); }, "warm-up"); });
	}, "Start your warm-up: the next drill block in your Routine's warm-up section (as Home's Warm-up button)", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_prev", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { player_->previous(); });
	}, "Go back to the previous rep (its seed)", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_stop", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { player_->stop(); LOG("Free Touch: stopped"); });
	}, "Stop the current exercise", PERMISSION_ALL);

	cvarManager->registerNotifier("ft_state", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { logState(); });
	}, "Print the current game state (tick, ball, cars) and write it to the event log", PERMISSION_ALL);

	cvarManager->registerNotifier("ft_takeover", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { takeoverNow(0, 2, true); });
	}, "Replay takeover: play the paused replay moment in Free Play as the camera's player (R3 in a replay)", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_mark", [this](std::vector<std::string> args) {
		// Tuning data: the last 3 s of the car and the ball, written to the event log ("user_mark") right after a
		// mechanic the detectors missed or named wrong. An optional word says what it was (ft_mark musty).
		const std::string what = args.size() > 1 ? args[1] : "";
		game_->runOnGameThread([this, what] {
			eventLog_.write("user_mark", {{"what", what}, {"tick", tick_},
				{"columns", "tick, car x y z, vx vy vz, pitch yaw roll, onGround onWall hasFlip jumped doubleJumped, ball x y z, vx vy vz"},
				{"rows", detector_.trace(3.0)}});
			hud_.pushMechanic("MARKED FOR TUNING", 0.f, 0, true);
			playTrainingSound("go", 1.f, 0.6f);
		});
	}, "Log the last 3 s of car and ball for mechanic-detector tuning (bound to Select while tuning)", PERMISSION_ALL);
	cvarManager->registerNotifier("ft_dump", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { writeDump(); });
	}, "Write the ball and every car (location, velocity, rotation, spin, boost, flip, ground/wall) to freetouch/dumps/",
		PERMISSION_ALL);

	cvarManager->registerNotifier("ft_capture", [this](std::vector<std::string> args) {
		const std::string name = args.size() > 1 ? capture::sanitizeId(joinArgs(args, 1)) : std::string();
		game_->runOnGameThread([this, name] { captureMoment(name); });
	}, "Save this moment as a scenario in freetouch/content/captured/: ft_capture [name] (default: the time)",
		PERMISSION_ALL);
	cvarManager->registerNotifier("ft_record", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] { recording_ ? stopRecording() : startRecording(); });
	}, "Record page: start recording a feed in freeplay, or stop (then pick the start on the Record page)", PERMISSION_ALL);

	// Adapter checks for the state-set path (the Phase 0 spikes, now through GameAdapter).
	cvarManager->registerNotifier("ft_test_ball", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] {
			const bool ok = game_->setBall({{0.f, 0.f, 500.f}, {}, {}});
			LOG("ft_test_ball: {}", ok ? "ball set to (0, 0, 500)" : "not in freeplay");
			eventLog_.write("test_set_ball", {{"ok", ok}});
		});
	}, "Put the ball at (0, 0, 500) with zero velocity", PERMISSION_ALL);

	cvarManager->registerNotifier("ft_test_car", [this](std::vector<std::string>) {
		game_->runOnGameThread([this] {
			CarTarget t;
			t.location = {0.f, -2000.f, 17.f};
			t.rotation = {0, 16384, 0};
			t.boost = 50.f;
			const bool ok = game_->setLocalCar(t);
			LOG("ft_test_car: {}", ok ? "car set to (0, -2000, 17) facing +y, 50 boost" : "not in freeplay");
			eventLog_.write("test_set_car", {{"ok", ok}});
		});
	}, "Put your car at (0, -2000, 17) facing +y with 50 boost", PERMISSION_ALL);

	// Last: it reads ft_content_dir, which has to be registered first (1.0.0.204 crashed reading it before then).
	loadFonts();
	LOG("Free Touch loaded (v{}). {} opens Free Touch, {} reloads the plugin.", plugin_version, kMenuKey, kReloadKey);
}

void FreeTouch::loadFonts()
{
	// The dashboard's fonts (content/fonts/, Barlow): BakkesMod loads fonts from its data/fonts/ folder only, so they
	// are copied to data/fonts/freetouch/ first (again when the file changed), then asked for by name.
	const auto from = contentRoot() / "fonts";
	const auto to = game_->dataFolder() / "fonts" / "freetouch";
	std::error_code ec;
	std::filesystem::create_directories(to, ec);
	int copied = 0, asked = 0;
	for (const auto& spec : theme::fontSpecs())
	{
		const auto src = from / spec.file;
		const auto dst = to / spec.file;
		if (!std::filesystem::exists(src))
			continue;
		if (!std::filesystem::exists(dst) || std::filesystem::file_size(dst, ec) != std::filesystem::file_size(src, ec))
		{
			std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
			copied += ec ? 0 : 1;
		}
		game_->loadFont(spec.name, std::string("freetouch/") + spec.file, static_cast<int>(spec.size));
		++asked;
	}
	eventLog_.write("fonts", {{"asked", asked}, {"copied", copied}, {"to", to.string()}});
}

void FreeTouch::Render()
{
	// Render thread: the dashboard reads only its published copies (hard rule 2).
	// Its fonts (Barlow), once BakkesMod has built them (they were asked for at load).
	if (!theme::hasFonts() && game_)
		for (const auto& spec : theme::fontSpecs())
			theme::setFont(spec.name, static_cast<ImFont*>(game_->font(spec.name)));
	if (dashboard_)
		dashboard_->render(&isWindowOpen_);
	if (!isWindowOpen_)
		cvarManager->executeCommand("togglemenu " + GetMenuName());
}

void FreeTouch::startInFreeplay(std::function<void()> start, const std::string& what, const std::string& mapOverride)
{
	if (game_->inFreeplay())
	{
		pendingStart_ = nullptr;
		start();
		return;
	}
	const std::string map = !mapOverride.empty() ? mapOverride : cvarManager->getCvar("ft_freeplay_map").getStringValue();
	if (!game_->loadFreeplay(map))
	{
		LOG("Free Touch: {} needs Free Play (not started from an online match)", what);
		eventLog_.write("freeplay_load_refused", {{"what", what}, {"map", map}});
		return;
	}
	pendingStart_ = std::move(start);
	pendingWhat_ = what;
	pendingSince_ = std::chrono::steady_clock::now();
	pendingReadyTicks_ = 0;
	LOG("Free Touch: loading Free Play ({}) for {}", map, what);
	eventLog_.write("freeplay_load", {{"what", what}, {"map", map}});
	publishStatus();
}

void FreeTouch::tickPendingStart(const Snapshot& s)
{
	if (!pendingStart_)
		return;
	if (std::chrono::steady_clock::now() - pendingSince_ > std::chrono::seconds(90))
	{
		LOG("Free Touch: Free Play did not load in time; {} not started", pendingWhat_);
		eventLog_.write("freeplay_load_timeout", {{"what", pendingWhat_}});
		pendingStart_ = nullptr;
		return;
	}
	// Wait until freeplay's own kickoff reset is over and the car is ready (about a second and a quarter).
	pendingReadyTicks_ = s.inFreeplay && game_->localCarReady() ? pendingReadyTicks_ + 1 : 0;
	if (pendingReadyTicks_ < 150)
		return;
	auto start = std::move(pendingStart_);
	pendingStart_ = nullptr;
	eventLog_.write("freeplay_loaded_start", {{"what", pendingWhat_}});
	start();
}

void FreeTouch::startExercise(const std::string& id, const std::string& difficulty, std::optional<uint32_t> seed,
	std::optional<bool> mirror)
{
	player_->setMirrorPolicy(mirror); // a side picked on the exercise's page, else the scenario's coin flip
	if (player_->play(id, difficulty, seed))
	{
		lastExerciseId_ = id;
		// A shot from the Free Touch service: a play counted there (once a day per player).
		const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
		if (item && shots_ && item->data.contains("shared") && item->data["shared"].is_object())
			if (const std::string online = item->data["shared"].value("id", ""); !online.empty())
				shots_->played(online, static_cast<int64_t>(std::time(nullptr)));
	}
	publishStatus();
}

void FreeTouch::startSession(const SessionSettings& settings)
{
	std::string error;
	if (!session_->start(settings, error))
	{
		LOG("Free Touch: session not started: {}", error);
		eventLog_.write("session_refused", {{"session", settings.sessionId}, {"reason", error}});
	}
	publishStatus();
}

void FreeTouch::publishStatus()
{
	if (!dashboard_)
		return;
	const auto s = player_->status();
	DashboardStatus d;
	d.exerciseActive = s.active;
	d.exerciseId = s.scenarioId;
	d.exerciseName = s.scenarioName;
	d.difficulty = s.difficulty;
	d.rep = s.rep;
	d.seed = s.seed;
	d.repSeconds = s.repSeconds;
	d.lastGoalSide = s.goalSide;
	d.inFreeplay = game_->inFreeplay();
	d.lastExerciseId = lastExerciseId_;
	d.goalExplosion = goalExplosion_;
	d.goalReplays = goalReplays_;
	if (CVarWrapper select = cvarManager->getCvar("ft_select_command"); !select.IsNull()) // registered a little later in onLoad
		d.selectCommand = select.getStringValue();
	d.practice = practice_.settings();
	d.firstRun = !firstRunDone_;
	if (uiLoaded_)
		d.tutorialsSeen = std::make_shared<const std::set<std::string>>(tutorialsSeen_);
	d.devTools = devTools_;
	d.mechanicsFeed = mechanicsFeed_;
	d.recording = recording_;
	d.recordSeconds = recording_ ? static_cast<float>((tick_ - recordStartTick_) / 120.0) : 0.f;
	d.takeSeconds = recording_ ? 0.f : static_cast<float>(take_.seconds());
	d.take = recording_ ? nullptr : takeView_;
	d.inbox = inbox_;
	d.notice = notice_;
	if (account_)
	{
		d.account = account_->view();
		d.shareActivity = account_->shareActivity();
	}
	if (friends_)
		d.friends = friends_->view();
	if (shots_)
		d.shots = shots_->view();
#if FT_SELF_UPDATE
	if (updater_)
		d.update = updater_->view();
#else
	d.update.selfUpdate = false;
	d.update.current = plugin_version;
#endif
	if (boards_)
		d.boards = boards_->view();
	d.haveKeys = haveKeys_;
	d.noticeSeq = noticeSeq_;
	d.defender = defender_;
	d.packDefender = packDefender_;
	d.packTarget = packTarget_;
	d.packImporting = packImport_.on;
	d.packImportMessage = packImport_.message;
	const bool inPack = trainingNow_.active && game_->inCustomTraining();
	d.trainingPackName = inPack ? trainingNow_.name : std::string();
	d.trainingPackCode = inPack ? trainingNow_.code : std::string();
	d.packTileSize = packTileSize_;
	d.packTile = packTile_;
	d.favorites = favoritesPub_;
	d.nectoAvailable = brainAvailable("necto");
	d.nextoAvailable = brainAvailable("nexto");
	d.startPending = static_cast<bool>(pendingStart_);
	d.freeplayMap = cvarManager->getCvar("ft_freeplay_map").getStringValue();
	d.menuVolume = menuVolume_;
	d.trainingVolume = trainingVolume_;
	refreshHome(false);
	d.home = home_;
	d.routine = routinePub_;
	d.today = todayPub_;
	d.todayDate = today_;
	d.stats = exerciseStats_;
	refreshProgress(false);
	d.progress = progressPub_;
	d.replay = replayPub_;
	d.daily = dailyPub_;
	trainingCues(s);
	const auto ss = session_->status();
	d.sessionActive = ss.active;
	d.sessionName = ss.name;
	d.sessionSecondsLeft = ss.secondsLeft;
	d.sessionExerciseIndex = ss.exerciseIndex;
	d.sessionExerciseCount = ss.exerciseCount;
	d.lastSummary = session_->lastSummary();
	dashboard_->setStatus(d);
}

void FreeTouch::publishDashboardModel(int errors, int warnings)
{
	if (!dashboard_)
		return;
	auto model = std::make_shared<DashboardModel>();
	// The tutorials (content/tutorials/tutorials.json).
	if (std::ifstream tin(contentRoot() / "tutorials" / "tutorials.json"); tin)
	{
		try
		{
			for (const auto& t : nlohmann::json::parse(tin).value("tutorials", nlohmann::json::array()))
			{
				Tutorial tut;
				tut.id = t.value("id", "");
				tut.title = t.value("title", "");
				for (const auto& st : t.value("steps", nlohmann::json::array()))
					tut.steps.push_back({st.value("scene", ""), st.value("title", ""), st.value("text", "")});
				if (!tut.id.empty() && !tut.steps.empty())
					model->tutorials.push_back(std::move(tut));
			}
		}
		catch (const std::exception& e)
		{
			LOG("Tutorials: content/tutorials/tutorials.json could not be read ({})", e.what());
		}
	}
	model->contentErrors = errors;
	model->contentWarnings = warnings;
	model->contentRoot = contentRoot().string();

	// Card art: content/images/{exercises,sessions}/<id>.png|.jpg, else images/categories/<category>.png|.jpg;
	// without one the dashboard draws its mini-pitch preview.
	const auto imageDir = contentRoot() / "images";
	auto findImage = [&](const std::string& folder, const std::string& id, const std::string& category) {
		for (const auto& [dir, name] : {std::pair{folder, id}, std::pair{std::string("categories"), category}})
		{
			if (name.empty())
				continue;
			for (const char* ext : {".png", ".jpg"})
			{
				const int handle = game_->loadImage(imageDir / dir / (name + ext));
				if (handle >= 0)
					return handle;
			}
		}
		return -1;
	};

	// Freeplay+ (category speed_lab) first: it is the everyday mode.
	static const char* kCategoryOrder[] = {"speed_lab", "vs_bot", "basics", "saves", "first_touch", "mechanics", "movement",
		"pinch_bowling", "glade", "backboard_breakout", "defense", "fifties", "horse", "combo", "races", "speed_flip",
		"pro_saves", "replay", "packs", "captured", "dev"};
	for (const auto* item : content_.all(ContentLibrary::Kind::Scenario))
	{
		ExerciseCard card;
		card.id = item->id;
		card.name = item->name;
		card.category = item->category;
		card.description = item->data.value("description", "");
		if (item->data.contains("tags"))
			card.tags = item->data["tags"].get<std::vector<std::string>>();
		card.allowMirror = item->data.value("allowMirror", false);
		card.hasDifficulty = scenario::hasDifficulty(item->data);
		card.pending = scenario::pendingFeature(item->data);
		card.image = findImage("exercises", item->id, item->category);
		{
			std::error_code ec;
			const auto written = std::filesystem::last_write_time(item->file, ec);
			if (!ec)
				card.updated = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch()).count();
		}
		for (const auto& bot : item->data.value("bots", nlohmann::json::array()))
		{
			const std::string type = bot.value("type", "");
			if (type == "psyonix" || type == "necto" || type == "nexto")
				card.opponent = type;
		}
		if (item->data.contains("situation"))
		{
			const auto& sit = item->data["situation"];
			card.variantLabel = sit.value("variantLabel", "");
			for (const auto& v : sit.value("variants", nlohmann::json::array()))
				if (v.contains("id") && v["id"].is_string())
					card.variants.emplace_back(v["id"].get<std::string>(), v.value("short", v.value("name", v["id"].get<std::string>())));
		}

		// Preview: rep with seed 1 at medium, not mirrored by choice of seed.
		const ResolvedScenario r = scenario::resolve(item->data, 1, "medium");
		card.timeLimitSeconds = r.timeLimitSeconds;
		card.preview.ballVisible = !r.ballHidden;
		card.preview.ball = r.ball.location;
		card.preview.ballVelocity = r.ball.velocity;
		card.preview.car = r.player.location;
		card.preview.carYaw = r.player.rotation.yaw;
		auto vec = [](const nlohmann::json& v) {
			return v.is_array() && v.size() == 3 ? Vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>()) : Vec3();
		};
		if (r.resolved.contains("bots"))
		{
			for (const auto& bot : r.resolved["bots"])
				if (bot.contains("location")) card.preview.bots.push_back(vec(bot["location"]));
		}
		if (r.resolved.contains("objects"))
		{
			for (const auto& obj : r.resolved["objects"])
			{
				if (!obj.contains("location"))
					continue;
				card.preview.objects.push_back(vec(obj["location"]));
				card.preview.objectRadii.push_back(obj.value("radius", 150.f));
			}
		}
		model->exercises.push_back(std::move(card));
	}
	for (const char* cat : kCategoryOrder)
	{
		if (std::any_of(model->exercises.begin(), model->exercises.end(), [&](const ExerciseCard& c) { return c.category == cat; }))
			model->categories.push_back(cat);
	}
	// A category the order above does not know yet (new content) still gets its place in the menus, before the dev ones.
	for (const auto& card : model->exercises)
	{
		if (std::find(model->categories.begin(), model->categories.end(), card.category) == model->categories.end())
		{
			const auto at = std::find_if(model->categories.begin(), model->categories.end(),
				[](const std::string& c) { return c == "captured" || c == "dev"; });
			model->categories.insert(at, card.category);
		}
	}
	// Quick Play: the saved warm-up, else the default; exercises that no longer exist are dropped.
	const auto* warmUp = content_.find(ContentLibrary::Kind::Session, quickplay::kSessionId);
	model->quickPlaySaved = warmUp != nullptr;
	if (!warmUp)
		warmUp = content_.find(ContentLibrary::Kind::Session, quickplay::kDefaultSessionId);
	if (warmUp)
		model->quickPlay = quickplay::fromSession(warmUp->data);
	auto& qpEx = model->quickPlay.exercises;
	qpEx.erase(std::remove_if(qpEx.begin(), qpEx.end(), [&](const std::string& id) {
		return !content_.find(ContentLibrary::Kind::Scenario, id);
	}), qpEx.end());

	for (const auto* item : content_.all(ContentLibrary::Kind::Session))
	{
		if (item->category == "quick_play")
			continue; // shown on the Quick Play page instead
		SessionCard s;
		s.id = item->id;
		s.name = item->name;
		s.category = item->category;
		s.description = item->data.value("description", "");
		s.durationMinutes = item->data.value("durationMinutes", 10.0);
		s.difficulty = item->data.value("difficulty", "medium");
		s.exerciseRefs = static_cast<int>(item->data["exercises"].size());
		s.image = findImage("sessions", item->id, item->data.value("category", ""));
		model->sessions.push_back(std::move(s));
	}
	// Races: each one's ghost key (as settings/ghosts/ names it), for its boards on the service.
	for (const auto* item : content_.all(ContentLibrary::Kind::Scenario))
		if (item->category == "races")
			model->raceKeys[item->id] = capture::sanitizeId(BestTimes::key(BestTimes::recordId(item->id, item->data), "medium"));
	// Saved Shots: captured moments, favourites first, then newest (ids end in the capture time).
	for (const auto* item : content_.all(ContentLibrary::Kind::Scenario))
	{
		if (item->category == "captured")
		{
			const shotshare::Extras e = shotshare::extrasOf(item->data);
			const std::string by = item->data.contains("shared") ? item->data["shared"].value("by", "") : "";
			model->savedShots.push_back({item->id, item->name, item->data.value("description", ""), item->data.value("favorite", false),
				e, by});
		}
	}
	std::sort(model->savedShots.begin(), model->savedShots.end(), [](const SavedShot& a, const SavedShot& b) {
		return a.favorite != b.favorite ? a.favorite : a.id > b.id;
	});
	// Replay moments (replay takeover), newest first.
	std::vector<std::pair<std::string, ReplayMoment>> moments;
	for (const auto* item : content_.all(ContentLibrary::Kind::Scenario))
	{
		if (item->category != "replay")
			continue;
		ReplayMoment mo;
		mo.id = item->id;
		mo.name = item->name;
		mo.description = item->data.value("description", "");
		mo.arena = item->data.value("arena", "");
		mo.save = item->data.contains("success") && item->data["success"].value("type", "") == "save";
		const std::string when = item->data.contains("source") ? item->data["source"].value("label", "") : "";
		moments.push_back({when, mo});
	}
	std::sort(moments.begin(), moments.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	for (auto& [when, mo] : moments)
		model->replayMoments.push_back(std::move(mo));

	// Freeplay+ picker: the objectives (one_of choices with ids) and which are switched on.
	model->freeplayId = kFreeplayId;
	model->freeplayEnabled = freeplayEnabled_;
	model->freeplayUnlimitedBoost = freeplayUnlimited_;
	model->routineTemplates = routineTemplates_;
	if (const auto* fpp = content_.find(ContentLibrary::Kind::Scenario, kFreeplayId))
	{
		for (const auto& obj : fpp->data.value("objects", nlohmann::json::array()))
		{
			for (const auto& c : obj.value("choices", nlohmann::json::array()))
			{
				if (c.contains("id"))
					model->freeplayChoices.push_back({c["id"].get<std::string>(), c.value("label", ""), c.value("group", "Other"),
						c.value("about", ""), c.value("beginner", false)});
			}
		}
	}

	// Exercise tiles and session cards in category order (Freeplay+ first), then by id.
	auto rank = [&](const std::string& category) {
		for (size_t i = 0; i < std::size(kCategoryOrder); ++i)
			if (category == kCategoryOrder[i])
				return static_cast<int>(i);
		return static_cast<int>(std::size(kCategoryOrder));
	};
	std::stable_sort(model->exercises.begin(), model->exercises.end(),
		[&](const ExerciseCard& a, const ExerciseCard& b) { return rank(a.category) < rank(b.category); });
	std::stable_sort(model->sessions.begin(), model->sessions.end(),
		[&](const SessionCard& a, const SessionCard& b) { return rank(a.category) < rank(b.category); });
	// Home tiles' art.
	static const char* kHomeImages[] = {"exercises", "sessions", "quick_play", "settings"};
	for (int i = 0; i < 4; ++i)
	{
		for (const char* ext : {".png", ".jpg"})
		{
			const int handle = game_->loadImage(imageDir / "home" / (std::string(kHomeImages[i]) + ext));
			if (handle >= 0)
			{
				model->homeImages[i] = handle;
				break;
			}
		}
	}
	// Try next: playable exercises by category (not Freeplay+, captures or dev drills).
	suggestPool_.clear();
	for (const auto& cat : model->categories)
	{
		if (cat == "speed_lab" || cat == "captured" || cat == "dev")
			continue;
		std::vector<std::string> ids;
		for (const auto& c : model->exercises)
			if (c.category == cat && c.pending.empty())
				ids.push_back(c.id);
		if (!ids.empty())
			suggestPool_.push_back({cat, ids});
	}
	refreshHome(true);
	dashboard_->setModel(std::move(model));
}

std::shared_ptr<PolicyBot> FreeTouch::makeBrain(const std::string& id)
{
	// content/bots/<id>/: policy.bin (tools/export_policy.py) and bot.json. Loaded once, shared by every rep.
	auto& slot = policies_[id];
	if (!slot.tried)
	{
		slot.tried = true;
		const auto dir = contentRoot() / "bots" / id;
		std::string error;
		if (!PolicyBotConfig::load(dir / "bot.json", slot.config, error))
			LOG("Bots: {} has no usable bot.json ({}); using defaults", id, error);
		auto policy = std::make_shared<EarlPolicy>();
		if (policy->load(dir / "policy.bin", error, slot.config.heads))
		{
			slot.policy = policy;
			LOG("Bots: {} loaded ({} inputs, {} outputs)", slot.config.name, policy->queryDim(), policy->outputs());
			eventLog_.write("bot_model_loaded", {{"bot", id}, {"queryDim", policy->queryDim()}, {"entityDim", policy->entityDim()},
				{"outputs", policy->outputs()}});
		}
		else
		{
			LOG("Bots: {} model not loaded ({})", id, error);
			eventLog_.write("bot_model_failed", {{"bot", id}, {"error", error}});
		}
	}
	if (!slot.policy)
		return nullptr;
	return std::make_shared<PolicyBot>(slot.policy, slot.config);
}

bool FreeTouch::brainAvailable(const std::string& id)
{
	std::error_code ec;
	return std::filesystem::exists(contentRoot() / "bots" / id / "policy.bin", ec);
}

void FreeTouch::refreshHome(bool force)
{
	if (!force && history_.version() == homeVersion_ && tick_ < homeBuiltTick_ + 240)
		return;
	homeVersion_ = history_.version();
	homeBuiltTick_ = tick_;
	if (today_.empty())
		today_ = localTime("%Y-%m-%d");
	HomeStats h;
	const auto day = history_.day(today_);
	h.todayMinutes = day.seconds / 60.0;
	h.goalMinutes = dailyGoalMinutes_;
	h.repsToday = day.reps;
	h.successesToday = day.successes;
	h.streakDays = history_.streak(today_);
	for (int i = 0; i < 7; ++i)
		h.week[i] = history_.trained(TrainingHistory::shiftDay(today_, i - 6));
	for (const auto& id : history_.recent(6))
	{
		const auto* e = history_.exercise(id);
		if (e && content_.find(ContentLibrary::Kind::Scenario, id))
			h.recent.push_back({id, e->reps, e->successes, e->lastLabel, e->lastOutcome});
		if (h.recent.size() == 4)
			break;
	}
	const auto next = history_.suggest(suggestPool_, static_cast<long long>(std::time(nullptr)));
	h.nextId = next.id;
	h.nextCategory = next.category;
	h.nextDaysAgo = next.daysAgo;
	home_ = std::move(h);

	// Per exercise, for the Exercises page (detail and sorting).
	static const char* kDiff[] = {"easy", "medium", "hard"};
	auto stats = std::make_shared<std::map<std::string, ExerciseStatsView>>();
	for (const auto* item : content_.all(ContentLibrary::Kind::Scenario))
	{
		ExerciseStatsView v;
		bool any = false;
		if (const auto* e = history_.exercise(item->id))
		{
			v.reps = e->reps;
			v.successes = e->successes;
			v.lastPlayed = e->lastPlayed;
			v.lastLabel = e->lastLabel;
			v.lastOutcome = e->lastOutcome;
			any = true;
		}
		for (int d = 0; d < 3; ++d)
		{
			v.best[d] = bests_.best(BestTimes::key(BestTimes::recordId(item->id, item->data), kDiff[d]));
			any = any || v.best[d] > 0.0;
		}
		if (any)
			(*stats)[item->id] = std::move(v);
	}
	exerciseStats_ = std::move(stats);
}

std::filesystem::path FreeTouch::historyFile()
{
	return game_->dataFolder() / "freetouch" / "settings" / "history.json";
}

void FreeTouch::loadHistory()
{
	std::ifstream in(historyFile());
	if (!in)
		return;
	try
	{
		history_.fromJson(nlohmann::json::parse(in));
	}
	catch (const std::exception&)
	{
		LOG("Free Touch: settings/history.json could not be read; starting a new history");
	}
}

void FreeTouch::saveHistory()
{
	if (!history_.dirty())
		return;
	std::error_code ec;
	std::filesystem::create_directories(historyFile().parent_path(), ec);
	const auto tmp = std::filesystem::path(historyFile().string() + ".tmp");
	{
		std::ofstream out(tmp, std::ios::trunc);
		out << history_.toJson().dump(1);
		if (!out)
			return;
	}
	std::filesystem::rename(tmp, historyFile(), ec);
	if (!ec)
		history_.clearDirty();
}

void FreeTouch::playMenuSound(const char* name)
{
	if (std::strcmp(name, "ui_confirm") == 0)
		lastConfirmMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	sound_.play(name, menuVolume_);
}

void FreeTouch::onMechanics()
{
	// Every detector decision goes to the event log with the values that triggered it (plain touches are logged by
	// TouchLogger already); the shown ones go to the feed, and the bigger ones get a chime that climbs with the points.
	const bool forced = player_->wantsMechanicsFeed();
	for (const auto& e : detector_.take())
	{
		player_->onMechanic(e);
		if (e.id != "touch")
			eventLog_.write("mechanic", {{"id", e.id}, {"label", e.label}, {"tick", e.tick}, {"points", e.points},
				{"shown", e.show}, {"kmh", std::round(e.kmh)}, {"values", e.values}});
		if (!e.show)
			continue;
		if (e.id != "goal")
			progress_.bump("mech:" + e.id);
		hud_.pushMechanic(e.label, e.id == "goal" || e.id == "pinch" || e.id == "flick" || e.id == "power_shot" ? e.kmh : 0.f,
			e.points, forced);
		if ((feedVisible() || forced) && e.points >= 3 && e.id != "goal")
			playTrainingSound("object", std::pow(2.f, std::min(e.points, 12) / 12.f), 0.8f);
	}
}

void FreeTouch::playTrainingSound(const char* name, float pitch, float gain)
{
	sound_.play(name, trainingVolume_ * gain, pitch);
}

void FreeTouch::trainingCues(const ScenarioPlayer::Status& s)
{
	// Game thread, every tick: sounds for what changed since the last tick.
	if (!s.active)
	{
		cueRep_ = 0;
		cueObjects_ = cuePins_ = 0;
		cueObjective_.clear();
		return;
	}
	if (s.rep != cueRep_)
	{
		// A rep with a start countdown opens on its first beep; others get a quiet "ready" ping, unless a result cue
		// just played (the next rep often follows it at once).
		if (s.countdown > 0)
			playTrainingSound("count");
		else if (tick_ > lastResultCueTick_ + 70)
			playTrainingSound("rep_start", 1.f, 0.7f);
		cueCountdown_ = s.countdown;
		cueRep_ = s.rep;
		cueObjects_ = s.objectsHit;
		cuePins_ = s.pinsDown;
		cueObjective_ = s.objective;
		return;
	}
	// Rings, circles and targets along a course: each one a step higher (a pentatonic run).
	if (s.objectsHit > cueObjects_)
	{
		static const int kSteps[] = {0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24};
		const int step = kSteps[std::min(s.objectsHit - 1, static_cast<int>(std::size(kSteps)) - 1)];
		playTrainingSound("object", std::pow(2.f, step / 12.f));
	}
	cueObjects_ = s.objectsHit;
	if (s.pinsDown > cuePins_)
	{
		for (int i = 0; i < std::min(s.pinsDown - cuePins_, 3); ++i)
			playTrainingSound("pin", 0.9f + 0.1f * static_cast<float>((tick_ + i * 7) % 4), 0.8f);
	}
	cuePins_ = s.pinsDown;
	// Saved Shots: the frozen shot went live.
	if (cueObjective_ == "Press any button to play" && s.objective != cueObjective_)
		playTrainingSound("go");
	// The start countdown, like a kickoff: a beep on 3, 2 and 1, then GO.
	if (s.countdown != cueCountdown_)
	{
		if (s.countdown > 0)
			playTrainingSound("count");
		else if (cueCountdown_ > 0)
			playTrainingSound("count_go");
		cueCountdown_ = s.countdown;
	}
	cueProgress_ = s.objectiveProgress;
	cueObjective_ = s.objective;
}

void FreeTouch::OnOpen()
{
	PluginWindowBase::OnOpen();
	playMenuSound("ui_open");
}

void FreeTouch::OnClose()
{
	PluginWindowBase::OnClose();
	const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	if (nowMs - lastConfirmMs_ > 600) // starting something already made its sound
		playMenuSound("ui_close");
}

std::filesystem::path FreeTouch::uiSettingsFile()
{
	return game_->dataFolder() / "freetouch" / "settings" / "ui.json";
}

void FreeTouch::loadUiSettings()
{
	std::ifstream in(uiSettingsFile());
	if (!in)
		return;
	try
	{
		const auto doc = nlohmann::json::parse(in);
		if (doc.contains("accent") && doc["accent"].is_array() && doc["accent"].size() == 3)
		{
			const auto& a = doc["accent"];
			theme::setAccent(ImVec4(a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), 1.f));
		}
		goalExplosion_ = doc.value("goalExplosion", true);
		goalReplays_ = doc.value("goalReplays", false);
		firstRunDone_ = doc.value("firstRunDone", false);
		tutorialsSeen_.clear();
		for (const auto& id : doc.value("tutorialsSeen", nlohmann::json::array()))
			if (id.is_string())
				tutorialsSeen_.insert(id.get<std::string>());
		devTools_ = doc.value("devTools", false);
		autoUpdate_ = doc.value("autoUpdate", true);
		mechanicsFeed_ = doc.value("mechanicsFeed", true);
		defender_ = doc.value("defender", std::string("psyonix"));
		if (defender_ != "necto" && defender_ != "nexto")
			defender_ = "psyonix";
		packDefender_ = doc.value("packDefender", std::string());
		if (packDefender_ != "psyonix" && packDefender_ != "necto" && packDefender_ != "nexto")
			packDefender_.clear();
		packTarget_ = doc.value("packTarget", std::string());
		if (packTarget_ != "tile" && packTarget_ != "moving" && packTarget_ != "pick")
			packTarget_.clear();
		packTileSize_ = doc.value("packTileSize", std::string("medium"));
		if (packTileSize_ != "easy" && packTileSize_ != "hard" && packTileSize_ != "impossible")
			packTileSize_ = "medium";
		packTile_ = std::clamp(doc.value("packTile", 0), 0, 7);
		favorites_.clear();
		for (const auto& id : doc.value("favorites", nlohmann::json::array()))
			if (id.is_string())
				favorites_.insert(id.get<std::string>());
		favoritesPub_ = std::make_shared<std::set<std::string>>(favorites_);
		dailyGoalMinutes_ = std::clamp(doc.value("dailyGoalMinutes", 20.0), 5.0, 240.0);
		if (doc.contains("sound") && doc["sound"].is_object())
		{
			menuVolume_ = std::clamp(doc["sound"].value("menu", 0.6f), 0.f, 1.f);
			trainingVolume_ = std::clamp(doc["sound"].value("training", 0.7f), 0.f, 1.f);
		}
		if (doc.contains("practice") && doc["practice"].is_object())
		{
			const auto& p = doc["practice"];
			PracticeSpeedSettings s;
			s.enabled = p.value("enabled", s.enabled);
			s.hold = p.value("hold", s.hold);
			s.practice = p.value("speed", s.practice);
			s.clutch = p.value("clutch", s.clutch);
			practice_.setSettings(s);
		}
	}
	catch (const std::exception&)
	{
	}
	player_->setGoalExplosion(goalExplosion_);
	player_->setGoalReplays(goalReplays_);
	player_->setPackDefender(packDefender_);
	player_->setPackTarget(packTarget_, packTileSize_, packTile_);
	bots_->setDefender(defender_);
	hud_.setFeedEnabled(feedVisible());
}

void FreeTouch::saveUiSettings()
{
	const ImVec4 accent = theme::palette().accent;
	std::error_code ec;
	std::filesystem::create_directories(uiSettingsFile().parent_path(), ec);
	safefile::write(uiSettingsFile(), nlohmann::json{
		{"accent", {accent.x, accent.y, accent.z}},
		{"goalExplosion", goalExplosion_},
		{"goalReplays", goalReplays_},
		{"firstRunDone", firstRunDone_},
		{"tutorialsSeen", tutorialsSeen_},
		{"devTools", devTools_},
		{"autoUpdate", autoUpdate_},
		{"mechanicsFeed", mechanicsFeed_},
		{"defender", defender_},
		{"packDefender", packDefender_},
		{"packTarget", packTarget_},
		{"packTileSize", packTileSize_},
		{"packTile", packTile_},
		{"favorites", favorites_},
		{"dailyGoalMinutes", dailyGoalMinutes_},
		{"sound", {{"menu", menuVolume_.load()}, {"training", trainingVolume_.load()}}},
		{"practice", {{"enabled", practice_.settings().enabled}, {"hold", practice_.settings().hold},
			{"speed", practice_.settings().practice}, {"clutch", practice_.settings().clutch}}},
	}.dump(2));
	eventLog_.write("ui_settings", {{"accent", {accent.x, accent.y, accent.z}}, {"goalExplosion", goalExplosion_}});
}

void FreeTouch::saveAccent(const ImVec4& accent)
{
	theme::setAccent(accent);
	saveUiSettings();
}

void FreeTouch::refreshControls()
{
	// Every swapped bind and setting back to the player's own, then Free Touch's again.
	settings_->restoreAll();
	settings_->setBind(kSelectKey, "ft_select");
	settings_->setBind(kMenuKey, "ft_menu");
	if (controls_)
		controls_->reapply();
	navBindsOn_ = false; // the next tick sets the menu's controller binds again if it is open
	replayBindOn_ = false; // in a replay, the next frame takes R3 for takeover again
	practice_.update(false, false, false);
	eventLog_.write("controls_refreshed");
	LOG("Free Touch: controls refreshed");
}

bool FreeTouch::editShot(const std::string& id, const std::function<void(nlohmann::json&)>& change)
{
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
	if (!item || item->category != "captured")
		return false;
	nlohmann::json doc = item->data;
	change(doc);
	{
		std::ofstream out(item->file, std::ios::trunc);
		out << doc.dump(2) << "\n";
		if (!out)
		{
			LOG("Saved Shots: could not write {}", item->file.string());
			return false;
		}
	}
	eventLog_.write("shot_edited", {{"id", id}});
	reloadContent();
	return true;
}

void FreeTouch::deleteShot(const std::string& id)
{
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
	if (!item || item->category != "captured")
		return;
	std::error_code ec;
	std::filesystem::remove(item->file, ec);
	eventLog_.write("shot_deleted", {{"id", id}, {"ok", !ec}});
	reloadContent();
}

namespace
{
	std::string utf8Of(const std::filesystem::path& p)
	{
		const std::u8string u = p.u8string();
		return std::string(reinterpret_cast<const char*>(u.data()), u.size());
	}
}

void FreeTouch::startAccount()
{
	// Requests go out through the adapter (BakkesMod's curl); replies come back here on the game thread. Nothing is
	// sent during an online match.
	account_ = std::make_unique<Account>(
		[this](const online::HttpRequest& request, online::HttpDone done) {
			if (game_->inOnlineGame())
			{
				done(0, "");
				return;
			}
			game_->http(request, [this, done](int status, const std::string& body) {
				game_->runOnGameThread([done, status, body] { done(status, body); });
			});
		},
		[this](const std::string& event, const nlohmann::json& fields) { eventLog_.write(event, fields); });
	account_->setChanged([this] {
		saveAccount();
		publishStatus();
	});
	friends_ = std::make_unique<Friends>(*account_,
		[this](const std::string& event, const nlohmann::json& fields) { eventLog_.write(event, fields); });
	friends_->setChanged([this] { publishStatus(); });
	shots_ = std::make_unique<OnlineShots>(*account_,
		[this](const std::string& event, const nlohmann::json& fields) { eventLog_.write(event, fields); });
	shots_->setChanged([this] { publishStatus(); });
	boards_ = std::make_unique<Leaderboards>(*account_,
		[this](const std::string& event, const nlohmann::json& fields) { eventLog_.write(event, fields); });
	boards_->setChanged([this] { publishStatus(); });
}

void FreeTouch::postDailyScore(const daily::Attempt& a)
{
	// Lambourne 2026-10-29: the daily's leaderboard. Its squares as digits: 1 fast hit, 2 hit, 3 miss.
	if (!boards_ || !account_ || !account_->signedIn() || !a.done || a.results.empty())
		return;
	std::string marks;
	for (const auto& r : a.results)
		marks += static_cast<char>('0' + std::clamp(static_cast<int>(r.mark), 0, 3));
	boards_->postDaily(a.date, a.number, a.points(), a.hits(), marks, static_cast<int64_t>(std::time(nullptr)));
}

Friends::Activity FreeTouch::activityNow()
{
	// From the training history on this PC: the last 7 days' minutes, today's, the streak, the last drill.
	if (today_.empty())
		today_ = localTime("%Y-%m-%d");
	Friends::Activity a;
	double week = 0.0;
	for (int i = 0; i < 7; ++i)
		week += history_.day(TrainingHistory::shiftDay(today_, -i)).seconds;
	a.weekMinutes = static_cast<int>(week / 60.0);
	a.todayMinutes = static_cast<int>(history_.day(today_).seconds / 60.0);
	a.streakDays = history_.streak(today_);
	for (const auto& id : history_.recent(1))
		if (const auto* item = content_.find(ContentLibrary::Kind::Scenario, id))
			a.lastDrill = item->name;
	a.trainedSinceLast = activityVersion_ >= 0 && history_.version() != activityVersion_;
	return a;
}

void FreeTouch::tickOnline()
{
	if (!account_ || !friends_)
		return;
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	const bool in = account_->signedIn();
#if FT_SELF_UPDATE
	if (in)
		sharePublishToken(now);
#endif
	if (in != onlineSignedIn_)
	{
		onlineSignedIn_ = in;
#if FT_SELF_UPDATE
		isPublisher_ = -1;
#endif
		if (!in)
		{
			friends_->clear();
			shots_->clear();
			boards_->clear();
			return;
		}
		// Just signed in: the lists now, the activity on the next check.
		friendsAt_ = now;
		activityAt_ = 0;
		friends_->refresh(now);
		shots_->refreshInbox(now);
		boards_->refreshRaceLeaders(now);
		if (!ghostsUploaded_)
		{
			// The bests from before Phase 8 (settings/ghosts/), once: the service keeps only improvements.
			ghostsUploaded_ = true;
			std::error_code ec;
			const auto dir = game_->dataFolder() / "freetouch" / "settings" / "ghosts";
			for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
			{
				if (it->path().extension() != ".json")
					continue;
				try
				{
					std::ifstream in(it->path());
					const auto j = nlohmann::json::parse(in);
					const double seconds = j.value("seconds", 0.0);
					if (seconds > 0.3)
						boards_->postRace(it->path().stem().string(), seconds, j, now);
				}
				catch (const std::exception&)
				{
				}
			}
		}
	}
	if (!in || game_->inOnlineGame())
		return;
	// Today's daily already on the service (a lost file, another PC): done here too. Once a day.
	checkDailyWithService({});
	// Today's daily, finished while signed out or offline: posted now (once).
	if (const auto it = daily_->attempts().find(localTime("%Y-%m-%d")); it != daily_->attempts().end() && it->second.done
		&& boards_->view().postedDay != it->first)
		postDailyScore(it->second);
	const bool trained = history_.version() != activityVersion_;
	if (account_->shareActivity() && (activityAt_ == 0 || now >= activityAt_ + 300 || (trained && now >= activityAt_ + 60)))
	{
		friends_->sendActivity(activityNow(), now);
		activityVersion_ = history_.version();
		activityAt_ = now;
	}
	if (isWindowOpen_ && now >= friendsAt_ + 120)
	{
		friendsAt_ = now;
		friends_->refresh(now);
		shots_->refreshInbox(now); // shots friends sent
	}
}

void FreeTouch::pollOnline()
{
	// Same pattern as pollMmr: rescheduled every 15 s until unload.
	if (unloading_)
		return;
	loadAccountOnce();
	tickOnline();
#if FT_SELF_UPDATE
	// Updates: 30 s after start, then every 6 hours (not in an online match).
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	if (updater_ && !game_->inOnlineGame())
	{
		if (updateCheckAt_ == 0)
			updateCheckAt_ = now + 30;
		else if (now >= updateCheckAt_)
		{
			updateCheckAt_ = now + 6 * 3600;
			const bool devContent = !cvarManager->getCvar("ft_content_dir").getStringValue().empty();
			updater_->check(autoUpdate_ && !devContent);
		}
	}
#endif
	game_->runAfter(15.f, [this] { pollOnline(); });
}

void FreeTouch::runInBackground(std::function<void()> work, std::function<void()> then)
{
	std::lock_guard lock(workersMutex_);
	// Finished workers are joined as new ones start; the rest before unload.
	workers_.emplace_back([this, work = std::move(work), then = std::move(then)] {
		work();
		if (!unloading_)
			game_->runOnGameThread([this, then] {
				if (!unloading_)
					then();
			});
	});
}

#if FT_SELF_UPDATE
void FreeTouch::startUpdater()
{
	const std::filesystem::path root = game_->dataFolder().parent_path(); // BakkesMod's folder (plugins/, data/)
	std::error_code ec;
	std::filesystem::remove(root / "plugins" / "FreeTouch.dll.old", ec); // the one an update moved aside
	Updater::Io io;
	io.http = [this](const online::HttpRequest& request, online::HttpDone done) {
		game_->http(request, [this, done](int status, const std::string& body) {
			game_->runOnGameThread([done, status, body] { done(status, body); });
		});
	};
	io.download = [this](const std::string& url, const std::filesystem::path& to, std::function<void(int)> done) {
		game_->download(url, to, [this, done](int status) { game_->runOnGameThread([done, status] { done(status); }); });
	};
	io.background = [this](std::function<void()> work, std::function<void()> then) { runInBackground(std::move(work), std::move(then)); };
	updater_ = std::make_unique<Updater>(std::move(io), root, plugin_version, VERSION_BUILD,
		[this](const std::string& event, const nlohmann::json& fields) { eventLog_.write(event, fields); });
	updater_->setAutoInstall(autoUpdate_);
	updater_->setChanged([this] {
		publishStatus();
		if (updater_->view().state == Updater::State::Installed && !updateNoticed_)
		{
			updateNoticed_ = true; // once
			notice("Free Touch " + updater_->view().latest + " is installed: press F9 (or restart the game) to load it");
		}
	});
	// Started by an update: say so once, with what's new.
	const auto installed = root / "data" / "freetouch" / "update" / "installed.json";
	if (std::ifstream in(installed); in)
	{
		try
		{
			const auto j = nlohmann::json::parse(in);
			in.close();
			if (j.value("version", "") == std::string(plugin_version))
			{
				updater_->setJustUpdated(plugin_version, j.value("notes", ""));
				eventLog_.write("update_loaded", {{"version", plugin_version}});
			}
		}
		catch (const std::exception&)
		{
		}
		std::filesystem::remove(installed, ec);
	}
}

void FreeTouch::sharePublishToken(int64_t now)
{
	// Publishers only (asked once per sign-in): the current sign-in for tools/release.py while the game is open, so
	// the script never has to refresh (and so invalidate) the plugin's own saved one.
	if (!account_ || !account_->signedIn())
		return;
	if (isPublisher_ < 0)
	{
		isPublisher_ = 0;
		online::HttpRequest r;
		r.method = "POST";
		r.url = "/rest/v1/rpc/is_publisher";
		r.body = "{}";
		account_->send(r, now, [this](int status, const std::string& body) {
			isPublisher_ = status >= 200 && status < 300 && body.find("true") != std::string::npos ? 1 : 0;
			eventLog_.write("publisher_checked", {{"publisher", isPublisher_ == 1}});
		});
		return;
	}
	if (isPublisher_ != 1)
		return;
	// Kept fresh: a sign-in about to run out is renewed with a small request.
	if (account_->accessExpiresAt() < now + 600)
	{
		online::HttpRequest r;
		r.method = "POST";
		r.url = "/rest/v1/rpc/is_publisher";
		r.body = "{}";
		account_->send(r, now, [](int, const std::string&) {});
		return;
	}
	if (account_->accessToken() == publishTokenWritten_)
		return;
	publishTokenWritten_ = account_->accessToken();
	safefile::write(game_->dataFolder() / "freetouch" / "settings" / "publish_token.json",
		nlohmann::json{{"access_token", account_->accessToken()}, {"expires_at", account_->accessExpiresAt()},
			{"user_id", account_->userId()}}.dump());
}
#endif


void FreeTouch::loadAccountOnce()
{
	// Sign back in with the saved session, once, and never during an online match.
	if (accountStarted_ || !account_ || game_->inOnlineGame())
		return;
	accountStarted_ = true;
	nlohmann::json saved = nlohmann::json::object();
	std::ifstream in(game_->dataFolder() / "freetouch" / "settings" / "account.json");
	if (in)
	{
		try
		{
			saved = nlohmann::json::parse(in);
		}
		catch (const std::exception&)
		{
		}
	}
	account_->load(saved, static_cast<int64_t>(std::time(nullptr)));
	publishStatus();
}

void FreeTouch::saveAccount()
{
	const auto dir = game_->dataFolder() / "freetouch" / "settings";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	safefile::write(dir / "account.json", account_->save().dump(2) + "\n"); // the sign-in: never half written
}

void FreeTouch::notice(const std::string& text)
{
	notice_ = text;
	++noticeSeq_;
	publishStatus();
}

bool FreeTouch::shareShot(const std::string& id)
{
	// Lambourne 2026-10-29: "an easy way to share recorded shots". A file, so it travels in Discord as one: written
	// to freetouch/shared/ and put on the clipboard as a file (Ctrl+V in Discord attaches it). No network.
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
	if (!item || item->category != "captured")
		return false;
	const Snapshot s = game_->snapshot();
	const CarState* me = s.localCar();
	const std::string by = me && !me->name.empty() ? me->name : "a Free Touch player";
	const auto doc = shotshare::shareDoc(item->data, by, localTime("%Y-%m-%d"), plugin_version);
	const auto dir = game_->dataFolder() / "freetouch" / "shared";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const auto path = dir / shotshare::shareFileName(doc.value("name", "shot"));
	{
		std::ofstream out(path, std::ios::trunc | std::ios::binary);
		out << doc.dump() << "\n";
		if (!out)
		{
			LOG("Share: could not write {}", path.string());
			notice("Couldn't save the shot to share");
			return false;
		}
	}
	const bool copied = game_->copyFileToClipboard(path);
	eventLog_.write("shot_shared", {{"id", id}, {"path", utf8Of(path)}, {"by", by}, {"key", doc["key"]}, {"clipboard", copied},
		{"bytes", std::filesystem::file_size(path, ec)}});
	LOG("Share: {} -> {}{}", id, path.string(), copied ? " (on the clipboard)" : "");
	notice(copied ? "Copied! Paste it in Discord with Ctrl+V" : "Saved in freetouch/shared/ (the clipboard was busy)");
	return true;
}

void FreeTouch::scanInbox()
{
	// Shared shots (.ftshot) in Downloads, where Discord saves them, or dropped into freetouch/inbox/. Only those
	// files are read.
	std::set<std::string> have;
	for (const auto* item : content_.all(ContentLibrary::Kind::Scenario))
	{
		if (item->category != "captured")
			continue;
		have.insert(shotshare::keyOf(item->data));
		if (item->data.contains("shared") && item->data["shared"].is_object())
			have.insert(item->data["shared"].value("key", ""));
	}
	haveKeys_ = std::make_shared<const std::set<std::string>>(have);
	std::vector<std::pair<std::filesystem::file_time_type, SharedShotFile>> found;
	for (const auto& dir : {game_->downloadsFolder(), game_->dataFolder() / "freetouch" / "inbox"})
	{
		std::error_code ec;
		if (dir.empty() || !std::filesystem::is_directory(dir, ec))
			continue;
		for (auto it = std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
			!ec && it != std::filesystem::directory_iterator(); it.increment(ec))
		{
			std::error_code fe;
			if (!it->is_regular_file(fe) || it->path().extension() != shotshare::kExtension
				|| it->file_size(fe) > shotshare::kMaxFileBytes)
				continue;
			std::ifstream in(it->path(), std::ios::binary);
			std::stringstream text;
			text << in.rdbuf();
			shotshare::Shared sh;
			std::string error;
			if (!shotshare::readShare(text.str(), sh, error))
				continue;
			found.push_back({it->last_write_time(fe), {utf8Of(it->path()), sh.name, sh.by, sh.on, have.count(sh.key) > 0}});
		}
	}
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	auto list = std::make_shared<std::vector<SharedShotFile>>();
	for (size_t i = 0; i < found.size() && i < 12; ++i)
		list->push_back(found[i].second);
	inbox_ = list;
	publishStatus();
}

bool FreeTouch::importShared(const std::filesystem::path& file)
{
	std::error_code ec;
	if (file.extension() != shotshare::kExtension || std::filesystem::file_size(file, ec) > shotshare::kMaxFileBytes || ec)
	{
		notice("That isn't a Free Touch shot (a .ftshot file)");
		return false;
	}
	std::ifstream in(file, std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	return !importSharedText(text.str(), utf8Of(file), "").empty();
}

std::string FreeTouch::importSharedText(const std::string& text, const std::string& from, const std::string& onlineId)
{
	// A .ftshot document (a file, or a shot from the Free Touch service) as a Saved Shot; one already here (same
	// fingerprint) is not added twice. Returns its scenario id ("" if it couldn't be added).
	std::error_code ec;
	shotshare::Shared sh;
	std::string error;
	if (!shotshare::readShare(text, sh, error))
	{
		eventLog_.write("shot_import_failed", {{"from", from}, {"why", error}});
		notice("Couldn't import it: " + error);
		return "";
	}
	for (const auto* item : content_.all(ContentLibrary::Kind::Scenario))
	{
		if (item->category == "captured" && (shotshare::keyOf(item->data) == sh.key
			|| (item->data.contains("shared") && item->data["shared"].value("key", "") == sh.key)))
		{
			eventLog_.write("shot_import_skipped", {{"from", from}, {"id", item->id}, {"why", "already here"}});
			return item->id;
		}
	}
	const auto dir = contentRoot() / "captured";
	std::filesystem::create_directories(dir, ec);
	const std::string base = shotshare::importFileName(sh);
	std::string finalName = base;
	for (int n = 2; std::filesystem::exists(dir / (finalName + ".json")); ++n)
		finalName = base + "_" + std::to_string(n);
	const auto doc = shotshare::importedScenario(sh, finalName, localTime("%Y-%m-%d %H:%M:%S"), onlineId);
	const auto path = dir / (finalName + ".json");
	{
		std::ofstream out(path, std::ios::trunc);
		out << doc.dump(2) << "\n";
		if (!out)
		{
			notice("Couldn't save the imported shot");
			return "";
		}
	}
	reloadContent();
	// Checked like every other content file: one the schema rejects is not kept.
	const std::string id = doc["id"].get<std::string>();
	const bool ok = content_.find(ContentLibrary::Kind::Scenario, id) != nullptr;
	if (!ok)
	{
		std::filesystem::remove(path, ec);
		reloadContent();
	}
	eventLog_.write("shot_imported", {{"from", from}, {"id", id}, {"by", sh.by}, {"key", sh.key}, {"online", onlineId}, {"ok", ok}});
	LOG("Share: imported {} from {} ({})", id, sh.by, ok ? "ok" : "rejected by the schema, see the content errors");
	notice(ok ? "Added \"" + sh.name + "\" from " + sh.by : "Couldn't import it (see the console)");
	return ok ? id : "";
}

void FreeTouch::shareShotOnline(const std::string& id, bool makePublic, const std::vector<std::string>& friendIds,
	const std::string& toNames)
{
	// The same document as a .ftshot file, uploaded instead (Lambourne 2026-10-29: share shots online).
	const auto* item = content_.find(ContentLibrary::Kind::Scenario, id);
	if (!item || item->category != "captured" || !account_->signedIn())
		return;
	const auto doc = shotshare::shareDoc(item->data, account_->view().name, localTime("%Y-%m-%d"), plugin_version);
	shots_->post(doc, makePublic, friendIds, toNames, static_cast<int64_t>(std::time(nullptr)));
}

void FreeTouch::addOnlineShot(const std::string& shotId, const std::string& delivery, bool play,
	std::optional<shotshare::Extras> setup, const std::string& difficulty)
{
	// From the Shot Board or a friend: fetched, added to your drills (once), given the setup picked before playing,
	// and played if asked.
	shots_->fetch(shotId, static_cast<int64_t>(std::time(nullptr)), [this, shotId, delivery, play, setup, difficulty](
		const std::string& doc, const std::string& error) {
		if (doc.empty())
		{
			notice(error);
			return;
		}
		const std::string id = importSharedText(doc, "online " + shotId, shotId);
		if (id.empty())
			return;
		if (!delivery.empty())
			shots_->dismiss(delivery, static_cast<int64_t>(std::time(nullptr)));
		if (setup)
			editShot(id, [&](nlohmann::json& d) { shotshare::setExtras(d, *setup); });
		if (play)
		{
			const std::string diff = difficulty.empty() ? "medium" : difficulty;
			startInFreeplay([this, id, diff] { startExercise(id, diff, {}, {}); }, id);
		}
	});
}

void FreeTouch::addToWarmup(const std::string& id)
{
	// Into the warm-up section's first drill block (made if there is none; a Warm-up section too if needed).
	if (routine_.sections.empty())
	{
		routine::Section s = routine::makeSection(routine_, "Warm-up");
		s.warmup = true;
		routine_.sections.push_back(s);
	}
	const std::string warm = routine::warmupSection(routine_)->id;
	auto it = std::find_if(routine_.steps.begin(), routine_.steps.end(),
		[&](const routine::Step& s) { return s.type == "drills" && s.section == warm; });
	if (it == routine_.steps.end())
	{
		const auto at = std::find_if(routine_.steps.begin(), routine_.steps.end(), [&](const routine::Step& s) { return s.section == warm; });
		it = routine_.steps.insert(at, routine::makeStep(routine_, "drills", warm));
	}
	if (std::none_of(it->drills.begin(), it->drills.end(), [&](const routine::Drill& d) { return d.id == id; }))
		it->drills.push_back({id, 5, ""});
	saveRoutine();
	reloadContent();
	publishRoutine();
}

void FreeTouch::updateMenuNavBinds()
{
	// While the window is open the controller drives it: D-pad moves, A selects, B goes back, Start closes. The
	// original binds come back when it closes; an exercise running meanwhile gets its own binds again.
	const bool open = isWindowOpen_;
	if (open == navBindsOn_)
		return;
	if (open)
	{
		scanInbox(); // a shot a friend shared may have landed in Downloads since
		loadAccountOnce();
	}
	static const std::pair<const char*, const char*> kNavBinds[] = {
		{"XboxTypeS_DPad_Up", "ft_nav up"}, {"XboxTypeS_DPad_Down", "ft_nav down"},
		{"XboxTypeS_DPad_Left", "ft_nav left"}, {"XboxTypeS_DPad_Right", "ft_nav right"},
		{"XboxTypeS_A", "ft_nav select"}, {"XboxTypeS_B", "ft_nav back"}, {"XboxTypeS_Start", "ft_nav close"},
	};
	for (const auto& [key, command] : kNavBinds)
	{
		if (open)
			settings_->setBind(key, command);
		else
			settings_->restoreBind(key);
	}
	if (!open && controls_)
		controls_->reapply();
	navBindsOn_ = open;
	updateFreeplayBindings();
	eventLog_.write("menu_nav_binds", {{"on", open}});
}

void FreeTouch::updatePracticeSpeed()
{
	// The slow-motion and clutch buttons are read from the key state (hold works); the game speed changes only
	// when the wanted speed does, and goes back to the original value (normal speed) through the SettingSwapper.
	const bool active = player_ && player_->status().active && game_->inFreeplay();
	bool slowHeld = false, clutchHeld = false;
	if (active)
	{
		const std::string slowKey = controls_->controls().keyFor("slowmo");
		const std::string clutchKey = controls_->controls().keyFor("clutch");
		std::vector<std::string> keys;
		if (!slowKey.empty()) keys.push_back(slowKey);
		if (!clutchKey.empty()) keys.push_back(clutchKey);
		for (const auto& k : game_->keysDown(keys))
		{
			slowHeld = slowHeld || k == slowKey;
			clutchHeld = clutchHeld || k == clutchKey;
		}
	}
	const float before = practice_.speed();
	const float speed = practice_.update(active, slowHeld, clutchHeld);
	hud_.setGameSpeed(speed);
	if (std::fabs(speed - before) < 1e-3f)
		return;
	constexpr auto kGameSpeedCvar = "sv_soccar_gamespeed";
	if (speed >= 0.999f)
		settings_->restoreCvar(kGameSpeedCvar);
	else if (!settings_->setCvar(kGameSpeedCvar, std::to_string(speed)))
		LOG("Free Touch: slow motion unavailable ({} not found)", kGameSpeedCvar);
	eventLog_.write("game_speed", {{"speed", statejson::round2(speed)}});
}

void FreeTouch::onUnload()
{
	unloading_ = true;
	if (game_)
	{
		game_->closeFilePicker(); // a Browse dialog still open
		game_->stopRequests();    // no account replies after this
	}
	{
		// Background work (an update's hashing or file moves) finishes before the plugin's code goes away.
		std::lock_guard lock(workersMutex_);
		for (auto& w : workers_)
			if (w.joinable())
				w.join();
		workers_.clear();
	}
	if (player_)
		player_->stop(); // shows a hidden ball again
	saveHistory();
	saveProgress();
	sound_.shutdown();
	if (settings_)
		settings_->restoreAll(); // Select gets its original bind back
	eventLog_.write("plugin_unload");
	eventLog_.close();
}

void FreeTouch::RenderWindow()
{
	// Render thread: read plugin-side state only (hard rule 2). The dashboard replaces this in its own task.
	if (bindingsScreen_)
		bindingsScreen_->render();
}

// Build 1.0.0.33 switched sv_freeplay_enablegoal / sv_soccar_enablegoal off during exercises. The two are linked, so
// restoring them one after the other could leave goal scoring off. Lambourne's config.cfg had both at 1: put that
// back once, and remember that it was done.
void FreeTouch::repairGoalCvarsOnce()
{
	const auto marker = game_->dataFolder() / "freetouch" / "repairs" / "goal_cvars_v33.done";
	std::error_code ec;
	if (std::filesystem::exists(marker, ec))
		return;
	nlohmann::ordered_json before = nlohmann::ordered_json::object();
	for (const char* name : {"sv_freeplay_enablegoal", "sv_soccar_enablegoal"})
	{
		if (CVarWrapper cvar = cvarManager->getCvar(name))
		{
			before[name] = cvar.getStringValue();
			cvar.setValue("1");
		}
	}
	std::filesystem::create_directories(marker.parent_path(), ec);
	std::ofstream(marker) << "done\n";
	eventLog_.write("repair_goal_cvars", {{"before", before}, {"after", "1"}});
	LOG("Free Touch: goal scoring cvars set back to 1 (one-time repair)");
}

void FreeTouch::logState()
{
	const Snapshot s = game_->snapshot();
	LOG("ft_state: tick {} ({:.1f}s) freeplay {} replay {} paused {}, {} car(s)",
		s.tick, s.time, s.inFreeplay, s.inReplay, s.paused, s.cars.size());
	if (s.ball)
	{
		LOG("  ball ({:.0f}, {:.0f}, {:.0f}) speed {:.0f}",
			s.ball->location.x, s.ball->location.y, s.ball->location.z, s.ball->velocity.length());
	}
	for (const auto& c : s.cars)
	{
		LOG("  car {} id {} team {}{}{} at ({:.0f}, {:.0f}, {:.0f}) yaw {} boost {:.0f} flip {} ground {} wall {}",
			c.name, c.id, c.team, c.isLocal ? " [you]" : "", c.isBot ? " [bot]" : "",
			c.location.x, c.location.y, c.location.z, c.rotation.yaw, c.boost, c.hasFlip, c.onGround, c.onWall);
	}
	eventLog_.write("state", statejson::toJson(s));
}

void FreeTouch::writeDump()
{
	const Snapshot s = game_->snapshot();
	statejson::json doc;
	doc["version"] = plugin_version;
	doc["time"] = localTime("%Y-%m-%d %H:%M:%S");
	const auto ps = player_->status();
	if (ps.active)
		doc["exercise"] = {{"id", ps.scenarioId}, {"rep", ps.rep}, {"seed", ps.seed}, {"difficulty", ps.difficulty}};
	doc["state"] = statejson::toJson(s);

	const auto dir = game_->dataFolder() / "freetouch" / "dumps";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const auto path = dir / (localTime("%Y%m%d-%H%M%S") + ".json");
	std::ofstream out(path, std::ios::trunc);
	out << doc.dump(2);
	if (!out)
	{
		LOG("ft_dump: could not write {}", path.string());
		return;
	}
	LOG("ft_dump: ball {} and {} car(s) -> {}", s.ball ? "yes" : "no", s.cars.size(), path.string());
	eventLog_.write("dump", {{"path", path.string()}, {"cars", s.cars.size()}, {"ball", s.ball.has_value()}});
}

void FreeTouch::captureMoment(const std::string& name)
{
	const Snapshot s = game_->snapshot();
	const std::string base = name.empty() ? localTime("%Y%m%d_%H%M%S") : name;

	// Never overwrite an earlier capture: add _2, _3, ... to the name.
	const auto dir = contentRoot() / "captured";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::string finalName = base;
	for (int n = 2; std::filesystem::exists(dir / (finalName + ".json")); ++n)
		finalName = base + "_" + std::to_string(n);

	auto doc = capture::scenarioFromSnapshot(s, finalName, localTime("%Y-%m-%d %H:%M:%S"));
	if (doc.is_null())
	{
		LOG("ft_capture: nothing to capture (no ball or no car of yours)");
		return;
	}
	// The seconds before: played back first, then the shot freezes until the player presses something.
	const leadin::Track lead = leadInRecorder_.track();
	if (lead.frames.size() >= 2)
	{
		doc["leadIn"] = leadin::toJson(lead);
		char buf[96];
		snprintf(buf, sizeof(buf), " Replays the %.1f s before it, then waits for you.", lead.seconds());
		doc["description"] = doc["description"].get<std::string>() + buf;
	}
	const auto path = dir / (finalName + ".json");
	{
		std::ofstream out(path, std::ios::trunc);
		out << doc.dump(2) << "\n";
		if (!out)
		{
			LOG("ft_capture: could not write {}", path.string());
			return;
		}
	}
	const std::string id = doc["id"].get<std::string>();
	LOG("ft_capture: saved {} -> {} with a {:.1f} s lead-in (play it with ft_play {})", id, path.string(),
		lead.seconds(), id);
	eventLog_.write("capture", {{"id", id}, {"path", path.string()}, {"ball", statejson::toJson(*s.ball)},
		{"player", statejson::toJson(*s.localCar())}});
	reloadContent(); // it shows up under Captured on the Exercises page right away
}

void FreeTouch::startRecording()
{
	if (!game_->inFreeplay())
	{
		LOG("ft_record: go to freeplay to record");
		return;
	}
	recordRec_.clear();
	take_ = leadin::Track{};
	takeView_.reset();
	takeStart_ = 0.f;
	recording_ = true;
	recordStartTick_ = tick_;
	eventLog_.write("record_start", {{"tick", tick_}});
	LOG("ft_record: recording (up to 15 s); ft_record again to stop");
	publishStatus();
}

void FreeTouch::stopRecording()
{
	if (!recording_)
		return;
	recording_ = false;
	take_ = recordRec_.track();
	recordRec_.clear();
	takeStart_ = 0.f;
	{
		// The page's view of it: 30 a second.
		auto view = std::make_shared<RecordTake>();
		const size_t every = std::max<size_t>(1, static_cast<size_t>(std::lround(take_.hz / 30.0)));
		view->hz = static_cast<float>(take_.hz / every);
		for (size_t i = 0; i < take_.frames.size(); i += every)
		{
			view->ball.push_back(take_.frames[i].ball.location);
			view->car.push_back(take_.frames[i].car.location);
			view->carYaw.push_back(take_.frames[i].car.rotation.yaw);
		}
		takeView_ = view;
	}
	eventLog_.write("record_stop", {{"seconds", take_.seconds()}, {"frames", take_.frames.size()}});
	LOG("ft_record: {:.1f} s recorded; pick where the drill starts on the Record page", take_.seconds());
	publishStatus();
}

void FreeTouch::saveRecording(float startSeconds, bool leadIn, const std::string& name)
{
	if (take_.frames.size() < 2)
		return;
	const float start = std::clamp(startSeconds, 0.f, static_cast<float>(take_.seconds()));
	const leadin::Frame f = leadin::sample(take_, start);
	// The moment as a snapshot: the ball and your car as they were there (the recording has no boost: a full tank).
	Snapshot s;
	s.ball = BallState{f.ball.location, f.ball.velocity, f.ball.angularVelocity};
	CarState me;
	me.isLocal = true;
	me.location = f.car.location;
	me.rotation = f.car.rotation;
	me.velocity = f.car.velocity;
	me.angularVelocity = f.car.angularVelocity;
	me.boost = 100.f;
	s.cars.push_back(me);
	s.localCarIndex = 0;

	const std::string shown = name.empty() ? localTime("%Y%m%d_%H%M%S") : name;
	const std::string base = "rec_" + capture::sanitizeId(shown);
	const auto dir = contentRoot() / "captured";
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	std::string finalName = base;
	for (int n = 2; std::filesystem::exists(dir / (finalName + ".json")); ++n)
		finalName = base + "_" + std::to_string(n);
	const std::string stamp = localTime("%Y-%m-%d %H:%M:%S");
	auto doc = capture::scenarioFromSnapshot(s, finalName, stamp);
	doc["name"] = "Recorded: " + shown;
	char buf[160];
	snprintf(buf, sizeof(buf), "Recorded in freeplay on %s; the drill starts %.1f s into the recording.", stamp.c_str(), start);
	std::string about = buf;
	if (leadIn && start >= 0.2f)
	{
		// The feed up to the start, replayed first; then it waits for you.
		leadin::Track lead;
		lead.hz = take_.hz;
		const size_t last = std::min(take_.frames.size() - 1, static_cast<size_t>(std::lround(start * take_.hz)));
		lead.frames.assign(take_.frames.begin(), take_.frames.begin() + last + 1);
		if (lead.frames.size() >= 2)
		{
			doc["leadIn"] = leadin::toJson(lead);
			snprintf(buf, sizeof(buf), " Replays the %.1f s before it, then waits for you.", lead.seconds());
			about += buf;
		}
	}
	doc["description"] = about;
	doc["source"] = {{"kind", "captured"}, {"label", "recorded " + stamp}};
	const auto path = dir / (finalName + ".json");
	{
		std::ofstream out(path, std::ios::trunc);
		out << doc.dump(2) << "\n";
		if (!out)
		{
			LOG("ft_record: could not write {}", path.string());
			return;
		}
	}
	eventLog_.write("record_saved", {{"id", doc["id"].get<std::string>()}, {"path", path.string()}, {"startSeconds", start},
		{"leadIn", doc.contains("leadIn")}, {"takeSeconds", take_.seconds()}});
	LOG("ft_record: saved {} ({:.1f} s in)", doc["id"].get<std::string>(), start);
	take_ = leadin::Track{};
	takeView_.reset();
	reloadContent(); // it shows up with the Saved Shots right away
	publishStatus();
}

void FreeTouch::drawRecording(Canvas& canvas)
{
	// The recorded ball's path and where the drill would start (the ball and your car there). Thick enough to read
	// from across the field (Lambourne, 2026-10-19): the part that replays bright, the part you play dim; the start a
	// ring round the ball with a beam down to the floor, your car an arrow with a post.
	if (recording_ || take_.frames.size() < 2)
		return;
	const size_t startIdx = std::min(take_.frames.size() - 1, static_cast<size_t>(std::lround(takeStart_ * take_.hz)));
	for (size_t i = 2; i < take_.frames.size(); i += 2)
	{
		const bool replays = i <= startIdx;
		canvas.line3d(take_.frames[i - 2].ball.location, take_.frames[i].ball.location,
			replays ? Rgba{255, 255, 255, 235} : Rgba{255, 255, 255, 110}, replays ? 12.f : 8.f, replays ? 4.f : 2.5f);
	}
	const leadin::Frame at = leadin::sample(take_, takeStart_);
	const Rgba mark{255, 200, 80, 245};
	const Vec3 b = at.ball.location;
	constexpr int kSeg = 28;
	for (int k = 0; k < kSeg; ++k)
	{
		const float a0 = 6.2831853f * k / kSeg, a1 = 6.2831853f * (k + 1) / kSeg;
		// Flat ring and an upright one, so it reads from any camera.
		canvas.line3d(b + Vec3(std::cos(a0) * 150.f, std::sin(a0) * 150.f, 0.f), b + Vec3(std::cos(a1) * 150.f, std::sin(a1) * 150.f, 0.f), mark, 10.f, 3.5f);
		canvas.line3d(b + Vec3(std::cos(a0) * 150.f, 0.f, std::sin(a0) * 150.f), b + Vec3(std::cos(a1) * 150.f, 0.f, std::sin(a1) * 150.f), mark, 10.f, 3.5f);
	}
	canvas.line3d(Vec3(b.x, b.y, 0.f), Vec3(b.x, b.y, std::max(0.f, b.z - 150.f)), Rgba{255, 200, 80, 170}, 10.f, 3.f);
	// Your car: a post and an arrow along its facing.
	const float yaw = at.car.rotation.yaw * 3.14159265f / 32768.f;
	const Vec3 c = at.car.location;
	const Vec3 fwd(std::cos(yaw), std::sin(yaw), 0.f), side(-std::sin(yaw), std::cos(yaw), 0.f);
	const Vec3 tip = c + fwd * 260.f + Vec3(0.f, 0.f, 30.f);
	canvas.line3d(c + Vec3(0.f, 0.f, 30.f), tip, mark, 12.f, 4.f);
	canvas.line3d(tip, tip - fwd * 90.f + side * 70.f, mark, 12.f, 4.f);
	canvas.line3d(tip, tip - fwd * 90.f - side * 70.f, mark, 12.f, 4.f);
	canvas.line3d(c, c + Vec3(0.f, 0.f, 260.f), mark, 10.f, 3.5f);
}

std::filesystem::path FreeTouch::contentRoot()
{
	const std::string configured = cvarManager->getCvar("ft_content_dir").getStringValue();
	if (!configured.empty())
		return std::filesystem::path(configured);
	return game_->dataFolder() / "freetouch" / "content";
}

void FreeTouch::reloadContent()
{
	const auto report = content_.load(contentRoot());
	policies_.clear();
	loadQuickPlay();
	if (!routineLoaded_)
		loadRoutine();
	registerRoutineSessions();
	loadRoutineTemplates();
	loadDaily();
	publishRoutine(); // the Warm-up page waits for this
	const int sounds = sound_.load(contentRoot() / "sounds");
	LOG("Sound: {} sound(s) loaded{}", sounds, sound_.ready() ? "" : " (audio unavailable: playing silently)");
	LOG("Content: {} file(s) from {}: {} scenarios, {} sessions, {} combo ladders, {} HORSE lists; {} error(s), {} warning(s)",
		report.filesChecked, report.root.string(), report.scenarios, report.sessions, report.combos, report.horse,
		report.errors.size(), report.warnings.size());
	for (const auto& e : report.errors)
		LOG("  ERROR {}", e);
	for (const auto& w : report.warnings)
		LOG("  warning {}", w);
	eventLog_.write("content_loaded", {
		{"root", report.root.string()},
		{"files", report.filesChecked},
		{"scenarios", report.scenarios},
		{"sessions", report.sessions},
		{"combos", report.combos},
		{"horse", report.horse},
		{"errors", report.errors},
		{"warnings", report.warnings},
	});
	publishDashboardModel(static_cast<int>(report.errors.size()), static_cast<int>(report.warnings.size()));
	scanInbox(); // which shared shots are already imported
}

std::filesystem::path FreeTouch::freeplayFile()
{
	return game_->dataFolder() / "freetouch" / "settings" / "freeplay_plus.json";
}

void FreeTouch::loadFreeplayPlus()
{
	freeplayEnabled_.clear();
	std::ifstream in(freeplayFile());
	if (in)
	{
		try
		{
			const auto doc = nlohmann::json::parse(in);
			for (const auto& id : doc.value("enabled", nlohmann::json::array()))
				freeplayEnabled_.push_back(id.get<std::string>());
			freeplayUnlimited_ = doc.value("boost", "unlimited") != "standard";
		}
		catch (const std::exception&)
		{
		}
	}
	player_->setChoiceFilter(kFreeplayId, freeplayEnabled_);
	player_->setBoostOverride(kFreeplayId, freeplayUnlimited_);
}

void FreeTouch::saveFreeplayPlus(const std::vector<std::string>& enabled, bool unlimitedBoost)
{
	freeplayEnabled_ = enabled;
	freeplayUnlimited_ = unlimitedBoost;
	player_->setChoiceFilter(kFreeplayId, freeplayEnabled_);
	player_->setBoostOverride(kFreeplayId, freeplayUnlimited_);
	std::error_code ec;
	std::filesystem::create_directories(freeplayFile().parent_path(), ec);
	safefile::write(freeplayFile(), nlohmann::json{{"enabled", enabled}, {"boost", unlimitedBoost ? "unlimited" : "standard"}}.dump(2));
	eventLog_.write("freeplay_plus_saved", {{"enabled", enabled}, {"unlimitedBoost", unlimitedBoost}});
	publishDashboardModel(0, 0);
}

std::filesystem::path FreeTouch::quickPlayFile()
{
	return game_->dataFolder() / "freetouch" / "settings" / "quick_play.json";
}

void FreeTouch::loadQuickPlay()
{
	const auto file = quickPlayFile();
	std::ifstream in(file);
	if (!in)
		return; // not saved yet: the default warm-up is used
	std::string error;
	try
	{
		if (content_.add(ContentLibrary::Kind::Session, nlohmann::json::parse(in), file, error))
			return;
	}
	catch (const std::exception& e)
	{
		error = e.what();
	}
	LOG("Quick Play: {} ignored ({})", file.string(), error);
	eventLog_.write("quick_play_invalid", {{"file", file.string()}, {"error", error}});
}


void FreeTouch::startQuickPlay()
{
	// Home's Warm-up: the warm-up section's first drill block not done today, else its first one.
	const routine::Section* warm = routine::warmupSection(routine_);
	const routine::DayLog& day = journal_[today_];
	const routine::Step* pick = nullptr;
	for (const auto& s : routine_.steps)
	{
		if (!((s.type == "drills" && !s.drills.empty()) || s.type == "freeplay") || (warm && s.section != warm->id))
			continue;
		if (!pick)
			pick = &s;
		if (!routine::isDone(s, day))
		{
			pick = &s;
			break;
		}
	}
	if (!pick)
	{
		LOG("Warm-up: no Free Touch drills in your warm-up yet (add them on the Routine page)");
		return;
	}
	startRoutineStep(pick->id);
}

void FreeTouch::startRoutineStep(const std::string& stepId)
{
	const auto it = std::find_if(routine_.steps.begin(), routine_.steps.end(),
		[&](const routine::Step& s) { return s.id == stepId && (s.type == "drills" || s.type == "freeplay"); });
	if (it == routine_.steps.end() || (it->type == "drills" && it->drills.empty()))
		return;
	SessionSettings settings;
	settings.sessionId = routine::sessionId(*it);
	settings.minutes = 120.0; // a drill block ends when its list is done
	if (it->type == "freeplay")
	{
		// Freeplay+ for the step's minutes, with its own objectives and boost for this run (put back when it ends).
		settings.minutes = routine::freeplayMinutes(*it);
		player_->setChoiceFilter(kFreeplayId, it->objectives.empty() ? freeplayEnabled_ : it->objectives);
		player_->setBoostOverride(kFreeplayId, it->boost.empty() ? freeplayUnlimited_ : it->boost == "unlimited");
		routineFreeplayOverride_ = true;
		eventLog_.write("routine_freeplay", {{"step", it->id}, {"minutes", settings.minutes}, {"objectives", it->objectives.size()},
			{"boost", it->boost.empty() ? "page" : it->boost}});
	}
	settings.difficulty = it->difficulty;
	settings.airRoll = it->airRoll;
	startSession(settings);
}

std::filesystem::path FreeTouch::routineFile()
{
	return game_->dataFolder() / "freetouch" / "settings" / "routine.json";
}

std::filesystem::path FreeTouch::journalFile()
{
	return game_->dataFolder() / "freetouch" / "settings" / "journal.json";
}

void FreeTouch::loadRoutine()
{
	// No file yet: the Routine page offers the templates to start from.
	routineLoaded_ = true;
	std::ifstream in(routineFile());
	if (in)
	{
		try
		{
			const auto doc = nlohmann::json::parse(in);
			routine_ = routine::fromJson(doc);
			if (doc.value("version", 1) < 2)
				saveRoutine(); // one list -> Warm-up / Play / After
		}
		catch (const std::exception&)
		{
			LOG("Routine: settings/routine.json could not be read; pick a template on the Routine page");
			routine_ = {};
		}
	}
	std::ifstream jin(journalFile());
	if (jin)
	{
		try
		{
			const auto doc = nlohmann::json::parse(jin);
			for (const auto& [day, log] : doc.value("days", nlohmann::json::object()).items())
				journal_[day] = routine::dayFromJson(log);
		}
		catch (const std::exception&)
		{
			LOG("Routine: settings/journal.json could not be read");
		}
	}
}

void FreeTouch::saveRoutine()
{
	std::error_code ec;
	std::filesystem::create_directories(routineFile().parent_path(), ec);
	safefile::write(routineFile(), routine::toJson(routine_).dump(2));
	eventLog_.write("routine_saved", {{"steps", routine_.steps.size()}});
	if (journal_.count(today_))
		saveJournal(); // today's snapshot follows the edit
}

void FreeTouch::saveJournal()
{
	// Today's log keeps the routine as it is now, so the Progress journal can show that day's routine later.
	if (const auto it = journal_.find(today_); it != journal_.end())
		it->second.plan = routine::planOf(routine_);
	nlohmann::json days = nlohmann::json::object();
	for (const auto& [day, log] : journal_)
	{
		auto j = routine::toJson(log);
		if (!j.empty())
			days[day] = std::move(j);
	}
	std::error_code ec;
	std::filesystem::create_directories(journalFile().parent_path(), ec);
	safefile::write(journalFile(), nlohmann::json{{"version", 1}, {"days", days}}.dump(1));
}

std::filesystem::path FreeTouch::dailyFile()
{
	return game_->dataFolder() / "freetouch" / "settings" / "daily.json";
}

void FreeTouch::loadDaily()
{
	// The pool comes with the content (reloaded with it); the attempts are read once (a run may be going on).
	daily::Pool pool;
	std::ifstream in(contentRoot() / "daily" / "daily.json");
	if (in)
	{
		try
		{
			pool = daily::Pool::fromJson(nlohmann::json::parse(in));
			for (auto& slot : pool.slots)
				slot.drills.erase(std::remove_if(slot.drills.begin(), slot.drills.end(),
					[this](const std::string& id) { return !content_.find(ContentLibrary::Kind::Scenario, id); }), slot.drills.end());
			pool.slots.erase(std::remove_if(pool.slots.begin(), pool.slots.end(), [](const daily::Slot& s) { return s.drills.empty(); }),
				pool.slots.end());
		}
		catch (const std::exception& e)
		{
			LOG("Daily: content/daily/daily.json could not be read ({})", e.what());
			pool = {};
		}
	}
	daily_->setPool(pool);
	static bool attemptsLoaded = false;
	if (!attemptsLoaded)
	{
		attemptsLoaded = true;
		std::ifstream din(dailyFile());
		if (din)
		{
			try
			{
				daily_->loadAttempts(nlohmann::json::parse(din));
			}
			catch (const std::exception& e)
			{
				// Kept as daily.json.broken (not overwritten by the next save); the service still locks a day already
				// posted (lockDailyFromService). Saving goes on into a fresh file.
				LOG("Daily: settings/daily.json could not be read ({}); kept as daily.json.broken", e.what());
				eventLog_.write("daily_file_broken", {{"error", e.what()}});
				din.close();
				safefile::setAside(dailyFile());
			}
		}
	}
	publishDaily();
}

void FreeTouch::saveDaily()
{
	std::error_code ec;
	std::filesystem::create_directories(dailyFile().parent_path(), ec);
	if (!safefile::write(dailyFile(), daily_->attemptsJson().dump(1)))
		LOG("Daily: settings/daily.json could not be saved");
}

void FreeTouch::publishDaily()
{
	const std::string today = today_.empty() ? localTime("%Y-%m-%d") : today_;
	auto v = std::make_shared<DailyView>();
	const daily::Challenge& c = daily_->challenge(today);
	v->date = today;
	v->number = c.number;
	v->available = c.number >= 1 && !c.shots.empty();
	v->running = daily_->active();
	v->finishedSeq = dailyFinishedSeq_;
	const auto& attempts = daily_->attempts();
	const auto it = attempts.find(today);
	daily::Attempt a;
	if (it != attempts.end())
		a = it->second;
	a.number = c.number;
	for (size_t i = 0; i < c.shots.size(); ++i)
	{
		DailyView::Shot s;
		s.slot = c.shots[i].slot;
		s.scenarioId = c.shots[i].scenarioId;
		const auto* item = content_.find(ContentLibrary::Kind::Scenario, s.scenarioId);
		s.name = item ? item->name : s.scenarioId;
		s.difficulty = c.shots[i].difficulty;
		s.seed = c.shots[i].seed;
		if (i < a.results.size())
		{
			s.mark = static_cast<int>(a.results[i].mark);
			s.points = a.results[i].points;
			s.label = a.results[i].label;
		}
		v->shots.push_back(std::move(s));
	}
	v->played = static_cast<int>(a.results.size());
	v->hits = a.hits();
	v->points = a.points();
	v->done = a.done;
	v->share = daily::shareText(a, static_cast<int>(c.shots.size()));
	v->streak = daily::streak(attempts, today);
	for (auto d = attempts.rbegin(); d != attempts.rend(); ++d)
	{
		if (d->second.done)
			v->bestPoints = std::max(v->bestPoints, d->second.points());
		if (d->first == today || d->first > today || v->history.size() >= 30)
			continue;
		DailyView::Day day;
		day.date = d->first;
		day.number = d->second.number;
		day.hits = d->second.hits();
		day.points = d->second.points();
		day.shots = static_cast<int>(c.shots.size());
		day.done = d->second.done;
		for (const auto& r : d->second.results)
			day.marks.push_back(static_cast<int>(r.mark));
		v->history.push_back(std::move(day));
	}
	dailyPub_ = v;
}

void FreeTouch::updateFreeplayBindings()
{
	// Rocket League's own freeplay controls (Epic's defaults, used when BakkesMod's "Enable bakkesmod freeplay
	// bindings" is off) put ball actions on the D-pad, LB and RB: ball in front, on the car, a redirect pass, pop it
	// up, a defend shot, a reset. Free Touch's drill buttons and menu use the same D-pad, so one press both moved to
	// the next shot and threw the ball (Lambourne 2026-10-29: his friend's setup). While a drill runs or the menu is
	// open, BakkesMod's freeplay bindings are on (they replace the game's); the player's own setting comes back after.
	const bool want = exerciseBindsOn_ || navBindsOn_;
	if (want == freeplayBindingsOn_)
		return;
	freeplayBindingsOn_ = want;
	if (want)
	{
		const bool ok = settings_->setCvar("sv_freeplay_bindings", "1");
		eventLog_.write("freeplay_bindings", {{"on", true}, {"ok", ok}});
	}
	else
	{
		settings_->restoreCvar("sv_freeplay_bindings");
		eventLog_.write("freeplay_bindings", {{"on", false}});
	}
}

void FreeTouch::checkDailyWithService(std::function<void()> then)
{
	const std::string today = localTime("%Y-%m-%d");
	if (!account_ || !account_->signedIn() || dailyCheckedDay_ == today)
	{
		if (then)
			then();
		return;
	}
	boards_->fetchMyDaily(today, static_cast<int64_t>(std::time(nullptr)),
		[this, today, then](bool reached, bool found, int number, int points, const std::string& marks) {
			if (reached)
				dailyCheckedDay_ = today;
			if (found && daily_->lockFromService(today, number, marks, points))
			{
				LOG("Daily: today's ({} points) is already on the service: done here too", points);
				notice("Today's daily is already done: " + daily::groupThousands(points) + " points");
				saveDaily();
				publishDaily();
				publishStatus();
				return; // nothing to start
			}
			if (then)
				then();
		});
}

void FreeTouch::startDaily()
{
	// Signed in: asked the service first, so a day already posted (from a lost file or another PC) can't be played
	// again (Lambourne 2026-10-29: "your daily score should be permanent for the day").
	checkDailyWithService([this] { startDailyNow(); });
}

void FreeTouch::startDailyNow()
{
	// One thing at a time: a session or exercise running now ends first.
	if (session_->active())
		session_->finish("stopped");
	startInFreeplay([this] {
		std::string error;
		if (!daily_->start(today_.empty() ? localTime("%Y-%m-%d") : today_, error))
		{
			LOG("Daily: not started ({})", error);
			eventLog_.write("daily_refused", {{"reason", error}});
		}
		publishDaily();
		publishStatus();
	}, "daily challenge");
}

void FreeTouch::loadRoutineTemplates()
{
	// content/routines/*.json: starting points for the Routine page, in their "order".
	routineTemplates_.clear();
	std::vector<std::pair<int, routine::Template>> found;
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(contentRoot() / "routines", ec))
	{
		if (entry.path().extension() != ".json")
			continue;
		try
		{
			std::ifstream in(entry.path());
			const auto doc = nlohmann::json::parse(in);
			routine::Template t;
			t.id = entry.path().stem().string();
			t.name = doc.value("name", t.id);
			t.description = doc.value("description", "");
			t.routine = routine::fromJson(doc);
			found.push_back({doc.value("order", 50), std::move(t)});
		}
		catch (const std::exception& e)
		{
			LOG("Routine: template {} skipped ({})", entry.path().filename().string(), e.what());
		}
	}
	std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	for (auto& [order, t] : found)
		routineTemplates_.push_back(std::move(t));
}

void FreeTouch::registerRoutineSessions()
{
	// Each drill block plays as its own session (content is reloaded from disk, so they are added again each time).
	for (const auto& s : routine_.steps)
	{
		if (!((s.type == "drills" && !s.drills.empty()) || s.type == "freeplay"))
			continue;
		std::string error;
		if (!content_.add(ContentLibrary::Kind::Session, routine::toSession(routine_, s), routineFile(), error))
			LOG("Warm-up: drill block '{}' not added ({})", s.title, error);
	}
}

void FreeTouch::publishRoutine()
{
	if (today_.empty())
		today_ = localTime("%Y-%m-%d");
	routinePub_ = std::make_shared<routine::Routine>(routine_);
	todayPub_ = std::make_shared<routine::DayLog>(journal_[today_]);
	publishStatus();
}

void FreeTouch::openEventLog()
{
	const auto folder = game_->dataFolder() / "freetouch" / "logs";
	if (!eventLog_.open(folder))
	{
		LOG("Free Touch: could not open an event log in {}", folder.string());
		return;
	}

	eventLog_.write("plugin_load", {
		{"version", plugin_version},
		{"inFreeplay", game_->inFreeplay()},
		{"inReplay", game_->inReplay()},
	});
	LOG("Event log: {}", eventLog_.path().string());
}
