// SPDX-License-Identifier: GPL-2.0-or-later
#include "check.hpp"
#include "rig.hpp"

using namespace tonearm;

namespace {
Style glass()
{
	return makePreset("glass", LayoutMode::ExpandedHorizontal);
}
} // namespace

TEST("scene: nothing is drawn while idle with the default hide behaviour")
{
	Rig r(glass());
	r.hasSession = false;
	r.run(0, 1);
	CHECK(r.ops.empty());
	CHECK(!r.scene.visible());
}

TEST("scene: a playing track fades in and settles")
{
	Rig r(glass());
	r.step(0);
	const float first = r.totalOpacity();
	r.run(0, 1.5);
	CHECK(r.scene.visible());
	CHECK(!r.ops.empty());
	CHECK(r.totalOpacity() > first);
	CHECK(r.layerRasters == 1);
	// Static layers are rasterized once, not per frame.
	r.run(1.5, 3);
	CHECK(r.layerRasters == 1);
}

TEST("scene: a track change crossfades between both decks and then drops the old one")
{
	Rig r(glass());
	r.run(0, 2);
	const auto before = r.imageIds();
	r.setTrack(1, PlaybackStatus::Playing, 0, 2.0);
	r.run(2.0, 2.2);
	const auto during = r.imageIds();
	size_t shared = 0;
	for (auto id : before)
		shared += during.count(id);
	CHECK_MSG(shared > 0, "outgoing layers should still be drawn mid-transition");
	CHECK_MSG(during.size() > shared, "incoming layers should already be drawn mid-transition");
	r.run(2.2, 4.5);
	const auto after = r.imageIds();
	for (auto id : before)
		CHECK_MSG(!after.count(id) || id == 0, "old deck should be gone once the transition ends");
	CHECK(r.layerRasters == 2);
}

TEST("scene: pausing dims the card and resuming restores it")
{
	Rig r(glass());
	r.run(0, 2);
	const float playing = r.totalOpacity();
	r.setTimeline(PlaybackStatus::Paused, 32, 2.0, demoDuration(0));
	r.run(2.0, 4);
	const float paused = r.totalOpacity();
	CHECK(paused < playing * 0.95f);
	CHECK(paused > playing * 0.4f);
	r.setTimeline(PlaybackStatus::Playing, 32, 4.0, demoDuration(0));
	r.run(4.0, 6);
	CHECK_NEAR(r.totalOpacity(), playing, playing * 0.02);
}

TEST("scene: hide-after-delay removes a paused card")
{
	Style s = glass();
	s.paused = PausedBehavior::HideAfterDelay;
	s.hideAfterPausedSec = 1.0f;
	Rig r(s);
	r.run(0, 1);
	r.setTimeline(PlaybackStatus::Paused, 31, 1.0, demoDuration(0));
	r.run(1.0, 1.8);
	CHECK(!r.ops.empty());
	r.run(1.8, 3.5);
	CHECK(r.ops.empty());
}

TEST("scene: a stopped player hides the card")
{
	Rig r(glass());
	r.run(0, 1);
	r.session.status = PlaybackStatus::Stopped;
	r.run(1, 2.5);
	CHECK(r.ops.empty());
}

TEST("scene: the idle placeholder appears when configured")
{
	Style s = glass();
	s.idle = IdleBehavior::ShowPlaceholder;
	Rig r(s);
	r.hasSession = false;
	r.run(0, 1.5);
	CHECK(!r.ops.empty());
	CHECK(r.progressFill() < 0); // no timeline for a placeholder
}

TEST("scene: progress follows the clock while playing and holds while paused")
{
	Rig r(glass());
	r.run(0, 1);
	const float a = r.progressFill();
	r.run(1, 3);
	const float b = r.progressFill();
	CHECK(a > 0);
	CHECK_NEAR(b - a, 2.0 / demoDuration(0), 0.004);
	r.setTimeline(PlaybackStatus::Paused, 33, 3.0, demoDuration(0));
	r.run(3, 5);
	const float c = r.progressFill();
	r.run(5, 6);
	CHECK_NEAR(r.progressFill(), c, 1e-4);
}

TEST("scene: seeking glides instead of snapping, unless motion is reduced")
{
	Rig r(glass());
	r.run(0, 1);
	const double d = demoDuration(0);
	r.setTimeline(PlaybackStatus::Playing, d * 0.8, 1.0, d);
	r.step(1.0 + 1.0 / 60);
	const float early = r.progressFill();
	CHECK(early < 0.75f);
	r.run(1.0, 1.8);
	CHECK_NEAR(r.progressFill(), 0.8 + 0.8 / d, 0.01);

	Style s = glass();
	s.reducedMotion = true;
	Rig q(s);
	q.run(0, 1);
	q.setTimeline(PlaybackStatus::Playing, d * 0.8, 1.0, d);
	q.step(1.0 + 1.0 / 60);
	CHECK_NEAR(q.progressFill(), 0.8, 0.01);
}

TEST("scene: unknown durations hide progress rather than inventing it")
{
	Rig r(glass());
	r.session.timeline.known = false;
	r.session.timeline.durationSec = 0;
	r.run(0, 1.5);
	CHECK(!r.ops.empty());
	const float f = r.progressFill();
	CHECK(f <= 0.0001f);
}

