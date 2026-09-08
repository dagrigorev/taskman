/* Exercises the pure tree transform with synthetic rows. No UI stubs are
   needed: proc_tree.c calls nothing from user32 or comctl32. */
#include "../include/proc_tree.h"
#include <stdio.h>
#include "../src/tabs/proc_tree.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

/* Build a row with just the fields the tree cares about. */
static ProcRow MakeRow(DWORD pid, DWORD parentPid, ULONGLONG createTime,
                       float cpu, ULONGLONG mem, BOOL memKnown)
{
    ProcRow row;
    ZeroMemory(&row, sizeof(row));
    row.pid          = pid;
    row.parentPid    = parentPid;
    row.createTime   = createTime;
    row.cpuPct       = cpu;
    row.privateBytes = mem;
    row.memoryKnown  = memKnown;
    return row;
}

static void TestLink(void)
{
    ProcRow rows[4];
    ProcTreeInfo tree[4];
    int root = -1;

    /* 10 -> 11 -> 12, plus unrelated root 20. */
    rows[0] = MakeRow(10, 0,  100, 0, 0, TRUE);
    rows[1] = MakeRow(11, 10, 200, 0, 0, TRUE);
    rows[2] = MakeRow(12, 11, 300, 0, 0, TRUE);
    rows[3] = MakeRow(20, 0,  400, 0, 0, TRUE);
    ProcTree_Link(rows, tree, 4, &root);

    CHECK(tree[0].parent == -1 && tree[0].depth == 0);
    CHECK(tree[1].parent ==  0 && tree[1].depth == 1);
    CHECK(tree[2].parent ==  1 && tree[2].depth == 2);
    CHECK(tree[3].parent == -1 && tree[3].depth == 0);
    CHECK(tree[0].childCount == 1);
    CHECK(tree[1].childCount == 1);
    CHECK(tree[2].childCount == 0);
    CHECK(tree[0].firstChild == 1);
    CHECK(tree[1].firstChild == 2);
}

/* A recycled PID must never adopt a process older than itself. */
static void TestLinkRejectsPidReuse(void)
{
    ProcRow rows[2];
    ProcTreeInfo tree[2];
    int root = -1;

    rows[0] = MakeRow(500, 0,   900, 0, 0, TRUE);  /* created LATE  */
    rows[1] = MakeRow(600, 500, 100, 0, 0, TRUE);  /* created EARLY */
    ProcTree_Link(rows, tree, 2, &root);

    CHECK(tree[1].parent == -1);        /* orphaned, not misparented */
    CHECK(tree[0].childCount == 0);
}

static void TestLinkMissingParent(void)
{
    ProcRow rows[1];
    ProcTreeInfo tree[1];
    int root = -1;

    rows[0] = MakeRow(42, 9999, 100, 0, 0, TRUE);  /* 9999 not present */
    ProcTree_Link(rows, tree, 1, &root);

    CHECK(tree[0].parent == -1 && tree[0].depth == 0);
}

/* Equal createTime is the only way a cycle can survive the time rule. */
static void TestLinkCycleTerminates(void)
{
    ProcRow rows[2];
    ProcTreeInfo tree[2];
    int root = -1;

    rows[0] = MakeRow(1, 2, 500, 0, 0, TRUE);
    rows[1] = MakeRow(2, 1, 500, 0, 0, TRUE);
    ProcTree_Link(rows, tree, 2, &root);   /* must return, not hang */

    CHECK(tree[0].depth < PROC_DEPTH_MAX);
    CHECK(tree[1].depth < PROC_DEPTH_MAX);
    CHECK(tree[0].parent == -1 || tree[1].parent == -1);
}

/* The cycle must be broken INSIDE itself, not at whatever node the walk
   started from. X hangs off the cycle A <-> B: X is innocent and must keep
   its parent, while one of A/B is orphaned to break the loop. A bounded
   step count cannot tell those apart and detaches X too. */
static void TestLinkBreaksCycleNotBystander(void)
{
    ProcRow rows[3];
    ProcTreeInfo tree[3];
    int root = -1;

    rows[0] = MakeRow(10, 20, 500, 0, 0, TRUE);   /* A, parented to B */
    rows[1] = MakeRow(20, 10, 500, 0, 0, TRUE);   /* B, parented to A */
    rows[2] = MakeRow(30, 10, 600, 0, 0, TRUE);   /* X, hangs off A    */
    ProcTree_Link(rows, tree, 3, &root);          /* must return, not hang */

    CHECK(tree[2].parent == 0);                   /* X keeps its parent A */
    CHECK(tree[0].parent == -1 || tree[1].parent == -1);  /* loop severed */
    CHECK(tree[2].depth == tree[0].depth + 1);    /* and stays beneath A  */
}

