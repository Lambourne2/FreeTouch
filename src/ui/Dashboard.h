#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include "IMGUI/imgui.h"
#include "ui/DashboardModel.h"

class BindingsScreen;

// The Free Touch window: left rail + pages (Home, Sessions & Labs, Exercises, Quick Play, Races, Replays, Stats,
// Settings). Render thread: it draws from DashboardModel / DashboardStatus copies and hands every action to the
// callbacks, which queue the real work on the game thread. Pure ImGui, no SDK calls.
class Dashboard
{
public:
	struct Callbacks
	{
		std::function<void(const std::string& exerciseId, const std::string& difficulty)> play;
		std::function<void()> stop;
		std::function<void()> reloadContent;
		std::function<void(const ImVec4& accent)> accentChanged;
		std::function<void(bool on)> goalExplosionChanged;
		std::function<void(bool on)> goalReplaysChanged;
		std::function<void(const std::string& command)> selectCommandChanged; // what the controller's Select runs
		std::function<void(bool on)> devToolsChanged;
		std::function<void(bool on)> mechanicsFeedChanged;
		std::function<void(const std::string& who)> defenderChanged; // psyonix | necto
		std::function<void(const std::string& who)> packDefenderChanged; // "" | psyonix | necto | nexto
		std::function<void(const std::string& target, const std::string& size, int tile)> packTargetChanged;
		std::function<void(const std::string& code)> importPack; // "" = the pack the player is in
		std::function<void(const std::string& id)> deletePack;
		std::function<void(const std::string& id)> favoriteToggled;
		std::function<void(const PracticeSpeedSettings& settings)> practiceChanged;
		std::function<void()> refreshControls;      // put every bind back and set Free Touch's again
		std::function<void()> dismissFirstRun;
		std::function<void(const std::string& id)> tutorialSeen; // shown once: not again by itself
		std::function<void()> resetTutorials;                    // Settings: show the tips again
		std::function<void(const std::string& id, const std::string& name)> renameShot;
		std::function<void(const std::string& id, bool favorite)> favoriteShot;
		std::function<void(const std::string& id)> deleteShot;
		// A saved shot's extras: a defender in the orange net ("" none) and its open tiles (bit per cell; 0 = whole net).
		std::function<void(const std::string& id, const shotshare::Extras& extras)> shotExtrasChanged;
		std::function<void(const std::string& id)> shareShot;         // a .ftshot file, put on the clipboard
		std::function<void(const std::string& path)> importSharedShot; // one from the inbox (Downloads)
		std::function<void()> scanInbox;                               // look in Downloads again
		std::function<void()> browseSharedShot;                        // Windows' Open dialog for a .ftshot
		// Account (Settings > Account): pick a name (signs in), rename, sign out, try again when offline.
		std::function<void(const std::string& name)> createAccount;
		std::function<void(const std::string& name)> renameAccount;
		std::function<void()> signOutAccount;
		std::function<void()> retryAccount;
		std::function<void()> makeRecoveryCode;
		// Friends page.
		std::function<void()> refreshFriends;
		std::function<void(const std::string& who)> addFriend; // friend code or name
		std::function<void(const std::string& id, bool accept)> respondFriend;
		std::function<void(const std::string& id)> removeFriend;
		std::function<void(bool on)> shareActivityChanged;
		// Shots online: share a saved shot (public, and / or to friends), add (and play) one from the service.
		std::function<void(const std::string& id, bool makePublic, const std::vector<std::string>& friendIds,
			const std::string& toNames)> shareShotOnline;
		// Adds a shot from the service (and plays it): with the setup picked before playing when applyExtras.
		std::function<void(const std::string& shotId, const std::string& delivery, bool play, bool applyExtras,
			const shotshare::Extras& extras, const std::string& difficulty)> addOnlineShot;
		std::function<void(const std::string& delivery)> dismissOnlineShot;
		std::function<void(const std::string& sort)> refreshShotBoard;
		std::function<void(const std::string& day, const std::string& scope)> refreshDailyBoard; // friends | all
		std::function<void()> refreshRaceLeaders;
		// Support (Settings > Support, Home): opens the Ko-fi page in the browser.
		std::function<void()> openSupport;
		// Updates (Settings > Updates).
		std::function<void()> checkUpdate;
		std::function<void()> installUpdate;
		std::function<void(bool on)> autoUpdateChanged;
		std::function<void(const std::string& race, const std::string& scope)> refreshRaceBoard;
		// Race someone's ghost from a board: the race's exercise id, its ghost key, their id and name.
		std::function<void(const std::string& exerciseId, const std::string& race, const std::string& player,
			const std::string& name)> raceGhost;
		std::function<void(const std::string& shotId, bool on)> likeShot;
		std::function<void(const std::string& shotId)> reportShot;
		std::function<void(const std::string& shotId, bool on)> setShotPublic;
		std::function<void(const std::string& shotId)> deleteOnlineShot;
		std::function<void(const std::string& code)> restoreAccount;
		// Record page: start / stop recording a feed, mark where the drill would start, save it, or throw it away.
		std::function<void()> startRecording;
		std::function<void()> stopRecording;
		std::function<void(float atSeconds)> previewRecording;
		std::function<void(float atSeconds, bool leadIn, const std::string& name)> saveRecording;
		std::function<void()> discardRecording;
		std::function<void(const std::string& id)> addShotToQuickPlay;
		std::function<void(const SessionSettings& settings)> startSession;
		std::function<void()> dismissSummary;
		std::function<void()> startQuickPlay; // the warm-up's next drill block
		std::function<void()> startDaily;     // start or continue today's Daily Challenge
		// Copies text to the clipboard with emoji intact (ImGui's own clipboard drops them); false if it failed.
		std::function<bool(const std::string& utf8)> copyText;
		// Practise one of the daily's shots again (from its seed; doesn't count).
		std::function<void(const std::string& id, const std::string& difficulty, uint32_t seed)> practiceDailyShot;
		std::function<void(const routine::Routine& r)> saveRoutine;
		std::function<void(const routine::DayLog& day)> updateDay;  // today's ticks, tallies, notes, focus
		std::function<void(const std::string& stepId)> startRoutineStep;
		std::function<void(bool unlimited)> freeplayBoostChanged;
		std::function<ImageInfo(int handle)> image; // render thread: a card image's texture (null until loaded)
		std::function<void(const std::vector<std::string>& enabled)> saveFreeplayPlus; // objectives switched on
		std::function<void(const char* name)> sound;
		// Start with a side chosen on the exercise's page (nullopt: the scenario's own coin flip) and, for exercises
		// with picked variants, that variant's id ("" = any).
		std::function<void(const std::string& id, const std::string& difficulty, std::optional<bool> mirror,
			const std::string& variant)> playWith;
		std::function<void(const std::string& id)> addToQuickPlay;
		std::function<void(const std::string& map)> freeplayMapChanged;                 // render thread: a menu sound (content/sounds)
		std::function<void(float menu, float training, bool preview)> volumesChanged; // preview: play a sample
		std::function<void(double minutes)> dailyGoalChanged;
		// Progress page: opened (read the MMR now), an MMR typed in for a playlist (1v1 / 2v2 / 3v3).
		std::function<void()> progressOpened;
		std::function<void(const std::string& playlist, float mmr)> addMmr;
		// Replays page: take over the current replay moment as this player (opponents as bots or not); delete a saved one.
		// others: 2 everyone as bots, 1 opponents only, 0 none; leadIn: watch the moments before it first.
		std::function<void(uint64_t playerId, int others, bool leadIn)> takeover;
		std::function<void(const std::string& id)> deleteReplayMoment;
	};

