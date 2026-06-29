#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <ocidl.h>
#include <oleauto.h>
#include <propkey.h>
#include <propvarutil.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <tchar.h>
#include <stdlib.h>
#include <stdio.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "plugin_api.h"
#include "mf_player.h"
#include "ds_player.h"

// Property keys not in older SDK versions
// Correct GUIDs from Windows SDK propkey.h
static const PROPERTYKEY kPKEY_Music_AlbumCoverArt =
    {0x2b842dc0, 0x51d7, 0x4c3b, {0xb1, 0xcc, 0xce, 0x3e, 0x1a, 0x72, 0x9a, 0x41}, 14};
static const PROPERTYKEY kPKEY_Music_TrackNumber =
    {0x56a87397, 0xfa77, 0x11d3, {0x8a, 0x26, 0x00, 0xc0, 0x4f, 0x68, 0x37, 0x10}, 7};
static const PROPERTYKEY kPKEY_Audio_EncodingBitrate =
    {0x64440490, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}, 4};
static const PROPERTYKEY kPKEY_Media_Duration =
    {0x64440490, 0x4C8B, 0x11D1, {0x8B, 0x70, 0x08, 0x00, 0x36, 0xB1, 0x1A, 0x03}, 3};

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
    BOOL  appendMode;       // FALSE=replace, TRUE=append
    BOOL  switchInProgress; // lock: ignore Next/Prev while switching
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

static BOOL IsDuplicate(PluginState* state, const TCHAR* path) {
    if (!state || !state->playlist || !path) return FALSE;
    for (int i = 0; i < state->playlistCount; i++) {
        if (_tcsicmp(state->playlist[i], path) == 0)
            return TRUE;
    }
    return FALSE;
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

static void SaveAppendMode(PluginState* state) {
    if (!state || iniPath[0] == 0) return;
    TCHAR buf[16];
    _sntprintf(buf, 16, TEXT("%d"), state->appendMode);
    WritePrivateProfileString(TEXT("MediaShow2"), TEXT("AppendMode"), buf, iniPath);
}

static int LoadAppendMode(void) {
    if (iniPath[0] == 0) return 0;
    return (int)GetPrivateProfileInt(TEXT("MediaShow2"), TEXT("AppendMode"), 0, iniPath);
}

static const TCHAR* GetRepeatLabel(int mode) {
    switch (mode) {
    case 1:  return L"\u21BB";     // ↻ Repeat All
    case 2:  return L"\u21BB\u2081"; // ↻₁ Repeat One
    default: return L"\u25CB";     // ○ Off
    }
}

/* -----------------------------------------------------------------------
    Playlist persistence
    ----------------------------------------------------------------------- */
static TCHAR* GetPlaylistPath(void) {
    static TCHAR path[MAX_PATH] = {0};
    if (path[0] != 0) return path;
    if (iniPath[0] == 0) return NULL;
    _tcsncpy(path, iniPath, MAX_PATH - 1);
    TCHAR* slash = _tcsrchr(path, TEXT('\\'));
    if (slash) {
        _tcscpy(slash + 1, TEXT("MediaShow2_playlist.txt"));
    } else {
        return NULL;
    }
    return path;
}

static void SavePlaylist(PluginState* state) {
    if (!state || !state->playlist || state->playlistCount == 0) return;
    TCHAR* path = GetPlaylistPath();
    if (!path) return;
    FILE* f = _tfopen(path, TEXT("w"));
    if (!f) return;
    _ftprintf(f, TEXT("%d\n"), state->playlistIndex);
    for (int i = 0; i < state->playlistCount; i++) {
        _ftprintf(f, TEXT("%s\n"), state->playlist[i]);
    }
    fclose(f);
}

static void LoadPlaylist(PluginState* state) {
    if (!state) return;
    TCHAR* path = GetPlaylistPath();
    if (!path) return;
    FILE* f = _tfopen(path, TEXT("r"));
    if (!f) return;
    TCHAR line[MAX_PATH];
    if (!fgetws(line, MAX_PATH, f)) { fclose(f); return; }
    int savedIndex = _ttoi(line);
    TCHAR** files = NULL;
    FILETIME* dates = NULL;
    int count = 0;
    int allocSize = 64;
    files = (TCHAR**)calloc(allocSize, sizeof(TCHAR*));
    dates = (FILETIME*)calloc(allocSize, sizeof(FILETIME));
    while (fgetws(line, MAX_PATH, f)) {
        // Remove trailing newline
        int len = (int)_tcslen(line);
        while (len > 0 && (line[len-1] == TEXT('\n') || line[len-1] == TEXT('\r')))
            line[--len] = TEXT('\0');
        if (len == 0) continue;
        if (count >= allocSize) {
            allocSize *= 2;
            TCHAR** tmp = (TCHAR**)realloc(files, allocSize * sizeof(TCHAR*));
            FILETIME* tmpD = (FILETIME*)realloc(dates, allocSize * sizeof(FILETIME));
            if (!tmp || !tmpD) break;
            files = tmp; dates = tmpD;
        }
        // Check file exists
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesEx(line, GetFileExInfoStandard, &fad)) {
            files[count] = _tcsdup(line);
            dates[count] = fad.ftLastWriteTime;
            count++;
        }
    }
    fclose(f);
    if (count == 0) { free(files); free(dates); return; }
    FreePlaylist(state);
    state->playlist      = files;
    state->fileDates     = dates;
    state->playlistCount = count;
    state->playlistIndex = (savedIndex >= 0 && savedIndex < count) ? savedIndex : 0;
}

