// ============================================================================
//  MYNATASK PRO — NATIVE C++ EDITION (OPTIMIZED)
//  A lightweight Win32 process manager. Dark, flat, DPI-aware UI on a plain
//  Win32 core (no WPF / no XAML / no extra runtime).
//
//  Targets Windows 10 1809+ and Windows 11.
//  Compatible with MSVC and MinGW GCC compilers.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define WINVER        0x0A00
#define _WIN32_WINNT  0x0A00

#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <psapi.h>
#include <pdh.h>
#include <tlhelp32.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cwchar>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")
#endif

#ifndef FW_SEMIBOLD
#define FW_SEMIBOLD 600
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

// ============================================================================
//  [ 1 ]  CONTROL IDS / TIMERS
// ============================================================================
#define IDC_SEARCHBOX      1001
#define IDC_BTN_SYNC       1002
#define IDC_CHK_AUTOSYNC   1003
#define IDC_BTN_SUSPEND    1004
#define IDC_BTN_RESUME     1005
#define IDC_BTN_TERMINATE  1006
#define IDC_LISTVIEW       1007
#define IDC_CPU_BAR        1008
#define IDC_CPU_LABEL      1009
#define IDC_CPU_CAPTION    1010
#define IDC_LOG_LABEL      1011

#define ID_TIMER_CPU       2001
#define ID_TIMER_SYNC      2002

// ============================================================================
//  [ 2 ]  NTDLL SUSPEND / RESUME
// ============================================================================
typedef LONG (NTAPI *NtSuspendProcess_t)(HANDLE);
typedef LONG (NTAPI *NtResumeProcess_t)(HANDLE);

// ============================================================================
//  [ 3 ]  THEME / PALETTE
// ============================================================================
namespace Theme {
    constexpr COLORREF BgWindow     = RGB(0x1B, 0x1B, 0x1F);
    constexpr COLORREF BgPanel      = RGB(0x23, 0x23, 0x28);
    constexpr COLORREF BgControl    = RGB(0x29, 0x29, 0x2F);
    constexpr COLORREF BgListRow    = RGB(0x1E, 0x1E, 0x23);
    constexpr COLORREF BgListRowAlt = RGB(0x24, 0x24, 0x2A);
    constexpr COLORREF BgListSel    = RGB(0x1F, 0x46, 0x3A);
    constexpr COLORREF Border       = RGB(0x35, 0x35, 0x3C);
    constexpr COLORREF TextPrimary  = RGB(0xEC, 0xEC, 0xEE);
    constexpr COLORREF TextMuted    = RGB(0x8B, 0x8B, 0x95);
    constexpr COLORREF Accent       = RGB(0x2E, 0xC9, 0x8E);  // emerald
    constexpr COLORREF AccentDim    = RGB(0x1F, 0x8F, 0x66);
    constexpr COLORREF Amber        = RGB(0xE3, 0xA8, 0x4C);
    constexpr COLORREF Cyan         = RGB(0x4F, 0xC3, 0xF7);
    constexpr COLORREF Red          = RGB(0xE5, 0x5B, 0x5B);
}

// ============================================================================
//  [ 4 ]  DATA MODEL & APP STATE
// ============================================================================
struct ProcessEntry {
    DWORD pid = 0;
    std::wstring name;
    std::wstring path;
    ULONGLONG workingSetBytes = 0;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND hSearch = nullptr;
    HWND hBtnSync = nullptr;
    HWND hChkAuto = nullptr;
    HWND hBtnSuspend = nullptr;
    HWND hBtnResume = nullptr;
    HWND hBtnTerminate = nullptr;
    HWND hList = nullptr;
    HWND hCpuBar = nullptr;
    HWND hCpuLabel = nullptr;
    HWND hCpuCaption = nullptr;
    HWND hLog = nullptr;

    HFONT fontTitle = nullptr;
    HFONT fontSubtitle = nullptr;
    HFONT fontUI = nullptr;
    HFONT fontUIBold = nullptr;
    HFONT fontMono = nullptr;

    // Cached GDI Brushes for high-performance painting
    HBRUSH hbrBgWindow  = nullptr;
    HBRUSH hbrBgPanel   = nullptr;
    HBRUSH hbrBgControl = nullptr;
    HBRUSH hbrListRow   = nullptr;
    HBRUSH hbrListRowAlt= nullptr;
    HBRUSH hbrListSel   = nullptr;

