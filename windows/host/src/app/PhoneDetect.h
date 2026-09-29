// Recognizes an Android phone on USB even before USB debugging is enabled (from the USB vendor ID Windows already
// sees through MTP), so the first-run guide can give brand-specific instructions.
#pragma once

#include <optional>
#include <string>

namespace celmon {

struct DetectedPhone {
    std::wstring brand;        // "Xiaomi", "Samsung"... or "Android"
    std::wstring name;         // device description when available (e.g. "MI MAX 3")
    bool debuggingEnabled = false;  // an ADB interface is exposed
    bool accessoryMode = false;     // 18D1:2D0x (already switched by CelMonitor)
};

std::optional<DetectedPhone> DetectUsbPhone();

// Where "USB debugging" lives on this brand, as numbered steps (Portuguese, for the UI).
std::wstring DebuggingSteps(const std::wstring& brand);

}  // namespace celmon
