// Standalone single-file graphical installer for SplitDisplay.
// Embeds the release zip as RCDATA resource 101, extracts it to %TEMP%, and runs install.ps1.
#include <Windows.h>
#include <CommCtrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <ShellScalingApi.h>

#include <string>
#include <thread>

#ifndef SPLITDISPLAY_VERSION
#define SPLITDISPLAY_VERSION "0.4.1"
#endif

namespace
{

enum : int
{
    IDC_TITLE = 100,
    IDC_DESC,
    IDC_STATUS,
    IDC_PROGRESS,
    IDC_INSTALL,
    IDC_CANCEL,
};

constexpr UINT WM_APP_STATUS = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;

struct SetupUi
{
    HWND wnd = nullptr;
    HFONT font = nullptr, bold = nullptr, title = nullptr;
    HBRUSH bgBrush = nullptr;
    UINT dpi = 96;
    bool dark = false;
    bool installing = false;
    bool autoStart = false;
    int exitCode = 0;
    std::wstring error;
} g;

int S(int v) { return MulDiv(v, (int)g.dpi, 96); }
HWND Ctl(int id) { return GetDlgItem(g.wnd, id); }

bool IsDarkMode()
{
    DWORD light = 1, sz = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &sz);
    return light == 0;
}

COLORREF BgColor() { return g.dark ? RGB(0x20, 0x20, 0x20) : GetSysColor(COLOR_WINDOW); }
COLORREF TextColor() { return g.dark ? RGB(0xF0, 0xF0, 0xF0) : GetSysColor(COLOR_WINDOWTEXT); }

