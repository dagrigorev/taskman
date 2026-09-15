/* ------------------------------------------------------------------------
 * tab_processes.c - the Processes tab.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ntapi.h"
#include <tlhelp32.h>
#include <commdlg.h>
#include <shlobj.h>
#include <wctype.h>
#include "ui.h"
#include "proc_tree.h"
#include "proc_diff.h"
#include "gpu.h"
#include "blame.h"

#define PROC_COL_GPU 6

/* ----------------------------------------------------------------- model -- */

/* collector-thread state (no lock needed - worker thread only) */
static BYTE      *s_ntBuf;
static ULONG      s_ntBufSz;
static ULONG      s_ntUsed;
/* The listing Proc_Collect walks: the collector's shared enumeration when
   one was available this sample, otherwise s_ntBuf. Bounds checks use
   these, never s_ntBuf directly. */
static const BYTE *s_listBase;
static ULONG       s_listUsed;

typedef struct { DWORD pid; ULONGLONG createTime; ULONGLONG kernel; ULONGLONG user; } PrevCpu;
static PrevCpu   *s_prev;
static int        s_prevCnt;
static int        s_prevCap;
static ULONGLONG  s_prevTick;

#define USER_CACHE_MAX 4096
typedef struct { DWORD pid; ULONGLONG createTime; WCHAR name[PROC_USER_MAX];
    WCHAR description[PROC_DESC_MAX]; BOOL owned; } UserCache;
static UserCache  s_userCache[USER_CACHE_MAX];
static int        s_userCacheLen;
static int        s_userCacheNext;
static DWORD      s_pendingPid;

/* shared between collector and UI, guarded by g_procLock */
static ProcRow   *g_shared;
static int        g_sharedCnt;
static SRWLOCK    g_procLock = SRWLOCK_INIT;

/* UI-thread-only: no lock needed once copied */
static ProcRow   *g_view;
static int        g_viewCnt;

/* UI-thread-only: process tree state, parallel to g_view. NULL/0 when tree
   mode is off or when a build could not allocate. */
static ProcTreeInfo    *g_tree;        /* parallel to g_view, g_viewCnt long */
static int             *g_ordered;     /* every survivor, tree order          */
static int              g_orderedCnt;
static int             *g_visible;     /* visible rows, tree order            */
static int              g_visibleCnt;
static int              g_firstRoot = -1;  /* head of the root chain          */
static ProcCollapseSet *g_collapse;

/* Maps a ListView display row to its index in g_view. Falls back to the
   identity mapping when no tree is built, so the flat path is unchanged. */
static int ProcVisibleRow(int display)
{
    if (display < 0) return -1;
    if (g_visibleCnt > 0)
        return display < g_visibleCnt ? g_visible[display] : -1;
    return display < g_viewCnt ? display : -1;
}

/* Sort state is owned solely by the UI thread. */
static int g_sortCol = 2;  /* default: CPU descending */
static int g_sortDir = -1;

/* LVP_LISTITEM and the LISS_* item states live in <vssym32.h>, which this
   project deliberately avoids (see the TABP_BODY comment in app.h). Defined
   locally to match the header's values. */
#ifndef LVP_LISTITEM
#define LVP_LISTITEM 1
#endif
#ifndef LISS_NORMAL
#define LISS_NORMAL          1
#define LISS_HOT             2
#define LISS_SELECTED        3
#define LISS_DISABLED        4
#define LISS_SELECTEDNOTFOCUS 5
#define LISS_HOTSELECTED     6
#endif

/* UI state */
static HWND  s_list;
static HTHEME s_listTheme;  /* Explorer::ListView, opened in ProcCreate,
                                closed in ProcDestroy; NULL when unthemed. */
static HWND  s_allUsers;
static HWND  s_endProcess;
static HWND  s_search, s_filter, s_details, s_summary;
static WCHAR s_query[256];
static int s_filterMode;
#define PROC_FILTER_CHANGED 3   /* "Changed since mark"                    */
/* UI thread only. Taken from every collected process, not just the visible
   ones, so toggling "all users" later does not read as starts and exits. */
static ProcMark s_mark;
static BOOL s_refreshing;

static void ProcSnapshot(TabPage *p);
static WCHAR s_markText[400];   /* summary segment, rebuilt each snapshot   */
static void ProcCommand(TabPage *p, int id, int code, HWND ctl);

/* Every term must match a field; pid: is deliberately exact. */
static BOOL ProcMatches(const ProcRow *row, const WCHAR *query, int filter)
{
    WCHAR token[256], pid[24];
    if (filter == 1 && row->cpuPct < 1.0f) return FALSE;
    if (filter == 2 && (!row->memoryKnown || row->privateBytes < 100ULL * 1024 * 1024)) return FALSE;
    if (filter == PROC_FILTER_CHANGED &&
        ProcDiff_Classify(&s_mark, row, NULL, NULL) == PROC_CHANGE_NONE) return FALSE;
    StringCchPrintfW(pid, ARRAYSIZE(pid), L"%lu", (unsigned long)row->pid);
    while (*query) {
        size_t len = 0;
        while (iswspace(*query)) ++query;
        if (!*query) break;
        while (*query && !iswspace(*query)) {
            if (len + 1 >= ARRAYSIZE(token)) return FALSE;
            token[len++] = *query++;
        }
        token[len] = 0;
        if (!_wcsnicmp(token, L"pid:", 4)) {
            if (lstrcmpW(token + 4, pid)) return FALSE;
        } else if (!StrStrIW(row->imageName, token) && !StrStrIW(row->userName, token) &&
                   !StrStrIW(row->description, token) && !StrStrIW(pid, token)) return FALSE;
    }
    return TRUE;
}

/* RFC 4180 quoting, with spreadsheet formula neutralization for text fields. */
static BOOL ProcCsvField(const WCHAR *text, WCHAR *out, size_t capacity)
{
    size_t at = 0;
    const WCHAR *first = text;
    BOOL formula;
    while (*first == L' ') ++first;
    formula = *first && wcschr(L"=+-@\t\r\n", *first) != NULL;
    if (capacity < 3) return FALSE;
    out[at++] = L'"';
    if (formula) out[at++] = L'\'';
    while (*text) {
        if (at + (*text == L'"' ? 2 : 1) + 2 > capacity) { out[0] = 0; return FALSE; }
        if (*text == L'"') out[at++] = L'"';
        out[at++] = *text++;
    }
    if (at + 2 > capacity) { out[0] = 0; return FALSE; }
    out[at++] = L'"'; out[at] = 0;
    return TRUE;
}

/* ------------------------------------------------------ NtQuery helpers -- */

static UINT CpuCount(void)
{
    SYSTEM_INFO si;
    DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (count) return count;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
}

/* Normalize the documented fallback into the same bounded record layout.
   Toolhelp cannot provide private working set; expose that metric as unknown. */
static const CTM_SYSTEM_PROCESS_INFORMATION *ProcToolhelp(void)
{
    typedef struct { CTM_SYSTEM_PROCESS_INFORMATION info; WCHAR name[MAX_PATH]; } Record;
    HANDLE snapshot;
    PROCESSENTRY32W entry;
    Record *records = NULL;
    size_t count = 0, capacity = 0, i;
    DWORD error;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return NULL;
    ZeroMemory(&entry, sizeof(entry)); entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) { CloseHandle(snapshot); return NULL; }
    do {
        Record *record;
        HANDLE process;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 128;
            Record *grown;
            if (next > 65536) goto failed;
            grown = realloc(records, next * sizeof(*records));
            if (!grown) goto failed;
            records = grown; capacity = next;
        }
        record = &records[count++];
        ZeroMemory(record, sizeof(*record));
        record->info.UniqueProcessId = (HANDLE)(ULONG_PTR)entry.th32ProcessID;
        record->info.InheritedFromUniqueProcessId = (HANDLE)(ULONG_PTR)entry.th32ParentProcessID;
        record->info.NumberOfThreads = entry.cntThreads;
        record->info.WorkingSetPrivateSize.QuadPart = -1;
        StringCchCopyW(record->name, ARRAYSIZE(record->name), entry.szExeFile);
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process) {
            FILETIME c,e,k,u;
            if (GetProcessTimes(process,&c,&e,&k,&u)) {
                record->info.CreateTime.QuadPart = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
                record->info.KernelTime.QuadPart = ((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime;
                record->info.UserTime.QuadPart = ((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime;
            }
            CloseHandle(process);
        }
    } while (Process32NextW(snapshot, &entry));
    error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) goto failed;
    CloseHandle(snapshot);
    for (i = 0; i < count; ++i) {
        records[i].info.NextEntryOffset = i + 1 < count ? sizeof(*records) : 0;
        records[i].info.ImageName.Buffer = records[i].name;
        records[i].info.ImageName.Length = (USHORT)((size_t)lstrlenW(records[i].name) * sizeof(WCHAR));
    }
    free(s_ntBuf);
    s_ntBuf = (BYTE *)records;
    s_ntBufSz = (ULONG)(capacity * sizeof(*records));
    s_ntUsed = (ULONG)(count * sizeof(*records));
    s_listBase = s_ntBuf;
    s_listUsed = s_ntUsed;
    return (const CTM_SYSTEM_PROCESS_INFORMATION *)s_ntBuf;
failed:
    CloseHandle(snapshot);
    free(records);
    return NULL;
}

static const CTM_SYSTEM_PROCESS_INFORMATION *NtEnumProcesses(void)
{
    PFN_NtQuerySystemInformation pfn = Nt_QuerySystemInformation();
    CTM_NTSTATUS status;
    ULONG needed = 0;
    int attempts;

    /* Blame_Collect already enumerated on this thread moments ago. */
    {
        const BYTE *shared;
        ULONG used;
        if (Blame_TakeListing(&shared, &used)) {
            s_listBase = shared;
            s_listUsed = used;
            return (const CTM_SYSTEM_PROCESS_INFORMATION *)shared;
        }
    }

    if (!pfn) return NULL;

    if (!s_ntBuf) {
        s_ntBufSz = 512 * 1024;
        s_ntBuf = (BYTE *)malloc(s_ntBufSz);
        if (!s_ntBuf) return NULL;
    }

    for (attempts = 0; attempts < 8; ++attempts) {
        status = pfn(CtmSystemProcessInformation, s_ntBuf, s_ntBufSz, &needed);
        if (NT_SUCCESS(status)) {
            s_ntUsed = needed;
            s_listBase = s_ntBuf;
            s_listUsed = needed;
            return needed >= sizeof(CTM_SYSTEM_PROCESS_INFORMATION) && needed <= s_ntBufSz
                ? (const CTM_SYSTEM_PROCESS_INFORMATION *)s_ntBuf : NULL;
        }
        if (status == (CTM_NTSTATUS)0xC0000004L) {  /* STATUS_INFO_LENGTH_MISMATCH */
            ULONG newSz;
            if (needed > 64 * 1024 * 1024 - 65536 || s_ntBufSz >= 64 * 1024 * 1024)
                return NULL;
            newSz = needed > s_ntBufSz ? needed + 65536 : s_ntBufSz * 2;
            BYTE *newBuf = (BYTE *)realloc(s_ntBuf, newSz);
            if (!newBuf) return NULL;
            s_ntBuf = newBuf;
            s_ntBufSz = newSz;
        } else {
            return NULL;
        }
    }
    return NULL;
}

/* --------------------------------------------------- user name lookup --- */

static ULONGLONG ProcFileTime(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static BOOL ProcIdentityMatches(HANDLE process, ULONGLONG created)
{
    FILETIME c, e, k, u;
    return created && GetProcessTimes(process, &c, &e, &k, &u) && ProcFileTime(c) == created;
}

static void ProcDescription(HANDLE process, WCHAR *out, size_t cch)
{
    WCHAR path[32768];
    DWORD len = ARRAYSIZE(path), size, unused;
    void *data;
    out[0] = 0;
    if (!QueryFullProcessImageNameW(process, 0, path, &len)) return;
    size = GetFileVersionInfoSizeW(path, &unused);
    if (!size || size > 4 * 1024 * 1024) return;
    data = malloc(size);
    if (!data) return;
    if (GetFileVersionInfoW(path, 0, size, data)) {
        struct { WORD language, codepage; } *languages = NULL;
        UINT bytes = 0, chars = 0;
        WCHAR query[80], *value = NULL;
        if (VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&languages, &bytes) && bytes >= 4) {
            StringCchPrintfW(query, ARRAYSIZE(query), L"\\StringFileInfo\\%04x%04x\\FileDescription",
                languages[0].language, languages[0].codepage);
            if (VerQueryValueW(data, query, (void **)&value, &chars) && chars && value)
                StringCchCopyNW(out, cch, value, chars);
        }
    }
    free(data);
}

static void LookupMetadata(ProcRow *row)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row->pid), token;
    if (!process) return;
    if (!ProcIdentityMatches(process, row->createTime)) { CloseHandle(process); return; }
    ProcDescription(process, row->description, ARRAYSIZE(row->description));
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
        BYTE storage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
        BYTE ownStorage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
        DWORD needed = 0;
        WCHAR name[256], domain[256];
        DWORD nameLen = ARRAYSIZE(name), domainLen = ARRAYSIZE(domain);
        SID_NAME_USE use;
        HANDLE ownToken;
        if (GetTokenInformation(token, TokenUser, storage, sizeof(storage), &needed)) {
            PSID sid = ((TOKEN_USER *)storage)->User.Sid;
            if (LookupAccountSidW(NULL, sid, name, &nameLen, domain, &domainLen, &use)) {
                if (domain[0]) StringCchPrintfW(row->userName, ARRAYSIZE(row->userName), L"%s\\%s", domain, name);
                else StringCchCopyW(row->userName, ARRAYSIZE(row->userName), name);
            }
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ownToken)) {
                if (GetTokenInformation(ownToken, TokenUser, ownStorage, sizeof(ownStorage), &needed))
                    row->ownedByCurrentUser = EqualSid(sid, ((TOKEN_USER *)ownStorage)->User.Sid);
                CloseHandle(ownToken);
            }
        }
        CloseHandle(token);
    }
    CloseHandle(process);
}

