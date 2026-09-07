# Process tree for the Processes tab

Show the Processes table as a parent/child hierarchy instead of a flat list.
`ProcRow` already carries `parentPid` and `createTime`, and the inspector
already prints the parent PID, so this feature adds no data acquisition at
all: it is entirely a view-construction and row-painting change.

This is the first of three planned features. Per-process history and threshold
alerts follow, and both depend on a process tracking table that this feature
does not build. Nothing here should anticipate them.

## Decisions

Four decisions were settled before design and are not open for
reinterpretation during implementation:

1. **Sorting preserves hierarchy.** The active sort column orders siblings
   within each parent, and orders roots among themselves. Sorting never
   flattens the tree.
2. **Search keeps ancestors as context.** A row that does not match the query
   but has a descendant that does is shown as a dimmed context row.
3. **Collapsed parents roll up.** An expanded parent shows its own values; a
   collapsed parent shows its subtree totals.
4. **Tree is on by default, fully expanded.** On first run the row set is
   identical to today's; only ordering and indentation differ. A View menu
   toggle returns to the flat list, and the choice persists.

## Approach

The tree is built on the UI thread, in `ProcSnapshot`. `Proc_Collect` is not
modified; it keeps publishing a flat `ProcRow[]` under `g_procLock` exactly as
it does now.

This matches the split the codebase already holds: the collector acquires, the
UI decides presentation. Sort column, search query and resource filter are all
already UI-thread-only state, and collapse state belongs with them. No new
locking is introduced, and collapsing a node re-runs the view build without
waiting for a sample.

Two alternatives were rejected:

- **Building the tree on the collector thread.** Expand/collapse is UI state
  that the collector would then need, requiring either new locking or a second
  ordering pass on the UI thread — doing the work twice.
- **Hybrid, with the collector computing subtree aggregates.** Rejected as
  incorrect rather than merely awkward. Aggregates depend on which rows survive
  the user and search filters, which are UI concerns, so any aggregate computed
  by the collector is wrong whenever a filter is active.

Cost of the chosen approach is O(n log n) per tick on roughly 400 rows. The
existing `s_prev` CPU-delta lookup is already O(n^2) per tick at that size.

## Data model

`ProcRow` is unchanged. It is the acquisition record shared with the collector,
and `ProcSelected`, `ProcExport`, `ProcDrawDetails` and the end-process path all
consume it. Tree structure lives in a parallel UI-side array:

```c
#define PROC_INDENT_CAP  8       /* visual indent stops here; depth does not */
#define PROC_DEPTH_MAX  64       /* structural ceiling; see "Cycles" below    */

typedef struct {
    int       depth;             /* 0 = root                                 */
    int       childCount;        /* children in the filtered set, not all;   */
                                 /* 0 => no twisty painted                   */
    BOOL      collapsed;
    BOOL      context;           /* ancestor of a match, not a match itself  */
    float     cpuRollup;         /* subtree totals, including this row       */
    ULONGLONG memRollup;
} ProcTreeInfo;

static ProcTreeInfo *g_tree;     /* always exactly g_viewCnt entries         */
static int          *g_visible;  /* indices into g_view, in display order    */
static int           g_visibleCnt;
```

`g_view` holds **every** surviving row in tree order — matches and context
rows, including rows underneath a collapsed parent. `g_tree` is parallel to it.
`g_visible` is the subset actually shown, and it is what drives the control:
`ListView_SetItemCountEx` is called with `g_visibleCnt`, and `LVN_GETDISPINFOW`
maps a display row through `g_view[g_visible[row]]`.

Splitting the two is what makes collapse purely a display concern. Export and
the summary line walk `g_view` and are unaffected by what is collapsed;
`ProcSelected` resolves through `g_visible`.

The invariant is that `g_tree` holds `g_viewCnt` entries indexed alongside
`g_view`, and every element of `g_visible` is a valid index into it. All three
are rebuilt at the single point in `ProcSnapshot` where the view is rebuilt, and
that is the only place the invariant can break.

Adding these fields to `ProcRow` instead was rejected: it would push view
concerns into the record the collector fills for every process on the machine.

Collapse state persists across the one-second refresh in a separate array:

```c
typedef struct { DWORD pid; ULONGLONG createTime; } ProcCollapsed;
```

Because the default is expanded, this stores the **collapsed** set, which is
normally empty and is bounded by user clicks. It is pruned each tick against
processes that still exist. It is not persisted across runs: PIDs are
meaningless after a reboot.

## Tree construction

Parent lookup uses an index sorted by PID with binary search, built once per
tick.

Three rules govern linking, and all three are load-bearing:

**PID reuse.** A candidate parent is valid only if
`parent.createTime <= child.createTime`. Windows recycles PIDs, so a row's
`parentPid` can name a process that started *after* it; without this check the
tree parents processes to their own successors. `createTime` is already
collected, so the check is free.

