/* ------------------------------------------------------------------------
 * tab_apps.c - the Applications tab.
 * ------------------------------------------------------------------------ */
#include "app.h"

/* ----------------------------------------------------------------- model -- */

#define APP_TITLE_MAX 256
#define APP_ROW_LIMIT 1024
#define APP_PROBE_BUDGET_MS 500

typedef struct {
    HWND  hwnd;
    DWORD pid, tid;
    WCHAR title[APP_TITLE_MAX];
    BOOL  hung;
    ULONGLONG hungSince;    /* tick the hang was first seen, 0 if responding */
    BOOL  selected;
} AppRow;

static AppRow  *g_shared;
static int      g_sharedCnt;
static SRWLOCK  g_appsLock = SRWLOCK_INIT;

static AppRow  *g_view;
static int      g_viewCnt;

static HWND s_list;
static HIMAGELIST app_smallIcons, app_largeIcons;
static int app_sortColumn;
static BOOL app_sortDescending;

static BOOL app_same(const AppRow *a, const AppRow *b)
{
    return a->hwnd == b->hwnd && a->pid == b->pid && a->tid == b->tid;
}

static BOOL app_valid(const AppRow *row)
{
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(row->hwnd, &pid);
    return IsWindow(row->hwnd) && tid && pid == row->pid && tid == row->tid;
}

static int app_compare(const void *va, const void *vb)
{
    const AppRow *a = va, *b = vb;
    int cmp = app_sortColumn == 1 ? (int)a->hung - (int)b->hung : 0;
    /* Between two hung windows, the one hung longer ranks as more hung. */
    if (!cmp && app_sortColumn == 1 && a->hung && a->hungSince != b->hungSince)
        cmp = a->hungSince < b->hungSince ? 1 : -1;
    if (!cmp) cmp = lstrcmpiW(a->title, b->title);
    if (!cmp) cmp = (UINT_PTR)a->hwnd < (UINT_PTR)b->hwnd ? -1 :
                    (UINT_PTR)a->hwnd > (UINT_PTR)b->hwnd;
    return app_sortDescending ? -cmp : cmp;
}

/* ------------------------------------------------------ hang tracking --- */

/* Stamps each hung row with the tick its hang began: carried over from the
   previous sample when the same window identity was already hung, else
   'now'. A window reusing a handle under another pid or thread starts over. */
static void app_trackHangs(AppRow *rows, int count, const AppRow *prev, int prevCount, ULONGLONG now)
{
    int i, j;
    for (i = 0; i < count; ++i) {
        rows[i].hungSince = 0;
        if (!rows[i].hung) continue;
        rows[i].hungSince = now;
        for (j = 0; j < prevCount; ++j) {
            if (app_same(&rows[i], &prev[j])) {
                if (prev[j].hung && prev[j].hungSince) rows[i].hungSince = prev[j].hungSince;
                break;
            }
        }
    }
}

static void app_formatStatus(const AppRow *row, ULONGLONG now, WCHAR *buf, size_t cch)
{
    ULONGLONG secs;
    if (!row->hung) { StringCchCopyW(buf, cch, L"Running"); return; }
    secs = row->hungSince && now > row->hungSince ? (now - row->hungSince) / 1000 : 0;
    if (secs < 1)
        StringCchCopyW(buf, cch, L"Not Responding");
    else if (secs < 60)
        StringCchPrintfW(buf, cch, L"Not Responding (%us)", (unsigned)secs);
    else if (secs < 3600)
        StringCchPrintfW(buf, cch, L"Not Responding (%um %02us)",
                         (unsigned)(secs / 60), (unsigned)(secs % 60));
    else
        StringCchPrintfW(buf, cch, L"Not Responding (%uh %02um)",
                         (unsigned)(secs / 3600), (unsigned)(secs % 3600 / 60));
}

/* ------------------------------------------------------ enumeration ----- */

typedef struct { AppRow *buf; int cnt; int cap; ULONGLONG start; BOOL failed; } EnumCtx;

