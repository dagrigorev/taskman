/* ------------------------------------------------------------------------
 * proc_tree.h - process record and hierarchy model.
 *
 * The tree side is a pure data transform: no Win32 UI calls, no globals
 * owned here, so it can be unit tested with no window and no message pump.
 * ------------------------------------------------------------------------ */
#ifndef CTM_PROC_TREE_H
#define CTM_PROC_TREE_H

#include <windows.h>

#define PROC_IMAGE_MAX   64
#define PROC_USER_MAX    512
#define PROC_DESC_MAX    128

#define PROC_INDENT_CAP  8    /* visual indent stops here; depth does not   */
#define PROC_DEPTH_MAX   64   /* structural ceiling, defeats cycles         */

typedef struct {
    DWORD     pid;
    ULONGLONG createTime;
    WCHAR     imageName[PROC_IMAGE_MAX];
    WCHAR     userName[PROC_USER_MAX];
    WCHAR     description[PROC_DESC_MAX];
    float     cpuPct;
    float     gpuPct;         /* summed across engines and adapters       */
    BOOL      gpuKnown;       /* FALSE when the query never saw this pid  */
    ULONGLONG privateBytes;
    BOOL      memoryKnown;
    BOOL      ownedByCurrentUser;
    DWORD     parentPid, threads, handles;
    ULONGLONG cpuTime, ioRead, ioWrite;
    BOOL      countersKnown;
} ProcRow;

/* Parallel to a ProcRow array; same indices, same length. */
typedef struct {
    int       parent;         /* index, -1 = root                           */
    int       firstChild;     /* index, -1 = none                           */
    int       nextSibling;    /* index, -1 = none                           */
    int       depth;          /* 0 = root                                   */
    int       childCount;     /* children among survivors; 0 => no twisty   */
    BOOL      collapsed;
    BOOL      context;        /* ancestor of a match, not a match itself    */
    float     cpuRollup;      /* subtree total, includes this row           */
    float     gpuRollup;      /* subtree total, includes this row           */
    ULONGLONG memRollup;
    BOOL      memRollupKnown; /* FALSE only when nothing in subtree is known */
} ProcTreeInfo;

/* Resets every ProcTreeInfo field for `count` rows EXCEPT `context`, then
   fills parent/firstChild/nextSibling/depth/childCount from `rows`.
   `context` is the caller's responsibility: ProcTree_ApplyContext sets
   tree[kept].context immediately before calling Link to relink the
   compacted array, so Link must not clear it or it would erase the value
   ApplyContext just computed.
   A parent is only accepted when its createTime is not later than the
   child's, which is what stops a recycled PID adopting an older process.
   Rows whose parent is absent, invalid, or would exceed PROC_DEPTH_MAX
   become roots. Roots are linked into their own sibling chain; its head is
   written to *firstRoot, which is -1 when count is 0. Safe against cycles. */
void ProcTree_Link(const ProcRow *rows, ProcTreeInfo *tree, int count,
                   int *firstRoot);

/* Computes subtree totals into cpuRollup/gpuRollup/memRollup/memRollupKnown.
   Rollups include the row itself. Rows with memoryKnown == FALSE contribute
   nothing to memRollup; memRollupKnown is FALSE only when no row in the
   subtree has a known value. Rows with gpuKnown == FALSE contribute nothing
   to gpuRollup and there is no corresponding known flag: a process the GPU
   query never saw is using no measurable GPU, which is a real zero rather
   than an absence. Requires ProcTree_Link to have run. */
void ProcTree_Aggregate(const ProcRow *rows, ProcTreeInfo *tree, int count);

/* Keeps matches and their ancestors, drops the rest, and marks ancestors
   that are not themselves matches as context. Compacts rows and tree in
   place, re-links over the survivors, and returns the new count.
   `matches` is parallel to `rows`; passing NULL means no search is active,
   in which case every row is kept, nothing is marked context, and the
   existing links and *firstRoot are left untouched.
   Requires ProcTree_Link to have run. */
int ProcTree_ApplyContext(ProcRow *rows, ProcTreeInfo *tree, int count,
                          const BOOL *matches, int *firstRoot);

/* Remembers which processes the user collapsed, across the 1s refresh.
   The default is expanded, so this stores the collapsed set, which is
   normally empty. Identity is (pid, createTime): a recycled PID is a
   different process and starts expanded. Not persisted across runs. */
typedef struct ProcCollapseSet ProcCollapseSet;

/* Orders two rows. Implementations should use the rollup when collapsed
   and the row's own value otherwise, so a row sorts by what it displays. */
typedef int (*ProcTreeCmp)(const ProcRow *ra, const ProcTreeInfo *ta,
                           const ProcRow *rb, const ProcTreeInfo *tb);

/* Sorts roots among themselves and each sibling group internally. Only the
   nextSibling/firstChild links move; array indices stay valid. */
void ProcTree_Sort(const ProcRow *rows, ProcTreeInfo *tree, int count,
                   ProcTreeCmp cmp, int *firstRoot);

/* Writes row indices in depth-first order into `out` (capacity `count`),
   starting from the root chain at `firstRoot`. With respectCollapse,
   subtrees under a collapsed row are skipped. Returns indices written. */
int ProcTree_Flatten(const ProcTreeInfo *tree, int count, int firstRoot,
                     BOOL respectCollapse, int *out);

ProcCollapseSet *ProcCollapse_Create(void);
void ProcCollapse_Destroy(ProcCollapseSet *set);
BOOL ProcCollapse_Contains(const ProcCollapseSet *set, DWORD pid, ULONGLONG createTime);
/* Returns the new collapsed state. On allocation failure returns FALSE. */
BOOL ProcCollapse_Toggle(ProcCollapseSet *set, DWORD pid, ULONGLONG createTime);
/* Drops entries whose process is no longer in `rows`. */
void ProcCollapse_Prune(ProcCollapseSet *set, const ProcRow *rows, int count);

/* ---- held order ---------------------------------------------------------
   Where each process was last displayed, so the list can stop reordering
   under the pointer. Keyed by pid and createTime; a reused pid is unseen. */
#define PROC_RANK_UNKNOWN 0x7fffffff

typedef struct { DWORD pid; ULONGLONG createTime; int rank; } ProcRank;
typedef struct { ProcRank *ranks; int count; } ProcHeldOrder;

/* Records rows[order[i]] at rank i, or rows[i] when order is NULL,
   replacing what was held. FALSE, leaving it empty, on no rows or no memory. */
BOOL ProcOrder_Capture(ProcHeldOrder *held, const ProcRow *rows, const int *order, int count);
int  ProcOrder_Rank(const ProcHeldOrder *held, DWORD pid, ULONGLONG createTime);
void ProcOrder_Free(ProcHeldOrder *held);

#endif /* CTM_PROC_TREE_H */
