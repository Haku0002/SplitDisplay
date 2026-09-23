#include "autostart.h"

#include <Windows.h>
#include <comdef.h>
#include <taskschd.h>
#define SECURITY_WIN32
#include <security.h>

#include <string>

#include "log.h"

static constexpr wchar_t kTaskName[] = L"SplitDisplay";

struct ComInit
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComInit()
    {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

static ITaskFolder* RootFolder(ITaskService** serviceOut)
{
    ITaskService* service = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)))) return nullptr;
    if (FAILED(service->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t())))
    {
        service->Release();
        return nullptr;
    }
    ITaskFolder* folder = nullptr;
    service->GetFolder(_bstr_t(L"\\"), &folder);
    *serviceOut = service;
    return folder;
}

AutostartState GetAutostart()
{
    ComInit com;
    ITaskService* service = nullptr;
    ITaskFolder* folder = RootFolder(&service);
    if (!folder) return AutostartState::Missing;
    AutostartState state = AutostartState::Missing;
    IRegisteredTask* task = nullptr;
    if (SUCCEEDED(folder->GetTask(_bstr_t(kTaskName), &task)))
    {
        VARIANT_BOOL enabled = VARIANT_FALSE;
        task->get_Enabled(&enabled);
        state = enabled ? AutostartState::Enabled : AutostartState::Disabled;
        task->Release();
    }
    folder->Release();
    service->Release();
    return state;
}

static std::wstring XmlEscape(const std::wstring& s)
{
    std::wstring out;
    for (wchar_t c : s)
    {
        switch (c)
        {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

bool SetAutostart(bool enabled)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t user[256];
    ULONG userLen = 256;
    if (!GetUserNameExW(NameSamCompatible, user, &userLen))
    {
        Log(L"autostart: GetUserNameEx failed %lu", GetLastError());
        return false;
    }

    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>Splits the configured display into native monitors (SplitDisplay).</Description></RegistrationInfo>"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" + XmlEscape(user) + L"</UserId><Delay>PT3S</Delay></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"Author\"><UserId>" + XmlEscape(user) + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>"
        L"<Enabled>" + std::wstring(enabled ? L"true" : L"false") + L"</Enabled></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>\"" + XmlEscape(exe) + L"\"</Command><Arguments>run</Arguments></Exec></Actions>"
        L"</Task>";

    ComInit com;
    ITaskService* service = nullptr;
    ITaskFolder* folder = RootFolder(&service);
    if (!folder)
    {
        Log(L"autostart: Task Scheduler unavailable");
        return false;
    }
    IRegisteredTask* task = nullptr;
    HRESULT hr = folder->RegisterTask(_bstr_t(kTaskName), _bstr_t(xml.c_str()), TASK_CREATE_OR_UPDATE, _variant_t(), _variant_t(),
        TASK_LOGON_INTERACTIVE_TOKEN, _variant_t(L""), &task);
    if (task) task->Release();
    folder->Release();
    service->Release();
    Log(L"autostart %s for %s: 0x%08X", enabled ? L"enabled" : L"disabled", user, (UINT)hr);
    return SUCCEEDED(hr);
}
