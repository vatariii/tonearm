// SPDX-License-Identifier: GPL-2.0-or-later
#include "hls-cache.hpp"

#ifdef _WIN32

#include "http-client.hpp"
#include "media/cache_budget.hpp"
#include "media/net_policy.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>

namespace tonearm {
namespace {
namespace fs = std::filesystem;

constexpr size_t kMaxManifest = 256 * 1024;
constexpr size_t kMaxSegment = 20 * 1024 * 1024;
constexpr size_t kMaxEntireAlbum = 100 * 1024 * 1024;
constexpr size_t kMaxSegments = 128;
// Two OBS overlay sources must not download the SAME album concurrently, but
// downloads for DIFFERENT albums should not block each other behind one
// global mutex. Timed locks permit prompt cancellation on rapid song changes.
std::mutex keyMapMutex;
std::unordered_map<std::wstring, std::weak_ptr<std::timed_mutex>> keyLocks;
std::shared_ptr<std::timed_mutex> mutexForKey(const std::wstring &key)
{
	std::lock_guard<std::mutex> guard(keyMapMutex);
	auto &weak = keyLocks[key];
	if (auto live = weak.lock())
		return live;
	auto fresh = std::make_shared<std::timed_mutex>();
	weak = fresh;
	if (keyLocks.size() > 256) {
		for (auto it = keyLocks.begin(); it != keyLocks.end();) {
			if (it->second.expired())
				it = keyLocks.erase(it);
			else
				++it;
		}
	}
	return fresh;
}
void cacheTrace(const char *stage) {
  char flag[2]{};
  if (GetEnvironmentVariableA("TONEARM_HLS_DIAGNOSTICS", flag, 2))
    std::fprintf(stderr, "[Tonearm HLS diagnostic] %s\n", stage);
}

bool appleCdnUrl(const std::string &url)
{
	return trustedAppleMediaUrl(url);
}

fs::path fromUtf8(const std::string &s)
{
	return fs::path(std::u8string(s.begin(), s.end()));
}

std::optional<std::string> absoluteUrl(const std::string &base, std::string child)
{
	if (child.empty() || child.find_first_of("\r\n") != std::string::npos)
		return std::nullopt;
	if (child.rfind("https://", 0) == 0)
		return appleCdnUrl(child) ? std::optional<std::string>{child} : std::nullopt;
	if (child.rfind("http:", 0) == 0)
		return std::nullopt;
	if (child.rfind("//", 0) == 0)
		return appleCdnUrl("https:" + child) ? std::optional<std::string>{"https:" + child} : std::nullopt;
	const size_t originEnd = base.find('/', 8);
	if (originEnd == std::string::npos)
		return std::nullopt;
	std::string origin = base.substr(0, originEnd);
	std::string path;
	if (child[0] == '/') {
		path = origin + child;
	} else {
		std::string basePath = base.substr(0, base.find_first_of("?#"));
		auto slash = basePath.rfind('/');
		if (slash == std::string::npos || slash < originEnd)
			return std::nullopt;
		path = basePath.substr(0, slash + 1) + child;
	}
	// URLs are always used for GET, not as disk paths. Reject traversal.
	if (path.find("/../") != std::string::npos || path.find("/./") != std::string::npos)
		return std::nullopt;
	return appleCdnUrl(path) ? std::optional<std::string>{path} : std::nullopt;
}

HttpGet cdnRequest(const std::string &url, size_t cap)
{
	HttpGet req;
	req.url = url;
	req.allowUrl = appleCdnUrl; // also every redirect target
	req.cap = cap;
	req.resolveMs = req.connectMs = 4000;
	req.sendMs = req.receiveMs = 7000;
	return req;
}

bool fetch(const std::string &url, size_t cap, std::string &body, const std::atomic<bool> &cancelled)
{
	body.clear();
	if (!appleCdnUrl(url) || cancelled)
		return false;
	HttpResult r = httpGet(cdnRequest(url, cap), cancelled);
	if (!r.ok)
		return false;
	body = std::move(r.body);
	return true;
}

// Streams a large download straight into `dest` instead of holding it in memory.
// The file must start with an ISO BMFF box whose type is `ftyp` and be at least
// `minBytes` long. A partial or rejected file is removed.
bool fetchMp4ToFile(const std::string &url, size_t cap, size_t minBytes, const fs::path &dest,
		    const std::atomic<bool> &cancelled)
{
	if (!appleCdnUrl(url) || cancelled)
		return false;
	const fs::path part = dest.wstring() + L".part";
	bool ok = false;
	{
		std::ofstream out(part, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		std::string head;
		size_t written = 0;
		HttpGet req = cdnRequest(url, cap);
		req.sink = [&](const char *data, size_t n) {
			if (head.size() < 8)
				head.append(data, std::min(n, 8 - head.size()));
			if (head.size() >= 8 && head.compare(4, 4, "ftyp") != 0)
				return false; // HTML, an error page or a mislabeled file: stop early
			out.write(data, static_cast<std::streamsize>(n));
			written += n;
			return out.good();
		};
		const HttpResult r = httpGet(req, cancelled);
		out.flush();
		ok = r.ok && out.good() && written >= minBytes && head.size() >= 8 && head.compare(4, 4, "ftyp") == 0;
	}
	std::error_code ec;
	if (ok) {
		fs::rename(part, dest, ec);
		ok = !ec;
	}
	if (!ok)
		fs::remove(part, ec);
	return ok;
}

std::vector<std::string> lines(const std::string &s)
{
	std::istringstream in(s);
	std::vector<std::string> out;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		out.push_back(std::move(line));
	}
	return out;
}

std::optional<std::string> quotedUri(const std::string &tag)
{
	const auto begin = tag.find("URI=\"");
	if (begin == std::string::npos)
		return std::nullopt;
	const auto start = begin + 5;
	const auto end = tag.find('"', start);
	if (end == std::string::npos)
		return std::nullopt;
	return tag.substr(start, end - start);
}

// Reads a decimal attribute value such as BANDWIDTH=123 with a checked conversion.
bool parseAttributeInt(const std::string &line, const char *name, int &value)
{
	const size_t pos = line.find(name);
	if (pos == std::string::npos)
		return false;
	const char *begin = line.data() + pos + std::strlen(name);
	const char *end = line.data() + line.size();
	int v = 0;
	const auto r = std::from_chars(begin, end, v);
	if (r.ec != std::errc() || r.ptr == begin)
		return false;
	value = v;
	return true;
}

// RESOLUTION=<width>x<height>
bool parseResolution(const std::string &line, int &width, int &height)
{
	const size_t pos = line.find("RESOLUTION=");
	if (pos == std::string::npos)
		return false;
	const char *p = line.data() + pos + 11;
	const char *end = line.data() + line.size();
	int w = 0, h = 0;
	auto r = std::from_chars(p, end, w);
	if (r.ec != std::errc() || r.ptr == p || r.ptr == end || *r.ptr != 'x')
		return false;
	p = r.ptr + 1;
	r = std::from_chars(p, end, h);
	if (r.ec != std::errc() || r.ptr == p)
		return false;
	width = w;
	height = h;
	return true;
}

std::vector<std::string> chooseMediaPlaylists(const std::string &master, const std::string &url, MotionQuality quality)
{
	struct Variant {
		std::string url;
		int area = 0, bandwidth = 0;
		bool h264 = true;
	};
	std::vector<Variant> variants;
	const auto rows = lines(master);
	for (size_t i = 0; i + 1 < rows.size(); ++i) {
		const auto &line = rows[i];
		// EXCLUDE #EXT-X-I-FRAME-STREAM-INF. Those higher-resolution trick-play
		// thumbnails contain sparse keyframes, not continuously moving artwork.
		if (line.rfind("#EXT-X-STREAM-INF:", 0) != 0)
			continue;
		const bool h264 = line.find("avc1.") != std::string::npos;
		const bool hevc = line.find("hvc1.") != std::string::npos;
		if (!h264 && !(quality == MotionQuality::Maximum && hevc))
			continue;
		const auto next = absoluteUrl(url, rows[i + 1]);
		if (!next)
			continue;
		int width = 0, height = 0, bitrate = 0;
		if (!parseResolution(line, width, height))
			continue; // a variant without a readable size can't be checked against the limit
		parseAttributeInt(line, "BANDWIDTH=", bitrate);
		const int limit = quality == MotionQuality::Maximum ? 2160 : 1080;
		if (width < 1 || height < 1 || width > limit || height > limit)
			continue;
		variants.push_back(Variant{*next, width * height, bitrate, h264});
	}
	std::stable_sort(variants.begin(), variants.end(), [](const Variant &a, const Variant &b) {
		if (a.area != b.area)
			return a.area > b.area;
		if (a.h264 != b.h264)
			return a.h264; // same-size H.264 is more widely compatible.
		return a.bandwidth > b.bandwidth;
	});
	std::vector<std::string> result;
	for (const auto &v : variants)
		if (std::find(result.begin(), result.end(), v.url) == result.end())
			result.push_back(v.url);
	return result;
}

std::string utf8Path(const fs::path &p)
{
	const auto value = p.u8string();
	return {value.begin(), value.end()};
}

std::string extensionFor(const std::string &url)
{
	std::string path = url.substr(0, url.find_first_of("?#"));
	const auto slash = path.rfind('/');
	const auto dot = path.rfind('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
		std::string ext = path.substr(dot);
		if (ext == ".ts" || ext == ".m4s" || ext == ".mp4" || ext == ".aac")
			return ext;
	}
	return ".mp4";
}

bool save(const fs::path &path, const std::string &data)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		return false;
	out.write(data.data(), static_cast<std::streamsize>(data.size()));
	return out.good();
}

// Apple's video-on-demand playlists commonly describe BYTE RANGES of a single
// complete fragmented MP4: a small init segment followed by video fragments.
// Reading that MP4 as an ordinary local file is more reliable than asking OBS's
// HLS demuxer to juggle range requests and seeking across remote fragments.
std::optional<std::string> cacheSingleMp4(const std::string &media, const std::string &playlistUrl,
					  const fs::path &dir, const std::atomic<bool> &cancelled)
{
	const auto rows = lines(media);
	if (media.find("#EXT-X-ENDLIST") == std::string::npos ||
	    media.find("#EXT-X-KEY:") != std::string::npos)
		return std::nullopt;

	std::optional<std::string> sameFile;
	size_t segments = 0;
	bool byteranges = false;
	std::uint64_t rangeEnd = 0;
	for (const auto &row : rows) {
		if (row.rfind("#EXT-X-MAP:", 0) == 0) {
			const auto uri = quotedUri(row);
			if (!uri)
				return std::nullopt;
			const auto full = absoluteUrl(playlistUrl, *uri);
			if (!full)
				return std::nullopt;
			if (sameFile && *sameFile != *full)
				return std::nullopt;
			sameFile = *full;
		} else if (row.rfind("#EXT-X-BYTERANGE:", 0) == 0) {
			byteranges = true;
			// The playlist exposes the full file's byte ranges. Reject
			// over-budget renditions BEFORE downloading 100 MB of an MP4 that
			// cannot fit in the cache. Then try the next lower-bitrate stream.
			const std::string spec = row.substr(sizeof("#EXT-X-BYTERANGE:") - 1);
			try {
				const auto at = spec.find('@');
				const auto length = static_cast<std::uint64_t>(std::stoull(spec.substr(0, at)));
				const auto start = at == std::string::npos ? rangeEnd :
					static_cast<std::uint64_t>(std::stoull(spec.substr(at + 1)));
				if (length == 0 || start > kMaxEntireAlbum ||
				    length > kMaxEntireAlbum - start) {
					cacheTrace("Apple rendition exceeds motion cache budget; trying smaller stream");
					return std::nullopt;
				}
				rangeEnd = start + length;
			} catch (...) {
				return std::nullopt;
			}
		} else if (!row.empty() && row[0] != '#') {
			const auto full = absoluteUrl(playlistUrl, row);
			if (!full || (sameFile && *sameFile != *full))
				return std::nullopt;
			sameFile = *full;
			++segments;
		}
	}
	if (!sameFile || !byteranges || segments < 1 || segments > kMaxSegments ||
	    extensionFor(*sameFile) != ".mp4")
		return std::nullopt;

	// Streamed to disk in chunks (up to 100 MB). fMP4 video starts with an ISO BMFF
	// file-type box; HTML, an error response or a mislabeled binary is not cached.
	if (!fetchMp4ToFile(*sameFile, kMaxEntireAlbum, 4096, dir / L"artwork.mp4", cancelled)) {
		cacheTrace("failed to download single Apple MP4 (or it was not an MP4)");
		return std::nullopt;
	}
	if (!save(dir / L".complete", "1"))
		return std::nullopt;
	cacheTrace("cached entire finite Apple MP4");
	return utf8Path(dir / L"artwork.mp4");
}

std::optional<std::string> createCache(const std::string &media, const std::string &url, const fs::path &dir,
				       const std::atomic<bool> &cancelled)
{
	const auto rows = lines(media);
	if (rows.empty() || rows[0] != "#EXTM3U" || media.find("#EXT-X-ENDLIST") == std::string::npos) {
		cacheTrace("media playlist missing ENDLIST");
		return std::nullopt;
	} // Only finite VOD artwork can loop reliably.
	if (media.find("#EXT-X-KEY:") != std::string::npos ||
	    media.find("#EXT-X-SESSION-KEY:") != std::string::npos ||
	    media.find("#EXT-X-STREAM-INF:") != std::string::npos) {
		cacheTrace("unsupported encrypted or master playlist");
		return std::nullopt;
	} // Never decrypt media or accidentally cache a master.

	std::string rewritten;
	size_t total = 0, count = 0;
	bool sawVideo = false;
	for (const auto &row : rows) {
		if (cancelled)
			return std::nullopt;
		if (row.rfind("#EXT-X-MAP:", 0) == 0) {
			auto name = quotedUri(row);
			if (!name)
				return std::nullopt;
			auto remote = absoluteUrl(url, *name);
			if (!remote)
				return std::nullopt;
			std::string data;
			if (!fetch(*remote, kMaxSegment, data, cancelled) ||
			    data.size() > kMaxEntireAlbum - total)
				return std::nullopt;
			total += data.size();
			std::string local = "init" + extensionFor(*remote);
			if (!save(dir / fromUtf8(local), data))
				return std::nullopt;
			std::string changed = row;
			changed.replace(changed.find("URI=\"") + 5, name->size(), local);
			rewritten += changed + "\n";
		} else if (row.empty() || row[0] == '#') {
			// Unknown instructions referencing remote keys must not be forwarded.
			if (row.find("URI=") != std::string::npos && row.rfind("#EXT-X-MAP:", 0) != 0)
				return std::nullopt;
			rewritten += row + "\n";
		} else {
			if (++count > kMaxSegments)
				return std::nullopt;
			auto remote = absoluteUrl(url, row);
			if (!remote)
				return std::nullopt;
			std::string data;
			if (!fetch(*remote, kMaxSegment, data, cancelled) ||
			    data.size() > kMaxEntireAlbum - total)
				return std::nullopt;
			total += data.size();
			std::ostringstream name;
			name << "segment-" << std::setfill('0') << std::setw(3) << count << extensionFor(*remote);
			if (!save(dir / fromUtf8(name.str()), data))
				return std::nullopt;
			rewritten += name.str() + "\n";
			sawVideo = true;
		}
	}
	if (!sawVideo || !save(dir / L"artwork.m3u8", rewritten) ||
	    !save(dir / L".complete", "1"))
		return std::nullopt;
	return utf8Path(dir / L"artwork.m3u8");
}

std::wstring cacheKey(const std::string &url)
{
	uint64_t h = 1469598103934665603ULL;
	for (unsigned char c : url) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	std::wostringstream out;
	out << std::hex << std::setw(16) << std::setfill(L'0') << h;
	return out.str();
}
} // namespace

