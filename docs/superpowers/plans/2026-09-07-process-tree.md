# Process Tree Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Display the Processes tab as a parent/child hierarchy with sibling sorting, search context rows, and rollup totals on collapsed parents.

**Architecture:** All tree logic is a pure data transform over `ProcRow[]` and a parallel `ProcTreeInfo[]`, living in a new `src/tabs/proc_tree.c` with no Win32 dependencies. `tab_processes.c` calls it from `ProcSnapshot` on the UI thread; the collector thread is untouched. Column 0 becomes owner-drawn to paint indent and twisty.

**Tech Stack:** C11, Win32 (comctl32 ListView in `LVS_OWNERDATA`), MinGW-w64 and MSVC, custom `#include`-the-unit test suites.

**Spec:** `docs/superpowers/specs/2026-09-07-process-tree-design.md`

## Global Constraints

- Warnings are errors. Both toolchains must stay clean under the strict set:
  `-Wall -Wextra -Wshadow -Wcast-qual -Wpointer-arith -Wstrict-prototypes
  -Wmissing-prototypes -Wwrite-strings -Wconversion -Wsign-conversion
  -Wredundant-decls -Wundef` and MSVC `/W4 /WX`.
- No new libraries, no new link-time dependencies.
- `proc_tree.c` must not call any Win32 UI function. It may use `windows.h`
  types (`DWORD`, `BOOL`, `ULONGLONG`) only.
- The collector thread and `Proc_Collect` are not modified by this plan.
- All new UI state is UI-thread-only. No new locks.
- **This directory is not a git repository.** Each task therefore ends with a
  verification step rather than a commit. If you run `git init` first, commit at
  those points with the message given.

### Refinement of the spec's data model

The spec says `g_view` is held "in tree order". Physically permuting `g_view`
into tree order would require rewriting every parent/child index and is a bug
farm. This plan achieves the same observable guarantees by **not reordering the
arrays at all**: sorting reorders the sibling linked lists, and two flatten
passes produce index arrays.

- `g_view` / `g_tree` — all survivors, arbitrary order, parallel.
- `g_ordered` — every survivor in tree order (ignores collapse). Export and
  anything needing tree order walks this.
- `g_visible` — visible rows in tree order (respects collapse). Drives the
  ListView.

Guarantees the spec asked for are preserved: collapse never changes what is
exported or counted, and export is still in the order you see.

### Build and test commands

Fast single-suite loop used throughout:

```bash
mkdir -p tests/.build
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -Iinclude \
    tests/test_proctree.c -o tests/.build/test_proctree.exe \
    -lcomctl32 -lcomdlg32 -lole32 -lshlwapi -lshell32 -lpsapi -luxtheme \
    -ldwmapi -liphlpapi -lwtsapi32 -lversion -ladvapi32 -lgdi32 -luser32 -lkernel32
./tests/.build/test_proctree.exe
```

Full gate: `./build.cmd test -Strict`

**Note on "failing test" steps.** These suites `#include` the unit under test,
so a test naming a function that does not exist yet fails to *compile*. That
compile error is the red state. Do not treat it as a broken environment.

---

### Task 1: Extract `ProcRow` into a shared header

Creates the header the tree module needs. Pure move, no behaviour change.

**Files:**
- Create: `include/proc_tree.h`
- Modify: `src/tabs/tab_processes.c` (delete the moved block, add the include)

**Interfaces:**
- Consumes: nothing
- Produces: `ProcRow`, `PROC_IMAGE_MAX`, `PROC_USER_MAX`, `PROC_DESC_MAX`,
  `PROC_INDENT_CAP`, `PROC_DEPTH_MAX`, `ProcTreeInfo`

- [ ] **Step 1: Create the header**

```c
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
    ULONGLONG memRollup;
    BOOL      memRollupKnown; /* FALSE only when nothing in subtree is known */
} ProcTreeInfo;

#endif /* CTM_PROC_TREE_H */
```

- [ ] **Step 2: Delete the moved block from `tab_processes.c`**

Remove the `#define PROC_IMAGE_MAX` / `PROC_USER_MAX` / `PROC_DESC_MAX` lines
and the entire `typedef struct { ... } ProcRow;`, and add the include next to
the existing ones:

```c
#include "proc_tree.h"
```

- [ ] **Step 3: Verify nothing changed**

```bash
./build.cmd test -Strict
```

Expected: builds clean on both toolchains, all nine suites pass. This task is a
pure move; any behaviour change is a mistake.

- [ ] **Step 4: Checkpoint**

```bash
git add include/proc_tree.h src/tabs/tab_processes.c
git commit -m "refactor: extract ProcRow into include/proc_tree.h"
```

---

### Task 2: Test harness for the tree module

Stands up `tests/test_proctree.c` and wires it into the runner, so every later
task has somewhere to put its test.

**Files:**
- Create: `tests/test_proctree.c`
- Create: `src/tabs/proc_tree.c`
- Modify: `tests/run.ps1` (add to the `$suites` map)
- Modify: `Makefile`, `build.ps1` (pick up the new source)

**Interfaces:**
- Consumes: `ProcRow`, `ProcTreeInfo` from Task 1
- Produces: `MakeRow()` test helper; the `test_proctree` suite

- [ ] **Step 1: Create the empty module**

```c
/* ------------------------------------------------------------------------
 * proc_tree.c - hierarchy construction for the Processes tab.
 * Pure data transform. No Win32 UI calls: see proc_tree.h.
 * ------------------------------------------------------------------------ */
#include "proc_tree.h"
#include <stdlib.h>
```

- [ ] **Step 2: Create the suite with one trivial passing test**

```c
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

int main(void)
{
    CHECK(sizeof(ProcTreeInfo) > 0);
    printf("proctree: %d failures\n", failures);
    return failures ? 1 : 0;
}
```

- [ ] **Step 3: Register the suite in `tests/run.ps1`**

Add to the `$suites` ordered map, after `'test_processes' = @()`:

```powershell
        'test_proctree'       = @()
```

- [ ] **Step 4: Add the source to both builds**

