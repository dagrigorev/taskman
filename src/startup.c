/* ------------------------------------------------------------------------
 * startup.c - startup entries and what their processes cost.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "startup.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <wctype.h>

/* ------------------------------------------------------------- parsing -- */

/* A candidate only ends the unquoted search when it looks like a file with
   an extension; "C:\Program Files\Acme" can exist as a directory. */
static BOOL StartupHasExtension(const WCHAR *path)
{
    const WCHAR *slash = wcsrchr(path, L'\\');
    const WCHAR *dot = wcsrchr(path, L'.');
    return dot && (!slash || dot > slash) && dot[1] != 0;
}

BOOL Startup_ExeFromCommand(const WCHAR *command, StartupExistsFn exists,
                            WCHAR *out, size_t cch)
{
    WCHAR expanded[STARTUP_COMMAND_MAX];
    const WCHAR *start;
    size_t len;
    if (!out || !cch) return FALSE;
    out[0] = 0;
    if (!command) return FALSE;
    {
        /* The return counts the terminator, and is what it WOULD need when
           the buffer is too small, so a truncating call is caught here.
           Testing the buffer's last character instead would read whatever
           the call left untouched. */
        DWORD needed = ExpandEnvironmentStringsW(command, expanded, ARRAYSIZE(expanded));
        if (!needed || needed > ARRAYSIZE(expanded))
            StringCchCopyW(expanded, ARRAYSIZE(expanded), command);
    }

    start = expanded;
    while (iswspace(*start)) ++start;
    if (!*start) return FALSE;

    if (*start == L'"') {
        const WCHAR *close = wcschr(start + 1, L'"');
        if (!close || close == start + 1) return FALSE;
        len = (size_t)(close - (start + 1));
        if (len >= cch) return FALSE;
        memcpy(out, start + 1, len * sizeof(WCHAR));
        out[len] = 0;
        return TRUE;
    }

    /* Unquoted: grow a word at a time until an existing file is named. */
    {
        const WCHAR *space = start;
        WCHAR candidate[MAX_PATH];
        while ((space = wcschr(space, L' ')) != NULL) {
            len = (size_t)(space - start);
            if (len < ARRAYSIZE(candidate)) {
                memcpy(candidate, start, len * sizeof(WCHAR));
                candidate[len] = 0;
                if (StartupHasExtension(candidate) && exists && exists(candidate)) {
                    StringCchCopyW(out, cch, candidate);
                    return TRUE;
                }
            }
            ++space;
        }
        /* The whole remainder: a path with no arguments, or one whose
           spaces all belong to it. */
        len = (size_t)lstrlenW(start);
        while (len && iswspace(start[len - 1])) --len;
        if (len < ARRAYSIZE(candidate)) {
            memcpy(candidate, start, len * sizeof(WCHAR));
            candidate[len] = 0;
            if (!wcschr(candidate, L' ') || (exists && exists(candidate))) {
                StringCchCopyW(out, cch, candidate);
                return TRUE;
            }
        }
        /* Nothing exists: the first token is the best guess. */
        len = (size_t)(wcschr(start, L' ') - start);
        if (len >= cch) return FALSE;
        memcpy(out, start, len * sizeof(WCHAR));
        out[len] = 0;
        return TRUE;
    }
}

BOOL Startup_IsDisabled(const BYTE *approved, DWORD size)
{
    return approved && size >= 1 && (approved[0] & 1) != 0;
}

void Startup_Attribute(StartupEntry *entries, int count,
                       const StartupProcess *procs, int procCount)
{
    int i, j;
    for (i = 0; i < count; ++i) {
        StartupEntry *e = &entries[i];
        BOOL byName;
        e->running = 0;
        e->cpuTime = 0;
        e->privateBytes = 0;
        ZeroMemory(e->pids, sizeof(e->pids));
        if (!e->exe[0]) continue;
        byName = wcschr(e->exe, L'\\') == NULL;
        for (j = 0; j < procCount; ++j) {
            const WCHAR *path = procs[j].path;
            if (!path[0]) continue;
            if (byName ? lstrcmpiW(PathFindFileNameW(path), e->exe) : lstrcmpiW(path, e->exe))
                continue;
            if (e->running < STARTUP_MAX_PIDS) e->pids[e->running] = procs[j].pid;
            ++e->running;
            e->cpuTime += procs[j].cpuTime;
            e->privateBytes += procs[j].privateBytes;
        }
    }
}

