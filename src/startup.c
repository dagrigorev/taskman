/* ------------------------------------------------------------------------
 * startup.c - startup entries and what their processes cost.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ntapi.h"
#include "startup.h"
#include <shlobj.h>
#include <tlhelp32.h>
#include <taskschd.h>
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
        /* A service names its process directly; no path matching needed. */
        if (e->servicePid) {
            for (j = 0; j < procCount; ++j) {
                if (procs[j].pid != e->servicePid) continue;
                e->pids[0] = procs[j].pid;
                e->running = 1;
                e->cpuTime = procs[j].cpuTime;
                e->privateBytes = procs[j].privateBytes;
                break;
            }
            continue;
        }
        if (!e->exe[0]) continue;
        byName = wcschr(e->exe, L'\\') == NULL;
        for (j = 0; j < procCount; ++j) {
            const WCHAR *path = procs[j].path;
            const WCHAR *name = procs[j].name;
            /* Without a path on either side the name is all there is: an
               elevated or protected process reports one but not the other,
               and would otherwise read as not running. */
            if (byName || !path[0]) {
                const WCHAR *mine = byName ? e->exe : PathFindFileNameW(e->exe);
                const WCHAR *theirs = path[0] ? PathFindFileNameW(path) : name;
                if (!theirs[0] || lstrcmpiW(theirs, mine)) continue;
            } else if (lstrcmpiW(path, e->exe)) {
                continue;
            }
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
    case STARTUP_SOURCE_SERVICE:       return L"Service (automatic)";
    case STARTUP_SOURCE_TASK:          return L"Scheduled task";
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

/* Services set to start automatically, with the pid the SCM reports so a
   running one is credited even when its image path cannot be matched. */
static int StartupReadServices(StartupEntry *entries, int count, int max)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    ENUM_SERVICE_STATUS_PROCESSW *services;
    DWORD needed = 0, returned = 0, resume = 0, i;
    if (!scm) return count;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                          NULL, 0, &needed, &returned, &resume, NULL);
    if (!needed || GetLastError() != ERROR_MORE_DATA) { CloseServiceHandle(scm); return count; }
    services = (ENUM_SERVICE_STATUS_PROCESSW *)malloc(needed);
    if (!services) { CloseServiceHandle(scm); return count; }
    if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
                              (BYTE *)services, needed, &needed, &returned, &resume, NULL)) {
        for (i = 0; i < returned && count < max; ++i) {
            SC_HANDLE service = OpenServiceW(scm, services[i].lpServiceName, SERVICE_QUERY_CONFIG);
            QUERY_SERVICE_CONFIGW *config;
            DWORD configSize = 0;
            if (!service) continue;
            QueryServiceConfigW(service, NULL, 0, &configSize);
            config = configSize ? (QUERY_SERVICE_CONFIGW *)malloc(configSize) : NULL;
            if (config && QueryServiceConfigW(service, config, configSize, &configSize) &&
                config->dwStartType == SERVICE_AUTO_START) {
                StartupEntry *e = &entries[count++];
                ZeroMemory(e, sizeof(*e));
                StringCchCopyW(e->name, ARRAYSIZE(e->name),
                               services[i].lpDisplayName ? services[i].lpDisplayName
                                                         : services[i].lpServiceName);
                StringCchCopyW(e->command, ARRAYSIZE(e->command), config->lpBinaryPathName);
                e->source = STARTUP_SOURCE_SERVICE;
                /* A service is switched off by being set to another start
                   type, so anything reaching here is enabled. */
                Startup_ExeFromCommand(e->command, StartupFileExists, e->exe, ARRAYSIZE(e->exe));
                e->servicePid = services[i].ServiceStatusProcess.dwProcessId;
            }
            free(config);
            CloseServiceHandle(service);
        }
    }
    free(services);
    CloseServiceHandle(scm);
    return count;
}

