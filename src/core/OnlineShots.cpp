#include "pch.h"
#include "core/OnlineShots.h"

#include <algorithm>

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

	std::string text(const json& row, const char* key)
	{
		return row.is_object() && row.contains(key) && row[key].is_string() ? row[key].get<std::string>() : std::string();
	}

	int number(const json& row, const char* key, int fallback = 0)
	{
		return row.is_object() && row.contains(key) && row[key].is_number() ? row[key].get<int>() : fallback;
	}
}

std::vector<OnlineShots::InboxItem> OnlineShots::parseInbox(const std::string& body)
{
	std::vector<InboxItem> out;
	const json j = parse(body);
	if (!j.is_array())
		return out;
	for (const auto& row : j)
	{
		InboxItem i;
		i.delivery = text(row, "delivery");
		i.shotId = text(row, "shot");
		i.name = text(row, "name");
		i.key = text(row, "key");
		i.from = text(row, "from_name");
		i.ageSeconds = number(row, "age_seconds", -1);
		if (!i.delivery.empty() && !i.shotId.empty())
			out.push_back(std::move(i));
	}
	return out;
}

std::vector<OnlineShots::BoardItem> OnlineShots::parseBoard(const std::string& body)
{
	std::vector<BoardItem> out;
	const json j = parse(body);
	if (!j.is_array())
		return out;
	for (const auto& row : j)
	{
		BoardItem b;
		b.id = text(row, "id");
		b.name = text(row, "name");
		b.key = text(row, "key");
		b.owner = text(row, "owner_name");
		b.mine = row.value("mine", false);
		b.isPublic = row.value("public", false);
		b.liked = row.value("liked", false);
		b.ageSeconds = number(row, "age_seconds", -1);
		b.likes = number(row, "likes");
		b.playsWeek = number(row, "plays_week");
		if (!b.id.empty())
			out.push_back(std::move(b));
	}
	return out;
}

std::string OnlineShots::ftshotFromRow(const std::string& body)
{
	const json j = parse(body);
	const json& row = j.is_array() && !j.empty() ? j[0] : j;
	if (!row.is_object() || !row.contains("shot") || !row["shot"].is_object())
		return "";
	const std::string created = text(row, "created_at");
	json doc = {{"freeTouchShot", 1}, {"name", text(row, "name")}, {"by", text(row, "owner_name")},
		{"on", created.substr(0, 10)}, {"key", text(row, "key")}, {"shot", row["shot"]}};
	return doc.dump();
}

void OnlineShots::rpc(const std::string& name, const json& args, int64_t now, online::HttpDone done)
{
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/rpc/" + name;
	r.body = args.dump();
	account_.send(r, now, std::move(done));
}

void OnlineShots::post(const json& ftshot, bool makePublic, const std::vector<std::string>& friendIds, const std::string& toNames,
	int64_t now)
{
	if (!account_.signedIn() || view_.busy || !ftshot.contains("shot"))
		return;
	view_.busy = true;
	view_.message.clear();
	view_.error.clear();
	changed();
	const std::string name = ftshot.value("name", "Shot").substr(0, 60);
	const json args = {{"name", name.empty() ? "Shot" : name}, {"key", ftshot.value("key", "")}, {"shot", ftshot["shot"]},
		{"make_public", makePublic}};
	log_("shot_upload", {{"name", name}, {"public", makePublic}, {"friends", friendIds.size()}, {"bytes", ftshot["shot"].dump().size()}});
	rpc("put_shot", args, now, [this, makePublic, friendIds, toNames, name, now](int status, const std::string& body) {
		const json j = parse(body);
		if (status < 200 || status >= 300 || !j.is_string())
		{
			view_.busy = false;
			return failed(status, body, "upload");
		}
		const std::string id = j.get<std::string>();
		if (friendIds.empty())
		{
			view_.busy = false;
			view_.message = "\"" + name + "\" is on the Shot Board.";
			log_("shot_posted", {{"id", id}, {"public", makePublic}});
			changed();
			return;
		}
		rpc("send_shot", {{"shot", id}, {"friends", friendIds}}, now, [this, id, toNames, name, makePublic](int s2, const std::string& b2) {
			view_.busy = false;
			if (s2 < 200 || s2 >= 300)
				return failed(s2, b2, "send");
			view_.message = "Sent \"" + name + "\" to " + toNames + (makePublic ? " and posted it on the Shot Board." : ".");
			log_("shot_sent", {{"id", id}, {"to", toNames}, {"got", parse(b2)}});
			changed();
		});
	});
}

void OnlineShots::refreshInbox(int64_t now)
{
	if (!account_.signedIn())
		return;
	rpc("shot_inbox", json::object(), now, [this](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			return; // quietly: the inbox is a bonus; the next check tries again
		view_.inbox = parseInbox(body);
		changed();
	});
}

