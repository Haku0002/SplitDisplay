#include "updater.h"

#include <Windows.h>
#include <Shellapi.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

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

// Stamps DriverVer = MM/DD/YYYY,1.<major>.<minor>.<patch> into a staged SplitDisplayIdd.inf so a
// local developer build always has a newer version than whatever driver package is in the store.
void StampInfDriverVer(const std::wstring& infPath)
{
    std::string content = ReadUtf8File(infPath);
    if (content.empty()) return;
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char verLine[128];
    sprintf_s(verLine, "DriverVer = %02u/%02u/%04u,1.%d.%d.%d", st.wMonth, st.wDay, st.wYear, SD_VER_MAJOR, SD_VER_MINOR, SD_VER_PATCH);
    size_t pos = content.find("DriverVer");
    if (pos == std::string::npos) return;
    size_t eol = content.find_first_of("\r\n", pos);
    content.replace(pos, eol == std::string::npos ? std::string::npos : eol - pos, verLine);
    WriteUtf8File(infPath, content.c_str());
}

const char kEmbeddedInstallPs1[] = R"SD_INST(# SplitDisplay installer (run through Install.cmd, SplitDisplaySetup.exe, or in-app Upgrade).
#
# 1. Signs the bundled driver AND executables (splitdisplay.exe, sdctl.exe) with a brand-new
#    certificate created on THIS machine, trusts that certificate, then deletes its private key.
#    Nothing signed by anyone else is trusted,nobody can sign anything else with it afterwards,
#    and Windows UAC / Defender sees the installed binaries as validly signed.
# 2. Installs the virtual monitor driver.
# 3. Copies SplitDisplay to Program Files, picks the display to split and registers a logon task.
#    Optional: -Panel "<monitor name>" to choose the display without asking.
#    Optional: -NoOpenGui to skip opening the settings window at the end.
param(
    [string]$Panel,
    [switch]$NoOpenGui
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$dest = Join-Path $env:ProgramFiles 'SplitDisplay'
$data = Join-Path $env:ProgramData 'SplitDisplay'
$subject = 'CN=SplitDisplay Local Driver Signing'

# Strip Mark-of-the-Web (Zone.Identifier) from downloaded/extracted files so SmartScreen and UAC
# do not flag installed binaries.
Get-ChildItem $here -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue

# The settings window runs unprivileged and writes config/logs here; files created so far were
# made by an elevated process and are read-only for the user. Grant the installing user Modify
# and also grant Interactive Users Modify so any interactive session can save settings.
New-Item -ItemType Directory -Force $data | Out-Null
$me = [Security.Principal.WindowsIdentity]::GetCurrent().Name
icacls $data /grant "${me}:(OI)(CI)M" /grant "*S-1-5-4:(OI)(CI)M" /T /C 2>&1 | Out-Null

function Step($msg) { Write-Host "`n== $msg" -ForegroundColor Cyan }

# Published names (oemNN.inf) of every SplitDisplayIdd package in the driver store.
# pnputil's text output is localized, so parse its XML; fall back to the text form on
# Windows versions whose pnputil has no /format option.
function Get-SplitDisplayDriverPackages {
    try {
        $text = (pnputil /enum-drivers /format xml 2>$null | Out-String)
        $start = $text.IndexOf('<?xml')
        if ($start -ge 0) {
            $doc = [xml]$text.Substring($start)
            return @($doc.PnpUtil.Driver | Where-Object { $_.OriginalName -ieq 'splitdisplayidd.inf' } | ForEach-Object { $_.DriverName })
        }
    } catch { }
    $names = @()
    $lines = pnputil /enum-drivers
    for ($i = 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'Original Name:\s+splitdisplayidd\.inf' -and $lines[$i - 1] -match 'Published Name:\s+(oem\d+\.inf)') { $names += $Matches[1] }
    }
    return $names
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run Install.cmd (it asks for administrator rights).' }
foreach ($f in 'splitdisplay.exe', 'sdctl.exe', 'driver\SplitDisplayIdd.dll', 'driver\SplitDisplayIdd.inf') {
    if (-not (Test-Path (Join-Path $here $f))) { throw "missing $f next to install.ps1" }
}

Step 'Stopping a running SplitDisplay'
# Ask any running compositor to restore displays and exit, then close any open settings window.
$stopExe = if (Test-Path (Join-Path $dest 'splitdisplay.exe')) { Join-Path $dest 'splitdisplay.exe' } else { Join-Path $here 'splitdisplay.exe' }
try { & $stopExe stop | Out-Null } catch { }
for ($i = 0; $i -lt 40; $i++) {
    if (-not (Get-Process splitdisplay -ErrorAction SilentlyContinue)) { break }
    Start-Sleep -Milliseconds 150
}
Get-Process splitdisplay, sdctl -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

# From here on, any failure restarts the previous SplitDisplay (if one was installed) and cleans up.
$pkg = $null
$cert = $null
try {

Step 'Signing driver and executables with a one-time local certificate'
$pkg = Join-Path $env:TEMP ("SplitDisplayDriver-" + [guid]::NewGuid())
New-Item -ItemType Directory $pkg | Out-Null
$binStage = Join-Path $pkg "bin"
New-Item -ItemType Directory $binStage | Out-Null
Copy-Item (Join-Path $here 'driver\SplitDisplayIdd.dll'), (Join-Path $here 'driver\SplitDisplayIdd.inf') $pkg
Copy-Item (Join-Path $here 'splitdisplay.exe'), (Join-Path $here 'sdctl.exe') $binStage
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\LocalMachine\My `
    -KeyExportPolicy NonExportable -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(30)
try {
    # Trust first: Set-AuthenticodeSignature reports Valid only for a chain the machine trusts.
    $cer = Join-Path $pkg 'SplitDisplay.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null

    $sig = Set-AuthenticodeSignature (Join-Path $pkg 'SplitDisplayIdd.dll') -Certificate $cert -HashAlgorithm SHA256
    if ($sig.Status -ne 'Valid') { throw "signing the driver dll failed: $($sig.Status) $($sig.StatusMessage)" }
    New-FileCatalog -Path $pkg -CatalogFilePath (Join-Path $pkg 'SplitDisplayIdd.cat') -CatalogVersion 2.0 | Out-Null
    $sig = Set-AuthenticodeSignature (Join-Path $pkg 'SplitDisplayIdd.cat') -Certificate $cert -HashAlgorithm SHA256
    if ($sig.Status -ne 'Valid') { throw "signing the driver catalog failed: $($sig.Status) $($sig.StatusMessage)" }

    # Sign splitdisplay.exe and sdctl.exe too (unless already signed by a trusted CA) so UAC and
    # Windows security checks see a verified publisher instead of "Unknown publisher".
    foreach ($exeName in 'splitdisplay.exe', 'sdctl.exe') {
        $exePath = Join-Path $binStage $exeName
        $existing = Get-AuthenticodeSignature $exePath -ErrorAction SilentlyContinue
        if (-not $existing -or $existing.Status -ne 'Valid') {
            $sig = Set-AuthenticodeSignature $exePath -Certificate $cert -HashAlgorithm SHA256
            if ($sig.Status -ne 'Valid') { throw "signing $exeName failed: $($sig.Status) $($sig.StatusMessage)" }
        }
    }
}
finally {
    # The private key is never needed again. The trusted root stays behind, so a key that
    # survives this would let its holder sign drivers forever: verify it is really gone.
    Remove-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -DeleteKey -ErrorAction SilentlyContinue
    if (Get-Item "Cert:\LocalMachine\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue) { throw 'could not delete the one-time signing key' }
}
Write-Host "Certificate $($cert.Thumbprint) trusted; driver + executables signed; private key deleted."

Step 'Installing the virtual monitor driver'
& (Join-Path $binStage 'sdctl.exe') driver-install (Join-Path $pkg 'SplitDisplayIdd.inf')
if ($LASTEXITCODE) { throw "driver install failed (see $env:ProgramData\SplitDisplay\sdctl.log)" }

# Driver packages from earlier installs are no longer needed (the one in use cannot be deleted).
foreach ($oem in Get-SplitDisplayDriverPackages) { pnputil /delete-driver $oem 2>&1 | Out-Null }

# Keep the newly created certificate in Root/TrustedPublisher so the signed driver and
# executables remain trusted; remove certificates from earlier installs.
foreach ($store in 'Root', 'TrustedPublisher') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like "$subject*" -and $_.Thumbprint -ne $cert.Thumbprint } |
        ForEach-Object { Remove-Item $_.PSPath }
}

Step 'Installing SplitDisplay'
New-Item -ItemType Directory -Force $dest | Out-Null
New-Item -ItemType Directory -Force (Join-Path $dest 'driver') | Out-Null
Get-ChildItem $dest -Filter 'splitdisplay.old*.exe' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue

# Copy the signed executables from $binStage and the signed driver from $pkg.
foreach ($exeName in 'splitdisplay.exe', 'sdctl.exe') {
    $src = Join-Path $binStage $exeName
    $dst = Join-Path $dest $exeName
    try {
        Copy-Item $src $dst -Force
    } catch {
        if (Test-Path $dst) {
            Move-Item $dst (Join-Path $dest "$exeName.old.$PID.exe") -Force -ErrorAction SilentlyContinue
            Copy-Item $src $dst -Force
        }
    }
}
Copy-Item (Join-Path $pkg 'SplitDisplayIdd.dll'), (Join-Path $pkg 'SplitDisplayIdd.inf'), (Join-Path $pkg 'SplitDisplayIdd.cat') (Join-Path $dest 'driver') -Force

if ((Resolve-Path $here).Path.TrimEnd('\') -ne (Resolve-Path $dest).Path.TrimEnd('\')) {
    foreach ($f in 'install.ps1', 'Install.cmd', 'uninstall.ps1', 'Uninstall.cmd', 'README.txt') {
        $src = Join-Path $here $f
        if (Test-Path $src) { Copy-Item $src (Join-Path $dest $f) -Force }
    }
}

# Create Start Menu shortcut so SplitDisplay appears in Start / search.
try {
    $lnkPath = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\SplitDisplay.lnk'
    $wsh = New-Object -ComObject WScript.Shell
    $sc = $wsh.CreateShortcut($lnkPath)
    $sc.TargetPath = Join-Path $dest 'splitdisplay.exe'
    $sc.WorkingDirectory = $dest
    $sc.Description = 'SplitDisplay virtual monitor compositor'
    $sc.Save()
} catch { }

Unregister-ScheduledTask -TaskName 'SplitDisplay Failsafe Revert' -Confirm:$false -ErrorAction SilentlyContinue
$exe = Join-Path $dest 'splitdisplay.exe'
if ($Panel) {
    $r = Start-Process $exe -ArgumentList '--panel', "`"$Panel`"", 'configure' -Wait -PassThru
} else {
    $r = Start-Process $exe -ArgumentList 'autodetect' -Wait -PassThru
    if ($r.ExitCode -eq 2) { Write-Host 'Several displays found: pick the one to split in the settings window.' -ForegroundColor Yellow }
}
$r = Start-Process $exe -ArgumentList 'autostart', 'on' -Wait -PassThru
if ($r.ExitCode) { throw 'creating the logon task failed' }

}
catch {
    Write-Host "`nInstall failed: $_" -ForegroundColor Red
    if ($cert) {
        foreach ($store in 'Root', 'TrustedPublisher') { Remove-Item "Cert:\LocalMachine\$store\$($cert.Thumbprint)" -ErrorAction SilentlyContinue }
    }
    if (Get-ScheduledTask -TaskName 'SplitDisplay' -ErrorAction SilentlyContinue) {
        Write-Host 'Restarting the previously installed SplitDisplay.'
        Start-ScheduledTask -TaskName 'SplitDisplay'
    }
    exit 1
}
finally {
    if ($pkg) { Remove-Item -Recurse -Force $pkg -ErrorAction SilentlyContinue }
}

