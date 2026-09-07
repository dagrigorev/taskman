/* ------------------------------------------------------------------------
 * tab_network.c - the Networking tab.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ui.h"
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>

#ifndef IF_TYPE_SOFTWARE_LOOPBACK
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#endif

/* ----------------------------------------------------------------- model -- */

#define NET_HIST_MAX 128
#define NET_NAME_MAX 257

typedef struct {
    WCHAR name[NET_NAME_MAX];
    ULONGLONG luid, speedBps, receiveSpeed, bytesIn, bytesOut, tick;
    double receiveRate, sendRate; /* bytes per second over the sample interval */
    BOOL connected;
    float history[3][NET_HIST_MAX]; /* total, sent, received percent */
    int head, count;
} NetRow;
static NetRow *g_shared, *g_view;
static int g_sharedCnt, g_viewCnt;
static SRWLOCK g_netLock = SRWLOCK_INIT;
static DWORD net_error, net_reported;
static int net_column, net_direction = 1;
static ULONGLONG net_selected;
static BOOL net_refreshing;
static HWND s_graphHost, s_list;

static float net_percent(ULONGLONG current, ULONGLONG previous, ULONGLONG speed, ULONGLONG elapsed)
{
    double value;
    if (current < previous || !speed || !elapsed) return 0;
    value = (double)(current - previous) * 800000.0 / ((double)speed * (double)elapsed);
    return value > 100.0 ? 100.0f : (float)value;
}

static void net_sample(NetRow *row, const NetRow *previous)
{
    float sent = 0, received = 0;
    row->receiveRate = row->sendRate = 0;
    if (previous) {
        memcpy(row->history, previous->history, sizeof(row->history));
        row->head = previous->head; row->count = previous->count;
        if (row->connected && previous->connected && row->tick > previous->tick &&
            row->speedBps == previous->speedBps && row->receiveSpeed == previous->receiveSpeed &&
            row->bytesIn >= previous->bytesIn && row->bytesOut >= previous->bytesOut) {
            sent = net_percent(row->bytesOut, previous->bytesOut, row->speedBps, row->tick - previous->tick);
            received = net_percent(row->bytesIn, previous->bytesIn, row->receiveSpeed, row->tick - previous->tick);
            row->receiveRate = (double)(row->bytesIn - previous->bytesIn) * 1000.0 / (double)(row->tick - previous->tick);
            row->sendRate = (double)(row->bytesOut - previous->bytesOut) * 1000.0 / (double)(row->tick - previous->tick);
        }
    }
    row->history[0][row->head] = sent + received > 100 ? 100 : sent + received;
    row->history[1][row->head] = sent;
    row->history[2][row->head] = received;
    row->head = (row->head + 1) % NET_HIST_MAX;
    if (row->count < NET_HIST_MAX) ++row->count;
}

static float net_history(const NetRow *row, int mode, int index)
{
    if (mode < 0 || mode > 2) mode = 0;
    if (index < 0 || index >= row->count) return 0;
    return row->history[mode][(row->head - row->count + index + NET_HIST_MAX) % NET_HIST_MAX];
}

static void net_format_rate(double rate, WCHAR *text, size_t capacity)
{
    static const WCHAR *units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s", L"PB/s"};
    int unit = 0;
    while (rate >= 1024.0 && unit < 5) { rate /= 1024.0; ++unit; }
    StringCchPrintfW(text, capacity, L"%.1f %s", rate, units[unit]);
}

