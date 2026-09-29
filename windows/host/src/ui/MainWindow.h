// Main window: connection status, phone, virtual monitor, stream settings and live statistics.
#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "../app/Controller.h"
#include "../util/ProcessStats.h"

namespace celmon {

class MainWindow {
public:
    explicit MainWindow(Controller& controller) : controller_(controller) {}
    bool Create(HINSTANCE instance, int show);
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

    Controller& controller_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    HFONT font_ = nullptr, bold_ = nullptr;
    HBRUSH bg_ = nullptr;
    std::map<int, HWND> ctl_;
    std::map<int, std::wstring> textCache_;
    std::vector<Mode> comboModes_;   // resolution combo items after "Automática"
    bool updating_ = false;          // programmatic combo changes, ignore notifications
    bool messageIsError_ = false;
    ProcessStats stats_;
    ULONGLONG lastStatsSample_ = 0;
};

}  // namespace celmon
