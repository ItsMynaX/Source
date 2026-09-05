#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <cwchar>

// Control IDs
#define ID_LISTVIEW      1001
#define ID_BTN_KILL      1002
#define ID_BTN_REFRESH   1003
#define ID_PROGRESS_CPU  1004
#define ID_PROGRESS_RAM  1005
#define ID_TIMER_UPDATE  1

// Global UI Handles
HWND g_hMainWnd   = NULL;
HWND g_hListView  = NULL;
HWND g_hCpuBar    = NULL;
HWND g_hRamBar    = NULL;
HWND g_hCpuText   = NULL;
HWND g_hRamText   = NULL;
HWND g_hStatus    = NULL;

// Global State
FILETIME g_ftPrevIdle{}, g_ftPrevKernel{}, g_ftPrevUser{};

struct ProcessEntry {
    DWORD pid;
    std::wstring name;
    DWORD memoryMB;
};

// ============================================================================
// SYSTEM TELEMETRY (Kernel32 Only)
// ============================================================================
static ULONGLONG SubtractTimes(const FILETIME& ftA, const FILETIME& ftB) {
    ULARGE_INTEGER a, b;
    a.LowPart = ftA.dwLowDateTime; a.HighPart = ftA.dwHighDateTime;
    b.LowPart = ftB.dwLowDateTime; b.HighPart = ftB.dwHighDateTime;
    return a.QuadPart - b.QuadPart;
}

double GetSystemCpuUsage() {
    FILETIME ftIdle, ftKernel, ftUser;
    if (!GetSystemTimes(&ftIdle, &ftKernel, &ftUser)) return 0.0;

    ULONGLONG idleDiff   = SubtractTimes(ftIdle, g_ftPrevIdle);
    ULONGLONG kernelDiff = SubtractTimes(ftKernel, g_ftPrevKernel);
    ULONGLONG userDiff   = SubtractTimes(ftUser, g_ftPrevUser);
    ULONGLONG totalSys   = kernelDiff + userDiff;

    g_ftPrevIdle   = ftIdle;
    g_ftPrevKernel = ftKernel;
    g_ftPrevUser   = ftUser;

    if (totalSys == 0) return 0.0;
    return (double)(totalSys - idleDiff) * 100.0 / (double)totalSys;
}

DWORD GetSystemRamUsage() {
    MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
    if (GlobalMemoryStatusEx(&memStatus)) {
        return memStatus.dwMemoryLoad;
    }
    return 0;
}

std::vector<ProcessEntry> FetchProcesses() {
    std::vector<ProcessEntry> procs;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return procs;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnap, &pe32)) {
        do {
            ProcessEntry entry;
            entry.pid = pe32.th32ProcessID;
            entry.name = pe32.szExeFile;
            entry.memoryMB = 0;

            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, entry.pid);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                    entry.memoryMB = static_cast<DWORD>(pmc.WorkingSetSize / (1024 * 1024));
                }
                CloseHandle(hProc);
            }
            procs.push_back(entry);
        } while (Process32NextW(hSnap, &pe32));
    }
    CloseHandle(hSnap);
    return procs;
}

// ============================================================================
// UI LOGIC
// ============================================================================
void RefreshProcessList() {
    ListView_DeleteAllItems(g_hListView);
    std::vector<ProcessEntry> list = FetchProcesses();

    int index = 0;
    for (const auto& proc : list) {
        LVITEMW lvi = { 0 };
        lvi.mask = LVIF_TEXT;
        lvi.iItem = index;
        lvi.iSubItem = 0;
        lvi.pszText = const_cast<wchar_t*>(proc.name.c_str());
        ListView_InsertItem(g_hListView, &lvi);

        wchar_t buf[32];
        swprintf_s(buf, L"%lu", proc.pid);
        ListView_SetItemText(g_hListView, index, 1, buf);

        swprintf_s(buf, L"%lu MB", proc.memoryMB);
        ListView_SetItemText(g_hListView, index, 2, buf);

        index++;
    }

    wchar_t statusBuf[64];
    swprintf_s(statusBuf, L"Processes: %d", static_cast<int>(list.size()));
    SetWindowTextW(g_hStatus, statusBuf);
}

