# SplitDisplay changelog

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