    UINT dpi = 96;
    double scale = 1.0;

    std::vector<ProcessEntry> allProcesses;
    std::wstring filterLower;
    COLORREF logColor = Theme::Accent;

    NtSuspendProcess_t pNtSuspendProcess = nullptr;
    NtResumeProcess_t  pNtResumeProcess  = nullptr;

    PDH_HQUERY   pdhQuery = nullptr;
    PDH_HCOUNTER pdhCounter = nullptr;
};

static AppState g_app;

inline int SC(int v) { return static_cast<int>(v * g_app.scale + (v >= 0 ? 0.5 : -0.5)); }

// ============================================================================
//  [ 5 ]  FORWARD DECLARATIONS
// ============================================================================
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void EnableHighDpiAwareness();
void ApplyDarkTitleBar(HWND hwnd);
void ApplyRoundedCorners(HWND hwnd);
void TryEnableMica(HWND hwnd);

void CreateFontsForDpi(UINT dpi);
void DeleteAppFonts();
void ApplyFontsToControls();

void InitGdiResources();
void CleanupGdiResources();

void CreateControls(HWND hwnd);
void RelayoutControls();
void MakeButtonRounded(HWND hBtn, int w, int h);

RECT GetUnionClientRect(const std::vector<HWND>& hwnds, int pad);
void DrawPanel(HDC hdc, RECT r);
void DrawFrame(HDC hdc, RECT r, COLORREF color);

void InitNtdllFunctions();
void InitCpuMonitor();
int  ReadCpuPercent();

std::vector<ProcessEntry> EnumerateProcesses();
bool DoSuspend(DWORD pid);
bool DoResume(DWORD pid);
bool DoTerminate(DWORD pid);

void RefreshProcessData();
void RefreshListDisplay();
DWORD GetSelectedPid();
void SetLog(const std::wstring& text, COLORREF color);

LRESULT HandleDrawItem(LPARAM lParam);
LRESULT HandleListCustomDraw(LPARAM lParam);

// ============================================================================
//  [ 6 ]  ENTRY POINTS
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    EnableHighDpiAwareness();

    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"MynaTaskProWindowClass";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_app.dpi = GetDpiForSystem();
    g_app.scale = g_app.dpi / 96.0;

    int w = SC(1150), h = SC(750);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"MynaTask Pro",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        nullptr, nullptr, hInst, nullptr);

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// Fallback entry point for GCC/MinGW when building without -municode flag
#ifndef _MSC_VER
extern "C" int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return wWinMain(hInstance, hPrevInstance, GetCommandLineW(), nCmdShow);
}
#endif

