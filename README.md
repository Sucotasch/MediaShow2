# MediaShow2

A multimedia lister plugin for [Total Commander](https://www.ghisler.com/), rewriting the original MediaShow v0.9.5 (2006, Delphi 6) in modern C++.

## Features

- **Video/Audio playback** via Media Foundation (primary) with DirectShow fallback
- **Playlist** from TC selected files, directory scan, or auto-load from previous session
- **Append mode** — add files to existing playlist via F3 without creating new tabs
- **Fullscreen** (double-click) with aspect ratio preservation
- **Dark mode** support via TC's `lcp_darkmode`
- **Repeat** — Off / All / One (toolbar button, persisted)
- **File Info** — metadata panel with album art, codec, bitrate, tags (title/artist/album/genre/year/track)
- **Context menu** with all playback controls
- **64-bit** support (`.wlx64`)

### Supported Formats

| Video | Audio |
|-------|-------|
| AVI, MPG, MPEG, ASF, VOB | MP3, WAV, WMA, OGG, FLAC, AAC, OPUS |
| MKV, WEBM, MP4, M4A, DAT | MIDI, MID, KAR, MP1, MP2 |

### Keyboard Shortcuts

| Key | Video mode | Playlist visible |
|-----|-----------|-----------------|
| Space | Play/Pause | Play/Pause |
| S | Stop | — |
| ↑/↓ | Volume ±5% | Navigate playlist |
| ←/→ | Seek ±10s | — |
| Esc | Close plugin | Close lister tab |
| F11 | Toggle fullscreen | — (disabled) |
| M | Toggle mute | Toggle mute |

### Playlist Management

- **Delete** — remove selected track from playlist
- **Ctrl+↑/↓** — reorder tracks in playlist
- **Enter** — play selected track
- **Click column header** — sort by #, Name, Type, or Date
- **Auto-save** — playlist saved to file on every change
- **Auto-load** — restored on first plugin launch
- **Clear** — context menu clears playlist and saved file

### Append Mode

Toggle via context menu ("Add files to playlist"):
- **ON:** F3 adds files to existing playlist (never replaces)
- **OFF:** F3 closes old tab, creates new with new files

## Building

Requires Visual Studio 2022 with C++ Desktop workload.

```bash
# x86
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release

# x64
cmake -B build_x64 -G "Visual Studio 17 2022" -A x64
cmake --build build_x64 --config Release

# Package into .wlx / .wlx64
python package.py
```

## Installation

Double-click `MediaShow2.wlx` (x86) or `MediaShow2.wlx64` (x64) in Total Commander, or copy the `.wlx`/`.wlx64` file to TC's plugin directory.

## Project Structure

```
src/
├── dllmain.cpp       — Plugin entry, TC API, UI, playlist, append mode
├── mf_player.h/cpp   — Media Foundation playback engine
├── ds_player.h/cpp   — DirectShow fallback engine
├── plugin_api.h      — TC WLX SDK constants and control IDs
├── resources.rc      — Resource file
└── app.manifest      — DPI awareness (currently disabled)
sdk/
└── listplug.h        — TC WLX SDK header
```

## Technical Notes

### TC WLX API

The plugin implements the standard TC lister interface:
- `ListLoad` / `ListLoadW` — Called on F3, creates the player window
- `ListLoadNext` — Called for file navigation (n/p keys)
- `ListCloseWindow` — Cleanup
- `ListGetDetectString` — File type detection
- `ListSendCommand` — Handles `lc_newparams` (dark mode) and `lc_setpercent` (seek)

### Append Mode Implementation

When append mode is ON and F3 is pressed:
1. Find existing plugin window via static HWND
2. Collect selected files from TC's LCLListBox
3. Append to existing playlist
4. Close the new lister tab via `PostMessage(ParentWin, WM_CLOSE)`
5. Return NULL

**TC limitation:** F3 always creates a new lister tab (WLX API). Append mode closes it immediately after file collection.

### Selected Files from TC

TC passes selected files via `LCLListBox` (Lazarus/Free Pascal). The plugin reads using `LB_GETTEXT`:

```
filename.ext NNN NBSP NNN NBSP NNN TAB DD.MM.YYYY HH:MM -a--
```

**Important:** TC uses non-breaking spaces (U+00A0) between file size groups and TAB (U+0009) after the last group.

### Playback Engines

- **Media Foundation** (Windows 7+): Handles AVI, MP4, MP3, WAV, AAC, WMA natively
- **DirectShow** (fallback): Handles OGG, FLAC, MKV, DAT, VOB, MIDI

### Video Switching

Rapid Next/Prev on video files can cause MF pipeline corruption. Solution:
- Recreate MFPlayer on each video switch
- Sleep(50) after MF Stop for async cleanup
- 500ms cooldown timer prevents rapid switching

## Credits

- Original MediaShow v0.9.5 by Chernih Sergey (SCHM@ukr.net)
- TC WLX SDK by [ghisler](https://github.com/ghisler/WLX-SDK)
- Reference: [csvtab-wlx](https://github.com/little-brother/csvtab-wlx) plugin patterns

## License

See source code for details.
