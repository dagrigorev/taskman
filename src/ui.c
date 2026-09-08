#include "app.h"
#include "ui.h"
#include "gdiplusapi.h"

static HBRUSH ui_background;
static HFONT ui_fonts[4];

/* ------------------------------------------------- antialiased drawing --- */

/* GDI has no antialiasing for lines or polygons, so the chart strokes and the
   area under them go through GDI+ instead.  gdiplus.dll is resolved lazily
   with GetProcAddress: when anything is missing every entry point below
   reports "no canvas" and the callers fall back to the plain GDI path, which
   is exactly what shipped before.  Charts are drawn on the UI thread only. */

/* Members are named exactly after the gdiplus.dll exports, in the same order
   as gp_names below. Short names are not an option here: several of them
   (DeletePen, DeleteBrush) are function-like macros in <windowsx.h>. */
static struct {
    PFN_GdiplusStartup          GdiplusStartup;
    PFN_GdiplusShutdown         GdiplusShutdown;
    PFN_GdipCreateFromHDC       GdipCreateFromHDC;
    PFN_GdipDeleteGraphics      GdipDeleteGraphics;
    PFN_GdipSetSmoothingMode    GdipSetSmoothingMode;
    PFN_GdipSetPixelOffsetMode  GdipSetPixelOffsetMode;
    PFN_GdipCreatePen1          GdipCreatePen1;
    PFN_GdipDeletePen           GdipDeletePen;
    PFN_GdipSetPenLineJoin      GdipSetPenLineJoin;
    PFN_GdipSetPenStartCap      GdipSetPenStartCap;
    PFN_GdipSetPenEndCap        GdipSetPenEndCap;
    PFN_GdipCreateSolidFill     GdipCreateSolidFill;
    PFN_GdipDeleteBrush         GdipDeleteBrush;
    PFN_GdipDrawLinesI          GdipDrawLinesI;
    PFN_GdipFillPolygonI        GdipFillPolygonI;
    PFN_GdipCreatePath          GdipCreatePath;
    PFN_GdipDeletePath          GdipDeletePath;
    PFN_GdipAddPathArc          GdipAddPathArc;
    PFN_GdipClosePathFigure     GdipClosePathFigure;
    PFN_GdipFillPath            GdipFillPath;
    PFN_GdipDrawPath            GdipDrawPath;
} gp;

static const char *const gp_names[] = {
    "GdiplusStartup", "GdiplusShutdown", "GdipCreateFromHDC",
    "GdipDeleteGraphics", "GdipSetSmoothingMode", "GdipSetPixelOffsetMode",
    "GdipCreatePen1", "GdipDeletePen", "GdipSetPenLineJoin",
    "GdipSetPenStartCap", "GdipSetPenEndCap", "GdipCreateSolidFill",
    "GdipDeleteBrush", "GdipDrawLinesI", "GdipFillPolygonI",
    "GdipCreatePath", "GdipDeletePath", "GdipAddPathArc",
    "GdipClosePathFigure", "GdipFillPath", "GdipDrawPath"
};

static HMODULE   gp_module;
static ULONG_PTR gp_token;
static LONG      gp_state;      /* 0 = untried, 1 = in progress, 2 = settled  */
static BOOL      gp_ready;

/* GpLoad walks `gp` as a flat array of function pointers, so gp_names must
   stay index-aligned with the struct: adding a member without adding its
   export name (or the reverse) fails the build here rather than silently
   calling the wrong entry point. Every member is a same-sized pointer, so
   there is no padding to step over.

   The same technique proves the POINT -> CTM_GpPoint cast is safe, instead
   of trusting the platform ABI. Both are written as negative array bounds
   rather than _Static_assert, which needs a C11 mode neither build selects. */
typedef char ctm_gp_table_check[
    (sizeof(gp) == ARRAYSIZE(gp_names) * sizeof(FARPROC)) ? 1 : -1];

