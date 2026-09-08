// MynaTaskFull.cpp
// Native Windows Process Manager
// Features: Processes, Services, Startup, System Info
// With virtual list, sorting, dark theme, DPI aware, tray icon, elevation.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define WINVER        0x0A00
#define _WIN32_WINNT  0x0A00

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <psapi.h>
#include <pdh.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winsvc.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
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
//  CONTROL IDS / TIMERS / MESSAGES
// ============================================================================
#define IDC_TAB              1000
#define IDC_SEARCHBOX        1001
#define IDC_BTN_SYNC         1002
#define IDC_CHK_AUTOSYNC     1003
#define IDC_BTN_SUSPEND      1004
#define IDC_BTN_RESUME       1005
#define IDC_BTN_TERMINATE    1006
#define IDC_LISTVIEW         1007
#define IDC_CPU_BAR          1008
#define IDC_CPU_LABEL        1009
#define IDC_CPU_CAPTION      1010
#define IDC_LOG_LABEL        1011
#define IDC_BTN_ELEVATE      1012

#define IDC_LIST_SERVICES    1100
#define IDC_BTN_SVC_START    1101
#define IDC_BTN_SVC_STOP     1102
#define IDC_BTN_SVC_RESTART  1103
#define IDC_BTN_SVC_REFRESH  1104

#define IDC_LIST_STARTUP        1200
#define IDC_BTN_STARTUP_ENABLE  1201
#define IDC_BTN_STARTUP_DISABLE 1202
#define IDC_BTN_STARTUP_OPENLOC 1203
#define IDC_BTN_STARTUP_REFRESH 1204

#define IDC_LIST_SYSINFO        1400

#define ID_TIMER_CPU   2001
#define ID_TIMER_SYNC  2002

#define WM_TRAYICON (WM_APP + 100)
#define IDM_TRAY_RESTORE 2100
#define IDM_TRAY_EXIT    2101

#define IDM_CTX_SUSPEND     3000
#define IDM_CTX_RESUME      3001
#define IDM_CTX_TERMINATE   3002
#define IDM_CTX_OPENLOC     3003
#define IDM_CTX_COPYPATH    3004
#define IDM_CTX_PROPERTIES  3005
#define IDM_CTX_OPENPROC    3006
#define IDM_CTX_COPYNAME    3007

#define IDM_PRIO_IDLE         3100
#define IDM_PRIO_BELOWNORMAL  3101
#define IDM_PRIO_NORMAL       3102
#define IDM_PRIO_ABOVENORMAL  3103
#define IDM_PRIO_HIGH         3104
#define IDM_PRIO_REALTIME     3105

#define IDM_AFFINITY_BASE   3200
#define IDM_AFFINITY_MAX_CORES 64

enum TabIndex { TAB_PROCESSES = 0, TAB_SERVICES = 1, TAB_STARTUP = 2, TAB_SYSTEM = 3 };

// ============================================================================
//  NTDLL SUSPEND / RESUME
// ============================================================================
typedef LONG (NTAPI *NtSuspendProcess_t)(HANDLE);
typedef LONG (NTAPI *NtResumeProcess_t)(HANDLE);

// ============================================================================
//  THEME
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
    constexpr COLORREF Accent       = RGB(0x2E, 0xC9, 0x8E);
    constexpr COLORREF AccentDim    = RGB(0x1F, 0x8F, 0x66);
    constexpr COLORREF Amber        = RGB(0xE3, 0xA8, 0x4C);
    constexpr COLORREF Cyan         = RGB(0x4F, 0xC3, 0xF7);
    constexpr COLORREF Red          = RGB(0xE5, 0x5B, 0x5B);
    constexpr COLORREF Violet       = RGB(0xB1, 0x8C, 0xF2);
}

// ============================================================================
//  DATA MODEL
// ============================================================================
struct ProcessEntry {
    DWORD pid = 0;
    DWORD parentPid = 0;
    std::wstring name;
    std::wstring path;
    ULONGLONG workingSetBytes = 0;
    double cpuPercent = -1.0;
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    DWORD threads = 0;
};

struct ServiceEntry {
    std::wstring name;
    std::wstring displayName;
    std::wstring description;
    std::wstring binaryPath;
    DWORD state = 0;
    DWORD startType = SERVICE_DEMAND_START;
    DWORD pid = 0;
};

enum class StartupSource { HkcuRun, HklmRun, FolderUser, FolderCommon };

struct StartupEntry {
    std::wstring name;
    std::wstring command;
    StartupSource source = StartupSource::HkcuRun;
    bool enabled = true;
};

struct SystemInfoEntry {
    std::wstring label;
    std::wstring value;
};

struct AppState {
    HWND hwnd = nullptr;
    HWND hTab = nullptr;
    HWND hBtnElevate = nullptr;
    HWND hCpuBar = nullptr, hCpuLabel = nullptr, hCpuCaption = nullptr;
    HWND hLog = nullptr;
    HWND hSearch = nullptr, hBtnSync = nullptr, hChkAuto = nullptr;
    HWND hBtnSuspend = nullptr, hBtnResume = nullptr, hBtnTerminate = nullptr;
    HWND hList = nullptr;

    HWND hListServices = nullptr;
    HWND hBtnSvcStart = nullptr, hBtnSvcStop = nullptr, hBtnSvcRestart = nullptr, hBtnSvcRefresh = nullptr;

    HWND hListStartup = nullptr;
    HWND hBtnStartupEnable = nullptr, hBtnStartupDisable = nullptr;
    HWND hBtnStartupOpenLoc = nullptr, hBtnStartupRefresh = nullptr;

    HWND hListSysInfo = nullptr;

    HFONT fontTitle = nullptr, fontSubtitle = nullptr, fontUI = nullptr, fontUIBold = nullptr, fontMono = nullptr;
    HBRUSH hbrBgWindow = nullptr, hbrBgPanel = nullptr, hbrBgControl = nullptr;
    HBRUSH hbrListRow = nullptr, hbrListRowAlt = nullptr, hbrListSel = nullptr;

    UINT dpi = 96;
    double scale = 1.0;
    int activeTab = 0;

    std::vector<ProcessEntry> allProcesses;
    std::wstring filterLower;
    COLORREF logColor = Theme::Accent;

    std::vector<ServiceEntry> allServices;
    std::vector<StartupEntry> allStartupItems;
    std::vector<SystemInfoEntry> allSystemInfo;

    NtSuspendProcess_t pNtSuspendProcess = nullptr;
    NtResumeProcess_t  pNtResumeProcess  = nullptr;

    PDH_HQUERY   pdhQuery = nullptr;
    PDH_HCOUNTER pdhCounter = nullptr;

    std::unordered_map<DWORD, ULONGLONG> cpuPrevTimes;
    ULONGLONG cpuPrevTick = 0;
    DWORD numCores = 1;

    bool isElevated = false;
    DWORD contextMenuPid = 0;

    NOTIFYICONDATAW nid{};
    bool trayActive = false;

    int processSortColumn = -1;
    bool processSortAscending = false;

    std::wofstream logFile;
};

static AppState g_app;

inline int SC(int v) { return static_cast<int>(v * g_app.scale + (v >= 0 ? 0.5 : -0.5)); }

// ============================================================================
//  FORWARD DECLARATIONS
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
void CreateProcessTabControls(HWND hwnd, HINSTANCE hInst);
void CreateServicesTabControls(HWND hwnd, HINSTANCE hInst);
void CreateStartupTabControls(HWND hwnd, HINSTANCE hInst);
void CreateSystemTabControls(HWND hwnd, HINSTANCE hInst);

void SwitchTab(int index);
void RelayoutControls();
void MakeButtonRounded(HWND hBtn, int w, int h);

RECT GetUnionClientRect(const std::vector<HWND>& hwnds, int pad);
void DrawPanel(HDC hdc, RECT r);
void DrawFrame(HDC hdc, RECT r, COLORREF color);
std::vector<HWND> GetActiveToolbarControls();
HWND GetActiveListControl();

BOOL IsProcessElevated();
void RelaunchAsAdmin();

void InitTrayIcon();
void RemoveTrayIcon();
void ShowTrayMenu();

void InitNtdllFunctions();
void InitCpuMonitor();
int  ReadCpuPercent();

const wchar_t* PriorityToString(DWORD pc);
DWORD PriorityMenuIdToClass(UINT id);

std::vector<ProcessEntry> EnumerateProcesses();
bool DoSuspend(DWORD pid);
bool DoResume(DWORD pid);
bool DoTerminate(DWORD pid);
void ApplyPriority(DWORD pid, DWORD priorityClass);
void ToggleAffinityBit(DWORD pid, int coreIndex);
void ShowProcessContextMenu(HWND owner, POINT screenPt, DWORD pid);

void RefreshProcessData();
void RefreshListDisplay();
DWORD GetSelectedPid();
void SetLog(const std::wstring& text, COLORREF color);

std::wstring ServiceStateToString(DWORD state);
std::wstring ServiceStartTypeToString(DWORD startType);
std::vector<ServiceEntry> EnumerateServices();
void RefreshServicesList();
int  GetSelectedServiceIndex();
bool StartServiceByName(const std::wstring& name);
bool StopServiceByName(const std::wstring& name);
bool RestartServiceByName(const std::wstring& name);

std::wstring ExtractExecutablePath(const std::wstring& command);
std::vector<StartupEntry> EnumerateStartupItems();
void RefreshStartupList();
int  GetSelectedStartupIndex();
bool DisableStartupEntry(const StartupEntry& e);
bool EnableStartupEntry(const StartupEntry& e);

std::vector<SystemInfoEntry> EnumerateSystemInfo();
void RefreshSystemInfoList();

void OpenFileLocation(const std::wstring& path);
void CopyTextToClipboard(HWND hwnd, const std::wstring& text);
void ShowFileProperties(const std::wstring& path);
void OpenProcessFolder(DWORD pid);

LRESULT HandleDrawItem(LPARAM lParam);
LRESULT HandleListCustomDraw(LPARAM lParam);

// ============================================================================
//  RAII HANDLE WRAPPER
// ============================================================================
class ScopedHandle {
public:
    ScopedHandle(HANDLE h = nullptr) : handle_(h) {}
    ~ScopedHandle() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    HANDLE get() const { return handle_; }
    HANDLE* put() { return &handle_; }
    void reset(HANDLE h = nullptr) {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = h;
    }
    bool valid() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE handle_;
};

