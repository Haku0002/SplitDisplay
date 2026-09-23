SplitDisplay
============

Splits one physical display into two real Windows monitors, for example a foldable
dual-panel monitor that shows up as a single 2560x2880 display over HDMI.

Install
  Double-click Install.cmd and accept the administrator prompt. Pick the display
  to split when asked. SplitDisplay then starts right away, and again every time
  you sign in.

  The installer signs the bundled driver with a certificate created on your PC,
  then deletes that certificate's private key. Test-signing mode is not used.

Use
  Emergency exit (restores the single display):  Ctrl+Alt+Shift+F12
  Stop:        "C:\Program Files\SplitDisplay\splitdisplay.exe" stop
  Split again: "C:\Program Files\SplitDisplay\splitdisplay.exe" --panel "<name>" run
  Logs:        C:\ProgramData\SplitDisplay\

Uninstall
  Run "C:\Program Files\SplitDisplay\Uninstall.cmd". It removes the program,
  the driver, the certificate and the logon task, and restores the display.

Requirements
  Windows 11, x64. The GPU driver must support "Remove display from desktop"
  (Settings > System > Display > Advanced display). Developed and tested on an
  NVIDIA RTX 4060. AMD and Intel should work but are untested.

Source, issues and details: https://github.com/brkDOTstl/SplitDisplay
