#include "Settings.h"

#include <shlobj.h>

#include <algorithm>

namespace celmon {

namespace {

constexpr wchar_t kAppSection[] = L"CelMonitor";
constexpr wchar_t kDefaultSection[] = L"Padrao";

std::wstring SectionFor(const std::string& serial) {
    return serial.empty() ? kDefaultSection : L"Celular " + std::wstring(serial.begin(), serial.end());
}

bool HasKey(const std::wstring& ini, const std::wstring& section, const wchar_t* key) {
    wchar_t buf[8];
    return GetPrivateProfileStringW(section.c_str(), key, L"\x01", buf, 8, ini.c_str()) && buf[0] != L'\x01';
}

// Value from the phone's section, else from the defaults section, else the built-in default.
UINT GetInt(const std::wstring& ini, const std::wstring& section, const wchar_t* key, UINT def) {
    if (HasKey(ini, section, key)) return GetPrivateProfileIntW(section.c_str(), key, int(def), ini.c_str());
    return GetPrivateProfileIntW(kDefaultSection, key, int(def), ini.c_str());
}

void PutInt(const std::wstring& ini, const std::wstring& section, const wchar_t* key, UINT v) {
    WritePrivateProfileStringW(section.c_str(), key, std::to_wstring(v).c_str(), ini.c_str());
}

}  // namespace

std::wstring SettingsFilePath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\CelMonitor";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\settings.ini";
    }
    CoTaskMemFree(base);
    return path;
}

DeviceProfile DeviceProfile::Load(const std::string& serial) {
    DeviceProfile p;
    std::wstring ini = SettingsFilePath();
    if (ini.empty()) return p;
    std::wstring sec = SectionFor(serial);
    auto& s = p.session;
    s.codec = GetInt(ini, sec, L"codec", 1) == 2 ? proto::Codec::H265 : proto::Codec::H264;
    s.fps = std::clamp<UINT>(GetInt(ini, sec, L"fps", 60), 1, 60);
    s.quality = std::clamp<UINT>(GetInt(ini, sec, L"quality", 50), 1, 100);
    s.portrait = GetInt(ini, sec, L"portrait", 0) != 0;
    UINT w = GetInt(ini, sec, L"width", 0), h = GetInt(ini, sec, L"height", 0);
    if (w && h) s.mode = Mode{w, h, 60};
    s.decoderLowLatency = GetInt(ini, sec, L"decoderLowLatency", 1) != 0;
    s.maxBitrateKbps = std::min<UINT>(GetInt(ini, sec, L"maxBitrateKbps", 0), 100000);
    UINT t = GetInt(ini, sec, L"transport", 0);
    p.transport = t == 1 ? TransportMode::AdbOnly : t == 2 ? TransportMode::AoaOnly : TransportMode::Auto;
    p.usbMaxTransfer = std::clamp<UINT>(GetInt(ini, sec, L"usbMaxTransfer", 16000), 512, 65536);
    return p;
}

void DeviceProfile::Save(const std::string& serial) const {
    std::wstring ini = SettingsFilePath();
    if (ini.empty()) return;
    std::wstring sec = SectionFor(serial);
    PutInt(ini, sec, L"codec", UINT(session.codec));
    PutInt(ini, sec, L"fps", session.fps);
    PutInt(ini, sec, L"quality", session.quality);
    PutInt(ini, sec, L"portrait", session.portrait ? 1 : 0);
    PutInt(ini, sec, L"width", session.mode ? session.mode->width : 0);
    PutInt(ini, sec, L"height", session.mode ? session.mode->height : 0);
    PutInt(ini, sec, L"decoderLowLatency", session.decoderLowLatency ? 1 : 0);
    PutInt(ini, sec, L"maxBitrateKbps", session.maxBitrateKbps);
    PutInt(ini, sec, L"transport", transport == TransportMode::AdbOnly ? 1 : transport == TransportMode::AoaOnly ? 2 : 0);
    PutInt(ini, sec, L"usbMaxTransfer", usbMaxTransfer);
}

AppSettings AppSettings::Load() {
    AppSettings s;
    std::wstring ini = SettingsFilePath();
    if (ini.empty()) return s;
    wchar_t serial[128] = {};
    GetPrivateProfileStringW(kAppSection, L"lastSerial", L"", serial, 128, ini.c_str());
    for (wchar_t* p = serial; *p; ++p) s.lastSerial += char(*p < 128 ? *p : '?');
    s.startMinimized = GetPrivateProfileIntW(kAppSection, L"startMinimized", 0, ini.c_str()) != 0;
    return s;
}

void AppSettings::Save() const {
    std::wstring ini = SettingsFilePath();
    if (ini.empty()) return;
    WritePrivateProfileStringW(kAppSection, L"lastSerial", std::wstring(lastSerial.begin(), lastSerial.end()).c_str(), ini.c_str());
    WritePrivateProfileStringW(kAppSection, L"startMinimized", startMinimized ? L"1" : L"0", ini.c_str());
}

}  // namespace celmon