static void ClearPlaylist(PluginState* state) {
    if (!state) return;
    FreePlaylist(state);
    TCHAR* path = GetPlaylistPath();
    if (path) _tremove(path);
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
    SendMessage(state->hPlaylist, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state->hPlaylist);

    if (!state->playlist || state->playlistCount == 0) {
        LVITEM lvi = {0};
        lvi.mask    = LVIF_TEXT;
        lvi.iItem   = 0;
        TCHAR empty[] = TEXT("(empty)");
        lvi.pszText = empty;
        ListView_InsertItem(state->hPlaylist, &lvi);
        SendMessage(state->hPlaylist, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(state->hPlaylist, NULL, TRUE);
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

    SendMessage(state->hPlaylist, WM_SETREDRAW, TRUE, 0);

    if (state->playlistIndex >= 0 && state->playlistIndex < state->playlistCount) {
        ListView_SetItemState(state->hPlaylist, state->playlistIndex,
            LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(state->hPlaylist, state->playlistIndex, FALSE);
    }

    InvalidateRect(state->hPlaylist, NULL, TRUE);
    SavePlaylist(state);
}/* -----------------------------------------------------------------------
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

    // Replace: clear old playlist, set new
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
    SavePlaylist(state);
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
    const int pad     = 4;     // padding from window edge

    if (state->hToolbar) {
        MoveWindow(state->hToolbar, pad, pad, tbW, tbH, TRUE);
        SendMessage(state->hToolbar, TB_AUTOSIZE, 0, 0);
    }

    // Seekbar: fills space between toolbar and volume slider
    int seekX = pad + tbW + 4;
    int volW  = 140;
    int volX  = w - volW - pad;
    int seekW = volX - seekX - 4;
    if (seekW < 40) seekW = 40;

    int trackY = pad + (tbH - ctrlH) / 2;
    if (state->hSeekbar)
        SetWindowPos(state->hSeekbar, HWND_TOP, seekX, trackY, seekW, ctrlH, SWP_NOZORDER);
    if (state->hVolSlider)
        SetWindowPos(state->hVolSlider, HWND_TOP, volX, trackY, volW, ctrlH, SWP_NOZORDER);

    if (state->hStatus)
        MoveWindow(state->hStatus, 0, h - statusH, w, statusH, TRUE);

    int contentH = h - tbH - pad - statusH;
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

        MoveWindow(state->hVideoWnd, vx, tbH + pad + vy, vw, vh, TRUE);
    }

    // Плейлист поверх видео — на всю content area, Z-order top
    if (state->hPlaylist) {
        MoveWindow(state->hPlaylist, pad, tbH + pad, w - pad * 2, contentH, TRUE);
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
    AppendMenu(hMenu, state->appendMode ? MF_CHECKED : MF_STRING,
        IDM_APPENDMODE, TEXT("Add files to playlist"));
    AppendMenu(hMenu, MF_STRING, IDM_CLEARPLAYLIST, TEXT("Clear playlist"));
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
static LRESULT CALLBACK PlaylistProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

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
    if (state->switchInProgress) return;
    state->switchInProgress = TRUE;

    // Skip unplayable files (up to playlistCount attempts to avoid infinite loop)
    for (int attempt = 0; attempt < state->playlistCount; attempt++) {
        state->playlistIndex = idx;
        TCHAR* f = state->playlist[idx];

        HRESULT hr = E_FAIL;
        if (state->useDirectShow) {
            DSPlayer_Stop(state->pDSPlayer);
            hr = DSPlayer_Open(state->pDSPlayer, f);
            if (SUCCEEDED(hr)) DSPlayer_Play(state->pDSPlayer);
        } else if (MFPlayer_HasVideo(state->pMFPlayer)) {
            // Video: recreate to avoid EVR corruption
            MFPlayer_Destroy(state->pMFPlayer);
            state->pMFPlayer = MFPlayer_Create(state->hVideoWnd, OnMFEnd, state);
            hr = MFPlayer_Open(state->pMFPlayer, f);
            if (SUCCEEDED(hr)) MFPlayer_Play(state->pMFPlayer);
        } else {
            // Audio: Stop/Open/Play
            MFPlayer_Stop(state->pMFPlayer);
            hr = MFPlayer_Open(state->pMFPlayer, f);
            if (SUCCEEDED(hr)) MFPlayer_Play(state->pMFPlayer);
        }

        if (SUCCEEDED(hr)) break;

        // Failed — try next track
        idx = (idx + 1) % state->playlistCount;
        if (idx == state->playlistIndex) break; // full cycle, no playable files
    }

    ApplyVolume(state);

    TCHAR* f = state->playlist[state->playlistIndex];
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
    // Keep switchInProgress=TRUE, reset after cooldown timer
    SetTimer(state->hMainWnd, IDT_COOLDOWN, 500, NULL);
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
    Playlist ListView subclass: intercept keys when playlist has focus
    ----------------------------------------------------------------------- */
static LRESULT CALLBACK PlaylistProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR subclassId, DWORD_PTR refData) {
    if (msg == WM_KEYDOWN) {
        PluginState* state = (PluginState*)refData;
        if (state) {
            switch (wParam) {
            case VK_ESCAPE:
                if (state->hParentWnd && IsWindow(state->hParentWnd))
                    PostMessage(state->hParentWnd, WM_CLOSE, 0, 0);
                return 0;
            case VK_F11:
                if (!state->showPlaylist)
                    SendMessage(state->hMainWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
                return 0;
            case VK_SPACE:
                SendMessage(state->hMainWnd, WM_COMMAND, IDM_PLAY, 0);
                return 0;
            case 0x4D: // M
                SendMessage(state->hMainWnd, WM_COMMAND, IDM_MUTE, 0);
                return 0;
            }
        }
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
   File Info dialog — structured metadata panel with album art
   ----------------------------------------------------------------------- */
struct MediaInfo {
    TCHAR fileName[MAX_PATH];
    TCHAR duration[32];
    TCHAR fileSize[32];
    TCHAR format[64];
    TCHAR codec[64];
    TCHAR bitrate[32];
    TCHAR channels[32];
    TCHAR sampleRate[32];
    TCHAR bitsPerSample[32];
    TCHAR resolution[32];
    TCHAR fps[16];
    TCHAR title[256];
    TCHAR artist[256];
    TCHAR album[256];
    TCHAR genre[128];
    TCHAR year[16];
    TCHAR track[16];
    HBITMAP hAlbumArt;
};

static HBITMAP LoadAlbumArtFromBytes(BYTE* data, DWORD size) {
    if (!data || size == 0) return NULL;
    // Try IPicture for OLE-compatible formats
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return NULL;
    void* pMem = GlobalLock(hMem);
    memcpy(pMem, data, size);
    GlobalUnlock(hMem);
    IStream* pStream = NULL;
    if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &pStream))) { GlobalFree(hMem); return NULL; }
    typedef HRESULT (WINAPI *OleLoadPicture_t)(IStream*, LONG, BOOL, REFIID, void**);
    static OleLoadPicture_t pOleLoadPicture = NULL;
    if (!pOleLoadPicture) {
        HMODULE hOle = LoadLibrary(TEXT("oleaut32.dll"));
        if (hOle) pOleLoadPicture = (OleLoadPicture_t)GetProcAddress(hOle, "OleLoadPicture");
    }
    if (pOleLoadPicture) {
        IPicture* pPic = NULL;
        HRESULT hr = pOleLoadPicture(pStream, 0, FALSE, IID_IPicture, (void**)&pPic);
        if (SUCCEEDED(hr) && pPic) {
            short picType = 0;
            pPic->get_Type(&picType);
            // Only render if it's a bitmap (PICTYPE_BITMAP = 1)
            if (picType == 1) {
                HDC hdcScreen = GetDC(NULL);
                HDC hdcMem = CreateCompatibleDC(hdcScreen);
                HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, 200, 200);
                HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);
                RECT rcRender = {0, 200, 200, 0};
                pPic->Render(hdcMem, 0, 0, 200, 200, 0, 200, -200, 200, &rcRender);
                SelectObject(hdcMem, hOld);
                DeleteDC(hdcMem);
                ReleaseDC(NULL, hdcScreen);
                pPic->Release();
                pStream->Release();
                return hBmp;
            }
            pPic->Release();
        }
    }
    pStream->Release();
    return NULL;
}

