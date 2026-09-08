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

#define SENSORS_MAX_PHYSICAL_DRIVES 32

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

/* --------------------------------------------------------------- drives -- */

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

/* ------------------------------------------------------- published model -- */

static SRWLOCK       s_lock = SRWLOCK_INIT;
static SensorReading s_model[SENSORS_MAX];
static int           s_modelCount;
static ULONGLONG     s_lastCollect;

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
