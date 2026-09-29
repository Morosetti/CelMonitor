#include "Controller.h"

#include "../util/Log.h"

namespace celmon {

const wchar_t* PhaseText(Phase p) {
    switch (p) {
    case Phase::Starting: return L"Iniciando...";
    case Phase::NoDriver: return L"Driver do monitor virtual não instalado";
    case Phase::NoAdb: return L"ADB não encontrado";
    case Phase::WaitingDevice: return L"Conecte o celular ao PC via USB";
    case Phase::Unauthorized: return L"Autorize a depuração USB no celular";
    case Phase::InstallingApp: return L"Instalando o app no celular...";
    case Phase::WaitingApp: return L"Aguardando o app no celular";
    case Phase::Connecting: return L"Conectando...";
    case Phase::Connected: return L"Conectado";
    case Phase::Disconnected: return L"Desconectado";
    }
    return L"";
}

Controller::Controller() : settings_(AppSettings::Load()) { wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

Controller::~Controller() {
    Shutdown();
    CloseHandle(wake_);
}

void Controller::Start() {
    running_ = true;
    thread_ = std::thread(&Controller::Run, this);
}

void Controller::Shutdown() {
    if (!running_.exchange(false)) return;
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
}

ControllerState Controller::State() const {
    ControllerState s;
    {
        std::lock_guard<std::mutex> lock(lock_);
        s = state_;
    }
    std::lock_guard<std::mutex> lock(sessionLock_);
    if (session_) {
        s.hasSession = true;
        s.session = session_->Info();
    }
    return s;
}

AppSettings Controller::Settings() const {
    std::lock_guard<std::mutex> lock(lock_);
    return settings_;
}

void Controller::SetPhase(Phase p, const std::wstring& status) {
    std::lock_guard<std::mutex> lock(lock_);
    state_.phase = p;
    state_.status = status.empty() ? PhaseText(p) : status;
}

void Controller::SetMessage(const std::wstring& m, bool error) {
    std::lock_guard<std::mutex> lock(lock_);
    state_.message = m;
    state_.messageIsError = error;
}

// ------------------------------------------------------------------------------------------------ user actions

void Controller::Connect() {
    {
        std::lock_guard<std::mutex> lock(lock_);
        userDisconnected_ = false;
        launchAllowed_ = true;
        failures_ = 0;
        nextAttempt_ = 0;
        installTried_ = false;
        aoaFailures_ = 0;
        aoaDisabledFor_.clear();
        nextAoaDriverCheck_ = 0;  // the driver may have just been installed
        state_.message.clear();
    }
    SetEvent(wake_);
}

void Controller::Disconnect() {
    {
        std::lock_guard<std::mutex> lock(lock_);
        userDisconnected_ = true;
    }
    std::lock_guard<std::mutex> lock(sessionLock_);
    if (session_) session_->Stop(L"desconectado no PC");
}

void Controller::SetFps(uint32_t fps) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        settings_.session.fps = fps;
        settings_.Save();
    }
    std::lock_guard<std::mutex> lock(sessionLock_);
    if (session_) session_->SetFps(fps);
}

void Controller::SetQuality(uint32_t quality) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        settings_.session.quality = quality;
        settings_.Save();
    }
    std::lock_guard<std::mutex> lock(sessionLock_);
    if (session_) session_->SetQuality(quality);
}

void Controller::SetCodec(proto::Codec codec) {
    std::lock_guard<std::mutex> lock(lock_);
    settings_.session.codec = codec;
    settings_.Save();
}

void Controller::SetOrientation(bool portrait) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        auto& s = settings_.session;
        s.portrait = portrait;
        if (s.mode && (s.mode->height > s.mode->width) != portrait) s.mode = Mode{s.mode->height, s.mode->width, 60};
        settings_.Save();
    }
    std::lock_guard<std::mutex> lock(sessionLock_);
    if (session_) session_->SetOrientation(portrait);
}

void Controller::SetResolution(std::optional<Mode> mode) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        settings_.session.mode = mode;
        if (mode) settings_.session.portrait = mode->height > mode->width;
        settings_.Save();
    }
    Status s = Status::Ok();
    {
        std::lock_guard<std::mutex> lock(sessionLock_);
        if (session_ && mode) s = session_->SetResolution(*mode);
    }
    if (!s.ok) SetMessage(s.message, true);
}

