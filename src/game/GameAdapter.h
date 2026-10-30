#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/Canvas.h"
#include "core/Online.h"
#include "flavor.h"
#include "game/GameState.h"

// The only door to the game. Everything outside src/game/ uses this interface; BakkesGameAdapter implements it
// with the BakkesMod SDK. All methods must be called on the game thread (use runOnGameThread from UI code).
class GameAdapter
{
public:
	using TickListener = std::function<void(const Snapshot&)>;
	using TouchListener = std::function<void(const TouchEvent&)>;
	// Game events that matter for exercises (goal, ball explode/reset, kickoff, players reset): (event name, tick).
	using GameEventListener = std::function<void(const std::string&, uint64_t)>;

	virtual ~GameAdapter() = default;

	// Installs the game hooks. Call once from onLoad.
	virtual void start() = 0;

	// Called once per physics tick (120 Hz) while the local car is simulated; stops while paused.
	virtual void addTickListener(TickListener listener) = 0;
	// Called once per ball touch (any car), on the tick of the touch.
	virtual void addTouchListener(TouchListener listener) = 0;
	virtual void addGameEventListener(GameEventListener listener) = 0;
	// Freeplay's own kickoff reset (a goal, or a "reset" input) has just finished: ball and cars are at kickoff.
	virtual void addRoundResetListener(std::function<void()> listener) = 0;
	// Called every frame with a canvas for 2D overlays (HUD). Game thread.
	virtual void addDrawListener(std::function<void(Canvas&)> listener) = 0;

	virtual Snapshot snapshot() = 0;
	virtual bool inFreeplay() = 0;
	virtual bool inReplay() = 0;

	// Freeplay only. Moves land on the next physics step.
	virtual bool setBall(const BallTarget& target) = 0;
	// Freeplay only. Fills the tank for the move (so pads on the way are not taken), then sets the requested boost
	// once the car has landed.
	virtual bool setLocalCar(const CarTarget& target) = 0;
	// Sets only the local car's boost (0..100), e.g. to keep the tank full.
	virtual bool setLocalBoost(float percent) = 0;
	// Ballless drills: shrinks the ball to 1% (it can no longer be touched and its floor ring all but vanishes; ball
	// cam still follows it). Always show it again afterwards.
	virtual bool setBallHidden(bool hidden) = 0;
	// Starts offline Free Play on `map` (e.g. "Stadium_P") from the main menu or an offline match. Refuses (false) in
	// an online match or when already in freeplay.
	virtual bool loadFreeplay(const std::string& map) = 0;
	// Moves the local car only (no boost changes): for replaying motion every tick.
	virtual bool poseLocalCar(const CarTarget& target) = 0;

	// Which of these BakkesMod key names (e.g. "XboxTypeS_A", "SpaceBar") are held right now.
	virtual std::vector<std::string> keysDown(const std::vector<std::string>& keyNames) = 0;

	// Plays a goal explosion at the ball for the net on `side` (+1 orange, -1 blue): the local player's own when
	// `creditPlayer`, else the default one. No score, slow motion or replay. Nearby cars may be demolished. The ball
	// is dead afterwards: call respawnBall() before using it again.
	virtual bool explodeBall(int side, bool creditPlayer) = 0;
	// The local car exists and is not waiting to respawn (after a demolition).
	virtual bool localCarReady() = 0;

	// Bots (freeplay). spawnBot adds a Psyonix bot car named `name` (it shows up in snapshots a tick later, on the
	// orange team). removeBots removes every bot whose name starts with `namePrefix`.
	virtual bool spawnBot(const std::string& name) = 0;
	// Gives a spawned bot this car body (product id, 23 = Octane) through BakkesMod's bot loadout.
	virtual bool setBotBody(uint64_t carId, int body) = 0;
	// Moves a spawned bot to team 0 (blue, the player's) or 1 (orange).
	virtual bool setBotTeam(uint64_t carId, int team) = 0;
	// Psyonix bot skill for this freeplay game: 0 rookie, 0.5 pro, 1 all-star. Returns the value read back.
	virtual float setBotSkill(float skill) = 0;
	virtual int removeBots(const std::string& namePrefix) = 0;
	// Sets any car's location, velocity, rotation and spin by its stable id (no boost handling).
	virtual bool setCarState(uint64_t carId, const CarTarget& target) = 0;
	// Overrides a bot's inputs on every physics tick (its AI is ignored) until cleared with nullopt.
	virtual void setBotInput(uint64_t carId, std::optional<CarInput> input) = 0;
	// Replaces the ball with a fresh one at centre (ResetBalls; not freeplay's full kickoff reset).
	virtual bool respawnBall() = 0;

