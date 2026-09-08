/* ------------------------------------------------------------------------
 * tab_sensors.c - the Sensors tab: GPU adapters now, temperatures later.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ui.h"
#include "gpu.h"

static HWND s_graphHost, s_list;

/* ----------------------------------------------------------------- UI ---- */

static void SensCreate(TabPage *p)
{
    s_graphHost = UI_CreateStatic(p->hwnd, IDC_SENS_GRAPHHOST, L"", SS_BLACKRECT);
    s_list = UI_CreateListView(p->hwnd, IDC_SENS_LIST, 0);
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
    (void)p;
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
    SensSnapshot, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL,
    SensPrimary
};

TabPage *TabSensors(void) { return &s_page; }
