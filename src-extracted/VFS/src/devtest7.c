#include <windows.h>
#include <stdio.h>
int wmain(void) {
    HMODULE m = GetModuleHandleW(L"winfsp-x64.dll");
    if (m) {
        WCHAR p[MAX_PATH];
        GetModuleFileNameW(m, p, MAX_PATH);
        wprintf(L"DLL: %s\n", p);
        /* проверить наличие .sxs рядом */
        wcscpy_s(p + wcslen(p) - 4, 8, L".sxs");
        HANDLE f = CreateFileW(p, FILE_READ_DATA, 7, 0, OPEN_EXISTING, 0, 0);
        if (f != INVALID_HANDLE_VALUE) {
            char b[64] = {0}; DWORD n = 0;
            ReadFile(f, b, 63, &n, 0);
            CloseHandle(f);
            printf("SXS file content: '%s'\n", b);
        } else printf("no .sxs file\n");
    } else printf("winfsp-x64.dll NOT loaded\n");

    const wchar_t *names[] = {
        L"\\?\GLOBALROOT\Device\WinFsp.Disk",
        L"\\?\GLOBALROOT\Device\WinFsp.Disk+20260805T124445Z",
        L"\\?\GLOBALROOT\Device\WinFsp.Disk+sxs.20260805T124445Z"
    };
    for (int i = 0; i < 3; i++) {
        HANDLE h = CreateFileW(names[i], 0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
        printf("  %ls: %s (err=%lu)\n", names[i],
               h == INVALID_HANDLE_VALUE ? "FAILED" : "OK", (unsigned long)GetLastError());
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    return 0;
}