	// Ranked playlists: 10 Duel (1v1), 11 Doubles (2v2), 13 Standard (3v3). Game thread.
	virtual MmrReading readMmr(int playlistId) = 0;
	// Called (game thread) when the game updates the player's MMR, e.g. after a ranked match. It does not always fire
	// (2026-10-18: once at start-up, never after the matches), so the plugin also re-reads the MMR now and then.
	virtual void addMmrListener(std::function<void()> listener) = 0;
	// In an online match (the plugin leaves the game alone there).
	virtual bool inOnlineGame() { return false; }
	// Dashboard fonts through BakkesMod's GUI manager: load `file` (relative to bakkesmod/data/fonts/) as `name` at
	// `size` pixels (at load), then the ImFont* once BakkesMod has built it (render thread; null until then).
	virtual void loadFont(const std::string& name, const std::string& file, int size) { (void)name; (void)file; (void)size; }
	virtual void* font(const std::string& name) { (void)name; return nullptr; }
	// The replay being watched (active false when not in a replay).
	virtual ReplayInfo replayInfo() = 0;
	// Every rendered frame while a replay is open (there are no physics ticks in a replay): the replay's ball and
	// cars, and where it is. Game thread.
	virtual void addReplayFrameListener(std::function<void(const Snapshot&, const ReplayInfo&)> listener) = 0;
	// Every rendered frame while a custom training pack is open (physics ticks stop while a shot waits frozen): its
	// ball and cars, and which shot it is. Snapshot::tick counts these frames. Game thread.
	virtual void addTrainingTickListener(std::function<void(const Snapshot&, const TrainingInfo&)> listener) { (void)listener; }
	// Custom training: move on to the pack's next shot (method 0 NextRound, 1 IncrementRound, as the game's own next
	// shot button may use either). False outside custom training.
	// On to the next shot: 0 IncrementRound + PlayerResetTraining (the game's reset-shot), 1 IncrementRound +
	// StartNewRound, 2 NextRound. Placing the shot the counter already shows: 10 PlayerResetTraining, 11 StartNewRound,
	// 12 ResetRound. Starting a shot that waits frozen: 20 StartRound, 21 bRoundActive, 22 unpause; as if the player
	// pressed something, held for a few frames: 23 throttle (the controller's input), 24 boost, 25 jump.
	virtual bool trainingNextRound(int method) { (void)method; return false; }
	// The player's own car gets this input for the next `ticks` ticks (custom training: a light tap starts a shot).
	virtual void setLocalInputOverride(const CarInput& input, int ticks) { (void)input; (void)ticks; }
	virtual bool inCustomTraining() { return false; }

	virtual void runOnGameThread(std::function<void()> fn) = 0;
	virtual void runAfter(float seconds, std::function<void()> fn) = 0;

	virtual std::filesystem::path dataFolder() = 0;

	// Puts UTF-8 text on the Windows clipboard, emoji included (ImGui's clipboard drops characters past U+FFFF).
	// Any thread. False if the clipboard was busy.
	virtual bool copyToClipboard(const std::string& utf8) = 0;

	// Puts a file on the Windows clipboard as a file (Explorer's Copy), so Ctrl+V in Discord attaches it. Any thread.
	virtual bool copyFileToClipboard(const std::filesystem::path& file) { (void)file; return false; }
	// Opens an https:// page in the player's browser (Settings > Support). False if it couldn't. Any thread.
	virtual bool openWebPage(const std::string& url) { (void)url; return false; }
	// The user's Downloads folder ("" if unknown): where a shot shared in Discord lands.
	virtual std::filesystem::path downloadsFolder() { return {}; }
	// Windows' Open dialog for a shared shot (.ftshot), starting in `startDir`, without blocking the caller. `done` gets
	// the chosen file ("" if cancelled) on the dialog's own thread; never after closeFilePicker. One at a time.
	virtual void pickFile(const std::filesystem::path& startDir, std::function<void(const std::filesystem::path&)> done)
	{
		(void)startDir;
		done({});
	}
	// Closes an open dialog (plugin unload) and waits for its thread.
	virtual void closeFilePicker() {}

	// One HTTPS request to Free Touch's service (Phase 8). `done` runs on a worker thread, never after
	// stopRequests; the caller hands the reply to the game thread. Status 0 = no reply.
	virtual void http(const online::HttpRequest& request, online::HttpDone done) { (void)request; done(0, ""); }
	// Unloading: no more replies; waits briefly for requests still out.
	virtual void stopRequests() {}
#if FT_SELF_UPDATE
	// Downloads a URL to a file (the updater; not in the BakkesPlugins build, src/flavor.h); `done` (HTTP status,
	// 0 = failed) on a worker thread, never after stopRequests.
	virtual void download(const std::string& url, const std::filesystem::path& to, std::function<void(int status)> done)
	{
		(void)url;
		(void)to;
		done(0);
	}
#endif

	// Dashboard images (PNG/JPG). loadImage (game thread) returns a handle, cached per file and modification time,
	// or -1 if the file does not exist. image (render thread) gives its texture, null until it has loaded.
	virtual int loadImage(const std::filesystem::path& file) = 0;
	virtual ImageInfo image(int handle) = 0;
};
