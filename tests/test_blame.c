/* Exercises Spike Blame's ranking, ring, coordinate mapping and collection
   with synthetic data. NtQuerySystemInformation is replaced by a fixture
   that serves hand-built process records. */
#include "../include/app.h"
#include "../include/ntapi.h"
#include "../include/blame.h"
#include <stdio.h>

typedef struct {
    CTM_SYSTEM_PROCESS_INFORMATION info;
    WCHAR name[32];
} FakeRecord;

static FakeRecord fakes[8];
static ULONG fakeCount;
static BOOL failQuery;

static CTM_NTSTATUS WINAPI FakeQuery(ULONG cls, PVOID buf, ULONG len, PULONG needed)
{
    ULONG i, size = fakeCount * (ULONG)sizeof(FakeRecord);
    (void)cls;
    if (failQuery) return (CTM_NTSTATUS)0xC0000001L;
    if (needed) *needed = size;
    if (len < size) return (CTM_NTSTATUS)0xC0000004L;
    memcpy(buf, fakes, size);
    /* Rebase the image name pointers into the caller's buffer. */
    for (i = 0; i < fakeCount; ++i) {
        FakeRecord *r = (FakeRecord *)((BYTE *)buf + i * sizeof(FakeRecord));
        r->info.ImageName.Buffer = r->name;
    }
    return 0;
}

static PFN_NtQuerySystemInformation TestNtQuery(void) { return FakeQuery; }
#define Nt_QuerySystemInformation TestNtQuery

static ULONGLONG fakeTick;
static ULONGLONG TestTick(void) { return fakeTick; }
#define GetTickCount64 TestTick

#include "../src/blame.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static BlameEntry Entry(DWORD pid, float cpu, ULONGLONG bytes)
{
    BlameEntry e;
    ZeroMemory(&e, sizeof(e));
    e.pid = pid; e.cpuPct = cpu; e.privateBytes = bytes;
    return e;
}

static void TestOfferKeepsTopDescending(void)
{
    BlameEntry top[BLAME_TOP], e;
    int count = 0;
    float values[] = { 3, 9, 1, 7, 5, 8, 2 };
    int i;
    for (i = 0; i < 7; ++i) {
        e = Entry((DWORD)(i + 1), values[i], 0);
        Blame_Offer(top, &count, &e, BLAME_BY_CPU);
    }
    CHECK(count == BLAME_TOP);
    CHECK(top[0].cpuPct == 9 && top[1].cpuPct == 8 && top[2].cpuPct == 7);
    CHECK(top[3].cpuPct == 5 && top[4].cpuPct == 3);
}

static void TestOfferTiesAndIdle(void)
{
    BlameEntry top[BLAME_TOP], e;
    int count = 0;
    e = Entry(10, 4, 0); Blame_Offer(top, &count, &e, BLAME_BY_CPU);
    e = Entry(20, 4, 0); Blame_Offer(top, &count, &e, BLAME_BY_CPU);
    e = Entry(30, 0, 0); Blame_Offer(top, &count, &e, BLAME_BY_CPU);
    CHECK(count == 2);
    CHECK(top[0].pid == 10 && top[1].pid == 20);
}

static void TestOfferByMemory(void)
{
    BlameEntry top[BLAME_TOP], e;
    int count = 0;
    e = Entry(1, 50, 100); Blame_Offer(top, &count, &e, BLAME_BY_MEM);
    e = Entry(2, 0, 900);  Blame_Offer(top, &count, &e, BLAME_BY_MEM);
    CHECK(count == 2);
    CHECK(top[0].pid == 2);
}

static void TestRingAlignsNewestLast(void)
{
    static BlameRing ring;
    BlameSample s, out[4];
    int i;
    Blame_RingReset(&ring);
    ZeroMemory(&s, sizeof(s));
    for (i = 1; i <= 2; ++i) { s.sequence = (ULONG64)i; Blame_RingPush(&ring, &s); }
    CHECK(Blame_RingCopy(&ring, out, 4) == 2);
    CHECK(out[0].sequence == 0 && out[1].sequence == 0);
    CHECK(out[2].sequence == 1 && out[3].sequence == 2);
}