**Missing parent.** If no valid parent is present in the filtered set — it
exited, or "show processes from all users" is off and it belongs to another
account — the row is promoted to root. The user filter therefore reshapes the
tree, which is correct.

**Cycles.** The `createTime` rule makes a cycle require non-decreasing creation
times around a loop, which is only possible when two processes share a 100ns
tick. A visited mark during the depth-first walk plus `PROC_DEPTH_MAX` as a hard
ceiling makes a cycle structurally impossible rather than unlikely. A row that
would exceed the ceiling is promoted to root.

PID 0 is already excluded upstream. PID 4 (`System`) has parent PID 0 and so
becomes a root, which is correct.

## Pipeline

The order in `ProcSnapshot` is not interchangeable:

```
copy shared under g_procLock
  -> user filter (showAllUsers / ownedByCurrentUser / pending / selected)
  -> link parents (createTime-validated) over the user-filtered set
  -> mark search and resource matches
  -> walk up from each match, marking unmatched ancestors as context
  -> drop rows that are neither match nor context
  -> recompute depth and childCount over the survivors
  -> post-order aggregate subtree cpu/mem
  -> apply collapse state (context rows forced expanded)
  -> sort siblings and roots by the active column  => g_view, g_tree
  -> walk depth-first, skipping under collapsed nodes => g_visible
```

Linking must precede the search pass: ancestry is defined by the parent links,
so ancestors cannot be identified before the links exist. Dropping non-survivors
then requires a second depth and child-count pass, because removing a row
changes the depth of everything beneath it.

Aggregation runs after filtering so totals describe what is on screen.

**Unknown memory.** `ProcRow.memoryKnown` is false for processes the caller
cannot open. A rollup sums only known descendants. A collapsed parent displays
its rolled-up figure when at least one row in its subtree (including itself) has
a known value, and `N/A` only when none do. A partial total is more useful than
`N/A`, but it does understate, and the same rule already governs the existing
summary line.

**Sort key.** The existing `ProcCompare`, including its PID tie-break, is
reused unchanged and applied within each sibling group and across the roots.
A row sorts by the value it currently displays: own value when
expanded, rollup when collapsed. A consequence worth stating plainly, because it
is intended and will look like a bug otherwise: collapsing a node can move it in
the sort order, since its displayed value jumps from its own to its subtree's.

**Summary line.** `N of M processes` counts matching processes, not visible
rows: collapsed-away descendants still count, and context rows do not. The CPU
and memory figures in the summary sum **own values only**, never rollups, or
they would double-count every branch.

**Context rows.** Two rules that the pipeline above does not imply on its own:

- A context row is never collapsed. An ancestor added purely to give a match
  its context is pointless if the match stays hidden underneath it, so any
  persisted collapse state on such a row is ignored while it is a context row.
  The collapse entry is retained, not discarded, and takes effect again once
  the search is cleared.
- Context rows are drawn only from rows that survived the **user** filter.
  An ancestor excluded by "show processes from all users" is not re-admitted
  to the view, because doing so would leak another account's process list past
  a filter the user set deliberately. The matching descendant is promoted to
  root instead.

**Export.** `ProcExport` writes every filtered, sorted, non-context row with its
own values, flat — **including rows hidden underneath a collapsed parent**.
Collapse is a viewing convenience and must not silently change what gets
exported. This falls out of the data model for free: export walks `g_view`,
which holds every surviving row, rather than `g_visible`.
Context rows are excluded, because a search for `renderer` should not export
browser parents the user never asked for.

## Rendering

The first column is owner-drawn. `ProcNotify` returns `CDRF_SKIPDEFAULT` for
subitem 0 and paints indent, twisty and text directly; other subitems keep
returning `CDRF_NEWFONT` as they do now.

`LVIF_INDENT` was rejected. ListView's built-in indent works in virtual mode,
but its unit is the small image list icon width, and the Processes list has no
image list — it would need a dummy one attached purely to define the step.
Owner-drawing keeps the indent step under `DPX()` control at every DPI, and
`ListStyleProc` in `ui.c` already establishes `CDRF_SKIPDEFAULT` painting as a
pattern in this codebase.

Geometry, all DPI-scaled:

- Indent step `DPX(14)` per level, clamped at `PROC_INDENT_CAP` levels.
- Twisty occupies `DPX(12)` at the start of the row's indent, vertically
  centred, painted only when `childCount > 0`.
- The twisty is a two-segment chevron through `UI_Polyline` at `DPX(2)`,
  pointing right when collapsed and down when expanded — antialiased through
  the GDI+ layer.
- Context rows draw their text in `UI_MUTED` instead of `UI_INK`.

