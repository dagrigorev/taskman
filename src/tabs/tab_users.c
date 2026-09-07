/* ------------------------------------------------------------------------
 * tab_users.c - the Users tab.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include <wtsapi32.h>

/* ----------------------------------------------------------------- model -- */

#define USER_NAME_MAX   257
#define USER_CLIENT_MAX 64
#define USER_SESSION_MAX 32

typedef struct {
    DWORD sessionId;
    WCHAR domain[257];
    LARGE_INTEGER logonTime;
    BOOL identityValid;
    WCHAR userName[USER_NAME_MAX];
    WCHAR clientName[USER_CLIENT_MAX];
    WCHAR sessionName[USER_SESSION_MAX];
    WTS_CONNECTSTATE_CLASS state;
} UserRow;

static UserRow *g_shared;
static int      g_sharedCnt;
static SRWLOCK  g_usersLock = SRWLOCK_INIT;

static UserRow *g_view;
static int      g_viewCnt;

static HWND s_list;
static DWORD users_error, users_reported;
static int users_column, users_direction = 1;

static BOOL users_identity(UserRow *row)
{
    WCHAR *buffer = NULL;
    DWORD bytes = 0;
    WTSINFOW *info;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, row->sessionId,
                                   WTSSessionInfo, &buffer, &bytes)) return FALSE;
    if (bytes < sizeof(WTSINFOW)) { WTSFreeMemory(buffer); SetLastError(ERROR_INVALID_DATA); return FALSE; }
    info = (WTSINFOW *)buffer;
    row->logonTime = info->LogonTime;
    lstrcpynW(row->domain, info->Domain, ARRAYSIZE(row->domain));
    lstrcpynW(row->userName, info->UserName, ARRAYSIZE(row->userName));
    row->identityValid = row->userName[0] && row->logonTime.QuadPart != 0;
    WTSFreeMemory(buffer);
    return row->identityValid;
}

static BOOL users_same(const UserRow *a, const UserRow *b)
{
    return a->identityValid && b->identityValid && a->sessionId == b->sessionId &&
        a->logonTime.QuadPart == b->logonTime.QuadPart &&
        !lstrcmpW(a->userName, b->userName) && !lstrcmpW(a->domain, b->domain);
}

static DWORD users_revalidate(const UserRow *row)
{
    UserRow current = {0};
    current.sessionId = row->sessionId;
    if (!users_identity(&current)) return ERROR_NO_SUCH_LOGON_SESSION;
    return users_same(row, &current) ? ERROR_SUCCESS : ERROR_NO_SUCH_LOGON_SESSION;
}

/* --------------------------------------------------------- collector ----- */

static WCHAR *WtsQueryStr(DWORD sessionId, WTS_INFO_CLASS cls)
{
    WCHAR *buf = NULL;
    DWORD  bytes = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
                                    sessionId, cls, &buf, &bytes))
        return buf;
    return NULL;
}

void Users_Collect(void)
{
    WTS_SESSION_INFOW *sessions = NULL;
    DWORD count = 0;
    UserRow *rows = NULL;
    int rowCnt = 0;
    DWORD i;
    UserRow *old;
    DWORD error = ERROR_SUCCESS;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count))
        { error = GetLastError(); goto publish; }

    if (count > 0) {
        rows = (UserRow *)malloc(count * sizeof(UserRow));
        if (!rows) { error = ERROR_NOT_ENOUGH_MEMORY; goto publish; }
        if (rows) {
            for (i = 0; i < count; i++) {
                WCHAR *str;
                ZeroMemory(&rows[rowCnt], sizeof(rows[rowCnt]));
                rows[rowCnt].sessionId = sessions[i].SessionId;
                rows[rowCnt].state     = sessions[i].State;

                str = WtsQueryStr(sessions[i].SessionId, WTSUserName);
                if (str) { lstrcpynW(rows[rowCnt].userName, str, USER_NAME_MAX); WTSFreeMemory(str); }

                str = WtsQueryStr(sessions[i].SessionId, WTSClientName);
                if (str) { lstrcpynW(rows[rowCnt].clientName, str, USER_CLIENT_MAX); WTSFreeMemory(str); }

                if (sessions[i].pWinStationName)
                    lstrcpynW(rows[rowCnt].sessionName, sessions[i].pWinStationName, USER_SESSION_MAX);

                users_identity(&rows[rowCnt]);

                /* skip sessions with no user */
                if (rows[rowCnt].userName[0] || rows[rowCnt].sessionName[0])
                    rowCnt++;
            }
        }
    }

