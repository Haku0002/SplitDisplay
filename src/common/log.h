#pragma once
#include <string>

// Minimal logger: console + append to a file next to the executable.
void LogInit(const wchar_t* fileName);
void Log(const wchar_t* fmt, ...);
