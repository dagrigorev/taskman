/* Exercises GPU instance parsing and aggregation with synthetic data.
   No UI stubs needed: gpu.c calls nothing from user32 or comctl32. */
#include "../include/gpu.h"
#include <stdio.h>
#include <strsafe.h>
#include "../src/gpu.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

/* Real instance names copied verbatim from a live machine. */
static void TestParseInstance(void)
{
    GpuInstance g;

    CHECK(Gpu_ParseEngineInstance(
        L"pid_55648_luid_0x00000000_0x00012cae_phys_0_eng_0_engtype_3d", &g));
    CHECK(g.pid == 55648);
    CHECK(g.luid == 0x0000000000012caeULL);
    CHECK(g.phys == 0);
    CHECK(g.engine == 0);
    CHECK(g.kind == GPU_ENGINE_3D);

    CHECK(Gpu_ParseEngineInstance(
        L"pid_25944_luid_0x00000000_0x00014b13_phys_0_eng_4_engtype_copy", &g));
    CHECK(g.pid == 25944);
    CHECK(g.luid == 0x0000000000014b13ULL);
    CHECK(g.engine == 4);
    CHECK(g.kind == GPU_ENGINE_COPY);
}

/* A non-zero LUID high half must not be truncated or swapped with the low. */
static void TestParseLuidHighHalf(void)
{
    GpuInstance g;
    CHECK(Gpu_ParseEngineInstance(
        L"pid_1_luid_0x0000abcd_0x00012cae_phys_0_eng_0_engtype_3d", &g));
    CHECK(g.luid == 0x0000abcd00012caeULL);
}

/* The probe saw an empty engtype among the distinct values. It must classify
   as OTHER and still parse, not be rejected: dropping the instance would
   understate the adapter's maximum. */
static void TestParseEmptyAndUnknownEngtype(void)
{
    GpuInstance g;

    CHECK(Gpu_ParseEngineInstance(
        L"pid_7_luid_0x00000000_0x00012cae_phys_0_eng_2_engtype_", &g));
    CHECK(g.kind == GPU_ENGINE_OTHER);

    CHECK(Gpu_ParseEngineInstance(
        L"pid_7_luid_0x00000000_0x00012cae_phys_0_eng_3_engtype_ofa_0", &g));
    CHECK(g.kind == GPU_ENGINE_OTHER);

    CHECK(Gpu_ParseEngineInstance(
        L"pid_7_luid_0x00000000_0x00012cae_phys_0_eng_5_engtype_legacyoverlay", &g));
    CHECK(g.kind == GPU_ENGINE_OTHER);
}

/* "_eng_" is a prefix of nothing in "_engtype_" (which reads "_engt"), but a
   sloppy search would still confuse them. Pin the behaviour. */
static void TestParseEngineIndexNotEngtype(void)
{
    GpuInstance g;
    CHECK(Gpu_ParseEngineInstance(
        L"pid_9_luid_0x00000000_0x00012cae_phys_1_eng_11_engtype_videodecode", &g));
    CHECK(g.engine == 11);
    CHECK(g.phys == 1);
    CHECK(g.kind == GPU_ENGINE_VIDEO_DECODE);
}

static void TestParseRejectsGarbage(void)
{
    GpuInstance g;
    CHECK(!Gpu_ParseEngineInstance(NULL, &g));
    CHECK(!Gpu_ParseEngineInstance(L"", &g));
    CHECK(!Gpu_ParseEngineInstance(L"luid_0x0_0x1_phys_0_eng_0_engtype_3d", &g));
    CHECK(!Gpu_ParseEngineInstance(L"pid_1_phys_0_eng_0_engtype_3d", &g));
    CHECK(!Gpu_ParseEngineInstance(L"pid_x_luid_0x0_0x1_phys_0_eng_0_engtype_3d", &g));
}

static void TestClassifyEngine(void)
{
    CHECK(Gpu_ClassifyEngine(L"3d")              == GPU_ENGINE_3D);
    CHECK(Gpu_ClassifyEngine(L"copy")            == GPU_ENGINE_COPY);
    CHECK(Gpu_ClassifyEngine(L"videodecode")     == GPU_ENGINE_VIDEO_DECODE);
    CHECK(Gpu_ClassifyEngine(L"videoencode")     == GPU_ENGINE_VIDEO_ENCODE);
    CHECK(Gpu_ClassifyEngine(L"videoprocessing") == GPU_ENGINE_VIDEO_PROCESS);
    CHECK(Gpu_ClassifyEngine(L"compute")         == GPU_ENGINE_COMPUTE);
    CHECK(Gpu_ClassifyEngine(L"vr")              == GPU_ENGINE_OTHER);
    CHECK(Gpu_ClassifyEngine(L"")                == GPU_ENGINE_OTHER);
    CHECK(Gpu_ClassifyEngine(NULL)               == GPU_ENGINE_OTHER);
}

