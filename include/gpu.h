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

/* Caller-allocated. Roughly 14 KB, so callers place it in static or heap
   storage rather than on a thread stack. */
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

#define GPU_HISTORY             128
/* Set from the Task 1 measurement: a native collection costs 1.25 ms at
   1029 instances, comfortably inside the 10 ms budget, so one second. */
#define GPU_COLLECT_INTERVAL_MS 1000

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
/* Copies an adapter's ring into a flat oldest-first array, as UI_Chart
   expects. When 'max' is smaller than the ring the NEWEST 'max' samples are
   returned -- a short graph should show recent history, not ancient history.
   Returns the number of samples written. */
int Gpu_History(const GpuAdapter *adapter, float *out, int max);

const GpuAdapter *Gpu_Lock(int *count);
void Gpu_Unlock(void);

/* Writes that pid's utilisation and returns 1, or returns 0 and leaves
   *out untouched when the pid has no GPU usage this sample. */
int Gpu_ProcessUsage(DWORD pid, double *out);

/* Frees the model and closes the query. Call after the collector has been
   joined, as the other per-tab models already require. */
void Gpu_Reset(void);

#endif /* CTM_GPU_H */
