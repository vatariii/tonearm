// SPDX-License-Identifier: GPL-2.0-or-later
#include "net_policy.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace tonearm {

namespace {

char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool startsWithNoCase(const std::string &s, const char *prefix)
{
	size_t i = 0;
	for (; prefix[i]; ++i)
		if (i >= s.size() || lower(s[i]) != prefix[i])
			return false;
	return true;
}

bool validHost(const std::string &host)
{
	if (host.empty() || host.size() > 253 || host.front() == '.' || host.back() == '.')
		return false;
	size_t label = 0;
	char prev = '.';
	for (char c : host) {
		const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
		if (c == '.') {
			if (label == 0 || prev == '-')
				return false;
			label = 0;
		} else if (alnum || c == '-') {
			if (c == '-' && label == 0)
				return false;
			if (++label > 63)
				return false;
		} else {
			return false;
		}
		prev = c;
	}
	return prev != '-';
}

// True when the path part of a request target contains "." or ".." segments,
// literally or percent-encoded. Such URLs are never needed for Apple's CDN.
bool hasDotSegment(const std::string &target)
{
	std::string path = target.substr(0, target.find('?'));
	std::string decoded;
	for (size_t i = 0; i < path.size(); ++i) {
		if (path[i] == '%' && i + 2 < path.size() && path[i + 1] == '2' && lower(path[i + 2]) == 'e') {
			decoded += '.';
			i += 2;
		} else {
			decoded += path[i];
		}
	}
	size_t start = 0;
	while (start <= decoded.size()) {
		size_t end = decoded.find('/', start);
		if (end == std::string::npos)
			end = decoded.size();
		const std::string seg = decoded.substr(start, end - start);
		if (seg == "." || seg == "..")
			return true;
		start = end + 1;
	}
	return false;
}

} // namespace

std::optional<HttpsUrl> parseHttpsUrl(const std::string &url)
{
	if (url.size() < 9 || url.size() > 4096 || !startsWithNoCase(url, "https://"))
		return std::nullopt;
	for (unsigned char c : url)
		if (c <= 0x20 || c >= 0x7f || c == '\\')
			return std::nullopt; // whitespace, control, non-ASCII (must be percent-encoded) or backslash
	const size_t authEnd = url.find_first_of("/?#", 8);
	std::string authority = url.substr(8, authEnd == std::string::npos ? std::string::npos : authEnd - 8);
	if (authority.find('@') != std::string::npos)
		return std::nullopt; // user information
	const size_t colon = authority.find(':');
	if (colon != std::string::npos) {
		if (authority.substr(colon + 1) != "443")
			return std::nullopt; // only the default HTTPS port
		authority.resize(colon);
	}
	std::transform(authority.begin(), authority.end(), authority.begin(), lower);
	if (!validHost(authority))
		return std::nullopt;
	std::string target = authEnd == std::string::npos ? std::string() : url.substr(authEnd);
	target = target.substr(0, target.find('#'));
	if (target.empty() || target[0] == '?')
		target.insert(target.begin(), '/');
	return HttpsUrl{authority, target};
}

bool hostWithinDomain(const std::string &host, const std::string &domain)
{
	if (host == domain)
		return true;
	return host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
	       host[host.size() - domain.size() - 1] == '.';
}

bool trustedAppleMediaUrl(const std::string &url)
{
	const auto parsed = parseHttpsUrl(url);
	return parsed && (hostWithinDomain(parsed->host, "apple.com") || hostWithinDomain(parsed->host, "mzstatic.com"));
}

std::optional<std::string> resolveRedirect(const std::string &base, const std::string &location)
{
	const auto from = parseHttpsUrl(base);
	if (!from || location.empty() || location.size() > 4096)
		return std::nullopt;
	std::string candidate;
	if (startsWithNoCase(location, "https://")) {
		candidate = location;
	} else if (location.rfind("//", 0) == 0) {
		candidate = "https:" + location;
	} else if (location[0] == '/') {
		candidate = "https://" + from->host + location;
	} else {
		// Any other scheme ("http:", "file:", "javascript:") before the first '/' is refused.
		const size_t colon = location.find(':');
		const size_t slash = location.find_first_of("/?#");
		if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
			return std::nullopt;
		const std::string basePath = from->target.substr(0, from->target.find('?'));
		const size_t dir = basePath.rfind('/');
		candidate = "https://" + from->host + basePath.substr(0, dir + 1) + location;
	}
	const auto parsed = parseHttpsUrl(candidate);
	if (!parsed || hasDotSegment(parsed->target))
		return std::nullopt;
	return "https://" + parsed->host + parsed->target;
}

std::optional<int> parseRetryAfterSeconds(const std::string &value)
{
	size_t b = value.find_first_not_of(" \t");
	size_t e = value.find_last_not_of(" \t\r\n");
	if (b == std::string::npos)
		return std::nullopt;
	const std::string v = value.substr(b, e - b + 1);
	if (v.empty() || v.size() > 7)
		return std::nullopt;
	int n = 0;
	for (char c : v) {
		if (c < '0' || c > '9')
			return std::nullopt;
		n = n * 10 + (c - '0');
	}
	return n;
}

EndpointBackoff::EndpointBackoff() : EndpointBackoff(Config{}) {}

EndpointBackoff::EndpointBackoff(Config config, std::function<double()> random) : config_(config), random_(std::move(random))
{
	if (!random_) {
		random_ = [] {
			static thread_local std::mt19937 rng{std::random_device{}()};
			return std::uniform_real_distribution<double>(0.0, 1.0)(rng);
		};
	}
}

EndpointBackoff &EndpointBackoff::shared()
{
	static EndpointBackoff instance;
	return instance;
}

bool EndpointBackoff::allowed(const std::string &endpoint, Clock::time_point now) const
{
	return remaining(endpoint, now) == Clock::duration::zero();
}

EndpointBackoff::Clock::duration EndpointBackoff::remaining(const std::string &endpoint, Clock::time_point now) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = states_.find(endpoint);
	if (it == states_.end() || it->second.until <= now)
		return Clock::duration::zero();
	return it->second.until - now;
}

bool EndpointBackoff::record(const std::string &endpoint, unsigned status, std::optional<int> retryAfterSeconds,
			     Clock::time_point now)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!isOutageStatus(status)) {
		states_.erase(endpoint);
		return false;
	}
	State &st = states_[endpoint];
	st.failures = std::min(st.failures + 1, 30);
	const double base = static_cast<double>(config_.base.count());
	double delay = std::min(base * std::pow(2.0, st.failures - 1), static_cast<double>(config_.cap.count()));
	const double r = std::clamp(random_(), 0.0, 1.0);
	delay *= 1.0 + config_.jitter * (2.0 * r - 1.0);
	if (retryAfterSeconds && *retryAfterSeconds > 0)
		delay = std::max(delay, static_cast<double>(std::min<long long>(*retryAfterSeconds, config_.retryAfterCap.count())));
	delay = std::max(delay, 1.0);
	const auto until = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(delay));
	st.until = std::max(st.until, until);
	return true;
}

} // namespace tonearm
