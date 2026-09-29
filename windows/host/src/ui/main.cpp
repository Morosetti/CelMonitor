// CelMonitor.exe — Windows app: uses a USB-connected Android phone as an extra monitor.
//   --minimized  start in the tray only (used by "start with Windows")
//   --exit       ask a running instance to quit gracefully (installer/uninstaller)
//   --autostart-on / --autostart-off   set "start with Windows" for the current user and quit
#include <windows.h>
#include <shellapi.h>

#include "../app/Controller.h"
#include "../util/Autostart.h"
#include "../util/Log.h"
#include "MainWindow.h"

static bool HasArg(const wchar_t* name) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0) found = true;
    LocalFree(argv);
    return found;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Installer helpers, run as the logged-in user (not elevated) so the setting lands in *their* registry.
    if (HasArg(L"--autostart-on")) return celmon::Autostart::SetEnabled(true) ? 0 : 1;
    if (HasArg(L"--autostart-off")) return celmon::Autostart::SetEnabled(false) ? 0 : 1;

    const bool wantExit = HasArg(L"--exit");
    // One instance only: two would fight over the phone and the virtual monitor.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\CelMonitor.Host.Instance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"CelMonitorMain", nullptr)) {
            if (wantExit) {
                DWORD pid = 0;
                GetWindowThreadProcessId(other, &pid);
                HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
                PostMessageW(other, celmon::MainWindow::kMsgExit, 0, 0);
                if (proc) {
                    WaitForSingleObject(proc, 15000);  // it removes the virtual monitor before exiting
                    CloseHandle(proc);
                }
            } else {
                ShowWindow(other, SW_SHOWNORMAL);
                SetForegroundWindow(other);
            }
        }
        return 0;
    }
    if (wantExit) return 0;  // nothing running

    // Per-monitor DPI awareness is also required by Desktop Duplication.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    celmon::Log::Init(false);
    celmon::Log::Info("CelMonitor started");

    celmon::Controller controller;
    celmon::MainWindow window(controller);
    if (!window.Create(instance, show, HasArg(L"--minimized"))) return 1;
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
