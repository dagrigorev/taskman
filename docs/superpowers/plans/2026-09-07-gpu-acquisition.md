# GPU Acquisition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a self-contained GPU acquisition module that reports per-adapter utilisation, per-engine-type breakdown, video memory and per-process GPU usage, sampled from PDH.

**Architecture:** `src/gpu.c` is a pure acquisition module with no Win32 UI calls, testable headlessly the way `src/tabs/proc_tree.c` already is. It owns one persistent PDH query, parses `\GPU Engine(*)` instance names into structured samples, aggregates them, and publishes a model under its own SRWLOCK following the existing per-tab model pattern. The collector thread calls it before the per-tab collectors, and only when something needs the data.

**Tech Stack:** C11, Win32, PDH (`pdh.dll`), DXGI (`dxgi.dll`), CMake + Ninja + CTest, MSVC and MinGW-w64.

**Spec:** `docs/superpowers/specs/2026-09-07-gpu-and-temperatures-design.md`

This plan implements **unit 1 of 4** from that spec. Units 2-4 (the Sensors tab, temperature acquisition, the per-process GPU column) are separate plans. Nothing in this plan renders anything: it ends with a tested module that the collector calls and whose model nothing displays yet.

## Global Constraints

- Warnings are errors on BOTH toolchains. MinGW set, copied from `CMakeLists.txt`:
  `-Wall -Wextra -Wshadow -Wcast-qual -Wpointer-arith -Wstrict-prototypes
  -Wmissing-prototypes -Wwrite-strings -Wconversion -Wsign-conversion
  -Wredundant-decls -Wundef`. MSVC: `/W4 /WX`.
- No kernel driver, no vendor SDK. `pdh.dll` and `dxgi.dll` both ship with Windows.
- `src/gpu.c` must call NO Win32 UI function. It may use `windows.h` types, PDH, DXGI and the CRT.
- Counters MUST be added with `PdhAddEnglishCounterW`, never `PdhAddCounterW`. The plain variant takes *localised* names and finds nothing on a non-English Windows — the development machine is Russian-localised.
- The collector thread must never read `g_cfg`. Cross-thread state is published with `InterlockedExchange`, as `SysInfo_SetActiveTab` already does.
- Everything runs unelevated.
- This IS a git repository, on `master`, clean. Branch before the first commit:
  `git checkout -b gpu-acquisition`.

### Metric definitions (from the spec — do not reinterpret)

An instance name has the shape
`pid_<pid>_luid_0x<high>_0x<low>_phys_<n>_eng_<n>_engtype_<type>`.

- An **engine** is the tuple `(luid, phys, eng, kind)`.
- **Engine utilisation** = the **sum** over every pid's instance of that engine.
- **Adapter utilisation** = the **maximum** across that adapter's engines.
  Summing engines would let an adapter read over 100 %.
- **Per-adapter, per-kind** = the maximum across the engines of that kind.
- **Per-process** = the sum of that pid's instances across all engines and
  adapters, clamped to 100.

### Build and test commands

```bash
./build.cmd test -Strict
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test -Strict -Toolchain mingw
```

Both must be run: `build.cmd` alone only exercises the auto-detected toolchain.
A single suite can be run directly from `build/msvc-release/bin/Release/`.
The `test_workspace` suite creates real windows and fails intermittently for
environmental reasons — re-run before concluding anything is broken.

---

### Task 1: Measure the native PDH cost

The spec makes this a gate: the 1-second cadence is only affordable if a native
collection is cheap at ~1029 instances. The PowerShell probe took 4.8 s, but
almost all of that is `Get-Counter`'s own sampling interval and marshalling, so
it is not evidence about the native path. This task replaces a guess with a
number, and that number picks a constant every later task depends on.

**Files:**
- Create: `docs/superpowers/plans/gpu-cost-measurement.md` (findings; throwaway harness is NOT kept)

**Interfaces:**
- Consumes: nothing
- Produces: the measured per-collection cost, and the value of
  `GPU_COLLECT_INTERVAL_MS` used from Task 7 onward

- [ ] **Step 1: Write a throwaway timing harness**

Write it to your system temp directory, NOT into the repository. It is deleted
in Step 4.

```c
/* THROWAWAY - measures native PDH cost at real instance count. Not kept. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <stdio.h>

int main(void)
{
    PDH_HQUERY query = NULL;
    PDH_HCOUNTER counter = NULL;
    LARGE_INTEGER freq, a, b;
    int pass;

    QueryPerformanceFrequency(&freq);
    if (PdhOpenQueryW(NULL, 0, &query) != ERROR_SUCCESS) { printf("open failed\n"); return 1; }
    /* English name: the localised variant finds nothing on this machine. */
    if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage",
                              0, &counter) != ERROR_SUCCESS) {
        printf("add failed\n"); return 1;
    }
    PdhCollectQueryData(query);          /* rate counters need two samples */
    Sleep(1000);

    for (pass = 0; pass < 5; ++pass) {
        DWORD size = 0, count = 0;
        PDH_FMT_COUNTERVALUE_ITEM_W *items;
        PDH_STATUS st;
        double collectMs, formatMs;

        QueryPerformanceCounter(&a);
        PdhCollectQueryData(query);
        QueryPerformanceCounter(&b);
        collectMs = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;

        QueryPerformanceCounter(&a);
        st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, NULL);
        items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(size ? size : 1);
        if (items) st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &size, &count, items);
        QueryPerformanceCounter(&b);
        formatMs = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;

        printf("pass %d: instances=%lu collect=%.2f ms format=%.2f ms total=%.2f ms (status 0x%lx)\n",
               pass, (unsigned long)count, collectMs, formatMs, collectMs + formatMs,
               (unsigned long)st);
        free(items);
        Sleep(1000);
    }
    PdhCloseQuery(query);
    return 0;
}
```

- [ ] **Step 2: Build and run it**

```bash
gcc -std=gnu11 -O2 -Wall "$TMP/gpu_cost.c" -o "$TMP/gpu_cost.exe" -lpdh
"$TMP/gpu_cost.exe"
```

Expected: five lines reporting an instance count near 1000 and a per-pass total.
If the instance count is 0, the English-counter-name assumption is wrong and
that is a finding in itself — record it and stop rather than proceeding.

- [ ] **Step 3: Record the finding and pick the interval**

Write `docs/superpowers/plans/gpu-cost-measurement.md` containing the raw output
and the decision:

- total ≤ 10 ms per collection → `GPU_COLLECT_INTERVAL_MS = 1000`
- 10 ms < total ≤ 40 ms → `GPU_COLLECT_INTERVAL_MS = 2000`
- total > 40 ms → `GPU_COLLECT_INTERVAL_MS = 5000`, and note in the document
  that the Sensors tab graph will be coarse

State the chosen value explicitly; Task 7 copies it verbatim.

- [ ] **Step 4: Delete the harness**

```bash
rm -f "$TMP/gpu_cost.c" "$TMP/gpu_cost.exe"
```

- [ ] **Step 5: Commit**

```bash
git checkout -b gpu-acquisition
git add docs/superpowers/plans/gpu-cost-measurement.md
git commit -m "docs: measure native PDH GPU counter cost"
```