	Dashboard(Callbacks callbacks, BindingsScreen* bindings, std::string version)
		: cb_(std::move(callbacks)), bindings_(bindings), version_(std::move(version)) {}

	// Game thread: publish new data.
	void setModel(std::shared_ptr<const DashboardModel> model);
	void setStatus(const DashboardStatus& status);

	// Render thread. `open` is cleared when the user closes the window.
	void render(bool* open);
	// The preview tool (tools/ui_preview): open a page by name ("home", "exercises", "record", ...), or an exercise's
	// own page with `detailId`. Not used by the plugin.
	void previewPage(const std::string& page, const std::string& detailId = "");

	// Controller navigation (Free Touch binds the D-pad, A, B and Start to "ft_nav ..." while the window is open):
	// up / down / left / right / select / back / close. Any thread.
	void queueNav(const std::string& action);
	// Render thread, right after an interactive item: registers it for navigation; true when select activates it.
	bool navHook(int zone);
	// Render thread, right after an interactive item: hover tick, and on activation its sound and a press flash.
	enum class Feedback { Click, Confirm, Toggle, Silent }; // Silent: no press sound (e.g. a page change has its own)
	void itemFeedback(bool activated, Feedback fx);

private:
	enum class Page { Home, Daily, FreeplayPlus, Sessions, Exercises, QuickPlay, SavedShots, Record, Races, Packs, Replays, Stats, Friends, Board, Settings };

