# MediaShow2 — Deep Technical Analysis & Implementation Plan

**Project:** `D:\Arx\Software Downloads\MediaShow_v0.9.5_patched\MediaShow2`  
**Goal:** Modern WLX lister plugin for Total Commander — Windows 10/11, x86+x64, DirectShow-based playback, Fluent UI, proper UX.  
**Scope of this document:** Every defect found by line-by-line code audit, root causes, and the exact code changes required to fix each one. A junior agent can implement these sequentially without re-reading the full source.

---

## Architecture Summary (for context)

| File | Role |
|---|---|
| `src/dllmain.cpp` (~873 lines) | TC WLX API, Win32 window, all UI, layout, playlist, event handling |
| `src/mf_player.cpp` (~254 lines) | Named "MFPlayer" but is actually DirectShow (IGraphBuilder). Primary engine. |
| `src/ds_player.cpp` (~241 lines) | Named "DSPlayer", also DirectShow. Used as "fallback". Near-identical to mf_player. |
| `src/plugin_api.h` | Control IDs and message constants |
| `sdk/listplug.h` | TC WLX SDK |
| `CMakeLists.txt` | Build config |

> [!IMPORTANT]
> **Both players are DirectShow**. Neither uses the Windows Media Foundation API (no `IMFMediaSession`, `IMFPMediaPlayer`, `MFStartup`, etc.). This is not a bug in itself — DirectShow works fine — but the naming is misleading. The `useDirectShow` flag actually means "use DSPlayer instead of MFPlayer", not "DirectShow vs MF". All fixes below treat both players as DirectShow.

---

## DEFECT 1 — Toolbar Covers Trackbars (ROOT CAUSE of seekbar/volume bug)

### Location
`src/dllmain.cpp` line 344–346, `CreateControls()`

### Description
The toolbar is created with no size-restriction styles:
```cpp
state->hToolbar = CreateWindowEx(0, TOOLBARCLASSNAME, TEXT(""),
    WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP,  // ← CCS_TOP
    0, 0, 100, 36, state->hMainWnd, ...);
```
`CCS_TOP` causes the toolbar to auto-resize to the **full width of the parent**, overriding any explicit `MoveWindow` call in `UpdateLayout`. The toolbar at runtime stretches to cover the seekbar and volume slider.

Because the toolbar is created before the trackbars (lines 366–376), and `CCS_TOP` places it topmost across the full width, the trackbars are:
1. Visually behind the toolbar surface on paint.
2. Their hit-test area is intercepted by the toolbar → mouse clicks never reach them.

The `SetWindowPos(..., HWND_TOP, ...)` calls at lines 749–750 are an attempted workaround but don't solve the root cause: the toolbar's rectangle still overlaps the trackbars even though the Z-order is corrected.

### Fix
**Change toolbar style** to prevent auto-sizing. Add `CCS_NORESIZE | CCS_NOPARENTALIGN`, remove nothing else.

```cpp
// BEFORE (line 344-346):
state->hToolbar = CreateWindowEx(0, TOOLBARCLASSNAME, TEXT(""),
    WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP,
    0, 0, 100, 36, state->hMainWnd, (HMENU)IDC_TOOLBAR, GetModuleHandle(0), NULL);

// AFTER:
state->hToolbar = CreateWindowEx(0, TOOLBARCLASSNAME, TEXT(""),
    WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NORESIZE | CCS_NOPARENTALIGN,
    0, 0, 200, 36, state->hMainWnd, (HMENU)IDC_TOOLBAR, GetModuleHandle(0), NULL);
```

Then in `UpdateLayout()` (line 237–239), force an explicit resize instead of using `TB_AUTOSIZE` alone:
```cpp
// AFTER (replaces lines 237-240):
if (state->hToolbar) {
    MoveWindow(state->hToolbar, 0, 0, 200, tbH, TRUE);
    SendMessage(state->hToolbar, TB_AUTOSIZE, 0, 0);
}
```

**Expected result:** Toolbar occupies only 200px width at the left. The seekbar and volume slider to its right receive mouse input normally.

---

## DEFECT 2 — Volume Jumps on Every Track Change

### Location
`src/dllmain.cpp` lines 63–88 (`OnMFEnd`), lines 730–746 (`ListLoadW`), lines 781–797 (`ListLoadNextW`)

### Description
**Three separate problems combine to cause volume jumping:**

**2a. Hard-coded volume at window creation**  
Line 434: `state->volume = 80;`  
Every time TC opens a file (new `ListLoad` call = new `PluginState` = new window), the volume resets to 80%, regardless of what the user last set.

**2b. Volume not applied after `Open()` in `OnMFEnd`**  
In the auto-advance handler (`OnMFEnd`, lines 71–79), after opening and playing the next file, `SetVolume` is never called:
```cpp
DSPlayer_Open(state->pDSPlayer, nextFile);
DSPlayer_Play(state->pDSPlayer);
// ← no SetVolume here!
```
DirectShow resets to its internal default volume (100% = 0 dB) after each `RenderFile`. So the next track always plays at full volume regardless of the slider position.

