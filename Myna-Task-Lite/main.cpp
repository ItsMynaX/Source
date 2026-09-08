// ============================================================================
// MynaTaskLite.cpp
// Lightweight native Windows process manager.
// Dark theme, DPI aware, virtual list, suspend/resume/terminate,
// priority/affinity.
//
// Fixed:
// - LVS_OWNERDATA empty-list bug
// - Refresh ordering
// - Virtual-list data lifetime/indexing
// - Selection preservation after refresh/filter/sort
// - Safer clipboard handling
// - Safer affinity handling
// - Better ListView initialization
// ============================================================================

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

#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <chrono>
#include <iomanip>
#include <cstring>

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
// Control IDs
// ============================================================================

#define IDC_SEARCHBOX        1001
#define IDC_BTN_REFRESH      1002
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

#define ID_TIMER_CPU   2001
#define ID_TIMER_SYNC  2002

#define IDM_CTX_SUSPEND     3000
#define IDM_CTX_RESUME      3001
#define IDM_CTX_TERMINATE   3002
#define IDM_CTX_OPENLOC     3003
#define IDM_CTX_COPYPATH    3004
#define IDM_CTX_PROPERTIES  3005
#define IDM_CTX_COPYNAME    3006

#define IDM_PRIO_IDLE         3100
#define IDM_PRIO_BELOWNORMAL  3101
#define IDM_PRIO_NORMAL       3102
#define IDM_PRIO_ABOVENORMAL  3103
#define IDM_PRIO_HIGH         3104
#define IDM_PRIO_REALTIME     3105

#define IDM_AFFINITY_BASE       3200
#define IDM_AFFINITY_MAX_CORES  64

// ============================================================================
// NT functions
// ============================================================================

typedef LONG (NTAPI *NtSuspendProcess_t)(HANDLE);
typedef LONG (NTAPI *NtResumeProcess_t)(HANDLE);

// ============================================================================
// Theme
// ============================================================================

namespace Theme
{
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
// Data structures
// ============================================================================

struct ProcessEntry
{
    DWORD pid = 0;
    std::wstring name;
    std::wstring path;

    ULONGLONG workingSetBytes = 0;
    double cpuPercent = -1.0;

    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    DWORD threads = 0;
};

struct AppState
{
    HWND hwnd = nullptr;

    HWND hSearch = nullptr;
    HWND hBtnRefresh = nullptr;
    HWND hChkAuto = nullptr;

    HWND hBtnSuspend = nullptr;
    HWND hBtnResume = nullptr;
    HWND hBtnTerminate = nullptr;

    HWND hList = nullptr;

    HWND hCpuBar = nullptr;
    HWND hCpuLabel = nullptr;
    HWND hCpuCaption = nullptr;

    HWND hLog = nullptr;
    HWND hBtnElevate = nullptr;

    HFONT fontTitle = nullptr;
    HFONT fontSubtitle = nullptr;
    HFONT fontUI = nullptr;
    HFONT fontUIBold = nullptr;
    HFONT fontMono = nullptr;

    HBRUSH hbrBgWindow = nullptr;
    HBRUSH hbrBgPanel = nullptr;
    HBRUSH hbrBgControl = nullptr;

    HBRUSH hbrListRow = nullptr;
    HBRUSH hbrListRowAlt = nullptr;
    HBRUSH hbrListSel = nullptr;

    UINT dpi = 96;
    double scale = 1.0;

    // IMPORTANT:
    // This is always the exact dataset currently backing LVS_OWNERDATA.
    std::vector<ProcessEntry> allProcesses;

    std::wstring filterLower;

    COLORREF logColor = Theme::Accent;

    NtSuspendProcess_t pNtSuspendProcess = nullptr;
    NtResumeProcess_t  pNtResumeProcess  = nullptr;

    PDH_HQUERY   pdhQuery = nullptr;
    PDH_HCOUNTER pdhCounter = nullptr;

    std::unordered_map<DWORD, ULONGLONG> cpuPrevTimes;
    ULONGLONG cpuPrevTick = 0;

    DWORD numCores = 1;

    bool isElevated = false;
    DWORD contextMenuPid = 0;

    int sortColumn = -1;
    bool sortAscending = true;
};

static AppState g_app;

// ============================================================================
// Helpers
// ============================================================================

inline int SC(int v)
{
    return static_cast<int>(
        v * g_app.scale + (v >= 0 ? 0.5 : -0.5)
    );
}

// ============================================================================
// Forward declarations
// ============================================================================

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void EnableHighDpiAwareness();
void ApplyDarkTitleBar(HWND);
void ApplyRoundedCorners(HWND);
void TryEnableMica(HWND);

void CreateFontsForDpi(UINT);
void DeleteAppFonts();

void InitGdiResources();
void CleanupGdiResources();

void CreateControls(HWND);
void RelayoutControls();
void MakeButtonRounded(HWND, int, int);

BOOL IsProcessElevated();
void RelaunchAsAdmin();

void InitNtdllFunctions();

void InitCpuMonitor();
int ReadCpuPercent();

const wchar_t* PriorityToString(DWORD);
DWORD PriorityMenuIdToClass(UINT);

std::vector<ProcessEntry> EnumerateProcesses();

bool DoSuspend(DWORD);
bool DoResume(DWORD);
bool DoTerminate(DWORD);

void ApplyPriority(DWORD, DWORD);
void ToggleAffinityBit(DWORD, int);

void ShowProcessContextMenu(HWND, POINT, DWORD);

void RefreshProcessData();
void RefreshListDisplay();

DWORD GetSelectedPid();

void SetLog(const std::wstring&, COLORREF);

void OpenFileLocation(const std::wstring&);
void CopyTextToClipboard(HWND, const std::wstring&);
void ShowFileProperties(const std::wstring&);

LRESULT HandleDrawItem(LPARAM);
LRESULT HandleListCustomDraw(LPARAM);

// ============================================================================
// ScopedHandle
// ============================================================================

class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE h = nullptr)
        : h_(h)
    {
    }

    ~ScopedHandle()
    {
        close();
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : h_(other.h_)
    {
        other.h_ = nullptr;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other)
        {
            close();
            h_ = other.h_;
            other.h_ = nullptr;
        }

        return *this;
    }

    HANDLE get() const
    {
        return h_;
    }

    bool valid() const
    {
        return h_ &&
               h_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE h = nullptr)
    {
        close();
        h_ = h;
    }

private:
    void close()
    {
        if (h_ && h_ != INVALID_HANDLE_VALUE)
            CloseHandle(h_);

        h_ = nullptr;
    }

    HANDLE h_ = nullptr;
};

// ============================================================================
// Entry point
// ============================================================================

int WINAPI wWinMain(
    HINSTANCE hInst,
    HINSTANCE,
    LPWSTR,
    int nCmdShow)
{
    EnableHighDpiAwareness();

    INITCOMMONCONTROLSEX icc{
        sizeof(icc),
        ICC_LISTVIEW_CLASSES |
        ICC_PROGRESS_CLASS |
        ICC_STANDARD_CLASSES |
        ICC_BAR_CLASSES
    };

    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"MynaTaskLiteWindowClass";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc))
        return 0;

    g_app.dpi = GetDpiForSystem();
    g_app.scale = g_app.dpi / 96.0;
    g_app.isElevated = IsProcessElevated();

    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    g_app.numCores =
        si.dwNumberOfProcessors > 0
        ? si.dwNumberOfProcessors
        : 1;

    int w = SC(1000);
    int h = SC(650);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Myna Task Lite",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        w,
        h,
        nullptr,
        nullptr,
        hInst,
        nullptr
    );

    if (!hwnd)
        return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};

    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}

#ifndef _MSC_VER

extern "C"
int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPSTR,
    int nCmdShow)
{
    return wWinMain(
        hInstance,
        hPrevInstance,
        GetCommandLineW(),
        nCmdShow
    );
}

#endif

// ============================================================================
// Window procedure
// ============================================================================

