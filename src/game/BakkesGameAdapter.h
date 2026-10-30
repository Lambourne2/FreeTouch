#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "game/GameAdapter.h"

class GameWrapper;
class CarWrapper;
class ServerWrapper;
class ImageWrapper;

class BakkesGameAdapter : public GameAdapter
{
public:
	explicit BakkesGameAdapter(std::shared_ptr<GameWrapper> gameWrapper);
	~BakkesGameAdapter() override
	{
		closeFilePicker();
		stopRequests();
	}

	void start() override;
	void addTickListener(TickListener listener) override;
	void addTouchListener(TouchListener listener) override;
	void addGameEventListener(GameEventListener listener) override;
	void addRoundResetListener(std::function<void()> listener) override;
	void addDrawListener(std::function<void(Canvas&)> listener) override;

	Snapshot snapshot() override;
	bool inFreeplay() override;
	MmrReading readMmr(int playlistId) override;
	void addMmrListener(std::function<void()> listener) override;
	bool inOnlineGame() override;
	bool copyToClipboard(const std::string& utf8) override;
	bool copyFileToClipboard(const std::filesystem::path& file) override;
	std::filesystem::path downloadsFolder() override;
	void pickFile(const std::filesystem::path& startDir, std::function<void(const std::filesystem::path&)> done) override;
	void closeFilePicker() override;
	void http(const online::HttpRequest& request, online::HttpDone done) override;
	void stopRequests() override;
#if FT_SELF_UPDATE
	void download(const std::string& url, const std::filesystem::path& to, std::function<void(int status)> done) override;
#endif
	void loadFont(const std::string& name, const std::string& file, int size) override;
	void* font(const std::string& name) override;
	ReplayInfo replayInfo() override;
	void addReplayFrameListener(std::function<void(const Snapshot&, const ReplayInfo&)> listener) override;
	void addTrainingTickListener(std::function<void(const Snapshot&, const TrainingInfo&)> listener) override;
	bool trainingNextRound(int method) override;
	void setLocalInputOverride(const CarInput& input, int ticks) override
	{
		localOverride_ = input;
		localOverrideTicks_ = ticks;
	}
	bool inCustomTraining() override { return gw_->IsInCustomTraining(); }
	bool inReplay() override;

	bool setBall(const BallTarget& target) override;
	bool setLocalCar(const CarTarget& target) override;
	bool setLocalBoost(float percent) override;
	bool setBallHidden(bool hidden) override;
	bool poseLocalCar(const CarTarget& target) override;
	bool loadFreeplay(const std::string& map) override;
	std::vector<std::string> keysDown(const std::vector<std::string>& keyNames) override;
	bool explodeBall(int side, bool creditPlayer) override;
	bool localCarReady() override;
	bool spawnBot(const std::string& name) override;
	bool setBotBody(uint64_t carId, int body) override;
	bool setBotTeam(uint64_t carId, int team) override;
	float setBotSkill(float skill) override;
	int removeBots(const std::string& namePrefix) override;
	bool setCarState(uint64_t carId, const CarTarget& target) override;
	void setBotInput(uint64_t carId, std::optional<CarInput> input) override;
	bool respawnBall() override;

	void runOnGameThread(std::function<void()> fn) override;
	void runAfter(float seconds, std::function<void()> fn) override;

	std::filesystem::path dataFolder() override;

	int loadImage(const std::filesystem::path& file) override;
	ImageInfo image(int handle) override;

private:
	void onVehicleInput(CarWrapper& caller, void* params);
	std::array<uint64_t, 34> padTakenUntil_{}; // tick each boost pad comes back (0 = available)
	void onHitBall(CarWrapper& caller);
	ServerWrapper currentServer();  // freeplay server, or the replay server in a replay; may be null
	Snapshot snapshotOf(ServerWrapper server);
	void tickTraining();            // custom training: reads the pack and the shot, calls the training listeners

	std::shared_ptr<GameWrapper> gw_;
	// The Open dialog's thread (pickFile).
	std::thread picker_;
	std::atomic<bool> pickerBusy_{false};
	std::atomic<bool> pickerClosing_{false};
	std::atomic<unsigned long> pickerThreadId_{0};
	// Requests still out (http) and whether their replies are still wanted.
	std::shared_ptr<std::atomic<int>> requestsOut_ = std::make_shared<std::atomic<int>>(0);
	std::shared_ptr<std::atomic<bool>> requestsWanted_ = std::make_shared<std::atomic<bool>>(true);
	// Images are never freed while the plugin runs (the render thread may be drawing them); a changed file
	// gets a new handle.
	std::mutex imagesMutex_;
	std::vector<std::shared_ptr<ImageWrapper>> images_;
	std::map<std::string, int> imageHandles_; // path + modification time -> handle
	std::vector<TickListener> tickListeners_;
	std::vector<TouchListener> touchListeners_;
	std::vector<GameEventListener> gameEventListeners_;
	std::vector<std::function<void()>> roundResetListeners_;
	std::vector<std::function<void(Canvas&)>> drawListeners_;
	std::vector<std::function<void()>> mmrListeners_;
	std::shared_ptr<void> mmrToken_; // keeps the MMR notification registered (released on unload)
	std::vector<std::function<void(const Snapshot&, const ReplayInfo&)>> replayFrameListeners_;
	std::vector<std::function<void(const Snapshot&, const TrainingInfo&)>> trainingListeners_;
	TrainingInfo training_;          // the pack's details, re-read when the round changes
	uint64_t trainingFrames_ = 0;    // frames seen in custom training (its snapshots' tick)
	int pressMethod_ = 0;            // 23-25: a press on the player's controller, held for pressFrames_ more frames
	int pressFrames_ = 0;
	void applyPress();
	uint64_t tick_ = 0;
	std::unordered_map<uintptr_t, uint64_t> lastInputTick_; // car address -> tick of its last SetVehicleInput
	std::unordered_map<std::string, int> keyIndex_;
	CarInput localOverride_;
	int localOverrideTicks_ = 0;
	std::unordered_map<uint64_t, CarInput> botInputs_;     // car id -> input override         // key name -> FName index (looked up once)
	bool started_ = false;
};
