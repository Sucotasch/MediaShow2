#include "ds_player.h"
#include <dshow.h>
#include <stdlib.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

struct tagDSPlayer {
    IGraphBuilder*    pGraph;
    IMediaControl*    pControl;
    IMediaEvent*      pEvent;
    IMediaSeeking*    pSeeking;
    IVideoWindow*     pVideoWindow;
    IBasicAudio*      pAudio;
    IBasicVideo*      pBasicVideo;
    HWND              hVideoWnd;
    DSEndCallback     onEnd;
    void*             userData;
    BOOL              isOpen;
    volatile LONG     isPlaying;
    volatile LONG     isPaused;
    HANDLE            hEventThread;
    volatile BOOL     stopThread;
};

static DWORD WINAPI EventThread(LPVOID lpParam) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    tagDSPlayer* p = (tagDSPlayer*)lpParam;
    long evCode = 0;
    while (!p->stopThread) {
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

DSPlayer* DSPlayer_Create(HWND hVideoWnd, DSEndCallback onEnd, void* userData) {
    tagDSPlayer* p = (tagDSPlayer*)calloc(1, sizeof(tagDSPlayer));
    if (!p) return NULL;
    p->hVideoWnd = hVideoWnd;
    p->onEnd     = onEnd;
    p->userData  = userData;
    return (DSPlayer*)p;
}

static void DS_StopEventThread(tagDSPlayer* p) {
    if (!p->hEventThread) return;
    p->stopThread = TRUE;
    WaitForSingleObject(p->hEventThread, 2000);
    CloseHandle(p->hEventThread);
    p->hEventThread = NULL;
    p->stopThread = FALSE;
}

static void DS_ReleaseGraph(tagDSPlayer* p) {
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

void DSPlayer_Destroy(DSPlayer* player) {
    if (!player) return;
    tagDSPlayer* p = (tagDSPlayer*)player;
    DSPlayer_Stop(player);
    DS_StopEventThread(p);
    DS_ReleaseGraph(p);
    free(p);
}

HRESULT DSPlayer_Open(DSPlayer* player, const WCHAR* filePath) {
    if (!player || !filePath) return E_INVALIDARG;
    tagDSPlayer* p = (tagDSPlayer*)player;

    DSPlayer_Stop(player);
    DS_StopEventThread(p);
    DS_ReleaseGraph(p);

    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
        IID_IGraphBuilder, (void**)&p->pGraph);
    if (FAILED(hr)) return hr;

    p->pGraph->QueryInterface(IID_IMediaControl, (void**)&p->pControl);
    p->pGraph->QueryInterface(IID_IMediaEvent,   (void**)&p->pEvent);
    p->pGraph->QueryInterface(IID_IMediaSeeking, (void**)&p->pSeeking);
    p->pGraph->QueryInterface(IID_IBasicAudio,   (void**)&p->pAudio);

    hr = p->pGraph->RenderFile(filePath, NULL);
    if (FAILED(hr)) { DS_ReleaseGraph(p); return hr; }

    p->pGraph->QueryInterface(IID_IVideoWindow, (void**)&p->pVideoWindow);
    p->pGraph->QueryInterface(IID_IBasicVideo,  (void**)&p->pBasicVideo);

    if (p->pVideoWindow) {
        p->pVideoWindow->put_Owner((OAHWND)p->hVideoWnd);
        p->pVideoWindow->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS);
        p->pVideoWindow->put_MessageDrain((OAHWND)p->hVideoWnd);

        RECT rc;
        GetClientRect(p->hVideoWnd, &rc);
        p->pVideoWindow->SetWindowPosition(0, 0, rc.right, rc.bottom);
        p->pVideoWindow->put_Visible(OATRUE);
    }

    p->isOpen    = TRUE;
    p->isPlaying = FALSE;
    p->isPaused  = FALSE;

    if (p->pEvent)
        p->hEventThread = CreateThread(NULL, 0, EventThread, player, 0, NULL);

    return S_OK;
}

