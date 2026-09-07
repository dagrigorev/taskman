# Application and test source review

Reviewed 2026-09-07. This is a baseline review of the existing application, separate from the CMake/build-script migration. Findings below were validated by tracing the source; this review did not execute builds or tests and does not claim runtime reproduction. No application source was changed by this review.

## Scope and inventory

Read all 11 application C files and all six headers (approximately 300 KB), all ten `tests/*.c` fixtures (approximately 71 KB), and both textual resources. Inspected the icon directory: five 32-bit images at 16, 24, 32, 48 and 64 pixels; all image extents fit the 34,494-byte file. Icon appearance was not visually reviewed. Build scripts are covered by the separate build review.

| Files | Responsibility and dependencies |
| --- | --- |
| `src/main.c` | Unicode GUI entry point, window/message loop, dialog host, menu/tray, DPI, shell launch, shared control helpers; user32, gdi32, comctl32, shell32, shlwapi, advapi32, uxtheme. |
| `src/settings.c` | Typed and bounded HKCU persistence; advapi32. |
| `src/sysinfo.c` | Single sampling worker, SRW-protected snapshots/history, CPU groups, memory; kernel32, psapi, dynamically resolved ntdll exports. |
| `src/ui.c` | Native control styling, dashboard and charts; GDI and dynamically resolved GDI+ flat API. |
| `src/tabs/proc_tree.c` | Process hierarchy, cycle/depth handling, aggregates, filter context, collapse identities; CRT plus Windows types. |
| `src/tabs/tab_processes.c` | Native/Toolhelp process collection, metadata cache, search/tree/list/inspector, CSV and process actions; psapi/kernel32, advapi32, version, comdlg32, shell32/shlwapi, ole32, comctl32, uxtheme. |
| `src/tabs/tab_apps.c` | Window enumeration, response probes, task selection and window actions; user32/comctl32. |
| `src/tabs/tab_services.c` | SCM enumeration, status and service actions; advapi32. |
| `src/tabs/tab_users.c` | WTS session enumeration and identity-checked actions; wtsapi32. |
| `src/tabs/tab_network.c` | IP Helper adapter counters, rate/history model and view; iphlpapi and Win32 controls. |
| `src/tabs/tab_perf.c` | CPU/memory charts and metric presentation. |
| `include/app.h`, `include/ui.h` | Shared contracts, global state, tab callbacks, controls and drawing. |
| `include/proc_tree.h` | Process data and tree transform contract. |
| `include/ntapi.h`, `include/gdiplusapi.h` | Locally declared native ABI structures and function signatures. |
| `include/resource.h` | Dialog/control/menu identifiers. |
| `res/resource.rc`, `res/app.manifest`, `res/app.ico` | Dialogs, icon, version metadata, common controls v6, asInvoker and DPI manifest. |

The architecture has a useful worker/UI ownership split: the worker collects only the active tab, publishes owned buffers under locks, and posts a notification. The UI copies shared models before sorting or displaying them. The tree module is separable and directly testable. UI/control helpers live partly in `main.c`, so application files are not individually reusable libraries without stubs or additional extraction.

## Validated findings

### P2 — Filtering discards collapse state for still-running processes

At `src/tabs/tab_processes.c:572`, `ProcTree_ApplyContext` removes rows unrelated to the current search. At line 579, `ProcCollapse_Prune` then prunes against that filtered array. Its implementation at `src/tabs/proc_tree.c:312` removes every identity absent from the supplied rows. Collapse branch A, search for unrelated branch B, and clear the search: A has lost its collapsed state even though its process never exited. This also contradicts the nearby intent to retain collapse entries while searching. Prune against the complete collected process identities before applying visibility filters; add a regression for collapse → unrelated search → clear.

### P2 — Internal tree allocation failures bypass the flat fallback

At `src/tabs/proc_tree.c:135`, `ProcTree_ApplyContext` resets `*firstRoot` to -1 before allocating its keep mask. If that allocation fails at line 138, it returns the original count with no usable root. Independently, `ProcTree_Flatten` returns zero when its stack allocation fails at line 223. `ProcBuildTree` accepts both outcomes and returns success unconditionally after flattening (`src/tabs/tab_processes.c:589`). The view retains a non-null tree, so the list uses zero visible rows rather than the promised flat fallback. An ordered-flatten failure can also yield a header-only CSV while the model contains rows. Preserve the root on an unchanged-model return and propagate allocation failure explicitly to the tree builder. The current fallback test turns tree mode off; it does not inject allocation failures.

### P2 — Minimize can hide the application without a tray icon

