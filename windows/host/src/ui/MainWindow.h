// Main window: connection status, phone, virtual monitor, stream settings and live statistics.
#pragma once

#include <windows.h>
#include <shellapi.h>

#include <map>
#include <string>
#include <vector>

#include "../app/Controller.h"
#include "../util/ProcessStats.h"

namespace celmon {

class MainWindow {
public:
    static constexpr UINT kMsgTray = WM_APP + 1;  // tray icon notifications
    static constexpr UINT kMsgExit = WM_APP + 2;  // posted by "CelMonitor.exe --exit" (installer, scripts)

    explicit MainWindow(Controller& controller) : controller_(controller) {}
    // startHidden: only the tray icon (used when starting with Windows).
    bool Create(HINSTANCE instance, int show, bool startHidden);
    HWND Handle() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l);
    LRESULT Handle(UINT msg, WPARAM w, LPARAM l);
    void CreateControls();
    void Layout();
    void Refresh();
    void OnCommand(int id, int code);
    void SetText(int id, const std::wstring& text);
    void FillCombo(int id, const std::vector<std::wstring>& items);
    void SelectCombo(int id, int index);
    int Scale(int v) const { return MulDiv(v, dpi_, 96); }
    void RecreateFonts();
    void LoadIcons();
    void SetStreamingLook(bool streaming);
    void Paint();
    NOTIFYICONDATAW TrayData() const;
    void AddTrayIcon();
    void UpdateTrayTip(const std::wstring& status);
    void ShowFromTray();
    void ShowTrayMenu();
    void ExitApp();

    Controller& controller_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr, bold_ = nullptr, big_ = nullptr;
    HBRUSH bg_ = nullptr;
    // Logo in the header (large) and in the tray/title bar (small): coloured while streaming, grey otherwise.
    HICON logoLive_ = nullptr, logoIdle_ = nullptr, smallLive_ = nullptr, smallIdle_ = nullptr;
    RECT logoRect_ = {};
    bool streamingLook_ = true;      // so the first SetStreamingLook(false) applies the grey icons
    bool showDetails_ = false;       // "Detalhes técnicos" expanded
    std::map<int, HWND> ctl_;
    std::map<int, std::wstring> textCache_;
    std::vector<Mode> comboModes_;   // resolution combo items after "Automática"
    bool updating_ = false;          // programmatic combo changes, ignore notifications
    bool messageIsError_ = false;
    ProcessStats stats_;
    ULONGLONG lastStatsSample_ = 0;
    UINT taskbarCreated_ = 0;
    bool trayNotified_ = false;
    bool exiting_ = false;
    std::wstring trayTip_;
};

}  // namespace celmon