void ApplyTheme()
{
    g.dark = IsDarkMode();
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    auto setAppMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    auto allowWindow = ux ? (AllowDarkModeForWindowFn)GetProcAddress(ux, MAKEINTRESOURCEA(133)) : nullptr;
    if (setAppMode) setAppMode(g.dark ? 2 : 0);
    if (allowWindow) allowWindow(g.wnd, g.dark);
    BOOL dark = g.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g.wnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    if (g.bgBrush) DeleteObject(g.bgBrush);
    g.bgBrush = CreateSolidBrush(BgColor());
    SetClassLongPtrW(g.wnd, GCLP_HBRBACKGROUND, (LONG_PTR)g.bgBrush);
    for (int id : { IDC_TITLE, IDC_DESC, IDC_STATUS, IDC_PROGRESS, IDC_INSTALL, IDC_CANCEL })
    {
        HWND h = Ctl(id);
        if (!h) continue;
        if (allowWindow) allowWindow(h, g.dark);
        SetWindowTheme(h, g.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    InvalidateRect(g.wnd, nullptr, TRUE);
}

bool ExtractEmbeddedZip(const std::wstring& zipPath, std::wstring& errOut)
{
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!res) { errOut = L"Embedded release package not found."; return false; }
    DWORD sz = SizeofResource(mod, res);
    HGLOBAL hgl = LoadResource(mod, res);
    const void* data = hgl ? LockResource(hgl) : nullptr;
    if (!data || sz < 64) { errOut = L"This Setup binary does not contain a release payload."; return false; }
    HANDLE f = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { errOut = L"Could not write temporary package."; return false; }
    DWORD written = 0;
    bool ok = WriteFile(f, data, sz, &written, nullptr) && written == sz;
    CloseHandle(f);
    if (!ok) errOut = L"Failed to write temporary package.";
    return ok;
}

void RunInstallerThread()
{
    auto postStatus = [](const wchar_t* s) { PostMessageW(g.wnd, WM_APP_STATUS, 0, (LPARAM)_wcsdup(s)); };
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"SplitDisplay-Setup-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(work.c_str(), nullptr);
    std::wstring zipPath = work + L"\\pkg.zip";
    std::wstring pkgDir = work + L"\\pkg";
    CreateDirectoryW(pkgDir.c_str(), nullptr);

    postStatus(L"Extracting installation package...");
    std::wstring err;
    if (!ExtractEmbeddedZip(zipPath, err))
    {
        g.error = err;
        PostMessageW(g.wnd, WM_APP_DONE, 1, 0);
        return;
    }

    std::wstring tarCmd = L"tar.exe -xf \"" + zipPath + L"\" -C \"" + pkgDir + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, tarCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    postStatus(L"Signing and installing virtual display driver...");
    std::wstring psCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + pkgDir + L"\\install.ps1\"";
    if (!CreateProcessW(nullptr, psCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, pkgDir.c_str(), &si, &pi))
    {
        g.error = L"Could not start PowerShell installer.";
        PostMessageW(g.wnd, WM_APP_DONE, 1, 0);
        return;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) g.error = L"Installation failed (exit code " + std::to_wstring(code) + L"). Check C:\\ProgramData\\SplitDisplay\\sdctl.log.";
    PostMessageW(g.wnd, WM_APP_DONE, code, 0);
}

void StartInstall()
{
    if (g.installing) return;
    g.installing = true;
    EnableWindow(Ctl(IDC_INSTALL), FALSE);
    EnableWindow(Ctl(IDC_CANCEL), FALSE);
    SendMessageW(Ctl(IDC_PROGRESS), PBM_SETMARQUEE, TRUE, 30);
    std::thread(RunInstallerThread).detach();
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g.wnd = wnd;
        g.dpi = GetDpiForWindow(wnd);
        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
        LOGFONTW lf = ncm.lfMessageFont;
        g.font = CreateFontIndirectW(&lf);
        lf.lfWeight = FW_SEMIBOLD;
        g.bold = CreateFontIndirectW(&lf);
        lf.lfHeight = lf.lfHeight * 4 / 3;
        g.title = CreateFontIndirectW(&lf);

        CreateWindowExW(0, WC_STATIC, L"SplitDisplay v" _CRT_WIDE(SPLITDISPLAY_VERSION) L" Setup", WS_CHILD | WS_VISIBLE | SS_LEFT,
            S(20), S(16), S(400), S(28), wnd, (HMENU)IDC_TITLE, nullptr, nullptr);
        CreateWindowExW(0, WC_STATIC,
            L"Installs or upgrades the locally-signed virtual display driver and SplitDisplay in C:\\Program Files\\SplitDisplay.",
            WS_CHILD | WS_VISIBLE | SS_LEFT, S(20), S(50), S(400), S(38), wnd, (HMENU)IDC_DESC, nullptr, nullptr);
        CreateWindowExW(0, WC_STATIC, L"Ready to install.", WS_CHILD | WS_VISIBLE | SS_LEFT,
            S(20), S(96), S(400), S(20), wnd, (HMENU)IDC_STATUS, nullptr, nullptr);
        CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_MARQUEE,
            S(20), S(120), S(400), S(18), wnd, (HMENU)IDC_PROGRESS, nullptr, nullptr);
        CreateWindowExW(0, WC_BUTTON, L"Install / Upgrade", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            S(212), S(152), S(116), S(30), wnd, (HMENU)IDC_INSTALL, nullptr, nullptr);
        CreateWindowExW(0, WC_BUTTON, L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            S(336), S(152), S(84), S(30), wnd, (HMENU)IDC_CANCEL, nullptr, nullptr);

        SendMessageW(Ctl(IDC_TITLE), WM_SETFONT, (WPARAM)g.title, TRUE);
        for (int id : { IDC_DESC, IDC_STATUS, IDC_INSTALL, IDC_CANCEL })
            SendMessageW(Ctl(id), WM_SETFONT, (WPARAM)g.font, TRUE);
        ApplyTheme();
        if (g.autoStart) StartInstall();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
        HDC dc = (HDC)wp;
        SetBkColor(dc, BgColor());
        SetTextColor(dc, TextColor());
        return (LRESULT)(g.bgBrush ? g.bgBrush : GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_APP_STATUS:
    {
        wchar_t* s = (wchar_t*)lp;
        if (s) { SetWindowTextW(Ctl(IDC_STATUS), s); free(s); }
        return 0;
    }
    case WM_APP_DONE:
        SendMessageW(Ctl(IDC_PROGRESS), PBM_SETMARQUEE, FALSE, 0);
        if (wp != 0)
        {
            g.installing = false;
            g.exitCode = (int)wp;
            SetWindowTextW(Ctl(IDC_STATUS), g.error.c_str());
            EnableWindow(Ctl(IDC_INSTALL), TRUE);
            EnableWindow(Ctl(IDC_CANCEL), TRUE);
            if (!g.autoStart) MessageBoxW(wnd, g.error.c_str(), L"SplitDisplay Setup", MB_ICONERROR);
            else DestroyWindow(wnd);
        }
        else
        {
            SetWindowTextW(Ctl(IDC_STATUS), L"Installation complete.");
            DestroyWindow(wnd);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_INSTALL) StartInstall();
        else if (LOWORD(wp) == IDC_CANCEL && !g.installing) DestroyWindow(wnd);
        return 0;
    case WM_CLOSE:
        if (g.installing) return 0;
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(g.exitCode);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int)
{
    if (cmdLine && (wcsstr(cmdLine, L"/S") || wcsstr(cmdLine, L"--silent") || wcsstr(cmdLine, L"--auto")))
        g.autoStart = true;

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"SplitDisplaySetupWindow";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{ 0, 0, MulDiv(440, (int)dpi, 96), MulDiv(198, (int)dpi, 96) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"SplitDisplay Setup", style, sx, sy, w, h, nullptr, nullptr, inst, nullptr);
    ShowWindow(wnd, SW_SHOWNORMAL);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(wnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
