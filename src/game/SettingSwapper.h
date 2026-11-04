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

	// What BakkesMod binds a key to out of the box (cfg/default_training.cfg): only the D-pad's ball controls.
	static std::string stockBind(const std::string& key)
	{
		if (key == "XboxTypeS_DPad_Up") return "default_up";
		if (key == "XboxTypeS_DPad_Down") return "default_down";
		if (key == "XboxTypeS_DPad_Left") return "default_left";
		if (key == "XboxTypeS_DPad_Right") return "default_right";
		return "";
	}
	// A key's bind as its owner's: one of Free Touch's own commands (left by a crash) is never the player's, so the key's
	// stock bind stands in. Restoring one put ft_next back on the D-pad, under Select's ball controls too
	// (Lambourne 2026-11-03).
	static std::string playersBind(const std::string& key, const std::string& bind)
	{
		return bind.rfind("ft_", 0) == 0 ? stockBind(key) : bind;
	}
	// Puts a key's leftover Free Touch command right, if it has one and Free Touch isn't holding the key. True if it did.
	bool repairLeftover(const std::string& key);

	const std::map<std::string, std::string>& originalBinds() const { return binds_; }
	const std::map<std::string, std::string>& originalCvars() const { return cvars_; }

private:
	void save() const;

	std::shared_ptr<CVarManagerWrapper> cvarManager_;
	std::filesystem::path backupFile_;
	std::map<std::string, std::string> binds_; // key -> bind before Free Touch took it ("" = none)
	std::map<std::string, std::string> cvars_; // cvar -> value before Free Touch changed it
};
