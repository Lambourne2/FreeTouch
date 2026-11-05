#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Account.h"
#include "third_party/nlohmann/json.hpp"

// Shot Swap (Phase 8): a Shot Bank sent to friends to defend, the banks friends sent you, and how everyone did. The first
// finished try is the result both sides see. Replies come back on the caller's thread (the game thread). No SDK calls.
class OnlineBanks
{
public:
	struct Challenge
	{
		std::string id, from;
		int ageSeconds = -1;
		std::string results; // "" until played; else five of S (saved) and X (went in), by shot
	};

	struct Sent
	{
		std::string id, bank, to;
		int ageSeconds = -1;
		std::string results;
		int playedAgo = -1;
	};

	struct View
	{
		bool busy = false;
		bool loaded = false;
		std::vector<Challenge> inbox;
		std::vector<Sent> sent;
		int waiting = 0; // sent to you, not played yet
		std::string message, error;
	};

	using Log = std::function<void(const std::string& event, const nlohmann::json& fields)>;
	// The bank (shotbank::shareDoc), who sent it and any result already in; an empty `error` means it came back.
	using Fetched = std::function<void(const nlohmann::json& bank, const std::string& from, const std::string& results,
		const std::string& error)>;

	OnlineBanks(Account& account, Log log) : account_(account), log_(std::move(log)) {}
	void setChanged(std::function<void()> changed) { changed_ = std::move(changed); }
	// Someone new has sent you a bank since the last look (their name).
	void setNewChallenge(std::function<void(const std::string& from)> listener) { newChallenge_ = std::move(listener); }

	void send(const std::string& key, const nlohmann::json& bank, const std::vector<std::string>& friendIds,
		const std::string& toNames, int64_t now);
	void refresh(int64_t now);
	void fetch(const std::string& challenge, int64_t now, Fetched done);
	void finish(const std::string& challenge, const std::string& results, int64_t now);
	void remove(const std::string& challenge, int64_t now); // take one back, or turn one down
	void clear();

	View view() const { return view_; }

	static std::vector<Challenge> parseInbox(const std::string& body);
	static std::vector<Sent> parseSent(const std::string& body);

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
	std::function<void(const std::string&)> newChallenge_;
	View view_;
};