typedef char ctm_gppoint_layout_check[
    (sizeof(POINT) == sizeof(CTM_GpPoint) &&
     sizeof(((POINT *)0)->x) == sizeof(((CTM_GpPoint *)0)->X) &&
     offsetof(POINT, y) == offsetof(CTM_GpPoint, Y)) ? 1 : -1];

static CTM_ARGB ArgbFrom(COLORREF color, BYTE alpha)
{
    return ((CTM_ARGB)alpha << 24) | ((CTM_ARGB)GetRValue(color) << 16) |
           ((CTM_ARGB)GetGValue(color) << 8) | (CTM_ARGB)GetBValue(color);
}

static BOOL GpLoad(void)
{
    CTM_GdiplusStartupInput input;
    HMODULE module;
    int i;
    FARPROC *slots = (FARPROC *)(void *)&gp;

    if (InterlockedCompareExchange(&gp_state, 1, 0) != 0) {
        /* Another path already resolved (or is resolving) the exports. */
        while (InterlockedCompareExchange(&gp_state, 2, 2) != 2) Sleep(0);
        return gp_ready;
    }

    module = LoadLibraryW(L"gdiplus.dll");
    if (module) {
        for (i = 0; i < (int)ARRAYSIZE(gp_names); ++i) {
            slots[i] = GetProcAddress(module, gp_names[i]);
            if (!slots[i]) break;
        }
        if (i == (int)ARRAYSIZE(gp_names)) {
            ZeroMemory(&input, sizeof(input));
            input.GdiplusVersion = 1;
            if (gp.GdiplusStartup(&gp_token, &input, NULL) == 0) {
                gp_module = module;
                gp_ready  = TRUE;
            }
        }
        if (!gp_ready) { ZeroMemory(&gp, sizeof(gp)); FreeLibrary(module); }
    }
    InterlockedExchange(&gp_state, 2);
    return gp_ready;
}

void UI_GfxShutdown(void)
{
    if (!gp_ready) return;
    gp.GdiplusShutdown(gp_token);
    gp_ready  = FALSE;
    gp_token  = 0;
    ZeroMemory(&gp, sizeof(gp));
    /* The module stays loaded: GDI+ forbids unloading between Shutdown and
       process exit, and this only ever runs on the way out. */
    gp_module = NULL;
}

/* Returns a smoothing-enabled GDI+ surface for `dc`, or NULL when GDI+ is
   unavailable. Callers must fall back to GDI on NULL. */
static CTM_GpGraphics *GpBegin(HDC dc)
{
    CTM_GpGraphics *graphics = NULL;
    if (!GpLoad()) return NULL;
    if (gp.GdipCreateFromHDC(dc, &graphics) != 0 || !graphics) return NULL;
    gp.GdipSetSmoothingMode(graphics, CTM_SmoothingModeAntiAlias);
    gp.GdipSetPixelOffsetMode(graphics, CTM_PixelOffsetModeHalf);
    return graphics;
}

static void GpStroke(CTM_GpGraphics *graphics, const POINT *points, int count,
                     COLORREF color, int width)
{
    CTM_GpPen *pen = NULL;
    if (count < 2) return;
    if (gp.GdipCreatePen1(ArgbFrom(color, 255), (CTM_REAL)width, CTM_UnitPixel, &pen) != 0 || !pen)
        return;
    /* Rounded joins and caps stop the spikes in a busy CPU trace from
       growing miter darts several pixels past the sample. */
    gp.GdipSetPenLineJoin(pen, CTM_LineJoinRound);
    gp.GdipSetPenStartCap(pen, CTM_LineCapRound);
    gp.GdipSetPenEndCap(pen, CTM_LineCapRound);
    gp.GdipDrawLinesI(graphics, pen, (const CTM_GpPoint *)points, count);
    gp.GdipDeletePen(pen);
}

static void GpArea(CTM_GpGraphics *graphics, const POINT *points, int count,
                   COLORREF color)
{
    CTM_GpBrush *brush = NULL;
    if (count < 3) return;
    if (gp.GdipCreateSolidFill(ArgbFrom(color, 255), &brush) != 0 || !brush) return;
    gp.GdipFillPolygonI(graphics, brush, (const CTM_GpPoint *)points, count,
                    CTM_FillModeAlternate);
    gp.GdipDeleteBrush(brush);
}