/* Per-process GPU is published by the collector thread before this tab's
   collector runs, so the figure is already current. A pid the query never
   saw is unknown rather than zero: the column shows an empty cell for it,
   which is honest about a process whose usage was not measured. The value
   is cleared alongside the flag so a stale figure from an earlier sample
   can never be displayed. */
static void ProcStampGpu(ProcRow *row)
{
    double usage = 0;
    if (Gpu_IsEnabled() && Gpu_ProcessUsage(row->pid, &usage)) {
        row->gpuPct = (float)usage;
        row->gpuKnown = TRUE;
    } else {
        row->gpuPct = 0.0f;
        row->gpuKnown = FALSE;
    }
}

static float ProcCpuDelta(ULONGLONG kernel, ULONGLONG user,
                          const PrevCpu *prev, ULONGLONG elapsed)
{
    double pct;
    if (!elapsed || kernel < prev->kernel || user < prev->user) return 0;
    pct = ((double)(kernel - prev->kernel) + (double)(user - prev->user)) * 100.0 / (double)elapsed;
    return (float)(pct > 100.0 ? 100.0 : pct);
}

/* -------------------------------------------------------- collector thread */

void Proc_Collect(void)
{
    const CTM_SYSTEM_PROCESS_INFORMATION *head, *entry;
    ULONGLONG nowTick;
    UINT cpuCount;
    ULONGLONG deltaSys;
    ProcRow *buf;
    int cnt = 0, cap = 64;
    PrevCpu *newPrev;
    int newPrevCnt = 0, newPrevCap = 128;
    BOOL native;

    nowTick = GetTickCount64();
    cpuCount = CpuCount();
    deltaSys = (s_prevTick > 0 && nowTick > s_prevTick)
               ? (nowTick - s_prevTick) * 10000ULL * (ULONGLONG)cpuCount
               : 0;

    head = NtEnumProcesses();
    native = head != NULL;
    if (!head) head = ProcToolhelp();
    if (!head) return;

    buf = (ProcRow *)malloc((size_t)cap * sizeof(ProcRow));
    if (!buf) return;

    newPrev = (PrevCpu *)malloc((size_t)newPrevCap * sizeof(PrevCpu));
    if (!newPrev) { free(buf); return; }

    entry = head;
    for (;;) {
        DWORD pid       = (DWORD)(ULONG_PTR)entry->UniqueProcessId;
        ULONGLONG ctime = (ULONGLONG)entry->CreateTime.QuadPart;
        ULONGLONG ktm   = (ULONGLONG)entry->KernelTime.QuadPart;
        ULONGLONG utm   = (ULONGLONG)entry->UserTime.QuadPart;
        ProcRow *row;
        int i;

        if (cnt >= cap) {
            int newCap = cap * 2;
            ProcRow *tmp = (ProcRow *)realloc(buf, (size_t)newCap * sizeof(ProcRow));
            if (!tmp) goto failed;
            buf = tmp; cap = newCap;
        }

        row = &buf[cnt++];
        ZeroMemory(row, sizeof(*row));
        row->pid          = pid;
        row->createTime   = ctime;
        row->memoryKnown = entry->WorkingSetPrivateSize.QuadPart >= 0;
        row->privateBytes = row->memoryKnown ? (ULONGLONG)entry->WorkingSetPrivateSize.QuadPart : 0;
        row->parentPid = (DWORD)(ULONG_PTR)entry->InheritedFromUniqueProcessId;
        row->threads = entry->NumberOfThreads;
        row->handles = entry->HandleCount;
        row->cpuTime = ktm + utm;
        row->ioRead = (ULONGLONG)entry->ReadTransferCount.QuadPart;
        row->ioWrite = (ULONGLONG)entry->WriteTransferCount.QuadPart;
        row->countersKnown = native;

        if (entry->ImageName.Buffer && entry->ImageName.Length > 0 &&
            !(entry->ImageName.Length % sizeof(WCHAR)) &&
            (ULONG_PTR)entry->ImageName.Buffer >= (ULONG_PTR)s_listBase &&
            (ULONG_PTR)entry->ImageName.Buffer <= (ULONG_PTR)s_listBase + s_listUsed &&
            entry->ImageName.Length <= (ULONG_PTR)s_listBase + s_listUsed - (ULONG_PTR)entry->ImageName.Buffer) {
            USHORT len = entry->ImageName.Length / sizeof(WCHAR);
            if (len >= PROC_IMAGE_MAX) len = PROC_IMAGE_MAX - 1;
            memcpy(row->imageName, entry->ImageName.Buffer, len * sizeof(WCHAR));
            row->imageName[len] = L'\0';
        }

        if (!row->imageName[0])
            StringCchCopyW(row->imageName, ARRAYSIZE(row->imageName), pid == 0 ? L"System Idle Process" : pid == 4 ? L"System" : L"Unknown");

        /* CPU delta */
        for (i = 0; i < s_prevCnt; i++) {
            if (s_prev[i].pid == pid && s_prev[i].createTime == ctime) {
                row->cpuPct = ProcCpuDelta(ktm, utm, &s_prev[i], deltaSys);
                break;
            }
        }

        /* Outside the loop above: that one only runs for a process seen in
           the previous sample, and a row whose CPU could not be computed
           still deserves its GPU figure. */
        ProcStampGpu(row);

        /* update prev table */
        if (newPrevCnt >= newPrevCap) {
            int nc = newPrevCap * 2;
            PrevCpu *tmp = (PrevCpu *)realloc(newPrev, (size_t)nc * sizeof(PrevCpu));
            if (!tmp) goto failed;
            newPrev = tmp; newPrevCap = nc;
        }
        if (newPrevCnt < newPrevCap) {
            newPrev[newPrevCnt].pid       = pid;
            newPrev[newPrevCnt].createTime= ctime;
            newPrev[newPrevCnt].kernel    = ktm;
            newPrev[newPrevCnt].user      = utm;
            newPrevCnt++;
        }

        /* user name cache */
        row->userName[0] = L'\0';
        for (i = 0; i < s_userCacheLen; i++) {
            if (s_userCache[i].pid == pid && s_userCache[i].createTime == ctime) {
                lstrcpynW(row->userName, s_userCache[i].name, PROC_USER_MAX);
                lstrcpynW(row->description, s_userCache[i].description, PROC_DESC_MAX);
                row->ownedByCurrentUser = s_userCache[i].owned;
                goto have_user;
            }
        }
        LookupMetadata(row);
        {
            int cache;
            if (s_userCacheLen < USER_CACHE_MAX) cache = s_userCacheLen++;
            else { cache = s_userCacheNext; s_userCacheNext = (s_userCacheNext + 1) % USER_CACHE_MAX; }
            s_userCache[cache].pid = pid;
            s_userCache[cache].createTime = ctime;
            s_userCache[cache].owned = row->ownedByCurrentUser;
            lstrcpynW(s_userCache[cache].name, row->userName, PROC_USER_MAX);
            lstrcpynW(s_userCache[cache].description, row->description, PROC_DESC_MAX);
        }
        have_user:;

        if (!entry->NextEntryOffset) break;
        {
            size_t remaining = s_listUsed - (size_t)((const BYTE *)entry - s_listBase);
            if (entry->NextEntryOffset < sizeof(*entry) ||
                entry->NextEntryOffset > remaining || remaining - entry->NextEntryOffset < sizeof(*entry))
                goto failed;
            entry = (const CTM_SYSTEM_PROCESS_INFORMATION *)((const BYTE *)entry + entry->NextEntryOffset);
        }
    }

    free(s_prev);
    s_prev    = newPrev;
    s_prevCnt = newPrevCnt;
    s_prevTick= nowTick;

    /* swap shared buffer */
    {
        ProcRow *old;
        AcquireSRWLockExclusive(&g_procLock);
        old = g_shared;
        g_shared    = buf;
        g_sharedCnt = cnt;
        ReleaseSRWLockExclusive(&g_procLock);
        free(old);
    }
    return;
failed:
    free(buf);
    free(newPrev);
}