static BOOL CALLBACK EnumTopLevel(HWND hwnd, LPARAM lp)
{
    EnumCtx *ctx = (EnumCtx *)lp;
    WCHAR title[APP_TITLE_MAX];
    int len;
    LONG style, exStyle;
    AppRow *row;
    DWORD pid = 0, tid;
    DWORD_PTR response = 0;

    if (!ctx->start) ctx->start = GetTickCount64();
    if (ctx->cnt >= APP_ROW_LIMIT || GetTickCount64() - ctx->start >= 1000) return FALSE;
    /* This enumeration can run for up to a second, and SysInfo_Stop joins
       the worker with an INFINITE wait -- so without this the whole budget
       is added to the application's shutdown time. Discard the partial
       result: nobody is going to display it. */
    if (SysInfo_Stopping()) { ctx->failed = TRUE; return FALSE; }
    if (!IsWindowVisible(hwnd)) return TRUE;
    /* DWM stands a "Ghost" window in for a hung one: same caption, but owned
       by dwm.exe. The hung window itself is listed, so the ghost would only
       duplicate it and put DWM's pid behind End Task. */
    {
        WCHAR cls[16];
        if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && !lstrcmpW(cls, L"Ghost")) return TRUE;
    }

    style   = GetWindowLongW(hwnd, GWL_STYLE);
    exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

    /* skip child windows, tool windows, and windows with no title */
    if (style   & WS_CHILD)          return TRUE;
    if (exStyle & WS_EX_TOOLWINDOW)  return TRUE;
    if (GetWindow(hwnd, GW_OWNER) && !(exStyle & WS_EX_APPWINDOW)) return TRUE;
    tid = GetWindowThreadProcessId(hwnd, &pid);
    if (!tid) return TRUE;

    title[0] = 0;
    if (pid == GetCurrentProcessId()) {
        /* Read the caption without invoking our UI thread's window procedure.
           A timed-out WM_GETTEXT could leave that thread using a dead buffer. */
        len = InternalGetWindowText(hwnd, title, APP_TITLE_MAX);
    } else {
        /* GetWindowText reads foreign top-level captions without messaging them. */
        len = GetWindowTextW(hwnd, title, APP_TITLE_MAX);
    }
    if (len <= 0) return TRUE;

    if (ctx->cnt >= ctx->cap) {
        int nc = ctx->cap ? ctx->cap * 2 : 32;
        AppRow *tmp = (AppRow *)realloc(ctx->buf, (size_t)nc * sizeof(AppRow));
        if (!tmp) { ctx->failed = TRUE; return FALSE; }
        ctx->buf = tmp; ctx->cap = nc;
    }

    row = &ctx->buf[ctx->cnt++];
    ZeroMemory(row, sizeof(*row));
    row->hwnd = hwnd;
    row->pid = pid;
    row->tid = tid;
    lstrcpynW(row->title, title, APP_TITLE_MAX);
    row->hung = IsHungAppWindow(hwnd);
    /* Our own windows are probed too, deliberately. This runs on the
       collector thread, so a hung UI thread of ours is exactly as
       detectable -- and as worth reporting -- as any other application's.
       Only the caption read above avoids messaging our own thread. */
    if (!row->hung && GetTickCount64() - ctx->start < APP_PROBE_BUDGET_MS) {
        SetLastError(ERROR_SUCCESS);
        if (!SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 10, &response))
            row->hung = GetLastError() == ERROR_TIMEOUT;
    }
    return TRUE;
}

void Apps_Collect(void)
{
    EnumCtx ctx;
    AppRow *old;

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.start = GetTickCount64();
    EnumWindows(EnumTopLevel, (LPARAM)&ctx);
    if (ctx.failed) { free(ctx.buf); return; }
    /* g_shared is only ever replaced on this thread, so reading it here
       without the lock is safe. */
    app_trackHangs(ctx.buf, ctx.cnt, g_shared, g_sharedCnt, GetTickCount64());

    AcquireSRWLockExclusive(&g_appsLock);
    old        = g_shared;
    g_shared   = ctx.buf;
    g_sharedCnt= ctx.cnt;
    ReleaseSRWLockExclusive(&g_appsLock);

    free(old);
}

void Apps_Reset(void)
{
    AcquireSRWLockExclusive(&g_appsLock);
    free(g_shared); g_shared = NULL; g_sharedCnt = 0;
    ReleaseSRWLockExclusive(&g_appsLock);

    free(g_view); g_view = NULL; g_viewCnt = 0;
}

/* ----------------------------------------------------------------- UI ---- */

