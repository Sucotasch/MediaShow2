#ifndef DS_PLAYER_H
#define DS_PLAYER_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tagDSPlayer DSPlayer;

typedef void (*DSEndCallback)(void* userData);

DSPlayer* DSPlayer_Create(HWND hVideoWnd, DSEndCallback onEnd, void* userData);
void DSPlayer_Destroy(DSPlayer* player);

HRESULT DSPlayer_Open(DSPlayer* player, const WCHAR* filePath);
HRESULT DSPlayer_Play(DSPlayer* player);
HRESULT DSPlayer_Pause(DSPlayer* player);
HRESULT DSPlayer_Stop(DSPlayer* player);
HRESULT DSPlayer_Seek(DSPlayer* player, double seconds);
HRESULT DSPlayer_SetVolume(DSPlayer* player, long volume);

BOOL    DSPlayer_IsPlaying(DSPlayer* player);
BOOL    DSPlayer_IsPaused(DSPlayer* player);
double  DSPlayer_GetDuration(DSPlayer* player);
double  DSPlayer_GetPosition(DSPlayer* player);

void    DSPlayer_UpdateVideoWindow(DSPlayer* player, RECT* rc);

#ifdef __cplusplus
}
#endif

#endif /* DS_PLAYER_H */
