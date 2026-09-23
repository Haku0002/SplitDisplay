// Settings window, shown when splitdisplay.exe is started without arguments.
//
// Left: status, the display to split, start/stop, autostart.
// Right: the layout editor. Edits go to a draft shown in the preview; "Apply layout" saves it and
// restarts a running split.

#include <Windows.h>
#include <CommCtrl.h>
#include <ShellScalingApi.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "autostart.h"
#include "layout.h"
#include "log.h"
#include "names.h"
#include "panel.h"
#include "shm.h"
#include "topology.h"

#ifndef SPLITDISPLAY_VERSION
#define SPLITDISPLAY_VERSION "dev"
#endif

namespace
{
enum : int
{
    IDC_TITLE = 100,
    IDC_VERSION,
    IDC_DRIVER_L,
    IDC_DRIVER,
    IDC_DISPLAY_L,
    IDC_DISPLAY,
    IDC_SPLIT_L,
    IDC_SPLIT,
    IDC_DISPLAYS_L,
    IDC_MAP,
    IDC_PICK_INFO,
    IDC_PICK,
    IDC_START,
    IDC_STOP,
    IDC_AUTOSTART,
    IDC_LOGS,
    IDC_UNINSTALL,
    IDC_HINT,
    IDC_LINK,
    // layout editor
    IDC_LAYOUT_L,
    IDC_PRESET,
    IDC_CANVAS,
    IDC_SEL,
    IDC_EQUAL_L,
    IDC_EQUAL_DIR,
    IDC_EQUAL_N,
    IDC_EQUAL_GO,
    IDC_MERGE,
    IDC_MANUAL_L,
    IDC_MANUAL_INFO,
    IDC_MANUAL_POS,
    IDC_MANUAL_SET,
    IDC_LAYOUT_STATE,
    IDC_LAYOUT_APPLY,
    IDC_LAYOUT_DISCARD,
};

constexpr UINT WM_APP_BUSY_DONE = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;
constexpr wchar_t kCanvasClass[] = L"SplitDisplayLayoutCanvas";
constexpr wchar_t kMapClass[] = L"SplitDisplayMonitorMap";

// One display on the monitor map, at its Windows desktop position.
struct MapItem
{
    PanelTarget target;
    RECT desk;         // desktop coordinates
    int splitInto = 0; // > 0: the selected display, currently split into this many monitors
};

struct Ui
{
    HWND wnd = nullptr;
    HFONT font = nullptr, bold = nullptr, title = nullptr, smallFont = nullptr;
    UINT dpi = 96;
    bool busy = false;
    bool splitActive = false;
    bool running = false;
    bool problem = false;

    // layout editor
    int panelW = 2560, panelH = 2880;
    LayoutNode draft;
    std::wstring applied; // serialized layout in config.ini
    std::optional<NodePath> selRegion;
    std::optional<LayoutSplitter> selSplitter;
    bool dragging = false;
    int hoverSplitter = -1;

