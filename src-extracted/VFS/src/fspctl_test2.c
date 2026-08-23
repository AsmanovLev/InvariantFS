#include <windows.h>
#include <stdio.h>
#include "winfsp/winfsp.h"
int wmain(void) {
    NTSTATUS st = FspFsctlStartService();
    wprintf(L"FspFsctlStartService: 0x%lx\n", (ULONG)st);
    return 0;
}
