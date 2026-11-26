// SPDX-License-Identifier: GPL-2.0-or-later
#include "remote-motion-source.hpp"
#include "hls-cache.hpp"
#include "motion-index.hpp"
#include <obs-module.h>
#include "media/catalog_match.hpp"

#ifdef _WIN32

#include <obs-data.h>

#include <windows.h>
#include <winhttp.h>

#include "http-client.hpp"
#include "media/net_policy.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <set>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tonearm {
namespace {

constexpr const char *kArtworkHost = "artwork.m8tec.top";

// After a new video lands in the cache, keep the whole cache within its budget.
void enforceCacheBudget()
{
	const CachePruneResult r = pruneMotionCacheNow();
	if (r.evicted || r.staleRemoved || r.indexRemoved || r.overBudget)
		blog(LOG_INFO, "[tonearm] motion cache %.1f MB -> %.1f MB (budget %.0f MB): %d evicted, %d stale removed, %d index entries removed%s",
		     r.bytesBefore / 1048576.0, r.bytesAfter / 1048576.0, kDefaultMotionCacheBudget / 1048576.0, r.evicted,
		     r.staleRemoved, r.indexRemoved, r.overBudget ? "; still over budget because entries are in use" : "");
}

std::string percentEncode(const std::string &text)
{
	static constexpr char hex[] = "0123456789ABCDEF";
	std::string result;
	for (unsigned char c : text) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
			result += static_cast<char>(c);
		} else {
			result += '%';
			result += hex[c >> 4];
			result += hex[c & 15];
		}
	}
	return result;
}

// Don't allow a network service to feed arbitrary local files or unrelated URLs
// into OBS's FFmpeg decoder: only https URLs on Apple's media CDNs.
bool trustedVideoUrl(const std::string &url)
{
	return trustedAppleMediaUrl(url);
}

// GET a small JSON document from one of the two lookup services. Redirects are
// followed only to the same host. Every outcome feeds the shared per-host backoff,
// and while a host is cooling down after an outage no request is sent at all.
std::string getJson(const std::string &host, const std::string &path, const std::atomic<bool> &cancelled,
		    DWORD *responseStatus)
{
	if (responseStatus)
		*responseStatus = 0;
	if (cancelled || !EndpointBackoff::shared().allowed(host))
		return {};
	HttpGet req;
	req.url = "https://" + host + path;
	req.allowUrl = [host](const std::string &url) {
		const auto parsed = parseHttpsUrl(url);
		return parsed && parsed->host == host;
	};
	req.headers = L"Accept: application/json\r\n";
	// Exact soundtrack verification may use an album's complete track
	// listing, which is larger than the artwork service's small JSON.
	req.cap = host == "itunes.apple.com" ? 512 * 1024 : 65536;
	req.resolveMs = req.connectMs = 2500;
	req.sendMs = req.receiveMs = 4500;
	const HttpResult r = httpGet(req, cancelled);
	if (cancelled)
		return {};
	if (r.rejected)
		blog(LOG_WARNING, "[tonearm] refused a redirect away from %s", host.c_str());
	else if (EndpointBackoff::shared().record(host, r.status, r.retryAfter))
		blog(LOG_INFO, "[tonearm] %s unavailable (HTTP %u); pausing lookups for %lld s", host.c_str(), r.status,
		     static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
						    EndpointBackoff::shared().remaining(host)).count()));
	if (responseStatus)
		*responseStatus = r.status;
	return r.ok ? r.body : std::string();
}

std::string getArtworkJson(const std::string &artist, const std::string &album,
			   const std::atomic<bool> &cancelled, DWORD *responseStatus)
{
	const std::string path = "/api/v1/artwork/search?artist=" + percentEncode(artist) +
				 "&album=" + percentEncode(album);
	return getJson(kArtworkHost, path, cancelled, responseStatus);
}

