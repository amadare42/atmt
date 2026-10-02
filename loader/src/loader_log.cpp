// loader_log.cpp - the loader's own log file.
//
// One file for the whole loader (atmt_loader.log in the game folder): what was
// loaded, what a mod asked for, what failed. A mod's own output (the dialog log,
// for example) goes wherever that mod decides.
//
// Two kinds of line: informational (Log) and failures (LogError). LogLevel in atmt_loader.ini
// decides which reach the file; the default is errors, and the file is only created by the first
// line that is written - a player whose session went fine finds no log at all.
#include "loader.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace atmt_loader {

namespace {
CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
FILE* g_file = nullptr;
bool g_open_failed = false;
std::wstring g_path;
LogLevel g_level = LogLevel::Off;   // until LogInit: nothing is written

void EnsureLock() {
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = true;
    }
}

void Write(const char* line, bool error) {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    const bool wanted = g_level == LogLevel::All || (error && g_level == LogLevel::Errors);
    if (wanted && g_file == nullptr && !g_open_failed && !g_path.empty()) {
        // "wb", not "ab": each start begins the file fresh rather than piling onto however many
        // previous sessions' worth of lines are already there.
        g_file = _wfopen(g_path.c_str(), L"wb");
        g_open_failed = g_file == nullptr;
    }
    if (wanted && g_file != nullptr) {
        fputs(line, g_file);
        fflush(g_file);
    }
    LeaveCriticalSection(&g_lock);
    OutputDebugStringA("[atmt_loader] ");
    OutputDebugStringA(line);
}

void Format(char* line, size_t size, const char* prefix, const char* format, va_list args) {
    const size_t p = strlen(prefix);
    memcpy(line, prefix, p);
    _vsnprintf(line + p, size - p - 2, format, args);
    line[size - 2] = '\0';
    const size_t n = strlen(line);
    line[n] = '\n';
    line[n + 1] = '\0';
}

// the log is UTF-8; the paths we print are ASCII in practice, so a plain narrowing is enough and
// keeps the loader free of conversion dependencies
void Narrow(const wchar_t* wide, char* narrow, size_t size) {
    size_t i = 0;
    for (; i < size - 1 && wide[i] != L'\0'; ++i) {
        narrow[i] = wide[i] < 128 ? static_cast<char>(wide[i]) : '?';
    }
    narrow[i] = '\0';
}
}  // namespace

void LogInit(LogLevel level) {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    g_path = GameDir() + L"\\atmt_loader.log";
    g_level = level;
    LeaveCriticalSection(&g_lock);
}

void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    Format(line, sizeof(line), "", format, args);
    va_end(args);
    Write(line, false);
}

void LogError(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    Format(line, sizeof(line), "ERROR ", format, args);
    va_end(args);
    Write(line, true);
}

void LogW(const wchar_t* format, ...) {
    wchar_t wide[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf(wide, sizeof(wide) / sizeof(wide[0]) - 1, format, args);
    va_end(args);
    wide[sizeof(wide) / sizeof(wide[0]) - 1] = L'\0';
    char narrow[1024];
    Narrow(wide, narrow, sizeof(narrow));
    Log("%s", narrow);
}

void LogErrorW(const wchar_t* format, ...) {
    wchar_t wide[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf(wide, sizeof(wide) / sizeof(wide[0]) - 1, format, args);
    va_end(args);
    wide[sizeof(wide) / sizeof(wide[0]) - 1] = L'\0';
    char narrow[1024];
    Narrow(wide, narrow, sizeof(narrow));
    LogError("%s", narrow);
}

void LogClose() {
    EnsureLock();
    EnterCriticalSection(&g_lock);
    if (g_file != nullptr) {
        fclose(g_file);
        g_file = nullptr;
    }
    LeaveCriticalSection(&g_lock);
}

}  // namespace atmt_loader
