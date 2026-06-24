#include "mf_player.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <evr.h>
#include <stdlib.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfuuid.lib")

struct tagMFPlayer {
    IMFPMediaPlayer*        pPlayer;
    IMFVideoDisplayControl*  pVideoCtrl;
    HWND                    hVideoWnd;
    MFPlayerEndCallback     onEnd;
    void*                   userData;
    volatile LONG           isPlaying;
    volatile LONG           isPaused;
};

class MFCallback : public IMFPMediaPlayerCallback {
public:
    MFCallback(tagMFPlayer* p) : m_p(p), m_ref(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFPMediaPlayerCallback) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { ULONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* pEventHeader) {
        if (!pEventHeader) return;
        if (pEventHeader->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
            InterlockedExchange(&m_p->isPlaying, FALSE);
            InterlockedExchange(&m_p->isPaused,  FALSE);
            if (m_p->onEnd) m_p->onEnd(m_p->userData);
        }
    }
private:
    tagMFPlayer* m_p;
    volatile LONG m_ref;
};

static HRESULT InitMF() {
    static BOOL g_inited = FALSE;
    if (g_inited) return S_OK;
    HRESULT hr = MFStartup(MF_VERSION);
    if (SUCCEEDED(hr)) g_inited = TRUE;
    return hr;
}

MFPlayer* MFPlayer_Create(HWND hVideoWnd, MFPlayerEndCallback onEnd, void* userData) {
    InitMF();
    tagMFPlayer* p = (tagMFPlayer*)calloc(1, sizeof(tagMFPlayer));
    if (!p) return NULL;
    p->hVideoWnd = hVideoWnd;
    p->onEnd     = onEnd;
    p->userData  = userData;
    return (MFPlayer*)p;
}

void MFPlayer_Destroy(MFPlayer* player) {
    if (!player) return;
    tagMFPlayer* p = (tagMFPlayer*)player;
    MFPlayer_Stop(player);
    if (p->pVideoCtrl) { p->pVideoCtrl->Release(); p->pVideoCtrl = NULL; }
    if (p->pPlayer)    { p->pPlayer->Release(); p->pPlayer = NULL; }
    free(p);
}

HRESULT MFPlayer_Open(MFPlayer* player, const WCHAR* filePath) {
    if (!player || !filePath) return E_INVALIDARG;
    tagMFPlayer* p = (tagMFPlayer*)player;

    MFPlayer_Stop(player);
    if (p->pVideoCtrl) { p->pVideoCtrl->Release(); p->pVideoCtrl = NULL; }
    if (p->pPlayer)    { p->pPlayer->Release(); p->pPlayer = NULL; }

    InitMF();

    MFCallback* cb = new MFCallback(p);
    HRESULT hr = MFPCreateMediaPlayer(filePath, FALSE, MFP_OPTION_NONE, cb, p->hVideoWnd, &p->pPlayer);
    cb->Release();
    if (FAILED(hr)) return hr;

    hr = p->pPlayer->QueryInterface(IID_IMFVideoDisplayControl, (void**)&p->pVideoCtrl);
    if (SUCCEEDED(hr)) {
        p->pVideoCtrl->SetVideoWindow(p->hVideoWnd);
        p->pVideoCtrl->SetAspectRatioMode(MFVideoARMode_PreservePicture);
        RECT rc;
        GetClientRect(p->hVideoWnd, &rc);
        p->pVideoCtrl->SetVideoPosition(NULL, &rc);
    }

    InterlockedExchange(&p->isPlaying, FALSE);
    InterlockedExchange(&p->isPaused,  FALSE);
    return S_OK;
}

HRESULT MFPlayer_Play(MFPlayer* player) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pPlayer) return E_FAIL;
    HRESULT hr = p->pPlayer->Play();
    if (SUCCEEDED(hr)) {
        InterlockedExchange(&p->isPlaying, TRUE);
        InterlockedExchange(&p->isPaused,  FALSE);
    }
    return hr;
}

HRESULT MFPlayer_Pause(MFPlayer* player) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pPlayer) return E_FAIL;
    HRESULT hr = p->pPlayer->Pause();
    if (SUCCEEDED(hr)) InterlockedExchange(&p->isPaused, TRUE);
    return hr;
}

