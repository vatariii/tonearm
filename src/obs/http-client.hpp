// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Small synchronous HTTPS GET client for the optional online motion artwork
// lookup (Windows, WinHTTP). Runs only on the resolver worker thread.
//
// Trust boundary: WinHTTP's automatic redirects are turned off. A redirect is
// followed only after its target has been resolved and accepted by the same
// policy as the original URL, at most kMaxRedirects times. TLS certificate
// validation is left at WinHTTP's secure defaults.

#ifdef _WIN32

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace tonearm {

struct HttpGet {
	std::string url;
	// Every URL (the first and each redirect target) must pass this.
	std::function<bool(const std::string &)> allowUrl;
	const wchar_t *headers = nullptr; // extra request headers, CRLF separated
	std::size_t cap = 64 * 1024;       // maximum body size; larger answers fail
	int resolveMs = 2500, connectMs = 2500, sendMs = 4500, receiveMs = 4500;
	// When set, the body is streamed here instead of collected; returning false aborts.
	std::function<bool(const char *data, std::size_t size)> sink;
};

struct HttpResult {
	unsigned status = 0;          // 0 = no HTTP answer (DNS, connection, TLS, timeout, policy)
	bool ok = false;              // status 200 and the whole body was read within the cap
	bool rejected = false;        // a URL or redirect target failed the policy
	std::string body;             // empty when a sink was used
	std::optional<int> retryAfter; // seconds, from a 429/503 answer
	std::string finalUrl;
};

constexpr int kMaxRedirects = 3;

HttpResult httpGet(const HttpGet &request, const std::atomic<bool> &cancelled);

// "Tonearm/<version> (OBS Studio)" from the build's project version.
const wchar_t *tonearmUserAgent();

} // namespace tonearm

#endif
