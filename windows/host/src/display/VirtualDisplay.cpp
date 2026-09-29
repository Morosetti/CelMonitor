#include "VirtualDisplay.h"

#include <setupapi.h>

#include <chrono>

#include "../util/Log.h"

#pragma comment(lib, "setupapi.lib")

namespace celmon {

std::wstring Win32ErrorText(DWORD error) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
                   0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s + L" (" + std::to_wstring(error) + L")";
}

VirtualDisplay::VirtualDisplay() = default;

VirtualDisplay::~VirtualDisplay() {
    Remove();
    if (device_ != INVALID_HANDLE_VALUE) CloseHandle(device_);
}

Status VirtualDisplay::Open() {
    if (IsOpen()) return Status::Ok();
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_CELMON_IDD, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return Status::Error(L"Falha ao enumerar dispositivos: " + Win32ErrorText(GetLastError()));

    std::wstring path;
    SP_DEVICE_INTERFACE_DATA ifData = {sizeof(ifData)};
    if (SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_CELMON_IDD, 0, &ifData)) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifData, nullptr, 0, &needed, nullptr);
        std::vector<uint8_t> buf(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(*detail);
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifData, detail, needed, nullptr, nullptr)) path = detail->DevicePath;
    }
    SetupDiDestroyDeviceInfoList(set);
    if (path.empty())
        return Status::Error(L"Driver do monitor virtual não encontrado ou parado. Instale-o com "
                             L"windows\\installer\\install-driver.ps1 (como administrador).");

    device_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    if (device_ == INVALID_HANDLE_VALUE)
        return Status::Error(L"Não foi possível abrir o driver do monitor virtual: " + Win32ErrorText(GetLastError()));

    auto info = Info();
    if (!info) return Status::Error(L"O driver não respondeu à consulta de versão.");
    if (info->apiVersion != CELMON_DRIVER_API_VERSION) {
        CloseHandle(device_);
        device_ = INVALID_HANDLE_VALUE;
        return Status::Error(L"Versão do driver incompatível (" + std::to_wstring(info->apiVersion) + L"); reinstale o driver.");
    }
    Log::Info("driver opened: api=%u version=%u.%u active=%u", info->apiVersion, info->driverVersion >> 16,
              info->driverVersion & 0xFFFF, info->activeMonitors);
    return Status::Ok();
}

bool VirtualDisplay::Ioctl(DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize, DWORD* returned) {
    std::lock_guard<std::mutex> lock(ioLock_);
    if (device_ == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }
    DWORD r = 0;
    BOOL ok = DeviceIoControl(device_, code, const_cast<void*>(in), inSize, out, outSize, &r, nullptr);
    if (returned) *returned = r;
    return ok != FALSE;
}

std::optional<CELMON_INFO> VirtualDisplay::Info() {
    CELMON_INFO info{};
    DWORD r = 0;
    if (!Ioctl(IOCTL_CELMON_GET_INFO, nullptr, 0, &info, sizeof(info), &r) || r != sizeof(info)) return std::nullopt;
    return info;
}

Status VirtualDisplay::Add(uint64_t deviceKey, const std::string& name, const std::vector<Mode>& modes, size_t preferred) {
    if (!IsOpen()) {
        Status s = Open();
        if (!s.ok) return s;
    }
    if (HasMonitor()) return Status::Error(L"Já existe um monitor virtual ativo.");
    if (modes.empty() || modes.size() > CELMON_MAX_MODES || preferred >= modes.size())
        return Status::Error(L"Lista de resoluções inválida.");

    CELMON_ADD_MONITOR_IN in{};
    in.apiVersion = CELMON_DRIVER_API_VERSION;
    in.modeCount = uint32_t(modes.size());
    in.preferredMode = uint32_t(preferred);
    in.deviceKey = deviceKey;
    strncpy_s(in.deviceName, name.c_str(), _TRUNCATE);
    for (size_t i = 0; i < modes.size(); ++i) in.modes[i] = {modes[i].width, modes[i].height, modes[i].refreshHz};

    CELMON_ADD_MONITOR_OUT out{};
    DWORD r = 0;
    if (!Ioctl(IOCTL_CELMON_ADD_MONITOR, &in, sizeof(in), &out, sizeof(out), &r) || r != sizeof(out)) {
        DWORD e = GetLastError();
        return Status::Error(L"O driver não criou o monitor virtual: " + Win32ErrorText(e));
    }
    monitorId_ = out.monitorId;
    targetId_ = out.targetId;
    adapterLuid_ = out.adapterLuid;
    Log::Info("monitor %u created: target=%u adapter=%08X:%08X", monitorId_, targetId_, adapterLuid_.HighPart,
              adapterLuid_.LowPart);

    pinging_ = true;
    pingThread_ = std::thread(&VirtualDisplay::PingLoop, this);
    return Status::Ok();
}