void Proc_Reset(void)
{
    free(s_ntBuf);   s_ntBuf = NULL;  s_ntBufSz = 0;
    s_listBase = NULL; s_listUsed = 0;
    free(s_prev);    s_prev  = NULL;  s_prevCnt = 0; s_prevCap = 0;
    s_prevTick    = 0;
    s_userCacheLen = 0; s_userCacheNext = 0; s_pendingPid = 0;

    AcquireSRWLockExclusive(&g_procLock);
    free(g_shared); g_shared = NULL; g_sharedCnt = 0;
    ReleaseSRWLockExclusive(&g_procLock);

    free(g_view); g_view = NULL; g_viewCnt = 0;

    free(g_tree);    g_tree = NULL;
    free(g_ordered); g_ordered = NULL; g_orderedCnt = 0;
    free(g_visible); g_visible = NULL; g_visibleCnt = 0;
    g_firstRoot = -1;
    ProcCollapse_Destroy(g_collapse); g_collapse = NULL;
}

/* ------------------------------------------------------ sort + selection -- */

static int PROC_CMP_COL;
static int PROC_CMP_DIR;

static int ProcCompare(const void *a, const void *b)
{
    const ProcRow *ra = (const ProcRow *)a;
    const ProcRow *rb = (const ProcRow *)b;
    int cmp = 0;
    switch (PROC_CMP_COL) {
    case 0: cmp = lstrcmpiW(ra->imageName, rb->imageName); break;
    case 1: cmp = lstrcmpiW(ra->userName,  rb->userName);  break;
    case 2: cmp = (ra->cpuPct > rb->cpuPct) ? 1 : (ra->cpuPct < rb->cpuPct) ? -1 : 0; break;
    case 3: cmp = (ra->privateBytes > rb->privateBytes) ? 1
                : (ra->privateBytes < rb->privateBytes) ? -1 : 0; break;
    case 4: cmp = lstrcmpiW(ra->description, rb->description); break;
    case 5: cmp = (ra->pid > rb->pid) - (ra->pid < rb->pid); break;
    case PROC_COL_GPU: cmp = (ra->gpuPct > rb->gpuPct) ? 1
                           : (ra->gpuPct < rb->gpuPct) ? -1 : 0; break;
    default: break;
    }
    if (!cmp) cmp = (ra->pid > rb->pid) - (ra->pid < rb->pid);
    return cmp * PROC_CMP_DIR;
}

/* A row sorts by what it displays: rollup when collapsed, own value when not. */
static int ProcTreeCompare(const ProcRow *ra, const ProcTreeInfo *ta,
                           const ProcRow *rb, const ProcTreeInfo *tb)
{
    float ca = ta->collapsed ? ta->cpuRollup : ra->cpuPct;
    float cb = tb->collapsed ? tb->cpuRollup : rb->cpuPct;
    float ga = ta->collapsed ? ta->gpuRollup : ra->gpuPct;
    float gb = tb->collapsed ? tb->gpuRollup : rb->gpuPct;
    ULONGLONG ma = ta->collapsed ? ta->memRollup : ra->privateBytes;
    ULONGLONG mb = tb->collapsed ? tb->memRollup : rb->privateBytes;
    int cmp = 0;

    switch (g_sortCol) {
    case 0: cmp = lstrcmpiW(ra->imageName, rb->imageName); break;
    case 1: cmp = lstrcmpiW(ra->userName,  rb->userName);  break;
    case 2: cmp = (ca > cb) - (ca < cb); break;
    case 3: cmp = (ma > mb) - (ma < mb); break;
    case 4: cmp = lstrcmpiW(ra->description, rb->description); break;
    case 5: cmp = (ra->pid > rb->pid) - (ra->pid < rb->pid); break;
    case PROC_COL_GPU: cmp = (ga > gb) - (ga < gb); break;
    default: break;
    }
    if (!cmp) cmp = (ra->pid > rb->pid) - (ra->pid < rb->pid);
    return cmp * g_sortDir;
}

/* Rebuilds g_tree, g_ordered and g_visible from g_view. Returns FALSE and
   leaves the flat path in charge if anything cannot be allocated. */
static BOOL ProcBuildTree(const BOOL *matches)
{
    free(g_tree);    g_tree    = NULL;
    free(g_ordered); g_ordered = NULL; g_orderedCnt = 0;
    free(g_visible); g_visible = NULL; g_visibleCnt = 0;

    g_firstRoot = -1;
    if (g_viewCnt <= 0) return TRUE;

    g_tree    = (ProcTreeInfo *)calloc((size_t)g_viewCnt, sizeof(ProcTreeInfo));
    g_ordered = (int *)malloc((size_t)g_viewCnt * sizeof(int));
    g_visible = (int *)malloc((size_t)g_viewCnt * sizeof(int));
    if (!g_tree || !g_ordered || !g_visible) {
        free(g_tree); g_tree = NULL;
        free(g_ordered); g_ordered = NULL;
        free(g_visible); g_visible = NULL;
        return FALSE;
    }

    ProcTree_Link(g_view, g_tree, g_viewCnt, &g_firstRoot);
    if (matches)
        g_viewCnt = ProcTree_ApplyContext(g_view, g_tree, g_viewCnt, matches,
                                          &g_firstRoot);
    ProcTree_Aggregate(g_view, g_tree, g_viewCnt);

    if (!g_collapse) g_collapse = ProcCollapse_Create();
    if (g_collapse) {
        int i;
        ProcCollapse_Prune(g_collapse, g_view, g_viewCnt);
        for (i = 0; i < g_viewCnt; ++i)
            /* A context row is never collapsed: hiding the match it exists
               to explain would defeat the point. The entry is kept, so it
               takes effect again once the search is cleared. */
            g_tree[i].collapsed = !g_tree[i].context &&
                ProcCollapse_Contains(g_collapse, g_view[i].pid, g_view[i].createTime);
    }

    ProcTree_Sort(g_view, g_tree, g_viewCnt, ProcTreeCompare, &g_firstRoot);
    g_orderedCnt = ProcTree_Flatten(g_tree, g_viewCnt, g_firstRoot, FALSE, g_ordered);
    g_visibleCnt = ProcTree_Flatten(g_tree, g_viewCnt, g_firstRoot, TRUE,  g_visible);
    return TRUE;
}

/* Clears the collapse flag on every ancestor of a g_view row (identified by
   its model index) so it becomes visible. Returns TRUE when something
   changed, meaning the caller must rebuild before g_visible can be trusted. */
static BOOL ProcExpandAncestors(int row)
{
    BOOL changed = FALSE;
    int walk;
    if (!g_tree || !g_collapse || row < 0 || row >= g_viewCnt) return FALSE;
    for (walk = g_tree[row].parent; walk >= 0; walk = g_tree[walk].parent) {
        if (!ProcCollapse_Contains(g_collapse, g_view[walk].pid, g_view[walk].createTime))
            continue;
        ProcCollapse_Toggle(g_collapse, g_view[walk].pid, g_view[walk].createTime);
        changed = TRUE;
    }
    return changed;
}

