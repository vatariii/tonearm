# Notices

Tonearm
Copyright (C) 2026 Tonearm contributors

This program is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2 of the License, or (at your option) any later version. It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE) for details.

## Bundled fonts

The files in `data/fonts` are distributed unmodified under the SIL Open Font License, Version 1.1 (`data/fonts/OFL-1.1.txt`).

- **Inter** — Copyright 2016–2023 The Inter Project Authors (https://github.com/rsms/inter)
- **IBM Plex Sans, IBM Plex Mono** — Copyright 2017–2018 IBM Corp., with Reserved Font Name "Plex"
- **EB Garamond** — Copyright 2010–2012 Georg Duffner (http://www.georgduffner.at)

## Libraries used at run time

- **libobs** (OBS Studio), GPL-2.0-or-later. Tonearm links against it as an OBS plugin.
- **Windows Runtime, DirectWrite, Direct2D and Windows Imaging Component**, provided by Windows.
- **Pango, Cairo, fontconfig** on Linux builds, LGPL/MIT licensed system libraries.

## Prior work

Tonearm's feature set was informed by studying [OBSSpotifyPlugin](https://github.com/lingeriegoat/OBSSpotifyPlugin) by lingeriegoat (GPL-2.0), a native OBS plugin that shows Windows media session data. Tonearm contains no code or assets from it.

## Sample content

The sample tracks, artist names, album names and cover art shown by Tonearm are invented. The covers are generated procedurally at run time. Any resemblance to real releases is unintended.

## Media in this repository

Everything under `docs/media/` except `docs/media/artwork/` and the Turntable images listed below is rendered by Tonearm's own code from the invented sample tracks above, and is covered by the GPL like the rest of the project. The wordmark is set in EB Garamond (SIL OFL 1.1).

The clips in `docs/media/artwork/` are short screen recordings of Tonearm running in OBS Studio while Apple Music played. `compact`, `expanded`, `square`, `vertical`, `palettes` and `motion` in `docs/media/turntable/` are rendered by Tonearm's own code from covers and track information read from Apple Music. Together they show album artwork, including Apple Music motion artwork, for:

- *SOS* by SZA
- *Wonder* by Shawn Mendes
- *Merry Christmas* by Mariah Carey
- *Positions* by Ariana Grande
- *143* by Katy Perry

That artwork is the property of the artists, their labels and their designers. It is not covered by Tonearm's license, is not included in the release package, and appears here only in short clips and images that demonstrate what the plugin displays. No audio is included. If you hold rights in any of it and want a clip or image removed, please open an issue and it will be taken down.

## Trademarks

Apple Music, Spotify, TIDAL, Deezer, Amazon Music, Windows and OBS Studio are trademarks of their respective owners. Tonearm is not affiliated with or endorsed by them.