Step 'Starting'
Start-ScheduledTask -TaskName 'SplitDisplay'
if (-not $NoOpenGui) {
    # Launch through explorer.exe so the settings window opens under the normal interactive user token.
    Start-Process explorer.exe -ArgumentList "`"$exe`""
}
Write-Host @"

Done. SplitDisplay is installed and starts automatically after you sign in.
  Settings and layout:  "$dest\splitdisplay.exe"
  Emergency exit:       Ctrl+Alt+Shift+F12
  Uninstall:            "$dest\Uninstall.cmd"
  Logs:                 $env:ProgramData\SplitDisplay\
"@ -ForegroundColor Green
)SD_INST";

const char kEmbeddedUninstallPs1[] = R"SD_UNINST(# SplitDisplay uninstaller (run through Uninstall.cmd, which elevates).
# Restores the display, removes the logon task, the driver, the certificate and the program files.
$ErrorActionPreference = 'Continue'

# Published names (oemNN.inf) of every SplitDisplayIdd package in the driver store.
# pnputil's text output is localized, so parse its XML; fall back to the text form on
# Windows versions whose pnputil has no /format option.
function Get-SplitDisplayDriverPackages {
    try {
        $text = (pnputil /enum-drivers /format xml 2>$null | Out-String)
        $start = $text.IndexOf('<?xml')
        if ($start -ge 0) {
            $doc = [xml]$text.Substring($start)
            return @($doc.PnpUtil.Driver | Where-Object { $_.OriginalName -ieq 'splitdisplayidd.inf' } | ForEach-Object { $_.DriverName })
        }
    } catch { }
    $names = @()
    $lines = pnputil /enum-drivers
    for ($i = 1; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'Original Name:\s+splitdisplayidd\.inf' -and $lines[$i - 1] -match 'Published Name:\s+(oem\d+\.inf)') { $names += $Matches[1] }
    }
    return $names
}

$dest = Join-Path $env:ProgramFiles 'SplitDisplay'
$exe = Join-Path $dest 'splitdisplay.exe'
$sdctl = Join-Path $dest 'sdctl.exe'
if (-not (Test-Path $sdctl)) { $sdctl = Join-Path $PSScriptRoot 'sdctl.exe' }

Write-Host '== Restoring the display'
if (Test-Path $exe) {
    & $exe stop | Out-Null
    for ($i = 0; $i -lt 30; $i++) {
        if (-not (Get-Process splitdisplay -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Milliseconds 150
    }
    & $exe revert | Out-Null
}
Get-Process splitdisplay, sdctl -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Unregister-ScheduledTask -TaskName 'SplitDisplay' -Confirm:$false -ErrorAction SilentlyContinue

Write-Host '== Removing the driver'
if (Test-Path $sdctl) { & $sdctl driver-remove | Out-Null }
foreach ($oem in Get-SplitDisplayDriverPackages) {
    pnputil /delete-driver $oem /uninstall | Out-Null
    Write-Host "  removed $oem"
}

Write-Host '== Removing the certificate'
foreach ($store in 'Root', 'TrustedPublisher', 'My') {
    Get-ChildItem "Cert:\LocalMachine\$store" | Where-Object { $_.Subject -like 'CN=SplitDisplay Local Driver Signing*' } |
        ForEach-Object { Remove-Item $_.PSPath -DeleteKey -ErrorAction SilentlyContinue; Write-Host "  removed $($_.Thumbprint) from $store" }
}

Write-Host '== Removing program files'
Set-Location $env:TEMP
Remove-Item -Force (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\SplitDisplay.lnk') -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
Write-Host 'SplitDisplay removed. Logs remain in' "$env:ProgramData\SplitDisplay" -ForegroundColor Green
)SD_UNINST";

const char kEmbeddedInstallCmd[] =
    "@echo off\r\n"
    "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \\\"%~dp0install.ps1\\\"'\"\r\n";

const char kEmbeddedUninstallCmd[] =
    "@echo off\r\n"
    "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File \\\"%~dp0uninstall.ps1\\\"'\"\r\n";

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
    std::wstring script = stageDir + L"\\install.ps1";
    if (!FileExists(script)) WriteUtf8File(script, kEmbeddedInstallPs1);
    if (!FileExists(stageDir + L"\\uninstall.ps1")) WriteUtf8File(stageDir + L"\\uninstall.ps1", kEmbeddedUninstallPs1);
    if (!FileExists(stageDir + L"\\Install.cmd")) WriteUtf8File(stageDir + L"\\Install.cmd", kEmbeddedInstallCmd);
    if (!FileExists(stageDir + L"\\Uninstall.cmd")) WriteUtf8File(stageDir + L"\\Uninstall.cmd", kEmbeddedUninstallCmd);

    std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
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
    WriteUtf8File(stage + L"\\install.ps1", kEmbeddedInstallPs1);
    WriteUtf8File(stage + L"\\uninstall.ps1", kEmbeddedUninstallPs1);
    WriteUtf8File(stage + L"\\Install.cmd", kEmbeddedInstallCmd);
    WriteUtf8File(stage + L"\\Uninstall.cmd", kEmbeddedUninstallCmd);

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

    // Ensure the upgraded install script has the clean process-stop and Start Menu logic even if
    // the downloaded archive was packaged with an older install.ps1.
    WriteUtf8File(pkgDir + L"\\install.ps1", kEmbeddedInstallPs1);
    WriteUtf8File(pkgDir + L"\\uninstall.ps1", kEmbeddedUninstallPs1);

    if (onStatus) onStatus(L"Starting administrator upgrade to " + info.tag + L"...");
    return LaunchElevatedInstallScript(pkgDir, errOut);
}
