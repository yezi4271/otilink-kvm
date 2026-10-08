' OTiLink 角色代理（Windows side）-- launch hidden at logon.
'
' 只起一个进程：otiagent.ps1 -Cable -Clipboard（剪贴板 + 角色协商）。
' 键鼠代理 otiagent2.exe 不再无条件启动：由 otiagent.ps1 按 ROLE(10) 协商结果起停
' （L17：任何时刻只能有一个主控；只有本机才是 master 时才起 otiagent2，否则转零软件接收端）。
' Stop: Task Manager, or delete this file and re-logon.
Set sh = CreateObject("WScript.Shell")
' 日志重定向到 clip.log：不开日志的话，用户报"粘贴没反应"时无从下手。
' 注意本文件必须 CRLF 行尾（LF-only 时 cscript/WScript 会静默失效）。
base = sh.ExpandEnvironmentStrings("%USERPROFILE%") & "\otilink"   ' 不写死用户名（每台机器不同）
sh.Run "cmd /c powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & base & "\otiagent.ps1""" & " -Cable -Clipboard -InjectKeys -Device \\.\H: > C:\Users\Public\clip.log 2> C:\Users\Public\clip.err", 0, False
