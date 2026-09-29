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

    // Checks the app is installed, forwards the socket, starts the app if needed (launchApp) and connects.
    // With launchApp=false it only connects if the app is already listening (used for background polling, so the
    // app is never forced open after the user left monitor mode on the phone).
    // On failure the Status message tells the user what to do; appMissing is set when the app is not installed.
    Status Connect(const std::string& serial, std::unique_ptr<IConnection>& out, bool launchApp = true,
                   bool* appMissing = nullptr);

    // Drops cached state for a phone that went away (forward port).
    void Forget(const std::string& serial);

    // Installs the given APK (used when the app is missing).
    Status InstallApk(const std::string& serial, const std::wstring& apkPath);

private:
    // Runs adb with arguments; returns exit code, stdout+stderr in output. timeoutMs guards against hangs.
    int Run(const std::wstring& args, std::string& output, DWORD timeoutMs = 10000);
    std::optional<SOCKET> TryConnect(uint16_t port);

    std::wstring adb_;
    std::string preparedSerial_;   // phone whose app presence + forward are already set up
    uint16_t preparedPort_ = 0;
    bool lastConnectRefused_ = false;
};

std::wstring Widen(const std::string& s);

}  // namespace celmon