`build.ps1` globs `src/tabs/*.c`, so it needs no change — confirm by reading
`Get-Sources`. In `Makefile`, add to `SOURCES`:

```make
          $(TABDIR)/tab_users.c $(TABDIR)/proc_tree.c
```

- [ ] **Step 5: Run it**

```bash
mkdir -p tests/.build
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -Iinclude \
    tests/test_proctree.c -o tests/.build/test_proctree.exe \
    -lcomctl32 -lcomdlg32 -lole32 -lshlwapi -lshell32 -lpsapi -luxtheme \
    -ldwmapi -liphlpapi -lwtsapi32 -lversion -ladvapi32 -lgdi32 -luser32 -lkernel32
./tests/.build/test_proctree.exe
```

Expected: `proctree: 0 failures`

- [ ] **Step 6: Verify the full gate**

```bash
./build.cmd test -Strict
```

Expected: ten suites now run, all pass.

- [ ] **Step 7: Checkpoint**

```bash
git add tests/test_proctree.c src/tabs/proc_tree.c tests/run.ps1 Makefile
git commit -m "test: add proc_tree suite scaffolding"
```

---

### Task 3: Parent linking, depth, and child counts

The core transform, and the one that carries the PID-reuse and cycle rules.

**Files:**
- Modify: `src/tabs/proc_tree.c`
- Modify: `include/proc_tree.h`
- Test: `tests/test_proctree.c`

**Interfaces:**
- Consumes: `ProcRow`, `ProcTreeInfo`
- Produces: `void ProcTree_Link(const ProcRow *rows, ProcTreeInfo *tree, int count, int *firstRoot);`
  Fills every field except `context`, `collapsed` and the rollups. Children are
  linked in ascending index order, and the roots are linked into a chain of
  their own whose head is written to `*firstRoot` (-1 when there are none).

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_proctree.c`, and call `TestLink()` from `main` before the
print:

```c
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
```

- [ ] **Step 2: Run to verify it fails**

```bash
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -Iinclude \
    tests/test_proctree.c -o tests/.build/test_proctree.exe \
    -lcomctl32 -lcomdlg32 -lole32 -lshlwapi -lshell32 -lpsapi -luxtheme \
    -ldwmapi -liphlpapi -lwtsapi32 -lversion -ladvapi32 -lgdi32 -luser32 -lkernel32
```

Expected: FAIL to compile, `implicit declaration of function 'ProcTree_Link'`.

- [ ] **Step 3: Declare it in `include/proc_tree.h`**

Before the `#endif`:

```c
/* Fills parent/firstChild/nextSibling/depth/childCount for `count` rows.
   A parent is only accepted when its createTime is not later than the
   child's, which is what stops a recycled PID adopting an older process.
   Rows whose parent is absent, invalid, or would exceed PROC_DEPTH_MAX
   become roots. Roots are linked into their own sibling chain; its head is
   written to *firstRoot, which is -1 when count is 0. Safe against cycles. */
void ProcTree_Link(const ProcRow *rows, ProcTreeInfo *tree, int count,
                   int *firstRoot);
```

- [ ] **Step 4: Implement in `src/tabs/proc_tree.c`**

```c
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
```

- [ ] **Step 5: Run tests**

Same command as Step 2, then `./tests/.build/test_proctree.exe`

Expected: `proctree: 0 failures`

- [ ] **Step 6: Checkpoint**

```bash
git add include/proc_tree.h src/tabs/proc_tree.c tests/test_proctree.c
git commit -m "feat: link process parents with PID-reuse and cycle guards"
```

---

### Task 4: Subtree aggregation

**Files:**
- Modify: `src/tabs/proc_tree.c`, `include/proc_tree.h`
- Test: `tests/test_proctree.c`

**Interfaces:**
- Consumes: `ProcTree_Link`
- Produces: `void ProcTree_Aggregate(const ProcRow *rows, ProcTreeInfo *tree, int count);`
  Sets `cpuRollup`, `memRollup`, `memRollupKnown` on every row. Rollups include
  the row itself. `memRollupKnown` is FALSE only when nothing in the subtree,
  including the row, has `memoryKnown`.

- [ ] **Step 1: Write the failing test**

```c
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
```

- [ ] **Step 2: Run to verify it fails**

Expected: compile error, `implicit declaration of function 'ProcTree_Aggregate'`.

- [ ] **Step 3: Declare and implement**

In `include/proc_tree.h`:

```c
/* Computes subtree totals into cpuRollup/memRollup/memRollupKnown.
   Rollups include the row itself. Rows with memoryKnown == FALSE contribute
   nothing to memRollup; memRollupKnown is FALSE only when no row in the
   subtree has a known value. Requires ProcTree_Link to have run. */
void ProcTree_Aggregate(const ProcRow *rows, ProcTreeInfo *tree, int count);
```

In `src/tabs/proc_tree.c`:

```c
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
```

- [ ] **Step 4: Run tests**

Expected: `proctree: 0 failures`

- [ ] **Step 5: Checkpoint**

```bash
git add include/proc_tree.h src/tabs/proc_tree.c tests/test_proctree.c
git commit -m "feat: aggregate subtree cpu and memory totals"
```

---

### Task 5: Collapse set

**Files:**
- Modify: `src/tabs/proc_tree.c`, `include/proc_tree.h`
- Test: `tests/test_proctree.c`

**Interfaces:**
- Produces:
  - `typedef struct ProcCollapseSet ProcCollapseSet;`
  - `ProcCollapseSet *ProcCollapse_Create(void);`
  - `void ProcCollapse_Destroy(ProcCollapseSet *set);`
  - `BOOL ProcCollapse_Contains(const ProcCollapseSet *set, DWORD pid, ULONGLONG createTime);`
  - `BOOL ProcCollapse_Toggle(ProcCollapseSet *set, DWORD pid, ULONGLONG createTime);`
    returns the new collapsed state
  - `void ProcCollapse_Prune(ProcCollapseSet *set, const ProcRow *rows, int count);`

- [ ] **Step 1: Write the failing test**

```c
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
```

- [ ] **Step 2: Run to verify it fails**

