# Sensors Tab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a seventh "Sensors" tab that lists GPU adapters with a history graph, and enable GPU collection only while that tab is active.

**Architecture:** A new page module `src/tabs/tab_sensors.c` follows the existing per-tab pattern exactly as `tab_network.c` does: it copies the module-owned model out from under its lock into a private view, refreshes a list view, and repaints a subclassed graph host. The model itself already exists — unit 1 shipped `gpu.c` with `Gpu_Lock`/`Gpu_Unlock` over a `GpuAdapter[]` carrying a 128-sample ring. The only new acquisition code is a ring-to-linear accessor. The seventh tab also forces the fix of a pre-existing drift hazard: `UI_DrawNavigation` carries its own hardcoded six-element `names[]` array indexed by `i < TAB_COUNT`, so bumping `TAB_COUNT` to 7 without touching it is an out-of-bounds read, not merely a wrong label.

**Tech Stack:** Win32 C11, comctl32 list views, GDI/GDI+ via the existing `UI_Chart` family, PDH via the already-built `gpu.c`. CMake 3.25 + Ninja + CTest, MSVC `/W4 /WX` and MinGW `-Wall -Wextra -Werror`.

**Spec:** `docs/superpowers/specs/2026-09-07-gpu-and-temperatures-design.md` — this plan implements **Unit 2** of four. Unit 1 (GPU acquisition) is merged. Unit 3 (temperatures) adds the temperature list *to this same tab* afterwards; unit 4 adds the per-process GPU column.

## Global Constraints

- **No third-party libraries and no runtime dependencies beyond what ships with Windows.** This is the project's stated identity; it is why CPU-die and GPU temperatures are out of scope.
- **C11, MSVC's default C mode.** `_Static_assert` is not available; declarations may be mixed with statements (the existing tabs do), but keep to the local file's style.
- **Both toolchains must build clean at `-Werror` / `/WX`.** Several past defects were only caught by the second toolchain. Every task's verification runs both.
- **All 12 existing CTest suites must stay green.** `test_workspace` creates real windows and fails intermittently for environmental reasons — always re-run once before concluding it is a real failure.
- **Commit messages carry no `Co-Authored-By` line and no AI attribution trailer of any kind.** Subject and body only.
- **This unit adds no `Snapshot` fields.** GPU data is variable-length and lives behind `gpu.c`'s own SRWLOCK, per the spec's data-model section.

## Decisions locked for this unit

These were settled while writing this plan. Do not relitigate them during implementation.

1. **The phantom adapter is excluded by construction, and that is deliberate.** On the development machine PDH reports 3 adapters while DXGI reports 4; the extra is a software render driver with no engine counters. `GpuPublish` iterates *samples* and looks names up by LUID, so an adapter DXGI knows about but PDH never sampled is never published. That is the wanted behaviour — an adapter with no engine counters has nothing to show — and it is recorded here as a decision rather than left as an accident of implementation order.
2. **The nav-bar fix reads labels from the tab control, not from a new API.** `CreatePages` already sets each `TCITEMW::pszText` from `g_page[i]->title`, so the tab control is already a single source of the titles. `UI_DrawNavigation` reads them back with `TabCtrl_GetItem`, and iterates `TabCtrl_GetItemCount` rather than `TAB_COUNT`. This removes the duplicate without coupling `ui.c` to `main.c`'s `g_page` array.
3. **The graph uses `UI_Chart`, not a hand-rolled trace.** The Networking tab predates `UI_Chart` and hand-rolls its grid and polyline; the spec asks for `UI_Chart`/`UI_ChartLine` here, and those are already antialiased and already clamp samples to 0..100.
4. **This unit ships the adapter list and graph only.** The temperature list, the not-elevated explanatory row, and the "no GPU counters" wording for thermal sources belong to unit 3. The GPU-side empty-list explanatory row **is** in scope, because with no temperature section yet an empty tab would otherwise look broken.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `src/ui.c` | Modify (`UI_DrawNavigation`, ~line 607) | Draw nav labels from the tab control's own item text |
| `include/gpu.h` | Modify | Declare `Gpu_History` |
| `src/gpu.c` | Modify | Ring-to-linear history accessor |
| `tests/test_gpu.c` | Modify | Cover the accessor headlessly |
| `include/app.h` | Modify (tab enum ~line 42, factories ~line 161) | `TAB_SENSORS`, `TAB_COUNT` 7, `TabSensors()` |
| `include/resource.h` | Modify | Sensors control IDs, range 2600-2699 |
| `src/tabs/tab_sensors.c` | **Create** | The Sensors page: adapter list, history graph, activation gating |
| `src/main.c` | Modify (`CreatePages` ~1074, `TabCollect` ~1024) | Instantiate and route the seventh page |
| `CMakeLists.txt` | Modify (`TASKMAN_PAGE_SOURCES` ~line 51) | Build the new page |
| `tests/test_workspace.c` | Modify | Seventh tab reachable, labels single-sourced, cadence gated |
| `build.ps1` | Modify (line 11) | Stale "ten CTest suites" in the usage text |

---

### Task 1: Single-source the navigation labels

