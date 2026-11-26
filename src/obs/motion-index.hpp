// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Persistent, local-only mapping from playback album identity to an already
// downloaded original motion video. A repeat play must not repeat online
// catalog discovery or network fetches.

#include "core/motion.hpp"
#include "media/cache_budget.hpp"

#ifdef _WIN32
namespace tonearm {
std::optional<MotionAsset> cachedMotionForAlbum(const MotionQuery &query);
bool rememberMotionForAlbum(const MotionQuery &query, const MotionAsset &asset);
// Keeps %LOCALAPPDATA%\Tonearm\MotionCache within kDefaultMotionCacheBudget
// (least recently used first; leased entries are kept). Resolver thread only.
CachePruneResult pruneMotionCacheNow();
}
#endif
