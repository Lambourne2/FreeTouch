#include "pch.h"
#include "plugin/ExerciseControls.h"

#include <algorithm>

#include "data/EventLog.h"
#include "game/SettingSwapper.h"
#include "game/GameAdapter.h"

namespace
{
	constexpr float kPollSeconds = 0.02f;
	constexpr int kPollLimit = 500;                          // 10 s
	constexpr auto kQuickTestKey = "XboxTypeS_Back";         // Select stays the quick-test button
	constexpr auto kCancelKey = "Escape";
	std::string labelOf(const std::string& actionId)
	{
		for (const auto& a : Controls::actions())
		{
			if (a.id == actionId)
				return a.label;
		}
		return actionId;
	}

	std::string commandOf(const std::string& actionId)
	{
		for (const auto& a : Controls::actions())
		{
			if (a.id == actionId)
				return a.command;
		}
		return "";
	}
}

ExerciseControls::ExerciseControls(GameAdapter& game, SettingSwapper& settings, EventLog& log, std::filesystem::path settingsFile)
	: game_(game), settings_(settings), log_(log), settingsFile_(std::move(settingsFile))
{
}

void ExerciseControls::load()
{
	const bool fromFile = controls_.load(settingsFile_);
	nlohmann::ordered_json keys = nlohmann::ordered_json::object();
	for (const auto& [action, key] : controls_.bindings())
		keys[action] = key;
	log_.write("bindings_loaded", {{"fromFile", fromFile}, {"keys", keys}});
}

void ExerciseControls::setActive(bool active)
{
	if (active == active_)
		return;
	for (const auto& a : Controls::actions())
	{
		const std::string key = controls_.keyFor(a.id);
		if (key.empty())
			continue;
		if (active)
			settings_.setBind(key, a.command);
		else
			settings_.restoreBind(key);
	}
	active_ = active;
	log_.write("exercise_binds", {{"active", active}});
}

void ExerciseControls::reapply()
{
	if (!active_)
		return;
	for (const auto& a : Controls::actions())
	{
		const std::string key = controls_.keyFor(a.id);
		if (!key.empty())
			settings_.setBind(key, a.command);
	}
	log_.write("exercise_binds", {{"active", true}, {"reapplied", true}});
}

bool ExerciseControls::assign(const std::string& actionId, const std::string& key, std::string& message)
{
	if (key == kQuickTestKey)
	{
		message = "Select is the quick-test button; pick another one.";
		return false;
	}
	if (const std::string other = controls_.actionUsing(key, actionId); !other.empty())
	{
		message = Controls::displayName(key) + " is already used by \"" + labelOf(other) + "\".";
		return false;
	}
	const std::string oldKey = controls_.keyFor(actionId);
	if (active_)
	{
		if (!oldKey.empty())
			settings_.restoreBind(oldKey);
		settings_.setBind(key, commandOf(actionId));
	}
	controls_.setKey(actionId, key);
	controls_.save(settingsFile_);
	message = labelOf(actionId) + " is now " + Controls::displayName(key) + ".";
	log_.write("binding_changed", {{"action", actionId}, {"from", oldKey}, {"to", key}});
	return true;
}

void ExerciseControls::startCapture(const std::string& actionId)
{
	if (controls_.capture().capturing)
		return;
	// Keys already held (e.g. the one that clicked "Rebind" via a keyboard shortcut) do not count until released.
	heldAtStart_ = game_.keysDown(Controls::capturableKeys());
	pollsLeft_ = kPollLimit;
	const int generation = ++captureGeneration_;
	controls_.setCapture({true, actionId, ""});
	log_.write("binding_capture_start", {{"action", actionId}});
	game_.runAfter(kPollSeconds, [this, generation] { poll(generation); });
}

void ExerciseControls::cancelCapture(const std::string& reason)
{
	const auto state = controls_.capture();
	if (!state.capturing)
		return;
	++captureGeneration_; // stops the poll loop
	controls_.setCapture({false, "", reason});
	log_.write("binding_capture_end", {{"action", state.actionId}, {"result", reason}});
}

void ExerciseControls::poll(int generation)
{
	const auto state = controls_.capture();
	if (generation != captureGeneration_ || !state.capturing)
		return;

	if (!game_.keysDown({kCancelKey}).empty())
	{
		cancelCapture("Cancelled.");
		return;
	}

	const auto down = game_.keysDown(Controls::capturableKeys());
	// A key held at the start counts once it has been released and pressed again.
	std::erase_if(heldAtStart_, [&](const std::string& k) { return std::find(down.begin(), down.end(), k) == down.end(); });
	for (const auto& key : down)
	{
		if (std::find(heldAtStart_.begin(), heldAtStart_.end(), key) != heldAtStart_.end())
			continue;
		std::string message;
		const bool ok = assign(state.actionId, key, message);
		++captureGeneration_;
		controls_.setCapture({false, "", message});
		log_.write("binding_capture_end", {{"action", state.actionId}, {"key", key}, {"result", message}, {"ok", ok}});
		return;
	}

	if (--pollsLeft_ <= 0)
	{
		cancelCapture("No button pressed in 10 s; nothing changed.");
		return;
	}
	game_.runAfter(kPollSeconds, [this, generation] { poll(generation); });
}

void ExerciseControls::resetToDefault(const std::string& actionId)
{
	for (const auto& a : Controls::actions())
	{
		if (a.id != actionId)
			continue;
		std::string message;
		assign(a.id, a.defaultKey, message);
		controls_.setCapture({false, "", message});
	}
}
