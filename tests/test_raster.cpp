// SPDX-License-Identifier: GPL-2.0-or-later
#include "check.hpp"
#include "support.hpp"

#include "core/demo.hpp"
#include "core/raster.hpp"
#include "core/turntable.hpp"

using namespace tonearm;

namespace {
Color over(const Color &fg, const Color &bg)
{
	return {fg.r * fg.a + bg.r * (1 - fg.a), fg.g * fg.a + bg.g * (1 - fg.a), fg.b * fg.a + bg.b * (1 - fg.a), 1};
}

DisplayContent content(int track, bool art = true)
{
	return resolveContent(demoTrack(track, 256, art), "Apple Music");
}
} // namespace

TEST("raster: every preset rasterizes all of its elements")
{
	for (const auto &p : presetList()) {
		const Style s = makePreset(p.id, LayoutMode::ExpandedHorizontal);
		auto set = rasterizeLayers(s, content(0), testsupport::text());
		const std::string what = p.id;
		CHECK_MSG(set->surface.valid(), what);
		CHECK_MSG(set->title.valid(), what);
		CHECK_MSG(set->artist.valid(), what);
		CHECK_MSG(set->palette.valid, what);
		CHECK_MSG(set->surface.image->width() == s.width && set->surface.image->height() == s.height,
			  what + ": surface should cover the canvas");
	}
}

TEST("raster: text stays readable on artwork-derived surfaces")
{
	for (const auto &p : presetList())
		for (int i = 0; i < demoTrackCount(); ++i) {
			const Style s = makePreset(p.id, LayoutMode::ExpandedHorizontal);
			auto set = rasterizeLayers(s, content(i), testsupport::text());
			const float ratio = contrastRatio(over(set->timeColor, set->surfaceTone), set->surfaceTone);
			CHECK_MSG(ratio >= 4.4f, std::string(p.id) + " track " + std::to_string(i) + ": time contrast " + std::to_string(ratio));
			CHECK_MSG(contrastRatio(set->accent, set->surfaceTone) >= 2.9f || s.accentMode == AccentMode::Custom,
				  std::string(p.id) + " track " + std::to_string(i) + ": accent contrast");
		}
}

TEST("raster: long titles scroll, or truncate when motion is reduced")
{
	const int longTrack = 3; // "Everything I Never Said on the Last Train Home (Extended Version)"
	Style s = makePreset("glass", LayoutMode::ExpandedHorizontal);
	auto set = rasterizeLayers(s, content(longTrack), testsupport::text());
	CHECK(set->title.overflow);
	CHECK(set->title.naturalWidth > set->title.box.w);

	s.reducedMotion = true;
	auto calm = rasterizeLayers(s, content(longTrack), testsupport::text());
	CHECK(!calm.get()->title.overflow);
	CHECK(calm->title.dst.w <= calm->title.box.w + 2 * calm->title.dst.h); // fits its box plus padding

	s.reducedMotion = false;
	s.marquee = false;
	auto off = rasterizeLayers(s, content(longTrack), testsupport::text());
	CHECK(!off->title.overflow);
}

TEST("raster: Turntable keeps the sleeve separate from a colored vinyl record")
{
	Style s = makePreset("turntable", LayoutMode::ExpandedHorizontal);
	auto first = rasterizeLayers(s, content(0), testsupport::text());
	auto second = rasterizeLayers(s, content(2), testsupport::text());
	CHECK(first->art.valid());
	CHECK(first->videoMask.valid);
	CHECK(first->record.valid());
	CHECK(first->arm.valid());
	CHECK(first->record.id != first->art.id);
	CHECK(first->record.image->width() > 60);
	CHECK(first->record.image->data()[3] == 0); // transparent disc corners
	CHECK(first->palette.valid && second->palette.valid);
	CHECK(first->record.image->data() != second->record.image->data());
	CHECK(first->sheen.valid() && first->platter.valid());
	CHECK(first->layout.art.right() > first->layout.record.x);   // the record overlaps its sleeve
	CHECK(first->layout.record.cx() > first->layout.art.cx());  // and the sleeve peeks out behind it
	CHECK(first->layout.art.right() < first->layout.textColumn.x);
}

TEST("raster: Turntable pressings separate from their sleeves")
{
	// Light sleeve -> deep pressing, dark sleeve -> luminous pressing, grey sleeve -> black vinyl.
	Palette light{Color::hex("#efe8da"), Color::hex("#d8432f"), Color::hex("#1a1a1a"), Color::hex("#2a2420"), true};
	Palette dark{Color::hex("#0f2f33"), Color::hex("#5fd3b8"), Color::hex("#ff7f66"), Color::hex("#0a1f22"), true};
	Palette grey{Color::hex("#808080"), Color::hex("#9a9a9a"), Color::hex("#303030"), Color::hex("#202020"), true};
	const auto pl = turntable::pressingFor(light), pd = turntable::pressingFor(dark), pg = turntable::pressingFor(grey);
	CHECK(pl.colored && pd.colored && !pg.colored);
	CHECK(contrastRatio(pl.vinyl, light.dominant) > 2.5f);
	CHECK(contrastRatio(pd.vinyl, dark.dominant) > 2.5f);
	CHECK(relativeLuminance(pg.vinyl) < 0.02f);
}