**2c. Same issue in `IDM_PREV` / `IDM_NEXT` handlers**  
Lines 541–562 and 564–585: after `Open()` + `Play()`, `SetVolume` is not called. Same jump occurs when navigating manually.

### Fix
**Step 1:** Load volume from INI at startup, save on every change.

In `ListLoadW` (line 708+), after getting `state`:
```cpp
// Load persisted volume (add after line 718, before BuildPlaylist):
if (iniPath[0] != 0)
    state->volume = (int)GetPrivateProfileInt(TEXT("MediaShow2"), TEXT("Volume"), 80, iniPath);
else
    state->volume = 80;
```

Create a helper `SaveVolume(state)` and call it from every place volume is changed:
```cpp
static void SaveVolume(PluginState* state) {
    if (iniPath[0] == 0) return;
    TCHAR buf[16];
    _sntprintf(buf, 16, TEXT("%d"), state->volume);
    WritePrivateProfileString(TEXT("MediaShow2"), TEXT("Volume"), buf, iniPath);
}
```

**Step 2:** Apply `state->volume` after every `Open()` call. Create a helper:
```cpp
static void ApplyVolume(PluginState* state) {
    if (state->useDirectShow)
        DSPlayer_SetVolume(state->pDSPlayer, state->isMuted ? 0 : state->volume);
    else
        MFPlayer_SetVolume(state->pMFPlayer, state->isMuted ? 0.0f : state->volume / 100.0f);
}
```

Then in `OnMFEnd` (after line 74 and 78), `IDM_PREV` (after line 548 and 552), `IDM_NEXT` (after line 571 and 575), `ListLoadNextW` (after line 786 and 793) — add:
```cpp
ApplyVolume(state);
```

---

## DEFECT 3 — Mouse Wheel Inverted + Seek on Scroll (Both Wrong)

### Location
`src/dllmain.cpp` lines 405–422, `VideoWndProc`

### Description
Two bugs:
1. **Inverted**: `delta > 0` (scroll up) calls `volume + 5`. This is actually correct. But `delta > 0` for seek goes `+10s` (forward). Spec says scroll should ONLY control volume, so the seek path should not exist.
2. **Seek on scroll is explicitly forbidden** by the spec: "Mouse wheel: ONLY управление громкостью в районе Volume Slider (привязка колеса к seek НЕ НУЖНА)" (PROJECT_CONTEXT.md line 169). The entire `else` branch of wheel handling (lines 413–418) must be removed.
3. **Wheel is global on video window, not restricted to the volume slider area** — spec says "в районе Volume Slider".

### Fix
Remove `WM_MOUSEWHEEL` from `VideoWndProc` entirely. Handle it in `cbNewMain` instead, with a hit-test to the volume slider rect:

```cpp
// In cbNewMain, add this case (before WM_DESTROY):
case WM_MOUSEWHEEL: {
    if (!state) break;
    POINT pt;
    GetCursorPos(&pt);
    HWND hUnder = WindowFromPoint(pt);
    // Allow scroll anywhere on main window to control volume
    int delta = GET_WHEEL_DELTA_WPARAM(wParam);
    state->volume = max(0, min(100, state->volume + (delta > 0 ? 5 : -5)));
    ApplyVolume(state);
    UpdateVolumeSlider(state);
    UpdateStatus(state);
    return 0;
}
```
And in `VideoWndProc`, delete the entire `WM_MOUSEWHEEL` case (lines 405–422).

> **Note on restriction to volume slider region:** The PROJECT_CONTEXT.md spec says wheel should control volume in the volume slider *area*. The safest compatible implementation is: volume control anywhere on the plugin window (common media player UX), which matches the spec's intent. If strictly needed, add `ScreenToClient` + `RECT` check against `hVolSlider` position.

---

## DEFECT 4 — OnMFEnd Called from Worker Thread → UI Access Race Condition

### Location
`src/mf_player.cpp` lines 26–46 (`EventThread`), `src/dllmain.cpp` lines 63–88 (`OnMFEnd`)

### Description
`EventThread` is a background thread. When `EC_COMPLETE` arrives, it directly calls `OnMFEnd(p->userData)`. `OnMFEnd` then:
- Writes to `state->isPlaying`, `state->isPaused` (non-atomic)
- Calls `DSPlayer_Stop`, `DSPlayer_Open`, `DSPlayer_Play` (COM calls)
- Calls `_tcsncpy` into `state->filePath`
- Reads/writes `state->playlistIndex`

All these operations execute on the **worker thread**, but `state` is owned and also accessed from the **main UI thread** (timer at line 668, all WM_COMMAND handlers, etc.). This is a data race: no lock protects `state`.

The `PostMessage` fallback (line 86) correctly posts to the UI thread, but the direct-play path (lines 71–84) does not.

