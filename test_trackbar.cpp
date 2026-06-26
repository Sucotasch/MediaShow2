// Test: проверяет получает ли hMainWnd WM_HSCROLL от trackbar
// Если кнопка работает а trackbar нет — проблема в trackbar
// Если ничего не работает — проблема в window hierarchy

#include <windows.h>
#include <commctrl.h>

#define IDC_TEST_BTN  9001
#define IDC_TEST_TB   9002

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindow(L"BUTTON", L"CLICK ME", WS_CHILD | WS_VISIBLE,
            10, 10, 100, 30, hWnd, (HMENU)IDC_TEST_BTN, NULL, NULL);
        CreateWindow(TRACKBAR_CLASS, L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            10, 50, 300, 30, hWnd, (HMENU)IDC_TEST_TB, NULL, NULL);
        SendMessage(GetDlgItem(hWnd, IDC_TEST_TB), TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_TEST_BTN)
            MessageBox(hWnd, L"Button works!", L"Test", MB_OK);
        return 0;
    case WM_HSCROLL: {
        int pos = (int)SendMessage(GetDlgItem(hWnd, IDC_TEST_TB), TBM_GETPOS, 0, 0);
        TCHAR buf[64];
        wsprintf(buf, L"Seekbar: %d", pos);
        SetWindowText(hWnd, buf);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    InitCommonControlsEx(&(INITCOMMONCONTROLSEX{sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES}));
    WNDCLASSEX wc = {sizeof(wc), CS_HREDRAW|CS_VREDRAW, WndProc, 0, 0, hInst, NULL, NULL, (HBRUSH)(COLOR_WINDOW+1), NULL, L"TestWnd", NULL};
    RegisterClassEx(&wc);
    HWND hWnd = CreateWindow(L"TestWnd", L"Trackbar Test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 200, NULL, NULL, hInst, NULL);
    ShowWindow(hWnd, nShow);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
