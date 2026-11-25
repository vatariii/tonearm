// SPDX-License-Identifier: GPL-2.0-or-later
// tonearm-render: drives the real Scene, layer rasterizer and the CPU reference
// compositor headlessly. Used for documentation images and visual checks.
#include "core/compositor_cpu.hpp"
#include "core/demo.hpp"
#include "core/scene.hpp"
#include "media/provider.hpp"
#include "png.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>
#include <map>
#include <string>

using namespace tonearm;

namespace {

struct Args {
	std::string mode = "still";
	std::string preset = "glass";
	std::string layout = "expanded";
	std::string out = "out.png";
	std::string outdir = "frames";
	std::string bg = "stage";
	int track = 0, toTrack = 1;
	double pos = 74;
	std::string status = "playing";
	float scale = 1.0f;
	int fps = 30;
	double seconds = 2.4;
	double at = -1;
	int margin = 40;
	bool noArt = false;
	bool reduced = false;
	int width = 0, height = 0;
	std::string tracks; // manifest of real tracks (see loadManifest)
};

Args parse(int argc, char **argv)
{
	Args a;
	if (argc > 1)
		a.mode = argv[1];
	for (int i = 2; i < argc; ++i) {
		const std::string k = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (k == "--preset")
			a.preset = next();
		else if (k == "--layout")
			a.layout = next();
		else if (k == "--out")
			a.out = next();
		else if (k == "--outdir")
			a.outdir = next();
		else if (k == "--bg")
			a.bg = next();
		else if (k == "--track")
			a.track = std::atoi(next().c_str());
		else if (k == "--to")
			a.toTrack = std::atoi(next().c_str());
		else if (k == "--pos")
			a.pos = std::atof(next().c_str());
		else if (k == "--status")
			a.status = next();
		else if (k == "--scale")
			a.scale = static_cast<float>(std::atof(next().c_str()));
		else if (k == "--fps")
			a.fps = std::atoi(next().c_str());
		else if (k == "--seconds")
			a.seconds = std::atof(next().c_str());
		else if (k == "--at")
			a.at = std::atof(next().c_str());
		else if (k == "--margin")
			a.margin = std::atoi(next().c_str());
		else if (k == "--tracks")
			a.tracks = next();
		else if (k == "--no-art")
			a.noArt = true;
		else if (k == "--reduced-motion")
			a.reduced = true;
		else if (k == "--size") {
			const std::string v = next();
			std::sscanf(v.c_str(), "%dx%d", &a.width, &a.height);
		}
	}
	return a;
}

void loadFonts(TextEngine &te)
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::path(TONEARM_DATA_DIR) / "fonts";
	if (!fs::exists(dir))
		return;
	for (const auto &e : fs::directory_iterator(dir)) {
		const auto ext = e.path().extension().string();
		if (ext == ".otf" || ext == ".ttf")
			te.addFontFile(e.path().string());
	}
}

Image stage(int w, int h, float scale)
{
	Image img(w, h);
	const float W = static_cast<float>(w), H = static_cast<float>(h);
	fillRoundRect(img, {0, 0, W, H}, 0, Paint::linear({0, 0}, {W * 0.4f, H}, {{0, Color::hex("#1b1a20")}, {1, Color::hex("#0c0c10")}}));
	fillRoundRect(img, {0, 0, W, H}, 0,
		      Paint::radial({W * 0.85f, H * 0.1f}, std::max(W, H) * 0.8f, {{0, Color::hex("#ffffff0d")}, {1, Color::hex("#ffffff00")}}));
	addNoise(img, 0.012f, 7);
	(void)scale;
	return img;
}

// Real tracks for documentation renders. The manifest is tab-separated, one track
// per line: cover.rgba, title, artist, album, duration in seconds. Each cover is
// "RGBA <w> <h>\n" followed by straight RGBA bytes (tools/media/covers.py writes them).
struct ManifestTrack {
	TrackInfo info;
	double duration = 200;
};

