# GPU statistics and temperature sensors

Add GPU utilisation, video memory and device temperatures, on a new Sensors
tab, plus a per-process GPU column on the Processes tab.

Everything here runs unelevated, through components that ship with Windows.
No kernel driver, no vendor SDK, no new link-time dependency.

**This spec covers four units and should become four plans, in this order.**
Each produces working software on its own, and each later one depends on the
earlier: (1) GPU acquisition, (2) the Sensors tab and the seventh-tab
mechanics, (3) temperature acquisition, (4) the per-process GPU column. Trying
to implement all four from one plan would produce a document whose later
decisions invalidate its earlier ones.

## What a spike established

The design rests on measurements taken on the development machine rather than
on assumptions. They are recorded here because several of them contradict the
obvious guess.

| Probe | Result |
|---|---|
| `MSAcpi_ThermalZoneTemperature` | Class exists; querying instances is **Access Denied** unelevated |
| Storage temperature IOCTL | **Works unelevated** with a zero-access handle. Two NVMe drives reported 43/43/51 °C and 40/40/42 °C, critical 85, warning 82 |
| `\GPU Engine(*)\Utilization Percentage` | **Works unelevated**, instance names carry pid, adapter LUID, engine index and engine type |
| `\GPU Adapter Memory(*)\Dedicated Usage` | Works unelevated |
| Counter scale | **1029 instances**, 46 pids, 11 engine types, **3 adapter LUIDs** |

Two consequences that shape everything below. There are **three adapters**, so
a single "GPU %" is meaningless and every figure is per-adapter. And there is
**no pre-aggregated counter**, so even one machine-wide number requires walking
all 1029 instances — the enumeration cost is unavoidable for any GPU reading,
not just the per-process column.

## Decisions

Settled before design; not open for reinterpretation during implementation.

1. **GPU data comes from PDH.** Documented, ships with Windows, and the same
   source Task Manager uses. It yields per-adapter, per-engine and per-process
   from one query. `D3DKMTQueryStatistics` was considered and rejected: it is
   undocumented and cannot produce per-process figures at all.
2. **A seventh tab.** GPU and temperatures live on a new Sensors tab rather
   than being squeezed into the already-dense Performance tab.
3. **Drive temperatures always; ACPI thermal zones only when elevated.** When
   not elevated the ACPI row is shown as requiring administrator, not hidden —
   a silently absent feature is worse than an explained one.
4. **CPU-die and GPU temperature are out of scope.** Both require either a
   signed kernel driver or a vendor SDK, and both would break the project's
   stated "no runtime dependencies beyond what ships with Windows" identity.

## Metric definitions

Getting these wrong produces plausible-looking nonsense, so they are stated
exactly.

A GPU Engine counter instance name has the shape:

```
pid_<pid>_luid_0x<high>_0x<low>_phys_<n>_eng_<n>_engtype_<type>
```

- An **engine** is the tuple `(luid, phys, eng, engtype)`.
- **Engine utilisation** = the **sum** over every pid's instance of that engine.
  One engine shared by three processes is busy by the total of their shares.
- **Adapter utilisation** = the **maximum** across that adapter's engines. This
  matches Task Manager. Summing engines instead would let an adapter read well
  over 100 %: the probe measured 0.55 % summed against 0.45 % max.
- **Per-process GPU** = the sum of that pid's instances across all engines and
  all adapters, clamped to 100.

Adapter identity is the LUID. PDH renders it lowercase as
`luid_0x%08lx_0x%08lx` with **HighPart first, then LowPart**.

## Acquisition

### GPU — `src/gpu.c` + `include/gpu.h`

A module with no Win32 UI calls, testable headlessly, following the boundary
`proc_tree.c` already establishes.

**Counter names must be added with `PdhAddEnglishCounterW`, not
`PdhAddCounterW`.** The latter takes *localised* names, and the development
machine runs a Russian-language Windows where `GPU Engine` does not exist under
that name. This is not a nicety; the feature silently returns nothing without it.

Query lifecycle:

