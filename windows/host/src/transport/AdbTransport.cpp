#include "AdbTransport.h"

#include <ws2tcpip.h>
#include <shlobj.h>

#include <atomic>
#include <sstream>

#include "../util/Log.h"

#pragma comment(lib, "ws2_32.lib")

namespace celmon {

namespace {

constexpr const char* kPackage = "com.celmonitor";
constexpr const char* kActivity = "com.celmonitor/.MainActivity";
constexpr const char* kSocketName = "celmonitor";

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\r' || s[i] == '\n')) ++i;
    return s.substr(i);
}

class SocketConnection : public IConnection {
public:
    SocketConnection(SOCKET s, std::wstring desc) : s_(s), desc_(std::move(desc)) {}
    ~SocketConnection() override { Close(); }

    bool ReadExact(void* buf, size_t n) override {
        auto* p = static_cast<char*>(buf);
        while (n > 0) {
            int r = recv(s_, p, int(std::min<size_t>(n, 1 << 20)), 0);
            if (r <= 0) return false;
            p += r;
            n -= size_t(r);
        }
        return true;
    }
    bool WriteAll(const void* buf, size_t n) override {
        auto* p = static_cast<const char*>(buf);
        while (n > 0) {
            int r = send(s_, p, int(std::min<size_t>(n, 1 << 20)), 0);
            if (r <= 0) return false;
            p += r;
            n -= size_t(r);
        }
        return true;
    }
    void Close() override {
        SOCKET s = s_.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
    }
    std::wstring Description() const override { return desc_; }

private:
    std::atomic<SOCKET> s_;
    std::wstring desc_;
};

}  // namespace

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

Status AdbTransport::Init() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    std::vector<std::wstring> candidates;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    candidates.push_back(dir + L"\\adb\\adb.exe");
    candidates.push_back(dir + L"\\adb.exe");
    wchar_t env[MAX_PATH];
    for (const wchar_t* var : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"})
        if (GetEnvironmentVariableW(var, env, MAX_PATH)) candidates.push_back(std::wstring(env) + L"\\platform-tools\\adb.exe");
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        candidates.push_back(std::wstring(local) + L"\\Android\\Sdk\\platform-tools\\adb.exe");
        CoTaskMemFree(local);
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) candidates.push_back(found);
    for (auto& c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) { adb_ = c; break; }
    }
    if (adb_.empty())
        return Status::Error(L"adb.exe não encontrado. Instale o Android SDK Platform-Tools ou coloque adb.exe ao lado do CelMonitor.");

    // Start the server without redirected handles: the daemon would otherwise inherit our pipes.
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + adb_ + L"\" start-server";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    char path[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, adb_.c_str(), -1, path, MAX_PATH, nullptr, nullptr);
    Log::Info("adb: %s", path);
    return Status::Ok();
}

int AdbTransport::Run(const std::wstring& args, std::string& output, DWORD timeoutMs) {
    output.clear();
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + adb_ + L"\" " + args;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    char buf[4096];
    // Read while the process runs; don't wait for pipe EOF (a spawned adb server could keep it open).
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || got == 0) break;
            output.append(buf, got);
        }
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                DWORD got = 0;
                if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || got == 0) break;
                output.append(buf, got);
            }
            break;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    DWORD code = DWORD(-1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return int(code);
}

std::vector<PhoneDevice> AdbTransport::ListDevices() {
    std::vector<PhoneDevice> list;
    std::string out;
    if (Run(L"devices -l", out) != 0) return list;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line.rfind("List of", 0) == 0 || line[0] == '*') continue;
        std::istringstream f(line);
        PhoneDevice d;
        std::string state, token;
        f >> d.serial >> state;
        if (d.serial.empty()) continue;
        d.state = state == "device" ? DeviceState::Ready : state == "unauthorized" ? DeviceState::Unauthorized : DeviceState::Offline;
        while (f >> token)
            if (token.rfind("model:", 0) == 0) {
                std::string m = token.substr(6);
                for (auto& c : m) if (c == '_') c = ' ';
                d.model = Widen(m);
            }
        list.push_back(d);
    }
    return list;
}

Status AdbTransport::InstallApk(const std::string& serial, const std::wstring& apkPath) {
    std::string out;
    int rc = Run(L"-s " + Widen(serial) + L" install -r \"" + apkPath + L"\"", out, 120000);
    if (rc != 0 || out.find("Success") == std::string::npos) {
        if (out.find("INSTALL_FAILED_USER_RESTRICTED") != std::string::npos)
            return Status::Error(L"O celular bloqueou a instalação via USB. Na MIUI, ative 'Instalar via USB' nas Opções do desenvolvedor.");
        return Status::Error(L"Falha ao instalar o app no celular: " + Widen(Trim(out)));
    }
    return Status::Ok();
}

