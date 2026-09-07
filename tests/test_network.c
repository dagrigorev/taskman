#include "../src/tabs/tab_network.c"
#include <assert.h>
#include <math.h>
#include <stdio.h>
Settings g_cfg;
int main(void)
{
    NetRow previous = {0}, row = {0};
    assert(fabs(net_percent(62500000, 0, 1000000000, 500) - 100.0) < .01);
    assert(fabs(net_percent(1250000000ULL, 0, 100000000000ULL, 1000) - 10.0) < .01);
    assert(net_percent(0, 100, 1000, 500) == 0);
    assert(net_percent(100, 0, 0, 500) == 0);
    assert(net_percent(100, 0, 1000, 0) == 0);
    previous.connected = TRUE; previous.tick = 1000;
    previous.speedBps = previous.receiveSpeed = 1000000000;
    row = previous; row.tick = 1500; row.bytesIn = 6250000; row.bytesOut = 12500000;
    net_sample(&row, &previous);
    assert(fabs(row.receiveRate - 12500000.0) < .01);
    assert(fabs(row.sendRate - 25000000.0) < .01);
    assert(fabs(net_history(&row, 0, 0) - 30) < .01);
    assert(fabs(net_history(&row, 1, 0) - 20) < .01);
    assert(fabs(net_history(&row, 2, 0) - 10) < .01);
    previous = row; row.tick += 500; row.connected = FALSE;
    net_sample(&row, &previous); assert(net_history(&row, 0, 1) == 0);
    assert(row.receiveRate == 0 && row.sendRate == 0);
    previous = row; row.tick += 500; row.connected = TRUE;
    net_sample(&row, &previous); assert(net_history(&row, 0, 2) == 0);
    previous = row; row.tick += 500; row.bytesIn = 0;
    net_sample(&row, &previous); assert(net_history(&row, 0, 3) == 0);
    ZeroMemory(&row, sizeof(row));
    row.connected = TRUE; row.speedBps = row.receiveSpeed = 800000;
    for (int i = 0; i < NET_HIST_MAX + 20; ++i) {
        previous = row; row.tick += 1000; row.bytesOut += (ULONGLONG)(i % 100) * 1000;
        net_sample(&row, &previous);
    }
    assert(row.count == NET_HIST_MAX);
    assert(fabs(net_history(&row, 1, 0) - 20) < .01);
    assert(fabs(net_history(&row, 1, NET_HIST_MAX - 1) - 47) < .01);
    Net_Collect(); assert(net_error == 0);
    printf("network: counter/reset/disconnect/ring checks; %d live adapters passed\n", g_sharedCnt);
    NetSnapshot(TabNetworking());
    assert(g_viewCnt == g_sharedCnt);
    for (int i = 0; i < g_viewCnt; ++i) assert(g_view[i].count == 1);
    Net_Reset(); assert(!g_shared && !g_view);
    return 0;
}
HINSTANCE g_hInst;
HFONT g_hFont;
UINT g_dpi = 96;
void App_ReportError(HWND h, const WCHAR *s, DWORD e) { (void)h; (void)s; (void)e; }
void App_ShowProcess(DWORD pid) { (void)pid; }
void SysInfo_RefreshNow(void) {}
void App_MinimizeOnUse(void) {}
BOOL App_OpenSystemTool(HWND h, BOOL s) { (void)h; (void)s; return FALSE; }
void UI_ApplyFont(HWND h) { (void)h; }
void UI_Fill(HDC dc, const RECT *r, COLORREF c) { (void)dc; (void)r; (void)c; }
void UI_Polyline(HDC dc, const POINT *points, int count, COLORREF color, int width)
{ (void)dc; (void)points; (void)count; (void)color; (void)width; }
HWND UI_CreateListView(HWND h, int id, DWORD style) { (void)h; (void)id; (void)style; return NULL; }
HWND UI_CreateButton(HWND h, int id, const WCHAR *s, DWORD style) { (void)h; (void)id; (void)s; (void)style; return NULL; }
void UI_AddColumn(HWND h, int index, const WCHAR *s, int w, int f) { (void)h; (void)index; (void)s; (void)w; (void)f; }
int UI_Margin(HWND h) { (void)h; return 8; }
SIZE UI_ButtonSize(HWND h) { SIZE s = {80, 24}; (void)h; return s; }
void UI_PlaceButtonRow(HWND h, const int *ids, int count, int x, int y) { (void)h; (void)ids; (void)count; (void)x; (void)y; }
void UI_SetHeaderSortArrow(HWND h, int c, int d) { (void)h; (void)c; (void)d; }

HWND UI_CreateStatic(HWND h, int id, const WCHAR *s, DWORD style) { (void)h; (void)id; (void)s; (void)style; return NULL; }