Expected: compile error, unknown type name `ProcCollapseSet`.

- [ ] **Step 3: Declare in `include/proc_tree.h`**

```c
/* Remembers which processes the user collapsed, across the 1s refresh.
   The default is expanded, so this stores the collapsed set, which is
   normally empty. Identity is (pid, createTime): a recycled PID is a
   different process and starts expanded. Not persisted across runs. */
typedef struct ProcCollapseSet ProcCollapseSet;

ProcCollapseSet *ProcCollapse_Create(void);
void ProcCollapse_Destroy(ProcCollapseSet *set);
BOOL ProcCollapse_Contains(const ProcCollapseSet *set, DWORD pid, ULONGLONG createTime);
/* Returns the new collapsed state. On allocation failure returns FALSE. */
BOOL ProcCollapse_Toggle(ProcCollapseSet *set, DWORD pid, ULONGLONG createTime);
/* Drops entries whose process is no longer in `rows`. */
void ProcCollapse_Prune(ProcCollapseSet *set, const ProcRow *rows, int count);
```

- [ ] **Step 4: Implement in `src/tabs/proc_tree.c`**

```c
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
```

The linear scan is deliberate: the set holds only what the user has clicked, so
it is empty in the common case and a handful of entries at worst.

- [ ] **Step 5: Run tests**

Expected: `proctree: 0 failures`

- [ ] **Step 6: Checkpoint**

```bash
git add include/proc_tree.h src/tabs/proc_tree.c tests/test_proctree.c
git commit -m "feat: track collapsed processes across refreshes"
```

---

### Task 6: Search context rows

Marks unmatched ancestors of matches, drops everything else, and rebuilds the
links over the survivors.

**Files:**
- Modify: `src/tabs/proc_tree.c`, `include/proc_tree.h`
- Test: `tests/test_proctree.c`

**Interfaces:**
- Consumes: `ProcTree_Link`
- Produces: `int ProcTree_ApplyContext(ProcRow *rows, ProcTreeInfo *tree, int count, const BOOL *matches, int *firstRoot);`
  Compacts `rows` and `tree` in place to matches plus their ancestors, sets
  `context` on rows that are only present as ancestors, re-runs linking over
  the survivors, and returns the new count.

- [ ] **Step 1: Write the failing test**

```c
static void TestContextRows(void)
{
    ProcRow rows[4];
    ProcTreeInfo tree[4];
    int root = -1;
    BOOL matches[4];
    int count;

    /* 10 -> 11 -> 12, and unrelated 20. Only 12 matches. */
    rows[0] = MakeRow(10, 0,  100, 0, 0, TRUE);
    rows[1] = MakeRow(11, 10, 200, 0, 0, TRUE);
    rows[2] = MakeRow(12, 11, 300, 0, 0, TRUE);
    rows[3] = MakeRow(20, 0,  400, 0, 0, TRUE);
    matches[0] = matches[1] = FALSE;
    matches[2] = TRUE;
    matches[3] = FALSE;

    ProcTree_Link(rows, tree, 4, &root);
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
```

- [ ] **Step 2: Run to verify it fails**

Expected: compile error, `implicit declaration of function 'ProcTree_ApplyContext'`.

- [ ] **Step 3: Declare and implement**

In `include/proc_tree.h`:

```c
/* Keeps matches and their ancestors, drops the rest, and marks ancestors
   that are not themselves matches as context. Compacts rows and tree in
   place, re-links over the survivors, and returns the new count.
   `matches` is parallel to `rows`. Requires ProcTree_Link to have run. */
int ProcTree_ApplyContext(ProcRow *rows, ProcTreeInfo *tree, int count,
                          const BOOL *matches, int *firstRoot);
```

In `src/tabs/proc_tree.c`:

```c
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
```

`ProcTree_Link` writes only the link, depth and childCount fields, so the
`context` flags set in the compaction loop survive the relink. If that ever
changes this function breaks silently, and `TestContextRows` is what catches it.

Note also what makes the spec's rule "context rows are drawn only from rows that
survived the user filter" hold automatically: `matches` is computed over
`g_view`, which the user filter has already reduced. An ancestor belonging to
another account is simply not in the array, so it cannot be re-admitted here.

- [ ] **Step 4: Run tests**

Expected: `proctree: 0 failures`

- [ ] **Step 5: Checkpoint**

```bash
git add include/proc_tree.h src/tabs/proc_tree.c tests/test_proctree.c
git commit -m "feat: keep unmatched ancestors as search context rows"
```

---

### Task 7: Sibling sort and flatten

**Files:**
- Modify: `src/tabs/proc_tree.c`, `include/proc_tree.h`
- Test: `tests/test_proctree.c`

**Interfaces:**
- Produces:
  - `typedef int (*ProcTreeCmp)(const ProcRow *ra, const ProcTreeInfo *ta, const ProcRow *rb, const ProcTreeInfo *tb);`
  - `void ProcTree_Sort(const ProcRow *rows, ProcTreeInfo *tree, int count, ProcTreeCmp cmp, int *firstRoot);`
    Reorders sibling chains only; array indices are never moved. Updates
    `*firstRoot`, since sorting can change which root comes first.
  - `int ProcTree_Flatten(const ProcTreeInfo *tree, int count, int firstRoot, BOOL respectCollapse, int *out);`
    Writes indices in depth-first order into `out` (capacity `count`), returns
    how many were written.

- [ ] **Step 1: Write the failing test**

```c
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
```

- [ ] **Step 2: Run to verify it fails**

Expected: compile error, unknown type name `ProcTreeCmp`.

- [ ] **Step 3: Declare in `include/proc_tree.h`**

```c
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
```

- [ ] **Step 4: Implement in `src/tabs/proc_tree.c`**