`UI_DrawNavigation` holds `static const WCHAR *names[]` with exactly six strings and loops `for (i = 0; i < TAB_COUNT; ++i)`. Raising `TAB_COUNT` first would read `names[6]` out of bounds. Fixing it first means every later task runs against a nav bar that cannot drift.

**Files:**
- Modify: `src/ui.c:607-628` (`UI_DrawNavigation`)
- Test: `tests/test_workspace.c`

**Interfaces:**
- Consumes: `TabCtrl_GetItemCount`, `TabCtrl_GetItem` (comctl32, already included by `ui.c`); tab item text set by `CreatePages` in `src/main.c:1082-1089`.
- Produces: no signature change. `void UI_DrawNavigation(HWND tabs, HDC dc)` stays as declared in `include/ui.h:35`.

- [ ] **Step 1: Write the failing test**

In `tests/test_workspace.c`, immediately before the existing `for (i = 0; i < TAB_COUNT; ++i)` tab loop (around line 217), insert:

```c
    /* The nav bar used to keep its own copy of the tab names, indexed by
       TAB_COUNT. Assert the tab control is the single source, and drive a
       real paint so an out-of-bounds label read would fault here. */
    for (i = 0; i < TAB_COUNT; ++i) {
        WCHAR item[64];
        TCITEMW query;
        ZeroMemory(&query, sizeof(query));
        query.mask = TCIF_TEXT;
        query.pszText = item;
        query.cchTextMax = ARRAYSIZE(item);
        CHECK(TabCtrl_GetItem(App_TabControl(), i, &query));
        CHECK(lstrcmpW(item, g_page[i]->title) == 0);
    }
    CHECK(TabCtrl_GetItemCount(App_TabControl()) == TAB_COUNT);
    {
        HDC screen = GetDC(NULL);
        HDC memory = CreateCompatibleDC(screen);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 1180, 64);
        HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
        UI_DrawNavigation(App_TabControl(), memory);
        SelectObject(memory, oldBitmap);
        DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(NULL, screen);
    }
```

- [ ] **Step 2: Run the test and confirm it passes against the current six tabs**

```bash
./build.ps1 test
```

Expected: `test_workspace` PASS. This assertion cannot fail yet — `names[]` and the titles agree at six. It is the regression net for Task 3, which is when the duplicate would become an out-of-bounds read. Confirming it passes now proves the assertion is wired up and not silently skipped.

- [ ] **Step 3: Remove the duplicate**

Replace the body of `UI_DrawNavigation` in `src/ui.c`:

```c
void UI_DrawNavigation(HWND tabs, HDC dc)
{
    /* The labels come from the tab control's own items, which CreatePages
       sets from TabPage::title. A local copy of the names here would drift
       the moment a tab is added, renamed or reordered -- and being indexed
       by TAB_COUNT, drift would mean an out-of-bounds read, not a wrong
       string. Count from the control too, so a page that failed to create
       is skipped rather than painted as a blank card. */
    RECT rc; int i, count = TabCtrl_GetItemCount(tabs);
    int selected = TabCtrl_GetCurSel(tabs);
    GetClientRect(tabs, &rc); UI_Fill(dc, &rc, UI_BG);
    for (i = 0; i < count; ++i) {
        RECT item, label; WCHAR text[64], name[48];
        TCITEMW query;
        ZeroMemory(&query, sizeof(query));
        query.mask = TCIF_TEXT;
        query.pszText = name;
        query.cchTextMax = ARRAYSIZE(name);
        if (!TabCtrl_GetItem(tabs, i, &query)) name[0] = L'\0';
        TabCtrl_GetItemRect(tabs, i, &item);
        item.top = DPX(3); item.bottom = rc.bottom - DPX(2);
        if (i == selected) UI_Card(dc, &item, UI_SURFACE, UI_LINE);
        label = item; label.left += DPX(10); label.right -= DPX(10);
        StringCchPrintfW(text, ARRAYSIZE(text), L"%02d   %s", i + 1, name);
        UI_Text(dc, text, label, 1, i == selected ? UI_BLUE : UI_MUTED,
            DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        if (i == selected) {
            RECT line = {item.left + DPX(12), item.bottom - DPX(3), item.right - DPX(12), item.bottom - DPX(1)};
            UI_Fill(dc, &line, UI_BLUE);
            if (GetFocus() == tabs) { InflateRect(&label, -DPX(2), -DPX(6)); DrawFocusRect(dc, &label); }
        }
    }
}
```

- [ ] **Step 4: Run both toolchains**

```bash
./build.ps1 -Toolchain msvc test
```

```bash
./build.ps1 -Toolchain mingw test
```

Expected: both build clean, 12/12 suites PASS. Watch specifically for an unused-variable warning if any local was left behind — `-Werror` will reject it.

- [ ] **Step 5: Commit**

```bash
git add src/ui.c tests/test_workspace.c
git commit -m "fix: draw navigation labels from the tab control's own items

UI_DrawNavigation kept a private array of six tab names and indexed it by
TAB_COUNT. Renaming or reordering a tab would silently mislabel the nav bar,
and adding one would read past the end of the array. The tab control already
carries every title, set from TabPage::title, so read them back from there
and count the items rather than assuming TAB_COUNT of them."
```

---

### Task 2: Ring-to-linear history accessor