static void TestAggregate(void)
{
    ProcRow rows[3];
    ProcTreeInfo tree[3];
    int root = -1;

    rows[0] = MakeRow(10, 0,  100, 1.0f, 100, TRUE);
    rows[1] = MakeRow(11, 10, 200, 2.0f, 200, TRUE);
    rows[2] = MakeRow(12, 10, 300, 4.0f,   0, FALSE);  /* memory unknown */
    ProcTree_Link(rows, tree, 3, &root);
    ProcTree_Aggregate(rows, tree, 3);

    CHECK(tree[0].cpuRollup > 6.9f && tree[0].cpuRollup < 7.1f);
    CHECK(tree[1].cpuRollup > 1.9f && tree[1].cpuRollup < 2.1f);
    CHECK(tree[0].memRollup == 300);      /* unknown child contributes 0 */
    CHECK(tree[0].memRollupKnown);
    CHECK(!tree[2].memRollupKnown);
}

/* A parent with no known memory anywhere in its subtree stays unknown. */
static void TestAggregateAllUnknown(void)
{
    ProcRow rows[2];
    ProcTreeInfo tree[2];
    int root = -1;

    rows[0] = MakeRow(10, 0,  100, 0, 0, FALSE);
    rows[1] = MakeRow(11, 10, 200, 0, 0, FALSE);
    ProcTree_Link(rows, tree, 2, &root);
    ProcTree_Aggregate(rows, tree, 2);

    CHECK(!tree[0].memRollupKnown);
    CHECK(tree[0].memRollup == 0);
}

static void TestContextRows(void)
{
    ProcRow rows[4];
    ProcTreeInfo tree[4];
    int root = -1;
    BOOL matches[4];
    int count;

    /* 10 -> 11 -> 12, and unrelated 20. Only 12 matches. */
    rows[0] = MakeRow(10, 0,  100, 1.0f, 0, FALSE);
    rows[1] = MakeRow(11, 10, 200, 2.0f, 0, FALSE);
    rows[2] = MakeRow(12, 11, 300, 4.0f, 0, FALSE);
    rows[3] = MakeRow(20, 0,  400, 0.0f, 0, FALSE);
    matches[0] = matches[1] = FALSE;
    matches[2] = TRUE;
    matches[3] = FALSE;

    ProcTree_Link(rows, tree, 4, &root);

    /* Aggregation must fold across two levels: the grandchild's cpu should
       reach the root, not just the immediate child's. */
    ProcTree_Aggregate(rows, tree, 4);
    CHECK(tree[0].cpuRollup > 6.9f && tree[0].cpuRollup < 7.1f);  /* 1+2+4 */
    CHECK(tree[1].cpuRollup > 5.9f && tree[1].cpuRollup < 6.1f);  /* 2+4 */

    count = ProcTree_ApplyContext(rows, tree, 4, matches, &root);

    CHECK(count == 3);                       /* 20 dropped entirely */
    CHECK(rows[0].pid == 10 && tree[0].context);
    CHECK(rows[1].pid == 11 && tree[1].context);
    CHECK(rows[2].pid == 12 && !tree[2].context);
    CHECK(tree[2].depth == 2);               /* hierarchy survives */
}

/* Dropping a row must re-depth everything beneath it. */
static void TestContextRedepths(void)
{
    ProcRow rows[3];
    ProcTreeInfo tree[3];
    int root = -1;
    BOOL matches[3];
    int count;

    rows[0] = MakeRow(10, 0,  100, 0, 0, TRUE);
    rows[1] = MakeRow(11, 10, 200, 0, 0, TRUE);
    rows[2] = MakeRow(12, 99, 300, 0, 0, TRUE);   /* orphan root */
    matches[0] = FALSE; matches[1] = TRUE; matches[2] = TRUE;

    ProcTree_Link(rows, tree, 3, &root);
    count = ProcTree_ApplyContext(rows, tree, 3, matches, &root);

    CHECK(count == 3);
    CHECK(tree[2].parent == -1 && tree[2].depth == 0);
}