LRESULT CALLBACK WndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
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

        if (!g_app.isElevated)
        {
            SetLog(
                L"[!] Running without administrator rights.",
                Theme::Amber
            );
        }

        SetTimer(hwnd, ID_TIMER_CPU, 1000, nullptr);
        SetTimer(hwnd, ID_TIMER_SYNC, 2000, nullptr);

        return 0;
    }

    case WM_GETMINMAXINFO:
    {
        auto* mmi =
            reinterpret_cast<MINMAXINFO*>(lParam);

        mmi->ptMinTrackSize.x = SC(700);
        mmi->ptMinTrackSize.y = SC(400);

        return 0;
    }

    case WM_SIZE:
        RelayoutControls();
        return 0;

    case WM_DPICHANGED:
    {
        g_app.dpi = HIWORD(wParam);
        g_app.scale = g_app.dpi / 96.0;

        CreateFontsForDpi(g_app.dpi);

        RECT* suggested =
            reinterpret_cast<RECT*>(lParam);

        SetWindowPos(
            hwnd,
            nullptr,
            suggested->left,
            suggested->top,
            suggested->right - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE
        );

        RelayoutControls();

        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};

        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc{};
        GetClientRect(hwnd, &rc);

        FillRect(
            hdc,
            &rc,
            g_app.hbrBgWindow
        );

        int margin = SC(16);
        int headerH = SC(50);

        SetBkMode(hdc, TRANSPARENT);

        HFONT oldFont =
            static_cast<HFONT>(
                SelectObject(
                    hdc,
                    g_app.fontTitle
                )
            );

        SetTextColor(
            hdc,
            Theme::Accent
        );

        RECT titleRect{
            margin,
            margin,
            rc.right - margin,
            margin + SC(28)
        };

        DrawTextW(
            hdc,
            L"MYNA TASK LITE",
            -1,
            &titleRect,
            DT_LEFT |
            DT_TOP |
            DT_SINGLELINE
        );

        SelectObject(
            hdc,
            g_app.fontSubtitle
        );

        SetTextColor(
            hdc,
            Theme::TextMuted
        );

        RECT subRect{
            margin,
            margin + SC(28),
            rc.right - margin,
            margin + headerH
        };

        DrawTextW(
            hdc,
            L"LIGHTWEIGHT PROCESS MANAGER",
            -1,
            &subRect,
            DT_LEFT |
            DT_TOP |
            DT_SINGLELINE
        );

        SelectObject(hdc, oldFont);

        HBRUSH accentBrush =
            CreateSolidBrush(Theme::Accent);

        RECT underline{
            margin,
            margin + SC(33),
            margin + SC(120),
            margin + SC(36)
        };

        FillRect(
            hdc,
            &underline,
            accentBrush
        );

        DeleteObject(accentBrush);

        EndPaint(hwnd, &ps);

        return 0;
    }

    case WM_CTLCOLORSTATIC:
    {
        HDC hdc =
            reinterpret_cast<HDC>(wParam);

        HWND ctrl =
            reinterpret_cast<HWND>(lParam);

        SetBkMode(hdc, TRANSPARENT);

        if (ctrl == g_app.hLog)
        {
            SetTextColor(
                hdc,
                g_app.logColor
            );
        }
        else if (ctrl == g_app.hCpuLabel)
        {
            SetTextColor(
                hdc,
                Theme::Accent
            );
        }
        else if (ctrl == g_app.hCpuCaption)
        {
            SetTextColor(
                hdc,
                Theme::TextMuted
            );
        }
        else
        {
            SetTextColor(
                hdc,
                Theme::TextPrimary
            );
        }

        return reinterpret_cast<LRESULT>(
            g_app.hbrBgPanel
        );
    }

    case WM_CTLCOLORBTN:
    {
        HDC hdc =
            reinterpret_cast<HDC>(wParam);

        SetBkMode(
            hdc,
            TRANSPARENT
        );

        SetTextColor(
            hdc,
            Theme::TextPrimary
        );

        return reinterpret_cast<LRESULT>(
            g_app.hbrBgPanel
        );
    }

    case WM_CTLCOLOREDIT:
    {
        HDC hdc =
            reinterpret_cast<HDC>(wParam);

        SetBkMode(hdc, OPAQUE);

        SetBkColor(
            hdc,
            Theme::BgControl
        );

        SetTextColor(
            hdc,
            Theme::TextPrimary
        );

        return reinterpret_cast<LRESULT>(
            g_app.hbrBgControl
        );
    }

    case WM_SETCURSOR:
    {
        HWND target =
            reinterpret_cast<HWND>(wParam);

        if (target == g_app.hBtnRefresh ||
            target == g_app.hBtnSuspend ||
            target == g_app.hBtnResume ||
            target == g_app.hBtnTerminate ||
            target == g_app.hBtnElevate)
        {
            SetCursor(
                LoadCursorW(
                    nullptr,
                    IDC_HAND
                )
            );

            return TRUE;
        }

        break;
    }

    case WM_DRAWITEM:
        return HandleDrawItem(lParam);

    case WM_CONTEXTMENU:
    {
        if (
            reinterpret_cast<HWND>(wParam)
            == g_app.hList
        )
        {
            POINT pt{
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)
            };

            if (pt.x == -1 && pt.y == -1)
            {
                int idx =
                    ListView_GetNextItem(
                        g_app.hList,
                        -1,
                        LVNI_SELECTED
                    );

                if (idx < 0)
                    return 0;

                RECT r{};

                ListView_GetItemRect(
                    g_app.hList,
                    idx,
                    &r,
                    LVIR_BOUNDS
                );

                pt.x = r.left;
                pt.y = r.bottom;

                ClientToScreen(
                    g_app.hList,
                    &pt
                );
            }

            DWORD pid = GetSelectedPid();

            if (pid)
                ShowProcessContextMenu(
                    hwnd,
                    pt,
                    pid
                );

            return 0;
        }

        break;
    }

    case WM_NOTIFY:
    {
        LPNMHDR hdr =
            reinterpret_cast<LPNMHDR>(lParam);

        // ------------------------------------------------------------
        // List custom drawing
        // ------------------------------------------------------------

        if (
            hdr->hwndFrom == g_app.hList &&
            hdr->code == NM_CUSTOMDRAW
        )
        {
            return HandleListCustomDraw(lParam);
        }

        // ------------------------------------------------------------
        // Column click
        // ------------------------------------------------------------

        if (
            hdr->hwndFrom == g_app.hList &&
            hdr->code == LVN_COLUMNCLICK
        )
        {
            auto* pnmv =
                reinterpret_cast<NMLISTVIEW*>(lParam);

            int col = pnmv->iSubItem;

            if (g_app.sortColumn == col)
            {
                g_app.sortAscending =
                    !g_app.sortAscending;
            }
            else
            {
                g_app.sortColumn = col;
                g_app.sortAscending = true;
            }

            RefreshListDisplay();

            return 0;
        }

        // ------------------------------------------------------------
        // Virtual ListView text provider
        // ------------------------------------------------------------

        if (
            hdr->hwndFrom == g_app.hList &&
            hdr->code == LVN_GETDISPINFO
        )
        {
            auto* pdi =
                reinterpret_cast<NMLVDISPINFO*>(lParam);

            int idx =
                pdi->item.iItem;

            if (
                idx >= 0 &&
                idx <
                static_cast<int>(
                    g_app.allProcesses.size()
                )
            )
            {
                const ProcessEntry& p =
                    g_app.allProcesses[
                        static_cast<size_t>(idx)
                    ];

                switch (pdi->item.iSubItem)
                {
                case 0:
                    pdi->item.pszText =
                        const_cast<LPWSTR>(
                            p.name.c_str()
                        );
                    break;

                case 1:
                {
                    static thread_local wchar_t buf[32];

                    swprintf_s(
                        buf,
                        _countof(buf),
                        L"%u",
                        p.pid
                    );

                    pdi->item.pszText = buf;
                    break;
                }

                case 2:
                {
                    static thread_local wchar_t buf[32];

                    if (p.cpuPercent >= 0.0)
                    {
                        swprintf_s(
                            buf,
                            _countof(buf),
                            L"%.1f%%",
                            p.cpuPercent
                        );
                    }
                    else
                    {
                        wcscpy_s(
                            buf,
                            _countof(buf),
                            L"-"
                        );
                    }

                    pdi->item.pszText = buf;
                    break;
                }

                case 3:
                {
                    static thread_local wchar_t buf[32];

                    double mb =
                        p.workingSetBytes /
                        (1024.0 * 1024.0);

                    swprintf_s(
                        buf,
                        _countof(buf),
                        L"%.1f MB",
                        mb
                    );

                    pdi->item.pszText = buf;
                    break;
                }

                case 4:
                    pdi->item.pszText =
                        const_cast<LPWSTR>(
                            PriorityToString(
                                p.priorityClass
                            )
                        );
                    break;

                default:
                    pdi->item.pszText =
                        const_cast<LPWSTR>(L"");
                    break;
                }
            }

            return 0;
        }

        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        // ------------------------------------------------------------
        // Search
        // ------------------------------------------------------------

        if (
            id == IDC_SEARCHBOX &&
            HIWORD(wParam) == EN_CHANGE
        )
        {
            wchar_t buf[256]{};

            GetWindowTextW(
                g_app.hSearch,
                buf,
                _countof(buf)
            );

            g_app.filterLower = buf;

            std::transform(
                g_app.filterLower.begin(),
                g_app.filterLower.end(),
                g_app.filterLower.begin(),
                [](wchar_t ch)
                {
                    return static_cast<wchar_t>(
                        std::towlower(ch)
                    );
                }
            );

            RefreshListDisplay();
        }

        // ------------------------------------------------------------
        // Refresh
        // ------------------------------------------------------------

        else if (id == IDC_BTN_REFRESH)
        {
            RefreshProcessData();
        }

        // ------------------------------------------------------------
        // Suspend
        // ------------------------------------------------------------

        else if (id == IDC_BTN_SUSPEND)
        {
            DWORD pid = GetSelectedPid();

            if (pid)
            {
                if (DoSuspend(pid))
                {
                    SetLog(
                        L"[>] Suspended PID " +
                        std::to_wstring(pid),
                        Theme::Amber
                    );
                }
                else
                {
                    SetLog(
                        L"[!] Access denied / failed",
                        Theme::Red
                    );
                }
            }
        }

        // ------------------------------------------------------------
        // Resume
        // ------------------------------------------------------------

        else if (id == IDC_BTN_RESUME)
        {
            DWORD pid = GetSelectedPid();

            if (pid)
            {
                if (DoResume(pid))
                {
                    SetLog(
                        L"[>] Resumed PID " +
                        std::to_wstring(pid),
                        Theme::Cyan
                    );
                }
                else
                {
                    SetLog(
                        L"[!] Access denied / failed",
                        Theme::Red
                    );
                }
            }
        }

        // ------------------------------------------------------------
        // Terminate
        // ------------------------------------------------------------

        else if (id == IDC_BTN_TERMINATE)
        {
            DWORD pid = GetSelectedPid();

            if (pid)
            {
                if (DoTerminate(pid))
                {
                    SetLog(
                        L"[X] Terminated PID " +
                        std::to_wstring(pid),
                        Theme::Red
                    );

                    RefreshProcessData();
                }
                else
                {
                    SetLog(
                        L"[!] Failed",
                        Theme::Red
                    );
                }
            }
        }

        // ------------------------------------------------------------
        // Elevate
        // ------------------------------------------------------------

        else if (id == IDC_BTN_ELEVATE)
        {
            RelaunchAsAdmin();
        }

        // ------------------------------------------------------------
        // Context: Suspend
        // ------------------------------------------------------------

        else if (id == IDM_CTX_SUSPEND)
        {
            if (
                g_app.contextMenuPid &&
                DoSuspend(g_app.contextMenuPid)
            )
            {
                SetLog(
                    L"[>] Suspended",
                    Theme::Amber
                );
            }
            else
            {
                SetLog(
                    L"[!] Suspend failed",
                    Theme::Red
                );
            }
        }

        // ------------------------------------------------------------
        // Context: Resume
        // ------------------------------------------------------------

        else if (id == IDM_CTX_RESUME)
        {
            if (
                g_app.contextMenuPid &&
                DoResume(g_app.contextMenuPid)
            )
            {
                SetLog(
                    L"[>] Resumed",
                    Theme::Cyan
                );
            }
            else
            {
                SetLog(
                    L"[!] Resume failed",
                    Theme::Red
                );
            }
        }

        // ------------------------------------------------------------
        // Context: Kill
        // ------------------------------------------------------------

        else if (id == IDM_CTX_TERMINATE)
        {
            if (
                g_app.contextMenuPid &&
                DoTerminate(g_app.contextMenuPid)
            )
            {
                SetLog(
                    L"[X] Terminated",
                    Theme::Red
                );

                RefreshProcessData();
            }
            else
            {
                SetLog(
                    L"[!] Terminate failed",
                    Theme::Red
                );
            }
        }

        // ------------------------------------------------------------
        // Context: Open location
        // ------------------------------------------------------------

        else if (id == IDM_CTX_OPENLOC)
        {
            auto it =
                std::find_if(
                    g_app.allProcesses.begin(),
                    g_app.allProcesses.end(),
                    [](const ProcessEntry& p)
                    {
                        return p.pid ==
                            g_app.contextMenuPid;
                    }
                );

            if (it != g_app.allProcesses.end())
                OpenFileLocation(it->path);
        }

        // ------------------------------------------------------------
        // Context: Copy path
        // ------------------------------------------------------------

        else if (id == IDM_CTX_COPYPATH)
        {
            auto it =
                std::find_if(
                    g_app.allProcesses.begin(),
                    g_app.allProcesses.end(),
                    [](const ProcessEntry& p)
                    {
                        return p.pid ==
                            g_app.contextMenuPid;
                    }
                );

            if (it != g_app.allProcesses.end())
                CopyTextToClipboard(
                    hwnd,
                    it->path
                );
        }

        // ------------------------------------------------------------
        // Context: Properties
        // ------------------------------------------------------------

        else if (id == IDM_CTX_PROPERTIES)
        {
            auto it =
                std::find_if(
                    g_app.allProcesses.begin(),
                    g_app.allProcesses.end(),
                    [](const ProcessEntry& p)
                    {
                        return p.pid ==
                            g_app.contextMenuPid;
                    }
                );

            if (it != g_app.allProcesses.end())
                ShowFileProperties(it->path);
        }

        // ------------------------------------------------------------
        // Context: Copy name
        // ------------------------------------------------------------

        else if (id == IDM_CTX_COPYNAME)
        {
            auto it =
                std::find_if(
                    g_app.allProcesses.begin(),
                    g_app.allProcesses.end(),
                    [](const ProcessEntry& p)
                    {
                        return p.pid ==
                            g_app.contextMenuPid;
                    }
                );

            if (it != g_app.allProcesses.end())
                CopyTextToClipboard(
                    hwnd,
                    it->name
                );
        }

        // ------------------------------------------------------------
        // Priority
        // ------------------------------------------------------------

        else if (
            id >= IDM_PRIO_IDLE &&
            id <= IDM_PRIO_REALTIME
        )
        {
            if (g_app.contextMenuPid)
            {
                ApplyPriority(
                    g_app.contextMenuPid,
                    PriorityMenuIdToClass(
                        static_cast<UINT>(id)
                    )
                );

                RefreshProcessData();
            }
        }

        // ------------------------------------------------------------
        // Affinity
        // ------------------------------------------------------------

        else if (
            id >= IDM_AFFINITY_BASE &&
            id <
            IDM_AFFINITY_BASE +
            IDM_AFFINITY_MAX_CORES
        )
        {
            if (g_app.contextMenuPid)
            {
                ToggleAffinityBit(
                    g_app.contextMenuPid,
                    id - IDM_AFFINITY_BASE
                );

                RefreshProcessData();
            }
        }

        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == ID_TIMER_CPU)
        {
            int pct = ReadCpuPercent();

            if (pct >= 0)
            {
                if (pct > 100)
                    pct = 100;

                SendMessageW(
                    g_app.hCpuBar,
                    PBM_SETPOS,
                    pct,
                    0
                );

                std::wstring text =
                    std::to_wstring(pct) + L"%";

                SetWindowTextW(
                    g_app.hCpuLabel,
                    text.c_str()
                );
            }
        }
        else if (wParam == ID_TIMER_SYNC)
        {
            if (
                IsDlgButtonChecked(
                    hwnd,
                    IDC_CHK_AUTOSYNC
                ) == BST_CHECKED &&
                GetFocus() != g_app.hSearch
            )
            {
                RefreshProcessData();
            }
        }

        return 0;
    }

    case WM_DESTROY:
    {
        KillTimer(hwnd, ID_TIMER_CPU);
        KillTimer(hwnd, ID_TIMER_SYNC);

        if (g_app.pdhQuery)
        {
            PdhCloseQuery(g_app.pdhQuery);
            g_app.pdhQuery = nullptr;
            g_app.pdhCounter = nullptr;
        }

        DeleteAppFonts();
        CleanupGdiResources();

        PostQuitMessage(0);

        return 0;
    }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam
    );
}

