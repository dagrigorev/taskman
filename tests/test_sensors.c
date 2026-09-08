/* Synthetic descriptors: the acceptance path and every rejection the
   device could provoke. The real drive path is test_sensors_live.c. */
#include "../include/sensors.h"
#include <stdio.h>
#include <winioctl.h>
#include "../src/sensors.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

/* STORAGE_TEMPERATURE_VALUE_NOT_REPORTED as a SHORT. Written as the
   negative literal because casting 0x8000 down truncates the constant. */
#define SENSOR_NOT_REPORTED ((short)-32768)

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
    const short temps[] = {SENSOR_NOT_REPORTED, -274, 38, 200};
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
    const short temps[] = {SENSOR_NOT_REPORTED, SENSOR_NOT_REPORTED};
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
        (unsigned)sizeof(STORAGE_TEMPERATURE_INFO), temps, -274, SENSOR_NOT_REPORTED, 0);
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
