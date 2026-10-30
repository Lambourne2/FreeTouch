#include "pch.h"
#include "core/Leaderboards.h"

#include <algorithm>
#include <cmath>

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
}

std::vector<Leaderboards::Row> Leaderboards::parseRows(const std::string& body, int& players)
{
	std::vector<Row> out;
	players = 0;
	const json j = parse(body);
	if (!j.is_array())
		return out;
	for (const auto& r : j)
	{
		if (!r.is_object())
			continue;
		Row row;
		row.rank = r.value("rank", 0);
		row.name = r.value("name", "");
		row.points = r.value("points", 0);
		row.hits = r.value("hits", 0);
		row.marks = r.value("marks", "");
		row.me = r.value("is_me", false);
		row.isFriend = r.value("is_friend", false);
		players = std::max(players, r.value("players", 0));
		out.push_back(std::move(row));
	}
	return out;
}

void Leaderboards::postDaily(const std::string& day, int number, int points, int hits, const std::string& marks, int64_t now)
{
	if (!account_.signedIn() || posting_ || view_.postedDay == day || marks.empty())
		return;
	posting_ = true;
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/daily_scores";
	r.headers["Prefer"] = "return=minimal";
	r.body = json{{"user_id", account_.userId()}, {"day", day}, {"number", number}, {"points", points}, {"hits", hits},
		{"marks", marks.substr(0, 10)}}.dump();
	account_.send(r, now, [this, day, points, now](int status, const std::string& body) {
		posting_ = false;
		// 409: already there (the first one stands).
		const bool posted = (status >= 200 && status < 300) || status == 409;
		log_("daily_posted", {{"day", day}, {"points", points}, {"status", status}, {"posted", posted},
			{"error", posted ? "" : body.substr(0, 200)}});
		if (!posted && (status == 0 || status == 418 || status >= 500))
			return; // unreachable: tried again on the next check
		view_.postedDay = day; // posted, or refused for good (logged): not sent again
		refreshDaily(day, view_.daily.scope, now);
	});
}

void Leaderboards::refreshDaily(const std::string& day, const std::string& scope, int64_t now)
{
	if (!account_.signedIn() || day.empty())
		return;
	Board& b = view_.daily;
	if (b.day != day || b.scope != scope)
		b.loaded = false;
	b.day = day;
	b.scope = scope == "all" ? "all" : "friends";
	b.busy = true;
	changed();
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/rpc/daily_board";
	r.body = json{{"day", day}, {"scope", b.scope}}.dump();
	account_.send(r, now, [this, day, scope = b.scope](int status, const std::string& body) {
		Board& b = view_.daily;
		if (b.day != day || b.scope != scope)
			return; // another day or tab was asked for meanwhile
		b.busy = false;
		if (status < 200 || status >= 300)
		{
			b.error = status == 0 || status >= 500 ? "Can't reach the Free Touch service right now." : "Couldn't load the board.";
			log_("daily_board_error", {{"status", status}});
			changed();
			return;
		}
		b.rows = parseRows(body, b.players);
		b.loaded = true;
		b.error.clear();
		changed();
	});
}

std::vector<Leaderboards::RaceRow> Leaderboards::parseRaceRows(const std::string& body, int& players)
{
	std::vector<RaceRow> out;
	players = 0;
	const json j = parse(body);
	if (!j.is_array())
		return out;
	for (const auto& r : j)
	{
		if (!r.is_object())
			continue;
		RaceRow row;
		row.rank = r.value("rank", 0);
		row.player = r.value("player", "");
		row.name = r.value("name", "");
		row.seconds = r.contains("seconds") && r["seconds"].is_number() ? r["seconds"].get<double>() : 0.0;
		row.me = r.value("is_me", false);
		row.isFriend = r.value("is_friend", false);
		row.hasGhost = r.value("has_ghost", false);
		players = std::max(players, r.value("players", 0));
		out.push_back(std::move(row));
	}
	return out;
}

std::map<std::string, Leaderboards::Leader> Leaderboards::parseLeaders(const std::string& body)
{
	std::map<std::string, Leader> out;
	const json j = parse(body);
	if (!j.is_array())
		return out;
	for (const auto& r : j)
	{
		if (!r.is_object() || !r.contains("race") || !r["race"].is_string())
			continue;
		Leader l;
		l.name = r.value("name", "");
		l.seconds = r.contains("seconds") && r["seconds"].is_number() ? r["seconds"].get<double>() : 0.0;
		l.me = r.value("is_me", false);
		out[r["race"].get<std::string>()] = l;
	}
	return out;
}