// ============================================================================
// DPI
// ============================================================================

void EnableHighDpiAwareness()
{
    HMODULE hUser32 =
        GetModuleHandleW(L"user32.dll");

    if (hUser32)
    {
        using SetProcessDpiAwarenessContext_t =
            BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);

        auto pSetDpi =
            reinterpret_cast<
                SetProcessDpiAwarenessContext_t
            >(
                GetProcAddress(
                    hUser32,
                    "SetProcessDpiAwarenessContext"
                )
            );

        if (pSetDpi)
        {
            pSetDpi(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
            );

            return;
        }
    }

    HMODULE hShcore =
        LoadLibraryW(L"shcore.dll");

    if (hShcore)
    {
        using SetProcessDpiAwareness_t =
            HRESULT (WINAPI*)(int);

        auto pSetDpi =
            reinterpret_cast<
                SetProcessDpiAwareness_t
            >(
                GetProcAddress(
                    hShcore,
                    "SetProcessDpiAwareness"
                )
            );

        if (pSetDpi)
            pSetDpi(2);

        FreeLibrary(hShcore);
    }
}

// ============================================================================
// DWM
// ============================================================================

void ApplyDarkTitleBar(HWND hwnd)
{
    BOOL dark = TRUE;

    DwmSetWindowAttribute(
        hwnd,
        DWMWA_USE_IMMERSIVE_DARK_MODE,
        &dark,
        sizeof(dark)
    );
}

void ApplyRoundedCorners(HWND hwnd)
{
    DWORD pref = 2;

    DwmSetWindowAttribute(
        hwnd,
        DWMWA_WINDOW_CORNER_PREFERENCE,
        &pref,
        sizeof(pref)
    );
}

void TryEnableMica(HWND hwnd)
{
    DWORD type = 2;

    DwmSetWindowAttribute(
        hwnd,
        DWMWA_SYSTEMBACKDROP_TYPE,
        &type,
        sizeof(type)
    );
}

// ============================================================================
// Elevation
// ============================================================================

BOOL IsProcessElevated()
{
    BOOL elevated = FALSE;

    HANDLE hToken = nullptr;

    if (
        OpenProcessToken(
            GetCurrentProcess(),
            TOKEN_QUERY,
            &hToken
        )
    )
    {
        TOKEN_ELEVATION elev{};
        DWORD size = sizeof(elev);

        if (
            GetTokenInformation(
                hToken,
                TokenElevation,
                &elev,
                sizeof(elev),
                &size
            )
        )
        {
            elevated =
                elev.TokenIsElevated;
        }

        CloseHandle(hToken);
    }

    return elevated;
}

void RelaunchAsAdmin()
{
    wchar_t path[MAX_PATH]{};

    if (
        GetModuleFileNameW(
            nullptr,
            path,
            _countof(path)
        )
    )
    {
        SHELLEXECUTEINFOW sei{
            sizeof(sei)
        };

        sei.lpVerb = L"runas";
        sei.lpFile = path;
        sei.hwnd = g_app.hwnd;
        sei.nShow = SW_NORMAL;

        if (ShellExecuteExW(&sei))
            PostQuitMessage(0);
    }
}

// ============================================================================
// GDI resources
// ============================================================================

void InitGdiResources()
{
    g_app.hbrBgWindow =
        CreateSolidBrush(
            Theme::BgWindow
        );

    g_app.hbrBgPanel =
        CreateSolidBrush(
            Theme::BgPanel
        );

    g_app.hbrBgControl =
        CreateSolidBrush(
            Theme::BgControl
        );

    g_app.hbrListRow =
        CreateSolidBrush(
            Theme::BgListRow
        );

    g_app.hbrListRowAlt =
        CreateSolidBrush(
            Theme::BgListRowAlt
        );

    g_app.hbrListSel =
        CreateSolidBrush(
            Theme::BgListSel
        );
}