HRESULT MFPlayer_Stop(MFPlayer* player) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pPlayer) return S_FALSE;
    HRESULT hr = p->pPlayer->Stop();
    if (SUCCEEDED(hr)) {
        InterlockedExchange(&p->isPlaying, FALSE);
        InterlockedExchange(&p->isPaused,  FALSE);
    }
    return hr;
}

HRESULT MFPlayer_Seek(MFPlayer* player, double seconds) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pPlayer) return E_FAIL;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_I8;
    pv.hVal.QuadPart = (LONGLONG)(seconds * 10000000.0);
    HRESULT hr = p->pPlayer->SetPosition(MFP_POSITIONTYPE_100NS, &pv);
    if (FAILED(hr)) {
        // Try VT_UI8 variant
        PropVariantInit(&pv);
        pv.vt = VT_UI8;
        pv.uhVal.QuadPart = (ULONGLONG)(seconds * 10000000.0);
        hr = p->pPlayer->SetPosition(MFP_POSITIONTYPE_100NS, &pv);
    }
    PropVariantClear(&pv);
    return hr;
}

HRESULT MFPlayer_SetVolume(MFPlayer* player, float volume) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pPlayer) return E_FAIL;
    return p->pPlayer->SetVolume(volume);
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
    if (!p->pPlayer) return 0;
    // Try IMFMediaItem for reliable duration
    IMFPMediaItem* pItem = NULL;
    HRESULT hr = p->pPlayer->GetMediaItem(&pItem);
    if (SUCCEEDED(hr) && pItem) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        hr = pItem->GetDuration(MFP_POSITIONTYPE_100NS, &pv);
        pItem->Release();
        if (SUCCEEDED(hr) && pv.vt == VT_I8) {
            double dur = pv.hVal.QuadPart / 10000000.0;
            PropVariantClear(&pv);
            return dur;
        }
        PropVariantClear(&pv);
    }
    // Fallback
    PROPVARIANT pv;
    PropVariantInit(&pv);
    hr = p->pPlayer->GetDuration(MFP_POSITIONTYPE_100NS, &pv);
    if (SUCCEEDED(hr)) {
        LONGLONG val = 0;
        if (pv.vt == VT_I8)  val = pv.hVal.QuadPart;
        else if (pv.vt == VT_UI8) val = (LONGLONG)pv.uhVal.QuadPart;
        PropVariantClear(&pv);
        if (val > 0) return val / 10000000.0;
    }
    PropVariantClear(&pv);
    return 0;
}

double MFPlayer_GetPosition(MFPlayer* player) {
    if (!player) return 0;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pPlayer) return 0;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    HRESULT hr = p->pPlayer->GetPosition(MFP_POSITIONTYPE_100NS, &pv);
    if (SUCCEEDED(hr)) {
        LONGLONG val = 0;
        if (pv.vt == VT_I8)  val = pv.hVal.QuadPart;
        else if (pv.vt == VT_UI8) val = (LONGLONG)pv.uhVal.QuadPart;
        PropVariantClear(&pv);
        if (val > 0) return val / 10000000.0;
    }
    PropVariantClear(&pv);
    return 0;
}

HRESULT MFPlayer_GetCurrentVideoSize(MFPlayer* player, DWORD* width, DWORD* height) {
    if (!player) return E_FAIL;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pVideoCtrl) return E_FAIL;
    MFVideoNormalizedRect nr;
    RECT dstRect;
    HRESULT hr = p->pVideoCtrl->GetVideoPosition(&nr, &dstRect);
    if (SUCCEEDED(hr)) {
        if (width)  *width  = dstRect.right;
        if (height) *height = dstRect.bottom;
    }
    return hr;
}

void MFPlayer_UpdateVideoWindow(MFPlayer* player, RECT* rc) {
    if (!player) return;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pVideoCtrl) return;

    RECT wrc;
    if (rc) {
        wrc = *rc;
    } else if (p->hVideoWnd) {
        GetClientRect(p->hVideoWnd, &wrc);
    } else {
        return;
    }

    p->pVideoCtrl->SetVideoPosition(NULL, &wrc);
}
