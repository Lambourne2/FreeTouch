#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "core/Controls.h"

class SettingSwapper;
class EventLog;
class GameAdapter;

// Puts the exercise binds (retry/next/previous/back) on their keys while an exercise runs and gives the keys back
// afterwards, and runs the "press a button" capture for the bindings screen. Game thread, except the methods
// marked as safe from the render thread.
class ExerciseControls
{
public:
	ExerciseControls(GameAdapter& game, SettingSwapper& settings, EventLog& log, std::filesystem::path settingsFile);

	void load();
	Controls& controls() { return controls_; }
	// Sets the exercise binds again (after another swap on the same keys, e.g. the menu's controller navigation).
	void reapply();

	void setActive(bool active);                       // exercise started / stopped
	bool active() const { return active_; }            // safe from the render thread

	void startCapture(const std::string& actionId);
	void cancelCapture(const std::string& reason);
	void resetToDefault(const std::string& actionId);

private:
	void poll(int generation);
	bool assign(const std::string& actionId, const std::string& key, std::string& message);

	GameAdapter& game_;
	SettingSwapper& settings_;
	EventLog& log_;
	std::filesystem::path settingsFile_;
	Controls controls_;
	std::atomic<bool> active_{false};

	int captureGeneration_ = 0;
	int pollsLeft_ = 0;
	std::vector<std::string> heldAtStart_;
};
