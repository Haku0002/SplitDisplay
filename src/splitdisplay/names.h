#pragma once

// Named objects shared by the compositor ("run"), the settings window and "stop".
inline constexpr wchar_t kStopEventName[] = L"Local\\SplitDisplay.Stop";
inline constexpr wchar_t kInstanceMutexName[] = L"Local\\SplitDisplay.Instance";
inline constexpr wchar_t kGuiMutexName[] = L"Local\\SplitDisplay.Gui";
inline constexpr wchar_t kGuiWindowClass[] = L"SplitDisplaySettings";
