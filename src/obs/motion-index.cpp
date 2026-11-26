// SPDX-License-Identifier: GPL-2.0-or-later
#include "motion-index.hpp"
#ifdef _WIN32

#include "media/cache_budget.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <system_error>

namespace tonearm {
namespace {
namespace fs = std::filesystem;

std::mutex indexMutex;

std::string u8(const fs::path &path)
{
	const auto bytes = path.u8string();
	return {bytes.begin(), bytes.end()};
}
fs::path fromUtf8(const std::string &path) { return fs::path(std::u8string(path.begin(), path.end())); }

fs::path cacheRoot()
{
	wchar_t p[32768]{};
	const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", p, 32768);
	if (!n || n >= 32768)
		return {};
	return fs::path(p) / L"Tonearm" / L"MotionCache";
}

// The same Windows SMTC album string is stable across tracks, even if artist
// credit changes between tracks. Canonicalize ASCII punctuation and whitespace,
// but preserve any UTF-8 non-ASCII bytes without lossy locale-dependent folding.
std::string canonicalPart(const std::string &s)
{
	std::string out;
	bool space = false;
	for (unsigned char c : s) {
		if (c == '\r' || c == '\n' || c == '\0')
			continue;
		if (c < 128 && std::isspace(c)) {
			space = true;
			continue;
		}
		if (space && !out.empty())
			out.push_back(' ');
		space = false;
		out.push_back(c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c));
	}
	if (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out;
}

std::string identity(const MotionQuery &q)
{
	const std::string performer = q.albumArtist.empty() ? q.artist : q.albumArtist;
	return "tonearm-motion-v2|" + canonicalPart(performer) + "|" +
	       canonicalPart(q.album) + "|" +
	       (q.preferred == MotionShape::Portrait ? "p" : "s") + "|" +
	       (q.quality == MotionQuality::Maximum ? "m" : "b");
}

fs::path indexFile(const fs::path &root, const std::string &identity)
{
	std::uint64_t h = 14695981039346656037ULL;
	for (unsigned char c : identity) {
		h ^= c;
		h *= 1099511628211ULL;
	}
	std::wostringstream os;
	os << L"album-" << std::hex << std::setfill(L'0') << std::setw(16) << h << L".index";
	return root / L"AlbumIndex" / os.str();
}

bool validCachedAsset(const fs::path &root, const fs::path &path)
{
	std::error_code ec;
	if (!fs::exists(path, ec) || !fs::is_regular_file(path, ec))
		return false;
	const auto ext = path.extension().wstring();
	if (ext != L".mp4" && ext != L".m3u8")
		return false;
	// Never follow an index entry outside Tonearm's own motion cache.
	const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
	if (ec)
		return false;
	const fs::path canonicalFile = fs::weakly_canonical(path, ec);
	if (ec)
		return false;
	const fs::path relative = canonicalFile.lexically_relative(canonicalRoot);
	if (relative.empty() || relative.is_absolute())
		return false;
	auto it = relative.begin();
	if (it == relative.end() || *it == fs::path(L"..") || *it == fs::path(L"AlbumIndex"))
		return false;
	if (!fs::exists(path.parent_path() / L".complete", ec))
		return false;
	return fs::file_size(path, ec) >= (ext == L".mp4" ? 4096 : 16) && !ec;
}
} // namespace

std::optional<MotionAsset> cachedMotionForAlbum(const MotionQuery &query)
{
	if (query.album.empty() || (query.artist.empty() && query.albumArtist.empty()))
		return std::nullopt;
	const fs::path root = cacheRoot();
	if (root.empty())
		return std::nullopt;
	const std::string key = identity(query);
	const fs::path path = indexFile(root, key);
	std::lock_guard<std::mutex> lock(indexMutex);
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return std::nullopt;
	std::string version, expectedKey, shape, location;
	if (!std::getline(f, version) || !std::getline(f, expectedKey) ||
	    !std::getline(f, shape) || !std::getline(f, location))
		return std::nullopt;
	if (version != "TONEARM_MOTION_INDEX_V2" || expectedKey != key ||
	    (shape != "s" && shape != "p"))
		return std::nullopt;
	const fs::path media = fromUtf8(location);
	if (!validCachedAsset(root, media))
		return std::nullopt;
	touchMotionCacheEntry(media.parent_path()); // most recently used: last to be evicted
	return MotionAsset{location, shape == "p" ? MotionShape::Portrait : MotionShape::Square};
}

bool rememberMotionForAlbum(const MotionQuery &query, const MotionAsset &asset)
{
	if (query.album.empty() || (query.artist.empty() && query.albumArtist.empty()))
		return false;
	const fs::path root = cacheRoot();
	if (root.empty() || !validCachedAsset(root, fromUtf8(asset.location)))
		return false;
	const std::string key = identity(query);
	const fs::path file = indexFile(root, key);
	std::lock_guard<std::mutex> lock(indexMutex);
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	if (ec)
		return false;
	const fs::path temp = file.wstring() + L".tmp";
	{
		std::ofstream f(temp, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		f << "TONEARM_MOTION_INDEX_V2\n" << key << "\n"
		  << (asset.shape == MotionShape::Portrait ? "p" : "s") << "\n"
		  << asset.location << "\n";
		if (!f.good())
			return false;
	}
	// Replace in one step: a reader (another overlay, or a crash in between) sees
	// either the previous valid index or the new one, never a missing file. If the
	// replacement fails the previous index stays as it was.
	if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		fs::remove(temp, ec);
		return false;
	}
	return true;
}

CachePruneResult pruneMotionCacheNow()
{
	const fs::path root = cacheRoot();
	if (root.empty())
		return {};
	// Index files are read and replaced under this lock; pruning removes dead ones.
	std::lock_guard<std::mutex> lock(indexMutex);
	return pruneMotionCache(root);
}
} // namespace tonearm
#endif