void Proc_SelectPid(DWORD pid)
{
    int i;
    s_pendingPid = pid;
    if (!s_list) return;
    /* Cross-tab navigation must reveal a target hidden by a previous search. */
    if (s_query[0] || s_filterMode) {
        s_query[0] = 0; s_filterMode = 0;
        if (s_filter) SendMessageW(s_filter, CB_SETCURSEL, 0, 0);
        if (s_search) SetWindowTextW(s_search, L"");
        ProcSnapshot(NULL);
    }
    for (i = 0; i < g_viewCnt; i++) {
        if (g_view[i].pid == pid) {
            int display = i;
            if (g_tree) {
                /* Hidden under a collapsed ancestor: expand it and rebuild.
                   g_visible/g_visibleCnt are stale the instant collapse
                   state changes, so we cannot resolve `display` from them
                   here -- rebuilding re-enters Proc_SelectPid (s_pendingPid
                   is still `pid`) which resolves it against fresh state.
                   That re-entry finds every ancestor already expanded, so
                   ProcExpandAncestors returns FALSE there and the recursion
                   stops after one extra level. */
                if (ProcExpandAncestors(i)) {
                    ProcSnapshot(NULL);
                    return;
                }
                display = -1;
                {
                    int k;
                    for (k = 0; k < g_visibleCnt; ++k)
                        if (g_visible[k] == i) { display = k; break; }
                }
                if (display < 0) return;
            }
            s_pendingPid = 0;
            ListView_SetItemState(s_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(s_list, display,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(s_list, display, FALSE);
            return;
        }
    }
    /* Deliberately leaves s_pendingPid set. A cross-tab target that belongs
       to another user is not in g_view yet -- it is the pending PID that
       exempts it from the user filter on the NEXT snapshot, which is what
       makes it appear at all. ProcSnapshot drops the request if that
       snapshot still cannot find it. */
}

/* Test-only introspection (see app.h): finds the first row in g_view/g_tree
   that has a real parent (skipping context rows, which are not matches),
   and reports the child's and parent's pid. Returns FALSE when no such row
   exists (e.g. tree mode is off, or the live process tree happens to be a
   single generation). */
BOOL ProcTest_FindChildWithParent(DWORD *childPid, DWORD *parentPid)
{
    int i;
    if (!g_tree) return FALSE;
    for (i = 0; i < g_viewCnt; i++) {
        if (g_tree[i].context) continue;
        if (g_tree[i].parent < 0) continue;
        if (g_tree[g_tree[i].parent].context) continue;
        if (childPid)  *childPid  = g_view[i].pid;
        if (parentPid) *parentPid = g_view[g_tree[i].parent].pid;
        return TRUE;
    }
    return FALSE;
}

/* Test-only introspection: TRUE when `pid` is currently a visible row. */
BOOL ProcTest_IsVisiblePid(DWORD pid)
{
    int i;
    if (g_tree) {
        for (i = 0; i < g_visibleCnt; i++)
            if (g_view[g_visible[i]].pid == pid) return TRUE;
        return FALSE;
    }
    for (i = 0; i < g_viewCnt; i++)
        if (g_view[i].pid == pid) return TRUE;
    return FALSE;
}

/* Test-only introspection: exposes s_pendingPid. */
DWORD ProcTest_PendingPid(void)
{
    return s_pendingPid;
}

/* ------------------------------------------------------ UI callbacks ----- */

static const ProcRow *ProcSelected(void)
{
    int sel = s_list ? ListView_GetNextItem(s_list, -1, LVNI_SELECTED) : -1;
    int row = ProcVisibleRow(sel);
    return row >= 0 && row < g_viewCnt ? &g_view[row] : NULL;
}

static void ProcSelectionChanged(void)
{
    const ProcRow *row = ProcSelected();
    HWND page = s_list ? GetParent(s_list) : NULL;
    if (s_endProcess) EnableWindow(s_endProcess,
        row && row->pid > 4 && row->pid != GetCurrentProcessId() && row->createTime);
    if (page) {
        EnableWindow(GetDlgItem(page, IDC_PROC_OPENLOCATION), row && row->pid > 4 && row->createTime);
        EnableWindow(GetDlgItem(page, IDC_PROC_COPY), row != NULL);
    }
    if (s_details) InvalidateRect(s_details, NULL, FALSE);
}

static void ProcDetailLine(HDC dc, int x, int y, int width, const WCHAR *label, const WCHAR *value)
{
    RECT r = {x, y, x + width, y + DPX(17)};
    UI_Text(dc, label, r, 0, UI_MUTED, DT_SINGLELINE);
    r.top += DPX(19); r.bottom += DPX(21);
    UI_Text(dc, value, r, 1, UI_INK, DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* How the row moved since the mark, or an empty string with no mark. */
static void ProcMarkDelta(const ProcRow *row, WCHAR *buf, size_t cch)
{
    LONGLONG mem;
    LONG handles;
    WCHAR size[48];
    buf[0] = 0;
    if (!ProcDiff_IsSet(&s_mark)) return;
    if (ProcDiff_Classify(&s_mark, row, &mem, &handles) == PROC_CHANGE_NEW) {
        StringCchCopyW(buf, cch, L"Started after the mark");
        return;
    }
    if (row->memoryKnown && mem != 0) {
        UI_FormatSize((ULONGLONG)(mem < 0 ? -mem : mem), size, ARRAYSIZE(size));
        StringCchPrintfW(buf, cch, L"%c%s private, ", mem < 0 ? L'-' : L'+', size);
    } else if (row->memoryKnown) {
        StringCchCopyW(buf, cch, L"+0 private, ");
    }
    {
        size_t len = (size_t)lstrlenW(buf);
        StringCchPrintfW(buf + len, cch - len, L"%c%ld handles",
                         handles < 0 ? L'-' : L'+', (long)(handles < 0 ? -handles : handles));
    }
}

static void ProcDrawDetails(HWND hwnd, HDC dc)
{
    RECT rc, r;
    WCHAR value[160], other[96];
    const ProcRow *row = ProcSelected();
    int pad = DPX(16), width, y;
    BOOL compact;
    GetClientRect(hwnd, &rc);
    UI_Fill(dc, &rc, UI_BG); UI_Card(dc, &rc, UI_SURFACE, UI_LINE);
    width = rc.right - 2 * pad; compact = rc.right >= DPX(400);
    r = (RECT){pad, DPX(13), rc.right - pad, DPX(30)};
    UI_Text(dc, L"PROCESS INSPECTOR", r, 0, UI_MUTED, DT_SINGLELINE);
    if (!row) {
        r.top = DPX(46); r.bottom = r.top + DPX(28);
        UI_Text(dc, L"A closer look", r, 2, UI_INK, DT_SINGLELINE);
        r.top += DPX(36); r.bottom = rc.bottom - DPX(12);
        UI_Text(dc, L"Select a process to inspect its resource usage, identity and activity.", r, 0, UI_MUTED, DT_WORDBREAK);
        return;
    }
    r.top = DPX(37); r.bottom = r.top + DPX(29);
    if (compact) r.right = rc.right / 3;
    UI_Text(dc, row->imageName, r, 2, UI_INK, DT_SINGLELINE | DT_END_ELLIPSIS);
    r.top += DPX(31); r.bottom += DPX(31);
    UI_Text(dc, row->description, r, 0, UI_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
    StringCchPrintfW(value, ARRAYSIZE(value), L"PID %lu  /  Parent %lu", (unsigned long)row->pid, (unsigned long)row->parentPid);
    r.top += DPX(22); r.bottom += DPX(22);
    UI_Text(dc, value, r, 0, UI_MUTED, DT_SINGLELINE | DT_END_ELLIPSIS);
    StringCchPrintfW(value, ARRAYSIZE(value), L"%.1f%%", row->cpuPct);
    if (row->memoryKnown) UI_FormatSize(row->privateBytes, other, ARRAYSIZE(other)); else lstrcpyW(other, L"Unavailable");
    if (compact) {
        ProcDetailLine(dc, rc.right / 3 + pad, DPX(36), rc.right / 6, L"CPU", value);
        ProcDetailLine(dc, rc.right / 2 + pad, DPX(36), rc.right / 5, L"PRIVATE MEMORY", other);
        ProcDetailLine(dc, rc.right / 3 + pad, DPX(83), rc.right / 3 - pad, L"ACCOUNT", row->userName);
        ProcMarkDelta(row, value, ARRAYSIZE(value));
        if (value[0])
            ProcDetailLine(dc, rc.right * 2 / 3 + pad, DPX(83), rc.right / 3 - 2 * pad, L"SINCE MARK", value);
        return;
    }
    y = DPX(130);
    ProcDetailLine(dc, pad, y, width / 2, L"CPU", value);
    ProcDetailLine(dc, pad + width / 2, y, width / 2, L"PRIVATE MEMORY", other);
    y += DPX(57);
    ProcDetailLine(dc, pad, y, width, L"ACCOUNT", row->userName[0] ? row->userName : L"Unavailable");
    y += DPX(57);
    if (row->countersKnown) StringCchPrintfW(value, ARRAYSIZE(value), L"%lu / %lu", (unsigned long)row->threads, (unsigned long)row->handles);
    else lstrcpyW(value, L"Unavailable");
    ProcDetailLine(dc, pad, y, width, L"THREADS / HANDLES", value);
    ProcMarkDelta(row, value, ARRAYSIZE(value));
    if (value[0] && rc.bottom > DPX(390)) {
        /* Takes the next slot; the lifetime lines below need more room. */
        y += DPX(57);
        ProcDetailLine(dc, pad, y, width, L"SINCE MARK", value);
        rc.bottom -= DPX(57);
    }
    if (rc.bottom > DPX(390)) {
        y += DPX(57);
        if (row->createTime) StringCchPrintfW(value, ARRAYSIZE(value), L"%llu:%02llu:%02llu",
            row->cpuTime / 36000000000ULL, row->cpuTime / 600000000ULL % 60, row->cpuTime / 10000000ULL % 60);
        else lstrcpyW(value, L"Unavailable");
        ProcDetailLine(dc, pad, y, width, L"TOTAL CPU TIME", value);
    }
    if (rc.bottom > DPX(445)) {
        y += DPX(57);
        if (row->countersKnown) {
            WCHAR read[48], write[48];
            UI_FormatSize(row->ioRead, read, ARRAYSIZE(read)); UI_FormatSize(row->ioWrite, write, ARRAYSIZE(write));
            StringCchPrintfW(value, ARRAYSIZE(value), L"%s / %s", read, write);
        } else lstrcpyW(value, L"Unavailable");
        ProcDetailLine(dc, pad, y, width, L"I/O READ / WRITE (LIFETIME)", value);
    }
}

static LRESULT CALLBACK ProcDetailsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                       UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if (msg == WM_ERASEBKGND) return TRUE;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; RECT rc;
        HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
        HBITMAP bmp, old;
        GetClientRect(hwnd, &rc);
        bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        if (mem && bmp) {
            old = SelectObject(mem, bmp); ProcDrawDetails(hwnd, mem);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
        } else ProcDrawDetails(hwnd, dc);
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(hwnd, &ps); return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ProcDetailsProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void ProcCreate(TabPage *p)
{
    s_list = UI_CreateListView(p->hwnd, IDC_PROC_LIST, LVS_OWNERDATA);
    if (s_list) {
        UI_AddColumn(s_list, 0, L"Process name",                   105, LVCFMT_LEFT);
        UI_AddColumn(s_list, 1, L"User Name",                      60, LVCFMT_LEFT);
        UI_AddColumn(s_list, 2, L"CPU",                            42, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 3, L"Private memory",                 78, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 4, L"Description",                   110, LVCFMT_LEFT);
        UI_AddColumn(s_list, 5, L"PID", 40, LVCFMT_RIGHT);
        UI_AddColumn(s_list, PROC_COL_GPU, L"GPU", 42, LVCFMT_RIGHT);
        /* Hidden until asked for: showing it is what makes the collector
           enumerate roughly a thousand GPU engine counter instances. */
        ListView_SetColumnWidth(s_list, PROC_COL_GPU, 0);
        UI_SetHeaderSortArrow(s_list, g_sortCol, g_sortDir);
        s_listTheme = OpenThemeData(s_list, L"Explorer::ListView");
    }

    s_allUsers = UI_CreateButton(p->hwnd, IDC_PROC_ALLUSERS,
                                 L"Show processes from &all users", 0);
    s_endProcess = UI_CreateButton(p->hwnd, IDC_PROC_ENDPROCESS,
                                   L"&End Process", 0);

    if (s_allUsers && !App_IsElevated())
        SendMessageW(s_allUsers, BCM_SETSHIELD, 0, (LPARAM)TRUE);

    s_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 10, 10, p->hwnd, (HMENU)(INT_PTR)IDC_PROC_SEARCH, g_hInst, NULL);
    SendMessageW(s_search, WM_SETFONT, (WPARAM)g_hFont, FALSE);
    SendMessageW(s_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search name, account or pid:1234   (Ctrl+F)");
    SendMessageW(s_search, EM_SETLIMITTEXT, ARRAYSIZE(s_query) - 1, 0);
    s_filter = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        0, 0, 10, 10, p->hwnd, (HMENU)(INT_PTR)IDC_PROC_FILTER, g_hInst, NULL);
    SendMessageW(s_filter, WM_SETFONT, (WPARAM)g_hFont, FALSE);
    SendMessageW(s_filter, CB_ADDSTRING, 0, (LPARAM)L"All resource usage");
    SendMessageW(s_filter, CB_ADDSTRING, 0, (LPARAM)L"Active CPU (1%+)");
    SendMessageW(s_filter, CB_ADDSTRING, 0, (LPARAM)L"Memory (100 MB+)");
    SendMessageW(s_filter, CB_ADDSTRING, 0, (LPARAM)L"Changed since mark");
    SendMessageW(s_filter, CB_SETCURSEL, 0, 0);
    UI_CreateButton(p->hwnd, IDC_PROC_CLEAR, L"Clear", 0);
    UI_CreateButton(p->hwnd, IDC_PROC_MARK, ProcDiff_IsSet(&s_mark) ? L"Unmark" : L"Mark", 0);
    UI_CreateButton(p->hwnd, IDC_PROC_EXPORT, L"Export CSV", 0);
    s_summary = UI_CreateStatic(p->hwnd, IDC_PROC_SUMMARY, L"Collecting processes...", SS_LEFT);
    s_details = UI_CreateStatic(p->hwnd, IDC_PROC_DETAILS, L"Process inspector", SS_BLACKRECT);
    SetWindowSubclass(s_details, ProcDetailsProc, 81, 0);
    UI_CreateButton(p->hwnd, IDC_PROC_OPENLOCATION, L"Open file location", 0);
    UI_CreateButton(p->hwnd, IDC_PROC_COPY, L"Copy details", 0);
    ProcSelectionChanged();
}

static void ProcLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    static const int buttons[] = { IDC_PROC_ENDPROCESS };
    int margin = UI_Margin(p->hwnd);
    SIZE bs = UI_ButtonSize(p->hwnd);
    int listBottom, listTop = DPX(75), detailX, detailY, detailW, detailH;
    BOOL side = cx >= DPX(1020) && cy - 2 * margin - bs.cy - listTop >= DPX(340);
    BOOL showDetails = cy >= DPX(340);
    static const int extra[] = {IDC_PROC_SEARCH, IDC_PROC_FILTER, IDC_PROC_CLEAR, IDC_PROC_MARK, IDC_PROC_EXPORT,
        IDC_PROC_DETAILS, IDC_PROC_SUMMARY, IDC_PROC_OPENLOCATION, IDC_PROC_COPY};
    int i;

    if (s_allUsers)   ShowWindow(s_allUsers,   tiny ? SW_HIDE : SW_SHOW);
    if (s_endProcess) ShowWindow(s_endProcess, tiny ? SW_HIDE : SW_SHOW);
    for (i = 0; i < (int)ARRAYSIZE(extra); ++i)
        ShowWindow(GetDlgItem(p->hwnd, extra[i]), tiny ? SW_HIDE : SW_SHOW);
    if (!tiny && !showDetails) {
        ShowWindow(s_details, SW_HIDE);
        ShowWindow(GetDlgItem(p->hwnd, IDC_PROC_OPENLOCATION), SW_HIDE);
        ShowWindow(GetDlgItem(p->hwnd, IDC_PROC_COPY), SW_HIDE);
    }

    if (tiny) {
        if (s_list) MoveWindow(s_list, 0, 0, cx, cy, TRUE);
        return;
    }

    {
        int searchW = cx - 2 * margin - DPX(445);
        MoveWindow(s_search, margin, margin, searchW, DPX(30), TRUE);
        MoveWindow(s_filter, margin + searchW + DPX(10), margin + DPX(2), DPX(175), DPX(200), TRUE);
        MoveWindow(GetDlgItem(p->hwnd, IDC_PROC_CLEAR), cx - margin - DPX(250), margin, DPX(60), DPX(30), TRUE);
        MoveWindow(GetDlgItem(p->hwnd, IDC_PROC_MARK), cx - margin - DPX(180), margin, DPX(70), DPX(30), TRUE);
        MoveWindow(GetDlgItem(p->hwnd, IDC_PROC_EXPORT), cx - margin - DPX(100), margin, DPX(100), DPX(30), TRUE);
        MoveWindow(s_summary, margin, margin + DPX(40), cx - 2 * margin, DPX(20), TRUE);
    }
    listBottom = cy - margin - bs.cy - margin;
    detailW = side ? DPX(270) : cx - 2 * margin;
    detailH = side ? listBottom - listTop : DPX(132);
    detailX = side ? cx - margin - detailW : margin;
    detailY = side ? listTop : listBottom - detailH;
    MoveWindow(s_details, detailX, detailY, detailW, detailH, TRUE);
    if (side) {
        MoveWindow(GetDlgItem(p->hwnd, IDC_PROC_OPENLOCATION), detailX + DPX(12), detailY + detailH - DPX(40), DPX(134), DPX(28), TRUE);
        MoveWindow(GetDlgItem(p->hwnd, IDC_PROC_COPY), detailX + DPX(152), detailY + detailH - DPX(40), DPX(106), DPX(28), TRUE);
    } else {
        MoveWindow(GetDlgItem(p->hwnd, IDC_PROC_OPENLOCATION), detailX + detailW - DPX(158), detailY + DPX(35), DPX(142), DPX(30), TRUE);
        MoveWindow(GetDlgItem(p->hwnd, IDC_PROC_COPY), detailX + detailW - DPX(158), detailY + DPX(77), DPX(142), DPX(30), TRUE);
        if (showDetails) listBottom = detailY - DPX(12);
    }
    if (listBottom < margin + DPX(40)) listBottom = margin + DPX(40);
    if (s_list)
        MoveWindow(s_list, margin, listTop,
                   side ? detailX - margin - DPX(12) : cx - 2 * margin,
                   listBottom - listTop > 0 ? listBottom - listTop : 1, TRUE);

    if (s_allUsers) {
        int w = cx - 2 * margin - DPX(70);
        if (w < DPX(60)) w = DPX(60);
        MoveWindow(s_allUsers, margin, cy - margin - bs.cy,
                   w > DPX(230) ? DPX(230) : w, bs.cy, TRUE);
    }
    UI_PlaceButtonRow(p->hwnd, buttons, (int)ARRAYSIZE(buttons), cx, cy);
}

/* The summary's mark segment: counts, and the first exited names. */
static void ProcMarkSummary(WCHAR *buf, size_t cch, const ProcDiffCounts *counts,
                            const ProcMarkEntry *exited, int shown, const WCHAR *when)
{
    int i;
    if (!counts->started && !counts->exited && !counts->grew) {
        StringCchPrintfW(buf, cch, L"  |  Since %s: no changes", when);
        return;
    }
    StringCchPrintfW(buf, cch, L"  |  Since %s: %d started, ", when, counts->started);
    if (counts->exited < 0) {
        StringCchCatW(buf, cch, L"exits unknown");
    } else {
        size_t len = (size_t)lstrlenW(buf);
        StringCchPrintfW(buf + len, cch - len, L"%d exited", counts->exited);
        if (shown > counts->exited) shown = counts->exited;
        for (i = 0; i < shown; ++i) {
            StringCchCatW(buf, cch, i ? L", " : L" (");
            StringCchCatW(buf, cch, exited[i].imageName);
        }
        if (shown > 0) {
            if (counts->exited > shown) {
                len = (size_t)lstrlenW(buf);
                StringCchPrintfW(buf + len, cch - len, L" +%d more", counts->exited - shown);
            }
            StringCchCatW(buf, cch, L")");
        }
    }
    {
        size_t len = (size_t)lstrlenW(buf);
        StringCchPrintfW(buf + len, cch - len, L", %d grew", counts->grew);
    }
}

static void ProcToggleMark(TabPage *p)
{
    HWND button = p ? GetDlgItem(p->hwnd, IDC_PROC_MARK) : NULL;
    if (ProcDiff_IsSet(&s_mark)) {
        ProcDiff_Free(&s_mark);
        if (s_filterMode == PROC_FILTER_CHANGED) {
            s_filterMode = 0;
            if (s_filter) SendMessageW(s_filter, CB_SETCURSEL, 0, 0);
        }
    } else {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        AcquireSRWLockShared(&g_procLock);
        if (!ProcDiff_Take(&s_mark, g_shared, g_sharedCnt, now) && g_sharedCnt > 0) {
            ReleaseSRWLockShared(&g_procLock);
            App_ReportError(p ? p->hwnd : NULL, L"Mark processes", ERROR_NOT_ENOUGH_MEMORY);
            return;
        }
        ReleaseSRWLockShared(&g_procLock);
    }
    if (button) SetWindowTextW(button, ProcDiff_IsSet(&s_mark) ? L"Unmark" : L"Mark");
    ProcSnapshot(p);
}

static void ProcSnapshot(TabPage *p)
{
    DWORD selPid = 0;
    ULONGLONG selCreate = 0;
    int selItem = -1;
    int i;
    BOOL requestedSelection = s_pendingPid != 0;

    (void)p;

    /* remember selection */
    if (s_list && g_viewCnt > 0) {
        int sel = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
        int row = ProcVisibleRow(sel);
        if (row >= 0 && row < g_viewCnt) {
            selPid    = g_view[row].pid;
            selCreate = g_view[row].createTime;
        }
    }

    /* copy shared -> view */
    {
        ProcRow *newView = NULL;
        int newCnt = 0;
        AcquireSRWLockShared(&g_procLock);
        if (g_sharedCnt > 0) {
            newView = (ProcRow *)malloc((size_t)g_sharedCnt * sizeof(ProcRow));
            if (newView) {
                memcpy(newView, g_shared, (size_t)g_sharedCnt * sizeof(ProcRow));
                newCnt = g_sharedCnt;
            }
        }
        if (g_sharedCnt && !newView) { ReleaseSRWLockShared(&g_procLock); return; }
        ReleaseSRWLockShared(&g_procLock);

        /* Against every collected process, before any visibility filter. */
        s_markText[0] = 0;
        if (ProcDiff_IsSet(&s_mark)) {
            ProcDiffCounts counts;
            ProcMarkEntry exited[3];
            FILETIME local;
            SYSTEMTIME st;
            WCHAR when[16] = L"mark";
            ProcDiff_Count(&s_mark, newView, newCnt, &counts, exited, (int)ARRAYSIZE(exited));
            if (FileTimeToLocalFileTime(&s_mark.wallTime, &local) && FileTimeToSystemTime(&local, &st))
                StringCchPrintfW(when, ARRAYSIZE(when), L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
            ProcMarkSummary(s_markText, ARRAYSIZE(s_markText), &counts, exited,
                            counts.exited < (int)ARRAYSIZE(exited) ? counts.exited : (int)ARRAYSIZE(exited), when);
        } else if (s_filterMode == PROC_FILTER_CHANGED) {
            StringCchCopyW(s_markText, ARRAYSIZE(s_markText), L"  |  Press Mark first, then look again later");
        }

        free(g_view);
        g_view    = newView;
        g_viewCnt = newCnt;
    }

    {
        int kept = 0;
        for (i = 0; i < g_viewCnt; ++i)
            /* PID 0 represents idle processor capacity, not resource usage. */
            if (g_view[i].pid && (g_cfg.showAllUsers || g_view[i].ownedByCurrentUser || g_view[i].pid == s_pendingPid ||
                (g_view[i].pid == selPid && g_view[i].createTime == selCreate)))
                g_view[kept++] = g_view[i];
        g_viewCnt = kept;
    }
    {
        int kept = 0, total = g_viewCnt;
        WCHAR summary[640];
        double cpu = 0; ULONGLONG memory = 0;
        WCHAR size[48];
        BOOL treeBuilt = FALSE;

        if (g_cfg.procTreeMode) {
            BOOL *matches = (BOOL *)malloc((size_t)(g_viewCnt > 0 ? g_viewCnt : 1) * sizeof(BOOL));
            if (matches) {
                for (i = 0; i < g_viewCnt; ++i)
                    matches[i] = ProcMatches(&g_view[i], s_query, s_filterMode);
                treeBuilt = ProcBuildTree(matches);
                free(matches);
            }
        }

        if (treeBuilt) {
            /* Summed from each row's own value, never the rollup, and never a
               context row (it exists only to explain a match, not as a hit
               itself) -- rollups already include descendants, so summing them
               too would double-count every branch. */
            for (i = 0; i < g_viewCnt; ++i) {
                if (g_tree && g_tree[i].context) continue;
                cpu += g_view[i].cpuPct;
                if (g_view[i].memoryKnown) memory += g_view[i].privateBytes;
                ++kept;
            }
        } else {
            /* Tree mode off, or tree mode on but the tree could not be built
               (matches allocation failed, or ProcBuildTree itself failed):
               both degrade to the same flat path so filtering/sorting still
               work rather than leaving stale tree state paired with a fresh
               g_view. ProcBuildTree already frees and NULLs g_tree/g_ordered/
               g_visible and zeroes both counts on its own failure path; do
               the same here for the "tree mode off" case so there is exactly
               one flat state either way. */
            free(g_tree);    g_tree = NULL;
            free(g_ordered); g_ordered = NULL; g_orderedCnt = 0;
            free(g_visible); g_visible = NULL; g_visibleCnt = 0;
            for (i = 0; i < g_viewCnt; ++i) {
                if (!ProcMatches(&g_view[i], s_query, s_filterMode)) continue;
                cpu += g_view[i].cpuPct;
                if (g_view[i].memoryKnown) memory += g_view[i].privateBytes;
                g_view[kept++] = g_view[i];
            }
            g_viewCnt = kept;
            if (g_view && g_viewCnt > 1) {
                PROC_CMP_COL = g_sortCol;
                PROC_CMP_DIR = g_sortDir;
                qsort(g_view, (size_t)g_viewCnt, sizeof(ProcRow), ProcCompare);
            }
        }

        UI_FormatSize(memory, size, ARRAYSIZE(size));
        StringCchPrintfW(summary, ARRAYSIZE(summary),
            L"%d of %d processes  |  %.1f%% CPU  |  %s private memory%s", kept, total, cpu, size,
            s_markText[0] ? s_markText :
            kept == 0 ? L"  |  No matches - try clearing your filters" : L"  |  Click a column to sort");
        if (s_summary) SetWindowTextW(s_summary, summary);
    }

    if (s_list) {
        s_refreshing = TRUE;
        ListView_SetItemState(s_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        /* g_tree is NULL when tree mode is off *or* when ProcBuildTree could
           not allocate. Both must fall back to the flat count, or a failed
           allocation would silently show an empty process list. */
        ListView_SetItemCountEx(s_list,
            g_tree ? g_visibleCnt : g_viewCnt, LVSICF_NOSCROLL);
        InvalidateRect(s_list, NULL, FALSE);
    }

    if (s_pendingPid) {
        /* The user-filter pass above already exempted this PID, so if it is
           still absent the process is genuinely gone -- drop the request.
           Left set, it would exempt a dead PID from the user filter on every
           future tick for the rest of the session. */
        int pending;
        BOOL present = FALSE;
        for (pending = 0; pending < g_viewCnt; ++pending)
            if (g_view[pending].pid == s_pendingPid) { present = TRUE; break; }
        if (present) Proc_SelectPid(s_pendingPid);
        else         s_pendingPid = 0;
    }
    if (s_allUsers && App_IsElevated())
        SetWindowTextW(s_allUsers, g_cfg.showAllUsers ? L"Show only &my processes" : L"Show processes from &all users");
    /* restore selection */
    if (s_list && selPid && !requestedSelection) {
        int viewIdx = -1;
        for (i = 0; i < g_viewCnt; i++) {
            if (g_view[i].pid == selPid && g_view[i].createTime == selCreate) {
                viewIdx = i; break;
            }
        }
        if (viewIdx >= 0) {
            int display = viewIdx;
            if (g_tree) {
                display = -1;
                for (i = 0; i < g_visibleCnt; ++i)
                    if (g_visible[i] == viewIdx) { display = i; break; }
            }
            if (display >= 0) {
                selItem = display;
                ListView_SetItemState(s_list, selItem,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
    }
    s_refreshing = FALSE;
    ProcSelectionChanged();
}

#define PROC_INDENT_STEP  14      /* logical px per level, DPI scaled       */
#define PROC_TWISTY_BOX   12

/* Twisty rect for a display row, in the cell's coordinates. FALSE when the
   row has no children and therefore no twisty. */
static BOOL ProcTwistyRect(int display, const RECT *cell, RECT *out)
{
    int row = ProcVisibleRow(display), depth, left, mid;
    if (!g_tree || row < 0 || row >= g_viewCnt) return FALSE;
    if (g_tree[row].childCount <= 0) return FALSE;

    depth = g_tree[row].depth;
    if (depth > PROC_INDENT_CAP) depth = PROC_INDENT_CAP;
    left = cell->left + depth * DPX(PROC_INDENT_STEP);
    mid  = (cell->top + cell->bottom) / 2;

    out->left   = left;
    out->right  = left + DPX(PROC_TWISTY_BOX);
    out->top    = mid - DPX(PROC_TWISTY_BOX) / 2;
    out->bottom = mid + DPX(PROC_TWISTY_BOX) / 2;
    return TRUE;
}

/* Toggles a display row's collapse state and rebuilds the view. Moves the
   selection to the row being collapsed if the selection is inside it, which
   would otherwise vanish without explanation. */
static void ProcToggleCollapse(TabPage *p, int display)
{
    int row = ProcVisibleRow(display), selected, walk;
    BOOL nowCollapsed;
    if (!g_tree || row < 0 || row >= g_viewCnt || g_tree[row].childCount <= 0) return;
    if (!g_collapse) return;

    selected = ProcVisibleRow(ListView_GetNextItem(s_list, -1, LVNI_SELECTED));
    nowCollapsed = ProcCollapse_Toggle(g_collapse, g_view[row].pid, g_view[row].createTime);

    /* Only a collapse can hide the current selection, and only when the
       selection is a strict descendant of the row being collapsed (the
       toggled row itself stays visible either way). Setting s_pendingPid
       on an expand, or when the selected row IS the toggled row, would
       make Proc_SelectPid clear an active search for no reason. */
    if (nowCollapsed && selected != row) {
        for (walk = selected; walk >= 0; walk = g_tree[walk].parent) {
            if (walk != row) continue;
            s_pendingPid = g_view[row].pid;     /* reselect the parent */
            break;
        }
    }
    ProcSnapshot(p);
}

/* Name-cell tint for a row that started or grew since the mark. */
static COLORREF ProcChangeTint(const ProcRow *row, COLORREF base)
{
    switch (ProcDiff_Classify(&s_mark, row, NULL, NULL)) {
    case PROC_CHANGE_NEW:  return RGB(222, 245, 228);
    case PROC_CHANGE_GREW: return RGB(255, 236, 204);
    default:               return base;
    }
}

static BOOL ProcNotify(TabPage *p, NMHDR *nm, LRESULT *result)
{
    *result = 0;

    if (nm->hwndFrom != s_list) return FALSE;
    if (nm->code == LVN_ITEMCHANGED || nm->code == LVN_ODSTATECHANGED) {
        if (!s_refreshing) ProcSelectionChanged();
        return FALSE;
    }

    if (nm->code == NM_CLICK && g_cfg.procTreeMode) {
        NMITEMACTIVATE *click = (NMITEMACTIVATE *)nm;
        LVHITTESTINFO hit;
        ZeroMemory(&hit, sizeof(hit));
        hit.pt = click->ptAction;
        if (ListView_SubItemHitTest(s_list, &hit) >= 0 && hit.iSubItem == 0) {
            RECT cell, twisty;
            /* LVIR_BOUNDS on subitem 0 gives the whole row, not the narrower
               cell used at paint time, but that's fine: ProcTwistyRect only
               reads cell->left/top/bottom, never cell->right, so both rects
               produce a bit-identical twisty. If ProcTwistyRect is ever
               changed to read cell->right, this hit-test would silently
               turn the entire row into a chevron hit target. */
            if (ListView_GetSubItemRect(s_list, hit.iItem, 0, LVIR_BOUNDS, &cell) &&
                ProcTwistyRect(hit.iItem, &cell, &twisty) &&
                click->ptAction.x >= twisty.left && click->ptAction.x < twisty.right) {
                ProcToggleCollapse(p, hit.iItem);
                *result = 0;
                return TRUE;
            }
        }
    }

    if (nm->code == LVN_KEYDOWN && g_cfg.procTreeMode && g_tree) {
        NMLVKEYDOWN *key = (NMLVKEYDOWN *)nm;
        int display = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
        int row = ProcVisibleRow(display);
        if (row < 0) return FALSE;

        if (key->wVKey == VK_RIGHT) {
            if (g_tree[row].collapsed) { ProcToggleCollapse(p, display); *result = 0; return TRUE; }
            if (g_tree[row].firstChild >= 0 && display + 1 < g_visibleCnt) {
                /* Clear the old row's selection explicitly: SetItemState only
                   sets bits on the target item, so without this the prior
                   row would stay LVIS_SELECTED and LVNI_SELECTED lookups
                   (ProcSelected, the next arrow press) would keep finding it
                   instead of the row the user just moved to. */
                ListView_SetItemState(s_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(s_list, display + 1,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(s_list, display + 1, FALSE);
                *result = 0; return TRUE;
            }
        }
        if (key->wVKey == VK_LEFT) {
            if (!g_tree[row].collapsed && g_tree[row].childCount > 0) {
                ProcToggleCollapse(p, display); *result = 0; return TRUE;
            }
            if (g_tree[row].parent >= 0) {
                int i;
                for (i = 0; i < g_visibleCnt; ++i) {
                    if (g_visible[i] != g_tree[row].parent) continue;
                    ListView_SetItemState(s_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_SetItemState(s_list, i,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_EnsureVisible(s_list, i, FALSE);
                    break;
                }
                *result = 0; return TRUE;
            }
        }
    }

    if (nm->code == NM_CUSTOMDRAW) {
        NMLVCUSTOMDRAW *draw = (NMLVCUSTOMDRAW *)nm;
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) { *result = CDRF_NOTIFYITEMDRAW; return TRUE; }
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) { *result = CDRF_NOTIFYSUBITEMDRAW; return TRUE; }
        if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            int rawIndex = (int)draw->nmcd.dwItemSpec;
            int index = ProcVisibleRow(rawIndex);
            COLORREF bg = rawIndex % 2 ? RGB(248, 250, 253) : UI_SURFACE;

            if (draw->iSubItem == 0 && g_cfg.procTreeMode && g_tree && index >= 0 && index < g_viewCnt) {
                RECT cell = draw->nmcd.rc, twisty, text;
                COLORREF back = rawIndex % 2 ? RGB(248, 250, 253) : UI_SURFACE;
                COLORREF ink  = g_tree[index].context ? UI_MUTED : UI_INK;
                int depth = g_tree[index].depth;
                /* uItemState at the subitem stage is a known comctl32 weak
                   spot for LVS_OWNERDATA lists; read selection/focus the
                   documented-reliable way instead. */
                UINT itemState = (UINT)ListView_GetItemState(s_list, rawIndex, LVIS_SELECTED | LVIS_FOCUSED);
                BOOL selected = (itemState & LVIS_SELECTED) != 0;
                BOOL focused  = GetFocus() == s_list;
                BOOL hot      = (draw->nmcd.uItemState & CDIS_HOT) != 0;
                int themeState = LISS_NORMAL;

                if (selected) themeState = focused ? LISS_SELECTED : LISS_SELECTEDNOTFOCUS;
                else if (hot) themeState = LISS_HOT;

                /* The theme normally leaves LISS_NORMAL transparent (no zebra
                   stripes of its own), so paint our alternating base first,
                   then let the theme overlay the selected/hot state exactly
                   as it does for columns 1-5. Falls back to the old
                   hand-painted colors when unthemed (classic mode). */
                back = ProcChangeTint(&g_view[index], back);
                UI_Fill(draw->nmcd.hdc, &cell, back);
                if (themeState != LISS_NORMAL) {
                    if (!s_listTheme || FAILED(DrawThemeBackground(s_listTheme, draw->nmcd.hdc,
                            LVP_LISTITEM, themeState, &cell, NULL))) {
                        back = selected ? RGB(205, 224, 253) : RGB(235, 242, 253);
                        UI_Fill(draw->nmcd.hdc, &cell, back);
                    }
                }
                if (selected) ink = UI_INK;

                if (depth > PROC_INDENT_CAP) depth = PROC_INDENT_CAP;
                if (ProcTwistyRect(rawIndex, &cell, &twisty)) {
                    POINT chevron[3];
                    int cx = (twisty.left + twisty.right) / 2;
                    int cy = (twisty.top + twisty.bottom) / 2;
                    int arm = DPX(3);
                    if (g_tree[index].collapsed) {      /* pointing right */
                        chevron[0].x = cx - arm / 2; chevron[0].y = cy - arm;
                        chevron[1].x = cx + arm / 2; chevron[1].y = cy;
                        chevron[2].x = cx - arm / 2; chevron[2].y = cy + arm;
                    } else {                            /* pointing down  */
                        chevron[0].x = cx - arm; chevron[0].y = cy - arm / 2;
                        chevron[1].x = cx;       chevron[1].y = cy + arm / 2;
                        chevron[2].x = cx + arm; chevron[2].y = cy - arm / 2;
                    }
                    UI_Polyline(draw->nmcd.hdc, chevron, 3, UI_MUTED, DPX(2));
                }

                text = cell;
                text.left += depth * DPX(PROC_INDENT_STEP) + DPX(PROC_TWISTY_BOX) + DPX(4);
                text.right -= DPX(4);
                UI_Text(draw->nmcd.hdc, g_view[index].imageName, text, 0, ink,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

                /* A themed list expresses focus through LISS_SELECTED /
                   LISS_SELECTEDNOTFOCUS, not a dotted rect; only draw one by
                   hand in the unthemed fallback. */
                if (!s_listTheme && (itemState & LVIS_FOCUSED)) {
                    RECT focus = cell;
                    DrawFocusRect(draw->nmcd.hdc, &focus);
                }
                *result = CDRF_SKIPDEFAULT;
                return TRUE;
            }

            if (index >= 0 && index < g_viewCnt) {
                BOOL collapsed = g_tree && g_tree[index].collapsed;
                if (draw->iSubItem == 0) {
                    bg = ProcChangeTint(&g_view[index], bg);
                } else if (draw->iSubItem == 2) {
                    float cpu = collapsed ? g_tree[index].cpuRollup : g_view[index].cpuPct;
                    bg = cpu >= 15 ? RGB(255, 218, 178) : cpu >= 1 ? RGB(255, 241, 217) : RGB(249, 246, 238);
                } else if (draw->iSubItem == PROC_COL_GPU) {
                    float gpu = collapsed ? g_tree[index].gpuRollup
                                          : g_view[index].gpuPct;
                    bg = gpu >= 15 ? RGB(214, 232, 255) :
                         gpu >= 1  ? RGB(235, 244, 255) : RGB(246, 249, 253);
                } else if (draw->iSubItem == 3) {
                    ULONGLONG mem = (collapsed && g_tree[index].memRollupKnown)
                        ? g_tree[index].memRollup : g_view[index].privateBytes;
                    bg = mem >= 100ULL * 1024 * 1024 ? RGB(233, 224, 252) : RGB(245, 241, 253);
                }
            }
            draw->clrText = UI_INK; draw->clrTextBk = bg;
            *result = CDRF_NEWFONT; return TRUE;
        }
    }
    if (nm->code == LVN_GETDISPINFOW) {
        NMLVDISPINFOW *di = (NMLVDISPINFOW *)nm;
        int row = ProcVisibleRow(di->item.iItem);
        const ProcRow *r;
        if (row < 0 || row >= g_viewCnt) return FALSE;
        r = &g_view[row];
        if (di->item.mask & LVIF_TEXT) {
            switch (di->item.iSubItem) {
            case 0:
                lstrcpynW(di->item.pszText, r->imageName, di->item.cchTextMax);
                break;
            case 1:
                {
                    const WCHAR *name = r->userName;
                    const WCHAR *slash = wcschr(name, L'\\');
                    if (!g_cfg.showFullAccountName && slash) name = slash + 1;
                    lstrcpynW(di->item.pszText, name, di->item.cchTextMax);
                }
                break;
            case 2: {
                float cpu = (g_tree && g_tree[row].collapsed)
                            ? g_tree[row].cpuRollup : r->cpuPct;
                StringCchPrintfW(di->item.pszText, (size_t)di->item.cchTextMax,
                                 L"%.1f%%", cpu);
                break;
            }
            case 3:
                if (g_tree && g_tree[row].collapsed) {
                    if (!g_tree[row].memRollupKnown)
                        lstrcpynW(di->item.pszText, L"N/A", di->item.cchTextMax);
                    else
                        UI_FormatSize(g_tree[row].memRollup, di->item.pszText,
                                      (size_t)di->item.cchTextMax);
                } else if (!r->memoryKnown) {
                    lstrcpynW(di->item.pszText, L"N/A", di->item.cchTextMax);
                } else {
                    UI_FormatSize(r->privateBytes, di->item.pszText,
                                  (size_t)di->item.cchTextMax);
                }
                break;
            case 5:
                StringCchPrintfW(di->item.pszText, (size_t)di->item.cchTextMax, L"%lu", (unsigned long)r->pid);
                break;
            case 4:
                lstrcpynW(di->item.pszText, r->description, di->item.cchTextMax);
                break;
            case PROC_COL_GPU:
                /* An unmeasured process shows nothing rather than 0.0%,
                   which would claim the GPU was sampled and found idle. */
                if (g_tree && g_tree[row].collapsed)
                    StringCchPrintfW(di->item.pszText,
                                     (size_t)di->item.cchTextMax,
                                     L"%.1f%%", g_tree[row].gpuRollup);
                else if (r->gpuKnown)
                    StringCchPrintfW(di->item.pszText,
                                     (size_t)di->item.cchTextMax,
                                     L"%.1f%%", r->gpuPct);
                else
                    di->item.pszText[0] = L'\0';
                break;
            }
        }
        return TRUE;
    }

    if (nm->code == LVN_COLUMNCLICK) {
        NMLISTVIEW *lv = (NMLISTVIEW *)nm;
        if (lv->iSubItem == g_sortCol) {
            g_sortDir = -g_sortDir;
        } else {
            g_sortCol = lv->iSubItem;
            g_sortDir = (g_sortCol == 2 || g_sortCol == 3) ? -1 : 1;
        }
        UI_SetHeaderSortArrow(s_list, g_sortCol, g_sortDir);
        ProcSnapshot(p);
        return TRUE;
    }

    return FALSE;
}

static HANDLE ProcOpenTarget(const ProcRow *row)
{
    HANDLE process;
    BOOL critical = TRUE;
    typedef BOOL (WINAPI *CriticalFn)(HANDLE, PBOOL);
    CriticalFn criticalFn = (CriticalFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsProcessCritical");
    if (row->pid <= 4 || row->pid == GetCurrentProcessId() || !row->createTime) {
        SetLastError(ERROR_ACCESS_DENIED); return NULL;
    }
    process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, row->pid);
    if (!process) return NULL;
    if (!ProcIdentityMatches(process, row->createTime)) {
        CloseHandle(process); SetLastError(ERROR_NOT_FOUND); return NULL;
    }
    if (!criticalFn || !criticalFn(process, &critical) || critical) {
        CloseHandle(process); SetLastError(ERROR_ACCESS_DENIED); return NULL;
    }
    return process;
}

/* Toggling a column's width is how this tab shows and hides columns; there
   is no separate visibility state to keep in step. The GPU column
   additionally owns a bit of the GPU collection flag, so the cost is paid
   only while the column is on screen. */
static void ProcToggleColumn(int column)
{
    int count = Header_GetItemCount(ListView_GetHeader(s_list));
    BOOL showing;
    if (!s_list || column < 1 || column >= count) return;
    showing = ListView_GetColumnWidth(s_list, column) == 0;
    ListView_SetColumnWidth(s_list, column,
                            showing ? LVSCW_AUTOSIZE_USEHEADER : 0);
    if (column == PROC_COL_GPU)
        Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, showing);
}

/* Builds the column menu without showing it, so the construction can be
   inspected by a test: TrackPopupMenu below blocks on user input and can
   never run in a fixture, which would otherwise leave this untested.
   The labels come from the header itself. A private copy of the names here
   would drift the moment a column is added or renamed -- and being indexed
   by a hardcoded count, drift would mean an out-of-range read rather than
   a wrong string. Returns NULL if the menu cannot be created. */
static HMENU ProcBuildColumnMenu(void)
{
    HMENU menu;
    int i, count;
    if (!s_list) return NULL;
    menu = CreatePopupMenu();
    if (!menu) return NULL;
    count = Header_GetItemCount(ListView_GetHeader(s_list));
    for (i = 1; i < count; ++i) {
        WCHAR label[64] = {0};
        LVCOLUMNW column;
        ZeroMemory(&column, sizeof(column));
        column.mask = LVCF_TEXT;
        column.pszText = label;
        column.cchTextMax = ARRAYSIZE(label);
        if (!ListView_GetColumn(s_list, i, &column)) continue;
        AppendMenuW(menu, (UINT)(MF_STRING |
            (ListView_GetColumnWidth(s_list, i) ? MF_CHECKED : 0)),
            (UINT_PTR)(IDM_PROC_COL_FIRST + i), label);
    }
    return menu;
}

/* Test-only introspection: the fixture cannot reach the popup through
   ProcColumns, which blocks in TrackPopupMenu. */
HMENU ProcTest_BuildColumnMenu(void) { return ProcBuildColumnMenu(); }

static void ProcColumns(HWND owner)
{
    HMENU menu = ProcBuildColumnMenu();
    POINT pt;
    int chosen;
    if (!menu) return;
    GetCursorPos(&pt);
    chosen = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, owner, NULL)
             - IDM_PROC_COL_FIRST;
    DestroyMenu(menu);
    ProcToggleColumn(chosen);
}

static BOOL ProcWriteBytes(HANDLE file, const void *data, DWORD bytes)
{
    const BYTE *cursor = data;
    while (bytes) {
        DWORD written = 0;
        if (!WriteFile(file, cursor, bytes, &written, NULL)) return FALSE;
        if (!written) { SetLastError(ERROR_WRITE_FAULT); return FALSE; }
        cursor += written; bytes -= written;
    }
    return TRUE;
}

static BOOL ProcWriteCsv(HANDLE file, const ProcRow *rows, int count)
{
    static const char header[] = "\xef\xbb\xbfProcess,Account,CPU percent,Private memory bytes,Description,PID\r\n";
    int i;
    if (!ProcWriteBytes(file, header, sizeof(header) - 1)) return FALSE;
    for (i = 0; i < count; ++i) {
        WCHAR name[2 * PROC_IMAGE_MAX + 4], user[2 * PROC_USER_MAX + 4];
        WCHAR description[2 * PROC_DESC_MAX + 4], memory[32];
        WCHAR line[1800]; char utf8[7200]; int bytes;
        if (!ProcCsvField(rows[i].imageName, name, ARRAYSIZE(name)) ||
            !ProcCsvField(rows[i].userName, user, ARRAYSIZE(user)) ||
            !ProcCsvField(rows[i].description, description, ARRAYSIZE(description))) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE;
        }
        if (rows[i].memoryKnown) StringCchPrintfW(memory, ARRAYSIZE(memory), L"%llu", rows[i].privateBytes);
        else memory[0] = 0;
        if (FAILED(StringCchPrintfW(line, ARRAYSIZE(line), L"%s,%s,%.2f,%s,%s,%lu\r\n",
            name, user, rows[i].cpuPct, memory, description, (unsigned long)rows[i].pid))) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE;
        }
        bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line, lstrlenW(line), utf8, sizeof(utf8), NULL, NULL);
        if (!bytes || !ProcWriteBytes(file, utf8, (DWORD)bytes)) return FALSE;
    }
    return TRUE;
}

static void ProcExport(HWND owner)
{
    OPENFILENAMEW dialog = {0};
    WCHAR path[MAX_PATH] = L"process-snapshot.csv", temporary[MAX_PATH];
    ProcRow *rows = NULL;
    int count = 0;
    HANDLE file;
    DWORD error;
    if (g_viewCnt) {
        int i;
        rows = malloc((size_t)g_viewCnt * sizeof(*rows));
        if (!rows) { App_ReportError(owner, L"Capture export snapshot", ERROR_NOT_ENOUGH_MEMORY); return; }
        if (g_cfg.procTreeMode && g_tree && g_ordered) {
            for (i = 0; i < g_orderedCnt; ++i) {
                int at = g_ordered[i];
                if (g_tree[at].context) continue;
                rows[count++] = g_view[at];
            }
        } else {
            for (i = 0; i < g_viewCnt; ++i) rows[count++] = g_view[i];
        }
    }
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"CSV spreadsheet (*.csv)\0*.csv\0\0";
    dialog.lpstrFile = path; dialog.nMaxFile = ARRAYSIZE(path); dialog.lpstrDefExt = L"csv";
    dialog.lpstrTitle = L"Export current filtered process snapshot";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) {
        error = CommDlgExtendedError();
        free(rows);
        if (error) App_ReportError(owner, L"Open export dialog", ERROR_INVALID_DATA);
        return;
    }
    /* Finish a sibling temporary file before replacing an existing export. */
    if (FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s.%lu.%llu.tmp", path,
        (unsigned long)GetCurrentProcessId(), GetTickCount64()))) {
        free(rows); App_ReportError(owner, L"Export path is too long", ERROR_FILENAME_EXCED_RANGE); return;
    }
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { free(rows); App_ReportError(owner, L"Create export", GetLastError()); return; }
    error = ProcWriteCsv(file, rows, count) && FlushFileBuffers(file) ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file); free(rows);
    if (!error && !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) error = GetLastError();
    if (error) { DeleteFileW(temporary); App_ReportError(owner, L"Write process export", error); }
    else {
        WCHAR message[96];
        StringCchPrintfW(message, ARRAYSIZE(message), L"Exported %d processes to CSV.", count);
        MessageBoxW(owner, message, L"Snapshot exported", MB_OK | MB_ICONINFORMATION);
    }
}

static void ProcOpenLocation(HWND owner)
{
    const ProcRow *selection = ProcSelected();
    ProcRow target;
    HANDLE process; WCHAR path[32768]; DWORD capacity = ARRAYSIZE(path), error = 0;
    PIDLIST_ABSOLUTE item;
    HRESULT hr, init;
    if (!selection) return;
    target = *selection;
    process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
    if (!process) { App_ReportError(owner, L"Open process location", GetLastError()); return; }
    if (!ProcIdentityMatches(process, target.createTime)) error = ERROR_NOT_FOUND;
    else if (!QueryFullProcessImageNameW(process, 0, path, &capacity)) error = GetLastError();
    CloseHandle(process);
    if (error) { App_ReportError(owner, L"Read process location", error); return; }
    init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    item = ILCreateFromPathW(path);
    hr = item ? SHOpenFolderAndSelectItems(item, 0, NULL, 0) : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (item) ILFree(item);
    if (SUCCEEDED(init)) CoUninitialize();
    if (FAILED(hr)) App_ReportError(owner, L"Open file location", HRESULT_FACILITY(hr) == FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);
}

static void ProcCopyDetails(HWND owner)
{
    const ProcRow *row = ProcSelected();
    WCHAR text[1600], memory[48]; HGLOBAL data; WCHAR *buffer; size_t bytes;
    if (!row) return;
    if (row->memoryKnown) UI_FormatSize(row->privateBytes, memory, ARRAYSIZE(memory)); else lstrcpyW(memory, L"Unavailable");
    StringCchPrintfW(text, ARRAYSIZE(text),
        L"Process: %s\r\nPID: %lu\r\nDescription: %s\r\nAccount: %s\r\nCPU: %.1f%%\r\nPrivate memory: %s\r\nParent PID: %lu\r\n",
        row->imageName, (unsigned long)row->pid, row->description, row->userName, row->cpuPct, memory, (unsigned long)row->parentPid);
    {
        WCHAR delta[160];
        ProcMarkDelta(row, delta, ARRAYSIZE(delta));
        if (delta[0]) {
            size_t len = wcslen(text);
            StringCchPrintfW(text + len, ARRAYSIZE(text) - len, L"Since mark: %s\r\n", delta);
        }
    }
    bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    data = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!data) { App_ReportError(owner, L"Copy details", ERROR_NOT_ENOUGH_MEMORY); return; }
    buffer = GlobalLock(data);
    if (!buffer) { GlobalFree(data); App_ReportError(owner, L"Copy details", ERROR_NOT_ENOUGH_MEMORY); return; }
    memcpy(buffer, text, bytes); GlobalUnlock(data);
    if (!OpenClipboard(owner)) { GlobalFree(data); App_ReportError(owner, L"Open clipboard", GetLastError()); return; }
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, data)) {
        DWORD error = GetLastError(); CloseClipboard(); GlobalFree(data);
        App_ReportError(owner, L"Copy details", error); return;
    }
    CloseClipboard();
}

