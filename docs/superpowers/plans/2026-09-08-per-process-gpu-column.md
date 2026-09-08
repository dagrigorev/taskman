# Per-Process GPU Column Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a GPU column to the Processes tab, off by default, that folds into the process tree exactly as CPU does and turns GPU collection on only while it is visible.

**Architecture:** `Gpu_ProcessUsage(pid, double *)` already exists from unit 1 and publishes a pid-keyed array on the collector thread ahead of `Proc_Collect`, so acquisition is done — this unit is plumbing plus one real design problem. That problem is ownership of the GPU enable flag: unit 2 made the Sensors tab its sole owner, and a second owner that can be on while the tab is off means a single `BOOL` no longer expresses the state. The flag becomes a small bitmask with one bit per owner, so collection runs while *any* owner wants it.

**Tech Stack:** Win32 C11, comctl32 owner-data list view, the existing `ProcRow`/`ProcTreeInfo` transform in `src/tabs/proc_tree.c`. CMake 3.25 + Ninja + CTest, MSVC `/W4 /WX` and MinGW `-Wall -Wextra -Werror`.

**Spec:** `docs/superpowers/specs/2026-09-07-gpu-and-temperatures-design.md` — this plan implements **Unit 4** of four, the last. Units 1–3 are merged.

## Global Constraints

- **No third-party libraries and no runtime dependencies beyond what ships with Windows.**
- **C11, MSVC's default C mode.** No `_Static_assert`; use the negative-array-bound typedef idiom from `src/ui.c` if a compile-time assertion is wanted.
- **Both toolchains must build clean at `-Werror` / `/WX`.** Run both on every task.
- **All 14 existing CTest suites must stay green.** `test_workspace` creates real windows and fails intermittently for environmental reasons — re-run once before treating a failure as real, and run it from the build tree, not the repo root, or its `Capture` calls fail on a missing `tests/.build`.
- **Commit messages carry no `Co-Authored-By` line and no AI attribution trailer of any kind.** Subject and body only.
- **The column is off by default.** Enabling it is what makes the collector pay the PDH enumeration over roughly a thousand engine instances.

## What the survey established

Read before planning; two of these change what the work is.

| Fact | Where | Consequence |
|---|---|---|
| "Select Columns" is **width-based visibility**, not a persisted mask | `ProcColumns`, `src/tabs/tab_processes.c:1389-1404` | "Off by default" means created with width 0. There is no settings key to add, and no persistence to write — the column returns to its default on every launch, like every other column. |
| `ProcColumns` keeps its own `names[]` of six strings, loops `i < 6`, and validates `chosen < 6` | same function | **The same drift hazard as `UI_DrawNavigation` in unit 2.** Adding a seventh column without touching all three would leave it unlistable, and a mismatched `names[]` would mislabel the menu. Fixed here by reading the headers from the list view, as unit 2 did for the tab strip. |
| `Gpu_SetEnabled(BOOL)` has exactly one production caller | `src/tabs/tab_sensors.c:307` (`SensActivate`) | A second owner cannot share a plain `BOOL`: leaving the Sensors tab would call `Gpu_SetEnabled(FALSE)` and silently kill the column's data. This is the carry-forward the unit 2 code comment warned about. |
| `Gpu_ProcessUsage(DWORD pid, double *out)` returns 0/1 and leaves `*out` untouched on a miss | `src/gpu.c` | Acquisition needs no new code. A miss must render as unavailable, not as 0 %. |
| `ProcTree_Aggregate` seeds every node then folds by descending depth | `src/tabs/proc_tree.c` | `gpuRollup` folds in the identical loop; the pattern to copy is `cpuRollup`, not `memRollup`, since GPU is always known once the column is on. |
| Sub-item shading reads `collapsed ? cpuRollup : cpuPct` | `src/tabs/tab_processes.c:1289-1299` | The GPU column's display and sort must make the same collapsed/expanded choice, or a collapsed parent shows rolled-up CPU beside raw GPU. |

