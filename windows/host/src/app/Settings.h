// User settings persisted in %LOCALAPPDATA%\CelMonitor\settings.ini.
#pragma once

#include <string>

#include "../session/HostSession.h"

namespace celmon {

enum class TransportMode { Auto, AdbOnly };  // Auto: USB accessory (AOA) when possible, ADB otherwise

struct AppSettings {
    SessionSettings session;
    TransportMode transport = TransportMode::Auto;
    bool autoConnect = true;
    std::string lastSerial;

    static AppSettings Load();
    void Save() const;
};

}  // namespace celmon