	void renderRail();
	// The side menu's seven places (Lambourne 2026-10-29: "so many tabs... overwhelming"): each holds one or more
	// pages, shown as tabs along the top of the page; the menu opens a place on the page last used there.
	enum class Hub { Home, Train, Compete, Create, Friends, Progress, Settings };
	struct HubTab
	{
		Page page;
		const char* label;
	};
	static Hub hubOf(Page page);
	static const char* hubKey(Hub hub); // home, train, compete, create, friends, progress, settings (tutorial ids)
	std::vector<HubTab> hubTabs(Hub hub) const;
	void drawHubTabs();     // the page's place: its tabs (when it has several) and the ? button
	// Tutorials: a popup of a few steps (a drawing, a title, a line or two), the first time a place opens and from
	// its ? button.
	struct TutorialOpen
	{
		bool requested = false;
		std::string id;
		int step = 0;
		double stepAt = 0.0;
	} tutorial_;
	std::set<std::string> tutorialsShown_; // shown this run (before the plugin's list catches up)
	void openTutorial(const std::string& id);
	void renderTutorial(const DashboardModel& m, const DashboardStatus& s);
	std::map<int, Page> hubLast_;
	bool dailyWaiting_ = false; // today's daily not played yet (a dot on Compete)
	int sharedWaiting_ = 0;     // shots shared with you not added yet (a count on Create)
	// Progress page (tabs) and the Replays page (replay takeover).
	void renderProgress(const DashboardModel& m, const DashboardStatus& s);
	void renderProgressHero(const ProgressView& p);
	void renderProgressOverview(const DashboardModel& m, const DashboardStatus& s, const ProgressView& p);
	void renderProgressRank(const ProgressView& p, bool compact);
	void renderProgressTraining(const DashboardModel& m, const ProgressView& p);
	void renderProgressMilestones(const ProgressView& p, bool compact);
	void renderProgressJournal(const ProgressView& p);
	void renderWeakSpots(const DashboardModel& m, const DashboardStatus& s, const ProgressView& p, float width);
	void milestoneRow(const ProgressView::Milestone& ms, float width);
	void drawMinuteBars(const ProgressView& p, int count, float width, float height);
	void renderReplays(const DashboardModel& m, const DashboardStatus& s);
	void renderRaces(const DashboardModel& m, const DashboardStatus& s);
	// Daily Challenge page, and its card on Home.
	void renderDaily(const DashboardModel& m, const DashboardStatus& s);
	void renderDailyCard(const DailyView& d);
	int seenDailySeq_ = -1;     // the last finished daily the page was opened for
	void renderPacks(const DashboardModel& m, const DashboardStatus& s);
	void packOptions(const DashboardStatus& s, float width); // defender, net target, tile size, the picked tile
	char packCode_[48] = {};
	std::string confirmPackDelete_;
	int progressTab_ = 0;
	float progressTabAnim_ = 0.f;
	double tabAt_ = 0.0;              // the Progress tab was entered (count-ups and growing charts)
	double progressOpenedAt_ = -1.0;
	int mmrPlaylist_ = 1;             // 0 1v1, 1 2v2, 2 3v3
	float mmrPlaylistAnim_ = 1.f;
	int mmrRange_ = 1;                // 0 30 days, 1 90 days, 2 all
	float mmrRangeAnim_ = 1.f;
	char mmrInput_[16] = {};
	std::string journalOpen_;
	uint64_t replayPick_ = 0;         // 0: the camera's player
	int replayOthers_ = 0;            // 0 everyone as bots, 1 opponents, 2 nobody
	float replayOthersAnim_ = 0.f;
	int replayStart_ = 0;             // 0 watch the lead-up, 1 3-2-1
	float replayStartAnim_ = 0.f;
	double chartAt_ = 0.0;            // the MMR chart's playlist / range changed: it draws itself again
	double journalOpenAt_ = 0.0;
	std::string replayDeleteAsk_;
	void renderHome(const DashboardModel& m, const DashboardStatus& s);
	void renderHero(const DashboardModel& m, const DashboardStatus& s);
	void renderToday(const DashboardModel& m, const DashboardStatus& s);
	void renderHomeTiles(const DashboardModel& m);
	void renderRecent(const DashboardModel& m, const DashboardStatus& s);
	void sound(const char* name);
	void renderSessions(const DashboardModel& m, const DashboardStatus& s);
	void renderSummary(const SessionSummary& summary);
	void renderExercises(const DashboardModel& m, const DashboardStatus& s);
	void renderExerciseDetail(const DashboardModel& m, const DashboardStatus& s, const ExerciseCard& card);
	int opponentPicker(const char* id, const DashboardStatus& s, float width); // Psyonix bot / Necto / Nexto
	void renderWarmup(const DashboardModel& m, const DashboardStatus& s);
	void renderRoutineTemplates(const DashboardModel& m, bool replacing);
	void renderFreeplayPlus(const DashboardModel& m);
	// Settings page (polish pass): section cards and rows of a name, one help line and the control on the right. A card's
	// background is drawn from its height last frame (the controls table inside uses columns, which rule out layering).
	void settingsCardBegin(const char* id, const char* title);
	void settingsCardEnd();
	void settingRow(const char* label, const char* help); // the left column; the control follows, then settingRowEnd
	void settingRowEnd();
	bool switchToggle(const char* id, bool* value);       // an on / off switch (as on the Freeplay+ cards)
	std::map<std::string, float> cardHeight_;
	std::map<std::string, std::pair<bool, double>> switchPending_; // switch id -> clicked value, until when it is held
	std::string cardId_;
	ImVec2 cardStart_{0.f, 0.f};
	float cardInner_ = 0.f, rowTop_ = 0.f, rowLeftBottom_ = 0.f, rowCtrlX_ = 0.f;
	bool firstRow_ = true;
	void renderSavedShots(const DashboardModel& m, const DashboardStatus& s);
	void renderShotList(const DashboardModel& m, const DashboardStatus& s); // the saved shots' cards (Saved Shots and Record pages)
	void renderShotEditor(const SavedShot& shot, const DashboardStatus& s, float width); // its name, defender and net
	void renderInbox(const DashboardStatus& s); // shots friends shared (in Downloads), to add
	// The orange net's tiles (2 rows of `cols`): click to open / close; `hole` picks one spot for the hole instead.
	uint32_t netTilePicker(uint32_t open, int cols, bool hole, float width);
	// Record page (Phase 5): record a feed, pick where the drill starts, save it.
	void renderRecord(const DashboardModel& m, const DashboardStatus& s);
	float recordStart_ = 0.f;
	bool recordLeadIn_ = true;
	char recordName_[64] = {};
	void renderFirstRun();
	void renderComingSoon(const char* title, const char* phase, const char* what);
	void renderSettings(const DashboardModel& m, const DashboardStatus& s);
	void renderStatusBar(const DashboardModel& m, const DashboardStatus& s);
	float footerHeight() const;
	void pageTitle(const char* title, const char* subtitle);
	// 0 nothing, 1 clicked (open its page), 2 double-clicked (start it).
	// 1 open its page, 2 start it (double-click), 3 its star was clicked.
	int exerciseCard(const ExerciseCard& card, float width, const ExerciseStatsView* stats, bool favorite);