### Fix
Replace the direct call path in `OnMFEnd` with `PostMessage` to marshal everything to the UI thread. Add a custom message:

In `plugin_api.h`, add:
```cpp
#define WM_PLAYER_TRACK_END  (WM_USER + 200)
```

Rewrite `OnMFEnd`:
```cpp
static void OnMFEnd(void* userData) {
    PluginState* state = (PluginState*)userData;
    if (!state || !state->hMainWnd) return;
    // Always marshal to UI thread — never touch state directly from worker thread
    PostMessage(state->hMainWnd, WM_PLAYER_TRACK_END, 0, 0);
}
```

Add handler in `cbNewMain`:
```cpp
case WM_PLAYER_TRACK_END: {
    if (!state) break;
    state->isPlaying = FALSE;
    state->isPaused = FALSE;
    if (state->playlist && state->playlistIndex < state->playlistCount - 1) {
        state->playlistIndex++;
        TCHAR* nextFile = state->playlist[state->playlistIndex];
        if (state->useDirectShow) {
            DSPlayer_Stop(state->pDSPlayer);
            DSPlayer_Open(state->pDSPlayer, nextFile);
            DSPlayer_Play(state->pDSPlayer);
        } else {
            MFPlayer_Stop(state->pMFPlayer);
            MFPlayer_Open(state->pMFPlayer, nextFile);
            MFPlayer_Play(state->pMFPlayer);
        }
        ApplyVolume(state);
        _tcsncpy(state->filePath, nextFile, MAX_PATH - 1);
        state->duration = state->useDirectShow ?
            DSPlayer_GetDuration(state->pDSPlayer) :
            MFPlayer_GetDuration(state->pMFPlayer);
        state->isPlaying = TRUE;
        UpdatePlaylist(state);
        UpdateStatus(state);
        UpdateSeekbar(state);
    } else {
        PostMessage(state->hMainWnd, WM_COMMAND, MAKELONG(0, itm_next), (LPARAM)state->hMainWnd);
    }
    return 0;
}
```

---

## DEFECT 5 — DSPlayer_Open Missing EventThread Teardown

### Location
`src/ds_player.cpp` lines 87–144, `DSPlayer_Open()`

### Description
When `DSPlayer_Open` is called for a second file (e.g., track change), it calls `DSPlayer_Stop()` at line 96, but **does NOT stop or wait for the `hEventThread`** before releasing `pEvent` at line 106. The thread then calls `p->pEvent->WaitForCompletion(100, &evCode)` on a released/null `pEvent`, causing a use-after-free crash or undefined behavior.

Compare with `MFPlayer_Open` (line 87–93 in mf_player.cpp), which correctly tears down the event thread first.

### Fix
In `DSPlayer_Open`, before the COM release block, add thread teardown (same pattern as `MFPlayer_Open`):

```cpp
// AFTER DSPlayer_Stop(player); (line 96), ADD:
if (p->hEventThread) {
    p->stopThread = TRUE;
    WaitForSingleObject(p->hEventThread, 2000);
    CloseHandle(p->hEventThread);
    p->hEventThread = NULL;
    p->stopThread = FALSE;
}
```

---

## DEFECT 6 — COM Not Initialized for MFPlayer Thread

### Location
`src/ds_player.cpp` line 91–94, `src/mf_player.cpp` — absent

### Description
`DSPlayer_Open` calls `CoInitializeEx` guarded by a global `g_comInitialized` flag (line 91–94). But `MFPlayer_Open` (`mf_player.cpp`) has no `CoInitialize` call at all. More critically, both players create COM objects (`CoCreateInstance`) inside the `EventThread` callback indirectly (through `OnMFEnd` → `DSPlayer_Open`/`MFPlayer_Open`), but `EventThread` never calls `CoInitialize`.

The global `g_comInitialized` flag in `ds_player.cpp` also has a race condition: two threads could evaluate `!g_comInitialized` simultaneously and both call `CoInitializeEx`.

### Fix
1. Call `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)` at the top of `EventThread` in both files, and `CoUninitialize()` before thread exit.
2. Remove the global `g_comInitialized` flag and its guarded check. The main thread's COM apartment is TC's responsibility.

```cpp
// In EventThread (both mf_player.cpp and ds_player.cpp):
static DWORD WINAPI EventThread(LPVOID lpParam) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    // ... existing loop ...
    CoUninitialize();
    return 0;
}
```

3. In `DSPlayer_Open`, remove lines 91–94 entirely.

---

## DEFECT 7 — `itm_next` Sent with Wrong wParam Encoding

### Location
`src/dllmain.cpp` line 86

### Description
```cpp
PostMessage(state->hMainWnd, WM_COMMAND, MAKELONG(0, itm_next), (LPARAM)state->hMainWnd);
```
`itm_next = 0xFFFA`. The TC SDK documents `ListNotificationReceived` as receiving a `WM_COMMAND` where `LOWORD(wParam) = itm_next`. Here the code puts `itm_next` in the **high word** (as the notification code), not the low word (which is 0). TC will never see `LOWORD(wParam) == itm_next`.

