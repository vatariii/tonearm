// SPDX-License-Identifier: GPL-2.0-or-later
#include "tonearm-source.hpp"

#include "core/demo.hpp"
#include "gpu-compositor.hpp"
#include "media-service.hpp"
#include "motion-player.hpp"
#include "raster-worker.hpp"
#include "settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define T_(s) obs_module_text(s)

namespace tonearm {

namespace {

std::string moduleFile(const char *rel)
{
	char *path = obs_module_file(rel);
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

struct TonearmSource {
	obs_source_t *source = nullptr;

	// Written by the UI thread in update(), consumed on the graphics thread.
	std::mutex pendingMutex;
	bool hasPending = false;
	Style pendingStyle;
	SelectionPolicy pendingPolicy;
	std::vector<TrackInfo> sampleTracks; // guarded by pendingMutex

	// Graphics thread state
	Style style;
	SelectionPolicy policy;
	std::unique_ptr<Scene> scene;
	std::unique_ptr<RasterWorker> worker;
	GpuCompositor gpu;
	MotionPlayer motion; // animated artwork; inert unless enabled
	bool gpuTried = false;
	std::vector<DrawOp> ops;
	double t = 0;
	std::string selectedAppId;
	MediaSession sample;
	int sampleIndex = -1;
	double sampleStartedAt = 0;

	std::atomic<uint32_t> width{560}, height{168};
	PropertiesContext props;

	explicit TonearmSource(obs_source_t *src, obs_data_t *settings) : source(src)
	{
		props.source = src;
		migrateSettings(settings); // older or hand-edited scene collections, before anything reads them
		style = styleFromSettings(settings);
		policy = policyFromSettings(settings);
		width = static_cast<uint32_t>(style.width);
		height = static_cast<uint32_t>(style.height);
		scene = std::make_unique<Scene>(style);
		worker = std::make_unique<RasterWorker>(moduleFile("fonts"));
		MediaService::instance().acquire();
		update(settings);
	}

	~TonearmSource()
	{
		worker.reset(); // joins the raster thread before GPU teardown
		MediaService::instance().release();
	}

	void update(obs_data_t *settings)
	{
		syncPreset(settings);
		Style s = styleFromSettings(settings);
		SelectionPolicy p = policyFromSettings(settings);
		width = static_cast<uint32_t>(s.width);
		height = static_cast<uint32_t>(s.height);
		motion.configure(s); // UI thread: creates or tears down the animated artwork player
		std::lock_guard<std::mutex> lock(pendingMutex);
		if (s.sampleWhenIdle && sampleTracks.empty()) {
			// Generated here (UI thread) so the graphics thread never pays for it.
			for (int i = 0; i < demoTrackCount(); ++i)
				sampleTracks.push_back(demoTrack(i, 384, true));
		} else if (!s.sampleWhenIdle && !sampleTracks.empty()) {
			sampleTracks.clear(); // about 3 MB of sample artwork nobody will see
			sampleTracks.shrink_to_fit();
		}
		pendingStyle = std::move(s);
		pendingPolicy = std::move(p);
		hasPending = true;
	}

	MotionShape motionShape() const
	{
		const auto &layers = scene->currentLayers();
		const Rect box = layers ? layers->videoMask.box : Rect{};
		return preferredShape(style.motionOrientation, box.w, box.h);
	}

	const MediaSession *sampleSession(Clock::time_point now)
	{
		std::lock_guard<std::mutex> lock(pendingMutex);
		if (sampleTracks.empty())
			return nullptr;
		// Rotate through the sample tracks so transitions can be previewed.
		constexpr double kSampleSeconds = 12.0;
		if (sampleIndex < 0) {
			sampleIndex = 0;
			sampleStartedAt = t;
		} else if (t - sampleStartedAt > kSampleSeconds) {
			sampleIndex = (sampleIndex + 1) % static_cast<int>(sampleTracks.size());
			sampleStartedAt = t;
		}
		sample.appId = "Tonearm.Sample";
		sample.appName = T_("SampleSource");
		sample.status = PlaybackStatus::Playing;
		sample.track = sampleTracks[static_cast<size_t>(sampleIndex)];
		sample.timeline.known = true;
		sample.timeline.durationSec = demoDuration(sampleIndex);
		sample.timeline.positionSec = 42.0 + (t - sampleStartedAt);
		sample.timeline.sampledAt = now;
		return &sample;
	}

	void tick(float seconds)
	{
		t += seconds;
		{
			std::lock_guard<std::mutex> lock(pendingMutex);
			if (hasPending) {
				style = pendingStyle;
				policy = pendingPolicy;
				scene->setStyle(style);
				hasPending = false;
			}
		}

		// Nothing to do while the source is not visible anywhere (keeps CPU at zero in game scenes
		// that don't include the overlay). Media snapshots are still kept fresh by the provider.
		const bool showing = obs_source_showing(source);
		if (!showing && !scene->visible()) {
			motion.tick(nullptr, false, MotionShape::Square); // pauses decoding while hidden
			return;
		}

		const auto snap = MediaService::instance().snapshot();
		const Clock::time_point now = Clock::now();
		const int idx = selectSession(*snap, policy, selectedAppId);
		const MediaSession *session = idx >= 0 ? &snap->sessions[static_cast<size_t>(idx)] : nullptr;
		if (session)
			selectedAppId = session->appId;
		else if (style.sampleWhenIdle)
			session = sampleSession(now);
		if (!style.sampleWhenIdle)
			sampleIndex = -1;

		// Animated artwork follows real players only, never the sample track.
		const bool real = idx >= 0;
		const auto &motionLayers = scene->currentLayers();
		const Rect motionBox = motionLayers ? motionLayers->videoMask.box : Rect{};
		motion.tick(real ? session : nullptr, showing, motionShape(), std::max(motionBox.w, motionBox.h));

		SceneInput in;
		in.provider = snap->state;
		in.session = session;
		in.now = now;
		in.motion = motion.frame();

		if (auto r = worker->takeLayers())
			scene->provideLayers(r->key, std::move(r->set));
		if (auto r = worker->takeTimes())
			scene->provideTimes(r->key, std::move(r->times));

		scene->update(t, in);

		if (auto req = scene->layerRequest(); req && req->key != worker->lastSubmittedLayerKey())
			worker->submit(*req);
		if (auto req = scene->timeRequest(); req && req->key != worker->lastSubmittedTimeKey())
			worker->submit(*req);
	}

	void render()
	{
		if (!gpuTried) {
			gpuTried = true;
			gpu.init(moduleFile("effects/tonearm.effect").c_str());
		}
		scene->buildFrame(t, ops);
		gs_texture_t *video = nullptr;
		if (motion.enabled()) {
			const auto &layers = scene->currentLayers();
			video = motion.prepare(ops, layers ? layers->videoMask.box : Rect{}, t);
		} else {
			motion.releaseGraphics(); // frees the capture target after animated artwork is switched off
		}
		if (!ops.empty())
			gpu.render(ops, video, motion.videoNeedsSrgbEncode());
	}

	obs_properties_t *properties()
	{
		props.detectedSummary = detectedPlayersSummary();
		return buildProperties(&props);
	}
};

const char *getName(void *)
{
	return T_("SourceName");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	return new TonearmSource(source, settings);
}

void destroy(void *data)
{
	delete static_cast<TonearmSource *>(data);
}

void update(void *data, obs_data_t *settings)
{
	static_cast<TonearmSource *>(data)->update(settings);
}

void tick(void *data, float seconds)
{
	static_cast<TonearmSource *>(data)->tick(seconds);
}

void render(void *data, gs_effect_t *)
{
	static_cast<TonearmSource *>(data)->render();
}

uint32_t getWidth(void *data)
{
	return static_cast<TonearmSource *>(data)->width;
}

uint32_t getHeight(void *data)
{
	return static_cast<TonearmSource *>(data)->height;
}

obs_properties_t *getProperties(void *data)
{
	if (!data) {
		static PropertiesContext empty;
		return buildProperties(&empty);
	}
	return static_cast<TonearmSource *>(data)->properties();
}

void getDefaults(obs_data_t *settings)
{
	settingsDefaults(settings);
}

} // namespace

void registerTonearmSource()
{
	obs_source_info info = {};
	info.id = "tonearm_now_playing";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = getName;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.video_tick = tick;
	info.video_render = render;
	info.get_width = getWidth;
	info.get_height = getHeight;
	info.get_properties = getProperties;
	info.get_defaults = getDefaults;
	obs_register_source(&info);
}

} // namespace tonearm
