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
            int j;
            for (j = 0; j < z; ++j) {
                CHECK(zones[j].name[0] != L'\0');
                CHECK(zones[j].celsius >= SENSOR_TEMP_MIN);
                CHECK(zones[j].celsius <= SENSOR_TEMP_MAX);
                printf("sensors_live: zone %ls = %d C\n",
                       zones[j].name, zones[j].celsius);
            }
        }
    }
    printf("sensors_live: %d failures (%d drives)\n", failures, n);
    return failures ? 1 : 0;
}