/* One scheduled task, when it starts at logon or boot. */
static int StartupAddTask(StartupEntry *entries, int count, int max, IRegisteredTask *task)
{
    ITaskDefinition *definition = NULL;
    ITriggerCollection *triggers = NULL;
    IActionCollection *actions = NULL;
    LONG triggerCount = 0, actionCount = 0, i;
    BOOL atStartup = FALSE;
    VARIANT_BOOL enabled = VARIANT_TRUE;
    BSTR name = NULL;
    StartupEntry *e;

    if (FAILED(task->lpVtbl->get_Definition(task, &definition)) || !definition) return count;
    if (SUCCEEDED(definition->lpVtbl->get_Triggers(definition, &triggers)) && triggers) {
        triggers->lpVtbl->get_Count(triggers, &triggerCount);
        for (i = 1; i <= triggerCount && !atStartup; ++i) {
            ITrigger *trigger = NULL;
            TASK_TRIGGER_TYPE2 type;
            if (FAILED(triggers->lpVtbl->get_Item(triggers, i, &trigger)) || !trigger) continue;
            if (SUCCEEDED(trigger->lpVtbl->get_Type(trigger, &type)) &&
                (type == TASK_TRIGGER_LOGON || type == TASK_TRIGGER_BOOT))
                atStartup = TRUE;
            trigger->lpVtbl->Release(trigger);
        }
        triggers->lpVtbl->Release(triggers);
    }
    if (!atStartup || count >= max) {
        definition->lpVtbl->Release(definition);
        return count;
    }

    e = &entries[count++];
    ZeroMemory(e, sizeof(*e));
    e->source = STARTUP_SOURCE_TASK;
    if (SUCCEEDED(task->lpVtbl->get_Path(task, &name)) && name) {
        StringCchCopyW(e->name, ARRAYSIZE(e->name), name);
        SysFreeString(name);
    }
    if (SUCCEEDED(task->lpVtbl->get_Enabled(task, &enabled)))
        e->disabled = enabled == VARIANT_FALSE;
    if (SUCCEEDED(definition->lpVtbl->get_Actions(definition, &actions)) && actions) {
        actions->lpVtbl->get_Count(actions, &actionCount);
        for (i = 1; i <= actionCount && !e->command[0]; ++i) {
            IAction *action = NULL;
            IExecAction *exec = NULL;
            if (FAILED(actions->lpVtbl->get_Item(actions, i, &action)) || !action) continue;
            if (SUCCEEDED(action->lpVtbl->QueryInterface(action, &IID_IExecAction, (void **)&exec)) && exec) {
                BSTR path = NULL, args = NULL;
                if (SUCCEEDED(exec->lpVtbl->get_Path(exec, &path)) && path) {
                    exec->lpVtbl->get_Arguments(exec, &args);
                    StringCchPrintfW(e->command, ARRAYSIZE(e->command),
                                     args && *args ? L"\"%s\" %s" : L"\"%s\"", path, args ? args : L"");
                    Startup_ExeFromCommand(e->command, StartupFileExists, e->exe, ARRAYSIZE(e->exe));
                    SysFreeString(path);
                    if (args) SysFreeString(args);
                }
                exec->lpVtbl->Release(exec);
            }
            action->lpVtbl->Release(action);
        }
        actions->lpVtbl->Release(actions);
    }
    definition->lpVtbl->Release(definition);
    return count;
}

static int StartupReadTaskFolder(StartupEntry *entries, int count, int max,
                                 ITaskFolder *folder, int depth)
{
    IRegisteredTaskCollection *tasks = NULL;
    ITaskFolderCollection *folders = NULL;
    LONG total = 0, i;

    if (depth > 8) return count;                 /* defeats a pathological tree */
    if (SUCCEEDED(folder->lpVtbl->GetTasks(folder, TASK_ENUM_HIDDEN, &tasks)) && tasks) {
        tasks->lpVtbl->get_Count(tasks, &total);
        for (i = 1; i <= total && count < max; ++i) {
            VARIANT index;
            IRegisteredTask *task = NULL;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            if (SUCCEEDED(tasks->lpVtbl->get_Item(tasks, index, &task)) && task) {
                count = StartupAddTask(entries, count, max, task);
                task->lpVtbl->Release(task);
            }
        }
        tasks->lpVtbl->Release(tasks);
    }
    if (SUCCEEDED(folder->lpVtbl->GetFolders(folder, 0, &folders)) && folders) {
        total = 0;
        folders->lpVtbl->get_Count(folders, &total);
        for (i = 1; i <= total && count < max; ++i) {
            VARIANT index;
            ITaskFolder *child = NULL;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            if (SUCCEEDED(folders->lpVtbl->get_Item(folders, index, &child)) && child) {
                count = StartupReadTaskFolder(entries, count, max, child, depth + 1);
                child->lpVtbl->Release(child);
            }
        }
        folders->lpVtbl->Release(folders);
    }
    return count;
}

static int StartupReadTasks(StartupEntry *entries, int count, int max)
{
    ITaskService *service = NULL;
    ITaskFolder *root = NULL;
    VARIANT empty;
    BSTR path;
    if (FAILED(CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                                &IID_ITaskService, (void **)&service)) || !service)
        return count;
    VariantInit(&empty);
    if (SUCCEEDED(service->lpVtbl->Connect(service, empty, empty, empty, empty)) &&
        (path = SysAllocString(L"\\")) != NULL) {
        if (SUCCEEDED(service->lpVtbl->GetFolder(service, path, &root)) && root) {
            count = StartupReadTaskFolder(entries, count, max, root, 0);
            root->lpVtbl->Release(root);
        }
        SysFreeString(path);
    }
    service->lpVtbl->Release(service);
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
    count = StartupReadServices(entries, count, max);
    count = StartupReadTasks(entries, count, max);
    /* Only balance a successful initialisation; RPC_E_CHANGED_MODE means
       the thread already had an apartment that is not ours to leave. */
    if (SUCCEEDED(com)) CoUninitialize();
    return count;
}