## Decisions locked for this unit

1. **The enable flag becomes an owner bitmask.** `Gpu_SetEnabled(unsigned owner, BOOL)` with `GPU_OWNER_SENSORS` and `GPU_OWNER_PROCESS_COLUMN`. Collection runs while any bit is set. The alternative — letting the Processes tab re-assert the flag on its next snapshot — is self-healing but racy, and would drop a sample every time the user leaves the Sensors tab. Changing the signature forces every call site to say which owner it is, which is the point.
2. **`gpuKnown` is per row, not global.** A pid with no GPU instances is genuinely at 0 %, but a pid the query never saw is unknown. `Gpu_ProcessUsage` distinguishes them, so the column does too: unknown renders as an empty cell, not `0.0%`.
3. **Sorting on the GPU column is by the displayed value.** Collapsed rows sort by rollup, expanded by their own figure, matching what `proc_compare_tree` already does for CPU at `src/tabs/tab_processes.c:530`.
4. **No CSV change.** The CSV export is a fixed set of columns with a test asserting its shape; widening it is not in this unit's scope and the spec does not ask for it.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `include/gpu.h` | Modify | Owner constants; `Gpu_SetEnabled` signature |
| `src/gpu.c` | Modify | Bitmask instead of a boolean |
| `tests/test_gpu.c` | Modify | Two owners, independently |
| `src/tabs/tab_sensors.c` | Modify (`SensActivate`, ~line 307) | Pass its own owner bit |
| `include/proc_tree.h` | Modify | `ProcRow::gpuPct`/`gpuKnown`, `ProcTreeInfo::gpuRollup` |
| `src/tabs/proc_tree.c` | Modify (`ProcTree_Aggregate`) | Fold `gpuRollup` |
| `tests/test_proctree.c` | Modify | Rollup folds like CPU |
| `src/tabs/tab_processes.c` | Modify | Column, stamp, display, sort, shading, menu fix, gating |
| `tests/test_processes.c` | Modify | Stamping leaves unknown rows unknown |
| `tests/test_workspace.c` | Modify | Column toggles, and toggling gates collection |

---

### Task 1: Let two owners enable GPU collection

Do this first: until the flag can express two owners, the column cannot be wired without breaking the Sensors tab.

