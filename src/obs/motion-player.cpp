// SPDX-License-Identifier: GPL-2.0-or-later
#include "motion-player.hpp"
#include "remote-motion-source.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace tonearm {

namespace {
constexpr double kMaxCaptureFps = 60.0; // match the fastest full-motion Apple artwork and 60 FPS OBS canvases
}

MotionPlayer::~MotionPlayer()
{
	teardown();
	if (texrender_) {
		obs_enter_graphics();
		gs_texrender_destroy(texrender_);
		texrender_ = nullptr;
		obs_leave_graphics();
	}
}

void MotionPlayer::teardown()
{
	std::unique_ptr<MotionResolver> resolver;
	obs_source_t *child = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		resolver = std::move(resolver_);
		child = child_;
		child_ = nullptr;
		loadedIdentity_.clear();
		loadedPath_.clear();
		loadedLease_ = {};
		lastMissAt_ = {};
		wantedSerial_ = 0;
		wantedIdentity_.clear();
	}
	resolver.reset(); // joins the resolver thread (it may be opening media) outside the lock
	if (child) {
		// This private media source is a child, not a scene item. We explicitly
		// hold a showing reference while it exists, so return it exactly once.
		obs_source_dec_showing(child);
		obs_source_release(child);
	}
	++loadGeneration_;
	enabled_ = false;
}

void MotionPlayer::configure(const Style &style)
{
	const bool want = wantsMotion(style);
	pauseWithMusic_ = style.motionPause == MotionPause::WithMusic;
	if (want == enabled_.load() && (!want || (style.motionFolder == folder_ && style.motionQuality == quality_.load())))
		return;

	teardown();
	folder_ = style.motionFolder;
	quality_.store(style.motionQuality);
	if (!want)
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", "");
	obs_data_set_string(settings, "input", "");
	obs_data_set_bool(settings, "looping", true);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "clear_on_media_end", false);
	obs_data_set_bool(settings, "hw_decode", true);
	obs_data_set_bool(settings, "log_changes", false);
	obs_source_t *child = obs_source_create_private("ffmpeg_source", "Tonearm animated artwork", settings);
	obs_data_release(settings);
	if (!child) {
		blog(LOG_WARNING, "[tonearm] animated artwork unavailable: OBS media source could not be created");
		return;
	}
	obs_source_set_muted(child, true); // motion artwork is silent; never feed the mixer
	// A private OBS FFmpeg source is NOT automatically considered visible just
	// because Tonearm renders it with obs_source_video_render(). Without an
	// explicit showing reference, FFmpeg's media_restart callback refuses
	// to start it. This matters most for short, finite Apple Music HLS loops.
	obs_source_inc_showing(child);

	std::unique_ptr<MotionSource> motionSource;
#ifdef _WIN32
	if (folder_.empty())
		motionSource = std::make_unique<RemoteArtworkApiSource>();
#endif
	if (!motionSource)
		motionSource = std::make_unique<LocalFolderSource>(folder_);
	auto resolver = std::make_unique<MotionResolver>(std::move(motionSource),
							 [this](uint64_t serial, const std::optional<MotionAsset> &asset) { deliver(serial, asset); });
	{
		std::lock_guard<std::mutex> lock(mutex_);
		child_ = child;
		resolver_ = std::move(resolver);
	}
	// The graphics thread notices the new generation and re-requests the current track.
	// requestedKey_ itself belongs to the graphics thread and is never written here.
	configureGeneration_.fetch_add(1);
	enabled_ = true;
	blog(LOG_INFO, "[tonearm] animated artwork on (%s)", folder_.empty() ? "online lookup" : "local folder");
}

obs_source_t *MotionPlayer::childRef() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return child_ ? obs_source_get_ref(child_) : nullptr;
}

