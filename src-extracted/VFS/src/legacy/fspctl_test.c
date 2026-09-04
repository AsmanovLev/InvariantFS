#include <windows.h>
#include <stdio.h>
#include "winfsp/winfsp.h"
int wmain(void) {
    FSP_FSCTL_VOLUME_PARAMS vp;
    WCHAR name[64];
    HANDLE h = INVALID_HANDLE_VALUE;
    memset(&vp, 0, sizeof vp);
    vp.Version = (UINT16)sizeof vp;
    vp.SectorSize = 512;
    vp.SectorsPerAllocationUnit = 8;
    NTSTATUS st = FspFsctlCreateVolume(FSP_FSCTL_DISK_DEVICE_NAME, &vp,
                                       name, sizeof name, &h);
    wprintf(L"FspFsctlCreateVolume: 0x%lx handle=%p name=%s\n",
            (ULONG)st, (void *)h, h != INVALID_HANDLE_VALUE ? name : L"-");
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return 0;
}
