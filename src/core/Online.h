#pragma once

#include <functional>
#include <map>
#include <string>

// Free Touch's online service (Phase 8, Supabase): where it is, and one HTTP request as the game adapter sends it.
// No SDK calls.
namespace online
{
	// The project and its publishable key (public by design: what a signed-in player may do is up to the database's
	// row-level security, supabase/migrations/).
	constexpr const char* kServiceUrl = "https://jkpkhwyzvquuwtzuozfh.supabase.co";
	constexpr const char* kPublishableKey = "sb_publishable_MF75UYGnwpnjQaUm_m3VKw_SQAOToT2";
	// Lambourne's Ko-fi page (2026-10-30): Settings > Support and Home open it in the browser. Donors who put their
	// friend code in the message get a heart by their name (tools/supporter.py, migrations/0012_supporters.sql).
	constexpr const char* kSupportUrl = "https://ko-fi.com/peytonlambourne";

	struct HttpRequest
	{
		std::string method = "GET";
		std::string url;
		std::map<std::string, std::string> headers;
		std::string body;
	};

	// The reply: an HTTP status (0 = no reply: offline, refused, or blocked during an online match) and its body.
	using HttpDone = std::function<void(int status, const std::string& body)>;
	using HttpFn = std::function<void(const HttpRequest& request, HttpDone done)>;
}
