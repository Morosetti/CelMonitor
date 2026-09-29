// Application logic behind the window: finds the phone, installs/opens the app, runs sessions, reconnects.
// Runs on its own thread; the UI polls State() and calls the user actions.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "../session/HostSession.h"
#include "../transport/AdbTransport.h"
#include "Settings.h"

namespace celmon {

enum class Phase {
    Starting,
    NoDriver,        // virtual display driver missing/stopped
    NoAdb,           // adb.exe not found
    WaitingDevice,   // no phone on USB
    Unauthorized,    // phone asks "Allow USB debugging?"
    InstallingApp,
    WaitingApp,      // phone present, app not in monitor mode (paused by the user / not opened)
    Connecting,
    Connected,       // session running (see SessionInfo for its own state)
    Disconnected,    // user pressed Disconnect on the PC
};

struct ControllerState {
    Phase phase = Phase::Starting;
    std::wstring status;        // short text for the status line
    std::wstring message;       // last warning/error, empty if none
    bool messageIsError = false;
    std::string serial;
    std::wstring deviceModel;
    bool hasSession = false;
    SessionInfo session;
};

class Controller {
public:
    Controller();
    ~Controller();

    void Start();
    void Shutdown();

    ControllerState State() const;
    AppSettings Settings() const;

    // User actions (UI thread).
    void Connect();
    void Disconnect();
    void SetFps(uint32_t fps);
    void SetQuality(uint32_t quality);
    void SetCodec(proto::Codec codec);  // applies on the next connection
    void SetOrientation(bool portrait);
    void SetResolution(std::optional<Mode> mode);  // nullopt = automatic (native)

private:
    void Run();
    void Tick();
    bool CheckDriver();
    void StartSession(std::unique_ptr<IConnection> conn);
    void ReapSession();
    void SetPhase(Phase p, const std::wstring& status);
    void SetMessage(const std::wstring& m, bool error);
    std::wstring FindApk() const;

    // Two locks, never nested: lock_ guards state/settings/policy; sessionLock_ guards session_'s lifetime.
    // Session callbacks (onWarning/onEnded) take lock_, so calls into the session must not hold it.
    mutable std::mutex lock_;
    mutable std::mutex sessionLock_;
    ControllerState state_;
    AppSettings settings_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    HANDLE wake_ = nullptr;

    AdbTransport adb_;
    bool adbReady_ = false;
    bool driverOk_ = false;
    ULONGLONG nextDriverCheck_ = 0;

    std::unique_ptr<HostSession> session_;
    std::atomic<bool> sessionEnded_{false};
    std::wstring sessionEndWhy_;
    bool sessionEndByPhoneUser_ = false;

    // Reconnection policy
    std::string knownSerial_;          // phone currently plugged in
    bool launchAllowed_ = true;        // may open the app on the phone (new device, user click, after an error)
    bool userDisconnected_ = false;
    int failures_ = 0;
    ULONGLONG nextAttempt_ = 0;
    bool installTried_ = false;
};

const wchar_t* PhaseText(Phase p);

}  // namespace celmon