**Files:**
- Modify: `include/gpu.h:148`, `src/gpu.c` (`Gpu_SetEnabled`/`Gpu_IsEnabled`)
- Modify: `src/tabs/tab_sensors.c:305-308`
- Test: `tests/test_gpu.c` (`TestEnabledFlag`, `TestCollectSkippedWhenDisabled`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `#define GPU_OWNER_SENSORS 0x1u`, `#define GPU_OWNER_PROCESS_COLUMN 0x2u`, `void Gpu_SetEnabled(unsigned owner, BOOL enabled);`. `BOOL Gpu_IsEnabled(void)` keeps its signature and means "any owner". Task 4 calls `Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, ...)`.

- [ ] **Step 1: Write the failing test**

Replace `TestEnabledFlag` in `tests/test_gpu.c`:

```c
static void TestEnabledFlag(void)
{
    /* Two independent owners. Collection runs while either wants it, so
       leaving the Sensors tab must not switch off a process column that
       is still visible -- and vice versa. */
    CHECK(Gpu_IsEnabled() == FALSE);

    Gpu_SetEnabled(GPU_OWNER_SENSORS, TRUE);
    CHECK(Gpu_IsEnabled() == TRUE);
    Gpu_SetEnabled(GPU_OWNER_SENSORS, FALSE);
    CHECK(Gpu_IsEnabled() == FALSE);

    /* Both on, then one off: still on. This is the case a single BOOL
       got wrong. */
    Gpu_SetEnabled(GPU_OWNER_SENSORS, TRUE);
    Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, TRUE);
    CHECK(Gpu_IsEnabled() == TRUE);
    Gpu_SetEnabled(GPU_OWNER_SENSORS, FALSE);
    CHECK(Gpu_IsEnabled() == TRUE);
    Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, FALSE);
    CHECK(Gpu_IsEnabled() == FALSE);

    /* Clearing an owner that was never set is not an error, and clearing
       one owner twice does not clear the other. */
    Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, TRUE);
    Gpu_SetEnabled(GPU_OWNER_SENSORS, FALSE);
    Gpu_SetEnabled(GPU_OWNER_SENSORS, FALSE);
    CHECK(Gpu_IsEnabled() == TRUE);
    Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, FALSE);
    CHECK(Gpu_IsEnabled() == FALSE);
}
```

In `TestCollectSkippedWhenDisabled`, change the single call to `Gpu_SetEnabled(GPU_OWNER_SENSORS, FALSE);`.

- [ ] **Step 2: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: compile FAIL — `GPU_OWNER_SENSORS` undeclared, and too many arguments to `Gpu_SetEnabled`.

- [ ] **Step 3: Implement the bitmask**

In `include/gpu.h`, replace the `Gpu_SetEnabled` declaration:

```c
/* GPU collection is wanted by more than one part of the UI, and they turn
   it on and off independently: the Sensors tab while it is active, the
   Processes tab while its GPU column is visible. A single flag would let
   whichever switched off last cancel the other, so each owner gets a bit
   and collection runs while any bit is set. */
#define GPU_OWNER_SENSORS        0x1u
#define GPU_OWNER_PROCESS_COLUMN 0x2u

void Gpu_SetEnabled(unsigned owner, BOOL enabled);
/* TRUE while any owner wants collection. */
BOOL Gpu_IsEnabled(void);
```

In `src/gpu.c`:

```c
void Gpu_SetEnabled(unsigned owner, BOOL enabled)
{
    LONG previous, updated;
    /* Compare-and-swap rather than a plain exchange: the owners are both
       on the UI thread today, but the flag is read from the collector
       thread, and a read-modify-write of a shared word must not be torn. */
    do {
        previous = InterlockedCompareExchange(&s_enabled, 0, 0);
        updated = enabled ? (previous | (LONG)owner) : (previous & ~(LONG)owner);
    } while (InterlockedCompareExchange(&s_enabled, updated, previous) != previous);
}

BOOL Gpu_IsEnabled(void)
{
    return InterlockedCompareExchange(&s_enabled, 0, 0) != 0;
}
```

`Gpu_Reset` must also clear it, so a restarted collector does not inherit a stale owner. Add to `Gpu_Reset`, beside `s_lastCollect = 0;`:

```c
    InterlockedExchange(&s_enabled, 0);
```

- [ ] **Step 4: Update the Sensors tab's call**

In `src/tabs/tab_sensors.c`, `SensActivate` becomes:

```c
/* Collection is gated on this tab so that a user who never opens it never
   pays for the PDH query -- around a thousand engine instances on a
   machine with three adapters. The Processes tab's GPU column owns its own
   bit, so leaving this tab no longer switches collection off underneath
   it. */
static void SensActivate(TabPage *p, BOOL active)
{
    (void)p;
    Gpu_SetEnabled(GPU_OWNER_SENSORS, active);
}
```

- [ ] **Step 5: Run both toolchains**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain msvc test
```

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: 14/14 PASS on both. `test_workspace`'s existing unit 2 assertions still hold — the Sensors tab is the only owner so far, so its on/off behaviour is unchanged.

- [ ] **Step 6: Commit**

```bash
git add include/gpu.h src/gpu.c src/tabs/tab_sensors.c tests/test_gpu.c
git commit -m "refactor: let several owners enable GPU collection

The Sensors tab was the only thing that could switch collection on, so a
single flag was enough. A second owner that can be active while the tab is
not means whichever switched off last would cancel the other, so each owner
now holds a bit and collection runs while any bit is set."
```

---

### Task 2: Roll GPU up the process tree

The tree transform is pure and headless, so this lands before any UI touches it.

**Files:**
- Modify: `include/proc_tree.h` (`ProcRow`, `ProcTreeInfo`)
- Modify: `src/tabs/proc_tree.c` (`ProcTree_Aggregate`)
- Test: `tests/test_proctree.c`

**Interfaces:**
- Consumes: `ProcTree_Link` (unchanged).
- Produces: `ProcRow::gpuPct` (`float`) and `ProcRow::gpuKnown` (`BOOL`); `ProcTreeInfo::gpuRollup` (`float`). Tasks 3 and 4 read all three.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_proctree.c`, matching the file's existing fixture style, and call it from `main`:

```c
static void TestAggregateFoldsGpuLikeCpu(void)
{
    /* A three-level chain: grandparent 0 -> parent 1 -> child 2, plus a
       second child 3 of the parent. GPU folds exactly as CPU does, so the
       same shape is asserted for both and a fold that touches one but not
       the other fails here. */
    ProcRow rows[4];
    ProcTreeInfo tree[4];
    int firstRoot = -1;

    ZeroMemory(rows, sizeof(rows));
    ZeroMemory(tree, sizeof(tree));
    rows[0].pid = 100; rows[0].parentPid = 0;
    rows[1].pid = 200; rows[1].parentPid = 100;
    rows[2].pid = 300; rows[2].parentPid = 200;
    rows[3].pid = 400; rows[3].parentPid = 200;
    rows[0].cpuPct = 1.0f; rows[0].gpuPct = 2.0f; rows[0].gpuKnown = TRUE;
    rows[1].cpuPct = 2.0f; rows[1].gpuPct = 4.0f; rows[1].gpuKnown = TRUE;
    rows[2].cpuPct = 4.0f; rows[2].gpuPct = 8.0f; rows[2].gpuKnown = TRUE;
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
    /* A row whose GPU is unknown contributes nothing rather than adding
       an uninitialised value, and does not make its parent's rollup
       unknown -- unlike memory, GPU has no "known" rollup flag, because
       an unseen process really is using no measurable GPU. */
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
```

- [ ] **Step 2: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: compile FAIL — `ProcRow` has no member `gpuPct`.

- [ ] **Step 3: Add the fields**

In `include/proc_tree.h`, `ProcRow` gains two members after `cpuPct`:

```c
    float     cpuPct;
    float     gpuPct;         /* summed across engines and adapters       */
    BOOL      gpuKnown;       /* FALSE when the query never saw this pid  */
```

and `ProcTreeInfo` gains one after `cpuRollup`:

```c
    float     cpuRollup;      /* subtree total, includes this row           */
    float     gpuRollup;      /* subtree total, includes this row           */
```

Update `ProcTree_Aggregate`'s contract comment in the header:

```c
/* Computes subtree totals into cpuRollup/gpuRollup/memRollup/memRollupKnown.
   Rollups include the row itself. Rows with memoryKnown == FALSE contribute
   nothing to memRollup; memRollupKnown is FALSE only when no row in the
   subtree has a known value. Rows with gpuKnown == FALSE contribute nothing
   to gpuRollup and there is no corresponding known flag: a process the GPU
   query never saw is using no measurable GPU, which is a real zero rather
   than an absence. Requires ProcTree_Link to have run. */
```

**`ProcTree_Link` clears the fields individually, not with a `ZeroMemory`** — confirmed by reading it. So `gpuRollup` must be added to its reset loop, beside `cpuRollup`:

```c
        tree[i].cpuRollup = 0.0f;
        tree[i].gpuRollup = 0.0f;
        tree[i].memRollup = 0;
```

Omitting this is not cosmetic: an uninitialised rollup was a real nondeterministic `-O2` test failure in the process-tree unit, and would be again.

- [ ] **Step 4: Fold it**

In `src/tabs/proc_tree.c`, `ProcTree_Aggregate` gains one line in the seed loop and one in the fold loop:

```c
    for (i = 0; i < count; ++i) {
        tree[i].cpuRollup      = rows[i].cpuPct;
        tree[i].gpuRollup      = rows[i].gpuKnown ? rows[i].gpuPct : 0.0f;
        tree[i].memRollup      = rows[i].memoryKnown ? rows[i].privateBytes : 0;
        tree[i].memRollupKnown = rows[i].memoryKnown;
        if (tree[i].depth > maxDepth) maxDepth = tree[i].depth;
    }
```

```c
            tree[parent].cpuRollup += tree[i].cpuRollup;
            tree[parent].gpuRollup += tree[i].gpuRollup;
            tree[parent].memRollup += tree[i].memRollup;
```

- [ ] **Step 5: Run both toolchains**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain msvc test
```

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: 14/14 PASS on both.

- [ ] **Step 6: Commit**

```bash
git add include/proc_tree.h src/tabs/proc_tree.c tests/test_proctree.c
git commit -m "feat: roll per-process GPU up the tree

Folded in the same pass as CPU, so a collapsed parent cannot show a
rolled-up CPU figure beside a raw GPU one. Unlike memory there is no
rollup-known flag: a process the GPU query never saw is using no
measurable GPU, which is a real zero rather than an absence."
```

---

### Task 3: Stamp the figure onto each row

**Files:**
- Modify: `src/tabs/tab_processes.c` (`Proc_Collect`'s sampling loop, near `row->cpuPct = ProcCpuDelta(...)` at line 408)
- Test: `tests/test_processes.c`

**Interfaces:**
- Consumes: `Gpu_ProcessUsage(DWORD pid, double *out)` from `include/gpu.h`; `Gpu_IsEnabled()`.
- Produces: rows whose `gpuPct`/`gpuKnown` are filled. Task 4 displays them.

- [ ] **Step 1: Write the failing test**

Add to `tests/test_processes.c`, in the file's existing style, and call it from `main`:

```c
static void TestGpuStampLeavesUnknownRowsUnknown(void)
{
    /* With collection off, nothing has published per-process figures, so
       every row must read unknown rather than a confident zero -- that is
       what makes the column render an empty cell instead of "0.0%". */
    ProcRow row;
    ZeroMemory(&row, sizeof(row));
    row.pid = 0xFFFFFFFEu;      /* cannot be a live pid */
    row.gpuPct = 42.0f;         /* stale value from a previous sample */
    row.gpuKnown = TRUE;

    Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, FALSE);
    Gpu_Reset();
    ProcStampGpu(&row);

    CHECK(row.gpuKnown == FALSE);
    CHECK(row.gpuPct == 0.0f);  /* cleared, not left stale */
}
```

`test_processes.c` `#include`s `tab_processes.c`, so the file-static helper is reachable. Its CMake entry is currently `taskman_add_test(test_processes ../src/tabs/proc_tree.c)` — **no `gpu.c`**, so this task must add it:

```cmake
taskman_add_test(test_processes ../src/tabs/proc_tree.c ../src/gpu.c)
```

This is the same linkage class that bit units 1, 2 and 3; here it is confirmed in advance rather than discovered at link time.

- [ ] **Step 2: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: compile FAIL — `ProcStampGpu` undeclared.

- [ ] **Step 3: Implement the stamp**

In `src/tabs/tab_processes.c`, add `#include "gpu.h"` if it is not already present, and a helper above `Proc_Collect`:

```c
/* Per-process GPU is published by the collector thread before this tab's
   collector runs, so the figure is already current. A pid the query never
   saw is unknown rather than zero: the column shows an empty cell for it,
   which is honest about a process whose usage was not measured. The value
   is cleared alongside the flag so a stale figure from an earlier sample
   can never be displayed. */
static void ProcStampGpu(ProcRow *row)
{
    double usage = 0;
    if (Gpu_IsEnabled() && Gpu_ProcessUsage(row->pid, &usage)) {
        row->gpuPct = (float)usage;
        row->gpuKnown = TRUE;
    } else {
        row->gpuPct = 0.0f;
        row->gpuKnown = FALSE;
    }
}
```

Call it in the sampling loop, immediately after the CPU figure is computed:

```c
                row->cpuPct = ProcCpuDelta(ktm, utm, &s_prev[i], deltaSys);
                ProcStampGpu(row);
```