/* Descending CPU, using the rollup when collapsed - the real comparator. */
static int CmpCpuDesc(const ProcRow *ra, const ProcTreeInfo *ta,
                      const ProcRow *rb, const ProcTreeInfo *tb)
{
    float a = ta->collapsed ? ta->cpuRollup : ra->cpuPct;
    float b = tb->collapsed ? tb->cpuRollup : rb->cpuPct;
    if (a < b) return 1;
    if (a > b) return -1;
    return (ra->pid > rb->pid) - (ra->pid < rb->pid);
}

static void TestSortAndFlatten(void)
{
    ProcRow rows[4];
    ProcTreeInfo tree[4];
    int root = -1, visible[4], n;

    /* root 10 with children 11 (1%), 12 (5%), 13 (3%) */
    rows[0] = MakeRow(10, 0,  100, 0.0f, 0, TRUE);
    rows[1] = MakeRow(11, 10, 200, 1.0f, 0, TRUE);
    rows[2] = MakeRow(12, 10, 300, 5.0f, 0, TRUE);
    rows[3] = MakeRow(13, 10, 400, 3.0f, 0, TRUE);
    ProcTree_Link(rows, tree, 4, &root);
    ProcTree_Aggregate(rows, tree, 4);
    ProcTree_Sort(rows, tree, 4, CmpCpuDesc, &root);

    n = ProcTree_Flatten(tree, 4, root, TRUE, visible);
    CHECK(n == 4);
    CHECK(rows[visible[0]].pid == 10);   /* parent first */
    CHECK(rows[visible[1]].pid == 12);   /* 5% */
    CHECK(rows[visible[2]].pid == 13);   /* 3% */
    CHECK(rows[visible[3]].pid == 11);   /* 1% */
}

/* Two roots: sorting must update *firstRoot itself, not just internal
   sibling links. With one root only (the existing tests), a regression
   that leaves *firstRoot stale would pass silently. */
static void TestSortUpdatesFirstRoot(void)
{
    ProcRow rows[2];
    ProcTreeInfo tree[2];
    int root = -1, visible[2], n;

    /* root 0: pid 10, 1.0% cpu; root 1: pid 20, 5.0% cpu.
       Link puts 10 first (lower index); descending-cpu sort must flip it
       so 20 becomes *firstRoot. */
    rows[0] = MakeRow(10, 0, 100, 1.0f, 0, TRUE);
    rows[1] = MakeRow(20, 0, 200, 5.0f, 0, TRUE);
    ProcTree_Link(rows, tree, 2, &root);
    CHECK(rows[root].pid == 10);             /* pre-sort: link order */

    ProcTree_Aggregate(rows, tree, 2);
    ProcTree_Sort(rows, tree, 2, CmpCpuDesc, &root);

    CHECK(root >= 0 && rows[root].pid == 20);  /* firstRoot updated */

    n = ProcTree_Flatten(tree, 2, root, TRUE, visible);
    CHECK(n == 2);
    CHECK(rows[visible[0]].pid == 20);   /* higher cpu first */
    CHECK(rows[visible[1]].pid == 10);
}

static void TestFlattenSkipsCollapsed(void)
{
    ProcRow rows[3];
    ProcTreeInfo tree[3];
    int root = -1, visible[3], all[3], n;

    rows[0] = MakeRow(10, 0,  100, 0, 0, TRUE);
    rows[1] = MakeRow(11, 10, 200, 0, 0, TRUE);
    rows[2] = MakeRow(12, 10, 300, 0, 0, TRUE);
    ProcTree_Link(rows, tree, 3, &root);
    tree[0].collapsed = TRUE;

    n = ProcTree_Flatten(tree, 3, root, TRUE, visible);
    CHECK(n == 1 && rows[visible[0]].pid == 10);

    /* Ignoring collapse must still yield every row: this is what export
       and the summary line walk. */
    n = ProcTree_Flatten(tree, 3, root, FALSE, all);
    CHECK(n == 3);
}