// ============================================================================
//  [ 7 ]  WINDOW PROCEDURE
// ============================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {

    case WM_CREATE: {
        g_app.hwnd = hwnd;
        InitGdiResources();
        ApplyDarkTitleBar(hwnd);
        ApplyRoundedCorners(hwnd);
        TryEnableMica(hwnd);

        CreateFontsForDpi(g_app.dpi);
        InitNtdllFunctions();
        InitCpuMonitor();
        CreateControls(hwnd);
        RelayoutControls();
        RefreshProcessData();

        SetTimer(hwnd, ID_TIMER_CPU, 1000, nullptr);
        SetTimer(hwnd, ID_TIMER_SYNC, 3500, nullptr);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = SC(800);
        mmi->ptMinTrackSize.y = SC(500);
        return 0;
    }

    case WM_SIZE:
        if (g_app.hList) RelayoutControls();
        return 0;

    case WM_DPICHANGED: {
        g_app.dpi = HIWORD(wParam);
        g_app.scale = g_app.dpi / 96.0;
        CreateFontsForDpi(g_app.dpi);
        ApplyFontsToControls();
        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        RelayoutControls();
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        FillRect(hdc, &rc, g_app.hbrBgWindow);

        int margin = SC(16);
        int headerH = SC(56);

        SetBkMode(hdc, TRANSPARENT);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_app.fontTitle));
        SetTextColor(hdc, Theme::Accent);
        RECT titleRect{ margin, margin, rc.right - margin, margin + SC(30) };
        DrawTextW(hdc, L"MYNATASK PRO", -1, &titleRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

        SelectObject(hdc, g_app.fontSubtitle);
        SetTextColor(hdc, Theme::TextMuted);
        RECT subRect{ margin, margin + SC(30), rc.right - margin, margin + headerH };
        DrawTextW(hdc, L"NATIVE PROCESS MANAGER  \u2022  C++ / WIN32  \u2022  WINDOWS 10 & 11",
            -1, &subRect, DT_LEFT | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, oldFont);

        HBRUSH accentBrush = CreateSolidBrush(Theme::Accent);
        RECT underline{ margin, margin + SC(35), margin + SC(140), margin + SC(38) };
        FillRect(hdc, &underline, accentBrush);
        DeleteObject(accentBrush);

        if (g_app.hSearch) {
            RECT toolRc = GetUnionClientRect({
                g_app.hSearch, g_app.hBtnSync, g_app.hChkAuto, g_app.hBtnSuspend,
                g_app.hBtnResume, g_app.hBtnTerminate, g_app.hCpuBar,
                g_app.hCpuLabel, g_app.hCpuCaption }, SC(10));
            DrawPanel(hdc, toolRc);

            RECT listRc; GetWindowRect(g_app.hList, &listRc);
            POINT tl{ listRc.left, listRc.top }, br{ listRc.right, listRc.bottom };
            ScreenToClient(hwnd, &tl); ScreenToClient(hwnd, &br);
            RECT listFrame{ tl.x - SC(1), tl.y - SC(1), br.x + SC(1), br.y + SC(1) };
            DrawFrame(hdc, listFrame, Theme::Border);

            RECT footRc = GetUnionClientRect({ g_app.hLog }, SC(10));
            DrawPanel(hdc, footRc);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctrl = reinterpret_cast<HWND>(lParam);
        SetBkMode(hdc, TRANSPARENT);
        if (ctrl == g_app.hLog)             SetTextColor(hdc, g_app.logColor);
        else if (ctrl == g_app.hCpuLabel)   SetTextColor(hdc, Theme::Accent);
        else if (ctrl == g_app.hCpuCaption) SetTextColor(hdc, Theme::TextMuted);
        else                                SetTextColor(hdc, Theme::TextPrimary);
        return reinterpret_cast<LRESULT>(g_app.hbrBgPanel);
    }

    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, Theme::TextPrimary);
        return reinterpret_cast<LRESULT>(g_app.hbrBgPanel);
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, Theme::BgControl);
        SetTextColor(hdc, Theme::TextPrimary);
        return reinterpret_cast<LRESULT>(g_app.hbrBgControl);
    }

    case WM_SETCURSOR: {
        HWND target = reinterpret_cast<HWND>(wParam);
        if (target == g_app.hBtnSync || target == g_app.hBtnSuspend ||
            target == g_app.hBtnResume || target == g_app.hBtnTerminate) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM:
        return HandleDrawItem(lParam);

    case WM_NOTIFY: {
        LPNMHDR hdr = reinterpret_cast<LPNMHDR>(lParam);
        if (hdr->hwndFrom == g_app.hList && hdr->code == NM_CUSTOMDRAW) {
            return HandleListCustomDraw(lParam);
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_SEARCHBOX && code == EN_CHANGE) {
            wchar_t buf[256];
            GetWindowTextW(g_app.hSearch, buf, 256);
            g_app.filterLower = buf;
            std::transform(g_app.filterLower.begin(), g_app.filterLower.end(),
                g_app.filterLower.begin(), ::towlower);
            RefreshListDisplay();
        }
        else if (id == IDC_BTN_SYNC && code == BN_CLICKED) {
            RefreshProcessData();
            SetLog(L"[>] Sync completed.", Theme::Accent);
        }
        else if (id == IDC_BTN_SUSPEND && code == BN_CLICKED) {
            DWORD pid = GetSelectedPid();
            if (pid) {
                wchar_t buf[64]; swprintf_s(buf, 64, L"%u", pid);
                if (DoSuspend(pid)) SetLog(std::wstring(L"[>] Suspended PID ") + buf, Theme::Amber);
                else SetLog(std::wstring(L"[!] Access denied for PID ") + buf, Theme::Red);
            }
        }
        else if (id == IDC_BTN_RESUME && code == BN_CLICKED) {
            DWORD pid = GetSelectedPid();
            if (pid) {
                wchar_t buf[64]; swprintf_s(buf, 64, L"%u", pid);
                if (DoResume(pid)) SetLog(std::wstring(L"[>] Resumed PID ") + buf, Theme::Cyan);
                else SetLog(std::wstring(L"[!] Access denied for PID ") + buf, Theme::Red);
            }
        }
        else if (id == IDC_BTN_TERMINATE && code == BN_CLICKED) {
            DWORD pid = GetSelectedPid();
            if (pid) {
                wchar_t buf[64]; swprintf_s(buf, 64, L"%u", pid);
                if (DoTerminate(pid)) {
                    SetLog(std::wstring(L"[X] Terminated PID ") + buf, Theme::Red);
                    RefreshProcessData();
                } else {
                    SetLog(std::wstring(L"[!] Failed to terminate protected PID ") + buf, Theme::Red);
                }
            }
        }
        return 0;
    }

    case WM_TIMER: {
        if (wParam == ID_TIMER_CPU) {
            int pct = ReadCpuPercent();
            if (pct >= 0) {
                if (pct > 100) pct = 100;
                SendMessageW(g_app.hCpuBar, PBM_SETPOS, static_cast<WPARAM>(pct), 0);
                SetWindowTextW(g_app.hCpuLabel, (std::to_wstring(pct) + L"%").c_str());
            }
        } else if (wParam == ID_TIMER_SYNC) {
            if (IsDlgButtonChecked(hwnd, IDC_CHK_AUTOSYNC) == BST_CHECKED &&
                GetFocus() != g_app.hSearch) {
                RefreshProcessData();
            }
        }
        return 0;
    }

    case WM_DESTROY: {
        KillTimer(hwnd, ID_TIMER_CPU);
        KillTimer(hwnd, ID_TIMER_SYNC);
        if (g_app.pdhQuery) PdhCloseQuery(g_app.pdhQuery);
        DeleteAppFonts();
        CleanupGdiResources();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
//  [ 8 ]  SYSTEM & DWM HELPERS
// ============================================================================
void EnableHighDpiAwareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL(WINAPI* SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
        auto pSetDpi = reinterpret_cast<SetProcessDpiAwarenessContext_t>(GetProcAddress(hUser32, "SetProcessDpiAwarenessContext"));
        if (pSetDpi) {
            pSetDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    }
    SetProcessDPIAware();
}

void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark)); // Win10 legacy fallback
    }
}