static void TestRingWraps(void)
{
    static BlameRing ring;
    static BlameSample out[BLAME_SAMPLES];
    BlameSample s;
    int i;
    Blame_RingReset(&ring);
    ZeroMemory(&s, sizeof(s));
    for (i = 1; i <= BLAME_SAMPLES + 3; ++i) { s.sequence = (ULONG64)i; Blame_RingPush(&ring, &s); }
    CHECK(Blame_RingCopy(&ring, out, BLAME_SAMPLES) == BLAME_SAMPLES);
    CHECK(out[0].sequence == 4);
    CHECK(out[BLAME_SAMPLES - 1].sequence == (ULONG64)BLAME_SAMPLES + 3);
}

/* The mapping must agree with UI_Chart's i * (width - 1) / (count - 1). */
static void TestCoordinateMapping(void)
{
    int i;
    CHECK(Blame_IndexFromX(0, 128, 128) == 0);
    CHECK(Blame_IndexFromX(127, 128, 128) == 127);
    CHECK(Blame_IndexFromX(-5, 128, 128) == 0);
    CHECK(Blame_IndexFromX(900, 128, 128) == 127);
    for (i = 0; i < 128; ++i)
        CHECK(Blame_IndexFromX(Blame_XFromIndex(i, 400, 128), 400, 128) == i);
    CHECK(Blame_IndexFromX(10, 1, 128) == 127);
}

static void TestFindPeak(void)
{
    BlameSample s[4];
    ZeroMemory(s, sizeof(s));
    CHECK(Blame_FindPeak(s, 4, BLAME_BY_CPU) == -1);
    s[1].sequence = 1; s[1].cpu = 80; s[1].mem = 10;
    s[2].sequence = 2; s[2].cpu = 80; s[2].mem = 30;
    s[3].sequence = 3; s[3].cpu = 20; s[3].mem = 20;
    CHECK(Blame_FindPeak(s, 4, BLAME_BY_CPU) == 2);
    CHECK(Blame_FindPeak(s, 4, BLAME_BY_MEM) == 2);
    CHECK(Blame_FindSequence(s, 4, 3) == 3);
    CHECK(Blame_FindSequence(s, 4, 99) == -1);
    CHECK(Blame_FindSequence(s, 4, 0) == -1);
}

static void Fake(ULONG i, DWORD pid, LONGLONG userTime, LONGLONG privateBytes, const WCHAR *name)
{
    FakeRecord *r = &fakes[i];
    ZeroMemory(r, sizeof(*r));
    r->info.NextEntryOffset = 0;
    r->info.UniqueProcessId = (HANDLE)(ULONG_PTR)pid;
    r->info.CreateTime.QuadPart = 1000 + pid;
    r->info.UserTime.QuadPart = userTime;
    r->info.WorkingSetPrivateSize.QuadPart = privateBytes;
    StringCchCopyW(r->name, ARRAYSIZE(r->name), name);
    r->info.ImageName.Length = (USHORT)(lstrlenW(name) * sizeof(WCHAR));
    r->info.ImageName.MaximumLength = sizeof(r->name);
}

static void Link(void)
{
    ULONG i;
    for (i = 0; i + 1 < fakeCount; ++i) fakes[i].info.NextEntryOffset = sizeof(FakeRecord);
}

