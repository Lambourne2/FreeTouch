#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Account.h"
#include "third_party/nlohmann/json.hpp"

// Shots online (Phase 8): a saved shot uploaded (the same JSON as a .ftshot file) and sent to friends or posted on
// the public Shot Board; the shots friends sent you; the board (newest, top this week, yours) with likes, plays and
// reports. Replies come back on the caller's thread (the game thread). No SDK calls.
class OnlineShots
{
public:
	struct InboxItem
	{
		std::string delivery, shotId, name, key, from;
		int ageSeconds = -1;
	};

	struct BoardItem
	{
		std::string id, name, key, owner;
		bool mine = false, isPublic = false, liked = false;
		int ageSeconds = -1, likes = 0, playsWeek = 0;
	};

	struct View
	{
		bool busy = false;
		std::vector<InboxItem> inbox;
		std::string sort = "new"; // the board's: new | top | mine
		bool boardLoaded = false;
		std::vector<BoardItem> board;
		std::string message, error;
	};

	using Log = std::function<void(const std::string& event, const nlohmann::json& fields)>;
	// A .ftshot document (shotshare::shareDoc / readShare's format), or "" when the fetch failed.
	using Fetched = std::function<void(const std::string& ftshot, const std::string& error)>;

	OnlineShots(Account& account, Log log) : account_(account), log_(std::move(log)) {}
	void setChanged(std::function<void()> changed) { changed_ = std::move(changed); }

	// Uploads a .ftshot document's shot (or finds it, by its fingerprint), then posts it publicly and / or sends it to
	// these friends (ids; their names for the message).
	void post(const nlohmann::json& ftshot, bool makePublic, const std::vector<std::string>& friendIds, const std::string& toNames,
		int64_t now);
	void refreshInbox(int64_t now);
	void refreshBoard(const std::string& sort, int64_t now);
	void fetch(const std::string& shotId, int64_t now, Fetched done);
	void dismiss(const std::string& delivery, int64_t now);
	void like(const std::string& shotId, bool on, int64_t now);
	void report(const std::string& shotId, int64_t now);
	void setPublic(const std::string& shotId, bool on, int64_t now);
	void remove(const std::string& shotId, int64_t now); // one of yours, from the service
	void played(const std::string& shotId, int64_t now); // counted once a day per player
	void clear();

	View view() const { return view_; }

	static std::vector<InboxItem> parseInbox(const std::string& body);
	static std::vector<BoardItem> parseBoard(const std::string& body);
	// get_shot's row as a .ftshot document; "" if it isn't one.
	static std::string ftshotFromRow(const std::string& body);

private:
	void rpc(const std::string& name, const nlohmann::json& args, int64_t now, online::HttpDone done);
	void failed(int status, const std::string& body, const std::string& what);
	void changed()
	{
		if (changed_)
			changed_();
	}

	Account& account_;
	Log log_;
	std::function<void()> changed_;
	View view_;
};