static void TestCollapseSet(void)
{
    ProcCollapseSet *set = ProcCollapse_Create();
    ProcRow rows[1];
    CHECK(set != NULL);
    if (!set) return;

    CHECK(!ProcCollapse_Contains(set, 10, 100));
    CHECK(ProcCollapse_Toggle(set, 10, 100));          /* now collapsed  */
    CHECK(ProcCollapse_Contains(set, 10, 100));
    CHECK(!ProcCollapse_Toggle(set, 10, 100));         /* back to open   */
    CHECK(!ProcCollapse_Contains(set, 10, 100));

    /* Same PID, different createTime is a different process. */
    ProcCollapse_Toggle(set, 10, 100);
    CHECK(!ProcCollapse_Contains(set, 10, 999));

    /* Pruning drops entries whose process is gone. */
    rows[0] = MakeRow(77, 0, 500, 0, 0, TRUE);
    ProcCollapse_Prune(set, rows, 1);
    CHECK(!ProcCollapse_Contains(set, 10, 100));

    ProcCollapse_Destroy(set);
}

static void TestAggregateFoldsGpuLikeCpu(void)
{
    /* A three-level chain: grandparent 0 -> parent 1 -> children 2 and 3.
       GPU folds exactly as CPU does, so the same shape is asserted for
       both and a fold that touches one but not the other fails here. */
    ProcRow rows[4];
    ProcTreeInfo tree[4];
    int firstRoot = -1;

    ZeroMemory(rows, sizeof(rows));
    ZeroMemory(tree, sizeof(tree));
    rows[0].pid = 100; rows[0].parentPid = 0;
    rows[1].pid = 200; rows[1].parentPid = 100;
    rows[2].pid = 300; rows[2].parentPid = 200;
    rows[3].pid = 400; rows[3].parentPid = 200;
    rows[0].cpuPct = 1.0f; rows[0].gpuPct = 2.0f;  rows[0].gpuKnown = TRUE;
    rows[1].cpuPct = 2.0f; rows[1].gpuPct = 4.0f;  rows[1].gpuKnown = TRUE;
    rows[2].cpuPct = 4.0f; rows[2].gpuPct = 8.0f;  rows[2].gpuKnown = TRUE;
    rows[3].cpuPct = 8.0f; rows[3].gpuPct = 16.0f; rows[3].gpuKnown = TRUE;

    ProcTree_Link(rows, tree, 4, &firstRoot);
    ProcTree_Aggregate(rows, tree, 4);

    /* Leaves are their own value. */
    CHECK(tree[2].gpuRollup == 8.0f);
    CHECK(tree[3].gpuRollup == 16.0f);
    /* The parent carries both children and itself. */
    CHECK(tree[1].gpuRollup == 28.0f);
    CHECK(tree[1].cpuRollup == 14.0f);
    /* And the whole chain reaches the root. */
    CHECK(tree[0].gpuRollup == 30.0f);
    CHECK(tree[0].cpuRollup == 15.0f);
}

static void TestAggregateTreatsUnknownGpuAsZero(void)
{
    /* A row whose GPU is unknown contributes nothing rather than adding a
       stale value, and does not make its parent's rollup unknown -- unlike
       memory, GPU has no rollup-known flag, because a process the query
       never saw is using no measurable GPU. */
    ProcRow rows[2];
    ProcTreeInfo tree[2];
    int firstRoot = -1;

    ZeroMemory(rows, sizeof(rows));
    ZeroMemory(tree, sizeof(tree));
    rows[0].pid = 100; rows[0].parentPid = 0;
    rows[1].pid = 200; rows[1].parentPid = 100;
    rows[0].gpuPct = 3.0f;  rows[0].gpuKnown = TRUE;
    rows[1].gpuPct = 99.0f; rows[1].gpuKnown = FALSE;   /* stale, unusable */

    ProcTree_Link(rows, tree, 2, &firstRoot);
    ProcTree_Aggregate(rows, tree, 2);

    CHECK(tree[1].gpuRollup == 0.0f);
    CHECK(tree[0].gpuRollup == 3.0f);
}

int main(void)
{
    CHECK(sizeof(ProcTreeInfo) > 0);
    ProcRow r = MakeRow(1, 0, 0, 0.0f, 0, FALSE);
    CHECK(r.pid == 1);
    TestLink();
    TestLinkRejectsPidReuse();
    TestLinkMissingParent();
    TestLinkCycleTerminates();
    TestLinkBreaksCycleNotBystander();
    TestAggregate();
    TestAggregateAllUnknown();
    TestContextRows();
    TestContextRedepths();
    TestSortAndFlatten();
    TestSortUpdatesFirstRoot();
    TestFlattenSkipsCollapsed();
    TestCollapseSet();
    TestAggregateFoldsGpuLikeCpu();
    TestAggregateTreatsUnknownGpuAsZero();
    printf("proctree: %d failures\n", failures);
    return failures ? 1 : 0;
}
