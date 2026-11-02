# SplitDisplay changelog

Releases 0.4.1 - 0.5.0 were short-lived fix-up builds and have been withdrawn; their entries are kept below.

## 0.5.1

- Same features as 0.5.0. The updater now ignores the symbols archive when it picks the release package to
  install (it took the first .zip of a release).

## 0.5.0

Lower latency and lower GPU cost in window mode, measured on hardware (100 Hz panel, both split monitors busy,
Desktop Duplication timestamps on the real outputs):

| | 0.4.3 | 0.5.0 |
|---|---|---|
| content latency, median / p90 | 9.6 / 10.2 ms | 0.9 / 1.1 ms |
| cursor latency, median | 8.6 ms | 1.9 ms |
| driver frames while moving the mouse (5 s) | 501 | 15 |

- Presents no longer wait for the panel's vblank (`DXGI_PRESENT_ALLOW_TEARING`). Fast motion can show tear lines;
  *Prevent tearing* in the settings window goes back to synced presents.
- Hardware cursor: the driver takes the cursor over from Windows (`IddCxMonitorSetupHardwareCursor`) and the
  compositor draws it on the panel, so mouse moves no longer cost frames from the split monitors. Inverting
  cursors (text beam, magnifier) are drawn as on a real display.
- With the cursor out of the frames, the driver copies only the dirty rectangles again (safe now: they never
  covered the cursor, which is what broke 0.4.0).
- *Compatibility mode* in the settings window restores the 0.4.3 behaviour (OS-drawn cursor, whole frames,
  synced presents) in case something misbehaves on another machine.
- Tried and dropped after measuring: incremental back-buffer updates with `FLIP_SEQUENTIAL` (added a frame of
  latency) and waiting on the frame latency object (made latency worse).

## 0.4.3

Fixes the mouse cursor leaving trails that never cleared, and heavy stutter, in window mode (0.4.0 - 0.4.2).
- Window mode tracked what each swap-chain back buffer already showed, by
  `IDXGISwapChain3::GetCurrentBackBufferIndex`. In D3D11 that call always returns 0 while the
  buffers keep rotating, so every buffer missed some updates and three different, partly stale
  images alternated on screen. The compositor now composes into one persistent canvas and copies
  it to the back buffer at each present (`FLIP_DISCARD` again).
- The driver copied only the dirty rectangles Windows reported, but those do not include the
  software cursor drawn into the frames: cursor moves were lost and old cursor images stayed.
  Every changed frame is copied whole again, as in 0.3.0. Unchanged regions are still skipped.
- The compositor takes the sequence number of the ring slot it actually read, not the one it
  saw a moment earlier.
- Verified on hardware with Desktop Duplication captures of the panel: no cursor trails, and the
  panel keeps up with the split monitors.

## 0.4.2

Fixes "Start split flashes once and does nothing" in 0.4.0 and 0.4.1.
- The session thread inherited the panel's colour profile through the legacy `Wcs*` calls with a
  GDI device name; mscms resolved that through the print spooler and could block until the
  watchdog gave up after 6 s. Colour profiles now go through the display colour APIs
  (adapter LUID + source id) on a background thread that can never hold up a session.
- The driver deleted monitor objects it had already handed back to IddCx, crashing its host on
  every unplug until Windows disabled the device (Code 43).
- Each new frame ring restarted the frame counter, so "wait for a fresh frame" could time out
  ("virtual monitors did not come up"); the counter is now continuous, and the compositor
  also tolerates drivers that restart it.
- A fresh plug no longer triggers a redundant mode update right after arrival.
- Scheduled task XML now validates (0x80041316), so starting from the settings window works
  without a UAC prompt; installed executables are re-signed when upgrading over a local signature.
- The watchdog writes a hang dump before ending a stuck compositor; release builds ship PDBs in a
  separate symbols archive; install scripts are compiled into the app from `release/` instead of
  a hand-maintained copy; installs log to `install.log`.

## 0.4.1

Installer, in-app updater, signing, and dark mode
- Fixed dark mode in the settings window: static labels, edit boxes, dropdowns (`DarkMode_CFD`),
  links, and the layout preview canvas now use dark backgrounds and appropriate text contrast
  instead of painting white `COLOR_WINDOW` boxes under light text.
