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
