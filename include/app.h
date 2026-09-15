/* ------------------------------------------------------------------------
 * app.h - Classic Task Manager : shared declarations
 * Win32 only. Compiled as UNICODE. C11.
 * ------------------------------------------------------------------------ */
#ifndef CTM_APP_H
#define CTM_APP_H

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <psapi.h>
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "resource.h"

/* TABP_BODY lives in <vssym32.h>, which is not present in every toolchain. */
#ifndef TABP_BODY
#define TABP_BODY 10
#endif

/* ---------------------------------------------------------------- tabs -- */
enum {
    TAB_APPS = 0,
    TAB_PROCESSES,
    TAB_SERVICES,
    TAB_PERFORMANCE,
    TAB_NETWORKING,
    TAB_USERS,
    TAB_SENSORS,
    TAB_COUNT
};

/* ------------------------------------------------------------ messages -- */
#define WM_APP_SNAPSHOT_READY   (WM_APP + 1)
#define WM_APP_TRAY             (WM_APP + 2)

/* ------------------------------------------------------- update speeds -- */
enum { SPEED_HIGH = 0, SPEED_NORMAL, SPEED_LOW, SPEED_PAUSED };

/* ------------------------------------------------------------ settings -- */
typedef struct {
    RECT  rcNormal;             /* restored window rect, normal mode        */
    RECT  rcTiny;               /* restored window rect, tiny footprint     */
    BOOL  haveNormalRect;
    BOOL  haveTinyRect;
    BOOL  maximized;
    BOOL  tiny;                 /* tiny footprint mode active               */
    int   activeTab;
    int   updateSpeed;          /* SPEED_*                                   */
    BOOL  alwaysOnTop;
    BOOL  minimizeOnUse;
    BOOL  hideWhenMinimized;
    BOOL  showFullAccountName;
    BOOL  showAllUsers;
    int   appsViewMode;         /* LV_VIEW_ICON / SMALLICON / DETAILS        */
    BOOL  perfOneGraphPerCpu;
    BOOL  perfShowKernelTimes;
    int   netHistoryMode;       /* 0 = total, 1 = sent, 2 = received         */
    BOOL  procTreeMode;         /* Processes tab shows a hierarchy          */
} Settings;

extern Settings g_cfg;

void  Settings_Load(void);
void  Settings_Save(void);

/* Generic registry helpers under HKCU\Software\ClassicTaskManager. */
BOOL  Reg_GetDword (const WCHAR *name, DWORD *value);
void  Reg_SetDword (const WCHAR *name, DWORD value);
BOOL  Reg_GetBinary(const WCHAR *name, void *buf, DWORD cb);
void  Reg_SetBinary(const WCHAR *name, const void *buf, DWORD cb);
BOOL  Reg_GetString(const WCHAR *name, WCHAR *buf, DWORD cch);
void  Reg_SetString(const WCHAR *name, const WCHAR *value);
BOOL  Reg_GetMultiString(const WCHAR *name, WCHAR *buf, DWORD cch);
void  Reg_SetMultiString(const WCHAR *name, const WCHAR *buf, DWORD cch);

/* ------------------------------------------------------------ snapshot -- */
#define CTM_HISTORY 1024        /* samples kept for the history graphs      */
#define CTM_MAX_CPUS 256

typedef struct {
    ULONG64   sequence;
    UINT      cpuCount;
    double    cpuUsage;         /* 0..100                                    */
    double    cpuKernel;        /* 0..100, kernel component of cpuUsage      */
    ULONGLONG memTotal;         /* bytes                                     */
    ULONGLONG memAvail;
    ULONGLONG memCached;
    ULONGLONG memFree;
    ULONGLONG commitTotal;
    ULONGLONG commitLimit;
    ULONGLONG kernelPaged;
    ULONGLONG kernelNonPaged;
    double    memUsage;         /* 0..100                                    */
    DWORD     processCount;
    DWORD     threadCount;
    DWORD     handleCount;
    ULONGLONG upTimeMs;
} Snapshot;

BOOL  SysInfo_Start(HWND notify);
void  SysInfo_Stop(void);
/* TRUE once shutdown has been requested. Long-running per-tab collectors
   poll this so SysInfo_Stop's join does not wait out their whole budget. */
BOOL  SysInfo_Stopping(void);
void  SysInfo_SetSpeed(int speed);          /* SPEED_*                       */
void  SysInfo_RefreshNow(void);
void  SysInfo_SetActiveTab(int tab);
int   SysInfo_ActiveTab(void);
void  SysInfo_SetTabCollector(void (*collect)(int tab)); /* before Start only */
const Snapshot *SysInfo_Lock(void);         /* shared read lock              */
void  SysInfo_Unlock(void);
/* Copies the newest 'count' CPU samples (0..100) into dst, oldest first.
   Returns the number of valid samples actually collected so far.           */
int   SysInfo_CopyCpuHistory(float *dst, int count);
int   SysInfo_CopyMemHistory(float *dst, int count);
int   SysInfo_CopyKernelHistory(float *dst, int count);
UINT  SysInfo_CpuHistoryCount(void);
int   SysInfo_CopyProcessorHistory(UINT cpu, float *busy, float *kernel, int count);