// Track performers and soundtrack composers often differ (e.g. Halle versus
// Alan Menken & Howard Ashman). Never accept the artist mismatch just because
// two soundtrack titles look similar. Validate both the official album credit
// and the exact track/performer using Apple's public iTunes Search/Lookup API.
bool verifySoundtrackPerformer(const MotionQuery &q, const std::string &motionArtist,
			      const std::string &motionAlbum, const std::atomic<bool> &cancelled)
{
	if (cancelled || q.title.empty() || q.artist.empty() ||
	    normalizeName(q.album).find("soundtrack") == std::string::npos ||
	    normalizeName(q.album) != normalizeName(motionAlbum))
		return false;
	DWORD status = 0;
	const std::string catalogPath = "/search?term=" + percentEncode(q.album) +
					"&entity=album&country=US&limit=35";
	const std::string jsonText = getJson("itunes.apple.com", catalogPath, cancelled, &status);
	if (status != 200 || jsonText.empty() || cancelled)
		return false;
	obs_data_t *root = obs_data_create_from_json(jsonText.c_str());
	if (!root)
		return false;
	std::vector<CatalogAlbum> candidates;
	obs_data_array_t *albums = obs_data_get_array(root, "results");
	if (albums) {
		const size_t limit = std::min<size_t>(obs_data_array_count(albums), 35);
		for (size_t i = 0; i < limit; ++i) {
			obs_data_t *item = obs_data_array_item(albums, i);
			if (!item)
				continue;
			const std::string officialArtist = obs_data_get_string(item, "artistName");
			const std::string officialAlbum = obs_data_get_string(item, "collectionName");
			const auto rawId = obs_data_get_int(item, "collectionId");
			obs_data_release(item);
			if (rawId <= 0 || normalizeName(officialAlbum) != normalizeName(q.album) ||
			    normalizeName(officialArtist) != normalizeName(motionArtist))
				continue;
			candidates.push_back({static_cast<std::uint64_t>(rawId), officialArtist, officialAlbum});
			if (candidates.size() >= 4)
				break;
		}
		obs_data_array_release(albums);
	}
	obs_data_release(root);
	// Catalog verification is bounded so switching tracks never starts a long
	// sequence of public API calls.
	for (const auto &album : candidates) {
		if (cancelled)
			return false;
		const auto path = "/lookup?id=" + std::to_string(album.id) + "&entity=song&limit=200";
		status = 0;
		const std::string tracks = getJson("itunes.apple.com", path, cancelled, &status);
		if (status != 200 || tracks.empty() || cancelled)
			continue;
		obs_data_t *catalog = obs_data_create_from_json(tracks.c_str());
		if (!catalog)
			continue;
		bool verified = false;
		obs_data_array_t *items = obs_data_get_array(catalog, "results");
		if (items) {
			const size_t count = std::min<size_t>(obs_data_array_count(items), 200);
			for (size_t i = 0; i < count && !verified; ++i) {
				obs_data_t *item = obs_data_array_item(items, i);
				if (!item)
					continue;
				const auto trackAlbumId = obs_data_get_int(item, "collectionId");
				const std::string artist = obs_data_get_string(item, "artistName");
				const std::string title = obs_data_get_string(item, "trackName");
				obs_data_release(item);
				if (trackAlbumId == static_cast<int64_t>(album.id))
					verified = verifiedSoundtrackTrack(q, album, motionArtist, motionAlbum, artist, title);
			}
			obs_data_array_release(items);
		}
		obs_data_release(catalog);
		if (verified) {
			blog(LOG_INFO, "[tonearm] soundtrack artist credit verified through official iTunes track list");
			return true;
		}
	}
	return false;
}

