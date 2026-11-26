// SPDX-License-Identifier: GPL-2.0-or-later
// Network trust boundary, outage backoff and the motion cache budget.
#include "check.hpp"

#include "media/cache_budget.hpp"
#include "media/net_policy.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace tonearm;
namespace fs = std::filesystem;

// --- URL policy -------------------------------------------------------------

TEST("net: strict https parsing")
{
	auto u = parseHttpsUrl("https://Mvod.iTunes.Apple.com/itunes-assets/a.m3u8?x=1#frag");
	CHECK(u && u->host == "mvod.itunes.apple.com" && u->target == "/itunes-assets/a.m3u8?x=1");
	CHECK(parseHttpsUrl("https://apple.com") && parseHttpsUrl("https://apple.com")->target == "/");
	CHECK(parseHttpsUrl("https://apple.com:443/x"));
	for (const char *bad : {"http://apple.com/x", "https://user@apple.com/x", "https://apple.com@evil.com/x",
				"https://apple.com:8443/x", "https://apple.com:/x", "https://apple.com\\@evil.com/",
				"https:///x", "https://apple .com/x", "https://apple.com/a b", "https://apple..com/",
				"https://.apple.com/", "https://apple.com./", "https://-apple.com/", "https://apple.com/\r\nX: y",
				"ftp://apple.com/", "https://[::1]/", "https://ap%70le.com/", "javascript:alert(1)", ""})
		CHECK_MSG(!parseHttpsUrl(bad), bad);
	CHECK(!parseHttpsUrl("https://apple.com/" + std::string(5000, 'a')));
}

TEST("net: domain boundary matching")
{
	CHECK(hostWithinDomain("apple.com", "apple.com"));
	CHECK(hostWithinDomain("mvod.itunes.apple.com", "apple.com"));
	CHECK(!hostWithinDomain("evilapple.com", "apple.com"));
	CHECK(!hostWithinDomain("apple.com.evil.net", "apple.com"));
	CHECK(!hostWithinDomain("com", "apple.com"));
	CHECK(trustedAppleMediaUrl("https://is1-ssl.mzstatic.com/image/thumb/a.mp4"));
	CHECK(!trustedAppleMediaUrl("https://mzstatic.com.example.org/a.mp4"));
	CHECK(!trustedAppleMediaUrl("https://artwork.m8tec.top/api"));
	CHECK(!trustedAppleMediaUrl("http://mvod.itunes.apple.com/a.m3u8"));
}

TEST("net: redirects are resolved and checked like any other URL")
{
	const std::string base = "https://mvod.itunes.apple.com/itunes-assets/HLS/a/b/master.m3u8?t=1";
	// Permitted forms.
	CHECK(resolveRedirect(base, "https://aod.itunes.apple.com/x/y.m3u8") ==
	      std::optional<std::string>("https://aod.itunes.apple.com/x/y.m3u8"));
	CHECK(resolveRedirect(base, "//is2.mzstatic.com/v.mp4") == std::optional<std::string>("https://is2.mzstatic.com/v.mp4"));
	CHECK(resolveRedirect(base, "/other/path.m3u8?q=2") ==
	      std::optional<std::string>("https://mvod.itunes.apple.com/other/path.m3u8?q=2"));
	CHECK(resolveRedirect(base, "variant.m3u8") ==
	      std::optional<std::string>("https://mvod.itunes.apple.com/itunes-assets/HLS/a/b/variant.m3u8"));
	// A cross-host redirect resolves, and the caller's allowlist then rejects it.
	const auto offsite = resolveRedirect(base, "https://evil.example.com/a.mp4");
	CHECK(offsite && !trustedAppleMediaUrl(*offsite));
	// Rejected outright.
	for (const char *bad : {"http://mvod.itunes.apple.com/a", "file:///C:/Windows/win.ini", "javascript:x",
				"../../../etc/passwd", "/a/./b", "/a/%2e%2e/b", "https://user@apple.com/", "https://apple.com:81/",
				"", "\r\nSet-Cookie: x", "data:text/plain,hi"})
		CHECK_MSG(!resolveRedirect(base, bad), bad);
	CHECK(!resolveRedirect("not a url", "/x"));
}

