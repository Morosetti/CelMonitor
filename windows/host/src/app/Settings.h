// Settings persisted in %LOCALAPPDATA%\CelMonitor\settings.ini.
//
// Each phone has its own profile (section "Celular <serial>"), so a phone that needs different options (transport,
// decoder tweaks, USB transfer size...) does not affect the others. A phone seen for the first time starts from the
// "Padrao" section, which the user can overwrite with "save as default" in the advanced settings.
#pragma once

#include <string>

#include "../session/HostSession.h"

namespace celmon {

enum class TransportMode { Auto, AdbOnly, AoaOnly };  // Auto: USB accessory (AOA) when possible, ADB otherwise

struct DeviceProfile {
    SessionSettings session;          // codec, fps, quality, orientation, resolution, decoder low latency, bitrate cap
    TransportMode transport = TransportMode::Auto;
    uint32_t usbMaxTransfer = 16000;  // AOA: largest bulk transfer to the phone (must fit its accessory read buffer)

    static DeviceProfile Load(const std::string& serial);  // empty serial = defaults section
    void Save(const std::string& serial) const;
    void SaveAsDefault() const { Save({}); }
};

struct AppSettings {
    std::string lastSerial;
    bool startMinimized = false;

    static AppSettings Load();
    void Save() const;
};

std::wstring SettingsFilePath();

// Small window preferences (e.g. "technical details expanded"), in the app section.
bool GetAppFlag(const wchar_t* key, bool def);
void SetAppFlag(const wchar_t* key, bool value);

}  // namespace celmon