Status VirtualDisplay::Remove() {
    if (pinging_.exchange(false) && pingThread_.joinable()) pingThread_.join();
    if (!HasMonitor()) return Status::Ok();
    CELMON_REMOVE_MONITOR_IN in{monitorId_};
    bool ok = Ioctl(IOCTL_CELMON_REMOVE_MONITOR, &in, sizeof(in), nullptr, 0);
    DWORD e = GetLastError();
    Log::Info("monitor %u removed (%s)", monitorId_, ok ? "ok" : "failed");
    monitorId_ = 0;
    if (!ok) return Status::Error(L"Falha ao remover o monitor virtual: " + Win32ErrorText(e));
    return Status::Ok();
}

Status VirtualDisplay::SetRenderAdapter(LUID luid) {
    CELMON_SET_RENDER_ADAPTER_IN in{luid};
    if (!Ioctl(IOCTL_CELMON_SET_RENDER_ADAPTER, &in, sizeof(in), nullptr, 0))
        return Status::Error(L"Falha ao escolher a GPU de renderização: " + Win32ErrorText(GetLastError()));
    return Status::Ok();
}

void VirtualDisplay::PingLoop() {
    using namespace std::chrono;
    auto next = steady_clock::now();
    while (pinging_) {
        if (steady_clock::now() >= next) {
            if (!Ioctl(IOCTL_CELMON_PING, nullptr, 0, nullptr, 0)) Log::Warn("driver ping failed: %lu", GetLastError());
            next = steady_clock::now() + seconds(2);
        }
        std::this_thread::sleep_for(milliseconds(100));
    }
}

// ------------------------------------------------------------------------------------------------ display config

namespace {

struct PathMatch {
    std::wstring gdiName;
    DISPLAYCONFIG_SOURCE_MODE source{};
    bool hasSource = false;
    uint32_t refreshHz = 0;
};

std::optional<PathMatch> FindPath(LUID adapter, uint32_t targetId) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return std::nullopt;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        LONG r = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (r == ERROR_INSUFFICIENT_BUFFER) continue;  // config changed between calls
        if (r != ERROR_SUCCESS) return std::nullopt;
        for (UINT32 i = 0; i < pathCount; ++i) {
            const auto& p = paths[i];
            if (p.targetInfo.adapterId.LowPart != adapter.LowPart || p.targetInfo.adapterId.HighPart != adapter.HighPart ||
                p.targetInfo.id != targetId)
                continue;
            PathMatch m;
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = p.sourceInfo.adapterId;
            src.header.id = p.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS) m.gdiName = src.viewGdiDeviceName;
            if (p.sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && p.sourceInfo.modeInfoIdx < modeCount) {
                m.source = modes[p.sourceInfo.modeInfoIdx].sourceMode;
                m.hasSource = true;
            }
            if (p.targetInfo.refreshRate.Denominator)
                m.refreshHz = (p.targetInfo.refreshRate.Numerator + p.targetInfo.refreshRate.Denominator / 2) /
                              p.targetInfo.refreshRate.Denominator;
            return m;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

std::wstring VirtualDisplay::GdiDeviceName() const {
    if (!monitorId_) return {};
    auto m = FindPath(adapterLuid_, targetId_);
    return m ? m->gdiName : std::wstring();
}

std::optional<RECT> VirtualDisplay::DesktopRect() const {
    if (!monitorId_) return std::nullopt;
    auto m = FindPath(adapterLuid_, targetId_);
    if (!m || !m->hasSource) return std::nullopt;
    RECT r;
    r.left = m->source.position.x;
    r.top = m->source.position.y;
    r.right = r.left + LONG(m->source.width);
    r.bottom = r.top + LONG(m->source.height);
    return r;
}

std::optional<Mode> VirtualDisplay::CurrentMode() const {
    if (!monitorId_) return std::nullopt;
    auto m = FindPath(adapterLuid_, targetId_);
    if (!m || !m->hasSource) return std::nullopt;
    return Mode{m->source.width, m->source.height, m->refreshHz ? m->refreshHz : 60};
}

Status VirtualDisplay::SetMode(const Mode& mode) {
    std::wstring name = GdiDeviceName();
    if (name.empty()) return Status::Error(L"O monitor virtual ainda não está ativo no Windows.");
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = mode.width;
    dm.dmPelsHeight = mode.height;
    dm.dmDisplayFrequency = mode.refreshHz;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsExW(name.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_GLOBAL, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL)
        return Status::Error(L"O Windows recusou a resolução " + std::to_wstring(mode.width) + L"×" +
                             std::to_wstring(mode.height) + L" (código " + std::to_wstring(r) + L").");
    return Status::Ok();
}

}  // namespace celmon