class ScopedSC_HANDLE {
public:
    ScopedSC_HANDLE(SC_HANDLE h = nullptr) : handle_(h) {}
    ~ScopedSC_HANDLE() { if (handle_) CloseServiceHandle(handle_); }
    ScopedSC_HANDLE(const ScopedSC_HANDLE&) = delete;
    ScopedSC_HANDLE& operator=(const ScopedSC_HANDLE&) = delete;
    ScopedSC_HANDLE(ScopedSC_HANDLE&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    ScopedSC_HANDLE& operator=(ScopedSC_HANDLE&& other) noexcept {
        if (this != &other) { if (handle_) CloseServiceHandle(handle_); handle_ = other.handle_; other.handle_ = nullptr; }
        return *this;
    }
    SC_HANDLE get() const { return handle_; }
    SC_HANDLE* put() { return &handle_; }
    void reset(SC_HANDLE h = nullptr) { if (handle_) CloseServiceHandle(handle_); handle_ = h; }
    bool valid() const { return handle_ != nullptr; }
private:
    SC_HANDLE handle_;
};

class ScopedRegKey {
public:
    ScopedRegKey(HKEY h = nullptr) : key_(h) {}
    ~ScopedRegKey() { if (key_) RegCloseKey(key_); }
    ScopedRegKey(const ScopedRegKey&) = delete;
    ScopedRegKey& operator=(const ScopedRegKey&) = delete;
    ScopedRegKey(ScopedRegKey&& other) noexcept : key_(other.key_) { other.key_ = nullptr; }
    ScopedRegKey& operator=(ScopedRegKey&& other) noexcept {
        if (this != &other) { if (key_) RegCloseKey(key_); key_ = other.key_; other.key_ = nullptr; }
        return *this;
    }
    HKEY get() const { return key_; }
    HKEY* put() { return &key_; }
    void reset(HKEY h = nullptr) { if (key_) RegCloseKey(key_); key_ = h; }
    bool valid() const { return key_ != nullptr; }
private:
    HKEY key_;
};

// ============================================================================
//  ENTRY POINT
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    EnableHighDpiAwareness();

    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES |
        ICC_BAR_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"MynaTaskFullWindowClass";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_app.dpi = GetDpiForSystem();
    g_app.scale = g_app.dpi / 96.0;
    g_app.isElevated = IsProcessElevated();

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    g_app.numCores = si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1;

    int w = SC(1250), h = SC(800);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Myna Task Full",
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

#ifndef _MSC_VER
extern "C" int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return wWinMain(hInstance, hPrevInstance, GetCommandLineW(), nCmdShow);
}
#endif

// ============================================================================
//  WINDOW PROCEDURE
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
        SwitchTab(TAB_PROCESSES);
        RelayoutControls();
        RefreshProcessData();
        RefreshServicesList();
        RefreshStartupList();
        RefreshSystemInfoList();
        InitTrayIcon();

        if (!g_app.isElevated) {
            SetLog(L"[!] Running without administrator rights. Some actions may be limited.", Theme::Amber);
        }

        g_app.logFile.open("myna_task_log.txt", std::ios::app);
        if (g_app.logFile.is_open()) {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tm;
            localtime_s(&tm, &t);
            g_app.logFile << L"\n===== Session started at " << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S") << L" =====\n";
        }

        SetTimer(hwnd, ID_TIMER_CPU, 1000, nullptr);
        SetTimer(hwnd, ID_TIMER_SYNC, 2000, nullptr);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = SC(860);
        mmi->ptMinTrackSize.y = SC(540);
        return 0;
    }

    case WM_SIZE:
        if (g_app.hTab) RelayoutControls();
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
        RECT rc;
        GetClientRect(hwnd, &rc);

        FillRect(hdc, &rc, g_app.hbrBgWindow);

        int margin = SC(16);
        int headerH = SC(56);

        SetBkMode(hdc, TRANSPARENT);
        HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_app.fontTitle));
        SetTextColor(hdc, Theme::Accent);
        RECT titleRect{ margin, margin, rc.right - margin, margin + SC(30) };
        DrawTextW(hdc, L"MYNA TASK FULL", -1, &titleRect, DT_LEFT | DT_TOP | DT_SINGLELINE);

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

        if (g_app.hTab) {
            std::vector<HWND> toolbar = GetActiveToolbarControls();
            toolbar.push_back(g_app.hCpuBar);
            toolbar.push_back(g_app.hCpuLabel);
            toolbar.push_back(g_app.hCpuCaption);
            RECT toolRc = GetUnionClientRect(toolbar, SC(10));
            DrawPanel(hdc, toolRc);

            HWND activeList = GetActiveListControl();
            if (activeList) {
                RECT listRc;
                GetWindowRect(activeList, &listRc);
                POINT tl{ listRc.left, listRc.top }, br{ listRc.right, listRc.bottom };
                ScreenToClient(hwnd, &tl); ScreenToClient(hwnd, &br);
                RECT listFrame{ tl.x - SC(1), tl.y - SC(1), br.x + SC(1), br.y + SC(1) };
                DrawFrame(hdc, listFrame, Theme::Border);
            }

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
            target == g_app.hBtnResume || target == g_app.hBtnTerminate ||
            target == g_app.hBtnElevate ||
            target == g_app.hBtnSvcStart || target == g_app.hBtnSvcStop ||
            target == g_app.hBtnSvcRestart || target == g_app.hBtnSvcRefresh ||
            target == g_app.hBtnStartupEnable || target == g_app.hBtnStartupDisable ||
            target == g_app.hBtnStartupOpenLoc || target == g_app.hBtnStartupRefresh) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM:
        return HandleDrawItem(lParam);

    case WM_CONTEXTMENU: {
        HWND target = reinterpret_cast<HWND>(wParam);
        if (target == g_app.hList) {
            POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (pt.x == -1 && pt.y == -1) {
                int idx = ListView_GetNextItem(g_app.hList, -1, LVNI_SELECTED);
                if (idx < 0) return 0;
                RECT r; ListView_GetItemRect(g_app.hList, idx, &r, LVIR_BOUNDS);
                pt.x = r.left; pt.y = r.bottom;
                ClientToScreen(g_app.hList, &pt);
            }
            DWORD pid = GetSelectedPid();
            if (pid) ShowProcessContextMenu(hwnd, pt, pid);
            return 0;
        }
        break;
    }

    case WM_NOTIFY: {
        LPNMHDR hdr = reinterpret_cast<LPNMHDR>(lParam);
        if ((hdr->hwndFrom == g_app.hList || hdr->hwndFrom == g_app.hListServices ||
             hdr->hwndFrom == g_app.hListStartup || hdr->hwndFrom == g_app.hListSysInfo) &&
            hdr->code == NM_CUSTOMDRAW) {
            return HandleListCustomDraw(lParam);
        }
        if (hdr->hwndFrom == g_app.hTab && hdr->code == TCN_SELCHANGE) {
            SwitchTab(TabCtrl_GetCurSel(g_app.hTab));
            RelayoutControls();
        }
        if (hdr->hwndFrom == g_app.hList && hdr->code == LVN_COLUMNCLICK) {
            NMLISTVIEW* pnmv = reinterpret_cast<NMLISTVIEW*>(lParam);
            int col = pnmv->iSubItem;
            if (g_app.processSortColumn == col) {
                g_app.processSortAscending = !g_app.processSortAscending;
            } else {
                g_app.processSortColumn = col;
                g_app.processSortAscending = true;
            }
            RefreshListDisplay();
        }
        if (hdr->hwndFrom == g_app.hList && hdr->code == LVN_GETDISPINFO) {
            NMLVDISPINFO* pdi = reinterpret_cast<NMLVDISPINFO*>(lParam);
            int idx = pdi->item.iItem;
            if (idx >= 0 && idx < static_cast<int>(g_app.allProcesses.size())) {
                const ProcessEntry& p = g_app.allProcesses[idx];
                switch (pdi->item.iSubItem) {
                case 0: pdi->item.pszText = const_cast<LPWSTR>(p.name.c_str()); break;
                case 1: {
                    static wchar_t buf[32];
                    swprintf_s(buf, 32, L"%u", p.pid);
                    pdi->item.pszText = buf;
                    break;
                }
                case 2: {
                    static wchar_t buf[32];
                    if (p.cpuPercent >= 0.0) swprintf_s(buf, 32, L"%.1f%%", p.cpuPercent);
                    else wcscpy_s(buf, L"-");
                    pdi->item.pszText = buf;
                    break;
                }
                case 3: {
                    static wchar_t buf[32];
                    double mb = p.workingSetBytes / (1024.0 * 1024.0);
                    swprintf_s(buf, 32, L"%.1f MB", mb);
                    pdi->item.pszText = buf;
                    break;
                }
                case 4: pdi->item.pszText = const_cast<LPWSTR>(PriorityToString(p.priorityClass)); break;
                case 5: pdi->item.pszText = const_cast<LPWSTR>(p.path.c_str()); break;
                }
            }
            return 0;
        }
        break;
    }

    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayMenu();
        }
        return 0;
    }

    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_SEARCHBOX && HIWORD(wParam) == EN_CHANGE) {
            wchar_t buf[256];
            GetWindowTextW(g_app.hSearch, buf, 256);
            g_app.filterLower = buf;
            std::transform(g_app.filterLower.begin(), g_app.filterLower.end(),
                g_app.filterLower.begin(), ::towlower);
            RefreshListDisplay();
        }
        else if (id == IDC_BTN_SYNC) {
            RefreshProcessData();
            SetLog(L"[>] Sync completed.", Theme::Accent);
        }
        else if (id == IDC_BTN_SUSPEND) {
            DWORD pid = GetSelectedPid();
            if (pid) {
                wchar_t buf[64];
                swprintf_s(buf, 64, L"%u", pid);
                if (DoSuspend(pid)) SetLog(std::wstring(L"[>] Suspended PID ") + buf, Theme::Amber);
                else SetLog(std::wstring(L"[!] Access denied for PID ") + buf, Theme::Red);
            }
        }
        else if (id == IDC_BTN_RESUME) {
            DWORD pid = GetSelectedPid();
            if (pid) {
                wchar_t buf[64];
                swprintf_s(buf, 64, L"%u", pid);
                if (DoResume(pid)) SetLog(std::wstring(L"[>] Resumed PID ") + buf, Theme::Cyan);
                else SetLog(std::wstring(L"[!] Access denied for PID ") + buf, Theme::Red);
            }
        }
        else if (id == IDC_BTN_TERMINATE) {
            DWORD pid = GetSelectedPid();
            if (pid) {
                wchar_t buf[64];
                swprintf_s(buf, 64, L"%u", pid);
                if (DoTerminate(pid)) {
                    SetLog(std::wstring(L"[X] Terminated PID ") + buf, Theme::Red);
                    RefreshProcessData();
                } else {
                    SetLog(std::wstring(L"[!] Failed to terminate protected PID ") + buf, Theme::Red);
                }
            }
        }
        else if (id == IDC_BTN_ELEVATE) {
            RelaunchAsAdmin();
        }
        else if (id == IDM_CTX_SUSPEND) {
            if (g_app.contextMenuPid && DoSuspend(g_app.contextMenuPid))
                SetLog(L"[>] Suspended selected process.", Theme::Amber);
        }
        else if (id == IDM_CTX_RESUME) {
            if (g_app.contextMenuPid && DoResume(g_app.contextMenuPid))
                SetLog(L"[>] Resumed selected process.", Theme::Cyan);
        }
        else if (id == IDM_CTX_TERMINATE) {
            if (g_app.contextMenuPid && DoTerminate(g_app.contextMenuPid)) {
                SetLog(L"[X] Terminated selected process.", Theme::Red);
                RefreshProcessData();
            }
        }
        else if (id == IDM_CTX_OPENLOC) {
            auto it = std::find_if(g_app.allProcesses.begin(), g_app.allProcesses.end(),
                [](const ProcessEntry& p) { return p.pid == g_app.contextMenuPid; });
            if (it != g_app.allProcesses.end()) OpenFileLocation(it->path);
        }
        else if (id == IDM_CTX_COPYPATH) {
            auto it = std::find_if(g_app.allProcesses.begin(), g_app.allProcesses.end(),
                [](const ProcessEntry& p) { return p.pid == g_app.contextMenuPid; });
            if (it != g_app.allProcesses.end()) CopyTextToClipboard(hwnd, it->path);
        }
        else if (id == IDM_CTX_PROPERTIES) {
            auto it = std::find_if(g_app.allProcesses.begin(), g_app.allProcesses.end(),
                [](const ProcessEntry& p) { return p.pid == g_app.contextMenuPid; });
            if (it != g_app.allProcesses.end()) ShowFileProperties(it->path);
        }
        else if (id == IDM_CTX_OPENPROC) {
            if (g_app.contextMenuPid) OpenProcessFolder(g_app.contextMenuPid);
        }
        else if (id == IDM_CTX_COPYNAME) {
            auto it = std::find_if(g_app.allProcesses.begin(), g_app.allProcesses.end(),
                [](const ProcessEntry& p) { return p.pid == g_app.contextMenuPid; });
            if (it != g_app.allProcesses.end()) CopyTextToClipboard(hwnd, it->name);
        }
        else if (id >= IDM_PRIO_IDLE && id <= IDM_PRIO_REALTIME) {
            if (g_app.contextMenuPid) ApplyPriority(g_app.contextMenuPid, PriorityMenuIdToClass(id));
        }
        else if (id >= IDM_AFFINITY_BASE && id < IDM_AFFINITY_BASE + IDM_AFFINITY_MAX_CORES) {
            if (g_app.contextMenuPid) ToggleAffinityBit(g_app.contextMenuPid, id - IDM_AFFINITY_BASE);
        }
        else if (id == IDC_BTN_SVC_START) {
            int idx = GetSelectedServiceIndex();
            if (idx >= 0) {
                if (StartServiceByName(g_app.allServices[idx].name))
                    SetLog(L"[>] Service start requested.", Theme::Accent);
                else
                    SetLog(L"[!] Failed to start service.", Theme::Red);
                RefreshServicesList();
            }
        }
        else if (id == IDC_BTN_SVC_STOP) {
            int idx = GetSelectedServiceIndex();
            if (idx >= 0) {
                if (StopServiceByName(g_app.allServices[idx].name))
                    SetLog(L"[>] Service stop requested.", Theme::Amber);
                else
                    SetLog(L"[!] Failed to stop service.", Theme::Red);
                RefreshServicesList();
            }
        }
        else if (id == IDC_BTN_SVC_RESTART) {
            int idx = GetSelectedServiceIndex();
            if (idx >= 0) {
                if (RestartServiceByName(g_app.allServices[idx].name))
                    SetLog(L"[>] Service restarted.", Theme::Cyan);
                else
                    SetLog(L"[!] Failed to restart service.", Theme::Red);
                RefreshServicesList();
            }
        }
        else if (id == IDC_BTN_SVC_REFRESH) {
            RefreshServicesList();
            SetLog(L"[>] Services list refreshed.", Theme::Accent);
        }
        else if (id == IDC_BTN_STARTUP_ENABLE) {
            int idx = GetSelectedStartupIndex();
            if (idx >= 0) {
                if (EnableStartupEntry(g_app.allStartupItems[idx]))
                    SetLog(L"[>] Startup item enabled.", Theme::Accent);
                else
                    SetLog(L"[!] Could not enable item (try running as admin).", Theme::Red);
                RefreshStartupList();
            }
        }
        else if (id == IDC_BTN_STARTUP_DISABLE) {
            int idx = GetSelectedStartupIndex();
            if (idx >= 0) {
                if (DisableStartupEntry(g_app.allStartupItems[idx]))
                    SetLog(L"[>] Startup item disabled.", Theme::Amber);
                else
                    SetLog(L"[!] Could not disable item (try running as admin).", Theme::Red);
                RefreshStartupList();
            }
        }
        else if (id == IDC_BTN_STARTUP_OPENLOC) {
            int idx = GetSelectedStartupIndex();
            if (idx >= 0) OpenFileLocation(ExtractExecutablePath(g_app.allStartupItems[idx].command));
        }
        else if (id == IDC_BTN_STARTUP_REFRESH) {
            RefreshStartupList();
            SetLog(L"[>] Startup list refreshed.", Theme::Accent);
        }
        else if (id == IDM_TRAY_RESTORE) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        else if (id == IDM_TRAY_EXIT) {
            DestroyWindow(hwnd);
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
        RemoveTrayIcon();
        if (g_app.pdhQuery) PdhCloseQuery(g_app.pdhQuery);
        if (g_app.logFile.is_open()) g_app.logFile.close();
        DeleteAppFonts();
        CleanupGdiResources();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
//  SYSTEM & DWM HELPERS
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
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore) {
        typedef HRESULT(WINAPI* SetProcessDpiAwareness_t)(int);
        auto pSetDpi = reinterpret_cast<SetProcessDpiAwareness_t>(GetProcAddress(hShcore, "SetProcessDpiAwareness"));
        if (pSetDpi) pSetDpi(2);
        FreeLibrary(hShcore);
    }
}

