/* minifs.c — минимальная WinFsp FS для диагностики: путь DLL + Create */
#include <windows.h>
#include <stdio.h>
#include "winfsp/winfsp.h"

static NTSTATUS MinGetVolumeInfo(FSP_FILE_SYSTEM *fs, FSP_FSCTL_VOLUME_INFO *vi)
{
    (void)fs;
    memset(vi, 0, sizeof *vi);
    vi->TotalSize = 1024 * 1024;
    vi->FreeSize = 1024 * 1024;
    vi->VolumeLabelLength = 8;
    wcscpy_s(vi->VolumeLabel, 32, L"MINIFS");
    return STATUS_SUCCESS;
}
static NTSTATUS MinGetSecurityByName(FSP_FILE_SYSTEM *fs, PCWSTR name,
    PUINT32 attrs, PSECURITY_DESCRIPTOR *sd, PULONG sdlen)
{
    (void)fs; (void)name; (void)sd; (void)sdlen;
    if (attrs) *attrs = FILE_ATTRIBUTE_DIRECTORY;
    return name[0] ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_SUCCESS;
}

int wmain(int argc, wchar_t **argv)
{
    FSP_FILE_SYSTEM_INTERFACE ifc;
    FSP_FSCTL_VOLUME_PARAMS vp;
    FSP_FILE_SYSTEM *fs = NULL;
    NTSTATUS st;
    WCHAR dlldir[MAX_PATH];

    /* где лежит DLL */
    HMODULE m = GetModuleHandleW(L"winfsp-x64.dll");
    if (m) {
        GetModuleFileNameW(m, dlldir, MAX_PATH);
        wprintf(L"DLL: %s\n", dlldir);
    } else {
        wprintf(L"DLL not yet loaded\n");
    }

    memset(&ifc, 0, sizeof ifc);
    ifc.GetVolumeInfo = MinGetVolumeInfo;
    ifc.GetSecurityByName = MinGetSecurityByName;

    memset(&vp, 0, sizeof vp);
    vp.Version = (UINT16)sizeof vp;
    vp.SectorSize = 512;
    vp.SectorsPerAllocationUnit = 8;
    wcscpy_s(vp.FileSystemName, sizeof vp.FileSystemName / sizeof(WCHAR), L"MINIFS");

    st = FspFileSystemCreate(FSP_FSCTL_DISK_DEVICE_NAME, &vp, &ifc, &fs);
    wprintf(L"FspFileSystemCreate: 0x%lx\n", (ULONG)st);

    if (NT_SUCCESS(st)) {
        st = FspFileSystemSetMountPoint(fs, L"Y:");
        wprintf(L"SetMountPoint Y:: 0x%lx\n", (ULONG)st);
        FspFileSystemDelete(fs);
    }
    return 0;
}
