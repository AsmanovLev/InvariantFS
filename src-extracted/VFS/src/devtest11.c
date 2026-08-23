#include <windows.h>
#include <stdio.h>
int wmain(void) {
    /* как DLL: GLOBALROOT + \Device\ + WinFsp.Disk+suffix + \VolumeParams= + (суррогаты) */
    WCHAR p[512];
    wcscpy_s(p, 512, L"\\\\?\\GLOBALROOT\\Device\\WinFsp.Disk+20260805T124445Z\\VolumeParams=");
    /* добавим пару суррогатных символов (приватная область) */
    size_t n = wcslen(p);
    p[n++] = (WCHAR)(0xF000 | 0x01);
    p[n++] = (WCHAR)(0xF000 | 0x02);
    p[n] = 0;

    HANDLE h = CreateFileW(p, 0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    printf("with VolumeParams: %s err=%lu\n",
           h != INVALID_HANDLE_VALUE ? "OK" : "FAILED", (unsigned long)GetLastError());
    if (h != INVALID_HANDLE_VALUE) {
        WCHAR vol[64] = {0}; DWORD ret = 0;
        BOOL b = DeviceIoControl(h, 0x90044, 0, 0, vol, sizeof vol, &ret, 0);
        printf("VOLUME_NAME ioctl: %d err=%lu\n", b, (unsigned long)GetLastError());
        CloseHandle(h);
    }

    /* без VolumeParams, просто device */
    HANDLE h2 = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\WinFsp.Disk+20260805T124445Z",
                            0, 7, 0, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
    printf("plain device: %s err=%lu\n",
           h2 != INVALID_HANDLE_VALUE ? "OK" : "FAILED", (unsigned long)GetLastError());
    if (h2 != INVALID_HANDLE_VALUE) CloseHandle(h2);
    return 0;
}