// The service's artist/album search has a long-lived negative cache and may
// incorrectly return 404 for real albums. Public iTunes Search can identify
// their catalog IDs without any Apple Music session credentials; the independent
// artwork service also exposes an album-URL lookup route.
std::optional<MotionAsset> resolveWithCatalogId(const MotionQuery &q, const std::atomic<bool> &cancelled,
						std::set<std::string> &failedUrls)
{
	struct Candidate {
		std::uint64_t id;
		int priority;
	};
	std::vector<Candidate> candidates;
	auto scanItunes = [&](const std::string &album) {
		const std::string term = q.artist + " " + album;
		const std::string path = "/search?term=" + percentEncode(term) + "&entity=album&country=US&limit=25";
		DWORD status = 0;
		const std::string jsonText = getJson("itunes.apple.com", path, cancelled, &status);
		if (status != 200 || jsonText.empty() || cancelled)
			return;
		obs_data_t *root = obs_data_create_from_json(jsonText.c_str());
		if (!root)
			return;
		obs_data_array_t *items = obs_data_get_array(root, "results");
		if (items) {
			const size_t n = obs_data_array_count(items);
			for (size_t i = 0; i < n; ++i) {
				obs_data_t *item = obs_data_array_item(items, i);
				if (!item)
					continue;
				const std::string artist = obs_data_get_string(item, "artistName");
				const std::string title = obs_data_get_string(item, "collectionName");
				const auto rawId = obs_data_get_int(item, "collectionId");
				obs_data_release(item);
				if (rawId <= 0 || !catalogArtistMatches(q, artist) ||
				    !compatibleMotionAlbumTitle(q.album, title))
					continue;
				const int priority = albumKey(q.album) == albumKey(title) ? 0 : 1;
				const auto id = static_cast<std::uint64_t>(rawId);
				if (std::find_if(candidates.begin(), candidates.end(),
						 [&](const Candidate &c) { return c.id == id; }) == candidates.end())
					candidates.push_back({id, priority});
			}
			obs_data_array_release(items);
		}
		obs_data_release(root);
	};

	scanItunes(q.album);
	const std::string base = motionBaseAlbumQuery(q.album);
	if (candidates.empty() && !base.empty() && !cancelled)
		scanItunes(base);
	std::stable_sort(candidates.begin(), candidates.end(),
			 [](const Candidate &a, const Candidate &b) { return a.priority < b.priority; });

	// Try a handful of matching catalog releases, never a fuzzy first hit.
	// Verify the API's actual returned artist + album again before using media.
	size_t lookedUp = 0;
	for (const auto &candidate : candidates) {
		if (cancelled || ++lookedUp > 4)
			break;
		const std::string publicUrl = "https://music.apple.com/album/" + std::to_string(candidate.id);
		const std::string path = "/api/v1/artwork/url?url=" + percentEncode(publicUrl);
		DWORD status = 0;
		const auto body = getJson(kArtworkHost, path, cancelled, &status);
		if (cancelled || EndpointBackoff::isOutageStatus(status))
			break;
		if (body.empty())
			continue;
		obs_data_t *json = obs_data_create_from_json(body.c_str());
		if (!json)
			continue;
		const std::string foundArtist = obs_data_get_string(json, "artist");
		const std::string foundAlbum = obs_data_get_string(json, "album");
		const std::string square = obs_data_get_string(json, "url");
		const std::string tall = obs_data_get_string(json, "url_tall");
		obs_data_release(json);

		if (!compatibleMotionAlbumTitle(q.album, foundAlbum) ||
		    (!catalogArtistMatches(q, foundArtist) &&
		     !verifySoundtrackPerformer(q, foundArtist, foundAlbum, cancelled)))
			continue;
		const bool portrait = q.preferred == MotionShape::Portrait;
		const std::string chosen = portrait && trustedVideoUrl(tall) ? tall :
					   trustedVideoUrl(square) ? square : trustedVideoUrl(tall) ? tall : "";
		if (chosen.empty())
			continue;
		if (failedUrls.count(chosen)) {
			// The search above already tried this exact video and it failed; this
			// candidate has nothing new to offer, but a different one still might.
			blog(LOG_INFO, "[tonearm] skipping a motion video that already failed in this lookup");
			continue;
		}
		auto cached = cacheMotionHls(chosen, cancelled, q.quality);
		if (cached) {
			blog(LOG_INFO, "[tonearm] animated artwork found via public album ID fallback");
			MotionAsset asset{*cached, chosen == tall ? MotionShape::Portrait : MotionShape::Square};
			rememberMotionForAlbum(q, asset);
			enforceCacheBudget();
			return asset;
		}
		failedUrls.insert(chosen);
		break; // Avoid downloading the same failed source multiple times.
	}
	return std::nullopt;
}
} // namespace