void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

void ApplyRoundedCorners(HWND hwnd) {
    DWORD cornerPref = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
}

void TryEnableMica(HWND hwnd) {
    DWORD backdropType = 2;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
}

BOOL IsProcessElevated() {
    BOOL elevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return elevated;
}

void RelaunchAsAdmin() {
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, szPath, MAX_PATH)) {
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.lpVerb = L"runas";
        sei.lpFile = szPath;
        sei.hwnd = g_app.hwnd;
        sei.nShow = SW_NORMAL;
        if (ShellExecuteExW(&sei)) {
            PostQuitMessage(0);
        }
    }
}

// ============================================================================
//  TRAY ICON HELPERS
// ============================================================================
void InitTrayIcon() {
    g_app.nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_app.nid.hWnd = g_app.hwnd;
    g_app.nid.uID = 1;
    g_app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.nid.uCallbackMessage = WM_TRAYICON;
    g_app.nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_app.nid.szTip, L"Myna Task Full");
    Shell_NotifyIconW(NIM_ADD, &g_app.nid);
    g_app.trayActive = true;
}

void RemoveTrayIcon() {
    if (g_app.trayActive) {
        Shell_NotifyIconW(NIM_DELETE, &g_app.nid);
        g_app.trayActive = false;
    }
}

void ShowTrayMenu() {
    HMENU hMenu = CreatePopupMenu();
    POINT pt;
    GetCursorPos(&pt);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, L"Restore Myna Task Full");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit");
    SetForegroundWindow(g_app.hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_app.hwnd, nullptr);
    PostMessageW(g_app.hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

// ============================================================================
//  FONTS & GDI CACHE
// ============================================================================
void InitGdiResources() {
    g_app.hbrBgWindow  = CreateSolidBrush(Theme::BgWindow);
    g_app.hbrBgPanel   = CreateSolidBrush(Theme::BgPanel);
    g_app.hbrBgControl = CreateSolidBrush(Theme::BgControl);
    g_app.hbrListRow   = CreateSolidBrush(Theme::BgListRow);
    g_app.hbrListRowAlt = CreateSolidBrush(Theme::BgListRowAlt);
    g_app.hbrListSel   = CreateSolidBrush(Theme::BgListSel);
}

void CleanupGdiResources() {
    if (g_app.hbrBgWindow)  DeleteObject(g_app.hbrBgWindow);
    if (g_app.hbrBgPanel)   DeleteObject(g_app.hbrBgPanel);
    if (g_app.hbrBgControl) DeleteObject(g_app.hbrBgControl);
    if (g_app.hbrListRow)   DeleteObject(g_app.hbrListRow);
    if (g_app.hbrListRowAlt) DeleteObject(g_app.hbrListRowAlt);
    if (g_app.hbrListSel)   DeleteObject(g_app.hbrListSel);
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
    auto mk = [&](int pt, int weight, const wchar_t* face) {
        int h = -MulDiv(pt, dpi, 72);
        return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    };
    g_app.fontTitle    = mk(16, FW_BOLD,     L"Segoe UI");
    g_app.fontSubtitle = mk(9,  FW_SEMIBOLD, L"Segoe UI");
    g_app.fontUI       = mk(9,  FW_NORMAL,   L"Segoe UI");
    g_app.fontUIBold   = mk(9,  FW_BOLD,     L"Segoe UI");
    g_app.fontMono     = mk(9,  FW_NORMAL,   L"Consolas");
}

void ApplyFontsToControls() {
    HWND ctrls[] = {
        g_app.hTab, g_app.hSearch, g_app.hBtnSync, g_app.hChkAuto,
        g_app.hBtnSuspend, g_app.hBtnResume, g_app.hBtnTerminate, g_app.hList,
        g_app.hCpuLabel, g_app.hCpuCaption, g_app.hBtnElevate,
        g_app.hListServices, g_app.hBtnSvcStart, g_app.hBtnSvcStop,
        g_app.hBtnSvcRestart, g_app.hBtnSvcRefresh,
        g_app.hListStartup, g_app.hBtnStartupEnable, g_app.hBtnStartupDisable,
        g_app.hBtnStartupOpenLoc, g_app.hBtnStartupRefresh,
        g_app.hListSysInfo
    };
    for (HWND c : ctrls) {
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontUI), TRUE);
    }
    if (g_app.hLog) SendMessageW(g_app.hLog, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.fontMono), TRUE);
}