std::optional<std::string> cacheMotionHls(const std::string &playlistUrl, const std::atomic<bool> &cancelled, MotionQuality quality)
{
	if (!appleCdnUrl(playlistUrl) || cancelled)
		return std::nullopt;
	// Album-specific locking is acquired below, after the normalized key is known.
	wchar_t localAppData[32768]{};
	const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, 32768);
	if (!size || size >= 32768)
		return std::nullopt;
	std::error_code ec;
	const fs::path root = fs::path(localAppData) / L"Tonearm" / L"MotionCache";
	fs::create_directories(root, ec);
	if (ec)
		return std::nullopt;

	// Quality variants must not reuse a previous lower-resolution cached file.
	const auto key = cacheKey(playlistUrl + (quality == MotionQuality::Maximum ? "|max" : "|balanced"));
	auto albumLock = mutexForKey(key);
	bool acquired = false;
	while (!cancelled) {
		if (albumLock->try_lock_for(std::chrono::milliseconds(100))) {
			acquired = true;
			break;
		}
		// A newer track superseded this lookup; don't block shutdown or the
		// foreground song behind another overlay's network request.
	}
	if (!acquired)
		return std::nullopt;
	std::unique_lock<std::timed_mutex> locked(*albumLock, std::adopt_lock);
	if (cancelled)
		return std::nullopt; // RAII unlocks even when cancellation races acquisition.
	// Keeps this entry (finished or still downloading) out of cache eviction for as
	// long as this call runs; the player takes its own lease once it loads the video.
	const std::string keyUtf8(key.begin(), key.end()); // hex digits only
	MotionCacheLeases::Lease lease(MotionCacheLeases::shared(), keyUtf8);
	const fs::path finished = root / key;
	const fs::path mp4Dest = finished / L"artwork.mp4";
	const fs::path playlistDest = finished / L"artwork.m3u8";
	if (fs::exists(finished / L".complete", ec)) {
		touchMotionCacheEntry(finished);
		if (fs::exists(mp4Dest, ec))
			return utf8Path(mp4Dest);
		if (fs::exists(playlistDest, ec))
			return utf8Path(playlistDest);
	}

	const fs::path building = root / (key + L"-building");
	auto fail = [&]() -> std::optional<std::string> {
		std::error_code ignored;
		fs::remove_all(building, ignored);
		return std::nullopt;
	};
	std::string master;
	if (!fetch(playlistUrl, kMaxManifest, master, cancelled)) {
		cacheTrace("master GET failed");
		return fail();
	}
	cacheTrace("master loaded");

	const auto candidates = master.find("#EXT-X-STREAM-INF:") != std::string::npos ?
				chooseMediaPlaylists(master, playlistUrl, quality) :
				std::vector<std::string>{playlistUrl};
	if (candidates.empty()) {
		cacheTrace("no compatible full-motion video variants");
		return fail();
	}
	// Quality first, then fall back to less demanding streams if an ultra
	// rendition is unavailable or exceeds the per-video 100 MB safety cap.
	for (const auto &mediaUrl : candidates) {
		if (cancelled)
			return fail();
		ec.clear();
		fs::remove_all(building, ec);
		ec.clear();
		fs::create_directories(building, ec);
		if (ec)
			return fail();

		std::string media;
		if (mediaUrl == playlistUrl)
			media = master;
		else if (!fetch(mediaUrl, kMaxManifest, media, cancelled))
			continue;

		const auto mp4 = cacheSingleMp4(media, mediaUrl, building, cancelled);
		bool ready = static_cast<bool>(mp4);
		if (!ready && media.find("#EXT-X-BYTERANGE:") == std::string::npos)
			ready = static_cast<bool>(createCache(media, mediaUrl, building, cancelled));
		if (!ready)
			continue;

		ec.clear();
		fs::remove_all(finished, ec);
		ec.clear();
		fs::rename(building, finished, ec);
		if (ec) {
			cacheTrace("failed to finalize cache directory");
			return fail();
		}
		cacheTrace("complete high-fidelity local video available");
		return utf8Path(mp4 ? mp4Dest : playlistDest);
	}
	cacheTrace("no video quality rendition downloaded successfully");
	return fail();
}

} // namespace tonearm
#endif
