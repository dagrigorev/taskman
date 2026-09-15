/* ------------------------------------------------------------------------
 * proc_diff.c - compare the process list against an earlier mark.
 * ------------------------------------------------------------------------ */
#include <stdlib.h>
#include <string.h>
#include "proc_diff.h"

static int ProcDiffOrder(DWORD pid, ULONGLONG createTime, const ProcMarkEntry *e)
{
    if (pid != e->pid) return pid < e->pid ? -1 : 1;
    if (createTime != e->createTime) return createTime < e->createTime ? -1 : 1;
    return 0;
}

static int ProcDiffSort(const void *va, const void *vb)
{
    const ProcMarkEntry *a = va, *b = vb;
    return ProcDiffOrder(a->pid, a->createTime, b);
}

/* Index of the mark entry for this identity, or -1. */
static int ProcDiffFind(const ProcMark *mark, DWORD pid, ULONGLONG createTime)
{
    int lo = 0, hi = mark->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = ProcDiffOrder(pid, createTime, &mark->entries[mid]);
        if (!cmp) return mid;
        if (cmp < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

BOOL ProcDiff_Take(ProcMark *mark, const ProcRow *rows, int count, FILETIME wallTime)
{
    ProcMarkEntry *entries;
    int i, kept = 0;
    if (!mark) return FALSE;
    ProcDiff_Free(mark);
    if (!rows || count <= 0) return FALSE;
    entries = (ProcMarkEntry *)malloc((size_t)count * sizeof(*entries));
    if (!entries) return FALSE;
    for (i = 0; i < count; ++i) {
        ProcMarkEntry *e;
        if (!rows[i].pid) continue;
        e = &entries[kept++];
        e->pid = rows[i].pid;
        e->createTime = rows[i].createTime;
        e->privateBytes = rows[i].privateBytes;
        e->memoryKnown = rows[i].memoryKnown;
        e->handles = rows[i].handles;
        memcpy(e->imageName, rows[i].imageName, sizeof(e->imageName));
        e->imageName[PROC_IMAGE_MAX - 1] = L'\0';
    }
    if (kept > 1) qsort(entries, (size_t)kept, sizeof(*entries), ProcDiffSort);
    mark->entries = entries;
    mark->count = kept;
    mark->wallTime = wallTime;
    return TRUE;
}

void ProcDiff_Free(ProcMark *mark)
{
    if (!mark) return;
    free(mark->entries);
    mark->entries = NULL;
    mark->count = 0;
}

BOOL ProcDiff_IsSet(const ProcMark *mark)
{
    return mark && mark->entries != NULL;
}

ProcChange ProcDiff_Classify(const ProcMark *mark, const ProcRow *row,
                             LONGLONG *memDelta, LONG *handleDelta)
{
    const ProcMarkEntry *e;
    LONGLONG mem = 0;
    LONG handles;
    int at;
    if (memDelta) *memDelta = 0;
    if (handleDelta) *handleDelta = 0;
    if (!ProcDiff_IsSet(mark) || !row || !row->pid) return PROC_CHANGE_NONE;
    at = ProcDiffFind(mark, row->pid, row->createTime);
    if (at < 0) return PROC_CHANGE_NEW;
    e = &mark->entries[at];
    if (row->memoryKnown && e->memoryKnown)
        mem = (LONGLONG)row->privateBytes - (LONGLONG)e->privateBytes;
    handles = (LONG)row->handles - (LONG)e->handles;
    if (memDelta) *memDelta = mem;
    if (handleDelta) *handleDelta = handles;
    return mem >= (LONGLONG)PROC_DIFF_GREW_BYTES || handles >= PROC_DIFF_GREW_HANDLES
        ? PROC_CHANGE_GREW : PROC_CHANGE_NONE;
}

void ProcDiff_Count(const ProcMark *mark, const ProcRow *rows, int count,
                    ProcDiffCounts *out, ProcMarkEntry *exited, int maxExited)
{
    BYTE *seen;
    int i, copied = 0;
    if (!out) return;
    ZeroMemory(out, sizeof(*out));
    if (!ProcDiff_IsSet(mark)) return;
    seen = (BYTE *)calloc((size_t)(mark->count > 0 ? mark->count : 1), 1);
    for (i = 0; i < count; ++i) {
        int at;
        switch (ProcDiff_Classify(mark, &rows[i], NULL, NULL)) {
        case PROC_CHANGE_NEW:  ++out->started; break;
        case PROC_CHANGE_GREW: ++out->grew;    break;
        default: break;
        }
        if (seen && rows[i].pid && (at = ProcDiffFind(mark, rows[i].pid, rows[i].createTime)) >= 0)
            seen[at] = 1;
    }
    if (!seen) {
        out->exited = -1;       /* unknown: could not track which survived */
        return;
    }
    for (i = 0; i < mark->count; ++i) {
        if (seen[i]) continue;
        ++out->exited;
        if (exited && copied < maxExited) exited[copied++] = mark->entries[i];
    }
    free(seen);
}