void CleanupGdiResources()
{
    if (g_app.hbrBgWindow)
    {
        DeleteObject(
            g_app.hbrBgWindow
        );

        g_app.hbrBgWindow = nullptr;
    }

    if (g_app.hbrBgPanel)
    {
        DeleteObject(
            g_app.hbrBgPanel
        );

        g_app.hbrBgPanel = nullptr;
    }

    if (g_app.hbrBgControl)
    {
        DeleteObject(
            g_app.hbrBgControl
        );

        g_app.hbrBgControl = nullptr;
    }

    if (g_app.hbrListRow)
    {
        DeleteObject(
            g_app.hbrListRow
        );

        g_app.hbrListRow = nullptr;
    }

    if (g_app.hbrListRowAlt)
    {
        DeleteObject(
            g_app.hbrListRowAlt
        );

        g_app.hbrListRowAlt = nullptr;
    }

    if (g_app.hbrListSel)
    {
        DeleteObject(
            g_app.hbrListSel
        );

        g_app.hbrListSel = nullptr;
    }
}

// ============================================================================
// Fonts
// ============================================================================

void DeleteAppFonts()
{
    if (g_app.fontTitle)
    {
        DeleteObject(g_app.fontTitle);
        g_app.fontTitle = nullptr;
    }

    if (g_app.fontSubtitle)
    {
        DeleteObject(g_app.fontSubtitle);
        g_app.fontSubtitle = nullptr;
    }

    if (g_app.fontUI)
    {
        DeleteObject(g_app.fontUI);
        g_app.fontUI = nullptr;
    }

    if (g_app.fontUIBold)
    {
        DeleteObject(g_app.fontUIBold);
        g_app.fontUIBold = nullptr;
    }

    if (g_app.fontMono)
    {
        DeleteObject(g_app.fontMono);
        g_app.fontMono = nullptr;
    }
}

void CreateFontsForDpi(UINT dpi)
{
    DeleteAppFonts();

    auto mk =
        [&](int pt,
            int weight,
            const wchar_t* face)
        {
            int h =
                -MulDiv(
                    pt,
                    static_cast<int>(dpi),
                    72
                );

            return CreateFontW(
                h,
                0,
                0,
                0,
                weight,
                FALSE,
                FALSE,
                FALSE,
                DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS,
                CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                DEFAULT_PITCH |
                FF_DONTCARE,
                face
            );
        };

    g_app.fontTitle =
        mk(16, FW_BOLD, L"Segoe UI");

    g_app.fontSubtitle =
        mk(9, FW_SEMIBOLD, L"Segoe UI");

    g_app.fontUI =
        mk(9, FW_NORMAL, L"Segoe UI");

    g_app.fontUIBold =
        mk(9, FW_BOLD, L"Segoe UI");

    g_app.fontMono =
        mk(9, FW_NORMAL, L"Consolas");
}

// ============================================================================
// Controls
// ============================================================================

void CreateControls(HWND hwnd)
{
    HINSTANCE hInst =
        GetModuleHandleW(nullptr);

    g_app.hBtnElevate =
        CreateWindowExW(
            0,
            L"BUTTON",
            L"ADMIN",
            WS_CHILD |
            WS_VISIBLE |
            BS_OWNERDRAW,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(
                IDC_BTN_ELEVATE
            ),
            hInst,
            nullptr
        );

    g_app.hCpuCaption =
        CreateWindowExW(
            0,
            L"STATIC",
            L"CPU",
            WS_CHILD |
            WS_VISIBLE |
            SS_RIGHT,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(
                IDC_CPU_CAPTION
            ),
            hInst,
            nullptr
        );

    g_app.hCpuBar =
        CreateWindowExW(
            0,
            PROGRESS_CLASSW,
            L"",
            WS_CHILD |
            WS_VISIBLE |
            PBS_SMOOTH,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(
                IDC_CPU_BAR
            ),
            hInst,
            nullptr
        );

    SetWindowTheme(
        g_app.hCpuBar,
        L"",
        L""
    );

    SendMessageW(
        g_app.hCpuBar,
        PBM_SETRANGE,
        0,
        MAKELPARAM(0, 100)
    );

    SendMessageW(
        g_app.hCpuBar,
        PBM_SETBARCOLOR,
        0,
        static_cast<LPARAM>(
            Theme::Accent
        )
    );

    SendMessageW(
        g_app.hCpuBar,
        PBM_SETBKCOLOR,
        0,
        static_cast<LPARAM>(
            Theme::BgControl
        )
    );

    g_app.hCpuLabel =
        CreateWindowExW(
            0,
            L"STATIC",
            L"0%",
            WS_CHILD |
            WS_VISIBLE |
            SS_LEFT,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(
                IDC_CPU_LABEL
            ),
            hInst,
            nullptr
        );

    g_app.hSearch =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD |
            WS_VISIBLE |
            ES_AUTOHSCROLL,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(
                IDC_SEARCHBOX
            ),
            hInst,
            nullptr
        );

    SetWindowTheme(
        g_app.hSearch,
        L"DarkMode_CFD",
        nullptr
    );

    SendMessageW(
        g_app.hSearch,
        EM_SETCUEBANNER,
        TRUE,
        reinterpret_cast<LPARAM>(
            L"Search..."
        )
    );

    auto mkBtn =
        [&](const wchar_t* text,
            int id)
        {
            return CreateWindowExW(
                0,
                L"BUTTON",
                text,
                WS_CHILD |
                WS_VISIBLE |
                BS_OWNERDRAW,
                0,
                0,
                0,
                0,
                hwnd,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(id)
                ),
                hInst,
                nullptr
            );
        };

    g_app.hBtnRefresh =
        mkBtn(
            L"REFRESH",
            IDC_BTN_REFRESH
        );

    g_app.hChkAuto =
        CreateWindowExW(
            0,
            L"BUTTON",
            L"Auto",
            WS_CHILD |
            WS_VISIBLE |
            BS_AUTOCHECKBOX,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(
                IDC_CHK_AUTOSYNC
            ),
            hInst,
            nullptr
        );

    Button_SetCheck(
        g_app.hChkAuto,
        BST_CHECKED
    );

    g_app.hBtnSuspend =
        mkBtn(
            L"SUSPEND",
            IDC_BTN_SUSPEND
        );

    g_app.hBtnResume =
        mkBtn(
            L"RESUME",
            IDC_BTN_RESUME
        );

    g_app.hBtnTerminate =
        mkBtn(
            L"KILL",
            IDC_BTN_TERMINATE
        );

    // ------------------------------------------------------------
    // Virtual ListView
    // ------------------------------------------------------------

    g_app.hList =
        CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTVIEWW,
            L"",
            WS_CHILD |
            WS_VISIBLE |
            LVS_REPORT |
            LVS_SINGLESEL |
            LVS_SHOWSELALWAYS |
            LVS_OWNERDATA,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(
                IDC_LISTVIEW
            ),
            hInst,
            nullptr
        );

    SetWindowTheme(
        g_app.hList,
        L"Explorer",
        nullptr
    );

    ListView_SetExtendedListViewStyle(
        g_app.hList,
        LVS_EX_FULLROWSELECT |
        LVS_EX_DOUBLEBUFFER |
        LVS_EX_HEADERDRAGDROP |
        LVS_EX_LABELTIP
    );

    // ------------------------------------------------------------
    // Columns
    // ------------------------------------------------------------

    LVCOLUMNW col{};

    col.mask =
        LVCF_TEXT |
        LVCF_WIDTH |
        LVCF_SUBITEM;

    col.pszText =
        const_cast<LPWSTR>(L"NAME");

    col.cx = SC(220);
    col.iSubItem = 0;

    ListView_InsertColumn(
        g_app.hList,
        0,
        &col
    );

    col.pszText =
        const_cast<LPWSTR>(L"PID");

    col.cx = SC(75);
    col.iSubItem = 1;

    ListView_InsertColumn(
        g_app.hList,
        1,
        &col
    );

    col.pszText =
        const_cast<LPWSTR>(L"CPU %");

    col.cx = SC(85);
    col.iSubItem = 2;

    ListView_InsertColumn(
        g_app.hList,
        2,
        &col
    );

    col.pszText =
        const_cast<LPWSTR>(L"MEMORY");

    col.cx = SC(100);
    col.iSubItem = 3;

    ListView_InsertColumn(
        g_app.hList,
        3,
        &col
    );

    col.pszText =
        const_cast<LPWSTR>(L"PRIORITY");

    col.cx = SC(110);
    col.iSubItem = 4;

    ListView_InsertColumn(
        g_app.hList,
        4,
        &col
    );

    // ------------------------------------------------------------
    // Log
    // ------------------------------------------------------------

    g_app.hLog =
        CreateWindowExW(
            0,
            L"STATIC",
            L"[SYSTEM] Ready.",
            WS_CHILD |
            WS_VISIBLE |
            SS_LEFT,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(
                IDC_LOG_LABEL
            ),
            hInst,
            nullptr
        );

    HWND ctrls[] =
    {
        g_app.hSearch,
        g_app.hBtnRefresh,
        g_app.hChkAuto,
        g_app.hBtnSuspend,
        g_app.hBtnResume,
        g_app.hBtnTerminate,
        g_app.hList,
        g_app.hCpuLabel,
        g_app.hCpuCaption,
        g_app.hBtnElevate,
        g_app.hLog
    };

    for (HWND c : ctrls)
    {
        if (c)
        {
            SendMessageW(
                c,
                WM_SETFONT,
                reinterpret_cast<WPARAM>(
                    g_app.fontUI
                ),
                TRUE
            );
        }
    }

    SendMessageW(
        g_app.hLog,
        WM_SETFONT,
        reinterpret_cast<WPARAM>(
            g_app.fontMono
        ),
        TRUE
    );
}

// ============================================================================
// Layout
// ============================================================================

