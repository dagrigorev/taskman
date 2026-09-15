/* ------------------------------------------------------------------------
 * tab_perf.c - the Performance tab.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ui.h"
#include "blame.h"

typedef struct {
    int   id;
    const WCHAR *caption;
} LabelDef;

static const LabelDef kPhysical[] = {
    { IDC_PERF_PHYS_TOTAL,  L"Total"     },
    { IDC_PERF_PHYS_CACHED, L"Cached"    },
    { IDC_PERF_PHYS_AVAIL,  L"Available" },
    { IDC_PERF_PHYS_FREE,   L"Free"      }
};
static const LabelDef kKernel[] = {
    { IDC_PERF_KERN_PAGED,    L"Paged"    },
    { IDC_PERF_KERN_NONPAGED, L"Nonpaged" }
};
static const LabelDef kSystem[] = {
    { IDC_PERF_SYS_HANDLES,   L"Handles"     },
    { IDC_PERF_SYS_THREADS,   L"Threads"     },
    { IDC_PERF_SYS_PROCESSES, L"Processes"   },
    { IDC_PERF_SYS_UPTIME,    L"Up Time"     },
    { IDC_PERF_SYS_COMMIT,    L"Commit (GB)" }
};

static HWND s_cpuGauge, s_cpuHistory, s_memGauge, s_memHistory;
static int  s_captionId;
static float s_cpuNow;
static float s_memNow;

/* ---------------------------------------------------------------- graphs -- */

enum { DRAW_CPU_GAUGE = 1, DRAW_CPU_HIST, DRAW_MEM_GAUGE, DRAW_MEM_HIST };

#define GRAPH_SAMPLES BLAME_SAMPLES   /* one blame slot per plotted column */

/* Spike Blame: which history graph the pointer is over, and where. A pin
   (Ctrl+B) names a sample by sequence so it rides along as the graph
   scrolls, and lets go once the sample scrolls off the left edge. */
static int     s_hoverGraph;        /* 0, DRAW_CPU_HIST or DRAW_MEM_HIST   */
static int     s_hoverX;
static int     s_pinGraph;
static ULONG64 s_pinSequence;

