/* ------------------------------------------------------------------------
 * settings.c - persistence in HKCU\Software\ClassicTaskManager
 * ------------------------------------------------------------------------ */
#include "app.h"

Settings g_cfg;

static const WCHAR kKey[] = L"Software\\ClassicTaskManager";

static HKEY OpenKey(BOOL create)
{
    HKEY hk = NULL;
    if (create) {
        DWORD disp = 0;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, NULL,
                            REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
                            NULL, &hk, &disp) != ERROR_SUCCESS)
            return NULL;
    } else {
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &hk) != ERROR_SUCCESS)
            return NULL;
    }
    return hk;
}

BOOL Reg_GetDword(const WCHAR *name, DWORD *value)
{
    HKEY hk = OpenKey(FALSE);
    DWORD type = 0, cb = sizeof(DWORD);
    LONG rc;
    if (!hk) return FALSE;
    rc = RegQueryValueExW(hk, name, NULL, &type, (BYTE *)value, &cb);
    RegCloseKey(hk);
    return (rc == ERROR_SUCCESS && type == REG_DWORD && cb == sizeof(DWORD));
}

void Reg_SetDword(const WCHAR *name, DWORD value)
{
    HKEY hk = OpenKey(TRUE);
    if (!hk) return;
    RegSetValueExW(hk, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(hk);
}

BOOL Reg_GetBinary(const WCHAR *name, void *buf, DWORD cb)
{
    HKEY hk = OpenKey(FALSE);
    DWORD type = 0, size = cb;
    LONG rc;
    if (!hk) return FALSE;
    rc = RegQueryValueExW(hk, name, NULL, &type, (BYTE *)buf, &size);
    RegCloseKey(hk);
    return (rc == ERROR_SUCCESS && type == REG_BINARY && size == cb);
}

void Reg_SetBinary(const WCHAR *name, const void *buf, DWORD cb)
{
    HKEY hk = OpenKey(TRUE);
    if (!hk) return;
    RegSetValueExW(hk, name, 0, REG_BINARY, (const BYTE *)buf, cb);
    RegCloseKey(hk);
}

static BOOL ReadRegistryString(const WCHAR *name, WCHAR *buf, DWORD cch,
                               BOOL multi)
{
    HKEY hk;
    DWORD type = 0, cb, used, missing, i;
    DWORD terminators = multi ? 2 : 1;
    LONG rc;

    if (!buf || cch < terminators || cch > MAXDWORD / sizeof(WCHAR))
        return FALSE;
    for (i = 0; i < terminators; ++i) buf[i] = L'\0';
    hk = OpenKey(FALSE);
    if (!hk) return FALSE;
    cb = cch * sizeof(WCHAR);
    rc = RegQueryValueExW(hk, name, NULL, &type, (BYTE *)buf, &cb);
    RegCloseKey(hk);

    if (rc != ERROR_SUCCESS || cb > cch * sizeof(WCHAR) ||
        cb % sizeof(WCHAR) != 0 ||
        (multi ? type != REG_MULTI_SZ : (type != REG_SZ && type != REG_EXPAND_SZ)))
        goto invalid;

    /* Registry APIs do not guarantee termination. Append at the returned
       length, never at the end of unused (possibly uninitialized) capacity. */
    used = cb / sizeof(WCHAR);
    missing = terminators;
    for (i = 0; i < terminators && i < used && buf[used - 1 - i] == L'\0'; ++i)
        --missing;
    if (missing > cch - used) goto invalid;
    for (i = 0; i < missing; ++i) buf[used + i] = L'\0';
    return TRUE;

invalid:
    for (i = 0; i < terminators; ++i) buf[i] = L'\0';
    return FALSE;
}

BOOL Reg_GetString(const WCHAR *name, WCHAR *buf, DWORD cch)
{
    return ReadRegistryString(name, buf, cch, FALSE);
}

void Reg_SetString(const WCHAR *name, const WCHAR *value)
{
    HKEY hk = OpenKey(TRUE);
    if (!hk) return;
    RegSetValueExW(hk, name, 0, REG_SZ, (const BYTE *)value,
                   (DWORD)((size_t)(lstrlenW(value) + 1) * sizeof(WCHAR)));
    RegCloseKey(hk);
}

BOOL Reg_GetMultiString(const WCHAR *name, WCHAR *buf, DWORD cch)
{
    return ReadRegistryString(name, buf, cch, TRUE);
}

void Reg_SetMultiString(const WCHAR *name, const WCHAR *buf, DWORD cch)
{
    HKEY hk = OpenKey(TRUE);
    if (!hk) return;
    RegSetValueExW(hk, name, 0, REG_MULTI_SZ, (const BYTE *)buf, cch * sizeof(WCHAR));
    RegCloseKey(hk);
}

/* ------------------------------------------------------------------------ */

static BOOL ValidRect(const RECT *rc)
{
    LONGLONG width = (LONGLONG)rc->right - rc->left;
    LONGLONG height = (LONGLONG)rc->bottom - rc->top;
    return width > 100 && width <= MAXLONG && height > 80 && height <= MAXLONG;
}

static DWORD GetDwordDef(const WCHAR *name, DWORD def)
{
    DWORD v = 0;
    return Reg_GetDword(name, &v) ? v : def;
}

void Settings_Load(void)
{
    ZeroMemory(&g_cfg, sizeof(g_cfg));

    g_cfg.haveNormalRect = Reg_GetBinary(L"WindowRect", &g_cfg.rcNormal, sizeof(RECT))
                           && ValidRect(&g_cfg.rcNormal);
    g_cfg.haveTinyRect   = Reg_GetBinary(L"TinyRect", &g_cfg.rcTiny, sizeof(RECT))
                           && ValidRect(&g_cfg.rcTiny);

    g_cfg.maximized            = GetDwordDef(L"Maximized", 0) ? TRUE : FALSE;
    g_cfg.tiny                 = GetDwordDef(L"TinyFootprint", 0) ? TRUE : FALSE;
    g_cfg.activeTab            = (int)GetDwordDef(L"ActiveTab", TAB_PROCESSES);
    g_cfg.updateSpeed          = (int)GetDwordDef(L"UpdateSpeed", SPEED_NORMAL);
    g_cfg.alwaysOnTop          = GetDwordDef(L"AlwaysOnTop", 0) ? TRUE : FALSE;
    g_cfg.minimizeOnUse        = GetDwordDef(L"MinimizeOnUse", 0) ? TRUE : FALSE;
    g_cfg.hideWhenMinimized    = GetDwordDef(L"HideWhenMinimized", 0) ? TRUE : FALSE;
    g_cfg.showFullAccountName  = GetDwordDef(L"ShowFullAccountName", 0) ? TRUE : FALSE;
    g_cfg.showAllUsers         = GetDwordDef(L"ShowAllUsers", 0) ? TRUE : FALSE;
    g_cfg.appsViewMode         = (int)GetDwordDef(L"AppsViewMode", LV_VIEW_DETAILS);
    g_cfg.perfOneGraphPerCpu   = GetDwordDef(L"CpuHistoryPerCpu", 0) ? TRUE : FALSE;
    g_cfg.perfShowKernelTimes  = GetDwordDef(L"ShowKernelTimes", 0) ? TRUE : FALSE;
    g_cfg.netHistoryMode       = (int)GetDwordDef(L"NetworkHistoryMode", 0);
    g_cfg.procTreeMode         = GetDwordDef(L"ProcessTreeView", 1) ? TRUE : FALSE;

    if (g_cfg.activeTab < 0 || g_cfg.activeTab >= TAB_COUNT)
        g_cfg.activeTab = TAB_PROCESSES;
    if (g_cfg.updateSpeed < SPEED_HIGH || g_cfg.updateSpeed > SPEED_PAUSED)
        g_cfg.updateSpeed = SPEED_NORMAL;
    if (g_cfg.appsViewMode != LV_VIEW_ICON &&
        g_cfg.appsViewMode != LV_VIEW_SMALLICON &&
        g_cfg.appsViewMode != LV_VIEW_DETAILS)
        g_cfg.appsViewMode = LV_VIEW_DETAILS;
    if (g_cfg.netHistoryMode < 0 || g_cfg.netHistoryMode > 2)
        g_cfg.netHistoryMode = 0;
}

void Settings_Save(void)
{
    if (g_cfg.haveNormalRect) Reg_SetBinary(L"WindowRect", &g_cfg.rcNormal, sizeof(RECT));
    if (g_cfg.haveTinyRect)   Reg_SetBinary(L"TinyRect",   &g_cfg.rcTiny,   sizeof(RECT));

    Reg_SetDword(L"Maximized",            g_cfg.maximized ? 1 : 0);
    Reg_SetDword(L"TinyFootprint",        g_cfg.tiny ? 1 : 0);
    Reg_SetDword(L"ActiveTab",            (DWORD)g_cfg.activeTab);
    Reg_SetDword(L"UpdateSpeed",          (DWORD)g_cfg.updateSpeed);
    Reg_SetDword(L"AlwaysOnTop",          g_cfg.alwaysOnTop ? 1 : 0);
    Reg_SetDword(L"MinimizeOnUse",        g_cfg.minimizeOnUse ? 1 : 0);
    Reg_SetDword(L"HideWhenMinimized",    g_cfg.hideWhenMinimized ? 1 : 0);
    Reg_SetDword(L"ShowFullAccountName",  g_cfg.showFullAccountName ? 1 : 0);
    Reg_SetDword(L"ShowAllUsers",         g_cfg.showAllUsers ? 1 : 0);
    Reg_SetDword(L"AppsViewMode",         (DWORD)g_cfg.appsViewMode);
    Reg_SetDword(L"CpuHistoryPerCpu",     g_cfg.perfOneGraphPerCpu ? 1 : 0);
    Reg_SetDword(L"ShowKernelTimes",      g_cfg.perfShowKernelTimes ? 1 : 0);
    Reg_SetDword(L"NetworkHistoryMode",   (DWORD)g_cfg.netHistoryMode);
    Reg_SetDword(L"ProcessTreeView",      g_cfg.procTreeMode ? 1 : 0);
}