// Resolver thread.
void MotionPlayer::deliver(uint64_t serial, const std::optional<MotionAsset> &asset)
{
	obs_source_t *child = nullptr;
	std::string identity;
	bool samePath = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (serial != wantedSerial_ || !child_)
			return; // obsolete answer
		child = obs_source_get_ref(child_);
		identity = wantedIdentity_;
		samePath = asset && asset->location == loadedPath_;
		if (!asset || !samePath)
			loadedIdentity_.clear(); // hide the old video before anything else happens
	}
	if (!child)
		return;

	if (!asset) {
		// No motion artwork for this release: stop decoding entirely.
		obs_source_media_stop(child);
		obs_source_output_video(child, nullptr);
		std::lock_guard<std::mutex> lock(mutex_);
		if (serial == wantedSerial_)
			lastMissAt_ = std::chrono::steady_clock::now();
		loadedPath_.clear();
		loadedLease_ = {};
	} else if (samePath) {
		// Same video (another track of the same album): keep it running seamlessly.
		std::lock_guard<std::mutex> lock(mutex_);
		if (serial == wantedSerial_) {
			loadedIdentity_ = identity;
			lastMissAt_ = {};
		}
	} else {
		// Clear the previous frame so a stale cover can never be shown for this track,
		// then open the new file. Opening may block briefly; this is not the graphics thread.
		obs_source_output_video(child, nullptr);
		obs_data_t *settings = obs_data_create();
		// OBS's non-local "input" mode treats HLS as a live broadcast and
		// explicitly DISABLES looping, even for a finite video-on-demand playlist.
		// Genuine Apple motion artwork is a short VOD loop. FFmpeg accepts an
		// HTTPS HLS URL as its "local_file" path; this selects OBS's seekable,
		// looping media-file playback semantics while still fetching over HTTPS.
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", asset->location.c_str());
		obs_data_set_string(settings, "input", "");
		obs_source_update(child, settings);
		obs_data_release(settings);
		std::lock_guard<std::mutex> lock(mutex_);
		loadedPath_ = asset->location;
		loadedLease_ = MotionCacheLeases::Lease(
			MotionCacheLeases::shared(),
			motionCacheKeyForPath(std::filesystem::path(std::u8string(asset->location.begin(), asset->location.end()))));
		if (serial == wantedSerial_) {
			loadedIdentity_ = identity;
			lastMissAt_ = {};
		}
		blog(LOG_INFO, "[tonearm] motion asset loaded; source=%s, shape=%s",
		     asset->location.find("MotionCache") != std::string::npos ? "cached online HLS" :
		     asset->location.rfind("https://", 0) == 0 ? "online" : "local",
		     asset->shape == MotionShape::Portrait ? "portrait" : "square");
	}
	++loadGeneration_;
	obs_source_release(child);
}

void MotionPlayer::tick(const MediaSession *session, bool showing, MotionShape preferred, float artworkSize)
{
	if (!enabled_)
		return;
	obs_source_t *child = childRef();
	if (!child)
		return;
	const uint64_t configured = configureGeneration_.load();
	if (configured != seenConfigureGeneration_) {
		seenConfigureGeneration_ = configured;
		requestedKey_.clear(); // a new resolver: ask again for the current track
	}

	// New track (or new preferred shape): ask the resolver. It cancels older work.
	if (session && session->track.hasMetadata()) {
		const std::string identity = session->track.identity();
		const MotionQuality requestedQuality = effectiveMotionQuality(quality_.load(), artworkSize);
		effectiveQuality_.store(requestedQuality);
		const std::string key = identity + (preferred == MotionShape::Portrait ? "\x1fp" : "\x1fs") +
				(requestedQuality == MotionQuality::Maximum ? "m" : "b");
		bool retryMiss = false;
		if (key == requestedKey_) {
			std::lock_guard<std::mutex> lock(mutex_);
			retryMiss = lastMissAt_ != std::chrono::steady_clock::time_point{} &&
				std::chrono::steady_clock::now() - lastMissAt_ > std::chrono::seconds(75);
		}
		if (key != requestedKey_ || retryMiss) {
			requestedKey_ = key;
			MotionQuery q;
			q.artist = session->track.artist;
			q.albumArtist = session->track.albumArtist;
			q.album = session->track.album;
			q.title = session->track.title;
			q.preferred = preferred;
			q.quality = requestedQuality;
			std::lock_guard<std::mutex> lock(mutex_);
			wantedSerial_ = ++serial_;
			wantedIdentity_ = identity;
			lastMissAt_ = {};
			if (loadedIdentity_ != identity)
				loadedIdentity_.clear();
			if (resolver_)
				resolver_->request(wantedSerial_, q);
		}
	}

	// Pause decoding when the music pauses (if asked) and whenever nobody can see it.
	const bool musicPaused = session && session->status == PlaybackStatus::Paused;
	const bool wantPaused = !showing || (pauseWithMusic_ && musicPaused);
	const uint64_t gen = loadGeneration_.load();
	if (wantPaused != appliedPaused_ || gen != appliedGeneration_) {
		const obs_media_state st = obs_source_media_get_state(child);
		if (st == OBS_MEDIA_STATE_PLAYING || st == OBS_MEDIA_STATE_PAUSED) {
			obs_source_media_play_pause(child, wantPaused);
			appliedPaused_ = wantPaused;
			appliedGeneration_ = gen;
		}
	}
	// Short Apple motion loops should continuously advance even when OBS keeps
	// reporting PLAYING. Recover from missing loop transitions and rare stalled
	// decoders without touching the current album's other UI layers.
	const bool shouldRunVideo = showing && session && (!musicPaused || !pauseWithMusic_);
	if (shouldRunVideo) {
		bool loaded = false;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			loaded = !loadedIdentity_.empty();
		}
		if (loaded) {
			const auto now = std::chrono::steady_clock::now();
			const obs_media_state st = obs_source_media_get_state(child);
			const int64_t elapsed = obs_source_media_get_time(child);
			if (gen != lastObservedGeneration_) {
				lastObservedGeneration_ = gen;
				lastObservedMediaTime_ = elapsed;
				lastPlaybackProgressAt_ = now;
			}
			if (elapsed >= 0 && elapsed != lastObservedMediaTime_) {
				lastObservedMediaTime_ = elapsed;
				lastPlaybackProgressAt_ = now;
			}
			if (now - lastDiagnosticsAt_ >= std::chrono::seconds(8)) {
				blog(LOG_INFO, "[tonearm] motion playback: state=%d, elapsed_ms=%lld, duration_ms=%lld, showing=%d",
				     static_cast<int>(st),
				     static_cast<long long>(elapsed),
				     static_cast<long long>(obs_source_media_get_duration(child)), showing ? 1 : 0);
				lastDiagnosticsAt_ = now;
			}
			const bool reachedEnd = st == OBS_MEDIA_STATE_ENDED;
			const bool stopped = st == OBS_MEDIA_STATE_STOPPED &&
					     now - lastPlaybackProgressAt_ >= std::chrono::seconds(7);
			const bool stalled = st == OBS_MEDIA_STATE_PLAYING &&
					    now - lastPlaybackProgressAt_ >= std::chrono::seconds(9);
			if ((reachedEnd || stopped || stalled) &&
			    now - lastRestartAt_ >= std::chrono::seconds(15)) {
				blog(LOG_WARNING, "[tonearm] motion playback %s; attempting bounded restart",
				     reachedEnd ? "ended" : stopped ? "stopped" : "stalled");
				obs_source_media_restart(child);
				lastRestartAt_ = now;
				lastObservedMediaTime_ = -1;
				lastPlaybackProgressAt_ = now;
			}
		}
	}

	obs_source_release(child);
}