    // monitor map
    std::vector<MapItem> map;
    std::wstring pick; // device path highlighted on the map
} g;

struct Item
{
    int id;
    const wchar_t* cls;
    const wchar_t* text;
    DWORD style;
    int x, y, w, h; // 96-DPI units
    int font;       // 0 normal, 1 bold, 2 title, 3 small
};

const Item kItems[] = {
    { IDC_TITLE, WC_STATIC, L"SplitDisplay", SS_LEFT, 16, 12, 250, 28, 2 },
    { IDC_VERSION, WC_STATIC, L"", SS_RIGHT, 700, 20, 184, 18, 3 },

    { IDC_DRIVER_L, WC_STATIC, L"Driver", SS_LEFT, 16, 56, 80, 20, 1 },
    { IDC_DRIVER, WC_STATIC, L"", SS_LEFT | SS_ENDELLIPSIS, 100, 56, 330, 20, 0 },
    { IDC_DISPLAY_L, WC_STATIC, L"Display", SS_LEFT, 16, 80, 80, 20, 1 },
    { IDC_DISPLAY, WC_STATIC, L"", SS_LEFT | SS_ENDELLIPSIS, 100, 80, 330, 20, 0 },
    { IDC_SPLIT_L, WC_STATIC, L"Split", SS_LEFT, 16, 104, 80, 20, 1 },
    { IDC_SPLIT, WC_STATIC, L"", SS_LEFT | SS_ENDELLIPSIS, 100, 104, 330, 20, 0 },
    { IDC_DISPLAYS_L, WC_STATIC, L"Displays: click the one to split", SS_LEFT, 16, 140, 414, 20, 1 },
    { IDC_MAP, kMapClass, L"", WS_TABSTOP, 16, 162, 414, 158, 0 },
    { IDC_PICK_INFO, WC_STATIC, L"", SS_LEFT | SS_ENDELLIPSIS, 16, 330, 270, 20, 0 },
    { IDC_PICK, WC_BUTTON, L"Split this display", BS_PUSHBUTTON | WS_TABSTOP, 294, 325, 136, 28, 0 },
    { IDC_START, WC_BUTTON, L"Start split", BS_PUSHBUTTON | WS_TABSTOP, 16, 366, 203, 34, 0 },
    { IDC_STOP, WC_BUTTON, L"Stop split", BS_PUSHBUTTON | WS_TABSTOP, 227, 366, 203, 34, 0 },
    { IDC_AUTOSTART, WC_BUTTON, L"Start splitting automatically when I sign in", BS_AUTOCHECKBOX | WS_TABSTOP, 16, 412, 414, 22, 0 },
    { IDC_LOGS, WC_BUTTON, L"Open logs", BS_PUSHBUTTON | WS_TABSTOP, 16, 444, 120, 28, 0 },
    { IDC_UNINSTALL, WC_BUTTON, L"Uninstall...", BS_PUSHBUTTON | WS_TABSTOP, 144, 444, 120, 28, 0 },
    { IDC_HINT, WC_STATIC, L"Emergency exit while split: Ctrl+Alt+Shift+F12", SS_LEFT, 16, 514, 414, 20, 3 },
    { IDC_LINK, WC_LINK, L"<a href=\"https://github.com/brkDOTstl/SplitDisplay\">github.com/brkDOTstl/SplitDisplay</a>", WS_TABSTOP, 16, 536, 414, 20, 3 },

    { IDC_LAYOUT_L, WC_STATIC, L"Layout", SS_LEFT, 452, 56, 100, 20, 1 },
    { IDC_PRESET, WC_COMBOBOX, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 452, 80, 250, 300, 0 },
    { IDC_CANVAS, kCanvasClass, L"", WS_TABSTOP, 452, 114, 250, 382, 0 },

    { IDC_SEL, WC_STATIC, L"", SS_LEFT, 716, 80, 168, 52, 0 },
    { IDC_EQUAL_L, WC_STATIC, L"Equal split", SS_LEFT, 716, 142, 168, 20, 1 },
    { IDC_EQUAL_DIR, WC_COMBOBOX, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 716, 164, 168, 200, 0 },
    { IDC_EQUAL_N, WC_COMBOBOX, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 716, 196, 70, 300, 0 },
    { IDC_EQUAL_GO, WC_BUTTON, L"Cut", BS_PUSHBUTTON | WS_TABSTOP, 794, 195, 90, 26, 0 },
    { IDC_MERGE, WC_BUTTON, L"Undo this cut", BS_PUSHBUTTON | WS_TABSTOP, 716, 230, 168, 26, 0 },
    { IDC_MANUAL_L, WC_STATIC, L"Manual", SS_LEFT, 716, 276, 168, 20, 1 },
    { IDC_MANUAL_INFO, WC_STATIC, L"", SS_LEFT, 716, 298, 168, 52, 0 },
    { IDC_MANUAL_POS, WC_EDIT, L"", ES_NUMBER | ES_RIGHT | WS_BORDER | WS_TABSTOP, 716, 354, 80, 24, 0 },
    { IDC_MANUAL_SET, WC_BUTTON, L"Move line", BS_PUSHBUTTON | WS_TABSTOP, 804, 353, 80, 26, 0 },

    { IDC_LAYOUT_STATE, WC_STATIC, L"", SS_LEFT, 716, 396, 168, 38, 3 },
    { IDC_LAYOUT_APPLY, WC_BUTTON, L"Apply layout", BS_PUSHBUTTON | WS_TABSTOP, 716, 438, 168, 30, 0 },
    { IDC_LAYOUT_DISCARD, WC_BUTTON, L"Discard changes", BS_PUSHBUTTON | WS_TABSTOP, 716, 472, 168, 26, 0 },
};
constexpr int kClientW = 900, kClientH = 568;

// Region fill colors (light) and the selection accent.
const COLORREF kRegionColors[] = { RGB(0xDC, 0xE9, 0xF8), RGB(0xE3, 0xF1, 0xDF), RGB(0xFB, 0xEB, 0xD7), RGB(0xEE, 0xE3, 0xF6),
    RGB(0xF9, 0xE0, 0xE3), RGB(0xDD, 0xF1, 0xF0), RGB(0xF3, 0xF0, 0xD6), RGB(0xE6, 0xE6, 0xE6) };
const COLORREF kAccent = RGB(0x00, 0x67, 0xC0);

int S(int v)
{
    return MulDiv(v, (int)g.dpi, 96);
}

HWND Ctl(int id)
{
    return GetDlgItem(g.wnd, id);
}

std::wstring ExeDir()
{
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L'\\'));
}

void MakeFonts()
{
    for (HFONT* f : { &g.font, &g.bold, &g.title, &g.smallFont })
        if (*f) DeleteObject(*f);
    NONCLIENTMETRICSW ncm{ sizeof(ncm) };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, g.dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    g.font = CreateFontIndirectW(&lf);
    LOGFONTW sm = lf;
    sm.lfHeight = sm.lfHeight * 9 / 10;
    g.smallFont = CreateFontIndirectW(&sm);
    lf.lfWeight = FW_SEMIBOLD;
    g.bold = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 3 / 2;
    g.title = CreateFontIndirectW(&lf);
}

void Layout()
{
    MakeFonts();
    for (auto& it : kItems)
    {
        HWND h = Ctl(it.id);
        SetWindowPos(h, nullptr, S(it.x), S(it.y), S(it.w), S(it.h), SWP_NOZORDER | SWP_NOACTIVATE);
        HFONT f = it.font == 2 ? g.title : it.font == 1 ? g.bold : it.font == 3 ? g.smallFont : g.font;
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    }
}

