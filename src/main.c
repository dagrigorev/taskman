/* ------------------------------------------------------------------------
 * main.c - Classic Task Manager
 *
 * Message loop, main window, menu bar, tab host, status bar, DPI and font
 * handling, tiny footprint mode, notification-area CPU meter, the
 * File > New Task (Run...) dialog and the shared UI helpers.
 * ------------------------------------------------------------------------ */
#include "app.h"
#include "ui.h"
#include "gpu.h"
#include "sensors.h"

/* --------------------------------------------------------------- state -- */

HINSTANCE g_hInst;
HWND      g_hMain;
HFONT     g_hFont;
UINT      g_dpi = 96;

static HWND      g_hTabs;
static HWND      g_hStatus;
static HWND      g_hDashboard;
static int       g_resumeSpeed = SPEED_NORMAL;
static HMENU     g_hMenuBar;
static HMENU     g_hViewMenu;
static HMENU     g_hSpeedMenu;
static HACCEL    g_hAccel;
static TabPage  *g_page[TAB_COUNT];
static int       g_active = TAB_PROCESSES;
static BOOL      g_trayShown;
static HICON     g_trayIcon;
static UINT      g_msgTaskbarCreated;

static const WCHAR kClassName[]  = L"ClassicTaskManagerWndClass";
static const WCHAR kMutexName[]  = L"Local\\ClassicTaskManagerSingleInstance";
static const WCHAR kWindowTitle[] = L"Task Manager | System Workspace";

/* ----------------------------------------------------------- DPI shims -- */

typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
typedef BOOL (WINAPI *PFN_SystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);

static PFN_GetDpiForWindow            p_GetDpiForWindow;
static PFN_GetDpiForSystem            p_GetDpiForSystem;
static PFN_SystemParametersInfoForDpi p_SystemParametersInfoForDpi;

static void InitDpiApi(void)
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    p_GetDpiForWindow = (PFN_GetDpiForWindow)(void *)
        GetProcAddress(u32, "GetDpiForWindow");
    p_GetDpiForSystem = (PFN_GetDpiForSystem)(void *)
        GetProcAddress(u32, "GetDpiForSystem");
    p_SystemParametersInfoForDpi = (PFN_SystemParametersInfoForDpi)(void *)
        GetProcAddress(u32, "SystemParametersInfoForDpi");
}

static UINT QueryDpi(HWND hwnd)
{
    if (hwnd && p_GetDpiForWindow) {
        UINT d = p_GetDpiForWindow(hwnd);
        if (d) return d;
    }
    if (p_GetDpiForSystem) {
        UINT d = p_GetDpiForSystem();
        if (d) return d;
    }
    {
        HDC hdc = GetDC(NULL);
        UINT d = 96;
        if (hdc) {
            int lx = GetDeviceCaps(hdc, LOGPIXELSX);
            if (lx > 0) d = (UINT)lx;
            ReleaseDC(NULL, hdc);
        }
        return d;
    }
}

/* ---------------------------------------------------------------- font -- */

static void CreateAppFont(void)
{
    NONCLIENTMETRICSW ncm;
    HFONT nf = NULL;
    BOOL ok = FALSE;

    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (p_SystemParametersInfoForDpi)
        ok = p_SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,
                                          sizeof(ncm), &ncm, 0, g_dpi);
    if (!ok) {
        ZeroMemory(&ncm, sizeof(ncm));
        ncm.cbSize = sizeof(ncm);
        ok = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        if (ok && g_dpi != 96)
            ncm.lfMessageFont.lfHeight =
                MulDiv(ncm.lfMessageFont.lfHeight, (int)g_dpi, 96);
    }
    if (ok) {
        ncm.lfMessageFont.lfHeight = -DPX(13);
        nf = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (!nf) {
        LOGFONTW lf;
        ZeroMemory(&lf, sizeof(lf));
        lf.lfHeight  = -MulDiv(9, (int)g_dpi, 72);
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
        nf = CreateFontIndirectW(&lf);
    }
    if (nf) {
        HFONT old = g_hFont;
        g_hFont = nf;
        if (old) DeleteObject(old);
    }
    UI_ThemeInit();
}

static BOOL CALLBACK SetFontProc(HWND hwnd, LPARAM lp)
{
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)(HFONT)lp, MAKELPARAM(TRUE, 0));
    return TRUE;
}

void UI_ApplyFont(HWND parent)
{
    SendMessageW(parent, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
    EnumChildWindows(parent, SetFontProc, (LPARAM)g_hFont);
}

/* ----------------------------------------------------------- UI helpers -- */

static int DluX(HWND dlg, int x)
{
    RECT r; r.left = 0; r.top = 0; r.right = x; r.bottom = 0;
    MapDialogRect(dlg, &r);
    return r.right;
}

static int DluY(HWND dlg, int y)
{
    RECT r; r.left = 0; r.top = 0; r.right = 0; r.bottom = y;
    MapDialogRect(dlg, &r);
    return r.bottom;
}

SIZE UI_ButtonSize(HWND page)
{
    SIZE s;
    s.cx = DluX(page, 54);
    s.cy = DluY(page, 14);
    if (s.cy < DPX(30)) s.cy = DPX(30);
    return s;
}

int UI_Margin(HWND page)
{
    return DluX(page, 7);
}

int UI_LineHeight(HWND page)
{
    return DluY(page, 10);
}

HWND UI_CreateListView(HWND parent, int id, DWORD extraStyle)
{
    HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                              LVS_REPORT | LVS_SHOWSELALWAYS | extraStyle,
                              0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                              g_hInst, NULL);
    if (!lv) return NULL;
    ListView_SetExtendedListViewStyle(lv,
        LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP |
        LVS_EX_DOUBLEBUFFER  | LVS_EX_LABELTIP);
    SendMessageW(lv, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
    UI_StyleList(lv);
    return lv;
}

void UI_AddColumn(HWND lv, int index, const WCHAR *text, int widthDlu, int fmt)
{
    LVCOLUMNW col;
    ZeroMemory(&col, sizeof(col));
    col.mask     = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
    col.fmt      = fmt;
    col.cx       = DluX(GetParent(lv), widthDlu);
    col.pszText  = UI_Str(text);
    col.iSubItem = index;
    ListView_InsertColumn(lv, index, &col);
}

HWND UI_CreateButton(HWND parent, int id, const WCHAR *text, DWORD extra)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             g_hInst, NULL);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
    return h;
}

HWND UI_CreateStatic(HWND parent, int id, const WCHAR *text, DWORD extra)
{
    HWND h = CreateWindowExW(0, L"STATIC", text,
                             WS_CHILD | WS_VISIBLE | extra,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             g_hInst, NULL);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
    return h;
}

HWND UI_CreateGroupBox(HWND parent, int id, const WCHAR *text)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             g_hInst, NULL);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
    return h;
}