TEST("raster: Turntable Compact is the bare record, and labels vary by album")
{
	auto set = rasterizeLayers(makePreset("turntable", LayoutMode::CompactHorizontal), content(0), testsupport::text());
	CHECK(set->record.valid());
	CHECK(!set->art.valid());                                 // no sleeve in Compact
	CHECK(set->layout.record.h > set->layout.card.h * 0.70f); // the record fills the bar
	CHECK(set->layout.record.right() < set->layout.textColumn.x);

	Palette pal{Color::hex("#1d1440"), Color::hex("#e2604a"), Color::hex("#7a2a62"), Color::hex("#120a1f"), true};
	bool seen[5] = {};
	for (uint64_t seed = 0; seed < 40; ++seed)
		seen[static_cast<int>(turntable::pressingFor(pal, nullptr, seed * 7919).style)] = true;
	int styles = 0;
	for (bool b : seen)
		styles += b ? 1 : 0;
	CHECK(styles >= 4);
}

TEST("raster: short titles never scroll")
{
	auto set = rasterizeLayers(makePreset("minimal", LayoutMode::ExpandedHorizontal), content(2), testsupport::text());
	CHECK(!set->title.overflow);
}

TEST("raster: missing artwork gets a generated cover, not an empty hole")
{
	for (const auto &p : presetList()) {
		const Style s = makePreset(p.id, LayoutMode::Square);
		auto set = rasterizeLayers(s, content(1, false), testsupport::text());
		CHECK_MSG(set->palette.valid, p.id);
		CHECK_MSG(set->art.valid() || set->surface.valid(), p.id);
	}
}

TEST("raster: missing title falls back to the next best field")
{
	TrackInfo t;
	t.artist = "Only An Artist";
	const DisplayContent c = resolveContent(t, "Spotify");
	CHECK(c.title == "Only An Artist");
	CHECK(c.artist.empty() || c.artist != c.title);

	const DisplayContent ph = placeholderContent("Nothing playing");
	CHECK(ph.kind == DisplayContent::Kind::Placeholder);
	auto set = rasterizeLayers(makePreset("glass", LayoutMode::CompactHorizontal), ph, testsupport::text());
	CHECK(set->placeholder);
	CHECK(set->title.valid());
}

TEST("raster: time labels use tabular figures and a real minus sign")
{
	CHECK(formatTime(0, false) == "0:00");
	CHECK(formatTime(65, false) == "1:05");
	CHECK(formatTime(3725, false) == "1:02:05");
	CHECK(formatTime(65, true) == "\xE2\x88\x92" "1:05");
	auto set = rasterizeLayers(makePreset("glass", LayoutMode::ExpandedHorizontal), content(0), testsupport::text());
	const TimeLayers t = rasterizeTimes(makePreset("glass", LayoutMode::ExpandedHorizontal), *set, 30, 217, testsupport::text());
	CHECK(t.elapsed.valid());
	CHECK(t.remaining.valid());
}

TEST("raster: non-Latin text renders")
{
	auto set = rasterizeLayers(makePreset("editorial", LayoutMode::Vertical), content(4), testsupport::text());
	CHECK(set->title.valid());
	CHECK(set->title.naturalWidth > 10);
}

// Audit H-1: every preset and layout must rasterize at the extremes of the supported
// size range (64-4096 x 32-4096), including very short, very narrow and lopsided
// canvases, with and without artwork. Run under AddressSanitizer to catch reads
// outside the artwork buffers.
TEST("raster: extreme canvas sizes rasterize safely in every preset and layout")
{
	const LayoutMode layouts[] = {LayoutMode::CompactHorizontal, LayoutMode::ExpandedHorizontal, LayoutMode::Square,
				      LayoutMode::Vertical};
	const std::pair<int, int> sizes[] = {{64, 32}, {64, 4096}, {300, 80}, {300, 32}, {80, 300}, {4096, 32},
					     {96, 96}, {120, 40}, {64, 200}, {1600, 48}};
	for (const auto &p : presetList())
		for (LayoutMode m : layouts)
			for (auto [w, h] : sizes)
				for (int variant = 0; variant < 4; ++variant) {
					if (w * h > 100000 && variant != 1)
						continue; // the largest canvases are slow to rasterize; one variant covers them
					Style s = makePreset(p.id, m);
					s.width = w;
					s.height = h;
					const bool art = variant & 1;
					if (variant & 2) {
						s.showAlbum = false;
						s.showProgress = false;
						s.times = TimeDisplay::Hidden;
						for (FontSpec *f : {&s.titleFont, &s.artistFont, &s.albumFont, &s.timeFont})
							f->sizePx *= 2.0f;
					}
					const std::string what = std::string(p.id) + "/" + layoutId(m) + " " + std::to_string(w) + "x" +
								 std::to_string(h) + " v" + std::to_string(variant);
					auto set = rasterizeLayers(s, content(3, art), testsupport::text());
					CHECK_MSG(set && set->surface.valid(), what);
					const Layout &L = set->layout;
					CHECK_MSG(L.art.w >= 0 && L.art.h >= 0, what + ": negative artwork size");
					CHECK_MSG(L.textColumn.w >= 0, what + ": negative text column");
				}
}

TEST("raster: tiny and one-pixel artwork images are handled")
{
	for (int side : {1, 2, 3}) {
		auto tiny = std::make_shared<Image>(side, side);
		tiny->fill(Color{0.8f, 0.2f, 0.1f, 1});
		for (const auto &p : presetList())
			for (LayoutMode m : {LayoutMode::ExpandedHorizontal, LayoutMode::Square, LayoutMode::Vertical}) {
				Style s = makePreset(p.id, m);
				DisplayContent c = content(0);
				c.artwork = tiny;
				auto set = rasterizeLayers(s, c, testsupport::text());
				CHECK_MSG(set && set->surface.valid(), std::string(p.id) + " " + std::to_string(side) + "px art");
			}
	}
}
