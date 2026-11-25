// SPDX-License-Identifier: GPL-2.0-or-later
#include "raster.hpp"

#include "motion.hpp"
#include "turntable.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tonearm {

using namespace tokens;

uint64_t nextImageId()
{
	static std::atomic<uint64_t> counter{1};
	return counter.fetch_add(1, std::memory_order_relaxed);
}

static ImagePtr share(Image &&img)
{
	return std::make_shared<const Image>(std::move(img));
}

std::string formatTime(int seconds, bool negative)
{
	seconds = std::max(0, seconds);
	const int h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
	char buf[32];
	// U+2212 minus sign reads better than a hyphen next to tabular figures.
	const char *sign = negative ? "\xE2\x88\x92" : "";
	if (h > 0)
		std::snprintf(buf, sizeof buf, "%s%d:%02d:%02d", sign, h, m, s);
	else
		std::snprintf(buf, sizeof buf, "%s%d:%02d", sign, m, s);
	return buf;
}

DisplayContent resolveContent(const TrackInfo &track, const std::string &appName)
{
	DisplayContent c;
	c.kind = DisplayContent::Kind::Track;
	c.title = track.title;
	c.artist = track.artist;
	c.album = track.album;
	c.sourceName = appName;
	c.artwork = track.artwork;
	c.artworkHash = track.artworkHash;

	// Never invent metadata: promote what exists instead of showing "Unknown".
	if (c.title.empty() && !c.artist.empty()) {
		c.title = c.artist;
		c.artist.clear();
	}
	if (c.title.empty() && !c.album.empty()) {
		c.title = c.album;
		c.album.clear();
	}
	if (c.title.empty())
		c.title = appName;
	if (c.album == c.title)
		c.album.clear();
	c.identity = track.identity();
	return c;
}

DisplayContent placeholderContent(const std::string &text)
{
	DisplayContent c;
	c.kind = DisplayContent::Kind::Placeholder;
	c.title = text;
	c.identity = "\x1fplaceholder\x1f" + text;
	return c;
}

// ---------------------------------------------------------------------------