`GpuAdapter` stores history as a `head`/`count` ring. `UI_Chart` wants a flat oldest-first array. The Networking tab solves this with a file-static `net_history()` that no test can reach; put this one in `gpu.c` where the existing headless suite can cover it, because off-by-one ring unwrapping is exactly the kind of bug that renders as "the graph looks slightly wrong" rather than as a failure.

**Files:**
- Modify: `include/gpu.h` (declaration block near `Gpu_Lock`)
- Modify: `src/gpu.c` (near `Gpu_Lock`)
- Test: `tests/test_gpu.c`

**Interfaces:**
- Consumes: `GpuAdapter` (`include/gpu.h`) with members `float history[GPU_HISTORY]; int head, count;`, filled by `GpuPublish` which writes at `head` then advances it.
- Produces: `int Gpu_History(const GpuAdapter *adapter, float *out, int max);` — copies up to `max` samples oldest-first into `out` and returns how many were written. Returns 0 for a NULL argument, a non-positive `max`, or an empty ring. Task 4 calls it.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_gpu.c`, and add a call to it from `main` alongside the existing case functions (match the file's existing `CHECK`-and-count style — do not copy `test_collector.c`'s `CHECK` macro, which does `return 1` and only works inside `main`):

```c
static void test_history_unwraps_oldest_first(void)
{
    GpuAdapter adapter;
    float out[GPU_HISTORY];
    int i, n;

    ZeroMemory(&adapter, sizeof(adapter));

    /* Empty ring yields nothing rather than a run of confident zeroes. */
    CHECK(Gpu_History(&adapter, out, GPU_HISTORY) == 0);

    /* Partially filled, not yet wrapped: three samples, in order. */
    adapter.history[0] = 10.0f; adapter.history[1] = 20.0f;
    adapter.history[2] = 30.0f;
    adapter.head = 3; adapter.count = 3;
    n = Gpu_History(&adapter, out, GPU_HISTORY);
    CHECK(n == 3);
    CHECK(out[0] == 10.0f && out[1] == 20.0f && out[2] == 30.0f);

    /* Full and wrapped: head points at the OLDEST sample, so the output
       must start there and run through the end of the buffer before
       coming back to index 0. A ring bug shows up here as a phase shift. */
    for (i = 0; i < GPU_HISTORY; ++i) adapter.history[i] = (float)i;
    adapter.head = 5; adapter.count = GPU_HISTORY;
    n = Gpu_History(&adapter, out, GPU_HISTORY);
    CHECK(n == GPU_HISTORY);
    CHECK(out[0] == 5.0f);
    CHECK(out[GPU_HISTORY - 6] == (float)(GPU_HISTORY - 1));
    CHECK(out[GPU_HISTORY - 5] == 0.0f);
    CHECK(out[GPU_HISTORY - 1] == 4.0f);

    /* A caller with a smaller buffer gets the NEWEST samples, not the
       oldest: a graph that can show 40 points should show the last 40. */
    n = Gpu_History(&adapter, out, 4);
    CHECK(n == 4);
    CHECK(out[0] == 1.0f && out[1] == 2.0f && out[2] == 3.0f && out[3] == 4.0f);

    /* Defensive arguments. */
    CHECK(Gpu_History(NULL, out, GPU_HISTORY) == 0);
    CHECK(Gpu_History(&adapter, NULL, GPU_HISTORY) == 0);
    CHECK(Gpu_History(&adapter, out, 0) == 0);
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
./build.ps1 -Toolchain mingw test
```

Expected: compile FAIL, `implicit declaration of function 'Gpu_History'`.

- [ ] **Step 3: Declare and implement it**

In `include/gpu.h`, beside `Gpu_Lock`:

```c
/* Copies an adapter's ring into a flat oldest-first array, as UI_Chart
   expects. When 'max' is smaller than the ring the NEWEST 'max' samples are
   returned -- a short graph should show recent history, not ancient history.
   Returns the number of samples written. */
int Gpu_History(const GpuAdapter *adapter, float *out, int max);
```

In `src/gpu.c`, above `Gpu_Lock`:

```c
int Gpu_History(const GpuAdapter *adapter, float *out, int max)
{
    int used, first, i;

    if (!adapter || !out || max <= 0) return 0;
    used = adapter->count;
    if (used > GPU_HISTORY) used = GPU_HISTORY;
    if (used <= 0) return 0;
    if (used > max) used = max;

    /* head is one past the newest sample, so the run of 'used' newest
       samples ends just before head and starts 'used' places earlier.
       GPU_HISTORY is added before the modulo because head - used is
       negative whenever the wanted run crosses the buffer start. */
    first = (adapter->head - used + GPU_HISTORY) % GPU_HISTORY;
    for (i = 0; i < used; ++i)
        out[i] = adapter->history[(first + i) % GPU_HISTORY];
    return used;
}
```

- [ ] **Step 4: Run the tests**

```bash
./build.ps1 -Toolchain msvc test
```

```bash
./build.ps1 -Toolchain mingw test
```

Expected: both build clean, `test_gpu` reports 0 failures, 12/12 suites PASS.

- [ ] **Step 5: Commit**

```bash
git add include/gpu.h src/gpu.c tests/test_gpu.c
git commit -m "feat: unwrap an adapter's GPU history ring into chart order

UI_Chart takes a flat oldest-first array while the model keeps a head/count
ring. Doing the unwrap in gpu.c rather than in the page keeps it reachable
from the headless suite, where an off-by-one shows as a failure instead of
as a graph that looks subtly wrong."
```

---

### Task 3: The seventh tab

Wire an empty Sensors page through the enum, the factory table, the collector router and the build, and prove it is reachable. The page has no content yet; that is Task 4. Splitting here means a reviewer can reject the tab-host wiring without also judging the list-view code.

**Files:**
- Create: `src/tabs/tab_sensors.c`
- Modify: `include/app.h` (tab enum at line 42, factory declarations at 161-166, `Gpu_*` is already in `include/gpu.h`)
- Modify: `include/resource.h` (after the Users block, line 83)
- Modify: `src/main.c:1024-1034` (`TabCollect`), `src/main.c:1074-1079` (`CreatePages`)
- Modify: `CMakeLists.txt:51-58` (`TASKMAN_PAGE_SOURCES`)
- Test: `tests/test_workspace.c` (the existing `for (i = 0; i < TAB_COUNT; ++i)` loop covers it automatically)

**Interfaces:**
- Consumes: `TabPage` (`include/app.h`) — a **positional** struct of function pointers in the order `title, hwnd, index, OnCreate, OnDestroy, OnLayout, OnSnapshot, OnCommand, OnNotify, OnActivate, OnFontChanged, OnContextMenu, BuildViewMenu, InitViewMenu, PrimaryControl`. Get the order wrong and it compiles but calls the wrong function. `UI_CreateListView`, `UI_AddColumn`, `UI_CreateStatic`, `UI_Margin`, `DPX` from `include/app.h` / `include/ui.h`.
- Produces: `TabPage *TabSensors(void);` and the enumerator `TAB_SENSORS`, with `TAB_COUNT == 7`. Task 4 fills the page in; Task 5 tests its activation behaviour.

- [ ] **Step 1: Add the enumerator and the factory**

In `include/app.h`, the tab enum at line 42 becomes:

```c
    TAB_APPS = 0,
    TAB_PROCESSES,
    TAB_SERVICES,
    TAB_PERFORMANCE,
    TAB_NETWORKING,
    TAB_USERS,
    TAB_SENSORS,
    TAB_COUNT
```

Below `TabPage *TabUsers(void);` add:

```c
TabPage *TabSensors(void);
```

`g_cfg.activeTab` is already clamped with `if (g_cfg.activeTab < 0 || g_cfg.activeTab >= TAB_COUNT)` in `src/settings.c:167`, so persisted settings from a six-tab build stay valid and need no migration.

In `include/resource.h`, after the Users block:

```c
/* Sensors tab (2600-2699) */
#define IDC_SENS_GRAPHHOST              2600
#define IDC_SENS_LIST                   2601
```

- [ ] **Step 2: Create the page module**

Create `src/tabs/tab_sensors.c`:

```c
/* ------------------------------------------------------------------------
 * tab_sensors.c - the Sensors tab: GPU adapters now, temperatures later.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ui.h"
#include "gpu.h"

static HWND s_graphHost, s_list;

/* ----------------------------------------------------------------- UI ---- */

static void SensCreate(TabPage *p)
{
    s_graphHost = UI_CreateStatic(p->hwnd, IDC_SENS_GRAPHHOST, L"", SS_BLACKRECT);
    s_list = UI_CreateListView(p->hwnd, IDC_SENS_LIST, 0);
}

static void SensLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    int margin = UI_Margin(p->hwnd);
    int graphH;

    /* Tiny footprint keeps the graph only, matching Performance and
       Networking. */
    if (tiny) {
        if (s_list) ShowWindow(s_list, SW_HIDE);
        if (s_graphHost) {
            ShowWindow(s_graphHost, SW_SHOW);
            MoveWindow(s_graphHost, 0, 0, cx, cy, TRUE);
        }
        return;
    }
    if (s_list) ShowWindow(s_list, SW_SHOW);

    graphH = (cy - 3 * margin) * 2 / 3;
    if (graphH < DPX(60)) graphH = DPX(60);
    if (graphH > cy - 3 * margin - DPX(60)) {
        graphH = cy - 3 * margin - DPX(60);
        if (graphH < DPX(40)) graphH = DPX(40);
    }

    if (s_graphHost)
        MoveWindow(s_graphHost, margin, margin,
                   cx - 2 * margin > 0 ? cx - 2 * margin : 1, graphH, TRUE);
    if (s_list)
        MoveWindow(s_list, margin, margin + graphH + margin,
                   cx - 2 * margin > 0 ? cx - 2 * margin : 1,
                   cy - margin - (margin + graphH + margin) > 0
                       ? cy - margin - (margin + graphH + margin) : 1, TRUE);
}

