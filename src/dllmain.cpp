#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <tchar.h>
#include <stdlib.h>
#include <stdio.h>
#include "plugin_api.h"
#include "mf_player.h"
#include "ds_player.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

#ifndef min
#define min(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef max
#define max(a,b) ((a)>(b)?(a):(b))
#endif

#define WM_DEFERRED_GETFILES (WM_APP + 100)

static TCHAR iniPath[MAX_PATH] = {0};
static ATOM  mainWndClass      = 0;
static ATOM  fullscreenWndClass = 0;

/* -----------------------------------------------------------------------
   Plugin State
   ----------------------------------------------------------------------- */
struct PluginState {
    HWND hMainWnd;
    HWND hParentWnd;        // TC lister window (for itm_next)
    HWND hVideoWnd;
    HWND hPlaylist;
    HWND hToolbar;
    HWND hStatus;
    HWND hSeekbar;
    HWND hVolSlider;
    HWND hFullscreenWnd;    // borderless popup when in fullscreen
    MFPlayer* pMFPlayer;
    DSPlayer* pDSPlayer;
    BOOL  useDirectShow;
    TCHAR filePath[MAX_PATH];
    BOOL  isPlaying;
    BOOL  isPaused;
    BOOL  isMuted;
    BOOL  showPlaylist;
    BOOL  isFullscreen;
    BOOL  isDarkMode;
    DWORD lastClickTime;
    double duration;
    double position;
    double videoAr;          // native video aspect ratio (0 = unknown)
    int   volume;
    int   repeatMode;       // 0=off, 1=all, 2=one
    TCHAR** playlist;
    FILETIME* fileDates;
    int   playlistCount;
    int   playlistIndex;
    int   sortColumn;
    BOOL  sortAscending;
    HFONT hFont;
    HFONT hIconFont;        // Segoe Fluent Icons / MDL2 Assets for toolbar
    HBRUSH hBackBrush;
};

static PluginState* GetState(HWND hWnd) {
    return (PluginState*)GetProp(hWnd, TEXT("STATE"));
}

/* -----------------------------------------------------------------------
   Playlist helpers
   ----------------------------------------------------------------------- */
static void FreePlaylist(PluginState* state) {
    if (!state || !state->playlist) return;
    for (int i = 0; i < state->playlistCount; i++)
        free(state->playlist[i]);
    free(state->playlist);
    free(state->fileDates);
    state->playlist      = NULL;
    state->fileDates     = NULL;
    state->playlistCount = 0;
    state->playlistIndex = 0;
}

/* -----------------------------------------------------------------------
   Volume helpers (Defect #2 fix)
   ----------------------------------------------------------------------- */
static void ApplyVolume(PluginState* state) {
    if (!state) return;
    int vol = state->isMuted ? 0 : state->volume;
    if (state->useDirectShow)
        DSPlayer_SetVolume(state->pDSPlayer, vol);
    else
        MFPlayer_SetVolume(state->pMFPlayer, vol / 100.0f);
}

static void SaveVolume(PluginState* state) {
    if (!state || iniPath[0] == 0) return;
    TCHAR buf[16];
    _sntprintf(buf, 16, TEXT("%d"), state->volume);
    WritePrivateProfileString(TEXT("MediaShow2"), TEXT("Volume"), buf, iniPath);
}

static int LoadVolume(void) {
    if (iniPath[0] == 0) return 80;
    return (int)GetPrivateProfileInt(TEXT("MediaShow2"), TEXT("Volume"), 80, iniPath);
}

static void SaveRepeatMode(PluginState* state) {
    if (!state || iniPath[0] == 0) return;
    TCHAR buf[16];
    _sntprintf(buf, 16, TEXT("%d"), state->repeatMode);
    WritePrivateProfileString(TEXT("MediaShow2"), TEXT("RepeatMode"), buf, iniPath);
}

static int LoadRepeatMode(void) {
    if (iniPath[0] == 0) return 1;
    return (int)GetPrivateProfileInt(TEXT("MediaShow2"), TEXT("RepeatMode"), 1, iniPath);
}

static const TCHAR* GetRepeatLabel(int mode) {
    switch (mode) {
    case 1:  return L"\u21BB";     // ↻ Repeat All
    case 2:  return L"\u21BB\u2081"; // ↻₁ Repeat One
    default: return L"\u25CB";     // ○ Off
    }
}

/* -----------------------------------------------------------------------
   Track-end callback (Defect #4 fix):
   Called from EventThread — must NOT touch state directly.
   Simply posts WM_PLAYER_TRACK_END to the UI thread.
   ----------------------------------------------------------------------- */
static void OnMFEnd(void* userData) {
    PluginState* state = (PluginState*)userData;
    if (!state || !state->hMainWnd) return;
    PostMessage(state->hMainWnd, WM_PLAYER_TRACK_END, 0, 0);
}

/* -----------------------------------------------------------------------
   Media file detection
   ----------------------------------------------------------------------- */
static BOOL IsMediaFile(const TCHAR* ext) {
    static const TCHAR* mediaExts[] = {
        TEXT("avi"), TEXT("mpg"), TEXT("mpeg"), TEXT("asf"), TEXT("vob"),
        TEXT("mp1"), TEXT("mp2"), TEXT("mp3"), TEXT("wav"), TEXT("ogg"),
        TEXT("wma"), TEXT("dat"), TEXT("mkv"), TEXT("webm"), TEXT("mp4"),
        TEXT("m4a"), TEXT("flac"), TEXT("aac"), TEXT("opus"), TEXT("mid"),
        TEXT("midi"), TEXT("kar"), NULL
    };
    for (int i = 0; mediaExts[i]; i++)
        if (_tcsicmp(ext, mediaExts[i]) == 0) return TRUE;
    return FALSE;
}

static BOOL IsAudioOnly(const TCHAR* filePath) {
    const TCHAR* dot = _tcsrchr(filePath, TEXT('.'));
    if (!dot) return FALSE;
    dot++;
    static const TCHAR* audioExts[] = {
        TEXT("mp3"), TEXT("wav"), TEXT("ogg"), TEXT("wma"), TEXT("flac"),
        TEXT("aac"), TEXT("opus"), TEXT("mid"), TEXT("midi"), TEXT("kar"),
        TEXT("mp1"), TEXT("mp2"), NULL
    };
    for (int i = 0; audioExts[i]; i++)
        if (_tcsicmp(dot, audioExts[i]) == 0) return TRUE;
    return FALSE;
}

/* -----------------------------------------------------------------------
   Directory scan (Defect #12 + #13 fix)
   ----------------------------------------------------------------------- */
static void ScanDirectoryForMedia(TCHAR* dir, TCHAR*** outFiles, FILETIME** outDates, int* outCount) {
    TCHAR searchPath[MAX_PATH];
    _sntprintf(searchPath, MAX_PATH, TEXT("%s\\*.*"), dir);

    int allocSize = 64;
    int count     = 0;
    TCHAR** files = (TCHAR**)calloc(allocSize, sizeof(TCHAR*));
    FILETIME* dates = (FILETIME*)calloc(allocSize, sizeof(FILETIME));
    if (!files || !dates) { free(files); free(dates); *outFiles = NULL; *outDates = NULL; *outCount = 0; return; }

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        free(files); free(dates);
        *outFiles = NULL; *outDates = NULL; *outCount = 0;
        return;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        TCHAR* dot = _tcsrchr(fd.cFileName, TEXT('.'));
        if (!dot || !IsMediaFile(dot + 1)) continue;

        if (count >= allocSize) {
            allocSize *= 2;
            TCHAR** tmp = (TCHAR**)realloc(files, allocSize * sizeof(TCHAR*));
            FILETIME* tmpD = (FILETIME*)realloc(dates, allocSize * sizeof(FILETIME));
            if (!tmp || !tmpD) break;
            files = tmp; dates = tmpD;
        }

        TCHAR fullPath[MAX_PATH];
        _sntprintf(fullPath, MAX_PATH, TEXT("%s\\%s"), dir, fd.cFileName);
        files[count] = _tcsdup(fullPath);
        dates[count] = fd.ftCreationTime;
        count++;

    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);

    if (count == 0) {
        free(files); free(dates);
        *outFiles = NULL; *outDates = NULL; *outCount = 0;
        return;
    }
    *outFiles = files;
    *outDates = dates;
    *outCount = count;
}