HRESULT DSPlayer_Play(DSPlayer* player) {
    if (!player) return E_FAIL;
    tagDSPlayer* p = (tagDSPlayer*)player;
    if (!p->pControl) return E_FAIL;
    HRESULT hr = p->pControl->Run();
    if (SUCCEEDED(hr)) {
        InterlockedExchange(&p->isPlaying, TRUE);
        InterlockedExchange(&p->isPaused,  FALSE);
    }
    return hr;
}

HRESULT DSPlayer_Pause(DSPlayer* player) {
    if (!player) return E_FAIL;
    tagDSPlayer* p = (tagDSPlayer*)player;
    if (!p->pControl) return E_FAIL;
    HRESULT hr = p->pControl->Pause();
    if (SUCCEEDED(hr)) InterlockedExchange(&p->isPaused, TRUE);
    return hr;
}

HRESULT DSPlayer_Stop(DSPlayer* player) {
    if (!player) return E_FAIL;
    tagDSPlayer* p = (tagDSPlayer*)player;
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

HRESULT DSPlayer_Seek(DSPlayer* player, double seconds) {
    if (!player) return E_FAIL;
    tagDSPlayer* p = (tagDSPlayer*)player;
    if (!p->pSeeking) return E_FAIL;
    LONGLONG pos = (LONGLONG)(seconds * 10000000.0);
    return p->pSeeking->SetPositions(&pos, AM_SEEKING_AbsolutePositioning,
        NULL, AM_SEEKING_NoPositioning);
}

HRESULT DSPlayer_SetVolume(DSPlayer* player, long volume) {
    if (!player) return E_FAIL;
    tagDSPlayer* p = (tagDSPlayer*)player;
    if (!p->pAudio) return E_FAIL;
    long dsVol = (volume <= 0) ? -10000 : (long)(-10000.0 * (1.0 - volume / 100.0));
    return p->pAudio->put_Volume(dsVol);
}

BOOL DSPlayer_IsPlaying(DSPlayer* player) {
    if (!player) return FALSE;
    tagDSPlayer* p = (tagDSPlayer*)player;
    return (BOOL)InterlockedCompareExchange(&p->isPlaying, 0, 0) &&
          !(BOOL)InterlockedCompareExchange(&p->isPaused,  0, 0);
}

BOOL DSPlayer_IsPaused(DSPlayer* player) {
    if (!player) return FALSE;
    return (BOOL)InterlockedCompareExchange(&((tagDSPlayer*)player)->isPaused, 0, 0);
}

double DSPlayer_GetDuration(DSPlayer* player) {
    if (!player) return 0;
    tagDSPlayer* p = (tagDSPlayer*)player;
    if (!p->pSeeking) return 0;
    LONGLONG dur = 0;
    if (FAILED(p->pSeeking->GetDuration(&dur))) return 0;
    return dur / 10000000.0;
}

double DSPlayer_GetPosition(DSPlayer* player) {
    if (!player) return 0;
    tagDSPlayer* p = (tagDSPlayer*)player;
    if (!p->pSeeking) return 0;
    LONGLONG pos = 0;
    if (FAILED(p->pSeeking->GetCurrentPosition(&pos))) return 0;
    return pos / 10000000.0;
}

double DSPlayer_GetAspectRatio(DSPlayer* player) {
    if (!player) return 0;
    tagDSPlayer* p = (tagDSPlayer*)player;
    if (!p->pBasicVideo) return 0;
    long natW = 0, natH = 0;
    if (SUCCEEDED(p->pBasicVideo->get_VideoWidth(&natW)) &&
        SUCCEEDED(p->pBasicVideo->get_VideoHeight(&natH)) &&
        natW > 0 && natH > 0) {
        return (double)natW / (double)natH;
    }
    return 0;
}

void DSPlayer_UpdateVideoWindow(DSPlayer* player, RECT* rc) {
    if (!player) return;
    tagDSPlayer* p = (tagDSPlayer*)player;
    if (!p->pVideoWindow) return;

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

    p->pVideoWindow->SetWindowPosition(0, 0, cw, ch);
}