TEST("net: Retry-After seconds")
{
	CHECK(parseRetryAfterSeconds("120") == std::optional<int>(120));
	CHECK(parseRetryAfterSeconds(" 5 ") == std::optional<int>(5));
	CHECK(!parseRetryAfterSeconds("Wed, 21 Oct 2015 07:28:00 GMT"));
	CHECK(!parseRetryAfterSeconds("-3"));
	CHECK(!parseRetryAfterSeconds("99999999"));
	CHECK(!parseRetryAfterSeconds(""));
}

TEST("net: endpoint backoff grows, honours Retry-After, caps and recovers")
{
	using namespace std::chrono;
	EndpointBackoff::Config cfg;
	cfg.base = seconds(30);
	cfg.cap = seconds(15 * 60);
	cfg.retryAfterCap = seconds(3600);
	cfg.jitter = 0.2;
	double r = 0.5; // no jitter
	EndpointBackoff b(cfg, [&] { return r; });
	const auto t0 = EndpointBackoff::Clock::now();
	const std::string api = "artwork.m8tec.top";

	CHECK(b.allowed(api, t0));
	CHECK(!b.record(api, 404, std::nullopt, t0)); // an ordinary miss is not an outage
	CHECK(b.allowed(api, t0));

	CHECK(b.record(api, 503, std::nullopt, t0));
	CHECK(!b.allowed(api, t0 + seconds(29)));
	CHECK(b.allowed(api, t0 + seconds(31)));
	CHECK(b.allowed("itunes.apple.com", t0)); // per endpoint

	b.record(api, 0, std::nullopt, t0); // network failure: 60 s
	CHECK(!b.allowed(api, t0 + seconds(59)));
	CHECK(b.allowed(api, t0 + seconds(61)));
	for (int i = 0; i < 20; ++i)
		b.record(api, 500, std::nullopt, t0);
	CHECK(!b.allowed(api, t0 + seconds(899)));
	CHECK(b.allowed(api, t0 + seconds(901))); // capped at 15 minutes

	b.record(api, 200, std::nullopt, t0); // recovered
	CHECK(b.allowed(api, t0));

	b.record(api, 429, 600, t0); // Retry-After longer than the first step
	CHECK(!b.allowed(api, t0 + seconds(599)));
	CHECK(b.allowed(api, t0 + seconds(601)));
	b.record(api, 200, std::nullopt, t0);
	b.record(api, 429, 999999, t0); // absurd Retry-After is capped at an hour
	CHECK(!b.allowed(api, t0 + seconds(3599)));
	CHECK(b.allowed(api, t0 + seconds(3601)));

	// Jitter stays within +/- 20 %.
	b.record(api, 200, std::nullopt, t0);
	r = 0.0;
	b.record(api, 503, std::nullopt, t0);
	CHECK(b.allowed(api, t0 + seconds(25)));
	b.record(api, 200, std::nullopt, t0);
	r = 0.999;
	b.record(api, 503, std::nullopt, t0);
	CHECK(!b.allowed(api, t0 + seconds(35)));
}

// --- Cache budget -------------------------------------------------------------

