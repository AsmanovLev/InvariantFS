# Windows Port

## I/O Path

On Windows, raw block device access is done via `\\.\PhysicalDriveN` or `\\.\G:` syntax:

```c
HANDLE hDisk = CreateFile(
    L"\\\\.\\PhysicalDrive0",           // Physical drive 0
    GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    NULL,
    OPEN_EXISTING,
    FILE_FLAG_NO_BUFFERING,             // O_DIRECT equivalent
    NULL
);

// Seek + read with 4KB alignment
LARGE_INTEGER offset;
offset.QuadPart = block_address * 4096;
SetFilePointerEx(hDisk, offset, NULL, FILE_BEGIN);

// Buffer must be 4KB-aligned
void* buffer = _aligned_malloc(4096, 4096);
DWORD bytesRead;
ReadFile(hDisk, buffer, 4096, &bytesRead, NULL);
```

### WinFsp Integration (Optional)

If mounting as a drive letter is desired, WinFsp provides the FUSE-like API:

```c
// WinFsp callbacks implement the filesystem interface
NTSTATUS InvariantFS_Create(PATH* Path, ...) { /* ... */ }
NTSTATUS InvariantFS_Read(PATH* Path, void* Buf, UINT64 Offset, UINT32 Length, ...) {
    // Route through AST recipe → read from appropriate zone
    return STATUS_SUCCESS;
}
NTSTATUS InvariantFS_Write(PATH* Path, void* Buf, UINT64 Offset, UINT32 Length, ...) {
    // Write to RAW Zone, update L2P
    return STATUS_SUCCESS;
}
```

### Raw Partition Access

To use the 43 GB unallocated space on Disk 0:

```c
// Open physical drive with partition offset
HANDLE hDisk = CreateFile(L"\\\\.\\PhysicalDrive0", ...);

// Calculate partition offset (end of Z: partition = 953,568,722,944)
// InvariantFS starts at this offset
LARGE_INTEGER invariFSOffset;
invariFSOffset.QuadPart = 953568722944;
SetFilePointerEx(hDisk, invariFSOffset, NULL, FILE_BEGIN);
```

### Alignment Requirements

| Parameter | Requirement |
|-----------|-------------|
| Buffer address | 4 KB aligned (`_aligned_malloc`) |
| Offset | Multiple of 4 KB (sector size) |
| Read/write size | Multiple of 4 KB |

## Tools

| Linux | Windows Equivalent |
|-------|-------------------|
| `/dev/sda4` | `\\.\PhysicalDrive0` + offset |
| `O_DIRECT` | `FILE_FLAG_NO_BUFFERING` |
| `pread`/`pwrite` | `SetFilePointerEx` + `ReadFile`/`WriteFile` |
| FUSE | WinFsp |
| `fallocate` | `SetFileValidData` / `SetEndOfFile` |
| `fsync` | `FlushFileBuffers` |
| `mmap` | `CreateFileMapping` + `MapViewOfFile` |
