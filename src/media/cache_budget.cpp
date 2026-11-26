// SPDX-License-Identifier: GPL-2.0-or-later
#include "cache_budget.hpp"

#include <algorithm>
#include <fstream>
#include <vector>

namespace tonearm {

namespace fs = std::filesystem;

namespace {

std::string utf8Name(const fs::path &p)
{
	const auto s = p.u8string();
	return {s.begin(), s.end()};
}

fs::path fromUtf8(const std::string &s)
{
	return fs::path(std::u8string(s.begin(), s.end()));
}

// A real directory, not a symbolic link, junction or other reparse point.
bool plainDirectory(const fs::path &p)
{
	std::error_code ec;
	const auto st = fs::symlink_status(p, ec);
	return !ec && st.type() == fs::file_type::directory;
}

bool plainFile(const fs::path &p)
{
	std::error_code ec;
	const auto st = fs::symlink_status(p, ec);
	return !ec && st.type() == fs::file_type::regular;
}

std::uint64_t directoryBytes(const fs::path &dir)
{
	std::uint64_t total = 0;
	std::error_code ec;
	// The iterator does not descend into directory symlinks by default.
	for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
	     !ec && it != end; it.increment(ec)) {
		if (!plainFile(it->path()))
			continue;
		std::error_code sizeEc;
		const auto n = fs::file_size(it->path(), sizeEc);
		if (!sizeEc)
			total += n;
	}
	return total;
}

fs::file_time_type lastUse(const fs::path &dir)
{
	std::error_code ec;
	auto t = fs::last_write_time(dir / ".complete", ec);
	if (ec) {
		ec.clear();
		t = fs::last_write_time(dir, ec);
	}
	return ec ? fs::file_time_type::min() : t;
}

bool olderThan(const fs::path &p, std::chrono::seconds age)
{
	std::error_code ec;
	const auto t = fs::last_write_time(p, ec);
	if (ec)
		return false;
	return fs::file_time_type::clock::now() - t > age;
}

// Renames first (fails on Windows while any file inside is open), then deletes.
bool evictDirectory(const fs::path &root, const std::string &key)
{
	std::error_code ec;
	const fs::path dir = root / fromUtf8(key);
	const fs::path doomed = root / fromUtf8(key + ".evict");
	if (fs::exists(doomed, ec)) {
		if (!plainDirectory(doomed))
			return false;
		fs::remove_all(doomed, ec);
		if (ec)
			return false;
	}
	ec.clear();
	fs::rename(dir, doomed, ec);
	if (ec)
		return false; // in use or not ours to move: keep it intact
	fs::remove_all(doomed, ec);
	return !ec;
}

} // namespace

MotionCacheLeases &MotionCacheLeases::shared()
{
	static MotionCacheLeases instance;
	return instance;
}

