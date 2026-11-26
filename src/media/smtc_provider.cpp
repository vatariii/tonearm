// SPDX-License-Identifier: GPL-2.0-or-later
// Windows System Media Transport Controls provider.
//
// Design notes
// - Everything WinRT happens on one dedicated MTA thread. OBS threads only read
//   immutable snapshots, so a slow or hung media app can never stall rendering.
// - Updates are event driven (SessionsChanged, MediaPropertiesChanged,
//   PlaybackInfoChanged, TimelinePropertiesChanged). Handlers only set flags;
//   the provider thread coalesces bursts (Apple Music and Spotify often fire
//   several events per track change) and then queries once.
// - A slow safety refresh (every few seconds) covers players that forget to
//   raise events. Metadata and artwork are only re-read on property events.
// - Timeline position is extrapolated in the core from LastUpdatedTime, because
//   players only push timeline updates on seeks and state changes.
#include "provider.hpp"
#include "wic_decode.hpp"

#include <windows.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace tonearm {

namespace {

namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
using GSession = wmc::GlobalSystemMediaTransportControlsSession;
using GManager = wmc::GlobalSystemMediaTransportControlsSessionManager;

// Shared with event handlers so a late callback never touches a destroyed provider.
struct Signal {
	std::mutex mutex;
	std::condition_variable cv;
	bool sessionsDirty = true;
	std::set<std::string> mediaDirty;    // app ids needing metadata/artwork refresh
	std::set<std::string> stateDirty;    // app ids needing playback/timeline refresh
	bool stop = false;

	void markSessions()
	{
		{
			std::lock_guard<std::mutex> l(mutex);
			sessionsDirty = true;
		}
		cv.notify_one();
	}
	void markMedia(const std::string &id)
	{
		{
			std::lock_guard<std::mutex> l(mutex);
			mediaDirty.insert(id);
		}
		cv.notify_one();
	}
	void markState(const std::string &id)
	{
		{
			std::lock_guard<std::mutex> l(mutex);
			stateDirty.insert(id);
		}
		cv.notify_one();
	}
};

struct Entry {
	GSession session{nullptr};
	winrt::event_token mediaToken{}, playbackToken{}, timelineToken{};
	MediaSession data;
	std::string artKey; // identity the current artwork belongs to
};

PlaybackStatus mapStatus(wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus s)
{
	using S = wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
	switch (s) {
	case S::Playing:
		return PlaybackStatus::Playing;
	case S::Paused:
		return PlaybackStatus::Paused;
	case S::Stopped:
	case S::Closed:
		return PlaybackStatus::Stopped;
	default:
		return PlaybackStatus::Unknown;
	}
}

// Waits for a WinRT async operation with a deadline. A media app that stops
// responding must not be able to hold this thread, or OBS shutdown would wait on it.
template <typename Op> auto awaitWithin(const Op &op, std::chrono::milliseconds limit)
{
	if (op.wait_for(limit) != winrt::Windows::Foundation::AsyncStatus::Completed) {
		try {
			op.Cancel();
		} catch (...) {
		}
		throw winrt::hresult_error(E_ABORT);
	}
	return op.GetResults();
}

constexpr std::chrono::milliseconds kQueryLimit{2000};

std::vector<uint8_t> readStream(const wss::IRandomAccessStreamReference &ref)
{
	std::vector<uint8_t> bytes;
	if (!ref)
		return bytes;
	auto stream = awaitWithin(ref.OpenReadAsync(), kQueryLimit);
	const uint64_t size = stream.Size();
	if (size == 0 || size > 32ull * 1024 * 1024)
		return bytes;
	wss::DataReader reader(stream);
	const uint32_t loaded = awaitWithin(reader.LoadAsync(static_cast<uint32_t>(size)), kQueryLimit);
	bytes.resize(loaded);
	if (loaded)
		reader.ReadBytes(winrt::array_view<uint8_t>(bytes.data(), bytes.data() + loaded));
	reader.DetachStream();
	stream.Close();
	return bytes;
}

class SmtcProvider final : public MediaProvider {
public:
	SmtcProvider() : signal_(std::make_shared<Signal>()) {}
	~SmtcProvider() override { stop(); }

	void start() override
	{
		if (thread_.joinable())
			return;
		thread_ = std::thread([this] { run(); });
	}

	void stop() override
	{
		{
			std::lock_guard<std::mutex> l(signal_->mutex);
			signal_->stop = true;
		}
		signal_->cv.notify_all();
		if (thread_.joinable())
			thread_.join();
	}

private:
	void run()
	{
		SetThreadDescription(GetCurrentThread(), L"tonearm-smtc");
		bool ownsApartment = false;
		try {
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
			ownsApartment = true;
		} catch (...) {
			// COM may already be initialized on this thread; that is fine.
		}
		runLoop();
		if (ownsApartment)
			winrt::uninit_apartment();
	}

