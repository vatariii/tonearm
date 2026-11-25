// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "geometry.hpp"
#include "style.hpp"

namespace tonearm {

enum class TextAlign { Left, Center };

struct TextBox {
	Rect rect;              // line box; height = font line height
	FontSpec font;          // size already scaled to source pixels
	TextAlign align = TextAlign::Left;
	bool visible = false;
};

enum class ArtPlacement { Inset, FullBleedLeft, FullBleedTop, Fill };

struct LayoutContent {
	bool hasArtwork = true;
	bool hasAlbum = true;
	bool hasTimeline = true;
	bool hasArtist = true;
	bool hasSource = true;      // cleared by the layout when the player name does not fit
};

struct Layout {
	float unit = 1;           // design scale: logical px -> source px
	int width = 0, height = 0;
	Rect card;                // surface bounds
	float cardRadius = 0;
	float shadowMargin = 0;

	// Only the Turntable preset uses these; the disc and stylus are separate from the cover sleeve.
	Rect record;            // the pressing (square bounds of the disc)
	Rect armZone;           // tonearm sprite bounds at rest
	Vec2 armPivot;          // bearing centre
	float armOuter = 0;     // arm rotation (radians from rest) at the first and last groove
	float armInner = 0;
	bool recordVisible = false;

	Rect art;
	ArtPlacement artPlacement = ArtPlacement::Inset;
	float artRadius = 0;
	bool artVisible = false;

	TextBox title, artist, album, source;
	TextBox elapsed, remaining;

	Rect progress;            // track rect
	float progressRadius = 0;
	bool progressVisible = false;

	// Overlay layouts draw a scrim from the artwork's deep color behind the text.
	Rect scrim;
	bool scrimVisible = false;

	// Text column (used for marquee clipping and placeholder text)
	Rect textColumn;
};

// Pure function: computes all element boxes for a style at its canvas size.
Layout computeLayout(const Style &style, const LayoutContent &content);

} // namespace tonearm
