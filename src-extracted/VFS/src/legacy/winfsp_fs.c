/*
 * winfsp_fs.c — InvariantFS WinFsp filesystem (read-only mount)
 *
 *   invf-mount <image> [mount-point]
 *
 *   mount-point: "X:" or empty for auto-assign
 *
 * Implements the minimal read-only operation set:
 *   GetVolumeInfo, GetSecurityByName, Open, Close,
 *   GetFileInfo, Read, ReadDirectory
 *
 * Data path: read -> AST -> L2P -> segment decode (LZ4/ZSTD/raw) -> user buffer
 */
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "winfsp/winfsp.h"
#include "invarifs.h"
#include "volume.h"

/* ---- file table: snapshot of inode area at mount time ---- */
typedef struct {
    wchar_t name[256];
    uint64_t inode_id;
    uint64_t size;
    uint64_t ctime;
} fs_entry;

static invfs_volume *g_vol;
static fs_entry *g_entries;
static int g_nentries;
static int g_cap;

static void utf8_to_utf16(const char *utf8, wchar_t *utf16, int max)
{
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, utf16, max);
}

static void utf16_to_utf8(const wchar_t *utf16, char *utf8, int max)
{
    WideCharToMultiByte(CP_UTF8, 0, utf16, -1, utf8, max, NULL, NULL);
}

static fs_entry *find_entry(const wchar_t *name)
{
    int i;
    for (i = 0; i < g_nentries; i++) {
        if (_wcsicmp(g_entries[i].name, name) == 0)
            return &g_entries[i];
    }
    return NULL;
}

static void build_file_table(void)
{
    const invfs_superblock *sb = vol_sb(g_vol);
    uint64_t bm = (sb->total_blocks / 8 + INVFS_BLOCK_SIZE - 1) / INVFS_BLOCK_SIZE;
    uint64_t p = (sb->metadata_zone_start + bm + INVFS_JOURNAL_BLOCKS) * INVFS_BLOCK_SIZE;
    uint64_t end = (sb->metadata_zone_start + sb->metadata_zone_blocks) * INVFS_BLOCK_SIZE;

    g_entries = NULL; g_nentries = 0; g_cap = 0;
    while (p + 8 <= end) {
        uint32_t magic, rec_len;
        uint64_t inode_id, file_size, ctime;
        uint32_t name_len;
        char name[257];

        if (vol_read_raw(g_vol, p, &magic, 4) != 0) break;
        if (magic != 0x444F4E49u) break;
        if (vol_read_raw(g_vol, p + 4, &rec_len, 4) != 0) break;
        if (vol_read_raw(g_vol, p + 8, &inode_id, 8) != 0) break;
        if (vol_read_raw(g_vol, p + 16, &file_size, 8) != 0) break;
        if (vol_read_raw(g_vol, p + 24, &ctime, 8) != 0) break;
        if (vol_read_raw(g_vol, p + 32, &name_len, 4) != 0) break;
        if (name_len > 256) break;
        if (vol_read_raw(g_vol, p + 36, name, name_len) != 0) break;
        name[name_len] = 0;

        if (g_nentries == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 16;
            g_entries = (fs_entry *)realloc(g_entries, g_cap * sizeof(fs_entry));
            if (!g_entries) return;
        }
        memset(&g_entries[g_nentries], 0, sizeof(fs_entry));
        utf8_to_utf16(name, g_entries[g_nentries].name, 256);
        g_entries[g_nentries].inode_id = inode_id;
        g_entries[g_nentries].size = file_size;
        g_entries[g_nentries].ctime = ctime;
        g_nentries++;
        p += (uint64_t)rec_len + 4;
    }
}

/* ---- helpers ---- */
static void fill_file_info(fs_entry *e, FSP_FSCTL_FILE_INFO *fi)
{
    ULARGE_INTEGER t;
    memset(fi, 0, sizeof(*fi));
    t.QuadPart = (ULONGLONG)e->ctime * 10000000ull + 116444736000000000ull;  /* unix->windows */
    fi->CreationTime = fi->LastAccessTime = fi->LastWriteTime = fi->ChangeTime = t.QuadPart;
    fi->FileSize = e->size;
    fi->AllocationSize = (e->size + 4095) & ~(uint64_t)4095;
    fi->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    fi->IndexNumber = e->inode_id;
}

static NTSTATUS fill_root_info(FSP_FSCTL_FILE_INFO *fi)
{
    memset(fi, 0, sizeof(*fi));
    fi->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    return STATUS_SUCCESS;
}

