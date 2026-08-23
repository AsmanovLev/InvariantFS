#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#pragma comment(lib, "ntdll.lib")
static void nt_open(const wchar_t *path) {
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    RtlInitUnicodeString(&name, path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    NTSTATUS st = NtCreateFile(&h, FILE_READ_ATTRIBUTES, &oa, &iosb, 0,
                               FILE_ATTRIBUTE_NORMAL, 7, FILE_OPEN,
                               FILE_NON_DIRECTORY_FILE, 0, 0);
    wprintf(L"  %s -> 0x%lx\n", path, (ULONG)st);
    if (NT_SUCCESS(st) && h) NtClose(h);
}
int wmain(void) {
    nt_open(L"\Device\WinFsp.Disk");
    nt_open(L"\Device\WinFsp.Disk+20260805T124445Z");
    nt_open(L"\Device\WinFsp.Disk+sxs.20260805T124445Z");
    return 0;
}
