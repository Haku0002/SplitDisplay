#include "log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

static FILE* g_file = nullptr;
static std::mutex g_lock;

void LogInit(const wchar_t* fileName)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    wcscat_s(path, fileName);
    _wfopen_s(&g_file, path, L"a, ccs=UTF-8");
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
