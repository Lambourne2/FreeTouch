#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Account.h"
#include "third_party/nlohmann/json.hpp"

// Friends (Phase 8): the list (friends, requests you got and sent) from the service's friends_list, adding by friend
// code or name, accept / decline, remove; and the training activity friends see, sent while signed in with sharing
// on. Replies come back on the caller's thread (the game thread). No SDK calls.
class Friends
{
public:
	struct Entry
	{
		std::string id;
		std::string name;
		std::string friendCode;
		std::string state;      // "friend", "incoming" (they asked you) or "sent" (you asked them)
		bool online = false;    // a friend active in the last few minutes
		int seenAgo = -1;       // seconds since a friend was last on (-1 = unknown)
		int weekMinutes = -1;   // what a friend shares (-1 = nothing shared)
		int todayMinutes = -1;
		int streakDays = -1;
		std::string lastDrill;
		int trainedAgo = -1;    // seconds since their last training
	};

	struct View
	{
		bool loaded = false; // a list has come back since signing in
		bool busy = false;
		std::vector<Entry> list; // friends, then requests you got, then sent (each by name)
		int incoming = 0;
		std::string message; // the last action's result ("Request sent")
		std::string error;
		std::vector<std::string> supporters;     // lower-case names of players with the supporter heart (sorted)
		std::vector<std::string> supporterNames; // the same players as they write their names, newest supporter first
	};

	// What this player shares (from the training history).
	struct Activity
	{
		int weekMinutes = 0;
		int todayMinutes = 0;
		int streakDays = 0;
		std::string lastDrill;
		bool trainedSinceLast = false; // trained since the last one sent: last_trained_at becomes now
	};

	using Log = std::function<void(const std::string& event, const nlohmann::json& fields)>;

	Friends(Account& account, Log log) : account_(account), log_(std::move(log)) {}
	void setChanged(std::function<void()> changed) { changed_ = std::move(changed); }

	void refresh(int64_t now);
	void add(const std::string& who, int64_t now); // a friend code or a name
	void respond(const std::string& id, bool accept, int64_t now);
	void remove(const std::string& id, int64_t now); // unfriend, cancel, decline
	void sendActivity(const Activity& a, int64_t now);
	void stopSharing(int64_t now); // deletes the shared activity
	void clear();                  // signed out

	View view() const { return view_; }

	static std::vector<Entry> parseList(const std::string& body);
	// The supporters list (profiles?supporter=is.true, newest first): lower-case names, sorted.
	static std::vector<std::string> parseSupporters(const std::string& body);
	// The same list as the names are written, in the service's order (newest supporter first).
	static std::vector<std::string> parseSupporterNames(const std::string& body);
	// "Amy", "Amy and bob", "Amy, bob and Cid", "Amy, bob, Cid and 4 more".
	static std::string nameList(const std::vector<std::string>& names, size_t shown);
	// Whether this name has the supporter heart (any case).
	static bool isSupporter(const View& view, const std::string& name);
	// "just now", "12 min ago", "3 h ago", "2 days ago".
	static std::string ago(int seconds);

private:
	void rpc(const std::string& name, const nlohmann::json& args, int64_t now, std::function<void(int, const std::string&)> done);
	void refreshSupporters(int64_t now);
	void failed(int status, const std::string& body, const std::string& what);

	Account& account_;
	Log log_;
	std::function<void()> changed_;
	View view_;
	int64_t supportersAt_ = 0; // when the supporters list was last asked for (0 = never)
};
