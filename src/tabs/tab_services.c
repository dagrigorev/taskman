/* ------------------------------------------------------------------------
 * tab_services.c - the Services tab.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include <winsvc.h>

/* ----------------------------------------------------------------- model -- */

#define SVC_NAME_MAX  257
#define SVC_DESC_MAX  257

typedef struct {
    WCHAR name[SVC_NAME_MAX];
    WCHAR description[SVC_DESC_MAX];
    DWORD pid;
    DWORD state;
} SvcRow;

static SvcRow  *svc_g_shared;
static int      svc_g_sharedCnt;
static SRWLOCK  svc_g_svcLock = SRWLOCK_INIT;

static SvcRow  *svc_g_view;
static int      svc_g_viewCnt;

static HWND svc_s_list;

/* --------------------------------------------------------- collector ----- */

static DWORD svc_error, svc_reported;
static int svc_column, svc_direction = 1;
enum { svc_start = 48001, svc_stop, svc_process };

void Svc_Collect(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    BYTE *buffer = NULL;
    SvcRow *rows = NULL, *old;
    DWORD error = ERROR_SUCCESS, resume = 0, count = 0;
    int total = 0;
    if (!scm) { error = GetLastError(); goto publish; }
    buffer = malloc(256 * 1024);
    if (!buffer) { error = ERROR_NOT_ENOUGH_MEMORY; goto publish; }
    for (;;) {
        DWORD needed = 0, before = resume;
        BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
            SERVICE_STATE_ALL, buffer, 256 * 1024, &needed, &count, &resume, NULL);
        DWORD result = ok ? ERROR_SUCCESS : GetLastError();
        ENUM_SERVICE_STATUS_PROCESSW *entries = (void *)buffer;
        if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) { error = result; break; }
        if (count) {
            SvcRow *grown = realloc(rows, ((size_t)total + count) * sizeof(*rows));
            if (!grown) { error = ERROR_NOT_ENOUGH_MEMORY; break; }
            rows = grown;
            for (DWORD i = 0; i < count; ++i) {
                SvcRow *row = &rows[total++];
                ZeroMemory(row, sizeof(*row));
                /* SCM names have a documented 256-character maximum. Never act
                   on a truncated identifier even if a provider violates it. */
                if (wcslen(entries[i].lpServiceName) >= SVC_NAME_MAX) {
                    error = ERROR_INVALID_DATA; break;
                }
                lstrcpyW(row->name, entries[i].lpServiceName);
                lstrcpynW(row->description, entries[i].lpDisplayName, SVC_DESC_MAX);
                row->pid = entries[i].ServiceStatusProcess.dwProcessId;
                row->state = entries[i].ServiceStatusProcess.dwCurrentState;
            }
        }
        if (error || ok) break;
        if (!count && resume == before) { error = ERROR_MORE_DATA; break; }
    }
publish:
    free(buffer);
    if (scm) CloseServiceHandle(scm);
    AcquireSRWLockExclusive(&svc_g_svcLock);
    svc_error = error;
    old = NULL;
    if (!error) { old = svc_g_shared; svc_g_shared = rows; svc_g_sharedCnt = total; rows = NULL; }
    ReleaseSRWLockExclusive(&svc_g_svcLock);
    free(old); free(rows);
}

void Svc_Reset(void)
{
    AcquireSRWLockExclusive(&svc_g_svcLock);
    free(svc_g_shared); svc_g_shared = NULL; svc_g_sharedCnt = 0;
    ReleaseSRWLockExclusive(&svc_g_svcLock);
    free(svc_g_view); svc_g_view = NULL; svc_g_viewCnt = 0;
}

/* ----------------------------------------------------------------- UI ---- */

static const WCHAR *svc_SvcStateName(DWORD state)
{
    switch (state) {
    case SERVICE_RUNNING:        return L"Running";
    case SERVICE_STOPPED:        return L"Stopped";
    case SERVICE_START_PENDING:  return L"Starting";
    case SERVICE_STOP_PENDING:   return L"Stopping";
    case SERVICE_PAUSED:         return L"Paused";
    case SERVICE_PAUSE_PENDING:  return L"Pausing";
    case SERVICE_CONTINUE_PENDING: return L"Resuming";
    default:                     return L"Unknown";
    }
}

static void svc_SvcCreate(TabPage *p)
{
    svc_s_list = UI_CreateListView(p->hwnd, IDC_SVC_LIST, 0);
    if (svc_s_list) {
        UI_AddColumn(svc_s_list, 0, L"Name",         80, LVCFMT_LEFT);
        UI_AddColumn(svc_s_list, 1, L"PID",          30, LVCFMT_RIGHT);
        UI_AddColumn(svc_s_list, 2, L"Description", 160, LVCFMT_LEFT);
        UI_AddColumn(svc_s_list, 3, L"Status",       45, LVCFMT_LEFT);
    }
    UI_CreateButton(p->hwnd, IDC_SVC_SERVICES, L"Se&rvices...", 0);
}