### Fix
```cpp
// BEFORE (line 86):
PostMessage(state->hMainWnd, WM_COMMAND, MAKELONG(0, itm_next), (LPARAM)state->hMainWnd);

// AFTER:
PostMessage(GetParent(state->hMainWnd), WM_COMMAND, MAKELONG(itm_next, 0), (LPARAM)state->hMainWnd);
```
The message should be sent to the **parent** (TC lister window, `ParentWin`), not to `hMainWnd` itself. Store `ParentWin` in `PluginState`:
```cpp
// In PluginState struct, add:
HWND hParentWnd;

// In ListLoadW (after creating hWnd):
state->hParentWnd = ParentWin;
```

---

## DEFECT 8 — Dark Mode Not Implemented

### Location
`src/dllmain.cpp` — `ListLoadW` (line 708), `ListLoadNextW` (line 771)

### Description
The `ShowFlags` parameter carries `lcp_darkmode` (value 128) and `lcp_darkmodenative` (256) from TC. The code ignores these flags entirely. The `PluginState` struct has no `isDarkMode` field. The summary claims "Dark mode support — works" but it does not.

### Fix

**Step 1:** Add to `PluginState`:
```cpp
BOOL isDarkMode;
```

**Step 2:** In `ListLoadW`, after creating window and getting state:
```cpp
state->isDarkMode = (ShowFlags & lcp_darkmode) || (ShowFlags & lcp_darkmodenative);
```
And in `ListLoadNextW`:
```cpp
state->isDarkMode = (ShowFlags & lcp_darkmode) || (ShowFlags & lcp_darkmodenative);
```

**Step 3:** Add a helper `ApplyTheme(state)` called from `ListLoadW`, `ListLoadNextW`, and `WM_COMMAND/lc_newparams`:
```cpp
static void ApplyTheme(PluginState* state) {
    COLORREF bg  = state->isDarkMode ? RGB(28, 28, 28) : GetSysColor(COLOR_BTNFACE);
    COLORREF fg  = state->isDarkMode ? RGB(230, 230, 230) : GetSysColor(COLOR_BTNTEXT);
    COLORREF sel = state->isDarkMode ? RGB(0, 84, 153)   : GetSysColor(COLOR_HIGHLIGHT);

    if (state->hBackBrush) DeleteObject(state->hBackBrush);
    state->hBackBrush = CreateSolidBrush(bg);

    if (state->hPlaylist) {
        ListView_SetBkColor(state->hPlaylist, bg);
        ListView_SetTextBkColor(state->hPlaylist, bg);
        ListView_SetTextColor(state->hPlaylist, fg);
        InvalidateRect(state->hPlaylist, NULL, TRUE);
    }
    InvalidateRect(state->hMainWnd, NULL, TRUE);
}
```

**Step 4:** Handle `WM_CTLCOLORLISTBOX`, `WM_CTLCOLORSTATIC` in `cbNewMain` to return the dark brush for child controls.

---

## DEFECT 9 — Manifest Not Embedded → Old-Style Controls

### Location
`CMakeLists.txt` — source list (lines 9–13), `src/app.manifest` exists but is not compiled

### Description
`app.manifest` is present in `src/` but is not included in the `add_library` source list. Without an embedded manifest, the DLL will use old-style Common Controls (Windows 95 appearance: 3D borders, beveled trackbars, etc.) even on Windows 11, because Common Controls v6 requires the manifest to be embedded in the binary.

### Fix

**Step 1:** Create `src/resources.rc`:
```rc
#include <winuser.h>
// Manifest resource ID 2 = DLL manifest
2 RT_MANIFEST "app.manifest"
```

**Step 2:** In `CMakeLists.txt`, add `src/resources.rc` to sources:
```cmake
add_library(MediaShow2 SHARED
    src/dllmain.cpp
    src/mf_player.cpp
    src/ds_player.cpp
    src/resources.rc       # ← ADD THIS
)
```

**Expected result:** All controls (trackbars, buttons, listview) render with the modern Fluent/Vista appearance on Windows 10/11.

---

## DEFECT 10 — Video Aspect Ratio Not Preserved

### Location
`src/mf_player.cpp` lines 240–253 (`MFPlayer_UpdateVideoWindow`), `src/ds_player.cpp` lines 235–240 (`DSPlayer_UpdateVideoWindow`)

### Description
Both `UpdateVideoWindow` implementations call:
```cpp
p->pVideoWindow->SetWindowPosition(0, 0, windowRect.right, windowRect.bottom);
```
This always stretches video to fill the entire container, ignoring the native video dimensions. The spec explicitly requires aspect ratio preservation with letterboxing (PROJECT_CONTEXT.md lines 253, 292–295).

### Fix

In both `mf_player.cpp` and `ds_player.cpp`, add `IBasicVideo` to the struct and query it in `Open()`:

```cpp
// In struct tagMFPlayer / tagDSPlayer, add:
IBasicVideo* pBasicVideo;

// In Open(), after QueryInterface for pVideoWindow, add:
p->pGraph->QueryInterface(IID_IBasicVideo, (void**)&p->pBasicVideo);
```

In `UpdateVideoWindow` (both files), replace the single `SetWindowPosition` call with aspect-ratio-correct math:

```cpp
void MFPlayer_UpdateVideoWindow(MFPlayer* player, RECT* rc) {
    if (!player) return;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pVideoWindow) return;

    RECT wrc;
    if (rc) wrc = *rc;
    else { GetClientRect(p->hVideoWnd, &wrc); }

    int cw = wrc.right  - wrc.left;
    int ch = wrc.bottom - wrc.top;

    int vx = 0, vy = 0, vw = cw, vh = ch;

    if (p->pBasicVideo) {
        long natW = 0, natH = 0;
        if (SUCCEEDED(p->pBasicVideo->get_VideoWidth(&natW)) &&
            SUCCEEDED(p->pBasicVideo->get_VideoHeight(&natH)) &&
            natW > 0 && natH > 0) {

            double ar = (double)natW / natH;
            double cAr = (double)cw / ch;

            if (ar > cAr) {          // letterbox (black bars top+bottom)
                vw = cw;
                vh = (int)(cw / ar);
                vy = (ch - vh) / 2;
            } else {                  // pillarbox (black bars left+right)
                vh = ch;
                vw = (int)(ch * ar);
                vx = (cw - vw) / 2;
            }
        }
    }
    p->pVideoWindow->SetWindowPosition(vx, vy, vw, vh);
}
```

Apply identical change to `DSPlayer_UpdateVideoWindow`.

Also in `VideoWndProc` (`dllmain.cpp` line 386–387), paint the background black:
```cpp
case WM_ERASEBKGND: {
    HDC hdc = (HDC)wParam;
    RECT rc;
    GetClientRect(hWnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    return 1;
}
```

---

## DEFECT 11 — Playlist Is Not Editable (No Double-Click, No Delete, No Reorder)

### Location
`src/dllmain.cpp` — `cbNewMain` switch, no `WM_NOTIFY` handler

### Description
The ListView playlist has no interaction:
- Double-click on an item does nothing (no `NM_DBLCLK` handler).
- `Del` key does nothing (no `LVN_KEYDOWN` handler).
- There is no "Move Up / Move Down" in the context menu.
- There is no "Add Files" option.
- The `IDM_PLAYLIST` command (line 624) shows a `MessageBox` listing filenames — effectively dead UI code.

The spec requires playlist editing: add/remove/reorder (PROJECT_CONTEXT.md lines 172–175).

### Fix

Add a `WM_NOTIFY` case to `cbNewMain`:

```cpp
case WM_NOTIFY: {
    if (!state) break;
    NMHDR* hdr = (NMHDR*)lParam;
    if (hdr->hwndFrom != state->hPlaylist) break;

    if (hdr->code == NM_DBLCLK) {
        int idx = ListView_GetNextItem(state->hPlaylist, -1, LVNI_SELECTED);
        if (idx >= 0 && idx < state->playlistCount) {
            state->playlistIndex = idx;
            TCHAR* f = state->playlist[idx];
            if (state->useDirectShow) {
                DSPlayer_Stop(state->pDSPlayer);
                DSPlayer_Open(state->pDSPlayer, f);
                DSPlayer_Play(state->pDSPlayer);
            } else {
                MFPlayer_Stop(state->pMFPlayer);
                MFPlayer_Open(state->pMFPlayer, f);
                MFPlayer_Play(state->pMFPlayer);
            }
            ApplyVolume(state);
            _tcsncpy(state->filePath, f, MAX_PATH - 1);
            state->duration = state->useDirectShow ?
                DSPlayer_GetDuration(state->pDSPlayer) :
                MFPlayer_GetDuration(state->pMFPlayer);
            state->isPlaying = TRUE;
            state->isPaused  = FALSE;
            UpdatePlaylist(state);
            UpdateStatus(state);
        }
    }

    if (hdr->code == LVN_KEYDOWN) {
        NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lParam;
        if (kd->wVKey == VK_DELETE) {
            int idx = ListView_GetNextItem(state->hPlaylist, -1, LVNI_SELECTED);
            if (idx >= 0 && idx < state->playlistCount) {
                free(state->playlist[idx]);
                memmove(&state->playlist[idx], &state->playlist[idx + 1],
                    (state->playlistCount - idx - 1) * sizeof(TCHAR*));
                state->playlistCount--;
                if (state->playlistIndex >= state->playlistCount)
                    state->playlistIndex = state->playlistCount - 1;
                UpdatePlaylist(state);
            }
        }
    }
    break;
}
```

Add "Move Up" / "Move Down" to `ShowContextMenu`, and implement swapping items in the playlist array. Add "Add Files" using `GetOpenFileName`.