// ============================================================================
//  CONTROL CREATION
// ============================================================================
void CreateControls(HWND hwnd) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    g_app.hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_TAB), hInst, nullptr);

    TCITEMW tie{};
    tie.mask = TCIF_TEXT;
    tie.pszText = const_cast<LPWSTR>(L"Processes");
    TabCtrl_InsertItem(g_app.hTab, TAB_PROCESSES, &tie);
    tie.pszText = const_cast<LPWSTR>(L"Services");
    TabCtrl_InsertItem(g_app.hTab, TAB_SERVICES, &tie);
    tie.pszText = const_cast<LPWSTR>(L"Startup");
    TabCtrl_InsertItem(g_app.hTab, TAB_STARTUP, &tie);
    tie.pszText = const_cast<LPWSTR>(L"System");
    TabCtrl_InsertItem(g_app.hTab, TAB_SYSTEM, &tie);

    g_app.hBtnElevate = CreateWindowExW(0, L"BUTTON", L"ADMIN",
        WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_BTN_ELEVATE), hInst, nullptr);

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

    g_app.hLog = CreateWindowExW(0, L"STATIC", L"[SYSTEM] Engine ready.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LOG_LABEL), hInst, nullptr);

    CreateProcessTabControls(hwnd, hInst);
    CreateServicesTabControls(hwnd, hInst);
    CreateStartupTabControls(hwnd, hInst);
    CreateSystemTabControls(hwnd, hInst);

    ApplyFontsToControls();
}

