// SPDX-License-Identifier: GPL-2.0-or-later
#include "layout.hpp"
#include "turntable.hpp"

#include <algorithm>

namespace tonearm {

using namespace tokens;

namespace {

float lineHeightFor(const FontSpec &f)
{
	const bool serif = f.family.find("Garamond") != std::string::npos || f.family.find("Serif") != std::string::npos;
	return std::ceil(f.sizePx * (serif ? 1.16f : 1.3f));
}

FontSpec scaled(FontSpec f, float u)
{
	f.sizePx = std::max(6.0f, std::round(f.sizePx * u * 2.0f) / 2.0f);
	return f;
}

TextBox box(float x, float y, float w, const FontSpec &font, TextAlign align = TextAlign::Left)
{
	TextBox t;
	t.font = font;
	t.rect = {std::round(x), std::round(y), std::max(0.0f, std::floor(w)), lineHeightFor(font)};
	t.align = align;
	t.visible = true;
	return t;
}

bool showTimes(const Style &s, const LayoutContent &c)
{
	return s.showProgress && s.times != TimeDisplay::Hidden && c.hasTimeline;
}

// Stacks the text block (title, artist, album) starting at y; returns the bottom.
FontSpec sourceFont(const Style &s, float u)
{
	FontSpec f = scaled(s.timeFont, u);
	f.uppercase = true;
	f.tabularNumbers = false;
	f.trackingEm = 0.08f;
	return f;
}

float stackText(Layout &L, const Style &s, const LayoutContent &c, float x, float y, float w, TextAlign align, bool artistFirst)
{
	const float u = L.unit;
	FontSpec tf = scaled(s.titleFont, u), af = scaled(s.artistFont, u), bf = scaled(s.albumFont, u);
	const float gapTitle = std::round((artistFirst ? space1 + 2 : space1 - 1) * u);
	const float gapAlbum = std::round(space1 * u);
	if (s.showSourceApp && c.hasSource) {
		// The player name sits above the text as a small eyebrow label.
		L.source = box(x, y, w, sourceFont(s, u), align);
		y += L.source.rect.h + std::round(space1 * u);
	}
	if (artistFirst && c.hasArtist) {
		L.artist = box(x, y, w, af, align);
		y += L.artist.rect.h + gapTitle;
		L.title = box(x, y, w, tf, align);
		y += L.title.rect.h;
	} else {
		L.title = box(x, y, w, tf, align);
		y += L.title.rect.h;
		if (c.hasArtist) {
			L.artist = box(x, y + gapTitle, w, af, align);
			y += gapTitle + L.artist.rect.h;
		}
	}
	if (s.showAlbum && c.hasAlbum) {
		L.album = box(x, y + gapAlbum, w, bf, align);
		y += gapAlbum + L.album.rect.h;
	}
	return y;
}

float textBlockHeight(const Style &s, const LayoutContent &c, float u)
{
	const bool artistFirst = s.composition == Composition::Editorial;
	float h = lineHeightFor(scaled(s.titleFont, u));
	if (s.showSourceApp && c.hasSource)
		h += lineHeightFor(sourceFont(s, u)) + std::round(space1 * u);
	if (c.hasArtist)
		h += lineHeightFor(scaled(s.artistFont, u)) + std::round((artistFirst ? space1 + 2 : space1 - 1) * u);
	if (s.showAlbum && c.hasAlbum)
		h += lineHeightFor(scaled(s.albumFont, u)) + std::round(space1 * u);
	return h;
}

// Progress bar plus an optional time row below or beside it. Returns height used.
float placeProgress(Layout &L, const Style &s, const LayoutContent &c, float x, float bottom, float w, bool timesBeside)
{
	if (!s.showProgress || !c.hasTimeline)
		return 0;
	const float u = L.unit;
	const float ph = std::max(1.0f, std::round(s.progressHeight * u));
	L.progressVisible = true;
	L.progressRadius = ph * 0.5f;
	if (!showTimes(s, c)) {
		L.progress = {std::round(x), std::round(bottom - ph), std::round(w), ph};
		return ph;
	}
	const FontSpec tf = scaled(s.timeFont, u);
	const float th = lineHeightFor(tf);
	if (s.times != TimeDisplay::Both)
		timesBeside = true;
	if (timesBeside) {
		// [0:42 ━━━━━━━━━━ -2:13] on one line, used by compact layouts.
		const float tw = std::round(tf.sizePx * 2.9f);
		const float cy = bottom - th * 0.5f;
		const bool left = s.times != TimeDisplay::Remaining;
		const bool right = s.times != TimeDisplay::Elapsed;
		const float gap = std::round(space3 * u);
		L.elapsed = box(x, bottom - th, tw, tf, TextAlign::Left);
		L.remaining = box(x + w - tw, bottom - th, tw, tf, TextAlign::Left);
		L.elapsed.visible = left;
		L.remaining.visible = right;
		const float x0 = left ? x + tw + gap : x;
		const float x1 = right ? x + w - tw - gap : x + w;
		L.progress = {std::round(x0), std::round(cy - ph * 0.5f), std::round(x1 - x0), ph};
		return th;
	}
	const float gap = std::round(space2 * u) - 1;
	L.elapsed = box(x, bottom - th, w * 0.5f, tf, TextAlign::Left);
	L.remaining = box(x + w * 0.5f, bottom - th, w * 0.5f, tf, TextAlign::Left);
	L.elapsed.visible = s.times == TimeDisplay::Elapsed || s.times == TimeDisplay::Both;
	L.remaining.visible = s.times == TimeDisplay::Remaining || s.times == TimeDisplay::Both;
	L.progress = {std::round(x), std::round(bottom - th - gap - ph), std::round(w), ph};
	return th + gap + ph;
}

// Height of the progress row (bar plus time labels), without the gap above it.
float progressRowHeight(const Style &s, const LayoutContent &c, float u)
{
	if (!s.showProgress || !c.hasTimeline)
		return 0;
	const float ph = std::max(1.0f, std::round(s.progressHeight * u));
	if (!showTimes(s, c))
		return ph;
	const float th = lineHeightFor(scaled(s.timeFont, u));
	if (s.times != TimeDisplay::Both)
		return std::max(th, ph);
	return th + std::round(space2 * u) - 1 + ph;
}

void baseUnit(const Style &s, float &u)
{
	int bw = 0, bh = 0;
	defaultSize(s.composition, s.layout, bw, bh);
	const float sx = static_cast<float>(s.width) / bw, sy = static_cast<float>(s.height) / bh;
	if (s.layout == LayoutMode::CompactHorizontal || s.layout == LayoutMode::ExpandedHorizontal)
		u = std::min(sy, sx * 1.35f); // horizontal cards may grow wider without growing type
	else
		u = std::min(sx, sy);
	u = std::clamp(u, 0.25f, 8.0f);
}

} // namespace

Layout computeLayout(const Style &s, const LayoutContent &content)
{
	LayoutContent c = content;
	Layout L;
	L.width = s.width;
	L.height = s.height;
	baseUnit(s, L.unit);
	const float u = L.unit;

	// Room for the drop shadow so it is never clipped by the source bounds.
	L.shadowMargin = (s.showSurface && s.shadowStrength > 0) ? std::round((space3 + space4 * s.shadowStrength) * u) : 0;
	L.shadowMargin = std::min(L.shadowMargin, std::round(std::min(s.width, s.height) * 0.12f));
	L.card = Rect{0, 0, static_cast<float>(s.width), static_cast<float>(s.height)}.inset(L.shadowMargin);
	// Shadows fall downward, so give back some of the top margin.
	if (L.shadowMargin > 0) {
		const float lift = std::round(L.shadowMargin * 0.35f);
		L.card.y -= lift;
	}
	L.cardRadius = std::min(std::round(s.cornerRadius * u), std::min(L.card.w, L.card.h) * 0.5f);

	const bool horizontal = s.layout == LayoutMode::CompactHorizontal || s.layout == LayoutMode::ExpandedHorizontal;
	const bool compact = s.layout == LayoutMode::CompactHorizontal;
	const bool art = s.showArtwork && c.hasArtwork;
	const Composition comp = s.composition;

	const float pad = std::round((compact ? space3 : (comp == Composition::Minimal ? space4 : space5)) * u);
	const Rect inner = L.card.inset(pad);


	if (comp == Composition::Turntable) {
		// A record on its platter, in front of its sleeve, with the tonearm bearing
		// beside it. Each layout picks its own record offset and the deck is fitted
		// to the space: horizontal layouts give it the full height, square and
		// vertical layouts give it everything above the text block.
		float ox = 0.5f, oy = 0.0f;
		switch (s.layout) {
		case LayoutMode::CompactHorizontal: ox = 0.50f; oy = 0.0f; break;
		case LayoutMode::ExpandedHorizontal: ox = 0.52f; oy = 0.0f; break;
		case LayoutMode::Square: ox = 0.40f; oy = 0.04f; break;
		case LayoutMode::Vertical: ox = 0.14f; oy = 0.46f; break;
		}
		const Rect ext = turntable::deckExtents(ox, oy);
		const float progH = horizontal && compact ? 0.0f : progressRowHeight(s, c, u);
		const float progGap = progH > 0 ? std::round(space3 * u) : 0;
		const float copyH = textBlockHeight(s, c, u);
		const float deckGap = std::round((compact ? space3 : space4) * u);
		Rect deck;
		float side = 0;
		if (horizontal) {
			side = std::min(inner.h / ext.h, inner.w * (compact ? 0.40f : 0.44f) / ext.w);
			deck = {inner.x, inner.y + (inner.h - ext.h * side) * 0.5f, ext.w * side, ext.h * side};
		} else {
			const float textH = copyH + progGap + progH;
			const float room = std::max(8.0f, inner.h - textH - deckGap);
			side = std::min(inner.w / ext.w, room / ext.h);
			// The arm's extents include its soft shadow; centre on the visible hardware.
			deck = {inner.x + (inner.w - ext.w * side) * 0.5f + 0.03f * side, inner.y + (room - ext.h * side) * 0.5f,
				ext.w * side, ext.h * side};
		}
		side = std::max(8.0f, side);
		const Rect sleeve{std::round(deck.x - ext.x * side), std::round(deck.y - ext.y * side), std::round(side), std::round(side)};
		float R = std::round(side * turntable::kRecordToSleeve * 0.5f);
		Vec2 center{sleeve.cx() + ox * side, sleeve.cy() + oy * side};
		if (compact) {
			// Compact is the distilled deck: no sleeve, just the record and its arm,
			// filling the bar's height with an even margin on the left.
			const Rect bare = turntable::bareDeckExtents();
			const float margin = std::round(space2 * u);
			const float room = std::max(8.0f, L.card.h - margin * 2);
			R = std::floor(std::min(room / bare.h, inner.w * 0.34f / bare.w));
			const float top = L.card.y + (L.card.h - bare.h * R) * 0.5f;
			center = {std::round(L.card.x + margin + std::max(0.0f, -bare.x - turntable::kPlatter) * R + turntable::kPlatter * R),
				  std::round(top - bare.y * R)};
			deck = {center.x + bare.x * R, top, bare.w * R, bare.h * R};
		}
		L.record = {std::round(center.x - R), std::round(center.y - R), R * 2, R * 2};
		L.recordVisible = c.hasArtwork; // the idle placeholder shows no phantom record
		L.armPivot = {L.record.cx() + turntable::kPivotX * R, L.record.cy() + turntable::kPivotY * R};
		L.armZone = turntable::armRestBounds(L.armPivot, R);
		L.armOuter = turntable::armAngleForRadius(0.925f);
		L.armInner = turntable::armAngleForRadius(0.49f);
		if (art && !compact) {
			L.artVisible = true;
			L.artPlacement = ArtPlacement::Inset;
			L.art = sleeve;
			L.artRadius = std::min(std::round(s.artworkRadius * u), sleeve.w * 0.04f);
		}

		if (horizontal) {
			const float textX = std::round(deck.right() + deckGap + space1 * u);
			const float textW = std::max(0.0f, inner.right() - textX);
			L.textColumn = {textX, inner.y, textW, inner.h};
			if (compact) {
				const float ph = std::max(1.0f, std::round(s.progressHeight * u));
				if (s.showProgress && c.hasTimeline) {
					L.progressVisible = true;
					L.progressRadius = ph * 0.5f;
					L.progress = {textX, std::round(inner.bottom() - ph), textW, ph};
				}
				const float y = inner.y + std::max(0.0f, (inner.h - (L.progressVisible ? ph + space2 * u : 0) - copyH) * 0.5f);
				stackText(L, s, c, textX, y, textW, TextAlign::Left, false);
			} else {
				const float used = placeProgress(L, s, c, textX, inner.bottom(), textW, false);
				const float avail = inner.h - (used > 0 ? used + progGap : 0);
				const float y = inner.y + std::max(0.0f, (avail - copyH) * 0.5f);
				stackText(L, s, c, textX, y, textW, TextAlign::Left, false);
			}
		} else {
			// The text block is anchored to the bottom edge so the deck takes the slack.
			const float textY = inner.bottom() - progH - progGap - copyH;
			L.textColumn = {inner.x, textY, inner.w, inner.bottom() - textY};
			stackText(L, s, c, inner.x, textY, inner.w, TextAlign::Left, false);
			placeProgress(L, s, c, inner.x, inner.bottom(), inner.w, false);
		}
		return L;
	}

	if (horizontal) {
		float textX = inner.x;
		if (art) {
			L.artVisible = true;
			if (comp == Composition::Editorial) {
				L.artPlacement = ArtPlacement::FullBleedLeft;
				L.art = {L.card.x, L.card.y, L.card.h, L.card.h};
				L.artRadius = L.cardRadius;
				textX = L.art.right() + std::round((compact ? space4 : space6) * u);
			} else {
				L.artPlacement = ArtPlacement::Inset;
				L.art = {inner.x, inner.y, inner.h, inner.h};
				L.artRadius = std::min(std::round(s.artworkRadius * u), inner.h * 0.5f);
				textX = L.art.right() + std::round((compact ? space3 : space4) * u);
			}
		}
		const float textW = inner.right() - textX;
		L.textColumn = {textX, inner.y, textW, inner.h};

		// Responsive: drop the album line before anything collides.
		if (c.hasAlbum && s.showAlbum) {
			float progNeed = 0;
			if (s.showProgress && c.hasTimeline)
				progNeed = std::round(s.progressHeight * u) + std::round(space3 * u) +
					   (showTimes(s, c) && !compact && s.times == TimeDisplay::Both ? lineHeightFor(scaled(s.timeFont, u)) + std::round(space2 * u) : 0);
			if (textBlockHeight(s, c, u) + progNeed > inner.h + 0.5f)
				c.hasAlbum = false;
		}
		// Then the player name, which is the least important line.
		if (s.showSourceApp && textBlockHeight(s, c, u) > inner.h + 0.5f)
			c.hasSource = false;

		const bool artistFirst = comp == Composition::Editorial;
		if (compact) {
			// Title and artist centered on the art; a hairline progress bar runs along the bottom edge.
			const float block = textBlockHeight(s, c, u);
			float y = inner.y + std::max(0.0f, (inner.h - block) * 0.5f);
			if (s.showProgress && c.hasTimeline) {
				const float ph = std::max(1.0f, std::round(s.progressHeight * u));
				L.progressVisible = true;
				L.progressRadius = ph * 0.5f;
				L.progress = {std::round(textX), std::round(L.card.bottom() - std::round(space2 * u) - ph), std::round(textW), ph};
				y -= std::round(space1 * u * 0.5f);
			}
			stackText(L, s, c, textX, y, textW, TextAlign::Left, artistFirst);
		} else {
			const float progH = placeProgress(L, s, c, textX, inner.bottom(), textW, false);
			const float block = textBlockHeight(s, c, u);
			const float avail = inner.h - (progH > 0 ? progH + std::round(space3 * u) : 0);
			const float y = inner.y + std::max(0.0f, (avail - block) * 0.45f);
			stackText(L, s, c, textX, y, textW, TextAlign::Left, artistFirst);
		}
	} else {
		// Square and vertical layouts.
		const bool overlay = s.layout == LayoutMode::Square && comp == Composition::Glass && art;
		const bool fullBleed = comp == Composition::Editorial && art;
		const bool centered = comp == Composition::Neon;
		const TextAlign align = centered ? TextAlign::Center : TextAlign::Left;
		const bool artistFirst = comp == Composition::Editorial;

		const float block = textBlockHeight(s, c, u);
		const float progH = progressRowHeight(s, c, u);
		const float gapProg = progH > 0 ? std::round(space4 * u) : 0;
		const float gapArt = std::round(space4 * u);

		if (overlay) {
			L.artVisible = true;
			L.artPlacement = ArtPlacement::Fill;
			L.art = L.card;
			L.artRadius = L.cardRadius;
			const float textH = block + gapProg + progH;
			L.scrimVisible = true;
			L.scrim = {L.card.x, L.card.bottom() - (textH + pad * 2) * 1.6f, L.card.w, (textH + pad * 2) * 1.6f};
			const float y = inner.bottom() - textH;
			const float bottom = stackText(L, s, c, inner.x, y, inner.w, align, artistFirst);
			placeProgress(L, s, c, inner.x, bottom + gapProg + progH, inner.w, false);
			L.textColumn = {inner.x, y, inner.w, textH};
		} else {
			float y = inner.y;
			if (art) {
				L.artVisible = true;
				if (fullBleed) {
					L.artPlacement = ArtPlacement::FullBleedTop;
					const float side = std::min(L.card.w * 1.2f, L.card.h - block - gapProg - progH - pad * 2);
					// Full bleed spans the card width; a short card crops the artwork vertically.
					L.art = {L.card.x, L.card.y, L.card.w, std::round(side)};
					L.artRadius = L.cardRadius;
					y = L.art.bottom() + pad;
				} else {
					L.artPlacement = ArtPlacement::Inset;
					const float side = std::max(0.0f, std::min(inner.w, inner.h - block - gapProg - progH - gapArt));
					const float stackH = side + gapArt + block + gapProg + progH;
					const float top = inner.y + std::max(0.0f, std::round((inner.h - stackH) * 0.5f));
					const float ax = centered ? inner.x + (inner.w - side) * 0.5f : inner.x;
					L.art = {std::round(ax), top, std::round(side), std::round(side)};
					L.artRadius = std::min(std::round(s.artworkRadius * u), side * 0.5f);
					y = L.art.bottom() + gapArt;
				}
			} else {
				y = inner.y + std::max(0.0f, std::round((inner.h - block - gapProg - progH) * 0.5f));
			}
			const float bottom = stackText(L, s, c, inner.x, y, inner.w, align, artistFirst);
			placeProgress(L, s, c, inner.x, bottom + gapProg + progH, inner.w, false);
			L.textColumn = {inner.x, y, inner.w, bottom + gapProg + progH - y};
		}
	}

	return L;
}

} // namespace tonearm