/* Fills and outlines a rounded rectangle. Returns FALSE without drawing
   anything if GDI+ cannot build the shape, so the caller can fall back. */
static BOOL GpRoundRect(CTM_GpGraphics *graphics, const RECT *rc, int diameter,
                        COLORREF fill, COLORREF border)
{
    CTM_GpPath  *path  = NULL;
    CTM_GpBrush *brush = NULL;
    CTM_GpPen   *pen   = NULL;
    /* Half a pixel in from the edge: GDI+ centres a stroke on the path, so an
       integer-aligned outline would straddle two pixel columns and read as a
       soft two pixel edge instead of one crisp one. The size shrinks by a
       whole pixel to match, which also matches GDI's RoundRect, whose right
       and bottom edges are exclusive. */
    CTM_REAL x = (CTM_REAL)rc->left + 0.5f;
    CTM_REAL y = (CTM_REAL)rc->top  + 0.5f;
    CTM_REAL w = (CTM_REAL)(rc->right - rc->left) - 1.0f;
    CTM_REAL h = (CTM_REAL)(rc->bottom - rc->top) - 1.0f;
    CTM_REAL d = (CTM_REAL)diameter;

    if (w < 1.0f || h < 1.0f) return FALSE;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d < 1.0f) return FALSE;     /* too small to round; let GDI draw it */

    if (gp.GdipCreatePath(CTM_FillModeAlternate, &path) != 0 || !path) return FALSE;

    /* Four quarter-circle corners, clockwise from the top left. GDI+ joins
       consecutive arcs with straight edges, so the sides come for free. */
    gp.GdipAddPathArc(path, x,         y,         d, d, 180.0f, 90.0f);
    gp.GdipAddPathArc(path, x + w - d, y,         d, d, 270.0f, 90.0f);
    gp.GdipAddPathArc(path, x + w - d, y + h - d, d, d,   0.0f, 90.0f);
    gp.GdipAddPathArc(path, x,         y + h - d, d, d,  90.0f, 90.0f);
    gp.GdipClosePathFigure(path);

    /* Both objects up front: a half-drawn card is worse than a GDI one. */
    if (gp.GdipCreateSolidFill(ArgbFrom(fill, 255), &brush) != 0 || !brush ||
        gp.GdipCreatePen1(ArgbFrom(border, 255), 1.0f, CTM_UnitPixel, &pen) != 0 || !pen) {
        if (brush) gp.GdipDeleteBrush(brush);
        gp.GdipDeletePath(path);
        return FALSE;
    }

    gp.GdipFillPath(graphics, brush, path);
    gp.GdipDrawPath(graphics, pen, path);

    gp.GdipDeletePen(pen);
    gp.GdipDeleteBrush(brush);
    gp.GdipDeletePath(path);
    return TRUE;
}

/* Maps `count` percentages onto the plot rectangle, newest last.
   Returns the number of points written (clamped to CTM_HISTORY). */
static int ChartPoints(const RECT *rc, const float *samples, int count, POINT *out)
{
    int i, width = rc->right - rc->left, height = rc->bottom - rc->top;
    if (count > CTM_HISTORY) count = CTM_HISTORY;
    for (i = 0; i < count; ++i) {
        float value = samples[i];
        if (!(value >= 0)) value = 0;
        if (value > 100) value = 100;
        out[i].x = rc->left + (count > 1 ? i * (width - 1) / (count - 1) : width - 1);
        out[i].y = rc->bottom - 1 - (int)(value * (float)(height - 1) / 100.0f);
    }
    return count;
}