void CreateProcessTabControls(HWND hwnd, HINSTANCE hInst) {
    g_app.hSearch = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_SEARCHBOX), hInst, nullptr);
    SetWindowTheme(g_app.hSearch, L"DarkMode_CFD", nullptr);
    SendMessageW(g_app.hSearch, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search processes..."));

    auto mkBtn = [&](const wchar_t* text, int id) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    };

    g_app.hBtnSync      = mkBtn(L"SYNC", IDC_BTN_SYNC);
    g_app.hChkAuto     = CreateWindowExW(0, L"BUTTON", L"Auto",
        WS_CHILD | BS_AUTOCHECKBOX,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_CHK_AUTOSYNC), hInst, nullptr);
    Button_SetCheck(g_app.hChkAuto, BST_CHECKED);

    g_app.hBtnSuspend   = mkBtn(L"SUSPEND", IDC_BTN_SUSPEND);
    g_app.hBtnResume    = mkBtn(L"RESUME", IDC_BTN_RESUME);
    g_app.hBtnTerminate = mkBtn(L"KILL", IDC_BTN_TERMINATE);

    g_app.hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LISTVIEW), hInst, nullptr);
    SetWindowTheme(g_app.hList, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(g_app.hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<LPWSTR>(L"NAME");
    col.cx = SC(180); col.iSubItem = 0; ListView_InsertColumn(g_app.hList, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"PID");
    col.cx = SC(70);  col.iSubItem = 1; ListView_InsertColumn(g_app.hList, 1, &col);
    col.pszText = const_cast<LPWSTR>(L"CPU %");
    col.cx = SC(80);  col.iSubItem = 2; ListView_InsertColumn(g_app.hList, 2, &col);
    col.pszText = const_cast<LPWSTR>(L"MEMORY");
    col.cx = SC(90);  col.iSubItem = 3; ListView_InsertColumn(g_app.hList, 3, &col);
    col.pszText = const_cast<LPWSTR>(L"PRIORITY");
    col.cx = SC(100); col.iSubItem = 4; ListView_InsertColumn(g_app.hList, 4, &col);
    col.pszText = const_cast<LPWSTR>(L"PATH");
    col.cx = SC(300); col.iSubItem = 5; ListView_InsertColumn(g_app.hList, 5, &col);
}

void CreateServicesTabControls(HWND hwnd, HINSTANCE hInst) {
    auto mkBtn = [&](const wchar_t* text, int id) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    };

    g_app.hBtnSvcStart   = mkBtn(L"START", IDC_BTN_SVC_START);
    g_app.hBtnSvcStop    = mkBtn(L"STOP", IDC_BTN_SVC_STOP);
    g_app.hBtnSvcRestart = mkBtn(L"RESTART", IDC_BTN_SVC_RESTART);
    g_app.hBtnSvcRefresh = mkBtn(L"REFRESH", IDC_BTN_SVC_REFRESH);

    g_app.hListServices = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST_SERVICES), hInst, nullptr);
    SetWindowTheme(g_app.hListServices, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(g_app.hListServices, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<LPWSTR>(L"SERVICE NAME");
    col.cx = SC(180); col.iSubItem = 0; ListView_InsertColumn(g_app.hListServices, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"DISPLAY NAME");
    col.cx = SC(200); col.iSubItem = 1; ListView_InsertColumn(g_app.hListServices, 1, &col);
    col.pszText = const_cast<LPWSTR>(L"STATUS");
    col.cx = SC(90);  col.iSubItem = 2; ListView_InsertColumn(g_app.hListServices, 2, &col);
    col.pszText = const_cast<LPWSTR>(L"START TYPE");
    col.cx = SC(100); col.iSubItem = 3; ListView_InsertColumn(g_app.hListServices, 3, &col);
    col.pszText = const_cast<LPWSTR>(L"PID");
    col.cx = SC(60);  col.iSubItem = 4; ListView_InsertColumn(g_app.hListServices, 4, &col);
    col.pszText = const_cast<LPWSTR>(L"DESCRIPTION");
    col.cx = SC(200); col.iSubItem = 5; ListView_InsertColumn(g_app.hListServices, 5, &col);
    col.pszText = const_cast<LPWSTR>(L"BINARY PATH");
    col.cx = SC(250); col.iSubItem = 6; ListView_InsertColumn(g_app.hListServices, 6, &col);
}

void CreateStartupTabControls(HWND hwnd, HINSTANCE hInst) {
    auto mkBtn = [&](const wchar_t* text, int id) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | BS_OWNERDRAW,
            0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst, nullptr);
    };

    g_app.hBtnStartupEnable  = mkBtn(L"ENABLE", IDC_BTN_STARTUP_ENABLE);
    g_app.hBtnStartupDisable = mkBtn(L"DISABLE", IDC_BTN_STARTUP_DISABLE);
    g_app.hBtnStartupOpenLoc = mkBtn(L"LOCATION", IDC_BTN_STARTUP_OPENLOC);
    g_app.hBtnStartupRefresh = mkBtn(L"REFRESH", IDC_BTN_STARTUP_REFRESH);

    g_app.hListStartup = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST_STARTUP), hInst, nullptr);
    SetWindowTheme(g_app.hListStartup, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(g_app.hListStartup, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<LPWSTR>(L"NAME");
    col.cx = SC(150); col.iSubItem = 0; ListView_InsertColumn(g_app.hListStartup, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"COMMAND");
    col.cx = SC(350); col.iSubItem = 1; ListView_InsertColumn(g_app.hListStartup, 1, &col);
    col.pszText = const_cast<LPWSTR>(L"STATUS");
    col.cx = SC(80);  col.iSubItem = 2; ListView_InsertColumn(g_app.hListStartup, 2, &col);
    col.pszText = const_cast<LPWSTR>(L"SOURCE");
    col.cx = SC(150); col.iSubItem = 3; ListView_InsertColumn(g_app.hListStartup, 3, &col);
}

void CreateSystemTabControls(HWND hwnd, HINSTANCE hInst) {
    g_app.hListSysInfo = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(IDC_LIST_SYSINFO), hInst, nullptr);
    SetWindowTheme(g_app.hListSysInfo, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(g_app.hListSysInfo, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = const_cast<LPWSTR>(L"INFORMATION");
    col.cx = SC(300); col.iSubItem = 0; ListView_InsertColumn(g_app.hListSysInfo, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"VALUE");
    col.cx = SC(500); col.iSubItem = 1; ListView_InsertColumn(g_app.hListSysInfo, 1, &col);
}

void SwitchTab(int index) {
    g_app.activeTab = index;

    ShowWindow(g_app.hSearch, index == TAB_PROCESSES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnSync, index == TAB_PROCESSES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hChkAuto, index == TAB_PROCESSES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnSuspend, index == TAB_PROCESSES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnResume, index == TAB_PROCESSES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnTerminate, index == TAB_PROCESSES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hList, index == TAB_PROCESSES ? SW_SHOW : SW_HIDE);

    ShowWindow(g_app.hBtnSvcStart, index == TAB_SERVICES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnSvcStop, index == TAB_SERVICES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnSvcRestart, index == TAB_SERVICES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnSvcRefresh, index == TAB_SERVICES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hListServices, index == TAB_SERVICES ? SW_SHOW : SW_HIDE);

    ShowWindow(g_app.hBtnStartupEnable, index == TAB_STARTUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnStartupDisable, index == TAB_STARTUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnStartupOpenLoc, index == TAB_STARTUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hBtnStartupRefresh, index == TAB_STARTUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.hListStartup, index == TAB_STARTUP ? SW_SHOW : SW_HIDE);

    ShowWindow(g_app.hListSysInfo, index == TAB_SYSTEM ? SW_SHOW : SW_HIDE);

    InvalidateRect(g_app.hwnd, nullptr, TRUE);
}

std::vector<HWND> GetActiveToolbarControls() {
    switch (g_app.activeTab) {
    case TAB_PROCESSES:
        return { g_app.hSearch, g_app.hBtnSync, g_app.hChkAuto, g_app.hBtnSuspend, g_app.hBtnResume, g_app.hBtnTerminate };
    case TAB_SERVICES:
        return { g_app.hBtnSvcStart, g_app.hBtnSvcStop, g_app.hBtnSvcRestart, g_app.hBtnSvcRefresh };
    case TAB_STARTUP:
        return { g_app.hBtnStartupEnable, g_app.hBtnStartupDisable, g_app.hBtnStartupOpenLoc, g_app.hBtnStartupRefresh };
    case TAB_SYSTEM:
        return {};
    }
    return {};
}

HWND GetActiveListControl() {
    switch (g_app.activeTab) {
    case TAB_PROCESSES: return g_app.hList;
    case TAB_SERVICES: return g_app.hListServices;
    case TAB_STARTUP:   return g_app.hListStartup;
    case TAB_SYSTEM:    return g_app.hListSysInfo;
    }
    return nullptr;
}

// ============================================================================
//  LAYOUT
// ============================================================================
void RelayoutControls() {
    if (!g_app.hTab) return;

    RECT rc;
    GetClientRect(g_app.hwnd, &rc);

    int margin = SC(16);
    int headerH = SC(56);

    int elevateW = SC(74), elevateH = SC(28);
    MoveWindow(g_app.hBtnElevate, rc.right - margin - elevateW, margin + SC(4), elevateW, elevateH, TRUE);
    MakeButtonRounded(g_app.hBtnElevate, elevateW, elevateH);

    int tabY = headerH + margin;
    int tabH = rc.bottom - tabY - margin;
    MoveWindow(g_app.hTab, margin, tabY, rc.right - (margin * 2), tabH, TRUE);

    RECT tabDisplay = rc;
    tabDisplay.left += margin;
    tabDisplay.right -= margin;
    tabDisplay.top += tabY;
    tabDisplay.bottom = tabY + tabH;
    TabCtrl_AdjustRect(g_app.hTab, FALSE, &tabDisplay);

    int toolbarH = SC(32);
    int footerH = SC(34);
    int listY = tabDisplay.top + toolbarH + SC(12);
    int listH = tabDisplay.bottom - listY - footerH - SC(8);
    int listW = tabDisplay.right - tabDisplay.left - SC(8);

    int cx = tabDisplay.left + SC(4);
    int y = tabDisplay.top + SC(4);

    if (g_app.activeTab == TAB_PROCESSES) {
        int searchW = SC(220);
        MoveWindow(g_app.hSearch, cx, y, searchW, toolbarH, TRUE);
        cx += searchW + SC(8);

        int syncW = SC(70);
        MoveWindow(g_app.hBtnSync, cx, y, syncW, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnSync, syncW, toolbarH);
        cx += syncW + SC(6);

        int chkW = SC(60);
        MoveWindow(g_app.hChkAuto, cx, y + SC(6), chkW, toolbarH - SC(8), TRUE);
        cx += chkW + SC(12);

        int actW = SC(90);
        MoveWindow(g_app.hBtnSuspend, cx, y, actW, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnSuspend, actW, toolbarH);
        cx += actW + SC(6);

        MoveWindow(g_app.hBtnResume, cx, y, actW, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnResume, actW, toolbarH);
        cx += actW + SC(6);

        int killW = SC(70);
        MoveWindow(g_app.hBtnTerminate, cx, y, killW, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnTerminate, killW, toolbarH);
    }
    else if (g_app.activeTab == TAB_SERVICES) {
        int bw = SC(90);
        MoveWindow(g_app.hBtnSvcStart, cx, y, bw, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnSvcStart, bw, toolbarH);
        cx += bw + SC(8);

        MoveWindow(g_app.hBtnSvcStop, cx, y, bw, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnSvcStop, bw, toolbarH);
        cx += bw + SC(8);

        MoveWindow(g_app.hBtnSvcRestart, cx, y, bw, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnSvcRestart, bw, toolbarH);
        cx += bw + SC(8);

        MoveWindow(g_app.hBtnSvcRefresh, cx, y, bw, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnSvcRefresh, bw, toolbarH);
    }
    else if (g_app.activeTab == TAB_STARTUP) {
        int bw = SC(104);
        MoveWindow(g_app.hBtnStartupEnable, cx, y, bw, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnStartupEnable, bw, toolbarH);
        cx += bw + SC(8);

        MoveWindow(g_app.hBtnStartupDisable, cx, y, bw, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnStartupDisable, bw, toolbarH);
        cx += bw + SC(8);

        MoveWindow(g_app.hBtnStartupOpenLoc, cx, y, bw, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnStartupOpenLoc, bw, toolbarH);
        cx += bw + SC(8);

        MoveWindow(g_app.hBtnStartupRefresh, cx, y, bw, toolbarH, TRUE);
        MakeButtonRounded(g_app.hBtnStartupRefresh, bw, toolbarH);
    }

    int cpuBarW = SC(130), cpuLabelW = SC(44), cpuCapW = SC(30), gapSmall = SC(8);
    int rightEdge = tabDisplay.right - SC(4);
    int lblX = rightEdge - cpuLabelW;
    int barX = lblX - gapSmall - cpuBarW;
    int capX = barX - gapSmall - cpuCapW;

    MoveWindow(g_app.hCpuCaption, capX, y + SC(6), cpuCapW, toolbarH - SC(8), TRUE);
    MoveWindow(g_app.hCpuBar, barX, y + SC(8), cpuBarW, toolbarH - SC(16), TRUE);
    MoveWindow(g_app.hCpuLabel, lblX, y + SC(6), cpuLabelW, toolbarH - SC(8), TRUE);

    HWND currentList = GetActiveListControl();
    if (currentList) {
        MoveWindow(currentList, tabDisplay.left + SC(4), listY, listW, listH, TRUE);
    }

    if (g_app.activeTab == TAB_PROCESSES) {
        int col0 = SC(180), col1 = SC(70), col2 = SC(80), col3 = SC(90), col4 = SC(100);
        int col5 = listW - (col0 + col1 + col2 + col3 + col4) - SC(8);
        if (col5 < SC(150)) col5 = SC(150);
        ListView_SetColumnWidth(g_app.hList, 0, col0);
        ListView_SetColumnWidth(g_app.hList, 1, col1);
        ListView_SetColumnWidth(g_app.hList, 2, col2);
        ListView_SetColumnWidth(g_app.hList, 3, col3);
        ListView_SetColumnWidth(g_app.hList, 4, col4);
        ListView_SetColumnWidth(g_app.hList, 5, col5);
    }
    else if (g_app.activeTab == TAB_SERVICES) {
        int col0 = SC(150), col2 = SC(90), col3 = SC(100), col4 = SC(60);
        int col5 = SC(180), col6 = SC(220);
        int col1 = listW - (col0 + col2 + col3 + col4 + col5 + col6) - SC(8);
        if (col1 < SC(120)) col1 = SC(120);
        ListView_SetColumnWidth(g_app.hListServices, 0, col0);
        ListView_SetColumnWidth(g_app.hListServices, 1, col1);
        ListView_SetColumnWidth(g_app.hListServices, 2, col2);
        ListView_SetColumnWidth(g_app.hListServices, 3, col3);
        ListView_SetColumnWidth(g_app.hListServices, 4, col4);
        ListView_SetColumnWidth(g_app.hListServices, 5, col5);
        ListView_SetColumnWidth(g_app.hListServices, 6, col6);
    }
    else if (g_app.activeTab == TAB_STARTUP) {
        int col0 = SC(150), col2 = SC(80), col3 = SC(150);
        int col1 = listW - (col0 + col2 + col3) - SC(8);
        if (col1 < SC(200)) col1 = SC(200);
        ListView_SetColumnWidth(g_app.hListStartup, 0, col0);
        ListView_SetColumnWidth(g_app.hListStartup, 1, col1);
        ListView_SetColumnWidth(g_app.hListStartup, 2, col2);
        ListView_SetColumnWidth(g_app.hListStartup, 3, col3);
    }
    else if (g_app.activeTab == TAB_SYSTEM) {
        int col0 = SC(300);
        int col1 = listW - col0 - SC(8);
        if (col1 < SC(200)) col1 = SC(200);
        ListView_SetColumnWidth(g_app.hListSysInfo, 0, col0);
        ListView_SetColumnWidth(g_app.hListSysInfo, 1, col1);
    }

    int footerY = tabDisplay.bottom - footerH;
    MoveWindow(g_app.hLog, tabDisplay.left + SC(10), footerY + SC(9), listW - SC(12), footerH - SC(18), TRUE);

    InvalidateRect(g_app.hwnd, nullptr, TRUE);
}

RECT GetUnionClientRect(const std::vector<HWND>& hwnds, int pad) {
    RECT result{ 1000000, 1000000, -1000000, -1000000 };
    bool any = false;
    for (HWND h : hwnds) {
        if (!h) continue;
        any = true;
        RECT r;
        GetWindowRect(h, &r);
        POINT tl{ r.left, r.top }, br{ r.right, r.bottom };
        ScreenToClient(g_app.hwnd, &tl);
        ScreenToClient(g_app.hwnd, &br);
        result.left   = std::min(result.left,   static_cast<LONG>(tl.x));
        result.top    = std::min(result.top,    static_cast<LONG>(tl.y));
        result.right  = std::max(result.right,  static_cast<LONG>(br.x));
        result.bottom = std::max(result.bottom, static_cast<LONG>(br.y));
    }
    if (!any) return RECT{ 0,0,0,0 };
    result.left   -= pad;
    result.top    -= pad;
    result.right  += pad;
    result.bottom += pad;
    return result;
}

void DrawPanel(HDC hdc, RECT r) {
    HBRUSH br = CreateSolidBrush(Theme::BgPanel);
    FillRect(hdc, &r, br);
    DeleteObject(br);
    DrawFrame(hdc, r, Theme::Border);
}

void DrawFrame(HDC hdc, RECT r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FrameRect(hdc, &r, br);
    DeleteObject(br);
}

void MakeButtonRounded(HWND hBtn, int w, int h) {
    HRGN hRgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, SC(6), SC(6));
    SetWindowRgn(hBtn, hRgn, TRUE);
}

// ============================================================================
//  CUSTOM DRAW / OWNER DRAW
// ============================================================================
LRESULT HandleDrawItem(LPARAM lParam) {
    LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
    if (dis->CtlType != ODT_BUTTON) return FALSE;

    HDC hdc = dis->hDC;
    RECT r = dis->rcItem;
    UINT id = dis->CtlID;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    COLORREF bg = Theme::BgControl;
    COLORREF fg = Theme::TextPrimary;

    if (id == IDC_BTN_ELEVATE) {
        bg = g_app.isElevated ? Theme::AccentDim : Theme::Amber;
        fg = RGB(0, 0, 0);
    } else if (id == IDC_BTN_TERMINATE || id == IDC_BTN_SVC_STOP || id == IDC_BTN_STARTUP_DISABLE) {
        bg = pressed ? RGB(0xA0, 0x30, 0x30) : Theme::Red;
        fg = RGB(0xFF, 0xFF, 0xFF);
    } else if (id == IDC_BTN_SUSPEND) {
        bg = pressed ? RGB(0x90, 0x60, 0x20) : Theme::Amber;
        fg = RGB(0x00, 0x00, 0x00);
    } else if (id == IDC_BTN_RESUME || id == IDC_BTN_SVC_RESTART) {
        bg = pressed ? RGB(0x20, 0x70, 0x90) : Theme::Cyan;
        fg = RGB(0x00, 0x00, 0x00);
    } else if (id == IDC_BTN_SYNC || id == IDC_BTN_SVC_START || id == IDC_BTN_STARTUP_ENABLE ||
               id == IDC_BTN_SVC_REFRESH || id == IDC_BTN_STARTUP_REFRESH) {
        bg = pressed ? Theme::AccentDim : Theme::Accent;
        fg = RGB(0x00, 0x00, 0x00);
    } else {
        if (pressed) bg = Theme::BgListSel;
    }

    HBRUSH hbr = CreateSolidBrush(bg);
    FillRect(hdc, &r, hbr);
    DeleteObject(hbr);

    wchar_t text[128];
    GetWindowTextW(dis->hwndItem, text, 128);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_app.fontUIBold));
    DrawTextW(hdc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);

    return TRUE;
}

