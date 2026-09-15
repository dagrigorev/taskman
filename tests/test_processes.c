#include "../include/app.h"
#include "../include/ntapi.h"
#include <stdio.h>
#include "../include/ui.h"

Settings g_cfg;
HINSTANCE g_hInst;
HWND g_hMain;
HFONT g_hFont;
UINT g_dpi = 96;
HWND UI_CreateListView(HWND p,int id,DWORD s) { (void)p;(void)id;(void)s;return NULL; }
void UI_AddColumn(HWND h,int i,const WCHAR *t,int w,int f) { (void)h;(void)i;(void)t;(void)w;(void)f; }
HWND UI_CreateButton(HWND h,int i,const WCHAR *t,DWORD s) { (void)h;(void)i;(void)t;(void)s;return NULL; }
HWND UI_CreateStatic(HWND h,int i,const WCHAR *t,DWORD s) { (void)h;(void)i;(void)t;(void)s;return NULL; }
void UI_Fill(HDC d,const RECT *r,COLORREF c) { (void)d;(void)r;(void)c; }
void UI_Card(HDC d,const RECT *r,COLORREF c,COLORREF b) { (void)d;(void)r;(void)c;(void)b; }
void UI_Text(HDC d,const WCHAR *t,RECT r,int s,COLORREF c,UINT f) { (void)d;(void)t;(void)r;(void)s;(void)c;(void)f; }
void UI_Polyline(HDC d,const POINT *p,int n,COLORREF c,int w) { (void)d;(void)p;(void)n;(void)c;(void)w; }
int UI_Margin(HWND h) { (void)h;return 8; }
SIZE UI_ButtonSize(HWND h) { SIZE s={70,24};(void)h;return s; }
void UI_PlaceButtonRow(HWND h,const int *ids,int n,int x,int y) { (void)h;(void)ids;(void)n;(void)x;(void)y; }
void UI_SetHeaderSortArrow(HWND h,int c,int d) { (void)h;(void)c;(void)d; }
void UI_FormatKB(ULONGLONG b,WCHAR *s,size_t n) { StringCchPrintfW(s,n,L"%llu",b/1024); }
void UI_FormatSize(ULONGLONG b,WCHAR *s,size_t n) { StringCchPrintfW(s,n,L"%llu",b); }
void App_ReportError(HWND h,const WCHAR *op,DWORD e) { (void)h;(void)op;(void)e; }
BOOL App_IsElevated(void) { return FALSE; }
void App_RelaunchElevated(void) {}
void SysInfo_RefreshNow(void) {}
static BOOL noNative;
PFN_NtQuerySystemInformation Nt_QuerySystemInformation(void)
{
    return noNative ? NULL : (PFN_NtQuerySystemInformation)(void *)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtQuerySystemInformation");
}
/* Stands in for the collector's shared enumeration. Serves whatever the
   test staged, once, the way the real one is handed out once per sample. */
static BYTE *borrowBuf;
static ULONG borrowUsed;
static int borrowTakes;
BOOL Blame_TakeListing(const BYTE **base, ULONG *used)
{
    if (!borrowBuf) return FALSE;
    *base = borrowBuf; *used = borrowUsed;
    borrowBuf = NULL;
    ++borrowTakes;
    return TRUE;
}
#include "../src/tabs/tab_processes.c"
static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void TestVisibleMapping(void)
{
    /* With no tree state built, display row N must map to view row N so the
       flat path is unaffected. */
    g_viewCnt = 3;
    g_visibleCnt = 0;
    CHECK(ProcVisibleRow(0) == 0);
    CHECK(ProcVisibleRow(2) == 2);
    CHECK(ProcVisibleRow(-1) < 0);
    CHECK(ProcVisibleRow(99) < 0);
}

static ProcRow MakeProcRow(DWORD pid, DWORD parentPid, ULONGLONG createTime)
{
    ProcRow row;
    ZeroMemory(&row, sizeof(row));
    row.pid = pid; row.parentPid = parentPid; row.createTime = createTime;
    row.memoryKnown = TRUE;
    return row;
}

/* A search for a child must not inflate the process count with the parents
   that were only added to give it context, nor export them. */
