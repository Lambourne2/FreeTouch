#pragma once

#include "bakkesmod/plugin/bakkesmodplugin.h"
#include "flavor.h"
#if FT_SELF_UPDATE
#include "core/Updater.h"
#endif

#include <memory>

#include "data/EventLog.h"
#include "data/TouchLogger.h"
#include "detectors/MechanicDetector.h"
#include "data/ProgressLog.h"
#include "data/ReplayTakeover.h"
#include "data/PackRecorder.h"
#include "data/LeadIn.h"
#include "data/TrainingHistory.h"
#include "audio/SoundPlayer.h"
#include <atomic>
#include "content/ContentLibrary.h"
#include "scenario/ScenarioPlayer.h"
#include "data/ShotShare.h"
#include "core/SessionRunner.h"
#include "core/DailyRunner.h"
#include "bots/BotManager.h"
#include "game/SettingSwapper.h"
#include "game/GameAdapter.h"
#include "plugin/ExerciseControls.h"
#include "core/PracticeSpeed.h"
#include <optional>

#include "ui/BindingsScreen.h"
#include "ui/Dashboard.h"
#include "ui/Hud.h"
#include "ui/GuiBase.h"

#include "version.h"
constexpr auto plugin_version = stringify(VERSION_MAJOR) "." stringify(VERSION_MINOR) "." stringify(VERSION_PATCH) "." stringify(VERSION_BUILD);

class FreeTouch : public BakkesMod::Plugin::BakkesModPlugin, public PluginWindowBase
{
	void onLoad() override;
	void onUnload() override;

	void openEventLog();
	void logState();
	void writeDump();
	void captureMoment(const std::string& name);
	std::filesystem::path contentRoot();
	void reloadContent();
	void repairGoalCvarsOnce();
	void startExercise(const std::string& id, const std::string& difficulty, std::optional<uint32_t> seed,
		std::optional<bool> mirror = {});
	// Runs `start` now in freeplay; from the main menu (or an offline match) loads Free Play first and runs it once the
	// car is ready. Game thread.
	// Runs `start` now in freeplay, or loads Free Play first (on `map`, else the ft_freeplay_map arena).
	void startInFreeplay(std::function<void()> start, const std::string& what, const std::string& map = "");
	std::function<void()> pendingStart_;
	std::string pendingWhat_;
	std::chrono::steady_clock::time_point pendingSince_;
	std::chrono::steady_clock::time_point lastBackPress_{}; // ft_back's first tap (it needs a double tap)
	int pendingReadyTicks_ = 0;
	void tickPendingStart(const Snapshot& s);
	void publishStatus();
	void startSession(const SessionSettings& settings);
	void publishDashboardModel(int errors, int warnings);
	std::filesystem::path quickPlayFile();
	void loadQuickPlay();
	std::filesystem::path freeplayFile();
	void loadFreeplayPlus();
	void saveFreeplayPlus(const std::vector<std::string>& enabled, bool unlimitedBoost);
	bool freeplayUnlimited_ = true; // Freeplay+ boost: unlimited (default) or standard
	std::vector<std::string> freeplayEnabled_; // Freeplay+ objectives switched on (empty = all)
	void startQuickPlay(); // the warm-up's next drill block

	// Warm-up routine (settings/routine.json) and the day logs (settings/journal.json).
	routine::Routine routine_;
	std::map<std::string, routine::DayLog> journal_;
	bool routineLoaded_ = false;
	std::shared_ptr<const routine::Routine> routinePub_;
	std::shared_ptr<const routine::DayLog> todayPub_;
	std::filesystem::path routineFile();
	std::filesystem::path journalFile();
	void loadRoutine();
	void saveRoutine();
	void saveJournal();
	void registerRoutineSessions();
	std::vector<routine::Template> routineTemplates_; // content/routines/*.json
	void loadRoutineTemplates();
	void publishRoutine();
	void startRoutineStep(const std::string& stepId);
	std::filesystem::path uiSettingsFile();
	void loadUiSettings();
	void saveAccent(const ImVec4& accent);