static void BuildPlaylistFromSelection(PluginState* state, TCHAR** selFiles, int selCount, TCHAR* currentFile) {
    FreePlaylist(state);
    state->playlist      = selFiles;
    state->playlistCount = selCount;
    state->playlistIndex = 0;
    state->fileDates     = (FILETIME*)calloc(selCount, sizeof(FILETIME));
    for (int i = 0; i < selCount; i++) {
        if (_tcsicmp(selFiles[i], currentFile) == 0) {
            state->playlistIndex = i;
            break;
        }
    }
}

static void BuildPlaylist(PluginState* state, HWND /*hListerWnd*/, TCHAR* currentFile) {
    FreePlaylist(state);

    TCHAR dir[MAX_PATH];
    _tcsncpy(dir, currentFile, MAX_PATH - 1);
    TCHAR* lastSlash = _tcsrchr(dir, TEXT('\\'));
    if (lastSlash) *lastSlash = 0;

    TCHAR** files = NULL;
    FILETIME* dates = NULL;
    int     count = 0;
    ScanDirectoryForMedia(dir, &files, &dates, &count);

    if (!files || count == 0) {
        state->playlist      = (TCHAR**)calloc(1, sizeof(TCHAR*));
        state->fileDates     = (FILETIME*)calloc(1, sizeof(FILETIME));
        state->playlist[0]   = _tcsdup(currentFile);
        state->playlistCount = 1;
        state->playlistIndex = 0;
        return;
    }

    state->playlist      = files;
    state->fileDates     = dates;
    state->playlistCount = count;
    state->playlistIndex = 0;

    for (int i = 0; i < count; i++) {
        if (_tcsicmp(state->playlist[i], currentFile) == 0) {
            state->playlistIndex = i;
            break;
        }
    }
}

/* -----------------------------------------------------------------------
   Playlist sorting
   ----------------------------------------------------------------------- */
static TCHAR**   g_sort_pl  = NULL;
static FILETIME* g_sort_dt  = NULL;
static int       g_sort_col = 0;
static BOOL      g_sort_asc = TRUE;

static int __cdecl PlaylistCmp(void* ctx, const void* a, const void* b) {
    (void)ctx;
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    int r = 0;
    switch (g_sort_col) {
    case 1: r = _tcsicmp(g_sort_pl[ia], g_sort_pl[ib]); break;
    case 2: r = IsAudioOnly(g_sort_pl[ia]) - IsAudioOnly(g_sort_pl[ib]); break;
    case 3: r = CompareFileTime(&g_sort_dt[ia], &g_sort_dt[ib]); break;
    default: r = ia - ib; break;
    }
    return g_sort_asc ? r : -r;
}

static void SortPlaylist(PluginState* state) {
    if (state->sortColumn < 0 || state->sortColumn > 3) return;
    if (state->playlistCount <= 1) return;

    int* idx = (int*)calloc(state->playlistCount, sizeof(int));
    if (!idx) return;
    for (int i = 0; i < state->playlistCount; i++) idx[i] = i;

    g_sort_pl  = state->playlist;
    g_sort_dt  = state->fileDates;
    g_sort_col = state->sortColumn;
    g_sort_asc = state->sortAscending;
    qsort_s(idx, state->playlistCount, sizeof(int), PlaylistCmp, NULL);

    TCHAR** newPl = (TCHAR**)calloc(state->playlistCount, sizeof(TCHAR*));
    FILETIME* newDt = (FILETIME*)calloc(state->playlistCount, sizeof(FILETIME));
    if (!newPl || !newDt) { free(idx); free(newPl); free(newDt); return; }

    int newIdx = 0;
    for (int i = 0; i < state->playlistCount; i++) {
        newPl[newIdx]  = state->playlist[idx[i]];
        newDt[newIdx]  = state->fileDates[idx[i]];
        if (idx[i] == state->playlistIndex) state->playlistIndex = newIdx;
        newIdx++;
    }

    free(state->playlist);
    free(state->fileDates);
    state->playlist  = newPl;
    state->fileDates = newDt;
    free(idx);
}

/* -----------------------------------------------------------------------
   UI update helpers
   ----------------------------------------------------------------------- */