void ApplyRoundedCorners(HWND hwnd) {
    DWORD pref = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

void TryEnableMica(HWND hwnd) {
    DWORD backdrop = 2; // DWMSBT_MAINWINDOW
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
}

// ============================================================================
//  [ 9 ]  FONTS & GDI CACHE
// ============================================================================
void InitGdiResources() {
    g_app.hbrBgWindow   = CreateSolidBrush(Theme::BgWindow);
    g_app.hbrBgPanel    = CreateSolidBrush(Theme::BgPanel);
    g_app.hbrBgControl  = CreateSolidBrush(Theme::BgControl);
    g_app.hbrListRow    = CreateSolidBrush(Theme::BgListRow);
    g_app.hbrListRowAlt = CreateSolidBrush(Theme::BgListRowAlt);
    g_app.hbrListSel    = CreateSolidBrush(Theme::BgListSel);
}

void CleanupGdiResources() {
    if (g_app.hbrBgWindow)   DeleteObject(g_app.hbrBgWindow);
    if (g_app.hbrBgPanel)    DeleteObject(g_app.hbrBgPanel);
    if (g_app.hbrBgControl)  DeleteObject(g_app.hbrBgControl);
    if (g_app.hbrListRow)    DeleteObject(g_app.hbrListRow);
    if (g_app.hbrListRowAlt) DeleteObject(g_app.hbrListRowAlt);
    if (g_app.hbrListSel)    DeleteObject(g_app.hbrListSel);
}

void DeleteAppFonts() {
    if (g_app.fontTitle)    DeleteObject(g_app.fontTitle);
    if (g_app.fontSubtitle) DeleteObject(g_app.fontSubtitle);
    if (g_app.fontUI)       DeleteObject(g_app.fontUI);
    if (g_app.fontUIBold)   DeleteObject(g_app.fontUIBold);
    if (g_app.fontMono)     DeleteObject(g_app.fontMono);
}

void CreateFontsForDpi(UINT dpi) {
    DeleteAppFonts();
    auto mk = [&](int pt, int weight, const wchar_t* face) -> HFONT {
        return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    };
    g_app.fontTitle    = mk(20, FW_BOLD,     L"Segoe UI");
    g_app.fontSubtitle = mk(9,  FW_NORMAL,   L"Segoe UI");
    g_app.fontUI       = mk(10, FW_NORMAL,   L"Segoe UI");
    g_app.fontUIBold   = mk(10, FW_SEMIBOLD, L"Segoe UI");
    g_app.fontMono     = mk(10, FW_NORMAL,   L"Consolas");
}

void ApplyFontsToControls() {
    SendMessageW(g_app.hSearch,      WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUI), TRUE);
    SendMessageW(g_app.hBtnSync,     WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUIBold), TRUE);
    SendMessageW(g_app.hChkAuto,     WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUI), TRUE);
    SendMessageW(g_app.hBtnSuspend,  WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUIBold), TRUE);
    SendMessageW(g_app.hBtnResume,   WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUIBold), TRUE);
    SendMessageW(g_app.hBtnTerminate,WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUIBold), TRUE);
    SendMessageW(g_app.hList,        WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontMono), TRUE);
    SendMessageW(g_app.hLog,         WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontMono), TRUE);
    SendMessageW(g_app.hCpuCaption,  WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUI), TRUE);
    SendMessageW(g_app.hCpuLabel,    WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUIBold), TRUE);
}

