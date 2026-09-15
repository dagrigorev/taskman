/* Exercises startup command parsing and attribution with synthetic data. */
#include "../include/app.h"
#include "../include/startup.h"
#include <stdio.h>
#include "../src/startup.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

/* Pretends these files exist, and nothing else. */
static BOOL FakeExists(const WCHAR *path)
{
    return !lstrcmpiW(path, L"C:\\Program Files\\Acme App\\acme.exe") ||
           !lstrcmpiW(path, L"C:\\Tools\\tray.exe") ||
           !lstrcmpiW(path, L"C:\\Program Files\\Acme");            /* a directory-like prefix */
}

static void TestExeFromCommand(void)
{
    WCHAR exe[MAX_PATH];

    CHECK(Startup_ExeFromCommand(L"\"C:\\Program Files\\Acme App\\acme.exe\" --minimized", FakeExists, exe, ARRAYSIZE(exe)));
    CHECK(!lstrcmpW(exe, L"C:\\Program Files\\Acme App\\acme.exe"));

    /* Unquoted with spaces: grows word by word until a file exists. */
    CHECK(Startup_ExeFromCommand(L"C:\\Program Files\\Acme App\\acme.exe -min", FakeExists, exe, ARRAYSIZE(exe)));
    CHECK(!lstrcmpW(exe, L"C:\\Program Files\\Acme App\\acme.exe"));

    CHECK(Startup_ExeFromCommand(L"  C:\\Tools\\tray.exe", FakeExists, exe, ARRAYSIZE(exe)));
    CHECK(!lstrcmpW(exe, L"C:\\Tools\\tray.exe"));

    /* Unquoted, nothing exists: fall back to the first token. */
    CHECK(Startup_ExeFromCommand(L"C:\\Gone\\old.exe /s", FakeExists, exe, ARRAYSIZE(exe)));
    CHECK(!lstrcmpW(exe, L"C:\\Gone\\old.exe"));

    /* Bare name stays bare, for matching by file name. */
    CHECK(Startup_ExeFromCommand(L"ctfmon.exe", FakeExists, exe, ARRAYSIZE(exe)));
    CHECK(!lstrcmpW(exe, L"ctfmon.exe"));

    /* Environment variables expand before parsing. */
    SetEnvironmentVariableW(L"CTM_TEST_ROOT", L"C:\\Tools");
    CHECK(Startup_ExeFromCommand(L"%CTM_TEST_ROOT%\\tray.exe -x", FakeExists, exe, ARRAYSIZE(exe)));
    CHECK(!lstrcmpW(exe, L"C:\\Tools\\tray.exe"));

    CHECK(!Startup_ExeFromCommand(L"", FakeExists, exe, ARRAYSIZE(exe)));
    CHECK(!Startup_ExeFromCommand(L"   ", FakeExists, exe, ARRAYSIZE(exe)));
    CHECK(!Startup_ExeFromCommand(L"\"unterminated", FakeExists, exe, ARRAYSIZE(exe)));
}

static void TestIsDisabled(void)
{
    const BYTE enabled2[12] = { 0x02 }, enabled6[12] = { 0x06 };
    const BYTE disabled3[12] = { 0x03 }, disabled7[12] = { 0x07 };
    CHECK(!Startup_IsDisabled(NULL, 0));
    CHECK(!Startup_IsDisabled(enabled2, 12));
    CHECK(!Startup_IsDisabled(enabled6, 12));
    CHECK(Startup_IsDisabled(disabled3, 12));
    CHECK(Startup_IsDisabled(disabled7, 12));
    CHECK(!Startup_IsDisabled(disabled3, 0));
}

static StartupProcess Proc(DWORD pid, const WCHAR *path, ULONGLONG cpu, ULONGLONG bytes)
{
    StartupProcess p;
    ZeroMemory(&p, sizeof(p));
    p.pid = pid;
    StringCchCopyW(p.path, ARRAYSIZE(p.path), path);
    p.cpuTime = cpu;
    p.privateBytes = bytes;
    return p;
}

