// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Network trust boundary for the optional online motion artwork lookup.
//
// Every URL Tonearm requests, and every redirect it is asked to follow, goes
// through these checks first. They are portable (no WinHTTP) so they can be
// unit tested on every platform; the Windows HTTP client applies them.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace tonearm {

struct HttpsUrl {
	std::string host;   // lower case
	std::string target; // path plus query, always starts with '/'
};

// Parses an absolute https URL strictly: scheme https, a plain DNS host name,
// default port only (no explicit port other than 443), no user information,
// no backslashes, whitespace or control characters, at most 4096 bytes.
std::optional<HttpsUrl> parseHttpsUrl(const std::string &url);

// True when `host` is `domain` itself or one of its subdomains, matched on a
// label boundary ("evilapple.com" is not inside "apple.com").
bool hostWithinDomain(const std::string &host, const std::string &domain);

// Apple's media CDNs: apple.com and mzstatic.com, including subdomains.
bool trustedAppleMediaUrl(const std::string &url);

// Resolves a redirect Location against the URL that returned it: absolute
// https, protocol-relative, absolute path and relative path forms. Rejects
// plain http, other schemes, dot segments and anything parseHttpsUrl rejects.
std::optional<std::string> resolveRedirect(const std::string &base, const std::string &location);

// HTTP status codes that carry a Location to follow.
inline bool isRedirectStatus(unsigned status)
{
	return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Parses a Retry-After value given in seconds. HTTP dates are not used by the
// services Tonearm talks to and yield nullopt (the caller's own backoff applies).
std::optional<int> parseRetryAfterSeconds(const std::string &value);

// Process-wide, per-endpoint backoff for the optional online services. After
// network failures, 429 or 5xx answers, an endpoint is left alone for a while
// that doubles with each consecutive failure (with jitter), or for as long as
// Retry-After asks, up to a cap. Any answer that shows the service is up
// (including 404 "no artwork") resets it. Lookups during a cooldown fall back
// to the still cover; cached and local artwork keep working.
class EndpointBackoff {
public:
	using Clock = std::chrono::steady_clock;
	struct Config {
		std::chrono::seconds base{30};
		std::chrono::seconds cap{15 * 60};
		std::chrono::seconds retryAfterCap{60 * 60};
		double jitter = 0.2; // +/- fraction of the computed delay
	};

	EndpointBackoff();
	explicit EndpointBackoff(Config config, std::function<double()> random = {});

	// Shared by every Tonearm source in the process.
	static EndpointBackoff &shared();

	bool allowed(const std::string &endpoint, Clock::time_point now = Clock::now()) const;
	// Remaining cooldown, zero when allowed.
	Clock::duration remaining(const std::string &endpoint, Clock::time_point now = Clock::now()) const;

	// Outcome of one request. `status` 0 means no HTTP answer (DNS, connect, TLS,
	// timeout). Returns true when the endpoint entered (or extended) a cooldown.
	bool record(const std::string &endpoint, unsigned status, std::optional<int> retryAfterSeconds = std::nullopt,
		    Clock::time_point now = Clock::now());

	static bool isOutageStatus(unsigned status) { return status == 0 || status == 429 || status >= 500; }

private:
	struct State {
		int failures = 0;
		Clock::time_point until{};
	};
	Config config_;
	std::function<double()> random_; // uniform in [0, 1)
	mutable std::mutex mutex_;
	std::map<std::string, State> states_;
};

} // namespace tonearm
