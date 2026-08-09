#ifndef PLUGIN_API_H
#define PLUGIN_API_H

#include "../sdk/listplug.h"

#define APP_NAME    L"MediaShow2"
#define APP_VERSION L"1.0.0"

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
#define IDM_ABOUT             4028
#define IDM_VOL_UP            4040
#define IDM_VOL_DOWN          4041
#define IDM_SEEK_FWD          4042
#define IDM_SEEK_BACK         4043
#define IDM_SHOWPLAYLIST      4044
#define IDM_FULLSCREEN        4051
#define IDM_REPEAT            4052
#define IDM_APPENDMODE        4053
#define IDM_CLEARPLAYLIST     4054

#define IDT_COOLDOWN          2
#define IDT_RECREATE          3

#endif /* PLUGIN_API_H */
