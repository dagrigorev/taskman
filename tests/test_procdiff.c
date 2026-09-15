/* Exercises the process diff with synthetic rows. No UI stubs needed:
   proc_diff.c calls nothing from user32 or comctl32. */
#include "../include/proc_diff.h"
#include <stdio.h>
#include <strsafe.h>
#include "../src/tabs/proc_diff.c"

static int failures;
#define CHECK(x) do { if (!(x)) { \
    printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

#define MB (1024ULL * 1024)

static ProcRow Row(DWORD pid, ULONGLONG created, ULONGLONG bytes, DWORD handles, const WCHAR *name)
{
    ProcRow r;
    ZeroMemory(&r, sizeof(r));
    r.pid = pid;
    r.createTime = created;
    r.privateBytes = bytes;
    r.memoryKnown = TRUE;
    r.handles = handles;
    StringCchCopyW(r.imageName, ARRAYSIZE(r.imageName), name);
    return r;
}

static FILETIME Wall(void)
{
    FILETIME ft = { 1, 2 };
    return ft;
}

static void TestEmptyMark(void)
{
    ProcMark mark = {0};
    ProcRow r = Row(10, 1, MB, 10, L"a.exe");
    ProcDiffCounts counts;
    CHECK(!ProcDiff_IsSet(&mark));
    CHECK(ProcDiff_Classify(&mark, &r, NULL, NULL) == PROC_CHANGE_NONE);
    ProcDiff_Count(&mark, &r, 1, &counts, NULL, 0);
    CHECK(counts.started == 0 && counts.exited == 0 && counts.grew == 0);
    ProcDiff_Free(&mark);
}

static void TestClassify(void)
{
    ProcMark mark = {0};
    /* Deliberately unsorted, with pid 0 that must be skipped. */
    ProcRow then[4];
    ProcRow now;
    LONGLONG mem = 123;
    LONG handles = 456;
    then[0] = Row(30, 5, 100 * MB, 100, L"grower.exe");
    then[1] = Row(0, 0, 0, 0, L"Idle");
    then[2] = Row(10, 1, 50 * MB, 50, L"steady.exe");
    then[3] = Row(20, 3, 10 * MB, 10, L"handles.exe");
    CHECK(ProcDiff_Take(&mark, then, 4, Wall()));
    CHECK(ProcDiff_IsSet(&mark));
    CHECK(mark.count == 3);
    CHECK(mark.wallTime.dwLowDateTime == 1 && mark.wallTime.dwHighDateTime == 2);

    now = Row(10, 1, 50 * MB + 15 * MB, 60, L"steady.exe");       /* under both floors */
    CHECK(ProcDiff_Classify(&mark, &now, &mem, &handles) == PROC_CHANGE_NONE);
    CHECK(mem == (LONGLONG)(15 * MB) && handles == 10);

    now = Row(30, 5, 100 * MB + 16 * MB, 100, L"grower.exe");     /* memory floor */
    CHECK(ProcDiff_Classify(&mark, &now, &mem, NULL) == PROC_CHANGE_GREW);
    CHECK(mem == (LONGLONG)(16 * MB));

    now = Row(20, 3, 1 * MB, 10 + PROC_DIFF_GREW_HANDLES, L"handles.exe"); /* shrank, more handles */
    CHECK(ProcDiff_Classify(&mark, &now, &mem, &handles) == PROC_CHANGE_GREW);
    CHECK(mem == -(LONGLONG)(9 * MB) && handles == PROC_DIFF_GREW_HANDLES);

    now = Row(20, 99, 10 * MB, 10, L"reused.exe");                /* same pid, other process */
    mem = 7; handles = 7;
    CHECK(ProcDiff_Classify(&mark, &now, &mem, &handles) == PROC_CHANGE_NEW);
    CHECK(mem == 0 && handles == 0);

    now = Row(40, 9, 1, 1, L"fresh.exe");
    CHECK(ProcDiff_Classify(&mark, &now, NULL, NULL) == PROC_CHANGE_NEW);

    now = Row(30, 5, 900 * MB, 100, L"grower.exe");               /* unknown memory now */
    now.memoryKnown = FALSE;
    CHECK(ProcDiff_Classify(&mark, &now, &mem, NULL) == PROC_CHANGE_NONE);
    CHECK(mem == 0);

    now = Row(0, 0, 0, 0, L"Idle");
    CHECK(ProcDiff_Classify(&mark, &now, NULL, NULL) == PROC_CHANGE_NONE);
    ProcDiff_Free(&mark);
    CHECK(!ProcDiff_IsSet(&mark) && mark.entries == NULL && mark.count == 0);
}

static void TestCount(void)
{
    ProcMark mark = {0};
    ProcRow then[4], now[4];
    ProcMarkEntry exited[1];
    ProcDiffCounts counts;
    then[0] = Row(1, 1, 10 * MB, 10, L"stays.exe");
    then[1] = Row(2, 2, 10 * MB, 10, L"gone-a.exe");
    then[2] = Row(3, 3, 10 * MB, 10, L"gone-b.exe");
    then[3] = Row(4, 4, 10 * MB, 10, L"grows.exe");
    CHECK(ProcDiff_Take(&mark, then, 4, Wall()));

    now[0] = Row(4, 4, 90 * MB, 10, L"grows.exe");
    now[1] = Row(1, 1, 10 * MB, 10, L"stays.exe");
    now[2] = Row(3, 77, 1 * MB, 1, L"reuses-pid.exe");      /* gone-b exited, pid reused */
    now[3] = Row(9, 9, 1 * MB, 1, L"new.exe");
    ProcDiff_Count(&mark, now, 4, &counts, exited, 1);
    CHECK(counts.started == 2);
    CHECK(counts.exited == 2);
    CHECK(counts.grew == 1);
    /* Only one slot: the first exited process in pid order. */
    CHECK(exited[0].pid == 2 && !lstrcmpW(exited[0].imageName, L"gone-a.exe"));

    /* Taking a new mark replaces the old one. */
    CHECK(ProcDiff_Take(&mark, now, 4, Wall()));
    ProcDiff_Count(&mark, now, 4, &counts, NULL, 0);
    CHECK(counts.started == 0 && counts.exited == 0 && counts.grew == 0);
    ProcDiff_Free(&mark);
}

int main(void)
{
    TestEmptyMark();
    TestClassify();
    TestCount();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_procdiff: all passed\n");
    return 0;
}
