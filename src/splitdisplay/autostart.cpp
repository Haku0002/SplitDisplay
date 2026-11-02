#include "autostart.h"

#include <Windows.h>
#include <comdef.h>
#include <taskschd.h>
#define SECURITY_WIN32
#include <security.h>
#include <winrt/base.h>

#include <string>

#include "log.h"

namespace
{

constexpr wchar_t kTaskName[] = L"SplitDisplay";

struct ComScope
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope()
    {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

winrt::com_ptr<ITaskFolder> OpenRootFolder()
{
    winrt::com_ptr<ITaskService> svc;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(svc.put())))) return nullptr;
    if (FAILED(svc->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t()))) return nullptr;
    winrt::com_ptr<ITaskFolder> root;
    if (FAILED(svc->GetFolder(_bstr_t(L"\\"), root.put()))) return nullptr;
    return root;
}

std::wstring CurrentUserAccount()
{
    wchar_t buf[256] = {};
    ULONG len = 256;
    if (GetUserNameExW(NameSamCompatible, buf, &len) && buf[0]) return buf;
    len = 256;
    if (GetUserNameW(buf, &len) && buf[0]) return buf;
    return L"";
}

std::wstring XmlEscape(const std::wstring& s)
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
        default: out += c; break;
        }
    }
    return out;
}

} // namespace

AutostartState GetAutostart()
{
    ComScope com;
    auto root = OpenRootFolder();
    if (!root) return AutostartState::Missing;
    winrt::com_ptr<IRegisteredTask> task;
    if (FAILED(root->GetTask(_bstr_t(kTaskName), task.put()))) return AutostartState::Missing;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(task->get_Enabled(&enabled)) || !enabled) return AutostartState::Disabled;

    winrt::com_ptr<ITaskDefinition> def;
    if (SUCCEEDED(task->get_Definition(def.put())) && def)
    {
        winrt::com_ptr<ITriggerCollection> triggers;
        if (SUCCEEDED(def->get_Triggers(triggers.put())) && triggers)
        {
            winrt::com_ptr<ITrigger> trig;
            if (SUCCEEDED(triggers->get_Item(1, trig.put())) && trig)
            {
                VARIANT_BOOL trigEnabled = VARIANT_TRUE;
                if (SUCCEEDED(trig->get_Enabled(&trigEnabled)) && !trigEnabled) return AutostartState::Disabled;
            }
        }
    }
    return AutostartState::Enabled;
}

bool SetAutostart(bool enabled)
{
    ComScope com;
    auto root = OpenRootFolder();
    if (!root) return false;

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring user = CurrentUserAccount();
    if (user.empty()) return false;

    // Keep <Settings><Enabled>true</Enabled> so StartAutostartTask() can always launch the elevated
    // compositor on demand without a UAC prompt; the checkbox toggles <LogonTrigger><Enabled>.
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Description>Splits the configured display into native monitors (SplitDisplay).</Description></RegistrationInfo>"
        L"<Triggers><LogonTrigger><Enabled>" + std::wstring(enabled ? L"true" : L"false") + L"</Enabled>"
        L"<UserId>" + XmlEscape(user) + L"</UserId><Delay>PT3S</Delay></LogonTrigger></Triggers>"
        L"<Principals><Principal id=\"Author\"><UserId>" + XmlEscape(user) + L"</UserId>"
        L"<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
        L"<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure>"
        L"<Enabled>true</Enabled></Settings>"
        L"<Actions Context=\"Author\"><Exec>"
        L"<Command>&quot;" + XmlEscape(exe) + L"&quot;</Command><Arguments>run</Arguments>"
        L"</Exec></Actions></Task>";

    // Grant Interactive Users (IU) Generic Read + Generic Execute on the task object so the
    // unprivileged settings window can query task state (GetAutostart) and start the task on
    // demand (StartAutostartTask) without a UAC prompt, while only SY/BA can modify the task.
    winrt::com_ptr<IRegisteredTask> task;
    HRESULT hr = root->RegisterTask(_bstr_t(kTaskName), _bstr_t(xml.c_str()), TASK_CREATE_OR_UPDATE,
        _variant_t(), _variant_t(), TASK_LOGON_INTERACTIVE_TOKEN,
        _variant_t(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;IU)"), task.put());
    if (FAILED(hr))
    {
        Log(L"RegisterTask failed: 0x%08X", (UINT)hr);
        return false;
    }
    Log(L"autostart %s for %s: 0x%08X", enabled ? L"enabled" : L"disabled", user.c_str(), (UINT)hr);
    return true;
}

bool StartAutostartTask()
{
    ComScope com;
    auto root = OpenRootFolder();
    if (!root) return false;
    winrt::com_ptr<IRegisteredTask> task;
    if (FAILED(root->GetTask(_bstr_t(kTaskName), task.put()))) return false;
    winrt::com_ptr<IRunningTask> running;
    return SUCCEEDED(task->Run(_variant_t(), running.put()));
}
