// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "raster.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace tonearm {

// One draw command for the compositor. Images are premultiplied.
struct DrawOp {
	enum class Kind { Image, RoundRect, Video } kind = Kind::Image;

	// Image
	ImagePtr image;
	uint64_t imageId = 0;
	Rect dst;
	float opacity = 1;
	float rotation = 0;     // radians; GPU rotates cached textures, never re-rasterizes per frame
	Vec2 rotationOrigin{0.5f, 0.5f}; // normalized anchor within the image destination
	bool clip = false;       // restrict to clipRect with soft horizontal edges
	Rect clipRect;
	float fadeLeft = 0, fadeRight = 0;

	// RoundRect (progress bar, glows)
	Rect rect;
	float radius = 0;
	Color color;             // straight alpha
	float softness = 0;      // 0 = crisp anti-aliased edge; > 0 = blurred glow falloff in px

	// Image drawn only together with the video (scrim/stroke over animated artwork);
	// skipped whenever the compositor has no video frame.
	bool needsVideo = false;

	// Video (animated artwork): the host supplies the current frame. `dst` is the
	// artwork box; the frame is center-cropped to fill it ("cover"), masked by a
	// rounded rect of `radius`, clipped to `clipRect`/`clipRadius`, and feathered
	// along one edge. Opacity is in `opacity`; `color` (rgb) is a tint mixed in by
	// `color.a` (used for the paused dim).
	float clipRadius = 0;
	int featherEdge = 0;     // 0 none, 1 right, 2 bottom
	float feather = 0;
};

// The host's animated artwork state for the current frame.
struct MotionFrame {
	std::string identity;    // track identity the loaded video belongs to
	bool ready = false;      // a decoded frame of that video is available
};

struct SceneInput {
	ProviderState provider = ProviderState::Starting;
	const MediaSession *session = nullptr; // selected session, may be null
	Clock::time_point now{};
	MotionFrame motion;
};

struct LayerRequest {
	uint64_t key = 0;
	Style style;
	DisplayContent content;
};

struct TimeRequest {
	uint64_t key = 0;
	Style style;
	std::shared_ptr<const LayerSet> set;
	int elapsed = 0, remaining = 0;
};

// Scene owns presentation state: which content is shown, transitions, visibility,
// marquee and progress animation. It never touches the GPU or the OS; the host
// rasterizes requested layers (on any thread) and executes the draw list.
class Scene {
public:
	explicit Scene(const Style &style);

	void setStyle(const Style &style);
	const Style &style() const { return style_; }

	// Advance with a monotonic time in seconds.
	void update(double t, const SceneInput &in);

	// Work the host should perform. Keys change when the request changes.
	std::optional<LayerRequest> layerRequest() const;
	void provideLayers(uint64_t key, std::shared_ptr<LayerSet> set);
	std::optional<TimeRequest> timeRequest() const;
	void provideTimes(uint64_t key, TimeLayers times);

	void buildFrame(double t, std::vector<DrawOp> &out);

	// True while anything is moving; hosts may skip redundant work otherwise.
	bool animating(double t) const;
	bool visible() const { return appear_ > 0.001f || targetVisible_; }

	// Identity of the track whose artwork is shown (empty when none).
	const std::string &shownIdentity() const;
	// Layers currently shown (null before the first raster).
	const std::shared_ptr<const LayerSet> &currentLayers() const { return cur_.layers; }
	// Opacity of the animated artwork over the still cover, 0..1.
	float videoMix() const { return videoMix_; }

private:
	struct Deck {
		std::shared_ptr<const LayerSet> layers;
		double shownAt = 0;
	};

	enum class TransitionKind { Track, Artwork, Restyle };

	float elementOpacityIn(int element, float p) const;
	float elementOpacityOut(int element, float p) const;
	Vec2 shiftIn(int element, float p) const;
	Vec2 shiftOut(int element, float p) const;
	void emitText(std::vector<DrawOp> &out, const LayerImage &li, double t, const Deck &deck, float opacity, Vec2 shift) const;
	void emitImage(std::vector<DrawOp> &out, const LayerImage &li, float opacity, Vec2 shift, float scale = 1.0f) const;
	void emitVideo(std::vector<DrawOp> &out, const LayerSet &set, float opacity, float scale, Vec2 shift = {}) const;
	Rect transform(const Rect &r) const;
	double duration(double baseMs) const;
	uint64_t lastProvidedKey() const;
	uint64_t providedKey_ = 0;

	Style style_;
	uint64_t styleVersion_ = 1;

	// Desired content
	DisplayContent desired_;
	bool hasDesired_ = false;
	uint64_t desiredKey_ = 0;

	// Displayed content
	Deck cur_, prev_;
	double transitionStart_ = -1e9;
	TransitionKind transitionKind_ = TransitionKind::Track;
	TimeLayers times_;
	int wantElapsed_ = -1, wantRemaining_ = -1;

	// Physical turntable animation (no per-frame CPU image generation).
	float recordAngle_ = 0; // platter angle, radians
	float spin_ = 0;        // platter speed, radians per second (eases up and down)
	float armAngle_ = 0;    // arm rotation from rest, radians (critically damped spring)
	float armVelocity_ = 0;
	float armTarget_ = 0;

	// Playback
	PlaybackStatus status_ = PlaybackStatus::Unknown;
	double positionSec_ = 0, durationSec_ = 0;
	bool timelineKnown_ = false;
	float progressShown_ = 0;
	double pausedSince_ = -1;

	// Visibility
	bool targetVisible_ = false;
	float appear_ = 0;          // eased presence 0..1
	double appearChangedAt_ = -1e9;
	float appearFrom_ = 0;
	float dim_ = 0;             // paused dimming 0..1

	// Animated artwork
	float videoMix_ = 0;
	std::string videoIdentity_;

	double lastT_ = 0;
	std::string lastAppId_;
	// Global transform for appear/disappear
	float gOpacity_ = 1, gScale_ = 1, gDy_ = 0;
};

} // namespace tonearm
