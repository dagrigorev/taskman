#include "../include/app.h"
#include <stdio.h>

static WCHAR launchedFile[1024], launchedArgs[1024], launchedDir[1024];
static int launches, directoryMode, moduleMode, destroyed, settingsSaveCalls;
static BOOL shellResult = TRUE;
static BOOL tokenElevated, tokenQueryFails;
static int registrations, existingWindowLookups;
static BOOL WINAPI TestOpenToken(HANDLE process, DWORD access, HANDLE *token)
{
    /* Never adjust privileges during a startup regression test. */
    if (access != TOKEN_QUERY) return FALSE;
    return OpenProcessToken(process, access, token);
}
static HANDLE WINAPI ExistingMutex(SECURITY_ATTRIBUTES *sa, BOOL owner, const WCHAR *name)
{
    HANDLE handle;
    (void)sa; (void)owner; (void)name;
    handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetLastError(ERROR_ALREADY_EXISTS);
    return handle;
}
static HWND WINAPI ExistingWindow(const WCHAR *cls, const WCHAR *title)
{ (void)cls; (void)title; ++existingWindowLookups; return (HWND)1; }
static BOOL WINAPI IgnoreShow(HWND window, int how) { (void)window; (void)how; return TRUE; }
static BOOL WINAPI IgnoreWindow(HWND window) { (void)window; return FALSE; }
static ATOM WINAPI StopAtRegistration(const WNDCLASSEXW *cls)
{ (void)cls; ++registrations; return 0; }
static BOOL WINAPI FakeTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS cls,
                                        void *info, DWORD size, DWORD *returned)
{
    (void)token;
    if (tokenQueryFails || cls != TokenElevation || size < sizeof(TOKEN_ELEVATION)) return FALSE;
    ((TOKEN_ELEVATION *)info)->TokenIsElevated = tokenElevated;
    *returned = sizeof(TOKEN_ELEVATION);
    return TRUE;
}
static BOOL WINAPI CaptureShell(SHELLEXECUTEINFOW *sei)
{
    ++launches;
    StringCchCopyW(launchedFile, ARRAYSIZE(launchedFile), sei->lpFile);
    StringCchCopyW(launchedArgs, ARRAYSIZE(launchedArgs), sei->lpParameters ? sei->lpParameters : L"");
    StringCchCopyW(launchedDir, ARRAYSIZE(launchedDir), sei->lpDirectory ? sei->lpDirectory : L"");
    return shellResult;
}
static UINT WINAPI FakeSystemDirectory(WCHAR *buf, UINT cch)
{
    if (directoryMode == 1) return 0;
    if (directoryMode == 2) return cch;
    if (directoryMode == 3) {
        UINT i;
        for (i = 0; i + 1 < cch; ++i) buf[i] = L'x';
        buf[cch - 1] = 0;
        return cch - 1;
    }
    StringCchCopyW(buf, cch, L"C:\\Test Windows\\System32");
    return 24;
}
static DWORD WINAPI FakeModulePath(HMODULE module, WCHAR *buf, DWORD cch)
{
    (void)module;
    if (moduleMode == 1) return 0;
    if (moduleMode == 2) return cch;
    StringCchCopyW(buf, cch, L"C:\\Task Manager\\taskman.exe");
    return 27;
}
static BOOL WINAPI CaptureDestroy(HWND hwnd) { (void)hwnd; ++destroyed; return TRUE; }
static void CaptureSave(void) { ++settingsSaveCalls; }

#define ShellExecuteExW CaptureShell
#define GetSystemDirectoryW FakeSystemDirectory
#define GetModuleFileNameW FakeModulePath
#define DestroyWindow CaptureDestroy
#define Settings_Save CaptureSave
#define GetTokenInformation FakeTokenInformation
#define OpenProcessToken TestOpenToken
#define CreateMutexW ExistingMutex
#define FindWindowW ExistingWindow
#define ShowWindow IgnoreShow
#define IsIconic IgnoreWindow
#define SetForegroundWindow IgnoreWindow
#define RegisterClassExW StopAtRegistration
#include "../src/main.c"
#undef Settings_Save
#undef DestroyWindow
#define s_page services_page
#include "../src/tabs/tab_services.c"
#undef s_page
#define s_page performance_page
#include "../src/tabs/tab_perf.c"
#undef s_page

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void)
{
    int mode;
    TabPage *perf = TabPerformance(), *svc = TabServices();
    perf->OnCommand(perf, IDC_PERF_RESMON, BN_CLICKED, NULL);
    CHECK(launches == 1);
    CHECK(wcscmp(launchedFile, L"C:\\Test Windows\\System32\\resmon.exe") == 0);
    CHECK(wcscmp(launchedDir, L"C:\\Test Windows\\System32") == 0);
    CHECK(launchedArgs[0] == 0);
    svc->OnCommand(svc, IDC_SVC_SERVICES, BN_CLICKED, NULL);
    CHECK(launches == 2);
    CHECK(wcscmp(launchedFile, L"C:\\Test Windows\\System32\\mmc.exe") == 0);
    CHECK(wcscmp(launchedArgs, L"\"C:\\Test Windows\\System32\\services.msc\"") == 0);
    for (mode = 1; mode <= 3; ++mode) {
        directoryMode = mode;
        perf->OnCommand(perf, IDC_PERF_RESMON, BN_CLICKED, NULL);
        svc->OnCommand(svc, IDC_SVC_SERVICES, BN_CLICKED, NULL);
        CHECK(launches == 2);
    }
    directoryMode = 0;
    moduleMode = 2;
    App_RelaunchElevated();
    CHECK(launches == 2 && destroyed == 0 && settingsSaveCalls == 0);
    moduleMode = 1;
    App_RelaunchElevated();
    CHECK(launches == 2 && destroyed == 0);
    moduleMode = 0;
    shellResult = FALSE;
    App_RelaunchElevated();
    CHECK(launches == 3 && destroyed == 0);
    shellResult = TRUE;
    App_RelaunchElevated();
    CHECK(launches == 4 && destroyed == 1);
    CHECK(wcscmp(launchedFile, L"C:\\Task Manager\\taskman.exe") == 0);
    CHECK(wcscmp(launchedArgs, L"--elevated-relaunch") == 0);
    CHECK(!IsElevatedRelaunch(L"--elevated-relaunch"));
    tokenElevated = TRUE;
    CHECK(IsElevatedRelaunch(L"--elevated-relaunch"));
    CHECK(!IsElevatedRelaunch(NULL) && !IsElevatedRelaunch(L""));
    CHECK(!IsElevatedRelaunch(L"--elevated-relaunch extra"));
    tokenQueryFails = TRUE;
    CHECK(!IsElevatedRelaunch(L"--elevated-relaunch"));
    tokenQueryFails = FALSE;
    /* Exercise the real entry point with an existing parent. Stop before
       creating a GUI; ordinary launches must reuse it, elevated handoff must
       progress to registration instead of activating the exiting parent. */
    CHECK(wWinMain(GetModuleHandleW(NULL), NULL, L"", SW_HIDE) == 0);
    CHECK(existingWindowLookups == 1 && registrations == 0);
    CHECK(wWinMain(GetModuleHandleW(NULL), NULL, L"--elevated-relaunch", SW_HIDE) == 1);
    CHECK(existingWindowLookups == 1 && registrations == 1);
    tokenElevated = FALSE;
    CHECK(wWinMain(GetModuleHandleW(NULL), NULL, L"--elevated-relaunch", SW_HIDE) == 0);
    CHECK(existingWindowLookups == 2 && registrations == 1);
    puts("PASS system tool launch boundaries and elevation path failures");
    return 0;
}
