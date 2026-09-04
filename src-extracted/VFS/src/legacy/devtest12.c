#include <windows.h>
#include <stdio.h>
int wmain(void) {
    /* без суррогатов, просто \VolumeParams= */
    HANDLE h1 = CreateFileW(
        L"\\\\?\\GLOBALROOT\\Device\\WinFsp.Disk+20260805T124445Z\\VolumeParams=",
        0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    printf("with = only: %s err=%lu\n", h1 != INVALID_HANDLE_VALUE ? "OK" : "FAILED",
           (unsigned long)GetLastError());
    if (h1 != INVALID_HANDLE_VALUE) CloseHandle(h1);

    /* без суффикса, с \VolumeParams= */
    HANDLE h2 = CreateFileW(
        L"\\\\?\\GLOBALROOT\\Device\\WinFsp.Disk\\VolumeParams=",
        0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    printf("no-suffix with =: %s err=%lu\n", h2 != INVALID_HANDLE_VALUE ? "OK" : "FAILED",
           (unsigned long)GetLastError());
    if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
    return 0;
}
