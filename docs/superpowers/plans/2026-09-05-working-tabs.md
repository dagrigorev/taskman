# Working Task Manager Implementation Plan

> For agentic workers: use subagent-driven-development with independent file
> ownership, focused tests and review. This folder has no Git metadata; retain
> source backups and review file diffs instead of commits/worktrees.

**Goal:** implement the six existing tabs and verify them interactively.
**Spec:** ../specs/2026-09-05-working-tabs-design.md
**Tech stack:** Unicode Win32 C11, Windows system APIs, MinGW/MSVC, no third-party runtime.

## Global interfaces and constraints

Parent owns app.h, main.c, sysinfo.c, Makefile, build.bat and tests/run.ps1.
Each tab exports `void Xxx_Collect(void)` called solely by the worker and
`void Xxx_Reset(void)` called by the UI after the worker exits. Prefixes are
`Proc`, `Apps`, `Svc`, `Users`, `Net`. Existing TabPage callbacks remain intact.
Process tab also exports `void Proc_SelectPid(DWORD pid)`; parent provides
`void App_ShowProcess(DWORD pid)` to switch tabs and select after refresh.
Parent provides `void App_ReportError(HWND owner, const WCHAR *operation, DWORD error)`.
No collector callback reads UI globals/settings or manipulates HWND controls.
No worker edits another worker's files or spawns further workers.

## Task 1: processes

Own tab_processes.c and tests/test_processes.c; add process-specific files only
if needed. First test self PID enumeration and PID+creation-time identity/delta
helpers, then implement collector/model and list actions. Tests must cover
sorting, creation-time reuse, missing access, and selection surviving refresh.
Compile the owned C modules with `gcc -std=gnu11 -DUNICODE -D_UNICODE -Wall
-Wextra -Werror -c`. Return full test link instructions to parent. Do not execute
destructive operations except on a private disposable child owned by the test.

## Task 2: applications

Own tab_apps.c and tests/test_apps.c. Add a bounded EnumWindows collector,
snapshot copy, visible-window filtering and timeout-based hung detection. Test
with a private window fixture; then implement sort, selection, close/switch,
view settings and all Windows menu actions. Use normal icon/list controls,
release copied icons and never block indefinitely on another process's window.
Export Apps_Collect/Apps_Reset. Compile/test owned files and report commands.

## Task 3: services and users

Own tab_services.c, tab_users.c and tests/test_services_users.c. Implement SCM
and WTS collectors with retained error state, snapshot copy and deterministic
sorting. UI actions must copy selected stable identifiers before modal dialogs,
use minimal rights, report failures, and confirm destructive actions. Use
App_ShowProcess for service PID navigation. Use svc_ prefixes for all services
static identifiers because existing launch tests include this file. Test
read-only enumeration and action boundaries with stubs; never stop real
services or disconnect/logoff real sessions. Export Svc/Users_Collect/Reset.

## Task 4: collector, performance and networking (parent)

Add SysInfo_SetTabCollector callback before Start, invoke from worker and wake
when active tab changes. Extend CPU history by logical CPU with bounded native
performance queries. Add reusable graph drawing and gauges, then implement
network adapter snapshots/history with GetIfTable2. Test delta/reset/wrap math,
per-CPU history and live adapter enumeration. Retain all old collector tests.

## Task 5: integration and manual verification

Integrate exports/cleanup and tab selection; update build/test runners for new
files. Run existing and new tests, GCC analyzer, and release build. Review all
changed boundary code and resolve concrete findings. Through the running GUI
verify all tabs populate, sorting and selection, refresh speeds/pause/F5,
window/tray behavior, New Task, system tools, graph options and persistence.
Test process/window termination only on a disposable fixture. Save results and
explicit privileged-runtime exclusions in MANUAL_TEST_RESULTS.md.