static void ProcCommand(TabPage *p, int id, int code, HWND ctl)
{
    (void)p; (void)code; (void)ctl;
    /* The column toggles occupy a reserved id block; route them through the
       same helper the popup menu uses so both paths behave identically. */
    if (id >= IDM_PROC_COL_FIRST && id <= IDM_PROC_COL_LAST) {
        ProcToggleColumn(id - IDM_PROC_COL_FIRST);
        return;
    }
    switch (id) {
    case IDM_PROC_FIND:
        SetFocus(s_search); SendMessageW(s_search, EM_SETSEL, 0, -1); break;
    case IDC_PROC_SEARCH:
        if (code == EN_CHANGE) {
            GetWindowTextW(s_search, s_query, ARRAYSIZE(s_query));
            ProcSnapshot(p);
        }
        break;
    case IDC_PROC_FILTER:
        if (code == CBN_SELCHANGE) {
            s_filterMode = (int)SendMessageW(s_filter, CB_GETCURSEL, 0, 0);
            ProcSnapshot(p);
        }
        break;
    case IDC_PROC_CLEAR:
        s_filterMode = 0; SendMessageW(s_filter, CB_SETCURSEL, 0, 0);
        s_query[0] = 0; SetWindowTextW(s_search, L"");
        ProcSnapshot(p); SetFocus(s_search); break;
    case IDC_PROC_MARK: ProcToggleMark(p); break;
    case IDC_PROC_EXPORT: ProcExport(p->hwnd); break;
    case IDC_PROC_OPENLOCATION: ProcOpenLocation(p->hwnd); break;
    case IDC_PROC_COPY: ProcCopyDetails(p->hwnd); break;
    case IDM_VIEW_SELECTCOLUMNS:
        ProcColumns(p->hwnd);
        break;
    case IDM_VIEW_PROCTREE:
        g_cfg.procTreeMode = !g_cfg.procTreeMode;
        ProcSnapshot(p);
        break;
    case IDC_PROC_ALLUSERS:
        if (!App_IsElevated()) {
            App_RelaunchElevated();
        } else {
            g_cfg.showAllUsers = !g_cfg.showAllUsers;
            SysInfo_RefreshNow();
        }
        break;

    case IDC_PROC_ENDPROCESS: {
        int sel, row;
        if (!s_list) break;
        sel = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
        row = ProcVisibleRow(sel);
        if (row < 0 || row >= g_viewCnt) break;
        {
            ProcRow target = g_view[row];
            WCHAR msg[256];
            HANDLE hProc;

            hProc = ProcOpenTarget(&target);
            if (!hProc) { App_ReportError(p->hwnd, L"Open selected process", GetLastError()); break; }

            StringCchPrintfW(msg, ARRAYSIZE(msg),
                L"WARNING: Terminating a process can cause undesired results "
                L"including loss of data and system instability. "
                L"Do you wish to terminate the process \"%s\"?",
                target.imageName);
            if (MessageBoxW(p->hwnd, msg, L"Task Manager Warning",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                if (!TerminateProcess(hProc, 1)) App_ReportError(p->hwnd, L"TerminateProcess", GetLastError());
                SysInfo_RefreshNow();
            }
            CloseHandle(hProc);
        }
        break;
    }
    default:
        break;
    }
}

static void ProcContext(TabPage *p, HWND from, int x, int y)
{
    HMENU menu;
    UINT cmd;
    POINT pt = {x, y};
    if (from != s_list) return;
    if (x == -1 && y == -1) { pt.x = 20; pt.y = 20; ClientToScreen(s_list, &pt); }
    menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDC_PROC_ENDPROCESS, L"&End Process");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDC_PROC_OPENLOCATION, L"Open file &location");
    AppendMenuW(menu, MF_STRING, IDC_PROC_COPY, L"&Copy details");
    cmd = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, p->hwnd, NULL);
    DestroyMenu(menu);
    if (cmd) ProcCommand(p, (int)cmd, 0, NULL);
}

