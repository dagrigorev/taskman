#include "../include/app.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); SysInfo_Stop(); return 1; } } while (0)
static Snapshot ReadSnapshot(void)
{
    const Snapshot *shared = SysInfo_Lock();
    Snapshot copy = *shared;
    SysInfo_Unlock();
    return copy;
}
static BOOL WaitForSequence(ULONG64 sequence)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        if (ReadSnapshot().sequence >= sequence) return TRUE;
        Sleep(10);
    }
    return FALSE;
}
int main(void)
{
    Snapshot snap;
    float history[16];
    int pass;
    for (pass = 0; pass < 3; ++pass) {
        SysInfo_SetSpeed(SPEED_PAUSED);
        CHECK(SysInfo_Start(NULL));
        CHECK(WaitForSequence(2));
        snap = ReadSnapshot();
        CHECK(snap.cpuCount >= 1 && snap.memTotal > 0);
        CHECK(snap.cpuUsage >= 0 && snap.cpuUsage <= 100);
        CHECK(snap.cpuKernel >= 0 && snap.cpuKernel <= snap.cpuUsage);
        CHECK(snap.memUsage >= 0 && snap.memUsage <= 100);
        Sleep(100);
        CHECK(ReadSnapshot().sequence == snap.sequence);
        SysInfo_RefreshNow();
        CHECK(WaitForSequence(snap.sequence + 1));
        CHECK(SysInfo_CopyCpuHistory(history, ARRAYSIZE(history)) >= 2);
        CHECK(SysInfo_CpuHistoryCount() > 0 && SysInfo_CpuHistoryCount() <= snap.cpuCount);
        CHECK(SysInfo_CopyProcessorHistory(0, history, NULL, ARRAYSIZE(history)) >= 2);
        CHECK(history[15] >= 0 && history[15] <= 100);
        snap = ReadSnapshot();
        SysInfo_SetActiveTab(TAB_NETWORKING);
        CHECK(WaitForSequence(snap.sequence + 1));
        SysInfo_Stop();
        snap = ReadSnapshot();
        Sleep(50);
        CHECK(ReadSnapshot().sequence == snap.sequence);
    }
    puts("PASS live collector sampling, pause, refresh, shutdown and repeated restart");
    return 0;
}
