' clip-watchdog.vbs -- 隐藏窗口拉起 Windows 侧**剪贴板代理看门狗**（clip-watchdog.ps1）。
'
' 为什么用 VBS：与 restart-clip.vbs / otilink-agent.vbs 同样的理由 —— WScript.Shell.Run 起的进程
' 与调用者完全脱离（不会继承 WSL 侧句柄），从启动文件夹运行时也不闪黑窗。
' 本文件必须 CRLF 行尾（LF-only 时 WScript 静默不执行）。
'
' 停止：任务管理器结束命令行含 clip-watchdog.ps1 的 powershell.exe；
'       或删除启动文件夹的 otilink-watchdog.vbs 后注销。
Set sh = CreateObject("WScript.Shell")
base = sh.ExpandEnvironmentStrings("%USERPROFILE%") & "\otilink"   ' 不写死用户名（每台机器不同）
sh.Run "cmd /c powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & base & "\clip-watchdog.ps1"" >> C:\Users\Public\watchdog.err 2>&1", 0, False