/* \GPU Adapter Memory instances name the adapter only -- no pid, no engine.
   A parser that demanded the engine fields would reject every one of them. */
static void TestParseMemoryInstance(void)
{
    ULONGLONG luid = 0;
    unsigned phys = 99;

    CHECK(Gpu_ParseMemoryInstance(L"luid_0x00000000_0x00012cae_phys_0", &luid, &phys));
    CHECK(luid == 0x0000000000012caeULL);
    CHECK(phys == 0);

    CHECK(Gpu_ParseMemoryInstance(L"luid_0x0000abcd_0x00014b13_phys_2", &luid, &phys));
    CHECK(luid == 0x0000abcd00014b13ULL);
    CHECK(phys == 2);

    CHECK(!Gpu_ParseMemoryInstance(L"phys_0", &luid, &phys));
    CHECK(!Gpu_ParseMemoryInstance(NULL, &luid, &phys));
}

/* PDH renders the LUID lowercase, high half first. Round-trip it. */
static void TestFormatLuid(void)
{
    WCHAR buf[64];
    GpuInstance g;

    Gpu_FormatLuid(0x0000000000012caeULL, buf, 64);
    CHECK(lstrcmpW(buf, L"luid_0x00000000_0x00012cae") == 0);

    Gpu_FormatLuid(0x0000abcd00012caeULL, buf, 64);
    CHECK(lstrcmpW(buf, L"luid_0x0000abcd_0x00012cae") == 0);

    {
        WCHAR name[128];
        StringCchPrintfW(name, 128, L"pid_1_%s_phys_0_eng_0_engtype_3d", buf);
        CHECK(Gpu_ParseEngineInstance(name, &g));
        CHECK(g.luid == 0x0000abcd00012caeULL);
    }
}

/* Sum across processes on one engine; max across engines for the adapter.
   The fixture is built so sum and max DIFFER: engine 0 totals 30 (10+20)
   and engine 1 totals 25, so the adapter is 30, never 55. A max/sum swap
   fails here rather than silently shipping >100% readings. */
static void TestAggregateSumThenMax(void)
{
    GpuAccumulator acc;
    GpuAdapterSample adapters[GPU_MAX_ADAPTERS];
    GpuInstance g;
    int n;

    Gpu_AccumReset(&acc);

    g.pid = 100; g.luid = 0xAAA; g.phys = 0; g.engine = 0; g.kind = GPU_ENGINE_3D;
    CHECK(Gpu_AccumAdd(&acc, &g, 10.0));
    g.pid = 200;
    CHECK(Gpu_AccumAdd(&acc, &g, 20.0));           /* same engine, other pid */
    g.pid = 100; g.engine = 1; g.kind = GPU_ENGINE_COPY;
    CHECK(Gpu_AccumAdd(&acc, &g, 25.0));

    n = Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS);
    CHECK(n == 1);
    CHECK(adapters[0].luid == 0xAAA);
    CHECK(adapters[0].utilization > 29.9 && adapters[0].utilization < 30.1);
    CHECK(adapters[0].engine[GPU_ENGINE_3D]   > 29.9);
    CHECK(adapters[0].engine[GPU_ENGINE_COPY] > 24.9 &&
          adapters[0].engine[GPU_ENGINE_COPY] < 25.1);
}

/* phys distinguishes engines on the same adapter, so two engines that share
   an index but differ in phys must not be merged. */
static void TestAggregateSeparatesByPhys(void)
{
    GpuAccumulator acc;
    GpuAdapterSample adapters[GPU_MAX_ADAPTERS];
    GpuInstance g;

    Gpu_AccumReset(&acc);
    g.pid = 1; g.luid = 0xBBB; g.phys = 0; g.engine = 0; g.kind = GPU_ENGINE_3D;
    CHECK(Gpu_AccumAdd(&acc, &g, 40.0));
    g.phys = 1;
    CHECK(Gpu_AccumAdd(&acc, &g, 10.0));

    CHECK(Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS) == 1);
    CHECK(adapters[0].utilization > 39.9 && adapters[0].utilization < 40.1);
}

/* Three adapters were observed on the development machine. */
static void TestAggregateMultipleAdapters(void)
{
    GpuAccumulator acc;
    GpuAdapterSample adapters[GPU_MAX_ADAPTERS];
    GpuInstance g;
    int n, i;
    double total = 0;

    Gpu_AccumReset(&acc);
    g.pid = 1; g.phys = 0; g.engine = 0; g.kind = GPU_ENGINE_3D;
    g.luid = 0x12cae; CHECK(Gpu_AccumAdd(&acc, &g, 5.0));
    g.luid = 0x14b13; CHECK(Gpu_AccumAdd(&acc, &g, 7.0));
    g.luid = 0x14b7d; CHECK(Gpu_AccumAdd(&acc, &g, 9.0));

    n = Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS);
    CHECK(n == 3);
    for (i = 0; i < n; ++i) total += adapters[i].utilization;
    CHECK(total > 20.9 && total < 21.1);
}

