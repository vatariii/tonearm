// SPDX-License-Identifier: GPL-2.0-or-later
// Turntable preset: deck geometry shared by layout, rasterizer and scene, plus the
// builders for its cached sprites. Every sprite is rendered once per track; motion is
// only GPU transforms (the disc and arm rotate, everything else is static).
#pragma once

#include "color.hpp"
#include "geometry.hpp"
#include "image.hpp"

namespace tonearm {
namespace turntable {

// All deck proportions are in record radii (R), so the hardware reads the same at
// any size. The record sits on a platter in front of its sleeve.
constexpr float kRecordToSleeve = 0.94f; // record diameter / sleeve side
constexpr float kPlatter = 1.035f;       // platter rim radius
constexpr float kPivotX = 1.06f;         // tonearm bearing, relative to the record centre
constexpr float kPivotY = -0.74f;
constexpr float kArmLength = 1.30f;      // bearing to stylus tip

// Tight bounds of the arm sprite at rest (pointing straight down), relative to the
// bearing, including its baked shadow.
Rect armRestBounds(Vec2 pivot, float R);

// Rotation from rest (radians, clockwise on screen) that puts the stylus on the
// groove at radius rho * R.
float armAngleForRadius(float rho);

// Deck bounds in sleeve-side units, relative to the sleeve's top-left corner, for a
// record offset (ox, oy) from the sleeve centre (also in sleeve-side units).
Rect deckExtents(float ox, float oy);

// Collectible pressing color: the cover's hue, pushed away from the sleeve.
// The centre label is designed per album: one of a few understated label styles,
// picked from the album's identity and colored from its cover.
enum class LabelStyle { Paper, Split, Band, Foil, Mark };
struct Pressing {
	Color vinyl;
	Color label;   // label paper
	Color label2;  // second tone: split half, band, or foil accent
	Color ink;     // printed marks
	LabelStyle style = LabelStyle::Paper;
	uint32_t variant = 0; // small print placement and mark shape
	bool colored = true;
};
Pressing pressingFor(const Palette &pal, const Image *art = nullptr, uint64_t seed = 0);
// Deck bounds for a record without its sleeve (Compact), in record radii around its centre.
Rect bareDeckExtents();

Image makeRecord(int side, const Pressing &p, uint64_t seed);
Image makeSheen(int side, const Pressing &p);
// Shadow plus platter, drawn under the record. `pad` is the margin around the record.
Image makePlatter(int side, int pad, float u);
Image makeArm(const Rect &zone, Vec2 pivot, float R, const Color &accent);
// Static hardware drawn into the plinth: bearing housing and arm rest.
void drawPlinthHardware(Image &card, Vec2 pivot, float R);

} // namespace turntable
} // namespace tonearm
