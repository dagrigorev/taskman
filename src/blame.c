/* ------------------------------------------------------------------------
 * blame.c - Spike Blame: per-sample culprits for the history graphs.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ntapi.h"
#include "blame.h"

/* ------------------------------------------------------------ ranking --- */

static BOOL BlameAhead(const BlameEntry *a, const BlameEntry *b, BlameMetric metric)
{
    return metric == BLAME_BY_MEM ? a->privateBytes > b->privateBytes
                                  : a->cpuPct > b->cpuPct;
}

void Blame_Offer(BlameEntry *top, int *count, const BlameEntry *e, BlameMetric metric)
{
    int at, i;
    if (!top || !count || !e) return;
    if (metric == BLAME_BY_CPU ? !(e->cpuPct > 0) : e->privateBytes == 0) return;
    /* Strictly ahead only: an equal value lands behind the earlier entry. */
    for (at = 0; at < *count && !BlameAhead(e, &top[at], metric); ++at) {}
    if (at >= BLAME_TOP) return;
    i = *count < BLAME_TOP ? *count : BLAME_TOP - 1;
    for (; i > at; --i) top[i] = top[i - 1];
    top[at] = *e;
    if (*count < BLAME_TOP) ++*count;
}

/* --------------------------------------------------------------- ring --- */

void Blame_RingReset(BlameRing *ring)
{
    if (ring) ZeroMemory(ring, sizeof(*ring));
}

void Blame_RingPush(BlameRing *ring, const BlameSample *sample)
{
    if (!ring || !sample) return;
    ring->samples[ring->head] = *sample;
    ring->head = (ring->head + 1) % BLAME_SAMPLES;
    if (ring->count < BLAME_SAMPLES) ring->count++;
}

int Blame_RingCopy(const BlameRing *ring, BlameSample *dst, int count)
{
    int available, i;
    if (!ring || !dst || count <= 0) return 0;
    ZeroMemory(dst, (size_t)count * sizeof(*dst));
    available = ring->count < count ? ring->count : count;
    for (i = 0; i < available; ++i) {
        int idx = (ring->head - 1 - i + 2 * BLAME_SAMPLES) % BLAME_SAMPLES;
        dst[count - 1 - i] = ring->samples[idx];
    }
    return available;
}

/* -------------------------------------------------------- coordinates --- */

int Blame_XFromIndex(int index, int width, int count)
{
    if (count <= 1) return width - 1;
    return index * (width - 1) / (count - 1);
}

int Blame_IndexFromX(int x, int width, int count)
{
    int index;
    if (count <= 1 || width <= 1) return count > 0 ? count - 1 : 0;
    if (x <= 0) return 0;
    if (x >= width - 1) return count - 1;
    /* Round to the nearest plotted column. */
    index = (int)(((LONGLONG)x * (count - 1) * 2 + (width - 1)) / (2LL * (width - 1)));
    return index >= count ? count - 1 : index;
}

int Blame_FindPeak(const BlameSample *samples, int count, BlameMetric metric)
{
    int i, best = -1;
    if (!samples) return -1;
    for (i = 0; i < count; ++i) {
        float value = metric == BLAME_BY_MEM ? samples[i].mem : samples[i].cpu;
        float bestValue;
        if (!samples[i].sequence) continue;
        if (best < 0) { best = i; continue; }
        bestValue = metric == BLAME_BY_MEM ? samples[best].mem : samples[best].cpu;
        if (value >= bestValue) best = i;
    }
    return best;
}

int Blame_FindSequence(const BlameSample *samples, int count, ULONG64 sequence)
{
    int i;
    if (!samples || !sequence) return -1;
    for (i = 0; i < count; ++i)
        if (samples[i].sequence == sequence) return i;
    return -1;
}

/* --------------------------------------------------------- collection --- */

typedef struct { DWORD pid; ULONGLONG createTime; ULONGLONG cpuTime; } BlamePrev;

