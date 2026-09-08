# Temperature Acquisition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read storage-drive temperatures unelevated, and ACPI thermal zones when elevated, and show them on the Sensors tab with thresholds driving the row colour.

**Architecture:** A new module `src/sensors.c` + `include/sensors.h` follows the shape `gpu.c` already established: pure parsing helpers that a headless suite can drive with synthetic buffers, an acquisition layer over them, and a published model behind its own SRWLOCK that the page copies into a private view. The collector thread calls `Sensors_Collect` alongside `Gpu_Collect`, throttled to one sample per 5 s. Storage temperatures come from `IOCTL_STORAGE_QUERY_PROPERTY` on a **zero-access** drive handle, which is what makes them work without elevation. ACPI zones come from WMI's `root\WMI` namespace and are attempted only when the process is elevated, since the query is Access Denied otherwise.

**Tech Stack:** Win32 C11, `winioctl.h` (both toolchains declare the temperature structures — see below), WMI via COM (`wbemuuid`), comctl32 list view with `NM_CUSTOMDRAW` row shading. CMake 3.25 + Ninja + CTest, MSVC `/W4 /WX` and MinGW `-Wall -Wextra -Werror`.

**Spec:** `docs/superpowers/specs/2026-09-07-gpu-and-temperatures-design.md` — this plan implements **Unit 3** of four. Units 1 (GPU acquisition) and 2 (Sensors tab) are merged. Unit 4 (per-process GPU column) follows.

## Global Constraints

- **No third-party libraries and no runtime dependencies beyond what ships with Windows.** This is why CPU-die and GPU temperatures are out of scope: both need a signed kernel driver or a vendor SDK.
- **C11, MSVC's default C mode.** No `_Static_assert`; use the negative-array-bound typedef idiom already in `src/ui.c` if a compile-time assertion is wanted.
- **Both toolchains must build clean at `-Werror` / `/WX`.** Run both on every task. Past defects were found by one and not the other in both directions.
- **All 12 existing CTest suites must stay green**, plus the two this plan adds. `test_workspace` creates real windows and fails intermittently for environmental reasons — re-run once before treating a failure as real.
- **Commit messages carry no `Co-Authored-By` line and no AI attribution trailer of any kind.** Subject and body only.
- **No `Snapshot` fields are added.** Temperature readings are variable-length and live behind `sensors.c`'s own lock, exactly as the GPU model does.
- **Sampling interval is 5000 ms** (`SENSORS_COLLECT_INTERVAL_MS`). Drive temperatures move slowly, a handle per drive per tick is real I/O, and a WMI query costs tens of milliseconds.

## Measurements taken before writing this plan

Run on the development machine with the probe in the scratchpad, unelevated. **Two of these correct the spec** — the spec's own text should be read against them, not the other way round.

| Fact | Value | Consequence |
|---|---|---|
| `sizeof(STORAGE_TEMPERATURE_INFO)` | **16** | The spec says the documented struct is 10 bytes and the observed stride 16, and mandates self-describing parsing to resolve the contradiction. **There is no contradiction** — the spike miscounted the struct. The stride matches `sizeof` exactly. |
| `FIELD_OFFSET(descriptor, TemperatureInfo)` | **24** | `sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR)` is **40**, because it embeds one `ANYSIZE_ARRAY` entry. Using `sizeof` as the header size is an off-by-16 that silently reads the wrong entries. |
| MinGW `winioctl.h` | declares `StorageDeviceTemperatureProperty`, `STORAGE_TEMPERATURE_INFO`, `STORAGE_TEMPERATURE_DATA_DESCRIPTOR` | The spec's "hand-declare for MinGW, following the `ntapi.h` precedent" is **not needed**. Both toolchains have identical declarations. Do not add a shim. |
| Drive 0 descriptor | `Version=40 Size=72 InfoCount=3 crit=85 warn=82`, sensors 43/43/53 °C | Real fixture data for the headless suite. |
| Drive 1 descriptor | `Version=40 Size=72 InfoCount=3 crit=85 warn=82`, sensors 42/42/43 °C | Two drives on this machine; drives 2..7 fail to open and are skipped. |
| Per-sensor `OverThreshold` | `82` on index 0, **`-274`** on indices 1 and 2 | −274 °C is below absolute zero. Per-sensor thresholds are unreliable; the **descriptor-level** `WarningTemperature`/`CriticalTemperature` are the trustworthy ones, which is what the spec already says to colour by. Values must still be range-checked. |
| Per-sensor `UnderThreshold` | `-32768` | `STORAGE_TEMPERATURE_VALUE_NOT_REPORTED` (`0x8000`) read as a `SHORT`. Any field can carry this sentinel. |
| Zero-access handle, unelevated | both drives answered | Confirms the spec's central claim; no elevation needed for storage. |

**Self-describing parsing is still what this plan implements** — it is cheap, and it is correct defensive practice against a descriptor from a device driver. What changes is the justification: it guards against a malformed or hostile descriptor, not against a documented-versus-real mismatch that does not exist.

## Decisions locked for this unit

1. **`sensors.c` queries its own elevation** rather than calling `App_IsElevated` from `main.c`. `App_IsElevated` is thread-safe and would work, but linking `main.c` into a headless unit suite is far worse than a ten-line token query. `gpu.c` is self-contained for the same reason.
2. **A drive row shows the highest of its sensors**, with the sensor count beside it. The question a user is asking is "is anything hot".
3. **Sentinel and out-of-range values are dropped, not displayed.** A sensor reporting `0x8000`, or a temperature outside −60..150 °C, is not a reading. A drive whose every sensor is unusable is reported unavailable rather than as 0 °C.
4. **The ACPI row is always present when unelevated**, saying it needs administrator. A silently absent feature is worse than an explained one — the spec's decision 3.
5. **ACPI is the last task** so it can be rejected or deferred on its own without touching the storage path, which is the part that works for every user.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `include/sensors.h` | **Create** | The temperature contract: `SensorReading`, parsing and model entry points |
| `src/sensors.c` | **Create** | Descriptor parsing, drive enumeration, WMI zones, published model |
| `tests/test_sensors.c` | **Create** | Headless: synthetic descriptors, acceptance and rejection |
| `tests/test_sensors_live.c` | **Create** | Opens real drives, following `test_gpu_live` |
| `tests/CMakeLists.txt` | Modify | Register both suites; link `sensors.c` where fixtures need it |
| `CMakeLists.txt` | Modify | `TASKMAN_SUPPORT_SOURCES`, `wbemuuid` link |
| `src/sysinfo.c` | Modify (near the `Gpu_Collect` call, ~line 345) | Call `Sensors_Collect` on the collector thread |
| `src/main.c` | Modify (~line 1591) | `Sensors_Reset()` beside `Gpu_Reset()` |
| `include/ui.h` | Modify | `UI_WARN` / `UI_CRIT` palette entries |
| `include/resource.h` | Modify | `IDC_SENS_TEMPLIST` |
| `src/tabs/tab_sensors.c` | Modify | Third layout band, temperature list, threshold shading |
| `tests/test_workspace.c` | Modify | The temperature list renders and is bounded |

---

### Task 1: Parse the temperature descriptor

Pure function over a caller-supplied buffer, so every acceptance and rejection case is reachable headlessly. This is the task that carries the real risk: a wrong header offset or stride reads plausible-looking garbage rather than failing.

