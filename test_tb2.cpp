#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

#define IDC_TB 1001
#define IDC_BTN 1002

static int g_scrollCount = 0;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
        InitCommonControlsEx(&icc);

        HWND hTb = CreateWindow(TRACKBAR_CLASS, L"", 
            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            10, 10, 300, 30, hWnd, (HMENU)IDC_TB, NULL, NULL);
        SendMessage(hTb, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessage(hTb, TBM_SETPOS, TRUE, 50);

        CreateWindow(L"BUTTON", L"TEST", WS_CHILD | WS_VISIBLE,
            10, 50, 100, 30, hWnd, (HMENU)IDC_BTN, NULL, NULL);
        return 0;
    }
    case WM_HSCROLL: {
        HWND hCtrl = (HWND)lParam;
        g_scrollCount++;
        TCHAR buf[128];
        wsprintf(buf, L"WM_HSCROLL from=%p seekbar=%p vol=%p count=%d pos=%d",
            hCtrl, GetDlgItem(hWnd, IDC_TB), NULL, g_scrollCount,
            (int)SendMessage(GetDlgItem(hWnd, IDC_TB), TBM_GETPOS, 0, 0));
        SetWindowText(hWnd, buf);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN) {
            MessageBox(hWnd, L"Button works!", L"Test", MB_OK);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEX wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"TestTB";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassEx(&wc);

    HWND hWnd = CreateWindow(L"TestTB", L"Trackbar Test",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        400, 150, NULL, NULL, hInst, NULL);
    ShowWindow(hWnd, nShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