	void runLoop()
	{

		GManager manager{nullptr};
		try {
			manager = awaitWithin(GManager::RequestAsync(), std::chrono::milliseconds(5000));
		} catch (const winrt::hresult_error &e) {
			OutputDebugStringW((L"[tonearm] SMTC unavailable: " + std::wstring(e.message())).c_str());
		} catch (...) {
		}
		if (!manager) {
			MediaSnapshot snap;
			snap.state = ProviderState::Unavailable;
			publish(std::move(snap));
			waitForStop();
			return;
		}

		std::weak_ptr<Signal> weak = signal_;
		auto sessionsToken = manager.SessionsChanged([weak](auto &&, auto &&) {
			if (auto s = weak.lock())
				s->markSessions();
		});
		auto currentToken = manager.CurrentSessionChanged([weak](auto &&, auto &&) {
			if (auto s = weak.lock())
				s->markSessions();
		});

		auto lastSafety = std::chrono::steady_clock::now();
		for (;;) {
			bool sessionsDirty = false;
			std::set<std::string> media, state;
			{
				std::unique_lock<std::mutex> l(signal_->mutex);
				auto dirty = [&] {
					return signal_->sessionsDirty || !signal_->mediaDirty.empty() || !signal_->stateDirty.empty();
				};
				signal_->cv.wait_for(l, std::chrono::seconds(1), [&] { return signal_->stop || dirty(); });
				if (signal_->stop)
					break;
				// Coalesce a burst of events into one query round. Only when there is
				// something to coalesce, and stop() still ends the wait at once. The 1 s
				// timeout above keeps driving the periodic safety refresh below.
				if (dirty())
					signal_->cv.wait_for(l, std::chrono::milliseconds(60), [&] { return signal_->stop; });
				if (signal_->stop)
					break;
			}
			{
				std::lock_guard<std::mutex> l(signal_->mutex);
				if (signal_->stop)
					break;
				sessionsDirty = signal_->sessionsDirty;
				signal_->sessionsDirty = false;
				media.swap(signal_->mediaDirty);
				state.swap(signal_->stateDirty);
			}

			const auto now = std::chrono::steady_clock::now();
			const bool safety = now - lastSafety > std::chrono::seconds(2);
			if (!sessionsDirty && media.empty() && state.empty() && !safety)
				continue;
			if (safety)
				lastSafety = now;

			if (sessionsDirty)
				syncSessions(manager, media);
			for (auto &[id, entry] : entries_) {
				// Some versions of Apple Music update the Windows media flyout
				// without reliably raising MediaPropertiesChanged. Periodically
				// recheck lightweight text metadata even without an event.
				// The poll path avoids re-decoding artwork when the track is unchanged.
				if (media.count(id) || safety)
					refreshMedia(entry, !media.count(id));
				if (media.count(id) || state.count(id) || safety)
					refreshState(entry);
			}
			publishSnapshot(manager);
		}

		for (auto &kv : entries_)
			unsubscribe(kv.second);
		entries_.clear();
		try {
			manager.SessionsChanged(sessionsToken);
			manager.CurrentSessionChanged(currentToken);
		} catch (...) {
		}
	}

	void waitForStop()
	{
		std::unique_lock<std::mutex> l(signal_->mutex);
		signal_->cv.wait(l, [&] { return signal_->stop; });
	}

	void syncSessions(const GManager &manager, std::set<std::string> &media)
	{
		std::map<std::string, GSession> live;
		try {
			for (const auto &s : manager.GetSessions())
				live.emplace(winrt::to_string(s.SourceAppUserModelId()), s);
		} catch (...) {
			return;
		}
		for (auto it = entries_.begin(); it != entries_.end();) {
			if (!live.count(it->first)) {
				unsubscribe(it->second);
				it = entries_.erase(it);
			} else {
				++it;
			}
		}
		for (auto &[id, session] : live) {
			if (entries_.count(id))
				continue;
			Entry e;
			e.session = session;
			e.data.appId = id;
			e.data.appName = friendlyAppName(id);
			subscribe(e, id);
			entries_.emplace(id, std::move(e));
			media.insert(id);
		}
	}

	void subscribe(Entry &e, const std::string &id)
	{
		std::weak_ptr<Signal> weak = signal_;
		try {
			e.mediaToken = e.session.MediaPropertiesChanged([weak, id](auto &&, auto &&) {
				if (auto s = weak.lock())
					s->markMedia(id);
			});
			e.playbackToken = e.session.PlaybackInfoChanged([weak, id](auto &&, auto &&) {
				if (auto s = weak.lock())
					s->markState(id);
			});
			e.timelineToken = e.session.TimelinePropertiesChanged([weak, id](auto &&, auto &&) {
				if (auto s = weak.lock())
					s->markState(id);
			});
		} catch (...) {
		}
	}

