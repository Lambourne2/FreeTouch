#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

// Saving settings files so a crash or the game closing mid-write never leaves one half written (2026-10-29: a
// broken settings/daily.json read as "no attempts", so the finished daily could be played again and the next save
// overwrote every past day). No SDK calls.
namespace safefile
{
	// Writes `text` to a temporary file beside `path`, then swaps it in. False (and `path` untouched) if it failed.
	inline bool write(const std::filesystem::path& path, const std::string& text)
	{
		std::error_code ec;
		std::filesystem::create_directories(path.parent_path(), ec);
		std::filesystem::path tmp = path;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
			out << text;
			out.flush();
			if (!out)
				return false;
		}
		std::filesystem::rename(tmp, path, ec); // replaces the old file in one step
		if (ec)
		{
			std::filesystem::remove(tmp, ec);
			return false;
		}
		return true;
	}

	// A file that couldn't be read is kept as <name>.broken (an older one replaced) instead of being overwritten by
	// the next save, so nothing in it is lost for good.
	inline void setAside(const std::filesystem::path& path)
	{
		std::error_code ec;
		std::filesystem::path aside = path;
		aside += ".broken";
		std::filesystem::rename(path, aside, ec);
	}
}