	EventLog eventLog_;
	std::unique_ptr<GameAdapter> game_;
	TouchLogger touchLogger_{eventLog_};
	leadin::Recorder leadInRecorder_{4.0}; // the last 4 s, saved with ft_capture as the shot's lead-in
	// Record page (Phase 5): record a feed in freeplay (up to 15 s), pick where the drill starts, save it with the
	// Saved Shots (the recording up to that point replays first as its lead-in). Game thread.
	leadin::Recorder recordRec_{15.0};
	bool recording_ = false;
	uint64_t recordStartTick_ = 0;
	leadin::Track take_;
	float takeStart_ = 0.f;   // where the drill would start (the page's slider), marked on the field
	std::shared_ptr<const RecordTake> takeView_; // the recording for the page's map and timeline
	void startRecording();
	void stopRecording();
	void saveRecording(float startSeconds, bool leadIn, const std::string& name);
	void drawRecording(Canvas& canvas);
	bool firstTickLogged_ = false;
	ContentLibrary content_;
	BestTimes bests_;
	std::unique_ptr<ScenarioPlayer> player_;
	std::unique_ptr<SessionRunner> session_;
	// Daily Challenge: today's ten shots for every player (content/daily/daily.json), attempts in settings/daily.json.
	std::unique_ptr<DailyRunner> daily_;
	std::shared_ptr<const DailyView> dailyPub_;
	int dailyFinishedSeq_ = 0;
	std::filesystem::path dailyFile();
	void loadDaily();          // the pool (content) and the attempts (settings)
	void saveDaily();
	void publishDaily();       // the Daily page's copy
	void startDaily();
	std::unique_ptr<BotManager> bots_;
	std::unique_ptr<SettingSwapper> settings_;
	std::unique_ptr<ExerciseControls> controls_;
	std::unique_ptr<BindingsScreen> bindingsScreen_;
	std::unique_ptr<Dashboard> dashboard_;
	Hud hud_;
	std::string lastExerciseId_;
	bool goalExplosion_ = true; // settings/ui.json "goalExplosion"
	bool goalReplays_ = false;  // settings/ui.json "goalReplays": goals go in for real, with the game's replay
	PracticeSpeed practice_;    // settings/ui.json "practice"
	void updatePracticeSpeed();
	void updateMenuNavBinds();
	void refreshControls();
	bool firstRunDone_ = false;  // settings/ui.json "firstRunDone"
	std::set<std::string> tutorialsSeen_; // settings/ui.json "tutorialsSeen": the tips already shown
	bool uiLoaded_ = false;
	MechanicDetector detector_;
	bool mechanicsFeed_ = true;  // settings/ui.json "mechanicsFeed": the on-screen feed of detected mechanics
	// The feed is a developer tool until the detectors are tuned (detection and its log run either way).
	bool feedVisible() const { return mechanicsFeed_ && devTools_; }
	void onMechanics();
	bool devTools_ = false;      // settings/ui.json "devTools": Saved Shots, Captured and Dev categories
	std::string defender_ = "psyonix"; // settings/ui.json "defender": psyonix | necto
	std::string packDefender_;         // settings/ui.json "packDefender": "" (none) | psyonix | necto | nexto
	std::string packTarget_;           // settings/ui.json "packTarget": "" (whole net) | pick | tile (random) | moving
	std::string packTileSize_ = "medium"; // "packTileSize": easy | medium | hard | impossible
	int packTile_ = 0;                 // "packTile": the picked tile (game cell index)
	std::set<std::string> favorites_;  // settings/ui.json "favorites": starred exercise ids
	bool routineFreeplayOverride_ = false; // a routine Freeplay+ step set its own objectives / boost for its run
	std::shared_ptr<const std::set<std::string>> favoritesPub_ = std::make_shared<std::set<std::string>>();
	// Network bots (content/bots/<id>/), loaded on first use.
	struct LoadedPolicy
	{
		bool tried = false;
		std::shared_ptr<const EarlPolicy> policy;
		PolicyBotConfig config;
	};
	std::map<std::string, LoadedPolicy> policies_;
	std::shared_ptr<PolicyBot> makeBrain(const std::string& id);
	bool brainAvailable(const std::string& id);
	// Saved Shots: change a captured scenario's file (rename / favourite), delete it, or add it to Quick Play.
	bool editShot(const std::string& id, const std::function<void(nlohmann::json&)>& change);
	void deleteShot(const std::string& id);
	void addToWarmup(const std::string& id);
	// Sharing: a saved shot as a .ftshot file in freetouch/shared/, put on the clipboard (Ctrl+V in Discord); shared
	// shots found in Downloads (or freetouch/inbox/) listed on the Record page, and imported as Saved Shots.
	bool shareShot(const std::string& id);
	void scanInbox();
	bool importShared(const std::filesystem::path& file);
	std::string importSharedText(const std::string& text, const std::string& from, const std::string& onlineId);
	void shareShotOnline(const std::string& id, bool makePublic, const std::vector<std::string>& friendIds, const std::string& toNames);
	void addOnlineShot(const std::string& shotId, const std::string& delivery, bool play,
		std::optional<shotshare::Extras> setup, const std::string& difficulty);
	std::shared_ptr<const std::set<std::string>> haveKeys_; // fingerprints of the saved shots (shared ones count as here)
	std::shared_ptr<const std::vector<SharedShotFile>> inbox_;
	std::string notice_; // the Dashboard shows it as a toast
	// Phase 8: the Free Touch account (settings/account.json), started once not in an online match.
	std::unique_ptr<Account> account_;
	std::unique_ptr<Friends> friends_;
	std::unique_ptr<OnlineShots> shots_;
	std::unique_ptr<Leaderboards> boards_;
	bool ghostsUploaded_ = false; // the race bests from before Phase 8, sent once a session
	void postDailyScore(const daily::Attempt& a); // a finished daily, if signed in
	// Signed in: today's daily already on the service is done here too (a lost file, another PC). Then `then`.
	void checkDailyWithService(std::function<void()> then);
	std::string dailyCheckedDay_; // the day already checked with the service
	void startDailyNow();
	// BakkesMod's freeplay bindings on while a drill runs or the menu is open (the game's own freeplay D-pad ball
	// actions off), the player's setting back after.
	void updateFreeplayBindings();
	bool exerciseBindsOn_ = false;
	bool freeplayBindingsOn_ = false;
	// Online upkeep (game thread): sign-in changes, the activity friends see (every 5 min, or a minute after training),
	// the friends list while the menu is open (every 2 min).
	void tickOnline();
	void pollOnline(); // every 15 s (menus too: game ticks only run in a match or freeplay)
	Friends::Activity activityNow();
	bool onlineSignedIn_ = false;
	int64_t activityAt_ = 0; // wall-clock seconds; 0 = send on the next check
	int activityVersion_ = -1;
	int64_t friendsAt_ = 0;
	bool accountStarted_ = false;
	void startAccount();
	// Auto updates (src/core/Updater): checked a little after start and every few hours; installed by itself unless
	// switched off (settings/ui.json "autoUpdate"), never while content is pointed at the repo (ft_content_dir). Not in
	// the BakkesPlugins build (src/flavor.h), which BakkesMod updates.
	bool autoUpdate_ = true;
#if FT_SELF_UPDATE
	std::unique_ptr<Updater> updater_;
	bool updateNoticed_ = false;
	int64_t updateCheckAt_ = 0; // wall-clock seconds of the next check (0 = soon after start)
	void startUpdater();
#endif
	// Work off the game thread (the updater's hashing and file moves), joined before unload.
	void runInBackground(std::function<void()> work, std::function<void()> then);
	std::mutex workersMutex_;
	std::vector<std::thread> workers_;
	// The release script's sign-in (tools/release.py) while the game is open: settings/publish_token.json, for
	// publishers only (never logged).
#if FT_SELF_UPDATE
	int isPublisher_ = -1; // -1 not asked yet, 0 no, 1 yes
	std::string publishTokenWritten_;
	void sharePublishToken(int64_t now);
#endif
	void loadAccountOnce();
	void saveAccount();
	int noticeSeq_ = 0;
	void notice(const std::string& text);
	bool navBindsOn_ = false;
	bool unloading_ = false;
	void saveUiSettings();