void UI_PlaceButtonRow(HWND page, const int *ids, int count, int cx, int cy)
{
    SIZE bs = UI_ButtonSize(page);
    int margin = UI_Margin(page);
    int gap = DluX(page, 5);
    int x = cx - margin;
    int y = cy - margin - bs.cy;
    int i;
    HDWP dwp;

    if (count <= 0) return;
    dwp = BeginDeferWindowPos(count);
    for (i = count - 1; i >= 0; i--) {
        HWND h = GetDlgItem(page, ids[i]);
        SIZE ideal = bs;
        int width;
        if (!h) continue;
        SendMessageW(h, BCM_GETIDEALSIZE, 0, (LPARAM)&ideal);
        width = ideal.cx + 2 * gap > bs.cx ? ideal.cx + 2 * gap : bs.cx;
        x -= width;
        if (dwp)
            dwp = DeferWindowPos(dwp, h, NULL, x, y, width, bs.cy,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
        else
            SetWindowPos(h, NULL, x, y, width, bs.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        x -= gap;
    }
    if (dwp) EndDeferWindowPos(dwp);
}

void UI_SetHeaderSortArrow(HWND lv, int column, int direction)
{
    HWND hdr = ListView_GetHeader(lv);
    int count, i;

    if (!hdr) return;
    count = Header_GetItemCount(hdr);
    for (i = 0; i < count; i++) {
        HDITEMW hd;
        ZeroMemory(&hd, sizeof(hd));
        hd.mask = HDI_FORMAT;
        if (!Header_GetItem(hdr, i, &hd)) continue;
        hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == column && direction > 0) hd.fmt |= HDF_SORTUP;
        if (i == column && direction < 0) hd.fmt |= HDF_SORTDOWN;
        Header_SetItem(hdr, i, &hd);
    }
}

void UI_FormatNumber(ULONGLONG value, WCHAR *buf, size_t cch)
{
    WCHAR raw[32];
    WCHAR sep[8];
    int len, groups, outLen, i, o, digits;

    StringCchPrintfW(raw, ARRAYSIZE(raw), L"%llu", (unsigned long long)value);
    if (!GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_STHOUSAND, sep, 8) || !sep[0])
        lstrcpynW(sep, L",", 8);

    len = lstrlenW(raw);
    groups = (len - 1) / 3;
    outLen = len + groups * lstrlenW(sep);
    if ((size_t)outLen + 1 > cch) { lstrcpynW(buf, raw, (int)cch); return; }

    o = outLen;
    buf[o--] = L'\0';
    digits = 0;
    for (i = len - 1; i >= 0; i--) {
        buf[o--] = raw[i];
        digits++;
        if (digits % 3 == 0 && i > 0) {
            int s = lstrlenW(sep);
            while (s > 0) buf[o--] = sep[--s];
        }
    }
    (void)o;
}

void UI_FormatKB(ULONGLONG bytes, WCHAR *buf, size_t cch)
{
    WCHAR num[48];
    UI_FormatNumber((bytes + 1023) / 1024, num, 48);
    StringCchPrintfW(buf, cch, L"%s K", num);
}

void UI_FormatSize(ULONGLONG bytes, WCHAR *buf, size_t cch)
{
    static const WCHAR *units[] = { L"B", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    if (u == 0)
        StringCchPrintfW(buf, cch, L"%llu %s", (unsigned long long)bytes, units[0]);
    else
        StringCchPrintfW(buf, cch, L"%.1f %s", v, units[u]);
}

/* ------------------------------------------------------------ tray icon -- */

static HICON BuildCpuMeterIcon(void)
{
    enum { N = 16 };
    BITMAPINFO bi;
    void *bits = NULL;
    HDC screen;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON icon = NULL;
    DWORD *px;
    BYTE maskBits[2 * N];
    float hist[N];
    int x, y;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = N;
    bi.bmiHeader.biHeight      = -N;         /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    if (!screen) return NULL;
    color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!color || !bits) { if (color) DeleteObject(color); return NULL; }

    px = (DWORD *)bits;
    for (y = 0; y < N * N; y++) px[y] = 0xFF000000u;    /* opaque black */

    SysInfo_CopyCpuHistory(hist, N);
    for (x = 0; x < N; x++) {
        float v = hist[x];
        int h;
        if (v < 0.0f)   v = 0.0f;
        if (v > 100.0f) v = 100.0f;
        h = (int)((v / 100.0f) * (float)N + 0.5f);
        for (y = N - h; y < N; y++) {
            DWORD c = (y == N - h) ? 0xFF00FF00u : 0xFF00C000u;
            px[y * N + x] = c;
        }
    }

    ZeroMemory(maskBits, sizeof(maskBits));
    mask = CreateBitmap(N, N, 1, 1, maskBits);
    if (!mask) { DeleteObject(color); return NULL; }

    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = color;
    icon = CreateIconIndirect(&ii);

    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static void TrayFill(NOTIFYICONDATAW *nid)
{
    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize = sizeof(*nid);
    nid->hWnd   = g_hMain;
    nid->uID    = 1;
}

static void TrayUpdate(double cpu)
{
    NOTIFYICONDATAW nid;
    HICON icon;

    if (!g_trayShown) return;
    icon = BuildCpuMeterIcon();
    if (!icon) return;

    TrayFill(&nid);
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon  = icon;
    StringCchPrintfW(nid.szTip, ARRAYSIZE(nid.szTip),
                     L"CPU Usage: %d%%", (int)(cpu + 0.5));
    Shell_NotifyIconW(NIM_MODIFY, &nid);

    if (g_trayIcon) DestroyIcon(g_trayIcon);
    g_trayIcon = icon;
}

static void TrayAdd(void)
{
    NOTIFYICONDATAW nid;
    HICON icon;
    BOOL owned;

    if (g_trayShown) return;
    icon = BuildCpuMeterIcon();
    owned = (icon != NULL);
    if (!icon) icon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));

    TrayFill(&nid);
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon            = icon;
    lstrcpynW(nid.szTip, L"Task Manager", ARRAYSIZE(nid.szTip));
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_trayShown = TRUE;
        if (g_trayIcon) DestroyIcon(g_trayIcon);
        g_trayIcon = owned ? icon : NULL;   /* shared icons are never destroyed */
    } else if (owned) {
        DestroyIcon(icon);
    }
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayShown) return;
    TrayFill(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayShown = FALSE;
    if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
}

static void TrayContextMenu(void)
{
    HMENU menu = CreatePopupMenu();
    POINT pt;
    int cmd;

    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IDM_TRAY_RESTORE, L"&Restore");
    AppendMenuW(menu, MF_STRING | (g_cfg.alwaysOnTop ? MF_CHECKED : 0),
                IDM_TRAY_ALWAYSONTOP, L"&Always On Top");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"E&xit");
    SetMenuDefaultItem(menu, IDM_TRAY_RESTORE, FALSE);

    GetCursorPos(&pt);
    SetForegroundWindow(g_hMain);
    cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, g_hMain, NULL);
    DestroyMenu(menu);
    if (cmd) PostMessageW(g_hMain, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

/* ------------------------------------------------------------ new task --- */

#define RUN_MRU_MAX   10
#define RUN_MRU_CCH   (RUN_MRU_MAX * (MAX_PATH + 1) + 1)

static void RunMruLoad(HWND combo)
{
    WCHAR *buf = (WCHAR *)calloc(RUN_MRU_CCH, sizeof(WCHAR));
    const WCHAR *p;
    if (!buf) return;
    if (Reg_GetMultiString(L"RunMRU", buf, RUN_MRU_CCH)) {
        for (p = buf; *p; p += lstrlenW(p) + 1)
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)p);
    }
    free(buf);
}

static void RunMruAdd(const WCHAR *cmd)
{
    WCHAR *oldBuf = (WCHAR *)calloc(RUN_MRU_CCH, sizeof(WCHAR));
    WCHAR *newBuf = (WCHAR *)calloc(RUN_MRU_CCH, sizeof(WCHAR));
    const WCHAR *p;
    size_t o = 0;
    int items = 0;
    size_t len;

    if (!oldBuf || !newBuf) { free(oldBuf); free(newBuf); return; }

    len = (size_t)lstrlenW(cmd);
    if (len && o + len + 2 < RUN_MRU_CCH) {
        memcpy(newBuf + o, cmd, (len + 1) * sizeof(WCHAR));
        o += len + 1;
        items = 1;
    }
    if (Reg_GetMultiString(L"RunMRU", oldBuf, RUN_MRU_CCH)) {
        for (p = oldBuf; *p && items < RUN_MRU_MAX; p += lstrlenW(p) + 1) {
            if (CompareStringOrdinal(p, -1, cmd, -1, TRUE) == CSTR_EQUAL) continue;
            len = (size_t)lstrlenW(p);
            if (o + len + 2 >= RUN_MRU_CCH) break;
            memcpy(newBuf + o, p, (len + 1) * sizeof(WCHAR));
            o += len + 1;
            items++;
        }
    }
    newBuf[o++] = L'\0';
    Reg_SetMultiString(L"RunMRU", newBuf, (DWORD)o);
    free(oldBuf);
    free(newBuf);
}

