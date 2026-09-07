#include "../include/app.h"
#include <stdio.h>
#include <limits.h>

static BYTE data[128];
static DWORD dataSize, dataType;
static LONG WINAPI FakeOpen(HKEY root, const WCHAR *name, DWORD options, REGSAM access, HKEY *out)
{ (void)root; (void)name; (void)options; (void)access; *out = (HKEY)1; return ERROR_SUCCESS; }
static LONG WINAPI FakeClose(HKEY key) { (void)key; return ERROR_SUCCESS; }
static LONG WINAPI FakeQuery(HKEY key, const WCHAR *name, DWORD *reserved, DWORD *type, BYTE *out, DWORD *size)
{
    DWORD capacity = *size;
    (void)key; (void)name; (void)reserved;
    *type = dataType;
    *size = dataSize;
    if (capacity < dataSize) return ERROR_MORE_DATA;
    if (dataSize) memcpy(out, data, dataSize);
    return ERROR_SUCCESS;
}
#define RegOpenKeyExW FakeOpen
#define RegCloseKey FakeClose
#define RegQueryValueExW FakeQuery
#include "../src/settings.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void Fixture(DWORD type, const void *value, DWORD bytes)
{ dataType = type; dataSize = bytes; if (bytes) memcpy(data, value, bytes); }

int main(void)
{
    WCHAR buf[16];
    RECT rc;
    size_t i;
    for (i = 0; i < ARRAYSIZE(buf); ++i) buf[i] = L'!';
    Fixture(REG_SZ, L"abc", 3 * sizeof(WCHAR));
    CHECK(Reg_GetString(L"test", buf, ARRAYSIZE(buf)));
    CHECK(wcscmp(buf, L"abc") == 0);
    Fixture(REG_MULTI_SZ, L"one\0two", 7 * sizeof(WCHAR));
    for (i = 0; i < ARRAYSIZE(buf); ++i) buf[i] = L'!';
    CHECK(Reg_GetMultiString(L"test", buf, ARRAYSIZE(buf)));
    CHECK(wcscmp(buf, L"one") == 0 && wcscmp(buf + 4, L"two") == 0 && buf[8] == 0);
    Fixture(REG_SZ, L"a", 1);
    CHECK(!Reg_GetString(L"test", buf, ARRAYSIZE(buf)));
    Fixture(REG_MULTI_SZ, L"a", 1);
    CHECK(!Reg_GetMultiString(L"test", buf, ARRAYSIZE(buf)));
    Fixture(REG_BINARY, L"abc", 8);
    CHECK(!Reg_GetString(L"test", buf, ARRAYSIZE(buf)));
    Fixture(REG_SZ, L"abc", 8);
    CHECK(!Reg_GetString(L"test", buf, 2));
    CHECK(!Reg_GetString(L"test", NULL, 16));
    CHECK(!Reg_GetString(L"test", buf, 0));
    CHECK(!Reg_GetString(L"test", buf, MAXDWORD));
    CHECK(!Reg_GetMultiString(L"test", NULL, 16));
    CHECK(!Reg_GetMultiString(L"test", buf, 0));
    CHECK(!Reg_GetMultiString(L"test", buf, 1));
    CHECK(!Reg_GetMultiString(L"test", buf, MAXDWORD));
    Fixture(REG_SZ, L"abc", 6);
    CHECK(!Reg_GetString(L"test", buf, 3)); /* cannot append terminator */
    Fixture(REG_SZ, L"abc", 8);
    CHECK(Reg_GetString(L"test", buf, 4)); /* exact, terminated fit */
    Fixture(REG_MULTI_SZ, L"a\0", 6);
    CHECK(Reg_GetMultiString(L"test", buf, 3));
    Fixture(REG_MULTI_SZ, L"a", 2);
    CHECK(!Reg_GetMultiString(L"test", buf, 2));
    Fixture(REG_MULTI_SZ, L"", 0);
    CHECK(Reg_GetMultiString(L"test", buf, 2) && buf[0] == 0 && buf[1] == 0);
    rc.left = 0; rc.top = 0; rc.right = 640; rc.bottom = 480;
    CHECK(ValidRect(&rc));
    rc.left = LONG_MIN; rc.right = LONG_MAX;
    CHECK(!ValidRect(&rc));
    rc.left = LONG_MAX; rc.right = LONG_MIN + 200;
    CHECK(!ValidRect(&rc));
    Fixture(REG_BINARY, NULL, 0); /* absent value: query fails, default applies */
    ZeroMemory(&g_cfg, sizeof(g_cfg));
    Settings_Load();
    /* Default is on: absent registry value must load as TRUE. */
    CHECK(g_cfg.procTreeMode == TRUE || g_cfg.procTreeMode == FALSE);
    CHECK(sizeof(g_cfg.procTreeMode) == sizeof(BOOL));

    /* With ProcessTreeView explicitly absent from the registry (same fixture
       as above: the fake query reports no value), procTreeMode must default
       to TRUE specifically -- the spec's "tree on by default" decision. */
    Fixture(REG_BINARY, NULL, 0);
    ZeroMemory(&g_cfg, sizeof(g_cfg));
    Settings_Load();
    CHECK(g_cfg.procTreeMode == TRUE);
    puts("PASS registry parsing and rectangle boundaries");
    return 0;
}
