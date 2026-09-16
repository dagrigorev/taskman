/* Exercise real windows and controls without writing settings or running tasks. */
#include "../include/app.h"
#include "../include/blame.h"
#include "../include/startup.h"
#include <stdio.h>
static void IgnoreSettingsSave(void) {}
#define Settings_Save IgnoreSettingsSave
#include "../src/main.c"
#undef Settings_Save

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL workspace line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void Pump(DWORD duration)
{
    ULONGLONG until = GetTickCount64() + duration;
    MSG msg;
    do {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(5);
    } while (GetTickCount64() < until);
}

static void Capture(HWND hwnd, const WCHAR *path)
{
    RECT rc; BITMAPINFO info = {0}; BITMAPFILEHEADER header = {0};
    HDC dc = GetDC(hwnd), memory = CreateCompatibleDC(dc);
    HBITMAP bitmap, old;
    void *pixels; HANDLE file; DWORD written;
    GetClientRect(hwnd, &rc);
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rc.right; info.bmiHeader.biHeight = -rc.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = (DWORD)(rc.right * rc.bottom * 4);
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    CHECK(bitmap != NULL);
    if (!bitmap) { DeleteDC(memory); ReleaseDC(hwnd, dc); return; }
    old = SelectObject(memory, bitmap);
    CHECK(PrintWindow(hwnd, memory, PW_CLIENTONLY));
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + info.bmiHeader.biSizeImage;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, &header, sizeof(header), &written, NULL);
        WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, NULL);
        WriteFile(file, pixels, info.bmiHeader.biSizeImage, &written, NULL);
        CloseHandle(file);
    }
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(hwnd, dc);
}

static DWORD ListPidAt(HWND list, int index)
{
    WCHAR buf[32] = {0};
    ListView_GetItemText(list, index, 5, buf, ARRAYSIZE(buf));
    return (DWORD)wcstoul(buf, NULL, 10);
}

static int ListFindPid(HWND list, DWORD pid)
{
    int i, count = ListView_GetItemCount(list);
    for (i = 0; i < count; i++)
        if (ListPidAt(list, i) == pid) return i;
    return -1;
}

static void CheckBounds(HWND control, HWND page)
{
    RECT r, parent;
    CHECK(control != NULL);
    GetWindowRect(control, &r); MapWindowPoints(NULL, page, (POINT *)&r, 2);
    GetClientRect(page, &parent);
    CHECK(r.left >= 0 && r.top >= 0 && r.right <= parent.right && r.bottom <= parent.bottom);
    CHECK(r.right > r.left && r.bottom > r.top);
}

/* A window whose thread never pumps: the Applications tab reports it as
   not responding, which is what puts the notice in the status bar. */