**Files:**
- Create: `include/sensors.h`
- Create: `src/sensors.c`
- Create: `tests/test_sensors.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `STORAGE_TEMPERATURE_DATA_DESCRIPTOR`, `STORAGE_TEMPERATURE_INFO`, `STORAGE_TEMPERATURE_VALUE_NOT_REPORTED` from `<winioctl.h>` — declared identically by both toolchains, so no shim.
- Produces: `SensorReading` and `BOOL Sensors_ParseTemperature(const void *buffer, DWORD returned, SensorReading *out);` — Tasks 2 and 4 use both.

- [ ] **Step 1: Write the header**

Create `include/sensors.h`:

```c
/* Temperature sensors: storage drives always, ACPI thermal zones when the
   process is elevated. Both are published behind this module's own lock,
   following the per-tab model pattern rather than adding Snapshot fields. */
#ifndef CTM_SENSORS_H
#define CTM_SENSORS_H

#include <windows.h>

#define SENSOR_NAME_MAX  128
#define SENSORS_MAX      64
#define SENSORS_COLLECT_INTERVAL_MS 5000

/* Temperatures outside this range are not readings. The storage descriptor
   uses 0x8000 as "not reported", and the development machine was observed
   returning -274 C -- below absolute zero -- in per-sensor threshold
   fields, so every value from the device is range-checked. */
#define SENSOR_TEMP_MIN  (-60)
#define SENSOR_TEMP_MAX  (150)

typedef struct {
    WCHAR name[SENSOR_NAME_MAX];
    int   celsius;          /* highest sensor on the device               */
    int   warning, critical;
    int   sensorCount;      /* usable sensors the device reported         */
    BOOL  thresholdsKnown;
} SensorReading;

/* Parses a STORAGE_TEMPERATURE_DATA_DESCRIPTOR from its own self-description.
   'returned' is the byte count DeviceIoControl actually wrote. Fills only
   celsius, warning, critical, sensorCount and thresholdsKnown; the caller
   owns the name. Returns FALSE, leaving *out untouched, for any descriptor
   that fails validation or carries no usable sensor. */
BOOL Sensors_ParseTemperature(const void *buffer, DWORD returned,
                              SensorReading *out);

#endif /* CTM_SENSORS_H */
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_sensors.c`. The helper builds a descriptor with a caller-chosen stride so the stride-dependent cases are reachable:

```c
/* Synthetic descriptors: the acceptance path and every rejection the
   device could provoke. The real drive path is test_sensors_live.c. */
#include "../include/sensors.h"
#include <stdio.h>
#include <winioctl.h>
#include "../src/sensors.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

#define HEADER_BYTES ((unsigned)FIELD_OFFSET( \
    STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo))

/* Builds a descriptor into 'buffer'. 'stride' is written into Size via the
   count, so a caller can produce both consistent and inconsistent ones.
   Returns the byte count a matching DeviceIoControl would report. */
static DWORD BuildDescriptor(BYTE *buffer, size_t capacity, int count,
                             unsigned stride, const short *temps,
                             short warning, short critical, DWORD sizeOverride)
{
    STORAGE_TEMPERATURE_DATA_DESCRIPTOR *desc =
        (STORAGE_TEMPERATURE_DATA_DESCRIPTOR *)buffer;
    DWORD size = HEADER_BYTES + stride * (unsigned)count;
    int i;
    if (size > capacity) return 0;
    ZeroMemory(buffer, capacity);
    desc->Version = (DWORD)sizeof(*desc);
    desc->Size = sizeOverride ? sizeOverride : size;
    desc->CriticalTemperature = critical;
    desc->WarningTemperature = warning;
    desc->InfoCount = (WORD)count;
    for (i = 0; i < count; ++i) {
        STORAGE_TEMPERATURE_INFO *info =
            (STORAGE_TEMPERATURE_INFO *)(buffer + HEADER_BYTES + stride * (unsigned)i);
        info->Index = (WORD)i;
        info->Temperature = temps[i];
    }
    return size;
}

static void TestParsesRealDescriptorShape(void)
{
    /* Exactly what PhysicalDrive0 returned on the development machine:
       three sensors at 43/43/53, warning 82, critical 85, stride 16. */
    BYTE buffer[256];
    const short temps[] = {43, 43, 53};
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 3,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), temps, 82, 85, 0);
    CHECK(returned == 72);          /* 24 header + 3 * 16 */
    ZeroMemory(&out, sizeof(out));
    CHECK(Sensors_ParseTemperature(buffer, returned, &out));
    CHECK(out.celsius == 53);       /* the highest, not the first */
    CHECK(out.sensorCount == 3);
    CHECK(out.warning == 82);
    CHECK(out.critical == 85);
    CHECK(out.thresholdsKnown);
}

static void TestUsesSelfDescribedStride(void)
{
    /* A device padding its entries wider than the struct must still parse:
       the entries are found by the descriptor's own stride, not sizeof. */
    BYTE buffer[256];
    const short temps[] = {30, 61};
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 2,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO) + 8, temps, 70, 90, 0);
    ZeroMemory(&out, sizeof(out));
    CHECK(Sensors_ParseTemperature(buffer, returned, &out));
    CHECK(out.celsius == 61);
    CHECK(out.sensorCount == 2);
}

static void TestRejectsShortStride(void)
{
    /* A stride below the struct would make each entry overlap the next. */
    BYTE buffer[256];
    const short temps[] = {40, 40};
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 2,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO) - 4, temps, 70, 90, 0);
    ZeroMemory(&out, sizeof(out));
    CHECK(!Sensors_ParseTemperature(buffer, returned, &out));
    CHECK(out.celsius == 0);        /* untouched on rejection */
}

static void TestRejectsInconsistentSize(void)
{
    /* Size, InfoCount and stride must agree exactly. A Size that is not
       header + stride * count means the descriptor is not what it claims. */
    BYTE buffer[256];
    const short temps[] = {40, 41, 42};
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 3,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), temps, 70, 90, 0);
    ((STORAGE_TEMPERATURE_DATA_DESCRIPTOR *)buffer)->Size = returned - 3;
    ZeroMemory(&out, sizeof(out));
    CHECK(!Sensors_ParseTemperature(buffer, returned, &out));
}

static void TestRejectsSizeBeyondReturned(void)
{
    /* Size larger than the bytes actually written would read off the end
       of the buffer. This is the one that matters for memory safety. */
    BYTE buffer[256];
    const short temps[] = {40, 41, 42};
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 3,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), temps, 70, 90, 0);
    ZeroMemory(&out, sizeof(out));
    CHECK(!Sensors_ParseTemperature(buffer, returned - 1, &out));
}

static void TestRejectsZeroInfoCount(void)
{
    BYTE buffer[256];
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 0,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), NULL, 70, 90, 0);
    ZeroMemory(&out, sizeof(out));
    CHECK(!Sensors_ParseTemperature(buffer, returned, &out));
}

static void TestRejectsTruncatedHeader(void)
{
    BYTE buffer[256];
    SensorReading out;
    ZeroMemory(buffer, sizeof(buffer));
    ZeroMemory(&out, sizeof(out));
    CHECK(!Sensors_ParseTemperature(buffer, HEADER_BYTES - 1, &out));
    CHECK(!Sensors_ParseTemperature(NULL, 72, &out));
    CHECK(!Sensors_ParseTemperature(buffer, 72, NULL));
}