std::vector<ManifestTrack> loadManifest(const std::string &path)
{
	std::vector<ManifestTrack> out;
	std::ifstream in(path);
	const auto dir = std::filesystem::path(path).parent_path();
	std::string line;
	while (std::getline(in, line)) {
		std::vector<std::string> f;
		std::stringstream ss(line);
		std::string cell;
		while (std::getline(ss, cell, '\t'))
			f.push_back(cell);
		if (f.size() < 5)
			continue;
		ManifestTrack t;
		t.info.title = f[1];
		t.info.artist = f[2];
		t.info.album = f[3];
		t.info.albumArtist = f[2];
		t.duration = std::atof(f[4].c_str());
		std::ifstream img(dir / f[0], std::ios::binary);
		std::string magic;
		int w = 0, h = 0;
		img >> magic >> w >> h;
		img.get();
		if (magic == "RGBA" && w > 0 && h > 0) {
			std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
			img.read(reinterpret_cast<char *>(px.data()), static_cast<std::streamsize>(px.size()));
			t.info.artwork = std::make_shared<Image>(Image::fromRgbaStraight(w, h, px.data(), w * 4));
			t.info.artworkHash = std::hash<std::string>{}(f[0]);
		}
		out.push_back(std::move(t));
	}
	return out;
}

class ManifestProvider final : public MediaProvider {
public:
	std::vector<ManifestTrack> tracks;
	Clock::time_point now{};
	void start() override {}
	void stop() override {}
	void set(int index, PlaybackStatus status, double positionSec, bool withArtwork)
	{
		const auto &t = tracks[static_cast<size_t>(index) % tracks.size()];
		MediaSession s;
		s.appId = "AppleInc.AppleMusicWin_nzyj5cx40ttba!App";
		s.appName = "Apple Music";
		s.status = status;
		s.track = t.info;
		if (!withArtwork)
			s.track.artwork.reset();
		s.timeline.known = true;
		s.timeline.durationSec = t.duration;
		s.timeline.positionSec = positionSec;
		s.timeline.sampledAt = now;
		MediaSnapshot snap;
		snap.state = ProviderState::Ready;
		snap.systemCurrentAppId = s.appId;
		snap.sessions.push_back(std::move(s));
		publish(std::move(snap));
	}
};

struct Harness {
	Style style;
	Scene scene;
	DemoProvider provider;
	ManifestProvider real;
	std::unique_ptr<TextEngine> text;
	Clock::time_point base = Clock::now();
	bool noArt = false;

	explicit Harness(const Style &s) : style(s), scene(s), provider(0), text(createTextEngine()) { loadFonts(*text); }

	void setTrack(int idx, PlaybackStatus st, double pos, double t)
	{
		const auto now = base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(t));
		if (!real.tracks.empty()) {
			real.now = now;
			real.set(idx, st, pos, !noArt);
			return;
		}
		provider.setNow(now);
		provider.set(idx, st, pos, !noArt);
	}

	void step(double t)
	{
		auto snap = real.tracks.empty() ? provider.snapshot() : real.snapshot();
		SceneInput in;
		in.provider = snap->state;
		in.session = snap->sessions.empty() ? nullptr : &snap->sessions[0];
		in.now = base + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(t));
		scene.update(t, in);
		if (auto req = scene.layerRequest()) {
			scene.provideLayers(req->key, rasterizeLayers(req->style, req->content, *text));
			scene.update(t, in);
		}
		if (auto req = scene.timeRequest())
			scene.provideTimes(req->key, rasterizeTimes(req->style, *req->set, req->elapsed, req->remaining, *text));
	}

	Image frame(double t, int margin, const std::string &bg)
	{
		std::vector<DrawOp> ops;
		scene.buildFrame(t, ops);
		const int W = style.width + margin * 2, H = style.height + margin * 2;
		Image img = bg == "none" ? Image(W, H) : stage(W, H, 1);
		for (auto &op : ops) {
			op.dst = op.dst.translated(static_cast<float>(margin), static_cast<float>(margin));
			op.rect = op.rect.translated(static_cast<float>(margin), static_cast<float>(margin));
			op.clipRect = op.clipRect.translated(static_cast<float>(margin), static_cast<float>(margin));
		}
		executeOnCpu(img, ops);
		return img;
	}
};

PlaybackStatus statusFrom(const std::string &s)
{
	if (s == "paused")
		return PlaybackStatus::Paused;
	if (s == "stopped")
		return PlaybackStatus::Stopped;
	return PlaybackStatus::Playing;
}

} // namespace

