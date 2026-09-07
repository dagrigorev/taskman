/* ------------------------------------------------------------------------
 * ntapi.h - hand written declarations for the native (undocumented) NT API.
 *
 * Nothing here comes from a header that ships with the SDK: every structure
 * is declared locally with a CTM_ prefix so it can never collide with
 * <winternl.h> or the MinGW equivalents.  Everything is reached through
 * GetProcAddress on ntdll.dll, so the executable has no static dependency
 * on a private export.
 * ------------------------------------------------------------------------ */
#ifndef CTM_NTAPI_H
#define CTM_NTAPI_H

#include <windows.h>

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((CTM_NTSTATUS)(s)) >= 0)
#endif

typedef LONG CTM_NTSTATUS;

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((CTM_NTSTATUS)0xC0000004L)
#endif

typedef struct _CTM_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} CTM_UNICODE_STRING;

typedef struct _CTM_CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CTM_CLIENT_ID;

/* SystemInformationClass values we use. */
enum {
    CtmSystemBasicInformation                  = 0,
    CtmSystemPerformanceInformation            = 2,
    CtmSystemTimeOfDayInformation              = 3,
    CtmSystemProcessInformation                = 5,
    CtmSystemProcessorPerformanceInformation   = 8,
    CtmSystemFileCacheInformation              = 21,
    CtmSystemProcessorIdleInformation          = 42,
    CtmSystemLogicalProcessorInformation       = 73,
    CtmSystemProcessorPerformanceInformationEx = 100
};

typedef struct _CTM_SYSTEM_BASIC_INFORMATION {
    ULONG     Reserved;
    ULONG     TimerResolution;
    ULONG     PageSize;
    ULONG     NumberOfPhysicalPages;
    ULONG     LowestPhysicalPageNumber;
    ULONG     HighestPhysicalPageNumber;
    ULONG     AllocationGranularity;
    ULONG_PTR MinimumUserModeAddress;
    ULONG_PTR MaximumUserModeAddress;
    ULONG_PTR ActiveProcessorsAffinityMask;
    CCHAR     NumberOfProcessors;
} CTM_SYSTEM_BASIC_INFORMATION;

typedef struct _CTM_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;      /* includes IdleTime */
    LARGE_INTEGER UserTime;
    LARGE_INTEGER DpcTime;
    LARGE_INTEGER InterruptTime;
    ULONG         InterruptCount;
} CTM_SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION;

/* Only the leading fields are stable across releases; we read no further. */
typedef struct _CTM_SYSTEM_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleProcessTime;
    LARGE_INTEGER IoReadTransferCount;
    LARGE_INTEGER IoWriteTransferCount;
    LARGE_INTEGER IoOtherTransferCount;
    ULONG IoReadOperationCount;
    ULONG IoWriteOperationCount;
    ULONG IoOtherOperationCount;
    ULONG AvailablePages;
    ULONG CommittedPages;
    ULONG CommitLimit;
    ULONG PeakCommitment;
    ULONG PageFaultCount;
    ULONG CopyOnWriteCount;
    ULONG TransitionCount;
    ULONG CacheTransitionCount;
    ULONG DemandZeroCount;
    ULONG PageReadCount;
    ULONG PageReadIoCount;
    ULONG CacheReadCount;
    ULONG CacheIoCount;
    ULONG DirtyPagesWriteCount;
    ULONG DirtyWriteIoCount;
    ULONG MappedPagesWriteCount;
    ULONG MappedWriteIoCount;
    ULONG PagedPoolPages;
    ULONG NonPagedPoolPages;
    ULONG PagedPoolAllocs;
    ULONG PagedPoolFrees;
    ULONG NonPagedPoolAllocs;
    ULONG NonPagedPoolFrees;
    ULONG FreeSystemPtes;
    ULONG ResidentSystemCodePage;
    ULONG TotalSystemDriverPages;
    ULONG TotalSystemCodePages;
    ULONG NonPagedPoolLookasideHits;
    ULONG PagedPoolLookasideHits;
    ULONG AvailablePagedPoolPages;
    ULONG ResidentSystemCachePage;
    ULONG ResidentPagedPoolPage;
    ULONG ResidentSystemDriverPage;
    ULONG CcFastReadNoWait;
    ULONG CcFastReadWait;
    ULONG CcFastReadResourceMiss;
    ULONG CcFastReadNotPossible;
    ULONG CcFastMdlReadNoWait;
    ULONG CcFastMdlReadWait;
    ULONG CcFastMdlReadResourceMiss;
    ULONG CcFastMdlReadNotPossible;
    ULONG CcMapDataNoWait;
    ULONG CcMapDataWait;
    ULONG CcMapDataNoWaitMiss;
    ULONG CcMapDataWaitMiss;
    ULONG CcPinMappedDataCount;
    ULONG CcPinReadNoWait;
    ULONG CcPinReadWait;
    ULONG CcPinReadNoWaitMiss;
    ULONG CcPinReadWaitMiss;
    ULONG CcCopyReadNoWait;
    ULONG CcCopyReadWait;
    ULONG CcCopyReadNoWaitMiss;
    ULONG CcCopyReadWaitMiss;
    ULONG CcMdlReadNoWait;
    ULONG CcMdlReadWait;
    ULONG CcMdlReadNoWaitMiss;
    ULONG CcMdlReadWaitMiss;
    ULONG CcReadAheadIos;
    ULONG CcLazyWriteIos;
    ULONG CcLazyWritePages;
    ULONG CcDataFlushes;
    ULONG CcDataPages;
    ULONG ContextSwitches;
    ULONG FirstLevelTbFills;
    ULONG SecondLevelTbFills;
    ULONG SystemCalls;
} CTM_SYSTEM_PERFORMANCE_INFORMATION;