	Callbacks cb_;
	BindingsScreen* bindings_;
	std::string version_;

	mutable std::mutex mutex_;
	std::shared_ptr<const DashboardModel> model_;
	DashboardStatus status_;

	Page page_ = Page::Home;
	bool devTools_ = false; // Settings > Developer tools (from the status)
	std::string category_;           // "" = all
	// Exercises page: search text, sort (0 recommended, 1 A-Z, 2 recent, 3 most played), the exercise whose page is
	// open, its side choice (0 random, 1 normal, 2 mirrored) and the option knobs' animation.
	char search_[96] = {};
	int sort_ = 0;
	bool groupByCategory_ = true;    // Exercises: sections per category (any sort) or one list
	std::string detailId_;
	int side_ = 0;
	float diffAnim_ = 1.f, sideAnim_ = 0.f, variantAnim_ = 0.f;
	std::map<std::string, int> variantPick_; // exercise id -> 0 any, else variants[i - 1]
	int difficulty_ = 1;             // 0 easy, 1 medium, 2 hard
	// Session setup panel (Sessions page)
	std::string selectedSession_;
	float sessionMinutes_ = 10.f;
	int sessionDifficulty_ = 1;
	int sessionAirRoll_ = 2;         // 0 left, 1 right, 2 free
	// Warm-up page: what is on screen (drafts of the routine and today's log), edit mode, a step waiting for its
	// Remove confirmation, and the add-drill search.
	routine::Routine routineDraft_;
	routine::DayLog dayDraft_;
	bool routineEditing_ = false;
	bool pickingTemplate_ = false;   // "Start from a template" while a routine exists
	double routineChangedAt_ = -10.0;
	double dayChangedAt_ = -10.0;
	bool dayTextActive_ = false;
	std::string confirmStep_;
	char drillSearch_[64] = {};
	// The drill block editor (its own page from a drills step's Edit drills): which step, the add-drills browser.
	std::string blockEditId_;
	char blockSearch_[64] = {};
	std::string blockCategory_;
	float blockOrderAnim_ = 0.f, blockDiffAnim_ = 0.f, blockRollAnim_ = 0.f;
	void renderBlockEditor(const DashboardModel& m, const DashboardStatus& s, routine::Routine& r, routine::Step& st,
		const std::function<void()>& commit);
	float blockMinutes(const DashboardModel& m, const routine::Step& st) const; // its estimated length
	// Edit mode of the Routine page: sections as cards of compact step rows (drag to move, Edit opens the step's page).
	void renderRoutineEditor(const DashboardModel& m, const DashboardStatus& s, routine::Routine& r, const std::function<void()>& commit);
	// One step's own page (packs, workshop maps, task, notes; drill blocks use renderBlockEditor).
	void renderStepEditor(const DashboardModel& m, const DashboardStatus& s, routine::Routine& r, routine::Step& st,
		const std::function<void()>& commit);
	std::string stepSummary(const DashboardModel& m, const routine::Step& st) const;
	std::string confirmSection_;
	float boostAnim_ = 0.f; // Freeplay+ boost switch
	float defenderAnim_ = 0.f;
	float packDefenderAnim_ = 0.f;
	float shotDefenderAnim_ = 0.f;
	float packTargetAnim_ = 0.f;
	float packSizeAnim_ = 0.f;
	void packTilePicker(const DashboardStatus& s, float width); // the net drawn as the player sees it: click a tile
	std::unordered_map<std::string, float> hoverAnim_; // tile id -> 0..1 hover animation
	bool closeRequested_ = false;

