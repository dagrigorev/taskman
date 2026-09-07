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
