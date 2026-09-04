#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#pragma comment(lib, "ntdll.lib")
int wmain(void) {
    UNICODE_STRING name; RTL_CONSTANT_STRING  /* cannot use constant */
    OBJECT_ATTRIBUTES oa;
    HANDLE hDir = NULL;
    /* open \GLOBALROOT\DosDevices */
    WCHAR path[] = L"\GLOBALROOT\DosDevices";
    UNICODE_STRING objName;
    RtlInitUnicodeString(&objName, path);
    InitializeObjectAttributes(&oa, &objName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    NTSTATUS st = NtOpenDirectoryObject(&hDir, DIRECTORY_QUERY, &oa);
    if (!NT_SUCCESS(st)) { printf("NtOpenDirectoryObject: 0x%lx\n", st); return 1; }
    /* enumerate */
    WCHAR buf[32768];
    ULONG ctx = 0;
    for (;;) {
        ULONG ret = 0;
        st = NtQueryDirectoryObject(hDir, buf, sizeof(buf), TRUE, FALSE, &ctx, &ret);
        if (!NT_SUCCESS(st) || ret == 0) break;
        const WCHAR *p = buf;
        while (ret-- > 0) {
            WCHAR n[256];
            swprintf(n, 256, L"%s", p);
            if (wcsstr(n, L"WinFsp"))
                wprintf(L"  %s\n", n);
            p += wcslen(p) + 1;
            while (*p == 0) p++;  /* skip attributes/name2 */
            while (*p) p++;
            p++;
        }
        break;  /* single buffer pass */
    }
    NtClose(hDir);
    printf("done\n");
    return 0;
}