---

## DEFECT 12 — `ScanDirectoryForMedia` Memory Leak on Empty Results

### Location
`src/dllmain.cpp` lines 108–120

### Description
```cpp
files = (TCHAR**)calloc(allocSize, sizeof(TCHAR*));  // allocated
HANDLE hFind = FindFirstFile(searchPath, &fd);
if (hFind == INVALID_HANDLE_VALUE) {
    free(files);   // freed correctly
    *outFiles = NULL;
    *outCount = 0;
    return;
}
```
This path is fine. However, if `FindFirstFile` succeeds but finds **zero matching files** (all files are non-media), the function exits the `do...while` loop normally. At that point `count == 0`, but `files` is still a non-null allocated array of 64 pointers — and is returned to `BuildPlaylist` where `*outCount == 0` causes the caller to fall into the single-file fallback (lines 155–161) **without freeing `files`**. This is a memory leak every time TC opens a media file in an otherwise non-media-file folder.

### Fix
At the end of `ScanDirectoryForMedia`, before `*outFiles = files`:
```cpp
if (count == 0) {
    free(files);
    *outFiles = NULL;
    *outCount = 0;
    return;
}
```

---

## DEFECT 13 — `realloc` Return Value Not Checked

### Location
`src/dllmain.cpp` line 130

### Description
```cpp
files = (TCHAR**)realloc(files, allocSize * sizeof(TCHAR*));
```
`realloc` can return `NULL` on allocation failure. In that case, the old pointer is lost (memory leak) and `files[count]` on the next line is a null-pointer dereference (crash).

### Fix
```cpp
TCHAR** tmp = (TCHAR**)realloc(files, allocSize * sizeof(TCHAR*));
if (!tmp) { /* handle: break out, use current list */ break; }
files = tmp;
```

---

## DEFECT 14 — Dead Code: `WM_LBUTTONDBLCLK` Handler

### Location
`src/dllmain.cpp` line 388–389

### Description
```cpp
case WM_LBUTTONDBLCLK:
    break;
```
This is explicitly handled (intercepting the message from `DefSubclassProc`) but does nothing. The spec requires double-click to toggle fullscreen. Currently it silently swallows the double-click event.

### Fix
Replace `break` with the fullscreen toggle call (to be implemented, see Defect 15):
```cpp
case WM_LBUTTONDBLCLK:
    SendMessage(state->hMainWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
    return 0;
```
Add `IDM_FULLSCREEN` to `plugin_api.h` and implement the handler.

---

## DEFECT 15 — Fullscreen Mode Not Implemented

### Location
`src/dllmain.cpp` — entire file, no fullscreen logic

### Description
F11 key does nothing (not in any keyboard handler). Double-click does nothing (DEFECT 14). The spec requires fullscreen (PROJECT_CONTEXT.md line 254).

### Fix (minimal, reparenting approach)

Add to `PluginState`:
```cpp
HWND hFullscreenWnd;
BOOL isFullscreen;
```

Register a fullscreen class and implement toggle:
```cpp
static void ToggleFullscreen(PluginState* state) {
    if (!state->isFullscreen) {
        // Get current monitor
        HMONITOR hMon = MonitorFromWindow(state->hMainWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        RECT& r = mi.rcMonitor;

        // Create borderless popup
        state->hFullscreenWnd = CreateWindowEx(WS_EX_TOPMOST, TEXT("MediaShow2Main"),
            TEXT(""), WS_POPUP | WS_VISIBLE,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            NULL, NULL, GetModuleHandle(0), NULL);

        SetParent(state->hVideoWnd, state->hFullscreenWnd);
        MoveWindow(state->hVideoWnd, 0, 0, r.right - r.left, r.bottom - r.top, TRUE);
        ShowCursor(FALSE);
        state->isFullscreen = TRUE;
    } else {
        SetParent(state->hVideoWnd, state->hMainWnd);
        DestroyWindow(state->hFullscreenWnd);
        state->hFullscreenWnd = NULL;
        ShowCursor(TRUE);
        state->isFullscreen = FALSE;
        UpdateLayout(state);
    }
    // Update video renderer position
    if (state->useDirectShow)
        DSPlayer_UpdateVideoWindow(state->pDSPlayer, NULL);
    else
        MFPlayer_UpdateVideoWindow(state->pMFPlayer, NULL);
}
```

Add to `WM_COMMAND` and to a keyboard handler (F11 = VK_F11, Esc in fullscreen).

---

## DEFECT 16 — Keyboard Handler Missing (WM_KEYDOWN Not Processed)

### Location
`src/dllmain.cpp` — `cbNewMain` switch — no `WM_KEYDOWN` case

### Description
The spec defines 10 keyboard shortcuts (Space, S, ←, →, ↑, ↓, M, F11, Ctrl+T, L, I, Esc). The main window procedure has zero `WM_KEYDOWN` handling. TC does subclass the plugin window for 'n'/'p' (documented in PROJECT_CONTEXT.md), but other keys must be handled via `WM_KEYDOWN` in `cbNewMain`.

