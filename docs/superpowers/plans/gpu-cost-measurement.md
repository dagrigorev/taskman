# GPU counter collection cost — measurement

Task 1 of `2026-09-07-gpu-acquisition.md`. The spec made the sampling cadence
conditional on this number rather than on a guess.

## Why it had to be measured

The exploratory probe used PowerShell's `Get-Counter` and took **4.8 seconds**.
Designing around that figure would have forced a cadence of several seconds and
made the Sensors tab graph useless. But `Get-Counter` imposes its own sampling
interval and marshals every instance into a `PSObject`, so it says nothing about
the cost of the native path the application actually uses.

## Method

A throwaway harness (built, run, deleted — not kept in the repository) opened a
persistent `PDH_HQUERY`, added `\GPU Engine(*)\Utilization Percentage` with
`PdhAddEnglishCounterW`, primed it with one discarded collection, then timed
`PdhCollectQueryData` and `PdhGetFormattedCounterArrayW` separately over five
passes a second apart.

Built with `gcc -std=gnu11 -O2 -Wall -lpdh`.

## Result

```
pass 0: instances=1029 collect=0.47 ms format=0.79 ms total=1.27 ms (status 0x0)
pass 1: instances=1029 collect=0.49 ms format=0.70 ms total=1.19 ms (status 0x0)
pass 2: instances=1029 collect=0.53 ms format=0.72 ms total=1.25 ms (status 0x0)
pass 3: instances=1029 collect=0.52 ms format=0.71 ms total=1.23 ms (status 0x0)
pass 4: instances=1029 collect=0.55 ms format=0.76 ms total=1.31 ms (status 0x0)
```

Mean total **1.25 ms** at 1029 instances, stable across passes, with
`status 0x0` (`ERROR_SUCCESS`) every time.

Native collection is roughly **3700x cheaper** than the PowerShell probe
implied. Essentially all of the 4.8 s was `Get-Counter`'s own overhead.

## Decision

The plan's table maps a total of 10 ms or less to a one second interval:

```c
#define GPU_COLLECT_INTERVAL_MS 1000
```

At 1.25 ms per collection that is about **0.13 % of one core**, and it is only
paid while the Sensors tab is open or the per-process GPU column is enabled.

Two secondary findings worth carrying forward:

- **`PdhAddEnglishCounterW` works and returns the counter.** Had the localised
  variant been used on this Russian-language Windows, the add would have failed
  and the whole feature would have silently reported nothing. The plan's
  insistence on the English variant is confirmed, not merely prudent.
- **Formatting costs slightly more than collecting** (0.73 ms against 0.51 ms).
  If this ever needs optimising, the lever is reducing how many instances are
  formatted, not how often the query is collected.
