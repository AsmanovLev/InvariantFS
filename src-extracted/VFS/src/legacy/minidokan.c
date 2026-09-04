/*
 * minidokan.c — минимальная Dokan FS: пустой том, чтобы проверить драйвер
 */
#include <windows.h>
#include <dokan.h>
#include <stdio.h>

static NTSTATUS DOKAN_CALLBACK MdCreateFile(LPCWSTR path, PDOKAN_IO_SECURITY_CONTEXT sec,
    ACCESS_MASK access, ULONG attrs, ULONG share, ULONG disp, ULONG opts,
    PDOKAN_FILE_INFO info)
{
    (void)sec; (void)access; (void)share; (void)opts;
    if (wcslen(path) == 1) {  /* root */
        info->IsDirectory = TRUE;
        return STATUS_SUCCESS;
    }
    if (disp == FILE_OPEN_IF || disp == FILE_CREATE) {
        info->IsDirectory = FALSE;
        info->Context = (ULONG64)1;
        return STATUS_SUCCESS;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS DOKAN_CALLBACK MdGetFileInformation(LPCWSTR path,
    LPBY_HANDLE_FILE_INFORMATION buf, PDOKAN_FILE_INFO info)
{
    (void)info;
    memset(buf, 0, sizeof *buf);
    if (wcslen(path) == 1) {
        buf->dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        buf->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        buf->nFileSizeHigh = 0;
        buf->nFileSizeLow = 0;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MdFindFiles(LPCWSTR path, PFillFindData fill,
    PDOKAN_FILE_INFO info)
{
    (void)fill; (void)info;
    if (wcslen(path) != 1)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    /* пустой корень — не добавляем ничего */
    return STATUS_SUCCESS;
}

static NTSTATUS DOKAN_CALLBACK MdReadFile(LPCWSTR path, LPVOID buf, DWORD len,
    LPDWORD read, LONGLONG off, PDOKAN_FILE_INFO info)
{
    (void)path; (void)buf; (void)len; (void)off; (void)info;
    *read = 0;
    return STATUS_SUCCESS;
}

static void DOKAN_CALLBACK MdCloseFile(LPCWSTR path, PDOKAN_FILE_INFO info)
{
    (void)path; (void)info;
}

static DOKAN_OPERATIONS md_ops = {
    .ZwCreateFile = MdCreateFile,
    .GetFileInformation = MdGetFileInformation,
    .FindFiles = MdFindFiles,
    .ReadFile = MdReadFile,
    .CloseFile = MdCloseFile,
};

int wmain(int argc, wchar_t **argv)
{
    DOKAN_OPTIONS opt;
    memset(&opt, 0, sizeof opt);
    opt.Version = DOKAN_VERSION;
    opt.SingleThread = FALSE;
    opt.Options = DOKAN_OPTION_DEBUG;
    opt.MountPoint = (argc > 1) ? argv[1] : L"Y:";

    wprintf(L"starting DokanMain...\n");
    fflush(stdout);
    int rc = DokanMain(&opt, &md_ops);
    wprintf(L"DokanMain returned: %d\n", rc);
    fflush(stdout);
    return rc;
}
