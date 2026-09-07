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
    printf("gpu: %d failures\n", failures);
    return failures ? 1 : 0;
}
