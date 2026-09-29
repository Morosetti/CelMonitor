// User settings persisted in %LOCALAPPDATA%\CelMonitor\settings.ini.
#pragma once

#include <string>

#include "../session/HostSession.h"

namespace celmon {

struct AppSettings {
    SessionSettings session;
    bool autoConnect = true;
    std::string lastSerial;

    static AppSettings Load();
    void Save() const;
};

}  // namespace celmon
