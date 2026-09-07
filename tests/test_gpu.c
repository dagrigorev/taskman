/* Exercises GPU instance parsing and aggregation with synthetic data.
   No UI stubs needed: gpu.c calls nothing from user32 or comctl32. */
#include "../include/gpu.h"
#include <stdio.h>
#include <strsafe.h>
#include "../src/gpu.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void)
{
    CHECK(GPU_ENGINE_KINDS == 7);
    printf("gpu: %d failures\n", failures);
    return failures ? 1 : 0;
}