typedef struct _CTM_SYSTEM_TIMEOFDAY_INFORMATION {
    LARGE_INTEGER BootTime;
    LARGE_INTEGER CurrentTime;
    LARGE_INTEGER TimeZoneBias;
    ULONG         TimeZoneId;
    ULONG         Reserved;
    ULONGLONG     BootTimeBias;
    ULONGLONG     SleepTimeBias;
} CTM_SYSTEM_TIMEOFDAY_INFORMATION;

typedef struct _CTM_SYSTEM_THREAD_INFORMATION {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG         WaitTime;
    PVOID         StartAddress;
    CTM_CLIENT_ID ClientId;
    LONG          Priority;
    LONG          BasePriority;
    ULONG         ContextSwitches;
    ULONG         ThreadState;
    ULONG         WaitReason;
} CTM_SYSTEM_THREAD_INFORMATION;

/* SYSTEM_PROCESS_INFORMATION as returned by SystemProcessInformation.
   Layout has been stable since Windows Vista; WorkingSetPrivateSize is the
   value the classic Task Manager shows as "Memory (Private Working Set)". */
typedef struct _CTM_SYSTEM_PROCESS_INFORMATION {
    ULONG              NextEntryOffset;
    ULONG              NumberOfThreads;
    LARGE_INTEGER      WorkingSetPrivateSize;
    ULONG              HardFaultCount;
    ULONG              NumberOfThreadsHighWatermark;
    ULONGLONG          CycleTime;
    LARGE_INTEGER      CreateTime;
    LARGE_INTEGER      UserTime;
    LARGE_INTEGER      KernelTime;
    CTM_UNICODE_STRING ImageName;
    LONG               BasePriority;
    HANDLE             UniqueProcessId;
    HANDLE             InheritedFromUniqueProcessId;
    ULONG              HandleCount;
    ULONG              SessionId;
    ULONG_PTR          UniqueProcessKey;
    SIZE_T             PeakVirtualSize;
    SIZE_T             VirtualSize;
    ULONG              PageFaultCount;
    SIZE_T             PeakWorkingSetSize;
    SIZE_T             WorkingSetSize;
    SIZE_T             QuotaPeakPagedPoolUsage;
    SIZE_T             QuotaPagedPoolUsage;
    SIZE_T             QuotaPeakNonPagedPoolUsage;
    SIZE_T             QuotaNonPagedPoolUsage;
    SIZE_T             PagefileUsage;
    SIZE_T             PeakPagefileUsage;
    SIZE_T             PrivatePageCount;
    LARGE_INTEGER      ReadOperationCount;
    LARGE_INTEGER      WriteOperationCount;
    LARGE_INTEGER      OtherOperationCount;
    LARGE_INTEGER      ReadTransferCount;
    LARGE_INTEGER      WriteTransferCount;
    LARGE_INTEGER      OtherTransferCount;
    /* CTM_SYSTEM_THREAD_INFORMATION Threads[1]; follows */
} CTM_SYSTEM_PROCESS_INFORMATION;

typedef CTM_NTSTATUS (WINAPI *PFN_NtQuerySystemInformation)(
        ULONG SystemInformationClass,
        PVOID SystemInformation,
        ULONG SystemInformationLength,
        PULONG ReturnLength);

typedef CTM_NTSTATUS (WINAPI *PFN_NtQueryInformationProcess)(
        HANDLE ProcessHandle,
        ULONG  ProcessInformationClass,
        PVOID  ProcessInformation,
        ULONG  ProcessInformationLength,
        PULONG ReturnLength);

typedef ULONG (WINAPI *PFN_RtlNtStatusToDosError)(CTM_NTSTATUS Status);

/* Resolved once in sysinfo.c; NULL when the export is unavailable, in which
   case every caller falls back to a documented API. */
BOOL  Nt_Init(void);
PFN_NtQuerySystemInformation   Nt_QuerySystemInformation(void);
PFN_NtQueryInformationProcess  Nt_QueryInformationProcess(void);

#endif /* CTM_NTAPI_H */
