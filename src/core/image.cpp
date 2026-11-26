// SPDX-License-Identifier: GPL-2.0-or-later
#include "image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace tonearm {

Image::Image(int w, int h) : w_(std::max(0, w)), h_(std::max(0, h)), px_(static_cast<size_t>(w_) * h_ * 4, 0) {}

void Image::clear()
{
	std::fill(px_.begin(), px_.end(), uint8_t{0});
}

void Image::fill(const Color &c)
{
	// Clamp before premultiplying so out-of-range or non-finite input can't wrap around.
	auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
	const float ca = unit(c.a);
	const uint8_t b = static_cast<uint8_t>(std::lround(unit(c.b) * ca * 255));
	const uint8_t g = static_cast<uint8_t>(std::lround(unit(c.g) * ca * 255));
	const uint8_t r = static_cast<uint8_t>(std::lround(unit(c.r) * ca * 255));
	const uint8_t a = static_cast<uint8_t>(std::lround(ca * 255));
	for (size_t i = 0; i < px_.size(); i += 4) {
		px_[i] = b;
		px_[i + 1] = g;
		px_[i + 2] = r;
		px_[i + 3] = a;
	}
}

static Image fromStraight(int w, int h, const uint8_t *src, int stride, bool rgba)
{
	Image img(w, h);
	for (int y = 0; y < h; ++y) {
		const uint8_t *s = src + static_cast<size_t>(y) * stride;
		uint8_t *d = img.row(y);
		for (int x = 0; x < w; ++x, s += 4, d += 4) {
			const uint32_t a = s[3];
			const uint8_t c0 = rgba ? s[2] : s[0]; // blue
			const uint8_t c2 = rgba ? s[0] : s[2]; // red
			d[0] = static_cast<uint8_t>((c0 * a + 127) / 255);
			d[1] = static_cast<uint8_t>((s[1] * a + 127) / 255);
			d[2] = static_cast<uint8_t>((c2 * a + 127) / 255);
			d[3] = static_cast<uint8_t>(a);
		}
	}
	return img;
}

Image Image::fromRgbaStraight(int w, int h, const uint8_t *rgba, int stride)
{
	return fromStraight(w, h, rgba, stride, true);
}

Image Image::fromBgraStraight(int w, int h, const uint8_t *bgra, int stride)
{
	return fromStraight(w, h, bgra, stride, false);
}

// --- Resampling -------------------------------------------------------------

namespace {

// One-dimensional area-averaging resample of a float plane, used for separable shrinking.
void resampleAxis(const std::vector<float> &src, int sw, int sh, std::vector<float> &dst, int dw, bool horizontal)
{
	// src/dst hold 4 floats per pixel.
	const int srcLen = horizontal ? sw : sh;
	const int dstLen = dw;
	const int lines = horizontal ? sh : sw;
	const float scale = static_cast<float>(srcLen) / dstLen;
	const int outW = horizontal ? dw : sw;
	dst.assign(static_cast<size_t>(horizontal ? dw * sh : sw * dw) * 4, 0.0f);

	for (int line = 0; line < lines; ++line) {
		for (int i = 0; i < dstLen; ++i) {
			float acc[4] = {0, 0, 0, 0};
			float wsum = 0;
			if (scale > 1.0f) {
				const float start = i * scale, end = start + scale;
				int s0 = static_cast<int>(std::floor(start));
				int s1 = std::min(srcLen, static_cast<int>(std::ceil(end)));
				for (int s = s0; s < s1; ++s) {
					const float w = std::min(end, s + 1.0f) - std::max(start, static_cast<float>(s));
					if (w <= 0)
						continue;
					const size_t idx = horizontal ? (static_cast<size_t>(line) * sw + s) * 4 : (static_cast<size_t>(s) * sw + line) * 4;
					for (int c = 0; c < 4; ++c)
						acc[c] += src[idx + c] * w;
					wsum += w;
				}
			} else {
				// Bilinear for enlarging.
				const float pos = (i + 0.5f) * scale - 0.5f;
				int s0 = static_cast<int>(std::floor(pos));
				const float t = pos - s0;
				const int a = std::clamp(s0, 0, srcLen - 1), b = std::clamp(s0 + 1, 0, srcLen - 1);
				const size_t ia = horizontal ? (static_cast<size_t>(line) * sw + a) * 4 : (static_cast<size_t>(a) * sw + line) * 4;
				const size_t ib = horizontal ? (static_cast<size_t>(line) * sw + b) * 4 : (static_cast<size_t>(b) * sw + line) * 4;
				for (int c = 0; c < 4; ++c)
					acc[c] = src[ia + c] * (1 - t) + src[ib + c] * t;
				wsum = 1;
			}
			const size_t o = horizontal ? (static_cast<size_t>(line) * outW + i) * 4 : (static_cast<size_t>(i) * outW + line) * 4;
			for (int c = 0; c < 4; ++c)
				dst[o + c] = wsum > 0 ? acc[c] / wsum : 0;
		}
	}
}

} // namespace

