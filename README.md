# SplitDisplay

Turn one physical display into several **real** Windows monitors.

> Vibe-coded with [Claude](https://claude.com/claude-code): the driver, compositor, reverse engineering and docs were
> written by Claude (Anthropic) under the direction of the maintainer, and tested on real hardware.

SplitDisplay was built for foldable dual-panel portable monitors (such as the *Sculptor* 2 x 2560x1440 foldable).
Over DisplayPort these panels use MST and Windows sees two monitors. Over HDMI they show up as a single tall
2560x2880 monitor. Windows then maximizes and fullscreens across both panels, and the taskbar and snapping treat
them as one screen.

Window-manager tools such as FancyZones and DisplayFusion's monitor splitting only *emulate* separate monitors.
SplitDisplay does not emulate anything. Windows sees genuine monitors: each has its own resolution, scaling and
taskbar, and maximize, fullscreen, `Win+Arrow` and DXGI fullscreen all behave natively.

You choose how to cut the panel: the default is two halves, and the layout editor splits it into up to 8 monitors of
any size, with equal cuts and freely placed lines that can be nested inside one another.

All split monitors share the physical panel's refresh rate. A single cable carries one signal, so they cannot
refresh independently.

## How it works

```
Windows desktop --> virtual monitor "Split 1"     (2560x1440@100) --+
                \-> virtual monitor "Split 2"     (2560x1440@100) --+  SplitDisplayIdd (IddCx UMDF driver)
                                                                     |  shared D3D11 textures + fences,
                                                                     v  same GPU, no CPU copies
                                  splitdisplay.exe (compositor)
                                  copies each frame into its region of the panel
                                                                     |  Windows.Devices.Display.Core
                                                                     v  exclusive ownership
                                  physical panel, removed from the desktop (2560x2880@100 over HDMI)
```

1. **`SplitDisplayIdd`** is an Indirect Display driver based on Microsoft's IddSampleDriver. It plugs virtual
   monitors on request (up to 8), each with a generated EDID and modes matching its region. It asks Windows to render them on
   the panel's GPU and republishes every desktop frame into a ring of shared textures.
2. **`splitdisplay.exe`** removes the physical panel from the Windows desktop. This is the same mechanism as
   *Settings > Display > Advanced display > Remove display from desktop*. The program then takes exclusive ownership
   of the panel with the DisplayCore API and, on every vblank, copies the newest frame of each virtual monitor into
   its region.

The DWM composites the virtual monitors like any other display. The lock screen, UAC prompts and windows excluded
from screen capture therefore all show up normally, which a screen-capture approach cannot do.

### Removing a display from the desktop

The documented `DISPLAYCONFIG_DEVICE_INFO_SET_MONITOR_SPECIALIZATION` packet returns `ERROR_INVALID_PARAMETER` here.
The Settings toggle sends an undocumented `DisplayConfigSetDeviceInfo` packet instead. It was reverse engineered from
`SettingsHandlers_PCDisplay.dll` and `SystemSettingsAdminFlows.exe` using Microsoft's public symbols:

| offset | field |
|---|---|
| 0  | `DISPLAYCONFIG_DEVICE_INFO_HEADER`, `type = -23`, `size = 48`, target adapter LUID and id |
| 20 | `GUID_MONITOR_OVERRIDE_PSEUDO_SPECIALIZED` `{F196C02F-F86F-4F9A-AA15-E9CEBDFE3B96}` (ntddvdeo.h) |
| 36 | `INT32 enable` |
| 40 | `UINT64 hash` |

To compute `hash`, take the target's `StableMonitorId` followed by the GUID string (`{...}`) and split it at half its
byte length, rounded down to whole characters. Each half is hashed with
`RtlHashUnicodeString(case-insensitive, HASH_STRING_ALGORITHM_DEFAULT)`. The first half's hash is the low 32 bits
and the second half's hash is the high 32 bits. The call requires elevation. Windows refuses to remove the last
remaining desktop display. Disabling an override that is not set returns `ERROR_GEN_FAILURE`. See
`src/common/panel.cpp`.

## Download

Prebuilt zips are on the [Releases](https://github.com/brkDOTstl/SplitDisplay/releases) page. Unzip and double-click
`Install.cmd`; the display to split is detected automatically (with several displays, click it on the map). The installer signs the bundled driver with a certificate created on
your machine and deletes its private key right after. Nothing needs to be built, and test-signing mode is not used.

## Requirements

- Windows 11 with a GPU driver that supports display specialization: the Settings toggle
  "Remove display from desktop" must be available for the panel. Nothing is vendor specific. Developed and tested on
  an NVIDIA RTX 4060; AMD and Intel are untested. The virtual monitors must render on the GPU the panel is attached
  to. The driver requests this, and the log warns if Windows picks another GPU.
- Visual Studio 2022/2026 with the C++ workload and the Windows 11 SDK (10.0.26100), plus CMake.
- Administrator rights.

## Build

```powershell
scripts\fetch-deps.ps1                     # WDK NuGet package -> third_party\wdk (no system WDK install)
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

## Install

The driver is signed with a local self-signed certificate. It is a user-mode (UMDF) driver, so a certificate
trusted by the machine is enough: **test-signing mode is not needed**.

```powershell
scripts\make-cert.ps1                      # create the signing cert (private key non-exportable)
scripts\trust-cert.ps1                     # elevated: trust it in LocalMachine\Root + TrustedPublisher
scripts\package-driver.ps1                 # sign dll, generate + sign the catalog -> out\driver
build\Release\sdctl.exe driver-install out\driver\SplitDisplayIdd.inf
scripts\install.ps1                        # elevated: Program Files + "SplitDisplay" logon task
```

Trusting a self-signed root lets anything signed with that key install drivers on the machine. Keep the private
key safe, or delete it from `CurrentUser\My` once the driver is installed.

## Usage

Start `splitdisplay.exe` without arguments (the installer opens it for you) to get the settings window:

- **Status**: driver, display and split state.
- **Displays**: a map of all connected displays at their desktop positions (name, resolution, connector). Click
  the one to split and press *Split this display*. Other displays are left where they are.
- **Start / Stop split** and **Start automatically when I sign in**.
- **Layout editor**: a live preview of the panel. Edits are a draft until you press *Apply layout*, which saves the
  layout and restarts a running split.
  - *Presets*: top/bottom, left/right, 2 x 2, top + bottom halved, three rows, three columns.
  - *Equal split*: click a region, pick rows or columns and a count (2 to 8), press *Cut*. Every piece can be cut
    again.
  - *Manual*: drag a split line (it snaps to 8 px; hold Shift for single pixels), or click it and type an exact
    position.
  - *Undo this cut* merges a region back with its siblings. Limits: 8 regions, 320 px minimum per side.

The Windows desktop arrangement mirrors the layout, so the cursor moves between the monitors exactly as they sit on
the panel.

| command | effect |
|---|---|
| `splitdisplay --panel "<name>" ...` | any command: the monitor name prefix to split (default `Sculptor`) |
| `splitdisplay run` | split the panel and keep it split, recovering from GPU resets, hot-plug and display power-off |
| `splitdisplay run --test 30` | split for 30 seconds, then restore |
| `splitdisplay stop` | stop a running instance (it restores the panel) |
| `splitdisplay --panel "<name>" configure` | select the display to split by name |
| `splitdisplay autodetect` | select a display automatically if none is selected |
| `splitdisplay autostart on\|off` | create/enable or disable the logon task |
| `splitdisplay revert` | restore the panel and unplug the virtual monitors |
| `Ctrl+Alt+Shift+F12` | emergency exit while running |
| `sdctl status` | driver, virtual monitor and frame state |
| `sdctl plug` / `sdctl unplug` | plug/unplug the virtual monitors without touching the panel |

Logs are written to `%ProgramData%\SplitDisplay\`.

Settings live in `%ProgramData%\SplitDisplay\config.ini`: `panel_id` (the selected monitor's device path), `panel`
(its name, used as a case-insensitive fallback) and `layout`. With nothing selected, the only taller-than-wide
display (or the only display) is picked automatically. A layout is a cut tree in panel pixels, `L` for one monitor, `R(size:node,...)` for rows and
`C(size:node,...)` for columns. For example, `R(1440:L,1440:C(1280:L,1280:L))` is a full-width top half and a bottom
half split into two. When the panel resolution differs, sizes are applied proportionally.

A display selected by name only is split if exactly one connected monitor matches; it is then remembered by device
path. A foldable panel connected over DP appears as two different monitors and is left alone.

## Safety

Taking over your only display is risky, so there are several layers of protection:

- The compositor plugs the virtual monitors *before* removing the panel, so Windows always has a desktop.
- A separate watchdog process restores the panel if the compositor exits, crashes or stops sending heartbeats for
  6 seconds.
- On start, a panel left off the desktop by an earlier crash or power loss is restored first.
- After 3 failures within 2 minutes, the program gives up and leaves the panel as a single display.

## Limitations and ideas

- About one frame of extra latency. No VRR/G-Sync or HDR yet.
- The login screen before sign-in shows the panel as a single display. The split starts at logon. Running the
  compositor as a service in the console session would fix this.
- Hardware-DRM video may be black on the virtual monitors.
- Changing the layout re-plugs the virtual monitors, so the screen flickers for a second and Windows re-places
  windows.
- GPU cost depends only on the panel's pixel count, not on the number of regions.

## License

MIT, see [LICENSE](LICENSE). Contains code derived from Microsoft's MIT-licensed Windows samples, see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