void OnlineShots::refreshBoard(const std::string& sort, int64_t now)
{
	if (!account_.signedIn())
		return;
	view_.sort = sort == "top" || sort == "mine" ? sort : "new";
	view_.busy = true;
	changed();
	rpc("shot_board", {{"sort", view_.sort}}, now, [this, sort = view_.sort](int status, const std::string& body) {
		view_.busy = false;
		if (status < 200 || status >= 300)
			return failed(status, body, "board");
		if (sort != view_.sort)
			return; // another tab was picked meanwhile
		view_.board = parseBoard(body);
		view_.boardLoaded = true;
		view_.error.clear();
		changed();
	});
}

void OnlineShots::fetch(const std::string& shotId, int64_t now, Fetched done)
{
	if (!account_.signedIn())
	{
		done("", "Sign in first (Settings > Account).");
		return;
	}
	view_.busy = true;
	changed();
	rpc("get_shot", {{"id", shotId}}, now, [this, shotId, done](int status, const std::string& body) {
		view_.busy = false;
		changed();
		const std::string doc = status >= 200 && status < 300 ? ftshotFromRow(body) : std::string();
		log_("shot_fetched", {{"id", shotId}, {"status", status}, {"ok", !doc.empty()}, {"bytes", body.size()}});
		done(doc, doc.empty() ? (status == 0 || status >= 500 ? "Can't reach the Free Touch service right now."
			: "That shot isn't there any more.") : std::string());
	});
}

void OnlineShots::dismiss(const std::string& delivery, int64_t now)
{
	view_.inbox.erase(std::remove_if(view_.inbox.begin(), view_.inbox.end(), [&](const InboxItem& i) { return i.delivery == delivery; }),
		view_.inbox.end());
	changed();
	online::HttpRequest r;
	r.method = "DELETE";
	r.url = "/rest/v1/shot_deliveries?id=eq." + delivery;
	r.headers["Prefer"] = "return=minimal";
	account_.send(r, now, [this, delivery](int status, const std::string&) { log_("shot_dismissed", {{"delivery", delivery}, {"status", status}}); });
}

void OnlineShots::like(const std::string& shotId, bool on, int64_t now)
{
	for (auto& b : view_.board)
		if (b.id == shotId && b.liked != on)
		{
			b.liked = on;
			b.likes += on ? 1 : -1;
		}
	changed();
	rpc("like_shot", {{"shot", shotId}, {"on_", on}}, now, [this, shotId, on](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			failed(status, body, "like");
		log_("shot_liked", {{"id", shotId}, {"on", on}, {"status", status}});
	});
}

void OnlineShots::report(const std::string& shotId, int64_t now)
{
	view_.board.erase(std::remove_if(view_.board.begin(), view_.board.end(), [&](const BoardItem& b) { return b.id == shotId; }),
		view_.board.end());
	view_.message = "Reported. Thanks: it's hidden for you, and three reports take it off the board.";
	changed();
	rpc("report_shot", {{"shot", shotId}, {"reason", "reported in game"}}, now,
		[this, shotId](int status, const std::string&) { log_("shot_reported", {{"id", shotId}, {"status", status}}); });
}

void OnlineShots::setPublic(const std::string& shotId, bool on, int64_t now)
{
	for (auto& b : view_.board)
		if (b.id == shotId)
			b.isPublic = on;
	changed();
	online::HttpRequest r;
	r.method = "PATCH";
	r.url = "/rest/v1/shots?id=eq." + shotId;
	r.headers["Prefer"] = "return=minimal";
	r.body = json{{"public", on}}.dump();
	account_.send(r, now, [this, shotId, on](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			failed(status, body, "public");
		log_("shot_public", {{"id", shotId}, {"on", on}, {"status", status}});
	});
}

void OnlineShots::remove(const std::string& shotId, int64_t now)
{
	view_.board.erase(std::remove_if(view_.board.begin(), view_.board.end(), [&](const BoardItem& b) { return b.id == shotId; }),
		view_.board.end());
	changed();
	online::HttpRequest r;
	r.method = "DELETE";
	r.url = "/rest/v1/shots?id=eq." + shotId;
	r.headers["Prefer"] = "return=minimal";
	account_.send(r, now, [this, shotId](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			failed(status, body, "delete");
		log_("shot_deleted_online", {{"id", shotId}, {"status", status}});
	});
}

void OnlineShots::played(const std::string& shotId, int64_t now)
{
	rpc("play_shot", {{"shot", shotId}}, now, [this, shotId](int status, const std::string&) {
		log_("shot_played_online", {{"id", shotId}, {"status", status}});
	});
}

void OnlineShots::clear()
{
	view_ = View{};
	changed();
}

void OnlineShots::failed(int status, const std::string& body, const std::string& what)
{
	const json j = parse(body);
	const std::string code = text(j, "code");
	if (code == "P0007")
		view_.error = "You have 50 shots online. Delete some under Shot Board > Mine first.";
	else if (status == 0 || status == 418 || status >= 500)
		view_.error = "Can't reach the Free Touch service right now. Your training works as usual.";
	else
		view_.error = "Something went wrong (" + what + ", " + std::to_string(status) + ").";
	log_("shots_error", {{"what", what}, {"status", status}, {"code", code}});
	changed();
}
