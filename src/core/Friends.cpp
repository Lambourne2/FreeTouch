#include "pch.h"
#include "core/Friends.h"

#include <algorithm>
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

	int number(const json& row, const char* key)
	{
		return row.contains(key) && row[key].is_number() ? row[key].get<int>() : -1;
	}

	std::string text(const json& row, const char* key)
	{
		return row.contains(key) && row[key].is_string() ? row[key].get<std::string>() : std::string();
	}
}

std::vector<Friends::Entry> Friends::parseList(const std::string& body)
{
	std::vector<Entry> out;
	const json j = parse(body);
	if (!j.is_array())
		return out;
	for (const auto& row : j)
	{
		if (!row.is_object() || text(row, "id").empty())
			continue;
		Entry e;
		e.id = text(row, "id");
		e.name = text(row, "name");
		e.friendCode = text(row, "friend_code");
		e.state = text(row, "state");
		e.online = row.value("online", false);
		e.seenAgo = number(row, "seen_ago_seconds");
		e.weekMinutes = number(row, "week_minutes");
		e.todayMinutes = number(row, "today_minutes");
		e.streakDays = number(row, "streak_days");
		e.lastDrill = text(row, "last_drill");
		e.trainedAgo = number(row, "trained_ago_seconds");
		out.push_back(std::move(e));
	}
	// Friends (online first), then requests you got, then the ones you sent; by name within each.
	const auto rank = [](const Entry& e) { return e.state == "friend" ? (e.online ? 0 : 1) : e.state == "incoming" ? 2 : 3; };
	std::stable_sort(out.begin(), out.end(), [&](const Entry& a, const Entry& b) {
		if (rank(a) != rank(b))
			return rank(a) < rank(b);
		std::string x = a.name, y = b.name;
		std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return x < y;
	});
	return out;
}

std::string Friends::ago(int seconds)
{
	if (seconds < 0)
		return "";
	if (seconds < 90)
		return "just now";
	if (seconds < 3600)
		return std::to_string(seconds / 60) + " min ago";
	if (seconds < 2 * 86400)
		return std::to_string(seconds / 3600) + " h ago";
	return std::to_string(seconds / 86400) + " days ago";
}

void Friends::rpc(const std::string& name, const json& args, int64_t now, std::function<void(int, const std::string&)> done)
{
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/rpc/" + name;
	r.body = args.dump();
	account_.send(r, now, std::move(done));
}

void Friends::refresh(int64_t now)
{
	if (!account_.signedIn() || view_.busy)
		return;
	view_.busy = true;
	if (changed_)
		changed_();
	rpc("friends_list", json::object(), now, [this](int status, const std::string& body) {
		view_.busy = false;
		if (status < 200 || status >= 300)
			return failed(status, body, "list");
		view_.list = parseList(body);
		view_.loaded = true;
		view_.error.clear();
		view_.incoming = static_cast<int>(std::count_if(view_.list.begin(), view_.list.end(),
			[](const Entry& e) { return e.state == "incoming"; }));
		if (changed_)
			changed_();
	});
}

void Friends::add(const std::string& who, int64_t now)
{
	size_t a = 0, b = who.size();
	while (a < b && std::isspace(static_cast<unsigned char>(who[a])))
		++a;
	while (b > a && std::isspace(static_cast<unsigned char>(who[b - 1])))
		--b;
	const std::string wanted = who.substr(a, b - a);
	if (wanted.empty() || !account_.signedIn() || view_.busy)
		return;
	view_.busy = true;
	view_.message.clear();
	view_.error.clear();
	if (changed_)
		changed_();
	log_("friend_add", {{"who", wanted}});
	rpc("add_friend", {{"who", wanted}}, now, [this, now, wanted](int status, const std::string& body) {
		view_.busy = false;
		const json j = parse(body);
		if (status < 200 || status >= 300 || !j.is_string())
			return failed(status, body, "add");
		const std::string result = j.get<std::string>();
		view_.message = result == "accepted" ? "You and " + wanted + " are friends now!"
			: result == "friends" ? "You're already friends with " + wanted + "."
			: result == "sent" ? "You've already asked " + wanted + "; it's up to them now."
			: "Request sent to " + wanted + ".";
		log_("friend_added", {{"who", wanted}, {"result", result}});
		refresh(now);
	});
}

void Friends::respond(const std::string& id, bool accept, int64_t now)
{
	if (!account_.signedIn() || view_.busy)
		return;
	view_.busy = true;
	view_.message.clear();
	view_.error.clear();
	if (changed_)
		changed_();
	rpc("respond_friend", {{"other", id}, {"accept", accept}}, now, [this, id, accept, now](int status, const std::string& body) {
		view_.busy = false;
		if (status < 200 || status >= 300)
			return failed(status, body, accept ? "accept" : "decline");
		log_("friend_responded", {{"id", id}, {"accept", accept}});
		refresh(now);
	});
}

void Friends::remove(const std::string& id, int64_t now)
{
	if (!account_.signedIn() || view_.busy)
		return;
	view_.busy = true;
	view_.message.clear();
	view_.error.clear();
	if (changed_)
		changed_();
	rpc("remove_friend", {{"other", id}}, now, [this, id, now](int status, const std::string& body) {
		view_.busy = false;
		if (status < 200 || status >= 300)
			return failed(status, body, "remove");
		log_("friend_removed", {{"id", id}});
		refresh(now);
	});
}

void Friends::sendActivity(const Activity& a, int64_t now)
{
	if (!account_.signedIn() || !account_.shareActivity())
		return;
	json row = {{"user_id", account_.userId()}, {"week_minutes", std::clamp(a.weekMinutes, 0, 10080)},
		{"today_minutes", std::clamp(a.todayMinutes, 0, 1440)}, {"streak_days", std::max(0, a.streakDays)},
		{"last_drill", a.lastDrill.substr(0, 80)}, {"updated_at", "now"}};
	if (a.trainedSinceLast)
		row["last_trained_at"] = "now"; // Postgres reads "now" as the current time
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/activity?on_conflict=user_id";
	r.headers["Prefer"] = "resolution=merge-duplicates,return=minimal";
	r.body = row.dump();
	account_.send(r, now, [this, week = a.weekMinutes](int status, const std::string&) {
		log_("activity_sent", {{"status", status}, {"weekMinutes", week}});
	});
}

void Friends::stopSharing(int64_t now)
{
	if (!account_.signedIn())
		return;
	online::HttpRequest r;
	r.method = "DELETE";
	r.url = "/rest/v1/activity?user_id=eq." + account_.userId();
	r.headers["Prefer"] = "return=minimal";
	account_.send(r, now, [this](int status, const std::string&) { log_("activity_deleted", {{"status", status}}); });
}

void Friends::clear()
{
	view_ = View{};
	if (changed_)
		changed_();
}

void Friends::failed(int status, const std::string& body, const std::string& what)
{
	const json j = parse(body);
	const std::string code = j.is_object() ? text(j, "code") : std::string();
	if (code == "P0004")
		view_.error = "No player has that friend code or name.";
	else if (code == "P0005")
		view_.error = "That's you!";
	else if (code == "P0006")
		view_.error = "That's a lot of friends and requests. Remove some first.";
	else if (status == 0 || status == 418 || status >= 500)
		view_.error = "Can't reach the Free Touch service right now. Your training works as usual.";
	else
		view_.error = "Something went wrong (" + what + ", " + std::to_string(status) + ").";
	log_("friends_error", {{"what", what}, {"status", status}, {"code", code}});
	if (changed_)
		changed_();
}
