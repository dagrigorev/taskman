#include "../include/app.h"
#include "../include/gpu.h"
#include <stdio.h>

static int eventCalls, failEvent, closeCalls, threadCalls, multipleWaits;
static int prematureClose;
static BOOL failThread, workerDone = TRUE;
static DWORD stopTimeout;
static BOOL pauseBoundary;
static DWORD secondTimeout;
static HANDLE WINAPI TestCreateEvent(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, const WCHAR *name)
{
    if (++eventCalls == failEvent) return NULL;
    return CreateEventW(sa, manual, initial, name);
}
static HANDLE WINAPI TestCreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack,
                                      LPTHREAD_START_ROUTINE proc, void *param, DWORD flags, DWORD *id)
{
    (void)sa; (void)stack; (void)proc; (void)param; (void)flags; (void)id;
    ++threadCalls;
    if (failThread) return NULL;
    workerDone = FALSE;
    /* Stand in for a slow worker; no application worker runs in these tests. */
    return CreateEventW(NULL, TRUE, FALSE, NULL);
}
static DWORD WINAPI TestWaitSingle(HANDLE handle, DWORD timeout)
{
    (void)handle;
    stopTimeout = timeout;
    if (timeout != INFINITE) return WAIT_TIMEOUT;
    workerDone = TRUE;
    return WAIT_OBJECT_0;
}
static BOOL WINAPI TestClose(HANDLE handle)
{
    if (!workerDone) ++prematureClose;
    ++closeCalls;
    return CloseHandle(handle);
}
static DWORD WINAPI TestWaitMultiple(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout)
{
    (void)count; (void)handles; (void)all; (void)timeout;
    if (pauseBoundary) {
        if (++multipleWaits == 1) { SysInfo_SetSpeed(SPEED_PAUSED); return WAIT_TIMEOUT; }
        secondTimeout = timeout;
        return WAIT_OBJECT_0;
    }
    /* The second result prevents the original faulty loop hanging a test. */
    return ++multipleWaits == 1 ? WAIT_FAILED : WAIT_OBJECT_0;
}
#define CreateEventW TestCreateEvent
#define CreateThread TestCreateThread
#define CloseHandle TestClose
#define WaitForSingleObject TestWaitSingle
#define WaitForMultipleObjects TestWaitMultiple
#include "../src/sysinfo.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void)
{
    int n;
    float history[4];
    float busy, kernel;
    int gpuCount = -1;
    /* Nothing should pay the PDH enumeration cost until the Sensors tab or
       the per-process column asks for it: the flag defaults to off and a
       collect while disabled must publish nothing. The ordering requirement
       -- Gpu_Collect before g_tabCollect -- is NOT asserted here; it is a
       statement about source order inside CollectorProc with no observable
       effect until the per-process column exists. Verify it by reading. */
    CHECK(Gpu_IsEnabled() == FALSE);
    Gpu_Collect(GetTickCount64());
    Gpu_Lock(&gpuCount);
    Gpu_Unlock();
    CHECK(gpuCount == 0);
    CpuPercent(120, 150, 50, 100, 100, 0, &busy, &kernel);
    CHECK(busy == 80 && kernel == 30);
    CpuPercent(0, 150, 50, 100, 100, 0, &busy, &kernel);
    CHECK(busy == 0 && kernel == 0);
    CpuPercent(300, 150, 50, 100, 100, 0, &busy, &kernel);
    CHECK(busy == 0 && kernel == 0);
    failEvent = 2;
    CHECK(!SysInfo_Start(NULL));
    CHECK(closeCalls == 1 && !g_evtQuit && !g_evtWake && !g_thread);
    eventCalls = 0; failEvent = 1; closeCalls = 0;
    CHECK(!SysInfo_Start(NULL));
    CHECK(!g_evtQuit && !g_evtWake && !g_thread);
    eventCalls = 0; failEvent = 0; closeCalls = 0; failThread = TRUE;
    CHECK(!SysInfo_Start(NULL));
    CHECK(closeCalls == 2 && !g_evtQuit && !g_evtWake && !g_thread);
    failThread = FALSE; closeCalls = 0;
    CHECK(SysInfo_Start(NULL));
    n = threadCalls;
    CHECK(!SysInfo_Start(NULL) && threadCalls == n);
    SysInfo_Stop();
    CHECK(stopTimeout == INFINITE && prematureClose == 0);
    CHECK(closeCalls == 3 && !g_thread && !g_evtQuit && !g_evtWake);
    SysInfo_Stop();
    CHECK(closeCalls == 3);
    CHECK(SysInfo_Start(NULL));
    CHECK(g_front == 0 && g_hist.count == 0 && g_snap[0].sequence == 1);
    SysInfo_Stop();
    multipleWaits = 0;
    CollectorProc(NULL);
    CHECK(multipleWaits == 1); /* WAIT_FAILED must not busy-loop */
    {
        ULONG64 before = g_sequence;
        multipleWaits = 0; pauseBoundary = TRUE;
        SysInfo_SetSpeed(SPEED_NORMAL);
        CollectorProc(NULL);
        CHECK(multipleWaits == 2 && secondTimeout == INFINITE);
        CHECK(g_sequence == before + 1); /* The armed timer cannot sample after Pause. */
        pauseBoundary = FALSE;
    }
    ZeroMemory(&g_hist, sizeof(g_hist));
    CHECK(SysInfo_CopyCpuHistory(history, 4) == 0 && history[3] == 0.0f);
    HistoryPush(12, 6, 40);
    CHECK(SysInfo_CopyCpuHistory(history, 4) == 1);
    CHECK(history[0] == 0 && history[2] == 0 && history[3] == 12);
    for (n = 0; n < CTM_HISTORY + 3; ++n) HistoryPush((float)n, (float)n / 2, 50);
    CHECK(SysInfo_CopyCpuHistory(history, 4) == 4);
    CHECK(history[0] == 1023 && history[1] == 1024 && history[2] == 1025 && history[3] == 1026);
    CHECK(SysInfo_CopyKernelHistory(history, 4) == 4 && history[3] == 513);
    CHECK(SysInfo_CopyMemHistory(history, 4) == 4 && history[3] == 50);
    CHECK(SysInfo_CopyCpuHistory(NULL, 0) == 0);
    CHECK(SysInfo_CopyCpuHistory(NULL, 4) == 0);
    g_cpuSampleCount = 2;
    g_cpuSample[0] = 25; g_cpuSample[1] = 75;
    g_kernelSample[0] = 5; g_kernelSample[1] = 20;
    HistoryPush(50, 12.5f, 50);
    CHECK(SysInfo_CpuHistoryCount() == 2);
    CHECK(SysInfo_CopyProcessorHistory(0, history, NULL, 4) == 4 && history[3] == 25);
    CHECK(SysInfo_CopyProcessorHistory(1, history, NULL, 4) == 4 && history[3] == 75);
    CHECK(SysInfo_CopyProcessorHistory(2, history, NULL, 4) == 0 && history[3] == 0);
    puts("PASS collector failure cleanup, shutdown, restart and history wraparound");
    return 0;
}
