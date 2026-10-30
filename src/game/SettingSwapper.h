#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>

class CVarManagerWrapper;

// Temporarily changes BakkesMod key binds and cvars and gives the originals back. Binds and cvars are global and
// saved to binds.cfg / config.cfg, so everything Free Touch changes goes through here and is restored (on exercise
// end and on unload). The originals are also kept in `backupFile`, so a game crash cannot leave them changed: the
// next load puts them back.
class SettingSwapper
{
public:
	SettingSwapper(std::shared_ptr<CVarManagerWrapper> cvarManager, std::filesystem::path backupFile);

	// Restores binds and cvars left in the backup file by a crash. Returns how many it restored.
	int recoverFromCrash();

	// Binds `key` to `command`, remembering the bind it had before (only the first time per key).
	void setBind(const std::string& key, const std::string& command);
	void restoreBind(const std::string& key);

	// Sets cvar `name` to `value`, remembering its value before. Returns false if the cvar does not exist.
	bool setCvar(const std::string& name, const std::string& value);
	void restoreCvar(const std::string& name);

	void restoreAll();

	const std::map<std::string, std::string>& originalBinds() const { return binds_; }
	const std::map<std::string, std::string>& originalCvars() const { return cvars_; }

private:
	void save() const;

	std::shared_ptr<CVarManagerWrapper> cvarManager_;
	std::filesystem::path backupFile_;
	std::map<std::string, std::string> binds_; // key -> bind before Free Touch took it ("" = none)
	std::map<std::string, std::string> cvars_; // cvar -> value before Free Touch changed it
};
