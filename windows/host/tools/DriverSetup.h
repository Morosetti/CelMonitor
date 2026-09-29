// Driver installation used by the installer (celmon-cli setup-drivers / remove-drivers). Needs administrator rights.
// Replaces devcon (not redistributable): creates the Root\CelMonIdd device and installs/removes both packages.
#pragma once

#include <windows.h>
#include <wincrypt.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "setupapi.lib")

namespace driversetup {

constexpr wchar_t kIddHardwareId[] = L"Root\\CelMonIdd";

inline std::wstring Err(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s + L" (0x" + std::to_wstring(e) + L")";
}

// Calls fn(set, info) for every device (present or not) whose hardware IDs include hwid.
template <typename Fn>
void ForEachDevice(const wchar_t* hwid, Fn fn) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA info = {sizeof(info)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids), sizeof(ids) - 4, nullptr))
            continue;
        for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
            if (_wcsicmp(p, hwid) == 0) { fn(set, info); break; }
    }
    SetupDiDestroyDeviceInfoList(set);
}

// Creates Root\CelMonIdd if missing and installs/updates its driver from infPath.
inline bool InstallIdd(const std::wstring& infPath, bool& reboot) {
    bool exists = false;
    ForEachDevice(kIddHardwareId, [&](HDEVINFO, SP_DEVINFO_DATA&) { exists = true; });
    if (!exists) {
        GUID cls = GUID_DEVCLASS_DISPLAY;
        HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
        SP_DEVINFO_DATA info = {sizeof(info)};
        // Double-NUL terminated multi-sz hardware ID list.
        std::wstring hwids = std::wstring(kIddHardwareId) + L'\0' + L'\0';
        bool ok = set != INVALID_HANDLE_VALUE &&
                  SetupDiCreateDeviceInfoW(set, L"Display", &cls, nullptr, nullptr, DICD_GENERATE_ID, &info) &&
                  SetupDiSetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(hwids.data()),
                                                    DWORD(hwids.size() * sizeof(wchar_t))) &&
                  SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &info);
        DWORD e = GetLastError();
        if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
        if (!ok) {
            fwprintf(stderr, L"falha ao criar o dispositivo do monitor virtual: %ls\n", Err(e).c_str());
            return false;
        }
    }
    BOOL needReboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kIddHardwareId, infPath.c_str(), INSTALLFLAG_FORCE, &needReboot)) {
        fwprintf(stderr, L"falha ao instalar o driver do monitor virtual: %ls\n", Err(GetLastError()).c_str());
        return false;
    }
    reboot = reboot || needReboot;
    return true;
}

// Adds a driver package to the driver store and installs it on matching present devices (like pnputil /install).
inline bool InstallPackage(const std::wstring& infPath, bool& reboot) {
    BOOL needReboot = FALSE;
    if (!DiInstallDriverW(nullptr, infPath.c_str(), DIIRFLAG_FORCE_INF, &needReboot)) {
        DWORD e = GetLastError();
        fwprintf(stderr, L"falha ao instalar %ls: %ls\n", infPath.c_str(), Err(e).c_str());
        return false;
    }
    reboot = reboot || needReboot;
    return true;
}

inline void RemoveIddDevices() {
    ForEachDevice(kIddHardwareId, [](HDEVINFO set, SP_DEVINFO_DATA& info) {
        BOOL reboot = FALSE;
        if (!DiUninstallDevice(nullptr, set, &info, 0, &reboot)) SetupDiRemoveDevice(set, &info);
    });
}

// Removes our packages (by original INF name) from the driver store.
inline void RemovePackages(const std::vector<std::wstring>& originalInfNames) {
    wchar_t dir[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(dir) + L"\\INF\\oem*.inf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring oem = std::wstring(dir) + L"\\INF\\" + fd.cFileName;
        wchar_t original[MAX_PATH] = {};
        SP_INF_INFORMATION* info = nullptr;
        DWORD size = 0;
        SetupGetInfInformationW(oem.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, nullptr, 0, &size);
        std::vector<BYTE> buf(size);
        info = reinterpret_cast<SP_INF_INFORMATION*>(buf.data());
        if (size && SetupGetInfInformationW(oem.c_str(), INFINFO_INF_NAME_IS_ABSOLUTE, info, size, nullptr)) {
            SP_ORIGINAL_FILE_INFO_W ofi = {sizeof(ofi)};
            if (SetupQueryInfOriginalFileInformationW(info, 0, nullptr, &ofi)) wcscpy_s(original, ofi.OriginalInfName);
        }
        for (auto& n : originalInfNames) {
            if (_wcsicmp(original, n.c_str()) == 0) {
                BOOL reboot = FALSE;
                if (!DiUninstallDriverW(nullptr, oem.c_str(), 0, &reboot)) SetupUninstallOEMInfW(fd.cFileName, SUOI_FORCEDELETE, nullptr);
                wprintf(L"removido do driver store: %ls (%ls)\n", fd.cFileName, original);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace driversetup
