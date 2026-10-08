' restart-clip.vbs -- 拉起 Windows 侧**剪贴板代理**（otiagent.ps1 -Cable -Clipboard），隐藏窗口，日志写 clip.log。
'
' 为什么单独用 VBS 而不是 PowerShell 的 Start-Process：
'   从 WSL 里跑 `powershell -Command "...Start-Process..."` 时，新起的代理会继承调用者的
'   stdout/stderr 句柄，WSL 的 interop 要等这些句柄全部关闭才返回 —— 表现是 deploy.sh
'   卡死到超时（实测踩过）。WScript.Shell.Run 启动的进程与调用者完全脱离，
'   这正是开机自启那份 VBS 一直很稳的原因。
'
' 用法（WSL 侧）：cscript.exe //B //NoLogo %USERPROFILE%\otilink\restart-clip.vbs
' 注意：**杀旧进程不在这里做**（PowerShell 侧按命令行匹配更可靠，见 deploy.sh）。
Set sh = CreateObject("WScript.Shell")
base = sh.ExpandEnvironmentStrings("%USERPROFILE%") & "\otilink"   ' 不写死用户名（每台机器不同）
sh.Run "cmd /c powershell.exe -NoProfile -ExecutionPolicy Bypass -File """ & base & "\otiagent.ps1""" & " -Cable -Clipboard -InjectKeys -Device \\.\H: > C:\Users\Public\clip.log 2> C:\Users\Public\clip.err", 0, False