LRESULT HandleListCustomDraw(LPARAM lParam) {
    LPNMLVCUSTOMDRAW cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT: {
        HWND hList = cd->nmcd.hdr.hwndFrom;
        int idx = static_cast<int>(cd->nmcd.dwItemSpec);
        bool selected = (ListView_GetItemState(hList, idx, LVIS_SELECTED) & LVIS_SELECTED) != 0;
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
//  PROCESS ENUMERATION + CPU% + PRIORITY
// ============================================================================
void InitNtdllFunctions() {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        g_app.pNtSuspendProcess = reinterpret_cast<NtSuspendProcess_t>(GetProcAddress(hNtdll, "NtSuspendProcess"));
        g_app.pNtResumeProcess  = reinterpret_cast<NtResumeProcess_t>(GetProcAddress(hNtdll, "NtResumeProcess"));
    }
}

const wchar_t* PriorityToString(DWORD pc) {
    switch (pc) {
    case IDLE_PRIORITY_CLASS:         return L"Idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return L"Below Normal";
    case NORMAL_PRIORITY_CLASS:       return L"Normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return L"Above Normal";
    case HIGH_PRIORITY_CLASS:         return L"High";
    case REALTIME_PRIORITY_CLASS:     return L"Realtime";
    }
    return L"Normal";
}

DWORD PriorityMenuIdToClass(UINT id) {
    switch (id) {
    case IDM_PRIO_IDLE:        return IDLE_PRIORITY_CLASS;
    case IDM_PRIO_BELOWNORMAL: return BELOW_NORMAL_PRIORITY_CLASS;
    case IDM_PRIO_NORMAL:      return NORMAL_PRIORITY_CLASS;
    case IDM_PRIO_ABOVENORMAL: return ABOVE_NORMAL_PRIORITY_CLASS;
    case IDM_PRIO_HIGH:        return HIGH_PRIORITY_CLASS;
    case IDM_PRIO_REALTIME:    return REALTIME_PRIORITY_CLASS;
    }
    return NORMAL_PRIORITY_CLASS;
}

void InitCpuMonitor() {
    PdhOpenQueryW(nullptr, 0, &g_app.pdhQuery);
    if (g_app.pdhQuery) {
        PdhAddEnglishCounterW(g_app.pdhQuery, L"\\Processor(_Total)\\% Processor Time", 0, &g_app.pdhCounter);
        PdhCollectQueryData(g_app.pdhQuery);
    }
}

int ReadCpuPercent() {
    if (!g_app.pdhQuery || !g_app.pdhCounter) return -1;
    PDH_FMT_COUNTERVALUE val;
    PdhCollectQueryData(g_app.pdhQuery);
    if (PdhGetFormattedCounterValue(g_app.pdhCounter, PDH_FMT_DOUBLE, nullptr, &val) == ERROR_SUCCESS) {
        return static_cast<int>(val.doubleValue + 0.5);
    }
    return -1;
}

std::vector<ProcessEntry> EnumerateProcesses() {
    std::vector<ProcessEntry> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;

    // Build thread count map once
    std::unordered_map<DWORD, DWORD> threadCounts;
    HANDLE tSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (tSnap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(tSnap, &te)) {
            do {
                threadCounts[te.th32OwnerProcessID]++;
            } while (Thread32Next(tSnap, &te));
        }
        CloseHandle(tSnap);
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    FILETIME fNow;
    GetSystemTimeAsFileTime(&fNow);
    ULONGLONG nowTick = (static_cast<ULONGLONG>(fNow.dwHighDateTime) << 32) | fNow.dwLowDateTime;
    ULONGLONG deltaTick = (nowTick > g_app.cpuPrevTick && g_app.cpuPrevTick > 0) ? (nowTick - g_app.cpuPrevTick) : 0;

    std::unordered_map<DWORD, ULONGLONG> newTimes;

    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;

            ProcessEntry entry{};
            entry.pid = pe.th32ProcessID;
            entry.parentPid = pe.th32ParentProcessID;
            entry.name = pe.szExeFile;
            entry.threads = threadCounts[entry.pid]; // from map

            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.pid);
            if (!hProc) hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.pid);

            if (hProc) {
                wchar_t pBuf[MAX_PATH];
                DWORD sz = MAX_PATH;
                if (QueryFullProcessImageNameW(hProc, 0, pBuf, &sz)) {
                    entry.path = pBuf;
                } else {
                    entry.path = L"System / Access Denied";
                }

                PROCESS_MEMORY_COUNTERS pmc{};
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    entry.workingSetBytes = pmc.WorkingSetSize;
                }

                entry.priorityClass = GetPriorityClass(hProc);

                FILETIME ftCreate, ftExit, ftKernel, ftUser;
                if (GetProcessTimes(hProc, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                    ULONGLONG k = (static_cast<ULONGLONG>(ftKernel.dwHighDateTime) << 32) | ftKernel.dwLowDateTime;
                    ULONGLONG u = (static_cast<ULONGLONG>(ftUser.dwHighDateTime) << 32) | ftUser.dwLowDateTime;
                    ULONGLONG total = k + u;
                    newTimes[entry.pid] = total;

                    if (deltaTick > 0 && g_app.cpuPrevTimes.count(entry.pid)) {
                        ULONGLONG prev = g_app.cpuPrevTimes[entry.pid];
                        ULONGLONG used = (total >= prev) ? (total - prev) : 0;
                        double pct = (static_cast<double>(used) / (static_cast<double>(deltaTick) * g_app.numCores)) * 100.0;
                        entry.cpuPercent = pct < 0.0 ? 0.0 : pct;
                    }
                }
                CloseHandle(hProc);
            }
            result.push_back(std::move(entry));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    g_app.cpuPrevTimes = std::move(newTimes);
    g_app.cpuPrevTick = nowTick;

    std::sort(result.begin(), result.end(), [](const ProcessEntry& a, const ProcessEntry& b) {
        return a.workingSetBytes > b.workingSetBytes;
    });

    return result;
}

bool DoSuspend(DWORD pid) {
    if (!g_app.pNtSuspendProcess) return false;
    ScopedHandle h(OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid));
    if (!h.valid()) return false;
    LONG status = g_app.pNtSuspendProcess(h.get());
    return status == 0;
}

bool DoResume(DWORD pid) {
    if (!g_app.pNtResumeProcess) return false;
    ScopedHandle h(OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid));
    if (!h.valid()) return false;
    LONG status = g_app.pNtResumeProcess(h.get());
    return status == 0;
}

bool DoTerminate(DWORD pid) {
    ScopedHandle h(OpenProcess(PROCESS_TERMINATE, FALSE, pid));
    if (!h.valid()) return false;
    BOOL ok = TerminateProcess(h.get(), 1);
    return ok != 0;
}

void ApplyPriority(DWORD pid, DWORD priorityClass) {
    ScopedHandle h(OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid));
    if (!h.valid()) {
        SetLog(L"[!] Access denied setting priority.", Theme::Red);
        return;
    }
    if (SetPriorityClass(h.get(), priorityClass)) {
        SetLog(L"[>] Priority changed successfully.", Theme::Accent);
        RefreshProcessData();
    } else {
        SetLog(L"[!] Failed to set priority.", Theme::Red);
    }
}

void ToggleAffinityBit(DWORD pid, int coreIndex) {
    ScopedHandle h(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_SET_INFORMATION, FALSE, pid));
    if (!h.valid()) {
        SetLog(L"[!] Access denied setting CPU affinity.", Theme::Red);
        return;
    }
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(h.get(), &processMask, &systemMask)) {
        DWORD_PTR bit = (static_cast<DWORD_PTR>(1) << coreIndex);
        if (processMask & bit) processMask &= ~bit;
        else                   processMask |= bit;

        if (processMask == 0) {
            SetLog(L"[!] Process must have affinity with at least one core.", Theme::Amber);
        } else if (SetProcessAffinityMask(h.get(), processMask)) {
            SetLog(L"[>] CPU affinity updated.", Theme::Accent);
        } else {
            SetLog(L"[!] Failed to set CPU affinity.", Theme::Red);
        }
    }
}