void RelayoutControls()
{
    RECT rc{};

    GetClientRect(
        g_app.hwnd,
        &rc
    );

    int margin = SC(16);
    int headerH = SC(50);

    int elevateW = SC(74);
    int elevateH = SC(28);

    MoveWindow(
        g_app.hBtnElevate,
        rc.right -
            margin -
            elevateW,
        margin + SC(4),
        elevateW,
        elevateH,
        TRUE
    );

    MakeButtonRounded(
        g_app.hBtnElevate,
        elevateW,
        elevateH
    );

    int toolbarY =
        headerH + margin;

    int toolbarH = SC(32);

    int listY =
        toolbarY +
        toolbarH +
        SC(12);

    int footerH = SC(30);

    int listH =
        rc.bottom -
        listY -
        footerH -
        margin;

    int listW =
        rc.right -
        margin * 2;

    if (listH < SC(80))
        listH = SC(80);

    if (listW < SC(300))
        listW = SC(300);

    // ------------------------------------------------------------
    // Search
    // ------------------------------------------------------------

    int cx = margin;
    int y = toolbarY;

    int searchW = SC(200);

    MoveWindow(
        g_app.hSearch,
        cx,
        y,
        searchW,
        toolbarH,
        TRUE
    );

    cx += searchW + SC(8);

    // ------------------------------------------------------------
    // Refresh
    // ------------------------------------------------------------

    int refreshW = SC(80);

    MoveWindow(
        g_app.hBtnRefresh,
        cx,
        y,
        refreshW,
        toolbarH,
        TRUE
    );

    MakeButtonRounded(
        g_app.hBtnRefresh,
        refreshW,
        toolbarH
    );

    cx += refreshW + SC(6);

    // ------------------------------------------------------------
    // Auto
    // ------------------------------------------------------------

    int autoW = SC(50);

    MoveWindow(
        g_app.hChkAuto,
        cx,
        y + SC(6),
        autoW,
        toolbarH - SC(8),
        TRUE
    );

    cx += autoW + SC(12);

    // ------------------------------------------------------------
    // Actions
    // ------------------------------------------------------------

    int actW = SC(80);

    MoveWindow(
        g_app.hBtnSuspend,
        cx,
        y,
        actW,
        toolbarH,
        TRUE
    );

    MakeButtonRounded(
        g_app.hBtnSuspend,
        actW,
        toolbarH
    );

    cx += actW + SC(6);

    MoveWindow(
        g_app.hBtnResume,
        cx,
        y,
        actW,
        toolbarH,
        TRUE
    );

    MakeButtonRounded(
        g_app.hBtnResume,
        actW,
        toolbarH
    );

    cx += actW + SC(6);

    int killW = SC(60);

    MoveWindow(
        g_app.hBtnTerminate,
        cx,
        y,
        killW,
        toolbarH,
        TRUE
    );

    MakeButtonRounded(
        g_app.hBtnTerminate,
        killW,
        toolbarH
    );

    // ------------------------------------------------------------
    // CPU display
    // ------------------------------------------------------------

    int cpuBarW = SC(130);
    int cpuLabelW = SC(44);
    int cpuCapW = SC(30);
    int gap = SC(8);

    int rightEdge =
        rc.right - margin;

    int lblX =
        rightEdge -
        cpuLabelW;

    int barX =
        lblX -
        gap -
        cpuBarW;

    int capX =
        barX -
        gap -
        cpuCapW;

    MoveWindow(
        g_app.hCpuCaption,
        capX,
        y + SC(6),
        cpuCapW,
        toolbarH - SC(8),
        TRUE
    );

    MoveWindow(
        g_app.hCpuBar,
        barX,
        y + SC(8),
        cpuBarW,
        toolbarH - SC(16),
        TRUE
    );

    MoveWindow(
        g_app.hCpuLabel,
        lblX,
        y + SC(6),
        cpuLabelW,
        toolbarH - SC(8),
        TRUE
    );

    // ------------------------------------------------------------
    // ListView
    // ------------------------------------------------------------

    MoveWindow(
        g_app.hList,
        margin,
        listY,
        listW,
        listH,
        TRUE
    );

    int col0 = SC(220);
    int col1 = SC(75);
    int col2 = SC(85);
    int col3 = SC(100);
    int col4 = SC(110);

    ListView_SetColumnWidth(
        g_app.hList,
        0,
        col0
    );

    ListView_SetColumnWidth(
        g_app.hList,
        1,
        col1
    );

    ListView_SetColumnWidth(
        g_app.hList,
        2,
        col2
    );

    ListView_SetColumnWidth(
        g_app.hList,
        3,
        col3
    );

    ListView_SetColumnWidth(
        g_app.hList,
        4,
        col4
    );

    // ------------------------------------------------------------
    // Footer
    // ------------------------------------------------------------

    int footerY =
        rc.bottom - footerH;

    MoveWindow(
        g_app.hLog,
        margin + SC(4),
        footerY + SC(5),
        listW - SC(8),
        footerH - SC(10),
        TRUE
    );

    InvalidateRect(
        g_app.hwnd,
        nullptr,
        TRUE
    );
}

// ============================================================================
// Rounded button
// ============================================================================

void MakeButtonRounded(
    HWND hBtn,
    int w,
    int h)
{
    if (!hBtn)
        return;

    HRGN rgn =
        CreateRoundRectRgn(
            0,
            0,
            w + 1,
            h + 1,
            SC(6),
            SC(6)
        );

    if (rgn)
        SetWindowRgn(
            hBtn,
            rgn,
            TRUE
        );
}

// ============================================================================
// Owner-draw buttons
// ============================================================================

LRESULT HandleDrawItem(LPARAM lParam)
{
    auto* dis =
        reinterpret_cast<
            LPDRAWITEMSTRUCT
        >(lParam);

    if (!dis)
        return FALSE;

    if (dis->CtlType != ODT_BUTTON)
        return FALSE;

    HDC hdc = dis->hDC;
    RECT r = dis->rcItem;

    UINT id = dis->CtlID;

    bool pressed =
        (dis->itemState & ODS_SELECTED) != 0;

    bool focused =
        (dis->itemState & ODS_FOCUS) != 0;

    COLORREF bg =
        Theme::BgControl;

    COLORREF fg =
        Theme::TextPrimary;

    if (id == IDC_BTN_ELEVATE)
    {
        bg =
            g_app.isElevated
            ? Theme::AccentDim
            : Theme::Amber;

        fg = RGB(0, 0, 0);
    }
    else if (id == IDC_BTN_TERMINATE)
    {
        bg =
            pressed
            ? RGB(0xA0, 0x30, 0x30)
            : Theme::Red;

        fg = RGB(255, 255, 255);
    }
    else if (id == IDC_BTN_SUSPEND)
    {
        bg =
            pressed
            ? RGB(0x90, 0x60, 0x20)
            : Theme::Amber;

        fg = RGB(0, 0, 0);
    }
    else if (id == IDC_BTN_RESUME)
    {
        bg =
            pressed
            ? RGB(0x20, 0x70, 0x90)
            : Theme::Cyan;

        fg = RGB(0, 0, 0);
    }
    else if (id == IDC_BTN_REFRESH)
    {
        bg =
            pressed
            ? Theme::AccentDim
            : Theme::Accent;

        fg = RGB(0, 0, 0);
    }
    else if (pressed)
    {
        bg = Theme::BgListSel;
    }

    HBRUSH hbr =
        CreateSolidBrush(bg);

    if (hbr)
    {
        FillRect(
            hdc,
            &r,
            hbr
        );

        DeleteObject(hbr);
    }

    if (focused)
    {
        HPEN pen =
            CreatePen(
                PS_SOLID,
                1,
                Theme::Accent
            );

        if (pen)
        {
            HPEN oldPen =
                static_cast<HPEN>(
                    SelectObject(
                        hdc,
                        pen
                    )
                );

            HBRUSH oldBrush =
                static_cast<HBRUSH>(
                    SelectObject(
                        hdc,
                        GetStockObject(
                            HOLLOW_BRUSH
                        )
                    )
                );

            Rectangle(
                hdc,
                r.left + 1,
                r.top + 1,
                r.right - 1,
                r.bottom - 1
            );

            SelectObject(
                hdc,
                oldBrush
            );

            SelectObject(
                hdc,
                oldPen
            );

            DeleteObject(pen);
        }
    }

    wchar_t text[128]{};

    GetWindowTextW(
        dis->hwndItem,
        text,
        _countof(text)
    );

    SetBkMode(
        hdc,
        TRANSPARENT
    );

    SetTextColor(
        hdc,
        fg
    );

    HFONT oldFont =
        static_cast<HFONT>(
            SelectObject(
                hdc,
                g_app.fontUIBold
            )
        );

    DrawTextW(
        hdc,
        text,
        -1,
        &r,
        DT_CENTER |
        DT_VCENTER |
        DT_SINGLELINE
    );

    SelectObject(
        hdc,
        oldFont
    );

    return TRUE;
}

// ============================================================================
// ListView custom drawing
// ============================================================================

LRESULT HandleListCustomDraw(LPARAM lParam)
{
    auto* cd =
        reinterpret_cast<
            LPNMLVCUSTOMDRAW
        >(lParam);

    if (!cd)
        return CDRF_DODEFAULT;

    switch (cd->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
    {
        int idx =
            static_cast<int>(
                cd->nmcd.dwItemSpec
            );

        bool selected =
            (ListView_GetItemState(
                g_app.hList,
                idx,
                LVIS_SELECTED
            ) & LVIS_SELECTED) != 0;

        bool alt =
            (idx % 2) == 1;

        cd->clrText =
            Theme::TextPrimary;

        cd->clrTextBk =
            selected
            ? Theme::BgListSel
            : (alt
               ? Theme::BgListRowAlt
               : Theme::BgListRow);

        return CDRF_NEWFONT;
    }

    default:
        return CDRF_DODEFAULT;
    }
}