void TerminateSelectedProcess() {
    int selectedRow = ListView_GetNextItem(g_hListView, -1, LVNI_SELECTED);
    if (selectedRow == -1) {
        MessageBoxW(g_hMainWnd, L"Please select a process first.", L"MynaTask Lite", MB_OK | MB_ICONWARNING);
        return;
    }

    wchar_t pidText[32] = { 0 };
    ListView_GetItemText(g_hListView, selectedRow, 1, pidText, 32);
    DWORD pid = _wtoi(pidText);

    if (pid == 0 || pid == 4) {
        MessageBoxW(g_hMainWnd, L"Cannot terminate System process.", L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (hProc) {
        if (TerminateProcess(hProc, 0)) {
            MessageBoxW(g_hMainWnd, L"Process terminated successfully.", L"Success", MB_OK | MB_ICONINFORMATION);
            RefreshProcessList();
        } else {
            MessageBoxW(g_hMainWnd, L"Failed to terminate process.", L"Error", MB_OK | MB_ICONERROR);
        }
        CloseHandle(hProc);
    } else {
        MessageBoxW(g_hMainWnd, L"Access Denied or Process Unavailable.", L"Error", MB_OK | MB_ICONERROR);
    }
}

// ============================================================================
// WINDOW PROCEDURE
// ============================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        CreateWindowW(L"STATIC", L"CPU Usage:", WS_CHILD | WS_VISIBLE, 10, 10, 80, 20, hwnd, NULL, NULL, NULL);
        g_hCpuBar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 90, 10, 200, 20, hwnd, reinterpret_cast<HMENU>(ID_PROGRESS_CPU), NULL, NULL);
        g_hCpuText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 298, 10, 50, 20, hwnd, NULL, NULL, NULL);

        CreateWindowW(L"STATIC", L"RAM Usage:", WS_CHILD | WS_VISIBLE, 360, 10, 80, 20, hwnd, NULL, NULL, NULL);
        g_hRamBar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 440, 10, 200, 20, hwnd, reinterpret_cast<HMENU>(ID_PROGRESS_RAM), NULL, NULL);
        g_hRamText = CreateWindowW(L"STATIC", L"0%", WS_CHILD | WS_VISIBLE, 648, 10, 50, 20, hwnd, NULL, NULL, NULL);

        g_hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            10, 40, 690, 360, hwnd, reinterpret_cast<HMENU>(ID_LISTVIEW), NULL, NULL);

        ListView_SetExtendedListViewStyle(g_hListView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        LVCOLUMNW lvc = { 0 };
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        lvc.iSubItem = 0; lvc.pszText = const_cast<wchar_t*>(L"Process Name"); lvc.cx = 380;
        ListView_InsertColumn(g_hListView, 0, &lvc);

        lvc.iSubItem = 1; lvc.pszText = const_cast<wchar_t*>(L"PID"); lvc.cx = 120;
        ListView_InsertColumn(g_hListView, 1, &lvc);

        lvc.iSubItem = 2; lvc.pszText = const_cast<wchar_t*>(L"Working Set"); lvc.cx = 160;
        ListView_InsertColumn(g_hListView, 2, &lvc);

        CreateWindowW(L"BUTTON", L"Refresh List", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 410, 110, 28, hwnd, reinterpret_cast<HMENU>(ID_BTN_REFRESH), NULL, NULL);
        CreateWindowW(L"BUTTON", L"End Process", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 130, 410, 110, 28, hwnd, reinterpret_cast<HMENU>(ID_BTN_KILL), NULL, NULL);
        g_hStatus = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE, 250, 415, 400, 20, hwnd, NULL, NULL, NULL);

        EnumChildWindows(hwnd, [](HWND child, LPARAM font) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        }, reinterpret_cast<LPARAM>(hFont));

        SetTimer(hwnd, ID_TIMER_UPDATE, 1000, NULL);
        RefreshProcessList();
        break;
    }

    case WM_TIMER: {
        if (wParam == ID_TIMER_UPDATE) {
            int cpu = static_cast<int>(GetSystemCpuUsage());
            int ram = static_cast<int>(GetSystemRamUsage());

            SendMessageW(g_hCpuBar, PBM_SETPOS, static_cast<WPARAM>(cpu), 0);
            SendMessageW(g_hRamBar, PBM_SETPOS, static_cast<WPARAM>(ram), 0);

            wchar_t cpuBuf[16], ramBuf[16];
            swprintf_s(cpuBuf, L"%d%%", cpu);
            swprintf_s(ramBuf, L"%d%%", ram);

            SetWindowTextW(g_hCpuText, cpuBuf);
            SetWindowTextW(g_hRamText, ramBuf);
        }
        break;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_BTN_REFRESH:
            RefreshProcessList();
            break;
        case ID_BTN_KILL:
            TerminateSelectedProcess();
            break;
        }
        break;
    }

    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_UPDATE);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ============================================================================
// ENTRY POINT
// ============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icex);

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MynaTaskLiteClass";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);

    RegisterClassW(&wc);

    g_hMainWnd = CreateWindowExW(0, L"MynaTaskLiteClass", L"MynaTask Lite (WinPE / WinLite Compatible)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 725, 485, NULL, NULL, hInstance, NULL);

    ShowWindow(g_hMainWnd, nCmdShow);
    UpdateWindow(g_hMainWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