Image resize(const Image &src, int w, int h)
{
	if (src.empty() || w <= 0 || h <= 0)
		return Image(std::max(0, w), std::max(0, h));
	if (w == src.width() && h == src.height())
		return src;

	std::vector<float> a(static_cast<size_t>(src.width()) * src.height() * 4);
	for (int y = 0; y < src.height(); ++y) {
		const uint8_t *p = src.row(y);
		for (int x = 0; x < src.width() * 4; ++x)
			a[static_cast<size_t>(y) * src.width() * 4 + x] = p[x];
	}
	std::vector<float> b, c;
	resampleAxis(a, src.width(), src.height(), b, w, true);
	resampleAxis(b, w, src.height(), c, h, false);

	Image out(w, h);
	for (int y = 0; y < h; ++y) {
		uint8_t *p = out.row(y);
		for (int x = 0; x < w * 4; ++x)
			p[x] = static_cast<uint8_t>(std::clamp(std::lround(c[static_cast<size_t>(y) * w * 4 + x]), 0L, 255L));
	}
	return out;
}

Image coverFit(const Image &src, int w, int h)
{
	if (src.empty() || w <= 0 || h <= 0)
		return Image(std::max(0, w), std::max(0, h));
	const float sa = static_cast<float>(src.width()) / src.height();
	const float da = static_cast<float>(w) / h;
	int cw = src.width(), ch = src.height(), cx = 0, cy = 0;
	if (sa > da) {
		cw = std::max(1, static_cast<int>(std::lround(src.height() * da)));
		cx = (src.width() - cw) / 2;
	} else if (sa < da) {
		ch = std::max(1, static_cast<int>(std::lround(src.width() / da)));
		cy = (src.height() - ch) / 2;
	}
	Image crop(cw, ch);
	for (int y = 0; y < ch; ++y)
		std::memcpy(crop.row(y), src.row(y + cy) + cx * 4, static_cast<size_t>(cw) * 4);
	return resize(crop, w, h);
}

// --- Blur -------------------------------------------------------------------

