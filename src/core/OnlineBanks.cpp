#include "pch.h"
#include "core/OnlineBanks.h"

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

	// Five of S and X, or nothing.
	std::string results(const json& row)
	{
		std::string r = text(row, "results");
		const bool fine = r.size() == 5 && r.find_first_not_of("SX") == std::string::npos;
		return fine ? r : std::string();
	}
}

std::vector<OnlineBanks::Challenge> OnlineBanks::parseInbox(const std::string& body)
{
	std::vector<Challenge> out;
	const json j = parse(body);
	if (!j.is_array())
		return out;
	for (const auto& row : j)
	{
		Challenge c;
		c.id = text(row, "challenge");
		c.from = text(row, "from_name");
		c.ageSeconds = number(row, "age_seconds", -1);
		c.results = results(row);
		if (!c.id.empty())
			out.push_back(std::move(c));
	}
	return out;
}

std::vector<OnlineBanks::Sent> OnlineBanks::parseSent(const std::string& body)
{
	std::vector<Sent> out;
	const json j = parse(body);
	if (!j.is_array())
		return out;
	for (const auto& row : j)
	{
		Sent s;
		s.id = text(row, "challenge");
		s.bank = text(row, "bank");
		s.to = text(row, "to_name");
		s.ageSeconds = number(row, "age_seconds", -1);
		s.results = results(row);
		s.playedAgo = number(row, "played_seconds", -1);
		if (!s.id.empty())
			out.push_back(std::move(s));
	}
	return out;
}

void OnlineBanks::rpc(const std::string& name, const json& args, int64_t now, online::HttpDone done)
{
	online::HttpRequest r;
	r.method = "POST";
	r.url = "/rest/v1/rpc/" + name;
	r.body = args.dump();
	account_.send(r, now, std::move(done));
}

void OnlineBanks::send(const std::string& key, const json& bank, const std::vector<std::string>& friendIds,
	const std::string& toNames, int64_t now)
{
	if (!account_.signedIn() || view_.busy || friendIds.empty())
		return;
	view_.busy = true;
	view_.message.clear();
	view_.error.clear();
	changed();
	const json args = {{"key", key}, {"bank", bank}};
	log_("bank_upload", {{"friends", friendIds.size()}, {"bytes", bank.dump().size()}});
	rpc("put_shot_bank", args, now, [this, friendIds, toNames, now](int status, const std::string& body) {
		const json j = parse(body);
		if (status < 200 || status >= 300 || !j.is_string())
		{
			view_.busy = false;
			return failed(status, body, "upload");
		}
		const std::string id = j.get<std::string>();
		rpc("send_shot_bank", {{"bank", id}, {"friends", friendIds}}, now, [this, id, toNames, now](int s2, const std::string& b2) {
			view_.busy = false;
			if (s2 < 200 || s2 >= 300)
				return failed(s2, b2, "send");
			const json got = parse(b2);
			view_.message = got.is_number() && got.get<int>() == 0 ? "They already have this bank."
				: "Sent your Shot Bank to " + toNames + ".";
			log_("bank_sent", {{"id", id}, {"to", toNames}, {"got", got}});
			refresh(now);
			changed();
		});
	});
}

void OnlineBanks::refresh(int64_t now)
{
	if (!account_.signedIn())
		return;
	rpc("bank_inbox", json::object(), now, [this](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			return; // quietly: the next check tries again
		const auto inbox = parseInbox(body);
		int waiting = 0;
		for (const auto& c : inbox)
		{
			waiting += c.results.empty();
			const bool known = std::any_of(view_.inbox.begin(), view_.inbox.end(), [&](const Challenge& o) { return o.id == c.id; });
			if (view_.loaded && !known && c.results.empty() && newChallenge_)
				newChallenge_(c.from);
		}
		view_.inbox = inbox;
		view_.waiting = waiting;
		view_.loaded = true;
		changed();
	});
	rpc("bank_sent", json::object(), now, [this](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			return;
		view_.sent = parseSent(body);
		changed();
	});
}

void OnlineBanks::fetch(const std::string& challenge, int64_t now, Fetched done)
{
	if (!account_.signedIn())
	{
		done(json(), "", "", "Sign in first.");
		return;
	}
	view_.busy = true;
	view_.error.clear();
	changed();
	rpc("get_bank_challenge", {{"challenge", challenge}}, now, [this, done](int status, const std::string& body) {
		view_.busy = false;
		const json j = parse(body);
		if (status < 200 || status >= 300 || !j.is_array() || j.empty() || !j[0].is_object() || !j[0].contains("bank"))
		{
			failed(status, body, "fetch");
			done(json(), "", "", view_.error.empty() ? "That bank is gone." : view_.error);
			return;
		}
		changed();
		done(j[0]["bank"], text(j[0], "from_name"), results(j[0]), "");
	});
}

void OnlineBanks::finish(const std::string& challenge, const std::string& results, int64_t now)
{
	if (!account_.signedIn())
		return;
	rpc("finish_bank_challenge", {{"challenge", challenge}, {"results", results}}, now,
		[this, challenge, results, now](int status, const std::string& body) {
			if (status < 200 || status >= 300)
				return failed(status, body, "result");
			log_("bank_result_sent", {{"challenge", challenge}, {"results", results}, {"counted", parse(body)}});
			refresh(now);
		});
}

void OnlineBanks::remove(const std::string& challenge, int64_t now)
{
	if (!account_.signedIn())
		return;
	online::HttpRequest r;
	r.method = "DELETE";
	r.url = "/rest/v1/bank_challenges?id=eq." + challenge;
	account_.send(r, now, [this, now](int status, const std::string& body) {
		if (status < 200 || status >= 300)
			return failed(status, body, "remove");
		refresh(now);
	});
}

void OnlineBanks::clear()
{
	view_ = View{};
	changed();
}

void OnlineBanks::failed(int status, const std::string& body, const std::string& what)
{
	const json j = parse(body);
	const std::string code = text(j, "code");
	if (status == 0 || status == 418 || status >= 500)
		view_.error = "Can't reach the Free Touch service right now. Your training works as usual.";
	else
		view_.error = "Something went wrong (" + what + ", " + std::to_string(status) + ").";
	log_("banks_error", {{"what", what}, {"status", status}, {"code", code}});
	changed();
}