- `PdhOpenQueryW(NULL, 0, &query)` once, owned by the collector thread.
- Add `\GPU Engine(*)\Utilization Percentage` and
  `\GPU Adapter Memory(*)\Dedicated Usage` as wildcard counters.
- `Utilization Percentage` is a rate counter: the **first**
  `PdhCollectQueryData` yields no usable value, so the first sample after
  startup is discarded rather than reported as zero.
- Read with `PdhGetFormattedCounterArrayW(PDH_FMT_DOUBLE, ...)`, which returns
  each instance's name alongside its value.

**Wildcard counters do not pick up instances created after the counter was
added.** A game launched after the query was opened would never appear. The
counter is therefore re-added on a **rebuild interval of 10 seconds**, which
bounds how long a new process stays invisible without paying re-expansion cost
on every tick. This is the single easiest thing to get wrong here.

Adapter **names** are not available from PDH. They come from DXGI:
`CreateDXGIFactory1` resolved through `GetProcAddress` on `dxgi.dll` (keeping
the no-load-time-dependency property), then `IDXGIAdapter1::GetDesc1` for
`Description`, `DedicatedVideoMemory` and `AdapterLuid`, matched to the PDH
LUID. C code uses `COBJMACROS`. If DXGI is unavailable the adapter is listed by
LUID rather than dropped.

### Cadence — and a measurement the plan must take

GPU collection runs only when it is needed: the Sensors tab is active, **or**
the per-process GPU column is enabled on the Processes tab. Otherwise it is
skipped entirely, matching how tab models are already collected on demand.

It runs inside `CollectorProc` **before** `g_tabCollect`, so that
`Proc_Collect` can stamp fresh per-process figures into `ProcRow` in the same
tick rather than lagging one behind.

The column's enabled state is UI-thread state in `g_cfg`, which the collector
must not read directly — the codebase already forbids that. The Processes tab
calls `Gpu_SetEnabled(BOOL)` when the column is toggled, and that stores
through `InterlockedExchange`, mirroring how `SysInfo_SetActiveTab` already
publishes the active tab to the worker.

When it does run, it is rate-limited to **at most once per 1000 ms**, so the
High update speed (500 ms) does not double the cost.

The plan must **measure the native `PdhCollectQueryData` +
`PdhGetFormattedCounterArrayW` cost at this instance count before committing**
to that cadence. The PowerShell probe took 4.8 s, but almost all of that is
`Get-Counter`'s own sampling interval and object marshalling, so it is not
evidence about the native path. Budget: **10 ms per collection**. If the
measurement exceeds it, lengthen the interval rather than abandon PDH — the
readings are slow-moving and a 2 s cadence is perfectly usable.

### Temperatures — `src/sensors.c` + `include/sensors.h`

**Storage.** Probe `\\.\PhysicalDrive0..31`, opening each with
`dwDesiredAccess = 0`, which is sufficient for property queries and is what
makes this work unelevated. For each drive that opens:

- `IOCTL_STORAGE_QUERY_PROPERTY` with `StorageDeviceProperty` for a friendly
  name (vendor and product ID) to label the row.
- `IOCTL_STORAGE_QUERY_PROPERTY` with `StorageDeviceTemperatureProperty` (52)
  for `STORAGE_TEMPERATURE_DATA_DESCRIPTOR`.

**The descriptor must be parsed from its own self-description, never from
fixed offsets.** The spike observed a **16-byte entry stride** where the
documented `STORAGE_TEMPERATURE_INFO` is 10 bytes, so hardcoding either number
is wrong. Required parsing:

```
stride = (Size - headerBytes) / InfoCount
reject unless InfoCount > 0
reject unless stride >= sizeof(STORAGE_TEMPERATURE_INFO)
reject unless headerBytes + stride * InfoCount == Size
reject unless Size <= bytes actually returned
```

A descriptor failing any check is reported as unavailable rather than parsed
optimistically. Use the SDK's declarations under MSVC and hand-declare for
MinGW, following the `ntapi.h` precedent.