// ============================================================================
//  [ 10 ]  CONTROL CREATION
// ============================================================================
void CreateControls(HWND hwnd) {
    HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

    g_app.hSearch = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SEARCHBOX), hInst, nullptr);
    SetWindowTheme(g_app.hSearch, L"DarkMode_CFD", nullptr);
    SendMessageW(g_app.hSearch, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search processes..."));

    auto mkBtn = [&](const wchar_t* text, int id) {
        return CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    };

    g_app.hBtnSync = mkBtn(L"SYNC", IDC_BTN_SYNC);

    g_app.hChkAuto = CreateWindowExW(0, L"BUTTON", L"Auto-Sync",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_CHK_AUTOSYNC), hInst, nullptr);
    CheckDlgButton(hwnd, IDC_CHK_AUTOSYNC, BST_CHECKED);

    g_app.hBtnSuspend   = mkBtn(L"SUSPEND",   IDC_BTN_SUSPEND);
    g_app.hBtnResume    = mkBtn(L"RESUME",    IDC_BTN_RESUME);
    g_app.hBtnTerminate = mkBtn(L"TERMINATE", IDC_BTN_TERMINATE);

    g_app.hCpuCaption = CreateWindowExW(0, L"STATIC", L"CPU",
        WS_CHILD | WS_VISIBLE | SS_RIGHT,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_CPU_CAPTION), hInst, nullptr);

    g_app.hCpuBar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_CPU_BAR), hInst, nullptr);
    SetWindowTheme(g_app.hCpuBar, L"", L"");
    SendMessageW(g_app.hCpuBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_app.hCpuBar, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(Theme::Accent));
    SendMessageW(g_app.hCpuBar, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(Theme::BgControl));

    g_app.hCpuLabel = CreateWindowExW(0, L"STATIC", L"0%",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_CPU_LABEL), hInst, nullptr);

    g_app.hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LISTVIEW), hInst, nullptr);
    ListView_SetExtendedListViewStyle(g_app.hList,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SetWindowTheme(g_app.hList, L"DarkMode_Explorer", nullptr);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<LPWSTR>(L"PROCESS"); col.cx = SC(230); col.iSubItem = 0;
    ListView_InsertColumn(g_app.hList, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"PID");     col.cx = SC(70);  col.iSubItem = 1;
    ListView_InsertColumn(g_app.hList, 1, &col);
    col.pszText = const_cast<LPWSTR>(L"MEMORY");  col.cx = SC(100); col.iSubItem = 2;
    ListView_InsertColumn(g_app.hList, 2, &col);
    col.pszText = const_cast<LPWSTR>(L"PATH");    col.cx = SC(400); col.iSubItem = 3;
    ListView_InsertColumn(g_app.hList, 3, &col);

    g_app.hLog = CreateWindowExW(0, L"STATIC", L"[SYSTEM] Engine ready.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LOG_LABEL), hInst, nullptr);

    ApplyFontsToControls();
}

void MakeButtonRounded(HWND hBtn, int w, int h) {
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, SC(8), SC(8));
    SetWindowRgn(hBtn, rgn, TRUE);
}