---

### Task 2: Module and test scaffolding

**Files:**
- Create: `include/gpu.h`, `src/gpu.c`, `tests/test_gpu.c`
- Modify: `CMakeLists.txt`, `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing
- Produces: `GpuEngineKind`, `GPU_ENGINE_KINDS`, `GpuInstance`, and the
  `test_gpu` suite that Tasks 3-4 extend

- [ ] **Step 1: Create the header**

```c
/* ------------------------------------------------------------------------
 * gpu.h - GPU utilisation and video memory acquisition.
 *
 * Pure acquisition: no Win32 UI calls, so the parsing and aggregation can
 * be unit tested with no window and no message pump.
 * ------------------------------------------------------------------------ */
#ifndef CTM_GPU_H
#define CTM_GPU_H

#include <windows.h>

#define GPU_NAME_MAX      128
#define GPU_MAX_ADAPTERS  8

/* Engine kinds worth surfacing. The probe observed eleven distinct engtype
   strings, including an empty one and vendor-specific names (ofa_0, vr,
   security_1, legacyoverlay). Everything unrecognised folds into
   GPU_ENGINE_OTHER rather than being dropped, so an adapter's maximum is
   never understated by an engine we failed to name. */
typedef enum {
    GPU_ENGINE_3D = 0,
    GPU_ENGINE_COPY,
    GPU_ENGINE_VIDEO_DECODE,
    GPU_ENGINE_VIDEO_ENCODE,
    GPU_ENGINE_VIDEO_PROCESS,
    GPU_ENGINE_COMPUTE,
    GPU_ENGINE_OTHER,
    GPU_ENGINE_KINDS
} GpuEngineKind;

/* One parsed \GPU Engine(...) instance name. */
typedef struct {
    DWORD         pid;
    ULONGLONG     luid;      /* HighPart << 32 | LowPart */
    unsigned      phys;
    unsigned      engine;
    GpuEngineKind kind;
} GpuInstance;

#endif /* CTM_GPU_H */
```

- [ ] **Step 2: Create the empty module**

```c
/* ------------------------------------------------------------------------
 * gpu.c - GPU utilisation and video memory acquisition.
 * Pure acquisition. No Win32 UI calls: see gpu.h.
 * ------------------------------------------------------------------------ */
#include "gpu.h"
#include <shlwapi.h>
#include <stdlib.h>
#include <wchar.h>
```

- [ ] **Step 3: Create the suite**

```c
/* Exercises GPU instance parsing and aggregation with synthetic data.
   No UI stubs needed: gpu.c calls nothing from user32 or comctl32. */
