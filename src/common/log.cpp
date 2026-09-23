#include "log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

static FILE* g_file = nullptr;
static std::mutex g_lock;

void LogInit(const wchar_t* fileName)
{
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\SplitDisplay\\", path, MAX_PATH);
    CreateDirectoryW(path, nullptr);
    wcscat_s(path, fileName);
    // Shared so the watchdog (a second process) and readers can use the same file.
    g_file = _wfsopen(path, L"a, ccs=UTF-8", _SH_DENYNO);
}

void Log(const wchar_t* fmt, ...)
{
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[2200];
    swprintf_s(line, L"%02u:%02u:%02u.%03u [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), msg);

    std::lock_guard lk(g_lock);
    fputws(line, stdout);
    fflush(stdout);
    if (g_file)
    {
        fputws(line, g_file);
        fflush(g_file);
    }
}