/* The native query reports every process, including those this program
   cannot open, with their CPU time, private memory and image name. Only
   the full path needs a handle, and it stays empty when there is none. */
static int StartupReadNative(StartupProcess *procs, int max)
{
    PFN_NtQuerySystemInformation query = Nt_QuerySystemInformation();
    const CTM_SYSTEM_PROCESS_INFORMATION *entry;
    BYTE *buffer = NULL;
    ULONG size = 512 * 1024, used = 0;
    int count = 0, attempts;

    if (!query) return 0;
    for (attempts = 0; attempts < 8; ++attempts) {
        CTM_NTSTATUS status;
        ULONG needed = 0;
        BYTE *grown = (BYTE *)realloc(buffer, size);
        if (!grown) { free(buffer); return 0; }
        buffer = grown;
        status = query(CtmSystemProcessInformation, buffer, size, &needed);
        if (NT_SUCCESS(status)) { used = needed; break; }
        if (status != (CTM_NTSTATUS)0xC0000004L || size >= 64 * 1024 * 1024) {
            free(buffer);
            return 0;
        }
        size = needed > size ? needed + 65536 : size * 2;
    }
    if (!used || used < sizeof(*entry)) { free(buffer); return 0; }

    entry = (const CTM_SYSTEM_PROCESS_INFORMATION *)buffer;
    for (;;) {
        DWORD pid = (DWORD)(ULONG_PTR)entry->UniqueProcessId;
        if (pid && count < max) {
            StartupProcess *p = &procs[count++];
            HANDLE process;
            ZeroMemory(p, sizeof(*p));
            p->pid = pid;
            p->cpuTime = (ULONGLONG)entry->KernelTime.QuadPart + (ULONGLONG)entry->UserTime.QuadPart;
            if (entry->WorkingSetPrivateSize.QuadPart > 0)
                p->privateBytes = (ULONGLONG)entry->WorkingSetPrivateSize.QuadPart;
            if (entry->ImageName.Buffer && entry->ImageName.Length) {
                USHORT chars = entry->ImageName.Length / sizeof(WCHAR);
                const BYTE *name = (const BYTE *)entry->ImageName.Buffer;
                if (name >= buffer && name <= buffer + used &&
                    (size_t)(buffer + used - name) >= entry->ImageName.Length) {
                    if (chars >= STARTUP_NAME_MAX) chars = STARTUP_NAME_MAX - 1;
                    memcpy(p->name, entry->ImageName.Buffer, chars * sizeof(WCHAR));
                    p->name[chars] = L'\0';
                }
            }
            process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process) {
                DWORD len = MAX_PATH;
                if (!QueryFullProcessImageNameW(process, 0, p->path, &len)) p->path[0] = 0;
                CloseHandle(process);
            }
        }
        if (!entry->NextEntryOffset) break;
        {
            size_t remaining = used - (size_t)((const BYTE *)entry - buffer);
            if (entry->NextEntryOffset < sizeof(*entry) || entry->NextEntryOffset > remaining ||
                remaining - entry->NextEntryOffset < sizeof(*entry))
                break;
            entry = (const CTM_SYSTEM_PROCESS_INFORMATION *)((const BYTE *)entry + entry->NextEntryOffset);
        }
    }
    free(buffer);
    return count;
}

/* Documented fallback for when the native query is unavailable: it only
   reaches processes this program can open. */
static int StartupReadToolhelp(StartupProcess *procs, int max)
{
    HANDLE snapshot;
    PROCESSENTRY32W pe;
    int count = 0;
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
            p = &procs[count];
            ZeroMemory(p, sizeof(*p));
            p->pid = pe.th32ProcessID;
            StringCchCopyW(p->name, ARRAYSIZE(p->name), pe.szExeFile);
            process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (process) {
                FILETIME c, e, k, u;
                PROCESS_MEMORY_COUNTERS_EX memory;
                if (!QueryFullProcessImageNameW(process, 0, p->path, &len)) p->path[0] = 0;
                if (GetProcessTimes(process, &c, &e, &k, &u))
                    p->cpuTime = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                                 (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
                ZeroMemory(&memory, sizeof(memory));
                memory.cb = sizeof(memory);
                if (GetProcessMemoryInfo(process, (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory)))
                    p->privateBytes = memory.PrivateUsage;
                CloseHandle(process);
            }
            ++count;
        } while (Process32NextW(snapshot, &pe));
    }
    CloseHandle(snapshot);
    return count;
}

int Startup_ReadProcesses(StartupProcess *procs, int max)
{
    int count;
    if (!procs || max <= 0) return 0;
    count = StartupReadNative(procs, max);
    return count ? count : StartupReadToolhelp(procs, max);
}