	// Motion: when the window opened, the page changed and Home was entered (ImGui time); the last pressed item.
	int lastFrame_ = -100;
	double openedAt_ = -10.0;
	double pageAt_ = -10.0;
	double homeAt_ = -10.0;
	Page shownPage_ = Page::Home;
	double flashAt_ = -10.0;
	std::string toast_;         // a short confirmation shown above the footer for a moment
	double toastAt_ = -10.0;
	void toast(const std::string& text) { toast_ = text; toastAt_ = ImGui::GetTime(); }
	int seenNoticeSeq_ = -1;
	// Settings > Account: the name being typed (filled from the account's name when it changes), a sign-out waiting
	// for its confirmation.
	char accountName_[32] = {};
	std::string accountNameFrom_;
	bool confirmSignOut_ = false;
	char recoveryInput_[40] = {};
	bool showRecovery_ = false;
	bool confirmNewCode_ = false;
	void renderAccountCard(const DashboardStatus& s);
	void renderSupportCard(const DashboardStatus& s);
	void renderSupportLine(const DashboardStatus& s); // Home's last line
	void renderUpdatesCard(const DashboardStatus& s);
	// A supporter's heart just after a name drawn at `at` with `shown` (its text) at font size `size`.
	void supporterHeart(ImDrawList* dl, const DashboardStatus& s, const std::string& name, ImVec2 at, const std::string& shown,
		float size);
	void renderFriends(const DashboardStatus& s);
	void renderBoard(const DashboardStatus& s);
	// Play on a saved / shared / Shot Board shot: its setup first (difficulty, defender, net), then it plays.
	struct PlaySetup
	{
		bool requested = false, open = false, online = false;
		std::string id, delivery, name;
		shotshare::Extras extras, start;
	} playSetup_;
	void openPlaySetup(bool online, const std::string& id, const std::string& delivery, const std::string& name,
		const shotshare::Extras& extras);
	float setupTargetAnim_ = 0.f, setupSizeAnim_ = 0.f;
	void renderPlaySetup(const DashboardStatus& s);
	float setupDiffAnim_ = 0.f;
	float dailyBoardAnim_ = 0.f;
	void renderDailyBoard(const DashboardStatus& s, const DailyView& d, float width);
	// A race's board (Races page): times for friends or everyone, any ghost to race.
	struct RaceBoardOpen
	{
		bool requested = false;
		std::string exerciseId, race, name;
	} raceBoard_;
	void renderRaceBoard(const DashboardStatus& s);
	float raceScopeAnim_ = 0.f;
	float boardSortAnim_ = 0.f;
	std::string confirmBoard_; // a Report / Delete waiting for its confirmation
	std::string shotsMessage_, shotsError_; // the last ones shown as a toast
	char friendInput_[40] = {};
	int friendRequests_ = 0; // the side menu's count on Friends
	bool supporterMe_ = false; // this player has the supporter heart (the side menu's Support row says thank you)
	std::string confirmRemoveFriend_;
	Page lastPage_ = Page::Home; // the Friends page asks for a fresh list when it opens
	ImVec2 flashMin_, flashMax_;
	unsigned int hoveredId_ = 0; // ImGuiID of the hovered item (a new one plays the hover tick)
	bool hoverSeen_ = false;
	// Settings sliders: what they show while held.
	bool sliderHeld_ = false;
	double sliderReleasedAt_ = -10.0;
	float menuDraft_ = 60.f, trainingDraft_ = 70.f, goalDraft_ = 20.f;

	void applyNav();
	// Saved Shots page: the shot whose editor (name, defender, net) is open and its name's text, and a delete waiting
	// for its confirmation.
	std::string editingShot_;
	char renameBuf_[96] = {};
	std::string confirmDelete_;
	std::mutex navMutex_;
	std::vector<std::string> navQueue_;
	bool navActive_ = false;   // showing a focus outline (until the mouse moves)
	int navZone_ = 0;          // 0 the rail, 1 the page
	int navIndex_[2] = {0, 0};
	int navCount_[2] = {0, 0}; // items in each zone last frame
	int navSeen_[2] = {0, 0};  // items registered so far this frame
	std::vector<ImVec4> navRects_[2];     // this frame's item rects (min x, min y, max x, max y), in registration order
	std::vector<ImVec4> navRectsPrev_[2]; // last frame's, which the D-pad moves between
	int navSpatial(int zone, const std::string& dir) const; // the item that way from the focused one, or -1
	bool navSelect_ = false;
	bool navScroll_ = false;
};