namespace {

Rect localRect(const Rect &r, const Rect &origin)
{
	return {r.x - origin.x, r.y - origin.y, r.w, r.h};
}

// Composites a `src` that covers rect `r` (source coords) into `dst` covering `origin`.
void place(Image &dst, const Image &src, const Rect &r, const Rect &origin, float opacity = 1.0f)
{
	composite(dst, src, static_cast<int>(std::lround(r.x - origin.x)), static_cast<int>(std::lround(r.y - origin.y)), opacity);
}

uint64_t seedFor(const DisplayContent &c)
{
	const std::string key = c.title + "|" + c.artist + "|" + c.album;
	return hashBytes(reinterpret_cast<const uint8_t *>(key.data()), key.size());
}

// Color with alpha blended over an opaque tone, for contrast checks.
Color over(const Color &fg, const Color &tone)
{
	return {tone.r + (fg.r - tone.r) * fg.a, tone.g + (fg.g - tone.g) * fg.a, tone.b + (fg.b - tone.b) * fg.a, 1};
}

// Keeps text legible: raises alpha first (stays in the preset's character), then lightness.
Color legible(Color fg, const Color &tone, float ratio)
{
	for (int i = 0; i < 12 && contrastRatio(over(fg, tone), tone) < ratio && fg.a < 1.0f; ++i)
		fg.a = std::min(1.0f, fg.a + 0.08f);
	if (contrastRatio(over(fg, tone), tone) >= ratio)
		return fg;
	Color solid = ensureContrast(over(fg, tone), tone, ratio);
	solid.a = 1;
	return solid;
}

Image artworkImage(const DisplayContent &c, const Palette &pal, int w, int h)
{
	if (c.artwork && !c.artwork->empty())
		return coverFit(*c.artwork, w, h);
	const int side = std::max(w, h);
	Image fb = fallbackArtwork(side, seedFor(c), pal);
	return coverFit(fb, w, h);
}

// Blurred, color-graded artwork filling `w` x `h`; the backbone of the Glass surface.
Image backdrop(const DisplayContent &c, const Palette &pal, int w, int h, float blurAmount)
{
	// Work at 1/16 scale: a large, smooth blur costs almost nothing there.
	const int sw = std::max(6, w / 16), sh = std::max(6, h / 16);
	Image small = artworkImage(c, pal, sw, sw); // square sample keeps the cover's color balance
	small = coverFit(small, sw, sh);
	blur(small, std::max(2.0f, std::min(sw, sh) * (0.35f + 0.45f * blurAmount)));
	adjustColor(small, 1.35f, 0.58f);
	Image big = resize(small, w, h);
	blur(big, 3.0f);
	return big;
}

void glassSurface(Image &card, const Style &s, const DisplayContent &c, const LayerSet &L, float u)
{
	const int w = card.width(), h = card.height();
	if (s.artworkBackground) {
		Image bd = backdrop(c, L.palette, w, h, s.backdropBlur);
		composite(card, bd, 0, 0);
		// Tint toward the deep tone so text keeps contrast on bright covers.
		fillRoundRect(card, {0, 0, (float)w, (float)h}, 0, Paint::solid(L.palette.deep.withAlpha(0.42f)));
	} else {
		fillRoundRect(card, {0, 0, (float)w, (float)h}, 0, Paint::solid(s.surfaceColor));
	}
	// Soft top sheen, the way light sits on frosted glass.
	fillRoundRect(card, {0, 0, (float)w, (float)h}, 0,
		      Paint::linear({0, 0}, {0, h * 0.6f}, {{0, Color{1, 1, 1, 0.09f}}, {1, Color{1, 1, 1, 0}}}));
	// Subtle vignette at the bottom grounds the progress row.
	fillRoundRect(card, {0, 0, (float)w, (float)h}, 0,
		      Paint::linear({0, h * 0.5f}, {0, (float)h}, {{0, Color{0, 0, 0, 0}}, {1, Color{0, 0, 0, 0.16f}}}));
	(void)u;
}

void editorialSurface(Image &card, const Style &s, const DisplayContent &c, const LayerSet &L, float u)
{
	const int w = card.width(), h = card.height();
	const Palette &p = L.palette;
	const bool hasArt = L.layout.artVisible;
	const Rect a = hasArt ? localRect(L.layout.art, L.layout.card) : Rect{};
	const bool side = L.layout.artPlacement == ArtPlacement::FullBleedLeft;
	Image art;
	Color edgeTint = p.deep;
	if (hasArt) {
		art = artworkImage(c, p, (int)std::lround(a.w), (int)std::lround(a.h));
		// The artwork's inner edge color, so the surface can pick up where the cover ends.
		const int strip = std::max(2, static_cast<int>((side ? art.width() : art.height()) * 0.08f));
		Image edge(side ? strip : art.width(), side ? art.height() : strip);
		for (int y = 0; y < edge.height(); ++y) {
			const uint8_t *src = side ? art.row(y) + (art.width() - strip) * 4 : art.row(art.height() - strip + y);
			std::copy(src, src + edge.stride(), edge.row(y));
		}
		const Color e = averageColor(edge);
		edgeTint = Color{e.r, e.g, e.b, 1}.mix(p.deep, 0.55f);
	}

	// Paper-like gradient: lifted next to the artwork, settling into the deep tone.
	const Color nearC = hasArt ? p.deep.mix(edgeTint, 0.45f) : p.deep;
	const Color farC = p.deep.mix(p.secondary, 0.3f).mix(Color{0, 0, 0, 1}, 0.2f);
	const Vec2 g0 = hasArt ? (side ? Vec2{a.right(), 0} : Vec2{0, a.bottom()}) : Vec2{0, 0};
	const Vec2 g1 = hasArt && !side ? Vec2{(float)w * 0.6f, (float)h} : Vec2{(float)w, (float)h};
	fillRoundRect(card, {0, 0, (float)w, (float)h}, 0, Paint::linear(g0, g1, {{0, nearC}, {1, farC}}));
	// A low, warm wash of the accent from the far corner.
	fillRoundRect(card, {0, 0, (float)w, (float)h}, 0,
		      Paint::radial({w * 1.0f, h * 1.1f}, std::max(w, h) * 0.9f,
				    {{0, p.accent.withAlpha(0.18f)}, {1, p.accent.withAlpha(0.0f)}}));
	if (s.artworkBackground && s.backdropBlur > 0) {
		Image bd = backdrop(c, p, w, h, 1.0f);
		composite(card, bd, 0, 0, 0.22f * s.backdropBlur);
	}

	if (hasArt) {
		// Feather the inner edge of the artwork into the surface, over a haze of its own
		// edge color, so the hand-off reads as light falling off rather than a seam.
		const float feather = side ? a.w * 0.32f : a.h * 0.32f;
		const float reach = (side ? a.w : a.h) * 0.5f;
		const Vec2 from = side ? Vec2{a.right() - feather, 0} : Vec2{0, a.bottom() - feather};
		const Vec2 to = side ? Vec2{a.right() + reach, 0} : Vec2{0, a.bottom() + reach};
		fillRoundRect(card, {0, 0, (float)w, (float)h}, 0,
			      Paint::linear(from, to, {{0, edgeTint.withAlpha(0.85f)}, {0.5f, edgeTint.withAlpha(0.3f)}, {1, edgeTint.withAlpha(0)}}));
		for (int y = 0; y < art.height(); ++y) {
			uint8_t *px = art.row(y);
			for (int x = 0; x < art.width(); ++x, px += 4) {
				const float d = side ? (art.width() - 0.5f - x) : (art.height() - 0.5f - y);
				if (d >= feather)
					continue;
				const float t = std::clamp(d / feather, 0.0f, 1.0f);
				const float k = t * t * (3 - 2 * t);
				for (int ch = 0; ch < 4; ++ch)
					px[ch] = static_cast<uint8_t>(px[ch] * k + 0.5f);
			}
		}
		composite(card, art, (int)std::lround(a.x), (int)std::lround(a.y));
	}
	(void)u;
}

void neonSurface(Image &card, const Style &s, const LayerSet &L, float u)
{
	const int w = card.width(), h = card.height();
	const Color base = s.surfaceColor.mix(L.palette.deep, 0.18f);
	fillRoundRect(card, {0, 0, (float)w, (float)h}, 0, Paint::solid(base.withAlpha(s.surfaceOpacity)));
	// Faint accent light from the top edge, like a tube reflecting on glass.
	fillRoundRect(card, {0, 0, (float)w, (float)h}, 0,
		      Paint::linear({0, 0}, {0, h * 0.7f}, {{0, L.accent.withAlpha(0.10f)}, {1, L.accent.withAlpha(0.0f)}}));
	(void)u;
}

} // namespace


