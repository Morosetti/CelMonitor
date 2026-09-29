#include "Controller.h"

#include "../util/Log.h"
#include "PhoneDetect.h"

namespace celmon {

const wchar_t* PhaseText(Phase p) {
    switch (p) {
    case Phase::Starting: return L"Iniciando...";
    case Phase::NoDriver: return L"Driver do monitor virtual não instalado";
    case Phase::NoAdb: return L"ADB não encontrado";
    case Phase::WaitingDevice: return L"Conecte o celular pelo cabo USB";
    case Phase::Unauthorized: return L"Permita a depuração USB no celular";
    case Phase::InstallingApp: return L"Instalando o app no celular...";
    case Phase::WaitingApp: return L"Aguardando o app no celular";
    case Phase::Connecting: return L"Conectando...";
    case Phase::Connected: return L"Conectado";
    case Phase::Disconnected: return L"Desconectado";
    }
    return L"";
}

Controller::Controller() : app_(AppSettings::Load()) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    profileSerial_ = app_.lastSerial;  // until a phone shows up, the UI edits the last phone's profile
    profile_ = DeviceProfile::Load(profileSerial_);
}

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

DeviceProfile Controller::Profile() const {
    std::lock_guard<std::mutex> lock(lock_);
    return profile_;
}

std::string Controller::ProfileSerial() const {
    std::lock_guard<std::mutex> lock(lock_);
    return profileSerial_;
}

void Controller::SetProfile(const DeviceProfile& p, bool alsoDefault) {
    std::lock_guard<std::mutex> lock(lock_);
    profile_ = p;
    profile_.Save(profileSerial_);
    if (alsoDefault) profile_.SaveAsDefault();
    aoaDisabledFor_.clear();  // transport options may have changed
    aoaFailures_ = 0;
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
        profile_.session.fps = fps;
        profile_.Save(profileSerial_);
    }
    std::lock_guard<std::mutex> lock(sessionLock_);
    if (session_) session_->SetFps(fps);
}

void Controller::SetQuality(uint32_t quality) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        profile_.session.quality = quality;
        profile_.Save(profileSerial_);
    }
    std::lock_guard<std::mutex> lock(sessionLock_);
    if (session_) session_->SetQuality(quality);
}

void Controller::SetCodec(proto::Codec codec) {
    std::lock_guard<std::mutex> lock(lock_);
    profile_.session.codec = codec;
    profile_.Save(profileSerial_);
}

void Controller::SetOrientation(bool portrait) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        auto& s = profile_.session;
        s.portrait = portrait;
        if (s.mode && (s.mode->height > s.mode->width) != portrait) s.mode = Mode{s.mode->height, s.mode->width, 60};
        profile_.Save(profileSerial_);
    }
    std::lock_guard<std::mutex> lock(sessionLock_);
    if (session_) session_->SetOrientation(portrait);
}