namespace {

struct TempDir {
	fs::path path;
	TempDir()
	{
		std::random_device rd;
		path = fs::temp_directory_path() / ("tonearm-cache-test-" + std::to_string(rd()) + std::to_string(rd()));
		fs::create_directories(path);
	}
	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

void writeFile(const fs::path &p, size_t bytes)
{
	fs::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary | std::ios::trunc);
	std::string data(bytes, 'x');
	f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string key(int i)
{
	char buf[17];
	std::snprintf(buf, sizeof buf, "%016x", i);
	return buf;
}

// A complete cache entry of `bytes`, last used `ageSeconds` ago.
void entry(const fs::path &root, int i, size_t bytes, int ageSeconds)
{
	const fs::path dir = root / key(i);
	writeFile(dir / "artwork.mp4", bytes);
	writeFile(dir / ".complete", 1);
	const auto when = fs::file_time_type::clock::now() - std::chrono::seconds(ageSeconds);
	fs::last_write_time(dir / ".complete", when);
}

void indexFor(const fs::path &root, const std::string &name, const fs::path &target)
{
	fs::create_directories(root / "AlbumIndex");
	std::ofstream f(root / "AlbumIndex" / name, std::ios::binary);
	const auto loc = target.u8string();
	f << "TONEARM_MOTION_INDEX_V2\nkey\ns\n" << std::string(loc.begin(), loc.end()) << "\n";
}

} // namespace

TEST("cache: least recently used entries are evicted down to the budget")
{
	TempDir t;
	for (int i = 0; i < 6; ++i)
		entry(t.path, i, 1000, 600 - i * 60); // entry 0 is the oldest
	CachePruneOptions o;
	o.budget = 3500;
	MotionCacheLeases leases;
	o.leases = &leases;
	const auto r = pruneMotionCache(t.path, o);
	CHECK(r.bytesBefore >= 6000);
	CHECK(r.evicted == 3);
	CHECK(r.bytesAfter <= 3500);
	CHECK(!r.overBudget);
	for (int i = 0; i < 3; ++i)
		CHECK(!fs::exists(t.path / key(i)));
	for (int i = 3; i < 6; ++i)
		CHECK(fs::exists(t.path / key(i) / "artwork.mp4"));
	// Under budget: nothing more happens.
	CHECK(pruneMotionCache(t.path, o).evicted == 0);
}

TEST("cache: touching an entry moves it to the back of the queue")
{
	TempDir t;
	for (int i = 0; i < 3; ++i)
		entry(t.path, i, 1000, 300 - i * 60);
	touchMotionCacheEntry(t.path / key(0)); // the oldest is used again
	CachePruneOptions o;
	o.budget = 2500;
	MotionCacheLeases leases;
	o.leases = &leases;
	pruneMotionCache(t.path, o);
	CHECK(fs::exists(t.path / key(0)));
	CHECK(!fs::exists(t.path / key(1)));
	CHECK(fs::exists(t.path / key(2)));
}

TEST("cache: leased entries and running downloads are never evicted")
{
	TempDir t;
	for (int i = 0; i < 4; ++i)
		entry(t.path, i, 1000, 400 - i * 60);
	writeFile(t.path / (key(9) + "-building") / "segment-001.m4s", 3000); // fresh, in progress
	MotionCacheLeases leases;
	MotionCacheLeases::Lease playing(leases, key(0)); // the oldest is on screen
	CachePruneOptions o;
	o.budget = 1000;
	o.leases = &leases;
	const auto r = pruneMotionCache(t.path, o);
	CHECK(fs::exists(t.path / key(0) / "artwork.mp4"));
	CHECK(fs::exists(t.path / (key(9) + "-building") / "segment-001.m4s"));
	CHECK(r.overBudget); // what is left is pinned; reported, not forced
	CHECK(!fs::exists(t.path / key(1)));
	CHECK(!fs::exists(t.path / key(2)));
	CHECK(!fs::exists(t.path / key(3)));
}

TEST("cache: abandoned downloads, eviction leftovers and dead index entries are cleaned")
{
	TempDir t;
	entry(t.path, 1, 100, 10);
	const fs::path stale = t.path / (key(2) + "-building");
	writeFile(stale / "segment-001.m4s", 500);
	fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours(3));
	const fs::path leased = t.path / (key(3) + "-building");
	writeFile(leased / "segment-001.m4s", 500);
	fs::last_write_time(leased, fs::file_time_type::clock::now() - std::chrono::hours(3));
	writeFile(t.path / (key(4) + ".evict") / "artwork.mp4", 500);
	indexFor(t.path, "album-live.index", t.path / key(1) / "artwork.mp4");
	indexFor(t.path, "album-dead.index", t.path / key(7) / "artwork.mp4");
	writeFile(t.path / "AlbumIndex" / "album-v1.index", 0);
	{
		std::ofstream v1(t.path / "AlbumIndex" / "album-v1.index");
		v1 << "TONEARM_MOTION_INDEX_V1\n";
	}
	MotionCacheLeases leases;
	MotionCacheLeases::Lease downloading(leases, key(3));
	CachePruneOptions o;
	o.leases = &leases;
	const auto r = pruneMotionCache(t.path, o);
	CHECK(!fs::exists(stale));
	CHECK(fs::exists(leased));
	CHECK(!fs::exists(t.path / (key(4) + ".evict")));
	CHECK(fs::exists(t.path / "AlbumIndex" / "album-live.index"));
	CHECK(!fs::exists(t.path / "AlbumIndex" / "album-dead.index"));
	CHECK(!fs::exists(t.path / "AlbumIndex" / "album-v1.index"));
	CHECK(r.staleRemoved == 2);
	CHECK(r.indexRemoved == 2);
}

