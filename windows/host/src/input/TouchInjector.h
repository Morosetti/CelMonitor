// The phone's touches as real Windows touch input (synthetic pointer device), mapped onto the virtual monitor's
// rectangle on the desktop. Windows does the rest like on a touchscreen: tap = click, press and hold = right click,
// drag = scroll or drag, pinch = zoom where the app supports it.
//
// Only what the phone is allowed to do is injected: coordinates are normalised (0..65535) and always land inside the
// virtual monitor, at most kMaxTouchContacts fingers, and the session only calls Apply() while streaming.
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "protocol/celmon_protocol.h"

namespace celmon {

class TouchInjector {
public:
    TouchInjector();
    ~TouchInjector();  // lifts any finger still down
    TouchInjector(const TouchInjector&) = delete;
    TouchInjector& operator=(const TouchInjector&) = delete;

    // One INPUT_TOUCH message; area = the virtual monitor on the desktop, in physical pixels.
    // Returns false when Windows refused the injection (typically an administrator window in front, or the secure
    // desktop): the fingers are then forgotten and the next touch starts over.
    bool Apply(const proto::InputTouch& msg, const RECT& area);
    bool AnyDown() const;
    void ReleaseAll();  // e.g. the monitor changed size or position under the fingers

private:
    struct Contact {
        bool down = false;
        POINT pt = {};
    };
    static constexpr size_t kMax = proto::kMaxTouchContacts;

    bool Inject(POINTER_TYPE_INFO* frame, size_t n);  // lock_ held
    bool AnyDownLocked() const;
    void KeepAlive();

    mutable std::mutex lock_;
    HSYNTHETICPOINTERDEVICE device_ = nullptr;
    std::array<Contact, kMax> contacts_{};
    ULONGLONG lastInject_ = 0;

    std::condition_variable cv_;
    bool stop_ = false;
    std::thread keepAlive_;
};

}  // namespace celmon
