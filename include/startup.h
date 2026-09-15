/* ------------------------------------------------------------------------
 * startup.h - startup entries and what their processes cost.
 *
 * Parsing and attribution are pure and unit tested with an injected
 * file-existence check; only the Startup_Read* functions touch the system.
 * ------------------------------------------------------------------------ */
#ifndef CTM_STARTUP_H
#define CTM_STARTUP_H

#include <windows.h>

#define STARTUP_NAME_MAX    128
#define STARTUP_COMMAND_MAX 1024
#define STARTUP_MAX_ENTRIES 256
#define STARTUP_MAX_PIDS    8

typedef enum {
    STARTUP_SOURCE_HKCU_RUN = 0,
    STARTUP_SOURCE_HKLM_RUN,
    STARTUP_SOURCE_HKLM_RUN32,
    STARTUP_SOURCE_USER_FOLDER,
    STARTUP_SOURCE_COMMON_FOLDER,
    STARTUP_SOURCE_COUNT
} StartupSource;

typedef struct {
    WCHAR         name[STARTUP_NAME_MAX];
    WCHAR         command[STARTUP_COMMAND_MAX];
    WCHAR         exe[MAX_PATH];        /* resolved executable, "" if unknown */
    StartupSource source;
    BOOL          disabled;             /* switched off in StartupApproved   */
    /* Filled by Startup_Attribute. */
    int           running;
    DWORD         pids[STARTUP_MAX_PIDS];
    ULONGLONG     cpuTime;              /* 100 ns, summed over matches       */
    ULONGLONG     privateBytes;
} StartupEntry;

/* One running process, as attribution needs it. */
typedef struct {
    DWORD     pid;
    WCHAR     path[MAX_PATH];
    ULONGLONG cpuTime;
    ULONGLONG privateBytes;
} StartupProcess;

typedef BOOL (*StartupExistsFn)(const WCHAR *path);

/* Reduces a Run command line to its executable. Environment variables are
   expanded. A quoted first token is taken as is; an unquoted one grows a
   space-separated word at a time until it names a file that exists, which
   is how Windows itself resolves "C:\Program Files\App\app.exe -min". A
   bare name without a directory is returned unchanged. FALSE when nothing
   usable is found. */
BOOL Startup_ExeFromCommand(const WCHAR *command, StartupExistsFn exists,
                            WCHAR *out, size_t cch);

/* A StartupApproved value marks an entry disabled when the low bit of its
   first byte is set (0x03, 0x07); enabled entries store 0x02 or 0x06. An
   absent or empty value means enabled. */
BOOL Startup_IsDisabled(const BYTE *approved, DWORD size);

/* Matches processes to entries by full image path, case-insensitively, or
   by file name when the entry has no directory. Resets and fills the
   running/pids/cpuTime/privateBytes fields. */
void Startup_Attribute(StartupEntry *entries, int count,
                       const StartupProcess *procs, int procCount);

const WCHAR *Startup_SourceName(StartupSource source);

enum {
    STARTUP_COL_NAME = 0,
    STARTUP_COL_STATUS,
    STARTUP_COL_RUNNING,
    STARTUP_COL_CPU,
    STARTUP_COL_MEMORY,
    STARTUP_COL_SOURCE,
    STARTUP_COL_COMMAND,
    STARTUP_COL_COUNT
};

/* Ascending order for a column; ties fall back to the name. */
int Startup_Compare(const StartupEntry *a, const StartupEntry *b, int column);

/* --- system -------------------------------------------------------------- */

/* Reads every source into entries; returns how many were stored. */
int  Startup_ReadEntries(StartupEntry *entries, int max);
/* Snapshots running processes with image paths; returns how many. */
int  Startup_ReadProcesses(StartupProcess *procs, int max);

#endif /* CTM_STARTUP_H */