A drive reports several sensors. The row shows the **highest** of them — the
question a user is asking is "is anything hot" — with the sensor count beside
it, and the descriptor's own `WarningTemperature` and `CriticalTemperature`
driving the row's colour rather than hardcoded thresholds.

**ACPI thermal zones**, elevation-gated. `root\WMI` →
`MSAcpi_ThermalZoneTemperature`, `CurrentTemperature` in tenths of a kelvin, so
`°C = value / 10 - 273.15`. Attempted only when `App_IsElevated()` returns
TRUE. This needs COM on the collector thread: `CoInitializeEx(NULL,
COINIT_MULTITHREADED)` once on first use, and `CoUninitialize` from
`Sensors_Reset` — which the host already calls on the collector's own thread
during shutdown, after `SysInfo_Stop` has joined it. Initialising and
uninitialising per sample would be both wasteful and wrong. This is distinct
from the apartment-threaded initialisation `ProcOpenLocation` performs on the
UI thread; the two must not be conflated.

Both temperature sources are sampled at **most once per 5000 ms**. Drive
temperatures move slowly, opening a handle per drive per tick is real I/O, and
a WMI query costs tens of milliseconds.

## Data model

`Snapshot` is copied wholesale under a lock and holds only fixed-size scalars.
GPU adapters, drives and thermal zones are all variable-length, so they follow
the **existing per-tab model pattern** — a module-owned array published under
its own SRWLOCK, exactly as `Proc`, `Net`, `Svc` and `Users` already do. No
`Snapshot` fields are added.

```c
typedef struct {
    ULONGLONG luid;              /* HighPart << 32 | LowPart              */
    WCHAR     name[128];         /* DXGI Description, or a LUID string    */
    double    utilization;       /* max across this adapter's engines     */
    double    engine[GPU_ENGINE_KINDS];  /* per-type, summed over pids    */
    /* GPU_ENGINE_KINDS is a fixed set of the engine types worth showing:
       3d, copy, videodecode, videoencode, videoprocessing, compute, other.
       The probe observed eleven distinct engtype strings including an empty
       one and vendor-specific names (ofa_0, security_1, vr, legacyoverlay).
       Unrecognised and empty types fold into `other` rather than being
       dropped, so an adapter's maximum is never understated. */
    ULONGLONG dedicatedUsed, dedicatedTotal, sharedUsed;
    BOOL      nameKnown;
} GpuAdapter;

typedef struct {
    WCHAR name[128];
    int   celsius, warning, critical, sensorCount;
    BOOL  thresholdsKnown;
} SensorReading;
```

The per-process figures are published separately as a pid-keyed array that
`Proc_Collect` reads while stamping `ProcRow`, since both run on the collector
thread and GPU collection is ordered before tab collection.

## User interface

### The Sensors tab

A seventh tab. Contents, top to bottom:

- **Adapter list** — name, utilisation, dedicated memory used against total,
  following the Networking tab's adapter-list idiom including its sortable
  columns and selection-preserving refresh.
- **History graph** for the selected adapter, reusing `UI_Chart` and
  `UI_ChartLine`, which are already antialiased. `gpu.c` keeps its own small
  per-adapter ring of 128 samples, sized like the Networking tab's history
  rather than the 1024-sample CPU ring: GPU is sampled at most once per second
  and only while it is needed, so a longer ring would mostly hold gaps.
- **Temperature list** — device, current temperature, warning and critical
  thresholds, and a status cell. Rows exceeding warning or critical are shaded
  using the descriptor's own thresholds.
- When not elevated, a single explanatory row states that CPU thermal zones
  require administrator, rather than the section being silently absent.

Tiny footprint mode shows the history graph only, matching Performance and
Networking.

### Seventh-tab mechanics

- `TAB_COUNT` becomes 7 and a `TAB_SENSORS` enumerator is added.
- The tab strip divides by `TAB_COUNT`, so labels get narrower; the label must
  still fit at 100 % DPI.