	// Sound: menu sounds from the dashboard (render thread), training cues from the player's status (game thread).
	SoundPlayer sound_;
	std::atomic<float> menuVolume_{0.6f};     // settings/ui.json "sound"
	std::atomic<float> trainingVolume_{0.7f};
	std::atomic<long long> lastConfirmMs_{0}; // a start sound just played: the window's close sound stays quiet
	void playMenuSound(const char* name);
	void playTrainingSound(const char* name, float pitch = 1.f, float gain = 1.f);
	void trainingCues(const ScenarioPlayer::Status& s);
	int cueRep_ = 0;
	int cueObjects_ = 0;
	int cuePins_ = 0;
	std::string cueObjective_;
	std::string cueProgress_;
	int cueCountdown_ = 0;
	uint64_t lastResultCueTick_ = 0;
	uint64_t tick_ = 0;

	// Training history (settings/history.json) for Home: today, the streak, recent, Try next.
	TrainingHistory history_;
	double dailyGoalMinutes_ = 20.0; // settings/ui.json "dailyGoalMinutes"
	std::filesystem::path historyFile();
	void loadHistory();
	void saveHistory();
	std::string today_;               // local date, refreshed every second
	HomeStats home_;                  // rebuilt when the history changes, else every 2 s
	std::shared_ptr<const std::map<std::string, ExerciseStatsView>> exerciseStats_;
	int homeVersion_ = -1;
	uint64_t homeBuiltTick_ = 0;
	std::vector<std::pair<std::string, std::vector<std::string>>> suggestPool_; // category -> playable ids
	void refreshHome(bool force);

