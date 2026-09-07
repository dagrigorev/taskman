/* ------------------------------------------------------------------------
 * sysinfo.c - the collector thread and all system data acquisition.
 *
 * The UI thread never calls a blocking enumeration API.  A single worker
 * thread samples the machine on the update-speed interval, writes into the
 * back buffer of a double-buffered snapshot, flips the buffers under an
 * SRWLOCK and posts WM_APP_SNAPSHOT_READY to the main window.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ntapi.h"
#include "gpu.h"

/* --------------------------------------------------------- native API --- */

static HMODULE                       g_ntdll;
static PFN_NtQuerySystemInformation  g_pNtQuerySystemInformation;
static PFN_NtQueryInformationProcess g_pNtQueryInformationProcess;
static LONG                          g_ntInitDone;

BOOL Nt_Init(void)
{
    if (InterlockedCompareExchange(&g_ntInitDone, 1, 0) == 0) {
        g_ntdll = GetModuleHandleW(L"ntdll.dll");
        if (g_ntdll) {
            g_pNtQuerySystemInformation = (PFN_NtQuerySystemInformation)
                (void *)GetProcAddress(g_ntdll, "NtQuerySystemInformation");
            g_pNtQueryInformationProcess = (PFN_NtQueryInformationProcess)
                (void *)GetProcAddress(g_ntdll, "NtQueryInformationProcess");
        }
    }
    return g_pNtQuerySystemInformation != NULL;
}

PFN_NtQuerySystemInformation Nt_QuerySystemInformation(void)
{
    Nt_Init();
    return g_pNtQuerySystemInformation;
}

PFN_NtQueryInformationProcess Nt_QueryInformationProcess(void)
{
    Nt_Init();
    return g_pNtQueryInformationProcess;
}

/* ------------------------------------------------------------- state ---- */

typedef struct {
    float  cpu[CTM_HISTORY];
    float  kernel[CTM_HISTORY];
    float  mem[CTM_HISTORY];
    int    head;            /* index of the next slot to write */
    int    count;           /* number of valid samples, <= CTM_HISTORY */
} History;

static Snapshot   g_snap[2];
static int        g_front;
static SRWLOCK    g_lock = SRWLOCK_INIT;

static History    g_hist;
static float      g_cpuHistory[CTM_MAX_CPUS][CTM_HISTORY];
static float      g_kernelHistory[CTM_MAX_CPUS][CTM_HISTORY];
static UINT       g_cpuHistoryCount;
static CTM_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION g_cpuPrevious[CTM_MAX_CPUS];
static UINT       g_cpuPreviousCount;
static float      g_cpuSample[CTM_MAX_CPUS], g_kernelSample[CTM_MAX_CPUS];
static UINT       g_cpuSampleCount;
static SRWLOCK    g_histLock = SRWLOCK_INIT;

static HANDLE     g_thread;
static HANDLE     g_evtQuit;
static HANDLE     g_evtWake;
static HWND       g_notify;
static volatile LONG g_interval = 1000;
static volatile LONG g_activeTab;
static void (*g_tabCollect)(int tab);

/* previous sample, owned by the collector thread only */
static ULONGLONG  g_prevIdle, g_prevKernel, g_prevUser;
static BOOL       g_havePrev;
static ULONG64    g_sequence;

static ULONGLONG FtToU64(const FILETIME *ft)
{
    ULARGE_INTEGER u;
    u.LowPart  = ft->dwLowDateTime;
    u.HighPart = ft->dwHighDateTime;
    return u.QuadPart;
}

static void HistoryPush(float cpu, float kernel, float mem)
{
    UINT i;
    AcquireSRWLockExclusive(&g_histLock);
    if (g_cpuHistoryCount != g_cpuSampleCount) {
        ZeroMemory(g_cpuHistory, sizeof(g_cpuHistory));
        ZeroMemory(g_kernelHistory, sizeof(g_kernelHistory));
        g_cpuHistoryCount = g_cpuSampleCount;
    }
    for (i = 0; i < g_cpuHistoryCount; ++i) {
        g_cpuHistory[i][g_hist.head] = g_cpuSample[i];
        g_kernelHistory[i][g_hist.head] = g_kernelSample[i];
    }
    g_hist.cpu[g_hist.head]    = cpu;
    g_hist.kernel[g_hist.head] = kernel;
    g_hist.mem[g_hist.head]    = mem;
    g_hist.head = (g_hist.head + 1) % CTM_HISTORY;
    if (g_hist.count < CTM_HISTORY) g_hist.count++;
    ReleaseSRWLockExclusive(&g_histLock);
}

