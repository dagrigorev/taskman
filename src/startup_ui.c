/* ------------------------------------------------------------------------
 * startup_ui.c - the Startup Impact window.
 *
 * A modeless window owned by the main window: every startup entry beside
 * the CPU time and memory its running processes have used, so heavy
 * autostarts stand out. Read on open and on Refresh; nothing here runs on
 * the collector thread.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ui.h"
#include "startup.h"

#define STARTUP_PROCESS_MAX 4096

static const WCHAR kStartupClass[] = L"ClassicTaskManagerStartupWnd";

static HWND          s_window, s_list, s_summary;
static StartupEntry  s_entries[STARTUP_MAX_ENTRIES];
static int           s_count;
static int           s_sortColumn = STARTUP_COL_CPU;
static BOOL          s_sortDescending = TRUE;

static int StartupSortThunk(const void *a, const void *b)
{
    int cmp = Startup_Compare((const StartupEntry *)a, (const StartupEntry *)b, s_sortColumn);
    return s_sortDescending ? -cmp : cmp;
}

static void StartupFormatCpu(ULONGLONG cpu, WCHAR *buf, size_t cch)
{
    ULONGLONG secs = cpu / 10000000ULL;
    StringCchPrintfW(buf, cch, L"%llu:%02llu:%02llu", secs / 3600, secs / 60 % 60, secs % 60);
}

static void StartupFill(void)
{
    int i, running = 0, enabled = 0;
    ULONGLONG cpu = 0;
    WCHAR text[256];
    if (!s_list) return;
    if (s_count > 1) qsort(s_entries, (size_t)s_count, sizeof(*s_entries), StartupSortThunk);
    SendMessageW(s_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < s_count; ++i) {
        const StartupEntry *e = &s_entries[i];
        LVITEMW item;
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = i;
        item.pszText = UI_Str(e->name[0] ? e->name : L"(unnamed)");
        item.lParam = i;
        ListView_InsertItem(s_list, &item);
        ListView_SetItemText(s_list, i, STARTUP_COL_STATUS, UI_Str(e->disabled ? L"Disabled" : L"Enabled"));
        if (e->running) {
            StringCchPrintfW(text, ARRAYSIZE(text), L"%d", e->running);
            ListView_SetItemText(s_list, i, STARTUP_COL_RUNNING, text);
            StartupFormatCpu(e->cpuTime, text, ARRAYSIZE(text));
            ListView_SetItemText(s_list, i, STARTUP_COL_CPU, text);
            UI_FormatSize(e->privateBytes, text, ARRAYSIZE(text));
            ListView_SetItemText(s_list, i, STARTUP_COL_MEMORY, text);
        } else {
            ListView_SetItemText(s_list, i, STARTUP_COL_RUNNING, UI_Str(L"Not running"));
        }
        ListView_SetItemText(s_list, i, STARTUP_COL_SOURCE, UI_Str(Startup_SourceName(e->source)));
        ListView_SetItemText(s_list, i, STARTUP_COL_COMMAND,
                             UI_Str(e->command[0] ? e->command : L"(empty command)"));
        if (e->running) ++running;
        if (!e->disabled) ++enabled;
        cpu += e->cpuTime;
    }
    UI_SetHeaderSortArrow(s_list, s_sortColumn, s_sortDescending ? -1 : 1);
    SendMessageW(s_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_list, NULL, TRUE);

    if (s_summary) {
        WCHAR total[32];
        StartupFormatCpu(cpu, total, ARRAYSIZE(total));
        StringCchPrintfW(text, ARRAYSIZE(text),
            L"%d startup entries  |  %d enabled  |  %d running now  |  %s CPU time since those processes started",
            s_count, enabled, running, total);
        SetWindowTextW(s_summary, text);
    }
}

static void StartupRefresh(void)
{
    static StartupProcess procs[STARTUP_PROCESS_MAX];
    int procCount;
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    s_count = Startup_ReadEntries(s_entries, STARTUP_MAX_ENTRIES);
    procCount = Startup_ReadProcesses(procs, STARTUP_PROCESS_MAX);
    Startup_Attribute(s_entries, s_count, procs, procCount);
    StartupFill();
    SetCursor(old);
}

static const StartupEntry *StartupSelected(void)
{
    int sel = s_list ? ListView_GetNextItem(s_list, -1, LVNI_SELECTED) : -1;
    return sel >= 0 && sel < s_count ? &s_entries[sel] : NULL;
}

static void StartupUpdateButtons(void)
{
    const StartupEntry *e = StartupSelected();
    EnableWindow(GetDlgItem(s_window, IDC_STARTUP_GOTO), e && e->running > 0);
    EnableWindow(GetDlgItem(s_window, IDC_STARTUP_LOCATION), e && wcschr(e->exe, L'\\') != NULL);
}

static void StartupLayout(int cx, int cy)
{
    int margin = DPX(12), buttonW = DPX(110), buttonH = DPX(30), top = DPX(34);
    int by = cy - margin - buttonH;
    MoveWindow(s_summary, margin, margin, cx - 2 * margin, DPX(20), TRUE);
    MoveWindow(s_list, margin, top, cx - 2 * margin, by - margin - top > 0 ? by - margin - top : 1, TRUE);
    MoveWindow(GetDlgItem(s_window, IDC_STARTUP_REFRESH),  margin, by, buttonW, buttonH, TRUE);
    MoveWindow(GetDlgItem(s_window, IDC_STARTUP_GOTO),     margin + buttonW + DPX(8), by, buttonW, buttonH, TRUE);
    MoveWindow(GetDlgItem(s_window, IDC_STARTUP_LOCATION), margin + 2 * (buttonW + DPX(8)), by, DPX(130), buttonH, TRUE);
    MoveWindow(GetDlgItem(s_window, IDCANCEL),             cx - margin - buttonW, by, buttonW, buttonH, TRUE);
}

static LRESULT CALLBACK StartupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        static const struct { const WCHAR *title; int width; int fmt; } columns[STARTUP_COL_COUNT] = {
            { L"Name", 140, LVCFMT_LEFT }, { L"Status", 75, LVCFMT_LEFT },
            { L"Running", 85, LVCFMT_RIGHT }, { L"CPU time", 80, LVCFMT_RIGHT },
            { L"Memory", 80, LVCFMT_RIGHT }, { L"Source", 150, LVCFMT_LEFT },
            { L"Command", 320, LVCFMT_LEFT }
        };
        int i;
        s_window = hwnd;
        s_summary = UI_CreateStatic(hwnd, IDC_STARTUP_SUMMARY, L"Reading startup entries...", SS_LEFT);
        s_list = UI_CreateListView(hwnd, IDC_STARTUP_LIST, LVS_SINGLESEL | LVS_SHOWSELALWAYS);
        if (s_list) {
            for (i = 0; i < STARTUP_COL_COUNT; ++i)
                UI_AddColumn(s_list, i, columns[i].title, columns[i].width, columns[i].fmt);
        }
        UI_CreateButton(hwnd, IDC_STARTUP_REFRESH, L"&Refresh", 0);
        UI_CreateButton(hwnd, IDC_STARTUP_GOTO, L"&Go to Process", 0);
        UI_CreateButton(hwnd, IDC_STARTUP_LOCATION, L"Open file &location", 0);
        UI_CreateButton(hwnd, IDCANCEL, L"Close", 0);
        StartupRefresh();
        StartupUpdateButtons();
        return 0;
    }
    case WM_SIZE:
        StartupLayout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = DPX(560);
        mm->ptMinTrackSize.y = DPX(300);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, UI_MUTED);
        return (LRESULT)UI_BackgroundBrush();
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        LRESULT result = 0;
        if (nm->hwndFrom == s_list) {
            if (nm->code == LVN_COLUMNCLICK) {
                int column = ((NMLISTVIEW *)lp)->iSubItem;
                if (column == s_sortColumn) s_sortDescending = !s_sortDescending;
                else {
                    s_sortColumn = column;
                    s_sortDescending = column == STARTUP_COL_CPU || column == STARTUP_COL_MEMORY ||
                                       column == STARTUP_COL_RUNNING;
                }
                StartupFill();
                StartupUpdateButtons();
                return 0;
            }
            if (nm->code == LVN_ITEMCHANGED) { StartupUpdateButtons(); return 0; }
            if (nm->code == NM_DBLCLK) {
                PostMessageW(hwnd, WM_COMMAND, IDC_STARTUP_GOTO, 0);
                return 0;
            }
        }
        if (UI_ControlNotify(nm, &result)) return result;
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_STARTUP_REFRESH:
            StartupRefresh();
            StartupUpdateButtons();
            return 0;
        case IDC_STARTUP_GOTO: {
            const StartupEntry *e = StartupSelected();
            if (e && e->running > 0) {
                App_ShowProcess(e->pids[0]);
                SetForegroundWindow(g_hMain);
            }
            return 0;
        }
        case IDC_STARTUP_LOCATION: {
            const StartupEntry *e = StartupSelected();
            if (e && wcschr(e->exe, L'\\')) {
                WCHAR args[MAX_PATH + 16];
                StringCchPrintfW(args, ARRAYSIZE(args), L"/select,\"%s\"", e->exe);
                if ((INT_PTR)ShellExecuteW(hwnd, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL) <= 32)
                    App_ReportError(hwnd, L"Open file location", GetLastError());
            }
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        s_window = s_list = s_summary = NULL;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND App_ShowStartupImpact(HWND owner)
{
    WNDCLASSEXW wc;
    if (s_window) {
        if (IsIconic(s_window)) ShowWindow(s_window, SW_RESTORE);
        SetForegroundWindow(s_window);
        return s_window;
    }
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(g_hInst, kStartupClass, &wc)) {
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = StartupWndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = UI_BackgroundBrush();
        wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = kStartupClass;
        if (!RegisterClassExW(&wc)) {
            App_ReportError(owner, L"Open Startup Impact", GetLastError());
            return NULL;
        }
    }
    s_window = CreateWindowExW(0, kStartupClass, L"Startup Impact",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        DPX(1040), DPX(560), owner, NULL, g_hInst, NULL);
    if (!s_window) {
        App_ReportError(owner, L"Open Startup Impact", GetLastError());
        return NULL;
    }
    ShowWindow(s_window, SW_SHOWNORMAL);
    return s_window;
}

int StartupTest_RowCount(void)
{
    return s_list ? ListView_GetItemCount(s_list) : -1;
}
