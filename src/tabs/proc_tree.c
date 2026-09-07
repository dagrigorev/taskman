/* ------------------------------------------------------------------------
 * proc_tree.c - hierarchy construction for the Processes tab.
 * Pure data transform. No Win32 UI calls: see proc_tree.h.
 * ------------------------------------------------------------------------ */
#include "proc_tree.h"
#include <stdlib.h>

/* Index sorted by pid, so parent lookup is a binary search per row. */
typedef struct { DWORD pid; int index; } ProcPidIndex;

static int ProcPidIndexCmp(const void *a, const void *b)
{
    DWORD x = ((const ProcPidIndex *)a)->pid;
    DWORD y = ((const ProcPidIndex *)b)->pid;
    return (x > y) - (x < y);
}

static int ProcFindPid(const ProcPidIndex *index, int count, DWORD pid)
{
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (index[mid].pid == pid) return index[mid].index;
        if (index[mid].pid < pid)  lo = mid + 1;
        else                       hi = mid - 1;
    }
    return -1;
}

void ProcTree_Link(const ProcRow *rows, ProcTreeInfo *tree, int count,
                   int *firstRoot)
{
    ProcPidIndex *index;
    int i, previousRoot = -1;

    if (firstRoot) *firstRoot = -1;
    for (i = 0; i < count; ++i) {
        tree[i].parent = tree[i].firstChild = tree[i].nextSibling = -1;
        tree[i].depth = tree[i].childCount = 0;
        tree[i].collapsed = FALSE;
        tree[i].cpuRollup = 0.0f;
        tree[i].memRollup = 0;
        tree[i].memRollupKnown = FALSE;
    }
    if (count <= 0) return;

    index = (ProcPidIndex *)malloc((size_t)count * sizeof(*index));
    if (!index) {                    /* every row stays a root */
        if (firstRoot) *firstRoot = 0;
        for (i = 0; i < count - 1; ++i) tree[i].nextSibling = i + 1;
        return;
    }
    for (i = 0; i < count; ++i) { index[i].pid = rows[i].pid; index[i].index = i; }
    qsort(index, (size_t)count, sizeof(*index), ProcPidIndexCmp);

    /* Resolve parents. The createTime test is what rejects PID reuse. */
    for (i = 0; i < count; ++i) {
        int parent = ProcFindPid(index, count, rows[i].parentPid);
        if (parent == i) parent = -1;
        if (parent >= 0 && rows[parent].createTime > rows[i].createTime)
            parent = -1;
        tree[i].parent = parent;
    }

    /* Break any cycle: walking up from a node must reach a root within
       PROC_DEPTH_MAX steps, otherwise that node is promoted to root. */
    for (i = 0; i < count; ++i) {
        int walk = tree[i].parent, steps = 0;
        while (walk >= 0 && steps < PROC_DEPTH_MAX) { walk = tree[walk].parent; ++steps; }
        if (steps >= PROC_DEPTH_MAX) tree[i].parent = -1;
    }

    /* Link children in ascending index order, and count them. */
    for (i = count - 1; i >= 0; --i) {
        int parent = tree[i].parent;
        if (parent < 0) continue;
        tree[i].nextSibling = tree[parent].firstChild;
        tree[parent].firstChild = i;
        tree[parent].childCount++;
    }

    /* Depth follows from the now-acyclic parent chain. */
    for (i = 0; i < count; ++i) {
        int walk = tree[i].parent, depth = 0;
        while (walk >= 0 && depth < PROC_DEPTH_MAX) { walk = tree[walk].parent; ++depth; }
        tree[i].depth = depth;
    }

    /* Roots need a chain of their own: they have no parent to hang one from,
       and ProcTree_Flatten must be able to walk them in sorted order. */
    for (i = 0; i < count; ++i) {
        if (tree[i].parent >= 0) continue;
        if (previousRoot < 0) { if (firstRoot) *firstRoot = i; }
        else                  tree[previousRoot].nextSibling = i;
        previousRoot = i;
    }

    free(index);
}