static int HistoryCopy(const float *src, float *dst, int count)
{
    int available, i, idx;

    if (!dst || count <= 0) return 0;
    AcquireSRWLockShared(&g_histLock);
    available = g_hist.count < count ? g_hist.count : count;
    for (i = 0; i < count; i++) dst[i] = 0.0f;
    /* newest sample lands in dst[count-1] */
    for (i = 0; i < available; i++) {
        idx = (g_hist.head - 1 - i + 2 * CTM_HISTORY) % CTM_HISTORY;
        dst[count - 1 - i] = src[idx];
    }
    ReleaseSRWLockShared(&g_histLock);
    return available;
}

int SysInfo_CopyCpuHistory(float *dst, int count)
{
    return HistoryCopy(g_hist.cpu, dst, count);
}

int SysInfo_CopyKernelHistory(float *dst, int count)
{
    return HistoryCopy(g_hist.kernel, dst, count);
}

int SysInfo_CopyMemHistory(float *dst, int count)
{
    return HistoryCopy(g_hist.mem, dst, count);
}

UINT SysInfo_CpuHistoryCount(void)
{
    UINT count;
    AcquireSRWLockShared(&g_histLock);
    count = g_cpuHistoryCount;
    ReleaseSRWLockShared(&g_histLock);
    return count;
}

int SysInfo_CopyProcessorHistory(UINT cpu, float *busy, float *kernel, int count)
{
    int available = 0, i;
    if (!busy || count <= 0) return 0;
    AcquireSRWLockShared(&g_histLock);
    for (i = 0; i < count; ++i) { busy[i] = 0; if (kernel) kernel[i] = 0; }
    if (cpu < g_cpuHistoryCount) {
        available = g_hist.count < count ? g_hist.count : count;
        for (i = 0; i < available; ++i) {
            int idx = (g_hist.head - 1 - i + 2 * CTM_HISTORY) % CTM_HISTORY;
            busy[count - 1 - i] = g_cpuHistory[cpu][idx];
            if (kernel) kernel[count - 1 - i] = g_kernelHistory[cpu][idx];
        }
    }
    ReleaseSRWLockShared(&g_histLock);
    return available;
}

/* ------------------------------------------------------------ sampling -- */

static UINT LogicalProcessorCount(void)
{
    SYSTEM_INFO si;
    DWORD n;

    /* GetActiveProcessorCount covers every processor group; fall back to the
       group of the current thread when the export is missing. */
    typedef DWORD (WINAPI *PFN_GetActiveProcessorCount)(WORD);
    static PFN_GetActiveProcessorCount pfn;
    static LONG resolved;

    if (InterlockedCompareExchange(&resolved, 1, 0) == 0) {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32)
            pfn = (PFN_GetActiveProcessorCount)(void *)
                  GetProcAddress(k32, "GetActiveProcessorCount");
    }
    if (pfn) {
        n = pfn(0xffff /* ALL_PROCESSOR_GROUPS */);
        if (n > 0) return (UINT)n;
    }
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (UINT)si.dwNumberOfProcessors : 1u;
}

static void CpuPercent(ULONGLONG idle, ULONGLONG kernel, ULONGLONG user,
                       ULONGLONG prevIdle, ULONGLONG prevKernel, ULONGLONG prevUser,
                       float *busy, float *kern)
{
    double total, idleDelta, kernelDelta;
    *busy = *kern = 0;
    if (idle < prevIdle || kernel < prevKernel || user < prevUser) return;
    kernelDelta = (double)(kernel - prevKernel);
    idleDelta = (double)(idle - prevIdle);
    total = kernelDelta + (double)(user - prevUser);
    if (total <= 0 || idleDelta > total || idleDelta > kernelDelta) return;
    *busy = (float)((total - idleDelta) * 100.0 / total);
    *kern = (float)((kernelDelta - idleDelta) * 100.0 / total);
}

