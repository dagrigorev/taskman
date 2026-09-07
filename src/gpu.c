/* ------------------------------------------------------------------------
 * gpu.c - GPU utilisation and video memory acquisition.
 * Pure acquisition. No Win32 UI calls: see gpu.h.
 * ------------------------------------------------------------------------ */
#include "gpu.h"
#include <shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>
#include <pdh.h>
/* PDH_MORE_DATA and the PDH_CSTATUS_* values live here, not in pdh.h. */
#include <pdhmsg.h>
#define COBJMACROS
#include <dxgi.h>

/* Returns the text just past `marker`, or NULL when it does not occur.
   Note "_eng_" cannot match inside "_engtype_", which reads "_engt". */
static const WCHAR *GpuAfter(const WCHAR *text, const WCHAR *marker)
{
    const WCHAR *at = text ? StrStrIW(text, marker) : NULL;
    return at ? at + lstrlenW(marker) : NULL;
}

GpuEngineKind Gpu_ClassifyEngine(const WCHAR *engtype)
{
    if (!engtype || !engtype[0])                 return GPU_ENGINE_OTHER;
    if (!lstrcmpiW(engtype, L"3d"))              return GPU_ENGINE_3D;
    if (!lstrcmpiW(engtype, L"copy"))            return GPU_ENGINE_COPY;
    if (!lstrcmpiW(engtype, L"videodecode"))     return GPU_ENGINE_VIDEO_DECODE;
    if (!lstrcmpiW(engtype, L"videoencode"))     return GPU_ENGINE_VIDEO_ENCODE;
    if (!lstrcmpiW(engtype, L"videoprocessing")) return GPU_ENGINE_VIDEO_PROCESS;
    if (!lstrcmpiW(engtype, L"compute"))         return GPU_ENGINE_COMPUTE;
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
    DWORD size = 0, count = 0, i;
    PDH_FMT_COUNTERVALUE_ITEM_W *items;
    PDH_STATUS status;

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
    /* PDH_STATUS is signed on MinGW while PDH_MORE_DATA expands unsigned;
       compare in the status type so -Wsign-compare stays quiet. */
    if (status != (PDH_STATUS)PDH_MORE_DATA || size == 0) return FALSE;
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
        if (status == (PDH_STATUS)PDH_MORE_DATA && size > 0) {
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
