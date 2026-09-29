// Talks to the CelMonIdd driver: creates/removes the phone's virtual monitor and finds it in the Windows display
// configuration. Keeps the driver watchdog alive while a monitor exists.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../../../shared/celmon_driver_api.h"

namespace celmon {

struct Mode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t refreshHz = 60;
    bool operator==(const Mode& o) const { return width == o.width && height == o.height && refreshHz == o.refreshHz; }
};

// Outcome of an operation, with a message suitable for the UI (Portuguese) — nothing fails silently.
struct Status {
    bool ok = true;
    std::wstring message;
    static Status Ok() { return {}; }
    static Status Error(std::wstring m) { return {false, std::move(m)}; }
};

class VirtualDisplay {
public:
    VirtualDisplay();
    ~VirtualDisplay();
    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    // Opens the driver's device interface. Fails with a clear message if the driver is missing or stopped.
    Status Open();
    bool IsOpen() const { return device_ != INVALID_HANDLE_VALUE; }
    std::optional<CELMON_INFO> Info();

    // Creates the monitor. modes[preferred] becomes the initial resolution.
    Status Add(uint64_t deviceKey, const std::string& name, const std::vector<Mode>& modes, size_t preferred);
    Status Remove();
    bool HasMonitor() const { return monitorId_ != 0; }

    // Prefer rendering on the GPU that will encode (avoids cross-adapter copies on hybrid laptops).
    Status SetRenderAdapter(LUID luid);

    // GDI device name of our monitor (e.g. \\.\DISPLAY3), empty until Windows activates it.
    std::wstring GdiDeviceName() const;
    // Current desktop rectangle of our monitor, if active.
    std::optional<RECT> DesktopRect() const;
    std::optional<Mode> CurrentMode() const;
    Status SetMode(const Mode& mode);

private:
    bool Ioctl(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize, DWORD* returned = nullptr);
    void PingLoop();

    HANDLE device_ = INVALID_HANDLE_VALUE;
    std::mutex ioLock_;
    uint32_t monitorId_ = 0;
    uint32_t targetId_ = 0;
    LUID adapterLuid_{};
    std::atomic<bool> pinging_{false};
    std::thread pingThread_;
};

std::wstring Win32ErrorText(DWORD error);

}  // namespace celmon