static void SensSnapshot(TabPage *p)
{
    (void)p;
}

static HWND SensPrimary(TabPage *p)
{
    (void)p;
    return s_graphHost;
}

static void SensDestroy(TabPage *p)
{
    (void)p;
    s_graphHost = NULL;
    s_list = NULL;
}

static TabPage s_page = {
    L"Sensors", NULL, TAB_SENSORS,
    SensCreate, SensDestroy, SensLayout,
    SensSnapshot, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL,
    SensPrimary
};

TabPage *TabSensors(void) { return &s_page; }
```

- [ ] **Step 3: Wire the host and the build**

In `src/main.c`, `CreatePages` gains a line after `TabUsers()`:

```c
    g_page[TAB_USERS]       = TabUsers();
    g_page[TAB_SENSORS]     = TabSensors();
```

`TabCollect` gains its case. The GPU is sampled by `Gpu_Collect` on the collector thread ahead of the tab collectors, so there is nothing per-tab to collect — say so rather than leaving a reader to wonder why the case is missing:

```c
static void TabCollect(int tab)
{
    switch (tab) {
    case TAB_PROCESSES:  Proc_Collect();  break;
    case TAB_APPS:       Apps_Collect();  break;
    case TAB_SERVICES:   Svc_Collect();   break;
    case TAB_USERS:      Users_Collect(); break;
    case TAB_NETWORKING: Net_Collect();   break;
    /* TAB_SENSORS has no tab collector: sysinfo.c calls Gpu_Collect on the
       collector thread ahead of this router, so the model is already
       current by the time the page reads it. */
    default: break;
    }
}
```

In `CMakeLists.txt`, add `src/tabs/tab_sensors.c` to `TASKMAN_PAGE_SOURCES`. `TASKMAN_PAGE_SOURCES` also feeds `test_workspace`, so the fixture picks the page up with no further edit.

- [ ] **Step 4: Check the tab strip still fits at 100% DPI**

`src/main.c:841` divides the strip by `TAB_COUNT`:

```c
TabCtrl_SetItemSize(g_hTabs, (cx - 2 * margin - DPX(4)) / TAB_COUNT, DPX(38));
```

At the 840 px minimum width and an 18 px margin this drops each label from 133 px to 114 px. "Performance" is the longest title and the labels are drawn `DT_END_ELLIPSIS`, so verify by eye that no label is truncated at 100 % DPI and the minimum window width. Run the app:

```bash
./build/msvc/taskman.exe
```

If a label does truncate, narrow the per-label padding in `UI_DrawNavigation` (`label.left += DPX(10)` / `label.right -= DPX(10)`) rather than shortening a title — the titles are user-visible in the tab control too.

- [ ] **Step 5: Run both toolchains**

```bash
./build.ps1 -Toolchain msvc test
```

```bash
./build.ps1 -Toolchain mingw test
```

Expected: 12/12 PASS. `test_workspace`'s existing `for (i = 0; i < TAB_COUNT; ++i)` loop now switches to, shows and bounds-checks the Sensors page, and Task 1's assertions now confirm the nav bar labels it from `TabPage::title`. If the Task 1 assertions fail here, the label single-sourcing regressed — that is precisely what they were added for.

- [ ] **Step 6: Commit**

```bash
git add include/app.h include/resource.h src/tabs/tab_sensors.c src/main.c CMakeLists.txt
git commit -m "feat: add the Sensors tab to the tab host