static void TestAttribute(void)
{
    StartupEntry e[3];
    StartupProcess p[5];
    ZeroMemory(e, sizeof(e));
    StringCchCopyW(e[0].exe, MAX_PATH, L"C:\\Program Files\\Acme App\\acme.exe");
    StringCchCopyW(e[1].exe, MAX_PATH, L"ctfmon.exe");
    e[2].exe[0] = 0;                                   /* unresolved */
    e[0].running = 99;                                 /* stale, must reset */

    p[0] = Proc(10, L"c:\\program files\\acme app\\ACME.EXE", 100, 1000);
    p[1] = Proc(11, L"C:\\Program Files\\Acme App\\acme.exe", 50, 500);
    p[2] = Proc(12, L"C:\\Windows\\System32\\ctfmon.exe", 7, 70);
    p[3] = Proc(13, L"C:\\Other\\acme.exe", 1, 1);     /* same name, other path */
    p[4] = Proc(14, L"", 1, 1);                        /* no path known */

    Startup_Attribute(e, 3, p, 5);
    CHECK(e[0].running == 2);
    CHECK(e[0].pids[0] == 10 && e[0].pids[1] == 11);
    CHECK(e[0].cpuTime == 150 && e[0].privateBytes == 1500);
    CHECK(e[1].running == 1 && e[1].pids[0] == 12 && e[1].cpuTime == 7);
    CHECK(e[2].running == 0 && e[2].cpuTime == 0);
}

static void TestAttributeCapsPids(void)
{
    StartupEntry e;
    StartupProcess p[STARTUP_MAX_PIDS + 2];
    int i;
    ZeroMemory(&e, sizeof(e));
    StringCchCopyW(e.exe, MAX_PATH, L"C:\\Tools\\tray.exe");
    for (i = 0; i < STARTUP_MAX_PIDS + 2; ++i) p[i] = Proc((DWORD)(i + 1), L"C:\\Tools\\tray.exe", 1, 1);
    Startup_Attribute(&e, 1, p, STARTUP_MAX_PIDS + 2);
    CHECK(e.running == STARTUP_MAX_PIDS + 2);    /* counted in full */
    CHECK(e.cpuTime == (ULONGLONG)STARTUP_MAX_PIDS + 2);
    CHECK(e.pids[STARTUP_MAX_PIDS - 1] == STARTUP_MAX_PIDS);   /* only the first kept */
}

static void TestCompare(void)
{
    StartupEntry a, b;
    ZeroMemory(&a, sizeof(a)); ZeroMemory(&b, sizeof(b));
    StringCchCopyW(a.name, STARTUP_NAME_MAX, L"alpha");
    StringCchCopyW(b.name, STARTUP_NAME_MAX, L"Beta");
    a.cpuTime = 10; b.cpuTime = 20;
    a.privateBytes = 5; b.privateBytes = 1;
    a.running = 1; b.running = 1;
    b.disabled = TRUE;
    CHECK(Startup_Compare(&a, &b, STARTUP_COL_NAME) < 0);      /* case-insensitive */
    CHECK(Startup_Compare(&a, &b, STARTUP_COL_CPU) < 0);
    CHECK(Startup_Compare(&a, &b, STARTUP_COL_MEMORY) > 0);
    CHECK(Startup_Compare(&a, &b, STARTUP_COL_STATUS) < 0);    /* enabled before disabled */
    CHECK(Startup_Compare(&a, &b, STARTUP_COL_RUNNING) < 0);   /* tie falls back to name */
    CHECK(Startup_Compare(&a, &a, STARTUP_COL_CPU) == 0);
}

int main(void)
{
    TestExeFromCommand();
    TestIsDisabled();
    TestAttribute();
    TestAttributeCapsPids();
    TestCompare();
    CHECK(Startup_SourceName(STARTUP_SOURCE_HKCU_RUN)[0] != 0);
    CHECK(Startup_SourceName((StartupSource)99)[0] != 0);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_startup: all passed\n");
    return 0;
}