void ProcTree_Aggregate(const ProcRow *rows, ProcTreeInfo *tree, int count)
{
    int i;

    /* Seed every node with its own values. */
    for (i = 0; i < count; ++i) {
        tree[i].cpuRollup      = rows[i].cpuPct;
        tree[i].memRollup      = rows[i].memoryKnown ? rows[i].privateBytes : 0;
        tree[i].memRollupKnown = rows[i].memoryKnown;
    }

    /* Fold each node into its parent, deepest first. Sorting indices by
       descending depth gives a valid post-order without recursion, so a
       pathological chain cannot blow the stack. */
    {
        int depth;
        for (depth = PROC_DEPTH_MAX; depth > 0; --depth) {
            for (i = 0; i < count; ++i) {
                int parent = tree[i].parent;
                if (tree[i].depth != depth || parent < 0) continue;
                tree[parent].cpuRollup += tree[i].cpuRollup;
                tree[parent].memRollup += tree[i].memRollup;
                if (tree[i].memRollupKnown) tree[parent].memRollupKnown = TRUE;
            }
        }
    }
}

int ProcTree_ApplyContext(ProcRow *rows, ProcTreeInfo *tree, int count,
                          const BOOL *matches, int *firstRoot)
{
    BOOL *keep;
    int i, kept = 0;

    if (firstRoot) *firstRoot = -1;
    if (count <= 0) return 0;
    keep = (BOOL *)calloc((size_t)count, sizeof(BOOL));
    if (!keep) return count;          /* keep everything rather than lie */

    /* Every match, plus every ancestor of a match. */
    for (i = 0; i < count; ++i) {
        int walk = i, steps = 0;
        if (!matches[i]) continue;
        while (walk >= 0 && steps <= PROC_DEPTH_MAX) {
            keep[walk] = TRUE;
            walk = tree[walk].parent;
            ++steps;
        }
    }

    for (i = 0; i < count; ++i) {
        if (!keep[i]) continue;
        rows[kept] = rows[i];
        tree[kept] = tree[i];
        tree[kept].context = !matches[i];
        ++kept;
    }
    free(keep);

    /* Indices moved, so the links are stale: rebuild them. This is also
       what re-depths rows whose ancestors were dropped. */
    ProcTree_Link(rows, tree, kept, firstRoot);

    return kept;
}

/* qsort has no context parameter, so the comparison inputs are file-static.
   Charts and lists in this codebase already use this pattern. */
static const ProcRow      *s_sortRows;
static const ProcTreeInfo *s_sortTree;
static ProcTreeCmp         s_sortCmp;

static int ProcSortThunk(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    return s_sortCmp(&s_sortRows[ia], &s_sortTree[ia],
                     &s_sortRows[ib], &s_sortTree[ib]);
}

/* Sorts one sibling chain, given its head, and returns the new head. */
static int ProcSortChain(ProcTreeInfo *tree, int head, int *scratch, int count)
{
    int n = 0, walk, i;
    for (walk = head; walk >= 0 && n < count; walk = tree[walk].nextSibling) scratch[n++] = walk;
    if (n < 2) return head;
    qsort(scratch, (size_t)n, sizeof(int), ProcSortThunk);
    for (i = 0; i < n - 1; ++i) tree[scratch[i]].nextSibling = scratch[i + 1];
    tree[scratch[n - 1]].nextSibling = -1;
    return scratch[0];
}

void ProcTree_Sort(const ProcRow *rows, ProcTreeInfo *tree, int count,
                   ProcTreeCmp cmp, int *firstRoot)
{
    int *scratch, i;

    if (count <= 1 || !cmp) return;
    scratch = (int *)malloc((size_t)count * sizeof(int));
    if (!scratch) return;

    s_sortRows = rows; s_sortTree = tree; s_sortCmp = cmp;

    for (i = 0; i < count; ++i)
        if (tree[i].firstChild >= 0)
            tree[i].firstChild = ProcSortChain(tree, tree[i].firstChild, scratch, count);

    /* The root chain is sorted the same way; its head can change. */
    if (firstRoot && *firstRoot >= 0)
        *firstRoot = ProcSortChain(tree, *firstRoot, scratch, count);

    s_sortRows = NULL; s_sortTree = NULL; s_sortCmp = NULL;
    free(scratch);
}