std::optional<SOCKET> AdbTransport::TryConnect(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return std::nullopt;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    lastConnectRefused_ = false;
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        lastConnectRefused_ = true;  // nothing listens locally: the adb forward is gone (adb server restarted)
        closesocket(s);
        return std::nullopt;
    }
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    int buf = 4 << 20;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    // adb accepts locally even when nothing listens on the phone, then closes. The app speaks first (HELLO),
    // so wait until data is readable and peek to tell "app listening" from "closed".
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s, &rs);
    timeval tv = {2, 0};
    if (select(0, &rs, nullptr, nullptr, &tv) == 1) {
        char c;
        if (recv(s, &c, 1, MSG_PEEK) == 1) return s;
    }
    closesocket(s);
    return std::nullopt;
}

void AdbTransport::StopServer() {
    std::string o;
    Run(L"kill-server", o, 10000);
    preparedSerial_.clear();
    preparedPort_ = 0;
}

void AdbTransport::StartServer() {
    // No redirected handles: the daemon would otherwise inherit our pipes (see Init()).
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + adb_ + L"\" start-server";
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void AdbTransport::LaunchApp(const std::string& serial) {
    std::string o;
    Run(L"-s " + Widen(serial) + L" shell am start -n " + Widen(kActivity), o);
}

void AdbTransport::ForceStopApp(const std::string& serial) {
    std::string o;
    Run(L"-s " + Widen(serial) + L" shell am force-stop " + Widen(kPackage), o);
}

void AdbTransport::Forget(const std::string& serial) {
    if (preparedSerial_ == serial) {
        preparedSerial_.clear();
        preparedPort_ = 0;
    }
}

Status AdbTransport::Connect(const std::string& serial, std::unique_ptr<IConnection>& out, bool launchApp, bool* appMissing) {
    if (appMissing) *appMissing = false;
    std::wstring dev = L"-s " + Widen(serial) + L" ";
    std::string o;
    // App check + forward only once per phone; background polling then costs a single local TCP connect.
    if (preparedSerial_ != serial || !preparedPort_) {
        Run(dev + L"shell pm path " + Widen(kPackage), o);
        if (o.find("package:") == std::string::npos) {
            if (appMissing) *appMissing = true;
            return Status::Error(L"O app CelMonitor não está instalado no celular.");
        }
        // Drop forwards left by previous sessions of ours (never touch other tools' forwards).
        Run(L"forward --list", o);
        std::istringstream fl(o);
        for (std::string l; std::getline(fl, l);) {
            std::istringstream f(l);
            std::string ser, local, remote;
            f >> ser >> local >> remote;
            if (ser == serial && remote == std::string("localabstract:") + kSocketName) {
                std::string ignored;
                Run(dev + L"forward --remove " + Widen(local), ignored);
            }
        }
        if (Run(dev + L"forward tcp:0 localabstract:" + Widen(kSocketName), o) != 0)
            return Status::Error(L"adb forward falhou: " + Widen(Trim(o)));
        int p = atoi(Trim(o).c_str());
        if (p <= 0 || p > 65535) return Status::Error(L"adb forward devolveu uma porta inválida: " + Widen(Trim(o)));
        preparedSerial_ = serial;
        preparedPort_ = uint16_t(p);
    }
    uint16_t port = preparedPort_;

    auto sock = TryConnect(port);
    if (!sock && lastConnectRefused_) Forget(serial);
    if (!sock && !launchApp) return Status::Error(L"O app no celular não está em modo monitor.");
    if (!sock) {
        // App not running (or not listening yet): open it and retry for a few seconds.
        Log::Info("app not listening; starting %s", kActivity);
        Run(dev + L"shell am start -n " + Widen(kActivity), o);
        for (int i = 0; i < 8 && !sock; ++i) {
            Sleep(500);
            sock = TryConnect(port);
        }
    }
    if (!sock) {
        Forget(serial);  // the forward may be stale (adb restarted); redo it next time
        return Status::Error(L"O app no celular não respondeu. Desbloqueie o celular e mantenha a tela ligada.");
    }
    out = std::make_unique<SocketConnection>(*sock, L"USB (ADB)");
    Log::Info("connected to %s via adb (port %u)", serial.c_str(), unsigned(port));
    return Status::Ok();
}

}  // namespace celmon