// ============================================================================
//  [ 11 ]  LAYOUT & DRAWING
// ============================================================================
void RelayoutControls() {
    if (!g_app.hList) return;
    RECT rc; GetClientRect(g_app.hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;

    int margin   = SC(16);
    int headerH  = SC(56);
    int toolbarH = SC(34);
    int footerH  = SC(36);
    int gap      = SC(14);

    int y = margin + headerH + gap;

    int cx = margin;
    int searchW = SC(220);
    MoveWindow(g_app.hSearch, cx, y + SC(2), searchW, toolbarH - SC(4), TRUE);
    cx += searchW + SC(14);

    int bwSync = SC(86);
    MoveWindow(g_app.hBtnSync, cx, y, bwSync, toolbarH, TRUE);
    MakeButtonRounded(g_app.hBtnSync, bwSync, toolbarH);
    cx += bwSync + SC(16);

    int chkW = SC(100);
    MoveWindow(g_app.hChkAuto, cx, y + SC(7), chkW, toolbarH - SC(10), TRUE);
    cx += chkW + SC(24);

    int bwSuspend = SC(92), bwResume = SC(92), bwTerminate = SC(104);
    MoveWindow(g_app.hBtnSuspend, cx, y, bwSuspend, toolbarH, TRUE);
    MakeButtonRounded(g_app.hBtnSuspend, bwSuspend, toolbarH);
    cx += bwSuspend + SC(8);

    MoveWindow(g_app.hBtnResume, cx, y, bwResume, toolbarH, TRUE);
    MakeButtonRounded(g_app.hBtnResume, bwResume, toolbarH);
    cx += bwResume + SC(8);

    MoveWindow(g_app.hBtnTerminate, cx, y, bwTerminate, toolbarH, TRUE);
    MakeButtonRounded(g_app.hBtnTerminate, bwTerminate, toolbarH);

    int cpuBarW = SC(130), cpuLabelW = SC(44), cpuCapW = SC(30), gapSmall = SC(8);
    int rightEdge = W - margin;
    int lblX = rightEdge - cpuLabelW;
    int barX = lblX - gapSmall - cpuBarW;
    int capX = barX - gapSmall - cpuCapW;

    MoveWindow(g_app.hCpuCaption, capX, y + SC(3), cpuCapW, toolbarH - SC(6), TRUE);
    MoveWindow(g_app.hCpuBar,     barX, y + SC(8), cpuBarW, toolbarH - SC(16), TRUE);
    MoveWindow(g_app.hCpuLabel,   lblX, y + SC(3), cpuLabelW, toolbarH - SC(6), TRUE);

    int listY = y + toolbarH + gap;
    int listH = H - listY - gap - footerH - margin;
    if (listH < SC(100)) listH = SC(100);
    int listW = W - margin * 2;
    MoveWindow(g_app.hList, margin, listY, listW, listH, TRUE);

    int col0 = SC(230), col1 = SC(70), col2 = SC(100);
    int scrollW = GetSystemMetrics(SM_CXVSCROLL);
    int col3 = listW - col0 - col1 - col2 - scrollW - SC(8);
    if (col3 < SC(150)) col3 = SC(150);
    ListView_SetColumnWidth(g_app.hList, 0, col0);
    ListView_SetColumnWidth(g_app.hList, 1, col1);
    ListView_SetColumnWidth(g_app.hList, 2, col2);
    ListView_SetColumnWidth(g_app.hList, 3, col3);

    int footerY = H - margin - footerH;
    MoveWindow(g_app.hLog, margin + SC(14), footerY + SC(9), listW - SC(28), footerH - SC(18), TRUE);

    InvalidateRect(g_app.hwnd, nullptr, TRUE);
}

RECT GetUnionClientRect(const std::vector<HWND>& hwnds, int pad) {
    RECT result{ 1000000, 1000000, -1000000, -1000000 };
    for (HWND h : hwnds) {
        if (!h) continue;
        RECT r; GetWindowRect(h, &r);
        POINT tl{ r.left, r.top }, br{ r.right, r.bottom };
        ScreenToClient(g_app.hwnd, &tl);
        ScreenToClient(g_app.hwnd, &br);
        result.left   = std::min(result.left, static_cast<LONG>(tl.x));
        result.top    = std::min(result.top, static_cast<LONG>(tl.y));
        result.right  = std::max(result.right, static_cast<LONG>(br.x));
        result.bottom = std::max(result.bottom, static_cast<LONG>(br.y));
    }
    result.left -= pad; result.top -= pad;
    result.right += pad; result.bottom += pad;
    return result;
}

void DrawPanel(HDC hdc, RECT r) {
    FillRect(hdc, &r, g_app.hbrBgPanel);
    DrawFrame(hdc, r, Theme::Border);
}

void DrawFrame(HDC hdc, RECT r, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// ============================================================================
//  [ 12 ]  OWNER-DRAW BUTTONS
// ============================================================================
LRESULT HandleDrawItem(LPARAM lParam) {
    LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);

    COLORREF accent = Theme::Accent;
    switch (dis->CtlID) {
        case IDC_BTN_SYNC:      accent = Theme::Accent; break;
        case IDC_BTN_SUSPEND:   accent = Theme::Amber;  break;
        case IDC_BTN_RESUME:    accent = Theme::Cyan;   break;
        case IDC_BTN_TERMINATE: accent = Theme::Red;    break;
        default: break;
    }

    bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    RECT r = dis->rcItem;
    HDC hdc = dis->hDC;

    COLORREF fillColor = disabled ? Theme::BgControl : (pressed ? Theme::AccentDim : accent);
    HBRUSH fillBrush = CreateSolidBrush(fillColor);
    HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(hdc, fillBrush));
    HPEN pen = CreatePen(PS_SOLID, 1, disabled ? Theme::Border : accent);
    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, pen));

    int radius = SC(8);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, radius, radius);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(fillBrush);
    DeleteObject(pen);

    wchar_t text[64];
    GetWindowTextW(dis->hwndItem, text, 64);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, disabled ? Theme::TextMuted : RGB(0x0A, 0x0A, 0x0C));
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_app.fontUIBold));
    DrawTextW(hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);

    return TRUE;
}