int ProcTree_Flatten(const ProcTreeInfo *tree, int count, int firstRoot,
                     BOOL respectCollapse, int *out)
{
    int written = 0, top = 0, i, node;
    int *stack;

    if (count <= 0 || !out || firstRoot < 0) return 0;
    stack = (int *)malloc((size_t)count * sizeof(int));
    if (!stack) return 0;

    /* Push the root chain in reverse, so the first root is popped first. */
    for (node = firstRoot; node >= 0 && top < count; node = tree[node].nextSibling)
        stack[top++] = node;
    for (i = 0; i < top / 2; ++i) {
        int swap = stack[i];
        stack[i] = stack[top - 1 - i];
        stack[top - 1 - i] = swap;
    }

    while (top > 0 && written < count) {
        int childCount = 0, child, at;
        node = stack[--top];
        out[written++] = node;
        if (respectCollapse && tree[node].collapsed) continue;

        for (child = tree[node].firstChild; child >= 0 && childCount < count; child = tree[child].nextSibling)
            ++childCount;
        if (childCount == 0 || top + childCount > count) continue;

        /* Reverse for the same reason as the roots. */
        at = top + childCount - 1;
        for (child = tree[node].firstChild; child >= 0 && at >= top; child = tree[child].nextSibling)
            stack[at--] = child;
        top += childCount;
    }

    free(stack);
    return written;
}

typedef struct { DWORD pid; ULONGLONG createTime; } ProcCollapseEntry;

struct ProcCollapseSet {
    ProcCollapseEntry *items;
    int count, capacity;
};

ProcCollapseSet *ProcCollapse_Create(void)
{
    return (ProcCollapseSet *)calloc(1, sizeof(ProcCollapseSet));
}

void ProcCollapse_Destroy(ProcCollapseSet *set)
{
    if (!set) return;
    free(set->items);
    free(set);
}

static int ProcCollapseFind(const ProcCollapseSet *set, DWORD pid, ULONGLONG createTime)
{
    int i;
    if (!set) return -1;
    for (i = 0; i < set->count; ++i)
        if (set->items[i].pid == pid && set->items[i].createTime == createTime)
            return i;
    return -1;
}

BOOL ProcCollapse_Contains(const ProcCollapseSet *set, DWORD pid, ULONGLONG createTime)
{
    return ProcCollapseFind(set, pid, createTime) >= 0;
}

BOOL ProcCollapse_Toggle(ProcCollapseSet *set, DWORD pid, ULONGLONG createTime)
{
    int at;
    if (!set) return FALSE;
    at = ProcCollapseFind(set, pid, createTime);
    if (at >= 0) {
        set->items[at] = set->items[--set->count];   /* order is irrelevant */
        return FALSE;
    }
    if (set->count == set->capacity) {
        int grown = set->capacity ? set->capacity * 2 : 16;
        ProcCollapseEntry *items =
            (ProcCollapseEntry *)realloc(set->items, (size_t)grown * sizeof(*items));
        if (!items) return FALSE;
        set->items = items;
        set->capacity = grown;
    }
    set->items[set->count].pid        = pid;
    set->items[set->count].createTime = createTime;
    set->count++;
    return TRUE;
}

void ProcCollapse_Prune(ProcCollapseSet *set, const ProcRow *rows, int count)
{
    int i, kept = 0;
    if (!set) return;
    for (i = 0; i < set->count; ++i) {
        int j;
        for (j = 0; j < count; ++j)
            if (rows[j].pid == set->items[i].pid &&
                rows[j].createTime == set->items[i].createTime) break;
        if (j < count) set->items[kept++] = set->items[i];
    }
    set->count = kept;
}
