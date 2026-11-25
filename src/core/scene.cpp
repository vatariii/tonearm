// SPDX-License-Identifier: GPL-2.0-or-later
#include "scene.hpp"

#include "motion.hpp"
#include "animation.hpp"

#include <algorithm>
#include <cmath>

namespace tonearm {

using namespace tokens;

namespace {

enum Element { kSurface, kGlow, kArt, kTitle, kArtist, kAlbum, kSource, kProgress, kTimes };

struct Choreography {
	float outEnd;        // fraction of the transition when outgoing text is gone
	float inStart;       // fraction when incoming text starts
	Vec2 outShift;       // logical px at full intensity
	Vec2 inShift;
	float artScaleFrom;  // incoming artwork scale
	float staggerMs;
};

Choreography choreographyFor(Composition c)
{
	switch (c) {
	case Composition::Glass:
		return {0.38f, 0.26f, {0, -6}, {0, 9}, 0.94f, stagger};
	case Composition::Minimal:
		return {0.34f, 0.30f, {0, -3}, {0, 4}, 1.0f, stagger * 0.6f};
	case Composition::Neon:
		return {0.30f, 0.34f, {-5, 0}, {8, 0}, 0.97f, stagger};
	case Composition::Editorial:
		return {0.40f, 0.30f, {-10, 0}, {16, 0}, 1.0f, stagger * 1.4f};
	case Composition::Turntable:
		return {0.36f, 0.28f, {0, -4}, {0, 6}, 1.0f, stagger};
	}
	return {0.38f, 0.26f, {0, -6}, {0, 9}, 0.94f, stagger};
}

int staggerIndex(int e)
{
	switch (e) {
	case kTitle:
		return 0;
	case kArtist:
		return 1;
	case kAlbum:
		return 2;
	case kSource:
		return 2;
	case kTimes:
	case kProgress:
		return 3;
	default:
		return 0;
	}
}

uint64_t mixKey(const std::string &s, uint64_t a, uint64_t b)
{
	uint64_t h = hashBytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
	h ^= a + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	h ^= b + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
	return h ? h : 1;
}

} // namespace

Scene::Scene(const Style &style) : style_(style) {}

void Scene::setStyle(const Style &style)
{
	style_ = style;
	++styleVersion_;
	if (hasDesired_)
		desiredKey_ = mixKey(desired_.identity, desired_.artworkHash, styleVersion_);
}

double Scene::duration(double baseMs) const
{
	if (style_.reducedMotion)
		return std::min(baseMs, 220.0) / 1000.0;
	return baseMs * std::clamp(style_.motionDuration, 0.1f, 4.0f) / 1000.0;
}

void Scene::update(double t, const SceneInput &in)
{
	const double dt = std::clamp(t - lastT_, 0.0, 0.25);
	lastT_ = t;

	const MediaSession *ms = in.session;
	const bool hasTrack = ms && ms->track.hasMetadata() && ms->status != PlaybackStatus::Stopped;
	status_ = hasTrack ? ms->status : PlaybackStatus::Unknown;

	bool wantVisible = false;
	if (hasTrack) {
		desired_ = resolveContent(ms->track, ms->appName);
		hasDesired_ = true;
		wantVisible = true;
		lastAppId_ = ms->appId;

		const bool playing = ms->status == PlaybackStatus::Playing;
		timelineKnown_ = ms->timeline.known && ms->timeline.durationSec > 0;
		durationSec_ = ms->timeline.durationSec;
		positionSec_ = ms->timeline.positionAt(in.now, playing);

		if (ms->status == PlaybackStatus::Paused) {
			if (pausedSince_ < 0)
				pausedSince_ = t;
			if (style_.paused == PausedBehavior::HideAfterDelay && t - pausedSince_ > style_.hideAfterPausedSec)
				wantVisible = false;
		} else {
			pausedSince_ = -1;
		}
	} else if (style_.idle == IdleBehavior::ShowPlaceholder) {
		desired_ = placeholderContent(style_.placeholderText);
		hasDesired_ = true;
		wantVisible = true;
		timelineKnown_ = false;
		pausedSince_ = -1;
	} else {
		pausedSince_ = -1;
		timelineKnown_ = false;
		// Keep the last content so the card can fade out with it.
	}
	if (hasDesired_)
		desiredKey_ = mixKey(desired_.identity, desired_.artworkHash, styleVersion_);

	// Only become visible once there is something rasterized to show.
	if (wantVisible && !cur_.layers)
		wantVisible = false;

	if (wantVisible != targetVisible_) {
		targetVisible_ = wantVisible;
		appearFrom_ = appear_;
		appearChangedAt_ = t;
	}
	{
		const double d = targetVisible_ ? duration(durAppear) : duration(durStandard + 60);
		const float p = phase(t, appearChangedAt_, 0, d);
		const float e = targetVisible_ ? easing::enter(p) : easing::exit(p);
		appear_ = appearFrom_ + ((targetVisible_ ? 1.0f : 0.0f) - appearFrom_) * e;
	}

	const bool dimWanted = status_ == PlaybackStatus::Paused && style_.paused == PausedBehavior::Dim;
	dim_ = approach(dim_, dimWanted ? 1.0f : 0.0f, static_cast<float>(dt), style_.reducedMotion ? 0.0f : 0.11f);

	// The platter spins up to 33 1/3 RPM while playing and coasts down on pause. The
	// arm sets down on the lead-in, tracks inward with the song, and lifts back to its
	// rest for pauses and track changes. These scalars only drive GPU transforms; the
	// disc and arm textures are never rebuilt per frame.
	if (style_.composition == Composition::Turntable) {
		const float fdt = std::min(static_cast<float>(dt), 0.05f);
		const bool playing = status_ == PlaybackStatus::Playing;
		const Layout *lay = cur_.layers ? &cur_.layers->layout : nullptr;
		armTarget_ = 0.0f;
		if (playing && lay && lay->recordVisible) {
			const float prog = timelineKnown_ ? static_cast<float>(std::clamp(positionSec_ / durationSec_, 0.0, 1.0)) : 0.0f;
			armTarget_ = lay->armOuter + (lay->armInner - lay->armOuter) * prog;
			// A new record: lift, swing clear, then cue the lead-in.
			if (transitionKind_ == TransitionKind::Track && t - transitionStart_ < duration(durTrack) * 0.85)
				armTarget_ = 0.0f;
		}
		if (style_.reducedMotion) {
			spin_ = 0;
			armAngle_ = armTarget_;
			armVelocity_ = 0;
		} else {
			spin_ = approach(spin_, playing ? 3.4906585f : 0.0f, fdt, playing ? 0.28f : 0.24f);
			if (!playing && spin_ < 0.04f)
				spin_ = 0;
			recordAngle_ = std::fmod(recordAngle_ + spin_ * fdt, 6.2831853f);
			constexpr float w0 = 6.5f; // settles in about 0.7 s without overshoot
			const float accel = w0 * w0 * (armTarget_ - armAngle_) - 2.0f * w0 * armVelocity_;
			armVelocity_ += accel * fdt;
			armAngle_ += armVelocity_ * fdt;
		}
	}

	// Animated artwork: fades in over the still cover once the host has a frame for
	// exactly the track on screen. A different track drops it at once (its still
	// cover is underneath, so nothing flashes).
	{
		const std::string &shown = shownIdentity();
		if (videoIdentity_ != shown) {
			videoIdentity_ = shown;
			videoMix_ = 0;
		}
		const bool wanted = wantsMotion(style_) && cur_.layers && cur_.layers->videoMask.valid && !cur_.layers->placeholder &&
				    in.motion.ready && in.motion.identity == shown;
		const float d = static_cast<float>(std::max(0.05, duration(durEmphasis)));
		const float step = static_cast<float>(dt) / d;
		videoMix_ = wanted ? std::min(1.0f, videoMix_ + step) : std::max(0.0f, videoMix_ - step * 1.6f);
	}

	// Progress: follow the timeline exactly, but glide across jumps (seeks, new tracks).
	const float target = timelineKnown_ ? static_cast<float>(std::clamp(positionSec_ / durationSec_, 0.0, 1.0)) : 0.0f;
	if (style_.reducedMotion || std::fabs(target - progressShown_) < 0.004f)
		progressShown_ = target;
	else
		progressShown_ = approach(progressShown_, target, static_cast<float>(dt), 0.085f);

	if (timelineKnown_) {
		wantElapsed_ = static_cast<int>(std::floor(positionSec_));
		wantRemaining_ = std::max(0, static_cast<int>(std::floor(durationSec_)) - wantElapsed_);
	} else {
		wantElapsed_ = wantRemaining_ = -1;
	}

	// Drop the outgoing deck once its transition finishes.
	if (prev_.layers) {
		const double d = transitionKind_ == TransitionKind::Track ? duration(durTrack)
				 : transitionKind_ == TransitionKind::Artwork ? duration(durEmphasis)
									     : duration(durQuick);
		if (t - transitionStart_ >= d)
			prev_ = {};
	}

	// Scene-wide transform for appearing and disappearing.
	const float u = cur_.layers ? cur_.layers->layout.unit : 1.0f;
	const float intensity = style_.reducedMotion ? 0.0f : std::clamp(style_.motionIntensity, 0.0f, 2.0f);
	gOpacity_ = appear_;
	gDy_ = (1.0f - appear_) * 10.0f * u * intensity;
	gScale_ = 1.0f - (1.0f - appear_) * 0.025f * intensity;
}

std::optional<LayerRequest> Scene::layerRequest() const
{
	if (!hasDesired_)
		return std::nullopt;
	if (cur_.layers && cur_.layers->identity == desired_.identity && cur_.layers->artworkHash == desired_.artworkHash &&
	    desiredKey_ == lastProvidedKey())
		return std::nullopt;
	LayerRequest r;
	r.key = desiredKey_;
	r.style = style_;
	r.content = desired_;
	return r;
}

uint64_t Scene::lastProvidedKey() const
{
	return providedKey_;
}

void Scene::provideLayers(uint64_t key, std::shared_ptr<LayerSet> set)
{
	if (!set || key != desiredKey_)
		return;
	providedKey_ = key;
	if (cur_.layers && appear_ > 0.001f) {
		if (cur_.layers->identity == set->identity)
			transitionKind_ = cur_.layers->artworkHash != set->artworkHash ? TransitionKind::Artwork : TransitionKind::Restyle;
		else
			transitionKind_ = TransitionKind::Track;
		prev_ = cur_;
		transitionStart_ = lastT_;
	} else {
		prev_ = {};
		transitionStart_ = -1e9;
	}
	cur_.layers = std::move(set);
	cur_.shownAt = transitionKind_ == TransitionKind::Track || !prev_.layers ? lastT_ : cur_.shownAt;
	if (!prev_.layers)
		cur_.shownAt = lastT_;
	times_ = {};
}

std::optional<TimeRequest> Scene::timeRequest() const
{
	if (!cur_.layers || cur_.layers->placeholder || wantElapsed_ < 0)
		return std::nullopt;
	const Layout &L = cur_.layers->layout;
	if (!L.elapsed.visible && !L.remaining.visible)
		return std::nullopt;
	if (times_.layerSerial == cur_.layers->serial && times_.elapsedSec == wantElapsed_ && times_.remainingSec == wantRemaining_)
		return std::nullopt;
	TimeRequest r;
	r.style = style_;
	r.set = cur_.layers;
	r.elapsed = wantElapsed_;
	r.remaining = wantRemaining_;
	r.key = mixKey("t", cur_.layers->serial, (static_cast<uint64_t>(wantElapsed_) << 32) | static_cast<uint32_t>(wantRemaining_));
	return r;
}

void Scene::provideTimes(uint64_t key, TimeLayers times)
{
	(void)key;
	if (!cur_.layers || times.layerSerial != cur_.layers->serial)
		return;
	times_ = std::move(times);
}

bool Scene::animating(double t) const
{
	if (prev_.layers)
		return true;
	if (style_.composition == Composition::Turntable && !style_.reducedMotion &&
	    (spin_ > 0 || std::fabs(armVelocity_) > 0.0005f || std::fabs(armTarget_ - armAngle_) > 0.0005f))
		return true;
	if (std::fabs(appear_ - (targetVisible_ ? 1.0f : 0.0f)) > 0.001f)
		return true;
	(void)t;
	return false;
}

// --- Choreography -----------------------------------------------------------

float Scene::elementOpacityOut(int e, float p) const
{
	const Choreography ch = choreographyFor(style_.composition);
	switch (e) {
	case kSurface:
		return 1.0f; // the incoming surface covers it
	case kGlow:
		return 1.0f - easing::exit(std::clamp(p / 0.35f, 0.0f, 1.0f));
	case kArt:
		return 1.0f - easing::inOut(std::clamp((p - 0.22f) / 0.5f, 0.0f, 1.0f));
	default:
		return 1.0f - easing::exit(std::clamp(p / ch.outEnd, 0.0f, 1.0f));
	}
}

float Scene::elementOpacityIn(int e, float p) const
{
	const Choreography ch = choreographyFor(style_.composition);
	if (transitionKind_ == TransitionKind::Restyle)
		return easing::standard(p);
	switch (e) {
	case kSurface:
		return easing::inOut(p);
	case kGlow: {
		if (p < 0.4f)
			return 0.0f;
		// Brief "power on": rise past full strength, then settle.
		const float q = (p - 0.4f) / 0.6f;
		const float rise = easing::enter(std::clamp(q / 0.45f, 0.0f, 1.0f));
		const float settle = q > 0.45f ? easing::standard((q - 0.45f) / 0.55f) : 0.0f;
		return rise * 1.18f - 0.18f * settle;
	}
	case kArt:
		return easing::enter(std::clamp((p - 0.12f) / 0.55f, 0.0f, 1.0f));
	default: {
		if (transitionKind_ == TransitionKind::Artwork)
			return 1.0f;
		const double total = duration(durTrack);
		const float delay = total > 0 ? static_cast<float>(staggerIndex(e) * ch.staggerMs / 1000.0 / total) : 0.0f;
		return easing::enter(std::clamp((p - ch.inStart - delay) / 0.5f, 0.0f, 1.0f));
	}
	}
}

Vec2 Scene::shiftOut(int e, float p) const
{
	if (style_.reducedMotion || e == kSurface || e == kGlow || e == kArt || e == kProgress)
		return {};
	const Choreography ch = choreographyFor(style_.composition);
	const float u = cur_.layers ? cur_.layers->layout.unit : 1.0f;
	const float k = easing::exit(std::clamp(p / ch.outEnd, 0.0f, 1.0f)) * style_.motionIntensity * u;
	return {ch.outShift.x * k, ch.outShift.y * k};
}

Vec2 Scene::shiftIn(int e, float p) const
{
	if (style_.reducedMotion || e == kSurface || e == kGlow || e == kArt || e == kProgress || transitionKind_ != TransitionKind::Track)
		return {};
	const Choreography ch = choreographyFor(style_.composition);
	const float u = cur_.layers ? cur_.layers->layout.unit : 1.0f;
	const float k = (1.0f - elementOpacityIn(e, p)) * style_.motionIntensity * u;
	return {ch.inShift.x * k, ch.inShift.y * k};
}

// --- Frame ------------------------------------------------------------------

const std::string &Scene::shownIdentity() const
{
	static const std::string none;
	return cur_.layers ? cur_.layers->identity : none;
}

void Scene::emitVideo(std::vector<DrawOp> &out, const LayerSet &set, float opacity, float scale, Vec2 shift) const
{
	const auto &m = set.videoMask;
	if (!m.valid || opacity <= 0.002f)
		return;
	DrawOp op;
	op.kind = DrawOp::Kind::Video;
	op.color = Color{0, 0, 0, 0}; // no tint
	Rect r = m.box.translated(shift.x, shift.y);
	if (scale != 1.0f) {
		const float cx = r.cx(), cy = r.cy();
		r = {cx + (r.x - cx) * scale, cy + (r.y - cy) * scale, r.w * scale, r.h * scale};
	}
	op.dst = transform(r);
	op.radius = m.radius * scale * gScale_;
	op.clipRect = transform(m.clip);
	op.clipRadius = m.clipRadius * gScale_;
	op.featherEdge = m.featherEdge;
	op.feather = m.feather * scale * gScale_;
	op.opacity = std::clamp(opacity * gOpacity_, 0.0f, 1.0f);
	out.push_back(std::move(op));
}

Rect Scene::transform(const Rect &r) const
{
	if (!cur_.layers)
		return r;
	const Rect &card = cur_.layers->layout.card;
	const float cx = card.cx(), cy = card.cy();
	return {cx + (r.x - cx) * gScale_, cy + (r.y - cy) * gScale_ + gDy_, r.w * gScale_, r.h * gScale_};
}

void Scene::emitImage(std::vector<DrawOp> &out, const LayerImage &li, float opacity, Vec2 shift, float scale) const
{
	if (!li.valid() || opacity <= 0.002f)
		return;
	DrawOp op;
	op.kind = DrawOp::Kind::Image;
	op.image = li.image;
	op.imageId = li.id;
	Rect r = li.dst.translated(shift.x, shift.y);
	if (scale != 1.0f) {
		// Scale about the element box center, not the padded image.
		const float cx = li.box.cx() + shift.x, cy = li.box.cy() + shift.y;
		r = {cx + (r.x - cx) * scale, cy + (r.y - cy) * scale, r.w * scale, r.h * scale};
	}
	op.dst = transform(r);
	op.opacity = std::clamp(opacity * gOpacity_, 0.0f, 1.0f);
	out.push_back(std::move(op));
}

void Scene::emitText(std::vector<DrawOp> &out, const LayerImage &li, double t, const Deck &deck, float opacity, Vec2 shift) const
{
	if (!li.valid() || opacity <= 0.002f)
		return;
	if (!li.overflow) {
		emitImage(out, li, opacity, shift);
		return;
	}
	const float u = deck.layers->layout.unit;
	const float speed = std::max(4.0f, style_.marqueeSpeed) * u;
	const float gap = std::max(40.0f * u, li.box.w * 0.22f);
	const float distance = li.naturalWidth + gap;
	const double pause = std::max(0.0f, style_.marqueePause);
	const double period = pause + distance / speed;
	const double start = deck.shownAt + duration(durTrack);
	const double local = t > start ? std::fmod(t - start, period) : 0.0;
	const float offset = local < pause ? 0.0f : static_cast<float>((local - pause) * speed);
	const float fadeW = 14.0f * u;

	for (int copy = 0; copy < 2; ++copy) {
		if (copy == 1 && offset <= 0)
			break;
		DrawOp op;
		op.image = li.image;
		op.imageId = li.id;
		const float dx = -offset + (copy == 1 ? distance : 0.0f);
		op.dst = transform(li.dst.translated(dx + shift.x, shift.y));
		op.opacity = std::clamp(opacity * gOpacity_, 0.0f, 1.0f);
		op.clip = true;
		op.clipRect = transform(Rect{li.box.x + shift.x, li.dst.y + shift.y, li.box.w, li.dst.h});
		op.fadeLeft = std::min(1.0f, offset / fadeW) * fadeW * gScale_;
		op.fadeRight = fadeW * gScale_;
		out.push_back(std::move(op));
	}
}

void Scene::buildFrame(double t, std::vector<DrawOp> &out)
{
	out.clear();
	if (!cur_.layers || appear_ <= 0.001f)
		return;

	const LayerSet &C = *cur_.layers;
	const LayerSet *P = prev_.layers.get();
	const double d = transitionKind_ == TransitionKind::Track ? duration(durTrack)
			 : transitionKind_ == TransitionKind::Artwork ? duration(durEmphasis)
								     : duration(durQuick);
	const float p = P ? phase(t, transitionStart_, 0, d) : 1.0f;
	const float dimArt = 1.0f - 0.38f * dim_;
	const float dimText = 1.0f - 0.28f * dim_;
	const Choreography ch = choreographyFor(style_.composition);

	// Surfaces
	if (P)
		emitImage(out, P->surface, 1.0f, {});
	emitImage(out, C.surface, P ? elementOpacityIn(kSurface, p) : 1.0f, {});

	// Neon edge glow
	if (P && P->glowRing.valid())
		emitImage(out, P->glowRing, elementOpacityOut(kGlow, p), {});
	if (C.glowRing.valid()) {
		const float g = P ? elementOpacityIn(kGlow, p) : 1.0f;
		emitImage(out, C.glowRing, std::min(1.0f, g) * (1.0f - 0.45f * dim_), {});
		if (g > 1.0f)
			emitImage(out, C.glowRing, g - 1.0f, {});
	}

	// Artwork
	const bool turntable = style_.composition == Composition::Turntable;
	const float sleeveSlide = turntable && !style_.reducedMotion ? 11.0f * C.layout.unit * style_.motionIntensity : 0.0f;
	if (P)
		emitImage(out, P->art, elementOpacityOut(kArt, p) * dimArt, {0, -sleeveSlide * easing::exit(p)});
	{
		float scale = 1.0f;
		if (P && transitionKind_ == TransitionKind::Track && !style_.reducedMotion) {
			const float from = 1.0f - (1.0f - ch.artScaleFrom) * std::clamp(style_.motionIntensity, 0.0f, 2.0f);
			scale = from + (1.0f - from) * easing::enter(std::clamp((p - 0.1f) / 0.75f, 0.0f, 1.0f));
		}
		const float inOpacity = P ? elementOpacityIn(kArt, p) : 1.0f;
		const float artOpacity = inOpacity * dimArt;
		const Vec2 sleeveShift{0, sleeveSlide * (1.0f - easing::enter(p))};
		emitImage(out, C.art, artOpacity, sleeveShift, scale);
		if (videoMix_ > 0.0f) {
			const float mix = easing::inOut(videoMix_);
			// The paused dim is applied to the video as a tint toward the card tone rather
			// than as transparency, so the still cover underneath never shows through it.
			// Editorial bakes its cover into the (never dimmed) surface, so it isn't tinted.
			const bool baked = !C.art.valid();
			const float videoOpacity = (baked ? (P ? elementOpacityIn(kSurface, p) : 1.0f) : inOpacity) * mix;
			emitVideo(out, C, videoOpacity, scale, sleeveShift);
			if (!baked && !out.empty() && out.back().kind == DrawOp::Kind::Video) {
				out.back().color = C.surfaceTone;
				out.back().color.a = 1.0f - dimArt;
			}
			const size_t before = out.size();
			emitImage(out, C.artOverlay, artOpacity * mix, sleeveShift, scale);
			if (out.size() > before)
				out.back().needsVideo = true;
		}
	}

	// The sleeve (including optional motion artwork) stays behind the record. Order:
	// platter and cast shadow, the turning pressing, the fixed light, then the arm.
	if (turntable) {
		// The hardware is never dimmed for pauses: a see-through record would read as a
		// ghost. The lifted arm and the stopped platter already say "paused".
		auto deck = [&](const LayerSet &layer, float opacity, float scale, bool sheen) {
			emitImage(out, layer.platter, opacity, {}, scale);
			const size_t before = out.size();
			emitImage(out, layer.record, opacity, {}, scale);
			if (out.size() > before)
				out.back().rotation = style_.reducedMotion ? 0.0f : recordAngle_;
			if (sheen)
				emitImage(out, layer.sheen, opacity, {}, scale);
		};
		// On a track change the new pressing settles onto the platter over the old one,
		// which stays opaque underneath so the two colors never muddy into each other.
		const bool swap = P && transitionKind_ == TransitionKind::Track && P->record.valid() && C.record.valid() &&
				  P->layout.record.x == C.layout.record.x && P->layout.record.w == C.layout.record.w;
		if (swap) {
			const float in = elementOpacityIn(kArt, p);
			deck(*P, in < 0.999f ? 1.0f : 0.0f, 1.0f, false);
			deck(C, in, style_.reducedMotion ? 1.0f : 1.0f + 0.04f * (1.0f - easing::enter(p)), false);
			emitImage(out, C.sheen, 1.0f, {});
		} else {
			if (P)
				deck(*P, elementOpacityOut(kArt, p), 1.0f, true);
			deck(C, P ? elementOpacityIn(kArt, p) : 1.0f, 1.0f, true);
		}
		const size_t before = out.size();
		emitImage(out, C.arm, 1.0f, {});
		if (out.size() > before) {
			DrawOp &arm = out.back();
			const Rect &zone = C.layout.armZone;
			arm.rotationOrigin = {(C.layout.armPivot.x - zone.x) / std::max(1.0f, zone.w),
				(C.layout.armPivot.y - zone.y) / std::max(1.0f, zone.h)};
			arm.rotation = armAngle_;
		}
	}

	// Text
	auto textOut = [&](const LayerImage &li, int e) { emitText(out, li, t, prev_, elementOpacityOut(e, p) * dimText, shiftOut(e, p)); };
	auto textIn = [&](const LayerImage &li, int e) {
		emitText(out, li, t, cur_, (P ? elementOpacityIn(e, p) : 1.0f) * dimText, P ? shiftIn(e, p) : Vec2{});
	};
	if (P && transitionKind_ != TransitionKind::Artwork) {
		textOut(P->title, kTitle);
		textOut(P->artist, kArtist);
		textOut(P->album, kAlbum);
		textOut(P->source, kSource);
	}
	textIn(C.title, kTitle);
	textIn(C.artist, kArtist);
	textIn(C.album, kAlbum);
	textIn(C.source, kSource);

	// Progress
	const Layout &L = C.layout;
	if (L.progressVisible && !C.placeholder) {
		const float showIn = P ? std::max(elementOpacityIn(kProgress, p), P->layout.progressVisible ? 1.0f : 0.0f) : 1.0f;
		Color accent = P ? P->accent.mix(C.accent, easing::inOut(p)) : C.accent;
		// Paused: the fill quiets down toward the track color.
		accent = accent.mix(Color{accent.r, accent.g, accent.b, accent.a * 0.45f}, dim_);
		DrawOp track;
		track.kind = DrawOp::Kind::RoundRect;
		track.rect = transform(L.progress);
		track.radius = L.progressRadius * gScale_;
		track.color = C.trackColor;
		track.color.a *= gOpacity_ * showIn;
		out.push_back(track);

		const float frac = timelineKnown_ ? progressShown_ : 0.0f;
		if (frac > 0) {
			Rect fill = L.progress;
			fill.w = std::max(L.progress.h, L.progress.w * frac);
			if (style_.progressGlow > 0) {
				DrawOp g;
				g.kind = DrawOp::Kind::RoundRect;
				g.rect = transform(fill);
				g.radius = L.progressRadius * gScale_;
				g.color = accent;
				g.color.a *= 0.55f * style_.progressGlow * gOpacity_ * showIn * (1.0f - 0.6f * dim_);
				g.softness = 7.0f * L.unit * gScale_;
				out.push_back(g);
			}
			DrawOp f;
			f.kind = DrawOp::Kind::RoundRect;
			f.rect = transform(fill);
			f.radius = L.progressRadius * gScale_;
			f.color = accent;
			f.color.a *= gOpacity_ * showIn;
			out.push_back(f);
		}
	}

	// Times (belong to the current deck only)
	if (times_.layerSerial == C.serial) {
		const float o = (P && transitionKind_ == TransitionKind::Track ? elementOpacityIn(kTimes, p) : 1.0f) * dimText;
		const Vec2 s = P ? shiftIn(kTimes, p) : Vec2{};
		emitImage(out, times_.elapsed, o, s);
		emitImage(out, times_.remaining, o, s);
	}
}

} // namespace tonearm