publish:
    if (sessions) WTSFreeMemory(sessions);

    AcquireSRWLockExclusive(&g_usersLock);
    users_error = error;
    old = NULL;
    if (!error) { old = g_shared; g_shared = rows; g_sharedCnt = rowCnt; rows = NULL; }
    ReleaseSRWLockExclusive(&g_usersLock);
    free(old); free(rows);
}

void Users_Reset(void)
{
    AcquireSRWLockExclusive(&g_usersLock);
    free(g_shared); g_shared = NULL; g_sharedCnt = 0;
    ReleaseSRWLockExclusive(&g_usersLock);
    free(g_view); g_view = NULL; g_viewCnt = 0;
}

/* ----------------------------------------------------------------- UI ---- */

static const WCHAR *WtsStateName(WTS_CONNECTSTATE_CLASS s)
{
    switch (s) {
    case WTSActive:       return L"Active";
    case WTSConnected:    return L"Connected";
    case WTSConnectQuery: return L"Connecting";
    case WTSShadow:       return L"Shadow";
    case WTSDisconnected: return L"Disconnected";
    case WTSIdle:         return L"Idle";
    case WTSListen:       return L"Listen";
    case WTSReset:        return L"Reset";
    case WTSDown:         return L"Down";
    case WTSInit:         return L"Init";
    default:              return L"Unknown";
    }
}

static void UsersCreate(TabPage *p)
{
    s_list = UI_CreateListView(p->hwnd, IDC_USERS_LIST, 0);
    if (s_list) {
        UI_AddColumn(s_list, 0, L"User",        90, LVCFMT_LEFT);
        UI_AddColumn(s_list, 1, L"ID",          25, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 2, L"Status",      55, LVCFMT_LEFT);
        UI_AddColumn(s_list, 3, L"Client Name", 75, LVCFMT_LEFT);
        UI_AddColumn(s_list, 4, L"Session",     70, LVCFMT_LEFT);
    }
    UI_CreateButton(p->hwnd, IDC_USERS_DISCONNECT, L"&Disconnect",      0);
    UI_CreateButton(p->hwnd, IDC_USERS_LOGOFF,     L"&Logoff",          0);
    UI_CreateButton(p->hwnd, IDC_USERS_SENDMSG,    L"Sen&d Message...", 0);
}

static void UsersLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    static const int buttons[] = {
        IDC_USERS_DISCONNECT, IDC_USERS_LOGOFF, IDC_USERS_SENDMSG
    };
    int margin = UI_Margin(p->hwnd);
    SIZE bs = UI_ButtonSize(p->hwnd);
    int listBottom, i;

    for (i = 0; i < (int)ARRAYSIZE(buttons); i++) {
        HWND h = GetDlgItem(p->hwnd, buttons[i]);
        if (h) ShowWindow(h, tiny ? SW_HIDE : SW_SHOW);
    }

    if (tiny) {
        if (s_list) MoveWindow(s_list, 0, 0, cx, cy, TRUE);
        return;
    }

    listBottom = cy - margin - bs.cy - margin;
    if (listBottom < margin + DPX(40)) listBottom = margin + DPX(40);
    if (s_list)
        MoveWindow(s_list, margin, margin,
                   cx - 2 * margin > 0 ? cx - 2 * margin : 1,
                   listBottom - margin > 0 ? listBottom - margin : 1, TRUE);

    UI_PlaceButtonRow(p->hwnd, buttons, (int)ARRAYSIZE(buttons), cx, cy);
}

static int users_compare(const void *left, const void *right)
{
    const UserRow *a = left, *b = right;
    int result;
    switch (users_column) {
    case 1: result = (a->sessionId > b->sessionId) - (a->sessionId < b->sessionId); break;
    case 2: result = lstrcmpiW(WtsStateName(a->state), WtsStateName(b->state)); break;
    case 3: result = lstrcmpiW(a->clientName, b->clientName); break;
    case 4: result = lstrcmpiW(a->sessionName, b->sessionName); break;
    default: result = lstrcmpiW(a->userName, b->userName); break;
    }
    if (!result) result = (a->sessionId > b->sessionId) - (a->sessionId < b->sessionId);
    return result * users_direction;
}