static void GetMediaInfo(const TCHAR* filePath, double duration, BOOL useDS, MediaInfo* info) {
    memset(info, 0, sizeof(MediaInfo));

    _tcsncpy(info->fileName, filePath, MAX_PATH - 1);
    TCHAR* fname = _tcsrchr(info->fileName, TEXT('\\'));
    if (fname) memmove(info->fileName, fname + 1, (_tcslen(fname + 1) + 1) * sizeof(TCHAR));

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesEx(filePath, GetFileExInfoStandard, &fad)) {
        ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (sz >= 1024 * 1024)
            _sntprintf(info->fileSize, 32, TEXT("%.1f MB"), sz / (1024.0 * 1024.0));
        else if (sz >= 1024)
            _sntprintf(info->fileSize, 32, TEXT("%.1f KB"), sz / 1024.0);
        else
            _sntprintf(info->fileSize, 32, TEXT("%llu bytes"), sz);
    }

    if (duration > 0) {
        int dm = (int)(duration / 60), ds = (int)duration % 60;
        _sntprintf(info->duration, 32, TEXT("%02d:%02d"), dm, ds);
    }

    // Ensure MF is initialized
    MFStartup(MF_VERSION);

    IMFSourceReader* reader = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(filePath, NULL, &reader);
    if (SUCCEEDED(hr) && reader) {
        // Try to read duration from source reader
        if (info->duration[0] == 0) {
            PROPVARIANT durVal;
            PropVariantInit(&durVal);
            // Try media source presentation attribute
            if (FAILED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &durVal)) ||
                durVal.vt != VT_I8) {
                PropVariantClear(&durVal);
                PropVariantInit(&durVal);
                // Try first audio stream
                hr = reader->GetPresentationAttribute(0, MF_PD_DURATION, &durVal);
            }
            if (SUCCEEDED(hr) && durVal.vt == VT_I8) {
                double dur = durVal.hVal.QuadPart / 10000000.0;
                int dm = (int)(dur / 60), ds = (int)dur % 60;
                _sntprintf(info->duration, 32, TEXT("%02d:%02d"), dm, ds);
            }
            PropVariantClear(&durVal);
        }
        IMFMediaType* audioType = NULL;
        hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &audioType);
        if (SUCCEEDED(hr) && audioType) {
            GUID subtype;
            if (SUCCEEDED(audioType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                if (subtype == MFAudioFormat_AAC) _tcscpy(info->codec, TEXT("AAC"));
                else if (subtype == MFAudioFormat_MP3) _tcscpy(info->codec, TEXT("MP3"));
                else if (subtype == MFAudioFormat_Float) _tcscpy(info->codec, TEXT("PCM Float"));
                else if (subtype == MFAudioFormat_PCM) _tcscpy(info->codec, TEXT("PCM"));
                else if (subtype == MFAudioFormat_WMAudioV8) _tcscpy(info->codec, TEXT("WMA v8"));
                else if (subtype == MFAudioFormat_WMAudioV9) _tcscpy(info->codec, TEXT("WMA v9"));
                else _sntprintf(info->codec, 64, TEXT("Audio (%08X)"), (DWORD)subtype.Data1);
            }
            UINT32 val = 0;
            if (SUCCEEDED(audioType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &val)))
                _sntprintf(info->channels, 32, TEXT("%u"), val);
            if (SUCCEEDED(audioType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &val)))
                _sntprintf(info->sampleRate, 32, TEXT("%u Hz"), val);
            if (SUCCEEDED(audioType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &val)))
                _sntprintf(info->bitsPerSample, 32, TEXT("%u bit"), val);
            if (SUCCEEDED(audioType->GetUINT32(MF_MT_AVG_BITRATE, &val)))
                _sntprintf(info->bitrate, 32, TEXT("%u kbps"), val / 1000);
            audioType->Release();
        }
        IMFMediaType* videoType = NULL;
        hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &videoType);
        if (SUCCEEDED(hr) && videoType) {
            GUID subtype;
            if (SUCCEEDED(videoType->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                if (subtype == MFVideoFormat_H264) _tcscpy(info->codec, TEXT("H.264"));
                else if (subtype == MFVideoFormat_HEVC) _tcscpy(info->codec, TEXT("HEVC"));
                else if (subtype == MFVideoFormat_VP80) _tcscpy(info->codec, TEXT("VP8"));
                else if (subtype == MFVideoFormat_VP90) _tcscpy(info->codec, TEXT("VP9"));
                else if (subtype == MFVideoFormat_MPEG2) _tcscpy(info->codec, TEXT("MPEG-2"));
                else _sntprintf(info->codec, 64, TEXT("Video (%08X)"), (DWORD)subtype.Data1);
                _tcscpy(info->format, TEXT("Video"));
            }
            UINT32 w = 0, h = 0;
            if (SUCCEEDED(MFGetAttributeSize(videoType, MF_MT_FRAME_SIZE, &w, &h)))
                _sntprintf(info->resolution, 32, TEXT("%u\u00D7%u"), w, h);
            UINT32 num = 0, den = 0;
            if (SUCCEEDED(MFGetAttributeRatio(videoType, MF_MT_FRAME_RATE, &num, &den)) && den > 0)
                _sntprintf(info->fps, 16, TEXT("%.1f"), (double)num / den);
            UINT32 val = 0;
            if (SUCCEEDED(videoType->GetUINT32(MF_MT_AVG_BITRATE, &val)))
                _sntprintf(info->bitrate, 32, TEXT("%u kbps"), val / 1000);
            videoType->Release();
        }
        if (info->format[0] == 0) _tcscpy(info->format, TEXT("Audio"));
        reader->Release();
    }

    // Bitrate fallback: calculate from file size and duration only if tags don't have it
    if (info->bitrate[0] == 0 && info->duration[0]) {
        WIN32_FILE_ATTRIBUTE_DATA fad2;
        if (GetFileAttributesEx(filePath, GetFileExInfoStandard, &fad2)) {
            ULONGLONG sz = ((ULONGLONG)fad2.nFileSizeHigh << 32) | fad2.nFileSizeLow;
            int dm = 0, ds = 0;
            _stscanf(info->duration, TEXT("%d:%d"), &dm, &ds);
            double dur = dm * 60.0 + ds;
            if (dur > 0) {
                UINT32 kbps = (UINT32)((sz * 8) / (dur * 1000));
                if (kbps > 0)
                    _sntprintf(info->bitrate, 32, TEXT("%u kbps"), kbps);
            }
        }
    }

    // Try IPropertyStore via Shell
    IPropertyStore* props = NULL;
    IShellItem2* shellItem = NULL;
    hr = SHCreateItemFromParsingName(filePath, NULL, IID_IShellItem2, (void**)&shellItem);
    if (SUCCEEDED(hr) && shellItem) {
        shellItem->GetPropertyStore(GPS_DEFAULT, IID_IPropertyStore, (void**)&props);
        // Fallback: try StgOpenStorageEx
        if (!props) {
            IStorage* pStorage = NULL;
            hr = StgOpenStorageEx(filePath, STGM_READ | STGM_SHARE_DENY_WRITE,
                STGFMT_FILE, 0, NULL, NULL, IID_IStorage, (void**)&pStorage);
            if (SUCCEEDED(hr) && pStorage) {
                pStorage->QueryInterface(IID_IPropertyStore, (void**)&props);
                pStorage->Release();
            }
        }
        if (props) {
            PROPVARIANT val;

            // Helper to read string property (handles both VT_LPWSTR and VT_VECTOR|VT_LPWSTR)
            auto readString = [&](REFPROPERTYKEY pk, TCHAR* dst, int dstMax) {
                PROPVARIANT v;
                PropVariantInit(&v);
                HRESULT hr = props->GetValue(pk, &v);
                if (SUCCEEDED(hr)) {
                    if (v.vt == VT_LPWSTR && v.pwszVal)
                        _tcsncpy(dst, v.pwszVal, dstMax - 1);
                    else if (v.vt == (VT_VECTOR | VT_LPWSTR) && v.calpwstr.cElems > 0 && v.calpwstr.pElems[0])
                        _tcsncpy(dst, v.calpwstr.pElems[0], dstMax - 1);
                }
                PropVariantClear(&v);
            };

            readString(PKEY_Title, info->title, 256);
            readString(PKEY_Music_Artist, info->artist, 256);
            readString(PKEY_Music_AlbumTitle, info->album, 256);
            readString(PKEY_Music_ContentGroupDescription, info->genre, 128);

            // Bitrate from tags
            PropVariantInit(&val);
            if (SUCCEEDED(props->GetValue(kPKEY_Audio_EncodingBitrate, &val))) {
                if (val.vt == VT_UI4 && val.ulVal > 0)
                    _sntprintf(info->bitrate, 32, TEXT("%u kbps"), val.ulVal / 1000);
                else if (val.vt == VT_I4 && val.lVal > 0)
                    _sntprintf(info->bitrate, 32, TEXT("%d kbps"), val.lVal / 1000);
            }
            PropVariantClear(&val);

            // Track number - try multiple types
            PropVariantInit(&val);
            if (SUCCEEDED(props->GetValue(kPKEY_Music_TrackNumber, &val))) {
                if (val.vt == VT_UI4)
                    _sntprintf(info->track, 16, TEXT("%u"), val.ulVal);
                else if (val.vt == VT_I4)
                    _sntprintf(info->track, 16, TEXT("%d"), val.lVal);
                else if (val.vt == VT_LPWSTR && val.pwszVal)
                    _tcsncpy(info->track, val.pwszVal, 15);
            }
            PropVariantClear(&val);

            // Duration from tags
            if (info->duration[0] == 0) {
                PropVariantInit(&val);
                if (SUCCEEDED(props->GetValue(kPKEY_Media_Duration, &val))) {
                    if (val.vt == VT_UI8) {
                        double dur = val.uhVal.QuadPart / 10000000.0;
                        int dm = (int)(dur / 60), ds = (int)dur % 60;
                        _sntprintf(info->duration, 32, TEXT("%02d:%02d"), dm, ds);
                    } else if (val.vt == VT_I8) {
                        double dur = val.hVal.QuadPart / 10000000.0;
                        int dm = (int)(dur / 60), ds = (int)dur % 60;
                        _sntprintf(info->duration, 32, TEXT("%02d:%02d"), dm, ds);
                    }
                }
                PropVariantClear(&val);
            }

            // Year
            PropVariantInit(&val);
            if (SUCCEEDED(props->GetValue(PKEY_Media_DateReleased, &val))) {
                if (val.vt == VT_UI4)
                    _sntprintf(info->year, 16, TEXT("%u"), val.ulVal);
                else if (val.vt == VT_LPWSTR && val.pwszVal && _tcslen(val.pwszVal) >= 4)
                    _tcsncpy(info->year, val.pwszVal, 4);
            }
            PropVariantClear(&val);
            // Fallback year key
            if (info->year[0] == 0) {
                PropVariantInit(&val);
                if (SUCCEEDED(props->GetValue(PKEY_Media_DateEncoded, &val))) {
                    if (val.vt == VT_FILETIME) {
                        SYSTEMTIME st;
                        if (FileTimeToSystemTime(&val.filetime, &st))
                            _sntprintf(info->year, 16, TEXT("%u"), st.wYear);
                    }
                }
                PropVariantClear(&val);
            }

            // Album art: embedded blob via IPicture
            PropVariantInit(&val);
            if (SUCCEEDED(props->GetValue(kPKEY_Music_AlbumCoverArt, &val))) {
                BYTE* pData = NULL;
                DWORD cbSize = 0;
                if (val.vt == (VT_VECTOR | VT_UI1)) {
                    pData = val.caub.pElems;
                    cbSize = val.caub.cElems;
                } else if ((val.vt & VT_ARRAY) && val.parray) {
                    pData = (BYTE*)val.parray->pvData;
                    cbSize = val.parray->rgsabound[0].cElements;
                }
                if (pData && cbSize > 0)
                    info->hAlbumArt = LoadAlbumArtFromBytes(pData, cbSize);
            }
            PropVariantClear(&val);
            props->Release();
        }
        // Shell thumbnail fallback (outside props block)
        if (!info->hAlbumArt) {
            IShellItemImageFactory* imgFactory = NULL;
            hr = shellItem->QueryInterface(IID_IShellItemImageFactory, (void**)&imgFactory);
            if (SUCCEEDED(hr) && imgFactory) {
                SIZE sz = {200, 200};
                imgFactory->GetImage(sz, 0x0001, &info->hAlbumArt); // SIIGBF_THUMBNAIL
                imgFactory->Release();
            }
        }
        shellItem->Release();
    }
}

