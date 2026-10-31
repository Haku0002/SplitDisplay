#include "guard.h"

#include <dwmapi.h>

#include <algorithm>
#include <climits>

#include "log.h"
#include "names.h"
#include "topology.h"

static bool MonitorRectByName(const std::wstring& gdiName, RECT& out)
{
    struct Ctx
    {
        const std::wstring* name;
        RECT* out;
        bool found;
    } c{ &gdiName, &out, false };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m, &mi) && _wcsicmp(mi.szDevice, c->name->c_str()) == 0)
        {
            *c->out = mi.rcMonitor;
            c->found = true;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&c);
    return c.found;
}

bool PanelLocator::Rect(RECT& out)
{
    if (gdi.empty() || ++uses % 10 == 0) gdi = GdiNameOfTarget(adapter, targetId);
    if (!gdi.empty() && MonitorRectByName(gdi, out)) return true;
    gdi = GdiNameOfTarget(adapter, targetId);
    return !gdi.empty() && MonitorRectByName(gdi, out);
}

#pragma region WindowGuard

static WindowGuard* g_activeGuard = nullptr; // read by the mouse hook, on the guard thread

void WindowGuard::Start(std::vector<ParkedPanel> parked)
{
    Stop();
    m_Parked = std::move(parked);
    m_Remaximize.clear();
    m_MoveTries.clear();
    m_Quit = false;
    g_activeGuard = this;
    m_Thread = std::thread(&WindowGuard::Run, this);
}

void WindowGuard::Stop()
{
    if (!m_Thread.joinable()) return;
    m_Quit = true;
    m_Thread.join();
    if (g_activeGuard == this) g_activeGuard = nullptr;
}

void WindowGuard::RefreshRects(bool homes)
{
    for (auto& p : m_Parked)
    {
        if (!p.where.Rect(p.rect)) p.rect = {};
        if (homes) DesktopRectOf(p.homeName, p.home); // keeps the last known rect if it is off right now
    }
    m_OpenMonitors.clear();
    struct Ctx
    {
        WindowGuard* self;
    } ctx{ this }; 
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* self = ((Ctx*)lp)->self;
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(m, &mi)) return TRUE;
        for (auto& p : self->m_Parked)
            if (EqualRect(&p.rect, &mi.rcMonitor)) return TRUE;
        self->m_OpenMonitors.push_back(mi.rcMonitor);
        return TRUE;
    }, (LPARAM)&ctx);
}

const ParkedPanel* WindowGuard::ParkedAt(POINT pt) const
{
    for (auto& p : m_Parked)
        if (PtInRect(&p.rect, pt)) return &p;
    return nullptr;
}

