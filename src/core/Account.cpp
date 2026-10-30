#include "pch.h"
#include "core/Account.h"

#include <cctype>

namespace
{
	using json = nlohmann::json;

	json parse(const std::string& body)
	{
		try
		{
			return json::parse(body);
		}
		catch (const std::exception&)
		{
			return json();
		}
	}

	std::string text(const json& j, const char* key)
	{
		return j.is_object() && j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
	}
}

std::string Account::checkName(const std::string& name, std::string& why)
{
	size_t a = 0, b = name.size();
	while (a < b && std::isspace(static_cast<unsigned char>(name[a])))
		++a;
	while (b > a && std::isspace(static_cast<unsigned char>(name[b - 1])))
		--b;
	const std::string out = name.substr(a, b - a);
	if (out.size() < 3 || out.size() > 20)
	{
		why = "Names are 3 to 20 characters.";
		return "";
	}
	for (const char ch : out)
	{
		const auto c = static_cast<unsigned char>(ch);
		if (!std::isalnum(c) && ch != ' ' && ch != '.' && ch != '_' && ch != '-')
		{
			why = "Use letters, numbers, spaces, dots, dashes and underscores.";
			return "";
		}
	}
	why.clear();
	return out;
}

void Account::load(const json& saved, int64_t now)
{
	now_ = now;
	userId_ = text(saved, "userId");
	refreshToken_ = text(saved, "refreshToken");
	name_ = text(saved, "name");
	friendCode_ = text(saved, "friendCode");
	recoveryCode_ = text(saved, "recoveryCode");
	shareActivity_ = !saved.is_object() || saved.value("shareActivity", true);
	accessToken_.clear();
	expiresAt_ = 0;
	if (refreshToken_.empty())
		return;
	// Sign back in, then read the profile (a session with no profile yet still needs a name).
	busy_ = true;
	if (changed_)
		changed_();
	refresh([this] { fetchProfile(); });
}

json Account::save() const
{
	// The access token is not kept: the next start refreshes the session.
	json out = json::object();
	if (!refreshToken_.empty())
	{
		out["userId"] = userId_;
		out["refreshToken"] = refreshToken_;
	}
	if (!name_.empty())
	{
		out["name"] = name_;
		out["friendCode"] = friendCode_;
	}
	if (!recoveryCode_.empty())
		out["recoveryCode"] = recoveryCode_;
	if (!shareActivity_)
		out["shareActivity"] = false;
	return out;
}

void Account::create(const std::string& name, int64_t now)
{
	if (busy_ || signedIn_)
		return;
	now_ = now;
	std::string why;
	const std::string clean = checkName(name, why);
	if (clean.empty())
	{
		error_ = why;
		if (changed_)
			changed_();
		return;
	}
	busy_ = true;
	error_.clear();
	if (changed_)
		changed_();
	log_("account_create", {{"name", clean}, {"hadSession", !refreshToken_.empty()}});
	if (refreshToken_.empty())
		signUp([this, clean] { insertProfile(clean); });
	else
		withSession(now, [this, clean] { insertProfile(clean); });
}

void Account::rename(const std::string& name, int64_t now)
{
	if (busy_ || !signedIn_)
		return;
	now_ = now;
	std::string why;
	const std::string clean = checkName(name, why);
	if (clean.empty() || clean == name_)
	{
		error_ = why;
		if (changed_)
			changed_();
		return;
	}
	busy_ = true;
	error_.clear();
	if (changed_)
		changed_();
	withSession(now, [this, clean] {
		online::HttpRequest r = rest("PATCH", "/rest/v1/profiles?id=eq." + userId_);
		r.body = json{{"name", clean}}.dump();
		http_(r, [this](int status, const std::string& body) {
			if (status < 200 || status >= 300 || !takeProfile(body))
				return fail(status, body, "rename");
			log_("account_renamed", {{"name", name_}});
			finish();
		});
	});
}

void Account::signOut()
{
	if (busy_)
		return;
	if (!accessToken_.empty())
	{
		// Best effort: the session ends on the service too. The local copy goes either way.
		online::HttpRequest r = auth("/auth/v1/logout");
		r.method = "POST";
		r.headers["Authorization"] = "Bearer " + accessToken_;
		http_(r, [](int, const std::string&) {});
	}
	log_("account_signed_out", {{"name", name_}});
	userId_.clear();
	accessToken_.clear();
	refreshToken_.clear();
	expiresAt_ = 0;
	name_.clear();
	friendCode_.clear();
	recoveryCode_.clear();
	signedIn_ = offline_ = false;
	error_.clear();
	if (changed_)
		changed_();
}

