// USB via ADB: `adb forward tcp:N localabstract:celmonitor`, then a local TCP connection that adb tunnels over the
// cable. Requires "USB debugging" on the phone. The Android app only accepts connections from adbd (shell uid).
#pragma once

#include <winsock2.h>

#include <optional>

#include "Transport.h"

namespace celmon {

class AdbTransport {
public:
    // Finds adb.exe: next to CelMonitor.exe, in the Android SDK, or on PATH.
    Status Init();
    const std::wstring& AdbPath() const { return adb_; }

    // Phones visible to adb (any state), with the reason when they are not usable.
    std::vector<PhoneDevice> ListDevices();

    // Checks the app is installed, forwards the socket, starts the app if needed and connects.
    // On failure the Status message tells the user what to do.
    Status Connect(const std::string& serial, std::unique_ptr<IConnection>& out);

    // Installs the given APK (used when the app is missing).
    Status InstallApk(const std::string& serial, const std::wstring& apkPath);

private:
    // Runs adb with arguments; returns exit code, stdout+stderr in output. timeoutMs guards against hangs.
    int Run(const std::wstring& args, std::string& output, DWORD timeoutMs = 10000);
    std::optional<SOCKET> TryConnect(uint16_t port);

    std::wstring adb_;
};

std::wstring Widen(const std::string& s);

}  // namespace celmon