static void TestContextExcludedFromCountAndExport(void)
{
    BOOL matches[3];
    int i, counted = 0, exported = 0;

    free(g_view);
    g_view = (ProcRow *)calloc(3, sizeof(ProcRow));
    CHECK(g_view != NULL);
    if (!g_view) return;
    g_viewCnt = 3;
    g_view[0] = MakeProcRow(10, 0,  100);
    g_view[1] = MakeProcRow(11, 10, 200);
    g_view[2] = MakeProcRow(12, 11, 300);
    matches[0] = FALSE; matches[1] = FALSE; matches[2] = TRUE;

    g_cfg.procTreeMode = TRUE;
    CHECK(ProcBuildTree(matches));
    CHECK(g_viewCnt == 3);

    for (i = 0; i < g_viewCnt; ++i) if (!g_tree[i].context) ++counted;
    CHECK(counted == 1);                 /* only the match is a process */

    for (i = 0; i < g_orderedCnt; ++i)
        if (!g_tree[g_ordered[i]].context) ++exported;
    CHECK(exported == 1);                /* and only it is exported */

    /* Everything is still reachable in tree order for display. */
    CHECK(g_orderedCnt == 3);
}

/* If tree mode is on but the tree could not be built (matches allocation
   failed, or ProcBuildTree itself failed), ProcSnapshot must degrade to the
   same clean flat state the "tree mode off" path produces -- not leave
   stale g_tree/g_ordered/g_visible from a previous, differently-sized
   snapshot paired with a freshly replaced g_view. Drives that fallback
   directly: seeds g_shared with a known row count, forces g_tree/counts
   into a stale non-NULL state first, then runs the real ProcSnapshot code
   path (tree mode off, so it takes the flat/fallback branch) and checks the
   state it leaves behind. */
static void TestTreeFallbackDegradesCleanly(void)
{
    const int N = 4;
    ProcRow *seed;
    int i;

    seed = (ProcRow *)calloc((size_t)N, sizeof(ProcRow));
    CHECK(seed != NULL);
    if (!seed) return;
    for (i = 0; i < N; ++i) seed[i] = MakeProcRow(100 + (DWORD)i, 0, 1);

    AcquireSRWLockExclusive(&g_procLock);
    free(g_shared);
    g_shared = seed;
    g_sharedCnt = N;
    ReleaseSRWLockExclusive(&g_procLock);

    /* Stale tree state from a differently-sized previous snapshot. */
    free(g_tree);
    g_tree = (ProcTreeInfo *)calloc(9, sizeof(ProcTreeInfo));
    CHECK(g_tree != NULL);
    free(g_ordered); g_ordered = (int *)malloc(9 * sizeof(int));
    free(g_visible); g_visible = (int *)malloc(9 * sizeof(int));
    g_orderedCnt = 9;
    g_visibleCnt = 9;
    g_viewCnt = 9;

    g_cfg.procTreeMode = FALSE;   /* drives the same fallback branch */
    g_cfg.showAllUsers = TRUE;    /* keep all seeded rows past the user filter */
    s_query[0] = 0; s_filterMode = 0;

    ProcSnapshot(NULL);   /* s_list is NULL here, so no ListView calls occur */

    CHECK(g_tree == NULL);
    CHECK(g_ordered == NULL);
    CHECK(g_visible == NULL);
    CHECK(g_orderedCnt == 0);
    CHECK(g_visibleCnt == 0);
    CHECK(g_viewCnt == N);

    for (i = 0; i < g_viewCnt; ++i) CHECK(ProcVisibleRow(i) == i);
    CHECK(ProcVisibleRow(-1) < 0);
    CHECK(ProcVisibleRow(g_viewCnt) < 0);

    AcquireSRWLockExclusive(&g_procLock);
    free(g_shared); g_shared = NULL; g_sharedCnt = 0;
    ReleaseSRWLockExclusive(&g_procLock);
    g_cfg.showAllUsers = FALSE;
}

/* Go to Process from the Services tab must not scroll to a row that is
   present in the model but hidden under a collapsed ancestor: it must
   expand every collapsed ancestor of the target instead. Builds a 3-deep
   chain (grandparent -> parent -> leaf), collapses the grandparent, then
   checks ProcExpandAncestors on the leaf's model index clears it. */
