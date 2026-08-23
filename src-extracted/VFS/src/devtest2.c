#include <windows.h>
#include <stdio.h>
int wmain(void) {
    const wchar_t *names[] = {
        L"\\.\WinFsp", L"\\.\WinFsp+20260805T111132Z",
        L"\\.\WinFsp.Disk", L"\\.\WinFsp+20260805T111132Z.Disk",
        L"\\.\WinFsp.Net", L"\\.\WinFsp+20260805T111132Z.Net"
    };
    for (int i = 0; i < 6; i++) {
        HANDLE h = CreateFileW(names[i], GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        printf("%ls: %s (err=%lu)\n", names[i],
               h == INVALID_HANDLE_VALUE ? "FAILED" : "OK", (unsigned long)GetLastError());
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    return 0;
}