#include "../include/gpu.h"
#include <stdio.h>
#include "../src/gpu.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void)
{
    CHECK(GPU_ENGINE_KINDS == 7);
    printf("gpu: %d failures\n", failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 4: Wire into CMake**

In `CMakeLists.txt`, add `pdh` and `dxgi` to the platform link libraries:

```cmake
target_link_libraries(taskman_platform INTERFACE
    comctl32 comdlg32 ole32 shlwapi shell32 psapi uxtheme dwmapi
    iphlpapi wtsapi32 version advapi32 gdi32 user32 kernel32 pdh dxgi)
```

and add the source:

```cmake
set(TASKMAN_SUPPORT_SOURCES src/ui.c src/settings.c src/sysinfo.c src/gpu.c)
```

and the header to the `add_executable` list:

```cmake
    include/app.h include/ui.h include/ntapi.h include/gdiplusapi.h
    include/proc_tree.h include/gpu.h include/resource.h)
```

In `tests/CMakeLists.txt`, after `taskman_add_test(test_proctree)`:

```cmake
taskman_add_test(test_gpu)
```

`test_gpu` gets no extra sources: it `#include`s `gpu.c` directly, so linking
the same translation unit again would duplicate every symbol.

Note that `TASKMAN_SUPPORT_SOURCES` is also expanded into `test_workspace`'s
source list, so `gpu.c` joins that suite automatically.

- [ ] **Step 5: Verify**

```bash
./build.cmd test -Strict
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test -Strict -Toolchain mingw
```

Expected: both toolchains clean, eleven suites pass, `gpu: 0 failures`.

- [ ] **Step 6: Commit**

```bash
git add include/gpu.h src/gpu.c tests/test_gpu.c CMakeLists.txt tests/CMakeLists.txt
git commit -m "test: add gpu module scaffolding"
```

---

### Task 3: Instance-name parsing

**Files:**
- Modify: `src/gpu.c`, `include/gpu.h`
- Test: `tests/test_gpu.c`

**Interfaces:**
- Consumes: `GpuInstance`, `GpuEngineKind` from Task 2
- Produces:
  - `GpuEngineKind Gpu_ClassifyEngine(const WCHAR *engtype);`
  - `BOOL Gpu_ParseEngineInstance(const WCHAR *name, GpuInstance *out);`
  - `void Gpu_FormatLuid(ULONGLONG luid, WCHAR *buf, size_t cch);`
  - `BOOL Gpu_ParseMemoryInstance(const WCHAR *name, ULONGLONG *luid, unsigned *phys);`

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_gpu.c` and call each from `main` before the summary printf.
A test defined but never called passes silently and proves nothing.

```c
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
    CHECK(!Gpu_ParseEngineInstance(L"luid_0x0_0x1_phys_0_eng_0_engtype_3d", &g)); /* no pid */
    CHECK(!Gpu_ParseEngineInstance(L"pid_1_phys_0_eng_0_engtype_3d", &g));        /* no luid */
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

    /* And what we format must be what we parse. */
    {
        WCHAR name[128];
        StringCchPrintfW(name, 128, L"pid_1_%s_phys_0_eng_0_engtype_3d", buf);
        CHECK(Gpu_ParseEngineInstance(name, &g));
        CHECK(g.luid == 0x0000abcd00012caeULL);
    }
}
```

`StringCchPrintfW` needs `#include <strsafe.h>` at the top of the test file if
it is not already reachable; `gpu.h` pulls in `windows.h` only.

- [ ] **Step 2: Run to verify it fails**

```bash
./build.cmd test -Strict
```

Expected: FAIL to compile with `implicit declaration of function 'Gpu_ParseEngineInstance'`.

- [ ] **Step 3: Declare in `include/gpu.h`**

Before the `#endif`:

```c
/* Maps a PDH engtype string to a kind. Unrecognised, empty and NULL all
   classify as GPU_ENGINE_OTHER. */
GpuEngineKind Gpu_ClassifyEngine(const WCHAR *engtype);

/* Parses a \GPU Engine(...) instance name of the form
   pid_<pid>_luid_0x<high>_0x<low>_phys_<n>_eng_<n>_engtype_<type>.
   A missing or empty engtype still parses, classified as OTHER: dropping
   the instance would understate its adapter's maximum. Returns FALSE only
   when a required numeric field is absent or unparseable. */
BOOL Gpu_ParseEngineInstance(const WCHAR *name, GpuInstance *out);

/* Renders a LUID the way PDH does: lowercase, high half first. */
void Gpu_FormatLuid(ULONGLONG luid, WCHAR *buf, size_t cch);

/* Parses a \GPU Adapter Memory(...) instance name, which identifies only
   the adapter: luid_0x<high>_0x<low>_phys_<n>. No pid, no engine fields. */
BOOL Gpu_ParseMemoryInstance(const WCHAR *name, ULONGLONG *luid, unsigned *phys);
```

- [ ] **Step 4: Implement in `src/gpu.c`**

```c
/* Returns the text just past `marker`, or NULL when it does not occur.
   Note "_eng_" cannot match inside "_engtype_", which reads "_engt". */
static const WCHAR *GpuAfter(const WCHAR *text, const WCHAR *marker)
{
    const WCHAR *at = text ? StrStrIW(text, marker) : NULL;
    return at ? at + lstrlenW(marker) : NULL;
}

GpuEngineKind Gpu_ClassifyEngine(const WCHAR *engtype)
{
    if (!engtype || !engtype[0])                        return GPU_ENGINE_OTHER;
    if (!lstrcmpiW(engtype, L"3d"))                     return GPU_ENGINE_3D;
    if (!lstrcmpiW(engtype, L"copy"))                   return GPU_ENGINE_COPY;
    if (!lstrcmpiW(engtype, L"videodecode"))            return GPU_ENGINE_VIDEO_DECODE;
    if (!lstrcmpiW(engtype, L"videoencode"))            return GPU_ENGINE_VIDEO_ENCODE;
    if (!lstrcmpiW(engtype, L"videoprocessing"))        return GPU_ENGINE_VIDEO_PROCESS;
    if (!lstrcmpiW(engtype, L"compute"))                return GPU_ENGINE_COMPUTE;
    return GPU_ENGINE_OTHER;
}

BOOL Gpu_ParseEngineInstance(const WCHAR *name, GpuInstance *out)
{
    const WCHAR *p;
    WCHAR *end;
    ULONGLONG high, low;

    if (!name || !out) return FALSE;
    ZeroMemory(out, sizeof(*out));

    p = GpuAfter(name, L"pid_");
    if (!p) return FALSE;
    out->pid = (DWORD)wcstoul(p, &end, 10);
    if (end == p) return FALSE;

    p = GpuAfter(name, L"luid_0x");
    if (!p) return FALSE;
    high = wcstoull(p, &end, 16);
    if (end == p) return FALSE;
    p = GpuAfter(end, L"_0x");        /* the low half follows immediately */
    if (!p) return FALSE;
    low = wcstoull(p, &end, 16);
    if (end == p) return FALSE;
    out->luid = (high << 32) | (low & 0xFFFFFFFFULL);

    p = GpuAfter(name, L"_phys_");
    if (!p) return FALSE;
    out->phys = (unsigned)wcstoul(p, &end, 10);
    if (end == p) return FALSE;

    p = GpuAfter(name, L"_eng_");
    if (!p) return FALSE;
    out->engine = (unsigned)wcstoul(p, &end, 10);
    if (end == p) return FALSE;

    /* Absent or empty engtype is legitimate; classify and keep the row. */
    out->kind = Gpu_ClassifyEngine(GpuAfter(name, L"_engtype_"));
    return TRUE;
}

BOOL Gpu_ParseMemoryInstance(const WCHAR *name, ULONGLONG *luid, unsigned *phys)
{
    const WCHAR *p;
    WCHAR *end;
    ULONGLONG high, low;

    if (!name || !luid || !phys) return FALSE;

    p = GpuAfter(name, L"luid_0x");
    if (!p) return FALSE;
    high = wcstoull(p, &end, 16);
    if (end == p) return FALSE;
    p = GpuAfter(end, L"_0x");
    if (!p) return FALSE;
    low = wcstoull(p, &end, 16);
    if (end == p) return FALSE;
    *luid = (high << 32) | (low & 0xFFFFFFFFULL);

    p = GpuAfter(name, L"_phys_");
    if (!p) return FALSE;
    *phys = (unsigned)wcstoul(p, &end, 10);
    return end != p;
}

void Gpu_FormatLuid(ULONGLONG luid, WCHAR *buf, size_t cch)
{
    if (!buf || cch == 0) return;
    StringCchPrintfW(buf, cch, L"luid_0x%08lx_0x%08lx",
                     (unsigned long)(luid >> 32),
                     (unsigned long)(luid & 0xFFFFFFFFULL));
}
```

`src/gpu.c` needs `#include <strsafe.h>` added to its include block for
`StringCchPrintfW`.

- [ ] **Step 5: Run to verify it passes**

```bash
./build.cmd test -Strict
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test -Strict -Toolchain mingw
```

Expected: `gpu: 0 failures`, both toolchains clean.

- [ ] **Step 6: Commit**

```bash
git add include/gpu.h src/gpu.c tests/test_gpu.c
git commit -m "feat: parse GPU engine counter instance names"
```

---

### Task 4: Aggregation

Turns a stream of parsed instances into per-adapter and per-process figures.
This is where the sum-versus-max distinction lives, and getting it backwards
produces numbers that look plausible and are wrong.

**Files:**
- Modify: `src/gpu.c`, `include/gpu.h`
- Test: `tests/test_gpu.c`

**Interfaces:**
- Consumes: `Gpu_ParseEngineInstance`, `GpuInstance`
- Produces:
  - `GpuAdapterSample` (luid, utilization, engine[GPU_ENGINE_KINDS])
  - `GpuProcessSample` (pid, utilization)
  - `GpuAccumulator` (opaque, caller-allocated)
  - `void Gpu_AccumReset(GpuAccumulator *acc);`
  - `BOOL Gpu_AccumAdd(GpuAccumulator *acc, const GpuInstance *inst, double value);`
  - `int Gpu_AccumAdapters(const GpuAccumulator *acc, GpuAdapterSample *out, int max);`
  - `int Gpu_AccumProcesses(const GpuAccumulator *acc, GpuProcessSample *out, int max);`
  - `BOOL Gpu_AccumMemory(GpuAccumulator *acc, ULONGLONG luid, ULONGLONG dedicatedUsed);`

- [ ] **Step 1: Write the failing tests**

```c
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
```

- [ ] **Step 2: Run to verify it fails**

```bash
./build.cmd test -Strict
```

Expected: FAIL to compile, `unknown type name 'GpuAccumulator'`.

- [ ] **Step 3: Declare in `include/gpu.h`**

```c
#define GPU_MAX_ENGINES   256   /* 3 adapters x 11 engine types, with room  */
#define GPU_MAX_PROCESSES 512

typedef struct {
    ULONGLONG luid;
    double    utilization;                  /* max across this adapter's engines */
    double    engine[GPU_ENGINE_KINDS];     /* max across engines of that kind   */
    ULONGLONG dedicatedUsed;                /* from \GPU Adapter Memory          */
} GpuAdapterSample;

typedef struct {
    DWORD  pid;
    double utilization;                     /* summed, clamped to 100            */
} GpuProcessSample;

/* Caller-allocated; large enough to live on the collector's stack is NOT
   assumed -- callers place it in static or heap storage. */
typedef struct {
    struct {
        ULONGLONG     luid;
        unsigned      phys, engine;
        GpuEngineKind kind;
        double        sum;
    } engines[GPU_MAX_ENGINES];
    int engineCount;
    struct { DWORD pid; double sum; } processes[GPU_MAX_PROCESSES];
    int processCount;
    /* Dedicated video memory in use, keyed by adapter. Separate from the
       engine buckets because its counter carries no pid and no engine. */
    struct { ULONGLONG luid; ULONGLONG dedicatedUsed; } memory[GPU_MAX_ADAPTERS];
    int memoryCount;
} GpuAccumulator;

void Gpu_AccumReset(GpuAccumulator *acc);
/* Adds one instance's value. Returns FALSE only when a bucket table is full,
   in which case that instance is dropped rather than misattributed. */
BOOL Gpu_AccumAdd(GpuAccumulator *acc, const GpuInstance *inst, double value);
/* Engine sums folded to one entry per adapter. Returns adapters written. */
int Gpu_AccumAdapters(const GpuAccumulator *acc, GpuAdapterSample *out, int max);
/* One entry per pid, summed across engines and adapters, clamped to 100. */
int Gpu_AccumProcesses(const GpuAccumulator *acc, GpuProcessSample *out, int max);
/* Records one adapter's dedicated video memory in use. Adapters with a
   memory reading but no active engine still appear in Gpu_AccumAdapters. */
BOOL Gpu_AccumMemory(GpuAccumulator *acc, ULONGLONG luid, ULONGLONG dedicatedUsed);
```

- [ ] **Step 4: Implement in `src/gpu.c`**

```c
void Gpu_AccumReset(GpuAccumulator *acc)
{
    if (!acc) return;
    acc->engineCount = 0;
    acc->processCount = 0;
    acc->memoryCount = 0;
}

BOOL Gpu_AccumMemory(GpuAccumulator *acc, ULONGLONG luid, ULONGLONG dedicatedUsed)
{
    int i;
    if (!acc) return FALSE;
    for (i = 0; i < acc->memoryCount; ++i)
        if (acc->memory[i].luid == luid) break;
    if (i == acc->memoryCount) {
        if (acc->memoryCount >= GPU_MAX_ADAPTERS) return FALSE;
        acc->memory[i].luid = luid;
        acc->memory[i].dedicatedUsed = 0;
        acc->memoryCount++;
    }
    /* The counter is reported per phys node; the adapter's usage is the
       largest node reading, not their sum, which would double-count. */
    if (dedicatedUsed > acc->memory[i].dedicatedUsed)
        acc->memory[i].dedicatedUsed = dedicatedUsed;
    return TRUE;
}

BOOL Gpu_AccumAdd(GpuAccumulator *acc, const GpuInstance *inst, double value)
{
    int i;
    BOOL ok = TRUE;

    if (!acc || !inst) return FALSE;
    if (!(value >= 0.0)) value = 0.0;      /* also rejects NaN */

    /* Engine bucket: an engine is (luid, phys, eng, kind), summed over pids. */
    for (i = 0; i < acc->engineCount; ++i) {
        if (acc->engines[i].luid == inst->luid &&
            acc->engines[i].phys == inst->phys &&
            acc->engines[i].engine == inst->engine &&
            acc->engines[i].kind == inst->kind) break;
    }
    if (i == acc->engineCount) {
        if (acc->engineCount >= GPU_MAX_ENGINES) ok = FALSE;
        else {
            acc->engines[i].luid   = inst->luid;
            acc->engines[i].phys   = inst->phys;
            acc->engines[i].engine = inst->engine;
            acc->engines[i].kind   = inst->kind;
            acc->engines[i].sum    = 0.0;
            acc->engineCount++;
        }
    }
    if (i < acc->engineCount) acc->engines[i].sum += value;

    /* Process bucket: summed across every engine and every adapter. */
    for (i = 0; i < acc->processCount; ++i)
        if (acc->processes[i].pid == inst->pid) break;
    if (i == acc->processCount) {
        if (acc->processCount >= GPU_MAX_PROCESSES) return FALSE;
        acc->processes[i].pid = inst->pid;
        acc->processes[i].sum = 0.0;
        acc->processCount++;
    }
    acc->processes[i].sum += value;
    return ok;
}

int Gpu_AccumAdapters(const GpuAccumulator *acc, GpuAdapterSample *out, int max)
{
    int count = 0, i, at, j;

    if (!acc || !out || max <= 0) return 0;
    for (i = 0; i < acc->engineCount; ++i) {
        int kind = (int)acc->engines[i].kind;
        at = -1;
        for (j = 0; j < count; ++j)
            if (out[j].luid == acc->engines[i].luid) { at = j; break; }
        if (at < 0) {
            if (count >= max) continue;    /* drop, never misattribute */
            at = count++;
            ZeroMemory(&out[at], sizeof(out[at]));
            out[at].luid = acc->engines[i].luid;
        }
        /* Adapter and per-kind figures are BOTH maxima over engines; the
           sum over pids already happened when the bucket was filled. */
        if (acc->engines[i].sum > out[at].utilization)
            out[at].utilization = acc->engines[i].sum;
        if (kind >= 0 && kind < GPU_ENGINE_KINDS &&
            acc->engines[i].sum > out[at].engine[kind])
            out[at].engine[kind] = acc->engines[i].sum;
    }

    /* An adapter can have memory in use with no engine busy -- an idle GPU
       still holds textures. Add those, then attach every memory reading. */
    for (i = 0; i < acc->memoryCount; ++i) {
        at = -1;
        for (j = 0; j < count; ++j)
            if (out[j].luid == acc->memory[i].luid) { at = j; break; }
        if (at < 0) {
            if (count >= max) continue;
            at = count++;
            ZeroMemory(&out[at], sizeof(out[at]));
            out[at].luid = acc->memory[i].luid;
        }
        out[at].dedicatedUsed = acc->memory[i].dedicatedUsed;
    }
    return count;
}

int Gpu_AccumProcesses(const GpuAccumulator *acc, GpuProcessSample *out, int max)
{
    int count = 0, i;

    if (!acc || !out || max <= 0) return 0;
    for (i = 0; i < acc->processCount && count < max; ++i) {
        double value = acc->processes[i].sum;
        if (value > 100.0) value = 100.0;
        out[count].pid = acc->processes[i].pid;
        out[count].utilization = value;
        ++count;
    }
    return count;
}
```

- [ ] **Step 5: Run to verify it passes**

```bash
./build.cmd test -Strict
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test -Strict -Toolchain mingw
```

Expected: `gpu: 0 failures`, both toolchains clean.

- [ ] **Step 6: Commit**

```bash
git add include/gpu.h src/gpu.c tests/test_gpu.c
git commit -m "feat: aggregate GPU engine samples per adapter and per process"
```

---

### Task 5: PDH query lifecycle

**Files:**
- Modify: `src/gpu.c`, `include/gpu.h`
- Create: `tests/test_gpu_live.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Gpu_ParseEngineInstance`, the `Gpu_Accum*` family
- Produces:
  - `BOOL Gpu_QueryOpen(void);`
  - `void Gpu_QueryClose(void);`
  - `BOOL Gpu_QuerySample(GpuAccumulator *acc, ULONGLONG nowTick);`
    returns FALSE when no usable data is available yet

- [ ] **Step 1: Write the failing live test**

`tests/test_gpu_live.c`, modelled on `tests/test_sysinfo_live.c`. It touches
the real machine, so it asserts shapes rather than exact values, and it skips
cleanly on a machine with no GPU counters.

```c
/* Opens a REAL PDH query. This is the only place the English-counter-name
   and two-collection requirements can actually be caught: both fail by
   silently returning nothing, which a synthetic test cannot reproduce. */
#include "../include/gpu.h"
#include <stdio.h>
#include "../src/gpu.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void)
{
    GpuAccumulator acc;
    GpuAdapterSample adapters[GPU_MAX_ADAPTERS];
    int n;

    if (!Gpu_QueryOpen()) {
        printf("SKIP gpu_live: no GPU performance counters on this machine\n");
        return 0;
    }

    /* A rate counter has no value from a single collection. The first
       sample must report "not ready" rather than a confident zero. */
    Gpu_AccumReset(&acc);
    CHECK(Gpu_QuerySample(&acc, GetTickCount64()) == FALSE);

    Sleep(1100);
    Gpu_AccumReset(&acc);
    if (!Gpu_QuerySample(&acc, GetTickCount64())) {
        printf("SKIP gpu_live: second collection still unavailable\n");
        Gpu_QueryClose();
        return 0;
    }

    /* The machine this was written on reported ~1000 instances across three
       adapters. Any GPU at all should yield at least one engine bucket. */
    CHECK(acc.engineCount > 0);
    n = Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS);
    CHECK(n > 0);
    if (n > 0) {
        int i;
        for (i = 0; i < n; ++i) {
            CHECK(adapters[i].utilization >= 0.0);
            CHECK(adapters[i].utilization <= 100.0);
        }
    }

    Gpu_QueryClose();
    printf("gpu_live: %d failures (%d engines, %d adapters)\n",
           failures, acc.engineCount, n);
    return failures ? 1 : 0;
}
```

Register it in `tests/CMakeLists.txt` next to the other suites:

```cmake
taskman_add_test(test_gpu_live)
```

- [ ] **Step 2: Run to verify it fails**

```bash
./build.cmd test -Strict
```

Expected: FAIL to compile, `implicit declaration of function 'Gpu_QueryOpen'`.

- [ ] **Step 3: Declare in `include/gpu.h`**

```c
/* Opens the PDH query and adds the wildcard counters. Returns FALSE when
   GPU counters are unavailable (pre-1709, or the provider is disabled), in
   which case every other query function is a safe no-op. */
BOOL Gpu_QueryOpen(void);
void Gpu_QueryClose(void);

/* Collects one sample into `acc`, which the caller has already reset.
   Returns FALSE when no usable value exists yet -- notably the first call
   after opening, because Utilization Percentage is a rate counter and needs
   two collections. `nowTick` is GetTickCount64(), used to decide when the
   wildcard counters are rebuilt. */
BOOL Gpu_QuerySample(GpuAccumulator *acc, ULONGLONG nowTick);
```

- [ ] **Step 4: Implement in `src/gpu.c`**

Add `#include <pdh.h>` to the include block.

```c
/* Wildcard counters do NOT pick up instances created after the counter was
   added, so a process that starts later never appears. Re-adding the
   counter on this interval bounds how long a newly launched application
   stays invisible, without paying re-expansion on every sample. */
#define GPU_REBUILD_INTERVAL_MS 10000

static PDH_HQUERY   s_query;
static PDH_HCOUNTER s_engine;
static PDH_HCOUNTER s_memory;
static BOOL         s_primed;      /* a first collection has happened     */
static ULONGLONG    s_builtTick;

static BOOL GpuAddCounters(void)
{
    /* English names, NOT PdhAddCounterW: the plain variant takes localised
       counter names and finds nothing at all on a non-English Windows. */
    if (PdhAddEnglishCounterW(s_query, L"\\GPU Engine(*)\\Utilization Percentage",
                              0, &s_engine) != ERROR_SUCCESS)
        return FALSE;
    /* Memory is optional: an adapter list with utilisation but no memory is
       still useful, so a missing counter must not fail the whole query. */
    if (PdhAddEnglishCounterW(s_query, L"\\GPU Adapter Memory(*)\\Dedicated Usage",
                              0, &s_memory) != ERROR_SUCCESS)
        s_memory = NULL;
    return TRUE;
}

BOOL Gpu_QueryOpen(void)
{
    if (s_query) return TRUE;
    if (PdhOpenQueryW(NULL, 0, &s_query) != ERROR_SUCCESS) { s_query = NULL; return FALSE; }
    if (!GpuAddCounters()) { PdhCloseQuery(s_query); s_query = NULL; return FALSE; }
    s_primed = FALSE;
    s_builtTick = GetTickCount64();
    return TRUE;
}

void Gpu_QueryClose(void)
{
    if (s_query) PdhCloseQuery(s_query);   /* also releases its counters */
    s_query = NULL;
    s_engine = NULL;
    s_memory = NULL;
    s_primed = FALSE;
    s_builtTick = 0;
}

BOOL Gpu_QuerySample(GpuAccumulator *acc, ULONGLONG nowTick)
{
    DWORD size = 0, count = 0;
    PDH_FMT_COUNTERVALUE_ITEM_W *items;
    PDH_STATUS status;
    DWORD i;

    if (!s_query || !acc) return FALSE;

    /* Pick up adapters and processes that appeared since the last rebuild. */
    if (s_primed && nowTick - s_builtTick >= GPU_REBUILD_INTERVAL_MS) {
        PdhRemoveCounter(s_engine);
        s_engine = NULL;
        if (s_memory) { PdhRemoveCounter(s_memory); s_memory = NULL; }
        if (!GpuAddCounters()) { Gpu_QueryClose(); return FALSE; }
        s_builtTick = nowTick;
        s_primed = FALSE;         /* the fresh counter needs priming again */
    }

    if (PdhCollectQueryData(s_query) != ERROR_SUCCESS) return FALSE;
    if (!s_primed) { s_primed = TRUE; return FALSE; }   /* rate counter */

    status = PdhGetFormattedCounterArrayW(s_engine, PDH_FMT_DOUBLE, &size, &count, NULL);
    if (status != PDH_MORE_DATA || size == 0) return FALSE;
    items = (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(size);
    if (!items) return FALSE;
    status = PdhGetFormattedCounterArrayW(s_engine, PDH_FMT_DOUBLE, &size, &count, items);
    if (status != ERROR_SUCCESS) { free(items); return FALSE; }

    for (i = 0; i < count; ++i) {
        GpuInstance instance;
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
            items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        if (!Gpu_ParseEngineInstance(items[i].szName, &instance)) continue;
        Gpu_AccumAdd(acc, &instance, items[i].FmtValue.doubleValue);
    }
    free(items);

    /* Dedicated video memory, keyed by adapter. Its counter carries no pid
       and no engine, so it needs its own instance parser. Absence is
       tolerated: utilisation without memory is still a usable reading. */
    if (s_memory) {
        size = 0; count = 0;
        status = PdhGetFormattedCounterArrayW(s_memory, PDH_FMT_LARGE, &size, &count, NULL);
        if (status == PDH_MORE_DATA && size > 0) {
            PDH_FMT_COUNTERVALUE_ITEM_W *memoryItems =
                (PDH_FMT_COUNTERVALUE_ITEM_W *)malloc(size);
            if (memoryItems) {
                if (PdhGetFormattedCounterArrayW(s_memory, PDH_FMT_LARGE, &size,
                                                 &count, memoryItems) == ERROR_SUCCESS) {
                    for (i = 0; i < count; ++i) {
                        ULONGLONG luid = 0;
                        unsigned phys = 0;
                        if (memoryItems[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                            memoryItems[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                        if (!Gpu_ParseMemoryInstance(memoryItems[i].szName, &luid, &phys)) continue;
                        if (memoryItems[i].FmtValue.largeValue < 0) continue;
                        Gpu_AccumMemory(acc, luid,
                                        (ULONGLONG)memoryItems[i].FmtValue.largeValue);
                    }
                }
                free(memoryItems);
            }
        }
    }
    return TRUE;
}
```

- [ ] **Step 5: Run to verify it passes**

```bash
./build.cmd test -Strict
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test -Strict -Toolchain mingw
```

Expected: twelve suites pass. `gpu_live` prints its engine and adapter counts;
on the development machine expect roughly a thousand engine instances folded
into three adapters.

- [ ] **Step 6: Commit**

```bash
git add include/gpu.h src/gpu.c tests/test_gpu_live.c tests/CMakeLists.txt
git commit -m "feat: sample GPU engine counters through a persistent PDH query"
```

---

### Task 6: Adapter names and video memory from DXGI

PDH identifies adapters only by LUID. A list reading `luid_0x00000000_0x00012cae`
is not a user interface, so names and total video memory come from DXGI.

**Files:**
- Modify: `src/gpu.c`, `include/gpu.h`
- Test: `tests/test_gpu_live.c`

**Interfaces:**
- Consumes: `Gpu_FormatLuid`
- Produces:
  - `typedef struct { ULONGLONG luid; WCHAR name[GPU_NAME_MAX]; ULONGLONG dedicatedTotal, sharedTotal; } GpuAdapterInfo;`
  - `int Gpu_DescribeAdapters(GpuAdapterInfo *out, int max);`

- [ ] **Step 1: Write the failing live test**

Append to `tests/test_gpu_live.c`, before `Gpu_QueryClose()`:

```c
    {
        /* DXGI is optional: absence must degrade to LUID-named adapters,
           never drop them. When present, every adapter must be named. */
        GpuAdapterInfo info[GPU_MAX_ADAPTERS];
        int describes = Gpu_DescribeAdapters(info, GPU_MAX_ADAPTERS);
        CHECK(describes >= 0);
        if (describes > 0) {
            int i;
            for (i = 0; i < describes; ++i) {
                CHECK(info[i].name[0] != L'\0');
                CHECK(info[i].luid != 0);
            }
            printf("gpu_live: DXGI described %d adapters, first = %ls\n",
                   describes, info[0].name);
        } else {
            printf("gpu_live: DXGI unavailable; adapters will be LUID-named\n");
        }
    }
```

- [ ] **Step 2: Run to verify it fails**

```bash
./build.cmd test -Strict
```

Expected: FAIL to compile, `unknown type name 'GpuAdapterInfo'`.

- [ ] **Step 3: Declare in `include/gpu.h`**

```c
typedef struct {
    ULONGLONG luid;
    WCHAR     name[GPU_NAME_MAX];
    ULONGLONG dedicatedTotal;
    ULONGLONG sharedTotal;
} GpuAdapterInfo;

/* Enumerates adapters through DXGI for their names and memory sizes.
   Returns 0 when DXGI is unavailable, which is not an error: callers fall
   back to naming adapters by LUID rather than hiding them. */
int Gpu_DescribeAdapters(GpuAdapterInfo *out, int max);
```

- [ ] **Step 4: Implement in `src/gpu.c`**

Add to the include block, before any other DXGI use:

```c
#define COBJMACROS
#include <dxgi.h>
```

```c
/* dxgi.dll is resolved at runtime so the executable keeps its property of
   having no load-time dependency on anything but core Windows DLLs. */
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID riid, void **factory);

int Gpu_DescribeAdapters(GpuAdapterInfo *out, int max)
{
    PFN_CreateDXGIFactory1 create;
    HMODULE library;
    IDXGIFactory1 *factory = NULL;
    int count = 0;
    UINT index;

    if (!out || max <= 0) return 0;
    library = LoadLibraryW(L"dxgi.dll");
    if (!library) return 0;
    create = (PFN_CreateDXGIFactory1)(void *)GetProcAddress(library, "CreateDXGIFactory1");
    if (!create || FAILED(create(&IID_IDXGIFactory1, (void **)&factory)) || !factory) {
        FreeLibrary(library);
        return 0;
    }

    for (index = 0; count < max; ++index) {
        IDXGIAdapter1 *adapter = NULL;
        DXGI_ADAPTER_DESC1 desc;
        if (IDXGIFactory1_EnumAdapters1(factory, index, &adapter) != S_OK || !adapter) break;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &desc))) {
            ZeroMemory(&out[count], sizeof(out[count]));
            /* PDH's LUID text is high half then low half; match that order. */
            out[count].luid = ((ULONGLONG)(ULONG)desc.AdapterLuid.HighPart << 32) |
                              (ULONGLONG)desc.AdapterLuid.LowPart;
            lstrcpynW(out[count].name, desc.Description, GPU_NAME_MAX);
            out[count].dedicatedTotal = (ULONGLONG)desc.DedicatedVideoMemory;
            out[count].sharedTotal    = (ULONGLONG)desc.SharedSystemMemory;
            ++count;
        }
        IDXGIAdapter1_Release(adapter);
    }

    IDXGIFactory1_Release(factory);
    /* dxgi.dll stays loaded: releasing the factory and unloading in the same
       breath has historically been unstable, and the module is tiny. */
    return count;
}
```

MinGW needs `-lole32` for `IID_IDXGIFactory1`, which is already in the platform
link libraries. If the linker cannot resolve `IID_IDXGIFactory1`, add `dxguid`
to `target_link_libraries(taskman_platform ...)` in `CMakeLists.txt` and note
it in your report.

- [ ] **Step 5: Run to verify it passes**

```bash
./build.cmd test -Strict
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test -Strict -Toolchain mingw
```

Expected: `gpu_live` prints the adapter names; on the development machine
three adapters, the first being the discrete GPU.

- [ ] **Step 6: Commit**

```bash
git add include/gpu.h src/gpu.c tests/test_gpu_live.c
git commit -m "feat: name GPU adapters and read video memory through DXGI"
```

---

### Task 7: The published model

Combines the sampled figures with the DXGI descriptions into a model the UI
will later read under a lock, following the per-tab model pattern that `Proc`,
`Net`, `Svc` and `Users` already use.

**Files:**
- Modify: `src/gpu.c`, `include/gpu.h`
- Test: `tests/test_gpu.c`

**Interfaces:**
- Consumes: everything from Tasks 3-6
- Produces:
  - `GpuAdapter` (luid, name, nameKnown, utilization, engine[], memory, history)
  - `void Gpu_SetEnabled(BOOL enabled);`
  - `BOOL Gpu_IsEnabled(void);`
  - `void Gpu_Collect(ULONGLONG nowTick);`
  - `const GpuAdapter *Gpu_Lock(int *count);` / `void Gpu_Unlock(void);`
  - `int Gpu_ProcessUsage(DWORD pid, double *out);`
  - `void Gpu_Reset(void);`

- [ ] **Step 1: Write the failing tests**

```c
/* Enabled state crosses the UI/collector boundary, so it must be readable
   without holding any lock and must default to off -- nothing should pay
   the enumeration cost until something asks for the data. */
static void TestEnabledFlag(void)
{
    CHECK(Gpu_IsEnabled() == FALSE);
    Gpu_SetEnabled(TRUE);
    CHECK(Gpu_IsEnabled() == TRUE);
    Gpu_SetEnabled(FALSE);
    CHECK(Gpu_IsEnabled() == FALSE);
}

/* Collecting while disabled must not open a query or publish anything. */
static void TestCollectSkippedWhenDisabled(void)
{
    int count = -1;
    const GpuAdapter *model;

    Gpu_SetEnabled(FALSE);
    Gpu_Collect(GetTickCount64());
    model = Gpu_Lock(&count);
    CHECK(count == 0);
    CHECK(model != NULL);        /* a valid empty model, never NULL */
    Gpu_Unlock();
    Gpu_Reset();
}

/* An unknown pid reports no usage rather than a stale or zero reading. */
static void TestProcessUsageUnknownPid(void)
{
    double value = 123.0;
    Gpu_Reset();
    CHECK(Gpu_ProcessUsage(0xFFFFFFFEu, &value) == 0);
    CHECK(value == 123.0);       /* untouched on a miss */
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
./build.cmd test -Strict
```

Expected: FAIL to compile, `implicit declaration of function 'Gpu_IsEnabled'`.

- [ ] **Step 3: Declare in `include/gpu.h`**

Use the interval chosen in Task 1; the value below assumes the ≤10 ms outcome
and MUST be replaced with whatever `gpu-cost-measurement.md` recorded.

```c
#define GPU_HISTORY             128
#define GPU_COLLECT_INTERVAL_MS 1000   /* set from Task 1's measurement */

typedef struct {
    ULONGLONG luid;
    WCHAR     name[GPU_NAME_MAX];
    BOOL      nameKnown;
    double    utilization;
    double    engine[GPU_ENGINE_KINDS];
    ULONGLONG dedicatedUsed;                /* from the PDH memory counter */
    ULONGLONG dedicatedTotal, sharedTotal;  /* from DXGI                   */
    float     history[GPU_HISTORY];
    int       head, count;
} GpuAdapter;

/* Published from the UI thread with an interlocked store; the collector
   never reads g_cfg. Defaults to FALSE so no PDH query is opened until
   the Sensors tab or the per-process column asks for data. */
void Gpu_SetEnabled(BOOL enabled);
BOOL Gpu_IsEnabled(void);

/* Called on the collector thread, before the per-tab collectors. Does
   nothing at all when disabled, and rate-limits itself to
   GPU_COLLECT_INTERVAL_MS regardless of the update speed. */
void Gpu_Collect(ULONGLONG nowTick);

/* Shared read lock over the published model. Always returns a valid
   pointer; *count may be 0. */
const GpuAdapter *Gpu_Lock(int *count);
void Gpu_Unlock(void);

/* Writes that pid's utilisation and returns 1, or returns 0 and leaves
   *out untouched when the pid has no GPU usage this sample. */
int Gpu_ProcessUsage(DWORD pid, double *out);

/* Frees the model and closes the query. Call after the collector has been
   joined, as the other per-tab models already require. */
void Gpu_Reset(void);
```

- [ ] **Step 4: Implement in `src/gpu.c`**

```c
static SRWLOCK          s_lock = SRWLOCK_INIT;
static GpuAdapter       s_model[GPU_MAX_ADAPTERS];
static int              s_modelCount;
static GpuProcessSample s_processes[GPU_MAX_PROCESSES];
static int              s_processCount;
static volatile LONG    s_enabled;
static ULONGLONG        s_lastCollect;
static GpuAccumulator   s_accum;   /* ~14 KB: static, not on the stack */

void Gpu_SetEnabled(BOOL enabled)
{
    InterlockedExchange(&s_enabled, enabled ? 1 : 0);
}

BOOL Gpu_IsEnabled(void)
{
    return InterlockedCompareExchange(&s_enabled, 0, 0) != 0;
}

/* Merges one sample into the persistent model, preserving each adapter's
   history ring across samples -- which is why the sample and the model are
   separate types rather than one struct reused. */
static void GpuPublish(const GpuAdapterSample *samples, int sampleCount,
                       const GpuAdapterInfo *info, int infoCount)
{
    GpuAdapter merged[GPU_MAX_ADAPTERS];
    int i, j;

    ZeroMemory(merged, sizeof(merged));
    for (i = 0; i < sampleCount && i < GPU_MAX_ADAPTERS; ++i) {
        merged[i].luid = samples[i].luid;
        merged[i].utilization = samples[i].utilization;
        merged[i].dedicatedUsed = samples[i].dedicatedUsed;
        CopyMemory(merged[i].engine, samples[i].engine, sizeof(merged[i].engine));

        for (j = 0; j < infoCount; ++j) {
            if (info[j].luid != samples[i].luid) continue;
            lstrcpynW(merged[i].name, info[j].name, GPU_NAME_MAX);
            merged[i].nameKnown = TRUE;
            merged[i].dedicatedTotal = info[j].dedicatedTotal;
            merged[i].sharedTotal    = info[j].sharedTotal;
            break;
        }
        if (!merged[i].nameKnown)
            Gpu_FormatLuid(samples[i].luid, merged[i].name, GPU_NAME_MAX);

        /* Carry the ring forward from the previous model entry for this
           adapter, so a graph is not reset every sample. */
        for (j = 0; j < s_modelCount; ++j) {
            if (s_model[j].luid != samples[i].luid) continue;
            CopyMemory(merged[i].history, s_model[j].history, sizeof(merged[i].history));
            merged[i].head  = s_model[j].head;
            merged[i].count = s_model[j].count;
            break;
        }
        merged[i].history[merged[i].head] = (float)samples[i].utilization;
        merged[i].head = (merged[i].head + 1) % GPU_HISTORY;
        if (merged[i].count < GPU_HISTORY) merged[i].count++;
    }

    AcquireSRWLockExclusive(&s_lock);
    CopyMemory(s_model, merged, sizeof(s_model));
    s_modelCount = (sampleCount < GPU_MAX_ADAPTERS) ? sampleCount : GPU_MAX_ADAPTERS;
    ReleaseSRWLockExclusive(&s_lock);
}

void Gpu_Collect(ULONGLONG nowTick)
{
    GpuAdapterSample samples[GPU_MAX_ADAPTERS];
    GpuAdapterInfo   info[GPU_MAX_ADAPTERS];
    int sampleCount, infoCount, processCount;

    if (!Gpu_IsEnabled()) return;
    if (s_lastCollect && nowTick - s_lastCollect < GPU_COLLECT_INTERVAL_MS) return;
    if (!Gpu_QueryOpen()) return;

    Gpu_AccumReset(&s_accum);
    if (!Gpu_QuerySample(&s_accum, nowTick)) return;  /* priming, or no data */
    s_lastCollect = nowTick;

    sampleCount  = Gpu_AccumAdapters(&s_accum, samples, GPU_MAX_ADAPTERS);
    infoCount    = Gpu_DescribeAdapters(info, GPU_MAX_ADAPTERS);
    processCount = Gpu_AccumProcesses(&s_accum, s_processes, GPU_MAX_PROCESSES);

    AcquireSRWLockExclusive(&s_lock);
    s_processCount = processCount;
    ReleaseSRWLockExclusive(&s_lock);

    GpuPublish(samples, sampleCount, info, infoCount);
}

const GpuAdapter *Gpu_Lock(int *count)
{
    AcquireSRWLockShared(&s_lock);
    if (count) *count = s_modelCount;
    return s_model;
}

void Gpu_Unlock(void)
{
    ReleaseSRWLockShared(&s_lock);
}

int Gpu_ProcessUsage(DWORD pid, double *out)
{
    int i, found = 0;
    if (!out) return 0;
    AcquireSRWLockShared(&s_lock);
    for (i = 0; i < s_processCount; ++i) {
        if (s_processes[i].pid != pid) continue;
        *out = s_processes[i].utilization;
        found = 1;
        break;
    }
    ReleaseSRWLockShared(&s_lock);
    return found;
}

void Gpu_Reset(void)
{
    Gpu_QueryClose();
    AcquireSRWLockExclusive(&s_lock);
    ZeroMemory(s_model, sizeof(s_model));
    s_modelCount = 0;
    s_processCount = 0;
    ReleaseSRWLockExclusive(&s_lock);
    s_lastCollect = 0;
}
```

Note `Gpu_DescribeAdapters` runs on every published sample. If the Task 1
measurement showed DXGI enumeration is expensive, cache it and refresh on the
same `GPU_REBUILD_INTERVAL_MS` schedule as the counters; record which you did.

- [ ] **Step 5: Run to verify it passes**

```bash
./build.cmd test -Strict
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test -Strict -Toolchain mingw
```

Expected: twelve suites pass, both toolchains clean.

- [ ] **Step 6: Commit**

```bash
git add include/gpu.h src/gpu.c tests/test_gpu.c
git commit -m "feat: publish the GPU model under a shared lock"
```

---

### Task 8: Collector integration

**Files:**
- Modify: `src/sysinfo.c`, `include/app.h`, `src/main.c`
- Test: `tests/test_collector.c`

**Interfaces:**
- Consumes: `Gpu_Collect`, `Gpu_Reset`
- Produces: GPU sampling on the collector thread, ordered before the per-tab
  collectors

- [ ] **Step 1: Write the failing test**

Add to `tests/test_collector.c`, and call it from that file's `main`:

```c
/* Nothing should pay the PDH enumeration cost until the Sensors tab or the
   per-process column asks for it, so the flag must default to off and a
   collect while disabled must publish nothing.
   The ordering requirement -- Gpu_Collect before g_tabCollect -- is NOT
   asserted here: it is a statement about source order inside CollectorProc
   with no observable effect until unit 4 reads the per-process figures.
   Verify it by reading the code, and do not rename this test to imply
   otherwise. */
static void TestGpuDisabledByDefault(void)
{
    int count = -1;

    CHECK(Gpu_IsEnabled() == FALSE);
    Gpu_Collect(GetTickCount64());
    Gpu_Lock(&count);
    Gpu_Unlock();
    CHECK(count == 0);
}
```

Then call it from that file's `main`, before its summary `printf`. A test that
is defined but never invoked passes silently and proves nothing:

```c
    TestGpuDisabledByDefault();
```

- [ ] **Step 2: Run to verify it fails**

```bash
./build.cmd test -Strict
```

Expected: FAIL to compile, `implicit declaration of function 'Gpu_IsEnabled'`
in `test_collector.c` until the include is added.

- [ ] **Step 3: Call it from the collector**

In `src/sysinfo.c`, add `#include "gpu.h"` to the include block, and in
`CollectorProc` place the call between `HistoryPush` and `g_tabCollect`:

```c
        HistoryPush((float)back->cpuUsage, (float)back->cpuKernel,
                    (float)back->memUsage);

        /* Before the tab collectors: Proc_Collect reads the per-process
           figures, and reading them after they were refreshed would leave
           the Processes tab a sample behind. Gpu_Collect returns
           immediately unless something has enabled it. */
        Gpu_Collect(GetTickCount64());

        if (g_tabCollect)
            g_tabCollect((int)InterlockedCompareExchange(&g_activeTab, 0, 0));
```

- [ ] **Step 4: Release it on shutdown**

`Gpu_Reset` closes the PDH query, so it must run after the worker has been
joined. In `src/main.c`'s `WM_DESTROY` handler, add it beside the other model
resets, which already run after `SysInfo_Stop`:

```c
        SysInfo_Stop();
        Proc_Reset();
        Apps_Reset();
        Svc_Reset();
        Users_Reset();
        Net_Reset();
        Gpu_Reset();
```

Add `#include "gpu.h"` to `src/main.c` and to `tests/test_collector.c`.

- [ ] **Step 5: Verify**

```bash
./build.cmd test -Strict
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 test -Strict -Toolchain mingw
```

Expected: twelve suites pass on both toolchains, zero warnings.

Then confirm the cost claim holds in the real application. Launch
`build\taskman.exe`, leave it on the Processes tab for thirty seconds, and
confirm its own CPU usage is unchanged from before this task — GPU collection
is disabled by default, so it must cost exactly nothing until unit 2 or unit 4
enables it.

- [ ] **Step 6: Commit**

```bash
git add src/sysinfo.c src/main.c tests/test_collector.c
git commit -m "feat: sample GPU on the collector thread ahead of the tab collectors"
```

---

## Final verification

- [ ] `./build.cmd rebuild -Strict` — MSVC, zero warnings
- [ ] `powershell -NoProfile -File build.ps1 rebuild -Strict -Toolchain mingw` — MinGW, zero warnings
- [ ] `./build.cmd test -Strict` — twelve suites pass
- [ ] `gpu_live` reports a plausible engine and adapter count, and adapter names
- [ ] `docs/superpowers/plans/gpu-cost-measurement.md` exists and the value it
      chose matches `GPU_COLLECT_INTERVAL_MS` in `include/gpu.h`
- [ ] The application launches, runs for a minute and closes cleanly, with no
      change in its own CPU usage — GPU collection is off by default
- [ ] Nothing in `src/gpu.c` calls a Win32 UI function