	static void unsubscribe(Entry &e)
	{
		try {
			if (e.mediaToken.value)
				e.session.MediaPropertiesChanged(e.mediaToken);
			if (e.playbackToken.value)
				e.session.PlaybackInfoChanged(e.playbackToken);
			if (e.timelineToken.value)
				e.session.TimelinePropertiesChanged(e.timelineToken);
		} catch (...) {
		}
	}

	void refreshMedia(Entry &e, bool safetyPoll = false)
	{
		try {
			const auto props = awaitWithin(e.session.TryGetMediaPropertiesAsync(), kQueryLimit);
			if (!props)
				return;
			TrackInfo t;
			t.title = winrt::to_string(props.Title());
			t.artist = winrt::to_string(props.Artist());
			t.album = winrt::to_string(props.AlbumTitle());
			t.albumArtist = winrt::to_string(props.AlbumArtist());
			normalizeMetadata(t, e.data.appId);

			const std::string key = t.identity();
			const bool sameTrack = key == e.artKey;
			// A safety poll is only meant to recover missed track-change events.
			// Do not fetch, hash, or decode a thumbnail on every poll when
			// the media app reports the same track.
			if (safetyPoll && sameTrack)
				return;
			// Re-read artwork on every property event: players often deliver the thumbnail
			// in a second event after the text. Unchanged bytes keep the same hash, so the
			// overlay does not re-render.
			ImagePtr art = sameTrack ? e.data.track.artwork : nullptr;
			uint64_t artHash = sameTrack ? e.data.track.artworkHash : 0;
			try {
				const auto bytes = readStream(props.Thumbnail());
				if (!bytes.empty()) {
					const uint64_t h = hashBytes(bytes.data(), bytes.size());
					if (h != artHash || !art) {
						Image img = decodeWithWic(bytes, 600);
						if (!img.empty()) {
							art = std::make_shared<const Image>(std::move(img));
							artHash = h;
						}
					}
				}
			} catch (...) {
				// Missing artwork is normal (podcasts, some browsers); keep text.
			}
			t.artwork = art;
			t.artworkHash = art ? artHash : 0;
			e.artKey = key;
			e.data.track = std::move(t);
			++e.data.version;
		} catch (...) {
		}
	}

	void refreshState(Entry &e)
	{
		try {
			const auto info = e.session.GetPlaybackInfo();
			const PlaybackStatus previous = e.data.status;
			e.data.status = mapStatus(info.PlaybackStatus());
			const bool resumed = previous != PlaybackStatus::Playing && e.data.status == PlaybackStatus::Playing;
			double rate = 1.0;
			if (auto r = info.PlaybackRate())
				rate = r.Value();

			const auto tl = e.session.GetTimelineProperties();
			const double start = std::chrono::duration<double>(tl.StartTime()).count();
			const double end = std::chrono::duration<double>(tl.EndTime()).count();
			const double pos = std::chrono::duration<double>(tl.Position()).count();
			Timeline t;
			t.known = end - start > 0.5;
			t.durationSec = std::max(0.0, end - start);
			t.positionSec = std::max(0.0, pos - start);
			t.rate = rate > 0 ? rate : 1.0;
			// Translate the player's wall-clock sample time into our steady clock.
			const auto updated = tl.LastUpdatedTime();
			auto age = winrt::clock::now() - updated;
			const auto steadyNow = Clock::now();
			if (updated.time_since_epoch().count() <= 0 || age < decltype(age)::zero() || age > std::chrono::hours(24))
				age = decltype(age)::zero();
			t.sampledAt = steadyNow - std::chrono::duration_cast<Clock::duration>(age);
			// Some players resume without a fresh timeline sample; the stored sample then dates
			// from before the pause and extrapolating from it would jump ahead. Treat the reported
			// position as current in that case.
			if (resumed && age > std::chrono::seconds(1))
				t.sampledAt = steadyNow;
			e.data.timeline = t;
			++e.data.version;
		} catch (...) {
		}
	}

	void publishSnapshot(const GManager &manager)
	{
		MediaSnapshot snap;
		snap.state = ProviderState::Ready;
		try {
			if (auto cur = manager.GetCurrentSession())
				snap.systemCurrentAppId = winrt::to_string(cur.SourceAppUserModelId());
		} catch (...) {
		}
		snap.sessions.reserve(entries_.size());
		for (const auto &kv : entries_)
			snap.sessions.push_back(kv.second.data);
		publish(std::move(snap));
	}

	std::shared_ptr<Signal> signal_;
	std::thread thread_;
	std::map<std::string, Entry> entries_; // provider thread only
};

} // namespace

std::unique_ptr<MediaProvider> createSystemProvider()
{
	return std::make_unique<SmtcProvider>();
}

} // namespace tonearm
