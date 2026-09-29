// Animated test window (moving bar + frame counter + clock) placed on a given desktop rectangle.
// Gives the capture something that changes every frame, and a visible clock for latency checks.
#pragma once

#include <windows.h>
#include <dwmapi.h>

#include <atomic>
#include <cstdio>
#include <thread>

class TestPattern {
public:
    void Start(RECT area) {
        area_ = area;
        running_ = true;
        thread_ = std::thread([this] { Run(); });
    }
    void Stop() {
        running_ = false;
        if (hwnd_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
    }
    ~TestPattern() { Stop(); }
    unsigned long long Frames() const { return frame_; }

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<TestPattern*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_PAINT && self) { self->Paint(h); return 0; }
        if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
        if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(h, m, w, l);
    }

    void Paint(HWND h) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        // Color bars (checks color conversion) + a moving white bar (checks motion) + text.
        const COLORREF bars[] = {RGB(255, 255, 255), RGB(255, 255, 0), RGB(0, 255, 255), RGB(0, 255, 0),
                                 RGB(255, 0, 255), RGB(255, 0, 0), RGB(0, 0, 255), RGB(0, 0, 0)};
        for (int i = 0; i < 8; ++i) {
            RECT b = {rc.right * i / 8, 0, rc.right * (i + 1) / 8, rc.bottom * 2 / 3};
            HBRUSH br = CreateSolidBrush(bars[i]);
            FillRect(mem, &b, br);
            DeleteObject(br);
        }
        RECT low = {0, rc.bottom * 2 / 3, rc.right, rc.bottom};
        FillRect(mem, &low, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
        int x = int((frame_ * 12) % (rc.right + 60)) - 60;
        RECT bar = {x, rc.bottom * 2 / 3, x + 60, rc.bottom};
        FillRect(mem, &bar, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t text[128];
        swprintf_s(text, L"CelMonitor  frame %llu  %02d:%02d:%02d.%03d", frame_.load(), t.wHour, t.wMinute, t.wSecond,
                   t.wMilliseconds);
        HFONT font = CreateFontW(rc.bottom / 12, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Consolas");
        HGDIOBJ oldFont = SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        RECT tr = {20, rc.bottom * 2 / 3 + 20, rc.right, rc.bottom};
        DrawTextW(mem, text, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);
        SelectObject(mem, oldFont);
        DeleteObject(font);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
    }

    void Run() {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"CelMonTestPattern";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"CelMonitor test", WS_POPUP | WS_VISIBLE,
                                area_.left, area_.top, area_.right - area_.left, area_.bottom - area_.top, nullptr, nullptr,
                                wc.hInstance, nullptr);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        // Redraw once per desktop composition (monitor vsync): 60 fps on a 60 Hz desktop.
        MSG msg;
        while (running_) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) running_ = false;
                DispatchMessageW(&msg);
            }
            if (!running_) break;
            ++frame_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
            DwmFlush();
        }
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

    RECT area_{};
    std::atomic<bool> running_{false};
    std::atomic<unsigned long long> frame_{0};
    HWND hwnd_ = nullptr;
    std::thread thread_;
};