static void TestSkipsSentinelAndImplausibleSensors(void)
{
    /* 0x8000 is STORAGE_TEMPERATURE_VALUE_NOT_REPORTED as a SHORT, and the
       development machine returned -274 C in threshold fields -- below
       absolute zero. Neither is a reading, and neither may become the
       reported maximum. */
    BYTE buffer[256];
    const short temps[] = {(short)0x8000, -274, 38, 200};
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 4,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), temps, 70, 90, 0);
    ZeroMemory(&out, sizeof(out));
    CHECK(Sensors_ParseTemperature(buffer, returned, &out));
    CHECK(out.celsius == 38);       /* 200 is out of range, not the max */
    CHECK(out.sensorCount == 1);    /* only one usable sensor of four */
}

static void TestRejectsWhenNoSensorIsUsable(void)
{
    /* Every sensor unreadable is "unavailable", never a confident 0 C. */
    BYTE buffer[256];
    const short temps[] = {(short)0x8000, (short)0x8000};
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 2,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), temps, 70, 90, 0);
    ZeroMemory(&out, sizeof(out));
    CHECK(!Sensors_ParseTemperature(buffer, returned, &out));
}

static void TestImplausibleThresholdsAreUnknown(void)
{
    /* Thresholds drive the row colour, so a nonsense pair must read as
       "unknown" rather than colouring every drive critical. */
    BYTE buffer[256];
    const short temps[] = {45};
    SensorReading out;
    DWORD returned = BuildDescriptor(buffer, sizeof(buffer), 1,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), temps, -274, (short)0x8000, 0);
    ZeroMemory(&out, sizeof(out));
    CHECK(Sensors_ParseTemperature(buffer, returned, &out));
    CHECK(out.celsius == 45);
    CHECK(!out.thresholdsKnown);

    /* Warning above critical is also incoherent. */
    returned = BuildDescriptor(buffer, sizeof(buffer), 1,
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), temps, 90, 70, 0);
    ZeroMemory(&out, sizeof(out));
    CHECK(Sensors_ParseTemperature(buffer, returned, &out));
    CHECK(!out.thresholdsKnown);
}

int main(void)
{
    /* The stride the development machine reported equals the struct
       exactly. The spec claimed the documented struct was 10 bytes and the
       two disagreed; they do not. Assert it so a toolchain that ever does
       disagree fails here rather than reading garbage. */
    CHECK(sizeof(STORAGE_TEMPERATURE_INFO) == 16);
    CHECK(HEADER_BYTES == 24);
    TestParsesRealDescriptorShape();
    TestUsesSelfDescribedStride();
    TestRejectsShortStride();
    TestRejectsInconsistentSize();
    TestRejectsSizeBeyondReturned();
    TestRejectsZeroInfoCount();
    TestRejectsTruncatedHeader();
    TestSkipsSentinelAndImplausibleSensors();
    TestRejectsWhenNoSensorIsUsable();
    TestImplausibleThresholdsAreUnknown();
    printf("sensors: %d failures\n", failures);
    return failures ? 1 : 0;
}
```

Register it in `tests/CMakeLists.txt`, beside `taskman_add_test(test_gpu)`. The suite `#include`s `sensors.c`, so do not also link it:

```cmake
taskman_add_test(test_sensors)
```

- [ ] **Step 3: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: compile FAIL — `sensors.c` does not exist yet.

- [ ] **Step 4: Write the implementation**

Create `src/sensors.c`:

```c
/* ------------------------------------------------------------------------
 * sensors.c - temperature acquisition.
 *
 * Storage drives are readable without elevation through a zero-access
 * handle, which is the whole reason this feature can ship to ordinary
 * users. ACPI thermal zones need administrator and are gated on it.
 * ------------------------------------------------------------------------ */
#include "sensors.h"
#include <winioctl.h>
#include <strsafe.h>

/* Both MSVC's SDK and MinGW's headers declare StorageDeviceTemperatureProperty,
   STORAGE_TEMPERATURE_INFO and STORAGE_TEMPERATURE_DATA_DESCRIPTOR
   identically, so unlike ntapi.h this module needs no hand-declared shim. */

static BOOL SensorPlausible(int celsius)
{
    return celsius >= SENSOR_TEMP_MIN && celsius <= SENSOR_TEMP_MAX;
}

BOOL Sensors_ParseTemperature(const void *buffer, DWORD returned,
                              SensorReading *out)
{
    const STORAGE_TEMPERATURE_DATA_DESCRIPTOR *desc = buffer;
    /* The entries begin at the member offset, NOT at sizeof(descriptor):
       the descriptor embeds one ANYSIZE_ARRAY entry, so sizeof is 40 while
       the header is 24. Using sizeof here reads entry 1 as entry 0. */
    const DWORD headerBytes =
        (DWORD)FIELD_OFFSET(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo);
    DWORD stride;
    int best = 0, usable = 0;
    unsigned i;
    BOOL found = FALSE;

    if (!buffer || !out || returned < headerBytes) return FALSE;
    if (desc->InfoCount == 0) return FALSE;
    /* Size is the device's own claim; it must not exceed what was written,
       or the entry walk below reads past the end of the buffer. */
    if (desc->Size > returned || desc->Size < headerBytes) return FALSE;

    stride = (desc->Size - headerBytes) / desc->InfoCount;
    if (stride < sizeof(STORAGE_TEMPERATURE_INFO)) return FALSE;
    /* Exact agreement, so a descriptor whose fields merely round to a
       plausible stride is rejected rather than parsed. */
    if (headerBytes + stride * desc->InfoCount != desc->Size) return FALSE;

    for (i = 0; i < desc->InfoCount; ++i) {
        const STORAGE_TEMPERATURE_INFO *info =
            (const STORAGE_TEMPERATURE_INFO *)((const BYTE *)buffer +
                                               headerBytes + stride * i);
        int celsius = info->Temperature;
        if (info->Temperature == (SHORT)STORAGE_TEMPERATURE_VALUE_NOT_REPORTED)
            continue;
        if (!SensorPlausible(celsius)) continue;
        if (!found || celsius > best) best = celsius;
        found = TRUE;
        ++usable;
    }
    if (!found) return FALSE;   /* unavailable, never a confident zero */

    out->celsius     = best;
    out->sensorCount = usable;
    out->warning     = desc->WarningTemperature;
    out->critical    = desc->CriticalTemperature;
    /* Per-sensor thresholds were observed at -274 C on real hardware, so
       the descriptor-level pair is range-checked too, and must be
       coherent, before it is allowed to colour a row. */
    out->thresholdsKnown = SensorPlausible(out->warning) &&
                           SensorPlausible(out->critical) &&
                           out->warning <= out->critical;
    return TRUE;
}
```

- [ ] **Step 5: Run both toolchains**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain msvc test
```

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: both clean, `test_sensors` reports 0 failures, 13/13 suites PASS.

- [ ] **Step 6: Commit**

```bash
git add include/sensors.h src/sensors.c tests/test_sensors.c tests/CMakeLists.txt
git commit -m "feat: parse the storage temperature descriptor defensively

