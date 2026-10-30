#pragma once

#include <chrono>
#include <deque>
#include <string>

#include "core/Canvas.h"
#include "core/SessionRunner.h"
#include "scenario/ScenarioPlayer.h"

// On-screen strip during exercises: exercise, rep number, timer, streak, difficulty, and a big feedback flash
// (GOAL / CONCEDED / SAVED / TIME) when a rep is judged. Drawn every frame through Canvas (game thread). No SDK.
class Hud
{
public:
	void draw(Canvas& canvas, const ScenarioPlayer::Status& status, const SessionRunner::Status& session);
	// The game speed when practice slow motion is on (1 = normal: nothing shown).
	void setGameSpeed(float speed) { gameSpeed_ = speed; }
	// Mechanics feed (left side, in freeplay and exercises): the mechanics the player just did, fading out.
	// `force`: shown even with the feed off (the mechanics modes always show it).
	void pushMechanic(const std::string& label, float kmh, int points, bool force = false);
	void drawFeed(Canvas& canvas);
	void setFeedEnabled(bool on) { feedOn_ = on; if (!on) feed_.clear(); }
	// A milestone reached: a banner slides up below the middle for a few seconds (tier 1 bronze, 2 silver, 3 gold).
	void showMilestone(const std::string& title, int tier)
	{
		milestone_ = title;
		milestoneTier_ = tier;
		milestoneAt_ = std::chrono::steady_clock::now();
	}

	// A short hint at the bottom middle (e.g. "Press up again for the menu"), gone after `seconds`.
	void showHint(const std::string& text, float seconds)
	{
		hint_ = text;
		hintSeconds_ = seconds;
		hintAt_ = std::chrono::steady_clock::now();
	}

private:
	std::string hint_;
	float hintSeconds_ = 0.f;
	std::chrono::steady_clock::time_point hintAt_{};
	void drawHint(Canvas& c);
	void drawStrip(Canvas& c, const ScenarioPlayer::Status& s, float k);
	void drawFlash(Canvas& c, float k);
	void drawObjective(Canvas& c, const ScenarioPlayer::Status& s, float k, float lineY);
	void drawReadCard(Canvas& c, const ScenarioPlayer::Status& s, float k);
	void drawPlayMap(Canvas& c, const ScenarioPlayer::Status::PlayMap& m, float k);

	float gameSpeed_ = 1.f;
	int lastSeq_ = 0;
	bool flashing_ = false;
	std::chrono::steady_clock::time_point flashStart_{};
	ScenarioPlayer::Status flash_;
	// Objective announcement: shown big when a rep with a new objective starts.
	int objectiveRep_ = -1;
	std::string bannerText_;
	std::chrono::steady_clock::time_point bannerStart_{};
	struct FeedItem
	{
		std::string label;
		float kmh = 0.f;
		int points = 0;
		std::chrono::steady_clock::time_point at;
	};
	std::deque<FeedItem> feed_;
	bool feedOn_ = true;
	std::string milestone_;
	int milestoneTier_ = 1;
	std::chrono::steady_clock::time_point milestoneAt_{};
	void drawMilestone(Canvas& c);
};
