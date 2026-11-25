// SPDX-License-Identifier: GPL-2.0-or-later
#include "turntable.hpp"

#include <algorithm>
#include <cmath>

namespace tonearm {
namespace turntable {

namespace {

constexpr float kPi = 3.14159265f;

// Groove geometry, in record radii.
constexpr float kLabel = 0.345f;
constexpr float kRunout = 0.36f;
constexpr float kGroovesIn = 0.48f;
constexpr float kGroovesOut = 0.94f;
constexpr float kLip = 0.975f;
constexpr float kGaps[] = {0.87f, 0.765f, 0.665f, 0.575f};

// Light comes from the upper left, the same direction as the other presets' sheen.
constexpr float kLightX = -0.78f, kLightY = -0.62f;

float sq(float v) { return v * v; }
float sat(float v) { return std::clamp(v, 0.0f, 1.0f); }
float smooth(float a, float b, float v)
{
	const float t = sat((v - a) / (b - a));
	return t * t * (3 - 2 * t);
}

// Deterministic per-groove reflectivity. The record and its static sheen use the same
// mapping, so glints stay locked to grooves while the disc turns underneath.
float ringHash(int i)
{
	uint32_t h = static_cast<uint32_t>(i) * 2654435761u + 0x9E3779B9u;
	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	h *= 3266489917u;
	h ^= h >> 16;
	return (h & 0xFFFF) / 65535.0f;
}

bool inGap(float rho, float halfWidth)
{
	for (float g : kGaps)
		if (std::fabs(rho - g) < halfWidth)
			return true;
	return false;
}

// Source-over of a straight color with coverage into premultiplied BGRA.
inline void over(uint8_t *p, const Color &c, float a)
{
	a = sat(a * c.a);
	if (a <= 0)
		return;
	const float inv = 1.0f - a;
	p[0] = static_cast<uint8_t>(std::min(255.0f, sat(c.b) * a * 255.0f + p[0] * inv) + 0.5f);
	p[1] = static_cast<uint8_t>(std::min(255.0f, sat(c.g) * a * 255.0f + p[1] * inv) + 0.5f);
	p[2] = static_cast<uint8_t>(std::min(255.0f, sat(c.r) * a * 255.0f + p[2] * inv) + 0.5f);
	p[3] = static_cast<uint8_t>(std::min(255.0f, a * 255.0f + p[3] * inv) + 0.5f);
}

Color scaled(const Color &c, float k) { return {sat(c.r * k), sat(c.g * k), sat(c.b * k), c.a}; }

// Brushed aluminium across a cylinder: v is -1..1 across the part, +1 facing the light.
Color metal(float v, float base = 1.0f)
{
	const float spec = std::exp(-sq((v - 0.42f) / 0.24f));
	const float body = 0.34f + 0.36f * std::sqrt(std::max(0.0f, 1.0f - v * v));
	const float rim = 0.10f * smooth(0.75f, 1.0f, -v); // bounce light on the shadow side
	const float l = sat((body + rim) * base + 0.50f * spec);
	return {l * 0.965f, l * 0.98f, l, 1.0f};
}

// Shaded capsule. shade(v) gets the across-coordinate, +1 toward the light.
template <class F> void capsule(Image &img, Vec2 a, Vec2 b, float r, F shade)
{
	const float vx = b.x - a.x, vy = b.y - a.y;
	const float len = std::max(0.001f, std::hypot(vx, vy));
	const float nx = -vy / len, ny = vx / len;
	const float toLight = nx * kLightX + ny * kLightY >= 0 ? 1.0f : -1.0f;
	const int x0 = std::max(0, static_cast<int>(std::floor(std::min(a.x, b.x) - r - 2)));
	const int y0 = std::max(0, static_cast<int>(std::floor(std::min(a.y, b.y) - r - 2)));
	const int x1 = std::min(img.width(), static_cast<int>(std::ceil(std::max(a.x, b.x) + r + 2)));
	const int y1 = std::min(img.height(), static_cast<int>(std::ceil(std::max(a.y, b.y) + r + 2)));
	for (int y = y0; y < y1; ++y) {
		uint8_t *p = img.row(y) + x0 * 4;
		for (int x = x0; x < x1; ++x, p += 4) {
			const float px = x + 0.5f - a.x, py = y + 0.5f - a.y;
			const float t = std::clamp((px * vx + py * vy) / (len * len), 0.0f, 1.0f);
			const float qx = px - vx * t, qy = py - vy * t;
			const float d = std::hypot(qx, qy);
			const float cov = sat(r + 0.5f - d);
			if (cov <= 0)
				continue;
			const float v = std::clamp((qx * nx + qy * ny) / std::max(0.5f, r), -1.0f, 1.0f) * toLight;
			over(p, shade(v), cov);
		}
	}
}

// Shaded rounded box along dir. shade(v, u): v across (+1 toward light), u along (-1..1).
template <class F> void box(Image &img, Vec2 c, Vec2 dir, float hl, float hw, float corner, F shade)
{
	const float ux = dir.x, uy = dir.y, nx = -uy, ny = ux;
	const float toLight = nx * kLightX + ny * kLightY >= 0 ? 1.0f : -1.0f;
	const float ext = hl + hw + 2;
	const int x0 = std::max(0, static_cast<int>(std::floor(c.x - ext)));
	const int y0 = std::max(0, static_cast<int>(std::floor(c.y - ext)));
	const int x1 = std::min(img.width(), static_cast<int>(std::ceil(c.x + ext)));
	const int y1 = std::min(img.height(), static_cast<int>(std::ceil(c.y + ext)));
	corner = std::min(corner, std::min(hl, hw));
	for (int y = y0; y < y1; ++y) {
		uint8_t *p = img.row(y) + x0 * 4;
		for (int x = x0; x < x1; ++x, p += 4) {
			const float px = x + 0.5f - c.x, py = y + 0.5f - c.y;
			const float lu = px * ux + py * uy, lv = px * nx + py * ny;
			const float qx = std::fabs(lu) - (hl - corner), qy = std::fabs(lv) - (hw - corner);
			const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - corner;
			const float cov = sat(0.5f - d);
			if (cov <= 0)
				continue;
			over(p, shade(std::clamp(lv / std::max(0.5f, hw), -1.0f, 1.0f) * toLight, std::clamp(lu / std::max(0.5f, hl), -1.0f, 1.0f)), cov);
		}
	}
}

// Shaded disc. shade(dx, dy) gets normalized offsets from the centre.
template <class F> void disc(Image &img, Vec2 c, float r, F shade)
{
	const int x0 = std::max(0, static_cast<int>(std::floor(c.x - r - 1)));
	const int y0 = std::max(0, static_cast<int>(std::floor(c.y - r - 1)));
	const int x1 = std::min(img.width(), static_cast<int>(std::ceil(c.x + r + 1)));
	const int y1 = std::min(img.height(), static_cast<int>(std::ceil(c.y + r + 1)));
	for (int y = y0; y < y1; ++y) {
		uint8_t *p = img.row(y) + x0 * 4;
		for (int x = x0; x < x1; ++x, p += 4) {
			const float dx = x + 0.5f - c.x, dy = y + 0.5f - c.y;
			const float cov = sat(r + 0.5f - std::hypot(dx, dy));
			if (cov > 0)
				over(p, shade(dx / std::max(0.5f, r), dy / std::max(0.5f, r)), cov);
		}
	}
}

// Diffuse term for a domed part lit from the upper left.
float dome(float dx, float dy)
{
	return sat(0.5f + 0.5f * (dx * kLightX + dy * kLightY));
}

// Rest-pose arm geometry in record radii, relative to the bearing.
struct ArmShape {
	Vec2 stylus{0, kArmLength};
	Vec2 head{-0.4067f, 0.9135f}; // headshell direction, 24 degrees of offset
	float headLen = 0.24f;
	Vec2 joint() const { return {stylus.x - head.x * headLen, stylus.y - head.y * headLen}; }
	Vec2 side() const { return {head.y, -head.x}; } // away from the record
};

} // namespace

Rect armRestBounds(Vec2 pivot, float R)
{
	return {pivot.x - 0.13f * R, pivot.y - 0.41f * R, 0.47f * R, 1.92f * R};
}

float armAngleForRadius(float rho)
{
	auto dist = [](float a) {
		const float x = kPivotX - kArmLength * std::sin(a);
		const float y = kPivotY + kArmLength * std::cos(a);
		return std::hypot(x, y);
	};
	float lo = 0.0f, hi = 1.4f;
	for (int i = 0; i < 40; ++i) {
		const float mid = (lo + hi) * 0.5f;
		if (dist(mid) > rho)
			lo = mid;
		else
			hi = mid;
	}
	return (lo + hi) * 0.5f;
}

Rect deckExtents(float ox, float oy)
{
	const float R = kRecordToSleeve * 0.5f;
	const float cx = 0.5f + ox, cy = 0.5f + oy;
	const Vec2 pivot{cx + kPivotX * R, cy + kPivotY * R};
	const Rect arm = armRestBounds(pivot, R);
	const float pr = kPlatter * R;
	const float x0 = std::min({0.0f, cx - pr, arm.x});
	const float y0 = std::min({0.0f, cy - pr, arm.y});
	const float x1 = std::max({1.0f, cx + pr, arm.right()});
	const float y1 = std::max({1.0f, cy + pr, arm.bottom()});
	return {x0, y0, x1 - x0, y1 - y0};
}

namespace {

Color lch(float L, float C, float h) { return fromOklab(fromOklch({L, C, h})); }

// Chooses and colors the centre label. Every style stays quiet: a paper or deep
// tone, at most one second tone, and fine printed marks.
void designLabel(Pressing &p, const Palette &pal, uint64_t seed)
{
	const Oklch vin = toOklch(toOklab(p.vinyl));
	const Oklch dom = toOklch(toOklab(pal.dominant));
	const Oklch acc = toOklch(toOklab(pal.accent));
	const Oklch sec = toOklch(toOklab(pal.secondary));
	const uint32_t h = static_cast<uint32_t>(seed ^ (seed >> 29) ^ (seed >> 47));
	p.variant = h >> 8;
	// Cream stock, nudged a little toward the sleeve's main color.
	const Color paper = Color::hex("#efe9dc").mix(lch(0.90f, std::min(0.05f, dom.C), dom.h), 0.10f);
	const float tintHue = dom.C > 0.03f ? dom.h : (sec.C > 0.03f ? sec.h : acc.h);
	const float tintC = std::clamp(std::max(dom.C, acc.C * 0.5f), 0.045f, 0.085f);
	const Color tint = lch(0.80f, tintC, tintHue);
	const Color deep = lch(0.27f, std::clamp(acc.C * 0.55f, 0.03f, 0.085f), acc.h);
	const Color accentInk = lch(0.56f, std::clamp(acc.C, 0.09f, 0.16f), acc.h);
	const Color vinylInk = lch(0.30f, std::min(vin.C, 0.09f), vin.h);
	const Color foil = acc.C > 0.06f ? lch(0.80f, 0.07f, acc.h) : Color::hex("#d9c48e");

	p.style = static_cast<LabelStyle>(h % 5);
	// Dark labels would vanish into a deep pressing; those get light variants instead.
	const bool deepVinyl = p.colored && vin.L < 0.48f;
	if (deepVinyl && p.style == LabelStyle::Foil)
		p.style = LabelStyle::Split;
	if (deepVinyl && p.style == LabelStyle::Band) {
		p.label = paper;
		p.label2 = tint;
		p.ink = vinylInk;
		return;
	}
	switch (p.style) {
	case LabelStyle::Paper:
		p.label = paper;
		p.label2 = paper;
		p.ink = vinylInk;
		break;
	case LabelStyle::Split:
		p.label = paper;
		p.label2 = tint;
		p.ink = vinylInk;
		break;
	case LabelStyle::Band:
		p.label = deep;
		p.label2 = lch(0.36f, std::clamp(acc.C * 0.6f, 0.03f, 0.09f), acc.h);
		p.ink = paper;
		break;
	case LabelStyle::Foil:
		p.label = Color::hex("#161618").mix(deep, 0.25f);
		p.label2 = foil;
		p.ink = foil;
		break;
	case LabelStyle::Mark:
		p.label = tint;
		p.label2 = accentInk.mix(Color::hex("#ffffff"), 0.0f);
		p.ink = lch(0.28f, std::min(tintC, 0.06f), tintHue);
		break;
	}
}

} // namespace

Rect bareDeckExtents()
{
	const Vec2 pivot{kPivotX, kPivotY};
	const Rect arm = armRestBounds(pivot, 1.0f);
	const float x0 = std::min(-kPlatter, arm.x), y0 = std::min(-kPlatter, arm.y);
	const float x1 = std::max(kPlatter, arm.right()), y1 = std::max(kPlatter, arm.bottom());
	return {x0, y0, x1 - x0, y1 - y0};
}

Pressing pressingFor(const Palette &pal, const Image *art, uint64_t seed)
{
	Pressing p;
	const Oklch acc = toOklch(toOklab(pal.accent));
	const Oklch dom = toOklch(toOklab(pal.dominant));
	const Oklch sec = toOklch(toOklab(pal.secondary));
	auto hueGap = [](float a, float b) {
		float d = std::fabs(a - b);
		while (d > kPi * 2)
			d -= kPi * 2;
		return std::min(d, kPi * 2 - d);
	};
	if (acc.C < 0.045f) {
		// A monochrome sleeve gets a classic black pressing with a cream label.
		p.colored = false;
		p.vinyl = Color::hex("#141517");
		designLabel(p, pal, seed);
		return p;
	}
	Oklch v = acc;
	// When the cover's most vivid color is also the sleeve's main hue, the record would
	// dissolve into it. Look for the strongest second hue on the cover instead.
	if (dom.C > 0.025f && hueGap(acc.h, dom.h) < 0.6f) {
		if (sec.C > 0.06f && hueGap(sec.h, dom.h) > 0.8f) {
			v = sec;
		} else if (art && !art->empty()) {
			constexpr int kBins = 24;
			float weight[kBins] = {}, chroma[kBins] = {}, total = 0;
			const int step = std::max(1, std::min(art->width(), art->height()) / 64);
			for (int y = 0; y < art->height(); y += step) {
				const uint8_t *row = art->row(y);
				for (int x = 0; x < art->width(); x += step) {
					const uint8_t *px = row + x * 4;
					if (px[3] < 128)
						continue;
					const float ia = 255.0f / px[3];
					const Oklch c = toOklch(toOklab(Color{px[2] * ia / 255.0f, px[1] * ia / 255.0f, px[0] * ia / 255.0f, 1}));
					total += 1;
					if (c.C < 0.06f || c.L < 0.3f || c.L > 0.92f || hueGap(c.h, dom.h) < 0.8f)
						continue;
					int bin = static_cast<int>(std::floor((c.h + kPi) / (kPi * 2) * kBins)) % kBins;
					bin = (bin + kBins) % kBins;
					weight[bin] += c.C;
					chroma[bin] = std::max(chroma[bin], c.C);
				}
			}
			int best = -1;
			for (int b = 0; b < kBins; ++b)
				if (weight[b] > 0.004f * total && (best < 0 || weight[b] > weight[best]))
					best = b;
			if (best >= 0) {
				v.h = (best + 0.5f) / kBins * kPi * 2 - kPi;
				v.C = chroma[best];
			}
		}
	}
	v.C = std::clamp(v.C * 1.1f, 0.07f, 0.145f);
	// Lightness is set against the sleeve: luminous on dark covers, deep on light ones.
	v.L = dom.L > 0.58f ? 0.42f : 0.60f;
	p.vinyl = fromOklab(fromOklch(v));
	designLabel(p, pal, seed);
	return p;
}

namespace {

// One pixel of the centre label, in label-local coordinates (record radii).
Color labelColor(const Pressing &pr, float lx, float ly, float rho, float theta, float Rd)
{
	const float px = 1.0f / std::max(1.0f, Rd); // one pixel, in record radii
	const float hair = std::max(0.006f, 0.8f * px);
	const uint32_t v = pr.variant;
	Color col = pr.label;
	switch (pr.style) {
	case LabelStyle::Split:
		// Upper half in the second tone with a fine printed divider.
		if (ly < -hair * 0.5f)
			col = pr.label2;
		if (std::fabs(ly) < hair * 0.5f)
			col = pr.ink.mix(col, 0.3f);
		break;
	case LabelStyle::Band:
	case LabelStyle::Foil:
		if (rho > 0.262f)
			col = pr.style == LabelStyle::Band ? pr.label2 : col;
		break;
	default:
		break;
	}
	bool ink = false;
	Color inkC = pr.ink;
	// Printed rings: one hairline, or a pair on foil labels.
	if (pr.style == LabelStyle::Foil) {
		if (std::fabs(rho - 0.262f) < hair * 0.5f || std::fabs(rho - 0.322f) < hair * 0.5f)
			ink = true;
	} else if (pr.style != LabelStyle::Band) {
		if (std::fabs(rho - 0.302f) < hair * 0.5f)
			ink = true;
	}
	// Circular small print, on the top or the bottom arc.
	const bool top = (v & 1) == 0;
	const float arcR0 = pr.style == LabelStyle::Band || pr.style == LabelStyle::Foil ? 0.280f : 0.248f;
	const float t0 = top ? -2.55f : 0.60f, t1 = top ? -0.60f : 2.55f;
	if (rho > arcR0 && rho < arcR0 + 0.019f && theta > t0 && theta < t1) {
		const float period = std::max(2.2f, 0.034f * Rd);
		const float s = (theta - t0) * rho * Rd / period;
		const int glyph = static_cast<int>(s);
		ink = ink || ((s - glyph) < 0.70f && ringHash(glyph + 77 + static_cast<int>(v & 255)) > 0.18f);
	}
	// Title and catalogue lines on the side opposite the small print, a mark on the other.
	const float sgn = top ? 1.0f : -1.0f;
	const float w1 = 0.07f + 0.05f * ((v >> 2) & 3) / 3.0f;
	if (std::fabs(ly - sgn * 0.118f) < 0.012f && std::fabs(lx) < w1)
		ink = true;
	if (((v >> 4) & 1) && std::fabs(ly - sgn * 0.160f) < 0.008f && std::fabs(lx) < w1 * 0.55f)
		ink = true;
	const float mx = 0.0f, my = -sgn * 0.150f;
	const float mr = pr.style == LabelStyle::Mark ? 0.050f : 0.030f;
	const float dxm = lx - mx, dym = ly - my;
	bool mark = false;
	switch ((v >> 5) % 3) {
	case 0: mark = std::hypot(dxm, dym) < mr; break;                                  // dot
	case 1: mark = std::fabs(dxm) + std::fabs(dym) < mr * 1.15f; break;               // diamond
	case 2: mark = std::fabs(std::hypot(dxm, dym) - mr * 0.8f) < std::max(0.009f, px * 0.6f); break; // ring
	}
	if (mark) {
		ink = true;
		if (pr.style == LabelStyle::Mark || pr.style == LabelStyle::Foil)
			inkC = pr.label2;
	}
	if (ink)
		col = inkC.mix(col, 0.10f);
	col = scaled(col, 1.0f - 0.07f * sq(rho / kLabel));
	// The label edge is pressed slightly into the vinyl.
	if (rho > kLabel - std::max(0.008f, 0.8f * px))
		col = scaled(col, 0.86f);
	return col;
}

} // namespace

Image makeRecord(int side, const Pressing &pr, uint64_t seed)
{
	Image img(side, side);
	const float R = side * 0.5f;
	const float Rd = R - 0.5f;
	const float phase = static_cast<float>(seed % 628) / 100.0f;
	const Color spindle = Color::hex("#c9ccd0");
	for (int y = 0; y < side; ++y) {
		uint8_t *p = img.row(y);
		for (int x = 0; x < side; ++x, p += 4) {
			const float dx = x + 0.5f - R, dy = y + 0.5f - R;
			const float r = std::hypot(dx, dy);
			const float cov = sat(Rd + 0.5f - r);
			if (cov <= 0)
				continue;
			const float rho = r / Rd;
			const float theta = std::atan2(dy, dx);
			Color col;
			float alpha = 1.0f;
			if (rho < kLabel) {
				col = labelColor(pr, dx / Rd, dy / Rd, rho, theta, Rd);
			} else {
				float shade;
				if (rho > kLip) {
					const float t = (rho - kLip) / (1.0f - kLip);
					shade = 1.04f - 0.22f * t * t;
				} else if (rho > kGroovesOut) {
					shade = 0.99f;
				} else if (rho > kGroovesIn) {
					const float n = ringHash(static_cast<int>(r * 1.15f));
					shade = 0.89f + 0.11f * n + 0.02f * std::sin(rho * 150.0f);
					if (inGap(rho, std::max(0.005f, 0.9f / Rd)))
						shade = 1.03f;
				} else if (rho > kRunout + 0.01f) {
					shade = 0.95f;
					if (std::fabs(rho - 0.405f) < std::max(0.003f, 0.45f / Rd) ||
					    std::fabs(rho - 0.432f) < std::max(0.003f, 0.45f / Rd))
						shade = 0.84f;
				} else {
					shade = 0.80f; // step down to the label
				}
				float marble = 1.0f, depth = 1.0f;
				if (pr.colored) {
					// Translucent pressings carry soft swirls; they also show the spin.
					marble = 1.0f + 0.05f * std::sin(theta + phase + 7.0f * rho + 0.9f * std::sin(3.0f * theta - phase));
					depth = 1.08f - 0.16f * rho * rho;
					alpha = rho > kLip ? 1.0f : 0.95f;
				} else {
					marble = 1.0f + 0.025f * std::sin(2.0f * theta + phase);
				}
				col = scaled(pr.vinyl, shade * marble * depth);
			}
			// Spindle hole with the platter's spindle showing through.
			if (rho < 0.032f) {
				col = rho < 0.022f ? spindle : Color::hex("#1d1e20");
				alpha = 1.0f;
			}
			over(p, col, cov * alpha);
		}
	}
	return img;
}

Image makeSheen(int side, const Pressing &pr)
{
	Image img(side, side);
	const float R = side * 0.5f;
	const float Rd = R - 0.5f;
	const float a0 = std::atan2(kLightY, kLightX);
	const Color tint = pr.colored ? Color{1, 1, 1, 1}.mix(scaled(pr.vinyl, 1.6f), 0.22f) : Color{1, 1, 1, 1};
	for (int y = 0; y < side; ++y) {
		uint8_t *p = img.row(y);
		for (int x = 0; x < side; ++x, p += 4) {
			const float dx = x + 0.5f - R, dy = y + 0.5f - R;
			const float r = std::hypot(dx, dy);
			const float cov = sat(Rd + 0.5f - r);
			if (cov <= 0)
				continue;
			const float rho = r / Rd;
			const float theta = std::atan2(dy, dx);
			const float c1 = std::max(0.0f, std::cos(theta - a0));
			const float c2 = std::max(0.0f, std::cos(theta - a0 - kPi));
			float a, shade = 0.0f;
			if (rho < kLabel) {
				a = 0.03f + 0.07f * std::pow(c1, 3.0f) * smooth(0.05f, 0.30f, rho);
			} else if (rho > kLip) {
				a = 0.08f + 0.42f * c1 * c1;
			} else {
				// Anisotropic highlights: two wedges toward and away from the light,
				// broken up by the grooves they catch on. Across the light the grooves
				// fall into shade, which gives the pressing its conical depth.
				const float lobe = 0.70f * std::pow(c1, 14.0f) + 0.46f * std::pow(c2, 14.0f);
				const float broad = 0.10f * std::pow(c1, 4.0f) + 0.05f * std::pow(c2, 4.0f);
				float grain = 0.8f;
				if (rho > kGroovesIn && rho < kGroovesOut && !inGap(rho, std::max(0.005f, 0.9f / Rd)))
					grain = 0.35f + 1.0f * ringHash(static_cast<int>(r * 1.15f));
				a = (lobe * grain + broad) * 0.60f;
				const float across = 1.0f - std::fabs(std::cos(theta - a0));
				shade = (pr.colored ? 0.20f : 0.28f) * across * across * smooth(kRunout, 0.55f, rho);
			}
			const float total = sat(a + shade);
			if (total > 0)
				over(p, Color{0, 0, 0, 1}.mix(tint, a / std::max(0.001f, a + shade)), cov * total);
		}
	}
	return img;
}

Image makePlatter(int side, int pad, float u)
{
	const int W = side + pad * 2;
	Image img(W, W);
	const float c = W * 0.5f;
	const float R = side * 0.5f;
	{
		Image shadow(W, W);
		disc(shadow, {c, c + 0.03f * R + 2.0f * u}, R * kPlatter, [](float, float) { return Color{0, 0, 0, 0.62f}; });
		blur(shadow, 0.05f * R + 3.0f * u);
		composite(img, shadow, 0, static_cast<int>(std::round(0.02f * R)));
	}
	disc(img, {c, c}, R * kPlatter + 0.6f, [](float, float) { return Color{0, 0, 0, 0.45f}; });
	disc(img, {c, c}, R * kPlatter, [&](float dx, float dy) {
		// Machined rim: bright where it faces the light, a dark well inside.
		const float rr = std::hypot(dx, dy) * kPlatter;
		if (rr < 0.995f)
			return Color::hex("#121315");
		const float facing = -(dx * kLightX + dy * kLightY) / std::max(0.01f, std::hypot(dx, dy));
		const float l = 0.20f + 0.34f * sat(-facing) + 0.10f * smooth(0.99f, 1.03f, rr);
		return Color{l * 0.97f, l * 0.98f, l, 1};
	});
	return img;
}

Image makeArm(const Rect &zone, Vec2 pivotAbs, float R, const Color &accent)
{
	Image img(std::max(1, static_cast<int>(std::ceil(zone.w))), std::max(1, static_cast<int>(std::ceil(zone.h))));
	const Vec2 P{pivotAbs.x - zone.x, pivotAbs.y - zone.y};
	const ArmShape A;
	auto at = [&](Vec2 v) { return Vec2{P.x + v.x * R, P.y + v.y * R}; };
	const Vec2 S = at(A.stylus), J = at(A.joint());
	const Vec2 hd = A.head, sd = A.side();
	const float tube = std::max(0.9f, 0.030f * R);
	const float cw = std::max(2.2f, 0.085f * R);
	const Vec2 cwTop = at({0, -0.30f}), cwBottom = at({0, -0.13f});
	const Vec2 headC{(J.x + S.x) * 0.5f + hd.x * 0.01f * R, (J.y + S.y) * 0.5f + hd.y * 0.01f * R};
	const float headHl = A.headLen * 0.55f * R, headHw = std::max(1.4f, 0.055f * R);
	const Vec2 cartC{S.x - hd.x * 0.07f * R, S.y - hd.y * 0.07f * R};
	const float cartHl = std::max(1.6f, 0.075f * R), cartHw = std::max(1.2f, 0.044f * R);
	const Vec2 liftA{J.x + (hd.x * 0.05f + sd.x * 0.05f) * R, J.y + (hd.y * 0.05f + sd.y * 0.05f) * R};
	const Vec2 liftB{liftA.x + sd.x * 0.08f * R, liftA.y + sd.y * 0.08f * R};
	const float hub = std::max(2.0f, 0.075f * R);

	// Soft shadow from the whole assembly, offset away from the light.
	{
		Image sil(img.width(), img.height());
		const auto black = [](float) { return Color{0, 0, 0, 1}; };
		capsule(sil, cwTop, cwBottom, cw, black);
		capsule(sil, P, J, tube, black);
		box(sil, headC, hd, headHl, headHw, 1, [](float, float) { return Color{0, 0, 0, 1}; });
		box(sil, cartC, hd, cartHl, cartHw, 1, [](float, float) { return Color{0, 0, 0, 1}; });
		capsule(sil, liftA, liftB, std::max(0.6f, 0.012f * R), black);
		blur(sil, std::max(1.0f, 0.035f * R));
		composite(img, sil, static_cast<int>(std::round(0.05f * R)), static_cast<int>(std::round(0.07f * R)), 0.5f);
	}

	// Counterweight on its stub, with two machined grooves.
	capsule(img, cwBottom, P, tube * 0.9f, [](float v) { return metal(v, 0.9f); });
	capsule(img, cwTop, cwBottom, cw, [](float v) { return metal(v, 0.78f); });
	for (float gy : {-0.185f, -0.245f}) {
		const Vec2 a = at({-0.085f, gy}), b = at({0.085f, gy});
		capsule(img, a, b, std::max(0.35f, 0.006f * R), [](float) { return Color{0, 0, 0, 0.45f}; });
	}
	// Arm tube.
	capsule(img, P, J, tube, [](float v) { return metal(v); });
	// Finger lift.
	capsule(img, liftA, liftB, std::max(0.6f, 0.012f * R), [](float v) { return metal(v, 0.95f); });
	// Headshell plate.
	box(img, headC, hd, headHl, headHw, std::max(0.8f, 0.012f * R), [](float v, float) {
		Color m = metal(v * 0.35f, 0.72f);
		return m;
	});
	// Cartridge body in the cover's accent, like a colored moving-magnet cartridge.
	Oklch body = toOklch(toOklab(accent));
	body.L = std::clamp(body.L, 0.42f, 0.56f);
	body.C = std::min(body.C, 0.16f);
	const Color bodyC = fromOklab(fromOklch(body));
	box(img, cartC, hd, cartHl, cartHw, std::max(0.6f, 0.012f * R), [&](float v, float u) {
		float k = 0.78f + 0.30f * sat(v * 0.5f + 0.5f);
		if (u > 0.72f)
			k *= 0.55f; // stylus guard
		return scaled(bodyC, k);
	});
	// Bearing hub.
	disc(img, P, hub, [](float dx, float dy) {
		const float l = 0.32f + 0.48f * dome(dx, dy) + 0.25f * std::exp(-sq(std::hypot(dx + 0.35f, dy + 0.35f) / 0.3f));
		return Color{sat(l) * 0.97f, sat(l) * 0.98f, sat(l), 1};
	});
	disc(img, P, hub * 0.45f, [](float dx, float dy) {
		const float l = 0.18f + 0.22f * dome(-dx, -dy);
		return Color{l, l, l * 1.02f, 1};
	});
	return img;
}

void drawPlinthHardware(Image &card, Vec2 P, float R)
{
	// Bearing housing.
	const float base = std::max(3.0f, 0.165f * R);
	{
		Image shadow(card.width(), card.height());
		disc(shadow, {P.x, P.y + 0.03f * R}, base, [](float, float) { return Color{0, 0, 0, 0.55f}; });
		blur(shadow, std::max(1.0f, 0.04f * R));
		composite(card, shadow, 0, 0);
	}
	disc(card, P, base, [](float dx, float dy) {
		const float l = 0.10f + 0.14f * dome(dx, dy);
		return Color{l, l, l * 1.03f, 1};
	});
	disc(card, P, base * 0.78f, [](float dx, float dy) {
		const float rr = std::hypot(dx, dy);
		const float l = 0.30f + 0.36f * dome(dx, dy) + 0.06f * std::sin(rr * 40.0f);
		return Color{sat(l) * 0.97f, sat(l) * 0.98f, sat(l), 1};
	});
	disc(card, P, base * 0.62f, [](float, float) { return Color::hex("#1b1c1e"); });

	// Arm rest: a machined cradle across the tube's rest position, so the stop reads as
	// hardware rather than a stray dot when the arm is out on the record.
	const ArmShape A;
	const Vec2 J = A.joint();
	const Vec2 rest{P.x + J.x * 0.63f * R, P.y + J.y * 0.63f * R};
	const float len = std::hypot(J.x, J.y);
	const Vec2 across{J.y / len, -J.x / len};
	const float hl = std::max(2.4f, 0.085f * R), hw = std::max(1.2f, 0.034f * R);
	{
		Image shadow(card.width(), card.height());
		box(shadow, {rest.x + 0.02f * R, rest.y + 0.035f * R}, across, hl, hw, hw, [](float, float) { return Color{0, 0, 0, 0.6f}; });
		blur(shadow, std::max(1.0f, 0.03f * R));
		composite(card, shadow, 0, 0);
	}
	box(card, rest, across, hl, hw, hw, [](float v, float) {
		const float l = 0.14f + 0.16f * sat(v * 0.5f + 0.5f);
		return Color{l, l, l * 1.03f, 1};
	});
	// Two posts that cradle the tube.
	for (float side : {-1.0f, 1.0f}) {
		const Vec2 c{rest.x + across.x * side * hl * 0.62f, rest.y + across.y * side * hl * 0.62f};
		disc(card, c, std::max(0.9f, 0.026f * R), [](float dx, float dy) {
			const float l = 0.30f + 0.40f * dome(dx, dy);
			return Color{l, l, l * 1.03f, 1};
		});
	}
}

} // namespace turntable
} // namespace tonearm