void Net_Collect(void)
{
    MIB_IF_TABLE2 *table = NULL;
    DWORD error = GetIfTable2(&table);
    NetRow *rows = NULL, *old = NULL;
    int count = 0;
    ULONG entries = 0;
    ULONGLONG tick = GetTickCount64();
    if (error) goto publish;
    entries = table->NumEntries;
    if (entries) {
        rows = calloc(entries, sizeof(*rows));
        if (!rows) { error = ERROR_NOT_ENOUGH_MEMORY; goto publish; }
    }
    /* Single collector thread owns previous model changes. The UI only reads
       under this shared lock; all data copied into the next model is private. */
    AcquireSRWLockShared(&g_netLock);
    for (ULONG i = 0; i < entries; ++i) {
        MIB_IF_ROW2 *source = &table->Table[i];
        NetRow *row;
        const NetRow *previous = NULL;
        if (source->Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        row = &rows[count++];
        row->luid = source->InterfaceLuid.Value;
        lstrcpynW(row->name, source->Alias[0] ? source->Alias : source->Description, NET_NAME_MAX);
        row->speedBps = source->TransmitLinkSpeed; row->receiveSpeed = source->ReceiveLinkSpeed;
        row->bytesIn = source->InOctets; row->bytesOut = source->OutOctets;
        row->connected = source->OperStatus == IfOperStatusUp;
        row->tick = tick;
        for (int j = 0; j < g_sharedCnt; ++j)
            if (g_shared[j].luid == row->luid) { previous = &g_shared[j]; break; }
        net_sample(row, previous);
    }
    ReleaseSRWLockShared(&g_netLock);
publish:
    if (table) FreeMibTable(table);
    AcquireSRWLockExclusive(&g_netLock);
    net_error = error;
    if (!error) { old = g_shared; g_shared = rows; g_sharedCnt = count; rows = NULL; }
    ReleaseSRWLockExclusive(&g_netLock);
    free(rows); free(old);
}

void Net_Reset(void)
{
    AcquireSRWLockExclusive(&g_netLock);
    free(g_shared); g_shared = NULL; g_sharedCnt = 0;
    net_error = 0;
    ReleaseSRWLockExclusive(&g_netLock);
    free(g_view); g_view = NULL; g_viewCnt = 0; net_selected = 0;
}

static LRESULT CALLBACK NetGraphSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                         UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if (msg == WM_ERASEBKGND) return TRUE;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, NetGraphSubclass, id);
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, graph;
        HPEN grid, old;
        const NetRow *row = NULL;
        WCHAR title[384];
        int mode = g_cfg.netHistoryMode;
        GetClientRect(hwnd, &rc);
        UI_Fill(dc, &rc, UI_SURFACE);
        for (int i = 0; i < g_viewCnt; ++i) if (g_view[i].luid == net_selected) { row = &g_view[i]; break; }
        StringCchPrintfW(title, ARRAYSIZE(title), L"%s - %s utilization (0-100%%)",
            row ? row->name : L"No adapter selected", mode == 1 ? L"Sent" : mode == 2 ? L"Received" : L"Total");
        SetTextColor(dc, UI_INK); SetBkMode(dc, TRANSPARENT);
        graph = rc; graph.left += DPX(14); graph.right -= DPX(14); graph.top += DPX(10);
        {
            /* Restore the previous font once the caption is drawn: g_hFont is
               deleted and recreated on a DPI or font change, and leaving a
               deleted object selected into a DC is undefined. */
            HGDIOBJ oldFont = SelectObject(dc, g_hFont);
            DrawTextW(dc, title, -1, &graph, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (oldFont) SelectObject(dc, oldFont);
        }
        graph.top += DPX(28); graph.bottom -= DPX(10);
        grid = CreatePen(PS_SOLID, 1, UI_LINE);
        old = SelectObject(dc, grid);
        for (int i = 0; i <= 4; ++i) {
            int y = graph.bottom - (graph.bottom - graph.top) * i / 4;
            MoveToEx(dc, graph.left, y, NULL); LineTo(dc, graph.right, y);
        }
        for (int i = 0; i <= 8; ++i) {
            int x = graph.left + (graph.right - graph.left) * i / 8;
            MoveToEx(dc, x, graph.top, NULL); LineTo(dc, x, graph.bottom);
        }
        SelectObject(dc, old); DeleteObject(grid);
        if (row && row->count > 0 && graph.right > graph.left && graph.bottom > graph.top) {
            /* Collected into one array so the trace can be stroked in a
               single antialiased pass instead of segment by segment. */
            POINT trace[NET_HIST_MAX];
            int used = row->count > NET_HIST_MAX ? NET_HIST_MAX : row->count;
            for (int i = 0; i < used; ++i) {
                float value = net_history(row, mode, i);
                trace[i].x = graph.right - (used - 1 - i) * (graph.right - graph.left) / (NET_HIST_MAX - 1);
                trace[i].y = graph.bottom - (int)(value * (float)(graph.bottom - graph.top) / 100.0f);
            }
            UI_Polyline(dc, trace, used, UI_TEAL, 2);
        }
        EndPaint(hwnd, &ps); return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ----------------------------------------------------------------- UI ---- */

static void NetCreate(TabPage *p)
{
    s_graphHost = UI_CreateStatic(p->hwnd, IDC_NET_GRAPHHOST, L"", SS_BLACKRECT);
    if (s_graphHost)
        SetWindowSubclass(s_graphHost, NetGraphSubclass, 1, 0);

    s_list = UI_CreateListView(p->hwnd, IDC_NET_LIST, 0);
    if (s_list) {
        UI_AddColumn(s_list, 0, L"Adapter Name",        110, LVCFMT_LEFT);
        UI_AddColumn(s_list, 1, L"Network Utilization",  70, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 2, L"Link Speed",           50, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 3, L"State",                55, LVCFMT_LEFT);
        UI_AddColumn(s_list, 4, L"Receive rate",         76, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 5, L"Send rate",            76, LVCFMT_RIGHT);
    }
}

static void NetLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    int margin = UI_Margin(p->hwnd);
    int graphH;

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

static int net_compare(const void *left, const void *right)
{
    const NetRow *a = left, *b = right;
    int result;
    if (net_column == 1) {
        float av = net_history(a, 0, a->count - 1), bv = net_history(b, 0, b->count - 1);
        result = (av > bv) - (av < bv);
    } else if (net_column == 2) result = (a->speedBps > b->speedBps) - (a->speedBps < b->speedBps);
    else if (net_column == 3) result = (a->connected > b->connected) - (a->connected < b->connected);
    else if (net_column == 4) result = (a->receiveRate > b->receiveRate) - (a->receiveRate < b->receiveRate);
    else if (net_column == 5) result = (a->sendRate > b->sendRate) - (a->sendRate < b->sendRate);
    else result = lstrcmpiW(a->name, b->name);
    if (!result) result = (a->luid > b->luid) - (a->luid < b->luid);
    return result * net_direction;
}

static void NetSnapshot(TabPage *p)
{
    NetRow *rows = NULL;
    DWORD error;
    int count;
    BOOL found = FALSE;
    AcquireSRWLockShared(&g_netLock);
    count = g_sharedCnt; error = net_error;
    if (count) {
        rows = malloc((size_t)count * sizeof(*rows));
        if (rows) memcpy(rows, g_shared, (size_t)count * sizeof(*rows));
    }
    ReleaseSRWLockShared(&g_netLock);
    if (error != net_reported) {
        net_reported = error;
        if (error) App_ReportError(p->hwnd, L"Enumerating network adapters", error);
    }
    if (count && !rows) return;
    free(g_view); g_view = rows; g_viewCnt = count;
    if (count > 1) qsort(g_view, (size_t)count, sizeof(*g_view), net_compare);
    for (int i = 0; i < count; ++i) if (g_view[i].luid == net_selected) found = TRUE;
    if (!found) {
        net_selected = count ? g_view[0].luid : 0;
        for (int i = 0; i < count; ++i) {
            if (g_view[i].connected) { net_selected = g_view[i].luid; break; }
        }
    }
    if (s_graphHost) InvalidateRect(s_graphHost, NULL, FALSE);
    if (!s_list) return;
    net_refreshing = TRUE;
    SendMessageW(s_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_list);
    for (int i = 0; i < count; ++i) {
        WCHAR util[32], speed[48], received[48], sent[48];
        LVITEMW item = {0};
        StringCchPrintfW(util, ARRAYSIZE(util), L"%.1f%%", (double)net_history(&g_view[i], 0, g_view[i].count - 1));
        if (g_view[i].speedBps >= 1000000000ULL)
            StringCchPrintfW(speed, ARRAYSIZE(speed), L"%.1f Gbps", (double)g_view[i].speedBps / 1000000000.0);
        else if (g_view[i].speedBps)
            StringCchPrintfW(speed, ARRAYSIZE(speed), L"%.1f Mbps", (double)g_view[i].speedBps / 1000000.0);
        else lstrcpyW(speed, L"Unknown");
        item.mask = LVIF_TEXT; item.iItem = i; item.pszText = g_view[i].name;
        ListView_InsertItem(s_list, &item);
        ListView_SetItemText(s_list, i, 1, util);
        ListView_SetItemText(s_list, i, 2, speed);
        ListView_SetItemText(s_list, i, 3, UI_Str(g_view[i].connected ? L"Connected" : L"Disconnected"));
        net_format_rate(g_view[i].receiveRate, received, ARRAYSIZE(received));
        net_format_rate(g_view[i].sendRate, sent, ARRAYSIZE(sent));
        ListView_SetItemText(s_list, i, 4, received);
        ListView_SetItemText(s_list, i, 5, sent);
        if (g_view[i].luid == net_selected)
            ListView_SetItemState(s_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    UI_SetHeaderSortArrow(s_list, net_column, net_direction);
    SendMessageW(s_list, WM_SETREDRAW, TRUE, 0); InvalidateRect(s_list, NULL, TRUE);
    net_refreshing = FALSE;
}

static BOOL net_notify(TabPage *p, NMHDR *nm, LRESULT *result)
{
    if (nm->hwndFrom != s_list || net_refreshing) return FALSE;
    if (nm->code == LVN_COLUMNCLICK) {
        int column = ((NMLISTVIEW *)nm)->iSubItem;
        net_direction = column == net_column ? -net_direction : 1;
        net_column = column; NetSnapshot(p); *result = 0; return TRUE;
    }
    if (nm->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *change = (NMLISTVIEW *)nm;
        if ((change->uNewState & LVIS_SELECTED) && change->iItem >= 0 && change->iItem < g_viewCnt) {
            net_selected = g_view[change->iItem].luid;
            if (s_graphHost) InvalidateRect(s_graphHost, NULL, FALSE);
        }
    }
    return FALSE;
}

static void NetBuildViewMenu(TabPage *p, HMENU view)
{
    HMENU sub = CreatePopupMenu();
    (void)p;
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING, IDM_VIEW_NET_BYTESSENT,     L"Bytes &Sent");
    AppendMenuW(sub, MF_STRING, IDM_VIEW_NET_BYTESRECEIVED, L"Bytes &Received");
    AppendMenuW(sub, MF_STRING, IDM_VIEW_NET_BYTESTOTAL,    L"Bytes &Total");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)sub, L"&Network Adapter History");
}