// ============================================================================
// NT functions
// ============================================================================

void InitNtdllFunctions()
{
    HMODULE hNtdll =
        GetModuleHandleW(L"ntdll.dll");

    if (!hNtdll)
        return;

    g_app.pNtSuspendProcess =
        reinterpret_cast<
            NtSuspendProcess_t
        >(
            GetProcAddress(
                hNtdll,
                "NtSuspendProcess"
            )
        );

    g_app.pNtResumeProcess =
        reinterpret_cast<
            NtResumeProcess_t
        >(
            GetProcAddress(
                hNtdll,
                "NtResumeProcess"
            )
        );
}

// ============================================================================
// CPU monitor
// ============================================================================

void InitCpuMonitor()
{
    if (
        PdhOpenQueryW(
            nullptr,
            0,
            &g_app.pdhQuery
        ) != ERROR_SUCCESS
    )
    {
        g_app.pdhQuery = nullptr;
        return;
    }

    if (
        PdhAddEnglishCounterW(
            g_app.pdhQuery,
            L"\\Processor(_Total)\\% Processor Time",
            0,
            &g_app.pdhCounter
        ) != ERROR_SUCCESS
    )
    {
        g_app.pdhCounter = nullptr;

        PdhCloseQuery(
            g_app.pdhQuery
        );

        g_app.pdhQuery = nullptr;

        return;
    }

    PdhCollectQueryData(
        g_app.pdhQuery
    );
}

int ReadCpuPercent()
{
    if (
        !g_app.pdhQuery ||
        !g_app.pdhCounter
    )
    {
        return -1;
    }

    if (
        PdhCollectQueryData(
            g_app.pdhQuery
        ) != ERROR_SUCCESS
    )
    {
        return -1;
    }

    PDH_FMT_COUNTERVALUE val{};

    if (
        PdhGetFormattedCounterValue(
            g_app.pdhCounter,
            PDH_FMT_DOUBLE,
            nullptr,
            &val
        ) == ERROR_SUCCESS
    )
    {
        return static_cast<int>(
            val.doubleValue + 0.5
        );
    }

    return -1;
}

// ============================================================================
// Priority
// ============================================================================

const wchar_t* PriorityToString(DWORD pc)
{
    switch (pc)
    {
    case IDLE_PRIORITY_CLASS:
        return L"Idle";

    case BELOW_NORMAL_PRIORITY_CLASS:
        return L"Below Normal";

    case NORMAL_PRIORITY_CLASS:
        return L"Normal";

    case ABOVE_NORMAL_PRIORITY_CLASS:
        return L"Above Normal";

    case HIGH_PRIORITY_CLASS:
        return L"High";

    case REALTIME_PRIORITY_CLASS:
        return L"Realtime";

    default:
        return L"Normal";
    }
}

DWORD PriorityMenuIdToClass(UINT id)
{
    switch (id)
    {
    case IDM_PRIO_IDLE:
        return IDLE_PRIORITY_CLASS;

    case IDM_PRIO_BELOWNORMAL:
        return BELOW_NORMAL_PRIORITY_CLASS;

    case IDM_PRIO_NORMAL:
        return NORMAL_PRIORITY_CLASS;

    case IDM_PRIO_ABOVENORMAL:
        return ABOVE_NORMAL_PRIORITY_CLASS;

    case IDM_PRIO_HIGH:
        return HIGH_PRIORITY_CLASS;

    case IDM_PRIO_REALTIME:
        return REALTIME_PRIORITY_CLASS;
    }

    return NORMAL_PRIORITY_CLASS;
}

// ============================================================================
// Process enumeration
// ============================================================================

std::vector<ProcessEntry> EnumerateProcesses()
{
    std::vector<ProcessEntry> result;

    HANDLE snap =
        CreateToolhelp32Snapshot(
            TH32CS_SNAPPROCESS,
            0
        );

    if (snap == INVALID_HANDLE_VALUE)
        return result;

    // ------------------------------------------------------------
    // Thread counts
    // ------------------------------------------------------------

    std::unordered_map<DWORD, DWORD>
        threadCounts;

    HANDLE tSnap =
        CreateToolhelp32Snapshot(
            TH32CS_SNAPTHREAD,
            0
        );

    if (tSnap != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);

        if (Thread32First(
                tSnap,
                &te))
        {
            do
            {
                ++threadCounts[
                    te.th32OwnerProcessID
                ];
            }
            while (
                Thread32Next(
                    tSnap,
                    &te
                )
            );
        }

        CloseHandle(tSnap);
    }

    // ------------------------------------------------------------
    // Time baseline
    // ------------------------------------------------------------

    FILETIME fNow{};

    GetSystemTimeAsFileTime(
        &fNow
    );

    ULONGLONG nowTick =
        (static_cast<ULONGLONG>(
            fNow.dwHighDateTime
        ) << 32) |
        fNow.dwLowDateTime;

    ULONGLONG deltaTick = 0;

    if (
        g_app.cpuPrevTick > 0 &&
        nowTick > g_app.cpuPrevTick
    )
    {
        deltaTick =
            nowTick -
            g_app.cpuPrevTick;
    }

    std::unordered_map<DWORD, ULONGLONG>
        newTimes;

    // ------------------------------------------------------------
    // Enumerate
    // ------------------------------------------------------------

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(
            snap,
            &pe))
    {
        do
        {
            if (pe.th32ProcessID == 0)
                continue;

            ProcessEntry entry{};

            entry.pid =
                pe.th32ProcessID;

            entry.name =
                pe.szExeFile;

            auto itThreads =
                threadCounts.find(
                    entry.pid
                );

            if (itThreads !=
                threadCounts.end())
            {
                entry.threads =
                    itThreads->second;
            }

            // ----------------------------------------------------
            // Open process
            // ----------------------------------------------------

            ScopedHandle hProc(
                OpenProcess(
                    PROCESS_QUERY_INFORMATION |
                    PROCESS_VM_READ,
                    FALSE,
                    entry.pid
                )
            );

            if (!hProc.valid())
            {
                hProc.reset(
                    OpenProcess(
                        PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE,
                        entry.pid
                    )
                );
            }

            if (hProc.valid())
            {
                // ------------------------------------------------
                // Path
                // ------------------------------------------------

                wchar_t pBuf[32768]{};
                DWORD sz =
                    _countof(pBuf);

                if (
                    QueryFullProcessImageNameW(
                        hProc.get(),
                        0,
                        pBuf,
                        &sz
                    )
                )
                {
                    entry.path.assign(
                        pBuf,
                        sz
                    );
                }
                else
                {
                    entry.path =
                        L"Access Denied";
                }

                // ------------------------------------------------
                // Memory
                // ------------------------------------------------

                PROCESS_MEMORY_COUNTERS pmc{};

                if (
                    GetProcessMemoryInfo(
                        hProc.get(),
                        &pmc,
                        sizeof(pmc)
                    )
                )
                {
                    entry.workingSetBytes =
                        static_cast<ULONGLONG>(
                            pmc.WorkingSetSize
                        );
                }

                // ------------------------------------------------
                // Priority
                // ------------------------------------------------

                DWORD pc =
                    GetPriorityClass(
                        hProc.get()
                    );

                if (pc != 0)
                    entry.priorityClass = pc;

                // ------------------------------------------------
                // CPU
                // ------------------------------------------------

                FILETIME ftCreate{};
                FILETIME ftExit{};
                FILETIME ftKernel{};
                FILETIME ftUser{};

                if (
                    GetProcessTimes(
                        hProc.get(),
                        &ftCreate,
                        &ftExit,
                        &ftKernel,
                        &ftUser
                    )
                )
                {
                    ULONGLONG k =
                        (static_cast<ULONGLONG>(
                            ftKernel.dwHighDateTime
                        ) << 32) |
                        ftKernel.dwLowDateTime;

                    ULONGLONG u =
                        (static_cast<ULONGLONG>(
                            ftUser.dwHighDateTime
                        ) << 32) |
                        ftUser.dwLowDateTime;

                    ULONGLONG total =
                        k + u;

                    newTimes[
                        entry.pid
                    ] = total;

                    auto prevIt =
                        g_app.cpuPrevTimes.find(
                            entry.pid
                        );

                    if (
                        deltaTick > 0 &&
                        prevIt !=
                            g_app.cpuPrevTimes.end()
                    )
                    {
                        ULONGLONG prev =
                            prevIt->second;

                        ULONGLONG used =
                            (total >= prev)
                            ? (total - prev)
                            : 0;

                        double pct =
                            (
                                static_cast<double>(
                                    used
                                ) /
                                (
                                    static_cast<double>(
                                        deltaTick
                                    ) *
                                    static_cast<double>(
                                        g_app.numCores
                                    )
                                )
                            ) * 100.0;

                        if (pct < 0.0)
                            pct = 0.0;

                        entry.cpuPercent =
                            pct;
                    }
                }
            }

            result.push_back(
                std::move(entry)
            );

        }
        while (
            Process32NextW(
                snap,
                &pe
            )
        );
    }

    CloseHandle(snap);

    g_app.cpuPrevTimes =
        std::move(newTimes);

    g_app.cpuPrevTick =
        nowTick;

    // Default ordering:
    // memory descending
    std::sort(
        result.begin(),
        result.end(),
        [](const ProcessEntry& a,
           const ProcessEntry& b)
        {
            if (a.workingSetBytes !=
                b.workingSetBytes)
            {
                return a.workingSetBytes >
                       b.workingSetBytes;
            }

            return _wcsicmp(
                       a.name.c_str(),
                       b.name.c_str()
                   ) < 0;
        }
    );

    return result;
}