/* ------------------------------------------------------------ tab page -- */
typedef struct TabPage TabPage;

struct TabPage {
    const WCHAR *title;
    HWND         hwnd;          /* the page dialog                           */
    int          index;

    void (*OnCreate)     (TabPage *p);
    void (*OnDestroy)    (TabPage *p);
    void (*OnLayout)     (TabPage *p, int cx, int cy, BOOL tiny);
    void (*OnSnapshot)   (TabPage *p);
    void (*OnCommand)    (TabPage *p, int id, int code, HWND ctl);
    BOOL (*OnNotify)     (TabPage *p, NMHDR *nm, LRESULT *result);
    void (*OnActivate)   (TabPage *p, BOOL active);
    void (*OnFontChanged)(TabPage *p);
    void (*OnContextMenu)(TabPage *p, HWND from, int x, int y);
    void (*BuildViewMenu)(TabPage *p, HMENU hView);   /* append tab items    */
    void (*InitViewMenu) (TabPage *p, HMENU hView);   /* refresh check marks */
    HWND (*PrimaryControl)(TabPage *p);               /* tiny footprint      */
};

TabPage *TabApps(void);
TabPage *TabProcesses(void);
TabPage *TabServices(void);
TabPage *TabPerformance(void);
TabPage *TabNetworking(void);
TabPage *TabUsers(void);
TabPage *TabSensors(void);

void Proc_Collect(void);
void Proc_Reset(void);
void Proc_SelectPid(DWORD pid);

/* Test-only introspection: lets the workspace test fixture (a separate
   translation unit, real Win32 controls, cannot see tab_processes.c's
   file-static model) observe process-tree state it cannot otherwise reach.
   Not used by any production code path. */
BOOL  ProcTest_FindChildWithParent(DWORD *childPid, DWORD *parentPid);
BOOL  ProcTest_IsVisiblePid(DWORD pid);
DWORD ProcTest_PendingPid(void);
/* Builds the Select Columns popup without showing it. TrackPopupMenu
   blocks on user input, so the fixture cannot reach the menu any other
   way. The caller owns the returned menu. */
HMENU ProcTest_BuildColumnMenu(void);
/* Per-CPU grid geometry shared by painting and blame hit testing. */
void  PerfTest_CellRect(const RECT *rc, UINT cpus, UINT cpu, RECT *cell);
int   PerfTest_CellAt(const RECT *rc, UINT cpus, int x, int y);
void Apps_Collect(void);
void Apps_Reset(void);
void Svc_Collect(void);
void Svc_Reset(void);
void Users_Collect(void);
void Users_Reset(void);
void Net_Collect(void);
void Net_Reset(void);

/* ------------------------------------------------------- app globals ---- */
extern HINSTANCE g_hInst;
extern HWND      g_hMain;
extern HFONT     g_hFont;
extern UINT      g_dpi;

#define DPX(v) MulDiv((v), (int)g_dpi, 96)

/* ------------------------------------------------------------ helpers --- */

/* Many Win32 structures declare read-only string members as LPWSTR
   (LVCOLUMNW::pszText, TCITEMW::pszText, LVITEMW::pszText on the way in).
   Routing the cast through UINT_PTR documents that the callee only reads,
   and keeps -Wcast-qual clean without disabling it for real defects. */
static inline LPWSTR UI_Str(const WCHAR *text)
{
    return (LPWSTR)(UINT_PTR)text;
}

HWND  UI_CreateListView(HWND parent, int id, DWORD extraStyle);
void  UI_AddColumn(HWND lv, int index, const WCHAR *text, int widthDlu, int fmt);
HWND  UI_CreateButton(HWND parent, int id, const WCHAR *text, DWORD extra);
HWND  UI_CreateStatic(HWND parent, int id, const WCHAR *text, DWORD extra);
HWND  UI_CreateGroupBox(HWND parent, int id, const WCHAR *text);
void  UI_ApplyFont(HWND parent);
SIZE  UI_ButtonSize(HWND page);
int   UI_Margin(HWND page);
int   UI_LineHeight(HWND page);
/* Right-aligns 'count' buttons on the bottom edge of a page. */
void  UI_PlaceButtonRow(HWND page, const int *ids, int count, int cx, int cy);
void  UI_SetHeaderSortArrow(HWND lv, int column, int direction /* -1,0,1 */);
void  UI_FormatSize(ULONGLONG bytes, WCHAR *buf, size_t cch);
void  UI_FormatKB(ULONGLONG bytes, WCHAR *buf, size_t cch);
void  UI_FormatNumber(ULONGLONG value, WCHAR *buf, size_t cch);

/* main.c services used by tab modules */
void  App_SetStatusText(int part, const WCHAR *text);
void  App_MinimizeOnUse(void);
BOOL  App_RunTaskDialog(HWND owner);
void  App_RelaunchElevated(void);
BOOL  App_OpenSystemTool(HWND owner, BOOL services);
BOOL  App_IsElevated(void);
HWND  App_TabControl(void);
void  App_ShowProcess(DWORD pid);
void  App_ReportError(HWND owner, const WCHAR *operation, DWORD error);

#endif /* CTM_APP_H */