std::optional<MotionAsset> RemoteArtworkApiSource::resolve(const MotionQuery &q, const std::atomic<bool> &cancelled)
{
	if (q.artist.empty() || q.album.empty() || cancelled)
		return std::nullopt;
	// A confirmed local album match should display immediately on replay.
	// Avoid the external metadata service entirely when we already own a
	// complete, validated motion video for this precise release and shape.
	if (auto indexed = cachedMotionForAlbum(q)) {
		blog(LOG_INFO, "[tonearm] instant animated album cache hit");
		return indexed;
	}

	// While the lookup service is cooling down after an outage, don't send anything.
	// The catalog fallback below also goes through it, so it would fail the same way.
	if (!EndpointBackoff::shared().allowed(kArtworkHost)) {
		blog(LOG_INFO, "[tonearm] motion lookup paused after a service outage; using still cover");
		return std::nullopt;
	}

	struct Lookup {
		std::string artist, album;
	};
	std::vector<Lookup> attempts;
	auto add = [&](const std::string &artist, const std::string &album) {
		if (artist.empty() || album.empty())
			return;
		for (const auto &item : attempts)
			if (normalizeName(item.artist) == normalizeName(artist) &&
			    normalizeName(item.album) == normalizeName(album))
				return;
		attempts.push_back({artist, album});
	};

	// The service can negatively cache the exact artist/album search for a
	// month. Prefer the album (not track title) so a title mismatch cannot
	// poison its cache, then try the album artist / track artist separately.
	const std::string albumArtist = q.albumArtist.empty() ? q.artist : q.albumArtist;
	const std::string base = motionBaseAlbumQuery(q.album);
	add(albumArtist, q.album);
	add(q.artist, q.album);
	if (!base.empty()) {
		add(albumArtist, base);
		add(q.artist, base);
	}
	const auto performerKeys = artistKeys(q.artist);
	if (performerKeys.size() > 1 && attempts.size() < 5)
		add(performerKeys.back(), base.empty() ? q.album : base);

	size_t checked = 0;
	bool outage = false;
	std::set<std::string> failedUrls; // videos that failed to download in this lookup
	for (const auto &search : attempts) {
		if (cancelled)
			return std::nullopt;
		DWORD status = 0;
		++checked;
		const std::string body = getArtworkJson(search.artist, search.album, cancelled, &status);
		if (cancelled)
			return std::nullopt;
		if (EndpointBackoff::isOutageStatus(status)) {
			blog(LOG_INFO, "[tonearm] motion lookup temporarily unavailable (HTTP %lu); using still cover",
			     static_cast<unsigned long>(status));
			outage = true;
			break; // Respect provider rate limits and transient failures.
		}
		if (body.empty())
			continue;
		obs_data_t *json = obs_data_create_from_json(body.c_str());
		if (!json)
			continue;
		const std::string responseArtist = obs_data_get_string(json, "artist");
		const std::string responseAlbum = obs_data_get_string(json, "album");
		const std::string square = obs_data_get_string(json, "url");
		const std::string tall = obs_data_get_string(json, "url_tall");
		obs_data_release(json);

		if (!compatibleMotionAlbumTitle(q.album, responseAlbum) ||
		    (!catalogArtistMatches(q, responseArtist) &&
		     !verifySoundtrackPerformer(q, responseArtist, responseAlbum, cancelled))) {
			blog(LOG_INFO, "[tonearm] rejected ambiguous motion artwork candidate; exact release not confirmed");
			continue;
		}
		const bool portrait = q.preferred == MotionShape::Portrait;
		const std::string chosen = portrait && trustedVideoUrl(tall) ? tall :
					   trustedVideoUrl(square) ? square : trustedVideoUrl(tall) ? tall : "";
		if (chosen.empty() || failedUrls.count(chosen))
			continue;
		auto cached = cacheMotionHls(chosen, cancelled, q.quality);
		if (cached) {
			blog(LOG_INFO, "[tonearm] animated artwork ready (query %zu/%zu, %s rendition)",
			     checked, attempts.size(), q.quality == MotionQuality::Maximum ? "maximum" : "balanced");
			MotionAsset asset{*cached, chosen == tall ? MotionShape::Portrait : MotionShape::Square};
			rememberMotionForAlbum(q, asset);
			enforceCacheBudget();
			return asset;
		}
		if (!cancelled)
			blog(LOG_WARNING, "[tonearm] motion URL found but video cache/decoder preparation failed");
		failedUrls.insert(chosen);
		// Alternate artist spellings for this same album won't repair a broken
		// video URL. Don't repeatedly download the same large asset.
		break;
	}
	if (!cancelled && !outage) {
		const auto catalog = resolveWithCatalogId(q, cancelled, failedUrls);
		if (catalog)
			return catalog;
	}
	blog(LOG_INFO, "[tonearm] no compatible animated artwork resolved (%zu metadata queries); using still cover", checked);
	return std::nullopt;
}

} // namespace tonearm
#endif
