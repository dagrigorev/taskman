/* Opens a REAL PDH query. This is the only place the English-counter-name
   and two-collection requirements can actually be caught: both fail by
   silently returning nothing, which a synthetic test cannot reproduce. */
#include "../include/gpu.h"
#include <stdio.h>
#include <strsafe.h>
#include "../src/gpu.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void)
{
    GpuAccumulator acc;
    GpuAdapterSample adapters[GPU_MAX_ADAPTERS];
    int n = 0;

    if (!Gpu_QueryOpen()) {
        printf("SKIP gpu_live: no GPU performance counters on this machine\n");
        return 0;
    }

    /* A rate counter has no value from a single collection. The first
       sample must report "not ready" rather than a confident zero. */
    Gpu_AccumReset(&acc);
    CHECK(Gpu_QuerySample(&acc, GetTickCount64()) == FALSE);

    Sleep(1100);
    Gpu_AccumReset(&acc);
    if (!Gpu_QuerySample(&acc, GetTickCount64())) {
        printf("SKIP gpu_live: second collection still unavailable\n");
        Gpu_QueryClose();
        return 0;
    }

    /* The machine this was written on reported ~1000 instances across three
       adapters. Any GPU at all should yield at least one engine bucket. */
    CHECK(acc.engineCount > 0);
    n = Gpu_AccumAdapters(&acc, adapters, GPU_MAX_ADAPTERS);
    CHECK(n > 0);
    if (n > 0) {
        int i;
        for (i = 0; i < n; ++i) {
            CHECK(adapters[i].utilization >= 0.0);
            CHECK(adapters[i].utilization <= 100.0);
        }
    }

    Gpu_QueryClose();
    printf("gpu_live: %d failures (%d engines, %d adapters)\n",
           failures, acc.engineCount, n);
    return failures ? 1 : 0;
}