int main(int argc, char **argv)
{
	const Args a = parse(argc, argv);
	Style style = makePreset(a.preset, layoutFromId(a.layout));
	if (a.width > 0 && a.height > 0) {
		style.width = a.width;
		style.height = a.height;
	}
	style.width = static_cast<int>(std::lround(style.width * a.scale));
	style.height = static_cast<int>(std::lround(style.height * a.scale));
	style.reducedMotion = a.reduced;
	const int margin = static_cast<int>(std::lround(a.margin * a.scale));

	Harness h(style);
	h.noArt = a.noArt;
	if (!a.tracks.empty()) {
		h.real.tracks = loadManifest(a.tracks);
		if (h.real.tracks.empty()) {
			std::fprintf(stderr, "no tracks in %s\n", a.tracks.c_str());
			return 1;
		}
	}
	const double dt = 1.0 / std::max(1, a.fps);

	if (a.mode == "still") {
		const double end = a.at >= 0 ? a.at : 1.6;
		h.setTrack(a.track, statusFrom(a.status), a.pos, 0);
		for (double t = 0; t <= end + 1e-9; t += dt)
			h.step(t);
		if (!writePng(h.frame(end, margin, a.bg), a.out)) {
			std::fprintf(stderr, "failed to write %s\n", a.out.c_str());
			return 1;
		}
		std::printf("%s\n", a.out.c_str());
		return 0;
	}

	if (a.mode == "transition") {
		// Shows track A, switches to track B at 1.0 s, records until `seconds`.
		std::filesystem::create_directories(a.outdir);
		h.setTrack(a.track, PlaybackStatus::Playing, a.pos, 0);
		int frame = 0;
		bool switched = false;
		for (double t = 0; t <= a.seconds + 1e-9; t += dt) {
			if (!switched && t >= 1.0) {
				h.setTrack(a.toTrack, PlaybackStatus::Playing, 2, t);
				switched = true;
			}
			h.step(t);
			if (t >= 0.6) {
				char name[64];
				std::snprintf(name, sizeof name, "/f%04d.png", frame++);
				writePng(h.frame(t, margin, a.bg), a.outdir + name);
			}
		}
		std::printf("%d frames\n", frame);
		return 0;
	}

	if (a.mode == "marquee") {
		std::filesystem::create_directories(a.outdir);
		h.setTrack(3, PlaybackStatus::Playing, a.pos, 0);
		int frame = 0;
		for (double t = 0; t <= a.seconds + 1e-9; t += dt) {
			h.step(t);
			if (t >= 0.8) {
				char name[64];
				std::snprintf(name, sizeof name, "/f%04d.png", frame++);
				writePng(h.frame(t, margin, a.bg), a.outdir + name);
			}
		}
		std::printf("%d frames\n", frame);
		return 0;
	}

	if (a.mode == "pause") {
		std::filesystem::create_directories(a.outdir);
		h.setTrack(a.track, PlaybackStatus::Playing, a.pos, 0);
		int frame = 0;
		bool paused = false, resumed = false;
		for (double t = 0; t <= a.seconds + 1e-9; t += dt) {
			if (!paused && t >= 1.0) {
				h.setTrack(a.track, PlaybackStatus::Paused, a.pos + 1.0, t);
				paused = true;
			}
			if (!resumed && t >= 2.2) {
				h.setTrack(a.track, PlaybackStatus::Playing, a.pos + 1.0, t);
				resumed = true;
			}
			h.step(t);
			if (t >= 0.6) {
				char name[64];
				std::snprintf(name, sizeof name, "/f%04d.png", frame++);
				writePng(h.frame(t, margin, a.bg), a.outdir + name);
			}
		}
		std::printf("%d frames\n", frame);
		return 0;
	}

	std::fprintf(stderr, "usage: tonearm-render still|transition|marquee|pause [--preset glass|minimal|neon|editorial] "
			     "[--layout compact|expanded|square|vertical] [--track N] [--to N] [--pos S] [--status playing|paused] "
			     "[--scale F] [--out file.png] [--outdir dir] [--bg stage|none] [--no-art] [--reduced-motion] [--tracks manifest.tsv]\n");
	return 2;
}