Image fallbackArtwork(int size, uint64_t seed, const Palette &pal)
{
	Image img(size, size);
	const float sz = static_cast<float>(size);
	const float angle = static_cast<float>(seed % 628) / 100.0f;
	const Vec2 a{sz * 0.5f - std::cos(angle) * sz * 0.6f, sz * 0.5f - std::sin(angle) * sz * 0.6f};
	const Vec2 b{sz * 0.5f + std::cos(angle) * sz * 0.6f, sz * 0.5f + std::sin(angle) * sz * 0.6f};
	fillRoundRect(img, {0, 0, sz, sz}, 0,
		      Paint::linear(a, b, {{0, pal.secondary.mix(pal.deep, 0.2f)}, {1, pal.deep.mix(Color{0, 0, 0, 1}, 0.2f)}}));
	// A quiet record motif: grooves and a label in the accent color.
	const float cx = sz * 0.5f, cy = sz * 0.5f, R = sz * 0.36f;
	for (int y = 0; y < size; ++y) {
		uint8_t *p = img.row(y);
		for (int x = 0; x < size; ++x, p += 4) {
			const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
			const float d = std::sqrt(dx * dx + dy * dy);
			float add = 0;
			if (d < R) {
				const float groove = 0.5f + 0.5f * std::sin(d / (sz * 0.012f));
				add = 0.035f * groove * std::clamp((R - d) / (sz * 0.01f), 0.0f, 1.0f);
			}
			const float label = std::clamp(sz * 0.11f - d + 0.5f, 0.0f, 1.0f);
			for (int ch = 0; ch < 3; ++ch) {
				float v = p[ch] / 255.0f + add;
				const float lc = ch == 0 ? pal.accent.b : (ch == 1 ? pal.accent.g : pal.accent.r);
				v = v * (1 - label * 0.85f) + lc * label * 0.85f;
				p[ch] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
			}
		}
	}
	addNoise(img, 0.02f, static_cast<uint32_t>(seed));
	return img;
}

