#include "mf_player.h"
#include <dshow.h>
#include <stdlib.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

struct tagMFPlayer {
    IGraphBuilder*    pGraph;
    IMediaControl*    pControl;
    IMediaEvent*      pEvent;
    IMediaSeeking*    pSeeking;
    IVideoWindow*     pVideoWindow;
    IBasicAudio*      pAudio;
    IBasicVideo*      pBasicVideo;
    HWND              hVideoWnd;
    HWND              hRenderWnd;
    MFPlayerEndCallback onEnd;
    void*             userData;
    BOOL              isOpen;
    volatile LONG     isPlaying;
    volatile LONG     isPaused;
    HANDLE            hEventThread;
    volatile LONG     stopThread;
};

static DWORD WINAPI EventThread(LPVOID lpParam) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    tagMFPlayer* p = (tagMFPlayer*)lpParam;
    long evCode = 0;
    while (!InterlockedCompareExchange(&p->stopThread, 0, 0)) {
        if (p->pEvent) {
            HRESULT hr = p->pEvent->WaitForCompletion(100, &evCode);
            if (SUCCEEDED(hr) && evCode == EC_COMPLETE) {
                InterlockedExchange(&p->isPlaying, FALSE);
                InterlockedExchange(&p->isPaused,  FALSE);
                if (p->onEnd) p->onEnd(p->userData);
            }
        } else {
            Sleep(100);
        }
    }
    CoUninitialize();
    return 0;
}

MFPlayer* MFPlayer_Create(HWND hVideoWnd, MFPlayerEndCallback onEnd, void* userData) {
    tagMFPlayer* p = (tagMFPlayer*)calloc(1, sizeof(tagMFPlayer));
    if (!p) return NULL;
    p->hVideoWnd = hVideoWnd;
    p->onEnd     = onEnd;
    p->userData  = userData;
    return (MFPlayer*)p;
}

static void StopEventThread(tagMFPlayer* p) {
    if (!p->hEventThread) return;
    InterlockedExchange(&p->stopThread, TRUE);
    WaitForSingleObject(p->hEventThread, 2000);
    CloseHandle(p->hEventThread);
    p->hEventThread = NULL;
    InterlockedExchange(&p->stopThread, FALSE);
}

static void ReleaseGraph(tagMFPlayer* p) {
    p->hRenderWnd = NULL;
    if (p->pVideoWindow) {
        p->pVideoWindow->put_Visible(OAFALSE);
        p->pVideoWindow->put_Owner(0);
        p->pVideoWindow->Release();
        p->pVideoWindow = NULL;
    }
    if (p->pBasicVideo) { p->pBasicVideo->Release(); p->pBasicVideo = NULL; }
    if (p->pAudio)      { p->pAudio->Release();      p->pAudio      = NULL; }
    if (p->pSeeking)    { p->pSeeking->Release();    p->pSeeking    = NULL; }
    if (p->pEvent)      { p->pEvent->Release();      p->pEvent      = NULL; }
    if (p->pControl)    { p->pControl->Release();    p->pControl    = NULL; }
    if (p->pGraph)      { p->pGraph->Release();      p->pGraph      = NULL; }
}

void MFPlayer_Destroy(MFPlayer* player) {
    if (!player) return;
    tagMFPlayer* p = (tagMFPlayer*)player;
    MFPlayer_Stop(player);
    StopEventThread(p);
    ReleaseGraph(p);
    free(p);
}

