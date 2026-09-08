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

#endif /* CTM_SENSORS_H */