static void TestSelectPidExpandsAncestors(void)
{
    ProcRow          *savedView       = g_view;
    int               savedViewCnt    = g_viewCnt;
    ProcTreeInfo     *savedTree       = g_tree;
    int              *savedOrdered    = g_ordered;
    int               savedOrderedCnt = g_orderedCnt;
    int              *savedVisible    = g_visible;
    int               savedVisibleCnt = g_visibleCnt;
    ProcCollapseSet  *savedCollapse   = g_collapse;
    BOOL              savedTreeMode   = g_cfg.procTreeMode;
    DWORD             savedPending    = s_pendingPid;

    g_tree = NULL; g_ordered = NULL; g_visible = NULL; g_collapse = NULL;
    CHECK(ProcExpandAncestors(0) == FALSE);   /* no tree: nothing to do */

    g_view = (ProcRow *)calloc(3, sizeof(ProcRow));
    CHECK(g_view != NULL);
    if (g_view) {
        g_viewCnt = 3;
        g_view[0] = MakeProcRow(1, 0, 10);   /* grandparent */
        g_view[1] = MakeProcRow(2, 1, 20);   /* parent */
        g_view[2] = MakeProcRow(3, 2, 30);   /* leaf */
        g_cfg.procTreeMode = TRUE;
        CHECK(ProcBuildTree(NULL));   /* no search context filtering: keep the full chain */
        CHECK(g_collapse != NULL);
        if (g_collapse) {
            ProcCollapse_Toggle(g_collapse, g_view[0].pid, g_view[0].createTime);
            CHECK(ProcCollapse_Contains(g_collapse, g_view[0].pid, g_view[0].createTime));
            CHECK(ProcExpandAncestors(2) == TRUE);
            CHECK(!ProcCollapse_Contains(g_collapse, g_view[0].pid, g_view[0].createTime));
        }
    }

    free(g_view);
    free(g_tree);
    free(g_ordered);
    free(g_visible);
    ProcCollapse_Destroy(g_collapse);

    g_view       = savedView;       g_viewCnt    = savedViewCnt;
    g_tree       = savedTree;
    g_ordered    = savedOrdered;    g_orderedCnt = savedOrderedCnt;
    g_visible    = savedVisible;    g_visibleCnt = savedVisibleCnt;
    g_collapse   = savedCollapse;
    g_cfg.procTreeMode = savedTreeMode;
    s_pendingPid = savedPending;
}

static void TestGpuStampLeavesUnknownRowsUnknown(void)
{
    /* With collection off, nothing has published per-process figures, so
       every row must read unknown rather than a confident zero -- that is
       what makes the column render an empty cell instead of "0.0%". */
    ProcRow row;
    ZeroMemory(&row, sizeof(row));
    row.pid = 0xFFFFFFFEu;      /* cannot be a live pid */
    row.gpuPct = 42.0f;         /* stale value from a previous sample */
    row.gpuKnown = TRUE;

    Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, FALSE);
    Gpu_Reset();
    ProcStampGpu(&row);

    CHECK(row.gpuKnown == FALSE);
    CHECK(row.gpuPct == 0.0f);  /* cleared, not left stale */
}