// ============================================================================
// Process actions
// ============================================================================

bool DoSuspend(DWORD pid)
{
    if (!g_app.pNtSuspendProcess)
        return false;

    ScopedHandle h(
        OpenProcess(
            PROCESS_SUSPEND_RESUME,
            FALSE,
            pid
        )
    );

    if (!h.valid())
        return false;

    return
        g_app.pNtSuspendProcess(
            h.get()
        ) == 0;
}

bool DoResume(DWORD pid)
{
    if (!g_app.pNtResumeProcess)
        return false;

    ScopedHandle h(
        OpenProcess(
            PROCESS_SUSPEND_RESUME,
            FALSE,
            pid
        )
    );

    if (!h.valid())
        return false;

    return
        g_app.pNtResumeProcess(
            h.get()
        ) == 0;
}

bool DoTerminate(DWORD pid)
{
    ScopedHandle h(
        OpenProcess(
            PROCESS_TERMINATE,
            FALSE,
            pid
        )
    );

    if (!h.valid())
        return false;

    return
        TerminateProcess(
            h.get(),
            1
        ) != 0;
}

void ApplyPriority(
    DWORD pid,
    DWORD cls)
{
    ScopedHandle h(
        OpenProcess(
            PROCESS_SET_INFORMATION |
            PROCESS_QUERY_INFORMATION,
            FALSE,
            pid
        )
    );

    if (!h.valid())
    {
        SetLog(
            L"[!] Access denied",
            Theme::Red
        );

        return;
    }

    if (
        SetPriorityClass(
            h.get(),
            cls
        )
    )
    {
        SetLog(
            L"[>] Priority changed",
            Theme::Accent
        );
    }
    else
    {
        SetLog(
            L"[!] Failed",
            Theme::Red
        );
    }
}

void ToggleAffinityBit(
    DWORD pid,
    int core)
{
    if (core < 0 ||
        core >= IDM_AFFINITY_MAX_CORES)
    {
        return;
    }

    ScopedHandle h(
        OpenProcess(
            PROCESS_QUERY_INFORMATION |
            PROCESS_SET_INFORMATION,
            FALSE,
            pid
        )
    );

    if (!h.valid())
    {
        SetLog(
            L"[!] Access denied",
            Theme::Red
        );

        return;
    }

    DWORD_PTR pmask = 0;
    DWORD_PTR smask = 0;

    if (
        !GetProcessAffinityMask(
            h.get(),
            &pmask,
            &smask
        )
    )
    {
        SetLog(
            L"[!] Unable to query affinity",
            Theme::Red
        );

        return;
    }

    if (
        core >=
        static_cast<int>(
            sizeof(DWORD_PTR) * 8
        )
    )
    {
        SetLog(
            L"[!] Core index unsupported",
            Theme::Amber
        );

        return;
    }

    DWORD_PTR bit =
        (static_cast<DWORD_PTR>(1)
         << core);

    if (pmask & bit)
        pmask &= ~bit;
    else
        pmask |= bit;

    if (pmask == 0)
    {
        SetLog(
            L"[!] At least one core required",
            Theme::Amber
        );

        return;
    }

    if (
        SetProcessAffinityMask(
            h.get(),
            pmask
        )
    )
    {
        SetLog(
            L"[>] Affinity updated",
            Theme::Accent
        );
    }
    else
    {
        SetLog(
            L"[!] Failed",
            Theme::Red
        );
    }
}

// ============================================================================
// Context menu
// ============================================================================

void ShowProcessContextMenu(
    HWND owner,
    POINT pt,
    DWORD pid)
{
    g_app.contextMenuPid = pid;

    HMENU menu =
        CreatePopupMenu();

    if (!menu)
        return;

    AppendMenuW(
        menu,
        MF_STRING,
        IDM_CTX_SUSPEND,
        L"Suspend"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        IDM_CTX_RESUME,
        L"Resume"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        IDM_CTX_TERMINATE,
        L"Kill"
    );

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    DWORD curPrio =
        NORMAL_PRIORITY_CLASS;

    DWORD_PTR pmask = 0;
    DWORD_PTR smask = 0;

    ScopedHandle hProc(
        OpenProcess(
            PROCESS_QUERY_INFORMATION,
            FALSE,
            pid
        )
    );

    if (hProc.valid())
    {
        DWORD pc =
            GetPriorityClass(
                hProc.get()
            );

        if (pc != 0)
            curPrio = pc;

        GetProcessAffinityMask(
            hProc.get(),
            &pmask,
            &smask
        );
    }

    // ------------------------------------------------------------
    // Priority submenu
    // ------------------------------------------------------------

    HMENU hPrio =
        CreatePopupMenu();

    if (hPrio)
    {
        struct PriorityItem
        {
            UINT id;
            DWORD cls;
            const wchar_t* label;
        };

        PriorityItem prios[] =
        {
            {
                IDM_PRIO_IDLE,
                IDLE_PRIORITY_CLASS,
                L"Idle"
            },
            {
                IDM_PRIO_BELOWNORMAL,
                BELOW_NORMAL_PRIORITY_CLASS,
                L"Below Normal"
            },
            {
                IDM_PRIO_NORMAL,
                NORMAL_PRIORITY_CLASS,
                L"Normal"
            },
            {
                IDM_PRIO_ABOVENORMAL,
                ABOVE_NORMAL_PRIORITY_CLASS,
                L"Above Normal"
            },
            {
                IDM_PRIO_HIGH,
                HIGH_PRIORITY_CLASS,
                L"High"
            },
            {
                IDM_PRIO_REALTIME,
                REALTIME_PRIORITY_CLASS,
                L"Realtime"
            }
        };

        for (const auto& p : prios)
        {
            AppendMenuW(
                hPrio,
                MF_STRING |
                    (
                        p.cls == curPrio
                        ? MF_CHECKED
                        : 0
                    ),
                p.id,
                p.label
            );
        }

        AppendMenuW(
            menu,
            MF_POPUP,
            reinterpret_cast<UINT_PTR>(
                hPrio
            ),
            L"Priority"
        );
    }

    // ------------------------------------------------------------
    // Affinity submenu
    // ------------------------------------------------------------

    HMENU hAff =
        CreatePopupMenu();

    if (hAff)
    {
        int maxBits =
            static_cast<int>(
                sizeof(DWORD_PTR) * 8
            );

        int maxCores =
            static_cast<int>(
                g_app.numCores
            );

        maxCores =
            std::min(
                maxCores,
                IDM_AFFINITY_MAX_CORES
            );

        maxCores =
            std::min(
                maxCores,
                maxBits
            );

        for (int i = 0;
             i < maxCores;
             ++i)
        {
            DWORD_PTR bit =
                (static_cast<DWORD_PTR>(1)
                 << i);

            if (!(smask & bit))
                continue;

            wchar_t label[32]{};

            swprintf_s(
                label,
                _countof(label),
                L"CPU %d",
                i
            );

            AppendMenuW(
                hAff,
                MF_STRING |
                    (
                        (pmask & bit)
                        ? MF_CHECKED
                        : 0
                    ),
                IDM_AFFINITY_BASE + i,
                label
            );
        }

        AppendMenuW(
            menu,
            MF_POPUP,
            reinterpret_cast<UINT_PTR>(
                hAff
            ),
            L"Affinity"
        );
    }

    AppendMenuW(
        menu,
        MF_SEPARATOR,
        0,
        nullptr
    );

    AppendMenuW(
        menu,
        MF_STRING,
        IDM_CTX_OPENLOC,
        L"Open Location"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        IDM_CTX_COPYPATH,
        L"Copy Path"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        IDM_CTX_COPYNAME,
        L"Copy Name"
    );

    AppendMenuW(
        menu,
        MF_STRING,
        IDM_CTX_PROPERTIES,
        L"Properties"
    );

    TrackPopupMenu(
        menu,
        TPM_LEFTALIGN |
        TPM_RIGHTBUTTON,
        pt.x,
        pt.y,
        0,
        owner,
        nullptr
    );

    DestroyMenu(menu);

    g_app.contextMenuPid = 0;
}

// ============================================================================
// Selected PID
// ============================================================================

DWORD GetSelectedPid()
{
    if (!g_app.hList)
        return 0;

    int idx =
        ListView_GetNextItem(
            g_app.hList,
            -1,
            LVNI_SELECTED
        );

    if (
        idx >= 0 &&
        idx <
        static_cast<int>(
            g_app.allProcesses.size()
        )
    )
    {
        return g_app.allProcesses[
            static_cast<size_t>(idx)
        ].pid;
    }

    return 0;
}

// ============================================================================
// FIXED: List filtering/sorting
// ============================================================================