// ============================================================================
//  [ 13 ]  LISTVIEW CUSTOM DRAW
// ============================================================================
LRESULT HandleListCustomDraw(LPARAM lParam) {
    LPNMLVCUSTOMDRAW cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
    switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            int idx = static_cast<int>(cd->nmcd.dwItemSpec);
            bool selected = (ListView_GetItemState(g_app.hList, idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            bool alt = (idx % 2) == 1;
            cd->clrText = Theme::TextPrimary;
            cd->clrTextBk = selected ? Theme::BgListSel : (alt ? Theme::BgListRowAlt : Theme::BgListRow);
            return CDRF_NEWFONT;
        }
        default:
            return CDRF_DODEFAULT;
    }
}

// ============================================================================
//  [ 14 ]  PROCESS ENUMERATION
// ============================================================================
std::vector<ProcessEntry> EnumerateProcesses() {
    std::vector<ProcessEntry> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcessEntry entry;
            entry.pid = pe.th32ProcessID;
            entry.name = pe.szExeFile;
            entry.path = L"System / Access Denied";

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                FALSE, pe.th32ProcessID);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS pmc{};
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    entry.workingSetBytes = pmc.WorkingSetSize;
                }
                wchar_t pathBuf[MAX_PATH] = { 0 };
                DWORD sz = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, pathBuf, &sz)) {
                    entry.path = pathBuf;
                }
                CloseHandle(hProc);
            }
            result.push_back(std::move(entry));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    std::sort(result.begin(), result.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
        return a.workingSetBytes > b.workingSetBytes;
    });
    return result;
}

// ============================================================================
//  [ 15 ]  SUSPEND / RESUME / TERMINATE
// ============================================================================
void InitNtdllFunctions() {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        g_app.pNtSuspendProcess = reinterpret_cast<NtSuspendProcess_t>(GetProcAddress(hNtdll, "NtSuspendProcess"));
        g_app.pNtResumeProcess  = reinterpret_cast<NtResumeProcess_t>(GetProcAddress(hNtdll, "NtResumeProcess"));
    }
}

