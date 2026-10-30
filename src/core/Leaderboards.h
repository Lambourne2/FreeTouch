#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/Account.h"
#include "third_party/nlohmann/json.hpp"

// Leaderboards (Phase 8): the Daily Challenge's (a finished daily's score and squares, posted once; the day's board
// for friends or everyone) and the races' (each player's best time with its ghost; boards per race, the fastest
// friend per race, and any ghost on a board to race). Replies come back on the caller's thread (the game thread).
// No SDK calls.
class Leaderboards
{
public:
	struct Row
	{
		int rank = 0;
		std::string name;
		int points = 0, hits = 0;
		std::string marks; // per shot: '1' fast hit, '2' hit, '3' miss, '0' not played
		bool me = false, isFriend = false;
	};

	struct Board
	{
		std::string day;               // "YYYY-MM-DD"
		std::string scope = "friends"; // friends | all
		bool loaded = false, busy = false;
		std::vector<Row> rows;
		int players = 0;               // on this board
		std::string error;
	};

	struct RaceRow
	{
		int rank = 0;
		std::string player; // their id (to fetch the ghost)
		std::string name;
		double seconds = 0.0;
		bool me = false, isFriend = false, hasGhost = false;
	};

	struct RaceBoard
	{
		std::string race;              // the ghost key, e.g. "races.half_flip_medium"
		std::string scope = "friends"; // friends | all
		bool loaded = false, busy = false;
		std::vector<RaceRow> rows;
		int players = 0;
		std::string error;
	};

	struct Leader
	{
		std::string name;
		double seconds = 0.0;
		bool me = false;
	};

	struct View
	{
		Board daily;
		std::string postedDay; // the day whose score is on the service ("" = none yet this run)
		RaceBoard race;        // the race whose board is open
		std::map<std::string, Leader> raceLeaders; // race -> the fastest of you and your friends
	};

	using GhostFetched = std::function<void(const nlohmann::json& ghost, const std::string& error)>;

	using Log = std::function<void(const std::string& event, const nlohmann::json& fields)>;

	Leaderboards(Account& account, Log log) : account_(account), log_(std::move(log)) {}
	void setChanged(std::function<void()> changed) { changed_ = std::move(changed); }

	// A finished daily: posted once (a second post for the same day is refused by the service and counts as posted).
	void postDaily(const std::string& day, int number, int points, int hits, const std::string& marks, int64_t now);
	void refreshDaily(const std::string& day, const std::string& scope, int64_t now);
	// This player's posted score for a day: found (and its number, points, squares), or not; reached is false when
	// the service couldn't be asked.
	using MyDaily = std::function<void(bool reached, bool found, int number, int points, const std::string& marks)>;
	void fetchMyDaily(const std::string& day, int64_t now, MyDaily done);
	// A race run with its ghost (ghost::toJson): the service keeps it only when it beats the player's best.
	void postRace(const std::string& race, double seconds, const nlohmann::json& ghost, int64_t now);
	void refreshRaceLeaders(int64_t now);
	void refreshRace(const std::string& race, const std::string& scope, int64_t now);
	void fetchGhost(const std::string& race, const std::string& player, int64_t now, GhostFetched done);
	void clear();

	View view() const { return view_; }

	static std::vector<Row> parseRows(const std::string& body, int& players);
	static std::vector<RaceRow> parseRaceRows(const std::string& body, int& players);
	static std::map<std::string, Leader> parseLeaders(const std::string& body);

private:
	void changed()
	{
		if (changed_)
			changed_();
	}

	Account& account_;
	Log log_;
	std::function<void()> changed_;
	View view_;
	bool posting_ = false;
};
