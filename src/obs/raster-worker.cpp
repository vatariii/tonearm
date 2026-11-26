// SPDX-License-Identifier: GPL-2.0-or-later
#include "raster-worker.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <filesystem>

namespace tonearm {

namespace {
std::filesystem::path pathFromUtf8(const std::string &s)
{
	return std::filesystem::path(reinterpret_cast<const char8_t *>(s.c_str()));
}
std::string utf8(const std::filesystem::path &p)
{
	const auto u = p.u8string();
	return std::string(u.begin(), u.end());
}
} // namespace

RasterWorker::RasterWorker(std::string fontDir) : fontDir_(std::move(fontDir))
{
	thread_ = std::thread([this] { run(); });
}

RasterWorker::~RasterWorker()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
}

void RasterWorker::submit(const LayerRequest &req)
{
	lastLayerKey_ = req.key;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pendingLayer_ = req;
	}
	cv_.notify_one();
}

void RasterWorker::submit(const TimeRequest &req)
{
	lastTimeKey_ = req.key;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pendingTime_ = req;
	}
	cv_.notify_one();
}

std::optional<RasterWorker::LayerResult> RasterWorker::takeLayers()
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto r = std::move(doneLayer_);
	doneLayer_.reset();
	return r;
}

std::optional<RasterWorker::TimeResult> RasterWorker::takeTimes()
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto r = std::move(doneTime_);
	doneTime_.reset();
	return r;
}

void RasterWorker::run()
{
	os_set_thread_name("tonearm-raster");
	std::unique_ptr<TextEngine> text = createTextEngine();
	{
		std::error_code ec;
		std::vector<std::string> fonts;
		for (const auto &e : std::filesystem::directory_iterator(pathFromUtf8(fontDir_), ec)) {
			const auto ext = e.path().extension().string();
			if (ext == ".otf" || ext == ".ttf")
				fonts.push_back(utf8(e.path()));
		}
		std::sort(fonts.begin(), fonts.end()); // same registration order on every machine
		for (const std::string &f : text->addFontFiles(fonts))
			blog(LOG_WARNING, "[tonearm] could not load font %s", f.c_str());
	}

	for (;;) {
		std::optional<LayerRequest> layer;
		std::optional<TimeRequest> time;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			cv_.wait(lock, [&] { return stop_ || pendingLayer_ || pendingTime_; });
			if (stop_)
				return;
			layer = std::move(pendingLayer_);
			pendingLayer_.reset();
			time = std::move(pendingTime_);
			pendingTime_.reset();
		}
		if (layer) {
			const uint64_t start = os_gettime_ns();
			auto set = rasterizeLayers(layer->style, layer->content, *text);
			blog(LOG_DEBUG, "[tonearm] rasterized layers in %.1f ms", (os_gettime_ns() - start) / 1e6);
			std::lock_guard<std::mutex> lock(mutex_);
			doneLayer_ = LayerResult{layer->key, std::move(set)};
		}
		if (time && time->set) {
			TimeLayers t = rasterizeTimes(time->style, *time->set, time->elapsed, time->remaining, *text);
			std::lock_guard<std::mutex> lock(mutex_);
			doneTime_ = TimeResult{time->key, std::move(t)};
		}
	}
}

} // namespace tonearm
