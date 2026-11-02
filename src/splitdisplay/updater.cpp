#include "updater.h"

#include <Windows.h>
#include <Shellapi.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

#include "embedded_scripts.h" // generated from release/ by CMakeLists.txt
#include "log.h"
#include "shm.h"

#ifndef SD_VER_MAJOR
#define SD_VER_MAJOR 0
#define SD_VER_MINOR 4
#define SD_VER_PATCH 0
#endif

namespace
{

struct HttpHandle
{
    HINTERNET h = nullptr;
    HttpHandle() = default;
    explicit HttpHandle(HINTERNET v) : h(v) {}
    ~HttpHandle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::wstring SelfPath()
{
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

std::wstring DirOf(const std::wstring& path)
{
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : path.substr(0, pos);
}

bool FileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::tuple<int, int, int> ParseSemVer(const std::wstring& s)
{
    int maj = 0, min = 0, pat = 0;
    const wchar_t* p = s.c_str();
    while (*p && (*p < L'0' || *p > L'9')) p++;
    swscanf_s(p, L"%d.%d.%d", &maj, &min, &pat);
    return { maj, min, pat };
}

std::tuple<int, int, int> FileVersionOf(const std::wstring& path)
{
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!sz) return { 0, 0, 0 };
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return { 0, 0, 0 };
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (LPVOID*)&ffi, &len) || !ffi) return { 0, 0, 0 };
    return { (int)HIWORD(ffi->dwFileVersionMS), (int)LOWORD(ffi->dwFileVersionMS), (int)HIWORD(ffi->dwFileVersionLS) };
}

// Minimal JSON string-field extractor for GitHub Releases API responses.
std::string ExtractJsonString(const std::string& json, const char* key, size_t startPos = 0, size_t* nextPos = nullptr)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle, startPos);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    std::string out;
    for (size_t i = pos + 1; i < json.size(); i++)
    {
        char c = json[i];
        if (c == '\\' && i + 1 < json.size())
        {
            out += json[++i];
            continue;
        }
        if (c == '"')
        {
            if (nextPos) *nextPos = i + 1;
            return out;
        }
        out += c;
    }
    return "";
}

bool WriteUtf8File(const std::wstring& path, const char* text)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD len = (DWORD)strlen(text), written = 0;
    bool ok = WriteFile(f, text, len, &written, nullptr) && written == len;
    CloseHandle(f);
    return ok;
}

// The embedded scripts carry LF line ends (CMake strips CR); cmd.exe misparses LF-only batch
// files in places, so scripts are written back with CRLF.
bool WriteScriptFile(const std::wstring& path, const char* text)
{
    std::string crlf;
    for (const char* p = text; *p; p++)
    {
        if (*p == '\n') crlf += '\r';
        crlf += *p;
    }
    return WriteUtf8File(path, crlf.c_str());
}

std::string ReadUtf8File(const std::wstring& path)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    DWORD sz = GetFileSize(f, nullptr), read = 0;
    std::string out(sz, '\0');
    if (sz && !ReadFile(f, out.data(), sz, &read, nullptr)) out.clear();
    CloseHandle(f);
    return out;
}

// Stamps DriverVer into a staged SplitDisplayIdd.inf using the same scheme as
// scripts/make-release.ps1 (1.<major*100+minor>.<patch>.<n>), with <n> = local HHMM, so a local
// build ranks above the release of the same version and above earlier builds of the same day.
void StampInfDriverVer(const std::wstring& infPath)
{
    std::string content = ReadUtf8File(infPath);
    if (content.empty()) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char verLine[128];
    sprintf_s(verLine, "DriverVer = %02u/%02u/%04u,1.%d.%d.%u", st.wMonth, st.wDay, st.wYear, SD_VER_MAJOR * 100 + SD_VER_MINOR, SD_VER_PATCH,
        st.wHour * 100u + st.wMinute);
    size_t pos = content.find("DriverVer");
    if (pos == std::string::npos) return;
    size_t eol = content.find_first_of("\r\n", pos);
    content.replace(pos, eol == std::string::npos ? std::string::npos : eol - pos, verLine);
    WriteUtf8File(infPath, content.c_str());
}


struct LocalSourcePaths
{
    std::wstring splitdisplayExe;
    std::wstring sdctlExe;
    std::wstring driverDll;
    std::wstring driverInf;
    std::wstring installPs1;
    std::wstring uninstallPs1;
};