POINT WindowGuard::NearestOpenPoint(POINT pt) const
{
    POINT best = pt;
    LONGLONG bestDist = LLONG_MAX;
    for (auto& r : m_OpenMonitors)
    {
        POINT c{ std::clamp(pt.x, r.left, r.right - 1), std::clamp(pt.y, r.top, r.bottom - 1) };
        LONGLONG d = (LONGLONG)(c.x - pt.x) * (c.x - pt.x) + (LONGLONG)(c.y - pt.y) * (c.y - pt.y);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

LRESULT CALLBACK WindowGuard::MouseProc(int code, WPARAM wp, LPARAM lp)
{
    WindowGuard* self = g_activeGuard;
    if (self && code == HC_ACTION && wp == WM_MOUSEMOVE && !self->m_OpenMonitors.empty())
    {
        POINT pt = ((MSLLHOOKSTRUCT*)lp)->pt;
        if (self->ParkedAt(pt))
        {
            // Swallow the move and put the cursor at the nearest point of an open monitor.
            POINT to = self->NearestOpenPoint(pt);
            SetCursorPos(to.x, to.y);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Moves application windows that ended up on a parked panel (hidden behind its compositor window)
// to the panel's first split monitor. Async calls only: a hung application must not stall the
// thread that also runs the mouse hook.
BOOL CALLBACK WindowGuard::SweepWindow(HWND hwnd, LPARAM lp)
{
    auto* self = (WindowGuard*)lp;
    // Cheap checks first: this runs for every top-level window twice a second, on the thread that
    // also serves the mouse hook.
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    auto* p = self->ParkedAt({ (r.left + r.right) / 2, (r.top + r.bottom) / 2 });
    if (!p)
    {
        // Moved off a parked panel after being restored there: maximize it again in its new place.
        if (self->m_Remaximize.erase(hwnd)) ShowWindowAsync(hwnd, SW_MAXIMIZE);
        self->m_MoveTries.erase(hwnd);
        return TRUE;
    }
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    for (auto* shell : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", kCompositorWindowClass })
        if (wcscmp(cls, shell) == 0) return TRUE;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;

    // A window that keeps coming back (it positions itself, or ignores the move) is left alone after a
    // few seconds rather than fought over twice a second.
    int& tries = self->m_MoveTries[hwnd];
    if (++tries > 10) return TRUE;
    wchar_t title[128] = {};
    GetWindowTextW(hwnd, title, 128);
    if (tries == 10) Log(L"window '%s' (%s) keeps returning to a parked panel; leaving it there", title, cls);

    // A maximized window is restored first and moved on a later sweep, once the restore is done
    // (async calls to another thread may run in any order), then maximized again where it landed.
    if (IsZoomed(hwnd))
    {
        ShowWindowAsync(hwnd, SW_RESTORE);
        self->m_Remaximize.insert(hwnd);
        return TRUE;
    }
    LONG hw = p->home.right - p->home.left, hh = p->home.bottom - p->home.top;
    LONG w = std::min(r.right - r.left, hw), h = std::min(r.bottom - r.top, hh);
    SetWindowPos(hwnd, nullptr, p->home.left + (hw - w) / 2, p->home.top + (hh - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    if (tries == 1) Log(L"moved window '%s' (%s) off a parked panel", title, cls);
    return TRUE;
}

void WindowGuard::Run()
{
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    if (!hook) Log(L"warning: mouse guard not installed (%lu); the cursor can reach the parked panel", GetLastError());
    ULONGLONG nextSweep = 0;
    int sweeps = 0;
    while (!m_Quit)
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (GetTickCount64() >= nextSweep)
        {
            nextSweep = GetTickCount64() + 500;
            RefreshRects(sweeps++ % 10 == 0); // split monitor rects every 5 s: a QueryDisplayConfig
            for (auto it = m_Remaximize.begin(); it != m_Remaximize.end();)
                it = IsWindow(*it) ? std::next(it) : m_Remaximize.erase(it);
            for (auto it = m_MoveTries.begin(); it != m_MoveTries.end();)
                it = IsWindow(it->first) ? std::next(it) : m_MoveTries.erase(it);
            EnumWindows(SweepWindow, (LPARAM)this);
        }
        // The hook only sees mouse input; programs can still place the cursor with SetCursorPos.
        POINT cur;
        if (!m_OpenMonitors.empty() && GetCursorPos(&cur) && ParkedAt(cur))
        {
            POINT to = NearestOpenPoint(cur);
            SetCursorPos(to.x, to.y);
        }
    }
    if (hook) UnhookWindowsHookEx(hook);
}

#pragma endregion

#pragma region Saved window layout

// A session that ends because the desktop went away (lock screen, Ctrl+Alt+Del, a prompt on its own
// desktop) unplugs the split monitors, and Windows piles every window onto the one display left
// behind. Their placement is taken first and applied again once the split is back, so an
// interruption costs a flicker instead of the arrangement the user had set up.
struct SavedWindow
{
    HWND hwnd;
    RECT rect;      // where to put it back, in screen coordinates
    bool maximized; // then maximize it there again
};
static std::vector<SavedWindow> g_savedWindows;

static bool ArrangeableWindow(HWND hwnd)
{
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    for (auto* shell : { L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", kCompositorWindowClass })
        if (wcscmp(cls, shell) == 0) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    return true;
}

// Everything is kept in screen coordinates. WINDOWPLACEMENT's rectangle is relative to the primary
// monitor's work area instead, and that work area is not back yet when the windows are put back
// (the taskbar is still being rebuilt), which would shift every window by the taskbar's size.
void SaveWindowLayout()
{
    g_savedWindows.clear();
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) work = {};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        if (!ArrangeableWindow(hwnd)) return TRUE;
        RECT rc;
        if (!GetWindowRect(hwnd, &rc)) return TRUE;
        WINDOWPLACEMENT wp{ sizeof(wp) };
        bool maximized = GetWindowPlacement(hwnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED;
        if (maximized)
        {
            // Its screen rectangle is the whole monitor; the restored one says where to maximize it.
            const RECT& work = *(const RECT*)lp;
            rc = wp.rcNormalPosition;
            OffsetRect(&rc, work.left, work.top);
        }
        g_savedWindows.push_back({ hwnd, rc, maximized });
        return TRUE;
    }, (LPARAM)&work);
    Log(L"noted where %zu windows sit", g_savedWindows.size());
}

// Runs on its own thread, so a slow application cannot hold up the split that has just come back.
// A maximized window is placed while restored first, so it maximizes on the right monitor.
void RestoreWindowLayout()
{
    if (g_savedWindows.empty()) return;
    auto saved = std::move(g_savedWindows);
    g_savedWindows.clear();
    HWND front = GetForegroundWindow();
    std::thread([saved = std::move(saved), front] {
        Sleep(150); // let Windows settle the split monitors it has just placed
        auto alive = [](HWND hwnd) { return IsWindow(hwnd) && IsWindowVisible(hwnd); };
        // Restored first, so the move below decides which monitor each one ends up on.
        for (auto& w : saved)
            if (alive(w.hwnd) && IsZoomed(w.hwnd)) ShowWindowAsync(w.hwnd, SW_RESTORE);
        Sleep(80);

        // Asked asynchronously and checked afterwards: an application still laying itself out after
        // the monitor change can take a moment to accept a move, or undo it once, and a synchronous
        // call would make every other window wait for the slowest one. Each window leaves the rounds
        // as soon as it is in place, so the pass ends with the last window that moves.
        struct Pending
        {
            const SavedWindow* w;
            int tries;
        };
        std::vector<Pending> pending;
        for (auto& w : saved) pending.push_back({ &w, 0 });

        int moved = 0, zoomed = 0;
        while (!pending.empty())
        {
            for (auto& p : pending)
                if (alive(p.w->hwnd))
                    SetWindowPos(p.w->hwnd, nullptr, p.w->rect.left, p.w->rect.top, p.w->rect.right - p.w->rect.left,
                        p.w->rect.bottom - p.w->rect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            Sleep(120);
            for (auto it = pending.begin(); it != pending.end();)
            {
                RECT now;
                bool gone = !alive(it->w->hwnd);
                bool there = !gone && GetWindowRect(it->w->hwnd, &now) && EqualRect(&now, &it->w->rect);
                if (there) moved++;
                // One that will not take the move (a system window, or one that places itself) is
                // given up on rather than holding the pass up.
                if (gone || there || ++it->tries >= 4) it = pending.erase(it);
                else ++it;
            }
        }
        Sleep(120);
        // ShowWindow, not a posted SW_MAXIMIZE: some applications ignore that one.
        for (auto& w : saved)
            if (w.maximized && alive(w.hwnd) && ShowWindow(w.hwnd, SW_MAXIMIZE) >= 0) zoomed++;
        if (front && IsWindow(front)) SetForegroundWindow(front);
        Log(L"put %d windows back where they were (%d maximized)", moved, zoomed);
    }).detach();
}

#pragma endregion
