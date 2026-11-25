// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "color.hpp"
#include "image.hpp"
#include "layout.hpp"
#include "media.hpp"
#include "style.hpp"
#include "text.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace tonearm {

uint64_t nextImageId();

// A rasterized element and where it sits at rest, in source pixels.
struct LayerImage {
	ImagePtr image;
	uint64_t id = 0;     // unique per raster, used by GPU caches
	Rect dst;            // destination of the full image (includes baked padding)
	Rect box;            // the logical element box (text line box, art rect)
	bool overflow = false; // text wider than its box: scroll it
	float naturalWidth = 0;

	bool valid() const { return image && !image->empty(); }
};

// What is being displayed, after resolving missing metadata.
struct DisplayContent {
	enum class Kind { Track, Placeholder } kind = Kind::Track;
	std::string title, artist, album, sourceName;
	ImagePtr artwork;
	uint64_t artworkHash = 0;
	std::string identity;
};

DisplayContent resolveContent(const TrackInfo &track, const std::string &appName);
DisplayContent placeholderContent(const std::string &text);

struct LayerSet {
	uint64_t serial = 0;
	std::string identity;
	uint64_t artworkHash = 0;
	Layout layout;
	Palette palette;
	Color accent;          // progress fill, highlights (contrast checked)
	Color trackColor;      // progress background
	Color timeColor;
	Color surfaceTone;     // average color behind the text column

	LayerImage surface;    // shadow + card (and baked art for full-bleed compositions)
	LayerImage glowRing;   // neon edge glow, animated separately
	LayerImage art;
	LayerImage platter;    // Turntable only: platter and the record's cast shadow (static)
	LayerImage record;     // Turntable only: color-matched pressing, rotated on the GPU (never artwork)
	LayerImage sheen;      // Turntable only: fixed studio light over the turning record
	LayerImage arm;        // Turntable only: tonearm assembly, rotated about its bearing

	// Animated artwork: where a video frame goes and how it is masked, plus the parts
	// of the still artwork that must stay on top of it (scrim, edge stroke).
	struct VideoMask {
		bool valid = false;
		Rect box;              // artwork rect
		float radius = 0;      // artwork corner radius
		Rect clip;             // card rect (video never leaves the card)
		float clipRadius = 0;
		int featherEdge = 0;   // 0 none, 1 right edge, 2 bottom edge (Editorial hand-off)
		float feather = 0;     // feather width in px
	} videoMask;
	LayerImage artOverlay;
	LayerImage title, artist, album, source;
	bool placeholder = false;
};

struct TimeLayers {
	LayerImage elapsed, remaining;
	int elapsedSec = -1, remainingSec = -1;
	uint64_t layerSerial = 0; // LayerSet these belong to
};

// CPU rasterization of every static layer. Runs on a worker thread.
std::shared_ptr<LayerSet> rasterizeLayers(const Style &style, const DisplayContent &content, TextEngine &text);

// Small per-second raster for the time labels.
TimeLayers rasterizeTimes(const Style &style, const LayerSet &set, int elapsedSec, int remainingSec, TextEngine &text);

std::string formatTime(int seconds, bool negative);

// Procedural artwork used when a track has none. Deterministic per seed.
Image fallbackArtwork(int size, uint64_t seed, const Palette &pal);

} // namespace tonearm
