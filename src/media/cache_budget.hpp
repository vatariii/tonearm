// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Size budget for the animated artwork cache (%LOCALAPPDATA%\Tonearm\MotionCache).
//
// The cache holds one directory per downloaded video, named by a 16-digit hex key,
// plus an AlbumIndex folder that maps releases to those directories. This module
// keeps the total under a budget by evicting the least recently used complete
// directories. It is portable so it can be tested with temporary folders.
//
// Safety rules:
//  - Only direct children of the cache root whose names Tonearm itself creates are
//    ever touched (<key>, <key>-building, <key>.evict). Anything else is left alone.
//  - Symbolic links, junctions and other reparse points are never followed or removed
//    through; their targets are never counted or deleted.
//  - Directories leased by a running download or by a video that is loaded for
//    playback are never evicted. A directory is first renamed to <key>.evict and only
//    then deleted, so a directory whose files are open (Windows refuses the rename)
//    stays complete instead of half-deleted.
//  - Unfinished downloads (<key>-building) are removed only when they are not leased
//    and have not been written to for `staleAfter`.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace tonearm {

constexpr std::uint64_t kDefaultMotionCacheBudget = 1024ull * 1024ull * 1024ull; // 1 GiB

// Process-wide record of cache directories in use. Holding a lease keeps a
// directory out of eviction. Keys are the directory names (the 16-digit hex key).
class MotionCacheLeases {
public:
	static MotionCacheLeases &shared();

	void acquire(const std::string &key);
	void release(const std::string &key);
	bool leased(const std::string &key) const;

	class Lease {
	public:
		Lease() = default;
		Lease(MotionCacheLeases &owner, std::string key);
		~Lease();
		Lease(Lease &&other) noexcept;
		Lease &operator=(Lease &&other) noexcept;
		Lease(const Lease &) = delete;
		Lease &operator=(const Lease &) = delete;
		const std::string &key() const { return key_; }

	private:
		MotionCacheLeases *owner_ = nullptr;
		std::string key_;
	};

private:
	mutable std::mutex mutex_;
	std::map<std::string, int> counts_;
};

// The cache key of a path inside the cache (".../MotionCache/<key>/artwork.mp4"),
// or empty when the path does not look like a cached motion video.
std::string motionCacheKeyForPath(const std::filesystem::path &file);

// True for a 16-digit lower-case hex key.
bool isMotionCacheKey(const std::string &name);

// Marks a complete cache directory as just used (LRU order).
void touchMotionCacheEntry(const std::filesystem::path &dir);

struct CachePruneResult {
	std::uint64_t bytesBefore = 0;
	std::uint64_t bytesAfter = 0;
	int evicted = 0;          // complete directories removed for the budget
	int staleRemoved = 0;     // abandoned downloads and leftovers removed
	int indexRemoved = 0;     // album index files that pointed at nothing
	bool overBudget = false;  // still above budget (everything left is leased or in use)
};

struct CachePruneOptions {
	std::uint64_t budget = kDefaultMotionCacheBudget;
	std::chrono::seconds staleAfter{60 * 60};
	const MotionCacheLeases *leases = nullptr; // defaults to MotionCacheLeases::shared()
};

// Enforces the budget on `root`. Never throws; I/O errors leave entries in place.
CachePruneResult pruneMotionCache(const std::filesystem::path &root, const CachePruneOptions &options = {});

} // namespace tonearm
