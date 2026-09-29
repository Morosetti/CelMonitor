// Thread-safe logger: %LOCALAPPDATA%\CelMonitor\celmonitor.log, the debugger (DebugView) and optionally stdout.
#pragma once

#include <string>

namespace celmon::Log {

void Init(bool console);
void Info(const char* fmt, ...);
void Warn(const char* fmt, ...);
void Error(const char* fmt, ...);
std::wstring FilePath();

}  // namespace celmon::Log