int main(void)
{
    PrevCpu prev = {1,1,100,200};
    ProcRow row = {0};
    FILETIME c,e,k,u;
    INITCOMMONCONTROLSEX controls = {sizeof(controls),ICC_LISTVIEW_CLASSES};
    HWND parent;

    TestVisibleMapping();
    TestContextExcludedFromCountAndExport();
    TestTreeFallbackDegradesCleanly();
    TestSelectPidExpandsAncestors();
    TestGpuStampLeavesUnknownRowsUnknown();
    /* Reset state the rest of main starts fresh from. */
    free(g_view); g_view = NULL; g_viewCnt = 0;
    free(g_tree); g_tree = NULL;
    free(g_ordered); g_ordered = NULL; g_orderedCnt = 0;
    free(g_visible); g_visible = NULL; g_visibleCnt = 0;
    g_firstRoot = -1;
    g_cfg.procTreeMode = FALSE;
    /* Search combines case-insensitive terms across fields and exact pid: tokens. */
    lstrcpyW(row.imageName, L"Code.exe");
    lstrcpyW(row.userName, L"DESKTOP\\Alice");
    lstrcpyW(row.description, L"Visual Studio Code");
    row.pid = 1234; row.cpuPct = 2.5f;
    row.privateBytes = 200ULL * 1024 * 1024; row.memoryKnown = TRUE;
    CHECK(ProcMatches(&row, L"", 0));
    CHECK(ProcMatches(&row, L"  CODE alice  ", 0));
    CHECK(ProcMatches(&row, L"studio pid:1234", 0));
    CHECK(!ProcMatches(&row, L"pid:123", 0));
    CHECK(!ProcMatches(&row, L"pid:", 0));
    CHECK(!ProcMatches(&row, L"code missing", 0));
    CHECK(ProcMatches(&row, L"", 1));
    CHECK(ProcMatches(&row, L"", 2));
    row.cpuPct = 0; row.memoryKnown = FALSE;
    CHECK(!ProcMatches(&row, L"", 1));
    CHECK(!ProcMatches(&row, L"", 2));
    {
        /* "Changed since mark" keeps new and grown rows only, and nothing
           at all until a mark exists. The summary names what exited. */
        ProcRow marked[2], now;
        ProcMarkEntry gone[2];
        ProcDiffCounts counts;
        FILETIME wall;
        SYSTEMTIME st = {0};
        WCHAR text[512];
        CHECK(!ProcMatches(&row, L"", 3));
        ZeroMemory(marked, sizeof(marked));
        marked[0].pid = 1234; marked[0].createTime = 5;
        marked[0].privateBytes = 10ULL * 1024 * 1024; marked[0].memoryKnown = TRUE;
        lstrcpyW(marked[0].imageName, L"Code.exe");
        marked[1].pid = 77; marked[1].createTime = 1;
        lstrcpyW(marked[1].imageName, L"gone.exe");
        st.wYear = 2026; st.wMonth = 9; st.wDay = 15; st.wHour = 12; st.wMinute = 3; st.wSecond = 44;
        SystemTimeToFileTime(&st, &wall);
        CHECK(ProcDiff_Take(&s_mark, marked, 2, wall));
        now = marked[0];
        CHECK(!ProcMatches(&now, L"", 3));                       /* unchanged */
        now.privateBytes += PROC_DIFF_GREW_BYTES;
        CHECK(ProcMatches(&now, L"", 3));                        /* grew */
        CHECK(!ProcMatches(&now, L"missing", 3));                /* search still applies */
        now = marked[0]; now.createTime = 6;
        CHECK(ProcMatches(&now, L"", 3));                        /* new */

        counts.started = 1; counts.exited = 1; counts.grew = 2;
        gone[0] = s_mark.entries[0];                             /* pid 77 sorts first */
        ProcMarkSummary(text, ARRAYSIZE(text), &counts, gone, 1, L"12:03:44");
        CHECK(!lstrcmpW(text, L"  |  Since 12:03:44: 1 started, 1 exited (gone.exe), 2 grew"));
        counts.exited = 4;
        gone[1] = s_mark.entries[1];
        ProcMarkSummary(text, ARRAYSIZE(text), &counts, gone, 2, L"12:03:44");
        CHECK(!lstrcmpW(text, L"  |  Since 12:03:44: 1 started, 4 exited (gone.exe, Code.exe +2 more), 2 grew"));
        counts.started = counts.exited = counts.grew = 0;
        ProcMarkSummary(text, ARRAYSIZE(text), &counts, gone, 0, L"12:03:44");
        CHECK(!lstrcmpW(text, L"  |  Since 12:03:44: no changes"));
        {
            /* The inspector's "since mark" value. This fixture's
               UI_FormatSize prints raw byte counts. */
            ProcRow grown = marked[0];
            grown.handles = 300;
            grown.privateBytes += PROC_DIFF_GREW_BYTES;
            ProcMarkDelta(&grown, text, ARRAYSIZE(text));
            CHECK(!lstrcmpW(text, L"+16777216 private, +300 handles"));
            grown.privateBytes = 1024;
            grown.handles = 0;
            ProcMarkDelta(&grown, text, ARRAYSIZE(text));
            CHECK(!lstrcmpW(text, L"-10484736 private, +0 handles"));
            grown.memoryKnown = FALSE;
            ProcMarkDelta(&grown, text, ARRAYSIZE(text));
            CHECK(!lstrcmpW(text, L"+0 handles"));
            grown.createTime = 99;
            ProcMarkDelta(&grown, text, ARRAYSIZE(text));
            CHECK(!lstrcmpW(text, L"Started after the mark"));
        }
        {
            /* While held, rows keep their last displayed order whatever
               their values do; unseen rows fall behind, in sort order. */
            ProcRow a = marked[0], b = marked[0], c = marked[0];
            ProcTreeInfo ta = {0}, tb = {0};
            a.pid = 1; a.createTime = 1; a.cpuPct = 1.0f;
            b.pid = 2; b.createTime = 2; b.cpuPct = 50.0f;
            c.pid = 3; c.createTime = 3; c.cpuPct = 90.0f;
            g_sortCol = 2; g_sortDir = -1;
            PROC_CMP_COL = 2; PROC_CMP_DIR = -1;
            CHECK(ProcCompare(&b, &a) < 0);              /* busier first, unheld */
            {
                ProcRow shown[2];
                shown[0] = a; shown[1] = b;              /* a was displayed above b */
                CHECK(ProcOrder_Capture(&s_heldOrder, shown, NULL, 2));
            }
            s_holdOrder = TRUE;
            CHECK(ProcCompare(&a, &b) < 0);
            CHECK(ProcTreeCompare(&a, &ta, &b, &tb) < 0);
            CHECK(ProcCompare(&b, &c) < 0);              /* known before unseen */
            CHECK(ProcCompare(&c, &a) > 0);
            s_holdOrder = FALSE;
            CHECK(ProcCompare(&b, &a) < 0);
            CHECK(ProcTreeCompare(&b, &tb, &a, &ta) < 0);
            ProcOrder_Free(&s_heldOrder);
            g_sortCol = 2; g_sortDir = -1;
            {
                /* The hold survives moving between the rows and the
                   scrollbar, and a thumb drag that strays outside, but
                   not a pointer that has really left. */
                RECT win = { 100, 100, 400, 300 };
                POINT inside = { 395, 150 }, outside = { 450, 150 }, edge = { 400, 150 };
                CHECK(ProcHoldKeeps(inside, &win, FALSE));
                CHECK(!ProcHoldKeeps(outside, &win, FALSE));
                CHECK(!ProcHoldKeeps(edge, &win, FALSE));       /* right edge is exclusive */
                CHECK(ProcHoldKeeps(outside, &win, TRUE));      /* dragging the thumb */
            }
        }
        ProcDiff_Free(&s_mark);
        ProcMarkDelta(&now, text, ARRAYSIZE(text));
        CHECK(text[0] == 0);
        CHECK(!ProcMatches(&now, L"", 3));
    }
    {
        WCHAR escaped[128];
        CHECK(ProcCsvField(L"Alice, \"dev\"", escaped, ARRAYSIZE(escaped)));
        CHECK(!wcscmp(escaped, L"\"Alice, \"\"dev\"\"\""));
        CHECK(ProcCsvField(L"=1+2", escaped, ARRAYSIZE(escaped)));
        CHECK(!wcscmp(escaped, L"\"'=1+2\""));
        CHECK(!ProcCsvField(L"too long", escaped, 4));
    }
    {
        HANDLE file = CreateFileW(L"tests/.build/export-fixture.csv", GENERIC_READ | GENERIC_WRITE, 0,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        char csv[1024] = {0}; DWORD read = 0;
        CHECK(file != INVALID_HANDLE_VALUE);
        lstrcpyW(row.description, L"Editor, \"Unicode\" \x0416");
        row.memoryKnown = FALSE;
        CHECK(ProcWriteCsv(file, &row, 1));
        SetFilePointer(file, 0, NULL, FILE_BEGIN);
        CHECK(ReadFile(file, csv, sizeof(csv) - 1, &read, NULL));
        CHECK(read > 3 && (BYTE)csv[0] == 0xef && (BYTE)csv[1] == 0xbb && (BYTE)csv[2] == 0xbf);
        CHECK(strstr(csv, "\"Code.exe\",\"DESKTOP\\Alice\",0.00,,\"Editor, \"\"Unicode\"\" \xd0\x96\",1234\r\n") != NULL);
        CHECK(!ProcWriteCsv(INVALID_HANDLE_VALUE, &row, 1));
        CloseHandle(file);
    }
    CHECK(ProcCpuDelta(110,210,&prev,100) == 20.0f);
    CHECK(ProcCpuDelta(99,210,&prev,100) == 0.0f);
    CHECK(ProcCpuDelta(110,199,&prev,100) == 0.0f);
    CHECK(ProcCpuDelta(110,210,&prev,0) == 0.0f);
    CHECK(ProcCpuDelta(1000,1000,&prev,1) == 100.0f);
    GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);
    CHECK(ProcIdentityMatches(GetCurrentProcess(),ProcFileTime(c)));
    CHECK(!ProcIdentityMatches(GetCurrentProcess(),ProcFileTime(c)+1));
    row.pid = GetCurrentProcessId(); row.createTime=ProcFileTime(c);
    CHECK(!ProcOpenTarget(&row));
    row.pid=4; CHECK(!ProcOpenTarget(&row));
    row.pid=0; CHECK(!ProcOpenTarget(&row));
    Proc_Collect();
    {
        int i; BOOL found=FALSE;
        for (i=0;i<g_sharedCnt;++i) if (g_shared[i].pid == GetCurrentProcessId()) {
            found=TRUE; CHECK(g_shared[i].ownedByCurrentUser);
            CHECK(g_shared[i].createTime == ProcFileTime(c));
            CHECK(g_shared[i].userName[0] != 0);
        }
        CHECK(found);
    }
    Proc_Reset();
    noNative=TRUE;
    Proc_Collect();
    {
        int i; BOOL found=FALSE;
        for (i=0;i<g_sharedCnt;++i) if (g_shared[i].pid==GetCurrentProcessId()) {
            found=TRUE;
            CHECK(g_shared[i].createTime==ProcFileTime(c));
        }
        CHECK(found);
    }
    Proc_Reset(); noNative=FALSE;
    {
        /* A listing taken from the collector is used as is: with the native
           query switched off, native counters can only have come from it.
           The listing is taken once, so the next collection falls back. */
        static BYTE staged[4 * 1024 * 1024];
        ULONG needed = 0;
        int i; BOOL found = FALSE;
        PFN_NtQuerySystemInformation query = Nt_QuerySystemInformation();
        CHECK(query && NT_SUCCESS(query(CtmSystemProcessInformation, staged, sizeof(staged), &needed)));
        noNative = TRUE;
        borrowBuf = staged; borrowUsed = needed; borrowTakes = 0;
        Proc_Collect();
        CHECK(borrowTakes == 1);
        for (i=0;i<g_sharedCnt;++i) if (g_shared[i].pid==GetCurrentProcessId()) {
            found = TRUE;
            CHECK(g_shared[i].countersKnown);
            CHECK(g_shared[i].memoryKnown);
        }
        CHECK(found);
        Proc_Collect();
        CHECK(borrowTakes == 1);
        found = FALSE;
        for (i=0;i<g_sharedCnt;++i) if (g_shared[i].pid==GetCurrentProcessId()) {
            found = TRUE;
            CHECK(!g_shared[i].countersKnown);
        }
        CHECK(found);
        Proc_Reset(); noNative = FALSE;
    }
    InitCommonControlsEx(&controls);
    parent=CreateWindowExW(0,L"STATIC",L"Process test fixture",0,0,0,100,100,NULL,NULL,GetModuleHandleW(NULL),NULL);
    s_list=CreateWindowExW(0,WC_LISTVIEWW,L"",WS_CHILD|LVS_REPORT|LVS_OWNERDATA,
        0,0,100,100,parent,NULL,GetModuleHandleW(NULL),NULL);
    CHECK(s_list != NULL);
    g_shared=calloc(2,sizeof(*g_shared)); g_sharedCnt=2;
    g_shared[0].pid=111; g_shared[0].createTime=1; g_shared[0].ownedByCurrentUser=TRUE;
    g_shared[1].pid=222; g_shared[1].createTime=2;
    ProcSnapshot(NULL); CHECK(g_viewCnt==1);
    ListView_SetItemState(s_list,0,LVIS_SELECTED,LVIS_SELECTED);
    Proc_SelectPid(222);
    ProcSnapshot(NULL);
    CHECK(ListView_GetSelectedCount(s_list)==1);
    CHECK(g_view[ListView_GetNextItem(s_list,-1,LVNI_SELECTED)].pid==222);
    ProcSnapshot(NULL);
    CHECK(g_viewCnt==2);
    CHECK(g_view[ListView_GetNextItem(s_list,-1,LVNI_SELECTED)].pid==222);
    lstrcpyW(s_query, L"nothing-matches");
    ProcSnapshot(NULL); CHECK(g_viewCnt==0);
    Proc_SelectPid(222);
    CHECK(!s_query[0] && g_viewCnt==2); /* Own processes plus the requested cross-user target. */
    CHECK(g_view[ListView_GetNextItem(s_list,-1,LVNI_SELECTED)].pid==222);
    /* A request for a process that no longer exists must be dropped after one
       snapshot. The pending PID is exempt from the user filter, so left set it
       would keep trying to reveal a dead process on every future tick. */
    Proc_SelectPid(999);
    CHECK(s_pendingPid==999);   /* held: it may only be filtered out so far */
    ProcSnapshot(NULL);
    CHECK(s_pendingPid==0);     /* the snapshot proved it is really gone */
    /* 222 is still here, but now only because it is the SELECTED row, which
       the user filter also exempts -- not because of any pending request. */
    CHECK(g_viewCnt==2);
    DestroyWindow(s_list);s_list=NULL;DestroyWindow(parent);
    Proc_Reset();
    printf("processes: %d failures\n",failures);
    return failures ? 1 : 0;
}
