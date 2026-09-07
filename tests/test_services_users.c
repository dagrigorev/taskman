#include "../include/app.h"
#include <wtsapi32.h>
#include <assert.h>
#include <stdio.h>
static int fixture, calls, fail_action;
static DWORD title_bytes, body_bytes;
static BOOL WINAPI fake_query(HANDLE server, DWORD id, WTS_INFO_CLASS cls, LPWSTR *buffer, DWORD *bytes)
{
    if (!fixture) return WTSQuerySessionInformationW(server, id, cls, buffer, bytes);
    WTSINFOW *info = calloc(1, sizeof(*info));
    assert(cls == WTSSessionInfo);
    info->LogonTime.QuadPart = fixture == 2 ? 101 : 100;
    lstrcpyW(info->UserName, L"alice"); lstrcpyW(info->Domain, L"DOMAIN");
    *buffer = (WCHAR *)info; *bytes = sizeof(*info); return TRUE;
}
static void WINAPI fake_free(void *buffer) { if (fixture) free(buffer); else WTSFreeMemory(buffer); }
static BOOL WINAPI fake_session(HANDLE server, DWORD id, BOOL wait)
{ (void)server; assert(id == 42); assert(!wait); calls++; SetLastError(ERROR_ACCESS_DENIED); return !fail_action; }
static BOOL WINAPI fake_send(HANDLE server, DWORD id, LPWSTR title, DWORD titleBytes, LPWSTR body, DWORD bodyBytes, DWORD style, DWORD timeout, DWORD *response, BOOL wait)
{ (void)server; (void)title; (void)body; (void)style; (void)timeout; (void)response; assert(id == 42 && !wait); calls++; title_bytes = titleBytes; body_bytes = bodyBytes; return TRUE; }
#define WTSQuerySessionInformationW fake_query
#define WTSFreeMemory fake_free
#define WTSDisconnectSession fake_session
#define WTSLogoffSession fake_session
#define WTSSendMessageW fake_send
#include "../src/tabs/tab_users.c"
#include <winsvc.h>
static DWORD wanted_access;
static SC_HANDLE WINAPI fake_scm(LPCWSTR machine, LPCWSTR database, DWORD access)
{ if (!fixture) return OpenSCManagerW(machine, database, access); assert(access == SC_MANAGER_CONNECT); return (SC_HANDLE)1; }
static SC_HANDLE WINAPI fake_service(SC_HANDLE scm, LPCWSTR name, DWORD access)
{ (void)scm; assert(!lstrcmpW(name, L"example")); wanted_access = access; return (SC_HANDLE)2; }
static BOOL WINAPI fake_status(SC_HANDLE service, SC_STATUS_TYPE level, LPBYTE buffer, DWORD size, LPDWORD needed)
{ (void)service; (void)level; (void)size; (void)needed; ((SERVICE_STATUS_PROCESS *)buffer)->dwProcessId = 123; return TRUE; }
static BOOL WINAPI fake_start(SC_HANDLE service, DWORD count, LPCWSTR *args)
{ (void)service; (void)count; (void)args; calls++; SetLastError(ERROR_ACCESS_DENIED); return !fail_action; }
static BOOL WINAPI fake_stop(SC_HANDLE service, DWORD control, LPSERVICE_STATUS status)
{ (void)service; (void)status; assert(control == SERVICE_CONTROL_STOP); calls++; return TRUE; }
static BOOL WINAPI fake_close(SC_HANDLE handle) { return fixture ? TRUE : CloseServiceHandle(handle); }
#define OpenSCManagerW fake_scm
#define OpenServiceW fake_service
#define QueryServiceStatusEx fake_status
#define StartServiceW fake_start
#define ControlService fake_stop
#define CloseServiceHandle fake_close
#include "../src/tabs/tab_services.c"
int main(void)
{
    UserRow row = {0}, other;
    SvcRow service = {0}, second = {0};
    Users_Collect(); Svc_Collect();
    assert(!users_error); assert(!svc_error && svc_g_sharedCnt > 0);
    Users_Reset(); Svc_Reset();
    fixture = 1; row.sessionId = 42; assert(users_identity(&row));
    assert(users_action(&row, IDC_USERS_DISCONNECT, NULL, NULL) == 0 && calls == 1);
    assert(users_action(&row, IDC_USERS_LOGOFF, NULL, NULL) == 0 && calls == 2);
    fixture = 2;
    assert(users_action(&row, IDC_USERS_LOGOFF, NULL, NULL) == ERROR_NO_SUCH_LOGON_SESSION && calls == 2);
    fixture = 1; fail_action = 1;
    assert(users_action(&row, IDC_USERS_DISCONNECT, NULL, NULL) == ERROR_ACCESS_DENIED);
    fail_action = 0;
    assert(!users_action(&row, IDC_USERS_SENDMSG, L"Hello", L"World!"));
    assert(title_bytes == 10 && body_bytes == 12);
    other = row; other.sessionId++; users_column = 1;
    assert(users_compare(&row, &other) < 0);
    service.pid = 9; second.pid = 10; svc_column = 1;
    assert(svc_compare(&service, &second) < 0);
    DWORD pid = 0;
    lstrcpyW(service.name, L"example");
    assert(!svc_action(&service, svc_process, &pid) && pid == 123 && wanted_access == SERVICE_QUERY_STATUS);
    assert(!svc_action(&service, svc_start, &pid) && wanted_access == (SERVICE_QUERY_STATUS | SERVICE_START));
    assert(!svc_action(&service, svc_stop, &pid) && wanted_access == (SERVICE_QUERY_STATUS | SERVICE_STOP));
    fail_action = 1;
    assert(svc_action(&service, svc_start, &pid) == ERROR_ACCESS_DENIED);
    puts("services/users: read-only enumeration and injected session action boundaries passed");
    return 0;
}
HINSTANCE g_hInst;
UINT g_dpi = 96;
void App_ReportError(HWND h, const WCHAR *s, DWORD e) { (void)h; (void)s; (void)e; }
void App_ShowProcess(DWORD pid) { (void)pid; }
void SysInfo_RefreshNow(void) {}
void App_MinimizeOnUse(void) {}
BOOL App_OpenSystemTool(HWND h, BOOL s) { (void)h; (void)s; return FALSE; }
void UI_ApplyFont(HWND h) { (void)h; }
HWND UI_CreateListView(HWND h, int id, DWORD style) { (void)h; (void)id; (void)style; return NULL; }
HWND UI_CreateButton(HWND h, int id, const WCHAR *s, DWORD style) { (void)h; (void)id; (void)s; (void)style; return NULL; }
void UI_AddColumn(HWND h, int index, const WCHAR *s, int w, int f) { (void)h; (void)index; (void)s; (void)w; (void)f; }
int UI_Margin(HWND h) { (void)h; return 8; }
SIZE UI_ButtonSize(HWND h) { SIZE s = {80, 24}; (void)h; return s; }
void UI_PlaceButtonRow(HWND h, const int *ids, int count, int x, int y) { (void)h; (void)ids; (void)count; (void)x; (void)y; }
void UI_SetHeaderSortArrow(HWND h, int c, int d) { (void)h; (void)c; (void)d; }