Read the surrounding lines first: the `row->cpuPct` assignment at line 408 sits inside a conditional branch. `ProcStampGpu` must run for **every** row, not only the branch where CPU could be computed, or processes whose CPU was unavailable would never get a GPU figure. Place the call where every row passes through it.

- [ ] **Step 4: Run both toolchains**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain msvc test
```

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: 14/14 PASS on both.

- [ ] **Step 5: Commit**

```bash
git add src/tabs/tab_processes.c tests/test_processes.c tests/CMakeLists.txt
git commit -m "feat: stamp per-process GPU usage onto each row

The collector publishes the pid-keyed figures before this tab's collector
runs, so the value is already current when it is read. A pid the query
never saw is marked unknown rather than zero, and the value is cleared
with the flag so a stale figure from an earlier sample cannot survive
into a row that is no longer measured."
```

---

### Task 4: The column

**Files:**
- Modify: `src/tabs/tab_processes.c` — column creation (~line 822), `ProcColumns` (~1389), display, sort (~515 and ~530), shading (~1289)
- Test: `tests/test_workspace.c`

**Interfaces:**
- Consumes: everything from Tasks 1–3.
- Produces: the finished feature. Nothing depends on it.

- [ ] **Step 1: Write the failing test**

In `tests/test_workspace.c`, after the existing Processes-tab assertions:

```c
    {
        /* The column ships hidden, and showing it is what turns GPU
           collection on. Toggling it must not disturb the Sensors tab's
           own ownership, which is asserted separately. */
        const int gpuColumn = 6;
        CHECK(ListView_GetColumnCount(ListView_GetHeader(list)) >= 7);
        CHECK(ListView_GetColumnWidth(list, gpuColumn) == 0);
        CHECK(!Gpu_IsEnabled());

        SendMessageW(page, WM_COMMAND, IDM_PROC_COL_FIRST + gpuColumn, 0);
        Pump(120);
        CHECK(ListView_GetColumnWidth(list, gpuColumn) > 0);
        CHECK(Gpu_IsEnabled());

        SendMessageW(page, WM_COMMAND, IDM_PROC_COL_FIRST + gpuColumn, 0);
        Pump(120);
        CHECK(ListView_GetColumnWidth(list, gpuColumn) == 0);
        CHECK(!Gpu_IsEnabled());
    }
