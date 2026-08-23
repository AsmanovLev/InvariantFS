#include <windows.h>
#include <stdio.h>
int wmain(void) {
    const wchar_t *names[] = {
        L"\\?\GLOBALROOT\Device\WinFsp",
        L"\\.\GLOBALROOT\Device\WinFsp",
        L"\\?\GLOBALROOT\DosDevices\WinFsp",
        L"\\.\WinFsp"
    };
    for (int i = 0; i < 4; i++) {
        HANDLE h = CreateFileW(names[i], GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        printf("%ls: %s (err=%lu)\n", names[i],
               h == INVALID_HANDLE_VALUE ? "FAILED" : "OK", (unsigned long)GetLastError());
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    return 0;
}
