#include "pch.h"
#include "core/Controls.h"

#include <cctype>

#include <fstream>

#include "third_party/nlohmann/json.hpp"

namespace
{
	const std::map<std::string, std::string> kPrettyNames = {
		{"XboxTypeS_A", "A"}, {"XboxTypeS_B", "B"}, {"XboxTypeS_X", "X"}, {"XboxTypeS_Y", "Y"},
		{"XboxTypeS_LeftShoulder", "LB"}, {"XboxTypeS_RightShoulder", "RB"},
		{"XboxTypeS_LeftTrigger", "LT"}, {"XboxTypeS_RightTrigger", "RT"},
		{"XboxTypeS_LeftThumbStick", "L3"}, {"XboxTypeS_RightThumbStick", "R3"},
		{"XboxTypeS_DPad_Up", "D-pad up"}, {"XboxTypeS_DPad_Down", "D-pad down"},
		{"XboxTypeS_DPad_Left", "D-pad left"}, {"XboxTypeS_DPad_Right", "D-pad right"},
		{"XboxTypeS_Back", "Select"}, {"XboxTypeS_Start", "Start"},
	};
}

Controls::Controls()
{
	for (const auto& a : actions())
		keys_[a.id] = a.defaultKey;
}

const std::vector<Controls::Action>& Controls::actions()
{
	static const std::vector<Action> list = {
		{"retry", "Retry (same rep)", "ft_retry", "XboxTypeS_RightThumbStick"},
		{"next", "Next rep", "ft_next", "XboxTypeS_DPad_Right"},
		{"previous", "Previous rep", "ft_prev", "XboxTypeS_DPad_Left"},
		{"back", "Back to the dashboard (double tap)", "ft_back", "XboxTypeS_DPad_Up"},
		{"slowmo", "Slow motion (practice speed)", "ft_slowmo", "XboxTypeS_DPad_Down"},
		{"clutch", "Clutch (hold: even slower)", "ft_clutch", ""},
	};
	return list;
}

const std::vector<std::string>& Controls::capturableKeys()
{
	static const std::vector<std::string> keys = [] {
		std::vector<std::string> k = {
			"XboxTypeS_A", "XboxTypeS_B", "XboxTypeS_X", "XboxTypeS_Y",
			"XboxTypeS_LeftShoulder", "XboxTypeS_RightShoulder", "XboxTypeS_LeftTrigger", "XboxTypeS_RightTrigger",
			"XboxTypeS_LeftThumbStick", "XboxTypeS_RightThumbStick",
			"XboxTypeS_DPad_Up", "XboxTypeS_DPad_Down", "XboxTypeS_DPad_Left", "XboxTypeS_DPad_Right",
			"XboxTypeS_Back", "XboxTypeS_Start",
			"SpaceBar", "Tab", "LeftShift", "RightShift", "LeftControl", "RightControl", "LeftAlt", "RightAlt",
			"Enter", "BackSpace", "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
			"Up", "Down", "Left", "Right",
			"F1", "F3", "F4", "F5", "F7", "F8", "F10", "F11", "F12",
			"One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine", "Zero",
			"NumPadZero", "NumPadOne", "NumPadTwo", "NumPadThree", "NumPadFour", "NumPadFive", "NumPadSix",
			"NumPadSeven", "NumPadEight", "NumPadNine",
		};
		for (char c = 'A'; c <= 'Z'; ++c)
			k.emplace_back(1, c);
		return k;
	}();
	return keys;
}

std::string Controls::displayName(const std::string& key)
{
	if (key.empty())
		return "(none)";
	const auto it = kPrettyNames.find(key);
	if (it != kPrettyNames.end())
		return it->second;
	// The game's key names read better with spaces: LeftShift -> Left Shift, SpaceBar -> Space Bar.
	std::string out;
	for (size_t i = 0; i < key.size(); ++i)
	{
		if (i > 0 && std::isupper(static_cast<unsigned char>(key[i])) && std::islower(static_cast<unsigned char>(key[i - 1])))
			out += ' ';
		out += key[i];
	}
	return out;
}

bool Controls::load(const std::filesystem::path& file)
{
	std::lock_guard lock(mutex_);
	for (const auto& a : actions())
		keys_[a.id] = a.defaultKey;
	std::ifstream in(file);
	if (!in)
		return false;
	try
	{
		const auto doc = nlohmann::json::parse(in);
		for (const auto& a : actions())
		{
			if (doc.contains(a.id) && doc[a.id].is_string())
				keys_[a.id] = doc[a.id].get<std::string>();
		}
		return true;
	}
	catch (const std::exception&)
	{
		return false;
	}
}

bool Controls::save(const std::filesystem::path& file) const
{
	nlohmann::ordered_json doc;
	{
		std::lock_guard lock(mutex_);
		for (const auto& a : actions())
			doc[a.id] = keys_.at(a.id);
	}
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	std::ofstream out(file, std::ios::trunc);
	out << doc.dump(2) << '\n';
	return static_cast<bool>(out);
}

std::map<std::string, std::string> Controls::bindings() const
{
	std::lock_guard lock(mutex_);
	return keys_;
}

std::string Controls::keyFor(const std::string& actionId) const
{
	std::lock_guard lock(mutex_);
	const auto it = keys_.find(actionId);
	return it == keys_.end() ? "" : it->second;
}

std::string Controls::actionUsing(const std::string& key, const std::string& exceptAction) const
{
	std::lock_guard lock(mutex_);
	for (const auto& [action, k] : keys_)
	{
		if (k == key && action != exceptAction)
			return action;
	}
	return "";
}

void Controls::setKey(const std::string& actionId, const std::string& key)
{
	std::lock_guard lock(mutex_);
	keys_[actionId] = key;
}

void Controls::resetToDefault(const std::string& actionId)
{
	std::lock_guard lock(mutex_);
	for (const auto& a : actions())
	{
		if (a.id == actionId)
			keys_[a.id] = a.defaultKey;
	}
}

Controls::CaptureState Controls::capture() const
{
	std::lock_guard lock(mutex_);
	return capture_;
}

void Controls::setCapture(CaptureState state)
{
	std::lock_guard lock(mutex_);
	capture_ = std::move(state);
}