void MotionCacheLeases::acquire(const std::string &key)
{
	if (key.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	++counts_[key];
}

void MotionCacheLeases::release(const std::string &key)
{
	if (key.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = counts_.find(key);
	if (it != counts_.end() && --it->second <= 0)
		counts_.erase(it);
}

bool MotionCacheLeases::leased(const std::string &key) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return counts_.count(key) != 0;
}

MotionCacheLeases::Lease::Lease(MotionCacheLeases &owner, std::string key) : owner_(&owner), key_(std::move(key))
{
	owner_->acquire(key_);
}

MotionCacheLeases::Lease::~Lease()
{
	if (owner_)
		owner_->release(key_);
}

MotionCacheLeases::Lease::Lease(Lease &&other) noexcept : owner_(other.owner_), key_(std::move(other.key_))
{
	other.owner_ = nullptr;
	other.key_.clear();
}

MotionCacheLeases::Lease &MotionCacheLeases::Lease::operator=(Lease &&other) noexcept
{
	if (this != &other) {
		if (owner_)
			owner_->release(key_);
		owner_ = other.owner_;
		key_ = std::move(other.key_);
		other.owner_ = nullptr;
		other.key_.clear();
	}
	return *this;
}

bool isMotionCacheKey(const std::string &name)
{
	if (name.size() != 16)
		return false;
	return std::all_of(name.begin(), name.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string motionCacheKeyForPath(const fs::path &file)
{
	const std::string key = utf8Name(file.parent_path().filename());
	return isMotionCacheKey(key) ? key : std::string();
}

void touchMotionCacheEntry(const fs::path &dir)
{
	std::error_code ec;
	const fs::path marker = dir / ".complete";
	if (plainFile(marker))
		fs::last_write_time(marker, fs::file_time_type::clock::now(), ec);
}

CachePruneResult pruneMotionCache(const fs::path &root, const CachePruneOptions &options)
{
	CachePruneResult result;
	const MotionCacheLeases &leases = options.leases ? *options.leases : MotionCacheLeases::shared();
	if (root.empty() || !plainDirectory(root))
		return result;

	struct Entry {
		std::string key;
		std::uint64_t bytes;
		fs::file_time_type used;
	};
	std::vector<Entry> complete;
	std::uint64_t total = 0;
	std::error_code ec;
	std::vector<fs::path> children;
	for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
	     it.increment(ec))
		children.push_back(it->path());

	for (const fs::path &child : children) {
		const std::string name = utf8Name(child.filename());
		if (name == "AlbumIndex" || !plainDirectory(child))
			continue;
		std::string key = name;
		enum class Kind { Complete, Building, Evict } kind = Kind::Complete;
		if (name.size() == 25 && name.compare(16, 9, "-building") == 0) {
			key = name.substr(0, 16);
			kind = Kind::Building;
		} else if (name.size() == 22 && name.compare(16, 6, ".evict") == 0) {
			key = name.substr(0, 16);
			kind = Kind::Evict;
		}
		if (!isMotionCacheKey(key))
			continue; // not something Tonearm created
		const std::uint64_t bytes = directoryBytes(child);
		result.bytesBefore += bytes;
		std::error_code rmEc;
		if (kind == Kind::Evict) {
			// Left over from an interrupted eviction.
			fs::remove_all(child, rmEc);
			if (!rmEc)
				++result.staleRemoved;
			else
				total += bytes;
			continue;
		}
		const bool finished = kind == Kind::Complete && plainFile(child / ".complete");
		if (!finished) {
			// An unfinished download: in progress (leased or recently written), or abandoned.
			if (!leases.leased(key) && olderThan(child, options.staleAfter)) {
				fs::remove_all(child, rmEc);
				if (!rmEc) {
					++result.staleRemoved;
					continue;
				}
			}
			total += bytes;
			continue;
		}
		total += bytes;
		complete.push_back({key, bytes, lastUse(child)});
	}

	// Least recently used first.
	std::sort(complete.begin(), complete.end(), [](const Entry &a, const Entry &b) { return a.used < b.used; });
	for (const Entry &e : complete) {
		if (total <= options.budget)
			break;
		if (leases.leased(e.key))
			continue;
		if (evictDirectory(root, e.key)) {
			total -= e.bytes;
			++result.evicted;
		}
	}
	result.bytesAfter = total;
	result.overBudget = total > options.budget;

	// Album index entries that point at directories which no longer exist, entries in
	// the old v1 format (never read since 0.2.0) and abandoned temporary files.
	const fs::path indexDir = root / "AlbumIndex";
	if (plainDirectory(indexDir)) {
		std::vector<fs::path> files;
		ec.clear();
		for (fs::directory_iterator it(indexDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
		     it.increment(ec))
			files.push_back(it->path());
		for (const fs::path &f : files) {
			if (!plainFile(f))
				continue;
			const std::string ext = utf8Name(f.extension());
			bool remove = false;
			if (ext == ".tmp") {
				remove = olderThan(f, options.staleAfter);
			} else if (ext == ".index") {
				std::ifstream in(f, std::ios::binary);
				std::string version, key, shape, location;
				if (!std::getline(in, version) || version != "TONEARM_MOTION_INDEX_V2") {
					remove = in.is_open();
				} else if (std::getline(in, key) && std::getline(in, shape) && std::getline(in, location)) {
					std::error_code exEc;
					remove = !fs::exists(fromUtf8(location), exEc) && !exEc;
				}
			}
			if (remove) {
				std::error_code rmEc;
				if (fs::remove(f, rmEc) && !rmEc)
					++result.indexRemoved;
			}
		}
	}
	return result;
}

} // namespace tonearm