const WCHAR *Startup_SourceName(StartupSource source)
{
    switch (source) {
    case STARTUP_SOURCE_HKCU_RUN:      return L"Registry (user)";
    case STARTUP_SOURCE_HKLM_RUN:      return L"Registry (machine)";
    case STARTUP_SOURCE_HKLM_RUN32:    return L"Registry (machine, 32-bit)";
    case STARTUP_SOURCE_USER_FOLDER:   return L"Startup folder (user)";
    case STARTUP_SOURCE_COMMON_FOLDER: return L"Startup folder (all users)";
    default:                           return L"Unknown";
    }
}

int Startup_Compare(const StartupEntry *a, const StartupEntry *b, int column)
{
    int cmp = 0;
    switch (column) {
    case STARTUP_COL_STATUS:  cmp = (int)a->disabled - (int)b->disabled; break;
    case STARTUP_COL_RUNNING: cmp = (a->running > b->running) - (a->running < b->running); break;
    case STARTUP_COL_CPU:     cmp = (a->cpuTime > b->cpuTime) - (a->cpuTime < b->cpuTime); break;
    case STARTUP_COL_MEMORY:  cmp = (a->privateBytes > b->privateBytes) - (a->privateBytes < b->privateBytes); break;
    case STARTUP_COL_SOURCE:  cmp = (int)a->source - (int)b->source; break;
    case STARTUP_COL_COMMAND: cmp = lstrcmpiW(a->command, b->command); break;
    default: break;
    }
    return cmp ? cmp : lstrcmpiW(a->name, b->name);
}

/* -------------------------------------------------------------- system -- */

#define STARTUP_APPROVED L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\"

static BOOL StartupFileExists(const WCHAR *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL StartupApprovedDisabled(HKEY root, const WCHAR *subkey, const WCHAR *name)
{
    WCHAR path[256];
    BYTE data[32];
    DWORD size = sizeof(data), type = 0;
    HKEY key;
    BOOL disabled = FALSE;
    StringCchPrintfW(path, ARRAYSIZE(path), STARTUP_APPROVED L"%s", subkey);
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return FALSE;
    if (RegQueryValueExW(key, name, NULL, &type, data, &size) == ERROR_SUCCESS && type == REG_BINARY)
        disabled = Startup_IsDisabled(data, size);
    RegCloseKey(key);
    return disabled;
}

static int StartupReadRun(StartupEntry *entries, int count, int max, HKEY root,
                          REGSAM view, StartupSource source, const WCHAR *approved)
{
    HKEY key;
    DWORD index;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_READ | view, &key) != ERROR_SUCCESS)
        return count;
    for (index = 0; count < max; ++index) {
        StartupEntry *e = &entries[count];
        DWORD nameLen = ARRAYSIZE(e->name), dataLen = sizeof(e->command) - sizeof(WCHAR), type;
        LSTATUS status;
        ZeroMemory(e, sizeof(*e));
        status = RegEnumValueW(key, index, e->name, &nameLen, NULL, &type,
                               (BYTE *)e->command, &dataLen);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        e->command[ARRAYSIZE(e->command) - 1] = 0;
        e->source = source;
        e->disabled = StartupApprovedDisabled(root, approved, e->name);
        Startup_ExeFromCommand(e->command, StartupFileExists, e->exe, ARRAYSIZE(e->exe));
        ++count;
    }
    RegCloseKey(key);
    return count;
}

/* Resolves a shortcut's target and arguments; FALSE leaves them empty. */
static BOOL StartupResolveLink(const WCHAR *lnk, WCHAR *target, size_t cchTarget,
                               WCHAR *args, size_t cchArgs)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    BOOL ok = FALSE;
    target[0] = 0;
    args[0] = 0;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IShellLinkW, (void **)&link)))
        return FALSE;
    if (SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&file)) &&
        SUCCEEDED(file->lpVtbl->Load(file, lnk, STGM_READ)) &&
        SUCCEEDED(link->lpVtbl->GetPath(link, target, (int)cchTarget, NULL, SLGP_RAWPATH))) {
        link->lpVtbl->GetArguments(link, args, (int)cchArgs);
        ok = target[0] != 0;
    }
    if (file) file->lpVtbl->Release(file);
    link->lpVtbl->Release(link);
    return ok;
}

