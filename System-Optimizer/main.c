#define _WIN32_WINNT 0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shlobj.h>

void set_color_yellow() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
}

void set_color_reset() {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

BOOL is_admin() {
    return IsUserAnAdmin();
}

void relaunch_as_admin() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas";
    sei.lpFile = path;
    sei.hwnd = NULL;
    sei.nShow = SW_NORMAL;

    if (!ShellExecuteExA(&sei)) {
        printf("[!] Failed to obtain Administrator privileges. Exiting.\n");
        system("pause");
        exit(1);
    }
    exit(0);
}

void environment_warning() {
    char choice[10];
    while (1) {
        system("cls");
        set_color_yellow();
        printf("==================================================\n");
        printf("         ENVIRONMENT COMPATIBILITY WARNING       \n");
        printf("==================================================\n\n");
        printf("NOTICE: Special OS Environment Compatibility Warning\n\n");
        set_color_reset();
        printf("  This application relies heavily on native Windows APIs, DWM composition,\n");
        printf("  WMI system telemetry, and standard core background services.\n\n");
        printf("  If running in WinPE, WinLite, or heavily modified OS environments:\n");
        printf("   * The application may freeze, crash, or fail to render UI elements.\n");
        printf("   * System metrics and process monitoring features may fail.\n");
        printf("   * SYSTEM or TrustedInstaller escalation may not function properly.\n\n");
        set_color_yellow();
        printf("==================================================\n");
        set_color_reset();
        printf("Please choose how you would like to proceed:\n\n");
        printf("  [1] I agree and understand the risks (Continue)\n");
        printf("  [2] Exit launcher\n\n");
        printf("Enter your choice (1-2): ");

        if (fgets(choice, sizeof(choice), stdin)) {
            if (choice[0] == '1') return;
            if (choice[0] == '2') exit(0);
        }
    }
}

void reset_vram() {
    printf("\n[*] Resetting VRAM and restarting GPU Display Subsystem...\n");

    // Simulate Win + Ctrl + Shift + B
    keybd_event(VK_LWIN, 0, 0, 0);
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event(VK_SHIFT, 0, 0, 0);
    keybd_event('B', 0, 0, 0);

    Sleep(100);

    keybd_event('B', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);

    printf("    - Sent native graphics driver reload sequence (Win + Ctrl + Shift + B).\n");
}

void clear_ram() {
    printf("\n[*] Trimming RAM working sets across all running processes...\n");
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32);

        if (Process32First(hSnapshot, &pe32)) {
            do {
                HANDLE hProcess = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pe32.th32ProcessID);
                if (hProcess) {
                    // (SIZE_T)-1 for both min and max forces Windows to trim the process working set
                    SetProcessWorkingSetSize(hProcess, (SIZE_T)-1, (SIZE_T)-1);
                    CloseHandle(hProcess);
                }
            } while (Process32Next(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }
    printf("    - Flushed unused process memory back to available system RAM.\n");
}

void clean_junk() {
    printf("\n[*] Cleaning system temp files, Prefetch, and cache directories...\n");
    system("del /q /f /s \"%TEMP%\\*\" >nul 2>&1");
    system("del /q /f /s \"C:\\Windows\\Temp\\*\" >nul 2>&1");
    system("del /q /f /s \"C:\\Windows\\Prefetch\\*\" >nul 2>&1");
    system("del /q /f /s \"C:\\Windows\\SoftwareDistribution\\Download\\*\" >nul 2>&1");
    printf("    - System temp folders and Prefetch cache cleared.\n");
}

void flush_network() {
    printf("\n[*] Flushing DNS resolver cache and ARP tables...\n");
    system("ipconfig /flushdns >nul 2>&1");
    system("netsh interface ip delete arpcache >nul 2>&1");
    printf("    - DNS cache cleared and network routing table refreshed.\n");
}

int main() {
    if (!is_admin()) {
        relaunch_as_admin();
    }

    environment_warning();

    char choice[10];
    while (1) {
        system("cls");
        printf("==================================================\n");
        printf("              FULL SYSTEM OPTIMIZER               \n");
        printf("==================================================\n\n");
        printf("  [1] Run Full System Optimization\n");
        printf("  [2] Reset VRAM / Display Driver Only\n");
        printf("  [3] Clear RAM Cache / Trim Working Sets Only\n");
        printf("  [4] Purge Temporary & Junk Files Only\n");
        printf("  [5] Flush Network & DNS Cache Only\n");
        printf("  [6] Exit\n\n");
        printf("Enter your choice (1-6): ");

        if (fgets(choice, sizeof(choice), stdin)) {
            switch (choice[0]) {
                case '1':
                    system("cls");
                    printf("==================================================\n");
                    printf("          RUNNING FULL OPTIMIZATION PROCESS        \n");
                    printf("==================================================\n");
                    reset_vram();
                    clear_ram();
                    clean_junk();
                    flush_network();
                    printf("\n==================================================\n");
                    printf("          ALL OPTIMIZATION TASKS COMPLETED         \n");
                    printf("==================================================\n\n");
                    system("pause");
                    break;
                case '2':
                    reset_vram();
                    system("pause");
                    break;
                case '3':
                    clear_ram();
                    system("pause");
                    break;
                case '4':
                    clean_junk();
                    system("pause");
                    break;
                case '5':
                    flush_network();
                    system("pause");
                    break;
                case '6':
                    return 0;
            }
        }
    }
    return 0;
}
