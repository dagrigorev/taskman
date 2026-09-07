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

#endif /* CTM_GPU_H */
