# Testing and verification status

This page says plainly what has been checked, how, and what still needs a Windows machine. It is updated with each release.

## Status for 0.3.1

0.3.1 is a stability and security release based on an external code audit; what changed for each finding is in [the audit disposition](audit/v0.3.1.md).

| Area | Status | How |
|---|---|---|
| Core, layout and rasterizer at the extremes of the size range | Verified | Every preset and layout at 10 extreme sizes (64×32 to 4096×32 and 64×4096), with and without artwork, doubled text and 1-pixel artwork. Run under AddressSanitizer and UndefinedBehaviorSanitizer; this caught a real out-of-bounds read in 0.3.0 that is now fixed |
| URL policy, redirects, outage backoff, cache budget | Verified in unit tests | `tests/test_net_cache.cpp`, also under ThreadSanitizer |
| Unit tests on Windows (DirectWrite) | Run by CI | Same suite on the Windows runner with MSVC |
| Windows package | Produced by CI | Same packaging as 0.3.0 |
| **Plugin loaded in OBS on Windows** | **Not yet checked for 0.3.1** | The changes to WinHTTP, the motion player, settings and the SMTC loop compile and package in CI but have not run inside OBS. A planned 12-hour soak test on the author's PC did not start |
| Performance against 0.3.0 | Not measured | No benchmark numbers yet |

## Status for 0.2.0

| Area | Status | How |
|---|---|---|
| Plugin loads and renders in OBS Studio on Windows 11 | Verified | Tested by the author on his own PC before approving the release candidate |
| SMTC media connection with Apple Music | Verified | Real playback: title, artist and album split correctly, artwork, progress, track changes |
| Online Apple Music motion artwork | Verified | Real playback across many albums, including SOS, Wonder, Merry Christmas, Positions and 143, which were also recorded for the README showcase in four Tonearm sources at once |
| Motion cache and replay | Verified | Windows CI downloads real motion videos and checks that replaying an album reuses the local cache |
| Spotify, TIDAL, Deezer and other SMTC players | Expected to work, not individually checked | They publish to the same Windows media controls; Apple Music is the player that was exercised |
| Hardware decoding cost on other GPUs | Depends on the PC | Check OBS's *Stats* window on your own setup; see [Animated artwork](animated-artwork.md#performance) |

### Earlier checks (0.1.0 development, mostly Linux)

