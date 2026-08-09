#include <windows.h>
#include <stdio.h>

int main() {
    OutputDebugString(TEXT("TEST: OutputDebugString works!\n"));
    OutputDebugString(TEXT("TEST: Second message\n"));
    
    for (int i = 0; i < 5; i++) {
        TCHAR buf[64];
        swprintf(buf, 64, L"TEST: Message %d\n", i);
        OutputDebugString(buf);
    }
    
    printf("Debug messages sent. Check DebugView.\n");
    return 0;
}