std::string Account::cleanCode(const std::string& code)
{
	std::string hex;
	for (const char ch : code)
	{
		const auto c = static_cast<unsigned char>(ch);
		if (std::isxdigit(c))
			hex += static_cast<char>(std::toupper(c));
		else if (ch != '-' && ch != ' ' && ch != '\t')
			return "";
	}
	if (hex.size() != 20)
		return "";
	return hex.substr(0, 5) + "-" + hex.substr(5, 5) + "-" + hex.substr(10, 5) + "-" + hex.substr(15, 5);
}

void Account::makeRecoveryCode(int64_t now)
{
	if (busy_ || !signedIn_)
		return;
	busy_ = true;
	error_.clear();
	if (changed_)
		changed_();
	withSession(now, [this] { requestCode([this] { finish(); }); });
}

void Account::requestCode(std::function<void()> then)
{
	online::HttpRequest r = rest("POST", "/rest/v1/rpc/make_recovery_code");
	r.body = "{}";
	http_(r, [this, then](int status, const std::string& body) {
		const json j = parse(body);
		const std::string code = j.is_string() ? cleanCode(j.get<std::string>()) : std::string();
		if (status < 200 || status >= 300 || code.empty())
		{
			error_ = "Couldn't make your recovery code (" + std::to_string(status) + "). Press Make one to try again.";
			log_("account_error", {{"what", "recovery code"}, {"status", status}});
		}
		else
		{
			recoveryCode_ = code;
			log_("account_recovery_code", {{"name", name_}}); // never the code itself
		}
		then();
	});
}

void Account::restore(const std::string& code, int64_t now)
{
	if (busy_ || signedIn_)
		return;
	now_ = now;
	const std::string clean = cleanCode(code);
	if (clean.empty())
	{
		error_ = "A recovery code is 20 letters and numbers, like A1231-E7BFE-AD218-4103A.";
		if (changed_)
			changed_();
		return;
	}
	busy_ = true;
	error_.clear();
	if (changed_)
		changed_();
	log_("account_restore", {{"hadSession", !refreshToken_.empty()}});
	const auto send = [this, clean] {
		online::HttpRequest r = rest("POST", "/rest/v1/rpc/restore_account");
		r.body = json{{"code", clean}}.dump();
		http_(r, [this, clean](int status, const std::string& body) {
			if (status < 200 || status >= 300 || !takeProfile(body))
				return fail(status, body, "restore");
			signedIn_ = true;
			offline_ = false;
			recoveryCode_ = clean;
			log_("account_restored", {{"name", name_}});
			finish();
		});
	};
	if (refreshToken_.empty())
		signUp(send);
	else
		withSession(now, send);
}

void Account::retry(int64_t now)
{
	if (busy_ || refreshToken_.empty())
		return;
	now_ = now;
	busy_ = true;
	error_.clear();
	if (changed_)
		changed_();
	refresh([this] { fetchProfile(); });
}

Account::View Account::view() const
{
	View v;
	v.state = busy_ ? State::Working : signedIn_ ? State::SignedIn : offline_ ? State::Offline : State::SignedOut;
	v.name = name_;
	v.friendCode = friendCode_;
	v.recoveryCode = recoveryCode_;
	v.error = error_;
	return v;
}

online::HttpRequest Account::auth(const std::string& path) const
{
	online::HttpRequest r;
	r.url = std::string(online::kServiceUrl) + path;
	r.headers["apikey"] = online::kPublishableKey;
	r.headers["Content-Type"] = "application/json";
	return r;
}

online::HttpRequest Account::rest(const std::string& method, const std::string& path) const
{
	online::HttpRequest r = auth(path);
	r.method = method;
	r.headers["Authorization"] = "Bearer " + accessToken_;
	r.headers["Prefer"] = "return=representation";
	return r;
}

void Account::signUp(std::function<void()> then)
{
	// An anonymous user: no email or password; the refresh token kept on this PC is the account.
	online::HttpRequest r = auth("/auth/v1/signup");
	r.method = "POST";
	r.body = "{}";
	http_(r, [this, then](int status, const std::string& body) {
		const json j = parse(body);
		if (status < 200 || status >= 300 || text(j, "access_token").empty() || text(j, "refresh_token").empty())
			return fail(status, body, "sign up");
		accessToken_ = text(j, "access_token");
		refreshToken_ = text(j, "refresh_token");
		expiresAt_ = now_ + j.value("expires_in", 3600);
		userId_ = j.contains("user") ? text(j["user"], "id") : "";
		log_("account_signed_up", {{"userId", userId_}});
		if (changed_)
			changed_(); // saved now, so a failed name doesn't make another user next time
		then();
	});
}

void Account::withSession(int64_t now, std::function<void()> then, std::function<void()> failed)
{
	now_ = now;
	if (!accessToken_.empty() && now + 60 < expiresAt_)
		then();
	else
		refresh(std::move(then), std::move(failed));
}

