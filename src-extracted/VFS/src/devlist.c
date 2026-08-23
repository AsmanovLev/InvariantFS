/*
 * devlist.c — enumerate \Device and \DosDevices object namespace
 */
#include <windows.h>
#include <winternl.h>
#ifndef DIRECTORY_QUERY
#define DIRECTORY_QUERY 0x0001
#endif
#include <stdio.h>
#pragma comment(lib, "ntdll.lib")

static void enumerate_dir(const WCHAR *path)
{
    UNICODE_STRING objName;
    OBJECT_ATTRIBUTES oa;
    HANDLE hDir = NULL;
    NTSTATUS st;
    WCHAR buf[65536];
    ULONG ctx = 0;

    RtlInitUnicodeString(&objName, path);
    InitializeObjectAttributes(&oa, &objName, OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = NtOpenDirectoryObject(&hDir, DIRECTORY_QUERY, &oa);
    if (!NT_SUCCESS(st)) {
        printf("  %ls: NtOpenDirectoryObject 0x%lx\n", path, (ULONG)st);
        return;
    }
    for (;;) {
        ULONG ret = 0;
        st = NtQueryDirectoryObject(hDir, buf, sizeof(buf), TRUE, FALSE, &ctx, &ret);
        if (!NT_SUCCESS(st) || ret == 0) break;
        ULONG_PTR p = (ULONG_PTR)buf;
        for (ULONG i = 0; i < ret; i++) {
            WCHAR *name = (WCHAR *)p;
            if (wcsstr(name, L"WinFsp"))
                wprintf(L"  %ls\\%s\n", path, name);
            p += (wcslen(name) + 1) * sizeof(WCHAR);
            while (*(WCHAR *)p == 0) p += sizeof(WCHAR);
        }
    }
    NtClose(hDir);
}

int wmain(void)
{
    printf("Device objects with 'WinFsp':\n");
    enumerate_dir(L"\\Device");
    printf("DosDevices with 'WinFsp':\n");
    enumerate_dir(L"\\DosDevices");
    printf("done\n");
    return 0;
}