typedef struct { HANDLE ready, stop; HWND hwnd; } HungWindow;
static DWORD WINAPI HungWindowThread(void *arg)
{
    HungWindow *f = (HungWindow *)arg;
    f->hwnd = CreateWindowExW(0, L"STATIC", L"Taskman workspace hang fixture",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, -30000, -30000, 120, 90,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetEvent(f->ready);
    WaitForSingleObject(f->stop, INFINITE);
    DestroyWindow(f->hwnd);
    return 0;
}

int main(void)
{
    WNDCLASSEXW cls = {0};
    INITCOMMONCONTROLSEX common = {sizeof(common), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    HWND hwnd, page, list, search;
    ULONG64 sequence;
    int before, i;
    int originalColumn;
    WCHAR query[64], summary[256];
    g_hInst = GetModuleHandleW(NULL);
    {
        RECT work = {0, 0, 1920, 1040}, window = {-10, -10, 1760, 1280};
        FitWindowRect(&window, &work);
        CHECK(window.left >= work.left && window.top >= work.top);
        CHECK(window.right <= work.right && window.bottom <= work.bottom);
        window = (RECT){LONG_MIN, LONG_MIN, LONG_MAX, LONG_MAX};
        FitWindowRect(&window, &work);
        CHECK(EqualRect(&window, &work));
    }
    InitDpiApi(); InitCommonControlsEx(&common);
    g_cfg.activeTab = TAB_PROCESSES; g_cfg.showAllUsers = TRUE;
    g_cfg.updateSpeed = SPEED_NORMAL;
    g_cfg.procTreeMode = TRUE;   /* mirrors the real registry default */
    cls.cbSize = sizeof(cls); cls.lpfnWndProc = MainWndProc; cls.hInstance = g_hInst;
    cls.lpszClassName = L"TaskmanWorkspaceTest"; cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    CHECK(RegisterClassExW(&cls) != 0);
    hwnd = CreateWindowExW(0, cls.lpszClassName, L"Taskman test fixture", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        -16000, -16000, 1180, 900, NULL, NULL, g_hInst, NULL);
    CHECK(hwnd != NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE); LayoutMain(); Pump(1800);
    page = TabProcesses()->hwnd; list = GetDlgItem(page, IDC_PROC_LIST); search = GetDlgItem(page, IDC_PROC_SEARCH);
    originalColumn = ListView_GetColumnWidth(list, 0);
    before = ListView_GetItemCount(list); CHECK(before > 0);
    /* Tree mode is on by default, so the model must have been built. */
    CHECK(g_cfg.procTreeMode);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_VIEW_PROCTREE, 0), 0);
    Pump(200);
    CHECK(!g_cfg.procTreeMode);                  /* toggled off */
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_VIEW_PROCTREE, 0), 0);
    Pump(200);
    CHECK(g_cfg.procTreeMode);                   /* and back on */
    {
        /* Proc_SelectPid's reveal path: re-entry through ProcSnapshot,
           recursion termination, and post-rebuild re-resolution of the
           display row. Only this fixture has a real s_list, so only this
           fixture can reach it -- the headless suite's s_list is NULL and
           Proc_SelectPid early-returns immediately there. Everything below
           is derived from the live process tree; the test skips itself
           when the running machine happens not to have any parent/child
           pair to work with, rather than asserting on a fabricated one. */
        DWORD childPid = 0, parentPid = 0;
        if (!ProcTest_FindChildWithParent(&childPid, &parentPid)) {
            fprintf(stderr, "SKIP workspace: no process with a live parent found; "
                             "cannot exercise Proc_SelectPid's reveal path\n");
        } else {
            int parentIndex = ListFindPid(list, parentPid);
            CHECK(parentIndex >= 0);
            if (parentIndex >= 0) {
                CHECK(ProcTest_IsVisiblePid(childPid));   /* expanded to start */
                ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(list, parentIndex,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                SendMessageW(list, WM_KEYDOWN, VK_LEFT, 0);   /* same path the UI uses */
                Pump(100);
                CHECK(!ProcTest_IsVisiblePid(childPid));      /* hidden under the collapsed parent */
                CHECK(ListFindPid(list, childPid) < 0);

                Proc_SelectPid(childPid);                     /* must return, not hang */
                Pump(100);
                CHECK(ProcTest_IsVisiblePid(childPid));
                {
                    int childIndex = ListFindPid(list, childPid);
                    CHECK(childIndex >= 0);
                    if (childIndex >= 0) {
                        UINT state = (UINT)ListView_GetItemState(list, childIndex,
                            LVIS_SELECTED | LVIS_FOCUSED);
                        CHECK(state == (LVIS_SELECTED | LVIS_FOCUSED));
                    }
                }
                CHECK(ProcTest_PendingPid() == 0);
            }
        }
    }
    /* Left and Right must not crash with a selection present. */
    ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(list, WM_KEYDOWN, VK_LEFT, 0); Pump(100);
    SendMessageW(list, WM_KEYDOWN, VK_RIGHT, 0); Pump(100);
    Capture(hwnd, L"tests/.build/workspace-tree.bmp");
    /* The rest of this fixture predates tree mode and asserts flat-list
       counts (e.g. exactly one row for a pid: search, with no ancestor
       context rows). Turn tree mode back off through the same UI path so
       those assertions still hold -- restoring what this block changed. */
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_VIEW_PROCTREE, 0), 0);
    Pump(200);
    CHECK(!g_cfg.procTreeMode);
    {
        MSG key = {0};
        HWND refresh = GetDlgItem(g_hDashboard, IDM_VIEW_REFRESH);
        SetFocus(refresh);
        key.hwnd = refresh; key.message = WM_KEYDOWN; key.wParam = VK_TAB;
        CHECK(HandleWorkspaceKey(&key));
        CHECK(GetFocus() == GetDlgItem(g_hDashboard, IDM_VIEW_TOGGLEPAUSE));
        key.hwnd = GetFocus(); CHECK(HandleWorkspaceKey(&key));
        CHECK(GetFocus() == GetDlgItem(g_hDashboard, IDM_FILE_NEWTASK));
        key.hwnd = GetFocus(); CHECK(HandleWorkspaceKey(&key));
        CHECK(GetFocus() != key.hwnd && IsChild(hwnd, GetFocus()));
    }
    CheckBounds(list, page); CheckBounds(GetDlgItem(page, IDC_PROC_DETAILS), page);
    {
        /* The inspector repaints every snapshot and its two buttons sit on
           top of it. Its painting must be clipped around them, which needs
           WS_CLIPSIBLINGS and both buttons above it in Z order; otherwise
           the card is drawn over the buttons until they are hovered. */
        HWND details = GetDlgItem(page, IDC_PROC_DETAILS), walk;
        BOOL seenOpen = FALSE, seenCopy = FALSE, detailsReached = FALSE;
        CHECK((GetWindowLongW(details, GWL_STYLE) & WS_CLIPSIBLINGS) != 0);
        for (walk = GetWindow(page, GW_CHILD); walk; walk = GetWindow(walk, GW_HWNDNEXT)) {
            if (walk == details) { detailsReached = TRUE; break; }
            if (walk == GetDlgItem(page, IDC_PROC_OPENLOCATION)) seenOpen = TRUE;
            if (walk == GetDlgItem(page, IDC_PROC_COPY)) seenCopy = TRUE;
        }
        CHECK(detailsReached && seenOpen && seenCopy);
    }
    StringCchPrintfW(query, ARRAYSIZE(query), L"pid:%lu", (unsigned long)GetCurrentProcessId());
    SetWindowTextW(search, query); CHECK(ListView_GetItemCount(list) == 1);
    ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    CHECK(IsWindowEnabled(GetDlgItem(page, IDC_PROC_COPY)));
    CHECK(!IsWindowEnabled(GetDlgItem(page, IDC_PROC_ENDPROCESS)));
    SendMessageW(page, WM_COMMAND, IDC_PROC_CLEAR, 0);
    /* Sampling stayed live during the tree checks, so processes may have
       started/exited since 'before'. Clearing must restore the full view,
       including this process, rather than retain the one-row PID filter. */
    CHECK(GetWindowTextLengthW(search) == 0);
    CHECK(ListView_GetItemCount(list) > 1);
    CHECK(ListFindPid(list, GetCurrentProcessId()) >= 0);
    App_ShowProcess(GetCurrentProcessId());
    Pump(60);
    Capture(hwnd, L"tests/.build/workspace-processes.bmp");
    SetWindowTextW(search, L"no-such-process-987654321");
    CHECK(ListView_GetItemCount(list) == 0);
    CHECK(!IsWindowEnabled(GetDlgItem(page, IDC_PROC_COPY)));
    GetWindowTextW(GetDlgItem(page, IDC_PROC_SUMMARY), summary, ARRAYSIZE(summary));
    CHECK(wcsstr(summary, L"No matches") != NULL);
    SendMessageW(hwnd, WM_COMMAND, IDM_PROC_FIND, 0); CHECK(GetFocus() == search);
    SendMessageW(page, WM_COMMAND, IDC_PROC_CLEAR, 0);
    SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_TOGGLEPAUSE, 0);
    CHECK(g_cfg.updateSpeed == SPEED_PAUSED); Pump(120);
    sequence = SysInfo_Lock()->sequence; SysInfo_Unlock(); Pump(1100);
    CHECK(SysInfo_Lock()->sequence == sequence); SysInfo_Unlock();
    SetWindowTextW(search, query); CHECK(ListView_GetItemCount(list) == 1);
    SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_REFRESH, 0); Pump(250);
    CHECK(SysInfo_Lock()->sequence > sequence); SysInfo_Unlock();
    SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_TOGGLEPAUSE, 0); CHECK(g_cfg.updateSpeed == SPEED_NORMAL);
    /* Nothing has opened the Sensors tab, so the collector must not have
       paid for a PDH query. */
    CHECK(!Gpu_IsEnabled());
    /* The nav bar used to keep its own copy of the tab names, indexed by
       TAB_COUNT. Assert the tab control is the single source, and drive a
       real paint so an out-of-bounds label read would fault here. */
    for (i = 0; i < TAB_COUNT; ++i) {
        WCHAR label[64];
        TCITEMW tab;
        ZeroMemory(&tab, sizeof(tab));
        tab.mask = TCIF_TEXT;
        tab.pszText = label;
        tab.cchTextMax = ARRAYSIZE(label);
        CHECK(TabCtrl_GetItem(App_TabControl(), i, &tab));
        CHECK(lstrcmpW(label, g_page[i]->title) == 0);
    }
    CHECK(TabCtrl_GetItemCount(App_TabControl()) == TAB_COUNT);
    {
        HDC screen = GetDC(NULL);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 1180, 64);
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
        UI_DrawNavigation(App_TabControl(), memory);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(NULL, screen);
    }
    for (i = 0; i < TAB_COUNT; ++i) {
        SwitchToTab(i, FALSE); Pump(120);
        CHECK(IsWindowVisible(g_page[i]->hwnd));
        CheckBounds(g_page[i]->hwnd, hwnd);
    }
    /* Activating the tab turns collection on; leaving it turns it back off,
       so an unopened tab costs nothing and a closed one stops costing. */
    SwitchToTab(TAB_SENSORS, FALSE); Pump(120);
    CHECK(Gpu_IsEnabled());
    {
        /* The first sample is discarded -- rate counters need two
           collections -- and the interval is a second, so the list cannot
           have populated yet. Wait past that and check the page actually
           renders the model, rather than only that the gate opened. A
           machine with no GPU counters at all is a supported case, so the
           row count is checked against the model, not against a constant. */
        const GpuAdapter *model;
        int adapters = 0;
        Pump(2600);
        model = Gpu_Lock(&adapters); (void)model; Gpu_Unlock();
        if (adapters > 0) {
            HWND sensors = GetDlgItem(TabSensors()->hwnd, IDC_SENS_LIST);
            WCHAR name[128] = {0};
            CHECK(ListView_GetItemCount(sensors) == adapters);
            ListView_GetItemText(sensors, 0, 0, name, ARRAYSIZE(name));
            CHECK(name[0] != L'\0');
            fprintf(stderr, "workspace: %d GPU adapter(s), first = %ls\n",
                    adapters, name);
        } else {
            fprintf(stderr, "SKIP workspace: no GPU performance counters; "
                            "the Sensors list cannot be exercised\n");
        }
    }
    {
        /* The temperature list is populated by the collector rather than by
           the tab, so it is checked against the model the same way. When
           unelevated the ACPI explanatory row is always appended, so the
           list is never completely empty on an ordinary run. */
        HWND temps = GetDlgItem(TabSensors()->hwnd, IDC_SENS_TEMPLIST);
        const SensorReading *model;
        int drives = 0;
        CHECK(temps != NULL);
        CheckBounds(temps, TabSensors()->hwnd);
        model = Sensors_Lock(&drives); (void)model; Sensors_Unlock();
        if (drives > 0) {
            WCHAR name[128] = {0};
            ListView_GetItemText(temps, 0, 0, name, ARRAYSIZE(name));
            CHECK(name[0] != L'\0');
            fprintf(stderr, "workspace: %d temperature source(s), first = %ls\n",
                    drives, name);
        }
        if (!Sensors_IsElevated())
            CHECK(ListView_GetItemCount(temps) == drives + 1);
        else
            CHECK(ListView_GetItemCount(temps) >= drives);
    }
    Capture(hwnd, L"tests/.build/workspace-sensors.bmp");
    SwitchToTab(TAB_PROCESSES, FALSE); Pump(60);
    CHECK(!Gpu_IsEnabled());
    {
        /* The left status part calls out hung applications on every tab. */
        WCHAR status[160];
        FormatStatusLeft(status, ARRAYSIZE(status), FALSE, 42, 0);
        CHECK(!lstrcmpW(status, L"  LIVE   |   42 processes   |   F5 refresh   Ctrl+F search"));
        FormatStatusLeft(status, ARRAYSIZE(status), TRUE, 42, 1);
        CHECK(!lstrcmpW(status, L"  PAUSED   |   42 processes   |   1 app not responding (click to view)"));
        FormatStatusLeft(status, ARRAYSIZE(status), FALSE, 7, 3);
        CHECK(!lstrcmpW(status, L"  LIVE   |   7 processes   |   3 apps not responding (click to view)"));
    }
    {
        /* A hung window is called out in the status bar on every tab, and
           clicking that part opens the Applications tab. The click cannot be
           driven through the screen here: capturing the desktop stalls while
           a hung window is on it, so the notification the status bar would
           send is posted directly. */
        HungWindow hung = {0};
        HANDLE thread;
        hung.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        hung.stop  = CreateEventW(NULL, TRUE, FALSE, NULL);
        thread = CreateThread(NULL, 0, HungWindowThread, &hung, 0, NULL);
        CHECK(thread != NULL);
        CHECK(WaitForSingleObject(hung.ready, 2000) == WAIT_OBJECT_0);
        /* Leave the Applications tab first: the watch that runs on other
           tabs trusts IsHungAppWindow alone, which needs five seconds, so
           it would clear a hang this young before the click arrives. */
        SwitchToTab(TAB_PERFORMANCE, FALSE); Pump(60);
        CHECK(g_active == TAB_PERFORMANCE);
        /* Two collections: one slow probe alone is not a hang. */
        Apps_Collect();
        Apps_Collect();
        CHECK(Apps_HungCount() > 0);
        {
            WCHAR status[160];
            FormatStatusLeft(status, ARRAYSIZE(status), FALSE, 1, Apps_HungCount());
            CHECK(wcsstr(status, L"not responding (click to view)") != NULL);
        }
        {
            NMMOUSE click;
            ZeroMemory(&click, sizeof(click));
            click.hdr.hwndFrom = g_hStatus;
            click.hdr.idFrom = 0;
            click.hdr.code = NM_CLICK;
            click.dwItemSpec = 0;                  /* the left part */
            SendMessageW(hwnd, WM_NOTIFY, 0, (LPARAM)&click);
            Pump(60);
            CHECK(g_active == TAB_APPS);
            /* Any other part is not the notice and must not switch tabs. */
            SwitchToTab(TAB_PERFORMANCE, FALSE);
            Apps_Collect(); Apps_Collect();
            click.dwItemSpec = 2;
            SendMessageW(hwnd, WM_NOTIFY, 0, (LPARAM)&click);
            Pump(60);
            CHECK(g_active == TAB_PERFORMANCE);
        }
        SetEvent(hung.stop);
        CHECK(WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0);
        CloseHandle(thread); CloseHandle(hung.ready); CloseHandle(hung.stop);
        Apps_Collect();
    }
    {
        /* File > Startup Impact opens one modeless window, fills it with every
           startup entry this machine has, and a second request reuses it. */
        static StartupEntry entries[STARTUP_MAX_ENTRIES];
        HWND first, second;
        int expected = Startup_ReadEntries(entries, STARTUP_MAX_ENTRIES);
        CHECK(StartupTest_RowCount() == -1);
        SendMessageW(hwnd, WM_COMMAND, IDM_FILE_STARTUP, 0);
        Pump(60);
        first = FindWindowW(L"ClassicTaskManagerStartupWnd", NULL);
        CHECK(first != NULL && GetWindow(first, GW_OWNER) == hwnd);
        CHECK(StartupTest_RowCount() == expected);
        second = App_ShowStartupImpact(hwnd);
        CHECK(second == first);
        if (first) SendMessageW(first, WM_COMMAND, IDCANCEL, 0);
        Pump(60);
        CHECK(!IsWindow(first));
        CHECK(StartupTest_RowCount() == -1);
    }
    SwitchToTab(TAB_PERFORMANCE, FALSE); Pump(60);
    Capture(hwnd, L"tests/.build/workspace-performance.bmp");
    {
        /* Spike Blame: hover and pin draw over the graphs, and a click on
           the memory graph jumps to its biggest culprit still running. The
           memory list is used because it is never empty, unlike CPU on an
           idle machine. */
        HWND memGraph = GetDlgItem(TabPerformance()->hwnd, IDC_PERF_MEMHISTORY);
        HWND cpuGraph = GetDlgItem(TabPerformance()->hwnd, IDC_PERF_CPUHISTORY);
        BlameSample latest;
        RECT client;
        LPARAM right;
        Pump(1500);
        CHECK(Blame_Copy(&latest, 1) == 1);
        CHECK(latest.memCount > 0);
        CHECK(memGraph && cpuGraph);
        SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_BLAME_PEAK, 0);
        Pump(60);
        CHECK(g_active == TAB_PERFORMANCE);
        Capture(hwnd, L"tests/.build/workspace-blame-pin.bmp");

        GetClientRect(memGraph, &client);
        right = MAKELPARAM(client.right - 2, client.bottom / 2);
        /* No hover capture: the fixture sits off screen, so TrackMouseEvent
           reports the leave at once and the overlay is gone before a
           capture could see it. The click path does not depend on hover. */
        SendMessageW(memGraph, WM_MOUSEMOVE, 0, right);
        SendMessageW(memGraph, WM_LBUTTONUP, 0, right);
        Pump(60);
        CHECK(g_active == TAB_PROCESSES);
        SwitchToTab(TAB_PERFORMANCE, FALSE); Pump(60);

        /* Per-CPU grid: Ctrl+B pins without leaving the grid, and a click
           on the gutter between two cells names no sample, so it must not
           switch tabs. */
        g_cfg.perfOneGraphPerCpu = TRUE;
        InvalidateRect(cpuGraph, NULL, FALSE);
        SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_BLAME_PEAK, 0);
        Pump(60);
        CHECK(g_cfg.perfOneGraphPerCpu);
        Capture(hwnd, L"tests/.build/workspace-blame-grid.bmp");
        {
            UINT cpus = SysInfo_CpuHistoryCount();
            GetClientRect(cpuGraph, &client);
            if (cpus > 1) {
                RECT cell;
                LPARAM gutter;
                PerfTest_CellRect(&client, cpus, 0, &cell);
                gutter = MAKELPARAM(cell.right, cell.top + 1);   /* right is exclusive */
                CHECK(PerfTest_CellAt(&client, cpus, cell.left + 1, cell.top + 1) == 0);
                CHECK(PerfTest_CellAt(&client, cpus, cell.right, cell.top + 1) == -1);
                CHECK(PerfTest_CellAt(&client, cpus, -1, 0) == -1);
                SendMessageW(cpuGraph, WM_MOUSEMOVE, 0, gutter);
                SendMessageW(cpuGraph, WM_LBUTTONUP, 0, gutter);
                Pump(60);
                CHECK(g_active == TAB_PERFORMANCE);
            }
        }
        g_cfg.perfOneGraphPerCpu = FALSE;
    }
    SwitchToTab(TAB_PROCESSES, FALSE);
    /* Clear the search before the GPU column check: a filtered list can be
       a single process that genuinely uses no GPU, which would make the
       "some row has a figure" assertion below fail for the wrong reason. */
    SendMessageW(page, WM_COMMAND, IDC_PROC_CLEAR, 0);
    Pump(120);
    {
        /* The column ships hidden, and showing it is what turns GPU
           collection on. Driven through WM_COMMAND rather than the popup
           menu, which would block on TrackPopupMenu. */
        const int gpuColumn = 6;
        CHECK(Header_GetItemCount(ListView_GetHeader(list)) >= 7);
        CHECK(ListView_GetColumnWidth(list, gpuColumn) == 0);
        CHECK(!Gpu_IsEnabled());

        SendMessageW(page, WM_COMMAND, IDM_PROC_COL_FIRST + gpuColumn, 0);
        Pump(120);
        CHECK(ListView_GetColumnWidth(list, gpuColumn) > 0);
        CHECK(Gpu_IsEnabled());

        {
            /* Wait out the discarded first sample and the one second
               interval, then confirm the column actually renders figures
               rather than only that the toggle worked. Rows are stamped
               unknown until a sample lands, so an empty column here would
               mean the stamp never reached them. */
            int r, populated = 0, rows;
            Pump(2600);
            SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_REFRESH, 0);
            Pump(400);
            rows = ListView_GetItemCount(list);
            /* Read what the control renders rather than the model: this
               also proves the display case is wired to the right column. */
            for (r = 0; r < rows; ++r) {
                WCHAR cell[32] = {0};
                ListView_GetItemText(list, r, gpuColumn, cell, ARRAYSIZE(cell));
                if (cell[0]) ++populated;
            }
            fprintf(stderr, "workspace: %d of %d rows render a GPU figure\n",
                    populated, rows);
            CHECK(populated > 0);
            Capture(hwnd, L"tests/.build/workspace-gpu-column.bmp");
        }

        {
            /* The menu the user actually right-clicks. Built without being
               shown, because TrackPopupMenu blocks on input and can never
               run here -- which is what left this construction uncovered.
               Every entry must match the header it came from, so a column
               added without touching the menu shows up as a mismatch here
               rather than as a missing or mislabelled item at runtime. */
            HMENU menu = ProcTest_BuildColumnMenu();
            int columns = Header_GetItemCount(ListView_GetHeader(list));
            CHECK(menu != NULL);
            if (menu) {
                int item, items = GetMenuItemCount(menu);
                /* Column 0 is deliberately not offered: the process name
                   cannot be hidden. */
                CHECK(items == columns - 1);
                for (item = 0; item < items; ++item) {
                    WCHAR menuText[64] = {0}, headerText[64] = {0};
                    LVCOLUMNW column;
                    MENUITEMINFOW info;
                    int source = item + 1;

                    ZeroMemory(&info, sizeof(info));
                    info.cbSize = sizeof(info);
                    info.fMask = MIIM_STRING | MIIM_ID | MIIM_STATE;
                    info.dwTypeData = menuText;
                    info.cch = ARRAYSIZE(menuText);
                    CHECK(GetMenuItemInfoW(menu, item, TRUE, &info));

                    ZeroMemory(&column, sizeof(column));
                    column.mask = LVCF_TEXT;
                    column.pszText = headerText;
                    column.cchTextMax = ARRAYSIZE(headerText);
                    CHECK(ListView_GetColumn(list, source, &column));

                    CHECK(lstrcmpW(menuText, headerText) == 0);
                    CHECK(info.wID == (UINT)(IDM_PROC_COL_FIRST + source));
                    /* The tick mirrors visibility, which is column width. */
                    CHECK(((info.fState & MFS_CHECKED) != 0) ==
                          (ListView_GetColumnWidth(list, source) != 0));
                }
                DestroyMenu(menu);
            }
        }

        SendMessageW(page, WM_COMMAND, IDM_PROC_COL_FIRST + gpuColumn, 0);
        Pump(120);
        CHECK(ListView_GetColumnWidth(list, gpuColumn) == 0);
        CHECK(!Gpu_IsEnabled());
    }
    SendMessageW(page, WM_COMMAND, IDC_PROC_CLEAR, 0);
    SetWindowPos(hwnd, NULL, 0, 0, 1180, 720, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutMain(); Pump(30);
    {
        RECT tableRect, inspectorRect;
        GetWindowRect(list, &tableRect);
        GetWindowRect(GetDlgItem(page, IDC_PROC_DETAILS), &inspectorRect);
        CHECK(inspectorRect.top >= tableRect.bottom); /* Short windows need the bottom inspector. */
    }
    App_ShowProcess(GetCurrentProcessId());
    {
        /* Services -> "Go to Process" reaches a service running as SYSTEM
           while the user filter is on, which every other test here misses
           because they all set showAllUsers. The one-tick s_pendingPid
           handoff is what exempts that row from the filter for the single
           snapshot that reveals it; clearing it too eagerly would make the
           command silently do nothing, which is how it would regress. */
        BOOL savedAllUsers = g_cfg.showAllUsers;
        g_cfg.showAllUsers = FALSE;
        TabProcesses()->OnSnapshot(TabProcesses()); Pump(60);
        CHECK(ListFindPid(list, 4) < 0);      /* System is filtered out */

        App_ShowProcess(4);                    /* the Services tab's path */
        Pump(120);
        /* The exemption is consumed by the next snapshot rather than by the
           command itself: App_ShowProcess only records the pending pid. */
        TabProcesses()->OnSnapshot(TabProcesses()); Pump(60);
        CHECK(ListFindPid(list, 4) >= 0);      /* exempted and revealed */
        {
            int shown = ListFindPid(list, 4);
            if (shown >= 0) {
                UINT state = (UINT)ListView_GetItemState(list, shown,
                    LVIS_SELECTED | LVIS_FOCUSED);
                CHECK(state == (LVIS_SELECTED | LVIS_FOCUSED));
            }
        }
        /* The pending request is consumed once it has been honoured, so it
           cannot exempt the pid forever. */
        CHECK(ProcTest_PendingPid() == 0);

        /* The row nonetheless stays while it is the selection: the filter
           exempts the selected row too, so a process the user was just sent
           to does not vanish from under them on the next refresh. */
        TabProcesses()->OnSnapshot(TabProcesses()); Pump(60);
        CHECK(ListFindPid(list, 4) >= 0);

        /* Only once the selection moves elsewhere does the filter reclaim
           it -- that is what bounds the exemption. */
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        {
            /* Any row but System itself: with CPU sorting, System is often
               row 0, and selecting it would keep the exemption alive. */
            int other = ListPidAt(list, 0) == 4 ? 1 : 0;
            CHECK(ListPidAt(list, other) != 4);
            ListView_SetItemState(list, other, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
        TabProcesses()->OnSnapshot(TabProcesses()); Pump(60);
        CHECK(ListFindPid(list, 4) < 0);

        g_cfg.showAllUsers = savedAllUsers;
        TabProcesses()->OnSnapshot(TabProcesses()); Pump(60);
    }
    Capture(hwnd, L"tests/.build/workspace-short-wide.bmp");
    SetWindowPos(hwnd, NULL, 0, 0, 840, 720, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutMain(); Pump(60);
    CheckBounds(list, page); CheckBounds(GetDlgItem(page, IDC_PROC_DETAILS), page);
    Capture(hwnd, L"tests/.build/workspace-compact.bmp");
    ToggleTinyFootprint(); CHECK(!IsWindowVisible(g_hDashboard));
    CheckBounds(list, page);
    ToggleTinyFootprint(); CHECK(IsWindowVisible(g_hDashboard));
    /* Exercise scaled fonts and layout without changing the user's desktop DPI. */
    ShowWindow(hwnd, SW_HIDE); /* DPI fitting may move the fixture onto a real monitor. */
    for (i = 144; i <= 192; i += 48) {
        RECT target = {-16000, -16000, -16000 + MulDiv(1180, i, 96), -16000 + MulDiv(900, i, 96)};
        SendMessageW(hwnd, WM_DPICHANGED, MAKELONG(i, i), (LPARAM)&target);
        {
            RECT work = {0}, actual;
            MINMAXINFO limits = {0};
            CHECK(WindowWorkArea(hwnd, NULL, &work)); GetWindowRect(hwnd, &actual);
            SendMessageW(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&limits);
            CHECK(limits.ptMinTrackSize.x <= work.right - work.left);
            CHECK(limits.ptMinTrackSize.y <= work.bottom - work.top);
            CHECK(actual.left >= work.left && actual.top >= work.top && actual.right <= work.right && actual.bottom <= work.bottom);
        }
        CheckBounds(list, page); CheckBounds(GetDlgItem(page, IDC_PROC_DETAILS), page);
        CHECK(abs(ListView_GetColumnWidth(list, 0) - MulDiv(originalColumn, i, 96)) <= 2);
        Pump(50);
    }
    Capture(hwnd, L"tests/.build/workspace-200dpi.bmp");
    /* Compact fallback also fits a 1080p work area at 200% scaling. */
    TabProcesses()->OnLayout(TabProcesses(), DPX(800), DPX(260), FALSE);
    {
        RECT actual;
        GetWindowRect(list, &actual); MapWindowPoints(NULL, page, (POINT *)&actual, 2);
        CHECK(actual.top >= 0 && actual.bottom <= DPX(260));
        CHECK(!(GetWindowLongW(GetDlgItem(page, IDC_PROC_DETAILS), GWL_STYLE) & WS_VISIBLE));
    }
    DestroyWindow(hwnd); Pump(10);
    printf("workspace: %d failures; search, selection, pause, refresh, seven tabs, resize, tiny mode and DPI\n", failures);
    return failures ? 1 : 0;
}
