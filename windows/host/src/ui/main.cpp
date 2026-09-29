// CelMonitor.exe — Windows app: uses a USB-connected Android phone as an extra monitor.
#include <windows.h>

#include "../app/Controller.h"
#include "../util/Log.h"
#include "MainWindow.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // One instance only: two would fight over the phone and the virtual monitor.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\CelMonitor.Host.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"CelMonitorMain", nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    // Per-monitor DPI awareness is also required by Desktop Duplication.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    celmon::Log::Init(false);
    celmon::Log::Info("CelMonitor started");

    celmon::Controller controller;
    celmon::MainWindow window(controller);
    if (!window.Create(instance, show)) return 1;
    controller.Start();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window.Handle(), &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    controller.Shutdown();
    celmon::Log::Info("CelMonitor exited");
    CloseHandle(single);
    return 0;
}