/* ---- WinFsp operations ---- */
static NTSTATUS InvFspGetVolumeInfo(FSP_FILE_SYSTEM *FileSystem,
                                    FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    const invfs_superblock *sb = vol_sb(g_vol);
    uint64_t total = sb->total_blocks * (uint64_t)INVFS_BLOCK_SIZE;
    /* free blocks: recount from bitmap via a simple scan is expensive;
     * use "allocated" estimate from L2P? For now: total minus a rough
     * metadata reservation; exact free recomputed on flush is overkill here. */
    uint64_t free_blocks = sb->total_blocks - (sb->metadata_zone_blocks + 1);
    memset(VolumeInfo, 0, sizeof(*VolumeInfo));
    VolumeInfo->TotalSize = total;
    VolumeInfo->FreeSize = free_blocks * (uint64_t)INVFS_BLOCK_SIZE;
    VolumeInfo->VolumeLabelLength = (UINT16)(wcslen(L"InvariantFS") * sizeof(wchar_t));
    wcscpy_s(VolumeInfo->VolumeLabel, 32, L"InvariantFS");
    return STATUS_SUCCESS;
}

static NTSTATUS InvFspGetSecurityByName(FSP_FILE_SYSTEM *FileSystem, PCWSTR FileName,
                                        PUINT32 PFileAttributes,
                                        PSECURITY_DESCRIPTOR *PSecurityDescriptor,
                                        PULONG PSecurityDescriptorSize)
{
    fs_entry *e;
    if (PSecurityDescriptor)
        *PSecurityDescriptor = NULL;
    if (PSecurityDescriptorSize)
        *PSecurityDescriptorSize = 0;

    if (FileName[0] == L'\0') {
        if (PFileAttributes)
            *PFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        return STATUS_SUCCESS;
    }
    e = find_entry(FileName);
    if (!e)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (PFileAttributes)
        *PFileAttributes = FILE_ATTRIBUTE_ARCHIVE;
    return STATUS_SUCCESS;
}

static NTSTATUS InvFspOpen(FSP_FILE_SYSTEM *FileSystem, PCWSTR FileName,
                           UINT64 CreateOptions, UINT32 GrantedAccess,
                           UINT32 FileAttributes, PSECURITY_DESCRIPTOR SecurityDescriptor,
                           UINT64 AllocationSize, PVOID *PFileContext,
                           FSP_FSCTL_FILE_INFO *FileInfo)
{
    fs_entry *e;
    (void)CreateOptions; (void)GrantedAccess; (void)FileAttributes;
    (void)SecurityDescriptor; (void)AllocationSize;

    if (FileName[0] == L'\0') {
        *PFileContext = (PVOID)(uintptr_t)0;
        return STATUS_SUCCESS;
    }
    e = find_entry(FileName);
    if (!e)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    *PFileContext = (PVOID)(uintptr_t)e->inode_id;
    fill_file_info(e, FileInfo);
    return STATUS_SUCCESS;
}

static NTSTATUS InvFspClose(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext)
{
    (void)FileSystem; (void)FileContext;
    return STATUS_SUCCESS;
}

