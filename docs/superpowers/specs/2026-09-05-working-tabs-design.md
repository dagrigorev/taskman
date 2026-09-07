# Complete the existing Task Manager

The user explicitly approved implementing the missing functionality after a
manual check found empty tabs and graph placeholders. Keep the existing classic
Win32 C interface, six tabs, registry settings and dependency-free executable.

## Architecture

The existing collector thread invokes an optional tab collection callback after
sampling system counters. Each tab owns an SRWLOCK-protected model, collects only
on the worker and copies model rows on the UI thread before updating controls.
No UI APIs or g_cfg reads on the collector: read-only settings affecting model
presentation are applied by the UI; worker flags must use interlocked access.
Tab switching wakes the collector even when paused to obtain initial data.
All model resources are freed only after SysInfo_Stop joins the worker.

## Features

- Applications: enumerate visible top-level app windows, display titles and
  responsiveness, preserve selection, sort, switch/activate, close selected task
  with confirmation, New Task, icons/detail modes, and Windows arrangement menu.
- Processes: PID/name/user/CPU/memory/description rows, accurate CPU deltas with
  process creation identity, sort, column selection, full account names and
  all-users filtering, confirmed termination and useful context actions. Refuse
  PID reuse, self/system/critical termination and report access errors.
- Services: SCM enumeration with name/PID/description/state, sorting, start/stop
  context actions with permission errors, select service process, Services console.
- Users: WTS sessions, identity/state/client/session rows, sorting, confirmed
  disconnect/logoff and a Send Message dialog. No actual user logoff during tests.
- Performance: CPU/memory gauges, continuous history graphs with grid, optional
  kernel series, all-CPU and per-CPU views and the existing memory/system numbers.
- Networking: adapters, link state/speed and bandwidth utilization plus history
  graphs for total/sent/received; counter resets and disconnected adapters do not
  produce spikes. Tiny mode has visible graphs as primary content.

## Validation

Test data collection, sorting/identity and history math with deterministic
fixtures plus real read-only OS sampling. Keep prior audit tests passing. Build
with MinGW warnings as errors, run GCC analysis, then exercise all available UI
flows through Computer Use. Termination tests target only our disposable test
process. Never stop unrelated services or log off real sessions; those runtime
checks require a disposable environment. Record any untested privileged action
honestly. New test executables stay under tests/.build.