- **`UI_DrawNavigation` keeps its own hardcoded `names[]` array duplicating
  `TabPage::title`.** Adding a seventh tab without touching it would render a
  wrong or out-of-bounds label. This is a pre-existing drift hazard and is
  fixed as part of this work by drawing the labels from the `TabPage` titles
  the host already owns, rather than by adding a seventh string to the
  duplicate.
- The persisted `activeTab` is already clamped to `TAB_COUNT`, so existing
  settings remain valid.
- `BuildMenuBar` and `TabCollect` gain the new case.

### Per-process GPU column

`ProcRow` gains `float gpuPct` and `BOOL gpuKnown`. The column joins the
existing Select Columns mechanism and is **off by default**, because enabling
it is what makes the collector pay the GPU enumeration cost.

It interacts with the process tree: `ProcTreeInfo` gains `gpuRollup`, and
`ProcTree_Aggregate` folds it exactly as it folds CPU. A collapsed parent
showing rolled-up CPU but not rolled-up GPU would be inconsistent.

## Non-goals

- CPU-die temperature, GPU temperature, fan speeds, voltages.
- Any kernel driver or vendor SDK.
- Per-process video memory. `\GPU Process Memory` could supply it; it is left
  out to keep this scope finishable.
- Historical temperature graphs. Temperatures are sampled every five seconds
  and are not interesting as a trace at that resolution.

## Edge cases

| Case | Behaviour |
|---|---|
| No GPU counters (pre-1709, or provider disabled) | Adapter list empty with an explanatory row; no error dialog |
| DXGI unavailable | Adapters listed by LUID, all figures still shown |
| Adapter appears or disappears | Picked up at the next 10 s counter rebuild |
| First GPU sample after startup | Discarded; rate counters need two collections |
| Drive rejects the temperature IOCTL | That drive is omitted; others still listed |
| Malformed temperature descriptor | Reported unavailable, never parsed on assumed offsets |
| Not elevated | Storage temperatures shown; ACPI row explains it needs administrator |
| Sensors tab never opened and GPU column off | No PDH query is opened at all |
| Allocation failure in any collector | Retain the previous model, as the existing tabs already do |

## Testing

Headless, in the `#include`-the-unit style the suites already use:

| Case | Assertion |
|---|---|
| Instance-name parsing | pid, LUID, engine index and engine type extracted from real observed strings, including the malformed and empty engtype the probe saw |
| LUID formatting | Round-trips `luid_0x00000000_0x00012cae` in both directions |
| Engine aggregation | Sum per engine, then max per adapter — a fixture where sum and max differ, so a max/sum swap fails |
| Per-process totals | Summed across engines and adapters, clamped at 100 |
| Temperature descriptor | Parses a synthetic 16-byte-stride buffer; **rejects** ones whose `Size`, `InfoCount` and stride disagree, and one whose `Size` exceeds the bytes returned |
| Threshold colouring | Driven by descriptor values, not constants |
| Cadence | GPU is not collected when the Sensors tab is inactive and the column is off |

A live suite, following `test_sysinfo_live`, opens a real PDH query and asserts
it returns instances and that a second collection yields usable values — the
only way to catch the English-counter-name and two-collection traps.

The workspace fixture gains the seventh tab: reachable, renders, survives tiny
mode and the DPI passes.

## Risks

1. **Native PDH cost at 1029 instances is unmeasured.** The plan measures it
   before committing to the 1 s cadence. Mitigation if it is slow is a longer
   interval, not a different API.
2. **Localised counter names.** `PdhAddCounterW` would silently find nothing on
   this very machine. `PdhAddEnglishCounterW` is mandatory.
3. **Wildcard instances are not automatically refreshed**, so without the 10 s
   rebuild new processes never appear — and the bug looks like "sometimes it
   works".
4. **Storage descriptor stride differs from the documented struct.** Parsing
   must be self-describing and validated.
5. **A seventh tab narrows the tab strip** and touches the tab host, the
   navigation painter and the menu builder.
6. **COM on the collector thread** for WMI must not disturb the UI thread's
   existing apartment-threaded use.
