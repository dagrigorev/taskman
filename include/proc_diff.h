/* ------------------------------------------------------------------------
 * proc_diff.h - compare the process list against an earlier mark.
 *
 * Pure data transform over ProcRow arrays: no Win32 UI calls and no
 * globals, so it is unit tested with synthetic rows.
 * ------------------------------------------------------------------------ */
#ifndef CTM_PROC_DIFF_H
#define CTM_PROC_DIFF_H

#include "proc_tree.h"

/* A row counts as grown when its private memory rose by at least this
   much, or its handle count by at least PROC_DIFF_GREW_HANDLES. Absolute
   floors keep small processes' ordinary churn out of the list. */
#define PROC_DIFF_GREW_BYTES   (16ULL * 1024 * 1024)
#define PROC_DIFF_GREW_HANDLES 256

typedef struct {
    DWORD     pid;
    ULONGLONG createTime;       /* identity: a reused pid is a new process  */
    ULONGLONG privateBytes;
    BOOL      memoryKnown;
    DWORD     handles;
    WCHAR     imageName[PROC_IMAGE_MAX];
} ProcMarkEntry;

typedef struct {
    ProcMarkEntry *entries;     /* sorted by pid, then createTime           */
    int            count;
    FILETIME       wallTime;    /* when the mark was taken, UTC             */
} ProcMark;

typedef enum {
    PROC_CHANGE_NONE = 0,
    PROC_CHANGE_NEW,            /* not running when the mark was taken      */
    PROC_CHANGE_GREW
} ProcChange;

typedef struct {
    int started, exited, grew;
} ProcDiffCounts;

/* Replaces any previous mark with the given rows. PID 0 is skipped, as the
   process table skips it. Returns FALSE, leaving the mark empty, when
   memory runs out. */
BOOL ProcDiff_Take(ProcMark *mark, const ProcRow *rows, int count, FILETIME wallTime);
void ProcDiff_Free(ProcMark *mark);
BOOL ProcDiff_IsSet(const ProcMark *mark);

/* Classifies one current row. The deltas are optional and written for
   every row found in the mark (zero for a new one). Unknown memory on
   either side never counts as growth. */
ProcChange ProcDiff_Classify(const ProcMark *mark, const ProcRow *row,
                             LONGLONG *memDelta, LONG *handleDelta);

/* Counts started, exited and grown processes between the mark and rows.
   Up to maxExited exited entries are copied to 'exited', in mark order. */
void ProcDiff_Count(const ProcMark *mark, const ProcRow *rows, int count,
                    ProcDiffCounts *out, ProcMarkEntry *exited, int maxExited);

#endif /* CTM_PROC_DIFF_H */
