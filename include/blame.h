/* ------------------------------------------------------------------------
 * blame.h - Spike Blame: who was busy at each point of the history graphs.
 *
 * The collector thread records, for every sample, the processes using the
 * most CPU and the most private memory. The Performance tab reads the ring
 * back aligned with SysInfo_CopyCpuHistory, so hovering a graph column
 * names the processes behind that column.
 *
 * The ranking, ring and coordinate mapping are pure and unit tested with
 * no window; only Blame_Collect touches the system.
 * ------------------------------------------------------------------------ */
#ifndef CTM_BLAME_H
#define CTM_BLAME_H

#include <windows.h>

#define BLAME_TOP       5       /* culprits kept per sample, per resource    */
#define BLAME_SAMPLES   128     /* matches the Performance graph width       */
#define BLAME_IMAGE_MAX 64
#define BLAME_MAX_PROCS 4096    /* previous-sample table for CPU deltas      */

typedef struct {
    DWORD     pid;
    ULONGLONG createTime;       /* disambiguates a reused pid                */
    float     cpuPct;           /* 0..100 of all processors                  */
    ULONGLONG privateBytes;
    WCHAR     image[BLAME_IMAGE_MAX];
} BlameEntry;

typedef struct {
    ULONG64    sequence;        /* 0 marks an empty slot                     */
    FILETIME   wallTime;        /* local time the sample was taken, as UTC   */
    float      cpu;             /* system CPU 0..100                         */
    float      mem;             /* system physical memory 0..100             */
    int        cpuCount;
    int        memCount;
    BlameEntry byCpu[BLAME_TOP];
    BlameEntry byMem[BLAME_TOP];
} BlameSample;

typedef struct {
    BlameSample samples[BLAME_SAMPLES];
    int         head;           /* next slot to write                        */
    int         count;
} BlameRing;

typedef enum { BLAME_BY_CPU = 0, BLAME_BY_MEM } BlameMetric;

/* Inserts 'e' into a descending top list of at most BLAME_TOP entries.
   Ties keep the earlier entry ahead, so a stable input order ranks stably.
   A zero value never enters the CPU list: an idle process is no culprit. */
void Blame_Offer(BlameEntry *top, int *count, const BlameEntry *e, BlameMetric metric);

void Blame_RingReset(BlameRing *ring);
void Blame_RingPush(BlameRing *ring, const BlameSample *sample);
/* Copies the newest 'count' samples oldest first. Like the history copies,
   the newest lands in dst[count-1] and missing samples are zeroed at the
   front. Returns the number of real samples. */
int  Blame_RingCopy(const BlameRing *ring, BlameSample *dst, int count);

/* Maps a client x coordinate to the sample index that UI_Chart plots
   nearest to it, for a chart of 'count' samples 'width' pixels wide. */
int  Blame_IndexFromX(int x, int width, int count);
/* The x coordinate UI_Chart uses for sample 'index'. */
int  Blame_XFromIndex(int index, int width, int count);

/* Index of the highest valid sample for the metric, or -1 if none. The
   latest sample wins a tie, since it is the one still worth chasing. */
int  Blame_FindPeak(const BlameSample *samples, int count, BlameMetric metric);

/* Index of the sample carrying 'sequence', or -1 once it scrolled away. */
int  Blame_FindSequence(const BlameSample *samples, int count, ULONG64 sequence);

/* --- collection, collector thread only ----------------------------------- */

/* Enumerates processes and pushes one sample. 'cpu' and 'mem' are the
   system figures the graphs will plot for this same sample. */
void Blame_Collect(ULONG64 sequence, float cpu, float mem);
/* Clears the ring and the CPU baseline; call before the collector starts. */
void Blame_Reset(void);
/* Thread-safe copy for the UI, same contract as Blame_RingCopy. */
int  Blame_Copy(BlameSample *dst, int count);
/* Hands this sample's raw SystemProcessInformation buffer to another
   collector on the same thread, so the process table need not enumerate a
   second time. Once per sample: a second call, or a sample whose query
   failed, returns FALSE. Valid until the next Blame_Collect or Blame_Reset. */
BOOL Blame_TakeListing(const BYTE **base, ULONG *used);
/* TRUE when the latest enumeration saw this pid with this creation time.
   Covers protected processes; may lag an exit by one sample. */
BOOL Blame_IsAlive(DWORD pid, ULONGLONG createTime);

#endif /* CTM_BLAME_H */
