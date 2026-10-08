@echo off
rem otilink.cmd —— 对拷线 Windows 侧「免安装绿色包」双击入口
rem
rem 用法（在本目录）：
rem   otilink.cmd                 交互问一次角色，然后启动
rem   otilink.cmd master --edge right
rem   otilink.cmd slave           键鼠在对端：只起剪贴板代理（键鼠零软件）
rem   otilink.cmd --scan          只探测线缆盘符
rem   otilink.cmd --stop          停掉本包拉起的代理
rem
rem 说明：不写注册表、不加开机自启、不需要管理员；包里已带编好的 otiagent2.exe。
setlocal
chcp 65001 >nul
cd /d "%~dp0"

if /i "%~1"=="" goto :ask
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0otilink-win.ps1" %*
goto :end

:ask
echo.
echo 键盘鼠标插在哪一侧？
echo   1^) 本机（这台 Windows 控制对端）  =^> master 主控端
echo   2^) 对端（对端控制这台 Windows）   =^> slave 被驱动侧
set /p _role=请选择 [1/2]（直接回车 = 1）:
if "%_role%"=="2" goto :slave
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0otilink-win.ps1" -Role master -Edge right
goto :end

:slave
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0otilink-win.ps1" -Role slave

:end
echo.
pause
