/* ------------------------------------------------------------------------
 * tab_sensors.c - the Sensors tab: GPU adapters now, temperatures later.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ui.h"
#include "gpu.h"
#include <stdlib.h>

static HWND s_graphHost, s_list;

/* ----------------------------------------------------------------- model -- */

static GpuAdapter s_view[GPU_MAX_ADAPTERS];
static int s_viewCount;
static ULONGLONG s_selected;
static int s_column, s_direction = 1;
static BOOL s_refreshing;

static int sens_compare(const void *left, const void *right)
{
    const GpuAdapter *a = left, *b = right;
    int result;
    if (s_column == 1)
        result = (a->utilization > b->utilization) - (a->utilization < b->utilization);
    else if (s_column == 2)
        result = (a->dedicatedUsed > b->dedicatedUsed) - (a->dedicatedUsed < b->dedicatedUsed);
    else if (s_column == 3)
        result = (a->dedicatedTotal > b->dedicatedTotal) - (a->dedicatedTotal < b->dedicatedTotal);
    else
        result = lstrcmpiW(a->name, b->name);
    /* LUID breaks ties so the order is stable across refreshes and rows do
       not swap under the user's cursor. */
    if (!result) result = (a->luid > b->luid) - (a->luid < b->luid);
    return result * s_direction;
}

/* ----------------------------------------------------------------- UI ---- */

static LRESULT CALLBACK SensGraphSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if (msg == WM_ERASEBKGND) return TRUE;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, SensGraphSubclass, id);
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, graph;
        const GpuAdapter *adapter = NULL;
        WCHAR title[256];
        float samples[GPU_HISTORY];
        int count = 0, i;

        GetClientRect(hwnd, &rc);
        UI_Fill(dc, &rc, UI_SURFACE);
        for (i = 0; i < s_viewCount; ++i)
            if (s_view[i].luid == s_selected) { adapter = &s_view[i]; break; }

        StringCchPrintfW(title, ARRAYSIZE(title), L"%s - GPU utilization (0-100%%)",
                         adapter ? adapter->name : L"No adapter selected");
        SetTextColor(dc, UI_INK); SetBkMode(dc, TRANSPARENT);
        graph = rc; graph.left += DPX(14); graph.right -= DPX(14); graph.top += DPX(10);
        {
            /* g_hFont is deleted and recreated on a DPI or font change;
               leaving a deleted object selected into a DC is undefined. */
            HGDIOBJ oldFont = SelectObject(dc, g_hFont);
            DrawTextW(dc, title, -1, &graph, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (oldFont) SelectObject(dc, oldFont);
        }
        graph.top += DPX(28); graph.bottom -= DPX(10);
        if (adapter) count = Gpu_History(adapter, samples, GPU_HISTORY);
        UI_Chart(dc, graph, count ? samples : NULL, count, UI_TEAL, TRUE);

        EndPaint(hwnd, &ps); return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void SensCreate(TabPage *p)
{
    s_graphHost = UI_CreateStatic(p->hwnd, IDC_SENS_GRAPHHOST, L"", SS_BLACKRECT);
    if (s_graphHost)
        SetWindowSubclass(s_graphHost, SensGraphSubclass, 1, 0);

    s_list = UI_CreateListView(p->hwnd, IDC_SENS_LIST, 0);
    if (s_list) {
        UI_AddColumn(s_list, 0, L"Adapter",           140, LVCFMT_LEFT);
        UI_AddColumn(s_list, 1, L"GPU Utilization",    72, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 2, L"Dedicated Used",     72, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 3, L"Dedicated Total",    72, LVCFMT_RIGHT);
    }
}

static void SensLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    int margin = UI_Margin(p->hwnd);
    int graphH;

    /* Tiny footprint keeps the graph only, matching Performance and
       Networking. */
    if (tiny) {
        if (s_list) ShowWindow(s_list, SW_HIDE);
        if (s_graphHost) {
            ShowWindow(s_graphHost, SW_SHOW);
            MoveWindow(s_graphHost, 0, 0, cx, cy, TRUE);
        }
        return;
    }
    if (s_list) ShowWindow(s_list, SW_SHOW);

    graphH = (cy - 3 * margin) * 2 / 3;
    if (graphH < DPX(60)) graphH = DPX(60);
    if (graphH > cy - 3 * margin - DPX(60)) {
        graphH = cy - 3 * margin - DPX(60);
        if (graphH < DPX(40)) graphH = DPX(40);
    }

    if (s_graphHost)
        MoveWindow(s_graphHost, margin, margin,
                   cx - 2 * margin > 0 ? cx - 2 * margin : 1, graphH, TRUE);
    if (s_list)
        MoveWindow(s_list, margin, margin + graphH + margin,
                   cx - 2 * margin > 0 ? cx - 2 * margin : 1,
                   cy - margin - (margin + graphH + margin) > 0
                       ? cy - margin - (margin + graphH + margin) : 1, TRUE);
}