static void NetInitViewMenu(TabPage *p, HMENU view)
{
    UINT check = IDM_VIEW_NET_BYTESTOTAL;
    (void)p;
    if (g_cfg.netHistoryMode == 1)      check = IDM_VIEW_NET_BYTESSENT;
    else if (g_cfg.netHistoryMode == 2) check = IDM_VIEW_NET_BYTESRECEIVED;
    CheckMenuRadioItem(view, IDM_VIEW_NET_BYTESSENT, IDM_VIEW_NET_BYTESTOTAL,
                       check, MF_BYCOMMAND);
}

static void NetCommand(TabPage *p, int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case IDM_VIEW_NET_BYTESTOTAL:    g_cfg.netHistoryMode = 0; break;
    case IDM_VIEW_NET_BYTESSENT:     g_cfg.netHistoryMode = 1; break;
    case IDM_VIEW_NET_BYTESRECEIVED: g_cfg.netHistoryMode = 2; break;
    default: return;
    }
    if (s_graphHost) InvalidateRect(s_graphHost, NULL, TRUE);
    (void)p;
}

static HWND NetPrimary(TabPage *p)
{
    (void)p;
    return s_graphHost;
}

static void NetDestroy(TabPage *p)
{
    (void)p;
    s_graphHost = NULL;
    s_list = NULL;
}

static TabPage s_page = {
    L"Networking", NULL, TAB_NETWORKING,
    NetCreate, NetDestroy, NetLayout,
    NetSnapshot, NetCommand, net_notify, NULL, NULL, NULL,
    NetBuildViewMenu, NetInitViewMenu,
    NetPrimary
};

TabPage *TabNetworking(void) { return &s_page; }
