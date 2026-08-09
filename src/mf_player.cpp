#include "mf_player.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <evr.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

struct tagMFPlayer {
    IMFPMediaPlayer*        pPlayer;
    IMFVideoDisplayControl*  pVideoCtrl;
    HWND                    hVideoWnd;
    MFPlayerEndCallback     onEnd;
    void*                   userData;
    volatile LONG           isPlaying;
    volatile LONG           isPaused;
    volatile LONG           refCount;    // 1 = API owner; +1 while callback is alive
    volatile LONG           destroying;  // set by MFPlayer_Destroy; suppresses onEnd
};

// Release one ownership reference; the struct is freed when the last reference
// (API owner OR live callback) goes away. This keeps the object valid for an
// in-flight MFP callback that may run after MFPlayer_Destroy returned.
static void MF_ReleaseRef(tagMFPlayer* p) {
    if (!p) return;
    if (InterlockedDecrement(&p->refCount) == 0) free(p);
}

class MFCallback : public IMFPMediaPlayerCallback {
public:
    MFCallback(tagMFPlayer* p) : m_p(p), m_ref(1) {
        if (m_p) InterlockedIncrement(&m_p->refCount); // keep player alive while we exist
    }
    ~MFCallback() {
        if (m_p) MF_ReleaseRef(m_p); // our time is up — drop the callback reference
    }
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
        if (!pEventHeader || !m_p) return;
        if (pEventHeader->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
            InterlockedExchange(&m_p->isPlaying, FALSE);
            InterlockedExchange(&m_p->isPaused,  FALSE);
            // If MFPlayer_Destroy has started, userData (PluginState) may be
            // torn down — suppress the callback instead of touching it.
            if (!InterlockedCompareExchange(&m_p->destroying, 0, 0))
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
    p->hVideoWnd  = hVideoWnd;
    p->onEnd      = onEnd;
    p->userData   = userData;
    p->refCount   = 1;         // API owner reference
    p->destroying = FALSE;
    return (MFPlayer*)p;
}

void MFPlayer_Destroy(MFPlayer* player) {
    if (!player) return;
    tagMFPlayer* p = (tagMFPlayer*)player;
    // Suppress onEnd FIRST — userData (PluginState) is torn down right after this.
    InterlockedExchange(&p->destroying, TRUE);
    MFPlayer_Stop(player);
    if (p->pVideoCtrl) { p->pVideoCtrl->Release(); p->pVideoCtrl = NULL; }
    if (p->pPlayer)    { p->pPlayer->Release(); p->pPlayer = NULL; }
    MF_ReleaseRef(p);   // drop the API owner reference
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
    // Retry once on failure (MF pipeline may not be fully ready on first call)
    if (FAILED(hr)) {
        hr = MFPCreateMediaPlayer(filePath, FALSE, MFP_OPTION_NONE, cb, p->hVideoWnd, &p->pPlayer);
    }
    cb->Release();
    {
        WCHAR dbg[256];
        swprintf(dbg, 256, L"MFOpen: MFP hr=0x%08X pP=%d file='%s'\n", hr, (p->pPlayer != NULL), filePath);
        OutputDebugStringW(dbg);
    }
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
    if (!p->pPlayer) { OutputDebugStringW(L"MFPlay: pPlayer=NULL!\n"); return E_FAIL; }
    HRESULT hr = p->pPlayer->Play();
    {
        WCHAR dbg[128];
        swprintf(dbg, 128, L"MFPlay: hr=0x%08X\n", hr);
        OutputDebugStringW(dbg);
    }
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
    // Wait for async MFP teardown only if the player was actually rendering.
    // A second Stop (e.g. MFPlayer_Destroy right after an earlier Stop in
    // ListLoadNextW/PlayIndex) has no in-flight async work — sleeping again
    // just adds ~50 ms to every file switch.
    BOOL wasPlaying = InterlockedCompareExchange(&p->isPlaying, 0, 0);
    if (wasPlaying) Sleep(50);
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
        return val / 10000000.0;
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
        return val / 10000000.0;
    }
    PropVariantClear(&pv);
    return 0;
}

double MFPlayer_GetAspectRatio(MFPlayer* player) {
    if (!player) return 0;
    tagMFPlayer* p = (tagMFPlayer*)player;
    if (!p->pVideoCtrl) return 0;
    SIZE nativeSize = {0};
    if (SUCCEEDED(p->pVideoCtrl->GetNativeVideoSize(&nativeSize, NULL)) &&
        nativeSize.cx > 0 && nativeSize.cy > 0) {
        return (double)nativeSize.cx / (double)nativeSize.cy;
    }
    return 0;
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

BOOL MFPlayer_AudioNeedsDS(const WCHAR* filePath) {
    if (!filePath || !filePath[0]) return FALSE;

    InitMF();

    IMFSourceReader* reader = NULL;
    HRESULT hr = MFCreateSourceReaderFromURL(filePath, NULL, &reader);
    if (FAILED(hr) || !reader) return FALSE;

    IMFMediaType* audioType = NULL;
    hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &audioType);
    if (FAILED(hr) || !audioType) {
        reader->Release();
        return FALSE; // No audio stream → video-only file, MF handles fine
    }

    GUID subtype = GUID_NULL;
    audioType->GetGUID(MF_MT_SUBTYPE, &subtype);
    audioType->Release();
    reader->Release();

    // Audio codecs that MF can READ from container but CANNOT decode
    // (no MFT decoder registered). Check by Data1 (WAVE format tag in GUID).
    // Opus:  Data1 = 0x4F707573 (ASCII "Opus")
    // Vorbis: Data1 = 0x564F5242 (ASCII "VORB")
    // AC-3:  Data1 = 0xE923AABE
    // E-AC-3: Data1 = 0x00000AAC (wFormatTag for Dolby Digital Plus)
    // DTS:   Data1 = 0x0009
    // Note: FLAC (0xF1AC) is NOT here — MF supports FLAC since Win 10 1709
    return (subtype.Data1 == 0x4F707573 ||  // Opus
            subtype.Data1 == 0x564F5242 ||  // Vorbis
            subtype.Data1 == 0xE923AABE ||  // AC-3
            subtype.Data1 == 0x00000AAC ||  // E-AC-3 (Dolby Digital Plus)
            subtype.Data1 == 0x00000009);   // DTS
}
