# Changelog

All notable changes to Tonearm are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.3.0] - 2026-11-25

### Added

- Turntable preset: a vinyl deck with the album sleeve behind a spinning record and a tonearm that follows the song. Animated artwork plays on the sleeve only.
- `tonearm-render --tracks` renders documentation images from real covers and track info, and `tools/media/read-covers.ps1` saves them from Apple Music through the Windows media session.

### Changed

- Turntable preset redesigned. The record now sits on a machined platter in front of its sleeve, with a full tonearm assembly (bearing housing, counterweight, offset headshell and a cartridge in the cover's accent color) and an arm rest. The pressing is colored from the artwork but kept apart from the sleeve: luminous on dark covers, deep on light ones, a second cover hue when the main one matches the sleeve, and classic black vinyl for monochrome covers. Grooves, track gaps, run-out and a printed label are rendered once per track; the light stays fixed while the record turns under it.
- Turntable motion: the platter spins up to 33 1/3 RPM and coasts to a stop on pause, the arm lifts to its rest for pauses and track changes and sets down on the lead-in, then tracks inward with the song. The hardware is no longer dimmed on pause, and a new record settles over the old one instead of cross-fading through it.
- Turntable layouts: each of Compact, Expanded, Square and Vertical fits its own deck composition to the space; Square and Vertical anchor the text to the bottom so the deck fills the rest. Compact is the distilled version: just the record and its arm, filling the bar's height, with no sleeve.
- Turntable labels are designed per album: one of five understated styles (paper, split, band, foil, mark), colored from the cover and shared by every track of an album.

## [0.2.0] - 2026-11-19

First public release. Tested by the author with real Apple Music playback and genuine motion artwork in OBS Studio on Windows 11.

### Added

- Animated artwork (experimental, off by default). It plays a looping video in place of the still cover, masked into all four presets and every layout. Videos come from a local folder named `Artist - Album.mp4` (add ` [tall]` for portrait). MP4, MOV, WebM, MKV and HLS, including fragmented-MP4 HLS, all decode through OBS's own media source. It falls back to the still cover whenever no video matches, the match is uncertain, or decoding fails. Reduce motion turns it off. See [docs/animated-artwork.md](docs/animated-artwork.md).
- Settings: Artwork (static or animated), preferred orientation, behavior when music pauses, optional local artwork folder, and Smart / Maximum / Balanced online motion quality.
- Genuine Apple Music animated-artwork lookup through an opt-in unofficial third-party metadata API (no user credentials), with complete local MP4 caching and per-album persistent replay index.
- Robust matching across albums and compatible edition aliases, album-ID fallback, verified soundtrack composer/performer credits, stalled-decoder recovery, and adaptive rendering workload.

### Fixed

- Stop over-brightening already-sRGB animated video frames. OBS FFmpeg can deliver either ordinary 8-bit sRGB or higher-precision linear video; Tonearm now uses the child source's actual reported color space to apply transfer encoding only when required.
- Explicit sRGB framebuffer-state handling while capturing the media source, so the transfer is not applied unpredictably across OBS rendering configurations.
- Fix an artificial 30 FPS animated artwork capture throttle; use a tested 60 FPS ceiling so 24 FPS source video frames are not lost on a 60 FPS OBS canvas.
- Match soundtrack albums credited to composers even when Windows SMTC identifies the current singer, but only after Apple's public album and track catalog verify the exact album edition, track title, and performer.

### Known limitations

- The third-party motion-artwork service and its album catalog are unofficial; not every Apple Music album is guaranteed to return a video.
- Playback performance on a particular Windows GPU and OBS configuration requires on-device validation. No claim of universal, zero-stutter playback is made.

## [0.1.0] - 2026-11-19

Internal preview, never published as a GitHub release. Everything below ships in 0.2.0.

### Added

- Tonearm Now Playing source for OBS Studio 30.2 and newer.
- Media from the Windows System Media Transport Controls: Apple Music, Spotify, TIDAL, Deezer, Amazon Music, foobar2000, MusicBee, VLC, Media Player and other players that publish there. Browsers are opt-in.
- Player selection: automatic, preferred order, or allow list.
- Four presets (Glass, Minimal, Neon, Editorial), each with compact, expanded, square and vertical layouts.
- Accent and surface colors from the artwork, with automatic contrast adjustment to WCAG AA.
- Extrapolated progress with smooth seeking, elapsed and remaining time.
- Per-preset transitions for track changes, late artwork and style changes; scrolling for long text; reduced-motion mode.
- Paused, idle and closed-player behaviors, including hide-after-delay and an idle message.
- Sample tracks for designing scenes without music playing.
- Preset export and import as JSON.
- Bundled Inter, IBM Plex and EB Garamond fonts (SIL OFL 1.1).
- `tonearm-render`, a headless renderer for documentation images, and a scripted test player (`TONEARM_SCRIPTED_MEDIA=1`).

[Unreleased]: https://github.com/cxrlosxk/tonearm/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/cxrlosxk/tonearm/releases/tag/v0.3.0
[0.2.0]: https://github.com/cxrlosxk/tonearm/releases/tag/v0.2.0