static void svc_SvcLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    static const int buttons[] = { IDC_SVC_SERVICES };
    int margin = UI_Margin(p->hwnd);
    SIZE bs = UI_ButtonSize(p->hwnd);
    int listBottom;
    HWND btn = GetDlgItem(p->hwnd, IDC_SVC_SERVICES);

    if (btn) ShowWindow(btn, tiny ? SW_HIDE : SW_SHOW);

    if (tiny) {
        if (svc_s_list) MoveWindow(svc_s_list, 0, 0, cx, cy, TRUE);
        return;
    }

    listBottom = cy - margin - bs.cy - margin;
    if (listBottom < margin + DPX(40)) listBottom = margin + DPX(40);
    if (svc_s_list)
        MoveWindow(svc_s_list, margin, margin,
                   cx - 2 * margin > 0 ? cx - 2 * margin : 1,
                   listBottom - margin > 0 ? listBottom - margin : 1, TRUE);

    UI_PlaceButtonRow(p->hwnd, buttons, (int)ARRAYSIZE(buttons), cx, cy);
}

static int svc_compare(const void *left, const void *right)
{
    const SvcRow *a = left, *b = right;
    int result;
    switch (svc_column) {
    case 1: result = (a->pid > b->pid) - (a->pid < b->pid); break;
    case 2: result = lstrcmpiW(a->description, b->description); break;
    case 3: result = lstrcmpiW(svc_SvcStateName(a->state), svc_SvcStateName(b->state)); break;
    default: result = lstrcmpiW(a->name, b->name); break;
    }
    if (!result) result = lstrcmpiW(a->name, b->name);
    return result * svc_direction;
}

static void svc_SvcSnapshot(TabPage *p)
{
    int i;
    WCHAR selected[SVC_NAME_MAX] = L"";
    int sel = svc_s_list ? ListView_GetNextItem(svc_s_list, -1, LVNI_SELECTED) : -1;
    if (sel >= 0 && sel < svc_g_viewCnt) lstrcpyW(selected, svc_g_view[sel].name);

    /* copy shared -> view */
    {
        SvcRow *newView = NULL;
        int newCnt = 0;
        AcquireSRWLockExclusive(&svc_g_svcLock);
        if (svc_g_sharedCnt > 0) {
            newView = (SvcRow *)malloc((size_t)svc_g_sharedCnt * sizeof(SvcRow));
            if (newView) {
                memcpy(newView, svc_g_shared, (size_t)svc_g_sharedCnt * sizeof(SvcRow));
                newCnt = svc_g_sharedCnt;
            }
        }
        DWORD error = svc_error;
        BOOL failed = svc_g_sharedCnt > 0 && !newView;
        ReleaseSRWLockExclusive(&svc_g_svcLock);
        if (error != svc_reported) {
            svc_reported = error;
            if (error) App_ReportError(p->hwnd, L"Enumerating services", error);
        }
        if (failed) return;
        free(svc_g_view);
        svc_g_view    = newView;
        svc_g_viewCnt = newCnt;
    }

    if (!svc_s_list) return;

    if (svc_g_viewCnt > 1) qsort(svc_g_view, (size_t)svc_g_viewCnt, sizeof(*svc_g_view), svc_compare);
    SendMessageW(svc_s_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(svc_s_list);
    for (i = 0; i < svc_g_viewCnt; i++) {
        WCHAR pidStr[16];
        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask     = LVIF_TEXT;
        item.iItem    = i;
        item.iSubItem = 0;
        item.pszText  = svc_g_view[i].name;
        ListView_InsertItem(svc_s_list, &item);
        if (!lstrcmpiW(selected, svc_g_view[i].name))
            ListView_SetItemState(svc_s_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

        if (svc_g_view[i].pid)
            StringCchPrintfW(pidStr, ARRAYSIZE(pidStr), L"%lu",
                             (unsigned long)svc_g_view[i].pid);
        else
            pidStr[0] = L'\0';

        ListView_SetItemText(svc_s_list, i, 1, pidStr);
        ListView_SetItemText(svc_s_list, i, 2, svc_g_view[i].description);
        ListView_SetItemText(svc_s_list, i, 3, UI_Str(svc_SvcStateName(svc_g_view[i].state)));
    }
    UI_SetHeaderSortArrow(svc_s_list, svc_column, svc_direction);
    SendMessageW(svc_s_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(svc_s_list, NULL, TRUE);
}

static DWORD svc_action(const SvcRow *row, int action, DWORD *pid)
{
    DWORD error = ERROR_SUCCESS, bytes;
    SERVICE_STATUS_PROCESS status;
    SERVICE_STATUS stopped;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), service;
    if (!scm) return GetLastError();
    service = OpenServiceW(scm, row->name, SERVICE_QUERY_STATUS |
        (action == svc_start ? SERVICE_START : action == svc_stop ? SERVICE_STOP : 0));
    if (!service) { error = GetLastError(); goto done; }
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (BYTE *)&status, sizeof(status), &bytes))
        error = GetLastError();
    else if (action == svc_start && !StartServiceW(service, 0, NULL)) error = GetLastError();
    else if (action == svc_stop && !ControlService(service, SERVICE_CONTROL_STOP, &stopped)) error = GetLastError();
    else if (action == svc_process) *pid = status.dwProcessId;
    CloseServiceHandle(service);
done:
    CloseServiceHandle(scm);
    return error;
}