The descriptor is walked by its own declared stride and validated for
internal consistency before any entry is read, so a malformed one is
reported unavailable rather than parsed on assumed offsets. The entries
start at the member offset, not at sizeof(descriptor), which embeds one
ANYSIZE_ARRAY element and is 16 bytes longer. Sentinel and out-of-range
values are dropped rather than reported as temperatures."
```

---

### Task 2: Read the drives

**Files:**
- Modify: `include/sensors.h`, `src/sensors.c`
- Create: `tests/test_sensors_live.c`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Sensors_ParseTemperature` from Task 1.
- Produces: `int Sensors_ReadDrives(SensorReading *out, int max);` — fills `out` with one entry per drive that answered and returns the count. Task 3 calls it.

- [ ] **Step 1: Write the failing live test**

Create `tests/test_sensors_live.c`. Following `test_gpu_live`, it skips rather than fails on a machine with no readable drive, since CI may have none:

```c
/* Opens REAL drives. The zero-access-handle requirement and the IOCTL's
   actual behaviour cannot be reproduced synthetically: getting the access
   mask wrong fails with ERROR_ACCESS_DENIED only on a real device. */
#include "../include/sensors.h"
#include <stdio.h>
#include <winioctl.h>
#include "../src/sensors.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void)
{
    SensorReading drives[SENSORS_MAX];
    int n, i;

    n = Sensors_ReadDrives(drives, SENSORS_MAX);
    CHECK(n >= 0);
    if (n == 0) {
        printf("SKIP sensors_live: no drive reported a temperature\n");
        return failures ? 1 : 0;
    }
    for (i = 0; i < n; ++i) {
        CHECK(drives[i].name[0] != L'\0');
        CHECK(drives[i].sensorCount > 0);
        /* A drive that answered must be in a plausible range: this is the
           assertion that fails if the stride or header offset is wrong,
           because a misread entry lands far outside it. */
        CHECK(drives[i].celsius >= SENSOR_TEMP_MIN);
        CHECK(drives[i].celsius <= SENSOR_TEMP_MAX);
        if (drives[i].thresholdsKnown)
            CHECK(drives[i].warning <= drives[i].critical);
        printf("sensors_live: %ls = %d C (%d sensors, warn %d, crit %d%s)\n",
               drives[i].name, drives[i].celsius, drives[i].sensorCount,
               drives[i].warning, drives[i].critical,
               drives[i].thresholdsKnown ? "" : ", thresholds unknown");
    }
    printf("sensors_live: %d failures (%d drives)\n", failures, n);
    return failures ? 1 : 0;
}
```

Register it:

```cmake
taskman_add_test(test_sensors_live)
```

- [ ] **Step 2: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: compile FAIL, `implicit declaration of function 'Sensors_ReadDrives'`.

- [ ] **Step 3: Implement the drive walk**

Declare in `include/sensors.h`, above the `#endif`:

```c
/* Probes PhysicalDrive0..31 and fills 'out' with one entry per drive that
   reported a usable temperature. Drives that cannot be opened, do not
   support the property, or return a malformed descriptor are skipped
   silently -- an absent drive is not an error. Returns the count. */
int Sensors_ReadDrives(SensorReading *out, int max);
```

Add to `src/sensors.c`:

```c
#define SENSORS_MAX_PHYSICAL_DRIVES 32

/* Builds "VENDOR PRODUCT" from the device descriptor's offsets, which are
   byte offsets into the same buffer and are zero when absent. */
static void SensorDriveName(const STORAGE_DEVICE_DESCRIPTOR *desc, DWORD returned,
                            int index, WCHAR *out, size_t cch)
{
    const char *vendor = NULL, *product = NULL;
    if (desc->VendorIdOffset && desc->VendorIdOffset < returned)
        vendor = (const char *)desc + desc->VendorIdOffset;
    if (desc->ProductIdOffset && desc->ProductIdOffset < returned)
        product = (const char *)desc + desc->ProductIdOffset;
    if (vendor || product) {
        WCHAR wide[SENSOR_NAME_MAX];
        char narrow[SENSOR_NAME_MAX];
        StringCchPrintfA(narrow, ARRAYSIZE(narrow), "%s%s%s",
                         vendor ? vendor : "", (vendor && product) ? " " : "",
                         product ? product : "");
        if (MultiByteToWideChar(CP_ACP, 0, narrow, -1, wide, ARRAYSIZE(wide))) {
            /* Vendor and product are space padded in the descriptor. */
            size_t end = wcslen(wide);
            while (end > 0 && wide[end - 1] == L' ') wide[--end] = L'\0';
            if (wide[0]) { StringCchCopyW(out, cch, wide); return; }
        }
    }
    StringCchPrintfW(out, cch, L"PhysicalDrive%d", index);
}

static BOOL SensorQueryDrive(HANDLE drive, int index, SensorReading *out)
{
    STORAGE_PROPERTY_QUERY query;
    BYTE buffer[1024];
    DWORD returned = 0;

    ZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceTemperatureProperty;
    query.QueryType  = PropertyStandardQuery;
    ZeroMemory(buffer, sizeof(buffer));
    if (!DeviceIoControl(drive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query), buffer, sizeof(buffer),
                         &returned, NULL))
        return FALSE;
    if (!Sensors_ParseTemperature(buffer, returned, out)) return FALSE;

    /* The name is only worth fetching once the temperature is known good. */
    ZeroMemory(&query, sizeof(query));
    query.PropertyId = StorageDeviceProperty;
    query.QueryType  = PropertyStandardQuery;
    ZeroMemory(buffer, sizeof(buffer));
    if (DeviceIoControl(drive, IOCTL_STORAGE_QUERY_PROPERTY,
                        &query, sizeof(query), buffer, sizeof(buffer),
                        &returned, NULL) &&
        returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR))
        SensorDriveName((const STORAGE_DEVICE_DESCRIPTOR *)buffer, returned,
                        index, out->name, SENSOR_NAME_MAX);
    else
        StringCchPrintfW(out->name, SENSOR_NAME_MAX, L"PhysicalDrive%d", index);
    return TRUE;
}

int Sensors_ReadDrives(SensorReading *out, int max)
{
    int index, count = 0;
    if (!out || max <= 0) return 0;
    for (index = 0; index < SENSORS_MAX_PHYSICAL_DRIVES && count < max; ++index) {
        WCHAR path[64];
        HANDLE drive;
        SensorReading reading;
        StringCchPrintfW(path, ARRAYSIZE(path), L"\\\\.\\PhysicalDrive%d", index);
        /* Zero desired access is what makes this work without elevation:
           property queries need no read or write right on the device, and
           asking for one would fail for an ordinary user. */
        drive = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
        if (drive == INVALID_HANDLE_VALUE) continue;
        ZeroMemory(&reading, sizeof(reading));
        if (SensorQueryDrive(drive, index, &reading)) out[count++] = reading;
        CloseHandle(drive);
    }
    return count;
}
```

- [ ] **Step 4: Run both toolchains**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain msvc test
```

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: 14/14 PASS. Then run the live suite directly to read its output, since CTest hides stdout for passing tests:

```bash
./build/msvc-release/bin/Release/test_sensors_live.exe
```

Expected on the development machine: two drives, around 53 °C and 43 °C, warning 82, critical 85. **If a drive reports a temperature far outside 20..90 °C, stop** — that is the signature of a wrong stride or header offset, not a hot drive.

- [ ] **Step 5: Commit**

```bash
git add include/sensors.h src/sensors.c tests/test_sensors_live.c tests/CMakeLists.txt
git commit -m "feat: read drive temperatures through a zero-access handle

