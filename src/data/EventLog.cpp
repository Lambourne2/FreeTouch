#include "pch.h"
#include "data/EventLog.h"

#include <cctype>
#include <cmath>
#include <ctime>
#include <format>

namespace
{
	std::tm localNow(std::chrono::system_clock::time_point now)
	{
		const std::time_t secs = std::chrono::system_clock::to_time_t(now);
		std::tm tm{};
		localtime_s(&tm, &secs);
		return tm;
	}

	std::string timestamp(std::chrono::system_clock::time_point now)
	{
		const std::tm tm = localNow(now);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
		return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}",
			tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
	}
}

EventLog::~EventLog()
{
	close();
}

bool EventLog::open(const std::filesystem::path& folder)
{
	std::lock_guard lock(mutex_);
	if (file_.is_open())
		file_.close();

	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	if (ec)
		return false;

	const std::tm tm = localNow(std::chrono::system_clock::now());
	const std::string name = std::format("session-{:04}{:02}{:02}-{:02}{:02}{:02}.jsonl",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

	path_ = folder / name;
	file_.open(path_, std::ios::out | std::ios::app);
	openedAt_ = std::chrono::steady_clock::now();
	return file_.is_open();
}

void EventLog::close()
{
	std::lock_guard lock(mutex_);
	if (file_.is_open())
		file_.close();
}

bool EventLog::isOpen() const
{
	std::lock_guard lock(mutex_);
	return file_.is_open();
}

void EventLog::write(const std::string& type, nlohmann::ordered_json fields)
{
	std::lock_guard lock(mutex_);
	if (!file_.is_open())
		return;

	const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - openedAt_).count();

	nlohmann::ordered_json line;
	line["ts"] = timestamp(std::chrono::system_clock::now());
	line["t"] = std::round(t * 1000.0) / 1000.0;
	line["type"] = type;
	if (fields.is_object())
	{
		for (auto& [key, value] : fields.items())
		{
			// "ts", "t" and "type" belong to the line; a field with one of those names keeps it as "field<Name>".
			if (key == "ts" || key == "t" || key == "type")
				line["field" + std::string(1, static_cast<char>(std::toupper(key[0]))) + key.substr(1)] = std::move(value);
			else
				line[key] = std::move(value);
		}
	}

	// replace invalid UTF-8 instead of throwing; a bad player name must never take the game down
	file_ << line.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace) << '\n';
	file_.flush();
}