static void SensSnapshot(TabPage *p)
{
    const GpuAdapter *model;
    int count = 0, i;
    BOOL found = FALSE;

    (void)p;
    model = Gpu_Lock(&count);
    if (count > GPU_MAX_ADAPTERS) count = GPU_MAX_ADAPTERS;
    if (count > 0) CopyMemory(s_view, model, (size_t)count * sizeof(*s_view));
    Gpu_Unlock();
    s_viewCount = count;

    if (count > 1) qsort(s_view, (size_t)count, sizeof(*s_view), sens_compare);
    for (i = 0; i < count; ++i) if (s_view[i].luid == s_selected) found = TRUE;
    if (!found) s_selected = count ? s_view[0].luid : 0;

    if (s_graphHost) InvalidateRect(s_graphHost, NULL, FALSE);
    if (!s_list) return;

    s_refreshing = TRUE;
    SendMessageW(s_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < count; ++i) {
        WCHAR util[32], used[48], total[48];
        LVITEMW item = {0};
        StringCchPrintfW(util, ARRAYSIZE(util), L"%.1f%%", s_view[i].utilization);
        UI_FormatSize(s_view[i].dedicatedUsed, used, ARRAYSIZE(used));
        if (s_view[i].dedicatedTotal)
            UI_FormatSize(s_view[i].dedicatedTotal, total, ARRAYSIZE(total));
        else
            lstrcpyW(total, L"Unknown");   /* no DXGI: figures still shown */
        item.mask = LVIF_TEXT; item.iItem = i; item.pszText = s_view[i].name;
        ListView_InsertItem(s_list, &item);
        ListView_SetItemText(s_list, i, 1, util);
        ListView_SetItemText(s_list, i, 2, used);
        ListView_SetItemText(s_list, i, 3, total);
        if (s_view[i].luid == s_selected)
            ListView_SetItemState(s_list, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (!count) {
        /* An empty list with no explanation reads as a broken tab. A machine
           with no GPU performance counters -- pre-1709, or the provider
           disabled -- is a supported case, not an error worth a dialog. */
        LVITEMW item = {0};
        item.mask = LVIF_TEXT; item.iItem = 0;
        item.pszText = UI_Str(L"No GPU performance counters available");
        ListView_InsertItem(s_list, &item);
    }
    UI_SetHeaderSortArrow(s_list, s_column, s_direction);
    SendMessageW(s_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_list, NULL, TRUE);
    s_refreshing = FALSE;
}

static BOOL sens_notify(TabPage *p, NMHDR *nm, LRESULT *result)
{
    if (nm->hwndFrom != s_list || s_refreshing) return FALSE;
    if (nm->code == LVN_COLUMNCLICK) {
        int column = ((NMLISTVIEW *)nm)->iSubItem;
        s_direction = column == s_column ? -s_direction : 1;
        s_column = column; SensSnapshot(p); *result = 0; return TRUE;
    }
    if (nm->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *change = (NMLISTVIEW *)nm;
        if ((change->uNewState & LVIS_SELECTED) &&
            change->iItem >= 0 && change->iItem < s_viewCount) {
            s_selected = s_view[change->iItem].luid;
            if (s_graphHost) InvalidateRect(s_graphHost, NULL, FALSE);
        }
    }
    return FALSE;
}

/* Collection is gated on this tab so that a user who never opens it never
   pays for the PDH query -- around a thousand engine instances on a machine
   with three adapters. Unit 4's per-process GPU column will enable it
   independently; when that lands, this must stop being the only owner of
   the flag. */
static void SensActivate(TabPage *p, BOOL active)
{
    (void)p;
    Gpu_SetEnabled(active);
}

static HWND SensPrimary(TabPage *p)
{
    (void)p;
    return s_graphHost;
}

static void SensDestroy(TabPage *p)
{
    (void)p;
    s_graphHost = NULL;
    s_list = NULL;
}

static TabPage s_page = {
    L"Sensors", NULL, TAB_SENSORS,
    SensCreate, SensDestroy, SensLayout,
    SensSnapshot, NULL, sens_notify, SensActivate, NULL, NULL,
    NULL, NULL,
    SensPrimary
};

TabPage *TabSensors(void) { return &s_page; }
