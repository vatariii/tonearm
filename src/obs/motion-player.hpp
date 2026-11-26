// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Animated artwork playback for one Tonearm source.
//
// Decoding is delegated to OBS's own media source ("ffmpeg_source"), created as a
// private child: it brings FFmpeg, hardware decoding, HLS, looping and frame timing,
// and decodes each video once no matter how many times Tonearm is drawn. Tonearm
// copies the current frame, cropped and supersampled for the artwork box, into
// one reusable texture at most 60 times a second (never exceeding OBS's draw cadence).
//
// Nothing here exists while the feature is off: no child source, no resolver thread,
// no disk or network access.

#include "core/motion.hpp"
#include "core/scene.hpp"
#include "media/motion_resolver.hpp"

#include <obs.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tonearm {

class MotionPlayer {
public:
	MotionPlayer() = default;
	~MotionPlayer();
	MotionPlayer(const MotionPlayer &) = delete;
	MotionPlayer &operator=(const MotionPlayer &) = delete;

	// UI thread (source update). Creates or tears down everything as the style asks.
	void configure(const Style &style);

	// Graphics thread, every tick, also while hidden. `session` may be null.
	void tick(const MediaSession *session, bool showing, MotionShape preferred, float artworkSize = 0.0f);

	// Graphics thread: what the scene should know this frame.
	MotionFrame frame() const;

	// Graphics thread, inside video_render: returns the frame texture for the Video op
	// in `ops`, cropped to `box` (the artwork box at rest), or null.
	gs_texture_t *prepare(const std::vector<DrawOp> &ops, const Rect &box, double t);

	// Graphics thread, inside video_render: drops the capture target while animated
	// artwork is off. It is created again on demand.
	void releaseGraphics();

	bool enabled() const { return enabled_.load(); }
	// Graphics-thread only. OBS async video color format, not the user's
	// quality preference, determines whether the shader needs sRGB encoding.
	bool videoNeedsSrgbEncode() const { return videoNeedsSrgbEncode_; }

private:
	void teardown();
	void deliver(uint64_t serial, const std::optional<MotionAsset> &asset);
	obs_source_t *childRef() const;

	// Configuration (UI thread)
	std::string folder_;
	std::atomic<MotionQuality> quality_{MotionQuality::Smart};
	std::atomic<MotionQuality> effectiveQuality_{MotionQuality::Balanced};
	MotionPause pause_ = MotionPause::WithMusic;
	std::atomic<bool> enabled_{false};
	std::atomic<bool> pauseWithMusic_{true};

	// Shared between threads
	mutable std::mutex mutex_;
	obs_source_t *child_ = nullptr;
	std::unique_ptr<MotionResolver> resolver_;
	uint64_t wantedSerial_ = 0;
	std::string wantedIdentity_;
	std::string loadedIdentity_; // identity whose video is loaded in the child
	std::string loadedPath_;
	std::chrono::steady_clock::time_point lastMissAt_{}; // guarded by mutex_, bounded retry for failed lookups
	std::atomic<uint64_t> loadGeneration_{0};
	// Bumped by configure() (UI thread) after a new child and resolver exist. The
	// graphics thread compares it with seenConfigureGeneration_ and clears its own
	// requestedKey_, so the key is only ever touched by the graphics thread.
	std::atomic<uint64_t> configureGeneration_{0};

	// Graphics thread only
	std::string requestedKey_;
	uint64_t seenConfigureGeneration_ = 0;
	uint64_t serial_ = 0;
	bool appliedPaused_ = false;
	uint64_t appliedGeneration_ = ~0ull;
	std::chrono::steady_clock::time_point lastRestartAt_{};
	std::chrono::steady_clock::time_point lastDiagnosticsAt_{};
	std::chrono::steady_clock::time_point lastPlaybackProgressAt_{};
	int64_t lastObservedMediaTime_ = -1;
	uint64_t lastObservedGeneration_ = ~0ull;
	gs_texrender_t *texrender_ = nullptr; // created and destroyed inside the graphics context
	double lastCapture_ = -1;
	uint32_t texW_ = 0, texH_ = 0;
	bool haveTexture_ = false;
	bool capturedWhilePaused_ = false; // the held texture is a frame taken after the decoder paused
	bool videoNeedsSrgbEncode_ = false;
	uint64_t loggedTransferGeneration_ = ~0ull;
	double captureReportStart_ = -1.0;
	uint32_t capturedFramesInWindow_ = 0;
	uint64_t capturedGeneration_ = ~0ull;
};

} // namespace tonearm