void Controller::SetResolution(std::optional<Mode> mode) {
    {
        std::lock_guard<std::mutex> lock(lock_);
        profile_.session.mode = mode;
        if (mode) profile_.session.portrait = mode->height > mode->width;
        profile_.Save(profileSerial_);
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
    SessionSettings settings;
    {
        std::lock_guard<std::mutex> lock(lock_);
        settings = profile_.session;
    }
    auto s = std::make_unique<HostSession>(std::move(conn), settings, ev);
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
    uint32_t maxTransfer;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (profile_.transport == TransportMode::AdbOnly) return false;
        if (profile_.transport == TransportMode::Auto && aoaDisabledFor_ == dev.serial) return false;
        maxTransfer = profile_.usbMaxTransfer;
    }
    // 1) Phone already in accessory mode: open the bulk channel.
    for (auto& acc : AoaTransport::Enumerate(GUID_DEVINTERFACE_CELMON_AOA)) {
        if (!acc.serial.empty() && acc.serial != dev.serial) continue;
        SetPhase(Phase::Connecting, L"Conectando ao " + dev.model + L"...");
        if (launch) adb_.LaunchApp(dev.serial);  // usually already opened by Android when the accessory attached
        std::unique_ptr<IConnection> conn;
        Status s = AoaTransport::Open(dev.serial, conn, maxTransfer);
        if (s.ok) {
            {
                std::lock_guard<std::mutex> lock(lock_);
                failures_ = 0;
                launchAllowed_ = false;
                if (app_.lastSerial != dev.serial) {
                    app_.lastSerial = dev.serial;
                    app_.Save();
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

    SetPhase(Phase::Connecting, L"Ativando o USB direto...");
    const std::wstring adbInstance = adbIf->instance;
    const UsbInterfaceInfo target = *adbIf;
    int protocol = 0;
    Status s = Status::Ok();
    bool appeared = false;
    // Returns true if the phone ignored the request (still enumerated in normal mode after 3 s).
    auto switchOnce = [&]() {
        adb_.StopServer();  // WinUSB is exclusive: adb must release the interface for a moment
        s = AoaTransport::SwitchToAccessory(target, &protocol);
        bool ignored = false;
        // Up to 20 s: the first time a phone enters accessory mode Windows installs the driver for it (~9 s measured).
        for (ULONGLONG start = GetTickCount64(); s.ok && !appeared && GetTickCount64() - start < 20000;) {
            Sleep(200);
            appeared = !AoaTransport::Enumerate(GUID_DEVINTERFACE_CELMON_AOA).empty();
            if (!appeared && GetTickCount64() - start > 3000) {
                bool stillNormal = false;
                for (auto& i : AoaTransport::Enumerate(GUID_DEVINTERFACE_ANDROID_ADB))
                    if (i.instance == adbInstance) stillNormal = true;
                if (stillNormal) { ignored = true; break; }  // a real switch drops the device off the bus at once
            }
        }
        adb_.StartServer();
        return ignored;
    };
    if (switchOnce() && !appeared) {
        // Android refuses accessory mode while /dev/usb_accessory is still held (e.g. by an app that kept the
        // accessory open across a cable pull). Stopping the app releases it; try once more.
        Log::Warn("phone ignored the AOA request; restarting the app on the phone and retrying");
        Sleep(1500);  // the restarted adb server needs a moment to see the phone again
        adb_.ForceStopApp(dev.serial);
        Sleep(1000);
        switchOnce();
        if (appeared) adb_.LaunchApp(dev.serial);
    }
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

// First-steps guide: what the user has to do next, with brand-specific instructions when the phone is recognized.
void Controller::UpdateHelp() {
    Phase phase;
    {
        std::lock_guard<std::mutex> lock(lock_);
        phase = state_.phase;
    }
    std::wstring help;
    switch (phase) {
    case Phase::NoDriver:
        help = L"O driver do monitor virtual não está instalado ou está parado.\nExecute o instalador do CelMonitor novamente.";
        break;
    case Phase::NoAdb:
        help = L"O componente ADB não foi encontrado.\nExecute o instalador do CelMonitor novamente (ele baixa o ADB do Google; é preciso internet).";
        break;
    case Phase::WaitingDevice: {
        auto phone = DetectUsbPhone();
        if (phone && !phone->debuggingEnabled && !phone->accessoryMode) {
            help = L"Encontramos um " + phone->brand + (phone->name.empty() ? L"" : L" (" + phone->name + L")") +
                   L" no USB, mas a Depuração USB está desligada. Ative assim:\n" + DebuggingSteps(phone->brand);
        } else {
            help = L"1) Conecte o celular ao PC com um cabo USB de dados (alguns cabos só carregam).\n"
                   L"2) Ative a Depuração USB no celular:\n" + DebuggingSteps(L"");
        }
        break;
    }
    case Phase::Unauthorized:
        help = L"Olhe a tela do celular: toque em \"Permitir\" e marque \"Sempre permitir deste computador\".\n"
               L"Se a pergunta não aparecer, desbloqueie o celular ou desconecte e reconecte o cabo.";
        break;
    case Phase::InstallingApp:
        help = L"Instalando o app CelMonitor no celular. Se o celular pedir confirmação, aceite.";
        break;
    case Phase::WaitingApp:
        help = L"Desbloqueie o celular e abra o app CelMonitor.\n"
               L"Se você saiu do modo monitor no celular, toque em \"Voltar ao modo monitor\".";
        break;
    case Phase::Disconnected:
        help = L"Clique em Conectar para voltar a usar o celular como monitor.";
        break;
    default:
        break;
    }
    std::lock_guard<std::mutex> lock(lock_);
    state_.help = help;
}

void Controller::Tick() {
    UpdateConnection();
    UpdateHelp();
}

void Controller::UpdateConnection() {
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
        lastSerial = app_.lastSerial;
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
        state_.status = unauthorized ? PhaseText(Phase::Unauthorized)
                                     : PhaseText(Phase::WaitingDevice);
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
        profileSerial_ = dev->serial;  // this phone's own settings from now on
        profile_ = DeviceProfile::Load(dev->serial);
    }
    if (userDisconnected) {
        SetPhase(Phase::Disconnected, L"");
        return;
    }

    bool launch;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (GetTickCount64() < nextAttempt_) return;
        launch = launchAllowed_;
    }
    if (TryAoa(*dev, launch)) return;
    TransportMode transport;
    {
        std::lock_guard<std::mutex> lock(lock_);
        transport = profile_.transport;
    }
    if (transport == TransportMode::AoaOnly) {
        // Profile forbids ADB: explain why the direct connection is not available instead of silently waiting.
        SetPhase(Phase::WaitingApp, L"USB direto indisponível");
        SetMessage(AoaDriverInstalled() ? L"O celular não entrou no modo acessório (perfil \"Somente USB direto\")."
                                        : L"O driver USB do CelMonitor não está instalado (perfil \"Somente USB direto\").", true);
        std::lock_guard<std::mutex> lock(lock_);
        nextAttempt_ = GetTickCount64() + 3000;
        return;
    }
    if (launch) SetPhase(Phase::Connecting, L"Conectando ao " + dev->model + L"...");

    std::unique_ptr<IConnection> conn;
    bool missing = false;
    Status s = adb_.Connect(dev->serial, conn, launch, &missing);
    if (s.ok) {
        {
            std::lock_guard<std::mutex> lock(lock_);
            failures_ = 0;
            launchAllowed_ = false;
            if (app_.lastSerial != dev->serial) {
                app_.lastSerial = dev->serial;
                app_.Save();
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
    state_.status = L"Abra o CelMonitor no celular";
    nextAttempt_ = GetTickCount64() + 1500;
}

}  // namespace celmon