Property queries need no read or write right on the device, so probing
PhysicalDrive0..31 with a desired access of zero works for an ordinary
user -- asking for any access would fail unelevated and is the whole
reason this ships without administrator. Drives that cannot be opened or
answer with a malformed descriptor are skipped rather than reported."
```

---

### Task 3: Publish the model and sample it on the collector thread

**Files:**
- Modify: `include/sensors.h`, `src/sensors.c`
- Modify: `src/sysinfo.c` (beside the `Gpu_Collect` call, ~line 345)
- Modify: `src/main.c` (~line 1591, beside `Gpu_Reset`)
- Modify: `CMakeLists.txt` (`TASKMAN_SUPPORT_SOURCES`, line 59)
- Modify: `tests/CMakeLists.txt` (fixtures that link `sysinfo.c` or `main.c`)
- Test: `tests/test_sensors.c`

**Interfaces:**
- Consumes: `Sensors_ReadDrives` from Task 2.
- Produces:
  - `void Sensors_Collect(ULONGLONG nowTick);` — throttled to `SENSORS_COLLECT_INTERVAL_MS`; called from the collector thread only.
  - `const SensorReading *Sensors_Lock(int *count);` / `void Sensors_Unlock(void);` — shared-lock read, mirroring `Gpu_Lock`.
  - `BOOL Sensors_IsElevated(void);` — Task 5 and the page's explanatory row use it.
  - `void Sensors_Reset(void);` — called on shutdown from the collector's own thread.

Task 4 calls `Sensors_Lock`/`Sensors_Unlock`/`Sensors_IsElevated`.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_sensors.c` and call from `main`:

```c
static void TestCollectThrottles(void)
{
    const SensorReading *model;
    int first = 0, second = 0;

    /* A fresh module has published nothing. */
    Sensors_Reset();
    model = Sensors_Lock(&first); (void)model; Sensors_Unlock();
    CHECK(first == 0);

    Sensors_Collect(1000000);
    model = Sensors_Lock(&first); (void)model; Sensors_Unlock();

    /* Well inside the interval: the model must not be resampled. A second
       collection here would mean every collector tick pays the drive I/O. */
    CHECK(Sensors_Collect_LastTick() == 1000000);
    Sensors_Collect(1000000 + SENSORS_COLLECT_INTERVAL_MS - 1);
    CHECK(Sensors_Collect_LastTick() == 1000000);

    /* Past the interval it samples again. */
    Sensors_Collect(1000000 + SENSORS_COLLECT_INTERVAL_MS);
    if (first > 0) CHECK(Sensors_Collect_LastTick() ==
                         1000000 + SENSORS_COLLECT_INTERVAL_MS);

    model = Sensors_Lock(&second); (void)model; Sensors_Unlock();
    CHECK(second >= 0);
    Sensors_Reset();
    model = Sensors_Lock(&second); (void)model; Sensors_Unlock();
    CHECK(second == 0);            /* Reset clears the published model */
}
```

`Sensors_Collect_LastTick` is test-only introspection, declared in `sensors.h` beside the `ProcTest_*` precedent in `app.h`:

```c
/* Test-only introspection: lets the headless suite observe the throttle
   without waiting five real seconds. Not used by any production path. */
ULONGLONG Sensors_Collect_LastTick(void);
```

- [ ] **Step 2: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: compile FAIL, `implicit declaration of function 'Sensors_Collect'`.

- [ ] **Step 3: Implement the model**

Add to `src/sensors.c`:

```c
/* ------------------------------------------------------- published model -- */

static SRWLOCK      s_lock = SRWLOCK_INIT;
static SensorReading s_model[SENSORS_MAX];
static int          s_modelCount;
static ULONGLONG    s_lastCollect;

BOOL Sensors_IsElevated(void)
{
    /* Queried here rather than through main.c's App_IsElevated so this
       module stays linkable into a headless suite on its own. */
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD size;
    BOOL result = FALSE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    if (GetTokenInformation(token, TokenElevation, &elevation,
                            sizeof(elevation), &size))
        result = elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return result;
}

void Sensors_Collect(ULONGLONG nowTick)
{
    SensorReading readings[SENSORS_MAX];
    int count;

    /* Drive temperatures move slowly and a handle per drive per tick is
       real I/O, so this runs at a fifth of the collector's cadence. */
    if (s_lastCollect && nowTick - s_lastCollect < SENSORS_COLLECT_INTERVAL_MS)
        return;
    s_lastCollect = nowTick;

    ZeroMemory(readings, sizeof(readings));
    count = Sensors_ReadDrives(readings, SENSORS_MAX);

    AcquireSRWLockExclusive(&s_lock);
    CopyMemory(s_model, readings, sizeof(s_model));
    s_modelCount = count;
    ReleaseSRWLockExclusive(&s_lock);
}

ULONGLONG Sensors_Collect_LastTick(void) { return s_lastCollect; }

const SensorReading *Sensors_Lock(int *count)
{
    AcquireSRWLockShared(&s_lock);
    if (count) *count = s_modelCount;
    return s_model;
}

void Sensors_Unlock(void)
{
    ReleaseSRWLockShared(&s_lock);
}

void Sensors_Reset(void)
{
    AcquireSRWLockExclusive(&s_lock);
    ZeroMemory(s_model, sizeof(s_model));
    s_modelCount = 0;
    ReleaseSRWLockExclusive(&s_lock);
    s_lastCollect = 0;
}
```

Declare all five in `include/sensors.h`.

- [ ] **Step 4: Wire the collector and the shutdown**

In `src/sysinfo.c`, add `#include "sensors.h"` beside the `gpu.h` include, and immediately after the `Gpu_Collect` call:

```c
        Gpu_Collect(GetTickCount64());
        /* Unlike the GPU, temperatures are not gated on a tab: they are
           cheap at a five second cadence and the Sensors tab must have
           something to show the moment it opens. */
        Sensors_Collect(GetTickCount64());
```

In `src/main.c`, add `#include "sensors.h"` and, beside `Gpu_Reset()`:

```c
        Gpu_Reset();
        Sensors_Reset();
```

In `CMakeLists.txt`, add `src/sensors.c` to `TASKMAN_SUPPORT_SOURCES`. That list feeds both `taskman` and `test_workspace`.

In `tests/CMakeLists.txt`, add `../src/sensors.c` to the source lists of **`test_launch`** and **`test_sysinfo_live`**, which link `main.c`/`sysinfo.c` by hand. This is the same class of breakage `gpu.c` caused in unit 1 and `tab_sensors.c` caused in unit 2. Note that **MinGW will not catch a miss in `test_launch`** — gcc discards `main.c`'s static `CreatePages` when nothing calls it, so the reference disappears; only MSVC fails. Run MSVC before concluding the wiring is right.

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
git add include/sensors.h src/sensors.c src/sysinfo.c src/main.c CMakeLists.txt tests/CMakeLists.txt tests/test_sensors.c
git commit -m "feat: publish temperatures from the collector thread

