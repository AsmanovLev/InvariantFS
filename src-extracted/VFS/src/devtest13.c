#include <windows.h>
#include <stdio.h>
int wmain(void) {
    WCHAR p[1200];
    wcscpy_s(p, 1200, L"\\\\?\\GLOBALROOT\\Device\\WinFsp.Disk+20260805T124445Z\\VolumeParams=");
    size_t n = wcslen(p);
    /* как DLL: 504 байта VolumeParams -> 504 суррогата */
    for (int i = 0; i < 504; i++)
        p[n++] = (WCHAR)(0xF000 | (i & 0xFF));
    p[n] = 0;
    wprintf(L"path len: %zu\n", n);

    HANDLE h = CreateFileW(p, 0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    printf("full path: %s err=%lu\n", h != INVALID_HANDLE_VALUE ? "OK" : "FAILED",
           (unsigned long)GetLastError());
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    /* короткий, но суррогаты */
    WCHAR p2[200];
    wcscpy_s(p2, 200, L"\\\\?\\GLOBALROOT\\Device\\WinFsp.Disk+20260805T124445Z\\VolumeParams=");
    size_t n2 = wcslen(p2);
    p2[n2++] = 0xF001; p2[n2] = 0;
    HANDLE h2 = CreateFileW(p2, 0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    printf("short path: %s err=%lu\n", h2 != INVALID_HANDLE_VALUE ? "OK" : "FAILED",
           (unsigned long)GetLastError());
    if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
    return 0;
}