| Area | Status | How |
|---|---|---|
| Core: color, imaging, layout, rasterizer, scene | Verified | 48 unit tests, also run under AddressSanitizer and UndefinedBehaviorSanitizer with no findings in Tonearm code |
| Layout across presets, layouts, sizes and missing content | Verified | Geometry invariants checked for 4 presets × 4 layouts × 8 content combinations × several sizes and aspect ratios |
| Plugin loads, renders and shuts down in OBS | Verified on Linux | OBS Studio 30.0.2 on Linux (software OpenGL), screenshots through obs-websocket; shutdown log reports `Number of memory leaks: 0` |
| Settings window, live preview, every group | Verified on Linux | Opened in OBS 30.0.2 and inspected |
| Transitions, seek, pause/resume, long titles, missing artwork, player hand-off, idle | Verified on Linux with a scripted player | `TONEARM_SCRIPTED_MEDIA=1`, see below |
| Preset or layout changes through obs-websocket | Verified on Linux | All 16 combinations switched live during the gallery capture |
| DirectWrite text backend | Verified in unit tests on Windows | The 48 unit tests pass on the Windows CI runner (MSVC 19.44), where text shaping and rasterization go through DirectWrite with the bundled fonts. Since seen inside OBS on Windows |
| WIC artwork decoding | Verified on Windows | Apple Music artwork decoded in OBS on Windows 11 |
| SMTC media connection (Apple Music, Spotify, others) | Verified on Windows with Apple Music | Other players use the same interface |
| Windows package layout | Produced by CI | The Windows job installs `obs-plugins\64bit\tonearm.dll` and `data\obs-plugins\tonearm\` and zips them, with debug symbols in a separate zip. |
| High-DPI | By design | OBS sources render in canvas pixels, not screen pixels. Tonearm scales everything from the source's width and height, so a larger source is rendered sharper rather than stretched. |


## Animated artwork

Detailed checks behind the animated artwork in 0.2.0.

| Area | Status | How |
|---|---|---|
| Release matching (editions, singles, remasters, similar titles, featured artists, ambiguity) | Verified | Unit tests in `tests/test_motion.cpp` |
| Resolver: cache, negative cache, cancellation on rapid track changes, duplicate suppression, failing source, prompt shutdown | Verified | Unit tests; also run under ThreadSanitizer (no reports in Tonearm code) |
| Masking in all 4 presets × 4 layouts: nothing outside the artwork changes, corners masked, Editorial feather, still cover when no frame | Verified | Unit tests with the CPU reference compositor |
| Fade in, track change, decode loss, pause tint, Reduce motion, static mode, placeholders | Verified | Unit tests driving the real scene |
| Playback inside OBS: MP4 (square), MP4 (portrait, cropped), fragmented-MP4 HLS with a Unicode file name | Verified on Linux | OBS 30.0.2, scripted player, screenshots through obs-websocket |
| Fallback inside OBS: no file, corrupt file | Verified on Linux | Still cover shown; OBS logs `MP: Failed to open media` |
| Seamless loop | Verified on Linux | 1,655 screenshots at about 28 per second over 60 s (around 10 loops of 4 s videos): no dark or dropped frames while a track played |
| Hide and show, scene switches, 30 rapid create/delete cycles, switching the feature off, clean shutdown | Verified on Linux | No growth in threads; open video files close when the feature is switched off; `Number of memory leaks: 0` |
| CPU cost | Measured on Linux, software rendering only | See [Animated artwork](animated-artwork.md#performance) |
| Windows: build, unit tests, package | Run by CI | Also tested in OBS on Windows by the author |
| Windows: hardware decoding, GPU load, encoder impact while gaming | Varies by PC | Smart quality and adaptive workload keep it modest; measure on your own setup |
| Apple Music motion artwork retrieval | Verified on Windows | Through the optional third-party lookup described in [Animated artwork](animated-artwork.md) |

### Windows check for animated artwork

1. Make a folder with a short H.264 MP4 named after an album you play, such as `Artist - Album.mp4`, and a `[tall]` version if you want to try portrait.
2. Set **Artwork** to *Animated artwork when available* and choose the folder. Play that album in Apple Music: the video fades in over the cover within about a second.
3. Play an album without a file: the still cover stays. Skip between tracks quickly: no wrong video ever appears.
4. Pause: the video dims and freezes. Turn on Reduce motion: the still cover returns.
5. Check each preset (Glass, Minimal, Neon, Editorial) in a horizontal and a vertical layout.
6. With a game running, compare OBS's *Stats* window (render time, skipped frames) and Task Manager GPU "Video Decode" with the feature on and off.

## Windows test plan

Run in a portable OBS so an existing installation and its plugins stay untouched.

1. **Load**: the log shows `[tonearm] loaded version …` and `system media provider started`; no warnings from `tonearm`.
2. **Apple Music**: play, pause, resume, skip, seek by dragging the scrubber. Title, artist and album appear separately (the combined field is split). Artwork appears, also when it arrives a moment after the text. Progress stays within a second of Apple Music's own display for a whole track.
3. **Spotify**: same as above, plus an ad or a podcast episode if available.
4. **Switching players**: pause Apple Music and start Spotify; the overlay follows. With *Prefer the apps below* and Apple Music first, start both; Apple Music wins.
5. **Browsers**: play a YouTube video in Edge. Nothing shows until **Include web browsers** is ticked.
6. **No artwork**: a local file without embedded art in Media Player shows the generated cover.
7. **Closing the player** with each idle behavior: hide, and show a message.
8. **Long and non-Latin titles**: a long title scrolls; a Japanese or Korean title renders with the Windows fallback font.
9. **Resizing and scaling**: change width and height, switch every preset and layout, try 125% and 200% Windows display scaling.
10. **Load**: with the overlay visible, OBS's CPU usage and render time (Stats dock) change by less than a percent and well under a millisecond respectively. With the source in a hidden scene, Tonearm adds nothing measurable.
11. **Shutdown and restart**: close OBS while music plays; it exits promptly, the log shows `media provider stopped` and `Number of memory leaks: 0`. Restart and the overlay comes back with its settings.
12. **Hung player**: suspend the player process in Task Manager's details view (or Resource Monitor); OBS keeps rendering, and closes normally.

## In-OBS scenario run

The scripted player replays a fixed sequence in a loop. This contact sheet shows one loop captured once per second from OBS 30.0.2 on Linux, with the Glass preset:

![Scenario run in OBS](media/obs-scenario-run.webp)

| Time | Event | What to look for |
|---|---|---|
| 0 s | Apple Music starts playing | Card fades in |
| 6 s | Seek forward | Bar glides to the new position; times update |
| 10 s | Pause | Art and text dim, time stops |
| 14 s | Resume | Dimming lifts, time continues from where it stopped |
| 18 s | Next track with a long title | Crossfade with the Glass choreography, then the title scrolls with soft edges |
| 25 s | Track without artwork | Generated cover, palette from the track |
| 30 s | Spotify starts while Apple Music pauses | Hand-off to the playing player |
| 36 s | Both players close | Card fades out |
| 41 s | Playing again | Card fades back in with the new track |

## Running the tests

```sh
ctest --test-dir build --output-on-failure
./build/tonearm-tests layout      # only tests whose name contains "layout"
```

To run them with sanitizers on Linux:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DTONEARM_BUILD_PLUGIN=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan --target tonearm-tests && ./build-asan/tonearm-tests
```

LeakSanitizer reports allocations made by fontconfig when fonts are registered; those belong to fontconfig's process-wide cache.