static void AppsCreate(TabPage *p)
{
    s_list = UI_CreateListView(p->hwnd, IDC_APPS_LIST, LVS_SHAREIMAGELISTS);
    if (s_list) {
        UI_AddColumn(s_list, 0, L"Task",   150, LVCFMT_LEFT);
        UI_AddColumn(s_list, 1, L"Status", 110, LVCFMT_LEFT);
        app_smallIcons = ImageList_Create(GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), ILC_COLOR32 | ILC_MASK, 32, 32);
        app_largeIcons = ImageList_Create(GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON), ILC_COLOR32 | ILC_MASK, 32, 32);
        ListView_SetImageList(s_list, app_smallIcons, LVSIL_SMALL);
        ListView_SetImageList(s_list, app_largeIcons, LVSIL_NORMAL);
        ListView_SetView(s_list, g_cfg.appsViewMode == LV_VIEW_ICON ? LV_VIEW_ICON :
            g_cfg.appsViewMode == LV_VIEW_SMALLICON ? LV_VIEW_SMALLICON : LV_VIEW_DETAILS);
    }
    UI_CreateButton(p->hwnd, IDC_APPS_ENDTASK,  L"&End Task",    0);
    UI_CreateButton(p->hwnd, IDC_APPS_SWITCHTO, L"&Switch To",   0);
    UI_CreateButton(p->hwnd, IDC_APPS_GOTOPROCESS, L"&Go to Process", 0);
    UI_CreateButton(p->hwnd, IDC_APPS_NEWTASK,  L"&New Task...", 0);
}

static void AppsLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    static const int buttons[] = {
        IDC_APPS_ENDTASK, IDC_APPS_SWITCHTO, IDC_APPS_GOTOPROCESS, IDC_APPS_NEWTASK
    };
    int margin = UI_Margin(p->hwnd);
    SIZE bs = UI_ButtonSize(p->hwnd);
    int listBottom;
    int i;

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

static void app_captureSelection(void)
{
    int i;
    for (i = 0; i < g_viewCnt; ++i)
        g_view[i].selected = s_list && (ListView_GetItemState(s_list, i, LVIS_SELECTED) & LVIS_SELECTED);
}