bool IsRunning()
{
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutexName);
    if (!m) return false;
    CloseHandle(m);
    return true;
}

std::wstring Trimmed(std::wstring s)
{
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s;
}

std::wstring Describe(const PanelTarget& t)
{
    std::wstring d = Trimmed(t.friendlyName);
    if (d.empty()) d = L"Unknown display";
    d += L"  " + std::to_wstring(t.width) + L"x" + std::to_wstring(t.height);
    auto c = ConnectorName(t.connector);
    if (!c.empty()) d += L"  " + c;
    return d;
}

// Every display on the desktop, plus the selected display while it is split (drawn where its
// split monitors are).
std::vector<MapItem> BuildMap()
{
    std::vector<MapItem> items;
    RECT splitArea{};
    int splitCount = 0;
    auto targets = EnumTargets();
    for (auto& t : targets)
    {
        if (!t.active || !t.width) continue;
        RECT r{ t.position.x, t.position.y, t.position.x + (LONG)t.width, t.position.y + (LONG)t.height };
        if (t.isVirtual)
        {
            splitArea = splitCount++ ? RECT{ std::min(splitArea.left, r.left), std::min(splitArea.top, r.top), std::max(splitArea.right, r.right),
                                            std::max(splitArea.bottom, r.bottom) }
                                     : r;
            continue;
        }
        items.push_back({ t, r, 0 });
    }
    if (splitCount)
    {
        for (auto& t : targets)
        {
            if (!t.active && !t.isVirtual && MatchesPanel(t))
            {
                items.push_back({ t, splitArea, splitCount });
                break;
            }
        }
    }
    return items;
}

const MapItem* PickedItem()
{
    for (auto& m : g.map)
        if (_wcsicmp(m.target.monitorDevicePath.c_str(), g.pick.c_str()) == 0) return &m;
    return nullptr;
}

struct MapTransform
{
    double scale, ox, oy;
    RECT ToCanvas(const RECT& r) const
    {
        return { (LONG)std::lround(ox + r.left * scale), (LONG)std::lround(oy + r.top * scale), (LONG)std::lround(ox + r.right * scale),
            (LONG)std::lround(oy + r.bottom * scale) };
    }
};

MapTransform MapTransformFor(HWND canvas)
{
    RECT rc;
    GetClientRect(canvas, &rc);
    if (g.map.empty()) return { 1, 0, 0 };
    RECT box = g.map[0].desk;
    for (auto& m : g.map)
        box = { std::min(box.left, m.desk.left), std::min(box.top, m.desk.top), std::max(box.right, m.desk.right), std::max(box.bottom, m.desk.bottom) };
    int margin = S(10);
    double sc = std::min((rc.right - 2.0 * margin) / std::max(1L, box.right - box.left), (rc.bottom - 2.0 * margin) / std::max(1L, box.bottom - box.top));
    double ox = (rc.right - (box.right - box.left) * sc) / 2 - box.left * sc;
    double oy = (rc.bottom - (box.bottom - box.top) * sc) / 2 - box.top * sc;
    return { sc, ox, oy };
}

