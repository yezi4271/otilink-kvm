' restart-km.vbs -- 拉起 Windows 侧**键鼠代理**（otiagent2.exe --edge right），隐藏窗口。
'
' 为什么单独用 VBS 而不是 PowerShell 的 Start-Process：
'   从 WSL 里跑 powershell -Command "...Start-Process..." 时，新进程会继承调用者的
'   stdout/stderr 句柄，WSL 的 interop 要等这些句柄全部关闭才返回 —— 表现是脚本卡死到超时。
'   WScript.Shell.Run 启动的进程与调用者完全脱离（开机自启那份一直很稳也是这个原因）。
'
' 用法（WSL 侧）：cscript.exe //B //NoLogo %USERPROFILE%\otilink\restart-km.vbs
' 注意：**杀旧进程不在这里做**（PowerShell 侧按命令行匹配更可靠，见 deploy.sh）。
'       `.vbs` 必须 CRLF 行尾，LF-only 时 WScript 会静默不执行。
Set sh = CreateObject("WScript.Shell")
' 日志重定向到 km.log：不开日志的话，"鼠标回不来"这类问题无从下手
' （它平时不写日志，状态只在内存里）。
sh.Run "cmd /c C:\Users\Public\otiagent2.exe --edge right > C:\Users\Public\km.log 2>&1", 0, False