static void DrawGauge(HDC dc, const RECT *rc, float pct, COLORREF color)
{
    RECT label = *rc, track = *rc, fill;
    WCHAR value[32];
    int width = rc->right - rc->left;
    if (!(pct >= 0)) pct = 0;
    if (pct > 100) pct = 100;
    UI_Fill(dc, rc, UI_SURFACE);
    label.bottom = label.top + DPX(34);
    StringCchPrintfW(value, ARRAYSIZE(value), L"%.0f%%", pct);
    UI_Text(dc, value, label, 2, color, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    track.left += width / 3; track.right -= width / 3;
    track.top += DPX(42); track.bottom -= DPX(6);
    if (track.bottom <= track.top) return;
    UI_Fill(dc, &track, RGB(233, 238, 247));
    fill = track; fill.top = track.bottom - (int)((float)(track.bottom - track.top) * pct / 100.0f);
    UI_Fill(dc, &fill, color);
}

static void DrawHistory(HDC dc, const RECT *rc,
                        const float *hist, const float *kernel, int count, COLORREF color)
{
    UI_Chart(dc, *rc, hist, count, color, TRUE);
    /* Kernel time rides on top of the CPU trace, antialiased and mapped by
       the same helper so the two series cannot drift apart by a pixel. */
    if (kernel)
        UI_ChartLine(dc, *rc, kernel, count, RGB(218, 137, 44), DPX(1));
}

/* ------------------------------------------------------------- blame ---- */

static BlameMetric BlameMetricFor(int graph)
{
    return graph == DRAW_MEM_HIST ? BLAME_BY_MEM : BLAME_BY_CPU;
}

/* The per-CPU grid plots many small charts; hovering it would be ambiguous. */
static BOOL BlameEnabledFor(int graph)
{
    return graph == DRAW_MEM_HIST || (graph == DRAW_CPU_HIST && !g_cfg.perfOneGraphPerCpu);
}

static BlameSample *BlameSnapshot(void)
{
    BlameSample *samples = (BlameSample *)malloc(GRAPH_SAMPLES * sizeof(BlameSample));
    if (samples) Blame_Copy(samples, GRAPH_SAMPLES);
    return samples;
}

/* The sample a graph should call out: the pointer if it is over this
   graph, otherwise a pin still on screen. -1 when neither applies. */
static int BlameIndexFor(int graph, int width, const BlameSample *samples)
{
    int index = -1;
    if (!BlameEnabledFor(graph)) return -1;
    if (s_hoverGraph == graph)
        index = Blame_IndexFromX(s_hoverX, width, GRAPH_SAMPLES);
    else if (s_pinGraph == graph)
        index = Blame_FindSequence(samples, GRAPH_SAMPLES, s_pinSequence);
    return index >= 0 && samples[index].sequence ? index : -1;
}

static void BlameFormatTime(const BlameSample *sample, WCHAR *buf, size_t cch)
{
    FILETIME local;
    SYSTEMTIME st;
    if (FileTimeToLocalFileTime(&sample->wallTime, &local) && FileTimeToSystemTime(&local, &st))
        StringCchPrintfW(buf, cch, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    else
        StringCchCopyW(buf, cch, L"--:--:--");
}

static void DrawBlameOverlay(HDC dc, const RECT *rc, int graph,
                             const BlameSample *sample, int index, COLORREF accent)
{
    BlameMetric metric = BlameMetricFor(graph);
    const BlameEntry *top = metric == BLAME_BY_MEM ? sample->byMem : sample->byCpu;
    int count = metric == BLAME_BY_MEM ? sample->memCount : sample->cpuCount;
    int width = rc->right - rc->left, height = rc->bottom - rc->top;
    int x = rc->left + Blame_XFromIndex(index, width, GRAPH_SAMPLES);
    int rowH = DPX(17), panelW = DPX(240), panelH, i;
    RECT cursor, panel, line;
    WCHAR text[128], when[16];

    cursor.left = x > rc->left ? x - 1 : x;
    cursor.right = cursor.left + (DPX(2) > 1 ? DPX(2) : 1);
    cursor.top = rc->top;
    cursor.bottom = rc->bottom;
    UI_Fill(dc, &cursor, UI_INK);

    panelH = rowH * ((count ? count : 1) + 1) + DPX(10);
    if (panelW > width - DPX(8)) panelW = width - DPX(8);
    if (panelH > height - DPX(8)) panelH = height - DPX(8);
    if (panelW < DPX(90) || panelH < rowH * 2) return;

    /* Keep the panel off the column it describes. */
    panel.left = x + DPX(10) + panelW <= rc->right - DPX(4) ? x + DPX(10) : x - DPX(10) - panelW;
    if (panel.left < rc->left + DPX(4)) panel.left = rc->left + DPX(4);
    panel.top = rc->top + DPX(4);
    panel.right = panel.left + panelW;
    panel.bottom = panel.top + panelH;
    UI_Card(dc, &panel, UI_SURFACE, accent);

    line = panel;
    line.left += DPX(8);
    line.right -= DPX(8);
    line.top += DPX(5);
    line.bottom = line.top + rowH;
    BlameFormatTime(sample, when, ARRAYSIZE(when));
    StringCchPrintfW(text, ARRAYSIZE(text), L"%s   %s %.0f%%", when,
                     metric == BLAME_BY_MEM ? L"Memory" : L"CPU",
                     (double)(metric == BLAME_BY_MEM ? sample->mem : sample->cpu));
    UI_Text(dc, text, line, 1, accent, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (!count) {
        OffsetRect(&line, 0, rowH);
        UI_Text(dc, L"No measurable process", line, 0, UI_MUTED,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        return;
    }
    for (i = 0; i < count; ++i) {
        RECT name, value;
        BOOL alive;
        OffsetRect(&line, 0, rowH);
        if (line.bottom > panel.bottom) break;
        alive = Blame_IsAlive(top[i].pid, top[i].createTime);
        name = value = line;
        value.left = line.right - DPX(70);
        name.right = value.left - DPX(4);
        StringCchPrintfW(text, ARRAYSIZE(text), alive ? L"%s (%lu)" : L"%s (%lu) exited",
                         top[i].image, (unsigned long)top[i].pid);
        UI_Text(dc, text, name, 0, alive ? UI_INK : UI_MUTED,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (metric == BLAME_BY_MEM)
            UI_FormatSize(top[i].privateBytes, text, ARRAYSIZE(text));
        else
            StringCchPrintfW(text, ARRAYSIZE(text), L"%.1f%%", (double)top[i].cpuPct);
        UI_Text(dc, text, value, 0, alive ? UI_INK : UI_MUTED,
                DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
}

static void DrawBlame(HDC dc, const RECT *rc, int graph, COLORREF accent)
{
    BlameSample *samples;
    int index;
    if (!BlameEnabledFor(graph) || (s_hoverGraph != graph && s_pinGraph != graph)) return;
    samples = BlameSnapshot();
    if (!samples) return;
    index = BlameIndexFor(graph, rc->right - rc->left, samples);
    if (index >= 0)
        DrawBlameOverlay(dc, rc, graph, &samples[index], index, accent);
    else if (s_pinGraph == graph)
        s_pinGraph = 0;             /* the pinned sample scrolled away */
    free(samples);
}

/* The busiest culprit under the pointer that has not exited since. */
static BOOL BlameCulpritAt(HWND hwnd, int graph, int x, DWORD *pid)
{
    BlameSample *samples;
    RECT rc;
    BOOL found = FALSE;
    int index, count, i;
    const BlameEntry *top;
    if (!BlameEnabledFor(graph)) return FALSE;
    samples = BlameSnapshot();
    if (!samples) return FALSE;
    GetClientRect(hwnd, &rc);
    index = Blame_IndexFromX(x, rc.right - rc.left, GRAPH_SAMPLES);
    top = graph == DRAW_MEM_HIST ? samples[index].byMem : samples[index].byCpu;
    count = graph == DRAW_MEM_HIST ? samples[index].memCount : samples[index].cpuCount;
    for (i = 0; i < count && !found; ++i) {
        if (Blame_IsAlive(top[i].pid, top[i].createTime)) {
            *pid = top[i].pid;
            found = TRUE;
        }
    }
    free(samples);
    return found;
}

static void BlamePinPeak(void)
{
    BlameSample *samples = BlameSnapshot();
    int index;
    if (!samples) return;
    index = Blame_FindPeak(samples, GRAPH_SAMPLES, BLAME_BY_CPU);
    if (index >= 0) {
        s_pinGraph = DRAW_CPU_HIST;
        s_pinSequence = samples[index].sequence;
        /* The pin belongs to the single CPU graph. */
        g_cfg.perfOneGraphPerCpu = FALSE;
    } else {
        MessageBeep(MB_OK);
    }
    free(samples);
    if (s_cpuHistory) InvalidateRect(s_cpuHistory, NULL, FALSE);
}

static LRESULT CALLBACK GraphSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR ref)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return TRUE;

    case WM_NCHITTEST:
        /* Static controls let the mouse fall through unless told not to. */
        /* Tiny footprint keeps the fall-through: its restore gesture is a
           double-click that must reach the page. */
        if (((int)ref == DRAW_CPU_HIST || (int)ref == DRAW_MEM_HIST) && !g_cfg.tiny)
            return HTCLIENT;
        break;

    case WM_MOUSEMOVE:
        if ((int)ref == DRAW_CPU_HIST || (int)ref == DRAW_MEM_HIST) {
            if (s_hoverGraph != (int)ref) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
            }
            s_hoverGraph = (int)ref;
            s_hoverX = GET_X_LPARAM(lp);
            s_pinGraph = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_MOUSELEAVE:
        if (s_hoverGraph == (int)ref) {
            s_hoverGraph = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        break;

    case WM_SETCURSOR:
        if (BlameEnabledFor((int)ref)) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONUP:
        if (BlameEnabledFor((int)ref)) {
            DWORD pid;
            if (BlameCulpritAt(hwnd, (int)ref, GET_X_LPARAM(lp), &pid)) {
                s_hoverGraph = 0;
                App_ShowProcess(pid);
            } else {
                MessageBeep(MB_OK);
            }
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC screen = BeginPaint(hwnd, &ps);
        HDC hdc = NULL;
        HBITMAP bitmap = NULL;
        HGDIOBJ oldBitmap = NULL;
        RECT rc;
        GetClientRect(hwnd, &rc);
        /* Buffered: the blame overlay repaints on every mouse move and
           would otherwise flicker over the freshly filled chart. */
        if (rc.right > 0 && rc.bottom > 0) {
            hdc = CreateCompatibleDC(screen);
            if (hdc) bitmap = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
            if (bitmap) oldBitmap = SelectObject(hdc, bitmap);
        }
        if (!bitmap) {
            if (hdc) DeleteDC(hdc);
            hdc = screen;
        }

        switch ((int)ref) {
        case DRAW_CPU_GAUGE:
            DrawGauge(hdc, &rc, s_cpuNow, UI_BLUE);
            break;
        case DRAW_MEM_GAUGE:
            DrawGauge(hdc, &rc, s_memNow, UI_VIOLET);
            break;
        case DRAW_CPU_HIST: {
            float hist[GRAPH_SAMPLES], hist2[GRAPH_SAMPLES];
            UINT cpus = SysInfo_CpuHistoryCount();
            if (g_cfg.perfOneGraphPerCpu && cpus) {
                UINT cols = 1, rows, cpu;
                int width = rc.right - rc.left, height = rc.bottom - rc.top;
                while (cols * cols < cpus) ++cols;
                rows = (cpus + cols - 1) / cols;
                UI_Fill(hdc, &rc, UI_SURFACE);
                for (cpu = 0; cpu < cpus; ++cpu) {
                    RECT cell = { rc.left + (int)(cpu % cols) * width / (int)cols,
                                  rc.top + (int)(cpu / cols) * height / (int)rows,
                                  rc.left + (int)(cpu % cols + 1) * width / (int)cols - 2,
                                  rc.top + (int)(cpu / cols + 1) * height / (int)rows - 2 };
                    WCHAR label[32];
                    SysInfo_CopyProcessorHistory(cpu, hist, hist2, GRAPH_SAMPLES);
                    DrawHistory(hdc, &cell, hist, g_cfg.perfShowKernelTimes ? hist2 : NULL, GRAPH_SAMPLES, UI_BLUE);
                    StringCchPrintfW(label, ARRAYSIZE(label), L"CPU %u", cpu);
                    SetTextColor(hdc, UI_MUTED); SetBkMode(hdc, TRANSPARENT);
                    if (cell.right - cell.left > 45 && cell.bottom - cell.top > 20)
                        DrawTextW(hdc, label, -1, &cell, DT_TOP | DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                }
                break;
            }
            SysInfo_CopyCpuHistory(hist, GRAPH_SAMPLES);
            if (g_cfg.perfShowKernelTimes) {
                SysInfo_CopyKernelHistory(hist2, GRAPH_SAMPLES);
                DrawHistory(hdc, &rc, hist, hist2, GRAPH_SAMPLES, UI_BLUE);
            } else {
                DrawHistory(hdc, &rc, hist, NULL, GRAPH_SAMPLES, UI_BLUE);
            }
            DrawBlame(hdc, &rc, DRAW_CPU_HIST, UI_BLUE);
            break;
        }
        case DRAW_MEM_HIST: {
            float hist[GRAPH_SAMPLES];
            SysInfo_CopyMemHistory(hist, GRAPH_SAMPLES);
            DrawHistory(hdc, &rc, hist, NULL, GRAPH_SAMPLES, UI_VIOLET);
            DrawBlame(hdc, &rc, DRAW_MEM_HIST, UI_VIOLET);
            break;
        }
        default:
            UI_Fill(hdc, &rc, UI_SURFACE);
            break;
        }
        if (hdc != screen) {
            BitBlt(screen, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, oldBitmap);
            DeleteObject(bitmap);
            DeleteDC(hdc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, GraphSubclass, id);
        return DefSubclassProc(hwnd, msg, wp, lp);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------- layout -- */

static void MakeGroup(HWND page, int groupId, const WCHAR *title,
                      const LabelDef *defs, int count)
{
    int i;
    UI_CreateGroupBox(page, groupId, title);
    for (i = 0; i < count; i++) {
        UI_CreateStatic(page, s_captionId++, defs[i].caption, SS_LEFTNOWORDWRAP);
        UI_CreateStatic(page, defs[i].id, L"0", SS_RIGHT);
    }
}

static void PerfCreate(TabPage *p)
{
    HWND h = p->hwnd;

    UI_CreateGroupBox(h, IDC_PERF_GRP_CPUUSAGE,   L"CPU Usage");
    UI_CreateGroupBox(h, IDC_PERF_GRP_CPUHISTORY, L"CPU Usage History");
    UI_CreateGroupBox(h, IDC_PERF_GRP_MEMUSAGE,   L"Memory");
    UI_CreateGroupBox(h, IDC_PERF_GRP_MEMHISTORY, L"Physical Memory Usage History");

    s_cpuGauge   = UI_CreateStatic(h, IDC_PERF_CPUGAUGE,   L"", SS_BLACKRECT);
    s_cpuHistory = UI_CreateStatic(h, IDC_PERF_CPUHISTORY, L"", SS_BLACKRECT);
    s_memGauge   = UI_CreateStatic(h, IDC_PERF_MEMGAUGE,   L"", SS_BLACKRECT);
    s_memHistory = UI_CreateStatic(h, IDC_PERF_MEMHISTORY, L"", SS_BLACKRECT);

    if (s_cpuGauge)   SetWindowSubclass(s_cpuGauge,   GraphSubclass, 1, DRAW_CPU_GAUGE);
    if (s_cpuHistory) SetWindowSubclass(s_cpuHistory, GraphSubclass, 2, DRAW_CPU_HIST);
    if (s_memGauge)   SetWindowSubclass(s_memGauge,   GraphSubclass, 3, DRAW_MEM_GAUGE);
    if (s_memHistory) SetWindowSubclass(s_memHistory, GraphSubclass, 4, DRAW_MEM_HIST);

    s_captionId = IDC_PERF_CAP_FIRST;
    MakeGroup(h, IDC_PERF_GRP_PHYSICAL, L"Physical Memory (MB)",
              kPhysical, (int)ARRAYSIZE(kPhysical));
    MakeGroup(h, IDC_PERF_GRP_KERNEL,   L"Kernel Memory (MB)",
              kKernel,   (int)ARRAYSIZE(kKernel));
    MakeGroup(h, IDC_PERF_GRP_SYSTEM,   L"System",
              kSystem,   (int)ARRAYSIZE(kSystem));

    UI_CreateButton(h, IDC_PERF_RESMON, L"Resource &Monitor...", 0);
}

static int LayoutGroup(HWND page, int groupId, const LabelDef *defs, int count,
                       int *captionId, int x, int y, int w, int lineH)
{
    int top = y;
    int i;
    HWND grp = GetDlgItem(page, groupId);
    int textTop = y + lineH + lineH / 3;
    int height;

    for (i = 0; i < count; i++) {
        HWND cap = GetDlgItem(page, (*captionId)++);
        HWND val = GetDlgItem(page, defs[i].id);
        int rowY = textTop + i * lineH;
        int pad = UI_Margin(page);
        if (cap) MoveWindow(cap, x + pad, rowY, w / 2, lineH, TRUE);
        if (val) MoveWindow(val, x + w / 2, rowY, w / 2 - pad, lineH, TRUE);
    }
    height = (textTop - top) + count * lineH + lineH / 2;
    if (grp) MoveWindow(grp, x, top, w, height, TRUE);
    return height;
}

static void PerfLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    static const int buttons[] = { IDC_PERF_RESMON };
    HWND h = p->hwnd;
    int margin = UI_Margin(h);
    SIZE bs = UI_ButtonSize(h);
    int lineH = UI_LineHeight(h);
    int captionId;
    int gaugeW, histX, histW, rowH, y;
    int bottomTop, groupW, gx, i;
    static const int allIds[] = {
        IDC_PERF_GRP_CPUUSAGE, IDC_PERF_GRP_CPUHISTORY,
        IDC_PERF_GRP_MEMUSAGE, IDC_PERF_GRP_MEMHISTORY,
        IDC_PERF_GRP_PHYSICAL, IDC_PERF_GRP_KERNEL, IDC_PERF_GRP_SYSTEM,
        IDC_PERF_RESMON
    };

    /* On short high-DPI work areas, retain the two graphs and dashboard. */
    if (cy < DPX(340)) tiny = TRUE;

    for (i = 0; i < (int)ARRAYSIZE(allIds); i++) {
        HWND c = GetDlgItem(h, allIds[i]);
        if (c) ShowWindow(c, tiny ? SW_HIDE : SW_SHOW);
    }
    for (i = IDC_PERF_CAP_FIRST; i < IDC_PERF_CAP_FIRST + 20; i++) {
        HWND c = GetDlgItem(h, i);
        if (c) ShowWindow(c, tiny ? SW_HIDE : SW_SHOW);
    }
    for (i = IDC_PERF_LBL_FIRST; i <= IDC_PERF_LBL_LAST; i++) {
        HWND c = GetDlgItem(h, i);
        if (c) ShowWindow(c, tiny ? SW_HIDE : SW_SHOW);
    }

    if (tiny) {
        int half = cy / 2;
        if (s_cpuGauge)   ShowWindow(s_cpuGauge,   SW_HIDE);
        if (s_memGauge)   ShowWindow(s_memGauge,   SW_HIDE);
        if (s_cpuHistory) { ShowWindow(s_cpuHistory, SW_SHOW);
                            MoveWindow(s_cpuHistory, 0, 0, cx, half, TRUE); }
        if (s_memHistory) { ShowWindow(s_memHistory, SW_SHOW);
                            MoveWindow(s_memHistory, 0, half, cx, cy - half, TRUE); }
        return;
    }

    if (s_cpuGauge) ShowWindow(s_cpuGauge, SW_SHOW);
    if (s_memGauge) ShowWindow(s_memGauge, SW_SHOW);

    bottomTop = cy - margin - bs.cy - margin - (8 * lineH);
    if (bottomTop < DPX(120)) bottomTop = DPX(120);

    gaugeW = DPX(120);
    histX  = margin + gaugeW + margin;
    histW  = cx - margin - histX;
    if (histW < DPX(60)) histW = DPX(60);
    rowH   = (bottomTop - margin - margin) / 2;
    if (rowH < DPX(50)) rowH = DPX(50);

    y = margin;
    {
        HWND g;
        g = GetDlgItem(h, IDC_PERF_GRP_CPUUSAGE);
        if (g) MoveWindow(g, margin, y, gaugeW, rowH, TRUE);
        if (s_cpuGauge)
            MoveWindow(s_cpuGauge, margin + margin, y + lineH + lineH / 3,
                       gaugeW - 2 * margin, rowH - lineH * 2, TRUE);

        g = GetDlgItem(h, IDC_PERF_GRP_CPUHISTORY);
        if (g) MoveWindow(g, histX, y, histW, rowH, TRUE);
        if (s_cpuHistory)
            MoveWindow(s_cpuHistory, histX + margin, y + lineH + lineH / 3,
                       histW - 2 * margin, rowH - lineH * 2, TRUE);

        y += rowH + margin;

        g = GetDlgItem(h, IDC_PERF_GRP_MEMUSAGE);
        if (g) MoveWindow(g, margin, y, gaugeW, rowH, TRUE);
        if (s_memGauge)
            MoveWindow(s_memGauge, margin + margin, y + lineH + lineH / 3,
                       gaugeW - 2 * margin, rowH - lineH * 2, TRUE);

        g = GetDlgItem(h, IDC_PERF_GRP_MEMHISTORY);
        if (g) MoveWindow(g, histX, y, histW, rowH, TRUE);
        if (s_memHistory)
            MoveWindow(s_memHistory, histX + margin, y + lineH + lineH / 3,
                       histW - 2 * margin, rowH - lineH * 2, TRUE);
    }

    groupW = (cx - 4 * margin) / 3;
    if (groupW < DPX(80)) groupW = DPX(80);
    gx = margin;
    captionId = IDC_PERF_CAP_FIRST;
    LayoutGroup(h, IDC_PERF_GRP_PHYSICAL, kPhysical, (int)ARRAYSIZE(kPhysical),
                &captionId, gx, bottomTop, groupW, lineH);
    gx += groupW + margin;
    LayoutGroup(h, IDC_PERF_GRP_KERNEL, kKernel, (int)ARRAYSIZE(kKernel),
                &captionId, gx, bottomTop, groupW, lineH);
    gx += groupW + margin;
    LayoutGroup(h, IDC_PERF_GRP_SYSTEM, kSystem, (int)ARRAYSIZE(kSystem),
                &captionId, gx, bottomTop, cx - margin - gx, lineH);

    UI_PlaceButtonRow(h, buttons, (int)ARRAYSIZE(buttons), cx, cy);
}

static void SetLabelNumber(HWND page, int id, ULONGLONG value)
{
    WCHAR text[64];
    UI_FormatNumber(value, text, ARRAYSIZE(text));
    SetDlgItemTextW(page, id, text);
}

static void PerfSnapshot(TabPage *p)
{
    const Snapshot *s = SysInfo_Lock();
    Snapshot copy = *s;
    SysInfo_Unlock();

    s_cpuNow = (float)copy.cpuUsage;
    s_memNow = (float)copy.memUsage;

    SetLabelNumber(p->hwnd, IDC_PERF_PHYS_TOTAL,  copy.memTotal  / (1024 * 1024));
    SetLabelNumber(p->hwnd, IDC_PERF_PHYS_CACHED, copy.memCached / (1024 * 1024));
    SetLabelNumber(p->hwnd, IDC_PERF_PHYS_AVAIL,  copy.memAvail  / (1024 * 1024));
    SetLabelNumber(p->hwnd, IDC_PERF_PHYS_FREE,   copy.memFree   / (1024 * 1024));
    SetLabelNumber(p->hwnd, IDC_PERF_KERN_PAGED,    copy.kernelPaged    / (1024 * 1024));
    SetLabelNumber(p->hwnd, IDC_PERF_KERN_NONPAGED, copy.kernelNonPaged / (1024 * 1024));
    SetLabelNumber(p->hwnd, IDC_PERF_SYS_HANDLES,   copy.handleCount);
    SetLabelNumber(p->hwnd, IDC_PERF_SYS_THREADS,   copy.threadCount);
    SetLabelNumber(p->hwnd, IDC_PERF_SYS_PROCESSES, copy.processCount);

    {
        WCHAR text[64];
        ULONGLONG secs = copy.upTimeMs / 1000;
        unsigned days  = (unsigned)(secs / 86400);
        unsigned hours = (unsigned)((secs % 86400) / 3600);
        unsigned mins  = (unsigned)((secs % 3600) / 60);
        unsigned rest  = (unsigned)(secs % 60);
        StringCchPrintfW(text, ARRAYSIZE(text), L"%u:%02u:%02u:%02u",
                         days, hours, mins, rest);
        SetDlgItemTextW(p->hwnd, IDC_PERF_SYS_UPTIME, text);

        StringCchPrintfW(text, ARRAYSIZE(text), L"%.1f / %.1f",
                         (double)copy.commitTotal / (1024.0 * 1024.0 * 1024.0),
                         (double)copy.commitLimit / (1024.0 * 1024.0 * 1024.0));
        SetDlgItemTextW(p->hwnd, IDC_PERF_SYS_COMMIT, text);
    }

    if (s_cpuGauge)   InvalidateRect(s_cpuGauge,   NULL, FALSE);
    if (s_cpuHistory) InvalidateRect(s_cpuHistory, NULL, FALSE);
    if (s_memGauge)   InvalidateRect(s_memGauge,   NULL, FALSE);
    if (s_memHistory) InvalidateRect(s_memHistory, NULL, FALSE);
}

static void PerfBuildViewMenu(TabPage *p, HMENU view)
{
    HMENU sub = CreatePopupMenu();
    (void)p;
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING, IDM_VIEW_CPU_ONEGRAPH, L"&One Graph, All CPUs");
    AppendMenuW(sub, MF_STRING, IDM_VIEW_CPU_PERCPU,   L"One Graph &Per CPU");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)sub, L"&CPU History");
    AppendMenuW(view, MF_STRING, IDM_VIEW_SHOWKERNELTIMES, L"Show &Kernel Times");
    AppendMenuW(view, MF_STRING, IDM_VIEW_BLAME_PEAK, L"&Blame CPU Peak\tCtrl+B");
}

static void PerfInitViewMenu(TabPage *p, HMENU view)
{
    (void)p;
    CheckMenuRadioItem(view, IDM_VIEW_CPU_ONEGRAPH, IDM_VIEW_CPU_PERCPU,
                       g_cfg.perfOneGraphPerCpu ? IDM_VIEW_CPU_PERCPU
                                                : IDM_VIEW_CPU_ONEGRAPH,
                       MF_BYCOMMAND);
    CheckMenuItem(view, IDM_VIEW_SHOWKERNELTIMES,
                  MF_BYCOMMAND | (g_cfg.perfShowKernelTimes ? MF_CHECKED
                                                            : MF_UNCHECKED));
}

static void PerfCommand(TabPage *p, int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case IDM_VIEW_CPU_ONEGRAPH:
        g_cfg.perfOneGraphPerCpu = FALSE;
        if (s_cpuHistory) InvalidateRect(s_cpuHistory, NULL, FALSE);
        break;
    case IDM_VIEW_CPU_PERCPU:
        g_cfg.perfOneGraphPerCpu = TRUE;
        if (s_cpuHistory) InvalidateRect(s_cpuHistory, NULL, FALSE);
        break;
    case IDM_VIEW_SHOWKERNELTIMES:
        g_cfg.perfShowKernelTimes = !g_cfg.perfShowKernelTimes;
        if (s_cpuHistory) InvalidateRect(s_cpuHistory, NULL, FALSE);
        break;
    case IDM_VIEW_BLAME_PEAK:
        BlamePinPeak();
        break;
    case IDC_PERF_RESMON:
        if (App_OpenSystemTool(p->hwnd, FALSE)) App_MinimizeOnUse();
        break;
    default:
        break;
    }
}

static HWND PerfPrimary(TabPage *p)
{
    (void)p;
    return s_cpuHistory;
}

static void PerfDestroy(TabPage *p)
{
    (void)p;
    s_cpuGauge = s_cpuHistory = s_memGauge = s_memHistory = NULL;
    s_hoverGraph = s_pinGraph = 0;
}

static TabPage s_page = {
    L"Performance", NULL, TAB_PERFORMANCE,
    PerfCreate, PerfDestroy, PerfLayout,
    PerfSnapshot, PerfCommand,
    NULL, NULL, NULL, NULL,
    PerfBuildViewMenu, PerfInitViewMenu,
    PerfPrimary
};

TabPage *TabPerformance(void) { return &s_page; }