struct EnumCtx { HWND hExclude; HWND hFound; };
static BOOL CALLBACK EnumFindPopup(HWND hwnd, LPARAM lParam) {
    EnumCtx* ctx = (EnumCtx*)lParam;
    if (hwnd == ctx->hExclude) return TRUE;
    HWND hParent = (HWND)GetWindowLongPtr(hwnd, GWLP_HWNDPARENT);
    if (hParent == ctx->hExclude) {
        LONG s = GetWindowLong(hwnd, GWL_STYLE);
        if ((s & WS_POPUP) && (s & WS_VISIBLE)) {
            ctx->hFound = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static void ForceChildStyle(HWND hChild, HWND hParent) {
    if (!hChild || !hParent) return;
    SetParent(hChild, hParent);
    LONG style = GetWindowLong(hChild, GWL_STYLE);
    style = (style & ~WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS;
    SetWindowLong(hChild, GWL_STYLE, style);
    RECT rc;
    GetClientRect(hParent, &rc);
    SetWindowPos(hChild, NULL, 0, 0, rc.right, rc.bottom,
        SWP_FRAMECHANGED | SWP_NOZORDER);
}

HRESULT MFPlayer_Open(MFPlayer* player, const WCHAR* filePath) {
    if (!player || !filePath) return E_INVALIDARG;
    tagMFPlayer* p = (tagMFPlayer*)player;

    MFPlayer_Stop(player);
    StopEventThread(p);
    ReleaseGraph(p);

    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
        IID_IGraphBuilder, (void**)&p->pGraph);
    if (FAILED(hr)) return hr;

    p->pGraph->QueryInterface(IID_IMediaControl, (void**)&p->pControl);
    p->pGraph->QueryInterface(IID_IMediaEvent,   (void**)&p->pEvent);
    p->pGraph->QueryInterface(IID_IMediaSeeking, (void**)&p->pSeeking);
    p->pGraph->QueryInterface(IID_IBasicAudio,   (void**)&p->pAudio);

    hr = p->pGraph->RenderFile(filePath, NULL);
    if (FAILED(hr)) { ReleaseGraph(p); return hr; }

    p->pGraph->QueryInterface(IID_IVideoWindow, (void**)&p->pVideoWindow);
    p->pGraph->QueryInterface(IID_IBasicVideo,  (void**)&p->pBasicVideo);

    if (p->pVideoWindow) {
        // Show the renderer so its window exists
        p->pVideoWindow->put_Visible(OATRUE);

        // Find the renderer popup window by diffing top-level windows
        EnumCtx ctx = { p->hVideoWnd, NULL };
        EnumWindows(EnumFindPopup, (LPARAM)&ctx);
        p->hRenderWnd = ctx.hFound;

        if (p->hRenderWnd) {
            // Reparent via Win32 and force child style
            ForceChildStyle(p->hRenderWnd, p->hVideoWnd);
        } else {
            // Fallback: use IVideoWindow (may render outside)
            p->pVideoWindow->put_Owner((OAHWND)p->hVideoWnd);
            p->pVideoWindow->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS);
            p->pVideoWindow->put_MessageDrain((OAHWND)p->hVideoWnd);
            RECT rc;
            GetClientRect(p->hVideoWnd, &rc);
            p->pVideoWindow->SetWindowPosition(0, 0, rc.right, rc.bottom);
        }

        p->pVideoWindow->put_MessageDrain((OAHWND)p->hVideoWnd);
    }

    p->isOpen = TRUE;
    InterlockedExchange(&p->isPlaying, FALSE);
    InterlockedExchange(&p->isPaused,  FALSE);

    if (p->pEvent) {
        p->hEventThread = CreateThread(NULL, 0, EventThread, player, 0, NULL);
    }

    return S_OK;
}

HRESULT MFPlayer_Play(MFPlayer* player) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pControl) return E_FAIL;
    HRESULT hr = p->pControl->Run();
    if (SUCCEEDED(hr)) {
        InterlockedExchange(&p->isPlaying, TRUE);
        InterlockedExchange(&p->isPaused,  FALSE);
    }
    return hr;
}

HRESULT MFPlayer_Pause(MFPlayer* player) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pControl) return E_FAIL;
    HRESULT hr = p->pControl->Pause();
    if (SUCCEEDED(hr)) InterlockedExchange(&p->isPaused, TRUE);
    return hr;
}

HRESULT MFPlayer_Stop(MFPlayer* player) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pControl) return S_FALSE;
    HRESULT hr = p->pControl->Stop();
    if (SUCCEEDED(hr)) {
        InterlockedExchange(&p->isPlaying, FALSE);
        InterlockedExchange(&p->isPaused,  FALSE);
    }
    if (p->pSeeking) {
        LONGLONG pos = 0;
        p->pSeeking->SetPositions(&pos, AM_SEEKING_AbsolutePositioning,
            NULL, AM_SEEKING_NoPositioning);
    }
    return hr;
}

