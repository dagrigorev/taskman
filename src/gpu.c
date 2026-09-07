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