static void UpdatePlaylist(PluginState* state) {
    if (!state || !state->hPlaylist) return;
    ListView_DeleteAllItems(state->hPlaylist);

    if (!state->playlist || state->playlistCount == 0) {
        LVITEM lvi = {0};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = 0;
        TCHAR empty[] = TEXT("(empty)");
        lvi.pszText = empty;
        ListView_InsertItem(state->hPlaylist, &lvi);
        return;
    }

    for (int i = 0; i < state->playlistCount; i++) {
        TCHAR* fname = _tcsrchr(state->playlist[i], TEXT('\\'));
        fname = fname ? fname + 1 : state->playlist[i];

        TCHAR num[8]; _sntprintf(num, 8, TEXT("%d"), i + 1);
        BOOL audio = IsAudioOnly(state->playlist[i]);

        TCHAR display[MAX_PATH + 4];
        _sntprintf(display, MAX_PATH + 4,
            (i == state->playlistIndex) ? TEXT("\u25BA %s") : TEXT("  %s"), fname);

        LVITEM lvi = {0};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = i;
        lvi.pszText = num;
        ListView_InsertItem(state->hPlaylist, &lvi);
        ListView_SetItemText(state->hPlaylist, i, 1, display);
        ListView_SetItemText(state->hPlaylist, i, 2, audio ? TEXT("Audio") : TEXT("Video"));

        TCHAR dateBuf[32] = TEXT("");
        if (state->fileDates) {
            FILETIME localFt;
            SYSTEMTIME st;
            FileTimeToLocalFileTime(&state->fileDates[i], &localFt);
            if (FileTimeToSystemTime(&localFt, &st))
                _sntprintf(dateBuf, 32, TEXT("%04d-%02d-%02d %02d:%02d"),
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        }
        ListView_SetItemText(state->hPlaylist, i, 3, dateBuf);
    }

    if (state->playlistIndex >= 0 && state->playlistIndex < state->playlistCount)
        ListView_SetItemState(state->hPlaylist, state->playlistIndex,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

/* -----------------------------------------------------------------------
   Get selected files from TC
   Find LCLListBox in TTOTAL_CMD and try LB_GETSELITEMS
   ----------------------------------------------------------------------- */
struct EnumFindData { HWND result; DWORD processId; };

static BOOL CALLBACK EnumFindLCLListBox(HWND hWnd, LPARAM lParam) {
    EnumFindData* pfd = (EnumFindData*)lParam;
    TCHAR cls[128] = {0};
    GetClassName(hWnd, cls, 128);

    if (_tcscmp(cls, TEXT("LCLListBox")) == 0) {
        int selCount = (int)SendMessage(hWnd, LB_GETSELCOUNT, 0, 0);
        TCHAR dbg[256];
        _sntprintf(dbg, 256, TEXT("MediaShow2: LCLListBox %p LB_GETSELCOUNT=%d\n"), hWnd, selCount);
        OutputDebugString(dbg);

        if (selCount > 0 && !pfd->result) {
            pfd->result = hWnd;
        }

        // Also try owner data messages
        int count = (int)SendMessage(hWnd, LB_GETCOUNT, 0, 0);
        _sntprintf(dbg, 256, TEXT("MediaShow2: LCLListBox %p LB_GETCOUNT=%d\n"), hWnd, count);
        OutputDebugString(dbg);
    }
    return TRUE;
}

static void RequestSelectedFiles(HWND hListerWnd, PluginState* state) {
    HWND hTC = FindWindow(TEXT("TTOTAL_CMD"), NULL);
    if (!hTC) {
        OutputDebugString(TEXT("MediaShow2: TTOTAL_CMD NOT FOUND\n"));
        return;
    }

    // Find LCLListBox
    EnumFindData fd = {0, 0};
    EnumChildWindows(hTC, EnumFindLCLListBox, (LPARAM)&fd);

    if (!fd.result) {
        OutputDebugString(TEXT("MediaShow2: NO LCLListBox WITH SELECTION\n"));
        return;
    }

    HWND hListBox = fd.result;
    int selCount = (int)SendMessage(hListBox, LB_GETSELCOUNT, 0, 0);
    TCHAR dbg[256];
    _sntprintf(dbg, 256, TEXT("MediaShow2: Using LCLListBox sel=%d\n"), selCount);
    OutputDebugString(dbg);

    if (selCount <= 0) return;

    int* selItems = (int*)calloc(selCount, sizeof(int));
    SendMessage(hListBox, LB_GETSELITEMS, selCount, (LPARAM)selItems);

    TCHAR dir[MAX_PATH];
    _tcsncpy(dir, state->filePath, MAX_PATH - 1);
    TCHAR* lastSlash = _tcsrchr(dir, TEXT('\\'));
    if (lastSlash) *lastSlash = 0;

    TCHAR** files = (TCHAR**)calloc(selCount, sizeof(TCHAR*));
    state->fileDates = (FILETIME*)calloc(selCount, sizeof(FILETIME));
    int validCount = 0;

    for (int i = 0; i < selCount; i++) {
        int len = (int)SendMessage(hListBox, LB_GETTEXTLEN, selItems[i], 0);
        if (len <= 0) continue;
        TCHAR* buf = (TCHAR*)calloc(len + 1, sizeof(TCHAR));
        SendMessage(hListBox, LB_GETTEXT, selItems[i], (LPARAM)buf);

        TCHAR dbg[512];
        _sntprintf(dbg, 512, TEXT("MediaShow2: LB_GETTEXT[%d]='%s'\n"), i, buf);
        OutputDebugString(dbg);

        // TC format: "filename.ext NNN NNN NNN DD.MM.YYYY HH:MM -a--"
        // Right-to-left: find date, strip date+time+attrs, then strip size (3 digit groups)
        TCHAR* datePos = NULL;
        for (TCHAR* p = buf; p[9]; p++) {
            if (p[2] == TEXT('.') && p[5] == TEXT('.') &&
                p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
                p[3] >= '0' && p[3] <= '9' && p[4] >= '0' && p[4] <= '9' &&
                p[6] >= '0' && p[6] <= '9' && p[7] >= '0' && p[7] <= '9' &&
                p[8] >= '0' && p[8] <= '9' && p[9] >= '0' && p[9] <= '9') {
                datePos = p;
                break;
            }
        }

        TCHAR fileName[MAX_PATH] = {0};
        if (datePos) {
            int beforeLen = (int)(datePos - buf);
            _tcsncpy(fileName, buf, beforeLen);
            fileName[beforeLen] = TEXT('\0');
            // TC uses NBSP (0x00A0) and TAB (0x0009) as separators, not just spaces
            while (beforeLen > 0 && (fileName[beforeLen-1] == TEXT(' ') || fileName[beforeLen-1] == 0x00A0 || fileName[beforeLen-1] == 0x0009))
                fileName[--beforeLen] = TEXT('\0');
            // Strip 3 size groups from right: "NNN NBSP NNN NBSP NNN"
            TCHAR* p = fileName + beforeLen - 1;
            // Group 3 (rightmost)
            while (p > fileName && *p >= '0' && *p <= '9') p--;
            if (p > fileName && (*p == TEXT(' ') || *p == 0x00A0 || *p == 0x0009)) p--;
            else { p = fileName + beforeLen - 1; }
            // Group 2
            while (p > fileName && *p >= '0' && *p <= '9') p--;
            if (p > fileName && (*p == TEXT(' ') || *p == 0x00A0 || *p == 0x0009)) p--;
            else { p = fileName + beforeLen - 1; }
            // Group 1 (leftmost)
            while (p > fileName && *p >= '0' && *p <= '9') p--;
            if (p > fileName && (*p == TEXT(' ') || *p == 0x00A0 || *p == 0x0009)) p--;
            else { p = fileName + beforeLen - 1; }
            *(p + 1) = TEXT('\0');
            while (beforeLen > 0 && (fileName[beforeLen-1] == TEXT(' ') || fileName[beforeLen-1] == 0x00A0 || fileName[beforeLen-1] == 0x0009))
                fileName[--beforeLen] = TEXT('\0');
        } else {
            _tcsncpy(fileName, buf, MAX_PATH - 1);
        }

        TCHAR fullPath[MAX_PATH];
        _sntprintf(fullPath, MAX_PATH, TEXT("%s\\%s"), dir, fileName);

        // Skip unsupported formats
        TCHAR* dot = _tcsrchr(fileName, TEXT('.'));
        if (!dot || !IsMediaFile(dot + 1)) {
            free(buf);
            continue;
        }

        files[validCount] = _tcsdup(fullPath);

        // Get file date from filesystem
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesEx(fullPath, GetFileExInfoStandard, &fad)) {
            state->fileDates[validCount] = fad.ftLastWriteTime;
        }

        validCount++;
        free(buf);
    }
    free(selItems);

    if (validCount == 0) return;

    // Save fileDates before FreePlaylist destroys them
    FILETIME* savedDates = state->fileDates;
    state->fileDates = NULL;
    FreePlaylist(state);
    state->playlist      = files;
    state->playlistCount = validCount;
    state->playlistIndex = 0;
    state->fileDates     = savedDates;
    for (int i = 0; i < validCount; i++) {
        if (_tcsicmp(files[i], state->filePath) == 0) {
            state->playlistIndex = i;
            break;
        }
    }
    // Update showPlaylist based on current file type
    state->showPlaylist = IsAudioOnly(state->filePath);
    UpdatePlaylist(state);
}

static double GetVideoAspectRatio(PluginState* state) {
    if (!state) return 0;
    if (state->useDirectShow)
        return DSPlayer_GetAspectRatio(state->pDSPlayer);
    return MFPlayer_GetAspectRatio(state->pMFPlayer);
}

static void UpdateLayout(PluginState* state) {
    if (!state || !state->hMainWnd) return;
    RECT rc;
    GetClientRect(state->hMainWnd, &rc);
    int w = rc.right;
    int h = rc.bottom;
    if (w < 100 || h < 60) return;

    const int tbH     = 40;    // toolbar row height
    const int ctrlH   = 28;    // trackbar height
    const int statusH = 22;    // status bar height
    const int tbW     = 220;   // fixed toolbar width

    if (state->hToolbar) {
        MoveWindow(state->hToolbar, 0, 0, tbW, tbH, TRUE);
        SendMessage(state->hToolbar, TB_AUTOSIZE, 0, 0);
    }

    // Seekbar: fills space between toolbar and volume slider
    int seekX = tbW + 4;
    int volW  = 140;
    int volX  = w - volW - 4;
    int seekW = volX - seekX - 4;
    if (seekW < 40) seekW = 40;

    int trackY = (tbH - ctrlH) / 2;
    if (state->hSeekbar)
        SetWindowPos(state->hSeekbar, HWND_TOP, seekX, trackY, seekW, ctrlH, SWP_NOZORDER);
    if (state->hVolSlider)
        SetWindowPos(state->hVolSlider, HWND_TOP, volX, trackY, volW, ctrlH, SWP_NOZORDER);

    if (state->hStatus)
        MoveWindow(state->hStatus, 0, h - statusH, w, statusH, TRUE);

    int contentH = h - tbH - statusH;
    if (contentH < 0) contentH = 0;

    // Видео скрыто когда плейлист виден
    if (state->hVideoWnd)
        ShowWindow(state->hVideoWnd, state->showPlaylist ? SW_HIDE : SW_SHOW);
    // Плейлист — оверлей, переключается по L
    if (state->hPlaylist)
        ShowWindow(state->hPlaylist, state->showPlaylist ? SW_SHOW : SW_HIDE);

    // Видео центрируем с учётом aspect ratio
    if (state->hVideoWnd) {
        double ar = state->videoAr;
        if (ar <= 0) ar = GetVideoAspectRatio(state);
        if (ar <= 0) ar = 16.0 / 9.0;

        int vw = w, vh = contentH;
        double contentAr = (double)w / (double)contentH;
        if (ar > contentAr) {
            vh = (int)(w / ar);
        } else {
            vw = (int)(contentH * ar);
        }
        int vx = (w - vw) / 2;
        int vy = (contentH - vh) / 2;

        MoveWindow(state->hVideoWnd, vx, tbH + vy, vw, vh, TRUE);
    }

    // Плейлист поверх видео — на всю content area, Z-order top
    if (state->hPlaylist) {
        MoveWindow(state->hPlaylist, 0, tbH, w, contentH, TRUE);
        if (state->showPlaylist)
            SetWindowPos(state->hPlaylist, HWND_TOP, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE);
    }
}

static void UpdateToolbarRepeat(PluginState* state) {
    if (!state || !state->hToolbar) return;
    // Find repeat button by ID and update its text
    int count = (int)SendMessage(state->hToolbar, TB_BUTTONCOUNT, 0, 0);
    for (int i = 0; i < count; i++) {
        TBBUTTONINFO tbi = {0};
        tbi.cbSize = sizeof(TBBUTTONINFO);
        tbi.dwMask = TBIF_COMMAND | TBIF_TEXT;
        TCHAR text[32];
        tbi.pszText = text;
        tbi.cchText = 32;
        if (SendMessage(state->hToolbar, TB_GETBUTTONINFO, IDM_REPEAT, (LPARAM)&tbi) >= 0) {
            _tcscpy(text, GetRepeatLabel(state->repeatMode));
            SendMessage(state->hToolbar, TB_SETBUTTONINFO, IDM_REPEAT, (LPARAM)&tbi);
            break;
        }
    }
}

static void UpdateStatus(PluginState* state) {
    if (!state || !state->hStatus) return;
    TCHAR buf[512];
    TCHAR* fname = _tcsrchr(state->filePath, TEXT('\\'));
    fname = fname ? fname + 1 : state->filePath;

    int pm = (int)(state->position / 60), ps = (int)state->position % 60;
    int dm = (int)(state->duration / 60), ds = (int)state->duration % 60;

    TCHAR plInfo[32] = {0};
    if (state->playlist && state->playlistCount > 1)
        _sntprintf(plInfo, 32, TEXT("  [%d/%d]"), state->playlistIndex + 1, state->playlistCount);

    _sntprintf(buf, 512, TEXT("  %s%s  |  %02d:%02d / %02d:%02d  |  Vol: %d%%%s  |  %s"),
        fname, plInfo, pm, ps, dm, ds,
        state->volume, state->isMuted ? TEXT(" (M)") : TEXT(""),
        state->isPlaying ? (state->isPaused ? TEXT("Paused") : TEXT("Playing")) : TEXT("Stopped"));
    SetWindowText(state->hStatus, buf);
}

static void UpdateSeekbar(PluginState* state) {
    if (!state || !state->hSeekbar) return;
    int pos = (state->duration > 0) ? (int)(state->position / state->duration * 100) : 0;
    SendMessage(state->hSeekbar, TBM_SETPOS, TRUE, pos);
}

static void UpdateVolumeSlider(PluginState* state) {
    if (!state || !state->hVolSlider) return;
    SendMessage(state->hVolSlider, TBM_SETPOS, TRUE, state->volume);
}

/* -----------------------------------------------------------------------
   Theme (Defect #8 fix)
   ----------------------------------------------------------------------- */
static void ApplyTheme(PluginState* state) {
    if (!state) return;
    COLORREF bg  = state->isDarkMode ? RGB(28,  28,  28)  : GetSysColor(COLOR_BTNFACE);
    COLORREF fg  = state->isDarkMode ? RGB(220, 220, 220) : GetSysColor(COLOR_BTNTEXT);

    if (state->hBackBrush) DeleteObject(state->hBackBrush);
    state->hBackBrush = CreateSolidBrush(bg);

    if (state->hPlaylist) {
        ListView_SetBkColor(state->hPlaylist, bg);
        ListView_SetTextBkColor(state->hPlaylist, bg);
        ListView_SetTextColor(state->hPlaylist, fg);
        // Try dark theme on the listview scrollbar/header
        SetWindowTheme(state->hPlaylist,
            state->isDarkMode ? L"DarkMode_Explorer" : L"Explorer", NULL);
        InvalidateRect(state->hPlaylist, NULL, TRUE);
    }
    InvalidateRect(state->hMainWnd, NULL, TRUE);
}

/* -----------------------------------------------------------------------
   Context menu
   ----------------------------------------------------------------------- */
static void ShowContextMenu(PluginState* state, int x, int y) {
    if (!state) return;
    HMENU hMenu = CreatePopupMenu();
    AppendMenu(hMenu, MF_STRING, IDM_PLAY,
        (state->isPlaying && !state->isPaused) ? TEXT("Pause") : TEXT("Play"));
    AppendMenu(hMenu, MF_STRING, IDM_STOP, TEXT("Stop"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_PREV, TEXT("Previous file"));
    AppendMenu(hMenu, MF_STRING, IDM_NEXT, TEXT("Next file"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_VOL_UP,   TEXT("Volume Up"));
    AppendMenu(hMenu, MF_STRING, IDM_VOL_DOWN, TEXT("Volume Down"));
    AppendMenu(hMenu, state->isMuted ? MF_CHECKED : MF_STRING, IDM_MUTE, TEXT("Mute"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_SEEK_FWD,  TEXT("Forward 10s"));
    AppendMenu(hMenu, MF_STRING, IDM_SEEK_BACK, TEXT("Back 10s"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_FULLSCREEN,
        state->isFullscreen ? TEXT("Exit Fullscreen") : TEXT("Fullscreen"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, state->showPlaylist ? MF_CHECKED : MF_STRING,
        IDM_SHOWPLAYLIST, TEXT("Show/Hide Playlist"));
    AppendMenu(hMenu, MF_STRING, IDM_FILEINFO, TEXT("File Info"));
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    const TCHAR* repeatText[] = { TEXT("Repeat: Off"), TEXT("Repeat: All"), TEXT("Repeat: One") };
    AppendMenu(hMenu, state->repeatMode ? MF_CHECKED : MF_STRING,
        IDM_REPEAT, repeatText[state->repeatMode]);
    AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenu(hMenu, MF_STRING, IDM_ABOUT, TEXT("About MediaShow2"));
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, 0, state->hMainWnd, NULL);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK VolSliderProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

/* -----------------------------------------------------------------------
   Controls creation (Defect #1 fix: CCS_NORESIZE|CCS_NOPARENTALIGN)
   ----------------------------------------------------------------------- */
static void CreateControls(PluginState* state) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_BAR_CLASSES | ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // Segoe UI Symbol (Vista+) → Segoe UI fallback: guaranteed Unicode media glyphs
    state->hIconFont = CreateFont(
        -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        TEXT("Segoe UI Symbol"));
    if (!state->hIconFont)
        state->hIconFont = CreateFont(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            TEXT("Segoe UI"));

    // Defect #1 fix: CCS_NORESIZE | CCS_NOPARENTALIGN prevent toolbar from
    // auto-stretching to parent width and covering the trackbars.
    state->hToolbar = CreateWindowEx(0, TOOLBARCLASSNAME, TEXT(""),
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | TBSTYLE_LIST |
        CCS_NORESIZE | CCS_NOPARENTALIGN,
        0, 0, 220, 40, state->hMainWnd,
        (HMENU)IDC_TOOLBAR, GetModuleHandle(0), NULL);

    if (state->hToolbar) {
        SendMessage(state->hToolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessage(state->hToolbar, TB_SETBUTTONSIZE, 0, MAKELPARAM(36, 36));
        SendMessage(state->hToolbar, TB_SETBITMAPSIZE, 0, MAKELPARAM(0, 0));
        if (state->hIconFont)
            SendMessage(state->hToolbar, WM_SETFONT, (WPARAM)state->hIconFont, FALSE);

        // Unicode media symbols (Segoe UI Symbol)
        TBBUTTON buttons[] = {
            { I_IMAGENONE, IDM_PREV,      TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"\u23EE" }, // ⏮ Prev
            { I_IMAGENONE, IDM_PLAY,      TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"\u25B6" }, // ▶ Play
            { I_IMAGENONE, IDM_STOP,      TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"\u25A0" }, // ■ Stop
            { I_IMAGENONE, IDM_NEXT,      TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"\u23ED" }, // ⏭ Next
            { 0,           0,             TBSTATE_ENABLED, BTNS_SEP,                                    {0}, 0, 0 },
            { I_IMAGENONE, IDM_SEEK_BACK, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"\u23EA" }, // ⏪ Rewind
            { I_IMAGENONE, IDM_SEEK_FWD,  TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"\u23E9" }, // ⏩ Forward
            { 0,           0,             TBSTATE_ENABLED, BTNS_SEP,                                    {0}, 0, 0 },
            { I_IMAGENONE, IDM_REPEAT,    TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"\u21BB" }, // ↻ Repeat
            { I_IMAGENONE, IDM_SHOWPLAYLIST, TBSTATE_ENABLED, BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT, {0}, 0, (INT_PTR)L"\u2630" }, // ☰ Playlist
        };
        SendMessage(state->hToolbar, TB_ADDBUTTONS, 10, (LPARAM)buttons);
        SendMessage(state->hToolbar, TB_AUTOSIZE, 0, 0);
    }

    state->hSeekbar = CreateWindow(TRACKBAR_CLASS, TEXT(""),
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_NOTICKS,
        0, 0, 100, 28,
        state->hMainWnd, (HMENU)IDC_SEEKBAR, GetModuleHandle(0), NULL);
    if (state->hSeekbar) {
        SendMessage(state->hSeekbar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessage(state->hSeekbar, TBM_SETPOS,   TRUE, 0);
    }

    state->hVolSlider = CreateWindow(TRACKBAR_CLASS, TEXT(""),
        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_NOTICKS,
        0, 0, 100, 28,
        state->hMainWnd, (HMENU)IDC_VOLSLIDER, GetModuleHandle(0), NULL);
    if (state->hVolSlider) {
        SendMessage(state->hVolSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessage(state->hVolSlider, TBM_SETPOS,   TRUE, state->volume);
        SetWindowSubclass(state->hVolSlider, VolSliderProc, 0, (DWORD_PTR)state);
    }

    state->hStatus = CreateWindowEx(0, STATUSCLASSNAME, TEXT(""),
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 100, 22,
        state->hMainWnd, (HMENU)IDC_SToolBar, GetModuleHandle(0), NULL);
}

/* -----------------------------------------------------------------------
   Fullscreen (Defect #15 fix)
   ----------------------------------------------------------------------- */
static LRESULT CALLBACK FullscreenWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PluginState* state = (PluginState*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_KEYDOWN:
        if ((wParam == VK_ESCAPE || wParam == VK_F11) && state)
            SendMessage(state->hMainWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (state)
            SendMessage(state->hMainWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void RegisterFullscreenClass() {
    if (fullscreenWndClass) return;
    WNDCLASSEX wc = {0};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = FullscreenWndProc;
    wc.hInstance     = GetModuleHandle(0);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = TEXT("MediaShow2Fullscreen");
    fullscreenWndClass = RegisterClassEx(&wc);
}

static void ToggleFullscreen(PluginState* state) {
    if (!state) return;
    if (!state->isFullscreen) {
        RegisterFullscreenClass();
        HMONITOR hMon = MonitorFromWindow(state->hMainWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        RECT r = mi.rcMonitor;

        state->hFullscreenWnd = CreateWindowEx(WS_EX_TOPMOST,
            TEXT("MediaShow2Fullscreen"), TEXT(""),
            WS_POPUP | WS_VISIBLE,
            r.left, r.top, r.right - r.left, r.bottom - r.top,
            NULL, NULL, GetModuleHandle(0), NULL);
        if (!state->hFullscreenWnd) return;

        SetWindowLongPtr(state->hFullscreenWnd, GWLP_USERDATA, (LONG_PTR)state);
        ShowWindow(state->hVideoWnd, SW_SHOW);
        SetParent(state->hVideoWnd, state->hFullscreenWnd);
        int fw = r.right - r.left, fh = r.bottom - r.top;

        double ar = state->videoAr;
        if (ar <= 0) ar = GetVideoAspectRatio(state);
        if (ar <= 0) ar = 16.0 / 9.0;

        int vw = fw, vh = fh;
        double screenAr = (double)fw / (double)fh;
        if (ar > screenAr) {
            vh = (int)(fw / ar);
        } else {
            vw = (int)(fh * ar);
        }
        int vx = (fw - vw) / 2;
        int vy = (fh - vh) / 2;

        MoveWindow(state->hVideoWnd, vx, vy, vw, vh, TRUE);

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
}

/* -----------------------------------------------------------------------
   Navigate to a playlist item (shared by IDM_PREV, IDM_NEXT, double-click,
   and the WM_PLAYER_TRACK_END handler)
   ----------------------------------------------------------------------- */
static void PlayIndex(PluginState* state, int idx) {
    if (!state || idx < 0 || idx >= state->playlistCount) return;
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
    // Defect #2 fix: always apply user volume after opening a new file
    ApplyVolume(state);

    _tcsncpy(state->filePath, f, MAX_PATH - 1);
    state->duration  = state->useDirectShow ?
        DSPlayer_GetDuration(state->pDSPlayer) :
        MFPlayer_GetDuration(state->pMFPlayer);
    state->videoAr   = state->useDirectShow ?
        DSPlayer_GetAspectRatio(state->pDSPlayer) :
        MFPlayer_GetAspectRatio(state->pMFPlayer);
    state->isPlaying = TRUE;
    state->isPaused  = FALSE;

    state->showPlaylist = IsAudioOnly(f);
    UpdatePlaylist(state);
    UpdateLayout(state);
    UpdateStatus(state);
    UpdateSeekbar(state);
}

/* -----------------------------------------------------------------------
   Volume slider subclass — intercept WM_MOUSEWHEEL so trackbar's
   default horizontal handling (up = left = decrease) doesn't invert it.
   ----------------------------------------------------------------------- */
static LRESULT CALLBACK VolSliderProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR subclassId, DWORD_PTR refData) {
    PluginState* state = (PluginState*)refData;
    if (msg == WM_MOUSEWHEEL && state) {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        state->volume = max(0, min(100, state->volume + (delta > 0 ? 5 : -5)));
        SendMessage(state->hVolSlider, TBM_SETPOS, TRUE, state->volume);
        ApplyVolume(state);
        SaveVolume(state);
        UpdateStatus(state);
        return 0;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

/* -----------------------------------------------------------------------
   Video window subclass (Defect #14 fix: double-click triggers fullscreen,
                          Defect #3 fix:  WM_MOUSEWHEEL removed)
   ----------------------------------------------------------------------- */
static LRESULT CALLBACK VideoWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR subclassId, DWORD_PTR refData) {
    PluginState* state = (PluginState*)refData;
    switch (msg) {
    // Defect #10: paint letterbox background black
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_SIZE: {
        if (state) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            if (state->useDirectShow)
                DSPlayer_UpdateVideoWindow(state->pDSPlayer, &rc);
            else
                MFPlayer_UpdateVideoWindow(state->pMFPlayer, &rc);
        }
        break;
    }
    case WM_KEYDOWN:
        if (state)
            SendMessage(state->hMainWnd, msg, wParam, lParam);
        return 0;
    case WM_LBUTTONDOWN:
        if (state)
            SendMessage(state->hMainWnd, msg, wParam, lParam);
        return 0;
    // Defect #14 fix: double-click triggers fullscreen toggle
    case WM_LBUTTONDBLCLK:
        if (state)
            SendMessage(state->hMainWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        return 0;

    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        if (state) {
            int x, y;
            if (msg == WM_CONTEXTMENU) {
                x = GET_X_LPARAM(lParam); y = GET_Y_LPARAM(lParam);
            } else {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ClientToScreen(hWnd, &pt);
                x = pt.x; y = pt.y;
            }
            ShowContextMenu(state, x, y);
        }
        return 0;
    // Defect #3 fix: WM_MOUSEWHEEL removed from video window.
    // Volume-only wheel is handled in cbNewMain.
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

/* -----------------------------------------------------------------------
   Main window procedure
   ----------------------------------------------------------------------- */
static LRESULT CALLBACK cbNewMain(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PluginState* state = GetState(hWnd);

    switch (msg) {

    /* ---- Initialization ---- */
    case WM_CREATE: {
        state = (PluginState*)calloc(1, sizeof(PluginState));
        if (!state) return -1;
        state->hMainWnd = hWnd;
        // Defect #2 fix: load persisted volume; don't hardcode 80 here
        state->volume   = LoadVolume();
        state->repeatMode = LoadRepeatMode();
        state->sortColumn = -1;
        state->hFont = CreateFont(
            -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, TEXT("Segoe UI"));
        state->hBackBrush = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        SetProp(hWnd, TEXT("STATE"), (HANDLE)state);

        state->hVideoWnd = CreateWindowEx(0, WC_STATIC, TEXT(""),
            WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
            0, 0, 100, 100,
            hWnd, (HMENU)IDC_VIDEO, GetModuleHandle(0), NULL);
        SetWindowSubclass(state->hVideoWnd, VideoWndProc, 0, (DWORD_PTR)state);

        state->hPlaylist = CreateWindow(WC_LISTVIEW, TEXT(""),
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, 0, 100, 100,
            hWnd, (HMENU)IDC_PLAYLIST, GetModuleHandle(0), NULL);
        ListView_SetExtendedListViewStyle(state->hPlaylist,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        LVCOLUMN lvc = {0};
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        lvc.fmt  = LVCFMT_LEFT;
        lvc.cx = 40;   lvc.pszText = (TCHAR*)TEXT("#");    ListView_InsertColumn(state->hPlaylist, 0, &lvc);
        lvc.cx = 240;  lvc.pszText = (TCHAR*)TEXT("Name");  ListView_InsertColumn(state->hPlaylist, 1, &lvc);
        lvc.cx = 55;   lvc.pszText = (TCHAR*)TEXT("Type");  ListView_InsertColumn(state->hPlaylist, 2, &lvc);
        lvc.cx = 120;  lvc.pszText = (TCHAR*)TEXT("Date");  ListView_InsertColumn(state->hPlaylist, 3, &lvc);
        ShowWindow(state->hPlaylist, SW_HIDE);

        state->pMFPlayer = MFPlayer_Create(state->hVideoWnd, OnMFEnd, state);
        state->pDSPlayer = DSPlayer_Create(state->hVideoWnd, OnMFEnd, state);

        CreateControls(state);
        UpdateToolbarRepeat(state);
        return 0;
    }

    /* ---- Layout ---- */
    case WM_SIZE:
        if (state) UpdateLayout(state);
        return 0;

    /* ---- Background (dark mode) ---- */
    case WM_ERASEBKGND: {
        if (!state) break;
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, (!state->showPlaylist)
            ? (HBRUSH)GetStockObject(BLACK_BRUSH) : state->hBackBrush);
        return 1;
    }

    /* ---- Seekbar / Volume slider (WM_HSCROLL / WM_VSCROLL) ---- */
    case WM_HSCROLL: {
        if (!state) break;
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == state->hSeekbar) {
            int pos = (int)SendMessage(state->hSeekbar, TBM_GETPOS, 0, 0);
            if (state->duration > 0) {
                state->position = state->duration * pos / 100.0;
                if (state->useDirectShow)
                    DSPlayer_Seek(state->pDSPlayer, state->position);
                else
                    MFPlayer_Seek(state->pMFPlayer, state->position);
            }
            UpdateStatus(state);
            UpdateSeekbar(state);
        } else if (hCtrl == state->hVolSlider) {
            state->volume = (int)SendMessage(state->hVolSlider, TBM_GETPOS, 0, 0);
            ApplyVolume(state);
            SaveVolume(state);          // Defect #2 fix: persist immediately
            UpdateStatus(state);
        }
        return 0;
    }

    case WM_VSCROLL: {
        if (!state) break;
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == state->hVolSlider) {
            state->volume = (int)SendMessage(state->hVolSlider, TBM_GETPOS, 0, 0);
            ApplyVolume(state);
            SaveVolume(state);
            UpdateStatus(state);
        }
        return 0;
    }

    /* ---- Mouse wheel: volume or playlist scroll ---- */
    case WM_MOUSEWHEEL: {
        if (!state) break;
        if (state->showPlaylist && state->hPlaylist) {
            SendMessage(state->hPlaylist, WM_MOUSEWHEEL, wParam, lParam);
        } else {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            state->volume = max(0, min(100, state->volume + (delta > 0 ? 5 : -5)));
            ApplyVolume(state);
            SaveVolume(state);
            UpdateVolumeSlider(state);
            UpdateStatus(state);
        }
        return 0;
    }

    /* ---- Context menu ---- */
    case WM_CONTEXTMENU:
        if (state) ShowContextMenu(state, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;

    /* ---- Focus (Defect #21 fix) ---- */
    case WM_SETFOCUS:
        if (state) {
            if (state->showPlaylist && state->hPlaylist)
                SetFocus(state->hPlaylist);
            else if (state->hVideoWnd)
                SetFocus(state->hVideoWnd);
        }
        return 0;

    /* ---- Mouse click on video: double-click for fullscreen ---- */
    case WM_LBUTTONDOWN: {
        if (!state || state->showPlaylist) break;
        DWORD now = GetTickCount();
        if (state->lastClickTime && (now - state->lastClickTime <= 650)) {
            state->lastClickTime = 0;
            SendMessage(hWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        } else {
            state->lastClickTime = now;
        }
        return 0;
    }

    /* ---- Playlist notifications (Defect #11 fix) ---- */
    case WM_NOTIFY: {
        if (!state) break;
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->hwndFrom != state->hPlaylist) break;

        if (hdr->code == NM_DBLCLK) {
            NMITEMACTIVATE* nm = (NMITEMACTIVATE*)lParam;
            if (nm->iItem >= 0 && nm->iItem < state->playlistCount) {
                PlayIndex(state, nm->iItem);
            }
            return 0;
        }

        if (hdr->code == LVN_COLUMNCLICK) {
            NMLISTVIEW* nmlv = (NMLISTVIEW*)lParam;
            int col = nmlv->iSubItem;
            if (col == state->sortColumn)
                state->sortAscending = !state->sortAscending;
            else {
                state->sortColumn = col;
                state->sortAscending = TRUE;
            }
            SortPlaylist(state);
            UpdatePlaylist(state);
            return 0;
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
                        state->playlistIndex = max(0, state->playlistCount - 1);
                    UpdatePlaylist(state);
                }
            } else if (kd->wVKey == VK_RETURN) {
                int idx = ListView_GetNextItem(state->hPlaylist, -1, LVNI_SELECTED);
                if (idx >= 0) {
                    PlayIndex(state, idx);
                    state->showPlaylist = FALSE;
                    UpdateLayout(state);
                }
            } else if (kd->wVKey == VK_UP && (GetKeyState(VK_CONTROL) & 0x8000)) {
                int idx = ListView_GetNextItem(state->hPlaylist, -1, LVNI_SELECTED);
                if (idx > 0) {
                    TCHAR* tmp = state->playlist[idx];
                    state->playlist[idx] = state->playlist[idx - 1];
                    state->playlist[idx - 1] = tmp;
                    if (state->playlistIndex == idx) state->playlistIndex--;
                    else if (state->playlistIndex == idx - 1) state->playlistIndex++;
                    UpdatePlaylist(state);
                    ListView_SetItemState(state->hPlaylist, idx - 1,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
            } else if (kd->wVKey == VK_DOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
                int idx = ListView_GetNextItem(state->hPlaylist, -1, LVNI_SELECTED);
                if (idx >= 0 && idx < state->playlistCount - 1) {
                    TCHAR* tmp = state->playlist[idx];
                    state->playlist[idx] = state->playlist[idx + 1];
                    state->playlist[idx + 1] = tmp;
                    if (state->playlistIndex == idx) state->playlistIndex++;
                    else if (state->playlistIndex == idx + 1) state->playlistIndex--;
                    UpdatePlaylist(state);
                    ListView_SetItemState(state->hPlaylist, idx + 1,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
            }
            return 0;
        }
        break;
    }

    /* ---- Keyboard shortcuts (Defect #16 fix) ---- */
    case WM_KEYDOWN: {
        if (!state) break;
        switch (wParam) {
        case VK_SPACE: SendMessage(hWnd, WM_COMMAND, IDM_PLAY,          0); return 0;
        case 'S':      SendMessage(hWnd, WM_COMMAND, IDM_STOP,          0); return 0;
        case VK_LEFT:  SendMessage(hWnd, WM_COMMAND, IDM_SEEK_BACK,     0); return 0;
        case VK_RIGHT: SendMessage(hWnd, WM_COMMAND, IDM_SEEK_FWD,      0); return 0;
        case VK_UP:    SendMessage(hWnd, WM_COMMAND, IDM_VOL_UP,        0); return 0;
        case VK_DOWN:  SendMessage(hWnd, WM_COMMAND, IDM_VOL_DOWN,      0); return 0;
        case 'M':      SendMessage(hWnd, WM_COMMAND, IDM_MUTE,          0); return 0;
        case VK_F11:   SendMessage(hWnd, WM_COMMAND, IDM_FULLSCREEN,    0); return 0;
        case 'L':      SendMessage(hWnd, WM_COMMAND, IDM_SHOWPLAYLIST,  0); return 0;
        case 'I':      SendMessage(hWnd, WM_COMMAND, IDM_FILEINFO,      0); return 0;
        case VK_ESCAPE:
            if (state->isFullscreen)
                SendMessage(hWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
            return 0;
        }
        break;
    }

    /* ---- Track-end notification from EventThread (Defect #4 fix) ---- */
    case WM_PLAYER_TRACK_END: {
        if (!state) break;
        state->isPlaying = FALSE;
        state->isPaused  = FALSE;
        if (state->repeatMode == 2) {
            // Repeat One: restart current track
            PlayIndex(state, state->playlistIndex);
        } else if (state->repeatMode == 1 && state->playlist && state->playlistCount > 0) {
            // Repeat All: wrap to beginning
            PlayIndex(state, (state->playlistIndex + 1) % state->playlistCount);
        } else if (state->playlist && state->playlistIndex < state->playlistCount - 1) {
            // Normal: play next
            PlayIndex(state, state->playlistIndex + 1);
        }
        // Off: stop (do nothing)
        return 0;
    }

    /* ---- Commands ---- */
    case WM_COMMAND: {
        if (!state) break;
        WORD cmd = LOWORD(wParam);
        switch (cmd) {

        case IDM_PLAY:
            if (state->isPlaying && !state->isPaused) {
                if (state->useDirectShow) DSPlayer_Pause(state->pDSPlayer);
                else                      MFPlayer_Pause(state->pMFPlayer);
                state->isPaused = TRUE;
            } else {
                if (state->useDirectShow) DSPlayer_Play(state->pDSPlayer);
                else                      MFPlayer_Play(state->pMFPlayer);
                state->isPlaying = TRUE;
                state->isPaused  = FALSE;
            }
            UpdateStatus(state);
            break;

        case IDM_STOP:
            if (state->useDirectShow) DSPlayer_Stop(state->pDSPlayer);
            else                      MFPlayer_Stop(state->pMFPlayer);
            state->isPlaying = FALSE;
            state->isPaused  = FALSE;
            state->position  = 0;
            UpdateStatus(state);
            UpdateSeekbar(state);
            break;

        case IDM_PREV:
            if (state->playlist && state->playlistCount > 0) {
                int idx = state->playlistIndex - 1;
                if (idx < 0) idx = (state->repeatMode == 1) ? state->playlistCount - 1 : 0;
                PlayIndex(state, idx);
            }
            break;

        case IDM_NEXT:
            if (state->playlist && state->playlistCount > 0) {
                int idx = state->playlistIndex + 1;
                if (idx >= state->playlistCount) idx = (state->repeatMode == 1) ? 0 : state->playlistCount - 1;
                PlayIndex(state, idx);
            }
            break;

        case IDM_VOL_UP:
            state->volume = min(100, state->volume + 5);
            ApplyVolume(state);
            SaveVolume(state);
            UpdateVolumeSlider(state);
            UpdateStatus(state);
            break;

        case IDM_VOL_DOWN:
            state->volume = max(0, state->volume - 5);
            ApplyVolume(state);
            SaveVolume(state);
            UpdateVolumeSlider(state);
            UpdateStatus(state);
            break;

        case IDM_MUTE:
            state->isMuted = !state->isMuted;
            ApplyVolume(state);
            UpdateStatus(state);
            break;

        case IDM_SEEK_FWD:
            state->position = min(state->duration, state->position + 10.0);
            if (state->useDirectShow) DSPlayer_Seek(state->pDSPlayer, state->position);
            else                      MFPlayer_Seek(state->pMFPlayer, state->position);
            UpdateSeekbar(state);
            UpdateStatus(state);
            break;

        case IDM_SEEK_BACK:
            state->position = max(0.0, state->position - 10.0);
            if (state->useDirectShow) DSPlayer_Seek(state->pDSPlayer, state->position);
            else                      MFPlayer_Seek(state->pMFPlayer, state->position);
            UpdateSeekbar(state);
            UpdateStatus(state);
            break;

        // Defect #15 fix
        case IDM_FULLSCREEN:
            ToggleFullscreen(state);
            break;

        case IDM_REPEAT:
            state->repeatMode = (state->repeatMode + 1) % 3;
            SaveRepeatMode(state);
            UpdateToolbarRepeat(state);
            break;

        case IDM_SHOWPLAYLIST:
            state->showPlaylist = !state->showPlaylist;
            UpdatePlaylist(state);
            UpdateLayout(state);
            if (state->showPlaylist)
                SetFocus(state->hPlaylist);
            else
                SetFocus(state->hVideoWnd);
            break;

        case IDM_FILEINFO: {
            TCHAR* f = _tcsrchr(state->filePath, TEXT('\\'));
            f = f ? f + 1 : state->filePath;
            TCHAR info[256];
            int dm = (int)(state->duration / 60), ds = (int)state->duration % 60;
            _sntprintf(info, 256, TEXT("File: %s\nDuration: %02d:%02d"), f, dm, ds);
            MessageBox(hWnd, info, APP_NAME, MB_OK | MB_ICONINFORMATION);
            break;
        }

        case IDM_ABOUT:
            MessageBox(hWnd,
                TEXT("MediaShow2 v1.0\n")
                TEXT("Multimedia lister plugin for Total Commander\n\n")
                TEXT("Shortcuts:\n")
                TEXT("  Space  Play/Pause\n")
                TEXT("  S      Stop\n")
                TEXT("  \u2190\u2192    Seek \u00B110s\n")
                TEXT("  \u2191\u2193    Volume \u00B15%\n")
                TEXT("  M      Mute\n")
                TEXT("  L      Toggle Playlist\n")
                TEXT("  F11    Fullscreen\n")
                TEXT("  Ctrl+T Always on Top"),
                APP_NAME, MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    }

    /* ---- Timer: position polling ---- */
    case WM_TIMER:
        if (wParam == 1 && state) {
            if (state->duration <= 0) {
                state->duration = state->useDirectShow ?
                    DSPlayer_GetDuration(state->pDSPlayer) :
                    MFPlayer_GetDuration(state->pMFPlayer);
            }
            state->position = state->useDirectShow ?
                DSPlayer_GetPosition(state->pDSPlayer) :
                MFPlayer_GetPosition(state->pMFPlayer);
            UpdateStatus(state);
            UpdateSeekbar(state);
        }
        return 0;

    /* ---- Cleanup ---- */
    case WM_DESTROY:
        if (state) {
            KillTimer(hWnd, 1);
            if (state->isFullscreen && state->hFullscreenWnd) {
                SetParent(state->hVideoWnd, hWnd);
                DestroyWindow(state->hFullscreenWnd);
            }
            FreePlaylist(state);
            MFPlayer_Destroy(state->pMFPlayer);
            DSPlayer_Destroy(state->pDSPlayer);
            RemoveWindowSubclass(state->hVideoWnd, VideoWndProc, 0);
            RemoveWindowSubclass(state->hVolSlider, VolSliderProc, 0);
            if (state->hFont)     DeleteObject(state->hFont);
            if (state->hIconFont) DeleteObject(state->hIconFont);
            if (state->hBackBrush) DeleteObject(state->hBackBrush);
            free(state);
            RemoveProp(hWnd, TEXT("STATE"));
        }
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

/* -----------------------------------------------------------------------
   Window class registration
   ----------------------------------------------------------------------- */
static void RegisterMainWndClass() {
    if (mainWndClass) return;
    WNDCLASSEX wc = {0};
    wc.cbSize        = sizeof(WNDCLASSEX);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = cbNewMain;
    wc.hInstance     = GetModuleHandle(0);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = TEXT("MediaShow2Main");
    mainWndClass = RegisterClassEx(&wc);
}

/* -----------------------------------------------------------------------
   TC WLX API
   ----------------------------------------------------------------------- */
HWND __stdcall ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags) {
    int n = MultiByteToWideChar(CP_ACP, 0, FileToLoad, -1, NULL, 0);
    TCHAR* w = (TCHAR*)calloc(n, sizeof(TCHAR));
    MultiByteToWideChar(CP_ACP, 0, FileToLoad, -1, w, n);
    HWND h = ListLoadW(ParentWin, w, ShowFlags);
    free(w);
    return h;
}

HWND __stdcall ListLoadW(HWND ParentWin, TCHAR* FileToLoad, int ShowFlags) {
    RegisterMainWndClass();
    HWND hWnd = CreateWindowEx(0, TEXT("MediaShow2Main"), APP_NAME,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 100, 100,
        ParentWin, (HMENU)IDC_MAIN, GetModuleHandle(0), NULL);
    if (!hWnd) return NULL;

    PluginState* state = GetState(hWnd);
    if (!state) { DestroyWindow(hWnd); return NULL; }

    // Defect #7 fix: store ParentWin for correct itm_next routing
    state->hParentWnd = ParentWin;

    // Defect #8 fix: read dark mode flag from TC
    state->isDarkMode = ((ShowFlags & lcp_darkmode) || (ShowFlags & lcp_darkmodenative)) ? TRUE : FALSE;
    ApplyTheme(state);

    _tcsncpy(state->filePath, FileToLoad, MAX_PATH - 1);

    // Start with single file — playlist will be updated asynchronously
    state->playlist      = (TCHAR**)calloc(1, sizeof(TCHAR*));
    state->fileDates     = (FILETIME*)calloc(1, sizeof(FILETIME));
    state->playlist[0]   = _tcsdup(FileToLoad);
    state->playlistCount = 1;
    state->playlistIndex = 0;

    // Request selected files from TC via clipboard
    RequestSelectedFiles(ParentWin, state);

    // If no files selected, scan directory for media files
    if (state->playlistCount <= 1)
        BuildPlaylist(state, NULL, FileToLoad);

    // Update showPlaylist based on first file in playlist
    if (state->playlistCount > 0)
        state->showPlaylist = IsAudioOnly(state->playlist[0]);

    SetTimer(hWnd, 1, 500, NULL);
    UpdatePlaylist(state);
    UpdateLayout(state);

    // Open and start playback
    state->useDirectShow = FALSE;
    HRESULT hr = MFPlayer_Open(state->pMFPlayer, FileToLoad);
    if (SUCCEEDED(hr)) {
        state->duration = MFPlayer_GetDuration(state->pMFPlayer);
        state->videoAr  = MFPlayer_GetAspectRatio(state->pMFPlayer);
        ApplyVolume(state);
        MFPlayer_Play(state->pMFPlayer);
        state->isPlaying = TRUE;
    } else if (state->pDSPlayer) {
        hr = DSPlayer_Open(state->pDSPlayer, FileToLoad);
        if (SUCCEEDED(hr)) {
            state->useDirectShow = TRUE;
            state->duration = DSPlayer_GetDuration(state->pDSPlayer);
            state->videoAr  = DSPlayer_GetAspectRatio(state->pDSPlayer);
            ApplyVolume(state);
            DSPlayer_Play(state->pDSPlayer);
            state->isPlaying = TRUE;
        }
    }

    UpdateLayout(state);
    UpdateStatus(state);
    UpdateSeekbar(state);
    UpdateVolumeSlider(state);
    return hWnd;
}

int __stdcall ListLoadNext(HWND ParentWin, HWND PluginWin, char* FileToLoad, int ShowFlags) {
    int n = MultiByteToWideChar(CP_ACP, 0, FileToLoad, -1, NULL, 0);
    TCHAR* w = (TCHAR*)calloc(n, sizeof(TCHAR));
    MultiByteToWideChar(CP_ACP, 0, FileToLoad, -1, w, n);
    int r = ListLoadNextW(ParentWin, PluginWin, w, ShowFlags);
    free(w);
    return r;
}

int __stdcall ListLoadNextW(HWND ParentWin, HWND PluginWin, WCHAR* FileToLoad, int ShowFlags) {
    PluginState* state = GetState(PluginWin);
    if (!state) return LISTPLUGIN_ERROR;

    // Stop current playback
    if (state->useDirectShow) DSPlayer_Stop(state->pDSPlayer);
    else                      MFPlayer_Stop(state->pMFPlayer);

    // Defect #8 fix: update dark mode flag if TC has changed it
    BOOL dm = ((ShowFlags & lcp_darkmode) || (ShowFlags & lcp_darkmodenative)) ? TRUE : FALSE;
    if (dm != state->isDarkMode) {
        state->isDarkMode = dm;
        ApplyTheme(state);
    }

    _tcsncpy(state->filePath, FileToLoad, MAX_PATH - 1);

    state->useDirectShow = FALSE;
    HRESULT hr = MFPlayer_Open(state->pMFPlayer, FileToLoad);
    if (SUCCEEDED(hr)) {
        state->duration  = MFPlayer_GetDuration(state->pMFPlayer);
        state->videoAr   = MFPlayer_GetAspectRatio(state->pMFPlayer);
        ApplyVolume(state);
        MFPlayer_Play(state->pMFPlayer);
        state->isPlaying = TRUE;
    } else if (state->pDSPlayer) {
        hr = DSPlayer_Open(state->pDSPlayer, FileToLoad);
        if (SUCCEEDED(hr)) {
            state->useDirectShow = TRUE;
            state->duration  = DSPlayer_GetDuration(state->pDSPlayer);
            state->videoAr   = DSPlayer_GetAspectRatio(state->pDSPlayer);
            ApplyVolume(state);
            DSPlayer_Play(state->pDSPlayer);
            state->isPlaying = TRUE;
        }
    }

    state->isPaused     = FALSE;
    state->showPlaylist = IsAudioOnly(FileToLoad);
    UpdatePlaylist(state);
    UpdateLayout(state);
    UpdateStatus(state);
    UpdateSeekbar(state);
    return LISTPLUGIN_OK;
}

void __stdcall ListCloseWindow(HWND ListWin) {
    PluginState* state = GetState(ListWin);
    if (state) {
        if (state->useDirectShow) DSPlayer_Stop(state->pDSPlayer);
        else                      MFPlayer_Stop(state->pMFPlayer);
    }
    DestroyWindow(ListWin);
}

int __stdcall ListNotificationReceived(HWND ListWin, int Message, WPARAM wParam, LPARAM lParam) {
    if (Message == WM_COMMAND) {
        PluginState* state = GetState(ListWin);
        if (state) SendMessage(ListWin, WM_COMMAND, wParam, lParam);
    }
    return 0;
}

void __stdcall ListGetDetectString(char* DetectString, int maxlen) {
    snprintf(DetectString, maxlen,
        "MULTIMEDIA & (ext=\"AVI\" | ext=\"MPG\" | ext=\"MPEG\" | ext=\"ASF\" | "
        "ext=\"VOB\" | ext=\"MP1\" | ext=\"MP2\" | ext=\"MP3\" | ext=\"WAV\" | "
        "ext=\"OGG\" | ext=\"WMA\" | ext=\"DAT\" | ext=\"MKV\" | ext=\"WEBM\" | "
        "ext=\"MP4\" | ext=\"M4A\" | ext=\"FLAC\" | ext=\"AAC\" | ext=\"OPUS\" | "
        "ext=\"MID\" | ext=\"MIDI\" | ext=\"KAR\")");
}

void __stdcall ListSetDefaultParams(ListDefaultParamStruct* dps) {
    if (iniPath[0] == 0 && dps && dps->DefaultIniName[0]) {
        MultiByteToWideChar(CP_ACP, 0, dps->DefaultIniName, -1, iniPath, MAX_PATH);
    }
}

int __stdcall ListSendCommand(HWND ListWin, int Command, int Parameter) {
    PluginState* state = GetState(ListWin);
    if (!state) return LISTPLUGIN_ERROR;

    if (Command == lc_newparams) {
        // Defect #20 fix: re-apply theme when TC toggles dark mode
        BOOL dm = ((Parameter & lcp_darkmode) || (Parameter & lcp_darkmodenative)) ? TRUE : FALSE;
        if (dm != state->isDarkMode) {
            state->isDarkMode = dm;
            ApplyTheme(state);
        }
        InvalidateRect(ListWin, NULL, TRUE);
        return LISTPLUGIN_OK;
    }

    if (Command == lc_setpercent && state->duration > 0) {
        state->position = state->duration * Parameter / 100.0;
        if (state->useDirectShow) DSPlayer_Seek(state->pDSPlayer, state->position);
        else                      MFPlayer_Seek(state->pMFPlayer, state->position);
        UpdateSeekbar(state);
        return LISTPLUGIN_OK;
    }

    return LISTPLUGIN_ERROR;
}

/* ---- Stubs for unused TC API functions ---- */
int     __stdcall ListSearchText(HWND, char*, int)               { return LISTPLUGIN_ERROR; }
int     __stdcall ListSearchTextW(HWND, WCHAR*, int)             { return LISTPLUGIN_ERROR; }
int     __stdcall ListSearchDialog(HWND, int)                    { return LISTPLUGIN_ERROR; }
int     __stdcall ListPrint(HWND, char*, char*, int, RECT*)      { return LISTPLUGIN_ERROR; }
int     __stdcall ListPrintW(HWND, WCHAR*, WCHAR*, int, RECT*)   { return LISTPLUGIN_ERROR; }
HBITMAP __stdcall ListGetPreviewBitmap(char*, int, int, char*, int)   { return NULL; }
HBITMAP __stdcall ListGetPreviewBitmapW(WCHAR*, int, int, char*, int) { return NULL; }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule);
    return TRUE;
}