static void app_render(void)
{
    int i;
    BOOL focused = FALSE;
    ULONGLONG now = GetTickCount64();
    WCHAR status[64];
    if (g_viewCnt > 1) qsort(g_view, (size_t)g_viewCnt, sizeof(*g_view), app_compare);
    if (s_list) {
        LVITEMW item;
        SendMessageW(s_list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(s_list);
        if (app_smallIcons) ImageList_RemoveAll(app_smallIcons);
        if (app_largeIcons) ImageList_RemoveAll(app_largeIcons);
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        for (i = 0; i < g_viewCnt; i++) {
            HICON shared = NULL, copy;
            if (app_valid(&g_view[i])) {
                shared = (HICON)GetClassLongPtrW(g_view[i].hwnd, GCLP_HICON);
                if (!shared) shared = (HICON)GetClassLongPtrW(g_view[i].hwnd, GCLP_HICONSM);
            }
            if (!shared) shared = LoadIconW(NULL, IDI_APPLICATION);
            copy = CopyIcon(shared);
            item.iImage = -1;
            if (copy) {
                int large = app_largeIcons ? ImageList_AddIcon(app_largeIcons, copy) : -1;
                int small_idx = app_smallIcons ? ImageList_AddIcon(app_smallIcons, copy) : -1;
                item.iImage = small_idx >= 0 ? small_idx : large;
                DestroyIcon(copy);
            }
            item.iItem    = i;
            item.iSubItem = 0;
            item.pszText  = g_view[i].title;
            ListView_InsertItem(s_list, &item);
            app_formatStatus(&g_view[i], now, status, ARRAYSIZE(status));
            ListView_SetItemText(s_list, i, 1, status);
            if (g_view[i].selected) {
                ListView_SetItemState(s_list, i, LVIS_SELECTED | (focused ? 0 : LVIS_FOCUSED),
                    LVIS_SELECTED | LVIS_FOCUSED);
                focused = TRUE;
            }
        }
        UI_SetHeaderSortArrow(s_list, app_sortColumn, app_sortDescending ? -1 : 1);
        SendMessageW(s_list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(s_list, NULL, TRUE);
    }
}

static void AppsSnapshot(TabPage *p)
{
    AppRow *newView = NULL;
    int newCnt, i, j;
    (void)p;
    app_captureSelection();
    AcquireSRWLockShared(&g_appsLock);
    newCnt = g_sharedCnt;
    if (newCnt > 0) {
        newView = malloc((size_t)newCnt * sizeof(*newView));
        if (newView) memcpy(newView, g_shared, (size_t)newCnt * sizeof(*newView));
    }
    ReleaseSRWLockShared(&g_appsLock);
    if (newCnt && !newView) return; /* Retain the last usable view on allocation failure. */
    for (i = 0; i < newCnt; ++i)
        for (j = 0; j < g_viewCnt; ++j)
            if (g_view[j].selected && app_same(&newView[i], &g_view[j])) {
                newView[i].selected = TRUE;
                break;
            }
    free(g_view);
    g_view = newView;
    g_viewCnt = newCnt;
    app_render();
}

static void AppsBuildViewMenu(TabPage *p, HMENU view)
{
    (void)p;
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_VIEW_LARGEICONS, L"Lar&ge Icons");
    AppendMenuW(view, MF_STRING, IDM_VIEW_SMALLICONS, L"S&mall Icons");
    AppendMenuW(view, MF_STRING, IDM_VIEW_DETAILS,    L"&Details");
}

static void AppsInitViewMenu(TabPage *p, HMENU view)
{
    UINT check = IDM_VIEW_DETAILS;
    (void)p;
    if (g_cfg.appsViewMode == LV_VIEW_ICON)      check = IDM_VIEW_LARGEICONS;
    else if (g_cfg.appsViewMode == LV_VIEW_SMALLICON) check = IDM_VIEW_SMALLICONS;
    CheckMenuRadioItem(view, IDM_VIEW_LARGEICONS, IDM_VIEW_DETAILS,
                       check, MF_BYCOMMAND);
}

static BOOL app_selected(AppRow *row)
{
    int selected = s_list ? ListView_GetNextItem(s_list, -1, LVNI_SELECTED) : -1;
    if (selected < 0 || selected >= g_viewCnt) return FALSE;
    *row = g_view[selected];
    return TRUE;
}

static BOOL app_validate(HWND owner, const AppRow *row)
{
    if (app_valid(row)) return TRUE;
    App_ReportError(owner, L"The selected application is no longer available", ERROR_INVALID_WINDOW_HANDLE);
    return FALSE;
}

static void app_activate(TabPage *p, BOOL restore)
{
    AppRow row;
    if (!app_selected(&row) || !app_validate(p->hwnd, &row)) return;
    if (restore && IsIconic(row.hwnd)) ShowWindowAsync(row.hwnd, SW_RESTORE);
    if (SetForegroundWindow(row.hwnd)) App_MinimizeOnUse();
    else App_ReportError(p->hwnd, L"Switch to application", ERROR_ACCESS_DENIED);
}

static void app_arrange(TabPage *p, int command)
{
    int count = 0, i, index;
    AppRow *rows;
    RECT work;
    app_captureSelection();
    if (!g_viewCnt) return;
    rows = malloc((size_t)g_viewCnt * sizeof(*rows));
    if (!rows) { App_ReportError(p->hwnd, L"Arrange applications", ERROR_NOT_ENOUGH_MEMORY); return; }
    for (i = 0; i < g_viewCnt; ++i)
        if (g_view[i].selected && app_validate(p->hwnd, &g_view[i])) rows[count++] = g_view[i];
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        free(rows);
        App_ReportError(p->hwnd, L"Read desktop work area", GetLastError());
        return;
    }
    for (index = 0; index < count; ++index) {
        int x = work.left, y = work.top, width = work.right - work.left, height = work.bottom - work.top;
        HWND hwnd = rows[index].hwnd;
        if (!app_validate(p->hwnd, &rows[index])) continue;
        if (command == IDM_WINDOWS_MINIMIZE || command == IDM_WINDOWS_MAXIMIZE) {
            if (!ShowWindowAsync(hwnd, command == IDM_WINDOWS_MINIMIZE ? SW_MINIMIZE : SW_MAXIMIZE))
                App_ReportError(p->hwnd, L"Change application window", GetLastError());
            continue;
        }
        if (command == IDM_WINDOWS_TILEHORZ) {
            y += MulDiv(height, index, count);
            height = MulDiv(height, index + 1, count) - MulDiv(height, index, count);
        } else if (command == IDM_WINDOWS_TILEVERT) {
            x += MulDiv(width, index, count);
            width = MulDiv(width, index + 1, count) - MulDiv(width, index, count);
        } else {
            int offset = (index % 10) * GetSystemMetrics(SM_CYCAPTION);
            x += offset; y += offset;
            width = width * 2 / 3; height = height * 2 / 3;
        }
        ShowWindowAsync(hwnd, SW_RESTORE);
        if (!SetWindowPos(hwnd, NULL, x, y, width > 0 ? width : 1, height > 0 ? height : 1,
                SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER))
            App_ReportError(p->hwnd, L"Arrange application window", GetLastError());
    }
    free(rows);
}