Sampled at a fifth of the collector's cadence: drive temperatures move
slowly and opening a handle per drive per tick is real I/O. Unlike the GPU
this is not gated on a tab, because it is cheap at five seconds and the
Sensors tab should have something to show the moment it opens."
```

---

### Task 4: Show temperatures on the Sensors tab

**Files:**
- Modify: `include/ui.h` (palette)
- Modify: `include/resource.h`
- Modify: `src/tabs/tab_sensors.c`
- Test: `tests/test_workspace.c`

**Interfaces:**
- Consumes: `Sensors_Lock`, `Sensors_Unlock`, `Sensors_IsElevated`, `SensorReading` from Task 3; `UI_Str`, `UI_INK`, `UI_SURFACE` from the existing headers; the `NM_CUSTOMDRAW` sub-item shading pattern at `src/tabs/tab_processes.c:1289-1299`.
- Produces: nothing later tasks depend on. Task 5's ACPI rows appear in this same list, since `Sensors_ReadDrives`' output and the zone output share the `SensorReading` type.

- [ ] **Step 1: Add the palette entries and the control ID**

The palette has no warning or danger colour. In `include/ui.h`, after `UI_NAVY`:

```c
/* Threshold shading for the Sensors tab. Tinted backgrounds rather than
   coloured text, so a hot row reads at a glance without the number
   becoming hard to read against it. */
#define UI_WARN     RGB(255, 241, 217)
#define UI_CRIT     RGB(255, 218, 218)
```

In `include/resource.h`, beside the Sensors IDs:

```c
#define IDC_SENS_TEMPLIST               2602
```

- [ ] **Step 2: Write the failing test**

In `tests/test_workspace.c`, inside the existing Sensors block added by unit 2 — right after the adapter-list assertions, before `Capture`:

```c
        {
            /* The temperature list is populated by the collector rather
               than by the tab, so it is checked against the model the same
               way. A machine whose drives report nothing still shows the
               ACPI explanatory row when unelevated, so the list is never
               completely empty on an unelevated run. */
            HWND temps = GetDlgItem(TabSensors()->hwnd, IDC_SENS_TEMPLIST);
            const SensorReading *model;
            int drives = 0;
            CHECK(temps != NULL);
            CheckBounds(temps, TabSensors()->hwnd);
            model = Sensors_Lock(&drives); (void)model; Sensors_Unlock();
            if (drives > 0) {
                WCHAR name[128] = {0};
                ListView_GetItemText(temps, 0, 0, name, ARRAYSIZE(name));
                CHECK(name[0] != L'\0');
                fprintf(stderr, "workspace: %d temperature source(s), "
                                "first = %ls\n", drives, name);
            }
            if (!Sensors_IsElevated())
                CHECK(ListView_GetItemCount(temps) == drives + 1);
            else
                CHECK(ListView_GetItemCount(temps) >= drives);
        }
```

- [ ] **Step 3: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: FAIL on `CHECK(temps != NULL)` — the control does not exist yet.

- [ ] **Step 4: Create the list and give it a layout band**

In `src/tabs/tab_sensors.c`, add `#include "sensors.h"`, a `static HWND s_temps;`, and the model view:

```c
static SensorReading s_tempView[SENSORS_MAX];
static int s_tempCount;
static BOOL s_elevated;
```

Extend `SensCreate`:

```c
    s_temps = UI_CreateListView(p->hwnd, IDC_SENS_TEMPLIST, 0);
    if (s_temps) {
        UI_AddColumn(s_temps, 0, L"Sensor",       140, LVCFMT_LEFT);
        UI_AddColumn(s_temps, 1, L"Temperature",   72, LVCFMT_RIGHT);
        UI_AddColumn(s_temps, 2, L"Warning",       60, LVCFMT_RIGHT);
        UI_AddColumn(s_temps, 3, L"Critical",      60, LVCFMT_RIGHT);
        UI_AddColumn(s_temps, 4, L"Status",       110, LVCFMT_LEFT);
    }
```

`SensLayout` becomes three bands. The graph keeps roughly half, and the two lists split the rest evenly; tiny mode is unchanged and still shows the graph alone:

```c
static void SensLayout(TabPage *p, int cx, int cy, BOOL tiny)
{
    int margin = UI_Margin(p->hwnd);
    int width = cx - 2 * margin > 0 ? cx - 2 * margin : 1;
    int graphH, listH, y;

    if (tiny) {
        if (s_list)  ShowWindow(s_list,  SW_HIDE);
        if (s_temps) ShowWindow(s_temps, SW_HIDE);
        if (s_graphHost) {
            ShowWindow(s_graphHost, SW_SHOW);
            MoveWindow(s_graphHost, 0, 0, cx, cy, TRUE);
        }
        return;
    }
    if (s_list)  ShowWindow(s_list,  SW_SHOW);
    if (s_temps) ShowWindow(s_temps, SW_SHOW);

    /* Four margins now: above the graph, and between each pair. */
    graphH = (cy - 4 * margin) / 2;
    if (graphH < DPX(60)) graphH = DPX(60);
    listH = (cy - 4 * margin - graphH) / 2;
    if (listH < DPX(48)) listH = DPX(48);

    y = margin;
    if (s_graphHost) MoveWindow(s_graphHost, margin, y, width, graphH, TRUE);
    y += graphH + margin;
    if (s_list)      MoveWindow(s_list,      margin, y, width, listH, TRUE);
    y += listH + margin;
    if (s_temps)
        MoveWindow(s_temps, margin, y, width,
                   cy - margin - y > 0 ? cy - margin - y : 1, TRUE);
}
```

Add `s_temps = NULL;` to `SensDestroy`.

- [ ] **Step 5: Fill the list**

Add to `SensSnapshot`, after the adapter list is refreshed:

```c
    if (s_temps) {
        const SensorReading *model;
        int temps = 0, row;

        model = Sensors_Lock(&temps);
        if (temps > SENSORS_MAX) temps = SENSORS_MAX;
        if (temps > 0) CopyMemory(s_tempView, model,
                                  (size_t)temps * sizeof(*s_tempView));
        Sensors_Unlock();
        s_tempCount = temps;
        s_elevated = Sensors_IsElevated();

        s_refreshing = TRUE;
        SendMessageW(s_temps, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(s_temps);
        for (row = 0; row < temps; ++row) {
            WCHAR value[32], warn[32], crit[32], status[64];
            LVITEMW item = {0};
            const SensorReading *r = &s_tempView[row];
            StringCchPrintfW(value, ARRAYSIZE(value), L"%d \x00B0""C", r->celsius);
            if (r->thresholdsKnown) {
                StringCchPrintfW(warn, ARRAYSIZE(warn), L"%d \x00B0""C", r->warning);
                StringCchPrintfW(crit, ARRAYSIZE(crit), L"%d \x00B0""C", r->critical);
                StringCchPrintfW(status, ARRAYSIZE(status), L"%s (%d sensor%s)",
                    r->celsius >= r->critical ? L"Critical" :
                    r->celsius >= r->warning  ? L"Warning"  : L"Normal",
                    r->sensorCount, r->sensorCount == 1 ? L"" : L"s");
            } else {
                lstrcpyW(warn, L"Unknown"); lstrcpyW(crit, L"Unknown");
                StringCchPrintfW(status, ARRAYSIZE(status),
                    L"%d sensor%s, no thresholds reported",
                    r->sensorCount, r->sensorCount == 1 ? L"" : L"s");
            }
            item.mask = LVIF_TEXT; item.iItem = row;
            item.pszText = UI_Str(r->name);
            ListView_InsertItem(s_temps, &item);
            ListView_SetItemText(s_temps, row, 1, value);
            ListView_SetItemText(s_temps, row, 2, warn);
            ListView_SetItemText(s_temps, row, 3, crit);
            ListView_SetItemText(s_temps, row, 4, status);
        }
        if (!s_elevated) {
            /* Explained, not hidden: a silently absent feature reads as a
               bug. The row sits after the drives, which do work. */
            LVITEMW item = {0};
            item.mask = LVIF_TEXT; item.iItem = temps;
            item.pszText = UI_Str(L"CPU thermal zones");
            ListView_InsertItem(s_temps, &item);
            ListView_SetItemText(s_temps, temps, 4,
                UI_Str(L"Requires administrator"));
        }
        SendMessageW(s_temps, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(s_temps, NULL, TRUE);
        s_refreshing = FALSE;
    }
```