static void CollectProcessors(Snapshot *s)
{
    typedef CTM_NTSTATUS (NTAPI *QueryEx)(ULONG, PVOID, ULONG, PVOID, ULONG, PULONG);
    QueryEx queryEx = (QueryEx)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformationEx");
    PFN_NtQuerySystemInformation query = Nt_QuerySystemInformation();
    CTM_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION current[CTM_MAX_CPUS];
    UINT count = 0, i;
    WORD group, groups = GetActiveProcessorGroupCount();
    double totalBusy = 0, totalKernel = 0;
    g_cpuSampleCount = 0;
    if (!query) { g_cpuPreviousCount = 0; return; }
    for (group = 0; group < (queryEx ? groups : 1); ++group) {
        ULONG returned = 0, capacity = (CTM_MAX_CPUS - count) * sizeof(current[0]);
        CTM_NTSTATUS status;
        if (!capacity) break;
        if (queryEx)
            status = queryEx(CtmSystemProcessorPerformanceInformation, &group, sizeof(group),
                             current + count, capacity, &returned);
        else
            status = query(CtmSystemProcessorPerformanceInformation, current + count, capacity, &returned);
        if (!NT_SUCCESS(status) || !returned || returned > capacity || returned % sizeof(current[0])) {
            g_cpuPreviousCount = 0; return;
        }
        count += returned / sizeof(current[0]);
    }
    for (i = 0; i < count; ++i) {
        g_cpuSample[i] = g_kernelSample[i] = 0;
        if (count == g_cpuPreviousCount)
            CpuPercent((ULONGLONG)current[i].IdleTime.QuadPart,
                       (ULONGLONG)current[i].KernelTime.QuadPart,
                       (ULONGLONG)current[i].UserTime.QuadPart,
                       (ULONGLONG)g_cpuPrevious[i].IdleTime.QuadPart,
                       (ULONGLONG)g_cpuPrevious[i].KernelTime.QuadPart,
                       (ULONGLONG)g_cpuPrevious[i].UserTime.QuadPart,
                       &g_cpuSample[i], &g_kernelSample[i]);
        totalBusy += g_cpuSample[i]; totalKernel += g_kernelSample[i];
    }
    if (count && count == s->cpuCount && count == g_cpuPreviousCount) {
        s->cpuUsage = totalBusy / count;
        s->cpuKernel = totalKernel / count;
    }
    memcpy(g_cpuPrevious, current, count * sizeof(current[0]));
    g_cpuPreviousCount = g_cpuSampleCount = count;
}

static void CollectOnce(Snapshot *s)
{
    FILETIME ftIdle, ftKernel, ftUser;
    MEMORYSTATUSEX ms;
    PERFORMANCE_INFORMATION pi;
    ULONGLONG pageSize = 4096;

    ZeroMemory(s, sizeof(*s));
    s->cpuCount = LogicalProcessorCount();

    /* ---- CPU ---------------------------------------------------------- */
    if (GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) {
        ULONGLONG idle   = FtToU64(&ftIdle);
        ULONGLONG kernel = FtToU64(&ftKernel);   /* includes idle */
        ULONGLONG user   = FtToU64(&ftUser);

        if (g_havePrev) {
            float busy, kern;
            CpuPercent(idle, kernel, user, g_prevIdle, g_prevKernel, g_prevUser, &busy, &kern);
            s->cpuUsage = busy; s->cpuKernel = kern;
        }
        g_prevIdle   = idle;
        g_prevKernel = kernel;
        g_prevUser   = user;
        g_havePrev   = TRUE;
    }
    CollectProcessors(s);

    /* ---- memory ------------------------------------------------------- */
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s->memTotal = ms.ullTotalPhys;
        s->memAvail = ms.ullAvailPhys;
        s->memUsage = ms.ullTotalPhys
                      ? (double)(ms.ullTotalPhys - ms.ullAvailPhys) * 100.0
                        / (double)ms.ullTotalPhys
                      : 0.0;
    }

    ZeroMemory(&pi, sizeof(pi));
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        pageSize            = (ULONGLONG)pi.PageSize;
        s->processCount     = pi.ProcessCount;
        s->threadCount      = pi.ThreadCount;
        s->handleCount      = pi.HandleCount;
        s->commitTotal      = (ULONGLONG)pi.CommitTotal * pageSize;
        s->commitLimit      = (ULONGLONG)pi.CommitLimit * pageSize;
        s->kernelPaged      = (ULONGLONG)pi.KernelPaged * pageSize;
        s->kernelNonPaged   = (ULONGLONG)pi.KernelNonpaged * pageSize;
        s->memCached        = (ULONGLONG)pi.SystemCache * pageSize;
        if (!s->memTotal)
            s->memTotal = (ULONGLONG)pi.PhysicalTotal * pageSize;
        if (!s->memAvail)
            s->memAvail = (ULONGLONG)pi.PhysicalAvailable * pageSize;
    }
    /* Free = available minus what the cache is holding on the standby list. */
    s->memFree = (s->memAvail > s->memCached) ? (s->memAvail - s->memCached) : 0;

    s->upTimeMs = GetTickCount64();
}