static LayerImage textLayer(TextEngine &te, const std::string &str, const TextBox &box, const Color &color, bool marquee,
			    int pad, const Style &s, const Color &glowTint, float glowAmount, float u)
{
	LayerImage L;
	if (!box.visible || str.empty() || box.rect.w <= 0)
		return L;
	TextRun run = te.render(str, box.font, color, box.rect.w, !marquee, pad);
	if (run.image.empty())
		return L;
	L.naturalWidth = run.metrics.width;
	L.overflow = marquee && run.metrics.width > box.rect.w + 0.5f;
	L.box = box.rect;
	float x = box.rect.x;
	if (!L.overflow && box.align == TextAlign::Center)
		x = box.rect.x + std::round((box.rect.w - run.metrics.width) * 0.5f);
	const float y = box.rect.y + std::round((box.rect.h - run.metrics.lineHeight) * 0.5f);
	if (glowAmount > 0) {
		Image withGlow(run.image.width(), run.image.height());
		glow(withGlow, run.image, 0, 0, 6.0f * u, glowTint, glowAmount);
		composite(withGlow, run.image, 0, 0);
		run.image = std::move(withGlow);
	}
	L.dst = {x - run.pad, y - run.pad, static_cast<float>(run.image.width()), static_cast<float>(run.image.height())};
	L.image = share(std::move(run.image));
	L.id = nextImageId();
	(void)s;
	return L;
}