void RefreshListDisplay()
{
    // ------------------------------------------------------------
    // Remember currently selected PID BEFORE replacing the dataset.
    // ------------------------------------------------------------

    DWORD selectedPid = GetSelectedPid();

    // ------------------------------------------------------------
    // Build the exact dataset that will back LVS_OWNERDATA.
    // ------------------------------------------------------------

    std::vector<ProcessEntry> filtered;

    filtered.reserve(
        g_app.allProcesses.size()
    );

    for (const auto& p :
         g_app.allProcesses)
    {
        if (!g_app.filterLower.empty())
        {
            std::wstring lower =
                p.name;

            std::transform(
                lower.begin(),
                lower.end(),
                lower.begin(),
                [](wchar_t ch)
                {
                    return static_cast<wchar_t>(
                        std::towlower(ch)
                    );
                }
            );

            if (
                lower.find(
                    g_app.filterLower
                ) == std::wstring::npos
            )
            {
                continue;
            }
        }

        filtered.push_back(p);
    }

    // ------------------------------------------------------------
    // Sort
    // ------------------------------------------------------------

    if (g_app.sortColumn >= 0)
    {
        int col =
            g_app.sortColumn;

        bool asc =
            g_app.sortAscending;

        std::sort(
            filtered.begin(),
            filtered.end(),
            [col, asc](
                const ProcessEntry& a,
                const ProcessEntry& b)
            {
                int cmp = 0;

                switch (col)
                {
                case 0:
                    cmp = _wcsicmp(
                        a.name.c_str(),
                        b.name.c_str()
                    );
                    break;

                case 1:
                    if (a.pid < b.pid)
                        cmp = -1;
                    else if (a.pid > b.pid)
                        cmp = 1;
                    else
                        cmp = 0;
                    break;

                case 2:
                {
                    double av =
                        a.cpuPercent < 0.0
                        ? -1.0
                        : a.cpuPercent;

                    double bv =
                        b.cpuPercent < 0.0
                        ? -1.0
                        : b.cpuPercent;

                    if (av < bv)
                        cmp = -1;
                    else if (av > bv)
                        cmp = 1;
                    else
                        cmp = 0;

                    break;
                }

                case 3:
                    if (
                        a.workingSetBytes <
                        b.workingSetBytes
                    )
                    {
                        cmp = -1;
                    }
                    else if (
                        a.workingSetBytes >
                        b.workingSetBytes
                    )
                    {
                        cmp = 1;
                    }
                    else
                    {
                        cmp = 0;
                    }

                    break;

                case 4:
                    cmp = _wcsicmp(
                        PriorityToString(
                            a.priorityClass
                        ),
                        PriorityToString(
                            b.priorityClass
                        )
                    );
                    break;

                default:
                    cmp = 0;
                    break;
                }

                return asc
                    ? (cmp < 0)
                    : (cmp > 0);
            }
        );
    }

    // ====================================================================
    // CRITICAL FIX
    //
    // The backing vector MUST be replaced BEFORE ListView_SetItemCountEx().
    //
    // Previously:
    //
    //     ListView_SetItemCountEx(...);
    //     g_app.allProcesses = filtered;
    //
    // That allowed LVN_GETDISPINFO to run while allProcesses still
    // contained the old data, causing a blank virtual ListView.
    // ====================================================================

    g_app.allProcesses =
        std::move(filtered);

    int count =
        static_cast<int>(
            g_app.allProcesses.size()
        );

    // Remove any stale selection state.
    ListView_SetItemState(
        g_app.hList,
        -1,
        0,
        LVIS_SELECTED |
        LVIS_FOCUSED
    );

    // Update virtual row count AFTER dataset replacement.
    ListView_SetItemCountEx(
        g_app.hList,
        count,
        LVSICF_NOSCROLL
    );

    // ------------------------------------------------------------
    // Restore selection by PID.
    // ------------------------------------------------------------

    if (selectedPid != 0)
    {
        for (
            int i = 0;
            i < count;
            ++i)
        {
            if (
                g_app.allProcesses[
                    static_cast<size_t>(i)
                ].pid == selectedPid
            )
            {
                ListView_SetItemState(
                    g_app.hList,
                    i,
                    LVIS_SELECTED |
                    LVIS_FOCUSED,
                    LVIS_SELECTED |
                    LVIS_FOCUSED
                );

                ListView_EnsureVisible(
                    g_app.hList,
                    i,
                    FALSE
                );

                break;
            }
        }
    }

    // Force virtual rows to repaint.
    InvalidateRect(
        g_app.hList,
        nullptr,
        TRUE
    );

    UpdateWindow(
        g_app.hList
    );
}

// ============================================================================
// Refresh process data
// ============================================================================

void RefreshProcessData()
{
    DWORD selectedPid =
        GetSelectedPid();

    std::vector<ProcessEntry> fresh =
        EnumerateProcesses();

    // Replace source dataset.
    g_app.allProcesses =
        std::move(fresh);

    // RefreshListDisplay intentionally rebuilds the filtered
    // virtual-list dataset and preserves selected PID.
    //
    // However, because RefreshListDisplay reads GetSelectedPid()
    // from the new dataset, temporarily restore selection by doing
    // the filtering ourselves below.

    std::vector<ProcessEntry> filtered;

    filtered.reserve(
        g_app.allProcesses.size()
    );

    for (const auto& p :
         g_app.allProcesses)
    {
        if (!g_app.filterLower.empty())
        {
            std::wstring lower =
                p.name;

            std::transform(
                lower.begin(),
                lower.end(),
                lower.begin(),
                [](wchar_t ch)
                {
                    return static_cast<wchar_t>(
                        std::towlower(ch)
                    );
                }
            );

            if (
                lower.find(
                    g_app.filterLower
                ) == std::wstring::npos
            )
            {
                continue;
            }
        }

        filtered.push_back(p);
    }

    // Apply current sort.
    if (g_app.sortColumn >= 0)
    {
        int col =
            g_app.sortColumn;

        bool asc =
            g_app.sortAscending;

        std::sort(
            filtered.begin(),
            filtered.end(),
            [col, asc](
                const ProcessEntry& a,
                const ProcessEntry& b)
            {
                int cmp = 0;

                switch (col)
                {
                case 0:
                    cmp = _wcsicmp(
                        a.name.c_str(),
                        b.name.c_str()
                    );
                    break;

                case 1:
                    cmp =
                        a.pid < b.pid
                        ? -1
                        : (a.pid > b.pid
                           ? 1
                           : 0);
                    break;

                case 2:
                {
                    double av =
                        a.cpuPercent < 0.0
                        ? -1.0
                        : a.cpuPercent;

                    double bv =
                        b.cpuPercent < 0.0
                        ? -1.0
                        : b.cpuPercent;

                    cmp =
                        av < bv
                        ? -1
                        : (av > bv
                           ? 1
                           : 0);

                    break;
                }

                case 3:
                    cmp =
                        a.workingSetBytes <
                        b.workingSetBytes
                        ? -1
                        : (a.workingSetBytes >
                           b.workingSetBytes
                           ? 1
                           : 0);
                    break;

                case 4:
                    cmp = _wcsicmp(
                        PriorityToString(
                            a.priorityClass
                        ),
                        PriorityToString(
                            b.priorityClass
                        )
                    );
                    break;

                default:
                    break;
                }

                return asc
                    ? cmp < 0
                    : cmp > 0;
            }
        );
    }

    // IMPORTANT:
    // Set the actual OWNERDATA dataset before setting row count.
    g_app.allProcesses =
        std::move(filtered);

    ListView_SetItemState(
        g_app.hList,
        -1,
        0,
        LVIS_SELECTED |
        LVIS_FOCUSED
    );

    int count =
        static_cast<int>(
            g_app.allProcesses.size()
        );

    ListView_SetItemCountEx(
        g_app.hList,
        count,
        LVSICF_NOSCROLL
    );

    // Restore previous PID.
    if (selectedPid)
    {
        for (int i = 0; i < count; ++i)
        {
            if (
                g_app.allProcesses[
                    static_cast<size_t>(i)
                ].pid == selectedPid
            )
            {
                ListView_SetItemState(
                    g_app.hList,
                    i,
                    LVIS_SELECTED |
                    LVIS_FOCUSED,
                    LVIS_SELECTED |
                    LVIS_FOCUSED
                );

                ListView_EnsureVisible(
                    g_app.hList,
                    i,
                    FALSE
                );

                break;
            }
        }
    }

    InvalidateRect(
        g_app.hList,
        nullptr,
        TRUE
    );
}

// ============================================================================
// Log
// ============================================================================

void SetLog(
    const std::wstring& text,
    COLORREF color)
{
    g_app.logColor = color;

    SetWindowTextW(
        g_app.hLog,
        text.c_str()
    );

    InvalidateRect(
        g_app.hLog,
        nullptr,
        TRUE
    );
}

// ============================================================================
// Shell helpers
// ============================================================================

void OpenFileLocation(
    const std::wstring& path)
{
    if (
        path.empty() ||
        path == L"Access Denied"
    )
    {
        return;
    }

    std::wstring arg =
        L"/select,\""
        + path +
        L"\"";

    ShellExecuteW(
        nullptr,
        L"open",
        L"explorer.exe",
        arg.c_str(),
        nullptr,
        SW_SHOWNORMAL
    );
}

void CopyTextToClipboard(
    HWND hwnd,
    const std::wstring& text)
{
    if (!OpenClipboard(hwnd))
        return;

    EmptyClipboard();

    SIZE_T bytes =
        (text.size() + 1) *
        sizeof(wchar_t);

    HGLOBAL hMem =
        GlobalAlloc(
            GMEM_MOVEABLE |
            GMEM_ZEROINIT,
            bytes
        );

    if (!hMem)
    {
        CloseClipboard();
        return;
    }

    void* pMem =
        GlobalLock(hMem);

    if (!pMem)
    {
        GlobalFree(hMem);
        CloseClipboard();
        return;
    }

    memcpy(
        pMem,
        text.c_str(),
        bytes
    );

    GlobalUnlock(hMem);

    if (
        SetClipboardData(
            CF_UNICODETEXT,
            hMem
        ) == nullptr
    )
    {
        GlobalFree(hMem);
    }

    CloseClipboard();
}

void ShowFileProperties(
    const std::wstring& path)
{
    if (
        path.empty() ||
        path == L"Access Denied"
    )
    {
        return;
    }

    SHELLEXECUTEINFOW sei{
        sizeof(sei)
    };

    sei.fMask =
        SEE_MASK_INVOKEIDLIST;

    sei.lpVerb =
        L"properties";

    sei.lpFile =
        path.c_str();

    sei.nShow =
        SW_SHOWNORMAL;

    ShellExecuteExW(&sei);
}
