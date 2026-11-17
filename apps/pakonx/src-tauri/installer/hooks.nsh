; PakonX installer hooks (tauri.conf.json bundle.windows.nsis.installerHooks).
; The installer runs per machine, elevated, so the hooks may bind the driver.

!macro NSIS_HOOK_POSTINSTALL
  DetailPrint "Installing the scanner driver (WinUSB)..."
  nsExec::ExecToLog 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\driver\install-driver.ps1" -Dir "$INSTDIR\driver"'
  Pop $0
  StrCmp $0 "0" pakonx_driver_ok
    MessageBox MB_OK|MB_ICONEXCLAMATION "The scanner driver could not be installed (code $0).$\r$\nDetails: $INSTDIR\driver\install.log" /SD IDOK
  pakonx_driver_ok:
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  DetailPrint "Removing the scanner driver..."
  nsExec::ExecToLog 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\driver\uninstall-driver.ps1"'
  Pop $0
!macroend