static void ProcBuildViewMenu(TabPage *p, HMENU view)
{
    (void)p;
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, IDM_VIEW_SELECTCOLUMNS, L"&Select Columns...");
    AppendMenuW(view, MF_STRING, IDM_PROC_FIND, L"&Find process\tCtrl+F");
    AppendMenuW(view, MF_STRING, IDC_PROC_EXPORT, L"&Export filtered snapshot...");
    AppendMenuW(view, MF_STRING, IDM_VIEW_PROCTREE, L"Process &Tree");
}

static void ProcInitViewMenu(TabPage *p, HMENU view)
{
    (void)p;
    CheckMenuItem(view, IDM_VIEW_PROCTREE,
                  MF_BYCOMMAND | (g_cfg.procTreeMode ? MF_CHECKED : MF_UNCHECKED));
}

static HWND ProcPrimary(TabPage *p)
{
    (void)p;
    return s_list;
}

static void ProcDestroy(TabPage *p)
{
    (void)p;
    if (s_listTheme) { CloseThemeData(s_listTheme); s_listTheme = NULL; }
    s_list = NULL;
    s_allUsers = NULL;
    s_endProcess = NULL;
    s_search = s_filter = s_details = s_summary = NULL;
    s_query[0] = 0; s_filterMode = 0;
    ProcDiff_Free(&s_mark);
}

static TabPage s_page = {
    L"Processes", NULL, TAB_PROCESSES,
    ProcCreate, ProcDestroy, ProcLayout,
    ProcSnapshot,
    ProcCommand,
    ProcNotify,
    NULL,               /* OnActivate     */
    NULL,               /* OnFontChanged  */
    ProcContext,        /* OnContextMenu  */
    ProcBuildViewMenu,
    ProcInitViewMenu,
    ProcPrimary
};

TabPage *TabProcesses(void) { return &s_page; }
