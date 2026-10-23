#pragma once
// Window-mode desktop guard: the physical panels stay on the desktop, parked in a corner
// behind their compositor windows. The guard keeps the cursor and stray application
// windows off them, and remembers window placements across the short periods where the
// split steps aside for another desktop (lock screen, secure prompts).

#include <Windows.h>

#include <atomic>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

// Finds a parked panel's desktop rect. Resolving the GDI name goes through QueryDisplayConfig,
// which takes about a millisecond, so it is cached and only re-resolved now and then, or when
// it stops matching a monitor (the rect lookup itself is cheap).
struct PanelLocator
{
    LUID adapter{};
    UINT32 targetId = 0;
    std::wstring gdi;
    int uses = 0;

    bool Rect(RECT& out);
};

struct ParkedPanel
{
    PanelLocator where;
    std::wstring homeName; // the panel's first split monitor ("Split N"): stray windows are moved there
    RECT home{};           // its desktop rect (the split monitors can be moved around as a group)
    RECT rect{};           // where the panel is on the desktop now (refreshed by the guard)
};

// Owns the guard thread. The low-level mouse hook runs on that thread and reaches the
// state through a file-static pointer to the one active guard, so no locking is needed:
// every reader and writer of the rects is the guard thread itself.
class WindowGuard
{
public:
    void Start(std::vector<ParkedPanel> parked);
    void Stop();
    ~WindowGuard() { Stop(); }

private:
    void Run();
    void RefreshRects(bool homes);
    const ParkedPanel* ParkedAt(POINT pt) const;
    POINT NearestOpenPoint(POINT pt) const;
    static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp);
    static BOOL CALLBACK SweepWindow(HWND hwnd, LPARAM self);

    std::thread m_Thread;
    std::atomic_bool m_Quit = false;
    std::vector<ParkedPanel> m_Parked;
    std::vector<RECT> m_OpenMonitors; // every monitor the cursor may use
    std::set<HWND> m_Remaximize;      // restored on a parked panel, to maximize again once moved
    std::map<HWND, int> m_MoveTries;  // windows the guard tried to move off a parked panel
};

// Window placement memory for step-asides: taken before the split monitors are unplugged
// (which piles every window onto the remaining display) and reapplied once they are back.
void SaveWindowLayout();
void RestoreWindowLayout(); // runs the actual moves on its own thread
