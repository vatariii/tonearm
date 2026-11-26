// SPDX-License-Identifier: GPL-2.0-or-later
#include "http-client.hpp"

#ifdef _WIN32

#include "media/net_policy.hpp"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#ifndef TONEARM_VERSION
#define TONEARM_VERSION "0.0.0"
#endif

namespace tonearm {

namespace {

struct Handle {
	HINTERNET h = nullptr;
	Handle() = default;
	explicit Handle(HINTERNET value) : h(value) {}
	~Handle() { reset(); }
	Handle(const Handle &) = delete;
	Handle &operator=(const Handle &) = delete;
	void reset(HINTERNET value = nullptr)
	{
		if (h)
			WinHttpCloseHandle(h);
		h = value;
	}
	explicit operator bool() const { return h != nullptr; }
	operator HINTERNET() const { return h; }
};

std::wstring ascii(const std::string &s)
{
	return std::wstring(s.begin(), s.end()); // parseHttpsUrl only accepts ASCII
}

std::string narrowAscii(const std::wstring &w)
{
	std::string out;
	out.reserve(w.size());
	for (wchar_t c : w) {
		if (c <= 0 || c >= 0x80)
			return {}; // a Location with raw non-ASCII is refused rather than guessed
		out += static_cast<char>(c);
	}
	return out;
}

std::optional<std::wstring> stringHeader(HINTERNET request, DWORD which)
{
	DWORD bytes = 0;
	WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes, WINHTTP_NO_HEADER_INDEX);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > 16 * 1024)
		return std::nullopt;
	std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
	if (!WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
		return std::nullopt;
	value.resize(bytes / sizeof(wchar_t));
	return value;
}

} // namespace

const wchar_t *tonearmUserAgent()
{
	static const std::wstring ua = L"Tonearm/" + ascii(TONEARM_VERSION) + L" (OBS Studio)";
	return ua.c_str();
}

HttpResult httpGet(const HttpGet &req, const std::atomic<bool> &cancelled)
{
	HttpResult result;
	if (cancelled)
		return result;
	Handle session(WinHttpOpen(tonearmUserAgent(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
				   WINHTTP_NO_PROXY_BYPASS, 0));
	if (!session)
		return result;
	// A failed service must never leave the plugin stuck or block the graphics thread.
	WinHttpSetTimeouts(session, req.resolveMs, req.connectMs, req.sendMs, req.receiveMs);
	DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));

	std::string url = req.url;
	for (int hop = 0; hop <= kMaxRedirects; ++hop) {
		if (cancelled)
			return result;
		const auto parsed = parseHttpsUrl(url);
		if (!parsed || !req.allowUrl || !req.allowUrl(url)) {
			result.rejected = true;
			return result;
		}
		result.finalUrl = url;
		Handle connection(WinHttpConnect(session, ascii(parsed->host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
		if (!connection || cancelled)
			return result;
		Handle request(WinHttpOpenRequest(connection, L"GET", ascii(parsed->target).c_str(), nullptr, WINHTTP_NO_REFERER,
						  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
		if (!request || cancelled)
			return result;
		// Also on the request handle, so no redirect is ever followed behind the policy's back.
		if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never)))
			return result;
		const wchar_t *headers = req.headers ? req.headers : WINHTTP_NO_ADDITIONAL_HEADERS;
		const DWORD headerLength = req.headers ? static_cast<DWORD>(-1) : 0;
		if (!WinHttpSendRequest(request, headers, headerLength, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || cancelled)
			return result;
		if (!WinHttpReceiveResponse(request, nullptr) || cancelled)
			return result;
		DWORD status = 0, size = sizeof(status);
		if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
					 &status, &size, WINHTTP_NO_HEADER_INDEX))
			return result;

		if (isRedirectStatus(status)) {
			const auto location = stringHeader(request, WINHTTP_QUERY_LOCATION);
			const std::string loc = location ? narrowAscii(*location) : std::string();
			const auto next = loc.empty() ? std::nullopt : resolveRedirect(url, loc);
			if (!next) {
				result.rejected = true;
				return result;
			}
			url = *next; // checked against allowUrl at the top of the loop
			continue;
		}

		result.status = status;
		if (status != 200) {
			if (status == 429 || status == 503)
				if (const auto ra = stringHeader(request, WINHTTP_QUERY_RETRY_AFTER))
					result.retryAfter = parseRetryAfterSeconds(narrowAscii(*ra));
			return result;
		}

		std::size_t total = 0;
		std::vector<char> chunk;
		for (;;) {
			if (cancelled)
				return result;
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request, &available))
				return result;
			if (available == 0)
				break;
			if (available > req.cap || total > req.cap - available)
				return result; // over the size limit: fail rather than truncate
			chunk.resize(available);
			DWORD read = 0;
			if (!WinHttpReadData(request, chunk.data(), available, &read))
				return result;
			if (read == 0)
				break;
			total += read;
			if (req.sink) {
				if (!req.sink(chunk.data(), read))
					return result;
			} else {
				result.body.append(chunk.data(), read);
			}
		}
		result.ok = true;
		return result;
	}
	result.rejected = true; // too many redirects
	return result;
}

} // namespace tonearm

#endif