void Leaderboards::postRace(const std::string& race, double seconds, const json& ghost, int64_t now)
{
	if (!account_.signedIn() || race.empty() || seconds <= 0.3)
		return;
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/rpc/post_race_time";
	r.body = json{{"race", race.substr(0, 80)}, {"seconds", std::round(seconds * 1000.0) / 1000.0},
		{"ghost", ghost.is_object() ? ghost : json()}}.dump();
	account_.send(r, now, [this, race, seconds, now](int status, const std::string& body) {
		const bool kept = status >= 200 && status < 300 && parse(body) == json(true);
		log_("race_posted", {{"race", race}, {"seconds", seconds}, {"status", status}, {"kept", kept}});
		if (kept)
			refreshRaceLeaders(now);
	});
}

void Leaderboards::refreshRaceLeaders(int64_t now)
{
	if (!account_.signedIn())
		return;
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/rpc/race_leaders";
	r.body = json{{"scope", "friends"}}.dump();
	account_.send(r, now, [this](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			return; // quietly: the cards show your own best meanwhile
		view_.raceLeaders = parseLeaders(body);
		changed();
	});
}

void Leaderboards::refreshRace(const std::string& race, const std::string& scope, int64_t now)
{
	if (!account_.signedIn() || race.empty())
		return;
	RaceBoard& b = view_.race;
	if (b.race != race || b.scope != scope)
		b.loaded = false;
	b.race = race;
	b.scope = scope == "all" ? "all" : "friends";
	b.busy = true;
	changed();
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/rpc/race_board";
	r.body = json{{"race", race}, {"scope", b.scope}}.dump();
	account_.send(r, now, [this, race, scope = b.scope](int status, const std::string& body) {
		RaceBoard& b = view_.race;
		if (b.race != race || b.scope != scope)
			return;
		b.busy = false;
		if (status < 200 || status >= 300)
		{
			b.error = status == 0 || status >= 500 ? "Can't reach the Free Touch service right now." : "Couldn't load the board.";
			changed();
			return;
		}
		b.rows = parseRaceRows(body, b.players);
		b.loaded = true;
		b.error.clear();
		changed();
	});
}

void Leaderboards::fetchGhost(const std::string& race, const std::string& player, int64_t now, GhostFetched done)
{
	if (!account_.signedIn())
	{
		done(json(), "Sign in first (Settings > Account).");
		return;
	}
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/rpc/race_ghost";
	r.body = json{{"race", race}, {"player", player}}.dump();
	account_.send(r, now, [this, race, player, done](int status, const std::string& body) {
		const json g = status >= 200 && status < 300 ? parse(body) : json();
		log_("race_ghost_fetched", {{"race", race}, {"player", player}, {"status", status}, {"bytes", body.size()}});
		if (!g.is_object() || !g.contains("frames"))
			done(json(), status == 0 || status >= 500 ? "Can't reach the Free Touch service right now." : "That ghost isn't there any more.");
		else
			done(g, "");
	});
}

void Leaderboards::fetchMyDaily(const std::string& day, int64_t now, MyDaily done)
{
	if (!account_.signedIn())
	{
		done(false, false, 0, 0, "");
		return;
	}
	online::HttpRequest r;
	r.url = "/rest/v1/daily_scores?select=number,points,marks&day=eq." + day + "&user_id=eq." + account_.userId();
	account_.send(r, now, [this, day, done](int status, const std::string& body) {
		if (status < 200 || status >= 300)
		{
			done(false, false, 0, 0, "");
			return;
		}
		const json j = parse(body);
		if (!j.is_array() || j.empty() || !j[0].is_object())
		{
			done(true, false, 0, 0, "");
			return;
		}
		view_.postedDay = day; // it's there: not posted again
		done(true, true, j[0].value("number", 0), j[0].value("points", 0), j[0].value("marks", ""));
	});
}

void Leaderboards::clear()
{
	view_ = View{};
	posting_ = false;
	changed();
}