void UI_Polyline(HDC dc, const POINT *points, int count, COLORREF color, int width)
{
    CTM_GpGraphics *graphics;

    if (!points || count < 1) return;
    if (width < 1) width = 1;
    if (count == 1) { SetPixel(dc, points[0].x, points[0].y, color); return; }

    graphics = GpBegin(dc);
    if (graphics) {
        GpStroke(graphics, points, count, color, width);
        gp.GdipDeleteGraphics(graphics);
        return;
    }
    {
        HPEN pen = CreatePen(PS_SOLID, width, color);
        HGDIOBJ old = pen ? SelectObject(dc, pen) : NULL;
        Polyline(dc, points, count);
        if (pen) { SelectObject(dc, old); DeleteObject(pen); }
    }
}

void UI_ChartLine(HDC dc, RECT rc, const float *samples, int count,
                  COLORREF color, int width)
{
    POINT points[CTM_HISTORY];
    if (!samples || count < 1) return;
    if (rc.right - rc.left < 2 || rc.bottom - rc.top < 2) return;
    count = ChartPoints(&rc, samples, count, points);
    UI_Polyline(dc, points, count, color, width);
}

void UI_ThemeDestroy(void)
{
    int i;
    for (i = 0; i < 4; ++i) {
        if (ui_fonts[i]) DeleteObject(ui_fonts[i]);
        ui_fonts[i] = NULL;
    }
    if (ui_background) DeleteObject(ui_background);
    ui_background = NULL;
}

void UI_ThemeInit(void)
{
    LOGFONTW lf = {0};
    static const int sizes[] = { 11, 12, 20, 28 };
    int i;
    UI_ThemeDestroy();
    if (g_hFont) GetObjectW(g_hFont, sizeof(lf), &lf);
    if (!lf.lfFaceName[0]) lstrcpyW(lf.lfFaceName, L"Segoe UI");
    for (i = 0; i < 4; ++i) {
        lf.lfHeight = -DPX(sizes[i]);
        lf.lfWeight = i > 0 ? FW_SEMIBOLD : FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        ui_fonts[i] = CreateFontIndirectW(&lf);
    }
    ui_background = CreateSolidBrush(UI_BG);
}

HBRUSH UI_BackgroundBrush(void)
{
    return ui_background ? ui_background : GetSysColorBrush(COLOR_WINDOW);
}

void UI_Fill(HDC dc, const RECT *rc, COLORREF color)
{
    HBRUSH br = CreateSolidBrush(color);
    if (br) { FillRect(dc, rc, br); DeleteObject(br); }
}

void UI_Text(HDC dc, const WCHAR *text, RECT rc, int size, COLORREF color, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, ui_fonts[size] ? ui_fonts[size] : g_hFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &rc, DT_NOPREFIX | flags);
    if (old) SelectObject(dc, old);
}

void UI_Card(HDC dc, const RECT *rc, COLORREF fill, COLORREF border)
{
    CTM_GpGraphics *graphics = GpBegin(dc);
    HPEN pen;
    HBRUSH brush;
    HGDIOBJ oldPen, oldBrush;

    if (graphics) {
        BOOL drawn = GpRoundRect(graphics, rc, DPX(10), fill, border);
        gp.GdipDeleteGraphics(graphics);
        if (drawn) return;
    }

    pen = CreatePen(PS_SOLID, 1, border);
    brush = CreateSolidBrush(fill);
    oldPen = SelectObject(dc, pen); oldBrush = SelectObject(dc, brush);
    RoundRect(dc, rc->left, rc->top, rc->right, rc->bottom, DPX(10), DPX(10));
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen);
    DeleteObject(brush); DeleteObject(pen);
}