static NTSTATUS InvFspGetFileInfo(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                                  FSP_FSCTL_FILE_INFO *FileInfo)
{
    uint64_t inode = (uint64_t)(uintptr_t)FileContext;
    int i;
    (void)FileSystem;
    if (inode == 0)
        return fill_root_info(FileInfo);
    for (i = 0; i < g_nentries; i++) {
        if (g_entries[i].inode_id == inode) {
            fill_file_info(&g_entries[i], FileInfo);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS InvFspRead(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                           PVOID Buffer, UINT64 Offset, ULONG Length,
                           PULONG PBytesTransferred)
{
    uint64_t inode = (uint64_t)(uintptr_t)FileContext;
    int rc;
    (void)FileSystem;
    if (inode == 0) {
        *PBytesTransferred = 0;
        return STATUS_SUCCESS;
    }
    rc = vol_read_range(g_vol, inode, Offset, Length, Buffer);
    if (rc < 0)
        return STATUS_INTERNAL_ERROR;
    *PBytesTransferred = (ULONG)rc;
    return STATUS_SUCCESS;
}

static NTSTATUS InvFspReadDirectory(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext,
                                    PWSTR Pattern, PWSTR Marker,
                                    PVOID Buffer, ULONG Length,
                                    PULONG PBytesTransferred)
{
    NTSTATUS Result = STATUS_SUCCESS;
    int i;
    (void)FileContext; (void)Pattern;

    *PBytesTransferred = 0;

    for (i = 0; i < g_nentries; i++) {
        fs_entry *e = &g_entries[i];
        FSP_FSCTL_DIR_INFO DirInfo;
        size_t name_len = (wcslen(e->name) + 1) * sizeof(wchar_t);
        if (Marker && _wcsicmp(e->name, Marker) <= 0)
            continue;
        memset(&DirInfo, 0, sizeof(DirInfo));
        DirInfo.Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + name_len);
        fill_file_info(e, &DirInfo.FileInfo);
        memcpy(DirInfo.FileNameBuf, e->name, name_len);
        if (!FspFileSystemAddDirInfo(&DirInfo, Buffer, Length, PBytesTransferred)) {
            /* buffer full: return what we have */
            Result = STATUS_SUCCESS;
            break;
        }
    }
    /* EOF marker */
    FspFileSystemAddDirInfo(NULL, Buffer, Length, PBytesTransferred);
    return Result;
}

/* ---- entry point ---- */
static FSP_FILE_SYSTEM_INTERFACE InvFspInterface;

static void init_interface(void)
{
    memset(&InvFspInterface, 0, sizeof(InvFspInterface));
    InvFspInterface.GetVolumeInfo = InvFspGetVolumeInfo;
    InvFspInterface.GetSecurityByName = InvFspGetSecurityByName;
    InvFspInterface.Open = InvFspOpen;
    InvFspInterface.Close = (VOID(*)(FSP_FILE_SYSTEM *, PVOID))InvFspClose;
    InvFspInterface.GetFileInfo = InvFspGetFileInfo;
    InvFspInterface.Read = InvFspRead;
    InvFspInterface.ReadDirectory = InvFspReadDirectory;
    /* Create/Write/etc = NULL -> read-only */
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <image> [mount-point]\n", prog);
    fprintf(stderr, "  mount-point: 'X:' or empty for auto-assign\n");
    fprintf(stderr, "  press Ctrl+C to unmount\n");
}

int wmain(int argc, wchar_t **argv)
{
    FSP_FILE_SYSTEM *FileSystem = NULL;
    FSP_FSCTL_VOLUME_PARAMS VolumeParams;
    NTSTATUS Status;
    int err;
    char image[1024];
    PWSTR MountPoint = NULL;
    int rc = 1;

    if (argc < 2) {
        usage("invf-mount");
        return 2;
    }
    WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, image, sizeof(image), NULL, NULL);
    if (argc >= 3)
        MountPoint = argv[2];

    g_vol = vol_open(image, &err);
    if (!g_vol) {
        fprintf(stderr, "cannot open volume %s (err %d)\n", image, err);
        return 1;
    }
    build_file_table();
    if (g_nentries == 0)
        fprintf(stderr, "warning: no files in volume\n");

    memset(&VolumeParams, 0, sizeof(VolumeParams));
    VolumeParams.Version = (UINT16)sizeof(FSP_FSCTL_VOLUME_PARAMS);
    VolumeParams.SectorSize = 512;
    VolumeParams.SectorsPerAllocationUnit = 8;  /* 4KB allocation unit */
    VolumeParams.VolumeSerialNumber = (UINT32)(time(NULL) & 0xFFFFFFFF);
    VolumeParams.FileInfoTimeout = 1000;
    VolumeParams.CaseSensitiveSearch = 0;
    VolumeParams.CasePreservedNames = 1;
    VolumeParams.UnicodeOnDisk = 1;
    VolumeParams.PersistentAcls = 1;
    VolumeParams.FlushAndPurgeOnCleanup = 0;
    wcscpy_s(VolumeParams.FileSystemName,
             sizeof(VolumeParams.FileSystemName) / sizeof(WCHAR), L"InvariantFS");
    init_interface();

    Status = FspFileSystemCreate(FSP_FSCTL_DISK_DEVICE_NAME, &VolumeParams,
                                 &InvFspInterface, &FileSystem);
    if (!NT_SUCCESS(Status)) {
        fprintf(stderr, "FspFileSystemCreate failed: 0x%x\n", (unsigned)Status);
        goto out;
    }
    Status = FspFileSystemSetMountPoint(FileSystem, MountPoint);
    if (!NT_SUCCESS(Status)) {
        fprintf(stderr, "FspFileSystemSetMountPoint failed: 0x%x\n", (unsigned)Status);
        goto out;
    }

    wprintf(L"InvariantFS mounted at %s (%d files)\n",
            FspFileSystemMountPoint(FileSystem), g_nentries);

    FspFileSystemStartDispatcher(FileSystem, 0);
    rc = 0;

    /* wait for Enter to unmount (Ctrl+C also works: process exit auto-unmounts) */
    {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hIn, &mode)) {
            wprintf(L"press Enter to unmount\n");
            char c; DWORD rd;
            ReadFile(hIn, &c, 1, &rd, NULL);
        } else {
            Sleep(INFINITE);  /* launched detached */
        }
    }

    FspFileSystemStopDispatcher(FileSystem);
    FspFileSystemRemoveMountPoint(FileSystem);
    FspFileSystemDelete(FileSystem);
    wprintf(L"unmounted\n");
out:
    vol_close(g_vol);
    free(g_entries);
    return rc;
}