void ShowProcessContextMenu(HWND owner, POINT screenPt, DWORD pid) {
    g_app.contextMenuPid = pid;
    HMENU hMenu = CreatePopupMenu();

    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SUSPEND, L"Suspend Process");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_RESUME, L"Resume Process");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_TERMINATE, L"Kill Process");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    DWORD currentPriority = NORMAL_PRIORITY_CLASS;
    DWORD_PTR processMask = 0, systemMask = 0;

    ScopedHandle hProc(OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid));
    if (hProc.valid()) {
        currentPriority = GetPriorityClass(hProc.get());
        GetProcessAffinityMask(hProc.get(), &processMask, &systemMask);
    }

    HMENU hPrio = CreatePopupMenu();
    struct { UINT id; DWORD cls; const wchar_t* label; } prios[] = {
        { IDM_PRIO_IDLE, IDLE_PRIORITY_CLASS, L"Idle" },
        { IDM_PRIO_BELOWNORMAL, BELOW_NORMAL_PRIORITY_CLASS, L"Below Normal" },
        { IDM_PRIO_NORMAL, NORMAL_PRIORITY_CLASS, L"Normal" },
        { IDM_PRIO_ABOVENORMAL, ABOVE_NORMAL_PRIORITY_CLASS, L"Above Normal" },
        { IDM_PRIO_HIGH, HIGH_PRIORITY_CLASS, L"High" },
        { IDM_PRIO_REALTIME, REALTIME_PRIORITY_CLASS, L"Realtime" },
    };
    for (auto& p : prios) {
        AppendMenuW(hPrio, MF_STRING | (p.cls == currentPriority ? MF_CHECKED : 0), p.id, p.label);
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hPrio), L"Set Priority");

    HMENU hAff = CreatePopupMenu();
    int coresShown = 0;
    for (int i = 0; i < IDM_AFFINITY_MAX_CORES && coresShown < static_cast<int>(g_app.numCores) + 4; i++) {
        DWORD_PTR bit = (static_cast<DWORD_PTR>(1) << i);
        if (!(systemMask & bit)) continue;
        wchar_t label[32];
        swprintf_s(label, 32, L"CPU %d", i);
        AppendMenuW(hAff, MF_STRING | ((processMask & bit) ? MF_CHECKED : 0), IDM_AFFINITY_BASE + i, label);
        coresShown++;
    }
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hAff), L"Set Affinity");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_OPENLOC, L"Open File Location");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_OPENPROC, L"Open Process Folder");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_COPYPATH, L"Copy Path");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_COPYNAME, L"Copy Name");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_PROPERTIES, L"Properties");

    TrackPopupMenu(hMenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0, owner, nullptr);
    DestroyMenu(hMenu);
}

DWORD GetSelectedPid() {
    int idx = ListView_GetNextItem(g_app.hList, -1, LVNI_SELECTED);
    if (idx < 0) return 0;
    if (idx >= 0 && idx < static_cast<int>(g_app.allProcesses.size())) {
        return g_app.allProcesses[idx].pid;
    }
    return 0;
}

void RefreshListDisplay() {
    std::vector<ProcessEntry> filtered;
    for (const auto& p : g_app.allProcesses) {
        if (!g_app.filterLower.empty()) {
            std::wstring nameLower = p.name;
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
            if (nameLower.find(g_app.filterLower) == std::wstring::npos) continue;
        }
        filtered.push_back(p);
    }

    if (g_app.processSortColumn >= 0) {
        int col = g_app.processSortColumn;
        bool asc = g_app.processSortAscending;
        std::sort(filtered.begin(), filtered.end(), [&](const ProcessEntry& a, const ProcessEntry& b) {
            int cmp = 0;
            switch (col) {
            case 0: cmp = _wcsicmp(a.name.c_str(), b.name.c_str()); break;
            case 1: cmp = (a.pid < b.pid) ? -1 : (a.pid > b.pid) ? 1 : 0; break;
            case 2: cmp = (a.cpuPercent < b.cpuPercent) ? -1 : (a.cpuPercent > b.cpuPercent) ? 1 : 0; break;
            case 3: cmp = (a.workingSetBytes < b.workingSetBytes) ? -1 : (a.workingSetBytes > b.workingSetBytes) ? 1 : 0; break;
            case 4: cmp = _wcsicmp(PriorityToString(a.priorityClass), PriorityToString(b.priorityClass)); break;
            case 5: cmp = _wcsicmp(a.path.c_str(), b.path.c_str()); break;
            }
            return asc ? cmp < 0 : cmp > 0;
        });
    }

    ListView_SetItemCountEx(g_app.hList, static_cast<int>(filtered.size()), LVSICF_NOSCROLL);
    g_app.allProcesses = filtered; // Replace with filtered/sorted data
    InvalidateRect(g_app.hList, nullptr, TRUE);
}

void RefreshProcessData() {
    DWORD selectedPid = GetSelectedPid();
    g_app.allProcesses = EnumerateProcesses();
    RefreshListDisplay();

    if (selectedPid) {
        int count = ListView_GetItemCount(g_app.hList);
        for (int i = 0; i < count; i++) {
            if (g_app.allProcesses[i].pid == selectedPid) {
                ListView_SetItemState(g_app.hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }
}

void SetLog(const std::wstring& text, COLORREF color) {
    g_app.logColor = color;
    SetWindowTextW(g_app.hLog, text.c_str());
    InvalidateRect(g_app.hLog, nullptr, TRUE);

    if (g_app.logFile.is_open()) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_s(&tm, &t);
        g_app.logFile << L"[" << std::put_time(&tm, L"%H:%M:%S") << L"] " << text << L"\n";
        g_app.logFile.flush();
    }
}

// ============================================================================
//  SERVICES CORE
// ============================================================================
std::wstring ServiceStateToString(DWORD state) {
    switch (state) {
    case SERVICE_RUNNING:       return L"Running";
    case SERVICE_STOPPED:       return L"Stopped";
    case SERVICE_START_PENDING: return L"Starting...";
    case SERVICE_STOP_PENDING:  return L"Stopping...";
    case SERVICE_PAUSED:        return L"Paused";
    }
    return L"Unknown";
}

std::wstring ServiceStartTypeToString(DWORD startType) {
    switch (startType) {
    case SERVICE_AUTO_START:   return L"Automatic";
    case SERVICE_DEMAND_START: return L"Manual";
    case SERVICE_DISABLED:     return L"Disabled";
    case SERVICE_BOOT_START:   return L"Boot";
    case SERVICE_SYSTEM_START: return L"System";
    }
    return L"Unknown";
}

std::vector<ServiceEntry> EnumerateServices() {
    std::vector<ServiceEntry> result;
    SC_HANDLE hSCM = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!hSCM) return result;

    DWORD bytesNeeded = 0, servicesReturned = 0, resumeHandle = 0;
    EnumServicesStatusExW(hSCM, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
        nullptr, 0, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr);

    if (bytesNeeded > 0) {
        std::vector<BYTE> buffer(bytesNeeded);
        if (EnumServicesStatusExW(hSCM, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buffer.data(), bytesNeeded, &bytesNeeded, &servicesReturned, &resumeHandle, nullptr)) {

            auto services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
            for (DWORD i = 0; i < servicesReturned; i++) {
                ServiceEntry e{};
                e.name = services[i].lpServiceName;
                e.displayName = services[i].lpDisplayName;
                e.state = services[i].ServiceStatusProcess.dwCurrentState;
                e.pid = services[i].ServiceStatusProcess.dwProcessId;

                SC_HANDLE hSvc = OpenServiceW(hSCM, e.name.c_str(), SERVICE_QUERY_CONFIG);
                if (hSvc) {
                    DWORD cfgNeeded = 0;
                    QueryServiceConfigW(hSvc, nullptr, 0, &cfgNeeded);
                    if (cfgNeeded > 0) {
                        std::vector<BYTE> cfgBuf(cfgNeeded);
                        auto cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
                        if (QueryServiceConfigW(hSvc, cfg, cfgNeeded, &cfgNeeded)) {
                            e.startType = cfg->dwStartType;
                            e.binaryPath = cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L"";
                        }
                    }
                    DWORD descNeeded = 0;
                    QueryServiceConfig2W(hSvc, SERVICE_CONFIG_DESCRIPTION, nullptr, 0, &descNeeded);
                    if (descNeeded > 0) {
                        std::vector<BYTE> descBuf(descNeeded);
                        auto desc = reinterpret_cast<SERVICE_DESCRIPTIONW*>(descBuf.data());
                        if (QueryServiceConfig2W(hSvc, SERVICE_CONFIG_DESCRIPTION, descBuf.data(), descNeeded, &descNeeded)) {
                            e.description = desc->lpDescription ? desc->lpDescription : L"";
                        }
                    }
                    CloseServiceHandle(hSvc);
                }
                result.push_back(std::move(e));
            }
        }
    }
    CloseServiceHandle(hSCM);
    return result;
}

int GetSelectedServiceIndex() {
    int idx = ListView_GetNextItem(g_app.hListServices, -1, LVNI_SELECTED);
    if (idx < 0) return -1;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = idx;
    if (!ListView_GetItem(g_app.hListServices, &item)) return -1;
    int dataIdx = static_cast<int>(item.lParam);
    if (dataIdx < 0 || dataIdx >= static_cast<int>(g_app.allServices.size())) return -1;
    return dataIdx;
}

void RefreshServicesList() {
    g_app.allServices = EnumerateServices();
    SendMessageW(g_app.hListServices, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_app.hListServices);

    for (size_t i = 0; i < g_app.allServices.size(); i++) {
        const auto& s = g_app.allServices[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(s.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(g_app.hListServices, &item);

        ListView_SetItemText(g_app.hListServices, static_cast<int>(i), 1, const_cast<LPWSTR>(s.displayName.c_str()));
        ListView_SetItemText(g_app.hListServices, static_cast<int>(i), 2, const_cast<LPWSTR>(ServiceStateToString(s.state).c_str()));
        ListView_SetItemText(g_app.hListServices, static_cast<int>(i), 3, const_cast<LPWSTR>(ServiceStartTypeToString(s.startType).c_str()));

        wchar_t pidBuf[32];
        if (s.pid > 0) swprintf_s(pidBuf, 32, L"%u", s.pid);
        else wcscpy_s(pidBuf, L"-");
        ListView_SetItemText(g_app.hListServices, static_cast<int>(i), 4, pidBuf);
        ListView_SetItemText(g_app.hListServices, static_cast<int>(i), 5, const_cast<LPWSTR>(s.description.c_str()));
        ListView_SetItemText(g_app.hListServices, static_cast<int>(i), 6, const_cast<LPWSTR>(s.binaryPath.c_str()));
    }
    SendMessageW(g_app.hListServices, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_app.hListServices, nullptr, TRUE);
}

bool StartServiceByName(const std::wstring& name) {
    ScopedSC_HANDLE hSCM(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!hSCM.valid()) return false;
    ScopedSC_HANDLE hSvc(OpenServiceW(hSCM.get(), name.c_str(), SERVICE_START));
    if (!hSvc.valid()) return false;
    return StartServiceW(hSvc.get(), 0, nullptr) != 0;
}

bool StopServiceByName(const std::wstring& name) {
    ScopedSC_HANDLE hSCM(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!hSCM.valid()) return false;
    ScopedSC_HANDLE hSvc(OpenServiceW(hSCM.get(), name.c_str(), SERVICE_STOP));
    if (!hSvc.valid()) return false;
    SERVICE_STATUS status{};
    return ControlService(hSvc.get(), SERVICE_CONTROL_STOP, &status) != 0;
}

bool RestartServiceByName(const std::wstring& name) {
    StopServiceByName(name);
    Sleep(500);
    return StartServiceByName(name);
}

// ============================================================================
//  STARTUP ITEMS CORE
// ============================================================================
std::wstring ExtractExecutablePath(const std::wstring& command) {
    if (command.empty()) return L"";
    std::wstring path = command;
    if (path.front() == L'\"') {
        size_t pos = path.find(L'\"', 1);
        if (pos != std::wstring::npos) {
            path = path.substr(1, pos - 1);
        }
    } else {
        size_t pos = path.find(L" ");
        if (pos != std::wstring::npos) {
            path = path.substr(0, pos);
        }
    }
    return path;
}

std::vector<StartupEntry> EnumerateStartupItems() {
    std::vector<StartupEntry> result;

    auto readReg = [&](HKEY root, const wchar_t* subkey, StartupSource source) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t valName[256];
            wchar_t valData[1024];
            DWORD nameSize = 256;
            DWORD dataSize = sizeof(valData);
            DWORD type = 0;
            DWORD index = 0;

            while (RegEnumValueW(hKey, index++, valName, &nameSize, nullptr, &type, reinterpret_cast<LPBYTE>(valData), &dataSize) == ERROR_SUCCESS) {
                if (type == REG_SZ || type == REG_EXPAND_SZ) {
                    StartupEntry e{};
                    e.name = valName;
                    e.command = valData;
                    e.source = source;
                    e.enabled = true;
                    result.push_back(e);
                }
                nameSize = 256;
                dataSize = sizeof(valData);
            }
            RegCloseKey(hKey);
        }
    };

    readReg(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSource::HkcuRun);
    readReg(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", StartupSource::HklmRun);

    wchar_t userStartup[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, 0, userStartup))) {
        WIN32_FIND_DATAW fd;
        std::wstring searchPath = std::wstring(userStartup) + L"\\*";
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                StartupEntry e{};
                e.name = fd.cFileName;
                e.command = std::wstring(userStartup) + L"\\" + fd.cFileName;
                e.source = StartupSource::FolderUser;
                e.enabled = true;
                result.push_back(e);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    wchar_t commonStartup[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_STARTUP, nullptr, 0, commonStartup))) {
        WIN32_FIND_DATAW fd;
        std::wstring searchPath = std::wstring(commonStartup) + L"\\*";
        HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                StartupEntry e{};
                e.name = fd.cFileName;
                e.command = std::wstring(commonStartup) + L"\\" + fd.cFileName;
                e.source = StartupSource::FolderCommon;
                e.enabled = true;
                result.push_back(e);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    return result;
}