// ------------------------------------------------------------------------------------------------ worker

void Controller::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running_) {
        Tick();
        WaitForSingleObject(wake_, 1000);
    }
    std::unique_ptr<HostSession> s;
    {
        std::lock_guard<std::mutex> lock(sessionLock_);
        s = std::move(session_);
    }
    if (s) s->Stop(L"programa do PC encerrado");
    s.reset();  // joins its threads, removes the monitor
    CoUninitialize();
}

bool Controller::CheckDriver() {
    if (driverOk_) return true;
    if (GetTickCount64() < nextDriverCheck_) return false;
    nextDriverCheck_ = GetTickCount64() + 5000;
    VirtualDisplay vd;
    Status s = vd.Open();
    if (!s.ok) {
        SetPhase(Phase::NoDriver, L"");
        SetMessage(s.message, true);
        return false;
    }
    driverOk_ = true;
    SetMessage(L"", false);
    return true;
}

std::wstring Controller::FindApk() const {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    // Installed layout first, then the development tree (build\host\Release -> repo root).
    for (std::wstring p : {dir + L"\\CelMonitor.apk", dir + L"\\..\\..\\..\\android\\app\\build\\outputs\\apk\\release\\app-release.apk",
                           dir + L"\\..\\..\\..\\android\\app\\build\\outputs\\apk\\debug\\app-debug.apk"})
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    return {};
}

void Controller::StartSession(std::unique_ptr<IConnection> conn) {
    HostSession::Events ev;
    ev.onWarning = [this](const Status& w) { SetMessage(w.message, false); };
    ev.onEnded = [this](const std::wstring& why, bool byPhoneUser) {
        {
            std::lock_guard<std::mutex> lock(lock_);
            sessionEndWhy_ = why;
            sessionEndByPhoneUser_ = byPhoneUser;
        }
        sessionEnded_ = true;
        SetEvent(wake_);
    };
    AppSettings settings = Settings();
    auto s = std::make_unique<HostSession>(std::move(conn), settings.session, ev);
    s->Start();
    {
        std::lock_guard<std::mutex> lock(sessionLock_);
        session_ = std::move(s);
    }
    std::lock_guard<std::mutex> lock(lock_);
    state_.phase = Phase::Connected;
    state_.status = PhaseText(Phase::Connected);
    state_.message.clear();
}