void PaintMap(HWND canvas, HDC dc)
{
    RECT rc;
    GetClientRect(canvas, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    if (g.map.empty())
    {
        SelectObject(mem, g.font);
        SetTextColor(mem, GetSysColor(COLOR_GRAYTEXT));
        DrawTextW(mem, L"No displays found", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    auto tf = MapTransformFor(canvas);
    for (auto& m : g.map)
    {
        RECT r = tf.ToCanvas(m.desk);
        InflateRect(&r, -S(2), -S(2));
        bool picked = _wcsicmp(m.target.monitorDevicePath.c_str(), g.pick.c_str()) == 0;
        bool selected = MatchesPanel(m.target);
        HBRUSH fill = CreateSolidBrush(picked ? RGB(0xCC, 0xE4, 0xF7) : RGB(0xFF, 0xFF, 0xFF));
        FillRect(mem, &r, fill);
        DeleteObject(fill);
        HBRUSH edge = CreateSolidBrush(picked ? kAccent : RGB(0x80, 0x80, 0x80));
        RECT e = r;
        for (int k = 0; k < (picked ? S(3) : 1); k++)
        {
            FrameRect(mem, &e, edge);
            InflateRect(&e, -1, -1);
        }
        DeleteObject(edge);

        // Dashed lines show where a split display is currently cut.
        if (m.splitInto)
        {
            HPEN pen = CreatePen(PS_DOT, 1, RGB(0x40, 0x40, 0x40));
            HGDIOBJ old = SelectObject(mem, pen);
            for (auto& t : EnumTargets())
            {
                if (!t.active || !t.isVirtual) continue;
                // Only interior cuts: the top/left edges of pieces that are not on the outline.
                if (t.position.y > m.desk.top)
                {
                    RECT v = tf.ToCanvas({ t.position.x, t.position.y, t.position.x + (LONG)t.width, t.position.y + (LONG)t.height });
                    MoveToEx(mem, std::max(v.left, r.left), v.top, nullptr);
                    LineTo(mem, std::min(v.right, r.right), v.top);
                }
                if (t.position.x > m.desk.left)
                {
                    RECT v = tf.ToCanvas({ t.position.x, t.position.y, t.position.x + (LONG)t.width, t.position.y + (LONG)t.height });
                    MoveToEx(mem, v.left, std::max(v.top, r.top), nullptr);
                    LineTo(mem, v.left, std::min(v.bottom, r.bottom));
                }
            }
            SelectObject(mem, old);
            DeleteObject(pen);
        }

        std::wstring label = Trimmed(m.target.friendlyName);
        if (label.empty()) label = L"Display";
        label += L"\n" + std::to_wstring(m.target.width) + L"x" + std::to_wstring(m.target.height);
        if (m.splitInto) label += L"\nsplit into " + std::to_wstring(m.splitInto);
        else if (selected) label += L"\nselected";
        else if (m.target.primary) label += L"\nprimary";
        SelectObject(mem, selected ? g.bold : g.font);
        SetTextColor(mem, RGB(0x20, 0x20, 0x20));
        RECT measure = r;
        DrawTextW(mem, label.c_str(), -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        RECT t = r;
        // A split display keeps its label clear of the cut lines, at the top.
        t.top = m.splitInto ? r.top + S(6) : std::max(r.top, (r.top + r.bottom - (measure.bottom - measure.top)) / 2);
        DrawTextW(mem, label.c_str(), -1, &t, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void UpdatePick()
{
    const MapItem* m = PickedItem();
    SetWindowTextW(Ctl(IDC_PICK_INFO), m ? Describe(m->target).c_str() : L"Click a display above.");
    bool isCurrent = m && !PanelId().empty() && _wcsicmp(m->target.monitorDevicePath.c_str(), PanelId().c_str()) == 0;
    EnableWindow(Ctl(IDC_PICK), m && !isCurrent && !g.busy && m->splitInto == 0);
    InvalidateRect(Ctl(IDC_MAP), nullptr, FALSE);
}

void RefreshMap()
{
    g.map = BuildMap();
    if (g.pick.empty() || !PickedItem())
    {
        g.pick.clear();
        for (auto& m : g.map)
            if (MatchesPanel(m.target)) g.pick = m.target.monitorDevicePath;
    }
    UpdatePick();
}

LRESULT CALLBACK MapProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        PaintMap(wnd, dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    {
        SetFocus(wnd);
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        auto tf = MapTransformFor(wnd);
        for (auto& m : g.map)
        {
            RECT r = tf.ToCanvas(m.desk);
            if (PtInRect(&r, pt)) g.pick = m.target.monitorDevicePath;
        }
        UpdatePick();
        return 0;
    }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void UpdateAutostart()
{
    auto s = GetAutostart();
    Button_SetCheck(Ctl(IDC_AUTOSTART), s == AutostartState::Enabled ? BST_CHECKED : BST_UNCHECKED);
}

std::optional<PanelTarget> FindConfiguredPanel()
{
    return FindPanel();
}

#pragma region Layout editor

void InvalidateCanvas()
{
    InvalidateRect(Ctl(IDC_CANVAS), nullptr, FALSE);
}

void UpdateLayoutControls()
{
    int regions = CountRegions(g.draft);
    bool dirty = SerializeLayout(g.draft) != g.applied;
    wchar_t buf[160];
    swprintf_s(buf, L"%d region%s, %dx%d panel%s", regions, regions == 1 ? L"" : L"s", g.panelW, g.panelH,
        dirty ? L"\nUnsaved changes" : L"");
    SetWindowTextW(Ctl(IDC_LAYOUT_STATE), buf);
    EnableWindow(Ctl(IDC_LAYOUT_APPLY), dirty && !g.busy && regions >= 2);
    EnableWindow(Ctl(IDC_LAYOUT_DISCARD), dirty && !g.busy);

    auto regs = ResolveLayout(g.draft, g.panelW, g.panelH);
    if (g.selRegion)
    {
        auto it = std::find_if(regs.begin(), regs.end(), [](auto& r) { return r.path == *g.selRegion; });
        if (it == regs.end())
        {
            g.selRegion.reset();
        }
        else
        {
            const RECT& r = it->rc;
            swprintf_s(buf, L"Split %d\n%d x %d at (%d, %d)", (int)(it - regs.begin()) + 1, r.right - r.left, r.bottom - r.top, r.left, r.top);
            SetWindowTextW(Ctl(IDC_SEL), buf);
        }
    }
    if (!g.selRegion) SetWindowTextW(Ctl(IDC_SEL), L"Click a region in the preview to cut it.");
    EnableWindow(Ctl(IDC_EQUAL_GO), g.selRegion.has_value() && regions < kMaxRegions);
    EnableWindow(Ctl(IDC_MERGE), g.selRegion.has_value() && !g.selRegion->empty());

    if (g.selSplitter)
    {
        auto& s = *g.selSplitter;
        swprintf_s(buf, L"%s line at %s = %d\nAllowed: %d to %d", s.vertical ? L"Vertical" : L"Horizontal", s.vertical ? L"x" : L"y", s.pos, s.lo, s.hi);
        SetWindowTextW(Ctl(IDC_MANUAL_INFO), buf);
    }
    else
    {
        SetWindowTextW(Ctl(IDC_MANUAL_INFO), L"Drag a line in the preview, or click it and type an exact position.");
    }
    EnableWindow(Ctl(IDC_MANUAL_POS), g.selSplitter.has_value());
    EnableWindow(Ctl(IDC_MANUAL_SET), g.selSplitter.has_value());
}

void SetManualPos(int pos)
{
    SetWindowTextW(Ctl(IDC_MANUAL_POS), std::to_wstring(pos).c_str());
}

// Re-finds the selected splitter after the tree changed (positions and limits move).
void RefreshSelectedSplitter()
{
    if (!g.selSplitter) return;
    for (auto& s : ResolveSplitters(g.draft, g.panelW, g.panelH))
    {
        if (s.parent == g.selSplitter->parent && s.index == g.selSplitter->index)
        {
            g.selSplitter = s;
            return;
        }
    }
    g.selSplitter.reset();
}

void LayoutChanged()
{
    RefreshSelectedSplitter();
    UpdateLayoutControls();
    InvalidateCanvas();
}

void LoadDraftFromConfig()
{
    if (auto p = FindConfiguredPanel(); p && p->width && p->height)
    {
        g.panelW = (int)p->width;
        g.panelH = (int)p->height;
    }
    g.applied = LoadConfigValue(L"layout");
    LayoutNode n;
    if (g.applied.empty() || !ParseLayout(g.applied, n))
    {
        n = DefaultLayout(g.panelW, g.panelH);
        g.applied.clear();
    }
    NormalizeLayout(n, g.panelW, g.panelH);
    g.draft = n;
    if (g.applied.empty()) g.applied = SerializeLayout(g.draft); // the default is what runs today
    g.selRegion.reset();
    g.selSplitter.reset();
    SendMessageW(Ctl(IDC_PRESET), CB_SETCURSEL, 0, 0);
    LayoutChanged();
}

struct CanvasMap
{
    double scale;
    double ox, oy;
    int ToX(int x) const { return (int)std::lround(ox + x * scale); }
    int ToY(int y) const { return (int)std::lround(oy + y * scale); }
    int FromX(int x) const { return (int)std::lround((x - ox) / scale); }
    int FromY(int y) const { return (int)std::lround((y - oy) / scale); }
};

CanvasMap MapFor(HWND canvas)
{
    RECT rc;
    GetClientRect(canvas, &rc);
    int m = S(8);
    double sx = (rc.right - 2.0 * m) / g.panelW, sy = (rc.bottom - 2.0 * m) / g.panelH;
    double sc = std::max(0.01, std::min(sx, sy));
    return { sc, (rc.right - g.panelW * sc) / 2, (rc.bottom - g.panelH * sc) / 2 };
}

int HitSplitter(HWND canvas, int mx, int my)
{
    auto map = MapFor(canvas);
    auto list = ResolveSplitters(g.draft, g.panelW, g.panelH);
    int tol = S(5);
    for (int i = (int)list.size() - 1; i >= 0; i--)
    {
        auto& s = list[i];
        if (s.vertical)
        {
            int x = map.ToX(s.pos);
            if (std::abs(mx - x) <= tol && my >= map.ToY(s.line.top) && my <= map.ToY(s.line.bottom)) return i;
        }
        else
        {
            int y = map.ToY(s.pos);
            if (std::abs(my - y) <= tol && mx >= map.ToX(s.line.left) && mx <= map.ToX(s.line.right)) return i;
        }
    }
    return -1;
}

void PaintCanvas(HWND canvas, HDC dc)
{
    RECT rc;
    GetClientRect(canvas, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRect(mem, &rc, GetSysColorBrush(COLOR_WINDOW));

    auto map = MapFor(canvas);
    RECT panel{ map.ToX(0), map.ToY(0), map.ToX(g.panelW), map.ToY(g.panelH) };
    HBRUSH frame = CreateSolidBrush(RGB(0x60, 0x60, 0x60));
    RECT outer = panel;
    InflateRect(&outer, S(3), S(3));
    FillRect(mem, &outer, frame);
    DeleteObject(frame);

    auto regs = ResolveLayout(g.draft, g.panelW, g.panelH);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(mem, g.bold);
    for (size_t i = 0; i < regs.size(); i++)
    {
        const RECT& r = regs[i].rc;
        RECT c{ map.ToX(r.left), map.ToY(r.top), map.ToX(r.right), map.ToY(r.bottom) };
        HBRUSH b = CreateSolidBrush(kRegionColors[i % std::size(kRegionColors)]);
        FillRect(mem, &c, b);
        DeleteObject(b);

        if (g.selRegion && regs[i].path == *g.selRegion)
        {
            HBRUSH a = CreateSolidBrush(kAccent);
            RECT in = c;
            InflateRect(&in, -S(1), -S(1));
            for (int k = 0; k < S(3); k++)
            {
                FrameRect(mem, &in, a);
                InflateRect(&in, -1, -1);
            }
            DeleteObject(a);
        }

        wchar_t label[64];
        swprintf_s(label, L"%zu\n%d x %d", i + 1, r.right - r.left, r.bottom - r.top);
        SetTextColor(mem, RGB(0x20, 0x20, 0x20));
        RECT measure = c;
        DrawTextW(mem, label, -1, &measure, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        RECT t = c;
        t.top = (c.top + c.bottom - (measure.bottom - measure.top)) / 2;
        DrawTextW(mem, label, -1, &t, DT_CENTER | DT_WORDBREAK | DT_NOCLIP);
    }
    SelectObject(mem, oldFont);

    // Split lines: thin dark, hovered/selected thick accent.
    auto list = ResolveSplitters(g.draft, g.panelW, g.panelH);
    for (int i = 0; i < (int)list.size(); i++)
    {
        auto& s = list[i];
        bool sel = g.selSplitter && g.selSplitter->parent == s.parent && g.selSplitter->index == s.index;
        bool hot = sel || i == g.hoverSplitter;
        int w = hot ? S(4) : S(2);
        HBRUSH b = CreateSolidBrush(hot ? kAccent : RGB(0x40, 0x40, 0x40));
        RECT l;
        if (s.vertical)
        {
            int x = map.ToX(s.pos);
            l = { x - w / 2, map.ToY(s.line.top), x - w / 2 + w, map.ToY(s.line.bottom) };
        }
        else
        {
            int y = map.ToY(s.pos);
            l = { map.ToX(s.line.left), y - w / 2, map.ToX(s.line.right), y - w / 2 + w };
        }
        FillRect(mem, &l, b);
        DeleteObject(b);
    }

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK CanvasProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        PaintCanvas(wnd, dc);
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
    {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(wnd, &pt);
        bool vertical = false;
        int hit = -1;
        if (g.dragging && g.selSplitter)
        {
            vertical = g.selSplitter->vertical;
        }
        else
        {
            hit = HitSplitter(wnd, pt.x, pt.y);
            if (hit >= 0) vertical = ResolveSplitters(g.draft, g.panelW, g.panelH)[hit].vertical;
        }
        SetCursor(LoadCursorW(nullptr, (g.dragging || hit >= 0) ? (vertical ? IDC_SIZEWE : IDC_SIZENS) : IDC_ARROW));
        return TRUE;
    }
    case WM_LBUTTONDOWN:
    {
        SetFocus(wnd);
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int hit = HitSplitter(wnd, mx, my);
        if (hit >= 0)
        {
            g.selSplitter = ResolveSplitters(g.draft, g.panelW, g.panelH)[hit];
            SetManualPos(g.selSplitter->pos);
            g.dragging = true;
            SetCapture(wnd);
        }
        else
        {
            auto map = MapFor(wnd);
            int px = map.FromX(mx), py = map.FromY(my);
            g.selRegion.reset();
            for (auto& r : ResolveLayout(g.draft, g.panelW, g.panelH))
                if (px >= r.rc.left && px < r.rc.right && py >= r.rc.top && py < r.rc.bottom) g.selRegion = r.path;
        }
        LayoutChanged();
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (g.dragging && g.selSplitter)
        {
            auto map = MapFor(wnd);
            int pos = g.selSplitter->vertical ? map.FromX(mx) : map.FromY(my);
            if (!(GetKeyState(VK_SHIFT) & 0x8000)) pos = (pos + 4) / 8 * 8; // snap to 8 px; Shift = free
            pos = std::clamp(pos, g.selSplitter->lo, g.selSplitter->hi);
            if (pos != g.selSplitter->pos)
            {
                MoveSplitter(g.draft, *g.selSplitter, pos, g.panelW, g.panelH);
                RefreshSelectedSplitter();
                if (g.selSplitter) SetManualPos(g.selSplitter->pos);
                UpdateLayoutControls();
                InvalidateCanvas();
            }
            return 0;
        }
        int hit = HitSplitter(wnd, mx, my);
        if (hit != g.hoverSplitter)
        {
            g.hoverSplitter = hit;
            InvalidateCanvas();
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, wnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g.hoverSplitter != -1)
        {
            g.hoverSplitter = -1;
            InvalidateCanvas();
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (g.dragging)
        {
            g.dragging = false;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            LayoutChanged();
        }
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

void OnPreset()
{
    int sel = (int)SendMessageW(Ctl(IDC_PRESET), CB_GETCURSEL, 0, 0);
    auto& presets = LayoutPresets();
    if (sel <= 0 || sel > (int)presets.size()) return;
    g.draft = presets[sel - 1].make(g.panelW, g.panelH);
    NormalizeLayout(g.draft, g.panelW, g.panelH);
    g.selRegion.reset();
    g.selSplitter.reset();
    LayoutChanged();
}

void OnEqualSplit()
{
    if (!g.selRegion) return;
    auto kind = SendMessageW(Ctl(IDC_EQUAL_DIR), CB_GETCURSEL, 0, 0) == 0 ? LayoutNode::Rows : LayoutNode::Cols;
    int parts = (int)SendMessageW(Ctl(IDC_EQUAL_N), CB_GETCURSEL, 0, 0) + 2;
    NodePath path = *g.selRegion;
    if (!SplitEqual(g.draft, path, kind, parts, g.panelW, g.panelH))
    {
        wchar_t msg[200];
        swprintf_s(msg, L"Cannot cut this region into %d parts: at most %d regions in total, each at least %d pixels.", parts, kMaxRegions, kMinRegionSize);
        MessageBoxW(g.wnd, msg, L"SplitDisplay", MB_ICONINFORMATION);
        return;
    }
    path.push_back(0); // select the first new piece so it can be cut further
    g.selRegion = path;
    g.selSplitter.reset();
    LayoutChanged();
}

void OnMerge()
{
    if (!g.selRegion || g.selRegion->empty()) return;
    NodePath parent(g.selRegion->begin(), g.selRegion->end() - 1);
    if (MergeParent(g.draft, *g.selRegion))
    {
        g.selRegion = parent;
        g.selSplitter.reset();
        NormalizeLayout(g.draft, g.panelW, g.panelH);
        LayoutChanged();
    }
}

void OnManualSet()
{
    if (!g.selSplitter) return;
    wchar_t text[32];
    GetWindowTextW(Ctl(IDC_MANUAL_POS), text, 32);
    MoveSplitter(g.draft, *g.selSplitter, _wtoi(text), g.panelW, g.panelH);
    RefreshSelectedSplitter();
    if (g.selSplitter) SetManualPos(g.selSplitter->pos);
    LayoutChanged();
}

#pragma endregion

void UpdateStatus()
{
    wchar_t buf[256];
    SharedView shm;
    bool driver = shm.Open();
    SetWindowTextW(Ctl(IDC_DRIVER), driver ? L"Installed and loaded" : L"Not loaded (reinstall SplitDisplay)");

    std::wstring name = Trimmed(PanelNamePrefix());
    const wchar_t* prefix = name.empty() ? L"(none)" : name.c_str();
    auto panel = FindConfiguredPanel();
    bool first = IsTargetActive(L"Split 1");

    g.running = IsRunning();
    g.splitActive = first && panel && !panel->active;
    g.problem = !driver;
    if (!HasPanelSelection())
    {
        swprintf_s(buf, L"No display selected: click one below");
    }
    else if (!panel)
    {
        swprintf_s(buf, L"'%s' is not connected", prefix);
    }
    else if (g.splitActive)
    {
        swprintf_s(buf, L"%s: split into %u monitors @ %u Hz", prefix, driver ? shm->config.count : 0, driver ? shm->config.refreshHz : 0);
    }
    else if (panel->active)
    {
        swprintf_s(buf, L"%s: %ux%u @ %.0f Hz, one display", prefix, panel->width, panel->height, panel->refreshHz);
    }
    else
    {
        swprintf_s(buf, L"%s: removed from the desktop", prefix);
    }
    SetWindowTextW(Ctl(IDC_DISPLAY), buf);

    const wchar_t* split = g.splitActive ? L"Active"
                         : g.running    ? L"Running, waiting for the display"
                                        : L"Off";
    SetWindowTextW(Ctl(IDC_SPLIT), split);

    EnableWindow(Ctl(IDC_START), !g.busy && driver && !g.running);
    EnableWindow(Ctl(IDC_STOP), !g.busy && g.running);
    EnableWindow(Ctl(IDC_UNINSTALL), !g.busy && GetFileAttributesW((ExeDir() + L"\\Uninstall.cmd").c_str()) != INVALID_FILE_ATTRIBUTES);
    UpdateLayoutControls();
    RefreshMap();
}

void StartCompositor()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" run";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi) ||
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
    {
        Log(L"gui: failed to start compositor %lu", GetLastError());
    }
}

// Signals a running compositor and waits until it has restored the display and exited.
void StopCompositor()
{
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!e) return;
    SetEvent(e);
    CloseHandle(e);
    for (int i = 0; i < 300 && IsRunning(); i++) Sleep(100);
}

// Runs slow work off the UI thread with the buttons disabled.
template <class F>
void Busy(F&& work)
{
    g.busy = true;
    UpdateStatus();
    std::thread([work = std::forward<F>(work)] {
        work();
        PostMessageW(g.wnd, WM_APP_BUSY_DONE, 0, 0);
    }).detach();
}

void RestartIfRunning()
{
    if (!IsRunning()) return;
    Busy([] {
        StopCompositor();
        StartCompositor();
        Sleep(1500);
    });
}

void OnPick()
{
    const MapItem* m = PickedItem();
    if (!m) return;
    SelectPanel(m->target);
    Log(L"gui: display to split set to '%s' (%s)", m->target.friendlyName.c_str(), m->target.monitorDevicePath.c_str());
    LoadDraftFromConfig();
    RestartIfRunning();
    UpdateStatus();
}

void OnApplyLayout()
{
    NormalizeLayout(g.draft, g.panelW, g.panelH);
    g.applied = SerializeLayout(g.draft);
    SaveConfigValue(L"layout", g.applied);
    Log(L"gui: layout set to %s", g.applied.c_str());
    RestartIfRunning();
    UpdateStatus();
}

void OnUninstall()
{
    if (MessageBoxW(g.wnd, L"Remove SplitDisplay, its driver and its certificate, and restore the display?", L"Uninstall SplitDisplay",
            MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    ShellExecuteW(nullptr, L"open", (ExeDir() + L"\\Uninstall.cmd").c_str(), nullptr, ExeDir().c_str(), SW_SHOWNORMAL);
    DestroyWindow(g.wnd);
}

void InitLayoutControls()
{
    HWND preset = Ctl(IDC_PRESET);
    SendMessageW(preset, CB_ADDSTRING, 0, (LPARAM)L"Presets...");
    for (auto& p : LayoutPresets()) SendMessageW(preset, CB_ADDSTRING, 0, (LPARAM)p.name);
    SendMessageW(preset, CB_SETCURSEL, 0, 0);

    HWND dir = Ctl(IDC_EQUAL_DIR);
    SendMessageW(dir, CB_ADDSTRING, 0, (LPARAM)L"Rows (top / bottom)");
    SendMessageW(dir, CB_ADDSTRING, 0, (LPARAM)L"Columns (left / right)");
    SendMessageW(dir, CB_SETCURSEL, 0, 0);

    HWND n = Ctl(IDC_EQUAL_N);
    for (int i = 2; i <= kMaxRegions; i++) SendMessageW(n, CB_ADDSTRING, 0, (LPARAM)std::to_wstring(i).c_str());
    SendMessageW(n, CB_SETCURSEL, 0, 0);
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g.wnd = wnd;
        g.dpi = GetDpiForWindow(wnd);
        for (auto& it : kItems)
            CreateWindowExW(0, it.cls, it.text, WS_CHILD | WS_VISIBLE | it.style, 0, 0, 10, 10, wnd, (HMENU)(INT_PTR)it.id, nullptr, nullptr);
        SetWindowTextW(Ctl(IDC_VERSION), L"v" _CRT_WIDE(SPLITDISPLAY_VERSION));
        Layout();
        InitLayoutControls();
        LoadDraftFromConfig();
        UpdateAutostart();
        UpdateStatus();
        SetTimer(wnd, kTimer, 1000, nullptr);
        return 0;
    }
    case WM_DPICHANGED:
    {
        g.dpi = HIWORD(wp);
        auto* r = (RECT*)lp;
        SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        InvalidateCanvas();
        return 0;
    }
    case WM_TIMER:
        if (!g.busy && !g.dragging) UpdateStatus();
        return 0;
    case WM_APP_BUSY_DONE:
        g.busy = false;
        UpdateStatus();
        return 0;
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        int id = GetDlgCtrlID((HWND)lp);
        if (id == IDC_SPLIT) SetTextColor(dc, g.splitActive ? RGB(0x10, 0x7C, 0x10) : GetSysColor(COLOR_WINDOWTEXT));
        if (id == IDC_DRIVER) SetTextColor(dc, g.problem ? RGB(0xC4, 0x2B, 0x1C) : GetSysColor(COLOR_WINDOWTEXT));
        if (id == IDC_VERSION || id == IDC_HINT || id == IDC_LAYOUT_STATE || id == IDC_MANUAL_INFO) SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_PICK: OnPick(); break;
        case IDC_START:
            Busy([] {
                StartCompositor();
                for (int i = 0; i < 100 && !IsRunning(); i++) Sleep(50);
            });
            break;
        case IDC_STOP: Busy([] { StopCompositor(); }); break;
        case IDC_AUTOSTART:
        {
            bool on = Button_GetCheck(Ctl(IDC_AUTOSTART)) == BST_CHECKED;
            if (!SetAutostart(on)) MessageBoxW(wnd, L"Could not change the logon task. See the log for details.", L"SplitDisplay", MB_ICONWARNING);
            UpdateAutostart();
            break;
        }
        case IDC_LOGS:
        {
            wchar_t dir[MAX_PATH];
            ExpandEnvironmentStringsW(L"%ProgramData%\\SplitDisplay", dir, MAX_PATH);
            ShellExecuteW(nullptr, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case IDC_UNINSTALL: OnUninstall(); break;
        case IDC_PRESET:
            if (HIWORD(wp) == CBN_SELCHANGE) OnPreset();
            break;
        case IDC_EQUAL_GO: OnEqualSplit(); break;
        case IDC_MERGE: OnMerge(); break;
        case IDC_MANUAL_SET: OnManualSet(); break;
        case IDC_LAYOUT_APPLY: OnApplyLayout(); break;
        case IDC_LAYOUT_DISCARD: LoadDraftFromConfig(); break;
        }
        return 0;
    case WM_NOTIFY:
    {
        auto* n = (NMHDR*)lp;
        if (n->idFrom == IDC_LINK && (n->code == NM_CLICK || n->code == NM_RETURN))
            ShellExecuteW(nullptr, L"open", ((NMLINK*)lp)->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(wnd, kTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}
} // namespace

int RunGui()
{
    HANDLE single = CreateMutexW(nullptr, TRUE, kGuiMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (HWND w = FindWindowW(kGuiWindowClass, nullptr))
        {
            ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
        return 0;
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW cc{ sizeof(cc) };
    cc.style = CS_HREDRAW | CS_VREDRAW;
    cc.lpfnWndProc = CanvasProc;
    cc.hInstance = inst;
    cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cc.lpszClassName = kCanvasClass;
    RegisterClassExW(&cc);
    cc.lpfnWndProc = MapProc;
    cc.lpszClassName = kMapClass;
    RegisterClassExW(&cc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = kGuiWindowClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    // Size the window for the DPI of the monitor it will open on.
    POINT pt{};
    GetCursorPos(&pt);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    g.dpi = dpiX;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{ 0, 0, S(kClientW), S(kClientH) };
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g.dpi);
    HWND wnd = CreateWindowExW(0, kGuiWindowClass, L"SplitDisplay", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, inst, nullptr);
    ShowWindow(wnd, SW_SHOWNORMAL);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (IsDialogMessageW(wnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(single);
    return 0;
}
