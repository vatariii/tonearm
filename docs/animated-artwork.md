# Animated artwork (experimental)

Tonearm can play a looping video in place of the still album cover. The video sits in the same place as the cover, with the same mask, in every preset and layout. When no video is available, or anything goes wrong, Tonearm shows the still cover as it always has.

The feature is **off by default**. While it is off, Tonearm creates no media decoder, starts no extra thread and reads no extra files.

<p align="center">
  <a href="media/artwork/artwork.mp4"><img src="media/artwork/artwork.webp" width="880" alt="Five Tonearm overlays recorded in OBS on Windows, each playing an album's genuine Apple Music motion artwork."></a>
</p>

*Recorded from Tonearm 0.2.0 in OBS Studio on Windows 11 with Apple Music. Each clip below shows one album in the Editorial vertical and Glass square layouts. The artwork belongs to its artists and labels; see [NOTICE](../NOTICE.md#media-in-this-repository).*

| | |
|---|---|
| <img src="media/artwork/sos.webp" width="420" alt="SZA, SOS: waves move around a figure sitting on a diving board, in Editorial vertical and Glass square cards"><br>SZA, *SOS* · [MP4](media/artwork/sos.mp4) | <img src="media/artwork/wonder.webp" width="420" alt="Shawn Mendes, Wonder: handwritten words drift over a stormy sea, in Editorial vertical and Glass square cards"><br>Shawn Mendes, *Wonder* · [MP4](media/artwork/wonder.mp4) |
| <img src="media/artwork/merry-christmas.webp" width="420" alt="Mariah Carey, Merry Christmas: snow falls around Mariah Carey in a red coat, in Editorial vertical and Glass square cards"><br>Mariah Carey, *Merry Christmas* · [MP4](media/artwork/merry-christmas.mp4) | <img src="media/artwork/positions.webp" width="420" alt="Ariana Grande, Positions: tiled portraits shift and move, in Editorial vertical and Glass square cards"><br>Ariana Grande, *Positions* · [MP4](media/artwork/positions.mp4) |
| <img src="media/artwork/143.webp" width="420" alt="Katy Perry, 143: a pink and blue vortex swirls around a floating figure, in Editorial vertical and Glass square cards"><br>Katy Perry, *143* · [MP4](media/artwork/143.mp4) | |

## Automatic Apple Music artwork (experimental, Windows)

When **Animated artwork when available** is selected and **Local artwork folder** is blank, Tonearm looks up the current artist/album/track in the independent public API at [artwork.m8tec.top](https://artwork.m8tec.top). Its response provides genuine Apple Music HLS artwork streams. For Apple's finite single-MP4 HLS playlists, Tonearm downloads the complete short original MP4 into a private per-user cache and asks OBS FFmpeg to loop that local video. The **Online motion quality** setting offers **Smart** (recommended, H.264 up to 1080p for small overlays; 2160p for large cards), **Maximum** (up to 2160p HEVC, with compatible fallbacks), or **Balanced** (up to 1080p H.264). Tonearm ignores HLS I-frame-only preview streams, which are not moving videos. Other unencrypted finite HLS layouts may be cached as local playlists. Tonearm doesn't synthesize animation.

The lookup sends **artist and album** to that third party. It does not transmit your Apple ID, Apple Music credentials, or listening history beyond the current lookup. Requests run off the graphics thread. For smooth playback, complete videos are cached in `%LOCALAPPDATA%\\Tonearm\\MotionCache` on Windows and reused, with a persistent release index so repeat plays can skip online discovery entirely; you can delete that folder while OBS is closed to reclaim space. Individual animations are limited to 100 MB, and since 0.3.1 the whole cache is kept to 1 GiB: when a new video would take it over, the least recently played videos are removed first. Videos that are downloading or on screen are never removed, and Tonearm only ever deletes folders it created inside `MotionCache`. The Maximum setting uses more storage, network bandwidth, and decode resources. Smart keeps the original video sharp for its actual on-screen size while avoiding unnecessary 2160p HEVC decoding for small cards. The GPU capture uses bounded supersampling instead of copying every full 2160p frame for a small cover. A failed lookup, ambiguous album, incomplete download, unsupported encryption, or unavailable service falls back to the ordinary still artwork.

This service is **unofficial**, can change, and may be rate-limited. It is not owned or operated by Apple or Tonearm. Tonearm validates normalized artist and album identity, allowing limited safe base/deluxe edition aliases while rejecting conflicting editions. Motion-video URLs are restricted to Apple CDN HTTPS hosts. Tonearm doesn't extract or store Apple web-player tokens.

### Use your own local motion library instead

To avoid network lookups, choose a folder. Name each file after the release:

```
Phoebe Bridgers - Punisher.mp4          square artwork
Phoebe Bridgers - Punisher [tall].mp4   portrait (3:4) artwork
Fleetwood Mac - Rumours (Super Deluxe).mp4
Phoebe Bridgers - Kyoto - Single.mp4
```

Supported files: `.mp4`, `.m4v`, `.mov`, `.webm`, `.mkv`, and `.m3u8`. New files are picked up within about ten seconds. When a local folder is set, Tonearm never queries the remote API.

### About CoverPy

[CoverPy](https://github.com/matteing/coverpy) established a working approach for locating Apple Music's genuine editorial motion HLS streams, including square and portrait artwork. Its experimental approach extracts the Apple Music web-player token to call Apple's undocumented `editorialVideo` catalog field. Tonearm's online mode instead uses a separately published third-party artwork lookup endpoint that returns the HLS URLs, avoiding web-player credential extraction inside the plugin. Both approaches use actual Apple motion streams rather than simulated animation.

The Apple Music API does not officially document `editorialVideo` for third parties. The experimental online mode is subject to service availability and content terms. It is intentionally opt-in and disabled by default.

## Settings

The settings are in **Animated artwork (Experimental)** in the source properties.

| Setting | What it does |
|---|---|
| **Artwork** | *Static artwork* (default) or *Animated artwork when available*. |
| **Online motion quality** | *Smart* (recommended for streaming), *Maximum* (2160p HEVC when available), or *Balanced* (1080p H.264). Existing sources with Maximum selected keep that setting until changed. |
| **Preferred orientation** | *Automatic* uses portrait videos for tall artwork areas and square ones otherwise. *Square* and *Portrait* force a choice. If only one shape exists, it is used, center-cropped to fit. |
| **When music pauses** | *Pause the animation* (default) freezes the current frame. *Keep looping* doesn't. |
| **Local artwork folder** | Optional. Blank enables online automatic artwork lookup on Windows; a folder selects local-only video files. |

**Reduce motion** in the Motion group always shows the still cover. Tonearm captures only the relevant cropped video region, with adaptive high-quality supersampling based on the artwork's actual display size.

## How it behaves

| Situation | Result |
|---|---|
| A video matches the playing release | The video fades in over the still cover (460 ms) and loops |
| No matching file, or the match is uncertain | Still cover |
| File can't be opened or decoded | Still cover (OBS logs `MP: Failed to open media`) |
| Track changes | The old video is dropped at once (the still cover underneath matches it), then the new one fades in when its first frame is decoded. A stale frame from the previous video is never shown |
| Next track from the same album | The same video keeps playing without restarting |
| Music paused | The video is dimmed like the still cover, using a tint so the cover underneath never shows through, and freezes unless *Keep looping* is on |
| Source not visible in any scene | Decoding is paused |
| Feature switched off | The decoder and its thread are destroyed, and the open file is closed |
| Sample tracks (*Show sample tracks when nothing is playing*) | Still covers only |

### Matching rules

An animation is used only after the artist and album identity have been validated. Normalization ignores case, accents, apostrophes and punctuation, and treats `&` as "and". A generic base-album video may be accepted for an edition when the artist and exact base title match, but two conflicting explicit editions are not interchangeable:

- *Midnights* and *Midnights (3am Edition)* are different releases.
- *Rumours* and *Rumours (Super Deluxe)* are different releases.
- *Kyoto - Single* and *Kyoto* are different releases.
- *Wish You Were Here* and *Wish You Were Here (2011 Remastered Version)* are different releases.
- "Deluxe" and "Deluxe Edition" count as the same qualifier.

Artist matching also accepts the album artist and the first artist of a credit like "A & B" or "A feat. B". If two different files claim the same release and shape, neither is used.

## Cold starts, replay, and fast song switching

The very first play of an uncached album can show a still cover while a real motion video is discovered and downloaded. That is expected: a complete 2160p motion video can be tens of megabytes. **Smart** reduces download and decoding work for small overlays. Tonearm writes a persistent album-to-local-video index after a successful download. Replaying that album then starts from disk without contacting the artwork service. Song changes within one release share the same resolver lookup instead of restarting a download for every featured performer. Distinct albums can now download concurrently without blocking one another behind a global cache mutex. An unsuccessful or rate-limited lookup keeps the still cover and is retried conservatively. Since 0.3.1, when the service is unreachable, rate-limits (HTTP 429) or fails (5xx), every Tonearm source in OBS pauses lookups for a while: 30 seconds at first, doubling up to 15 minutes, or as long as the service asks. Cached albums and local folders keep working during that time, and lookups resume on their own.

If the cover is still static after an uncached album has played for 30 seconds, collect the current OBS log. Useful messages begin with `[tonearm] animated`, `[tonearm] motion lookup`, `[tonearm] motion playback`, or `[tonearm] no compatible animated`. A genuine missing asset from the third-party service is not the same as a stopped decoder.

### Recommended pre-release validation

1. With Smart selected, try an uncached song for 30 seconds, then replay it to confirm the persistent index is used.
2. Play three tracks on one album in quick succession without restarting its animation.
3. Change between two entirely different albums and confirm a stale previous album's frames are never shown.
4. Keep one animated album playing for at least five minutes and monitor OBS Stats for render/encoding lag.
5. Compare Smart and Maximum at the same on-screen card size, and test with two Tonearm sources visible.
6. Stop and restart OBS to verify cache reuse survives a full application restart.

The Windows CI suite covers compilation, core matching/resolver tests, and live Apple HLS download/album-index checks. It does **not** reproduce the actual GPU, Apple Music app, or OBS scene layout on every viewer's PC.

## Soundtrack credits and fluidity

Some Apple Music soundtracks credit the album to composers or producers even when Windows reports an individual singer for the playing track. Tonearm accepts this mismatch **only after** the public iTunes catalog verifies the exact soundtrack album edition and confirms that the named track belongs to that album and performer. For example, the 2023 *The Little Mermaid* soundtrack is credited to Alan Menken & Howard Ashman, while the track “Part of Your World (Reprise)” is credited to Halle.

The Apple Music *143* full-motion stream advertises a native frame rate of 24 FPS. Tonearm supports up to 60 overlay-frame captures per second to avoid dropping those source frames on a higher-refresh OBS canvas. It does not claim to make original 24 FPS artwork into real 60 FPS video. For accurate fluidity diagnostics, OBS logs `[tonearm] animation capture performance` with its effective capture rate every 10 seconds.

## Accurate video color

OBS Studio's FFmpeg media source does not always expose frames in the same transfer encoding. Ordinary 8-bit SDR video can already be sRGB-encoded, while higher-precision sources may be rendered in linear light. Tonearm checks the private media source's **actual OBS color space** for each animation. It passes standard sRGB video to the compositor without another gamma conversion and applies a linear-to-sRGB transfer only for the appropriate high-precision SDR path. This prevents the over-bright, washed-out appearance that occurs if the transfer is applied to H.264/sRGB twice.

The intermediate capture explicitly disables framebuffer sRGB writes and restores the prior graphics state afterward. Static covers, UI colors, and preset backgrounds remain unchanged by this video-only correction.

## Performance

These numbers come from OBS Studio 30.0.2 on a 4-core Linux container. It has **no GPU**, so all rendering is software OpenGL and decoding is software only. Rendering work that a graphics card would do shows up here as CPU time, so read the numbers as an upper bound for this machine, not a prediction for Windows. Each figure is OBS's whole-process CPU over 48 seconds of the scripted player, at 1280×720 and 30 fps, with a 720×720 H.264 test video at 30 fps.

| Configuration | OBS process CPU (% of one core) |
|---|---|
| No Tonearm source | 21.1 |
| Tonearm, static artwork | 30.3 |
| Tonearm, animated artwork | 58.1 |
| Animated artwork switched back off | 30.8 |
| Animated artwork on, Tonearm not in the live scene | 20.6 |

Decoding that video alone takes about 7% of one core. The rest of the difference is software rendering: OBS's color conversion of each new frame, Tonearm's copy into its artwork texture, and the masked draw. On a graphics card these are small GPU passes, and OBS's media source uses hardware decoding where it's available (D3D11VA on Windows). Memory rose by about 2 MB.

The design choices that keep the cost down:

- Each Tonearm source creates one private OBS media decoder. A source is not decoded twice merely because its scene is rendered multiple times. Multiple separately configured Tonearm sources can each have their own decoder.
- Tonearm can copy up to 60 frames per second into one reused cropped texture, never exceeding OBS's render cadence. This preserves the original decoded 24/30/60 FPS motion frames without inventing additional animation. Smart picks the efficient 1080p H.264 stream for compact overlays while retaining high-quality supersampling.
- No copies happen while the still cover is shown, and decoding is paused while the source is hidden.
- File lookups run on their own thread, never on OBS's graphics or UI threads.

GPU load, encoder impact and hardware decoding on Windows still need measuring on a real PC (see [Testing](testing.md#animated-artwork)).

## Privacy and network

With static artwork selected, Tonearm makes no motion-artwork network requests. With animated artwork selected and no local folder, Windows Tonearm first checks its private album/video cache, and only sends the playing artist and album to artwork.m8tec.top for HLS URL discovery when no cached match exists. A public iTunes catalog-ID lookup is also used as a fallback when the service's name search fails. With a local folder set, Tonearm uses only local files. Every request is HTTPS with Windows' normal certificate checks. Since 0.3.1 redirects are only followed to the same lookup host, or for videos to Apple's own media servers (apple.com and mzstatic.com); anything else is refused and the still cover stays.