```

`ListView_GetColumnCount` is not a standard macro; count with `Header_GetItemCount(ListView_GetHeader(list))` instead if the build rejects it.

This test drives the toggle through `WM_COMMAND` rather than through `ProcColumns`' popup menu, which would block on `TrackPopupMenu`. That means **the command must be routed**, not only reachable from the menu — see Step 4.

- [ ] **Step 2: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: FAIL on the column count — there are six columns.

- [ ] **Step 3: Create the column hidden**

In `ProcCreate`, after the PID column:

```c
        UI_AddColumn(s_list, 5, L"PID", 40, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 6, L"GPU", 42, LVCFMT_RIGHT);
        /* Hidden until asked for: showing it is what makes the collector
           enumerate roughly a thousand GPU engine counter instances. */
        ListView_SetColumnWidth(s_list, 6, 0);
```

- [ ] **Step 4: Make the toggle single-sourced and gate collection**

`ProcColumns` currently hardcodes both the names and the count. Replace it, reading the labels from the header exactly as unit 2 did for the tab strip, and routing the toggle through a helper so `WM_COMMAND` and the menu share one path:

```c
/* Toggling a column's width is how this tab shows and hides columns; there
   is no separate visibility state to keep in step. The GPU column
   additionally owns a bit of the GPU collection flag, so the cost is paid
   only while the column is on screen. */