/* Splits "c:\path\app.exe -a -b" into file and arguments, honouring quotes. */
static void SplitCommand(const WCHAR *cmd, WCHAR *file, size_t fileCch,
                         WCHAR *args, size_t argsCch)
{
    const WCHAR *p = cmd;
    const WCHAR *start;
    size_t n;

    while (*p == L' ' || *p == L'\t') p++;
    if (*p == L'"') {
        p++;
        start = p;
        while (*p && *p != L'"') p++;
        n = (size_t)(p - start);
        if (*p == L'"') p++;
    } else {
        start = p;
        while (*p && *p != L' ' && *p != L'\t') p++;
        n = (size_t)(p - start);
    }
    if (n >= fileCch) n = fileCch - 1;
    memcpy(file, start, n * sizeof(WCHAR));
    file[n] = L'\0';

    while (*p == L' ' || *p == L'\t') p++;
    lstrcpynW(args, p, (int)argsCch);
}

static BOOL LaunchTask(HWND owner, const WCHAR *cmd, BOOL admin)
{
    WCHAR file[MAX_PATH * 2];
    WCHAR args[MAX_PATH * 4];
    SHELLEXECUTEINFOW sei;

    if (!cmd || !cmd[0]) return FALSE;

    if (!admin) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        WCHAR *mutableCmd;
        size_t cch = (size_t)lstrlenW(cmd) + 1;

        mutableCmd = (WCHAR *)malloc(cch * sizeof(WCHAR));
        if (mutableCmd) {
            memcpy(mutableCmd, cmd, cch * sizeof(WCHAR));
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));
            if (CreateProcessW(NULL, mutableCmd, NULL, NULL, FALSE,
                               0, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                free(mutableCmd);
                return TRUE;
            }
            free(mutableCmd);
        }
    }

    /* Not an executable we could start directly (a document, a folder, a URL)
       or elevation was requested: hand it to the shell. */
    SplitCommand(cmd, file, ARRAYSIZE(file), args, ARRAYSIZE(args));
    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.hwnd   = owner;
    sei.lpVerb = admin ? L"runas" : NULL;
    sei.lpFile = file;
    sei.lpParameters = args[0] ? args : NULL;
    sei.nShow  = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return TRUE;

    if (GetLastError() != ERROR_CANCELLED) {
        WCHAR msg[512];
        StringCchPrintfW(msg, ARRAYSIZE(msg),
                         L"Task Manager cannot find '%s'. Make sure you typed "
                         L"the name correctly, and then try again.", file);
        MessageBoxW(owner, msg, kWindowTitle, MB_OK | MB_ICONWARNING);
    }
    return FALSE;
}

static INT_PTR CALLBACK RunDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        HWND combo = GetDlgItem(dlg, IDC_RUN_COMBO);
        HICON icon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        if (icon) SendDlgItemMessageW(dlg, IDC_RUN_ICON, STM_SETICON,
                                      (WPARAM)icon, 0);
        RunMruLoad(combo);
        SendMessageW(combo, CB_LIMITTEXT, MAX_PATH * 2, 0);
        SetFocus(combo);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            WCHAR cmd[MAX_PATH * 4];
            BOOL admin = (IsDlgButtonChecked(dlg, IDC_RUN_ADMIN) == BST_CHECKED);
            cmd[0] = L'\0';
            GetDlgItemTextW(dlg, IDC_RUN_COMBO, cmd, ARRAYSIZE(cmd));
            StrTrimW(cmd, L" \t");
            if (!cmd[0]) { SetFocus(GetDlgItem(dlg, IDC_RUN_COMBO)); return TRUE; }
            if (LaunchTask(dlg, cmd, admin)) {
                RunMruAdd(cmd);
                EndDialog(dlg, IDOK);
            }
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        default:
            break;
        }
        break;
    default:
        break;
    }
    return FALSE;
}

BOOL App_RunTaskDialog(HWND owner)
{
    return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_RUNDLG), owner,
                           RunDlgProc, 0) == IDOK;
}

BOOL App_IsElevated(void)
{
    HANDLE token = NULL;
    TOKEN_ELEVATION elevation;
    DWORD size;
    BOOL result = FALSE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
        result = elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return result;
}

static BOOL IsElevatedRelaunch(const WCHAR *cmdLine)
{
    return cmdLine && lstrcmpW(cmdLine, L"--elevated-relaunch") == 0 && App_IsElevated();
}

void App_RelaunchElevated(void)
{
    WCHAR path[MAX_PATH];
    SHELLEXECUTEINFOW sei;
    DWORD length;

    length = GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return;

    Settings_Save();

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOASYNC;
    sei.hwnd   = g_hMain;
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = L"--elevated-relaunch";
    sei.nShow  = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei))
        DestroyWindow(g_hMain);
}

/* Fixed tool buttons must not search the inherited working directory or
   consult a document association to choose their executable. */
BOOL App_OpenSystemTool(HWND owner, BOOL services)
{
    WCHAR directory[MAX_PATH], file[MAX_PATH], args[MAX_PATH + 2];
    UINT length = GetSystemDirectoryW(directory, ARRAYSIZE(directory));
    SHELLEXECUTEINFOW sei;

    if (!length || length >= ARRAYSIZE(directory)) return FALSE;
    if (FAILED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\%s", directory,
                                services ? L"mmc.exe" : L"resmon.exe")))
        return FALSE;
    args[0] = L'\0';
    if (services && FAILED(StringCchPrintfW(args, ARRAYSIZE(args),
                                           L"\"%s\\services.msc\"", directory)))
        return FALSE;

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpFile = file;
    sei.lpParameters = services ? args : NULL;
    sei.lpDirectory = directory;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei);
}

/* ---------------------------------------------------------------- menu --- */

static void BuildMenuBar(void)
{
    HMENU old = g_hMenuBar;
    HMENU bar, file, options, view, speed, windows, help;

    bar = CreateMenu();
    if (!bar) return;

    file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_FILE_NEWTASK, L"&New Task (Run...)");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"E&xit Task Manager");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");

    options = CreatePopupMenu();
    AppendMenuW(options, MF_STRING, IDM_OPTIONS_ALWAYSONTOP,     L"&Always On Top");
    AppendMenuW(options, MF_STRING, IDM_OPTIONS_MINIMIZEONUSE,   L"&Minimize On Use");
    AppendMenuW(options, MF_STRING, IDM_OPTIONS_HIDEWHENMIN,     L"&Hide When Minimized");
    AppendMenuW(options, MF_STRING, IDM_OPTIONS_FULLACCOUNTNAME, L"Show &full account name");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)options, L"&Options");

    view  = CreatePopupMenu();
    speed = CreatePopupMenu();
    AppendMenuW(speed, MF_STRING, IDM_VIEW_SPEED_HIGH,   L"&High");
    AppendMenuW(speed, MF_STRING, IDM_VIEW_SPEED_NORMAL, L"&Normal");
    AppendMenuW(speed, MF_STRING, IDM_VIEW_SPEED_LOW,    L"&Low");
    AppendMenuW(speed, MF_STRING, IDM_VIEW_SPEED_PAUSED, L"&Paused");
    AppendMenuW(view, MF_STRING, IDM_VIEW_REFRESH, L"&Refresh Now\tF5");
    AppendMenuW(view, MF_STRING, IDM_VIEW_TOGGLEPAUSE, L"Pause / resume updates\tCtrl+P");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)speed, L"&Update Speed");
    if (g_page[g_active] && g_page[g_active]->BuildViewMenu)
        g_page[g_active]->BuildViewMenu(g_page[g_active], view);
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    g_hViewMenu  = view;
    g_hSpeedMenu = speed;

    if (g_active == TAB_APPS) {
        windows = CreatePopupMenu();
        AppendMenuW(windows, MF_STRING, IDM_WINDOWS_TILEHORZ,     L"Tile &Horizontally");
        AppendMenuW(windows, MF_STRING, IDM_WINDOWS_TILEVERT,     L"Tile &Vertically");
        AppendMenuW(windows, MF_STRING, IDM_WINDOWS_MINIMIZE,     L"Mi&nimize");
        AppendMenuW(windows, MF_STRING, IDM_WINDOWS_MAXIMIZE,     L"Ma&ximize");
        AppendMenuW(windows, MF_STRING, IDM_WINDOWS_CASCADE,      L"&Cascade");
        AppendMenuW(windows, MF_STRING, IDM_WINDOWS_BRINGTOFRONT, L"&Bring To Front");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)windows, L"&Windows");
    }

    help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"&About Task Manager");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");

    g_hMenuBar = bar;
    if (!g_cfg.tiny) SetMenu(g_hMain, bar);
    if (old) DestroyMenu(old);
    if (!g_cfg.tiny) DrawMenuBar(g_hMain);
}

