# Getting started

## Install

Tonearm is a regular OBS plugin: one DLL and a folder of data files.

1. Download `tonearm-<version>-windows-x64.zip` from the Releases page. (The `-pdb` zip next to it holds debug symbols and is only useful when reporting a crash.)
2. Close OBS completely, including from the system tray.
3. Extract the zip into the OBS installation folder, usually `C:\Program Files\obs-studio`. Windows will ask for administrator permission. The zip holds an `obs-plugins` and a `data` folder that merge into the ones OBS already has; let Windows merge them, never replace the folders. Afterwards you should have:

   ```
   C:\Program Files\obs-studio\obs-plugins\64bit\tonearm.dll
   C:\Program Files\obs-studio\data\obs-plugins\tonearm\
   ```

   The licence, notice, changelog and a copy of this README sit inside `data\obs-plugins\tonearm\`, so nothing is added to the OBS folder itself.

4. Start OBS. **Help → Log Files → View Current Log** should contain a line like `[tonearm] loaded version 0.3.1`.

### Alternative location

OBS 30 and newer also load plugins from `C:\ProgramData\obs-studio\plugins`, which keeps them apart from OBS's own files and survives reinstalling OBS. For that location the layout is different:

```
C:\ProgramData\obs-studio\plugins\tonearm\bin\64bit\tonearm.dll
C:\ProgramData\obs-studio\plugins\tonearm\data\        (the contents of data\obs-plugins\tonearm from the zip)
```

### Uninstall

Close OBS and delete `tonearm.dll` and the `tonearm` data folder from wherever you installed them. Scenes that contained a Tonearm source will show it as missing until you remove it.

## Add the overlay

1. Under **Sources**, click **+** and choose **Tonearm Now Playing**.
2. The properties window opens with a live preview. If nothing is playing, switch on **Show sample tracks when nothing is playing** (under Playback states) so you can see what you're designing.
3. Pick a **Preset** and a **Layout**. The source size changes to suit the layout; adjust **Width** and **Height** if you need it larger or smaller. Everything scales with the size, so prefer resizing here over stretching the source in the canvas, which would blur it.
4. Start playing music. Tonearm switches from the sample to the real track within a moment.
5. Turn the sample tracks off again before you go live, otherwise they appear on stream whenever your music stops.

## Choosing the player

By default Tonearm follows whichever player is playing. If several are playing, or none is, it keeps the one it showed last, then falls back to what Windows considers the current session.

To pin it to particular apps, open **Music sources** and set **Choose a player** to:

- **Prefer the apps below, in order**: a playing app still wins, but among equals your order decides.
- **Only the apps below**: anything not on the list is ignored.

Add apps by name, for example `Apple Music`, `Spotify`, `TIDAL`, `foobar2000`. The info line under the list shows which players Windows is reporting right now; press **Refresh** to update it.

Web browsers are ignored unless you tick **Include web browsers** or put the browser on your list. Browsers report whatever played most recently, including videos and ads.

## Several overlays

You can add more than one Tonearm source, for example a compact bar in your gameplay scene and a vertical card in your "be right back" scene. They share one connection to Windows, so extra sources cost almost nothing while hidden.

## Sharing a look

Under **Preset files**, choose a file in **Export to** and press **Export preset**. On another source or PC, choose it in **Import from** and press **Import preset**. The file is plain JSON with your look, layout, motion and playback-state settings. Which apps to follow is not included, so importing someone else's preset never changes your music sources.
