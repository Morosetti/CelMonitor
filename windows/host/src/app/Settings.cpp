#include "Settings.h"

#include <shlobj.h>

#include <algorithm>

namespace celmon {

namespace {

std::wstring IniPath() {
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

UINT GetInt(const std::wstring& ini, const wchar_t* key, UINT def) {
    return GetPrivateProfileIntW(L"CelMonitor", key, int(def), ini.c_str());
}

void PutInt(const std::wstring& ini, const wchar_t* key, UINT v) {
    WritePrivateProfileStringW(L"CelMonitor", key, std::to_wstring(v).c_str(), ini.c_str());
}

}  // namespace

AppSettings AppSettings::Load() {
    AppSettings s;
    std::wstring ini = IniPath();
    if (ini.empty()) return s;
    s.session.codec = GetInt(ini, L"codec", 1) == 2 ? proto::Codec::H265 : proto::Codec::H264;
    s.session.fps = std::clamp<UINT>(GetInt(ini, L"fps", 60), 1, 60);
    s.session.quality = std::clamp<UINT>(GetInt(ini, L"quality", 50), 1, 100);
    s.session.portrait = GetInt(ini, L"portrait", 0) != 0;
    UINT w = GetInt(ini, L"width", 0), h = GetInt(ini, L"height", 0);
    if (w && h) s.session.mode = Mode{w, h, 60};
    s.transport = GetInt(ini, L"transport", 0) == 1 ? TransportMode::AdbOnly : TransportMode::Auto;
    s.autoConnect = GetInt(ini, L"autoConnect", 1) != 0;
    wchar_t serial[128] = {};
    GetPrivateProfileStringW(L"CelMonitor", L"lastSerial", L"", serial, 128, ini.c_str());
    for (wchar_t* p = serial; *p; ++p) s.lastSerial += char(*p < 128 ? *p : '?');
    return s;
}

void AppSettings::Save() const {
    std::wstring ini = IniPath();
    if (ini.empty()) return;
    PutInt(ini, L"codec", UINT(session.codec));
    PutInt(ini, L"fps", session.fps);
    PutInt(ini, L"quality", session.quality);
    PutInt(ini, L"portrait", session.portrait ? 1 : 0);
    PutInt(ini, L"width", session.mode ? session.mode->width : 0);
    PutInt(ini, L"height", session.mode ? session.mode->height : 0);
    PutInt(ini, L"transport", transport == TransportMode::AdbOnly ? 1 : 0);
    PutInt(ini, L"autoConnect", autoConnect ? 1 : 0);
    WritePrivateProfileStringW(L"CelMonitor", L"lastSerial", std::wstring(lastSerial.begin(), lastSerial.end()).c_str(), ini.c_str());
}

}  // namespace celmon
