/* ------------------------------------------------------------------------
 * gpu.c - GPU utilisation and video memory acquisition.
 * Pure acquisition. No Win32 UI calls: see gpu.h.
 * ------------------------------------------------------------------------ */
#include "gpu.h"
#include <shlwapi.h>
#include <strsafe.h>
#include <stdlib.h>
#include <wchar.h>

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