	// Progress page (FreeTouchProgress.cpp): MMR, counters and milestones (settings/progress.json), the page's view.
	ProgressLog progress_;
	std::filesystem::path progressFile();
	void loadProgress();
	void saveProgress();
	void loadFonts(); // the dashboard's Barlow fonts, through BakkesMod's GUI manager
	// quiet: write the log, the file and the page only when a number changed (the 30 s re-read).
	void readMmrNow(const char* why, bool quiet = false);
	void pollMmr();
	nlohmann::json lastMmrRead_;
	void refreshProgress(bool force);
	void checkMilestones(const ProgressView& view);
	void countResult(const ScenarioPlayer::Status& s);
	std::shared_ptr<const ProgressView> progressPub_;
	std::pair<int, int> progressBuiltFor_{-1, -1}; // history / progress versions the view was built from
	uint64_t progressBuiltTick_ = 0;
	bool milestonesPrimed_ = false;  // the first check after loading only records what was already reached
	std::map<std::string, bool> mmrSynced_;

	// Replay takeover (FreeTouchProgress.cpp).
	takeover::Recorder replayRec_;
	ReplayInfo replayInfo_;
	std::shared_ptr<const ReplayView> replayPub_;
	std::string takeoverMessage_;
	bool replayBindOn_ = false;
	int replayFrames_ = 0;
	void onReplayFrame(const Snapshot& s, const ReplayInfo& info);
	void publishReplay();
	void leaveReplay();
	// others: 2 everyone as bots (teammates on your side), 1 opponents only, 0 none; leadIn: replay the moments before.
	void takeoverNow(uint64_t playerId, int others, bool leadIn);
	void deleteReplayMoment(const std::string& id);
	void drawReplayHint(Canvas& canvas, const Snapshot& s);

	// ---- Training packs (FreeTouchPacks.cpp): shots recorded while a pack is played in custom training ----
	packs::Recorder packRecorder_;
	std::string packRoundLogged_;
	std::string packChoicesKey_;                      // the pack whose shots packChoices_ holds
	std::map<int, nlohmann::ordered_json> packChoices_; // its shots by number: the saved file's, plus new ones // "code|round" last written to the log (one line per shot seen)
	bool packsDirty_ = false; // a pack file changed: content reloads at the next chance (freeplay tick, menu open)
	void onTrainingTick(const Snapshot& s, const TrainingInfo& t);
	void reloadPacksIfDirty();
	void savePackChoices(const TrainingInfo& t);
	// Importing a whole pack: the game opens it by its code (BakkesMod's load_training: the game fetches it, as when
	// a code is typed in its own menu), then every shot is read frozen, one after another, and saved. Then Free Play
	// loads and the pack starts.
	struct PackImport
	{
		bool on = false;
		bool started = false;     // the pack is open and being read
		std::string code;         // "" = the pack the player is in
		std::chrono::steady_clock::time_point since;
		// Each shot is filed under the game's own shot number. 2: read from its launch; 1: a weaker reading (a still
		// ball, or read late), which a later pass may replace.
		std::map<int, int> quality; // round (0-based) -> 1 or 2
		// Wait: for the shot the counter shows to be placed (a teleport of the ball): placed flying is read on that
		// frame; placed frozen goes to Launch (started from code, else one tap) and is read on its first moving frame.
		// Then on to the next shot while this one is live, so the next is placed flying. Round and round until every
		// shot is read.
		enum class Phase { Wait, Launch } phase = Phase::Wait;
		packs::Shot shot;         // the frozen shot being started
		uint64_t phaseTick = 0;
		int round = -1;           // the shot the counter showed last frame
		uint64_t roundTick = 0;   // when it changed
		int steps = 0;            // next-shot calls made in this import
		bool stepping = false;    // a step was asked and nothing new is placed yet
		int stepFrom = -1;
		uint64_t stepTick = 0;
		int method = 0;           // which next-shot call works here
		int startMethod = 23;     // which start call works here (remembered across packs)
		int tries = 0;
		bool askNext = false;     // no call moved it on: the player presses the game's next-shot button
		bool askTap = false;      // no call started it: the player taps throttle once
		bool haveLast = false;    // the last frame's ball (to tell a placed ball from one flying on)
		Vec3 lastBall, lastVel;
		std::chrono::steady_clock::time_point lastFrame;
		float frameSeconds = 1.f / 120.f;
		std::string message;      // shown on the Packs page
	} packImport_;
	uint64_t trainingTicks_ = 0;
	TrainingInfo trainingNow_;    // the pack being played (inTraining on the Packs page)
	void startPackImport(const std::string& code);
	void tickPackImport(const Snapshot& s, const TrainingInfo& t);
	void finishPackImport(const TrainingInfo& t, const std::string& why);
	void deletePack(const std::string& id);

public:
	void Render() override;
	void OnOpen() override;
	void OnClose() override;
	void RenderWindow() override;
};
