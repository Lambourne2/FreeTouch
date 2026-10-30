#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Free Touch's rebindable exercise actions and their keys. Pure data, no SDK calls; thread-safe because the
// bindings screen (render thread) reads it while the game thread changes it.
class Controls
{
public:
	struct Action
	{
		std::string id;          // "retry"
		std::string label;       // "Retry"
		std::string command;     // console command the bind runs
		std::string defaultKey;  // BakkesMod key name
	};

	struct CaptureState
	{
		bool capturing = false;
		std::string actionId;
		std::string message;     // last result, for the screen ("Retry is now R3", "Timed out")
	};

	Controls();

	static const std::vector<Action>& actions();
	// Keys the capture listens for. Mouse buttons, Escape (cancel) and the BakkesMod/Free Touch menu keys
	// (F2, F6, F9) are left out on purpose.
	static const std::vector<std::string>& capturableKeys();
	static std::string displayName(const std::string& key);

	bool load(const std::filesystem::path& file);   // missing or broken file = defaults
	bool save(const std::filesystem::path& file) const;

	std::map<std::string, std::string> bindings() const; // action id -> key
	std::string keyFor(const std::string& actionId) const;
	// Returns the action already using `key` ("" if free), ignoring `exceptAction`.
	std::string actionUsing(const std::string& key, const std::string& exceptAction) const;
	void setKey(const std::string& actionId, const std::string& key);
	void resetToDefault(const std::string& actionId);

	CaptureState capture() const;
	void setCapture(CaptureState state);

private:
	mutable std::mutex mutex_;
	std::map<std::string, std::string> keys_;
	CaptureState capture_;
};