void UI_Chart(HDC dc, RECT rc, const float *samples, int count, COLORREF color, BOOL grid)
{
    HPEN pen, old;
    POINT points[CTM_HISTORY + 2];
    CTM_GpGraphics *graphics;
    COLORREF shade;
    int i, width = rc.right - rc.left, height = rc.bottom - rc.top;
    if (width < 2 || height < 2) return;
    UI_Fill(dc, &rc, UI_SURFACE);
    if (grid) {
        /* The grid stays on plain GDI: these lines are axis aligned and one
           pixel wide, and antialiasing would only blur them. */
        pen = CreatePen(PS_SOLID, 1, UI_LINE); old = SelectObject(dc, pen);
        for (i = 0; i < 5; ++i) {
            int y = rc.top + (height - 1) * i / 4;
            MoveToEx(dc, rc.left, y, NULL); LineTo(dc, rc.right, y);
        }
        for (i = 1; i < 8; ++i) {
            int x = rc.left + width * i / 8;
            MoveToEx(dc, x, rc.top, NULL); LineTo(dc, x, rc.bottom);
        }
        SelectObject(dc, old); DeleteObject(pen);
    }
    if (!samples || count < 1) return;
    count = ChartPoints(&rc, samples, count, points);

    shade = RGB((GetRValue(color) + 6 * 255) / 7,
                (GetGValue(color) + 6 * 255) / 7,
                (GetBValue(color) + 6 * 255) / 7);
    /* Close the trace down to the baseline to get the filled area. */
    points[count].x     = points[count - 1].x; points[count].y     = rc.bottom;
    points[count + 1].x = points[0].x;         points[count + 1].y = rc.bottom;

    /* One surface for both the area and the stroke, so the stroke blends
       against the fill that is already there rather than against the grid. */
    graphics = GpBegin(dc);
    if (graphics) {
        GpArea(graphics, points, count + 2, shade);
        GpStroke(graphics, points, count, color, DPX(2));
        gp.GdipDeleteGraphics(graphics);
        return;
    }

    {
        HBRUSH fill = CreateSolidBrush(shade), oldFill = SelectObject(dc, fill);
        old = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, points, count + 2);
        SelectObject(dc, oldFill); SelectObject(dc, old); DeleteObject(fill);
    }
    pen = CreatePen(PS_SOLID, DPX(2), color); old = SelectObject(dc, pen);
    if (count > 1) Polyline(dc, points, count);
    else SetPixel(dc, points[0].x, points[0].y, color);
    SelectObject(dc, old); DeleteObject(pen);
}