An empty seventh page, wired through the tab enum, the factory table, the
collector router and the build. TAB_SENSORS has no tab collector because
Gpu_Collect already runs on the collector thread ahead of that router. The
persisted activeTab is clamped to TAB_COUNT, so settings written by a
six-tab build stay valid."
```

---

### Task 4: Adapter list and history graph

**Files:**
- Modify: `src/tabs/tab_sensors.c`
- Test: `tests/test_workspace.c`

**Interfaces:**
- Consumes: `const GpuAdapter *Gpu_Lock(int *count)` / `void Gpu_Unlock(void)` and `int Gpu_History(const GpuAdapter *, float *, int)` from `include/gpu.h`; `GpuAdapter` members `luid, name[GPU_NAME_MAX], nameKnown, utilization, engine[GPU_ENGINE_KINDS], dedicatedUsed, dedicatedTotal, sharedTotal, history[GPU_HISTORY], head, count`. `UI_Chart(HDC, RECT, const float *, int, COLORREF, BOOL grid)`, `UI_Fill`, `UI_FormatSize` from `include/ui.h` / `include/app.h`.
- Produces: nothing new for later tasks; Task 5 tests the `OnActivate` added here.

- [ ] **Step 1: Copy the model into a private view**

Replace `SensSnapshot` and add the model statics and comparator in `src/tabs/tab_sensors.c`. The view copy under the lock is the same idiom as `NetSnapshot`; the difference is that `gpu.c` owns a fixed-size array, so the copy is a stack array and cannot fail to allocate.

```c
/* ----------------------------------------------------------------- model -- */

static GpuAdapter s_view[GPU_MAX_ADAPTERS];
static int s_viewCount;
static ULONGLONG s_selected;
static int s_column, s_direction = 1;
static BOOL s_refreshing;

