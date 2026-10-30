#pragma once

#include <string>

// What Settings > Updates shows. src/core/Updater fills it; its own header so the BakkesPlugins build (src/flavor.h),
// which has no updater, still has the card.
enum class UpdateState { Idle, Checking, UpToDate, Available, Downloading, Installed, Failed };

struct UpdateView
{
	UpdateState state = UpdateState::Idle;
	std::string current;   // this build's version
	std::string latest;    // the newest release's version ("" until checked)
	std::string notes;     // its "what's new"
	int done = 0, total = 0; // files downloaded / to download
	std::string error;
	bool autoInstall = true;
	std::string justUpdated; // this build was installed by an update: its version (the notes say what's new)
	bool selfUpdate = true;  // false: the BakkesPlugins build, which BakkesMod updates
};