HRESULT MFPlayer_Seek(MFPlayer* player, double seconds) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pSeeking) return E_FAIL;
    LONGLONG pos = (LONGLONG)(seconds * 10000000.0);
    return p->pSeeking->SetPositions(&pos, AM_SEEKING_AbsolutePositioning,
        NULL, AM_SEEKING_NoPositioning);
}

HRESULT MFPlayer_SetVolume(MFPlayer* player, float volume) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pAudio) return E_FAIL;
    long dsVol = (volume <= 0.0f) ? -10000 : (long)(-10000.0 * (1.0 - volume));
    return p->pAudio->put_Volume(dsVol);
}

BOOL MFPlayer_IsPlaying(MFPlayer* player) {
    if (!player) return FALSE;
    tagMFPlayer* p = (tagMFPlayer*)player;
    return (BOOL)InterlockedCompareExchange(&p->isPlaying, 0, 0) &&
          !(BOOL)InterlockedCompareExchange(&p->isPaused,  0, 0);
}

BOOL MFPlayer_IsPaused(MFPlayer* player) {
    if (!player) return FALSE;
    return (BOOL)InterlockedCompareExchange(&((tagMFPlayer*)player)->isPaused, 0, 0);
}

double MFPlayer_GetDuration(MFPlayer* player) {
    if (!player) return 0;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pSeeking) return 0;
    LONGLONG dur = 0;
    if (FAILED(p->pSeeking->GetDuration(&dur))) return 0;
    return dur / 10000000.0;
}

double MFPlayer_GetPosition(MFPlayer* player) {
    if (!player) return 0;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pSeeking) return 0;
    LONGLONG pos = 0;
    if (FAILED(p->pSeeking->GetCurrentPosition(&pos))) return 0;
    return pos / 10000000.0;
}

HRESULT MFPlayer_GetCurrentVideoSize(MFPlayer* player, DWORD* width, DWORD* height) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pBasicVideo) return E_FAIL;
    long vidW = 0, vidH = 0;
    HRESULT hr = p->pBasicVideo->get_VideoWidth(&vidW);
    if (SUCCEEDED(hr)) hr = p->pBasicVideo->get_VideoHeight(&vidH);
    if (SUCCEEDED(hr)) {
        if (width)  *width  = (DWORD)vidW;
        if (height) *height = (DWORD)vidH;
    }
    return hr;
}

void MFPlayer_UpdateVideoWindow(MFPlayer* player, RECT* rc) {
    if (!player) return;
    tagMFPlayer* p = (tagMFPlayer*)player;

    RECT wrc;
    if (rc) {
        wrc = *rc;
    } else if (p->hVideoWnd) {
        GetClientRect(p->hVideoWnd, &wrc);
    } else {
        return;
    }

    int cw = wrc.right  - wrc.left;
    int ch = wrc.bottom - wrc.top;
    if (cw <= 0 || ch <= 0) return;

    int vx = 0, vy = 0, vw = cw, vh = ch;

    if (p->pBasicVideo) {
        long natW = 0, natH = 0;
        if (SUCCEEDED(p->pBasicVideo->get_VideoWidth(&natW)) &&
            SUCCEEDED(p->pBasicVideo->get_VideoHeight(&natH)) &&
            natW > 0 && natH > 0) {
            double srcAr = (double)natW / (double)natH;
            double dstAr = (double)cw   / (double)ch;
            if (srcAr > dstAr) {
                vw = cw;
                vh = (int)((double)cw / srcAr);
                vy = (ch - vh) / 2;
            } else {
                vh = ch;
                vw = (int)((double)ch * srcAr);
                vx = (cw - vw) / 2;
            }
        }
    }

    if (p->hRenderWnd) {
        SetWindowPos(p->hRenderWnd, NULL, vx, vy, vw, vh, SWP_NOZORDER);
    } else if (p->pVideoWindow) {
        p->pVideoWindow->SetWindowPosition(vx, vy, vw, vh);
    }
}