bool DoSuspend(DWORD pid) {
    if (!g_app.pNtSuspendProcess) return false;
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) return false;
    LONG status = g_app.pNtSuspendProcess(h);
    CloseHandle(h);
    return status == 0;
}

bool DoResume(DWORD pid) {
    if (!g_app.pNtResumeProcess) return false;
    HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid);
    if (!h) return false;
    LONG status = g_app.pNtResumeProcess(h);
    CloseHandle(h);
    return status == 0;
}

bool DoTerminate(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok != 0;
}

// ============================================================================
//  [ 16 ]  CPU MONITOR (PDH)
// ============================================================================
void InitCpuMonitor() {
    if (PdhOpenQueryW(nullptr, 0, &g_app.pdhQuery) == ERROR_SUCCESS) {
        PdhAddEnglishCounterW(g_app.pdhQuery, L"\\Processor(_Total)\\% Processor Time",
            0, &g_app.pdhCounter);
        PdhCollectQueryData(g_app.pdhQuery);
    }
}

int ReadCpuPercent() {
    if (!g_app.pdhQuery || !g_app.pdhCounter) return -1;
    if (PdhCollectQueryData(g_app.pdhQuery) != ERROR_SUCCESS) return -1;
    PDH_FMT_COUNTERVALUE val{};
    if (PdhGetFormattedCounterValue(g_app.pdhCounter, PDH_FMT_LONG, nullptr, &val) != ERROR_SUCCESS)
        return -1;
    return static_cast<int>(val.longValue);
}

// ============================================================================
//  [ 17 ]  LIST / SELECTION HELPERS
// ============================================================================
DWORD GetSelectedPid() {
    int idx = ListView_GetNextItem(g_app.hList, -1, LVNI_SELECTED);
    if (idx < 0) return 0;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = idx;
    ListView_GetItem(g_app.hList, &item);
    return static_cast<DWORD>(item.lParam);
}

void SetLog(const std::wstring& text, COLORREF color) {
    SetWindowTextW(g_app.hLog, text.c_str());
    g_app.logColor = color;
    InvalidateRect(g_app.hLog, nullptr, TRUE);
}

void RefreshListDisplay() {
    // Preserve current top visible scroll index to prevent UI jump on auto-sync
    int topIdx = ListView_GetTopIndex(g_app.hList);

    SendMessageW(g_app.hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_app.hList);

    int row = 0;
    for (const auto& p : g_app.allProcesses) {
        if (!g_app.filterLower.empty()) {
            std::wstring nameLower = p.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
            if (nameLower.find(g_app.filterLower) == std::wstring::npos) continue;
        }

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(p.name.c_str());
        item.lParam = static_cast<LPARAM>(p.pid);
        ListView_InsertItem(g_app.hList, &item);

        wchar_t pidBuf[32];
        swprintf_s(pidBuf, 32, L"%u", p.pid);
        ListView_SetItemText(g_app.hList, row, 1, pidBuf);

        double mb = p.workingSetBytes / (1024.0 * 1024.0);
        wchar_t memBuf[32];
        swprintf_s(memBuf, 32, L"%.1f MB", mb);
        ListView_SetItemText(g_app.hList, row, 2, memBuf);

        ListView_SetItemText(g_app.hList, row, 3, const_cast<LPWSTR>(p.path.c_str()));
        row++;
    }

    // Restore scroll position
    if (topIdx > 0 && topIdx < row) {
        POINT pt{};
        ListView_GetItemPosition(g_app.hList, topIdx, &pt);
        ListView_Scroll(g_app.hList, 0, pt.y);
    }

    SendMessageW(g_app.hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_app.hList, nullptr, TRUE);
}

void RefreshProcessData() {
    DWORD selectedPid = GetSelectedPid();
    g_app.allProcesses = EnumerateProcesses();
    RefreshListDisplay();

    if (selectedPid) {
        int count = ListView_GetItemCount(g_app.hList);
        for (int i = 0; i < count; i++) {
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = i;
            ListView_GetItem(g_app.hList, &item);
            if (static_cast<DWORD>(item.lParam) == selectedPid) {
                ListView_SetItemState(g_app.hList, i, LVIS_SELECTED | LVIS_FOCUSED,
                    LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }
}