### Fix
Add to `cbNewMain`:
```cpp
case WM_KEYDOWN:
    if (!state) break;
    switch (wParam) {
    case VK_SPACE:
        SendMessage(hWnd, WM_COMMAND, IDM_PLAY, 0);
        return 0;
    case 'S':
        SendMessage(hWnd, WM_COMMAND, IDM_STOP, 0);
        return 0;
    case VK_LEFT:
        SendMessage(hWnd, WM_COMMAND, IDM_SEEK_BACK, 0);
        return 0;
    case VK_RIGHT:
        SendMessage(hWnd, WM_COMMAND, IDM_SEEK_FWD, 0);
        return 0;
    case VK_UP:
        SendMessage(hWnd, WM_COMMAND, IDM_VOL_UP, 0);
        return 0;
    case VK_DOWN:
        SendMessage(hWnd, WM_COMMAND, IDM_VOL_DOWN, 0);
        return 0;
    case 'M':
        SendMessage(hWnd, WM_COMMAND, IDM_MUTE, 0);
        return 0;
    case VK_F11:
        SendMessage(hWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        return 0;
    case VK_ESCAPE:
        if (state->isFullscreen)
            SendMessage(hWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        return 0;
    case 'L':
        SendMessage(hWnd, WM_COMMAND, IDM_SHOWPLAYLIST, 0);
        return 0;
    }
    break;
```

> **TC Note:** TC intercepts Tab, F3, and some other keys at the lister level before they reach the plugin. Space, S, M, F11, Esc, L are safe.

---

## DEFECT 17 — Always On Top Not Implemented

### Location
`src/dllmain.cpp` — missing `IDM_ALWAYSONTOP` handler; `src/plugin_api.h` — no constant

### Fix
Add to `plugin_api.h`:
```cpp
#define IDM_ALWAYSONTOP  4050
```

Add to `PluginState`:
```cpp
BOOL isAlwaysOnTop;
```

Add to context menu in `ShowContextMenu`:
```cpp
AppendMenu(hMenu, state->isAlwaysOnTop ? MF_CHECKED : MF_STRING, IDM_ALWAYSONTOP, TEXT("Always on Top\tCtrl+T"));
```

Add to `WM_COMMAND` handler:
```cpp
case IDM_ALWAYSONTOP:
    state->isAlwaysOnTop = !state->isAlwaysOnTop;
    SetWindowPos(state->hMainWnd, state->isAlwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    break;
```

Add `Ctrl+T` in `WM_KEYDOWN` handler:
```cpp
case 'T':
    if (GetKeyState(VK_CONTROL) & 0x8000)
        SendMessage(hWnd, WM_COMMAND, IDM_ALWAYSONTOP, 0);
    return 0;
```

---

## DEFECT 18 — Modern UI / Fluent Icons Not Applied

### Location
`src/dllmain.cpp` lines 348–363, `CreateControls()` toolbar button setup

### Description
Toolbar buttons show text labels ("Prev", "Play", "Stop", etc.) using the default system font. The spec requires modern Fluent Design UI. On Windows 10/11, Microsoft provides `Segoe MDL2 Assets` and `Segoe Fluent Icons` (Win 11) fonts containing vector UI icons at Unicode codepoints.

### Fix

**Step 1:** In `CreateControls()`, after creating the toolbar, set its font to Segoe MDL2 Assets with a larger point size:
```cpp
HFONT hIconFont = CreateFont(
    -20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
    TEXT("Segoe Fluent Icons"));  // falls back to Segoe MDL2 Assets on Win10
if (!hIconFont)
    hIconFont = CreateFont(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        TEXT("Segoe MDL2 Assets"));
SendMessage(state->hToolbar, WM_SETFONT, (WPARAM)hIconFont, TRUE);
```

**Step 2:** Replace string labels in `TBBUTTON` with icon codepoints:
```cpp
TBBUTTON buttons[] = {
    // iString values are Unicode icon codepoints as string pointers
    { 0, IDM_PREV,      TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"\uE892" },  // Previous
    { 0, IDM_PLAY,      TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"\uE768" },  // Play
    { 0, IDM_STOP,      TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"\uE71A" },  // Stop
    { 0, IDM_NEXT,      TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"\uE893" },  // Next
    { 1, 0,             TBSTATE_ENABLED, BTNS_SEP,    {0}, 0, 0 },
    { 0, IDM_SEEK_BACK, TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"\uEB9E" },  // Rewind
    { 0, IDM_SEEK_FWD,  TBSTATE_ENABLED, BTNS_BUTTON, {0}, 0, (INT_PTR)L"\uEB9F" },  // Fast forward
};
```