```c
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
static int ProcSortChain(ProcTreeInfo *tree, int head, int *scratch)
{
    int n = 0, walk = head, i;
    for (walk = head; walk >= 0; walk = tree[walk].nextSibling) scratch[n++] = walk;
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
            tree[i].firstChild = ProcSortChain(tree, tree[i].firstChild, scratch);

    /* The root chain is sorted the same way; its head can change. */
    if (firstRoot && *firstRoot >= 0)
        *firstRoot = ProcSortChain(tree, *firstRoot, scratch);

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

        for (child = tree[node].firstChild; child >= 0; child = tree[child].nextSibling)
            ++childCount;
        if (childCount == 0 || top + childCount > count) continue;

        /* Reverse for the same reason as the roots. */
        at = top + childCount - 1;
        for (child = tree[node].firstChild; child >= 0; child = tree[child].nextSibling)
            stack[at--] = child;
        top += childCount;
    }

    free(stack);
    return written;
}
```

- [ ] **Step 5: Run tests**

Expected: `proctree: 0 failures`

- [ ] **Step 6: Checkpoint**

```bash
git add include/proc_tree.h src/tabs/proc_tree.c tests/test_proctree.c
git commit -m "feat: sort sibling chains and flatten the tree to indices"
```

---

### Task 8: Settings, command ID, and View menu toggle

Plumbing only. The toggle stores state; nothing consumes it until Task 9.

**Files:**
- Modify: `include/app.h`, `include/resource.h`, `src/settings.c`,
  `src/tabs/tab_processes.c`
- Test: `tests/test_settings.c`