- [ ] **Step 6: Shade rows by their own thresholds**

Extend `sens_notify`. The stage sequence and the `CDRF_NEWFONT` return follow `tab_processes.c:1211-1300` exactly; the difference is that the colour comes from the descriptor's thresholds rather than from constants:

```c
    if (nm->hwndFrom == s_temps && nm->code == NM_CUSTOMDRAW) {
        NMLVCUSTOMDRAW *draw = (NMLVCUSTOMDRAW *)nm;
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
            *result = CDRF_NOTIFYITEMDRAW; return TRUE;
        }
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            int index = (int)draw->nmcd.dwItemSpec;
            COLORREF bg = index % 2 ? RGB(248, 250, 253) : UI_SURFACE;
            /* Thresholds come from the device, never from constants: an
               NVMe drive warns at 82 where a spinning disk may warn at 50,
               and a drive that reported no usable pair is left unshaded
               rather than coloured on a guess. */
            if (index >= 0 && index < s_tempCount &&
                s_tempView[index].thresholdsKnown) {
                const SensorReading *r = &s_tempView[index];
                if (r->celsius >= r->critical)     bg = UI_CRIT;
                else if (r->celsius >= r->warning) bg = UI_WARN;
            }
            draw->clrText = UI_INK; draw->clrTextBk = bg;
            *result = CDRF_NEWFONT; return TRUE;
        }
    }
```

Put this **before** the `nm->hwndFrom != s_list` early return, which would otherwise reject every notification from `s_temps`.

- [ ] **Step 7: Run both toolchains and look at it**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain msvc test
```

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Then read the fixture's own output and its capture:

```bash
./build/msvc-release/bin/Release/test_workspace.exe
```

Expected: 14/14 PASS, and `workspace: 2 temperature source(s)` on the development machine. Inspect `build/msvc-release/tests/.build/workspace-sensors.bmp`: three bands, drives listed with °C and thresholds, and the "Requires administrator" row present on an unelevated run.

- [ ] **Step 8: Commit**

```bash
git add include/ui.h include/resource.h src/tabs/tab_sensors.c tests/test_workspace.c
git commit -m "feat: show drive temperatures with threshold shading

The Sensors tab splits into graph, adapters and temperatures. Rows are
shaded from the descriptor's own warning and critical values rather than
from constants -- an NVMe drive warns at 82 where a spinning disk may warn
at 50 -- and a drive that reported no usable pair is left unshaded rather
than coloured on a guess. When not elevated the ACPI row states that it
needs administrator instead of being silently absent."
```

---

### Task 5: ACPI thermal zones, elevation-gated

Last, so it can be deferred without touching the storage path that works for every user. On an unelevated machine this task changes nothing a user sees — the explanatory row from Task 4 stays.

**Files:**
- Modify: `src/sensors.c`, `include/sensors.h`
- Modify: `CMakeLists.txt` (link `wbemuuid`)
- Test: `tests/test_sensors_live.c`

**Interfaces:**
- Consumes: `Sensors_IsElevated` from Task 3; `SensorReading`.
- Produces: `int Sensors_ReadZones(SensorReading *out, int max);` — returns 0 immediately when not elevated. `Sensors_Collect` appends its output after the drives.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_sensors_live.c`, before the final `printf`:

```c
    {
        /* Unelevated this must be a clean, fast zero rather than an error
           or a hang -- that is the case almost every user is in. */
        SensorReading zones[SENSORS_MAX];
        int z = Sensors_ReadZones(zones, SENSORS_MAX);
        CHECK(z >= 0);
        if (!Sensors_IsElevated()) {
            CHECK(z == 0);
            printf("sensors_live: not elevated; ACPI zones correctly skipped\n");
        } else {
            int i;
            for (i = 0; i < z; ++i) {
                CHECK(zones[i].name[0] != L'\0');
                CHECK(zones[i].celsius >= SENSOR_TEMP_MIN);
                CHECK(zones[i].celsius <= SENSOR_TEMP_MAX);
                printf("sensors_live: zone %ls = %d C\n",
                       zones[i].name, zones[i].celsius);
            }
        }
    }
```

- [ ] **Step 2: Run it to verify it fails**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

Expected: compile FAIL, `implicit declaration of function 'Sensors_ReadZones'`.

- [ ] **Step 3: Implement the WMI query**

In `src/sensors.c`, above the published model, add the includes and the query. `COBJMACROS` is what lets C call the interfaces, exactly as `gpu.c` does for DXGI:

```c
#define COBJMACROS
#include <wbemidl.h>
#include <objbase.h>
```