void Controller::ReapSession() {
    if (!sessionEnded_.exchange(false)) return;
    std::unique_ptr<HostSession> s;
    std::wstring why;
    bool byPhoneUser, byPcUser;
    {
        std::lock_guard<std::mutex> lock(sessionLock_);
        s = std::move(session_);
    }
    {
        std::lock_guard<std::mutex> lock(lock_);
        why = sessionEndWhy_;
        byPhoneUser = sessionEndByPhoneUser_;
        byPcUser = userDisconnected_;
    }
    SessionInfo last = s->Info();
    s.reset();  // joins the session threads; the monitor is already removed
    bool gotHello = !last.model.empty();
    bool wasAoa = last.transport.find(L"AOA") != std::wstring::npos;
    if (wasAoa && !byPcUser && !byPhoneUser) {
        std::lock_guard<std::mutex> lock(lock_);
        if (gotHello) aoaFailures_ = 0;
        else if (++aoaFailures_ >= 3) {
            // The accessory opens but the app never answers (old app version, permission refused...): use ADB.
            aoaDisabledFor_ = knownSerial_;
            Log::Warn("USB accessory failed %d times; falling back to ADB for %s", aoaFailures_, knownSerial_.c_str());
        }
    }
    char buf[512];
    WideCharToMultiByte(CP_UTF8, 0, why.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    Log::Info("session reaped: %s (phone user=%d, pc user=%d)", buf, int(byPhoneUser), int(byPcUser));

    std::lock_guard<std::mutex> lock(lock_);
    if (byPcUser) {
        state_.message.clear();
    } else if (byPhoneUser) {
        // Respect the phone: don't reopen the app; reconnect only when monitor mode is resumed there.
        launchAllowed_ = false;
        state_.message = L"Modo monitor encerrado no celular. Toque em \"Voltar ao modo monitor\" no celular ou clique em Conectar.";
        state_.messageIsError = false;
    } else {
        // Failure (cable, app crash, timeout): retry, reopening the app, with a growing delay.
        launchAllowed_ = true;
        ++failures_;
        nextAttempt_ = GetTickCount64() + std::min<ULONGLONG>(2000ull * failures_, 10000);
        state_.message = L"Conexão encerrada: " + why;
        state_.messageIsError = true;
    }
}

bool Controller::AoaDriverInstalled() {
    if (GetTickCount64() >= nextAoaDriverCheck_) {
        aoaDriver_ = AoaTransport::DriverInstalled();
        nextAoaDriverCheck_ = GetTickCount64() + 30000;
    }
    return aoaDriver_;
}

// Direct USB (Android Open Accessory). Returns true when this tick is handled (session started, switch in progress
// or a retry scheduled); false to continue with ADB.
bool Controller::TryAoa(const PhoneDevice& dev, bool launch) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (settings_.transport != TransportMode::Auto || aoaDisabledFor_ == dev.serial) return false;
    }
    // 1) Phone already in accessory mode: open the bulk channel.
    for (auto& acc : AoaTransport::Enumerate(GUID_DEVINTERFACE_CELMON_AOA)) {
        if (!acc.serial.empty() && acc.serial != dev.serial) continue;
        SetPhase(Phase::Connecting, L"Conectando ao " + dev.model + L" por USB direto...");
        if (launch) adb_.LaunchApp(dev.serial);  // usually already opened by Android when the accessory attached
        std::unique_ptr<IConnection> conn;
        Status s = AoaTransport::Open(dev.serial, conn);
        if (s.ok) {
            {
                std::lock_guard<std::mutex> lock(lock_);
                failures_ = 0;
                launchAllowed_ = false;
                if (settings_.lastSerial != dev.serial) {
                    settings_.lastSerial = dev.serial;
                    settings_.Save();
                }
            }
            StartSession(std::move(conn));
            return true;
        }
        std::lock_guard<std::mutex> lock(lock_);
        state_.message = s.message;
        state_.messageIsError = true;
        if (++aoaFailures_ >= 3) aoaDisabledFor_ = dev.serial;
        nextAttempt_ = GetTickCount64() + 2000;
        return true;
    }
    // 2) Normal mode: switch to accessory mode through the ADB interface, if the PC has the driver for it.
    if (!AoaDriverInstalled()) return false;
    const UsbInterfaceInfo* adbIf = nullptr;
    auto adbIfs = AoaTransport::Enumerate(GUID_DEVINTERFACE_ANDROID_ADB);
    for (auto& i : adbIfs)
        if (i.serial == dev.serial && !(i.vid == 0x18D1 && (i.pid == 0x2D00 || i.pid == 0x2D01))) adbIf = &i;
    if (!adbIf) return false;

    SetPhase(Phase::Connecting, L"Ativando a conexão USB direta (modo acessório)...");
    adb_.StopServer();  // WinUSB is exclusive: adb must release the interface for a moment
    int protocol = 0;
    Status s = AoaTransport::SwitchToAccessory(*adbIf, &protocol);
    bool appeared = false;
    // Up to 20 s: the first time a phone enters accessory mode Windows installs the driver for it (~9 s measured).
    for (int i = 0; s.ok && i < 100 && !appeared; ++i) {
        Sleep(200);
        appeared = !AoaTransport::Enumerate(GUID_DEVINTERFACE_CELMON_AOA).empty();
    }
    adb_.StartServer();
    std::lock_guard<std::mutex> lock(lock_);
    if (appeared) {
        nextAttempt_ = 0;  // next tick opens the accessory
        SetEvent(wake_);
        return true;
    }
    aoaDisabledFor_ = dev.serial;
    state_.message = (s.ok ? std::wstring(L"O celular não entrou no modo acessório USB") : s.message) + L". Usando ADB.";
    state_.messageIsError = false;
    Log::Warn("AOA switch failed (protocol %d); using ADB", protocol);
    return true;  // adb server was restarted; the ADB attempt happens on the next tick
}

void Controller::SetTransport(TransportMode mode) {
    std::lock_guard<std::mutex> lock(lock_);
    settings_.transport = mode;
    settings_.Save();
    aoaDisabledFor_.clear();
    aoaFailures_ = 0;
}