bool FindLocalPayload(LocalSourcePaths& out)
{
    std::wstring self = SelfPath();
    std::wstring here = DirOf(self);
    std::wstring prog = L"C:\\Program Files\\SplitDisplay";
    wchar_t pf[MAX_PATH] = {};
    if (ExpandEnvironmentStringsW(L"%ProgramFiles%\\SplitDisplay", pf, MAX_PATH) && pf[0]) prog = pf;

    out.splitdisplayExe = self;
    for (const auto& cand : { here + L"\\sdctl.exe", prog + L"\\sdctl.exe" })
        if (out.sdctlExe.empty() && FileExists(cand)) out.sdctlExe = cand;

    for (const auto& cand : {
             here + L"\\driver\\SplitDisplayIdd.dll",
             here + L"\\SplitDisplayIdd.dll",
             prog + L"\\driver\\SplitDisplayIdd.dll" })
        if (out.driverDll.empty() && FileExists(cand)) out.driverDll = cand;

    for (const auto& cand : {
             here + L"\\driver\\SplitDisplayIdd.inf",
             here + L"\\..\\..\\driver\\SplitDisplayIdd.inf",
             here + L"\\..\\driver\\SplitDisplayIdd.inf",
             prog + L"\\driver\\SplitDisplayIdd.inf" })
        if (out.driverInf.empty() && FileExists(cand)) out.driverInf = cand;

    for (const auto& cand : {
             here + L"\\install.ps1",
             here + L"\\..\\..\\release\\install.ps1",
             prog + L"\\install.ps1" })
        if (out.installPs1.empty() && FileExists(cand)) out.installPs1 = cand;

    for (const auto& cand : {
             here + L"\\uninstall.ps1",
             here + L"\\..\\..\\release\\uninstall.ps1",
             prog + L"\\uninstall.ps1" })
        if (out.uninstallPs1.empty() && FileExists(cand)) out.uninstallPs1 = cand;

    return !out.splitdisplayExe.empty() && !out.sdctlExe.empty() && !out.driverDll.empty() && !out.driverInf.empty();
}

bool DownloadUrlToFile(const std::wstring& url, const std::wstring& destPath,
    const std::function<void(uint64_t, uint64_t)>& onProgress, std::wstring& errOut)
{
    URL_COMPONENTS uc{ sizeof(uc) };
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
    {
        errOut = L"Invalid download URL.";
        return false;
    }

    HttpHandle session(WinHttpOpen(L"SplitDisplay/" SPLITDISPLAY_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { errOut = L"WinHttpOpen failed."; return false; }
    WinHttpSetTimeouts(session, 8000, 10000, 15000, 30000);

    HttpHandle conn(WinHttpConnect(session, host, uc.nPort, 0));
    if (!conn) { errOut = L"Could not connect to GitHub."; return false; }

    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HttpHandle req(WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!req) { errOut = L"Could not create download request."; return false; }

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr))
    {
        errOut = L"Network error while downloading release.";
        return false;
    }

    DWORD status = 0, statusSz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        errOut = L"Download returned HTTP " + std::to_wstring(status) + L".";
        return false;
    }

    wchar_t clBuf[64] = {};
    DWORD clSz = sizeof(clBuf);
    uint64_t totalBytes = 0;
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, clBuf, &clSz, WINHTTP_NO_HEADER_INDEX))
        totalBytes = _wcstoui64(clBuf, nullptr, 10);

    HANDLE f = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        errOut = L"Could not create temporary download file.";
        return false;
    }

    std::vector<BYTE> buf(64 * 1024);
    uint64_t downloaded = 0;
    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) { CloseHandle(f); errOut = L"Download interrupted."; return false; }
        if (avail == 0) break;
        while (avail > 0)
        {
            DWORD chunk = avail > (DWORD)buf.size() ? (DWORD)buf.size() : avail;
            DWORD read = 0;
            if (!WinHttpReadData(req, buf.data(), chunk, &read) || read == 0)
            {
                CloseHandle(f);
                errOut = L"Download read error.";
                return false;
            }
            DWORD written = 0;
            if (!WriteFile(f, buf.data(), read, &written, nullptr) || written != read)
            {
                CloseHandle(f);
                errOut = L"Disk write error during download.";
                return false;
            }
            downloaded += read;
            avail -= read;
            if (onProgress) onProgress(downloaded, totalBytes);
        }
    }
    CloseHandle(f);
    return true;
}