TEST("cache: only Tonearm's own directories are touched, links are not followed")
{
	TempDir t;
	TempDir outside;
	writeFile(outside.path / "precious.mp4", 5000);
	writeFile(t.path / "My Music" / "song.mp4", 5000);   // not a cache key
	writeFile(t.path / "readme.txt", 10);                // loose file
	writeFile(t.path / "0123456789ABCDEF" / "x.mp4", 10); // upper case: not ours
	entry(t.path, 1, 100, 100);
	std::error_code ec;
	fs::create_directory_symlink(outside.path, t.path / key(5), ec); // a link that looks like a key
	const bool haveLink = !ec;
	CachePruneOptions o;
	o.budget = 0;
	MotionCacheLeases leases;
	o.leases = &leases;
	pruneMotionCache(t.path, o);
	CHECK(fs::exists(outside.path / "precious.mp4"));
	CHECK(fs::exists(t.path / "My Music" / "song.mp4"));
	CHECK(fs::exists(t.path / "readme.txt"));
	CHECK(fs::exists(t.path / "0123456789ABCDEF" / "x.mp4"));
	CHECK(!fs::exists(t.path / key(1)));
	if (haveLink)
		CHECK(fs::is_symlink(t.path / key(5)));
	// A root that is missing or a file is a no-op.
	CHECK(pruneMotionCache(t.path / "missing", o).evicted == 0);
	CHECK(pruneMotionCache(t.path / "readme.txt", o).evicted == 0);
}

TEST("cache: keys and leases")
{
	CHECK(isMotionCacheKey("0123456789abcdef"));
	CHECK(!isMotionCacheKey("0123456789abcde"));
	CHECK(!isMotionCacheKey("0123456789abcdeg"));
	CHECK(motionCacheKeyForPath(fs::path("C:/x/MotionCache/00000000000000ff/artwork.mp4")) == "00000000000000ff");
	CHECK(motionCacheKeyForPath(fs::path("D:/Videos/Artist - Album.mp4")).empty());
	MotionCacheLeases l;
	{
		MotionCacheLeases::Lease a(l, "k");
		MotionCacheLeases::Lease b(l, "k");
		CHECK(l.leased("k"));
		MotionCacheLeases::Lease moved = std::move(a);
		CHECK(l.leased("k"));
	}
	CHECK(!l.leased("k"));
}

TEST("cache: pruning while other threads lease and add entries stays consistent")
{
	TempDir t;
	MotionCacheLeases leases;
	for (int i = 0; i < 20; ++i)
		entry(t.path, i, 200, 1000 - i);
	std::atomic<bool> stop{false};
	std::thread writer([&] {
		for (int i = 100; i < 160 && !stop; ++i) {
			MotionCacheLeases::Lease lease(leases, key(i));
			const fs::path building = t.path / (key(i) + "-building");
			writeFile(building / "artwork.mp4", 200);
			writeFile(building / ".complete", 1);
			std::error_code ec;
			fs::rename(building, t.path / key(i), ec);
		}
	});
	CachePruneOptions o;
	o.budget = 2000;
	o.leases = &leases;
	for (int round = 0; round < 40; ++round)
		pruneMotionCache(t.path, o);
	stop = true;
	writer.join();
	const auto r = pruneMotionCache(t.path, o);
	CHECK(r.bytesAfter <= 2000);
	// No half-deleted entries: every remaining key directory still has its video.
	for (const auto &e : fs::directory_iterator(t.path))
		if (isMotionCacheKey(e.path().filename().string()))
			CHECK(fs::exists(e.path() / "artwork.mp4"));
}