namespace {

void boxBlurH(const std::vector<uint16_t> &in, std::vector<uint16_t> &out, int w, int h, int r)
{
	const int win = 2 * r + 1;
	for (int y = 0; y < h; ++y) {
		const uint16_t *row = in.data() + static_cast<size_t>(y) * w * 4;
		uint16_t *orow = out.data() + static_cast<size_t>(y) * w * 4;
		uint32_t sum[4] = {0, 0, 0, 0};
		for (int i = -r; i <= r; ++i) {
			const int xi = std::clamp(i, 0, w - 1);
			for (int c = 0; c < 4; ++c)
				sum[c] += row[xi * 4 + c];
		}
		for (int x = 0; x < w; ++x) {
			for (int c = 0; c < 4; ++c)
				orow[x * 4 + c] = static_cast<uint16_t>((sum[c] + win / 2) / win);
			const int add = std::min(x + r + 1, w - 1), sub = std::max(x - r, 0);
			for (int c = 0; c < 4; ++c)
				sum[c] += row[add * 4 + c] - row[sub * 4 + c];
		}
	}
}

void boxBlurV(const std::vector<uint16_t> &in, std::vector<uint16_t> &out, int w, int h, int r)
{
	const int win = 2 * r + 1;
	for (int x = 0; x < w; ++x) {
		uint32_t sum[4] = {0, 0, 0, 0};
		for (int i = -r; i <= r; ++i) {
			const int yi = std::clamp(i, 0, h - 1);
			for (int c = 0; c < 4; ++c)
				sum[c] += in[(static_cast<size_t>(yi) * w + x) * 4 + c];
		}
		for (int y = 0; y < h; ++y) {
			for (int c = 0; c < 4; ++c)
				out[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint16_t>((sum[c] + win / 2) / win);
			const int add = std::min(y + r + 1, h - 1), sub = std::max(y - r, 0);
			for (int c = 0; c < 4; ++c)
				sum[c] += in[(static_cast<size_t>(add) * w + x) * 4 + c] - in[(static_cast<size_t>(sub) * w + x) * 4 + c];
		}
	}
}

} // namespace

void blur(Image &img, float radius)
{
	if (img.empty() || radius < 0.5f)
		return;
	const int w = img.width(), h = img.height();
	// Three box passes approximate a gaussian with sigma ~= radius / 2.
	const float sigma = radius * 0.5f;
	const int boxes = 3;
	const float wIdeal = std::sqrt((12 * sigma * sigma / boxes) + 1);
	int wl = static_cast<int>(std::floor(wIdeal));
	if (wl % 2 == 0)
		--wl;
	const int wu = wl + 2;
	const float mIdeal = (12 * sigma * sigma - boxes * wl * wl - 4 * boxes * wl - 3 * boxes) / (-4 * wl - 4);
	const int m = static_cast<int>(std::lround(mIdeal));

	// 8.8 fixed point keeps precision across passes and avoids banding in soft shadows.
	std::vector<uint16_t> a(static_cast<size_t>(w) * h * 4), b(a.size());
	for (size_t i = 0; i < a.size(); ++i)
		a[i] = static_cast<uint16_t>(img.data()[i] << 8);
	for (int i = 0; i < boxes; ++i) {
		const int r = ((i < m ? wl : wu) - 1) / 2;
		if (r <= 0)
			continue;
		boxBlurH(a, b, w, h, r);
		boxBlurV(b, a, w, h, r);
	}
	for (size_t i = 0; i < a.size(); ++i)
		img.data()[i] = static_cast<uint8_t>((a[i] + 128) >> 8);
}

// --- Compositing ------------------------------------------------------------

void composite(Image &dst, const Image &src, int ox, int oy, float opacity)
{
	if (dst.empty() || src.empty() || opacity <= 0.0f)
		return;
	const uint32_t op = static_cast<uint32_t>(std::lround(std::clamp(opacity, 0.0f, 1.0f) * 256));
	const int x0 = std::max(0, ox), y0 = std::max(0, oy);
	const int x1 = std::min(dst.width(), ox + src.width()), y1 = std::min(dst.height(), oy + src.height());
	for (int y = y0; y < y1; ++y) {
		const uint8_t *s = src.row(y - oy) + (x0 - ox) * 4;
		uint8_t *d = dst.row(y) + x0 * 4;
		for (int x = x0; x < x1; ++x, s += 4, d += 4) {
			const uint32_t sa = (s[3] * op) >> 8;
			if (sa == 0)
				continue;
			const uint32_t inv = 255 - sa;
			for (int c = 0; c < 3; ++c)
				d[c] = static_cast<uint8_t>(std::min<uint32_t>(255, ((s[c] * op) >> 8) + (d[c] * inv + 127) / 255));
			d[3] = static_cast<uint8_t>(std::min<uint32_t>(255, sa + (d[3] * inv + 127) / 255));
		}
	}
}

namespace {

// Signed distance to a rounded rectangle centered at origin with half-size (hx, hy).
inline float sdRoundRect(float px, float py, float hx, float hy, float r)
{
	const float qx = std::fabs(px) - hx + r;
	const float qy = std::fabs(py) - hy + r;
	const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
	return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

struct GradientLut {
	std::array<Color, 256> c;
};

GradientLut buildLut(const std::vector<GradientStop> &stops)
{
	GradientLut lut;
	if (stops.empty()) {
		lut.c.fill(Color{0, 0, 0, 0});
		return lut;
	}
	for (int i = 0; i < 256; ++i) {
		const float t = i / 255.0f;
		size_t k = 0;
		while (k + 1 < stops.size() && stops[k + 1].pos < t)
			++k;
		if (k + 1 >= stops.size() || t <= stops[0].pos) {
			lut.c[i] = t <= stops[0].pos ? stops[0].color : stops.back().color;
			continue;
		}
		const GradientStop &a = stops[k], &b = stops[k + 1];
		const float span = std::max(1e-5f, b.pos - a.pos);
		const float u = std::clamp((t - a.pos) / span, 0.0f, 1.0f);
		// Interpolate in Oklab for smooth, perceptually even gradients.
		const Oklab la = toOklab(a.color), lb = toOklab(b.color);
		const Oklab m{la.L + (lb.L - la.L) * u, la.a + (lb.a - la.a) * u, la.b + (lb.b - la.b) * u};
		lut.c[i] = fromOklab(m, a.color.a + (b.color.a - a.color.a) * u);
	}
	return lut;
}

float gradientT(const Paint &p, float x, float y)
{
	if (p.kind == Paint::Kind::Linear) {
		const float dx = p.p1.x - p.p0.x, dy = p.p1.y - p.p0.y;
		const float len2 = dx * dx + dy * dy;
		if (len2 <= 0)
			return 0;
		return std::clamp(((x - p.p0.x) * dx + (y - p.p0.y) * dy) / len2, 0.0f, 1.0f);
	}
	if (p.kind == Paint::Kind::Radial) {
		const float dx = x - p.p0.x, dy = y - p.p0.y;
		return std::clamp(std::sqrt(dx * dx + dy * dy) / std::max(1e-3f, p.radius), 0.0f, 1.0f);
	}
	return 0;
}

template <typename CoverageFn>
void paintShape(Image &dst, const Rect &bounds, const Paint &paint, float opacity, CoverageFn coverage)
{
	if (dst.empty() || bounds.empty() || opacity <= 0)
		return;
	const int x0 = std::max(0, static_cast<int>(std::floor(bounds.x)));
	const int y0 = std::max(0, static_cast<int>(std::floor(bounds.y)));
	const int x1 = std::min(dst.width(), static_cast<int>(std::ceil(bounds.right())));
	const int y1 = std::min(dst.height(), static_cast<int>(std::ceil(bounds.bottom())));
	GradientLut lut;
	const bool gradient = paint.kind != Paint::Kind::Solid;
	if (gradient)
		lut = buildLut(paint.stops);

	for (int y = y0; y < y1; ++y) {
		uint8_t *d = dst.row(y) + x0 * 4;
		for (int x = x0; x < x1; ++x, d += 4) {
			const float cov = coverage(x + 0.5f, y + 0.5f) * opacity;
			if (cov <= 0.0f)
				continue;
			const Color c = gradient ? lut.c[static_cast<int>(gradientT(paint, x + 0.5f, y + 0.5f) * 255.0f + 0.5f)] : paint.color;
			const float a = c.a * cov;
			const float inv = 1.0f - a;
			d[0] = static_cast<uint8_t>(std::min(255.0f, c.b * a * 255.0f + d[0] * inv + 0.5f));
			d[1] = static_cast<uint8_t>(std::min(255.0f, c.g * a * 255.0f + d[1] * inv + 0.5f));
			d[2] = static_cast<uint8_t>(std::min(255.0f, c.r * a * 255.0f + d[2] * inv + 0.5f));
			d[3] = static_cast<uint8_t>(std::min(255.0f, a * 255.0f + d[3] * inv + 0.5f));
		}
	}
}

} // namespace

Color Paint::sample(float x, float y) const
{
	if (kind == Kind::Solid)
		return color;
	const GradientLut lut = buildLut(stops);
	return lut.c[static_cast<int>(gradientT(*this, x, y) * 255.0f + 0.5f)];
}

void applyRoundedMask(Image &img, float radius)
{
	if (img.empty() || radius <= 0)
		return;
	const float hx = img.width() * 0.5f, hy = img.height() * 0.5f;
	const float r = std::min({radius, hx, hy});
	for (int y = 0; y < img.height(); ++y) {
		uint8_t *p = img.row(y);
		for (int x = 0; x < img.width(); ++x, p += 4) {
			const float d = sdRoundRect(x + 0.5f - hx, y + 0.5f - hy, hx, hy, r);
			const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
			if (cov >= 1.0f)
				continue;
			for (int c = 0; c < 4; ++c)
				p[c] = static_cast<uint8_t>(p[c] * cov + 0.5f);
		}
	}
}

void fillRoundRect(Image &dst, const Rect &r, float radius, const Paint &paint, float opacity)
{
	const float hx = r.w * 0.5f, hy = r.h * 0.5f;
	const float rr = std::clamp(radius, 0.0f, std::min(hx, hy));
	paintShape(dst, r.inset(-1), paint, opacity, [&](float px, float py) {
		return std::clamp(0.5f - sdRoundRect(px - r.cx(), py - r.cy(), hx, hy, rr), 0.0f, 1.0f);
	});
}

void strokeRoundRect(Image &dst, const Rect &r, float radius, float width, const Paint &paint, float opacity)
{
	const float hx = r.w * 0.5f, hy = r.h * 0.5f;
	const float rr = std::clamp(radius, 0.0f, std::min(hx, hy));
	paintShape(dst, r.inset(-1), paint, opacity, [&](float px, float py) {
		const float d = sdRoundRect(px - r.cx(), py - r.cy(), hx, hy, rr);
		const float outer = std::clamp(0.5f - d, 0.0f, 1.0f);
		const float inner = std::clamp(0.5f - (d + width), 0.0f, 1.0f);
		return outer - inner;
	});
}

void dropShadow(Image &dst, const Rect &r, float radius, float blurRadius, float offsetY, const Color &color)
{
	if (color.a <= 0)
		return;
	const int pad = static_cast<int>(std::ceil(blurRadius * 1.5f)) + 2;
	const int w = static_cast<int>(std::ceil(r.w)) + pad * 2, h = static_cast<int>(std::ceil(r.h)) + pad * 2;
	Image tmp(w, h);
	fillRoundRect(tmp, {static_cast<float>(pad), static_cast<float>(pad), r.w, r.h}, radius, Paint::solid(color));
	blur(tmp, blurRadius);
	composite(dst, tmp, static_cast<int>(std::lround(r.x)) - pad, static_cast<int>(std::lround(r.y + offsetY)) - pad);
}

void glow(Image &dst, const Image &src, int x, int y, float radius, const Color &tint, float strength)
{
	if (src.empty() || strength <= 0)
		return;
	const int pad = static_cast<int>(std::ceil(radius * 1.5f)) + 2;
	Image tmp(src.width() + pad * 2, src.height() + pad * 2);
	// Tint the source alpha.
	for (int yy = 0; yy < src.height(); ++yy) {
		const uint8_t *s = src.row(yy);
		uint8_t *d = tmp.row(yy + pad) + pad * 4;
		for (int xx = 0; xx < src.width(); ++xx, s += 4, d += 4) {
			const float a = s[3] / 255.0f * tint.a;
			d[0] = static_cast<uint8_t>(tint.b * a * 255.0f + 0.5f);
			d[1] = static_cast<uint8_t>(tint.g * a * 255.0f + 0.5f);
			d[2] = static_cast<uint8_t>(tint.r * a * 255.0f + 0.5f);
			d[3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
		}
	}
	blur(tmp, radius);
	composite(dst, tmp, x - pad, y - pad, std::min(1.0f, strength));
	if (strength > 1.0f)
		composite(dst, tmp, x - pad, y - pad, strength - 1.0f);
}

void addNoise(Image &img, float amount, uint32_t seed)
{
	if (amount <= 0)
		return;
	uint32_t s = seed ? seed : 0x9E3779B9u; // xorshift32 never leaves 0
	const float amp = amount * 255.0f;
	for (int y = 0; y < img.height(); ++y) {
		uint8_t *p = img.row(y);
		for (int x = 0; x < img.width(); ++x, p += 4) {
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			if (p[3] == 0)
				continue;
			const float n = ((s & 0xffff) / 65535.0f - 0.5f) * amp * (p[3] / 255.0f);
			for (int c = 0; c < 3; ++c)
				p[c] = static_cast<uint8_t>(std::clamp(p[c] + n, 0.0f, static_cast<float>(p[3])));
		}
	}
}

Color averageColor(const Image &img)
{
	double r = 0, g = 0, b = 0, a = 0;
	for (int y = 0; y < img.height(); ++y) {
		const uint8_t *p = img.row(y);
		for (int x = 0; x < img.width(); ++x, p += 4) {
			b += p[0];
			g += p[1];
			r += p[2];
			a += p[3];
		}
	}
	if (a <= 0)
		return {0, 0, 0, 0};
	const double n = static_cast<double>(img.width()) * img.height();
	return {static_cast<float>(r / a), static_cast<float>(g / a), static_cast<float>(b / a), static_cast<float>(a / n / 255.0)};
}

void multiplyAlpha(Image &img, float a)
{
	a = std::clamp(a, 0.0f, 1.0f);
	if (a >= 1.0f)
		return;
	const size_t n = static_cast<size_t>(img.width()) * img.height() * 4;
	uint8_t *p = img.data();
	for (size_t i = 0; i < n; ++i)
		p[i] = static_cast<uint8_t>(p[i] * a + 0.5f);
}

void adjustColor(Image &img, float saturation, float brightness)
{
	for (int y = 0; y < img.height(); ++y) {
		uint8_t *p = img.row(y);
		for (int x = 0; x < img.width(); ++x, p += 4) {
			const float b = p[0], g = p[1], r = p[2], a = p[3];
			const float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
			const float nr = (l + (r - l) * saturation) * brightness;
			const float ng = (l + (g - l) * saturation) * brightness;
			const float nb = (l + (b - l) * saturation) * brightness;
			p[0] = static_cast<uint8_t>(std::clamp(nb, 0.0f, a));
			p[1] = static_cast<uint8_t>(std::clamp(ng, 0.0f, a));
			p[2] = static_cast<uint8_t>(std::clamp(nr, 0.0f, a));
		}
	}
}

} // namespace tonearm
