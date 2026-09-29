// CPU and GPU usage of this process, sampled on demand (call at ~1 s intervals).
#pragma once

#include <windows.h>
#include <pdh.h>

#include <optional>

namespace celmon {

class ProcessStats {
public:
    ProcessStats();
    ~ProcessStats();
    void Sample();
    float CpuPercent() const { return cpu_; }            // share of the whole machine
    std::optional<float> GpuPercent() const { return gpu_; }  // busiest GPU engine used by this process

private:
    ULONGLONG lastWall_ = 0, lastCpu_ = 0;
    float cpu_ = 0;
    std::optional<float> gpu_;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
};

}  // namespace celmon
