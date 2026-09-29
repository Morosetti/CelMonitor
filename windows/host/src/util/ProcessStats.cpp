#include "ProcessStats.h"

#include <pdhmsg.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace celmon {

namespace {
ULONGLONG ToU64(const FILETIME& f) { return (ULONGLONG(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
}  // namespace

ProcessStats::ProcessStats() {
    // "\GPU Engine(pid_<pid>_luid_..._engtype_3D)\Utilization Percentage": one instance per engine we use.
    std::wstring path = L"\\GPU Engine(pid_" + std::to_wstring(GetCurrentProcessId()) + L"_*)\\Utilization Percentage";
    if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS &&
        PdhAddEnglishCounterW(query_, path.c_str(), 0, &counter_) == ERROR_SUCCESS) {
        PdhCollectQueryData(query_);
    } else if (query_) {
        PdhCloseQuery(query_);
        query_ = nullptr;
    }
}

ProcessStats::~ProcessStats() {
    if (query_) PdhCloseQuery(query_);
}

void ProcessStats::Sample() {
    FILETIME c, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimeAsFileTime(&now);
    ULONGLONG cpu = ToU64(k) + ToU64(u), wall = ToU64(now);
    if (lastWall_ && wall > lastWall_) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        cpu_ = float(double(cpu - lastCpu_) / double(wall - lastWall_) / si.dwNumberOfProcessors * 100.0);
    }
    lastWall_ = wall;
    lastCpu_ = cpu;

    gpu_.reset();
    if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return;
    DWORD size = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA) {
        gpu_ = 0.0f;  // no engine instances yet (nothing rendered/encoded so far)
        return;
    }
    std::vector<uint8_t> buf(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS) return;
    // Sum per engine type (3D, VideoEncode, Copy...) across adapters, report the busiest one, like Task Manager.
    std::map<std::wstring, double> perEngine;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        std::wstring name = items[i].szName;
        size_t p = name.find(L"engtype_");
        perEngine[p == std::wstring::npos ? name : name.substr(p)] += items[i].FmtValue.doubleValue;
    }
    double best = 0;
    for (auto& [k2, v] : perEngine) best = std::max(best, v);
    gpu_ = float(std::min(best, 100.0));
}

}  // namespace celmon