static void svc_run(TabPage *p, const SvcRow *row, int id)
{
    DWORD error, pid = 0;
    if (id == svc_stop) {
        WCHAR question[512];
        StringCchPrintfW(question, ARRAYSIZE(question), L"Stop service %s?", row->name);
        if (MessageBoxW(p->hwnd, question, L"Stop Service", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    }
    error = svc_action(row, id, &pid);
    if (error) App_ReportError(p->hwnd, L"Service operation", error);
    else if (id == svc_process && pid) App_ShowProcess(pid);
    else SysInfo_RefreshNow();
}

static void svc_SvcCommand(TabPage *p, int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case svc_start: case svc_stop: case svc_process: {
        int sel = svc_s_list ? ListView_GetNextItem(svc_s_list, -1, LVNI_SELECTED) : -1;
        SvcRow row;
        if (sel < 0 || sel >= svc_g_viewCnt) break;
        row = svc_g_view[sel];
        svc_run(p, &row, id);
        break;
    }
    case IDC_SVC_SERVICES:
        if (App_OpenSystemTool(p->hwnd, TRUE)) App_MinimizeOnUse();
        break;
    default:
        break;
    }
}

static BOOL svc_notify(TabPage *p, NMHDR *nm, LRESULT *result)
{
    if (nm->hwndFrom == svc_s_list && nm->code == LVN_COLUMNCLICK) {
        int column = ((NMLISTVIEW *)nm)->iSubItem;
        svc_direction = column == svc_column ? -svc_direction : 1;
        svc_column = column; svc_SvcSnapshot(p); *result = 0; return TRUE;
    }
    return FALSE;
}
static void svc_context(TabPage *p, HWND from, int x, int y)
{
    int sel = svc_s_list ? ListView_GetNextItem(svc_s_list, -1, LVNI_SELECTED) : -1;
    HMENU menu;
    int command;
    SvcRow selected;
    (void)from;
    if (sel < 0 || sel >= svc_g_viewCnt) return;
    selected = svc_g_view[sel];
    if (x == -1 && y == -1) { POINT pt = {10, 10}; ClientToScreen(svc_s_list, &pt); x = pt.x; y = pt.y; }
    menu = CreatePopupMenu();
    AppendMenuW(menu, (UINT)(MF_STRING | (svc_g_view[sel].state == SERVICE_STOPPED ? 0 : MF_GRAYED)), svc_start, L"Start Service");
    AppendMenuW(menu, (UINT)(MF_STRING | (svc_g_view[sel].state == SERVICE_RUNNING ? 0 : MF_GRAYED)), svc_stop, L"Stop Service");
    AppendMenuW(menu, (UINT)(MF_STRING | (svc_g_view[sel].pid ? 0 : MF_GRAYED)), svc_process, L"Go to Process");
    command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, p->hwnd, NULL);
    DestroyMenu(menu);
    if (command) svc_run(p, &selected, command);
}

static HWND svc_SvcPrimary(TabPage *p)
{
    (void)p;
    return svc_s_list;
}

static void svc_SvcDestroy(TabPage *p)
{
    (void)p;
    svc_s_list = NULL;
}

static TabPage svc_s_page = {
    L"Services", NULL, TAB_SERVICES,
    svc_SvcCreate, svc_SvcDestroy, svc_SvcLayout,
    svc_SvcSnapshot, svc_SvcCommand, svc_notify, NULL, NULL, svc_context, NULL, NULL,
    svc_SvcPrimary
};

TabPage *TabServices(void) { return &svc_s_page; }
