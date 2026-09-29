// Byte-stream transport to the phone. The protocol layer (session/) is identical over every transport;
// new transports (AOA/WinUSB) implement IConnection + a discovery source.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../display/VirtualDisplay.h"  // Status

namespace celmon {

class IConnection {
public:
    virtual ~IConnection() = default;
    // Reads exactly n bytes; false on disconnect/error.
    virtual bool ReadExact(void* buf, size_t n) = 0;
    // Writes all n bytes; false on disconnect/error. Thread-safe with respect to other writes is NOT required
    // (the session serializes writes).
    virtual bool WriteAll(const void* buf, size_t n) = 0;
    // Unblocks pending reads/writes; safe from any thread.
    virtual void Close() = 0;
    virtual std::wstring Description() const = 0;
};

enum class DeviceState { Ready, Unauthorized, Offline, NoApp };

struct PhoneDevice {
    std::string serial;
    DeviceState state = DeviceState::Offline;
    std::wstring model;
};

}  // namespace celmon