struct FileInfoData {
    MediaInfo info;
};

static LRESULT CALLBACK FileInfoWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        FileInfoData* fd = (FileInfoData*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (fd && fd->info.hAlbumArt) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, fd->info.hAlbumArt);
            BITMAP bm;
            GetObject(fd->info.hAlbumArt, sizeof(bm), &bm);
            int maxW = 240, maxH = 240;
            double scale = min((double)maxW / bm.bmWidth, (double)maxH / bm.bmHeight);
            int w = (int)(bm.bmWidth * scale), h = (int)(bm.bmHeight * scale);
            int x = 16 + (maxW - w) / 2, y = 16 + (maxH - h) / 2;
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, x, y, w, h, hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            DestroyWindow(hWnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        PostMessage(hWnd, WM_USER, 0, 0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void ShowFileInfoDialog(HWND hParent, const TCHAR* filePath, double duration, BOOL useDS) {
    FileInfoData* fd = (FileInfoData*)calloc(1, sizeof(FileInfoData));
    GetMediaInfo(filePath, duration, useDS, &fd->info);

    static BOOL registered = FALSE;
    if (!registered) {
        WNDCLASSEX wc = {0};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = FileInfoWndProc;
        wc.hInstance = GetModuleHandle(0);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = TEXT("MediaShow2Info");
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassEx(&wc);
        registered = TRUE;
    }

    // Fixed layout: art on left, metadata on right
    int artW = 272;  // 16 + 240 + 16
    int colX = artW;
    int colW = 400;
    int dlgW = colX + colW + 16;

    // Calculate height: count all visible fields
    int lines = 0;
    lines += 4; // General: File, Duration, Size, Format
    if (fd->info.codec[0] || fd->info.bitrate[0] || fd->info.channels[0]) lines += 7; // Technical
    if (fd->info.title[0] || fd->info.artist[0] || fd->info.album[0]) lines += 6; // Tags
    int dlgH = 32 + lines * 24 + 16; // top margin + fields + bottom margin

    HWND hWnd = CreateWindowEx(WS_EX_DLGMODALFRAME, TEXT("MediaShow2Info"), TEXT("File Info"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        0, 0, dlgW, dlgH, hParent, NULL, GetModuleHandle(0), NULL);
    SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)fd);

    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int y = 16;

    auto addSection = [&](const TCHAR* title) {
        y += 4;
        HWND hs = CreateWindowEx(0, TEXT("STATIC"), title,
            WS_CHILD | WS_VISIBLE, colX, y, colW, 18, hWnd, NULL, GetModuleHandle(0), NULL);
        SendMessage(hs, WM_SETFONT, (WPARAM)hFont, TRUE);
        HDC hdc = GetDC(hWnd);
        HPEN hPen = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_GRAYTEXT));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        MoveToEx(hdc, colX, y + 17, NULL);
        LineTo(hdc, colX + colW, y + 17);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
        ReleaseDC(hWnd, hdc);
        y += 20;
    };

    auto addField = [&](const TCHAR* label, const TCHAR* value) {
        if (!value || !value[0]) return;
        HWND hl = CreateWindowEx(0, TEXT("STATIC"), label,
            WS_CHILD | WS_VISIBLE, colX, y + 3, 90, 20, hWnd, NULL, GetModuleHandle(0), NULL);
        SendMessage(hl, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hv = CreateWindowEx(0, TEXT("EDIT"), (TCHAR*)value,
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            colX + 94, y, colW - 94, 20, hWnd, NULL, GetModuleHandle(0), NULL);
        SendMessage(hv, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 24;
    };

    addSection(TEXT("General"));
    addField(TEXT("File:"), fd->info.fileName);
    addField(TEXT("Duration:"), fd->info.duration);
    addField(TEXT("Size:"), fd->info.fileSize);
    addField(TEXT("Format:"), fd->info.format);

    if (fd->info.codec[0] || fd->info.bitrate[0] || fd->info.channels[0]) {
        addSection(TEXT("Technical"));
        addField(TEXT("Codec:"), fd->info.codec);
        addField(TEXT("Bitrate:"), fd->info.bitrate);
        addField(TEXT("Channels:"), fd->info.channels);
        addField(TEXT("Sample Rate:"), fd->info.sampleRate);
        addField(TEXT("Bit Depth:"), fd->info.bitsPerSample);
        addField(TEXT("Resolution:"), fd->info.resolution);
        addField(TEXT("FPS:"), fd->info.fps);
    }

    if (fd->info.title[0] || fd->info.artist[0] || fd->info.album[0]) {
        addSection(TEXT("Tags"));
        addField(TEXT("Title:"), fd->info.title);
        addField(TEXT("Artist:"), fd->info.artist);
        addField(TEXT("Album:"), fd->info.album);
        addField(TEXT("Track:"), fd->info.track);
        addField(TEXT("Genre:"), fd->info.genre);
        addField(TEXT("Year:"), fd->info.year);
    }

    RECT pr; GetWindowRect(hParent, &pr);
    SetWindowPos(hWnd, HWND_TOP, pr.left + (pr.right - pr.left - dlgW) / 2,
        pr.top + (pr.bottom - pr.top - dlgH) / 2, 0, 0, SWP_NOSIZE);

    EnableWindow(hParent, FALSE);
    MSG msg;
    while (IsWindow(hWnd)) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            if (IsWindow(hWnd) && IsDialogMessage(hWnd, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            WaitMessage();
        }
    }
    EnableWindow(hParent, TRUE);
    SetFocus(hParent);

    if (fd->info.hAlbumArt) DeleteObject(fd->info.hAlbumArt);
    free(fd);
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
        state->appendMode = LoadAppendMode();
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
        SetWindowSubclass(state->hPlaylist, PlaylistProc, 0, (DWORD_PTR)state);

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
                KillTimer(state->hMainWnd, IDT_COOLDOWN);
                state->switchInProgress = FALSE;
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
                    KillTimer(state->hMainWnd, IDT_COOLDOWN);
                    state->switchInProgress = FALSE;
                    PlayIndex(state, idx);
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
        if (state->switchInProgress) return 0;
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

        case IDM_PLAY: {
            // Check if a different file is selected in playlist
            int selIdx = -1;
            if (state->hPlaylist)
                selIdx = ListView_GetNextItem(state->hPlaylist, -1, LVNI_SELECTED);
            if (selIdx >= 0 && selIdx < state->playlistCount && selIdx != state->playlistIndex) {
                PlayIndex(state, selIdx);
            } else if (state->isPlaying && !state->isPaused) {
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
        }

        case IDM_STOP:
            KillTimer(hWnd, IDT_COOLDOWN);
            state->switchInProgress = FALSE;
            if (state->useDirectShow) DSPlayer_Stop(state->pDSPlayer);
            else                      MFPlayer_Stop(state->pMFPlayer);
            state->isPlaying = FALSE;
            state->isPaused  = FALSE;
            state->position  = 0;
            UpdateStatus(state);
            UpdateSeekbar(state);
            break;

        case IDM_PREV:
            if (!state->switchInProgress && state->playlist && state->playlistCount > 0) {
                int idx = state->playlistIndex - 1;
                if (idx < 0) idx = (state->repeatMode == 1) ? state->playlistCount - 1 : 0;
                PlayIndex(state, idx);
            }
            break;

        case IDM_NEXT:
            if (!state->switchInProgress && state->playlist && state->playlistCount > 0) {
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

        case IDM_APPENDMODE:
            state->appendMode = !state->appendMode;
            SaveAppendMode(state);
            break;

        case IDM_CLEARPLAYLIST:
            ClearPlaylist(state);
            UpdatePlaylist(state);
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
            TCHAR* filePath = state->filePath;
            double dur = state->duration;
            BOOL useDS = state->useDirectShow;
            if (state->showPlaylist && state->playlist && state->playlistCount > 0) {
                int selIdx = ListView_GetNextItem(state->hPlaylist, -1, LVNI_SELECTED);
                if (selIdx >= 0 && selIdx < state->playlistCount) {
                    filePath = state->playlist[selIdx];
                    dur = 0; // will be read from file
                    useDS = FALSE;
                }
            }
            ShowFileInfoDialog(hWnd, filePath, dur, useDS);
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
        if (wParam == IDT_COOLDOWN && state) {
            KillTimer(hWnd, IDT_COOLDOWN);
            state->switchInProgress = FALSE;
        } else if (wParam == 1 && state) {
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
            RemoveWindowSubclass(state->hPlaylist, PlaylistProc, 0);
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

    // Append mode: check if existing plugin window is still alive
    static HWND hLastPluginWnd = NULL;
    if (hLastPluginWnd && IsWindow(hLastPluginWnd)) {
        PluginState* existState = GetState(hLastPluginWnd);
        if (existState && existState->appendMode) {
            // Get selected files from TC
            HWND hTC = FindWindow(TEXT("TTOTAL_CMD"), NULL);
            if (hTC) {
                EnumFindData fd = {0, 0};
                EnumChildWindows(hTC, EnumFindLCLListBox, (LPARAM)&fd);
                if (fd.result) {
                    HWND hListBox = fd.result;
                    int selCount = (int)SendMessage(hListBox, LB_GETSELCOUNT, 0, 0);

                    if (selCount > 0) {
                        int* selItems = (int*)calloc(selCount, sizeof(int));
                        SendMessage(hListBox, LB_GETSELITEMS, selCount, (LPARAM)selItems);

                        TCHAR dir[MAX_PATH];
                        _tcsncpy(dir, FileToLoad, MAX_PATH - 1);
                        TCHAR* lastSlash = _tcsrchr(dir, TEXT('\\'));
                        if (lastSlash) *lastSlash = 0;

                        int allocSize = selCount;
                        TCHAR** files = (TCHAR**)calloc(allocSize, sizeof(TCHAR*));
                        FILETIME* dates = (FILETIME*)calloc(allocSize, sizeof(FILETIME));
                        int validCount = 0;

                        for (int i = 0; i < selCount; i++) {
                            int len = (int)SendMessage(hListBox, LB_GETTEXTLEN, selItems[i], 0);
                            if (len <= 0) continue;
                            TCHAR* buf = (TCHAR*)calloc(len + 1, sizeof(TCHAR));
                            SendMessage(hListBox, LB_GETTEXT, selItems[i], (LPARAM)buf);

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
                                while (beforeLen > 0 && (fileName[beforeLen-1] == TEXT(' ') || fileName[beforeLen-1] == 0x00A0 || fileName[beforeLen-1] == 0x0009))
                                    fileName[--beforeLen] = TEXT('\0');
                                TCHAR* p = fileName + beforeLen - 1;
                                while (p > fileName && *p >= '0' && *p <= '9') p--;
                                if (p > fileName && (*p == TEXT(' ') || *p == 0x00A0 || *p == 0x0009)) p--;
                                else { p = fileName + beforeLen - 1; }
                                while (p > fileName && *p >= '0' && *p <= '9') p--;
                                if (p > fileName && (*p == TEXT(' ') || *p == 0x00A0 || *p == 0x0009)) p--;
                                else { p = fileName + beforeLen - 1; }
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

                            TCHAR* dot = _tcsrchr(fileName, TEXT('.'));
                            if (!dot || !IsMediaFile(dot + 1)) { free(buf); continue; }
                            if (IsDuplicate(existState, fullPath)) { free(buf); continue; }

                            files[validCount] = _tcsdup(fullPath);
                            WIN32_FILE_ATTRIBUTE_DATA fad;
                            if (GetFileAttributesEx(fullPath, GetFileExInfoStandard, &fad))
                                dates[validCount] = fad.ftLastWriteTime;
                            validCount++;
                            free(buf);
                        }
                        free(selItems);

                        if (validCount > 0) {
                            int oldCount = existState->playlistCount;
                            int newTotal = oldCount + validCount;
                            TCHAR** newPl = (TCHAR**)realloc(existState->playlist, newTotal * sizeof(TCHAR*));
                            FILETIME* newDt = (FILETIME*)realloc(existState->fileDates, newTotal * sizeof(FILETIME));
                            if (newPl && newDt) {
                                for (int i = 0; i < validCount; i++) {
                                    newPl[oldCount + i] = files[i];
                                    newDt[oldCount + i] = dates[i];
                                }
                                existState->playlist = newPl;
                                existState->fileDates = newDt;
                                existState->playlistCount = newTotal;
                                UpdatePlaylist(existState);
                                SavePlaylist(existState);
                            } else {
                                for (int i = 0; i < validCount; i++) free(files[i]);
                                free(files); free(dates);
                            }
                        } else {
                            free(files); free(dates);
                        }
                    }
                } else {
                    // No selection: scan directory and add all media files
                    TCHAR dir[MAX_PATH];
                    _tcsncpy(dir, FileToLoad, MAX_PATH - 1);
                    TCHAR* lastSlash = _tcsrchr(dir, TEXT('\\'));
                    if (lastSlash) *lastSlash = 0;

                    TCHAR** files = NULL;
                    FILETIME* dates = NULL;
                    int count = 0;
                    ScanDirectoryForMedia(dir, &files, &dates, &count);

                    if (files && count > 0) {
                        int added = 0;
                        for (int i = 0; i < count; i++) {
                            if (IsDuplicate(existState, files[i])) {
                                free(files[i]);
                                continue;
                            }
                            int newTotal = existState->playlistCount + 1;
                            TCHAR** newPl = (TCHAR**)realloc(existState->playlist, newTotal * sizeof(TCHAR*));
                            FILETIME* newDt = (FILETIME*)realloc(existState->fileDates, newTotal * sizeof(FILETIME));
                            if (newPl && newDt) {
                                newPl[existState->playlistCount] = files[i];
                                newDt[existState->playlistCount] = dates[i];
                                existState->playlist = newPl;
                                existState->fileDates = newDt;
                                existState->playlistCount = newTotal;
                                added++;
                            } else {
                                free(files[i]);
                            }
                        }
                        if (added > 0) {
                            UpdatePlaylist(existState);
                            SavePlaylist(existState);
                        }
                        free(files); free(dates);
                    }
                }
            }
            PostMessage(ParentWin, WM_CLOSE, 0, 0);
            return NULL;
        }
    }

    // When append mode is OFF: close old lister tab before creating new one
    if (hLastPluginWnd && IsWindow(hLastPluginWnd)) {
        HWND hOldLister = GetParent(hLastPluginWnd);
        if (hOldLister && IsWindow(hOldLister))
            PostMessage(hOldLister, WM_CLOSE, 0, 0);
    }

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

    // Append mode ON: load saved playlist, add new files to it
    if (state->appendMode) {
        LoadPlaylist(state);
        // Add current file(s) to loaded playlist
        int oldCount = state->playlistCount;
        // Try to get selected files from TC
        HWND hTC = FindWindow(TEXT("TTOTAL_CMD"), NULL);
        if (hTC) {
            EnumFindData fd = {0, 0};
            EnumChildWindows(hTC, EnumFindLCLListBox, (LPARAM)&fd);
            if (fd.result) {
                int selCount = (int)SendMessage(fd.result, LB_GETSELCOUNT, 0, 0);
                if (selCount > 0) {
                    // Collect and append selected files (same logic as append mode F3)
                    int* selItems = (int*)calloc(selCount, sizeof(int));
                    SendMessage(fd.result, LB_GETSELITEMS, selCount, (LPARAM)selItems);
                    TCHAR dir[MAX_PATH];
                    _tcsncpy(dir, FileToLoad, MAX_PATH - 1);
                    TCHAR* ls = _tcsrchr(dir, TEXT('\\'));
                    if (ls) *ls = 0;
                    for (int i = 0; i < selCount; i++) {
                        int len = (int)SendMessage(fd.result, LB_GETTEXTLEN, selItems[i], 0);
                        if (len <= 0) continue;
                        TCHAR* buf = (TCHAR*)calloc(len + 1, sizeof(TCHAR));
                        SendMessage(fd.result, LB_GETTEXT, selItems[i], (LPARAM)buf);
                        // Parse filename from TC format
                        TCHAR* datePos = NULL;
                        for (TCHAR* p = buf; p[9]; p++) {
                            if (p[2] == TEXT('.') && p[5] == TEXT('.') &&
                                p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
                                p[3] >= '0' && p[3] <= '9' && p[4] >= '0' && p[4] <= '9' &&
                                p[6] >= '0' && p[6] <= '9' && p[7] >= '0' && p[7] <= '9' &&
                                p[8] >= '0' && p[8] <= '9' && p[9] >= '0' && p[9] <= '9') {
                                datePos = p; break;
                            }
                        }
                        TCHAR fn[MAX_PATH] = {0};
                        if (datePos) {
                            int bL = (int)(datePos - buf);
                            _tcsncpy(fn, buf, bL); fn[bL] = TEXT('\0');
                            while (bL > 0 && (fn[bL-1] == TEXT(' ') || fn[bL-1] == 0x00A0 || fn[bL-1] == 0x0009))
                                fn[--bL] = TEXT('\0');
                            TCHAR* pp = fn + bL - 1;
                            while (pp > fn && *pp >= '0' && *pp <= '9') pp--;
                            if (pp > fn && (*pp == TEXT(' ') || *pp == 0x00A0 || *pp == 0x0009)) pp--;
                            else { pp = fn + bL - 1; }
                            while (pp > fn && *pp >= '0' && *pp <= '9') pp--;
                            if (pp > fn && (*pp == TEXT(' ') || *pp == 0x00A0 || *pp == 0x0009)) pp--;
                            else { pp = fn + bL - 1; }
                            while (pp > fn && *pp >= '0' && *pp <= '9') pp--;
                            if (pp > fn && (*pp == TEXT(' ') || *pp == 0x00A0 || *pp == 0x0009)) pp--;
                            else { pp = fn + bL - 1; }
                            *(pp + 1) = TEXT('\0');
                            while (bL > 0 && (fn[bL-1] == TEXT(' ') || fn[bL-1] == 0x00A0 || fn[bL-1] == 0x0009))
                                fn[--bL] = TEXT('\0');
                        } else {
                            _tcsncpy(fn, buf, MAX_PATH - 1);
                        }
                        TCHAR fp[MAX_PATH];
                        _sntprintf(fp, MAX_PATH, TEXT("%s\\%s"), dir, fn);
                        TCHAR* dot = _tcsrchr(fn, TEXT('.'));
                        if (!dot || !IsMediaFile(dot + 1)) { free(buf); continue; }
                        if (IsDuplicate(state, fp)) { free(buf); continue; }
                        // Append to playlist
                        int newTotal = state->playlistCount + 1;
                        TCHAR** np = (TCHAR**)realloc(state->playlist, newTotal * sizeof(TCHAR*));
                        FILETIME* nd = (FILETIME*)realloc(state->fileDates, newTotal * sizeof(FILETIME));
                        if (np && nd) {
                            np[state->playlistCount] = _tcsdup(fp);
                            WIN32_FILE_ATTRIBUTE_DATA fad;
                            if (GetFileAttributesEx(fp, GetFileExInfoStandard, &fad))
                                nd[state->playlistCount] = fad.ftLastWriteTime;
                            state->playlist = np;
                            state->fileDates = nd;
                            state->playlistCount = newTotal;
                        }
                        free(buf);
                    }
                    free(selItems);
                }
            }
        }
        // If no selection and playlist didn't grow, scan directory
        if (state->playlistCount <= oldCount) {
            TCHAR dir[MAX_PATH];
            _tcsncpy(dir, FileToLoad, MAX_PATH - 1);
            TCHAR* ls = _tcsrchr(dir, TEXT('\\'));
            if (ls) *ls = 0;
            TCHAR** files = NULL;
            FILETIME* dates = NULL;
            int count = 0;
            ScanDirectoryForMedia(dir, &files, &dates, &count);
            if (files && count > 0) {
                int added = 0;
                for (int i = 0; i < count; i++) {
                    if (IsDuplicate(state, files[i])) {
                        free(files[i]);
                        continue;
                    }
                    int newTotal = state->playlistCount + 1;
                    TCHAR** np = (TCHAR**)realloc(state->playlist, newTotal * sizeof(TCHAR*));
                    FILETIME* nd = (FILETIME*)realloc(state->fileDates, newTotal * sizeof(FILETIME));
                    if (np && nd) {
                        np[state->playlistCount] = files[i];
                        nd[state->playlistCount] = dates[i];
                        state->playlist = np;
                        state->fileDates = nd;
                        state->playlistCount = newTotal;
                        added++;
                    } else {
                        free(files[i]);
                    }
                }
                free(files); free(dates);
            }
        }
        UpdatePlaylist(state);
        SavePlaylist(state);
    } else {
        // Append mode OFF: normal flow
        RequestSelectedFiles(ParentWin, state);
        if (state->playlistCount <= 1)
            BuildPlaylist(state, NULL, FileToLoad);
    }

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
    hLastPluginWnd = hWnd;
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
        KillTimer(ListWin, IDT_COOLDOWN);
        state->switchInProgress = FALSE;
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