Because `CDRF_SKIPDEFAULT` suppresses the control's own drawing for that cell,
selection, focus and hot-state backgrounds must be painted from
`draw->nmcd.uItemState` (`CDIS_SELECTED`, `CDIS_FOCUS`, `CDIS_HOT`). This is the
single most likely source of visual regression in the feature.

## Interaction

**Twisty click.** `NM_CLICK` on the list, then `ListView_SubItemHitTest` to
confirm subitem 0 and identify the row, then test the click x against that row's
twisty rect. Selection has already moved by the time `NM_CLICK` arrives, so
clicking a twisty also selects that row. This is accepted: intercepting
`WM_LBUTTONDOWN` would mean touching `ListStyleProc`, which every list in the
application shares.

**Keyboard**, via `LVN_KEYDOWN`. Report-view ListView does not use Left or
Right, so nothing is displaced:

- Right: expand a collapsed node, or move to the first child if expanded.
- Left: collapse an expanded node, or move to the parent if collapsed.

**Collapsing under the selection.** If the selected row is inside a subtree
being collapsed it would silently disappear. Selection moves to the collapsing
parent instead.

**`Proc_SelectPid`.** The Services tab's "Go to Process" must expand every
ancestor of the target before selecting, or it scrolls to a row that is present
in `g_view` but absent from `g_visible`. This runs before the existing
search/filter reset logic.

## Settings and menu

- `include/app.h`: `BOOL procTreeMode` in `Settings`.
- `src/settings.c`: registry value `ProcessTreeView`, default 1, following the
  existing `GetDwordDef` / `Reg_SetDword` pattern.
- `include/resource.h`: `IDM_VIEW_PROCTREE` in the 400xx command block.
- `ProcBuildViewMenu` appends "Process &Tree". Processes currently passes `NULL`
  for `InitViewMenu`; that slot is filled to carry the check mark.

Turning tree mode off restores exactly today's behaviour: no tree build, no
context rows, flat sort over all filtered rows.

## Non-goals

- No CSV schema change. Exporting a tree is lossy without a parent column, but
  changing the header is a separate decision with its own compatibility cost.
- No tree on any other tab.
- No grouping concept beyond real parent/child links.
- Collapse state is not persisted across runs.

## Edge cases

| Case | Behaviour |
|---|---|
| All rows filtered out | Empty view, no tree build, no crash |
| Search matches a root only | No context rows added |
| Depth beyond `PROC_INDENT_CAP` | Indent clamps; structure and rollups unaffected |
| Parent hidden by user filter | Children promoted to root |
| Sort by PID in tree mode | Siblings ordered by PID, hierarchy intact |
| Tree mode off | No linking, aggregation or context rows; `g_tree` zeroed and `g_visible` is the identity mapping, so the render and selection paths stay uniform |
| Allocation failure for `g_tree` or `g_visible` | Retain the previous view entirely, as `ProcSnapshot` already does when `g_view` cannot be allocated |

## Testing

`tests/test_processes.c` already includes `tab_processes.c` with the UI stubbed,
so tree construction is testable headlessly from synthetic `ProcRow[]` — no
window and no message pump. This is the main reason the feature is built UI-side.

Cases to add there:

| Case | Assertion |
|---|---|
| Linking | Three-level hierarchy produces correct depths and child counts |
| PID reuse | Parent with a later `createTime` is rejected; child becomes a root |
| Missing parent | Row promoted to root |
| Cycle | Two rows with equal `createTime` naming each other terminates |
| Aggregation | Subtree totals correct and computed after filtering |
| Collapse | Flatten skips the subtree; rollup appears on the parent |
| Sibling sort | Children ordered by the active column, hierarchy intact |
| Search context | Ancestors added, excluded from the summary count and the export set |
| Summary math | Summary CPU sums own values, not rollups |
| Pruning | Collapse set drops entries for processes that no longer exist |
| Selection | Moves to the parent when its subtree collapses |

The PID-reuse and cycle cases are the ones expected to catch real defects. Both
are unreachable on demand through the UI, which is precisely why they need
synthetic input.

`tests/test_workspace.c` adds Win32-level coverage against real controls: View
menu toggle, a twisty click, Left and Right keys, and render captures at 100%,
150% and 200% DPI to catch selection-painting regressions in the owner-drawn
cell.

Both suites build with warnings as errors, as they do today.

## Risks

1. **Owner-drawn selection painting.** Suppressing default drawing for subitem 0
   means reproducing selection, focus and hot states by hand. Mitigated by the
   DPI render captures, but this is where a subtle regression is most likely.
2. **Sort order shifting on collapse.** A direct consequence of decision 3.
   Intended, and documented above, but it will be reported as a bug at least
   once.
3. **Parallel array invariant.** `g_tree` and `g_view` must stay the same
   length. Confined to one rebuild site, and asserted there.