static void UsersSnapshot(TabPage *p)
{
    int i;
    UserRow selected = {0};
    int sel = s_list ? ListView_GetNextItem(s_list, -1, LVNI_SELECTED) : -1;
    if (sel >= 0 && sel < g_viewCnt) selected = g_view[sel];

    {
        UserRow *newView = NULL;
        int newCnt = 0;
        AcquireSRWLockExclusive(&g_usersLock);
        if (g_sharedCnt > 0) {
            newView = (UserRow *)malloc((size_t)g_sharedCnt * sizeof(UserRow));
            if (newView) {
                memcpy(newView, g_shared, (size_t)g_sharedCnt * sizeof(UserRow));
                newCnt = g_sharedCnt;
            }
        }
        DWORD error = users_error;
        BOOL failed = g_sharedCnt > 0 && !newView;
        ReleaseSRWLockExclusive(&g_usersLock);
        if (error != users_reported) {
            users_reported = error;
            if (error) App_ReportError(p->hwnd, L"Enumerating sessions", error);
        }
        if (failed) return;
        free(g_view);
        g_view    = newView;
        g_viewCnt = newCnt;
    }

    if (!s_list) return;

    if (g_viewCnt > 1) qsort(g_view, (size_t)g_viewCnt, sizeof(*g_view), users_compare);
    SendMessageW(s_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < g_viewCnt; i++) {
        WCHAR idStr[16];
        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask     = LVIF_TEXT;
        item.iItem    = i;
        item.iSubItem = 0;
        item.pszText  = g_view[i].userName;
        ListView_InsertItem(s_list, &item);
        if (users_same(&selected, &g_view[i]))
            ListView_SetItemState(s_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

        StringCchPrintfW(idStr, ARRAYSIZE(idStr), L"%lu",
                         (unsigned long)g_view[i].sessionId);
        ListView_SetItemText(s_list, i, 1, idStr);
        ListView_SetItemText(s_list, i, 2, UI_Str(WtsStateName(g_view[i].state)));
        ListView_SetItemText(s_list, i, 3, g_view[i].clientName);
        ListView_SetItemText(s_list, i, 4, g_view[i].sessionName);
    }
    UI_SetHeaderSortArrow(s_list, users_column, users_direction);
    SendMessageW(s_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_list, NULL, TRUE);
}

typedef struct {
    UserRow target;
    WCHAR title[128];
    WCHAR body[4096];
} UsersMessage;

/* This boundary is also exercised with injected WTS calls by the tests. */
static DWORD users_action(const UserRow *row, int action, WCHAR *title, WCHAR *body)
{
    DWORD response = 0, error = users_revalidate(row);
    BOOL ok;
    if (error) return error;
    if (action == IDC_USERS_LOGOFF)
        ok = WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, row->sessionId, FALSE);
    else if (action == IDC_USERS_DISCONNECT)
        ok = WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, row->sessionId, FALSE);
    else
        ok = WTSSendMessageW(WTS_CURRENT_SERVER_HANDLE, row->sessionId,
            title, (DWORD)(wcslen(title) * sizeof(WCHAR)),
            body, (DWORD)(wcslen(body) * sizeof(WCHAR)), MB_OK, 0, &response, FALSE);
    return ok ? ERROR_SUCCESS : GetLastError();
}