static int sens_compare(const void *left, const void *right)
{
    const GpuAdapter *a = left, *b = right;
    int result;
    if (s_column == 1)
        result = (a->utilization > b->utilization) - (a->utilization < b->utilization);
    else if (s_column == 2)
        result = (a->dedicatedUsed > b->dedicatedUsed) - (a->dedicatedUsed < b->dedicatedUsed);
    else if (s_column == 3)
        result = (a->dedicatedTotal > b->dedicatedTotal) - (a->dedicatedTotal < b->dedicatedTotal);
    else
        result = lstrcmpiW(a->name, b->name);
    /* LUID breaks ties so the order is stable across refreshes and rows do
       not swap under the user's cursor. */
    if (!result) result = (a->luid > b->luid) - (a->luid < b->luid);
    return result * s_direction;
}

static void SensSnapshot(TabPage *p)
{
    const GpuAdapter *model;
    int count = 0, i;
    BOOL found = FALSE;

    (void)p;
    model = Gpu_Lock(&count);
    if (count > GPU_MAX_ADAPTERS) count = GPU_MAX_ADAPTERS;
    if (count > 0) CopyMemory(s_view, model, (size_t)count * sizeof(*s_view));
    Gpu_Unlock();
    s_viewCount = count;

    if (count > 1) qsort(s_view, (size_t)count, sizeof(*s_view), sens_compare);
    for (i = 0; i < count; ++i) if (s_view[i].luid == s_selected) found = TRUE;
    if (!found) s_selected = count ? s_view[0].luid : 0;

    if (s_graphHost) InvalidateRect(s_graphHost, NULL, FALSE);
    if (!s_list) return;

    s_refreshing = TRUE;
    SendMessageW(s_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_list);
    for (i = 0; i < count; ++i) {
        WCHAR util[32], used[48], total[48];
        LVITEMW item = {0};
        StringCchPrintfW(util, ARRAYSIZE(util), L"%.1f%%", s_view[i].utilization);
        UI_FormatSize(s_view[i].dedicatedUsed, used, ARRAYSIZE(used));
        if (s_view[i].dedicatedTotal)
            UI_FormatSize(s_view[i].dedicatedTotal, total, ARRAYSIZE(total));
        else
            lstrcpyW(total, L"Unknown");   /* no DXGI: figures still shown */
        item.mask = LVIF_TEXT; item.iItem = i; item.pszText = s_view[i].name;
        ListView_InsertItem(s_list, &item);
        ListView_SetItemText(s_list, i, 1, util);
        ListView_SetItemText(s_list, i, 2, used);
        ListView_SetItemText(s_list, i, 3, total);
        if (s_view[i].luid == s_selected)
            ListView_SetItemState(s_list, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (!count) {
        /* An empty list with no explanation reads as a broken tab. A machine
           with no GPU performance counters -- pre-1709, or the provider
           disabled -- is a supported case, not an error worth a dialog. */
        LVITEMW item = {0};
        item.mask = LVIF_TEXT; item.iItem = 0;
        item.pszText = UI_Str(L"No GPU performance counters available");
        ListView_InsertItem(s_list, &item);
    }
    UI_SetHeaderSortArrow(s_list, s_column, s_direction);
    SendMessageW(s_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_list, NULL, TRUE);
    s_refreshing = FALSE;
}
```

Add `#include <stdlib.h>` for `qsort` and `#include <strsafe.h>` for `StringCchPrintfW` if `app.h`/`ui.h` do not already pull them in — check the top of `tab_network.c`, which has the same needs, and match it.

- [ ] **Step 2: Add the columns**

Extend `SensCreate`. Widths are dialog units, matching the Networking tab's proportions:

```c
static void SensCreate(TabPage *p)
{
    s_graphHost = UI_CreateStatic(p->hwnd, IDC_SENS_GRAPHHOST, L"", SS_BLACKRECT);
    if (s_graphHost)
        SetWindowSubclass(s_graphHost, SensGraphSubclass, 1, 0);

    s_list = UI_CreateListView(p->hwnd, IDC_SENS_LIST, 0);
    if (s_list) {
        UI_AddColumn(s_list, 0, L"Adapter",           140, LVCFMT_LEFT);
        UI_AddColumn(s_list, 1, L"GPU Utilization",    72, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 2, L"Dedicated Used",     72, LVCFMT_RIGHT);
        UI_AddColumn(s_list, 3, L"Dedicated Total",    72, LVCFMT_RIGHT);
    }
}
```

- [ ] **Step 3: Paint the graph**

Add above `SensCreate`, using `UI_Chart` rather than a hand-rolled trace — it draws the surface, the grid and an antialiased filled trace in one call, and clamps samples to 0..100 itself:

```c
static LRESULT CALLBACK SensGraphSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    if (msg == WM_ERASEBKGND) return TRUE;
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, SensGraphSubclass, id);
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, graph;
        const GpuAdapter *adapter = NULL;
        WCHAR title[256];
        float samples[GPU_HISTORY];
        int count = 0, i;

        GetClientRect(hwnd, &rc);
        UI_Fill(dc, &rc, UI_SURFACE);
        for (i = 0; i < s_viewCount; ++i)
            if (s_view[i].luid == s_selected) { adapter = &s_view[i]; break; }

        StringCchPrintfW(title, ARRAYSIZE(title), L"%s - GPU utilization (0-100%%)",
                         adapter ? adapter->name : L"No adapter selected");
        SetTextColor(dc, UI_INK); SetBkMode(dc, TRANSPARENT);
        graph = rc; graph.left += DPX(14); graph.right -= DPX(14); graph.top += DPX(10);
        {
            /* g_hFont is deleted and recreated on a DPI or font change;
               leaving a deleted object selected into a DC is undefined. */
            HGDIOBJ oldFont = SelectObject(dc, g_hFont);
            DrawTextW(dc, title, -1, &graph, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (oldFont) SelectObject(dc, oldFont);
        }
        graph.top += DPX(28); graph.bottom -= DPX(10);
        if (adapter) count = Gpu_History(adapter, samples, GPU_HISTORY);
        UI_Chart(dc, graph, count ? samples : NULL, count, UI_TEAL, TRUE);

        EndPaint(hwnd, &ps); return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
```

- [ ] **Step 4: Handle sorting and selection**

Add the notify handler and place it in the vtable's `OnNotify` slot — position 6 of the function pointers, immediately after `OnCommand`:

```c
static BOOL sens_notify(TabPage *p, NMHDR *nm, LRESULT *result)
{
    if (nm->hwndFrom != s_list || s_refreshing) return FALSE;
    if (nm->code == LVN_COLUMNCLICK) {
        int column = ((NMLISTVIEW *)nm)->iSubItem;
        s_direction = column == s_column ? -s_direction : 1;
        s_column = column; SensSnapshot(p); *result = 0; return TRUE;
    }
    if (nm->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *change = (NMLISTVIEW *)nm;
        if ((change->uNewState & LVIS_SELECTED) &&
            change->iItem >= 0 && change->iItem < s_viewCount) {
            s_selected = s_view[change->iItem].luid;
            if (s_graphHost) InvalidateRect(s_graphHost, NULL, FALSE);
        }
    }
    return FALSE;
}
```

The vtable becomes:

```c
static TabPage s_page = {
    L"Sensors", NULL, TAB_SENSORS,
    SensCreate, SensDestroy, SensLayout,
    SensSnapshot, NULL, sens_notify, NULL, NULL, NULL,
    NULL, NULL,
    SensPrimary
};
```

- [ ] **Step 5: Run both toolchains and look at it**

```bash
./build.ps1 -Toolchain msvc test
```

```bash
./build.ps1 -Toolchain mingw test
```

```bash
./build/msvc/taskman.exe
```

Expected: 12/12 PASS. In the running app the Sensors tab shows nothing yet — the GPU collector is still gated off, which Task 5 fixes. Confirm the tab renders, the columns are present, the graph host paints its caption reading "No adapter selected", and tiny footprint mode shows the graph alone.

- [ ] **Step 6: Commit**

```bash
git add src/tabs/tab_sensors.c
git commit -m "feat: list GPU adapters and graph the selected one

Follows the Networking tab's idiom -- sortable columns, LUID-stable ordering
and selection preserved across refreshes -- but draws through UI_Chart rather
than hand-rolling the trace, since UI_Chart is already antialiased and already
clamps samples. An adapter with no DXGI description still lists, with its
capacity shown as Unknown; a machine with no counters at all gets an
explanatory row rather than a blank list."
```

---

### Task 5: Gate GPU collection on the tab being active

Until now nothing calls `Gpu_SetEnabled`, so `Gpu_Collect` returns immediately and the tab stays empty. This is the point of the gate: the spec requires that with the Sensors tab never opened and the per-process column off, no PDH query is opened at all.

**Files:**
- Modify: `src/tabs/tab_sensors.c`
- Modify: `tests/test_workspace.c`
- Modify: `build.ps1:11`
- Test: `tests/test_workspace.c`

**Interfaces:**
- Consumes: `void Gpu_SetEnabled(BOOL)`, `BOOL Gpu_IsEnabled(void)` from `include/gpu.h`; the `OnActivate` slot of `TabPage`, called by `SwitchToTab` in `src/main.c` with `FALSE` for the outgoing page and `TRUE` for the incoming one.
- Produces: the behavioural contract unit 4 extends — unit 4's per-process column must enable the GPU independently, so it must not simply clear the flag on deactivate. This task's implementation is written so that a later `Gpu_SetEnabled(TRUE)` from the column survives leaving the tab only if unit 4 re-asserts it; note that explicitly in unit 4's plan.

- [ ] **Step 1: Write the failing test**

In `tests/test_workspace.c`, before the existing `for (i = 0; i < TAB_COUNT; ++i)` loop, and after it:

```c
    /* Nothing has opened the Sensors tab, so the collector must not have
       paid for a PDH query. */
    CHECK(!Gpu_IsEnabled());
```

and immediately after the loop (which ends on `TAB_SENSORS`, the last tab):

```c
    /* Activating the tab turns collection on; leaving it turns it back off,
       so an unopened tab costs nothing and a closed one stops costing. */
    SwitchToTab(TAB_SENSORS, FALSE); Pump(120);
    CHECK(Gpu_IsEnabled());
    Capture(hwnd, L"tests/.build/workspace-sensors.bmp");
    SwitchToTab(TAB_PROCESSES, FALSE); Pump(60);
    CHECK(!Gpu_IsEnabled());
```

Add `#include "gpu.h"` to the fixture's includes if it is not already there.

- [ ] **Step 2: Run it to verify it fails**

```bash
./build.ps1 -Toolchain mingw test
```

Expected: `test_workspace` FAILs on `CHECK(Gpu_IsEnabled())` — the page has no `OnActivate` yet. The first assertion passes trivially, which is the point of keeping it.

- [ ] **Step 3: Add the activation handler**

In `src/tabs/tab_sensors.c`:

```c
/* Collection is gated on this tab so that a user who never opens it never
   pays for the PDH query -- around a thousand engine instances on a machine
   with three adapters. Unit 4's per-process GPU column will enable it
   independently; when that lands, this must stop being the only owner of
   the flag. */
static void SensActivate(TabPage *p, BOOL active)
{
    (void)p;
    Gpu_SetEnabled(active);
}
```

and place it in the vtable's `OnActivate` slot — position 7, immediately after `OnNotify`:

```c
static TabPage s_page = {
    L"Sensors", NULL, TAB_SENSORS,
    SensCreate, SensDestroy, SensLayout,
    SensSnapshot, NULL, sens_notify, SensActivate, NULL, NULL,
    NULL, NULL,
    SensPrimary
};
```

- [ ] **Step 4: Fix the stale suite count and the fixture's summary line**

`build.ps1:11` still reads `build and run all ten CTest suites`; there are twelve. Change "ten" to "twelve". In `tests/test_workspace.c`, the closing `printf` says "six tabs" — change it to "seven tabs".

- [ ] **Step 5: Run both toolchains**

```bash
./build.ps1 -Toolchain msvc test
```

```bash
./build.ps1 -Toolchain mingw test
```

Expected: 12/12 PASS on both. If `test_workspace` fails, re-run once before investigating — it creates real windows and is known to fail intermittently for environmental reasons.

- [ ] **Step 6: Confirm it live**

```bash
./build/msvc/taskman.exe
```

Open the Sensors tab and wait two seconds — the first GPU sample is discarded because rate counters need two collections, so the list populates on the second. Expected on the development machine: three adapters, the first named `NVIDIA GeForce RTX 4080 SUPER`, utilisation moving, and the graph filling from the right. Switch away and back; the history must carry forward rather than reset, because `GpuPublish` merges each sample into the existing ring by LUID.

- [ ] **Step 7: Commit**

```bash
git add src/tabs/tab_sensors.c tests/test_workspace.c build.ps1
git commit -m "feat: collect GPU statistics only while the Sensors tab is open

Gpu_Collect returns immediately unless something has enabled it, and until
now nothing did. Gating on activation means a user who never opens the tab
never pays for the PDH query over roughly a thousand engine instances. The
workspace fixture asserts both directions, so a future change that leaves
collection running after the tab closes fails the suite."
```

---

## Self-Review

**Spec coverage.** Every unit-2 requirement in the spec's "The Sensors tab" and "Seventh-tab mechanics" sections maps to a task: adapter list with the Networking idiom and sortable, selection-preserving columns (Task 4); history graph via `UI_Chart` over a 128-sample ring (Tasks 2 and 4); tiny footprint showing the graph only (Task 3, Step 2); `TAB_COUNT` 7 with `TAB_SENSORS` (Task 3); the `UI_DrawNavigation` duplicate fixed by drawing from the tab titles (Task 1); the tab strip narrowing checked at 100 % DPI (Task 3, Step 4); `activeTab` already clamped, verified and stated (Task 3, Step 1); `TabCollect` case added, with a comment saying why it is empty (Task 3, Step 3); `BuildMenuBar` needs no case — it dispatches through `g_page[g_active]->BuildViewMenu`, and this page supplies none, so the spec's "BuildMenuBar gains the new case" is satisfied by the existing indirection rather than by an edit. From the spec's edge-case table: no counters (Task 4, empty-list row), DXGI unavailable (Task 4, `Unknown` capacity with figures still shown), first sample discarded (Task 5, Step 6), adapter appears or disappears (handled by unit 1's 10 s rebuild, and by re-selecting when the selected LUID vanishes in `SensSnapshot`), tab never opened (Task 5). From the testing table, the "Cadence" row is Task 5's assertions and "the workspace fixture gains the seventh tab" is Task 3, Step 5.

**Deliberately deferred to unit 3, not gaps:** the temperature list, the threshold shading, and the not-elevated ACPI row. Deliberately deferred to unit 4: the per-process GPU column and `ProcTreeInfo::gpuRollup`.

**Placeholder scan.** No "TBD", no "add error handling", no "similar to Task N". Every code step carries the actual code. The one judgement call left to the implementer — Task 3, Step 4's label-fit check — has a stated pass criterion and a stated remedy.

**Type consistency.** `Gpu_History(const GpuAdapter *, float *, int)` is declared in Task 2 and called with that signature in Task 4, Step 3. `GpuAdapter` member names used in Task 4 (`luid`, `name`, `utilization`, `dedicatedUsed`, `dedicatedTotal`, `history`, `head`, `count`) match `include/gpu.h` as shipped by unit 1. `Gpu_Lock(int *count)` returns `const GpuAdapter *` and is paired with `Gpu_Unlock()` in Task 4, Step 1. The `TabPage` vtable is positional; Tasks 3, 4 and 5 each show the complete initialiser rather than describing an insertion, because the slots are unlabelled and a miscount compiles cleanly.