/* collector thread only */
static BYTE      *s_buf;
static ULONG      s_bufSize;
static BlamePrev  s_next[BLAME_MAX_PROCS];
static ULONGLONG  s_prevTick;
static ULONG      s_listingUsed;    /* nonzero while the listing is untaken */

/* shared with the UI thread. s_prev doubles as the liveness list: it is
   exactly the processes the latest successful enumeration saw. Written
   only by the collector, and only under the exclusive lock. */
static BlamePrev  s_prev[BLAME_MAX_PROCS];
static int        s_prevCount;
static BlameRing  s_ring;
static SRWLOCK    s_ringLock = SRWLOCK_INIT;

static ULONGLONG Blame_ProcessorCount(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
}

static const CTM_SYSTEM_PROCESS_INFORMATION *BlameQuery(ULONG *used)
{
    PFN_NtQuerySystemInformation query = Nt_QuerySystemInformation();
    int attempts;
    if (!query) return NULL;
    if (!s_buf) {
        s_bufSize = 512 * 1024;
        s_buf = (BYTE *)malloc(s_bufSize);
        if (!s_buf) return NULL;
    }
    for (attempts = 0; attempts < 8; ++attempts) {
        ULONG needed = 0;
        CTM_NTSTATUS status = query(CtmSystemProcessInformation, s_buf, s_bufSize, &needed);
        if (NT_SUCCESS(status)) {
            *used = needed;
            return needed >= sizeof(CTM_SYSTEM_PROCESS_INFORMATION) && needed <= s_bufSize
                ? (const CTM_SYSTEM_PROCESS_INFORMATION *)s_buf : NULL;
        }
        if (status == (CTM_NTSTATUS)0xC0000004L) {  /* STATUS_INFO_LENGTH_MISMATCH */
            ULONG size = needed > s_bufSize ? needed + 65536 : s_bufSize * 2;
            BYTE *grown;
            if (size > 64 * 1024 * 1024) return NULL;
            grown = (BYTE *)realloc(s_buf, size);
            if (!grown) return NULL;
            s_buf = grown;
            s_bufSize = size;
        } else {
            return NULL;
        }
    }
    return NULL;
}

static void BlameCopyName(BlameEntry *e, const CTM_SYSTEM_PROCESS_INFORMATION *entry, ULONG used)
{
    const BYTE *end = s_buf + used;
    const BYTE *name = (const BYTE *)entry->ImageName.Buffer;
    USHORT len = entry->ImageName.Length / sizeof(WCHAR);
    if (name && len && name >= s_buf && name <= end &&
        (size_t)(end - name) >= entry->ImageName.Length) {
        if (len >= BLAME_IMAGE_MAX) len = BLAME_IMAGE_MAX - 1;
        memcpy(e->image, name, len * sizeof(WCHAR));
        e->image[len] = L'\0';
    } else {
        StringCchCopyW(e->image, ARRAYSIZE(e->image), e->pid == 4 ? L"System" : L"Unknown");
    }
}