static void UpdateMenuChecks(void)
{
    if (!g_hMenuBar) return;
    CheckMenuItem(g_hMenuBar, IDM_OPTIONS_ALWAYSONTOP,
                  MF_BYCOMMAND | (g_cfg.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_hMenuBar, IDM_OPTIONS_MINIMIZEONUSE,
                  MF_BYCOMMAND | (g_cfg.minimizeOnUse ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_hMenuBar, IDM_OPTIONS_HIDEWHENMIN,
                  MF_BYCOMMAND | (g_cfg.hideWhenMinimized ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(g_hMenuBar, IDM_OPTIONS_FULLACCOUNTNAME,
                  MF_BYCOMMAND | (g_cfg.showFullAccountName ? MF_CHECKED : MF_UNCHECKED));
    if (g_hSpeedMenu)
        CheckMenuRadioItem(g_hSpeedMenu, IDM_VIEW_SPEED_HIGH, IDM_VIEW_SPEED_PAUSED,
                           (UINT)(IDM_VIEW_SPEED_HIGH + g_cfg.updateSpeed), MF_BYCOMMAND);
    if (g_page[g_active] && g_page[g_active]->InitViewMenu && g_hViewMenu)
        g_page[g_active]->InitViewMenu(g_page[g_active], g_hViewMenu);
}

/* -------------------------------------------------------------- layout --- */

static void LayoutMain(void)
{
    RECT rc, rcStatus, rcTab, rcPage;
    int cx, cy, statusH = 0, margin;
    HDWP dwp;
    TabPage *page = g_page[g_active];

    if (!g_hMain) return;
    GetClientRect(g_hMain, &rc);
    cx = rc.right;
    cy = rc.bottom;
    if (cx <= 0 || cy <= 0) return;

    if (!g_cfg.tiny && g_hStatus) {
        int parts[3];
        SendMessageW(g_hStatus, WM_SIZE, 0, 0);
        GetWindowRect(g_hStatus, &rcStatus);
        statusH = rcStatus.bottom - rcStatus.top;
        parts[0] = cx - DPX(115) - DPX(150);
        parts[1] = cx - DPX(150);
        parts[2] = -1;
        if (parts[0] < DPX(40)) parts[0] = DPX(40);
        if (parts[1] < parts[0] + DPX(40)) parts[1] = parts[0] + DPX(40);
        SendMessageW(g_hStatus, SB_SETPARTS, 3, (LPARAM)parts);
    }

    if (g_cfg.tiny) {
        if (page && page->hwnd) {
            SetWindowPos(page->hwnd, HWND_TOP, 0, 0, cx, cy,
                         SWP_NOACTIVATE);
            if (page->OnLayout) page->OnLayout(page, cx, cy, TRUE);
        }
        return;
    }

    margin = DPX(18);
    ShowWindow(g_hDashboard, SW_SHOW);
    MoveWindow(g_hDashboard, 0, 0, cx, cy < DPX(750) ? DPX(172) : DPX(208), TRUE);
    rcTab.left   = margin;
    rcTab.top    = cy < DPX(750) ? DPX(172) : DPX(208);
    rcTab.right  = cx - margin;
    rcTab.bottom = rcTab.top + DPX(44);
    TabCtrl_SetItemSize(g_hTabs, (cx - 2 * margin - DPX(4)) / TAB_COUNT, DPX(38));
    if (rcTab.right <= rcTab.left)  rcTab.right  = rcTab.left + 1;
    if (rcTab.bottom <= rcTab.top)  rcTab.bottom = rcTab.top + 1;

    dwp = BeginDeferWindowPos(2);
    if (dwp)
        dwp = DeferWindowPos(dwp, g_hTabs, NULL, rcTab.left, rcTab.top,
                             rcTab.right - rcTab.left, rcTab.bottom - rcTab.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
    else
        SetWindowPos(g_hTabs, NULL, rcTab.left, rcTab.top,
                     rcTab.right - rcTab.left, rcTab.bottom - rcTab.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);

    rcPage = rcTab;
    rcPage.top = rcTab.bottom + DPX(6);
    rcPage.bottom = cy - statusH - DPX(8);
    if (rcPage.right <= rcPage.left)  rcPage.right  = rcPage.left + 1;
    if (rcPage.bottom <= rcPage.top)  rcPage.bottom = rcPage.top + 1;

    if (page && page->hwnd) {
        if (dwp)
            dwp = DeferWindowPos(dwp, page->hwnd, HWND_TOP,
                                 rcPage.left, rcPage.top,
                                 rcPage.right - rcPage.left,
                                 rcPage.bottom - rcPage.top, SWP_NOACTIVATE);
        else
            SetWindowPos(page->hwnd, HWND_TOP, rcPage.left, rcPage.top,
                         rcPage.right - rcPage.left,
                         rcPage.bottom - rcPage.top, SWP_NOACTIVATE);
    }
    if (dwp) EndDeferWindowPos(dwp);

    if (page && page->hwnd && page->OnLayout)
        page->OnLayout(page, rcPage.right - rcPage.left,
                       rcPage.bottom - rcPage.top, FALSE);
}

void App_SetStatusText(int part, const WCHAR *text)
{
    if (g_hStatus)
        SendMessageW(g_hStatus, SB_SETTEXTW, (WPARAM)part, (LPARAM)text);
}

void App_MinimizeOnUse(void)
{
    if (g_cfg.minimizeOnUse) ShowWindow(g_hMain, SW_MINIMIZE);
}

HWND App_TabControl(void)
{
    return g_hTabs;
}

/* ---------------------------------------------------- tab pages --------- */

typedef struct {
    HDC  hdc;
    HWND page;
} ExcludeCtx;

/* Children that paint every pixel of their rectangle are clipped out of the
   page's background erase, so the erase costs nothing and cannot flicker. */
static BOOL CALLBACK ExcludeOpaqueChild(HWND child, LPARAM lp)
{
    ExcludeCtx *ctx = (ExcludeCtx *)lp;
    WCHAR cls[64];
    LONG style;
    RECT rc;
    BOOL opaque = FALSE;

    if (!IsWindowVisible(child)) return TRUE;
    if (!GetClassNameW(child, cls, ARRAYSIZE(cls))) return TRUE;

    style = GetWindowLongW(child, GWL_STYLE);
    if (lstrcmpiW(cls, WC_LISTVIEWW) == 0)
        opaque = TRUE;
    else if (lstrcmpiW(cls, L"Static") == 0 &&
             (style & SS_TYPEMASK) == SS_BLACKRECT)
        opaque = TRUE;

    if (!opaque) return TRUE;

    GetWindowRect(child, &rc);
    MapWindowPoints(NULL, ctx->page, (POINT *)&rc, 2);
    ExcludeClipRect(ctx->hdc, rc.left, rc.top, rc.right, rc.bottom);
    return TRUE;
}

static INT_PTR CALLBACK PageDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    TabPage *p = (TabPage *)(LONG_PTR)GetWindowLongPtrW(dlg, GWLP_USERDATA);

    switch (msg) {
    case WM_ERASEBKGND: {
        /* The page cannot use WS_CLIPCHILDREN: a group box paints only its
           frame and caption, so with the children clipped out nothing would
           ever erase the area inside a group and the previous tab's pixels
           would stay on screen.  Instead the page erases its whole client
           area and clips out only the children that paint every pixel
           themselves, which keeps the erase correct without flicker. */
        HDC hdc = (HDC)wp;
        RECT rc;
        ExcludeCtx ctx;
        int saved;

        GetClientRect(dlg, &rc);
        saved = SaveDC(hdc);
        ctx.hdc = hdc;
        ctx.page = dlg;
        EnumChildWindows(dlg, ExcludeOpaqueChild, (LPARAM)&ctx);
        FillRect(hdc, &rc, UI_BackgroundBrush());
        if (saved) RestoreDC(hdc, saved);
        return TRUE;
    }

    case WM_INITDIALOG:
        p = (TabPage *)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)p);
        p->hwnd = dlg;
        EnableThemeDialogTexture(dlg, ETDT_DISABLE);
        SendMessageW(dlg, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(FALSE, 0));
        if (p->OnCreate) p->OnCreate(p);
        return FALSE;

    case WM_COMMAND:
        if (p && p->OnCommand) {
            p->OnCommand(p, LOWORD(wp), HIWORD(wp), (HWND)lp);
            return TRUE;
        }
        break;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG:
        SetTextColor((HDC)wp, UI_INK);
        SetBkColor((HDC)wp, UI_BG);
        return (INT_PTR)UI_BackgroundBrush();

    case WM_NOTIFY:
        if (p && p->OnNotify) {
            LRESULT result = 0;
            if (p->OnNotify(p, (NMHDR *)lp, &result)) {
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, (LONG_PTR)result);
                return TRUE;
            }
        }
        {
            LRESULT result = 0;
            if (UI_ControlNotify((NMHDR *)lp, &result)) {
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, result);
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDBLCLK:
        /* In tiny footprint mode the page fills the client area, so the
           double-click that restores the full window lands here rather than
           on the main window or the tab control. */
        if (g_cfg.tiny) {
            PostMessageW(g_hMain, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_TINY, 0), 0);
            return TRUE;
        }
        break;

    case WM_CONTEXTMENU:
        if (p && p->OnContextMenu) {
            p->OnContextMenu(p, (HWND)wp, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (p && p->OnDestroy) p->OnDestroy(p);
        return FALSE;

    default:
        break;
    }
    return FALSE;
}

static void TabCollect(int tab)
{
    switch (tab) {
    case TAB_PROCESSES:  Proc_Collect();  break;
    case TAB_APPS:       Apps_Collect();  break;
    case TAB_SERVICES:   Svc_Collect();   break;
    case TAB_USERS:      Users_Collect(); break;
    case TAB_NETWORKING: Net_Collect();   break;
    /* TAB_SENSORS has no tab collector: sysinfo.c calls Gpu_Collect on the
       collector thread ahead of this router, so the model is already
       current by the time the page reads it. */
    default: break;
    }
    /* Every other tab keeps hang timers running and feeds the status bar
       notice; it only enumerates captions, without probing windows. */
    if (tab != TAB_APPS) Apps_Watch();
}

static void SwitchToTab(int index, BOOL force)
{
    TabPage *old, *now;

    if (index < 0 || index >= TAB_COUNT) return;
    if (index == g_active && !force) return;

    old = g_page[g_active];
    now = g_page[index];

    if (old && old->hwnd && old != now) {
        if (old->OnActivate) old->OnActivate(old, FALSE);
        ShowWindow(old->hwnd, SW_HIDE);
    }
    g_active = index;
    g_cfg.activeTab = index;
    SysInfo_SetActiveTab(index);

    if (TabCtrl_GetCurSel(g_hTabs) != index)
        TabCtrl_SetCurSel(g_hTabs, index);
    InvalidateRect(g_hTabs, NULL, FALSE);

    BuildMenuBar();
    LayoutMain();

    if (now && now->hwnd) {
        ShowWindow(now->hwnd, SW_SHOW);
        RedrawWindow(now->hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        if (now->OnActivate) now->OnActivate(now, TRUE);
        if (now->OnSnapshot) now->OnSnapshot(now);
        SetFocus(now->PrimaryControl ? now->PrimaryControl(now) : now->hwnd);
    }
}

static void CreatePages(HWND parent)
{
    int i;
    g_page[TAB_APPS]        = TabApps();
    g_page[TAB_PROCESSES]   = TabProcesses();
    g_page[TAB_SERVICES]    = TabServices();
    g_page[TAB_PERFORMANCE] = TabPerformance();
    g_page[TAB_NETWORKING]  = TabNetworking();
    g_page[TAB_USERS]       = TabUsers();
    g_page[TAB_SENSORS]     = TabSensors();

    for (i = 0; i < TAB_COUNT; i++) {
        TCITEMW item;
        if (!g_page[i]) continue;
        g_page[i]->index = i;

        ZeroMemory(&item, sizeof(item));
        item.mask    = TCIF_TEXT;
        item.pszText = UI_Str(g_page[i]->title);
        TabCtrl_InsertItem(g_hTabs, i, &item);

        CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(IDD_TABPAGE), parent,
                           PageDlgProc, (LPARAM)g_page[i]);
        if (g_page[i]->hwnd) ShowWindow(g_page[i]->hwnd, SW_HIDE);
    }
}

/* --------------------------------------------------- tab ctrl subclass --- */

static LRESULT CALLBACK TabSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    switch (msg) {
    case WM_ERASEBKGND: return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        UI_DrawNavigation(hwnd, dc);
        EndPaint(hwnd, &ps); return 0;
    }
    case WM_LBUTTONDBLCLK: {
        TCHITTESTINFO ht;
        ht.pt.x = GET_X_LPARAM(lp);
        ht.pt.y = GET_Y_LPARAM(lp);
        ht.flags = 0;
        if (TabCtrl_HitTest(hwnd, &ht) < 0) {
            PostMessageW(g_hMain, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_TINY, 0), 0);
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            TabPage *p = g_page[g_active];
            if (p && p->hwnd) {
                HWND target = p->PrimaryControl ? p->PrimaryControl(p) : NULL;
                if (!target) target = GetNextDlgTabItem(p->hwnd, NULL, FALSE);
                if (target) { SetFocus(target); return 0; }
            }
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------ window placement -- */

static void SaveCurrentRect(void)
{
    WINDOWPLACEMENT wp;
    ZeroMemory(&wp, sizeof(wp));
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(g_hMain, &wp)) return;
    if (wp.showCmd == SW_SHOWMINIMIZED) return;
    g_cfg.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    if (g_cfg.tiny) {
        g_cfg.rcTiny = wp.rcNormalPosition;
        g_cfg.haveTinyRect = TRUE;
    } else {
        g_cfg.rcNormal = wp.rcNormalPosition;
        g_cfg.haveNormalRect = TRUE;
    }
}

static void ToggleTinyFootprint(void)
{
    BOOL tiny = !g_cfg.tiny;
    const RECT *target = NULL;
    TabPage *p;
    int i;

    SaveCurrentRect();
    g_cfg.tiny = tiny;

    SetMenu(g_hMain, tiny ? NULL : g_hMenuBar);
    ShowWindow(g_hTabs,   tiny ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hStatus, tiny ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hDashboard, tiny ? SW_HIDE : SW_SHOW);

    /* every page stays created; only the active one is visible */
    for (i = 0; i < TAB_COUNT; i++) {
        p = g_page[i];
        if (p && p->hwnd && i != g_active) ShowWindow(p->hwnd, SW_HIDE);
    }

    if (tiny && g_cfg.haveTinyRect)         target = &g_cfg.rcTiny;
    else if (!tiny && g_cfg.haveNormalRect) target = &g_cfg.rcNormal;

    if (target) {
        WINDOWPLACEMENT wp;
        ZeroMemory(&wp, sizeof(wp));
        wp.length = sizeof(wp);
        wp.showCmd = SW_SHOWNORMAL;
        wp.rcNormalPosition = *target;
        SetWindowPlacement(g_hMain, &wp);
    }
    if (!tiny) DrawMenuBar(g_hMain);
    LayoutMain();
    InvalidateRect(g_hMain, NULL, TRUE);
}

/* ----------------------------------------------------------- wnd proc --- */

/* The left status part. A hung application is called out on every tab,
   and clicking the part while it is shown opens the Applications tab. */
static void FormatStatusLeft(WCHAR *buf, size_t cch, BOOL paused, DWORD procs, int hung)
{
    StringCchPrintfW(buf, cch, L"  %s   |   %lu processes   |   ",
                     paused ? L"PAUSED" : L"LIVE", (unsigned long)procs);
    if (hung > 0)
        StringCchPrintfW(buf + lstrlenW(buf), cch - (size_t)lstrlenW(buf),
                         hung == 1 ? L"1 app not responding (click to view)"
                                   : L"%d apps not responding (click to view)", hung);
    else
        StringCchCatW(buf, cch, L"F5 refresh   Ctrl+F search");
}

static void OnSnapshotReady(void)
{
    const Snapshot *s = SysInfo_Lock();
    WCHAR text[64];
    double cpu = s->cpuUsage;
    DWORD  procs = s->processCount;
    double mem = s->memUsage;
    SysInfo_Unlock();
    UI_UpdateDashboard(g_hDashboard);

    if (!g_cfg.tiny) {
        WCHAR left[160];
        FormatStatusLeft(left, ARRAYSIZE(left), g_cfg.updateSpeed == SPEED_PAUSED,
                         procs, Apps_HungCount());
        App_SetStatusText(0, left);
        StringCchPrintfW(text, ARRAYSIZE(text), L"CPU Usage: %d%%",
                         (int)(cpu + 0.5));
        App_SetStatusText(1, text);
        StringCchPrintfW(text, ARRAYSIZE(text), L"Physical Memory: %d%%",
                         (int)(mem + 0.5));
        App_SetStatusText(2, text);
    }

    if (g_trayShown) TrayUpdate(cpu);

    if (g_page[g_active] && g_page[g_active]->hwnd &&
        g_page[g_active]->OnSnapshot)
        g_page[g_active]->OnSnapshot(g_page[g_active]);
}

static void ApplyAlwaysOnTop(void)
{
    SetWindowPos(g_hMain, g_cfg.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void RestoreFromTray(void)
{
    WINDOWPLACEMENT wp;
    BOOL maximized = FALSE;

    /* IsZoomed is FALSE for a minimized or hidden window, so asking it here
       would restore a window that had been maximized as a normal one. The
       placement's WPF_RESTORETOMAXIMIZED flag is what survives minimizing. */
    ZeroMemory(&wp, sizeof(wp));
    wp.length = sizeof(wp);
    if (GetWindowPlacement(g_hMain, &wp))
        maximized = (wp.showCmd == SW_SHOWMAXIMIZED) ||
                    ((wp.flags & WPF_RESTORETOMAXIMIZED) != 0);

    ShowWindow(g_hMain, SW_SHOW);
    ShowWindow(g_hMain, maximized ? SW_SHOWMAXIMIZED : SW_RESTORE);
    SetForegroundWindow(g_hMain);
    if (!g_cfg.hideWhenMinimized) TrayRemove();
}

static void OnCommand(HWND hwnd, int id, int code, HWND ctl)
{
    switch (id) {
    case IDM_FILE_NEWTASK:
        App_RunTaskDialog(hwnd);
        return;
    case IDM_FILE_EXIT:
    case IDM_TRAY_EXIT:
        DestroyWindow(hwnd);
        return;

    case IDM_OPTIONS_ALWAYSONTOP:
    case IDM_TRAY_ALWAYSONTOP:
        g_cfg.alwaysOnTop = !g_cfg.alwaysOnTop;
        ApplyAlwaysOnTop();
        return;
    case IDM_OPTIONS_MINIMIZEONUSE:
        g_cfg.minimizeOnUse = !g_cfg.minimizeOnUse;
        return;
    case IDM_OPTIONS_HIDEWHENMIN:
        g_cfg.hideWhenMinimized = !g_cfg.hideWhenMinimized;
        if (!g_cfg.hideWhenMinimized && !IsWindowVisible(hwnd)) RestoreFromTray();
        if (!g_cfg.hideWhenMinimized) TrayRemove();
        return;
    case IDM_OPTIONS_FULLACCOUNTNAME:
        g_cfg.showFullAccountName = !g_cfg.showFullAccountName;
        SysInfo_RefreshNow();
        return;

    case IDM_VIEW_REFRESH:
        SysInfo_RefreshNow();
        return;
    case IDM_PROC_FIND:
        SwitchToTab(TAB_PROCESSES, FALSE);
        if (g_page[TAB_PROCESSES] && g_page[TAB_PROCESSES]->OnCommand)
            g_page[TAB_PROCESSES]->OnCommand(g_page[TAB_PROCESSES], IDM_PROC_FIND, 0, NULL);
        return;
    case IDM_VIEW_BLAME_PEAK:
        SwitchToTab(TAB_PERFORMANCE, FALSE);
        if (g_page[TAB_PERFORMANCE] && g_page[TAB_PERFORMANCE]->OnCommand)
            g_page[TAB_PERFORMANCE]->OnCommand(g_page[TAB_PERFORMANCE], IDM_VIEW_BLAME_PEAK, 0, NULL);
        return;
    case IDM_VIEW_TOGGLEPAUSE:
        if (g_cfg.updateSpeed == SPEED_PAUSED) g_cfg.updateSpeed = g_resumeSpeed;
        else { g_resumeSpeed = g_cfg.updateSpeed; g_cfg.updateSpeed = SPEED_PAUSED; }
        SysInfo_SetSpeed(g_cfg.updateSpeed);
        OnSnapshotReady();
        return;
    case IDM_VIEW_SPEED_HIGH:
    case IDM_VIEW_SPEED_NORMAL:
    case IDM_VIEW_SPEED_LOW:
    case IDM_VIEW_SPEED_PAUSED:
        g_cfg.updateSpeed = id - IDM_VIEW_SPEED_HIGH;
        SysInfo_SetSpeed(g_cfg.updateSpeed);
        if (g_cfg.updateSpeed != SPEED_PAUSED) g_resumeSpeed = g_cfg.updateSpeed;
        OnSnapshotReady();
        return;

    case IDM_NEXT_TAB:
        SwitchToTab((g_active + 1) % TAB_COUNT, FALSE);
        return;
    case IDM_PREV_TAB:
        SwitchToTab((g_active + TAB_COUNT - 1) % TAB_COUNT, FALSE);
        return;
    case IDM_TOGGLE_TINY:
        ToggleTinyFootprint();
        return;

    case IDM_TRAY_RESTORE:
        RestoreFromTray();
        return;

    case IDM_HELP_ABOUT:
        MessageBoxW(hwnd,
                    L"Classic Task Manager\n"
                    L"System Workspace\n\nLive monitoring, process search and "
                    L"inspection, and snapshot export.\n\n"
                    L"Ctrl+F  Find process\nCtrl+P  Pause / resume\n"
                    L"Ctrl+N  Run new task\nF5  Refresh\nCtrl+Tab  Next tab\n\nVersion 2.0",
                    kWindowTitle, MB_OK | MB_ICONINFORMATION);
        return;

    default:
        break;
    }

    /* anything else belongs to the active page */
    if (g_page[g_active] && g_page[g_active]->OnCommand)
        g_page[g_active]->OnCommand(g_page[g_active], id, code, ctl);
}

static void FitWindowRect(RECT *window, const RECT *work)
{
    LONGLONG width = (LONGLONG)window->right - window->left;
    LONGLONG height = (LONGLONG)window->bottom - window->top;
    LONG availableW = work->right - work->left, availableH = work->bottom - work->top;
    if (availableW <= 0 || availableH <= 0) return;
    if (width > availableW) width = availableW;
    if (height > availableH) height = availableH;
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    if (window->left < work->left) window->left = work->left;
    if (window->top < work->top) window->top = work->top;
    if ((LONGLONG)window->left + width > work->right) window->left = work->right - (LONG)width;
    if ((LONGLONG)window->top + height > work->bottom) window->top = work->bottom - (LONG)height;
    window->right = window->left + (LONG)width;
    window->bottom = window->top + (LONG)height;
}

static BOOL WindowWorkArea(HWND hwnd, const RECT *target, RECT *work)
{
    MONITORINFO info = {0};
    HMONITOR monitor = target ? MonitorFromRect(target, MONITOR_DEFAULTTONEAREST) :
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return FALSE;
    *work = info.rcWork;
    return TRUE;
}

/* The header is a control container, not a dialog; share Tab traversal with
   the page controls so focus can enter and leave every part of the workspace. */
static BOOL HandleWorkspaceKey(MSG *msg)
{
    HWND focus = GetFocus(), next;
    if (msg->message != WM_KEYDOWN || !focus || !IsChild(g_hMain, focus)) return FALSE;
    if (msg->wParam == VK_TAB && !(GetKeyState(VK_CONTROL) & 0x8000)) {
        LRESULT code = SendMessageW(focus, WM_GETDLGCODE, msg->wParam, (LPARAM)msg);
        if (code & (DLGC_WANTTAB | DLGC_WANTALLKEYS)) return FALSE;
        next = GetNextDlgTabItem(g_hMain, focus, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        if (next) { SetFocus(next); return TRUE; }
    }
    if (msg->wParam == VK_RETURN && GetParent(focus) == g_hDashboard) {
        SendMessageW(focus, BM_CLICK, 0, 0); return TRUE;
    }
    return FALSE;
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hMain = hwnd;
        g_dpi = QueryDpi(hwnd);
        CreateAppFont();
        g_hDashboard = UI_CreateDashboard(hwnd);

        g_hTabs = CreateWindowExW(0, WC_TABCONTROLW, L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                                  WS_TABSTOP | TCS_FOCUSONBUTTONDOWN | TCS_FIXEDWIDTH,
                                  0, 0, 10, 10, hwnd,
                                  (HMENU)(INT_PTR)IDC_TABCTRL, g_hInst, NULL);
        if (!g_hTabs) return -1;
        SendMessageW(g_hTabs, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
        SetWindowSubclass(g_hTabs, TabSubclass, 1, 0);

        g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                    WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP |
                                    CCS_BOTTOM,
                                    0, 0, 10, 10, hwnd,
                                    (HMENU)(INT_PTR)IDC_STATUSBAR, g_hInst, NULL);
        if (g_hStatus)
            SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
        SetWindowTheme(g_hTabs, L"", L"");

        CreatePages(hwnd);

        g_active = g_cfg.activeTab;
        if (g_active < 0 || g_active >= TAB_COUNT) g_active = TAB_PROCESSES;
        TabCtrl_SetCurSel(g_hTabs, g_active);
        BuildMenuBar();
        if (g_cfg.tiny) SetMenu(hwnd, NULL);

        SysInfo_SetActiveTab(g_active);
        SysInfo_SetTabCollector(TabCollect);
        if (!SysInfo_Start(hwnd)) {
            MessageBoxW(hwnd, L"Task Manager could not start system monitoring.",
                        kWindowTitle, MB_OK | MB_ICONERROR);
            return -1;
        }
        SysInfo_SetSpeed(g_cfg.updateSpeed);

        if (g_cfg.tiny) {
            ShowWindow(g_hTabs, SW_HIDE);
            ShowWindow(g_hStatus, SW_HIDE);
            ShowWindow(g_hDashboard, SW_HIDE);
        }
        if (g_page[g_active] && g_page[g_active]->hwnd) {
            ShowWindow(g_page[g_active]->hwnd, SW_SHOW);
            if (g_page[g_active]->OnActivate)
                g_page[g_active]->OnActivate(g_page[g_active], TRUE);
        }
        ApplyAlwaysOnTop();
        return 0;
    }

    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            if (g_cfg.hideWhenMinimized) {
                TrayAdd();
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        }
        LayoutMain();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT work;
        mmi->ptMinTrackSize.x = DPX(g_cfg.tiny ? 180 : 840);
        mmi->ptMinTrackSize.y = DPX(g_cfg.tiny ? 120 : 720);
        if (WindowWorkArea(hwnd, NULL, &work)) {
            if (mmi->ptMinTrackSize.x > work.right - work.left) mmi->ptMinTrackSize.x = work.right - work.left;
            if (mmi->ptMinTrackSize.y > work.bottom - work.top) mmi->ptMinTrackSize.y = work.bottom - work.top;
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        /* the margin around the tab control counts as empty area too */
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDM_TOGGLE_TINY, 0), 0);
        return 0;

    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, UI_BackgroundBrush()); return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->hwndFrom == g_hTabs && nm->code == (UINT)TCN_SELCHANGE) {
            SwitchToTab(TabCtrl_GetCurSel(g_hTabs), FALSE);
            return 0;
        }
        if (nm->hwndFrom == g_hStatus && nm->code == (UINT)NM_CLICK &&
            ((NMMOUSE *)lp)->dwItemSpec == 0 && Apps_HungCount() > 0) {
            SwitchToTab(TAB_APPS, FALSE);
            return TRUE;
        }
        break;
    }

    case WM_INITMENUPOPUP:
        UpdateMenuChecks();
        break;

    case WM_COMMAND:
        OnCommand(hwnd, LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;

    case WM_APP_SNAPSHOT_READY:
        OnSnapshotReady();
        return 0;

    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK:
            RestoreFromTray();
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            TrayContextMenu();
            break;
        default:
            break;
        }
        return 0;

    case WM_DPICHANGED: {
        RECT fitted = *(RECT *)lp, work;
        RECT *suggested = &fitted;
        int i;
        g_dpi = HIWORD(wp);
        CreateAppFont();
        UI_ApplyFont(g_hDashboard);
        if (WindowWorkArea(hwnd, suggested, &work)) FitWindowRect(suggested, &work);
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(g_hTabs, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
        if (g_hStatus)
            SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
        for (i = 0; i < TAB_COUNT; i++) {
            if (!g_page[i] || !g_page[i]->hwnd) continue;
            UI_ApplyFont(g_page[i]->hwnd);
            if (g_page[i]->OnFontChanged) g_page[i]->OnFontChanged(g_page[i]);
        }
        LayoutMain();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }

    case WM_SETTINGCHANGE:
        /* This is broadcast for every system setting, most of which cannot
           affect our fonts. Rebuilding five fonts, a brush and every
           control's font on each one is pure waste, so only react to the
           two that actually change the message font. WM_THEMECHANGED below
           is rare and always relevant, so it is handled unconditionally. */
        if (wp != SPI_SETNONCLIENTMETRICS && wp != SPI_SETICONTITLELOGFONT)
            break;
        /* fall through */
    case WM_THEMECHANGED: {
        int i;
        CreateAppFont();
        UI_ApplyFont(g_hDashboard);
        SendMessageW(g_hTabs, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
        if (g_hStatus)
            SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, MAKELPARAM(TRUE, 0));
        for (i = 0; i < TAB_COUNT; i++) {
            if (!g_page[i] || !g_page[i]->hwnd) continue;
            UI_ApplyFont(g_page[i]->hwnd);
            if (g_page[i]->OnFontChanged) g_page[i]->OnFontChanged(g_page[i]);
        }
        LayoutMain();
        return 0;
    }

    case WM_SETFOCUS:
        if (g_page[g_active] && g_page[g_active]->hwnd) {
            TabPage *p = g_page[g_active];
            HWND target = p->PrimaryControl ? p->PrimaryControl(p) : NULL;
            if (!target) target = GetNextDlgTabItem(p->hwnd, NULL, FALSE);
            if (target) SetFocus(target);
        }
        return 0;

    case WM_ENDSESSION:
        if (wp) { SaveCurrentRect(); Settings_Save(); }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY: {
        int i;
        SaveCurrentRect();
        Settings_Save();
        SysInfo_Stop();
        Proc_Reset();
        Apps_Reset();
        Svc_Reset();
        Users_Reset();
        Net_Reset();
        Gpu_Reset();
        Sensors_Reset();
        TrayRemove();
        for (i = 0; i < TAB_COUNT; i++) {
            if (g_page[i] && g_page[i]->hwnd) {
                DestroyWindow(g_page[i]->hwnd);
                g_page[i]->hwnd = NULL;
            }
        }
        if (g_hTabs) RemoveWindowSubclass(g_hTabs, TabSubclass, 1);
        SetMenu(hwnd, NULL);
        if (g_hMenuBar) { DestroyMenu(g_hMenuBar); g_hMenuBar = NULL; }
        if (g_hFont) { DeleteObject(g_hFont); g_hFont = NULL; }
        UI_ThemeDestroy();
        UI_GfxShutdown();
        g_hMain = NULL;
        PostQuitMessage(0);
        return 0;
    }

    default:
        if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated && g_trayShown) {
            /* Explorer restarted: the icon is gone, put it back. */
            g_trayShown = FALSE;
            if (g_trayIcon) { DestroyIcon(g_trayIcon); g_trayIcon = NULL; }
            TrayAdd();
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------------- start --- */

static void EnableDebugPrivilege(void)
{
    HANDLE token = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return;
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(token);
}

static HACCEL BuildAccelerators(void)
{
    ACCEL acc[7];
    acc[0].fVirt = FVIRTKEY;                     acc[0].key = VK_F5;  acc[0].cmd = IDM_VIEW_REFRESH;
    acc[1].fVirt = FVIRTKEY | FCONTROL;          acc[1].key = VK_TAB; acc[1].cmd = IDM_NEXT_TAB;
    acc[2].fVirt = FVIRTKEY | FCONTROL | FSHIFT; acc[2].key = VK_TAB; acc[2].cmd = IDM_PREV_TAB;
    acc[3].fVirt = FVIRTKEY | FCONTROL;          acc[3].key = 'N';    acc[3].cmd = IDM_FILE_NEWTASK;
    acc[4].fVirt = FVIRTKEY | FCONTROL;          acc[4].key = 'F';    acc[4].cmd = IDM_PROC_FIND;
    acc[5].fVirt = FVIRTKEY | FCONTROL;          acc[5].key = 'P';    acc[5].cmd = IDM_VIEW_TOGGLEPAUSE;
    acc[6].fVirt = FVIRTKEY | FCONTROL;          acc[6].key = 'B';    acc[6].cmd = IDM_VIEW_BLAME_PEAK;
    return CreateAcceleratorTable(acc, ARRAYSIZE(acc));
}

static BOOL ActivateExistingInstance(void)
{
    HWND other = FindWindowW(kClassName, NULL);
    if (!other) return FALSE;
    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
    ShowWindow(other, SW_SHOW);
    SetForegroundWindow(other);
    return TRUE;
}

void App_ShowProcess(DWORD pid)
{
    Proc_SelectPid(pid);
    SwitchToTab(TAB_PROCESSES, FALSE);
}

void App_ReportError(HWND owner, const WCHAR *operation, DWORD error)
{
    WCHAR msg[512];
    StringCchPrintfW(msg, ARRAYSIZE(msg), L"%s failed (error %lu).", operation, (unsigned long)error);
    MessageBoxW(owner, msg, kWindowTitle, MB_OK | MB_ICONWARNING);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR cmdLine, int nCmdShow)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    MSG msg;
    HANDLE mutex;
    HWND hwnd;
    int showCmd;

    ZeroMemory(&msg, sizeof(msg));

    (void)hPrev;

    g_hInst = hInstance;

    mutex = CreateMutexW(NULL, FALSE, kMutexName);
    /* The elevated child starts before ShellExecuteEx returns to the parent.
       It must not reactivate that parent and exit during the handoff. */
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS && !IsElevatedRelaunch(cmdLine)) {
        if (ActivateExistingInstance()) {
            CloseHandle(mutex);
            return 0;
        }
    }

    InitDpiApi();
    g_dpi = QueryDpi(NULL);
    EnableDebugPrivilege();
    Settings_Load();
    if (IsElevatedRelaunch(cmdLine)) g_cfg.showAllUsers = TRUE;
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES |
                 ICC_TAB_CLASSES  | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS |
                 ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                         IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    {
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        int w = DPX(1180), h = DPX(860);
        const RECT *rc = NULL;

        if (g_cfg.tiny && g_cfg.haveTinyRect)         rc = &g_cfg.rcTiny;
        else if (!g_cfg.tiny && g_cfg.haveNormalRect) rc = &g_cfg.rcNormal;
        if (rc) {
            x = rc->left; y = rc->top;
            w = rc->right - rc->left;
            h = rc->bottom - rc->top;
        }
        if (!g_cfg.tiny) {
            if (w < DPX(840)) w = DPX(840);
            if (h < DPX(720)) h = DPX(720);
        }
        {
            RECT work, desired;
            POINT cursor = {0, 0};
            if (x == CW_USEDEFAULT) { GetCursorPos(&cursor); x = cursor.x; y = cursor.y; }
            /* Select a monitor before expanding a persisted rectangle: saved
               coordinates near LONG limits must not overflow when enlarged. */
            desired = rc ? *rc : (RECT){x, y, x, y};
            if (WindowWorkArea(NULL, &desired, &work)) {
                if (w > work.right - work.left) w = work.right - work.left;
                if (h > work.bottom - work.top) h = work.bottom - work.top;
                FitWindowRect(&desired, &work);
                if (!rc) {
                    desired.left = work.left + (work.right - work.left - w) / 2;
                    desired.top = work.top + (work.bottom - work.top - h) / 2;
                }
                desired.right = desired.left + w; desired.bottom = desired.top + h;
                FitWindowRect(&desired, &work);
                x = desired.left; y = desired.top;
                w = desired.right - desired.left; h = desired.bottom - desired.top;
            }
        }
        hwnd = CreateWindowExW(0, kClassName, kWindowTitle,
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               x, y, w, h, NULL, NULL, hInstance, NULL);
    }
    if (!hwnd) {
        if (mutex) CloseHandle(mutex);
        return 1;
    }

    g_hAccel = BuildAccelerators();

    showCmd = nCmdShow;
    if (g_cfg.maximized && showCmd == SW_SHOWNORMAL) showCmd = SW_SHOWMAXIMIZED;
    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);
    LayoutMain();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TabPage *p = g_page[g_active];
        if (g_hAccel && TranslateAcceleratorW(hwnd, g_hAccel, &msg)) continue;
        if (HandleWorkspaceKey(&msg)) continue;
        if (p && p->hwnd && (msg.hwnd == p->hwnd || IsChild(p->hwnd, msg.hwnd)) &&
            IsDialogMessageW(p->hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hAccel) DestroyAcceleratorTable(g_hAccel);
    /* The mutex was created with bInitialOwner FALSE and is never acquired,
       so there is nothing to release; ReleaseMutex would just fail with
       ERROR_NOT_OWNER. Closing the handle is what drops the name. */
    if (mutex) CloseHandle(mutex);
    return (int)msg.wParam;
}
