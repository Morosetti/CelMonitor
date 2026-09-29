#include "PhoneDetect.h"

#include <windows.h>
#include <setupapi.h>

#include <cwchar>
#include <map>

namespace celmon {

namespace {

// USB-IF vendor IDs of the main Android manufacturers.
const std::map<unsigned, const wchar_t*> kBrands = {
    {0x2717, L"Xiaomi"},   {0x04E8, L"Samsung"},  {0x18D1, L"Google"},   {0x22B8, L"Motorola"}, {0x12D1, L"Huawei"},
    {0x2A70, L"OnePlus"},  {0x22D9, L"Oppo/Realme"}, {0x2D95, L"vivo"},  {0x0BB4, L"HTC"},      {0x1004, L"LG"},
    {0x0FCE, L"Sony"},     {0x19D2, L"ZTE"},      {0x17EF, L"Lenovo"},   {0x0B05, L"Asus"},     {0x29A9, L"Nokia/HMD"},
    {0x1EBF, L"Positivo"}, {0x0E8D, L"MediaTek"}, {0x1782, L"Spreadtrum"},
};

}  // namespace

std::optional<DetectedPhone> DetectUsbPhone() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return std::nullopt;
    std::optional<DetectedPhone> found;
    SP_DEVINFO_DATA info = {sizeof(info)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        wchar_t ids[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids), sizeof(ids) - 4, nullptr))
            continue;
        unsigned vid = 0, pid = 0;
        const wchar_t* v = wcsstr(ids, L"VID_");
        const wchar_t* p = wcsstr(ids, L"PID_");
        if (!v || !p || swscanf_s(v, L"VID_%4x", &vid) != 1 || swscanf_s(p, L"PID_%4x", &pid) != 1) continue;
        auto brand = kBrands.find(vid);
        if (brand == kBrands.end()) continue;
        bool isInterface = wcsstr(ids, L"&MI_") != nullptr;
        wchar_t compat[1024] = {};
        SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_COMPATIBLEIDS, nullptr, reinterpret_cast<BYTE*>(compat), sizeof(compat) - 4, nullptr);
        bool isAdb = false;
        for (const wchar_t* c = compat; *c; c += wcslen(c) + 1)
            if (wcsstr(c, L"Class_ff&SubClass_42&Prot_01")) isAdb = true;  // Android ADB interface signature
        bool isMtp = false;
        wchar_t cls[64] = {};
        if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_CLASS, nullptr, reinterpret_cast<BYTE*>(cls), sizeof(cls) - 2, nullptr))
            isMtp = _wcsicmp(cls, L"WPD") == 0;
        bool accessory = vid == 0x18D1 && (pid == 0x2D00 || pid == 0x2D01);
        // Only phones: an MTP/ADB interface or accessory mode (keeps Google keyboards/hubs etc. out).
        if (!isAdb && !isMtp && !accessory) continue;
        if (!found) {
            found = DetectedPhone{};
            found->brand = accessory ? L"Android" : brand->second;
        }
        if (isAdb) found->debuggingEnabled = true;
        if (accessory) found->accessoryMode = true;
        if (isMtp || (isInterface && found->name.empty())) {
            wchar_t name[256] = {};
            if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(name), sizeof(name) - 2, nullptr) ||
                SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<BYTE*>(name), sizeof(name) - 2, nullptr))
                if (isMtp) found->name = name;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

std::wstring DebuggingSteps(const std::wstring& brand) {
    if (brand == L"Xiaomi")
        return L"1. Configurações > Sobre o telefone > toque 7 vezes em \"Versão do MIUI\" (ou \"Versão do SO\").\n"
               L"2. Configurações > Configurações adicionais > Opções do desenvolvedor.\n"
               L"3. Ative \"Depuração USB\" (e \"Instalar via USB\", se aparecer).";
    if (brand == L"Samsung")
        return L"1. Configurações > Sobre o telefone > Informações do software > toque 7 vezes em \"Número de compilação\".\n"
               L"2. Configurações > Opções do desenvolvedor (no fim da lista).\n"
               L"3. Ative \"Depuração USB\".";
    if (brand == L"Huawei")
        return L"1. Configurações > Sobre o telefone > toque 7 vezes em \"Número da versão\".\n"
               L"2. Configurações > Sistema e atualizações > Opções do desenvolvedor.\n"
               L"3. Ative \"Depuração USB\".";
    if (brand == L"OnePlus" || brand == L"Oppo/Realme" || brand == L"vivo")
        return L"1. Configurações > Sobre o dispositivo > Versão > toque 7 vezes em \"Número da versão\".\n"
               L"2. Configurações > Configurações adicionais (ou Sistema) > Opções do desenvolvedor.\n"
               L"3. Ative \"Depuração USB\".";
    return L"1. Configurações > Sobre o telefone > toque 7 vezes em \"Número da versão\".\n"
           L"2. Configurações > Sistema > Opções do desenvolvedor.\n"
           L"3. Ative \"Depuração USB\".";
}

}  // namespace celmon