void Blame_Collect(ULONG64 sequence, float cpu, float mem)
{
    const CTM_SYSTEM_PROCESS_INFORMATION *entry;
    BlameSample sample;
    ULONG used = 0;
    ULONGLONG now = GetTickCount64();
    ULONGLONG elapsed = s_prevTick && now > s_prevTick
        ? (now - s_prevTick) * 10000ULL * Blame_ProcessorCount() : 0;
    int nextCount = 0, i;
    BOOL enumerated;

    ZeroMemory(&sample, sizeof(sample));
    sample.sequence = sequence;
    sample.cpu = cpu;
    sample.mem = mem;
    GetSystemTimeAsFileTime(&sample.wallTime);

    entry = BlameQuery(&used);
    enumerated = entry != NULL;
    s_listingUsed = enumerated ? used : 0;
    while (entry) {
        BlameEntry e;
        ULONGLONG cpuTime = (ULONGLONG)entry->KernelTime.QuadPart +
                            (ULONGLONG)entry->UserTime.QuadPart;
        ZeroMemory(&e, sizeof(e));
        e.pid = (DWORD)(ULONG_PTR)entry->UniqueProcessId;
        e.createTime = (ULONGLONG)entry->CreateTime.QuadPart;

        /* PID 0 is idle capacity, excluded like the process table does. */
        if (e.pid != 0) {
            for (i = 0; i < s_prevCount; ++i) {
                if (s_prev[i].pid == e.pid && s_prev[i].createTime == e.createTime) {
                    if (elapsed && cpuTime >= s_prev[i].cpuTime) {
                        double pct = (double)(cpuTime - s_prev[i].cpuTime) * 100.0 / (double)elapsed;
                        e.cpuPct = (float)(pct > 100.0 ? 100.0 : pct);
                    }
                    break;
                }
            }
            if (entry->WorkingSetPrivateSize.QuadPart > 0)
                e.privateBytes = (ULONGLONG)entry->WorkingSetPrivateSize.QuadPart;
            if (e.cpuPct > 0 || e.privateBytes > 0) {
                BlameCopyName(&e, entry, used);
                Blame_Offer(sample.byCpu, &sample.cpuCount, &e, BLAME_BY_CPU);
                Blame_Offer(sample.byMem, &sample.memCount, &e, BLAME_BY_MEM);
            }
            if (nextCount < BLAME_MAX_PROCS) {
                s_next[nextCount].pid = e.pid;
                s_next[nextCount].createTime = e.createTime;
                s_next[nextCount].cpuTime = cpuTime;
                ++nextCount;
            }
        }

        if (!entry->NextEntryOffset) break;
        {
            size_t remaining = used - (size_t)((const BYTE *)entry - s_buf);
            if (entry->NextEntryOffset < sizeof(*entry) || entry->NextEntryOffset > remaining ||
                remaining - entry->NextEntryOffset < sizeof(*entry))
                break;
            entry = (const CTM_SYSTEM_PROCESS_INFORMATION *)((const BYTE *)entry + entry->NextEntryOffset);
        }
    }

    /* A failed enumeration still pushes, so the ring stays aligned with
       the history graphs sample for sample, but it keeps the previous
       baseline and liveness list rather than emptying them. */
    AcquireSRWLockExclusive(&s_ringLock);
    if (enumerated) {
        memcpy(s_prev, s_next, (size_t)nextCount * sizeof(s_prev[0]));
        s_prevCount = nextCount;
        s_prevTick = now;
    }
    Blame_RingPush(&s_ring, &sample);
    ReleaseSRWLockExclusive(&s_ringLock);
}

void Blame_Reset(void)
{
    AcquireSRWLockExclusive(&s_ringLock);
    Blame_RingReset(&s_ring);
    s_prevCount = 0;
    s_prevTick = 0;
    s_listingUsed = 0;
    ReleaseSRWLockExclusive(&s_ringLock);
}

int Blame_Copy(BlameSample *dst, int count)
{
    int copied;
    AcquireSRWLockShared(&s_ringLock);
    copied = Blame_RingCopy(&s_ring, dst, count);
    ReleaseSRWLockShared(&s_ringLock);
    return copied;
}

BOOL Blame_TakeListing(const BYTE **base, ULONG *used)
{
    if (!base || !used || !s_listingUsed || !s_buf) return FALSE;
    *base = s_buf;
    *used = s_listingUsed;
    s_listingUsed = 0;
    return TRUE;
}

BOOL Blame_IsAlive(DWORD pid, ULONGLONG createTime)
{
    /* Answered from the latest enumeration rather than OpenProcess:
       protected processes refuse even limited access, and painting should
       not make system calls per row. At most one sample stale. */
    BOOL alive = FALSE;
    int i;
    AcquireSRWLockShared(&s_ringLock);
    for (i = 0; i < s_prevCount && !alive; ++i)
        alive = s_prev[i].pid == pid && s_prev[i].createTime == createTime;
    ReleaseSRWLockShared(&s_ringLock);
    return alive;
}
