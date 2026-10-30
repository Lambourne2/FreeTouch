#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "third_party/nlohmann/json.hpp"

// Append-only JSON Lines event log. One file per plugin load:
//   <folder>/session-YYYYMMDD-HHMMSS.jsonl
// Every line gets "ts" (local wall time) and "t" (seconds since the log opened) in front of the event's own fields.
// Lines are flushed immediately so the log survives a game crash.
class EventLog
{
public:
	~EventLog();

	bool open(const std::filesystem::path& folder);
	void close();
	bool isOpen() const;
	const std::filesystem::path& path() const { return path_; }

	void write(const std::string& type, nlohmann::ordered_json fields = nlohmann::ordered_json::object());

private:
	mutable std::mutex mutex_;
	std::ofstream file_;
	std::filesystem::path path_;
	std::chrono::steady_clock::time_point openedAt_;
};