MotionFrame MotionPlayer::frame() const
{
	MotionFrame f;
	if (!enabled_)
		return f;
	obs_source_t *child = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!child_ || loadedIdentity_.empty())
			return f;
		f.identity = loadedIdentity_;
		child = obs_source_get_ref(child_);
	}
	if (!child)
		return f;
	const obs_media_state st = obs_source_media_get_state(child);
	f.ready = (st == OBS_MEDIA_STATE_PLAYING || st == OBS_MEDIA_STATE_PAUSED) && obs_source_get_width(child) > 0 &&
		  obs_source_get_height(child) > 0 && haveTexture_;
	obs_source_release(child);
	return f;
}

void MotionPlayer::releaseGraphics()
{
	haveTexture_ = false;
	if (texrender_) {
		gs_texrender_destroy(texrender_);
		texrender_ = nullptr;
		texW_ = texH_ = 0;
	}
}

gs_texture_t *MotionPlayer::prepare(const std::vector<DrawOp> &ops, const Rect &box, double t)
{
	if (!enabled_) {
		releaseGraphics();
		return nullptr;
	}
	// Only copy frames while a video is loaded for the shown track. The scene asks for
	// the video op only after `frame().ready`, which needs a first copy, so capture
	// whenever something is loaded, and draw only when the op is present.
	bool loaded;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		loaded = child_ && !loadedIdentity_.empty();
	}
	if (!loaded) {
		haveTexture_ = false;
		captureReportStart_ = -1.0;
		capturedFramesInWindow_ = 0;
		return nullptr;
	}
	const bool wanted = std::any_of(ops.begin(), ops.end(), [](const DrawOp &op) { return op.kind == DrawOp::Kind::Video; });
	obs_source_t *child = childRef();
	if (!child)
		return nullptr;
	// OBS provides already sRGB-encoded RGB for ordinary 8-bit SDR media.
	// Its high-precision SDR format is linear and needs one transfer to match
	// Tonearm's sRGB layers. Inspect actual decoder output rather than assuming
	// HEVC = linear and H.264 = encoded; codecs can have different bit depths.
	const auto videoColorSpace = obs_source_get_color_space(child, 0, nullptr);
	const bool previousTransfer = videoNeedsSrgbEncode_;
	videoNeedsSrgbEncode_ = videoColorSpace == GS_CS_SRGB_16F;
	const uint64_t colorGeneration = loadGeneration_.load();
	if (colorGeneration != loggedTransferGeneration_ || previousTransfer != videoNeedsSrgbEncode_) {
		blog(LOG_INFO, "[tonearm] motion video colorspace=%d; sRGB-encode=%s",
		     static_cast<int>(videoColorSpace), videoNeedsSrgbEncode_ ? "yes" : "no");
		loggedTransferGeneration_ = colorGeneration;
	}
	const uint32_t vw = obs_source_get_width(child), vh = obs_source_get_height(child);
	if (vw == 0 || vh == 0 || box.empty()) {
		obs_source_release(child);
		haveTexture_ = false;
		return nullptr;
	}

	// Preserve native decoded detail until the final compositor draw. Capturing
	// directly at the small overlay box size used to throw away most of the
	// Apple artwork pixels, which became visibly soft when OBS scaled the card.
	// Cap the GPU capture to avoid excessive bandwidth on slower computers;
	// this is a *downscale-only* cap, never an artificial upscale.
	const CropRect crop = coverCrop(static_cast<int>(vw), static_cast<int>(vh), box.w, box.h);
	// A 2160-pixel GPU copy every frame is wasteful for a 200-400-pixel
	// OBS overlay, even when the source video itself is 2160p. Preserve
	// excellent detail by supersampling at 3x the displayed artwork size,
	// while bounding the GPU pass to 1440p (Maximum) or 1080p (Balanced).
	// This leaves the original high-bitrate decoded Apple video untouched.
	const bool maximum = effectiveQuality_.load() == MotionQuality::Maximum;
	const float requested = std::max(box.w, box.h) * (maximum ? 3.0f : 2.5f);
	const float captureLimit = std::clamp(requested, maximum ? 768.0f : 576.0f,
					     maximum ? 1440.0f : 1080.0f);
	const float captureScale = std::min(1.0f, captureLimit / std::max(crop.w, crop.h));
	const uint32_t tw = std::max(1u, static_cast<uint32_t>(std::lround(crop.w * captureScale)));
	const uint32_t th = std::max(1u, static_cast<uint32_t>(std::lround(crop.h * captureScale)));
	const bool sizeChanged = tw != texW_ || th != texH_;
	const uint64_t gen = loadGeneration_.load();
	if (gen != capturedGeneration_) {
		haveTexture_ = false; // a different video was loaded: never show the old frame
		captureReportStart_ = -1.0;
		capturedFramesInWindow_ = 0;
	}
	// Before the first frame is shown, capture once so the scene can start fading in;
	// after that, copy only while the video is drawn, up to 60 times a second.
	// A paused decoder keeps presenting the same frame, so once that frame has been
	// copied at the current size there is nothing new to capture until playback resumes,
	// the size changes or another video loads (the generation check above).
	const bool pausedFrameHeld = haveTexture_ && !sizeChanged && gen == capturedGeneration_ &&
				     capturedWhilePaused_ && obs_source_media_get_state(child) == OBS_MEDIA_STATE_PAUSED;
	const bool due = !haveTexture_ ||
			 (wanted && !pausedFrameHeld &&
			  (sizeChanged || shouldCaptureMotionFrame(t, lastCapture_, kMaxCaptureFps) || t < lastCapture_));
	if (due) {
		if (!texrender_)
			texrender_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_texrender_reset(texrender_);
		if (texrender_ && gs_texrender_begin(texrender_, tw, th)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(crop.x, crop.x + crop.w, crop.y, crop.y + crop.h, -100.0f, 100.0f);
			// Explicitly keep the intermediate in the same non-sRGB-write
			// mode as Tonearm's final compositor. Otherwise OBS's inherited
			// framebuffer state can encode an already-encoded 8-bit source.
			const bool previousSrgb = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(false);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			obs_source_video_render(child);
			gs_blend_state_pop();
			gs_enable_framebuffer_srgb(previousSrgb);
			gs_texrender_end(texrender_);
			texW_ = tw;
			texH_ = th;
			lastCapture_ = t;
			haveTexture_ = true;
			capturedGeneration_ = gen;
			capturedWhilePaused_ = obs_source_media_get_state(child) == OBS_MEDIA_STATE_PAUSED;
			if (wanted) {
				if (captureReportStart_ < 0.0)
					captureReportStart_ = t;
				++capturedFramesInWindow_;
				const double window = t - captureReportStart_;
				if (window >= 10.0) {
					blog(LOG_INFO, "[tonearm] animation capture performance: %.1f fps, %ux%u px",
					     static_cast<double>(capturedFramesInWindow_) / window, tw, th);
					captureReportStart_ = t;
					capturedFramesInWindow_ = 0;
				}
			}
		}
	}
	obs_source_release(child);
	return wanted && haveTexture_ ? gs_texrender_get_texture(texrender_) : nullptr;
}

} // namespace tonearm