void Account::send(online::HttpRequest request, int64_t now, online::HttpDone done)
{
	if (!signedIn_)
	{
		done(0, "");
		return;
	}
	withSession(now, [this, request, done]() mutable {
		const online::HttpRequest base = rest(request.method, "");
		request.url = base.url + request.url; // the path given
		for (const auto& [k, v] : base.headers)
			request.headers.emplace(k, v); // the caller's own (e.g. Prefer) win
		http_(request, done);
	}, [done] { done(0, ""); });
}

void Account::setShareActivity(bool on)
{
	if (on == shareActivity_)
		return;
	shareActivity_ = on;
	log_("account_share_activity", {{"on", on}});
	if (changed_)
		changed_();
}

void Account::refresh(std::function<void()> then, std::function<void()> failed)
{
	online::HttpRequest r = auth("/auth/v1/token?grant_type=refresh_token");
	r.method = "POST";
	r.body = json{{"refresh_token", refreshToken_}}.dump();
	http_(r, [this, then, failed](int status, const std::string& body) {
		const json j = parse(body);
		if (status < 200 || status >= 300 || text(j, "access_token").empty())
		{
			fail(status, body, "refresh");
			if (failed)
				failed();
			return;
		}
		accessToken_ = text(j, "access_token");
		if (!text(j, "refresh_token").empty())
			refreshToken_ = text(j, "refresh_token"); // the service hands out a new one each time
		expiresAt_ = now_ + j.value("expires_in", 3600);
		if (j.contains("user"))
			userId_ = text(j["user"], "id");
		offline_ = false;
		if (changed_)
			changed_();
		then();
	});
}

bool Account::takeProfile(const std::string& body)
{
	const json j = parse(body);
	const json& p = j.is_array() && !j.empty() ? j[0] : j;
	if (!p.is_object() || text(p, "name").empty())
		return false;
	name_ = text(p, "name");
	friendCode_ = text(p, "friend_code");
	return true;
}

void Account::fetchProfile()
{
	http_(rest("GET", "/rest/v1/profiles?select=name,friend_code&id=eq." + userId_), [this](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			return fail(status, body, "profile");
		if (!takeProfile(body))
		{
			// Signed in, but no name yet (a name that was taken last time): Pick a name again.
			name_.clear();
			friendCode_.clear();
			log_("account_needs_name", {{"userId", userId_}});
			finish();
			return;
		}
		signedIn_ = true;
		log_("account_signed_in", {{"name", name_}});
		touchLastSeen();
		finish();
	});
}

void Account::insertProfile(const std::string& name)
{
	online::HttpRequest r = rest("POST", "/rest/v1/profiles");
	r.body = json{{"id", userId_}, {"name", name}}.dump();
	http_(r, [this](int status, const std::string& body) {
		if (status < 200 || status >= 300 || !takeProfile(body))
			return fail(status, body, "create profile");
		signedIn_ = true;
		log_("account_created", {{"name", name_}, {"friendCode", friendCode_}});
		requestCode([this] { finish(); });
	});
}

void Account::touchLastSeen()
{
	online::HttpRequest r = rest("PATCH", "/rest/v1/profiles?id=eq." + userId_);
	r.headers["Prefer"] = "return=minimal";
	r.body = json{{"last_seen", "now"}}.dump(); // Postgres reads "now" as the current time
	http_(r, [](int, const std::string&) {}); // best effort
}

void Account::fail(int status, const std::string& body, const std::string& what)
{
	const json j = parse(body);
	const std::string code = text(j, "code") + text(j, "error_code");
	const std::string message = text(j, "message") + text(j, "msg") + text(j, "error_description");
	if (code == "P0002")
		error_ = "No account has that recovery code. Check it and try again.";
	else if (code == "P0003")
		error_ = "This PC already has an account here. Sign out first.";
	else if (status == 0 || status == 418 || status >= 500)
	{
		error_ = "Can't reach the Free Touch service right now. Your training works as usual.";
		offline_ = !name_.empty();
	}
	else if (code.find("23505") != std::string::npos || status == 409)
		error_ = "That name is taken. Try another.";
	else if (code.find("23514") != std::string::npos)
		error_ = "Use 3 to 20 letters, numbers, spaces, dots, dashes and underscores.";
	else if (code.find("anonymous_provider_disabled") != std::string::npos || message.find("nonymous") != std::string::npos)
		error_ = "Accounts aren't switched on on the server yet (anonymous sign-ins).";
	else if (what == "refresh" && (status == 400 || status == 401 || status == 403))
	{
		// The saved session is no longer valid: start again.
		error_ = "Your sign-in has run out. Pick a name to make a new account.";
		userId_.clear();
		accessToken_.clear();
		refreshToken_.clear();
		name_.clear();
		friendCode_.clear();
	}
	else
		error_ = "Something went wrong (" + what + ", " + std::to_string(status) + ").";
	log_("account_error", {{"what", what}, {"status", status}, {"code", code}, {"message", message.substr(0, 200)}});
	finish();
}

void Account::finish()
{
	busy_ = false;
	if (changed_)
		changed_();
}