static void ProcToggleColumn(int column)
{
    int count = Header_GetItemCount(ListView_GetHeader(s_list));
    BOOL showing;
    if (column < 1 || column >= count) return;
    showing = ListView_GetColumnWidth(s_list, column) == 0;
    ListView_SetColumnWidth(s_list, column,
                            showing ? LVSCW_AUTOSIZE_USEHEADER : 0);
    if (column == PROC_COL_GPU)
        Gpu_SetEnabled(GPU_OWNER_PROCESS_COLUMN, showing);
}

static void ProcColumns(HWND owner)
{
    /* The labels come from the header itself. A private copy of the names
       here would drift the moment a column is added or renamed -- and
       being indexed by a hardcoded count, drift would mean an out-of-range
       read rather than a wrong string. */
    HWND header = ListView_GetHeader(s_list);
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int i, chosen, count = Header_GetItemCount(header);
    if (!menu) return;
    for (i = 1; i < count; ++i) {
        WCHAR label[64] = {0};
        LVCOLUMNW column;
        ZeroMemory(&column, sizeof(column));
        column.mask = LVCF_TEXT;
        column.pszText = label;
        column.cchTextMax = ARRAYSIZE(label);
        if (!ListView_GetColumn(s_list, i, &column)) continue;
        AppendMenuW(menu, (UINT)(MF_STRING |
            (ListView_GetColumnWidth(s_list, i) ? MF_CHECKED : 0)),
            (UINT_PTR)(IDM_PROC_COL_FIRST + i), label);
    }
    GetCursorPos(&pt);
    chosen = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, owner, NULL)
             - IDM_PROC_COL_FIRST;
    DestroyMenu(menu);
    ProcToggleColumn(chosen);
}
```

Define `#define PROC_COL_GPU 6` beside the other file-local constants.

In `ProcCommand`, route the range so the fixture's `WM_COMMAND` reaches the same helper. Check whether `IDM_PROC_COL_FIRST + n` is already handled; if not, add:

```c
    if (id >= IDM_PROC_COL_FIRST && id <= IDM_PROC_COL_LAST) {
        ProcToggleColumn(id - IDM_PROC_COL_FIRST);
        return;
    }
```

`IDM_PROC_COL_LAST` (40219) already exists in `include/resource.h` and reserves twenty slots from `IDM_PROC_COL_FIRST` (40200), so use it rather than inventing a bound. `ProcToggleColumn` range-checks against the real header count anyway, so an id inside the reserved block but past the last column is ignored.

- [ ] **Step 5: Display, sort and shade it**

Display, in the owner-data handler beside the other columns — collapsed rows show the rollup, matching CPU:

```c
        case PROC_COL_GPU:
            /* An unmeasured process shows nothing rather than 0.0%, which
               would claim the GPU was sampled and found idle. */
            if (tree && tree[index].collapsed)
                StringCchPrintfW(text, cch, L"%.1f%%", tree[index].gpuRollup);
            else if (row->gpuKnown)
                StringCchPrintfW(text, cch, L"%.1f%%", row->gpuPct);
            else
                text[0] = L'\0';
            break;
```

Adapt the exact variable names to the surrounding handler — read it first; the CPU case is the model to copy.

Sort, beside the CPU cases at lines 515 and 530:

```c
    case PROC_COL_GPU: cmp = (ra->gpuPct > rb->gpuPct) ? 1 :
                             (ra->gpuPct < rb->gpuPct) ? -1 : 0; break;
```

and in `proc_compare_tree`, mirroring the collapsed choice it already makes for CPU:

```c
    if (g_sortCol == PROC_COL_GPU) {
        float ga = ta->collapsed ? ta->gpuRollup : ra->gpuPct;
        float gb = tb->collapsed ? tb->gpuRollup : rb->gpuPct;
        ...
    }
```

Follow the function's existing structure rather than pasting this verbatim — it is a chain of comparisons, not a switch.

Shading, in the custom-draw sub-item block at line 1289, following the CPU tint:

