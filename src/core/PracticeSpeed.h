#pragma once

#include <algorithm>

// Practice slow motion: the game speed wanted each tick from the settings and the two buttons. The slow-motion
// button gives the practice speed (toggle or hold); the clutch button, while held, gives the clutch speed (a share of
// the practice speed). Outside exercises it is always 1. No SDK calls: the plugin applies the speed.
struct PracticeSpeedSettings
{
	bool enabled = true;
	bool hold = false;      // false: the slow-motion button toggles; true: slow only while it is held
	float practice = 0.7f;  // practice speed (share of normal speed)
	float clutch = 0.5f;    // clutch speed as a share of the practice speed
};

class PracticeSpeed
{
public:
	void setSettings(const PracticeSpeedSettings& s)
	{
		settings_ = s;
		settings_.practice = std::clamp(settings_.practice, 0.2f, 1.f);
		settings_.clutch = std::clamp(settings_.clutch, 0.2f, 1.f);
	}
	const PracticeSpeedSettings& settings() const { return settings_; }

	// The slow-motion button in toggle mode.
	void toggle()
	{
		if (settings_.enabled && !settings_.hold)
			toggledOn_ = !toggledOn_;
	}

	// Speed wanted now. `slowHeld` / `clutchHeld`: whether those buttons are down this tick.
	float update(bool exerciseActive, bool slowHeld, bool clutchHeld)
	{
		if (!exerciseActive || !settings_.enabled)
		{
			toggledOn_ = false;
			speed_ = 1.f;
			return speed_;
		}
		const bool slow = settings_.hold ? slowHeld : toggledOn_;
		speed_ = clutchHeld ? settings_.practice * settings_.clutch : slow ? settings_.practice : 1.f;
		return speed_;
	}

	float speed() const { return speed_; }

private:
	PracticeSpeedSettings settings_;
	bool toggledOn_ = false;
	float speed_ = 1.f;
};