static LRESULT CALLBACK ListStyleProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref)
{
    if (msg == WM_SETFONT && ref && ref != g_dpi) {
        int i, count = Header_GetItemCount(ListView_GetHeader(hwnd));
        for (i = 0; i < count; ++i)
            ListView_SetColumnWidth(hwnd, i, MulDiv(ListView_GetColumnWidth(hwnd, i), (int)g_dpi, (int)ref));
        SetWindowSubclass(hwnd, ListStyleProc, id, g_dpi);
    }
    if (msg == WM_NOTIFY) {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->hwndFrom == ListView_GetHeader(hwnd) && nm->code == NM_CUSTOMDRAW) {
            NMCUSTOMDRAW *draw = (NMCUSTOMDRAW *)lp;
            if (draw->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
                HDITEMW item = {0}; WCHAR text[128]; RECT rc = draw->rc;
                item.mask = HDI_TEXT | HDI_FORMAT; item.pszText = text; item.cchTextMax = ARRAYSIZE(text);
                Header_GetItem(nm->hwndFrom, (int)draw->dwItemSpec, &item);
                UI_Fill(draw->hdc, &rc, RGB(237, 242, 249));
                rc.left += DPX(10); rc.right -= DPX(10);
                if (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
                    RECT arrow = rc; arrow.left = arrow.right - DPX(12);
                    UI_Text(draw->hdc, item.fmt & HDF_SORTUP ? L"\x2191" : L"\x2193", arrow, 1, UI_BLUE, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                    rc.right -= DPX(15);
                }
                UI_Text(draw->hdc, text, rc, 1, UI_MUTED, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS |
                    ((item.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT));
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ListStyleProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void UI_StyleList(HWND list)
{
    SetWindowTheme(list, L"Explorer", NULL);
    ListView_SetBkColor(list, UI_SURFACE);
    ListView_SetTextBkColor(list, UI_SURFACE);
    ListView_SetTextColor(list, UI_INK);
    SetWindowSubclass(list, ListStyleProc, 80, g_dpi);
}

BOOL UI_ControlNotify(NMHDR *nm, LRESULT *result)
{
    WCHAR cls[32];
    if (nm->code != NM_CUSTOMDRAW) return FALSE;
    GetClassNameW(nm->hwndFrom, cls, ARRAYSIZE(cls));
    if (!lstrcmpiW(cls, WC_LISTVIEWW)) {
        NMLVCUSTOMDRAW *draw = (NMLVCUSTOMDRAW *)nm;
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) { *result = CDRF_NOTIFYITEMDRAW; return TRUE; }
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            draw->clrText = UI_INK;
            draw->clrTextBk = draw->nmcd.dwItemSpec % 2 ? RGB(248, 250, 253) : UI_SURFACE;
            *result = CDRF_NEWFONT; return TRUE;
        }
    } else if (!lstrcmpiW(cls, L"Button") && nm->idFrom != IDC_PROC_ALLUSERS) {
        NMCUSTOMDRAW *draw = (NMCUSTOMDRAW *)nm;
        if (draw->dwDrawStage == CDDS_PREPAINT) {
            RECT rc = draw->rc;
            WCHAR text[128];
            BOOL enabled = IsWindowEnabled(nm->hwndFrom);
            BOOL primary = nm->idFrom == IDM_FILE_NEWTASK || nm->idFrom == IDC_APPS_NEWTASK;
            BOOL danger = nm->idFrom == IDC_PROC_ENDPROCESS || nm->idFrom == IDC_APPS_ENDTASK;
            COLORREF fill = primary ? UI_BLUE : UI_SURFACE;
            COLORREF ink = primary ? UI_SURFACE : danger ? RGB(186, 62, 75) : UI_INK;
            if (draw->uItemState & CDIS_HOT) fill = primary ? RGB(37, 89, 211) : RGB(235, 242, 253);
            if (draw->uItemState & CDIS_SELECTED) fill = primary ? RGB(29, 72, 183) : RGB(217, 228, 247);
            if (!enabled) { fill = UI_BG; ink = UI_MUTED; }
            UI_Fill(draw->hdc, &rc, GetDlgCtrlID(GetParent(nm->hwndFrom)) == IDC_DASHBOARD ? UI_NAVY : UI_BG);
            UI_Card(draw->hdc, &rc, fill, primary ? fill : UI_LINE);
            GetWindowTextW(nm->hwndFrom, text, ARRAYSIZE(text));
            /* Use the native font and mnemonic processing for buttons.
               Restore the previous font before returning: g_hFont is
               deleted and recreated on a DPI or font change, and leaving a
               deleted object selected into a DC is undefined. */
            SetBkMode(draw->hdc, TRANSPARENT); SetTextColor(draw->hdc, ink);
            {
                HGDIOBJ oldFont = SelectObject(draw->hdc, g_hFont);
                DrawTextW(draw->hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                if (draw->uItemState & CDIS_FOCUS) { InflateRect(&rc, -DPX(3), -DPX(3)); DrawFocusRect(draw->hdc, &rc); }
                if (oldFont) SelectObject(draw->hdc, oldFont);
            }
            *result = CDRF_SKIPDEFAULT; return TRUE;
        }
    }
    return FALSE;
}

static void DashboardPaint(HWND hwnd, HDC dc)
{
    RECT rc, r, header;
    Snapshot snap;
    const Snapshot *shared = SysInfo_Lock();
    WCHAR value[96], detail[160], machine[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD machineLen = ARRAYSIZE(machine);
    static const WCHAR *labels[] = { L"CPU UTILIZATION", L"PHYSICAL MEMORY", L"COMMIT CHARGE", L"SYSTEM ACTIVITY" };
    static const COLORREF colors[] = { UI_BLUE, UI_VIOLET, UI_TEAL, RGB(203, 133, 34) };
    int i, gap = DPX(12), pad = DPX(20), top = DPX(86), cardWidth;
    BOOL compact;
    snap = *shared; SysInfo_Unlock();
    GetClientRect(hwnd, &rc); UI_Fill(dc, &rc, UI_BG);
    compact = rc.bottom < DPX(190);
    header = rc; header.bottom = DPX(74); UI_Fill(dc, &header, UI_NAVY);
    r = (RECT){pad, DPX(10), rc.right - DPX(350), DPX(27)};
    UI_Text(dc, L"WORKSPACE  /  LOCAL SYSTEM", r, 0, RGB(141, 164, 199), DT_SINGLELINE);
    r.top = DPX(29); r.bottom = DPX(62);
    UI_Text(dc, L"Task Manager", r, 3, UI_SURFACE, DT_SINGLELINE | DT_VCENTER);
    if (GetComputerNameW(machine, &machineLen) && rc.right > DPX(920)) {
        r = (RECT){DPX(255), DPX(39), rc.right - DPX(430), DPX(58)};
        UI_Text(dc, machine, r, 0, RGB(163, 182, 208), DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    cardWidth = (rc.right - 2 * pad - 3 * gap) / 4;
    for (i = 0; i < 4; ++i) {
        RECT card = {pad + i * (cardWidth + gap), top, pad + i * (cardWidth + gap) + cardWidth, rc.bottom - DPX(10)};
        RECT label = card;
        if (card.right <= card.left) continue;
        UI_Card(dc, &card, UI_SURFACE, UI_LINE);
        label.left += DPX(14); label.top += DPX(11); label.right -= DPX(10); label.bottom = label.top + DPX(16);
        UI_Text(dc, labels[i], label, 0, UI_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
        switch (i) {
        case 0:
            StringCchPrintfW(value, ARRAYSIZE(value), L"%.1f%%", snap.cpuUsage);
            StringCchPrintfW(detail, ARRAYSIZE(detail), L"%u logical processors", snap.cpuCount); break;
        case 1:
            StringCchPrintfW(value, ARRAYSIZE(value), L"%.1f%%", snap.memUsage);
            StringCchPrintfW(detail, ARRAYSIZE(detail), L"%.1f / %.1f GB in use",
                (double)(snap.memTotal - snap.memAvail) / 1073741824.0, (double)snap.memTotal / 1073741824.0); break;
        case 2:
            StringCchPrintfW(value, ARRAYSIZE(value), L"%.1f GB", (double)snap.commitTotal / 1073741824.0);
            StringCchPrintfW(detail, ARRAYSIZE(detail), L"%.1f GB commit limit", (double)snap.commitLimit / 1073741824.0); break;
        default:
            StringCchPrintfW(value, ARRAYSIZE(value), L"%lu", (unsigned long)snap.processCount);
            StringCchPrintfW(detail, ARRAYSIZE(detail), L"Processes  /  %lu threads", (unsigned long)snap.threadCount); break;
        }
        if (!snap.sequence) { lstrcpyW(value, L"\x2014"); lstrcpyW(detail, L"Waiting for first sample"); }
        label.top += DPX(19); label.bottom = label.top + DPX(35);
        UI_Text(dc, value, label, 3, UI_INK, DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!compact) {
            label.top += DPX(39); label.bottom = label.top + DPX(18);
            UI_Text(dc, detail, label, 0, UI_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        r = card; r.left += DPX(14); r.right -= DPX(14); r.top = card.bottom - DPX(7); r.bottom = r.top + DPX(2);
        UI_Fill(dc, &r, RGB(233, 238, 247));
        {
            double pct = i == 0 ? snap.cpuUsage : i == 1 ? snap.memUsage : i == 2 ?
                (snap.commitLimit ? 100.0 * (double)snap.commitTotal / (double)snap.commitLimit : 0) : 100;
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            r.right = r.left + (int)((r.right - r.left) * pct / 100.0);
            UI_Fill(dc, &r, colors[i]);
        }
        if (!compact && i < 2 && cardWidth > DPX(230)) {
            float hist[48];
            if (i == 0) SysInfo_CopyCpuHistory(hist, 48); else SysInfo_CopyMemHistory(hist, 48);
            r = (RECT){card.right - DPX(88), card.top + DPX(32), card.right - DPX(14), card.top + DPX(65)};
            UI_Chart(dc, r, hist, 48, colors[i], FALSE);
        }
    }
}

static LRESULT CALLBACK DashboardProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    switch (msg) {
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps; RECT rc;
        HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
        HBITMAP bmp, old;
        GetClientRect(hwnd, &rc);
        bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        if (mem && bmp) {
            old = SelectObject(mem, bmp); DashboardPaint(hwnd, mem);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        } else DashboardPaint(hwnd, dc);
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_SIZE: {
        int right = LOWORD(lp) - DPX(20);
        MoveWindow(GetDlgItem(hwnd, IDM_FILE_NEWTASK), right - DPX(124), DPX(22), DPX(124), DPX(32), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDM_VIEW_TOGGLEPAUSE), right - DPX(248), DPX(22), DPX(112), DPX(32), TRUE);
        MoveWindow(GetDlgItem(hwnd, IDM_VIEW_REFRESH), right - DPX(340), DPX(22), DPX(80), DPX(32), TRUE);
        InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    case WM_COMMAND: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NOTIFY: {
        LRESULT result;
        if (UI_ControlNotify((NMHDR *)lp, &result)) return result;
        break;
    }
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, DashboardProc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

HWND UI_CreateDashboard(HWND parent)
{
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"System overview",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, 1, 1, parent, (HMENU)(INT_PTR)IDC_DASHBOARD, g_hInst, NULL);
    if (hwnd) {
        SetWindowSubclass(hwnd, DashboardProc, 70, 0);
        UI_CreateButton(hwnd, IDM_VIEW_REFRESH, L"Refresh", 0);
        UI_CreateButton(hwnd, IDM_VIEW_TOGGLEPAUSE, L"Pause updates", 0);
        UI_CreateButton(hwnd, IDM_FILE_NEWTASK, L"+  Run new task", 0);
    }
    return hwnd;
}

void UI_UpdateDashboard(HWND dashboard)
{
    if (!dashboard) return;
    SetWindowTextW(GetDlgItem(dashboard, IDM_VIEW_TOGGLEPAUSE),
        g_cfg.updateSpeed == SPEED_PAUSED ? L"Resume updates" : L"Pause updates");
    InvalidateRect(dashboard, NULL, FALSE);
}

void UI_DrawNavigation(HWND tabs, HDC dc)
{
    /* The labels come from the tab control's own items, which CreatePages
       sets from TabPage::title. A local copy of the names here would drift
       the moment a tab is added, renamed or reordered -- and being indexed
       by TAB_COUNT, drift would mean an out-of-bounds read, not a wrong
       string. Count from the control too, so a page that failed to create
       is skipped rather than painted as a blank card. */
    RECT rc; int i, count = TabCtrl_GetItemCount(tabs);
    int selected = TabCtrl_GetCurSel(tabs);
    GetClientRect(tabs, &rc); UI_Fill(dc, &rc, UI_BG);
    for (i = 0; i < count; ++i) {
        RECT item, label; WCHAR text[64], name[48];
        TCITEMW query;
        ZeroMemory(&query, sizeof(query));
        query.mask = TCIF_TEXT;
        query.pszText = name;
        query.cchTextMax = ARRAYSIZE(name);
        if (!TabCtrl_GetItem(tabs, i, &query)) name[0] = L'\0';
        TabCtrl_GetItemRect(tabs, i, &item);
        item.top = DPX(3); item.bottom = rc.bottom - DPX(2);
        if (i == selected) UI_Card(dc, &item, UI_SURFACE, UI_LINE);
        label = item; label.left += DPX(10); label.right -= DPX(10);
        StringCchPrintfW(text, ARRAYSIZE(text), L"%02d   %s", i + 1, name);
        UI_Text(dc, text, label, 1, i == selected ? UI_BLUE : UI_MUTED,
            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        if (i == selected) {
            RECT line = {item.left + DPX(12), item.bottom - DPX(3), item.right - DPX(12), item.bottom - DPX(1)};
            UI_Fill(dc, &line, UI_BLUE);
            if (GetFocus() == tabs) { InflateRect(&label, -DPX(2), -DPX(6)); DrawFocusRect(dc, &label); }
        }
    }
}
