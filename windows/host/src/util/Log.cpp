#include "Log.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace celmon::Log {

namespace {
std::mutex g_lock;
FILE* g_file = nullptr;
bool g_console = false;
std::wstring g_path;

void Write(const char* level, const char* fmt, va_list ap) {
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, msg);
    std::lock_guard<std::mutex> lock(g_lock);
    OutputDebugStringA(line);
    if (g_file) { fputs(line, g_file); fflush(g_file); }
    if (g_console) fputs(line, stdout);
}
}  // namespace

void Init(bool console) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_console = console;
    if (g_file) return;
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        std::wstring dir = std::wstring(base) + L"\\CelMonitor";
        CoTaskMemFree(base);
        CreateDirectoryW(dir.c_str(), nullptr);
        g_path = dir + L"\\celmonitor.log";
        g_file = _wfsopen(g_path.c_str(), L"a", _SH_DENYWR);
    }
}

std::wstring FilePath() { return g_path; }

void Info(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write("INFO", fmt, ap); va_end(ap); }
void Warn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write("WARN", fmt, ap); va_end(ap); }
void Error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write("ERROR", fmt, ap); va_end(ap); }

}  // namespace celmon::Log