bool ExtractZipToDir(const std::wstring& zipPath, const std::wstring& destDir, std::wstring& errOut)
{
    CreateDirectoryW(destDir.c_str(), nullptr);
    // Prefer built-in tar.exe (fast, native on Windows 10/11); fall back to PowerShell Expand-Archive.
    std::wstring cmd = L"tar.exe -xf \"" + zipPath + L"\" -C \"" + destDir + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code == 0 && FileExists(destDir + L"\\splitdisplay.exe")) return true;
    }
    std::wstring ps = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" + zipPath + L"' -DestinationPath '" + destDir + L"' -Force\"";
    if (CreateProcessW(nullptr, ps.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        WaitForSingleObject(pi.hProcess, 120000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code == 0 && FileExists(destDir + L"\\splitdisplay.exe")) return true;
    }
    errOut = L"Could not extract the release archive.";
    return false;
}

bool LaunchElevatedInstallScript(const std::wstring& stageDir, std::wstring& errOut)
{
    // A package brings its own scripts (always as new as its binaries); the embedded copies only
    // fill in what is missing.
    std::wstring script = stageDir + L"\\install.ps1";
    if (!FileExists(script)) WriteScriptFile(script, kEmbeddedInstallPs1);
    if (!FileExists(stageDir + L"\\uninstall.ps1")) WriteScriptFile(stageDir + L"\\uninstall.ps1", kEmbeddedUninstallPs1);
    if (!FileExists(stageDir + L"\\Install.cmd")) WriteScriptFile(stageDir + L"\\Install.cmd", kEmbeddedInstallCmd);
    if (!FileExists(stageDir + L"\\Uninstall.cmd")) WriteScriptFile(stageDir + L"\\Uninstall.cmd", kEmbeddedUninstallCmd);

    // The settings window closes once this starts, so a failure must stay on screen. Only scripts
    // that declare the switch get it: an older package's script would reject an unknown parameter.
    std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
    if (ReadUtf8File(script).find("PauseOnError") != std::string::npos) params += L" -PauseOnError";
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = params.c_str();
    sei.lpDirectory = stageDir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
    {
        DWORD err = GetLastError();
        errOut = err == ERROR_CANCELLED ? L"Administrator permission was declined." : (L"Could not start installer (error " + std::to_wstring(err) + L").");
        return false;
    }
    return true;
}

} // namespace

UpdateInfo CheckForGitHubUpdate()
{
    UpdateInfo info;
    HttpHandle session(WinHttpOpen(L"SplitDisplay/" SPLITDISPLAY_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) { info.error = L"WinHttpOpen failed"; return info; }
    WinHttpSetTimeouts(session, 5000, 8000, 8000, 12000);

    HttpHandle conn(WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn) { info.error = L"Could not connect to api.github.com"; return info; }

    HttpHandle req(WinHttpOpenRequest(conn, L"GET", L"/repos/Haku0002/SplitDisplay/releases/latest", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req) { info.error = L"WinHttpOpenRequest failed"; return info; }

    const wchar_t* hdrs = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(req, hdrs, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr))
    {
        info.error = L"Could not reach GitHub Releases";
        return info;
    }

    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        info.error = L"GitHub API HTTP " + std::to_wstring(status);
        return info;
    }

    std::string body;
    for (;;)
    {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        size_t old = body.size();
        body.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, body.data() + old, avail, &read)) break;
        body.resize(old + read);
        if (body.size() > 2 * 1024 * 1024) break;
    }

    std::string tag = ExtractJsonString(body, "tag_name");
    if (tag.empty())
    {
        info.error = L"No release tag found";
        return info;
    }
    info.tag = Utf8ToWide(tag);
    info.htmlUrl = Utf8ToWide(ExtractJsonString(body, "html_url"));
    auto [maj, min, pat] = ParseSemVer(info.tag);
    info.major = maj;
    info.minor = min;
    info.patch = pat;
    wchar_t vbuf[64];
    swprintf_s(vbuf, L"%d.%d.%d", maj, min, pat);
    info.version = vbuf;

    size_t pos = 0;
    while (pos < body.size())
    {
        size_t next = 0;
        std::string url = ExtractJsonString(body, "browser_download_url", pos, &next);
        if (url.empty() || next <= pos) break;
        pos = next;
        std::wstring wurl = Utf8ToWide(url);
        if (wurl.size() > 4 && _wcsicmp(wurl.c_str() + wurl.size() - 4, L".zip") == 0 && info.zipUrl.empty())
            info.zipUrl = wurl;
        else if (wurl.size() > 4 && _wcsicmp(wurl.c_str() + wurl.size() - 4, L".exe") == 0 && info.setupUrl.empty())
            info.setupUrl = wurl;
    }

    std::tuple<int, int, int> localVer{ SD_VER_MAJOR, SD_VER_MINOR, SD_VER_PATCH };
    std::tuple<int, int, int> remoteVer{ maj, min, pat };
    info.ok = true;
    info.hasUpdate = remoteVer > localVer;
    info.isMajorOrMinor = maj > SD_VER_MAJOR || (maj == SD_VER_MAJOR && min > SD_VER_MINOR);
    return info;
}