TEST("scene: changing the style re-rasterizes once")
{
	Rig r(glass());
	r.run(0, 1);
	r.scene.setStyle(makePreset("neon", LayoutMode::ExpandedHorizontal));
	r.run(1, 2);
	CHECK(r.layerRasters == 2);
	CHECK(!r.ops.empty());
}


TEST("scene: Turntable spins only during playback and lifts the stylus on pause")
{
	Rig r(makePreset("turntable", LayoutMode::ExpandedHorizontal));
	r.run(0, 1.5);
	const auto set = r.scene.currentLayers();
	CHECK(set && set->record.valid() && set->arm.valid() && set->platter.valid() && set->sheen.valid());
	auto find = [&](uint64_t imageId) -> const DrawOp * {
		for (const auto &op : r.ops)
			if (op.kind == DrawOp::Kind::Image && op.imageId == imageId)
				return &op;
		return nullptr;
	};
	const DrawOp *playingRecord = find(set->record.id);
	const DrawOp *playingArm = find(set->arm.id);
	CHECK(playingRecord != nullptr);
	// Playing: the stylus sits in the grooves, between the lead-in and the run-out.
	CHECK(playingArm != nullptr && playingArm->rotation > set->layout.armOuter * 0.9f &&
	      playingArm->rotation < set->layout.armInner);
	const float before = playingRecord ? playingRecord->rotation : -1;
	r.run(1.5, 1.7);
	playingRecord = find(set->record.id);
	CHECK(playingRecord != nullptr && std::fabs(playingRecord->rotation - before) > 0.1f);
	// The static light and platter never rotate.
	const DrawOp *sheen = find(set->sheen.id);
	CHECK(sheen != nullptr && sheen->rotation == 0.0f);

	r.setTimeline(PlaybackStatus::Paused, 35, 1.7, demoDuration(0));
	r.run(1.7, 4.5);
	const DrawOp *pausedRecord = find(set->record.id);
	const DrawOp *pausedArm = find(set->arm.id);
	CHECK(pausedRecord != nullptr);
	CHECK(pausedArm != nullptr && std::fabs(pausedArm->rotation) < 0.02f); // back on its rest
	CHECK(pausedRecord != nullptr && pausedRecord->opacity > 0.99f);         // never a see-through ghost
	const float held = pausedRecord ? pausedRecord->rotation : 0.0f;
	r.run(4.5, 4.8);
	pausedRecord = find(set->record.id);
	CHECK(pausedRecord != nullptr);
	if (pausedRecord) CHECK_NEAR(pausedRecord->rotation, held, 0.001f);

	Style reduced = makePreset("turntable", LayoutMode::CompactHorizontal);
	reduced.reducedMotion = true;
	Rig calm(reduced);
	calm.run(0, 1);
	const auto noMotion = calm.scene.currentLayers();
	bool fixed = false;
	for (const auto &op : calm.ops)
		if (op.imageId == noMotion->record.id && std::fabs(op.rotation) < 0.001f)
			fixed = true;
	CHECK(fixed);
}

TEST("scene: Turntable arm tracks inward as the song plays")
{
	Rig early(makePreset("turntable", LayoutMode::Square));
	early.run(0, 2);
	Rig late(makePreset("turntable", LayoutMode::Square));
	late.setTimeline(PlaybackStatus::Playing, demoDuration(0) - 10, 0, demoDuration(0));
	late.run(0, 2);
	auto armRotation = [](Rig &r) {
		const auto set = r.scene.currentLayers();
		for (const auto &op : r.ops)
			if (set && op.imageId == set->arm.id)
				return op.rotation;
		return -1.0f;
	};
	CHECK(armRotation(late) > armRotation(early) + 0.15f);
}

TEST("scene: Turntable can animate the sleeve without animating the pressing")
{
	Style s = makePreset("turntable", LayoutMode::ExpandedHorizontal);
	s.artworkMode = ArtworkMode::AnimatedWhenAvailable;
	Rig r(s);
	r.run(0, 1);
	r.motion = {r.session.track.identity(), true};
	r.run(1, 2);
	const auto set = r.scene.currentLayers();
	CHECK(set && set->videoMask.valid);
	CHECK(set->videoMask.box.x == set->layout.art.x);
	int videoIndex = -1, recordIndex = -1;
	for (size_t i = 0; i < r.ops.size(); ++i) {
		if (r.ops[i].kind == DrawOp::Kind::Video) videoIndex = static_cast<int>(i);
		if (r.ops[i].imageId == set->record.id) recordIndex = static_cast<int>(i);
	}
	CHECK(videoIndex >= 0 && recordIndex > videoIndex);
}

TEST("scene: every preset and layout produces a frame for every demo track")
{
	for (const auto &p : presetList())
		for (LayoutMode m : {LayoutMode::CompactHorizontal, LayoutMode::ExpandedHorizontal, LayoutMode::Square, LayoutMode::Vertical}) {
			Rig r(makePreset(p.id, m));
			for (int i = 0; i < demoTrackCount(); ++i) {
				r.setTrack(i, PlaybackStatus::Playing, 10, i * 2.0, i % 2 == 0);
				r.run(i * 2.0, i * 2.0 + 1.9);
				CHECK_MSG(!r.ops.empty(), std::string(p.id) + "/" + layoutId(m) + " track " + std::to_string(i));
			}
		}
}