static void TestCollectRanksByDelta(void)
{
    BlameSample out[2];
    ULONGLONG cpus = Blame_ProcessorCount();
    ULONGLONG unit = cpus * 100000ULL; /* 1% of one second across all CPUs */

    Blame_Reset();
    fakeTick = 5000;
    fakeCount = 3;
    Fake(0, 0, 0, 0, L"Idle");
    Fake(1, 100, 0, 5000, L"busy.exe");
    Fake(2, 200, 0, 9000, L"calm.exe");
    Link();
    Blame_Collect(1, 10, 20);

    fakeTick = 6000;
    fakes[1].info.UserTime.QuadPart = (LONGLONG)(40 * unit);
    fakes[2].info.UserTime.QuadPart = (LONGLONG)(2 * unit);
    fakes[0].info.UserTime.QuadPart = (LONGLONG)(58 * unit);
    Blame_Collect(2, 42, 21);

    CHECK(Blame_Copy(out, 2) == 2);
    /* First sample has no baseline: CPU culprits unknown, memory known. */
    CHECK(out[0].cpuCount == 0);
    CHECK(out[0].memCount == 2);
    CHECK(out[1].sequence == 2 && out[1].cpu == 42);
    CHECK(out[1].cpuCount == 2);
    CHECK(out[1].byCpu[0].pid == 100);
    CHECK(out[1].byCpu[0].cpuPct > 39.9f && out[1].byCpu[0].cpuPct < 40.1f);
    CHECK(!lstrcmpW(out[1].byCpu[0].image, L"busy.exe"));
    CHECK(out[1].byCpu[1].pid == 200);
    CHECK(out[1].byMem[0].pid == 200);
}

/* A reused pid restarts its baseline instead of inheriting a huge delta. */
static void TestCollectPidReuse(void)
{
    BlameSample out[1];
    Blame_Reset();
    fakeTick = 1000;
    fakeCount = 1;
    Fake(0, 300, 1000000000LL, 1, L"old.exe");
    Blame_Collect(1, 0, 0);
    fakeTick = 2000;
    Fake(0, 300, 1000000001LL, 1, L"new.exe");
    fakes[0].info.CreateTime.QuadPart = 777;
    Blame_Collect(2, 0, 0);
    CHECK(Blame_Copy(out, 1) == 1);
    CHECK(out[0].cpuCount == 0);
}

static void TestCollectFailureStillAligns(void)
{
    BlameSample out[1];
    Blame_Reset();
    failQuery = TRUE;
    Blame_Collect(7, 33, 44);
    failQuery = FALSE;
    CHECK(Blame_Copy(out, 1) == 1);
    CHECK(out[0].sequence == 7 && out[0].cpu == 33);
    CHECK(out[0].cpuCount == 0 && out[0].memCount == 0);
}

/* Liveness comes from the latest enumeration, so a protected process
   that refuses OpenProcess still counts, and a reused pid does not. */
static void TestAliveFromEnumeration(void)
{
    Blame_Reset();
    CHECK(!Blame_IsAlive(100, 1100));
    fakeTick = 1000;
    fakeCount = 2;
    Fake(0, 100, 0, 1, L"a.exe");
    Fake(1, 200, 0, 1, L"b.exe");
    Link();
    Blame_Collect(1, 0, 0);
    CHECK(Blame_IsAlive(100, 1100));
    CHECK(Blame_IsAlive(200, 1200));
    CHECK(!Blame_IsAlive(100, 999));      /* same pid, other process */

    fakeTick = 2000;
    fakeCount = 1;
    Fake(0, 200, 0, 1, L"b.exe");
    Blame_Collect(2, 0, 0);
    CHECK(!Blame_IsAlive(100, 1100));
    CHECK(Blame_IsAlive(200, 1200));

    /* A failed enumeration keeps the last known list rather than
       declaring every process dead. */
    failQuery = TRUE;
    Blame_Collect(3, 0, 0);
    failQuery = FALSE;
    CHECK(Blame_IsAlive(200, 1200));
}

int main(void)
{
    TestOfferKeepsTopDescending();
    TestOfferTiesAndIdle();
    TestOfferByMemory();
    TestRingAlignsNewestLast();
    TestRingWraps();
    TestCoordinateMapping();
    TestFindPeak();
    TestCollectRanksByDelta();
    TestCollectPidReuse();
    TestCollectFailureStillAligns();
    TestAliveFromEnumeration();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_blame: all passed\n");
    return 0;
}