static void AppsCommand(TabPage *p, int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case IDM_VIEW_LARGEICONS:
        g_cfg.appsViewMode = LV_VIEW_ICON;
        if (s_list) ListView_SetView(s_list, LV_VIEW_ICON);
        break;
    case IDM_VIEW_SMALLICONS:
        g_cfg.appsViewMode = LV_VIEW_SMALLICON;
        if (s_list) ListView_SetView(s_list, LV_VIEW_SMALLICON);
        break;
    case IDM_VIEW_DETAILS:
        g_cfg.appsViewMode = LV_VIEW_DETAILS;
        if (s_list) ListView_SetView(s_list, LV_VIEW_DETAILS);
        break;

    case IDC_APPS_NEWTASK:
        App_RunTaskDialog(p->hwnd);
        break;

    case IDC_APPS_SWITCHTO:
        app_activate(p, TRUE);
        break;

    case IDC_APPS_GOTOPROCESS: {
        AppRow row;
        if (app_selected(&row) && app_validate(p->hwnd, &row))
            App_ShowProcess(row.pid);
        break;
    }

    case IDC_APPS_ENDTASK: {
        AppRow row;
        WCHAR question[APP_TITLE_MAX + 128];
        BOOL ok = app_selected(&row) && app_validate(p->hwnd, &row);
        if (ok && row.hung) {
            /* A hung window never pumps the WM_CLOSE below, so asking it
               to close would silently do nothing. End the process. */
            StringCchPrintfW(question, ARRAYSIZE(question),
                L"\"%s\" is not responding and cannot be asked to close.\n\n"
                L"End its process? Unsaved work will be lost.", row.title);
            if (MessageBoxW(p->hwnd, question, L"End Task", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES &&
                app_validate(p->hwnd, &row)) {
                HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, row.pid);
                if (!process)
                    App_ReportError(p->hwnd, L"End process", GetLastError());
                else {
                    if (!TerminateProcess(process, 1))
                        App_ReportError(p->hwnd, L"End process", GetLastError());
                    CloseHandle(process);
                }
            }
        } else if (ok) {
            StringCchPrintfW(question, ARRAYSIZE(question),
                L"Close \"%s\"? Unsaved work may be lost.", row.title);
            if (MessageBoxW(p->hwnd, question, L"End Task", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES &&
                app_validate(p->hwnd, &row)) {
                if (!PostMessageW(row.hwnd, WM_CLOSE, 0, 0))
                    App_ReportError(p->hwnd, L"Close application", GetLastError());
            }
        }
        break;
    }

    case IDM_WINDOWS_TILEHORZ:
    case IDM_WINDOWS_TILEVERT:
    case IDM_WINDOWS_CASCADE:
    case IDM_WINDOWS_MINIMIZE:
    case IDM_WINDOWS_MAXIMIZE:
        app_arrange(p, id);
        break;
    case IDM_WINDOWS_BRINGTOFRONT:
        app_activate(p, FALSE);
        break;

    default:
        break;
    }
}

static HWND AppsPrimary(TabPage *p)
{
    (void)p;
    return s_list;
}

static void AppsDestroy(TabPage *p)
{
    (void)p;
    if (s_list && IsWindow(s_list)) {
        ListView_SetImageList(s_list, NULL, LVSIL_SMALL);
        ListView_SetImageList(s_list, NULL, LVSIL_NORMAL);
    }
    if (app_smallIcons) ImageList_Destroy(app_smallIcons);
    if (app_largeIcons) ImageList_Destroy(app_largeIcons);
    app_smallIcons = app_largeIcons = NULL;
    s_list = NULL;
}

static BOOL AppsNotify(TabPage *p, NMHDR *nm, LRESULT *result)
{
    if (nm->hwndFrom != s_list) return FALSE;
    if (nm->code == LVN_COLUMNCLICK) {
        int column = ((NMLISTVIEW *)nm)->iSubItem;
        app_captureSelection();
        app_sortDescending = column == app_sortColumn ? !app_sortDescending : FALSE;
        app_sortColumn = column;
        app_render();
    } else if (nm->code == NM_DBLCLK) {
        app_activate(p, TRUE);
    } else return FALSE;
    *result = 0;
    return TRUE;
}

static TabPage s_page = {
    L"Applications", NULL, TAB_APPS,
    AppsCreate, AppsDestroy, AppsLayout,
    AppsSnapshot,
    AppsCommand,
    AppsNotify,
    NULL,               /* OnActivate     */
    NULL,               /* OnFontChanged  */
    NULL,               /* OnContextMenu  */
    AppsBuildViewMenu,
    AppsInitViewMenu,
    AppsPrimary
};

TabPage *TabApps(void) { return &s_page; }
