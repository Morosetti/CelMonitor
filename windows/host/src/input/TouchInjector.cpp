#include "TouchInjector.h"

#include <algorithm>

#include "../util/Log.h"

namespace celmon {

namespace {

// A finger held still produces no events on the phone, but Windows expects a touch to keep reporting while it is in
// contact (press-and-hold = right click needs it): repeat the current position at this interval.
constexpr ULONGLONG kKeepAliveMs = 40;

constexpr POINTER_FLAGS kDown = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
constexpr POINTER_FLAGS kUpdate = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
constexpr POINTER_FLAGS kUp = POINTER_FLAG_UP;
constexpr POINTER_FLAGS kCancel = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;

POINTER_TYPE_INFO Entry(uint32_t id, POINT pt, POINTER_FLAGS flags) {
    POINTER_TYPE_INFO t = {};
    t.type = PT_TOUCH;
    POINTER_TOUCH_INFO& ti = t.touchInfo;
    ti.pointerInfo.pointerType = PT_TOUCH;
    ti.pointerInfo.pointerId = id;
    ti.pointerInfo.ptPixelLocation = pt;
    ti.pointerInfo.pointerFlags = flags;
    ti.touchFlags = TOUCH_FLAG_NONE;
    ti.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    ti.rcContact = {pt.x - 2, pt.y - 2, pt.x + 2, pt.y + 2};
    ti.orientation = 90;
    ti.pressure = (flags & POINTER_FLAG_INCONTACT) ? 32000 : 0;
    return t;
}

// 0..65535 over the monitor -> desktop pixel inside it.
POINT Map(uint16_t x, uint16_t y, const RECT& a) {
    LONG w = std::max<LONG>(a.right - a.left, 1), h = std::max<LONG>(a.bottom - a.top, 1);
    return {a.left + MulDiv(x, w - 1, 65535), a.top + MulDiv(y, h - 1, 65535)};
}

}  // namespace

TouchInjector::TouchInjector() : keepAlive_([this] { KeepAlive(); }) {}

TouchInjector::~TouchInjector() {
    ReleaseAll();
    {
        std::lock_guard<std::mutex> lock(lock_);
        stop_ = true;
    }
    cv_.notify_all();
    keepAlive_.join();
    if (device_) DestroySyntheticPointerDevice(device_);
}

bool TouchInjector::Apply(const proto::InputTouch& msg, const RECT& area) {
    std::lock_guard<std::mutex> lock(lock_);
    if (!device_) {
        device_ = CreateSyntheticPointerDevice(PT_TOUCH, ULONG(kMax), POINTER_FEEDBACK_DEFAULT);
        if (!device_) {
            Log::Warn("touch: CreateSyntheticPointerDevice failed (%lu)", GetLastError());
            return false;
        }
    }
    POINTER_TYPE_INFO frame[kMax];
    size_t n = 0;
    bool mentioned[kMax] = {};
    for (const auto& c : msg.contacts) {
        if (c.id >= kMax || mentioned[c.id]) continue;  // the parser validates ids; a repeated id is ignored
        Contact& s = contacts_[c.id];
        POINT pt = Map(c.x, c.y, area);
        POINTER_FLAGS flags;
        switch (c.action) {
        case proto::TouchAction::Down: flags = s.down ? kUpdate : kDown; s.down = true; break;
        case proto::TouchAction::Move:
            if (!s.down) continue;  // a move for a finger we never saw go down
            flags = kUpdate;
            break;
        case proto::TouchAction::Up:
            if (!s.down) continue;
            flags = kUp;
            s.down = false;
            break;
        default:  // Cancel
            if (!s.down) continue;
            flags = kCancel;
            pt = s.pt;
            s.down = false;
            break;
        }
        s.pt = pt;
        mentioned[c.id] = true;
        frame[n++] = Entry(c.id, pt, flags);
    }
    // Windows wants every finger in contact in each frame: repeat the ones this message did not mention.
    for (uint32_t id = 0; id < kMax; ++id)
        if (contacts_[id].down && !mentioned[id]) frame[n++] = Entry(id, contacts_[id].pt, kUpdate);
    if (n == 0) return true;
    bool ok = Inject(frame, n);
    if (AnyDownLocked()) cv_.notify_all();
    return ok;
}

bool TouchInjector::Inject(POINTER_TYPE_INFO* frame, size_t n) {
    lastInject_ = GetTickCount64();
    if (InjectSyntheticPointerInput(device_, frame, UINT32(n))) return true;
    DWORD err = GetLastError();
    Log::Warn("touch: InjectSyntheticPointerInput failed (%lu)", err);
    for (auto& c : contacts_) c.down = false;  // Windows does not track them: start over with the next touch
    return false;
}

bool TouchInjector::AnyDown() const {
    std::lock_guard<std::mutex> lock(lock_);
    return AnyDownLocked();
}

bool TouchInjector::AnyDownLocked() const {
    for (const auto& c : contacts_)
        if (c.down) return true;
    return false;
}

void TouchInjector::ReleaseAll() {
    std::lock_guard<std::mutex> lock(lock_);
    POINTER_TYPE_INFO frame[kMax];
    size_t n = 0;
    for (uint32_t id = 0; id < kMax; ++id)
        if (contacts_[id].down) {
            frame[n++] = Entry(id, contacts_[id].pt, kCancel);
            contacts_[id].down = false;
        }
    if (n && device_) Inject(frame, n);
}

void TouchInjector::KeepAlive() {
    std::unique_lock<std::mutex> lock(lock_);
    while (!stop_) {
        if (!AnyDownLocked()) {
            cv_.wait(lock, [&] { return stop_ || AnyDownLocked(); });
            continue;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(kKeepAliveMs));
        if (stop_ || !AnyDownLocked() || GetTickCount64() - lastInject_ < kKeepAliveMs) continue;
        POINTER_TYPE_INFO frame[kMax];
        size_t n = 0;
        for (uint32_t id = 0; id < kMax; ++id)
            if (contacts_[id].down) frame[n++] = Entry(id, contacts_[id].pt, kUpdate);
        Inject(frame, n);
    }
}

}  // namespace celmon
