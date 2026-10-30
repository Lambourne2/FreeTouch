#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/Online.h"
#include "third_party/nlohmann/json.hpp"

// The player's Free Touch account (Phase 8): an anonymous Supabase sign-in plus a profile (display name, friend code).
// "Pick a name" signs in and makes the profile; the session (its refresh token) is kept in settings/account.json and
// refreshed at the next start. One request at a time; replies must come back on the thread that calls the methods
// (the plugin's game thread). Tokens are never logged. No SDK calls.
class Account
{
public:
	enum class State
	{
		SignedOut, // no profile yet (maybe a session waiting for a name)
		Working,   // a request is out
		SignedIn,
		Offline    // a saved account that could not be reached; its name and code are shown from the last time
	};

	struct View
	{
		State state = State::SignedOut;
		std::string name;
		std::string friendCode;
		std::string recoveryCode; // moves the account to another PC ("" = none made, or made on another PC)
		std::string error; // the last problem in plain words ("" = none)
	};

	using Log = std::function<void(const std::string& event, const nlohmann::json& fields)>;

	Account(online::HttpFn http, Log log) : http_(std::move(http)), log_(std::move(log)) {}

	// Called when something a page shows changes, or the saved file should be written (save()).
	void setChanged(std::function<void()> changed) { changed_ = std::move(changed); }

	// settings/account.json, then sign back in with its session (now: seconds since the epoch).
	void load(const nlohmann::json& saved, int64_t now);
	nlohmann::json save() const;

	void create(const std::string& name, int64_t now); // "Pick a name"
	void rename(const std::string& name, int64_t now);
	void signOut();
	void retry(int64_t now);                           // Offline: try again
	void makeRecoveryCode(int64_t now);                // a new one (the old one stops working)
	void restore(const std::string& code, int64_t now); // signed out: bring an account here with its code

	// A recovery code as the service writes it (XXXXX-XXXXX-XXXXX-XXXXX), from what was typed; "" if it isn't one.
	static std::string cleanCode(const std::string& code);

	View view() const;
	bool busy() const { return busy_; }
	bool signedIn() const { return signedIn_; }
	// The current sign-in (for the release script's hand-off file; never logged).
	const std::string& accessToken() const { return accessToken_; }
	int64_t accessExpiresAt() const { return expiresAt_; }
	const std::string& userId() const { return userId_; }

	// A request as the signed-in player (its session refreshed first when old): the service's address, key and
	// sign-in are added. Status 0 when signed out or the refresh failed.
	void send(online::HttpRequest request, int64_t now, online::HttpDone done);

	// Friends see what you train (minutes, streak, last drill, on now). Kept in settings/account.json.
	bool shareActivity() const { return shareActivity_; }
	void setShareActivity(bool on);

	// A name as the service takes it: trimmed, 3 to 20 of A-Z a-z 0-9 space . _ -. "" and why if not.
	static std::string checkName(const std::string& name, std::string& why);

private:
	void signUp(std::function<void()> then);
	// Refreshes first when the access token is old; `failed` runs after a failed refresh.
	void withSession(int64_t now, std::function<void()> then, std::function<void()> failed = {});
	void refresh(std::function<void()> then, std::function<void()> failed = {});
	void fetchProfile();
	void insertProfile(const std::string& name);
	void touchLastSeen();
	void requestCode(std::function<void()> then); // make_recovery_code, then `then` (made or not)
	void fail(int status, const std::string& body, const std::string& what);
	void finish();
	online::HttpRequest auth(const std::string& path) const;
	online::HttpRequest rest(const std::string& method, const std::string& path) const;
	bool takeProfile(const std::string& body);

	online::HttpFn http_;
	Log log_;
	std::function<void()> changed_;
	bool busy_ = false;
	bool signedIn_ = false;
	bool offline_ = false;
	std::string error_;
	// The session.
	std::string userId_, accessToken_, refreshToken_;
	int64_t expiresAt_ = 0;
	int64_t now_ = 0; // the time of the last call (for expires_in)
	// The profile (kept from the last time for Offline).
	std::string name_, friendCode_;
	std::string recoveryCode_; // kept on this PC to show again
	bool shareActivity_ = true;
};