std::shared_ptr<LayerSet> rasterizeLayers(const Style &s, const DisplayContent &c, TextEngine &te)
{
	auto set = std::make_shared<LayerSet>();
	static std::atomic<uint64_t> serials{1};
	set->serial = serials.fetch_add(1);
	set->identity = c.identity;
	set->artworkHash = c.artworkHash;
	set->placeholder = c.kind == DisplayContent::Kind::Placeholder;

	LayoutContent lc;
	lc.hasArtwork = !set->placeholder;
	lc.hasAlbum = !c.album.empty();
	lc.hasArtist = !c.artist.empty();
	lc.hasTimeline = !set->placeholder;
	set->layout = computeLayout(s, lc);
	const Layout &L = set->layout;
	const float u = L.unit;
	const Composition comp = s.composition;

	// Palette: from artwork, else from a seed so tracks without art still feel distinct.
	if (c.artwork && !c.artwork->empty())
		set->palette = extractPalette(*c.artwork);
	if (!set->palette.valid) {
		const uint64_t seed = seedFor(c);
		const float hue = static_cast<float>(seed % 360) * 3.14159265f / 180.0f;
		set->palette = fallbackPalette(fromOklab(fromOklch({0.7f, set->placeholder ? 0.02f : 0.09f, hue})));
	}
	if (!s.artworkBackground && comp != Composition::Editorial) {
		// Artwork-independent surfaces keep a neutral deep tone.
		set->palette.deep = s.surfaceColor;
	}
	set->accent = s.accentMode == AccentMode::Custom ? s.accentColor : set->palette.accent;
	if (comp == Composition::Neon && s.accentMode == AccentMode::FromArtwork) {
		// Neon wants a vivid light source: push chroma, keep lightness in a readable band.
		Oklch lch = toOklch(toOklab(set->accent));
		if (lch.C > 0.04f) {
			lch.C = std::min(0.26f, lch.C * 1.45f + 0.03f);
			lch.L = std::clamp(lch.L, 0.68f, 0.78f);
			set->accent = fromOklab(fromOklch(lch));
		}
	}

	// --- Surface -------------------------------------------------------------
	const int W = s.width, H = s.height;
	const Rect full{0, 0, static_cast<float>(W), static_cast<float>(H)};
	Image surface(W, H);
	const Rect card = L.card;
	const bool drawSurface = s.showSurface;
	if (drawSurface) {
		if (s.shadowStrength > 0) {
			const float k = s.shadowStrength;
			// Two shadows: a tight contact shadow and a broad ambient one.
			dropShadow(surface, card, L.cardRadius, 3.0f * u, 1.0f * u, Color{0, 0, 0, 0.28f * k});
			dropShadow(surface, card, L.cardRadius, 22.0f * u, 9.0f * u, Color{0, 0, 0, 0.42f * k});
		}
		Image cardImg(static_cast<int>(std::lround(card.w)), static_cast<int>(std::lround(card.h)));
		switch (comp) {
		case Composition::Glass:
			glassSurface(cardImg, s, c, *set, u);
			break;
		case Composition::Editorial:
			editorialSurface(cardImg, s, c, *set, u);
			break;
		case Composition::Neon:
			neonSurface(cardImg, s, *set, u);
			break;
		case Composition::Minimal:
			fillRoundRect(cardImg, {0, 0, card.w, card.h}, 0, Paint::solid(s.surfaceColor));
			break;
		case Composition::Turntable: {
			// A matte plinth: graphite faintly warmed by the cover, lit from the upper left.
			const Color base = s.surfaceColor.mix(set->accent, 0.045f);
			fillRoundRect(cardImg, {0, 0, card.w, card.h}, 0, Paint::linear({0, 0}, {card.w * 0.25f, card.h},
				{{0, base.mix(Color::hex("#3a3d40"), 0.30f)}, {1, base.mix(Color::hex("#000000"), 0.18f)}}));
			if (L.recordVisible) {
				const Vec2 c{L.record.cx() - card.x, L.record.cy() - card.y};
				fillRoundRect(cardImg, {0, 0, card.w, card.h}, 0, Paint::radial(c, L.record.w * 1.1f,
					{{0, set->accent.withAlpha(0.07f)}, {1, set->accent.withAlpha(0.0f)}}));
			}
			fillRoundRect(cardImg, {0, 0, card.w, card.h * 0.5f}, 0, Paint::linear({0, 0}, {0, card.h * 0.5f},
				{{0, Color::hex("#ffffff0d")}, {1, Color::hex("#ffffff00")}}));
			if (L.recordVisible)
				turntable::drawPlinthHardware(cardImg, {L.armPivot.x - card.x, L.armPivot.y - card.y}, L.record.w * 0.5f);
			break;
		}
		}
		if (s.noise > 0)
			addNoise(cardImg, s.noise, static_cast<uint32_t>(set->serial * 2654435761u));
		applyRoundedMask(cardImg, L.cardRadius);
		const float opacity = comp == Composition::Neon ? 1.0f : s.surfaceOpacity;
		if (comp == Composition::Minimal || comp == Composition::Glass)
			multiplyAlpha(cardImg, opacity);

		// Tone behind the text, measured from the actual pixels for contrast decisions.
		{
			const Rect tc = localRect(L.textColumn, card);
			const int tx = std::clamp(static_cast<int>(tc.x), 0, cardImg.width() - 1);
			const int ty = std::clamp(static_cast<int>(tc.y), 0, cardImg.height() - 1);
			const int tw = std::clamp(static_cast<int>(tc.w), 1, cardImg.width() - tx);
			const int th = std::clamp(static_cast<int>(tc.h), 1, cardImg.height() - ty);
			Image region(tw, th);
			for (int y = 0; y < th; ++y)
				std::copy(cardImg.row(ty + y) + tx * 4, cardImg.row(ty + y) + (tx + tw) * 4, region.row(y));
			Color tone = averageColor(region);
			// Translucent surfaces sit over unknown video; assume a dark scene behind them.
			tone = Color{0.06f, 0.06f, 0.07f, 1}.mix(Color{tone.r, tone.g, tone.b, 1}, tone.a);
			set->surfaceTone = tone;
		}

		// Inner hairline catches light along the edge.
		if (s.borderOpacity > 0 && comp != Composition::Neon) {
			strokeRoundRect(cardImg, {0, 0, card.w, card.h}, L.cardRadius, std::max(1.0f, u),
					Paint::linear({0, 0}, {0, card.h},
						      {{0, Color{1, 1, 1, s.borderOpacity}}, {1, Color{1, 1, 1, s.borderOpacity * 0.35f}}}));
		}
		if (comp == Composition::Neon) {
			strokeRoundRect(cardImg, {0, 0, card.w, card.h}, L.cardRadius, std::max(1.0f, 1.5f * u),
					Paint::solid(set->accent.withAlpha(s.borderOpacity)));
		}
		place(surface, cardImg, card, full);
	} else {
		set->surfaceTone = Color{0.06f, 0.06f, 0.07f, 1};
	}
	set->surface.image = share(std::move(surface));
	set->surface.id = nextImageId();
	set->surface.dst = full;
	set->surface.box = card;

	// --- Neon glow ring ------------------------------------------------------
	if (drawSurface && comp == Composition::Neon && s.glowStrength > 0) {
		const float r = 12.0f * u;
		const int pad = static_cast<int>(std::ceil(r * 1.6f));
		Image ring(static_cast<int>(card.w) + pad * 2, static_cast<int>(card.h) + pad * 2);
		strokeRoundRect(ring, {(float)pad, (float)pad, card.w, card.h}, L.cardRadius, 2.5f * u, Paint::solid(set->accent));
		blur(ring, r);
		multiplyAlpha(ring, std::min(1.0f, s.glowStrength));
		set->glowRing.image = share(std::move(ring));
		set->glowRing.id = nextImageId();
		set->glowRing.dst = {card.x - pad, card.y - pad, card.w + pad * 2, card.h + pad * 2};
		set->glowRing.box = card;
	}

	// --- Artwork -------------------------------------------------------------
	const bool artBaked = comp == Composition::Editorial;
	if (L.artVisible && !artBaked) {
		const Rect a = L.art;
		const bool fill = L.artPlacement == ArtPlacement::Fill;
		const float shadowBlur = fill ? 0 : 14.0f * u;
		const int pad = s.artworkShadow > 0 && !fill ? static_cast<int>(std::ceil(shadowBlur * 1.6f + 6 * u)) : 0;
		Image layer(static_cast<int>(std::lround(a.w)) + pad * 2, static_cast<int>(std::lround(a.h)) + pad * 2);
		const Rect inLayer{(float)pad, (float)pad, std::round(a.w), std::round(a.h)};
		if (pad > 0) {
			dropShadow(layer, inLayer, L.artRadius, shadowBlur, 5.0f * u, Color{0, 0, 0, 0.5f * s.artworkShadow});
			dropShadow(layer, inLayer, L.artRadius, 2.0f * u, 1.0f * u, Color{0, 0, 0, 0.25f * s.artworkShadow});
		}
		Image art = artworkImage(c, set->palette, static_cast<int>(inLayer.w), static_cast<int>(inLayer.h));
		if (fill && L.scrimVisible) {
			const Rect sc = localRect(L.scrim, a);
			fillRoundRect(art, {0, 0, inLayer.w, inLayer.h}, 0,
				      Paint::linear({0, sc.y}, {0, sc.bottom()},
						    {{0, set->palette.deep.withAlpha(0)},
						     {0.55f, set->palette.deep.withAlpha(0.72f)},
						     {1, set->palette.deep.withAlpha(0.94f)}}));
			// Text sits on the scrim, so measure tone there.
			Image region(art.width(), std::max(1, static_cast<int>(art.height() * 0.25f)));
			for (int y = 0; y < region.height(); ++y)
				std::copy(art.row(art.height() - region.height() + y), art.row(art.height() - region.height() + y) + art.stride(),
					  region.row(y));
			const Color t = averageColor(region);
			set->surfaceTone = {t.r, t.g, t.b, 1};
		}
		applyRoundedMask(art, L.artRadius);
		// With animated artwork on, the scrim and edge stroke are also kept as their own
		// layer, drawn over the video so text contrast and the preset's edge survive.
		const bool overlay = wantsMotion(s) && !set->placeholder;
		Image ov;
		if (overlay) {
			ov = Image(static_cast<int>(inLayer.w), static_cast<int>(inLayer.h));
			if (fill && L.scrimVisible) {
				const Rect sc = localRect(L.scrim, a);
				fillRoundRect(ov, {0, 0, inLayer.w, inLayer.h}, 0,
					      Paint::linear({0, sc.y}, {0, sc.bottom()},
							    {{0, set->palette.deep.withAlpha(0)},
							     {0.55f, set->palette.deep.withAlpha(0.72f)},
							     {1, set->palette.deep.withAlpha(0.94f)}}));
				applyRoundedMask(ov, L.artRadius);
			}
		}
		auto stroke = [&](Image &img) {
			if (comp == Composition::Neon) {
				strokeRoundRect(img, {0, 0, inLayer.w, inLayer.h}, L.artRadius, std::max(1.0f, u), Paint::solid(set->accent.withAlpha(0.55f)));
			} else if (comp == Composition::Turntable) {
				// Printed card stock: a soft sheen, a shaded opening edge where the record
				// leaves the sleeve, and a hairline catching the light.
				const float w = inLayer.w, h = inLayer.h;
				fillRoundRect(img, {0, 0, w, h}, L.artRadius, Paint::linear({0, 0}, {w * 0.7f, h * 0.7f},
					{{0, Color{1, 1, 1, 0.10f}}, {0.5f, Color{1, 1, 1, 0.0f}}}));
				const float dx = L.record.cx() - a.cx(), dy = L.record.cy() - a.cy();
				const float len = std::max(1.0f, std::hypot(dx, dy));
				const Vec2 m{w * 0.5f + dx / len * w * 0.32f, h * 0.5f + dy / len * h * 0.32f};
				const Vec2 e{w * 0.5f + dx / len * w * 0.5f, h * 0.5f + dy / len * h * 0.5f};
				fillRoundRect(img, {0, 0, w, h}, L.artRadius, Paint::linear(m, e,
					{{0, Color{0, 0, 0, 0.0f}}, {1, Color{0, 0, 0, 0.30f}}}));
				strokeRoundRect(img, {0, 0, w, h}, L.artRadius, std::max(1.0f, u * 0.75f),
						Paint::linear({0, 0}, {0, h}, {{0, Color{1, 1, 1, 0.20f}}, {1, Color{1, 1, 1, 0.05f}}}));
			} else if (comp == Composition::Glass && !fill) {
				strokeRoundRect(img, {0, 0, inLayer.w, inLayer.h}, L.artRadius, std::max(1.0f, u * 0.75f),
						Paint::solid(Color{1, 1, 1, 0.12f}));
			}
		};
		stroke(art);
		if (overlay) {
			stroke(ov);
			set->artOverlay.image = share(std::move(ov));
			set->artOverlay.id = nextImageId();
			set->artOverlay.dst = {a.x, a.y, inLayer.w, inLayer.h};
			set->artOverlay.box = a;
		}
		composite(layer, art, pad, pad);
		set->art.image = share(std::move(layer));
		set->art.id = nextImageId();
		set->art.dst = {a.x - pad, a.y - pad, std::round(a.w) + pad * 2, std::round(a.h) + pad * 2};
		set->art.box = a;
	}
	if (L.artVisible && !set->placeholder && (!artBaked || drawSurface)) {
		auto &m = set->videoMask;
		m.valid = true;
		m.box = artBaked ? L.art : Rect{L.art.x, L.art.y, std::round(L.art.w), std::round(L.art.h)};
		m.radius = artBaked ? 0.0f : L.artRadius;
		m.clip = card;
		m.clipRadius = L.cardRadius;
		if (artBaked) {
			const bool side = L.artPlacement == ArtPlacement::FullBleedLeft;
			m.featherEdge = side ? 1 : 2;
			m.feather = (side ? L.art.w : L.art.h) * 0.32f; // same hand-off as editorialSurface
		}
	}

	// --- Turntable: platter, pressing, sheen and tonearm ----------------------
	// Rendered once per track. Only the pressing and the arm move, as GPU rotations;
	// the light (sheen) and shadows stay put, so the record reads as turning under it.
	if (comp == Composition::Turntable && L.recordVisible && !L.record.empty()) {
		const int side = std::max(8, static_cast<int>(std::lround(L.record.w)));
		const float R = side * 0.5f;
		const uint64_t seed = seedFor(c);
		// Labels belong to the album, so every track of an album shares its label design.
		const std::string albumKey = c.artist + "|" + (c.album.empty() ? c.title : c.album);
		const uint64_t labelSeed = hashBytes(reinterpret_cast<const uint8_t *>(albumKey.data()), albumKey.size());
		const turntable::Pressing press =
			turntable::pressingFor(set->palette, c.artwork && !c.artwork->empty() ? c.artwork.get() : nullptr, labelSeed);
		const int pad = static_cast<int>(std::ceil(0.16f * R + 8.0f * u));
		set->platter.image = share(turntable::makePlatter(side, pad, u));
		set->platter.id = nextImageId();
		set->platter.dst = {L.record.x - pad, L.record.y - pad, static_cast<float>(side + pad * 2), static_cast<float>(side + pad * 2)};
		set->platter.box = L.record;
		set->record.image = share(turntable::makeRecord(side, press, seed));
		set->record.id = nextImageId();
		set->record.dst = L.record;
		set->record.box = L.record;
		set->sheen.image = share(turntable::makeSheen(side, press));
		set->sheen.id = nextImageId();
		set->sheen.dst = L.record;
		set->sheen.box = L.record;
		set->arm.image = share(turntable::makeArm(L.armZone, L.armPivot, R, set->accent));
		set->arm.id = nextImageId();
		set->arm.dst = L.armZone;
		set->arm.box = L.armZone;
	}

	// --- Colors that depend on the surface -----------------------------------
	const Color tone = set->surfaceTone;
	Color titleC = s.titleColor, artistC = s.artistColor, albumC = s.albumColor;
	if (comp == Composition::Neon) {
		Oklch lch = toOklch(toOklab(set->accent));
		lch.L = std::max(lch.L, 0.82f);
		artistC = fromOklab(fromOklch(lch), 1.0f);
	}
	if (s.autoTextColor) {
		titleC = legible(titleC, tone, 4.5f);
		artistC = legible(artistC, tone, 4.5f);
		albumC = legible(albumC, tone, 3.0f);
	}
	if (s.accentMode == AccentMode::FromArtwork)
		set->accent = ensureContrast(set->accent, tone, 3.0f);
	set->trackColor = titleC.withAlpha(comp == Composition::Minimal ? 0.18f : 0.2f);
	set->timeColor = legible(s.artistColor.withAlpha(std::max(0.62f, s.artistColor.a * 0.85f)), tone, 4.5f);

	// --- Text ----------------------------------------------------------------
	const bool marquee = s.marquee && !s.reducedMotion;
	const bool neonGlow = comp == Composition::Neon && s.glowStrength > 0;
	const int glowPad = neonGlow ? static_cast<int>(std::ceil(10 * u)) : static_cast<int>(std::ceil(2 * u));
	set->title = textLayer(te, c.title, L.title, titleC, marquee, glowPad, s, set->accent, neonGlow ? 0.55f * s.glowStrength : 0, u);
	set->artist = textLayer(te, c.artist, L.artist, artistC, marquee, glowPad, s, set->accent, 0, u);
	set->album = textLayer(te, c.album, L.album, albumC, marquee, glowPad, s, set->accent, 0, u);
	if (s.showSourceApp && L.source.visible && !c.sourceName.empty())
		set->source = textLayer(te, c.sourceName, L.source, set->timeColor, false, glowPad, s, set->accent, 0, u);
	return set;
}

