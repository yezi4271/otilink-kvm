Set sh = CreateObject("WScript.Shell")
sh.ShellExecute "powershell.exe", "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File C:\Users\Public\vendor-off.ps1", "", "runas", 0