**Step 3:** Enable `BTNS_SHOWTEXT` and `TBSTYLE_LIST` on the toolbar, or use `TBSTYLE_TOOLTIPS` with `TB_SETTOOLTIPS` only, so glyphs show without text labels.

---

## DEFECT 19 — `hListerWnd` Parameter of `BuildPlaylist` Is Ignored

### Location
`src/dllmain.cpp` line 143, function signature

### Description
```cpp
static void BuildPlaylist(PluginState* state, HWND hListerWnd, TCHAR* currentFile) {
```
`hListerWnd` is never used inside the function. It was presumably intended to walk TC's window hierarchy to find selected files (PROJECT_CONTEXT.md line 409). Currently only a directory scan is done. The parameter is dead.

### Fix (short term)
Remove the unused parameter to clean up the call sites, or keep it with a comment explaining it's reserved for future TC selection querying.

### Future Enhancement
TC does not provide a public API to get selected files from a lister plugin. The best achievable alternative is reading the clipboard after sending `cm_CopyFullNamesToClip` via `WM_COMMAND` to the TC main window. This is non-trivial but documented in PROJECT_CONTEXT.md line 289 as option 2.

---

## DEFECT 20 — `ShowFlags` Not Propagated to State, Dark Mode Check Missing in `ListSendCommand`

### Location
`src/dllmain.cpp` line 844–858, `ListSendCommand`

### Description
When TC sends `lc_newparams` (e.g., dark mode toggled), the handler at line 847 just invalidates the window. It does not re-read `ShowFlags` or update `isDarkMode`. The visual will not update.

### Fix
In `ListSendCommand`, for `lc_newparams`, re-apply theme:
```cpp
if (Command == lc_newparams) {
    state->isDarkMode = (Parameter & lcp_darkmode) || (Parameter & lcp_darkmodenative);
    ApplyTheme(state);
    InvalidateRect(ListWin, NULL, TRUE);
    return LISTPLUGIN_OK;
}
```

---

## DEFECT 21 — `WM_SETFOCUS` Passes Focus to `hVideoWnd` Unconditionally

### Location
`src/dllmain.cpp` line 511–513

### Description
```cpp
case WM_SETFOCUS:
    if (state && state->hVideoWnd) SetFocus(state->hVideoWnd);
    return 0;
```
When the playlist is visible, focus should stay on `hPlaylist` so keyboard navigation (arrows, Enter, Delete) works. Forcing focus to `hVideoWnd` always breaks playlist keyboard interaction.

### Fix
```cpp
case WM_SETFOCUS:
    if (state) {
        if (state->showPlaylist && state->hPlaylist)
            SetFocus(state->hPlaylist);
        else if (state->hVideoWnd)
            SetFocus(state->hVideoWnd);
    }
    return 0;
```

---

## Prioritized Implementation Order

| Priority | Defect | Impact |
|---|---|---|
| P0 | **#1** Toolbar CCS_TOP | Seekbar/volume completely broken |
| P0 | **#4** Race condition OnMFEnd | Crash on track completion |
| P0 | **#5** DSPlayer thread teardown | Use-after-free crash on track change |
| P0 | **#7** itm_next wrong wParam | Auto-advance to next file broken |
| P1 | **#2** Volume jump | Fundamental audio UX broken |
| P1 | **#3** Mouse wheel inversion + seek | Wrong UX |
| P1 | **#9** Manifest not embedded | Old-style controls everywhere |
| P1 | **#10** Aspect ratio | Video visually distorted |
| P1 | **#11** Playlist not interactive | Core feature missing |
| P1 | **#16** No keyboard handler | Core feature missing |
| P2 | **#8** Dark mode not implemented | Summary claims it works; it doesn't |
| P2 | **#14** Double-click swallowed | Fullscreen entry broken |
| P2 | **#15** Fullscreen not implemented | Core feature missing |
| P2 | **#17** Always on Top missing | Spec feature missing |
| P2 | **#18** Fluent icons missing | UI looks outdated |
| P2 | **#20** Dark mode not re-applied on lc_newparams | Theme switch broken |
| P3 | **#6** COM not init in thread | Latent instability |
| P3 | **#12** Memory leak empty dir | Minor leak |
| P3 | **#13** realloc not checked | Potential crash |
| P3 | **#19** Dead hListerWnd param | Cleanup |
| P3 | **#21** WM_SETFOCUS always to video | Playlist keyboard broken |

---

## Files to Modify

| File | Changes |
|---|---|
| `src/dllmain.cpp` | Defects 1, 2, 3, 4, 7, 8, 10(partial), 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21 |
| `src/mf_player.cpp` | Defects 5(already OK), 6, 10 |
| `src/ds_player.cpp` | Defects 5, 6, 10 |
| `src/plugin_api.h` | Defects 4 (WM_PLAYER_TRACK_END), 17 (IDM_ALWAYSONTOP), 15 (IDM_FULLSCREEN) |
| `src/resources.rc` | Defect 9 — new file |
| `CMakeLists.txt` | Defect 9 — add resources.rc |