```c
/* ------------------------------------------------------------ ACPI zones -- */

/* COM is initialised once, on the collector thread, and torn down from
   Sensors_Reset -- which the host calls on that same thread during
   shutdown, after SysInfo_Stop has joined it. Initialising per sample
   would be both wasteful and wrong. This is multi-threaded apartment and
   is deliberately distinct from the apartment-threaded initialisation
   ProcOpenLocation performs on the UI thread; the two must not be
   conflated. */
static BOOL s_comReady;

static BOOL SensorComInit(void)
{
    HRESULT hr;
    if (s_comReady) return TRUE;
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    /* Already initialised on this thread with the same model is success. */
    if (hr == RPC_E_CHANGED_MODE) return FALSE;
    if (FAILED(hr) && hr != S_FALSE) return FALSE;
    s_comReady = TRUE;
    return TRUE;
}

int Sensors_ReadZones(SensorReading *out, int max)
{
    IWbemLocator  *locator = NULL;
    IWbemServices *services = NULL;
    IEnumWbemClassObject *rows = NULL;
    BSTR namespaceName = NULL, language = NULL, query = NULL;
    int count = 0;

    if (!out || max <= 0) return 0;
    /* The query is Access Denied without administrator, so it is not even
       attempted: a guaranteed failure per five seconds is pure cost. */
    if (!Sensors_IsElevated()) return 0;
    if (!SensorComInit()) return 0;

    if (FAILED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWbemLocator, (void **)&locator)) || !locator)
        return 0;

    namespaceName = SysAllocString(L"ROOT\\WMI");
    language      = SysAllocString(L"WQL");
    query         = SysAllocString(L"SELECT InstanceName, CurrentTemperature "
                                  L"FROM MSAcpi_ThermalZoneTemperature");
    if (!namespaceName || !language || !query) goto done;

    if (FAILED(IWbemLocator_ConnectServer(locator, namespaceName, NULL, NULL,
                                          NULL, 0, NULL, NULL, &services)) ||
        !services)
        goto done;
    /* Without this the enumeration fails with E_ACCESSDENIED even elevated. */
    if (FAILED(CoSetProxyBlanket((IUnknown *)services, RPC_C_AUTHN_WINNT,
                                 RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL,
                                 RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE)))
        goto done;

    if (FAILED(IWbemServices_ExecQuery(services, language, query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            NULL, &rows)) || !rows)
        goto done;

    while (count < max) {
        IWbemClassObject *row = NULL;
        ULONG returned = 0;
        VARIANT value;

        if (FAILED(IEnumWbemClassObject_Next(rows, 2000, 1, &row, &returned)) ||
            returned == 0 || !row)
            break;

        VariantInit(&value);
        if (SUCCEEDED(IWbemClassObject_Get(row, L"CurrentTemperature", 0,
                                           &value, NULL, NULL)) &&
            value.vt == VT_I4) {
            /* Tenths of a kelvin. */
            int celsius = (int)((double)value.lVal / 10.0 - 273.15);
            if (celsius >= SENSOR_TEMP_MIN && celsius <= SENSOR_TEMP_MAX) {
                SensorReading *reading = &out[count];
                VARIANT name;
                ZeroMemory(reading, sizeof(*reading));
                reading->celsius = celsius;
                reading->sensorCount = 1;
                reading->thresholdsKnown = FALSE;  /* ACPI reports none here */
                VariantInit(&name);
                if (SUCCEEDED(IWbemClassObject_Get(row, L"InstanceName", 0,
                                                   &name, NULL, NULL)) &&
                    name.vt == VT_BSTR && name.bstrVal)
                    StringCchPrintfW(reading->name, SENSOR_NAME_MAX,
                                     L"Thermal zone %s", name.bstrVal);
                else
                    StringCchPrintfW(reading->name, SENSOR_NAME_MAX,
                                     L"Thermal zone %d", count);
                VariantClear(&name);
                ++count;
            }
        }
        VariantClear(&value);
        IWbemClassObject_Release(row);
    }

done:
    if (rows)     IEnumWbemClassObject_Release(rows);
    if (services) IWbemServices_Release(services);
    if (locator)  IWbemLocator_Release(locator);
    SysFreeString(query); SysFreeString(language); SysFreeString(namespaceName);
    return count;
}
```

Declare `int Sensors_ReadZones(SensorReading *out, int max);` in `include/sensors.h`.

- [ ] **Step 4: Append zones to the published model and tear COM down**

In `Sensors_Collect`, after the drive read:

```c
    count = Sensors_ReadDrives(readings, SENSORS_MAX);
    if (count < SENSORS_MAX)
        count += Sensors_ReadZones(readings + count, SENSORS_MAX - count);
```

In `Sensors_Reset`, after the lock is released:

```c
    s_lastCollect = 0;
    /* The host calls this on the collector's own thread during shutdown,
       after SysInfo_Stop has joined it, so the apartment being torn down
       is the one SensorComInit created. */
    if (s_comReady) { CoUninitialize(); s_comReady = FALSE; }
```

In `CMakeLists.txt`, add `wbemuuid` to the link libraries beside `dxguid` — MSVC needs it for `CLSID_WbemLocator` and `IID_IWbemLocator`, the same way it needed `dxguid` for `IID_IDXGIFactory1` in unit 1.

- [ ] **Step 5: Run both toolchains**

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain msvc test
```

```bash
pwsh -NoProfile -File ./build.ps1 -Toolchain mingw test
```

```bash
./build/msvc-release/bin/Release/test_sensors_live.exe
```

Expected: 14/14 PASS, and `not elevated; ACPI zones correctly skipped` on an ordinary run. The elevated path cannot be exercised from this session; report that it is untested rather than claiming it works.

- [ ] **Step 6: Commit**

```bash
git add include/sensors.h src/sensors.c CMakeLists.txt tests/test_sensors_live.c
git commit -m "feat: read ACPI thermal zones when elevated

MSAcpi_ThermalZoneTemperature is Access Denied without administrator, so
the query is not attempted at all rather than failing once every five
seconds. COM is initialised once on the collector thread and torn down
from Sensors_Reset, which the host calls on that same thread after the
collector has been joined; per-sample initialisation would be both
wasteful and wrong. This apartment is multi-threaded and distinct from the
UI thread's apartment-threaded use."
```

---

## Self-Review

**Spec coverage.** The spec's "Temperatures" section maps task by task: zero-access handle probing `PhysicalDrive0..31` and the friendly name (Task 2); `StorageDeviceTemperatureProperty` and the full five-check validation list, verbatim (Task 1); highest sensor with the count beside it and descriptor-driven colour (Tasks 1 and 4); ACPI zones with the tenths-of-a-kelvin conversion, elevation gate, and once-per-thread COM matched to `Sensors_Reset` (Task 5); the 5000 ms cadence (Task 3). From the spec's UI section: the temperature list with device, current, warning, critical and status (Task 4), threshold shading (Task 4, Step 6), and the not-elevated explanatory row (Task 4, Step 5). From the edge-case table: drive rejects the IOCTL (Task 2, skipped silently), malformed descriptor (Task 1, six rejection tests), not elevated (Tasks 4 and 5). From the testing table: the descriptor parses a synthetic buffer and rejects disagreeing `Size`/`InfoCount`/stride and a `Size` beyond the bytes returned (Task 1, four dedicated tests), and threshold colouring is driven by descriptor values rather than constants (Task 4, Step 6, asserted structurally by leaving unknown-threshold rows unshaded).

**Where this plan overrides the spec, and why.** The spec mandates hand-declaring the structures for MinGW; measurement shows both toolchains declare them identically, so Task 1 explicitly says not to add a shim. The spec justifies self-describing parsing by a documented-10-bytes-versus-observed-16 contradiction; `sizeof` is 16 and there is no contradiction, so Task 1 keeps the parsing and restates the justification as defence against a malformed descriptor. Both are recorded in the Measurements table rather than silently applied.

**Deliberately out of scope**, per the spec's non-goals: CPU-die temperature, GPU temperature, fan speeds, voltages, and historical temperature graphs.

**Placeholder scan.** No "TBD", no "add error handling", no "similar to Task N". Every code step carries the code. The one judgement call — Task 2's "stop if a drive reads far outside 20..90 °C" — states the criterion and what it would mean.

**Type consistency.** `SensorReading` is defined once in Task 1 and its members (`name`, `celsius`, `warning`, `critical`, `sensorCount`, `thresholdsKnown`) are used with those names in Tasks 2, 4 and 5. `Sensors_ParseTemperature(const void *, DWORD, SensorReading *)` is declared in Task 1 and called with that signature in Task 2. `Sensors_ReadDrives(SensorReading *, int)` and `Sensors_ReadZones(SensorReading *, int)` share one shape, which is what lets Task 5 append into the same array. `Sensors_Lock(int *)` returns `const SensorReading *` and pairs with `Sensors_Unlock()`, mirroring `Gpu_Lock`/`Gpu_Unlock` from unit 1.

**Known risk this plan does not remove.** The elevated ACPI path cannot be exercised from an unelevated session. Task 5's live test asserts only the unelevated behaviour and says so; the elevated branch ships untested and must be reported that way, not claimed to work.
