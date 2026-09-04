#include <windows.h>
#include <stdio.h>
static void open_it(const wchar_t *name) {
    HANDLE h = CreateFileW(name, 0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    wprintf(L"  %s -> %s (err=%lu)\n", name,
            h == INVALID_HANDLE_VALUE ? L"FAILED" : L"OK", (unsigned long)GetLastError());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}
int wmain(void) {
    open_it(L"\??\GLOBALROOT\Device\WinFsp.Disk");
    open_it(L"\??\GLOBALROOT\Device\WinFsp.Disk+20260805T124445Z");
    open_it(L"\\.\WinFsp.Disk+20260805T124445Z");
    open_it(L"\\.\WinFsp.Disk+sxs.20260805T124445Z");
    return 0;
}