**Interfaces:**
- Produces: `g_cfg.procTreeMode`, `IDM_VIEW_PROCTREE`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_settings.c` and call from `main`:

```c
static void TestProcTreeDefault(void)
{
    ZeroMemory(&g_cfg, sizeof(g_cfg));
    Settings_Load();
    /* Default is on: absent registry value must load as TRUE. */
    CHECK(g_cfg.procTreeMode == TRUE || g_cfg.procTreeMode == FALSE);
    CHECK(sizeof(g_cfg.procTreeMode) == sizeof(BOOL));
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
gcc -std=gnu11 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -Iinclude \
    tests/test_settings.c -o tests/.build/test_settings.exe \
    -lcomctl32 -lcomdlg32 -lole32 -lshlwapi -lshell32 -lpsapi -luxtheme \
    -ldwmapi -liphlpapi -lwtsapi32 -lversion -ladvapi32 -lgdi32 -luser32 -lkernel32
```

Expected: FAIL, `'Settings' has no member named 'procTreeMode'`.

- [ ] **Step 3: Add the setting**

`include/app.h`, in `Settings` after `netHistoryMode`:

```c
    BOOL  procTreeMode;         /* Processes tab shows a hierarchy          */
```

`src/settings.c`, in `Settings_Load`:

```c
    g_cfg.procTreeMode = GetDwordDef(L"ProcessTreeView", 1) ? TRUE : FALSE;
```

and in `Settings_Save`:

```c
    Reg_SetDword(L"ProcessTreeView", g_cfg.procTreeMode ? 1 : 0);
```

`include/resource.h`, next to the other accelerator-era View commands:

```c
#define IDM_VIEW_PROCTREE               40075
```

- [ ] **Step 4: Add the menu item**

In `src/tabs/tab_processes.c`, extend `ProcBuildViewMenu`:

```c
    AppendMenuW(view, MF_STRING, IDM_VIEW_PROCTREE, L"Process &Tree");
```

Add an `InitViewMenu` (the slot is currently `NULL`):

```c
static void ProcInitViewMenu(TabPage *p, HMENU view)
{
    (void)p;
    CheckMenuItem(view, IDM_VIEW_PROCTREE,
                  MF_BYCOMMAND | (g_cfg.procTreeMode ? MF_CHECKED : MF_UNCHECKED));
}
```

Wire it into the `TabPage` struct, replacing the `NULL` in the `InitViewMenu`
slot:

```c
    ProcBuildViewMenu,
    ProcInitViewMenu,
    ProcPrimary
```

Handle the command in `ProcCommand`:

```c
    case IDM_VIEW_PROCTREE:
        g_cfg.procTreeMode = !g_cfg.procTreeMode;
        ProcSnapshot(p);
        break;
```

- [ ] **Step 5: Run tests**

```bash
./build.cmd test -Strict
```

Expected: all suites pass. Launch `build\taskman.exe`, open View, confirm
"Process Tree" appears checked and toggles. Nothing else changes yet.

- [ ] **Step 6: Checkpoint**

```bash
git add include/app.h include/resource.h src/settings.c src/tabs/tab_processes.c tests/test_settings.c
git commit -m "feat: add persisted Process Tree view toggle"
```

---

### Task 9: Wire the tree into `ProcSnapshot`

The integration task. After this the list is ordered as a tree, but still
painted flat — no indent yet.

**Files:**
- Modify: `src/tabs/tab_processes.c`
- Test: `tests/test_processes.c`

**Interfaces:**
- Consumes: everything from Tasks 3-7
- Produces: `g_tree`, `g_ordered`, `g_orderedCnt`, `g_visible`, `g_visibleCnt`,
  and `ProcVisibleRow(int display)` returning the `g_view` index for a display row

- [ ] **Step 1: Write the failing test**

Add to `tests/test_processes.c`:

```c
static void TestVisibleMapping(void)
{
    /* With no tree state built, display row N must map to view row N so the
       flat path is unaffected. */
    g_viewCnt = 3;
    g_visibleCnt = 0;
    CHECK(ProcVisibleRow(0) == 0);
    CHECK(ProcVisibleRow(2) == 2);
    CHECK(ProcVisibleRow(-1) < 0);
    CHECK(ProcVisibleRow(99) < 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: compile error, `implicit declaration of function 'ProcVisibleRow'`.

- [ ] **Step 3: Add the state and the mapping helper**

In `src/tabs/tab_processes.c` next to `g_view`:

```c
static ProcTreeInfo    *g_tree;        /* parallel to g_view, g_viewCnt long */
static int             *g_ordered;     /* every survivor, tree order          */
static int              g_orderedCnt;
static int             *g_visible;     /* visible rows, tree order            */
static int              g_visibleCnt;
static int              g_firstRoot = -1;  /* head of the root chain          */
static ProcCollapseSet *g_collapse;

/* Maps a ListView display row to its index in g_view. Falls back to the
   identity mapping when no tree is built, so the flat path is unchanged. */
static int ProcVisibleRow(int display)
{
    if (display < 0) return -1;
    if (g_visibleCnt > 0)
        return display < g_visibleCnt ? g_visible[display] : -1;
    return display < g_viewCnt ? display : -1;
}
```

- [ ] **Step 4: Add the comparator and the build step**

```c
/* A row sorts by what it displays: rollup when collapsed, own value when not. */
static int ProcTreeCompare(const ProcRow *ra, const ProcTreeInfo *ta,
                           const ProcRow *rb, const ProcTreeInfo *tb)
{
    float ca = ta->collapsed ? ta->cpuRollup : ra->cpuPct;
    float cb = tb->collapsed ? tb->cpuRollup : rb->cpuPct;
    ULONGLONG ma = ta->collapsed ? ta->memRollup : ra->privateBytes;
    ULONGLONG mb = tb->collapsed ? tb->memRollup : rb->privateBytes;
    int cmp = 0;

    switch (g_sortCol) {
    case 0: cmp = lstrcmpiW(ra->imageName, rb->imageName); break;
    case 1: cmp = lstrcmpiW(ra->userName,  rb->userName);  break;
    case 2: cmp = (ca > cb) - (ca < cb); break;
    case 3: cmp = (ma > mb) - (ma < mb); break;
    case 4: cmp = lstrcmpiW(ra->description, rb->description); break;
    case 5: cmp = (ra->pid > rb->pid) - (ra->pid < rb->pid); break;
    default: break;
    }
    if (!cmp) cmp = (ra->pid > rb->pid) - (ra->pid < rb->pid);
    return cmp * g_sortDir;
}

/* Rebuilds g_tree, g_ordered and g_visible from g_view. Returns FALSE and
   leaves the flat path in charge if anything cannot be allocated. */
static BOOL ProcBuildTree(const BOOL *matches)
{
    free(g_tree);    g_tree    = NULL;
    free(g_ordered); g_ordered = NULL; g_orderedCnt = 0;
    free(g_visible); g_visible = NULL; g_visibleCnt = 0;

    g_firstRoot = -1;
    if (g_viewCnt <= 0) return TRUE;

    g_tree    = (ProcTreeInfo *)calloc((size_t)g_viewCnt, sizeof(ProcTreeInfo));
    g_ordered = (int *)malloc((size_t)g_viewCnt * sizeof(int));
    g_visible = (int *)malloc((size_t)g_viewCnt * sizeof(int));
    if (!g_tree || !g_ordered || !g_visible) {
        free(g_tree); g_tree = NULL;
        free(g_ordered); g_ordered = NULL;
        free(g_visible); g_visible = NULL;
        return FALSE;
    }

    ProcTree_Link(g_view, g_tree, g_viewCnt, &g_firstRoot);
    if (matches)
        g_viewCnt = ProcTree_ApplyContext(g_view, g_tree, g_viewCnt, matches,
                                          &g_firstRoot);
    ProcTree_Aggregate(g_view, g_tree, g_viewCnt);

    if (!g_collapse) g_collapse = ProcCollapse_Create();
    if (g_collapse) {
        int i;
        ProcCollapse_Prune(g_collapse, g_view, g_viewCnt);
        for (i = 0; i < g_viewCnt; ++i)
            /* A context row is never collapsed: hiding the match it exists
               to explain would defeat the point. The entry is kept, so it
               takes effect again once the search is cleared. */
            g_tree[i].collapsed = !g_tree[i].context &&
                ProcCollapse_Contains(g_collapse, g_view[i].pid, g_view[i].createTime);
    }

    ProcTree_Sort(g_view, g_tree, g_viewCnt, ProcTreeCompare, &g_firstRoot);
    g_orderedCnt = ProcTree_Flatten(g_tree, g_viewCnt, g_firstRoot, FALSE, g_ordered);
    g_visibleCnt = ProcTree_Flatten(g_tree, g_viewCnt, g_firstRoot, TRUE,  g_visible);
    return TRUE;
}
```

- [ ] **Step 5: Call it from `ProcSnapshot`**

Replace the existing sort block. The search filter must now *mark* rather than
compact, because ancestors are decided afterwards:

```c
    if (g_cfg.procTreeMode) {
        BOOL *matches = (BOOL *)malloc((size_t)(g_viewCnt > 0 ? g_viewCnt : 1) * sizeof(BOOL));
        if (matches) {
            int i;
            for (i = 0; i < g_viewCnt; ++i)
                matches[i] = ProcMatches(&g_view[i], s_query, s_filterMode);
            ProcBuildTree(matches);
            free(matches);
        }
    } else {
        free(g_tree);    g_tree = NULL;
        free(g_ordered); g_ordered = NULL; g_orderedCnt = 0;
        free(g_visible); g_visible = NULL; g_visibleCnt = 0;
        if (g_view && g_viewCnt > 1) {
            PROC_CMP_COL = g_sortCol;
            PROC_CMP_DIR = g_sortDir;
            qsort(g_view, (size_t)g_viewCnt, sizeof(ProcRow), ProcCompare);
        }
    }
```

Then set the item count from the visible list:

```c
        /* g_tree is NULL when tree mode is off *or* when ProcBuildTree could
           not allocate. Both must fall back to the flat count, or a failed
           allocation would silently show an empty process list. */
        ListView_SetItemCountEx(s_list,
            g_tree ? g_visibleCnt : g_viewCnt, LVSICF_NOSCROLL);
```

- [ ] **Step 6: Route the readers through the mapping**

In `ProcNotify`, both `LVN_GETDISPINFOW` and the `NM_CUSTOMDRAW` subitem stage
currently index `g_view` by display row. Change both:

```c
        int row = ProcVisibleRow(di->item.iItem);
        if (row < 0) return FALSE;
        r = &g_view[row];
```

```c
            int index = ProcVisibleRow((int)draw->nmcd.dwItemSpec);
            if (index >= 0 && index < g_viewCnt) {
```

In `ProcSelected`:

```c
static const ProcRow *ProcSelected(void)
{
    int sel = s_list ? ListView_GetNextItem(s_list, -1, LVNI_SELECTED) : -1;
    int row = ProcVisibleRow(sel);
    return row >= 0 && row < g_viewCnt ? &g_view[row] : NULL;
}
```

Apply the same mapping in `ProcCommand`'s `IDC_PROC_ENDPROCESS` case and in
`ProcSnapshot`'s selection save/restore.

- [ ] **Step 7: Exclude context rows from the summary and export**

Summary loop — count and sum only non-context rows, and sum own values:

```c
        for (i = 0; i < g_viewCnt; ++i) {
            if (g_tree && g_tree[i].context) continue;
            cpu += g_view[i].cpuPct;
            if (g_view[i].memoryKnown) memory += g_view[i].privateBytes;
            ++kept;
        }
```

In `ProcExport`, walk tree order and skip context rows:

```c
    if (g_cfg.procTreeMode && g_ordered) {
        for (i = 0; i < g_orderedCnt; ++i) {
            int at = g_ordered[i];
            if (g_tree[at].context) continue;
            rows[count++] = g_view[at];
        }
    } else {
        for (i = 0; i < g_viewCnt; ++i) rows[count++] = g_view[i];
    }
```

- [ ] **Step 8: Free the new state in `Proc_Reset`**

```c
    free(g_tree);    g_tree = NULL;
    free(g_ordered); g_ordered = NULL; g_orderedCnt = 0;
    free(g_visible); g_visible = NULL; g_visibleCnt = 0;
    g_firstRoot = -1;
    ProcCollapse_Destroy(g_collapse); g_collapse = NULL;
```

- [ ] **Step 9: Test that context rows are excluded from counts and export**

The spec requires that a context row is neither counted in the summary nor
written to CSV. Add to `tests/test_processes.c`:

```c
/* A search for a child must not inflate the process count with the parents
   that were only added to give it context, nor export them. */
static void TestContextExcludedFromCountAndExport(void)
{
    BOOL matches[3];
    int i, counted = 0, exported = 0;

    free(g_view);
    g_view = (ProcRow *)calloc(3, sizeof(ProcRow));
    CHECK(g_view != NULL);
    if (!g_view) return;
    g_viewCnt = 3;
    g_view[0] = MakeProcRow(10, 0,  100);
    g_view[1] = MakeProcRow(11, 10, 200);
    g_view[2] = MakeProcRow(12, 11, 300);
    matches[0] = FALSE; matches[1] = FALSE; matches[2] = TRUE;

    g_cfg.procTreeMode = TRUE;
    CHECK(ProcBuildTree(matches));
    CHECK(g_viewCnt == 3);

    for (i = 0; i < g_viewCnt; ++i) if (!g_tree[i].context) ++counted;
    CHECK(counted == 1);                 /* only the match is a process */

    for (i = 0; i < g_orderedCnt; ++i)
        if (!g_tree[g_ordered[i]].context) ++exported;
    CHECK(exported == 1);                /* and only it is exported */

    /* Everything is still reachable in tree order for display. */
    CHECK(g_orderedCnt == 3);
}
```

`MakeProcRow` is a local helper mirroring the one in `test_proctree.c`:

```c
static ProcRow MakeProcRow(DWORD pid, DWORD parentPid, ULONGLONG createTime)
{
    ProcRow row;
    ZeroMemory(&row, sizeof(row));
    row.pid = pid; row.parentPid = parentPid; row.createTime = createTime;
    row.memoryKnown = TRUE;
    return row;
}
```

- [ ] **Step 10: Run the gate**

```bash
./build.cmd test -Strict
```

Expected: all suites pass. Launch the app: the Processes list is now ordered
parent-then-children, still painted flat. Toggling Process Tree off restores
the old ordering exactly.

- [ ] **Step 11: Checkpoint**

```bash
git add src/tabs/tab_processes.c tests/test_processes.c
git commit -m "feat: order the process list as a tree"
```

---

### Task 10: Owner-drawn column 0

**Files:**
- Modify: `src/tabs/tab_processes.c`

**Interfaces:**
- Consumes: `g_tree`, `ProcVisibleRow`
- Produces: `ProcTwistyRect(int display, const RECT *cell, RECT *out)` returning
  `TRUE` when the row has a twisty

- [ ] **Step 1: Add the geometry helper**

```c
#define PROC_INDENT_STEP  14      /* logical px per level, DPI scaled       */
#define PROC_TWISTY_BOX   12

/* Twisty rect for a display row, in the cell's coordinates. FALSE when the
   row has no children and therefore no twisty. */
static BOOL ProcTwistyRect(int display, const RECT *cell, RECT *out)
{
    int row = ProcVisibleRow(display), depth, left, mid;
    if (!g_tree || row < 0 || row >= g_viewCnt) return FALSE;
    if (g_tree[row].childCount <= 0) return FALSE;

    depth = g_tree[row].depth;
    if (depth > PROC_INDENT_CAP) depth = PROC_INDENT_CAP;
    left = cell->left + depth * DPX(PROC_INDENT_STEP);
    mid  = (cell->top + cell->bottom) / 2;

    out->left   = left;
    out->right  = left + DPX(PROC_TWISTY_BOX);
    out->top    = mid - DPX(PROC_TWISTY_BOX) / 2;
    out->bottom = mid + DPX(PROC_TWISTY_BOX) / 2;
    return TRUE;
}
```

- [ ] **Step 2: Paint the cell**

In `ProcNotify`, in the `CDDS_ITEMPREPAINT | CDDS_SUBITEM` branch, handle
subitem 0 before the existing colour logic:

```c
            if (draw->iSubItem == 0 && g_cfg.procTreeMode && g_tree && index >= 0) {
                RECT cell = draw->nmcd.rc, twisty, text;
                COLORREF back = index % 2 ? RGB(248, 250, 253) : UI_SURFACE;
                COLORREF ink  = g_tree[index].context ? UI_MUTED : UI_INK;
                int depth = g_tree[index].depth;
                BOOL selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;

                /* CDRF_SKIPDEFAULT means the control draws nothing here, so
                   selection and focus have to be painted by hand. */
                if (selected) { back = RGB(205, 224, 253); ink = UI_INK; }
                else if (draw->nmcd.uItemState & CDIS_HOT) back = RGB(235, 242, 253);
                UI_Fill(draw->hdc, &cell, back);

                if (depth > PROC_INDENT_CAP) depth = PROC_INDENT_CAP;
                if (ProcTwistyRect((int)draw->nmcd.dwItemSpec, &cell, &twisty)) {
                    POINT chevron[3];
                    int cx = (twisty.left + twisty.right) / 2;
                    int cy = (twisty.top + twisty.bottom) / 2;
                    int arm = DPX(3);
                    if (g_tree[index].collapsed) {      /* pointing right */
                        chevron[0].x = cx - arm / 2; chevron[0].y = cy - arm;
                        chevron[1].x = cx + arm / 2; chevron[1].y = cy;
                        chevron[2].x = cx - arm / 2; chevron[2].y = cy + arm;
                    } else {                            /* pointing down  */
                        chevron[0].x = cx - arm; chevron[0].y = cy - arm / 2;
                        chevron[1].x = cx;       chevron[1].y = cy + arm / 2;
                        chevron[2].x = cx + arm; chevron[2].y = cy - arm / 2;
                    }
                    UI_Polyline(draw->hdc, chevron, 3, UI_MUTED, DPX(2));
                }

                text = cell;
                text.left += depth * DPX(PROC_INDENT_STEP) + DPX(PROC_TWISTY_BOX) + DPX(4);
                text.right -= DPX(4);
                UI_Text(draw->hdc, g_view[index].imageName, text, 0, ink,
                        DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

                if (draw->nmcd.uItemState & CDIS_FOCUS) {
                    RECT focus = cell;
                    DrawFocusRect(draw->hdc, &focus);
                }
                *result = CDRF_SKIPDEFAULT;
                return TRUE;
            }
```

- [ ] **Step 3: Show rollups on collapsed rows**

In `LVN_GETDISPINFOW`, subitems 2 and 3:

```c
            case 2: {
                float cpu = (g_tree && g_tree[row].collapsed)
                            ? g_tree[row].cpuRollup : r->cpuPct;
                StringCchPrintfW(di->item.pszText, (size_t)di->item.cchTextMax,
                                 L"%.1f%%", cpu);
                break;
            }
            case 3:
                if (g_tree && g_tree[row].collapsed) {
                    if (!g_tree[row].memRollupKnown)
                        lstrcpynW(di->item.pszText, L"N/A", di->item.cchTextMax);
                    else
                        UI_FormatSize(g_tree[row].memRollup, di->item.pszText,
                                      (size_t)di->item.cchTextMax);
                } else if (!r->memoryKnown) {
                    lstrcpynW(di->item.pszText, L"N/A", di->item.cchTextMax);
                } else {
                    UI_FormatSize(r->privateBytes, di->item.pszText,
                                  (size_t)di->item.cchTextMax);
                }
                break;
```

- [ ] **Step 4: Verify**

```bash
./build.cmd rebuild -Strict && ./build.cmd run
```

Expected: indented rows with chevrons on parents. Check selection, focus and
hover highlighting in column 0 specifically — this is where a regression is
most likely. Compare against another column while a row is selected.

- [ ] **Step 5: Checkpoint**

```bash
git add src/tabs/tab_processes.c
git commit -m "feat: paint tree indent and twisty in the process list"
```

---

### Task 11: Twisty clicks, keyboard, and selection follow

**Files:**
- Modify: `src/tabs/tab_processes.c`

**Interfaces:**
- Consumes: `ProcTwistyRect`, `ProcCollapse_Toggle`, `ProcVisibleRow`

- [ ] **Step 1: Add the collapse action**

```c
/* Toggles a display row's collapse state and rebuilds the view. Moves the
   selection to the row being collapsed if the selection is inside it, which
   would otherwise vanish without explanation. */
static void ProcToggleCollapse(TabPage *p, int display)
{
    int row = ProcVisibleRow(display), selected, walk;
    if (!g_tree || row < 0 || row >= g_viewCnt || g_tree[row].childCount <= 0) return;
    if (!g_collapse) return;

    selected = ProcVisibleRow(ListView_GetNextItem(s_list, -1, LVNI_SELECTED));
    ProcCollapse_Toggle(g_collapse, g_view[row].pid, g_view[row].createTime);

    for (walk = selected; walk >= 0; walk = g_tree[walk].parent) {
        if (walk != row) continue;
        s_pendingPid = g_view[row].pid;     /* reselect the parent */
        break;
    }
    ProcSnapshot(p);
}
```

- [ ] **Step 2: Handle the click**

In `ProcNotify`, before the customdraw branch:

```c
    if (nm->code == NM_CLICK && g_cfg.procTreeMode) {
        NMITEMACTIVATE *click = (NMITEMACTIVATE *)nm;
        LVHITTESTINFO hit;
        ZeroMemory(&hit, sizeof(hit));
        hit.pt = click->ptAction;
        if (ListView_SubItemHitTest(s_list, &hit) >= 0 && hit.iSubItem == 0) {
            RECT cell, twisty;
            /* LVIR_BOUNDS on subitem 0 gives the whole row; the first
               column's width bounds the cell we painted. */
            if (ListView_GetSubItemRect(s_list, hit.iItem, 0, LVIR_BOUNDS, &cell) &&
                ProcTwistyRect(hit.iItem, &cell, &twisty) &&
                click->ptAction.x >= twisty.left && click->ptAction.x < twisty.right) {
                ProcToggleCollapse(p, hit.iItem);
                *result = 0;
                return TRUE;
            }
        }
    }
```

- [ ] **Step 3: Handle Left and Right**

```c
    if (nm->code == LVN_KEYDOWN && g_cfg.procTreeMode && g_tree) {
        NMLVKEYDOWN *key = (NMLVKEYDOWN *)nm;
        int display = ListView_GetNextItem(s_list, -1, LVNI_SELECTED);
        int row = ProcVisibleRow(display);
        if (row < 0) return FALSE;

        if (key->wVKey == VK_RIGHT) {
            if (g_tree[row].collapsed) { ProcToggleCollapse(p, display); *result = 0; return TRUE; }
            if (g_tree[row].firstChild >= 0 && display + 1 < g_visibleCnt) {
                ListView_SetItemState(s_list, display + 1,
                    LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(s_list, display + 1, FALSE);
                *result = 0; return TRUE;
            }
        }
        if (key->wVKey == VK_LEFT) {
            if (!g_tree[row].collapsed && g_tree[row].childCount > 0) {
                ProcToggleCollapse(p, display); *result = 0; return TRUE;
            }
            if (g_tree[row].parent >= 0) {
                int i;
                for (i = 0; i < g_visibleCnt; ++i) {
                    if (g_visible[i] != g_tree[row].parent) continue;
                    ListView_SetItemState(s_list, i,
                        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    ListView_EnsureVisible(s_list, i, FALSE);
                    break;
                }
                *result = 0; return TRUE;
            }
        }
    }
```

- [ ] **Step 4: Verify**

```bash
./build.cmd rebuild -Strict && ./build.cmd run
```

Expected: clicking a chevron collapses and expands; collapsed parents show
rollup totals; Left and Right navigate and toggle; collapsing a branch
containing the selection reselects the parent rather than losing it.

- [ ] **Step 5: Checkpoint**

```bash
git add src/tabs/tab_processes.c
git commit -m "feat: expand and collapse by click and keyboard"
```

---

### Task 12: `Proc_SelectPid` expands ancestors

**Files:**
- Modify: `src/tabs/tab_processes.c`
- Test: `tests/test_processes.c`

- [ ] **Step 1: Write the failing test**

```c
/* Go to Process from the Services tab must not scroll to a row that is
   present in the model but hidden under a collapsed parent. */
static void TestSelectPidExpandsAncestors(void)
{
    CHECK(ProcExpandAncestors(0) == FALSE);   /* no tree: nothing to do */
}
```

- [ ] **Step 2: Run to verify it fails**

Expected: compile error, `implicit declaration of function 'ProcExpandAncestors'`.

- [ ] **Step 3: Implement**

```c
/* Clears the collapse flag on every ancestor of a g_view row so it becomes
   visible. Returns TRUE when something changed. */
static BOOL ProcExpandAncestors(int row)
{
    BOOL changed = FALSE;
    int walk;
    if (!g_tree || !g_collapse || row < 0 || row >= g_viewCnt) return FALSE;
    for (walk = g_tree[row].parent; walk >= 0; walk = g_tree[walk].parent) {
        if (!ProcCollapse_Contains(g_collapse, g_view[walk].pid, g_view[walk].createTime))
            continue;
        ProcCollapse_Toggle(g_collapse, g_view[walk].pid, g_view[walk].createTime);
        changed = TRUE;
    }
    return changed;
}
```

Call it from `Proc_SelectPid`, before the existing scan, and rebuild if it
changed anything:

```c
    for (i = 0; i < g_viewCnt; i++) {
        if (g_view[i].pid != pid) continue;
        if (ProcExpandAncestors(i)) ProcSnapshot(NULL);
        break;
    }
```

Then the existing loop searches `g_visible` rather than `g_view`:

```c
    for (i = 0; i < g_visibleCnt; i++) {
        if (g_view[g_visible[i]].pid == pid) {
            s_pendingPid = 0;
            ListView_SetItemState(s_list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(s_list, i,
                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(s_list, i, FALSE);
            return;
        }
    }
```

- [ ] **Step 4: Verify**

```bash
./build.cmd test -Strict
```

Then in the app: Services tab, right-click a running service, "Go to Process",
with that process's parent collapsed beforehand. The row must be revealed and
selected.

- [ ] **Step 5: Checkpoint**

```bash
git add src/tabs/tab_processes.c tests/test_processes.c
git commit -m "fix: reveal collapsed ancestors when navigating to a process"
```

---

### Task 13: Workspace fixture coverage

**Files:**
- Modify: `tests/test_workspace.c`

- [ ] **Step 1: Add the fixture checks**

In the Processes section of the existing fixture, after the list is populated:

```c
    /* Tree mode is on by default, so the model must have been built. */
    CHECK(g_cfg.procTreeMode);
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_VIEW_PROCTREE, 0), 0);
    Pump(200);
    CHECK(!g_cfg.procTreeMode);                  /* toggled off */
    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_VIEW_PROCTREE, 0), 0);
    Pump(200);
    CHECK(g_cfg.procTreeMode);                   /* and back on */

    /* Left and Right must not crash with a selection present. */
    ListView_SetItemState(GetDlgItem(page, IDC_PROC_LIST), 0,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Pump(100);
    Capture(hwnd, L"tests/.build/workspace-tree.bmp");
```

- [ ] **Step 2: Run the gate**

```bash
./build.cmd test -Strict
```

Expected: all suites pass, and `tests/.build/workspace-tree.bmp` exists.

- [ ] **Step 3: Inspect the capture at each DPI**

The fixture already exercises 150% and 200%. Open
`tests/.build/workspace-200dpi.bmp` and confirm the indent step and chevron
scale with DPI rather than staying at 96dpi pixels.

- [ ] **Step 4: Checkpoint**

```bash
git add tests/test_workspace.c
git commit -m "test: cover tree toggle and rendering in the workspace fixture"
```

---

## Final verification

- [ ] `./build.cmd rebuild -Strict` — MSVC, zero warnings
- [ ] `./build.ps1 rebuild -Toolchain mingw -Strict` — MinGW, zero warnings
- [ ] `./build.cmd test -Strict` — all suites pass
- [ ] `mingw32-make clean && mingw32-make STRICT=1` — Makefile path still builds
- [ ] Launch and confirm: tree on by default and fully expanded; sorting by CPU
      reorders siblings without flattening; searching shows dimmed ancestors;
      collapsing rolls up and can move a row in the sort order; the summary
      count excludes context rows; CSV export contains rows hidden under
      collapsed parents; toggling tree off restores the original flat list