TimeLayers rasterizeTimes(const Style &s, const LayerSet &set, int elapsedSec, int remainingSec, TextEngine &te)
{
	TimeLayers t;
	t.elapsedSec = elapsedSec;
	t.remainingSec = remainingSec;
	t.layerSerial = set.serial;
	const Layout &L = set.layout;
	auto make = [&](const TextBox &box, const std::string &str, bool alignRight) {
		LayerImage li;
		if (!box.visible)
			return li;
		TextRun run = te.render(str, box.font, set.timeColor, 0, false, 2);
		if (run.image.empty())
			return li;
		float x = alignRight ? box.rect.right() - run.metrics.width : box.rect.x;
		const float y = box.rect.y + std::round((box.rect.h - run.metrics.lineHeight) * 0.5f);
		li.dst = {std::round(x) - run.pad, y - run.pad, (float)run.image.width(), (float)run.image.height()};
		li.box = box.rect;
		li.image = share(std::move(run.image));
		li.id = nextImageId();
		return li;
	};
	const bool both = L.elapsed.visible && L.remaining.visible;
	const bool besides = L.remaining.visible && L.remaining.rect.x > L.progress.right();
	if (L.elapsed.visible)
		t.elapsed = make(L.elapsed, formatTime(elapsedSec, false), false);
	if (L.remaining.visible) {
		// With a single label, remaining time sits at the right end of the bar.
		const bool right = both || besides || !L.elapsed.visible;
		TextBox box = L.remaining;
		if (!both && !besides) {
			box.rect.x = L.progress.x;
			box.rect.w = L.progress.w;
		}
		t.remaining = make(box, formatTime(remainingSec, true), right);
	}
	(void)s;
	return t;
}

} // namespace tonearm