static DWORD WINAPI CollectorProc(LPVOID param)
{
    HANDLE waits[2];
    (void)param;

    waits[0] = g_evtQuit;
    waits[1] = g_evtWake;

    for (;;) {
        Snapshot *back;
        LONG interval;
        DWORD wait;

        back = &g_snap[1 - g_front];
        CollectOnce(back);
        back->sequence = ++g_sequence;

        HistoryPush((float)back->cpuUsage, (float)back->cpuKernel,
                    (float)back->memUsage);

        /* Before the tab collectors: Proc_Collect reads the per-process
           figures, and refreshing them afterwards would leave the Processes
           tab a sample behind. Gpu_Collect returns immediately unless
           something has enabled it. */
        Gpu_Collect(GetTickCount64());

        if (g_tabCollect)
            g_tabCollect((int)InterlockedCompareExchange(&g_activeTab, 0, 0));

        AcquireSRWLockExclusive(&g_lock);
        g_front = 1 - g_front;
        ReleaseSRWLockExclusive(&g_lock);

        if (g_notify) PostMessageW(g_notify, WM_APP_SNAPSHOT_READY, 0, 0);

        do {
            interval = InterlockedCompareExchange(&g_interval, 0, 0);
            wait = WaitForMultipleObjects(2, waits, FALSE,
                                          interval > 0 ? (DWORD)interval : INFINITE);
            /* A timer armed before Pause must not start a new sample. An
               explicit wake (F5 or switching tabs) still samples once. */
        } while (wait == WAIT_TIMEOUT && InterlockedCompareExchange(&g_interval, 0, 0) == 0);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        /* WAIT_OBJECT_0 + 1 (wake) and WAIT_TIMEOUT both fall through */
    }
    return 0;
}

/* ---------------------------------------------------------------- api ---- */

BOOL SysInfo_Start(HWND notify)
{
    if (g_thread) return FALSE;
    Nt_Init();
    g_front = 0;
    g_havePrev = FALSE;
    ZeroMemory(g_snap, sizeof(g_snap));
    ZeroMemory(&g_hist, sizeof(g_hist));
    g_cpuHistoryCount = g_cpuPreviousCount = g_cpuSampleCount = 0;
    ZeroMemory(g_cpuHistory, sizeof(g_cpuHistory));
    ZeroMemory(g_kernelHistory, sizeof(g_kernelHistory));
    g_notify  = notify;
    g_evtQuit = CreateEventW(NULL, TRUE,  FALSE, NULL);
    g_evtWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_evtQuit || !g_evtWake) {
        SysInfo_Stop();
        return FALSE;
    }

    /* Prime the first sample synchronously so the very first paint has data. */
    CollectOnce(&g_snap[0]);
    g_sequence = 1;
    g_snap[0].sequence = g_sequence;

    g_thread = CreateThread(NULL, 0, CollectorProc, NULL, 0, NULL);
    if (!g_thread) {
        SysInfo_Stop();
        return FALSE;
    }
    return TRUE;
}

BOOL SysInfo_Stopping(void)
{
    /* Read once: SysInfo_Stop only closes the handle after joining the
       worker, so a collector still running cannot observe a stale one. */
    HANDLE quit = g_evtQuit;
    return quit && WaitForSingleObject(quit, 0) == WAIT_OBJECT_0;
}

void SysInfo_Stop(void)
{
    if (g_evtQuit) SetEvent(g_evtQuit);
    if (g_thread) {
        /* The worker only waits on our events and never sends synchronous
           UI messages. Its event handles must survive until it has exited. */
        if (WaitForSingleObject(g_thread, INFINITE) != WAIT_OBJECT_0) return;
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_evtQuit) { CloseHandle(g_evtQuit); g_evtQuit = NULL; }
    if (g_evtWake) { CloseHandle(g_evtWake); g_evtWake = NULL; }
    g_notify = NULL;
}

void SysInfo_SetSpeed(int speed)
{
    LONG ms;
    switch (speed) {
    case SPEED_HIGH:   ms = 500;  break;
    case SPEED_LOW:    ms = 4000; break;
    case SPEED_PAUSED: ms = 0;    break;
    case SPEED_NORMAL:
    default:           ms = 1000; break;
    }
    InterlockedExchange(&g_interval, ms);
    if (g_evtWake && ms != 0) SetEvent(g_evtWake);
}

void SysInfo_RefreshNow(void)
{
    if (g_evtWake) SetEvent(g_evtWake);
}

void SysInfo_SetActiveTab(int tab)
{
    InterlockedExchange(&g_activeTab, tab);
    SysInfo_RefreshNow();
}

int SysInfo_ActiveTab(void)
{
    return (int)InterlockedCompareExchange(&g_activeTab, 0, 0);
}

void SysInfo_SetTabCollector(void (*collect)(int tab))
{
    g_tabCollect = collect;
}

const Snapshot *SysInfo_Lock(void)
{
    AcquireSRWLockShared(&g_lock);
    return &g_snap[g_front];
}

void SysInfo_Unlock(void)
{
    ReleaseSRWLockShared(&g_lock);
}