bool IsInstalledAndCurrent()
{
    SharedView shm;
    if (!shm.OpenReadOnly() && !shm.Open()) return false;
    wchar_t pf[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\SplitDisplay\\splitdisplay.exe", pf, MAX_PATH);
    if (!FileExists(pf)) return false;
    auto instVer = FileVersionOf(pf);
    std::tuple<int, int, int> selfVer{ SD_VER_MAJOR, SD_VER_MINOR, SD_VER_PATCH };
    return instVer >= selfVer;
}

bool CanInstallFromLocal()
{
    LocalSourcePaths src;
    return FindLocalPayload(src);
}

bool RunLocalInstall(const std::function<void(const std::wstring&)>& onStatus, std::wstring& errOut)
{
    LocalSourcePaths src;
    if (!FindLocalPayload(src))
    {
        if (onStatus) onStatus(L"Local driver files not found; checking GitHub Releases...");
        UpdateInfo rel = CheckForGitHubUpdate();
        if (!rel.ok || rel.zipUrl.empty())
        {
            errOut = L"Driver files were not found next to splitdisplay.exe and GitHub Releases could not be reached.";
            return false;
        }
        return RunGitHubUpgrade(rel, onStatus, errOut);
    }

    if (onStatus) onStatus(L"Staging installer files...");
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring stage = std::wstring(tmp) + L"SplitDisplay-Install-" + std::to_wstring(GetTickCount64());
    std::wstring drvDir = stage + L"\\driver";
    CreateDirectoryW(stage.c_str(), nullptr);
    CreateDirectoryW(drvDir.c_str(), nullptr);

    CopyFileW(src.splitdisplayExe.c_str(), (stage + L"\\splitdisplay.exe").c_str(), FALSE);
    CopyFileW(src.sdctlExe.c_str(), (stage + L"\\sdctl.exe").c_str(), FALSE);
    CopyFileW(src.driverDll.c_str(), (drvDir + L"\\SplitDisplayIdd.dll").c_str(), FALSE);
    CopyFileW(src.driverInf.c_str(), (drvDir + L"\\SplitDisplayIdd.inf").c_str(), FALSE);
    StampInfDriverVer(drvDir + L"\\SplitDisplayIdd.inf");

    // Always use the latest embedded install/uninstall scripts so upgrades handle running processes cleanly.
    WriteScriptFile(stage + L"\\install.ps1", kEmbeddedInstallPs1);
    WriteScriptFile(stage + L"\\uninstall.ps1", kEmbeddedUninstallPs1);
    WriteScriptFile(stage + L"\\Install.cmd", kEmbeddedInstallCmd);
    WriteScriptFile(stage + L"\\Uninstall.cmd", kEmbeddedUninstallCmd);

    if (onStatus) onStatus(L"Starting administrator installer...");
    return LaunchElevatedInstallScript(stage, errOut);
}

bool RunGitHubUpgrade(const UpdateInfo& info, const std::function<void(const std::wstring&)>& onStatus, std::wstring& errOut)
{
    if (info.zipUrl.empty())
    {
        errOut = L"Release " + info.tag + L" does not contain a zip package.";
        return false;
    }
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring workDir = std::wstring(tmp) + L"SplitDisplay-Update-" + info.version + L"-" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(workDir.c_str(), nullptr);
    std::wstring zipPath = workDir + L"\\release.zip";
    std::wstring pkgDir = workDir + L"\\pkg";

    if (onStatus) onStatus(L"Downloading SplitDisplay " + info.tag + L"...");
    bool dlOk = DownloadUrlToFile(info.zipUrl, zipPath, [&](uint64_t done, uint64_t total) {
        if (!onStatus) return;
        wchar_t msg[128];
        if (total > 0)
            swprintf_s(msg, L"Downloading %s... %llu / %llu KB (%llu%%)", info.tag.c_str(), done / 1024, total / 1024, done * 100 / total);
        else
            swprintf_s(msg, L"Downloading %s... %llu KB", info.tag.c_str(), done / 1024);
        onStatus(msg);
    }, errOut);
    if (!dlOk) return false;

    if (onStatus) onStatus(L"Extracting " + info.tag + L"...");
    if (!ExtractZipToDir(zipPath, pkgDir, errOut)) return false;


    if (onStatus) onStatus(L"Starting administrator upgrade to " + info.tag + L"...");
    return LaunchElevatedInstallScript(pkgDir, errOut);
}
