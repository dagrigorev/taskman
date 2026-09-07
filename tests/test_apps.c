#include "../include/app.h"
#include <stdio.h>

Settings g_cfg;
HINSTANCE g_hInst;
HWND g_hMain;
HFONT g_hFont;
UINT g_dpi = 96;
HWND UI_CreateListView(HWND p, int id, DWORD s) { (void)p; (void)id; (void)s; return NULL; }
void UI_AddColumn(HWND h, int i, const WCHAR *t, int w, int f) { (void)h;(void)i;(void)t;(void)w;(void)f; }
HWND UI_CreateButton(HWND h,int i,const WCHAR *t,DWORD s) { (void)h;(void)i;(void)t;(void)s;return NULL; }
int UI_Margin(HWND h) { (void)h;return 8; }
SIZE UI_ButtonSize(HWND h) { SIZE s={70,24};(void)h;return s; }
void UI_PlaceButtonRow(HWND h,const int *ids,int n,int x,int y) { (void)h;(void)ids;(void)n;(void)x;(void)y; }
void UI_SetHeaderSortArrow(HWND h,int c,int d) { (void)h;(void)c;(void)d; }
BOOL App_RunTaskDialog(HWND h) { (void)h;return TRUE; }
void App_MinimizeOnUse(void) {}
void App_ReportError(HWND h,const WCHAR *op,DWORD e) { (void)h;(void)op;(void)e; }
/* Drives the shutdown-abort path in EnumTopLevel without a real collector. */
static BOOL simulateStopping;
BOOL SysInfo_Stopping(void) { return simulateStopping; }
static HWND actionTarget;
static int actionCount, confirmAnswer = IDNO;
static BOOL simulateTimeout;
static BOOL WINAPI captureShow(HWND h, int c) { (void)c; actionTarget=h; ++actionCount; return TRUE; }
static BOOL WINAPI capturePosition(HWND h,HWND a,int x,int y,int w,int height,UINT flags)
{ (void)a;(void)x;(void)y;(void)w;(void)height; if (!(flags & SWP_ASYNCWINDOWPOS)) return FALSE; actionTarget=h;++actionCount;return TRUE; }
static int WINAPI captureConfirm(HWND h,LPCWSTR text,LPCWSTR caption,UINT flags)
{ (void)h;(void)text;(void)caption;(void)flags;return confirmAnswer; }
static BOOL WINAPI capturePost(HWND h,UINT m,WPARAM w,LPARAM l)
{ (void)m;(void)w;(void)l;actionTarget=h;++actionCount;return TRUE; }
static LRESULT WINAPI probe(HWND h,UINT m,WPARAM w,LPARAM l,UINT flags,UINT timeout,PDWORD_PTR result)
{
    if (simulateTimeout && m == WM_NULL) { SetLastError(ERROR_TIMEOUT); return 0; }
    return SendMessageTimeoutW(h,m,w,l,flags,timeout,result);
}
#define ShowWindowAsync captureShow
#define SetWindowPos capturePosition
#define MessageBoxW captureConfirm
#define PostMessageW capturePost
#define SendMessageTimeoutW probe
#include "../src/tabs/tab_apps.c"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
static HWND fixture(HWND owner, DWORD ex, BOOL visible)
{
    return CreateWindowExW(ex, L"STATIC", L"Taskman private fixture", WS_OVERLAPPEDWINDOW |
        (visible ? WS_VISIBLE : 0), -30000,-30000,100,100,owner,NULL,GetModuleHandleW(NULL),NULL);
}
typedef struct { HANDLE ready, stop; HWND hwnd; } HungFixture;
static DWORD WINAPI hung_fixture(void *arg)
{
    HungFixture *f = arg;
    f->hwnd = fixture(NULL, 0, TRUE);
    SetEvent(f->ready);
    WaitForSingleObject(f->stop, INFINITE); /* Deliberately does not pump messages. */
    DestroyWindow(f->hwnd);
    return 0;
}
int main(void)
{
    EnumCtx ctx = {0};
    HWND top = fixture(NULL,0,TRUE), owned = fixture(top,0,TRUE);
    HWND tool = fixture(NULL,WS_EX_TOOLWINDOW,TRUE), hidden = fixture(NULL,0,FALSE);
    CHECK(top && owned && tool && hidden);
    EnumTopLevel(top,(LPARAM)&ctx);
    CHECK(ctx.cnt == 1);
    CHECK(app_valid(&ctx.buf[0]));
    {
        /* Shutdown must abort the enumeration rather than let SysInfo_Stop's
           join wait out the remaining budget, and the partial result must be
           marked failed so Apps_Collect discards it instead of publishing a
           truncated window list. */
        EnumCtx stopping = {0};
        simulateStopping = TRUE;
        CHECK(EnumTopLevel(top,(LPARAM)&stopping) == FALSE);
        CHECK(stopping.failed);
        CHECK(stopping.cnt == 0);
        simulateStopping = FALSE;
        free(stopping.buf);
    }
    {
        HungFixture hung = {0};
        EnumCtx hungCtx = {0};
        HANDLE thread;
        ULONGLONG start;
        hung.ready = CreateEventW(NULL,TRUE,FALSE,NULL);
        hung.stop = CreateEventW(NULL,TRUE,FALSE,NULL);
        thread = CreateThread(NULL,0,hung_fixture,&hung,0,NULL);
        CHECK(thread != NULL);
        CHECK(WaitForSingleObject(hung.ready,2000) == WAIT_OBJECT_0);
        start = GetTickCount64();
        EnumTopLevel(hung.hwnd,(LPARAM)&hungCtx);
        CHECK(GetTickCount64() - start < 1000);
        CHECK(hungCtx.cnt == 1 && hungCtx.buf[0].hung);
        SetEvent(hung.stop);
        CHECK(WaitForSingleObject(thread,2000) == WAIT_OBJECT_0);
        CloseHandle(thread); CloseHandle(hung.ready); CloseHandle(hung.stop);
        free(hungCtx.buf);
    }
    {
        EnumCtx timeoutCtx = {0};
        simulateTimeout = TRUE;
        EnumTopLevel(top,(LPARAM)&timeoutCtx);
        CHECK(timeoutCtx.cnt == 1 && timeoutCtx.buf[0].hung);
        simulateTimeout = FALSE;
        free(timeoutCtx.buf);
    }
    {
        AppRow stale = ctx.buf[0];
        stale.pid++;
        CHECK(!app_valid(&stale));
        CHECK(!app_same(&ctx.buf[0], &stale));
        stale = ctx.buf[0];
        stale.tid++;
        CHECK(!app_valid(&stale));
    }
    EnumTopLevel(owned,(LPARAM)&ctx);
    CHECK(ctx.cnt == 1);
    {
        HWND forced = fixture(top,WS_EX_APPWINDOW,TRUE);
        EnumTopLevel(forced,(LPARAM)&ctx);
        CHECK(ctx.cnt == 2);
        DestroyWindow(forced);
        CHECK(!app_valid(&ctx.buf[1]));
    }
    {
        AppRow sortRows[3] = {0};
        lstrcpyW(sortRows[0].title,L"Zulu");
        lstrcpyW(sortRows[1].title,L"Alpha");
        lstrcpyW(sortRows[2].title,L"Beta");
        sortRows[1].hung = TRUE;
        app_sortColumn = 0; app_sortDescending = FALSE;
        qsort(sortRows,3,sizeof(*sortRows),app_compare);
        CHECK(lstrcmpW(sortRows[0].title,L"Alpha") == 0);
        app_sortColumn = 1; app_sortDescending = TRUE;
        qsort(sortRows,3,sizeof(*sortRows),app_compare);
        CHECK(sortRows[0].hung);
    }
    {
        INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES};
        AppRow selected = ctx.buf[0];
        LVITEMW item = {0};
        InitCommonControlsEx(&controls);
        s_list = CreateWindowExW(0,WC_LISTVIEWW,L"",WS_CHILD | LVS_REPORT,
            0,0,100,100,top,NULL,GetModuleHandleW(NULL),NULL);
        CHECK(s_list != NULL);
        g_view = malloc(sizeof(*g_view));
        *g_view = selected; g_viewCnt = 1;
        item.mask = LVIF_TEXT; item.pszText = selected.title;
        CHECK(ListView_InsertItem(s_list,&item) == 0);
        ListView_SetItemState(s_list,0,LVIS_SELECTED,LVIS_SELECTED);
        g_shared = malloc(sizeof(*g_shared)); *g_shared = selected; g_sharedCnt = 1;
        AppsSnapshot(NULL);
        CHECK(ListView_GetSelectedCount(s_list) == 1);
        {
            TabPage page = {0};
            int commands[] = {IDM_WINDOWS_TILEHORZ, IDM_WINDOWS_TILEVERT,
                IDM_WINDOWS_CASCADE, IDM_WINDOWS_MINIMIZE, IDM_WINDOWS_MAXIMIZE};
            size_t command;
            page.hwnd = top;
            for (command = 0; command < ARRAYSIZE(commands); ++command) {
                actionCount = 0;
                AppsCommand(&page,commands[command],0,NULL);
                CHECK(actionCount > 0 && actionTarget == top);
            }
            confirmAnswer = IDNO; actionCount = 0;
            AppsCommand(&page,IDC_APPS_ENDTASK,0,NULL);
            CHECK(actionCount == 0);
            confirmAnswer = IDYES;
            AppsCommand(&page,IDC_APPS_ENDTASK,0,NULL);
            CHECK(actionCount == 1 && actionTarget == top);
            g_view[0].pid++; actionCount = 0;
            AppsCommand(&page,IDC_APPS_ENDTASK,0,NULL);
            CHECK(actionCount == 0);
            g_view[0].pid--;
        }
        g_shared[0].pid++; /* Reused identity must not inherit selection. */
        AppsSnapshot(NULL);
        CHECK(ListView_GetSelectedCount(s_list) == 0);
        DestroyWindow(s_list); s_list = NULL;
    }
    EnumTopLevel(tool,(LPARAM)&ctx);
    EnumTopLevel(hidden,(LPARAM)&ctx);
    CHECK(ctx.cnt == 2);
    free(ctx.buf);
    Apps_Reset();
    Apps_Collect();
    {
        int i;
        BOOL found = FALSE;
        for (i = 0; i < g_sharedCnt; ++i) if (g_shared[i].hwnd == top) found = TRUE;
        CHECK(found);
    }
    DestroyWindow(hidden); DestroyWindow(tool); DestroyWindow(owned); DestroyWindow(top);
    Apps_Reset();
    printf("applications: %d failures\n", failures);
    return failures ? 1 : 0;
}