/* Per-process sums across every engine AND every adapter, clamped at 100. */
static void TestAggregateProcesses(void)
{
    GpuAccumulator acc;
    GpuProcessSample procs[8];
    GpuInstance g;
    int n, i;
    double got = -1;

    Gpu_AccumReset(&acc);
    g.pid = 42; g.luid = 0xAAA; g.phys = 0; g.engine = 0; g.kind = GPU_ENGINE_3D;
    CHECK(Gpu_AccumAdd(&acc, &g, 30.0));
    g.engine = 1; g.kind = GPU_ENGINE_COPY;
    CHECK(Gpu_AccumAdd(&acc, &g, 20.0));
    g.luid = 0xBBB;                                  /* a second adapter */
    CHECK(Gpu_AccumAdd(&acc, &g, 15.0));

    n = Gpu_AccumProcesses(&acc, procs, 8);
    CHECK(n == 1);
    for (i = 0; i < n; ++i) if (procs[i].pid == 42) got = procs[i].utilization;
    CHECK(got > 64.9 && got < 65.1);                 /* 30 + 20 + 15 */
}

static void TestAggregateClampsProcess(void)
{
    GpuAccumulator acc;
    GpuProcessSample procs[8];
    GpuInstance g;

    Gpu_AccumReset(&acc);
    g.pid = 7; g.luid = 0xAAA; g.phys = 0; g.kind = GPU_ENGINE_3D;
    g.engine = 0; CHECK(Gpu_AccumAdd(&acc, &g, 80.0));
    g.engine = 1; CHECK(Gpu_AccumAdd(&acc, &g, 80.0));

    CHECK(Gpu_AccumProcesses(&acc, procs, 8) == 1);
    CHECK(procs[0].utilization > 99.9 && procs[0].utilization < 100.1);
}

/* An idle adapter still holds video memory, so a memory reading alone must
   produce an adapter entry -- otherwise an idle GPU vanishes from the list. */
static void TestAggregateMemoryOnlyAdapter(void)
{
    GpuAccumulator acc;
    GpuAdapterSample adapters[GPU_MAX_ADAPTERS];

    Gpu_AccumReset(&acc);
    CHECK(Gpu_AccumMemory(&acc, 0xCCC, 2809819136ULL));

    CHECK(Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS) == 1);
    CHECK(adapters[0].luid == 0xCCC);
    CHECK(adapters[0].dedicatedUsed == 2809819136ULL);
    CHECK(adapters[0].utilization == 0.0);
}

/* The counter reports per phys node; take the largest, never the sum. */
static void TestAggregateMemoryTakesMaxNode(void)
{
    GpuAccumulator acc;
    GpuAdapterSample adapters[GPU_MAX_ADAPTERS];

    Gpu_AccumReset(&acc);
    CHECK(Gpu_AccumMemory(&acc, 0xDDD, 1000));
    CHECK(Gpu_AccumMemory(&acc, 0xDDD, 4000));
    CHECK(Gpu_AccumMemory(&acc, 0xDDD, 2000));

    CHECK(Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS) == 1);
    CHECK(adapters[0].dedicatedUsed == 4000);
}

/* Reset must actually clear, or a second sample accumulates onto the first
   and every reading climbs forever. */
static void TestAggregateResetClears(void)
{
    GpuAccumulator acc;
    GpuAdapterSample adapters[GPU_MAX_ADAPTERS];
    GpuInstance g;

    Gpu_AccumReset(&acc);
    g.pid = 1; g.luid = 0xAAA; g.phys = 0; g.engine = 0; g.kind = GPU_ENGINE_3D;
    Gpu_AccumAdd(&acc, &g, 50.0);
    Gpu_AccumMemory(&acc, 0xAAA, 999);
    Gpu_AccumReset(&acc);
    CHECK(Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS) == 0);

    Gpu_AccumAdd(&acc, &g, 10.0);
    CHECK(Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS) == 1);
    CHECK(adapters[0].utilization > 9.9 && adapters[0].utilization < 10.1);
}

int main(void)
{
    CHECK(GPU_ENGINE_KINDS == 7);
    TestParseInstance();
    TestParseLuidHighHalf();
    TestParseEmptyAndUnknownEngtype();
    TestParseEngineIndexNotEngtype();
    TestParseRejectsGarbage();
    TestClassifyEngine();
    TestParseMemoryInstance();
    TestFormatLuid();
    TestAggregateSumThenMax();
    TestAggregateSeparatesByPhys();
    TestAggregateMultipleAdapters();
    TestAggregateProcesses();
    TestAggregateClampsProcess();
    TestAggregateMemoryOnlyAdapter();
    TestAggregateMemoryTakesMaxNode();
    TestAggregateResetClears();
    printf("gpu: %d failures\n", failures);
    return failures ? 1 : 0;
}