static INT_PTR CALLBACK users_message_dialog(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
{
    UsersMessage *data = (UsersMessage *)GetWindowLongPtrW(hwnd, DWLP_USER);
    if (message == WM_INITDIALOG) {
        RECT r;
        HWND edit;
        data = (UsersMessage *)lp;
        SetWindowLongPtrW(hwnd, DWLP_USER, lp);
        SetWindowTextW(hwnd, L"Send Message");
        GetClientRect(hwnd, &r);
        CreateWindowW(L"STATIC", L"Title:", WS_CHILD | WS_VISIBLE, 12, 12, 70, 20, hwnd, NULL, g_hInst, NULL);
        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", data->title,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 12, 34, r.right - 24, 26,
            hwnd, (HMENU)101, g_hInst, NULL);
        SendMessageW(edit, EM_SETLIMITTEXT, ARRAYSIZE(data->title) - 1, 0);
        CreateWindowW(L"STATIC", L"Message:", WS_CHILD | WS_VISIBLE, 12, 70, 90, 20, hwnd, NULL, g_hInst, NULL);
        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            12, 92, r.right - 24, r.bottom - 144, hwnd, (HMENU)102, g_hInst, NULL);
        SendMessageW(edit, EM_SETLIMITTEXT, ARRAYSIZE(data->body) - 1, 0);
        CreateWindowW(L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            r.right - 188, r.bottom - 38, 80, 26, hwnd, (HMENU)IDOK, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            r.right - 96, r.bottom - 38, 80, 26, hwnd, (HMENU)IDCANCEL, g_hInst, NULL);
        UI_ApplyFont(hwnd);
        SetFocus(edit);
        return FALSE;
    }
    if (message == WM_CLOSE) { EndDialog(hwnd, IDCANCEL); return TRUE; }
    if (message == WM_COMMAND && LOWORD(wp) == IDCANCEL) { EndDialog(hwnd, IDCANCEL); return TRUE; }
    if (message == WM_COMMAND && LOWORD(wp) == IDOK && data) {
        DWORD error;
        GetDlgItemTextW(hwnd, 101, data->title, ARRAYSIZE(data->title));
        GetDlgItemTextW(hwnd, 102, data->body, ARRAYSIZE(data->body));
        if (!data->body[0]) { SetFocus(GetDlgItem(hwnd, 102)); return TRUE; }
        error = users_action(&data->target, IDC_USERS_SENDMSG, data->title, data->body);
        if (error) App_ReportError(hwnd, L"Sending message", error);
        else EndDialog(hwnd, IDOK);
        return TRUE;
    }
    return FALSE;
}

static void UsersCommand(TabPage *p, int id, int code, HWND ctl)
{
    int sel;
    UserRow row;
    DWORD error;
    WCHAR question[512];
    (void)code; (void)ctl;
    if (id != IDC_USERS_DISCONNECT && id != IDC_USERS_LOGOFF && id != IDC_USERS_SENDMSG) return;
    sel = s_list ? ListView_GetNextItem(s_list, -1, LVNI_SELECTED) : -1;
    if (sel < 0 || sel >= g_viewCnt) return;
    row = g_view[sel]; /* Modal dialogs may dispatch a snapshot and replace g_view. */
    if (!row.identityValid) { App_ReportError(p->hwnd, L"Verifying session identity", ERROR_NO_SUCH_LOGON_SESSION); return; }
    if (id == IDC_USERS_SENDMSG) {
        struct { DLGTEMPLATE dialog; WORD menu, windowClass, title; } layout = {0};
        UsersMessage data = {0};
        data.target = row;
        lstrcpyW(data.title, L"Message from Task Manager");
        layout.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER;
        layout.dialog.cx = 280; layout.dialog.cy = 180;
        if (DialogBoxIndirectParamW(g_hInst, &layout.dialog, p->hwnd, users_message_dialog, (LPARAM)&data) == -1)
            App_ReportError(p->hwnd, L"Opening message dialog", GetLastError());
        return;
    }
    StringCchPrintfW(question, ARRAYSIZE(question),
        id == IDC_USERS_LOGOFF ? L"Log off %s? Unsaved work may be lost." : L"Disconnect %s? Applications will keep running.", row.userName);
    if (MessageBoxW(p->hwnd, question, L"Task Manager", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    error = users_action(&row, id, NULL, NULL);
    if (error) App_ReportError(p->hwnd, L"Session operation", error);
    else SysInfo_RefreshNow();
}

static BOOL users_notify(TabPage *p, NMHDR *nm, LRESULT *result)
{
    if (nm->hwndFrom == s_list && nm->code == LVN_COLUMNCLICK) {
        int column = ((NMLISTVIEW *)nm)->iSubItem;
        users_direction = column == users_column ? -users_direction : 1;
        users_column = column; UsersSnapshot(p); *result = 0; return TRUE;
    }
    return FALSE;
}

static HWND UsersPrimary(TabPage *p)
{
    (void)p;
    return s_list;
}

static void UsersDestroy(TabPage *p)
{
    (void)p;
    s_list = NULL;
}

static TabPage s_page = {
    L"Users", NULL, TAB_USERS,
    UsersCreate, UsersDestroy, UsersLayout,
    UsersSnapshot, UsersCommand, users_notify, NULL, NULL, NULL, NULL, NULL,
    UsersPrimary
};

TabPage *TabUsers(void) { return &s_page; }
