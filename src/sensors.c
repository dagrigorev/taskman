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
        /* Compared as the raw bit pattern: 0x8000 does not fit a SHORT,
           so casting the constant down would truncate it. */
        if ((WORD)info->Temperature == STORAGE_TEMPERATURE_VALUE_NOT_REPORTED)
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