- Built-in installer and GitHub Releases updater (`updater.cpp`, `Install now` / `Check update` /
  `Upgrade vX.Y.Z` buttons in the settings window and tray menu, plus `splitdisplay install` and
  `splitdisplay update` CLI commands): automatically checks GitHub Releases on startup and every
  6 hours, downloads new releases via WinHTTP with progress feedback, and upgrades in place without
  leaving the app.
- Standalone single-file graphical installer (`SplitDisplay-Setup-v<ver>-x64.exe`) built alongside
  the portable zip archive.
- Authenticode signing: `install.ps1` now strips Mark-of-the-Web (`Zone.Identifier`) and signs
  `splitdisplay.exe` and `sdctl.exe` alongside `SplitDisplayIdd.dll` and `SplitDisplayIdd.cat`
  with the machine-trusted certificate before deleting its private key, eliminating UAC "Unknown
  publisher" warnings on installed binaries. Added `scripts/sign-binaries.ps1` and CI secret hooks
  (`SD_SIGN_PFX_BASE64`) for pre-signing release packages.
- Unprivileged GUI permissions: granted Interactive Users read-only access to the driver's shared
  memory section, signal access to the compositor stop event, and read/execute access on the
  scheduled task so starting/stopping/inspecting the split from the unprivileged settings window
  works without UAC prompts.

## 0.4.0

Frame path
- Dirty-rectangle pipeline (protocol v5): the driver reads IddCx dirty/move information, skips
  no-update frames entirely, and both copy hops move only changed rects. The compositor tracks
  what each back buffer already shows and skips unchanged regions (window mode now uses
  FLIP_SEQUENTIAL so buffer contents survive presents). The full-screen clear per present is gone.
- Exclusive mode copies frames when they arrive and queues the scanout immediately instead of
  waiting for a vblank first, saving up to a frame of latency. GPU/MMCSS priorities now apply
  to the driver and exclusive threads too, not only window mode.
- Refresh rates are carried in milli-Hz end to end (config key `refresh_mhz`, rational in the
  driver modes): fractional panel rates like 59.94 Hz no longer drift against the display.
- Split monitors offer 3/4 and 1/2 scaled modes; the compositor stretches sub-native or rotated
  frames to their region with a small shader blit.

Behaviour
- Refresh-rate changes switch in place (IddCxMonitorUpdateModes + compositor mode re-apply);
  only monitors whose region size changed are re-plugged. Per-region scaling set in Windows
  Settings is remembered (`scales` in config.ini) instead of being overwritten every session.
- Virtual monitor EDIDs report the region's real physical size on the panel, and the panel's
  ICC profile is inherited by its split monitors.
- The settings window runs unprivileged (composing/reverting/logon task relaunch themselves
  elevated), has a tray icon, dark-mode support, a refresh-rate selector, keyboard control of
  split lines, and event-driven status updates.

Robustness
- The compositor watches the driver's heartbeat (a dead UMDF host froze the last frame forever),
  and keys shared handles on a driver epoch so a restarted driver cannot be mistaken for the
  old one. Exclusive mode no longer releases frame reservations while its copies are running.
- Driver: duplicate device nodes can no longer unmap the shared state, failed init no longer
  leaks the one-owner claim, monitor objects are destroyed on departure (leaked before), the
  plug config is seqlock-guarded, and ring slot selection validates shared state.
- Layout engine: distributing a cut now honours every child's minimum size (an outer drag could
  previously squeeze an inner region below 320 px). Found by the new randomized layout tests.
- config.ini / runtime.ini are replaced atomically; logs rotate at 5 MB; crashes write a
  minidump next to the logs; fast user switching no longer starts a competing compositor.

Project
- CI (build + tests), CTest wiring, ring-protocol unit tests, warnings-as-errors and /guard:cf
  + CET hardening flags. The version lives in CMakeLists.txt only (manifest and VERSIONINFO are
  generated from it); the app has an icon. Installer scripts parse pnputil's XML output (its
  text output is localized), verify the one-time signing key is really deleted, and the WDK
  download is hash-pinned.

## 0.3.0

- See the git history.
