#include "WindowMover.h"

#include <algorithm>

#include "../util/Log.h"

namespace celmon {

namespace {

struct MonitorFind {
    const std::wstring* name;
    HMONITOR found;
};

HMONITOR MonitorByName(const std::wstring& name) {
    MonitorFind f{&name, nullptr};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        auto* f = reinterpret_cast<MonitorFind*>(p);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi) && _wcsicmp(mi.szDevice, f->name->c_str()) == 0) {
            f->found = m;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.found;
}

MONITORINFOEXW Info(HMONITOR m) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    return mi;
}

// The desktop, the taskbar and similar shell windows are not "programs" to move.
bool Movable(HWND w) {
    if (!w || !IsWindowVisible(w) || IsIconic(w)) return false;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                                 L"Windows.UI.Core.CoreWindow"})
        if (wcscmp(cls, shell) == 0) return false;
    return true;
}

// Same place relative to the work area, shrunk if it does not fit.
RECT MapRect(const RECT& r, const RECT& from, const RECT& to) {
    const LONG fw = std::max<LONG>(from.right - from.left, 1), fh = std::max<LONG>(from.bottom - from.top, 1);
    const LONG tw = to.right - to.left, th = to.bottom - to.top;
    LONG w = std::min<LONG>(r.right - r.left, tw), h = std::min<LONG>(r.bottom - r.top, th);
    LONG x = to.left + MulDiv(r.left - from.left, tw, fw), y = to.top + MulDiv(r.top - from.top, th, fh);
    x = std::clamp<LONG>(x, to.left, to.right - w);
    y = std::clamp<LONG>(y, to.top, to.bottom - h);
    return {x, y, x + w, y + h};
}

}  // namespace

std::wstring WindowMover::ToggleForeground(const std::wstring& phoneDisplay) {
    HWND w = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!Movable(w)) return L"Clique primeiro na janela que você quer mover.";
    HMONITOR phone = MonitorByName(phoneDisplay);
    if (!phone) return L"O monitor do celular não está ativo.";

    // Forget windows that no longer exist.
    for (auto it = origin_.begin(); it != origin_.end();) it = IsWindow(it->first) ? std::next(it) : origin_.erase(it);

    HMONITOR current = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    HMONITOR target;
    if (current != phone) {
        target = phone;
        origin_[w] = Info(current).szDevice;
    } else {
        // Back to where it came from; if unknown (or that monitor is gone), the monitor under the mouse, else the
        // main monitor.
        auto it = origin_.find(w);
        target = it != origin_.end() ? MonitorByName(it->second) : nullptr;
        if (!target) {
            POINT pt;
            GetCursorPos(&pt);
            target = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
            if (target == phone) target = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        }
        if (target == phone) return L"Não há outro monitor para onde mover a janela.";
        origin_.erase(w);
    }

    const RECT from = Info(current).rcWork, to = Info(target).rcWork;
    const bool maximized = IsZoomed(w);
    if (maximized) ShowWindow(w, SW_SHOWNOACTIVATE);  // restore, move, maximize again on the new monitor
    RECT r;
    GetWindowRect(w, &r);
    RECT n = MapRect(r, from, to);
    if (!SetWindowPos(w, nullptr, n.left, n.top, n.right - n.left, n.bottom - n.top, SWP_NOZORDER | SWP_NOACTIVATE)) {
        DWORD err = GetLastError();
        Log::Warn("move window failed (%lu)", err);
        if (maximized) ShowWindow(w, SW_MAXIMIZE);
        return err == ERROR_ACCESS_DENIED ? L"Essa janela está aberta como administrador e não pode ser movida pelo CelMonitor."
                                          : L"Não foi possível mover a janela.";
    }
    if (maximized) ShowWindow(w, SW_MAXIMIZE);
    SetForegroundWindow(w);
    return {};
}

}  // namespace celmon