int GetSelectedStartupIndex() {
    int idx = ListView_GetNextItem(g_app.hListStartup, -1, LVNI_SELECTED);
    if (idx < 0) return -1;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = idx;
    if (!ListView_GetItem(g_app.hListStartup, &item)) return -1;
    int dataIdx = static_cast<int>(item.lParam);
    if (dataIdx < 0 || dataIdx >= static_cast<int>(g_app.allStartupItems.size())) return -1;
    return dataIdx;
}

void RefreshStartupList() {
    g_app.allStartupItems = EnumerateStartupItems();
    SendMessageW(g_app.hListStartup, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_app.hListStartup);

    for (size_t i = 0; i < g_app.allStartupItems.size(); i++) {
        const auto& s = g_app.allStartupItems[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(s.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(g_app.hListStartup, &item);

        ListView_SetItemText(g_app.hListStartup, static_cast<int>(i), 1, const_cast<LPWSTR>(s.command.c_str()));
        ListView_SetItemText(g_app.hListStartup, static_cast<int>(i), 2, const_cast<LPWSTR>(s.enabled ? L"Enabled" : L"Disabled"));

        const wchar_t* srcStr = L"HKCU Run";
        if (s.source == StartupSource::HklmRun) srcStr = L"HKLM Run";
        else if (s.source == StartupSource::FolderUser) srcStr = L"User Startup Folder";
        else if (s.source == StartupSource::FolderCommon) srcStr = L"Common Startup Folder";
        ListView_SetItemText(g_app.hListStartup, static_cast<int>(i), 3, const_cast<LPWSTR>(srcStr));
    }
    SendMessageW(g_app.hListStartup, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_app.hListStartup, nullptr, TRUE);
}

bool DisableStartupEntry(const StartupEntry& e) {
    if (e.source == StartupSource::FolderUser || e.source == StartupSource::FolderCommon) {
        std::wstring oldPath = e.command;
        std::wstring newPath = oldPath + L".disabled";
        return MoveFileW(oldPath.c_str(), newPath.c_str()) != 0;
    }
    HKEY root = (e.source == StartupSource::HkcuRun) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    ScopedRegKey hKey;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, hKey.put()) == ERROR_SUCCESS) {
        LSTATUS status = RegDeleteValueW(hKey.get(), e.name.c_str());
        return status == ERROR_SUCCESS;
    }
    return false;
}

bool EnableStartupEntry(const StartupEntry& e) {
    if (e.source == StartupSource::FolderUser || e.source == StartupSource::FolderCommon) {
        std::wstring oldPath = e.command;
        if (oldPath.size() > 9 && oldPath.substr(oldPath.size() - 9) == L".disabled") {
            std::wstring newPath = oldPath.substr(0, oldPath.size() - 9);
            return MoveFileW(oldPath.c_str(), newPath.c_str()) != 0;
        }
        return false;
    }
    HKEY root = (e.source == StartupSource::HkcuRun) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
    ScopedRegKey hKey;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, hKey.put()) == ERROR_SUCCESS) {
        LSTATUS status = RegSetValueExW(hKey.get(), e.name.c_str(), 0, REG_SZ,
            reinterpret_cast<const BYTE*>(e.command.c_str()),
            static_cast<DWORD>((e.command.size() + 1) * sizeof(wchar_t)));
        return status == ERROR_SUCCESS;
    }
    return false;
}

// ============================================================================
//  SYSTEM INFO
// ============================================================================
std::vector<SystemInfoEntry> EnumerateSystemInfo() {
    std::vector<SystemInfoEntry> info;

    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&osvi))) {
        std::wstringstream ss;
        ss << L"Windows " << osvi.dwMajorVersion << L"." << osvi.dwMinorVersion
           << L" Build " << osvi.dwBuildNumber;
        info.push_back({ L"Operating System", ss.str() });
    }

    ULONGLONG uptime = GetTickCount64();
    DWORD days = static_cast<DWORD>(uptime / (1000 * 60 * 60 * 24));
    DWORD hours = static_cast<DWORD>((uptime / (1000 * 60 * 60)) % 24);
    DWORD mins = static_cast<DWORD>((uptime / (1000 * 60)) % 60);
    std::wstringstream up;
    up << days << L" days " << hours << L" hours " << mins << L" mins";
    info.push_back({ L"System Uptime", up.str() });

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    std::wstringstream proc;
    proc << si.dwNumberOfProcessors << L" cores, Architecture: ";
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: proc << L"x64"; break;
    case PROCESSOR_ARCHITECTURE_INTEL: proc << L"x86"; break;
    case PROCESSOR_ARCHITECTURE_ARM64: proc << L"ARM64"; break;
    default: proc << L"Unknown";
    }
    info.push_back({ L"Processor", proc.str() });

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        std::wstringstream memStr;
        memStr << L"Total: " << (mem.ullTotalPhys / (1024 * 1024)) << L" MB, "
               << L"Available: " << (mem.ullAvailPhys / (1024 * 1024)) << L" MB ("
               << mem.dwMemoryLoad << L"% used)";
        info.push_back({ L"Physical Memory", memStr.str() });
    }

    wchar_t compName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(compName, &size)) {
        info.push_back({ L"Computer Name", compName });
    }

    wchar_t userName[256];
    DWORD userSize = 256;
    if (GetUserNameW(userName, &userSize)) {
        info.push_back({ L"Current User", userName });
    }

    int width = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    std::wstringstream res;
    res << width << L" x " << height;
    info.push_back({ L"Screen Resolution", res.str() });

    return info;
}

void RefreshSystemInfoList() {
    g_app.allSystemInfo = EnumerateSystemInfo();
    SendMessageW(g_app.hListSysInfo, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_app.hListSysInfo);

    for (size_t i = 0; i < g_app.allSystemInfo.size(); i++) {
        const auto& si = g_app.allSystemInfo[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(si.label.c_str());
        ListView_InsertItem(g_app.hListSysInfo, &item);
        ListView_SetItemText(g_app.hListSysInfo, static_cast<int>(i), 1, const_cast<LPWSTR>(si.value.c_str()));
    }
    SendMessageW(g_app.hListSysInfo, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_app.hListSysInfo, nullptr, TRUE);
}

// ============================================================================
//  UTILITIES & SHELL INTEGRATION
// ============================================================================
void OpenFileLocation(const std::wstring& path) {
    if (path.empty()) return;
    std::wstring arg = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
}

void CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* pMem = GlobalLock(hMem);
        if (pMem) {
            memcpy(pMem, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        }
    }
    CloseClipboard();
}

void ShowFileProperties(const std::wstring& path) {
    if (path.empty()) return;
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.lpVerb = L"properties";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

void OpenProcessFolder(DWORD pid) {
    auto it = std::find_if(g_app.allProcesses.begin(), g_app.allProcesses.end(),
        [&](const ProcessEntry& p) { return p.pid == pid; });
    if (it != g_app.allProcesses.end()) {
        OpenFileLocation(it->path);
    }
}
