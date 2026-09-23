SplitDisplay
============

Splits physical displays into real Windows monitors: two halves by default, or any
layout of up to 8 monitors per display. Several displays can be split at once, each
with its own layout. Built for foldable dual-panel monitors that show up as a single
2560x2880 display over HDMI.

Install
  Double-click Install.cmd and accept the administrator prompt. The display to
  split is detected automatically; with several displays, click the one to split
  in the settings window that opens. SplitDisplay starts right away, and again
  every time you sign in.

  The installer signs the bundled driver with a certificate created on your PC,
  then deletes that certificate's private key. Test-signing mode is not used.

Use
  Settings, start/stop and the layout editor:
               C:\Program Files\SplitDisplay\splitdisplay.exe  (opens after install)
  Layout editor: pick a preset, or click a region and cut it into equal rows or
               columns; drag a split line or type its exact position. Nothing
               changes until you press "Apply layout".
  Emergency exit (restores the single display):  Ctrl+Alt+Shift+F12
  Logs:        C:\ProgramData\SplitDisplay\

Uninstall
  Run "C:\Program Files\SplitDisplay\Uninstall.cmd". It removes the program,
  the driver, the certificate and the logon task, and restores the display.

Requirements
  Windows 11, x64. The GPU driver must support "Remove display from desktop"
  (Settings > System > Display > Advanced display). Developed and tested on an
  NVIDIA RTX 4060. AMD and Intel should work but are untested.

Source, issues and details: https://github.com/brkDOTstl/SplitDisplay
