#include <windows.h>
#include <stdio.h>
int wmain(void) {
    const wchar_t *p = L"\\\\?\\GLOBALROOT\\Device\\WinFsp.Disk+20260805T124445Z";
    /* hex dump path */
    for (int i = 0; p[i]; i++)
        printf("%04x ", p[i]);
    printf("\n");
    HANDLE h = CreateFileW(p, 0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    printf("CreateFile: %s err=%lu\n", h != INVALID_HANDLE_VALUE ? "OK" : "FAILED",
           (unsigned long)GetLastError());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    /* и без суффикса */
    const wchar_t *p2 = L"\\\\?\\GLOBALROOT\\Device\\WinFsp.Disk";
    h = CreateFileW(p2, 0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    printf("no-suffix: %s err=%lu\n", h != INVALID_HANDLE_VALUE ? "OK" : "FAILED",
           (unsigned long)GetLastError());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return 0;
}