```c
                } else if (draw->iSubItem == PROC_COL_GPU) {
                    float gpu = collapsed ? g_tree[index].gpuRollup
                                          : g_view[index].gpuPct;
                    bg = gpu >= 15 ? RGB(214, 232, 255) :
                         gpu >= 1  ? RGB(235, 244, 255) : RGB(246, 249, 253);
                }
```

- [ ] **Step 6: Run both toolchains and look at it**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain msvc test
```

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Then run the fixture from the build tree, which is where its captures resolve:

```bash
cd build/msvc-release && ./bin/Release/test_workspace.exe
```

Expected: 14/14 PASS, `0 failures`. Also run the app and confirm by eye: right-click the header, tick GPU, and check that a GPU-using process reports a non-zero figure while an idle one shows an empty cell rather than `0.0%`.

```bash
./build/msvc-release/bin/Release/taskman.exe
```

- [ ] **Step 7: Commit**

```bash
git add src/tabs/tab_processes.c tests/test_workspace.c
git commit -m "feat: add an optional per-process GPU column

Hidden by default, because showing it is what makes the collector
enumerate roughly a thousand GPU engine counter instances. It sorts and
shades on the same collapsed-or-own figure the CPU column uses, so a
collapsed parent reads consistently across both.

The column menu now reads its labels from the header rather than from a
private array indexed by a hardcoded count, which would have left a
seventh column unlistable and read out of range."
```

---

## Self-Review

**Spec coverage.** The spec's unit 4 is four sentences and each maps to a task: `ProcRow` gains `gpuPct` and `gpuKnown` (Task 2, Step 3); the column joins the existing Select Columns mechanism (Task 4, Step 4); it is off by default because enabling it is what costs (Task 4, Step 3, with the gating in Step 4); `ProcTreeInfo` gains `gpuRollup` and `ProcTree_Aggregate` folds it exactly as it folds CPU (Task 2, Steps 3-4, with a test asserting both fold identically). The spec's testing table has no unit-4 row beyond what units 1-3 covered.

**One thing the spec did not anticipate, and this plan adds:** the enable flag needed to become multi-owner. The spec assumed the column could simply enable collection, but unit 2 had already made the Sensors tab the sole owner, and the unit 2 code comment flagged it. Task 1 exists for that reason and is sequenced first because nothing else can be wired safely until it lands.

**A pre-existing defect fixed in passing:** `ProcColumns`' duplicate `names[]` with a hardcoded count — the same drift hazard `UI_DrawNavigation` had, and adding a seventh column is exactly what would have triggered it. Fixed the same way, by reading from the control that already owns the strings.

**Placeholder scan.** No "TBD", no "add error handling". Three steps deliberately say to read the surrounding code before pasting — Task 3 Step 3 (the CPU assignment sits in a conditional branch and the GPU stamp must not), Task 4 Step 5 (the display handler's and comparator's local names), and Task 4 Step 4 (the `IDM_PROC_COL_FIRST` range bound). Each states what to check and why, rather than leaving the reader to guess.

**Type consistency.** `Gpu_SetEnabled(unsigned owner, BOOL enabled)` is defined in Task 1 and called with that signature in Tasks 1, 3 and 4. `GPU_OWNER_SENSORS`/`GPU_OWNER_PROCESS_COLUMN` are defined once. `ProcRow::gpuPct` is `float` and `Gpu_ProcessUsage` writes a `double`, so Task 3 casts explicitly. `ProcTreeInfo::gpuRollup` is `float`, matching `cpuRollup`, and is read as such in Tasks 2 and 4. `PROC_COL_GPU` is defined once in Task 4 and used in all four of that task's sites.

**Known risk.** Task 4's test toggles the column through `WM_COMMAND` because `TrackPopupMenu` would block the fixture. That means the menu path itself — `ProcColumns` building the popup — stays untested, exactly as it was before this unit. The step calls for a by-eye check of the real menu to cover it.