`TrayAdd` sets `g_trayShown` only after `Shell_NotifyIconW(NIM_ADD)` succeeds (`src/main.c:429`). The minimize handler nevertheless calls `ShowWindow(hwnd, SW_HIDE)` unconditionally afterward (`src/main.c:1432`). If the shell rejects icon creation, the app disappears from ordinary taskbar/tray access. The Explorer-restart path likewise does not recover visibility if re-adding the icon fails (`src/main.c:1589`). Hide only after successful tray creation; otherwise keep the minimized window accessible. Add an injected tray-add failure test.

## Build and compiler constraints

- The application is Windows-only, uses `wWinMain`, wide Win32 APIs and Unicode common-control macros. CMake must retain `UNICODE` and `_UNICODE`, a Windows GUI target, and the Unicode CRT entry-point choice for MinGW. C11 is appropriate: the code uses compound literals, mixed declarations and declarations inside `for` loops.
- Link resources into the executable. The RC file resolves `app.manifest` and `app.ico` from `res`, and `resource.h` from `include`; out-of-source builds need explicit include/resource search paths. Avoid a conflicting autogenerated manifest when embedding this existing manifest.
- GDI+ and NT exports are dynamically loaded; those do not require static `gdiplus` or `ntdll` linkage. The custom GDI+ loader (`src/ui.c:86`) walks a structure of typed function pointers through `FARPROC *`; its size assertion checks layout size but does not establish ISO C aliasing/array semantics. A future portability cleanup should assign the typed members individually. This review did not demonstrate a compiler miscompilation.
- Do not treat the manifest's Vista compatibility entry as verified runtime support: `GetActiveProcessorGroupCount` and `GetActiveProcessorCount` are directly imported (`src/sysinfo.c:219`, `src/tabs/tab_processes.c:146`) despite nearby dynamic fallback code. The headers set a Windows 10 API baseline. Legacy OS support requires its own import/runtime audit.
- `test_network.c` and `test_services_users.c` rely on `assert`, including calls with side effects inside assertions. Test targets must keep assertions enabled in Release configurations, or those paths disappear while the executable reports success.
- Many test files include production `.c` files directly to replace APIs and inspect static state. Do not also link those same production translation units into the corresponding test executable. Keep ordinary application translation units separate; unity builds would collide on repeated static identifiers such as `g_shared` and `s_page` across tab modules.
- Process/export and workspace fixtures use relative `tests/.build` paths. A test working directory must contain that directory or the fixtures must accept an output path. Out-of-source test results should be placed under the build tree. Independent configurations should not write the same snapshot files concurrently.

## Test coverage and limits

| Fixture | Existing coverage |
| --- | --- |
| `test_settings.c` | Registry string termination/types/capacity, extreme rectangles, tree default. |
| `test_collector.c` | Injected event/thread/wait failures, cleanup/restart, pause boundary, CPU deltas and history wrap. |
| `test_sysinfo_live.c` | Real sampling, pause/refresh, per-CPU histories and repeated restart. |
| `test_proctree.c` | Parent links, PID reuse, cycles, aggregation, context filtering, sort/flatten and collapse identities. |
| `test_processes.c` | Search/filter and CSV encoding, process identities, native/Toolhelp acquisition, user filtering, selection, tree integration. |
| `test_apps.c` | Window eligibility, hung probes, stale identities, sorting/selection and injected window actions. |
| `test_services_users.c` | Real enumeration plus injected action/access and session-identity boundaries. |
| `test_network.c` | Counter normalization/reset/disconnect, ring history and real enumeration. |
| `test_launch.c` | System-tool paths, shell launch failures, elevated handoff and existing-instance startup. |
| `test_workspace.c` | Real controls, search/selection, pause/refresh, six pages, keyboard traversal, resize/tiny/DPI; writes screenshots. |

The suite provides useful regression coverage but is not entirely deterministic or headless. Live process/service/adapter checks depend on the host and permissions; workspace checks use fixed sleeps and compare process counts while collection continues. The tree reveal integration test explicitly skips when no suitable live parent/child pair exists. Screenshot files are captured without pixel-baseline assertions; their presence alone does not prove visual correctness.

Allocation failure inside the tree transform, tray failure/recovery, metadata lookup stalls, general Run-dialog parsing, full persistence round trips, GDI+ fallback and hardware beyond 256 logical CPUs do not have focused regression coverage. `SysInfo_Stop` waits indefinitely for the collector; the collector can be inside version-resource/account lookup APIs, so the shutdown-time tradeoff remains unbounded even though shared state is not released prematurely. No numeric line/branch coverage was collected.
