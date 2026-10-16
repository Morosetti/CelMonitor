// "Send the active window to the phone and back" (global shortcut, Ctrl+Alt+M by default).
//
// Unlike Windows' own Win+Shift+Left/Right, which steps through every monitor in turn, this toggles between the
// phone's virtual monitor and the monitor the window came from. Position and size are kept proportional to the work
// area, maximized windows stay maximized.
#pragma once

#include <windows.h>

#include <map>
#include <string>

namespace celmon {

class WindowMover {
public:
    // phoneDisplay: GDI name of the virtual monitor ("\\.\DISPLAY5"). Returns an empty string on success, otherwise
    // a short message for the user.
    std::wstring ToggleForeground(const std::wstring& phoneDisplay);

private:
    std::map<HWND, std::wstring> origin_;  // window -> monitor it was on before going to the phone
};

}  // namespace celmon