static int StartupReadFolder(StartupEntry *entries, int count, int max, REFKNOWNFOLDERID folder,
                             StartupSource source, HKEY approvedRoot)
{
    PWSTR dir = NULL;
    WCHAR pattern[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW find;
    HANDLE search;
    if (FAILED(SHGetKnownFolderPath(folder, 0, NULL, &dir))) return count;
    StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", dir);
    search = FindFirstFileW(pattern, &find);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            StartupEntry *e;
            if (count >= max) break;
            if (find.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!lstrcmpiW(find.cFileName, L"desktop.ini")) continue;
            e = &entries[count];
            ZeroMemory(e, sizeof(*e));
            StringCchPrintfW(full, ARRAYSIZE(full), L"%s\\%s", dir, find.cFileName);
            StringCchCopyW(e->name, ARRAYSIZE(e->name), find.cFileName);
            PathRemoveExtensionW(e->name);
            e->source = source;
            e->disabled = StartupApprovedDisabled(approvedRoot, L"StartupFolder", find.cFileName);
            if (!lstrcmpiW(PathFindExtensionW(find.cFileName), L".lnk")) {
                WCHAR args[512];
                if (StartupResolveLink(full, e->exe, ARRAYSIZE(e->exe), args, ARRAYSIZE(args))) {
                    WCHAR expanded[MAX_PATH];
                    DWORD needed = ExpandEnvironmentStringsW(e->exe, expanded, ARRAYSIZE(expanded));
                    if (needed && needed <= ARRAYSIZE(expanded))
                        StringCchCopyW(e->exe, ARRAYSIZE(e->exe), expanded);
                    StringCchPrintfW(e->command, ARRAYSIZE(e->command),
                                     args[0] ? L"\"%s\" %s" : L"\"%s\"", e->exe, args);
                } else {
                    StringCchCopyW(e->command, ARRAYSIZE(e->command), full);
                }
            } else {
                StringCchCopyW(e->command, ARRAYSIZE(e->command), full);
                StringCchCopyW(e->exe, ARRAYSIZE(e->exe), full);
            }
            ++count;
        } while (FindNextFileW(search, &find));
        FindClose(search);
    }
    CoTaskMemFree(dir);
    return count;
}

int Startup_ReadEntries(StartupEntry *entries, int max)
{
    int count = 0;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!entries || max <= 0) { if (SUCCEEDED(com)) CoUninitialize(); return 0; }
    count = StartupReadRun(entries, count, max, HKEY_CURRENT_USER, 0,
                           STARTUP_SOURCE_HKCU_RUN, L"Run");
    count = StartupReadRun(entries, count, max, HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY,
                           STARTUP_SOURCE_HKLM_RUN, L"Run");
    count = StartupReadRun(entries, count, max, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY,
                           STARTUP_SOURCE_HKLM_RUN32, L"Run32");
    count = StartupReadFolder(entries, count, max, &FOLDERID_Startup,
                              STARTUP_SOURCE_USER_FOLDER, HKEY_CURRENT_USER);
    count = StartupReadFolder(entries, count, max, &FOLDERID_CommonStartup,
                              STARTUP_SOURCE_COMMON_FOLDER, HKEY_LOCAL_MACHINE);
    /* Only balance a successful initialisation; RPC_E_CHANGED_MODE means
       the thread already had an apartment that is not ours to leave. */
    if (SUCCEEDED(com)) CoUninitialize();
    return count;
}

int Startup_ReadProcesses(StartupProcess *procs, int max)
{
    HANDLE snapshot;
    PROCESSENTRY32W pe;
    int count = 0;
    if (!procs || max <= 0) return 0;
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snapshot, &pe)) {
        do {
            StartupProcess *p;
            HANDLE process;
            DWORD len = MAX_PATH;
            if (!pe.th32ProcessID || count >= max) continue;
            process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!process) continue;
            p = &procs[count];
            ZeroMemory(p, sizeof(*p));
            p->pid = pe.th32ProcessID;
            if (QueryFullProcessImageNameW(process, 0, p->path, &len)) {
                FILETIME c, e, k, u;
                PROCESS_MEMORY_COUNTERS_EX memory;
                if (GetProcessTimes(process, &c, &e, &k, &u))
                    p->cpuTime = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                                 (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
                ZeroMemory(&memory, sizeof(memory));
                memory.cb = sizeof(memory);
                if (GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory)))
                    p->privateBytes = memory.PrivateUsage;
                ++count;
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return count;
}
