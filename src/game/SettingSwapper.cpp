#include "pch.h"
#include "game/SettingSwapper.h"

#include <fstream>

#include "third_party/nlohmann/json.hpp"

SettingSwapper::SettingSwapper(std::shared_ptr<CVarManagerWrapper> cvarManager, std::filesystem::path backupFile)
	: cvarManager_(std::move(cvarManager)), backupFile_(std::move(backupFile))
{
}

int SettingSwapper::recoverFromCrash()
{
	std::error_code ec;
	if (!std::filesystem::exists(backupFile_, ec))
		return 0;
	try
	{
		std::ifstream in(backupFile_);
		const auto saved = nlohmann::json::parse(in);
		if (saved.contains("binds") || saved.contains("cvars"))
		{
			for (const auto& [k, v] : saved.value("binds", nlohmann::json::object()).items())
				binds_[k] = v.get<std::string>();
			for (const auto& [k, v] : saved.value("cvars", nlohmann::json::object()).items())
				cvars_[k] = v.get<std::string>();
		}
		else
		{
			for (const auto& [k, v] : saved.items()) // first format: a flat map of binds
				binds_[k] = v.get<std::string>();
		}
	}
	catch (const std::exception&)
	{
		binds_.clear();
		cvars_.clear();
	}
	const int count = static_cast<int>(binds_.size() + cvars_.size());
	restoreAll();
	return count;
}

void SettingSwapper::setBind(const std::string& key, const std::string& command)
{
	if (!binds_.contains(key))
	{
		binds_[key] = cvarManager_->getBindStringForKey(key);
		save();
	}
	cvarManager_->setBind(key, command);
}

void SettingSwapper::restoreBind(const std::string& key)
{
	const auto it = binds_.find(key);
	if (it == binds_.end())
		return;
	if (it->second.empty())
		cvarManager_->removeBind(key);
	else
		cvarManager_->setBind(key, it->second);
	binds_.erase(it);
	save();
}

bool SettingSwapper::setCvar(const std::string& name, const std::string& value)
{
	CVarWrapper cvar = cvarManager_->getCvar(name);
	if (!cvar)
		return false;
	if (!cvars_.contains(name))
	{
		cvars_[name] = cvar.getStringValue();
		save();
	}
	cvar.setValue(value);
	return true;
}

void SettingSwapper::restoreCvar(const std::string& name)
{
	const auto it = cvars_.find(name);
	if (it == cvars_.end())
		return;
	if (CVarWrapper cvar = cvarManager_->getCvar(name))
		cvar.setValue(it->second);
	cvars_.erase(it);
	save();
}

void SettingSwapper::restoreAll()
{
	while (!binds_.empty())
		restoreBind(binds_.begin()->first);
	while (!cvars_.empty())
		restoreCvar(cvars_.begin()->first);
}

void SettingSwapper::save() const
{
	std::error_code ec;
	if (binds_.empty() && cvars_.empty())
	{
		std::filesystem::remove(backupFile_, ec);
		return;
	}
	std::filesystem::create_directories(backupFile_.parent_path(), ec);
	std::ofstream out(backupFile_, std::ios::trunc);
	out << nlohmann::json{{"binds", binds_}, {"cvars", cvars_}}.dump(2);
}
