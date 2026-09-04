#include <windows.h>
#include <stdio.h>
int wmain(void) {
    const wchar_t *names[] = { L"\\.\WinFsp", L"\\.\WinFsp.Disk", L"\\.\WinFsp.Net" };
    for (int i = 0; i < 3; i++) {
        HANDLE h = CreateFileW(names[i], GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            printf("%ls: FAILED err=0x%x (%lu)\n", names[i],
                   (unsigned)GetLastError(), (unsigned long)GetLastError());
        } else {
            printf("%ls: OK\n", names[i]);
            CloseHandle(h);
        }
    }
    return 0;
}
