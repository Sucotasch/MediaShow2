# Project knowledge

This file gives Freebuff context about your project: goals, commands, conventions, and gotchas.

## What this is
MediaShow2 — a multimedia lister plugin (`.wlx`/`.wlx64`) for Total Commander, a C++ rewrite of the 2006 Delphi plugin MediaShow v0.9.5. Plays video/audio in TC's F3 lister and QuickView (Ctrl+Q) modes. Windows-only, C++17, Win32 API, no external deps beyond Windows SDK.

## Key files
- `src/dllmain.cpp` — everything: TC WLX API, UI (toolbar/seekbar/volume/status/playlist), append mode, repeat, dark mode, video-switch logic (~2000 lines)
- `src/mf_player.cpp/.h` — Media Foundation playback engine (primary)
- `src/ds_player.cpp/.h` — DirectShow fallback engine (OGG, FLAC, MKV, DAT, VOB, MIDI)
- `src/plugin_api.h` — TC WLX SDK constants + control IDs
- `src/resources.rc`, `src/app.manifest` — manifest currently NOT linked (SxS issue)
- `sdk/listplug.h` — TC WLX SDK header (do not modify)
- `package.py` — packs built DLLs into `.wlx`/`.wlx64` ZIP archives (+ `pluginst.inf`)
- `PROJECT_CONTEXT.md` — detailed context in Russian; `Summary.txt` — progress notes; `README.md` — user-facing docs

## Commands (bash on Windows, from project root)
```bash
# Configure + build (VS 2022)
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
cmake -B build-x64 -G "Visual Studio 17 2022" -A x64
cmake --build build-x64 --config Release

# Package → MediaShow2.wlx / MediaShow2.wlx64 (needs built DLLs)
python package.py

# Headless verification scripts
python test_parse.py      # LB_GETTEXT filename parsing logic
python test_bug.py        # regression test for beforeLen bug
python test_verify.py     # PE/ASLR/DEP/exports/archive checks on built DLLs
```
No test framework, linter, or CI exists. Runtime behavior can only be tested manually inside Total Commander (F3 / Ctrl+Q).

## Conventions
- UNICODE build; `target_compile_definitions`: UNICODE, _UNICODE, WIN32_LEAN_AND_MEAN, NOMINMAX, WLX_EXPORTS
- Security: `/DYNAMICBASE`, `/NXCOMPAT` linked; `/MANIFEST:NO` (resources.rc provides manifest)
- Output DLL names: `MediaShow2.dll` (x86), `MediaShow2_x64.dll` (x64) → `build*/bin/Release/`
- Project state tracked in `PROJECT_CONTEXT.md` — update it after significant changes
- When fixing a bug, add a regression check in the matching `test_*.py` where applicable

## Gotchas
- **Build dir is `build-x64/` (hyphen), NOT `build_x64/`** — `package.py` and project docs reference the hyphenated one. Do not create `build_x64/`.
- **TC file panel is LCLListBox (Lazarus/Free Pascal), not SysListView32.** LB_GETTEXT returns `filename.ext NNN NBSP NNN NBSP NNN TAB DD.MM.YYYY HH:MM -a--` — size groups are separated by **non-breaking spaces (U+00A0)** and a **TAB (U+0009)** precedes the date. Parse right-to-left from the date.
- **TC lister intercepts most keys** — only Space (play/pause), S (stop), Esc (close) reach the plugin; arrows/F11/M/L/I are consumed by TC.
- **F3 always creates a new lister tab** (WLX API limitation). Append mode collects files then closes the new tab via `PostMessage(ParentWin, WM_CLOSE)` and returns NULL.
- **MFPlayer_Open always returns S_OK** even when `QI(IID_IMFVideoDisplayControl)` fails — check `pVideoCtrl != NULL` separately.
- **On this system pVideoCtrl is always NULL** → `MFPlayer_HasVideo()` always FALSE; MF renders video internally anyway, aspect-ratio helpers are no-ops.
- **VP9→video-only H.264 switching freezes.** Fix: in `ListLoadNextW` fallback and QuickView paths, call `RecreateVideoWindow(state)` (destroy+recreate the HWND) instead of `DestroyChildVideoWindows`, plus `Sleep(50)` after MF stop and a 500ms cooldown. Only the audio pipeline resets the stale D3D state left on the HWND.
- **QuickView vs F3**: `IsQuickView` = `GetParent(ParentWin) != NULL`. QuickView must skip playlist load/save and keep `showPlaylist=FALSE`.
- `OutputDebugString` debug calls still exist in `dllmain.cpp` — strip before release.
- Playlist auto-saves to `MediaShow2_playlist.txt` on every change.
- Original binary analysis is in `PROJECT_CONTEXT.md`; original source doesn't exist — all reverse-engineering notes live there.
