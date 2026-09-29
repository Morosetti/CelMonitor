// USB via Android Open Accessory 2.0 + WinUSB: a direct bulk channel to the app, no adb server in the data path.
//
// Bootstrap (limitation of Windows, see docs/ANALISE_TECNICA.md §4): a phone in normal mode is bound to the MTP
// driver, so the switch-to-accessory control requests go through the only WinUSB-bound interface it exposes,
// the ADB interface (needs "USB debugging"). After the switch the phone re-enumerates as 18D1:2D00/2D01, which
// windows/driver/CelMonAoa/CelMonAoa.inf binds to WinUSB for every phone brand.
#pragma once

#include <windows.h>
#include <winusb.h>

#include <string>
#include <vector>

#include "Transport.h"

namespace celmon {

// {5076744A-ED79-469F-8E71-C2131A5B33B4} — declared by CelMonAoa.inf for the accessory interface.
static const GUID GUID_DEVINTERFACE_CELMON_AOA = {0x5076744a, 0xed79, 0x469f, {0x8e, 0x71, 0xc2, 0x13, 0x1a, 0x5b, 0x33, 0xb4}};
// Standard Android ADB interface GUID (Google USB driver / MS OS descriptors of adbd).
static const GUID GUID_DEVINTERFACE_ANDROID_ADB = {0xf72fe0d4, 0xcbcb, 0x407d, {0x88, 0x14, 0x9e, 0xd6, 0x73, 0xd0, 0xdd, 0x6b}};

struct UsbInterfaceInfo {
    std::wstring path;     // device interface path for CreateFile
    std::wstring instance; // PnP instance id, e.g. USB\VID_2717&PID_FF48&MI_01\6&...
    uint16_t vid = 0, pid = 0;
    std::string serial;    // USB serial of the parent device (same as `adb devices` serial)
};

class AoaTransport {
public:
    static std::vector<UsbInterfaceInfo> Enumerate(const GUID& interfaceGuid);

    // True if CelMonAoa.inf is in the driver store (otherwise switching would strand the phone in accessory
    // mode with no driver on the PC).
    static bool DriverInstalled();

    // Sends the AOA handshake through the phone's ADB interface. adb.exe must not hold the interface
    // (the caller stops the adb server first). protocolOut receives the AOA version (1 or 2).
    static Status SwitchToAccessory(const UsbInterfaceInfo& adbInterface, int* protocolOut = nullptr);

    // Opens a phone already in accessory mode. serial may be empty (first one found). maxTransfer bounds every bulk
    // transfer to the phone (its accessory read buffer; 16 KB on most kernels, configurable per phone).
    static Status Open(const std::string& serial, std::unique_ptr<IConnection>& out, uint32_t maxTransfer = 16000);
};

}  // namespace celmon
