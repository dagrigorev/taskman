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

/* Probes PhysicalDrive0..31 and fills 'out' with one entry per drive that
   reported a usable temperature. Drives that cannot be opened, do not
   support the property, or return a malformed descriptor are skipped
   silently -- an absent drive is not an error. Returns the count. */
int Sensors_ReadDrives(SensorReading *out, int max);

/* Installs a predicate the drive scan consults before each drive; returning
   TRUE abandons the rest of the scan and keeps what was already collected.
   Shutdown joins the collector thread with an infinite wait, so a drive
   that is slow to answer would hold up application exit. Passed as a hook
   rather than calling SysInfo_Stopping directly so this module stays
   linkable into a headless suite on its own, as its elevation query is.
   NULL, the default, means the scan always runs to completion. */
void Sensors_SetCancelCheck(BOOL (*cancelled)(void));

/* Queries root\WMI for ACPI thermal zones. Returns 0 immediately when the
   process is not elevated: MSAcpi_ThermalZoneTemperature is Access Denied
   without administrator, so attempting it would be a guaranteed failure
   every five seconds. Shares SensorReading with the drives so both can be
   appended into one array. */
int Sensors_ReadZones(SensorReading *out, int max);

/* TRUE when the process is running elevated. Queried inside this module
   rather than through main.c's App_IsElevated so sensors.c stays linkable
   into a headless suite on its own, as gpu.c is. */
BOOL Sensors_IsElevated(void);

/* Samples every source and republishes the model. Throttled to
   SENSORS_COLLECT_INTERVAL_MS; called from the collector thread only. */
void Sensors_Collect(ULONGLONG nowTick);

/* Shared-lock read of the published model, mirroring Gpu_Lock/Gpu_Unlock.
   The returned pointer is valid until Sensors_Unlock. */
const SensorReading *Sensors_Lock(int *count);
void Sensors_Unlock(void);

/* Clears the published model. Safe from any thread. */
void Sensors_Reset(void);

/* Releases the COM apartment this module may have initialised. MUST be
   called on the collector thread, immediately before it exits: COM
   apartments are per-thread, so calling it anywhere else would leave the
   collector's apartment standing and decrement some other thread's COM
   reference count instead. Does nothing when no apartment was entered,
   which is every unelevated run. */
void Sensors_ThreadDetach(void);

/* Test-only introspection: lets the headless suite observe the throttle
   without waiting five real seconds. Not used by any production path. */
ULONGLONG Sensors_Collect_LastTick(void);

#endif /* CTM_SENSORS_H */
