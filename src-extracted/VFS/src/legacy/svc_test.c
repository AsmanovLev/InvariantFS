#include <windows.h>
#include <stdio.h>
#include <winsvc.h>
int wmain(void) {
    SC_HANDLE scm = OpenSCManagerW(0, 0, 0);
    if (!scm) { printf("OpenSCManager err=%lu\n", GetLastError()); return 1; }
    const wchar_t *names[] = {
        L"WinFsp+20260805T124445Z",
        L"WinFsp",
        L"WinFsp.Launcher"
    };
    for (int i = 0; i < 3; i++) {
        SC_HANDLE svc = OpenServiceW(scm, names[i], SERVICE_QUERY_STATUS | SERVICE_START);
        if (svc) {
            printf("  %ls: FOUND\n", names[i]);
            SERVICE_STATUS st;
            QueryServiceStatus(svc, &st);
            printf("    state: %lu\n", (ULONG)st.dwCurrentState);
            CloseServiceHandle(svc);
        } else {
            printf("  %ls: NOT FOUND (err=%lu)\n", names[i], (unsigned long)GetLastError());
        }
    }
    CloseServiceHandle(scm);
    return 0;
}
