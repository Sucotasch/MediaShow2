#ifndef MF_PLAYER_H
#define MF_PLAYER_H

#include <windows.h>

typedef struct tagMFPlayer MFPlayer;
typedef void (*MFPlayerEndCallback)(void* userData);

MFPlayer* MFPlayer_Create(HWND hVideoWnd, MFPlayerEndCallback onEnd, void* userData);
void MFPlayer_Destroy(MFPlayer* player);

HRESULT MFPlayer_Open(MFPlayer* player, const WCHAR* filePath);
HRESULT MFPlayer_Play(MFPlayer* player);
HRESULT MFPlayer_Pause(MFPlayer* player);
HRESULT MFPlayer_Stop(MFPlayer* player);
HRESULT MFPlayer_Seek(MFPlayer* player, double seconds);
HRESULT MFPlayer_SetVolume(MFPlayer* player, float volume);

BOOL    MFPlayer_IsPlaying(MFPlayer* player);
BOOL    MFPlayer_IsPaused(MFPlayer* player);
double  MFPlayer_GetDuration(MFPlayer* player);
double  MFPlayer_GetPosition(MFPlayer* player);
HRESULT MFPlayer_GetCurrentVideoSize(MFPlayer* player, DWORD* width, DWORD* height);

void    MFPlayer_UpdateVideoWindow(MFPlayer* player, RECT* rc);

#endif /* MF_PLAYER_H */