void Controller::Tick() {
    ReapSession();
    {
        std::lock_guard<std::mutex> lock(sessionLock_);
        if (session_) return;
    }
    if (!CheckDriver()) return;
    if (!adbReady_) {
        Status s = adb_.Init();
        if (!s.ok) {
            SetPhase(Phase::NoAdb, L"");
            SetMessage(s.message, true);
            return;
        }
        adbReady_ = true;
    }

    bool userDisconnected;
    std::string lastSerial;
    {
        std::lock_guard<std::mutex> lock(lock_);
        userDisconnected = userDisconnected_;
        lastSerial = settings_.lastSerial;
    }
    auto devices = adb_.ListDevices();
    const PhoneDevice* dev = nullptr;
    bool unauthorized = false;
    for (auto& d : devices) {
        if (d.state == DeviceState::Unauthorized) unauthorized = true;
        if (d.state != DeviceState::Ready) continue;
        if (!dev || d.serial == lastSerial) dev = &d;
    }
    if (!dev) {
        if (!knownSerial_.empty()) {
            adb_.Forget(knownSerial_);
            knownSerial_.clear();
        }
        std::lock_guard<std::mutex> lock(lock_);
        launchAllowed_ = true;
        failures_ = 0;
        installTried_ = false;
        aoaFailures_ = 0;
        aoaDisabledFor_.clear();
        state_.serial.clear();
        state_.deviceModel.clear();
        state_.phase = unauthorized ? Phase::Unauthorized : Phase::WaitingDevice;
        state_.status = unauthorized ? L"Toque em \"Permitir\" na mensagem de depuração USB do celular"
                                     : L"Conecte o celular ao PC via USB (com a Depuração USB ativada)";
        return;
    }
    if (dev->serial != knownSerial_) {
        knownSerial_ = dev->serial;
        std::lock_guard<std::mutex> lock(lock_);
        launchAllowed_ = true;  // a phone just arrived: opening the app is what the user expects
        failures_ = 0;
        installTried_ = false;
        nextAttempt_ = 0;
        state_.serial = dev->serial;
        state_.deviceModel = dev->model;
    }
    if (userDisconnected) {
        SetPhase(Phase::Disconnected, L"Desconectado. Clique em Conectar para usar o celular como monitor.");
        return;
    }

    bool launch;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (GetTickCount64() < nextAttempt_) return;
        launch = launchAllowed_;
    }
    if (TryAoa(*dev, launch)) return;
    if (launch) SetPhase(Phase::Connecting, L"Conectando ao " + dev->model + L"...");

    std::unique_ptr<IConnection> conn;
    bool missing = false;
    Status s = adb_.Connect(dev->serial, conn, launch, &missing);
    if (s.ok) {
        {
            std::lock_guard<std::mutex> lock(lock_);
            failures_ = 0;
            launchAllowed_ = false;
            if (settings_.lastSerial != dev->serial) {
                settings_.lastSerial = dev->serial;
                settings_.Save();
            }
        }
        StartSession(std::move(conn));
        return;
    }

    std::lock_guard<std::mutex> lock(lock_);
    if (missing) {
        if (installTried_) {
            state_.phase = Phase::WaitingApp;
            state_.status = L"Instale o app CelMonitor no celular";
            nextAttempt_ = GetTickCount64() + 5000;
            return;
        }
        installTried_ = true;
        std::wstring apk = FindApk();
        if (apk.empty()) {
            state_.phase = Phase::WaitingApp;
            state_.status = L"Instale o app CelMonitor no celular";
            state_.message = L"O app não está instalado no celular e o CelMonitor.apk não foi encontrado ao lado do programa.";
            state_.messageIsError = true;
            return;
        }
        state_.phase = Phase::InstallingApp;
        state_.status = PhaseText(Phase::InstallingApp);
        lock_.unlock();
        Status is = adb_.InstallApk(dev->serial, apk);
        lock_.lock();
        state_.message = is.ok ? L"App instalado no celular." : is.message;
        state_.messageIsError = !is.ok;
        nextAttempt_ = 0;
        launchAllowed_ = true;
        return;
    }
    if (launch) {
        // Could not open/reach the app: stop forcing it; keep polling quietly until it listens.
        launchAllowed_ = false;
        ++failures_;
        state_.message = s.message;
        state_.messageIsError = true;
    }
    state_.phase = Phase::WaitingApp;
    state_.status = L"Abra o CelMonitor no celular (ou toque em \"Voltar ao modo monitor\")";
    nextAttempt_ = GetTickCount64() + 1500;
}

}  // namespace celmon
