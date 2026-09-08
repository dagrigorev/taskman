/* Shared visual language for the native controls and live dashboard. */
#ifndef CTM_UI_H
#define CTM_UI_H

#define UI_BG       RGB(243, 246, 251)
#define UI_SURFACE  RGB(255, 255, 255)
#define UI_INK      RGB(27, 39, 62)
#define UI_MUTED    RGB(102, 117, 140)
#define UI_LINE     RGB(223, 229, 239)
#define UI_BLUE     RGB(47, 105, 235)
#define UI_VIOLET   RGB(126, 83, 222)
#define UI_TEAL     RGB(15, 153, 137)
#define UI_NAVY     RGB(20, 32, 55)
/* Threshold shading for the Sensors tab. Tinted backgrounds rather than
   coloured text, so a hot row reads at a glance without the number
   becoming hard to read against it. */
#define UI_WARN     RGB(255, 241, 217)
#define UI_CRIT     RGB(255, 218, 218)

void UI_ThemeInit(void);
void UI_ThemeDestroy(void);
HBRUSH UI_BackgroundBrush(void);
void UI_Fill(HDC dc, const RECT *rc, COLORREF color);
void UI_Text(HDC dc, const WCHAR *text, RECT rc, int size, COLORREF color, UINT flags);
void UI_Card(HDC dc, const RECT *rc, COLORREF fill, COLORREF border);
/* Chart drawing. The strokes and the area under them are antialiased through
   GDI+ when it is available, and fall back to plain GDI when it is not. */
void UI_Chart(HDC dc, RECT rc, const float *samples, int count, COLORREF color, BOOL grid);
/* Just the trace: same sample mapping as UI_Chart, no surface, grid or fill.
   For overlaying a second series on a chart that is already drawn. */
void UI_ChartLine(HDC dc, RECT rc, const float *samples, int count, COLORREF color, int width);
/* Antialiased open polyline through points already in device coordinates. */
void UI_Polyline(HDC dc, const POINT *points, int count, COLORREF color, int width);
/* Releases GDI+ if it was ever started. Call once, on the way out. */
void UI_GfxShutdown(void);
BOOL UI_ControlNotify(NMHDR *nm, LRESULT *result);
void UI_StyleList(HWND list);
HWND UI_CreateDashboard(HWND parent);
void UI_UpdateDashboard(HWND dashboard);
void UI_DrawNavigation(HWND tabs, HDC dc);
#endif
