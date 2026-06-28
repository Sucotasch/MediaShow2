#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include "../sdk/listplug.h"

#define APP_NAME    L"MediaShow2"
#define APP_VERSION L"1.0.0"

#define WM_PLAYER_PLAY        (WM_USER + 100)
#define WM_PLAYER_PAUSE       (WM_USER + 101)
#define WM_PLAYER_STOP        (WM_USER + 102)
#define WM_PLAYER_FULLSCREEN  (WM_USER + 104)
#define WM_PLAYER_TOPMOST     (WM_USER + 105)
#define WM_PLAYER_MUTE        (WM_USER + 107)
#define WM_PLAYER_INFO        (WM_USER + 108)
#define WM_PLAYER_PREV        (WM_USER + 109)
#define WM_PLAYER_NEXT        (WM_USER + 110)
#define WM_PLAYER_TRACK_END   (WM_USER + 200)

#define IDC_MAIN              100
#define IDC_VIDEO             101
#define IDC_TOOLBAR           102
#define IDC_SEEKBAR           103
#define IDC_VOLSLIDER         104
#define IDC_SToolBar          105
#define IDC_PLAYLIST          106

#define IDM_PLAY              4000
#define IDM_STOP              4002
#define IDM_PREV              4003
#define IDM_NEXT              4004
#define IDM_MUTE              4007
#define IDM_FILEINFO          4008
#define IDM_SETTINGS          4022
#define IDM_ABOUT             4028
#define IDM_PLAYLIST          4021
#define IDM_VOL_UP            4040
#define IDM_VOL_DOWN          4041
#define IDM_SEEK_FWD          4042
#define IDM_SEEK_BACK         4043
#define IDM_SHOWPLAYLIST      4044
#define IDM_FULLSCREEN        4051
#define IDM_REPEAT            4052

#endif /* PLUGIN_API_H */
