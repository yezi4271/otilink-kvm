OTiLink Windows 安装包（Win10 / Win11）
========================================

一、线缆需要驱动吗？
  不需要。0ea0:2213 在 Windows 上枚举为「USB 大容量存储 + HID 键鼠」，
  Win10/11 自带 usbstor / hidusb 驱动。厂商光盘里的 SKLoader / MacKMLink
  不是驱动，是会和我们抢同一条链路、把控制权锁死的厂商软件——不要安装它。
  如果设备管理器里能看到「Android+Mac USB Device」和「Virtual Link USB Device」，
  就说明驱动已经就绪。

二、这个包装什么
  1) OTiLink Windows 侧 agent（剪贴板 + 角色协商 + 键鼠代理 otiagent2.exe），
     登录自启、带看门狗；agent 默认 -NoTray（不显示托盘图标）。
  2) 静默线缆（quiet-cable）：
     - 关 AutoPlay / autorun（插线不再弹“选择要执行的操作”）；
     - 在“此电脑”里隐藏 MacKMLink（D:）与 VirtualLink（H:）——只隐藏，
       盘符还在，agent 的 \.H: 不受影响；
     - 隐藏“安全删除硬件并弹出媒体”托盘图标；
     - 清掉厂商 MacKMLink 的托盘注册。
  3) 关停厂商 MacKMLink / LEWD / LinkEngKM / SKLoader 并禁用其自启（如果装过）。

三、安装步骤
  1. 先把线缆插到目标电脑（这样 quiet-cable 才能按当前盘符隐藏）。
  2. 把整个文件夹拷到目标电脑的本地磁盘（不要只拷 install.cmd，也不要放在
     网络共享/压缩包里直接运行）。
  3. 双击 install.cmd（普通用户即可，不要“以管理员身份运行”）。
  4. 中途会弹一次 UAC：复制 otiagent2.exe 到 C:\Users\Public、写静默策略，
     点“是”。
  5. 完成后看日志：
       C:\Users\Public\otilink-install.log   （安装）
       C:\Users\Public\clip.log              （agent）
     打开“此电脑”应看不到 MacKMLink / VirtualLink；隐藏图标区也不再有
     “安全删除硬件”和“Mac KM Link”。

四、只想要“插线安静”，不要 agent
  运行：install.cmd quiet
  （只写 AutoPlay / 盘符隐藏 / 托盘设置，不装 agent、不加自启。）

五、卸载
  1) 先恢复盘符/托盘/自动播放（必须在删目录之前，脚本在安装目录里）：
       powershell -NoProfile -ExecutionPolicy Bypass -File %USERPROFILE%\otilink\quiet-cable.ps1 -Undo
     （会弹一次 UAC）
  2) 删自启、停 agent、删安装目录：
       powershell -NoProfile -ExecutionPolicy Bypass -File %USERPROFILE%\otilink\uninstall-windows.ps1
  3) 删除 C:\Users\Public\otiagent2.exe。

六、注意
  * 安装前插线缆才能按当前盘符隐藏；没插也能装，插上后再运行一次
    %USERPROFILE%\otilink\quiet-cable.ps1 即可。
  * 火绒 / Defender 可能拦截“改注册表 Policies / 杀厂商进程”的脚本：
    把本文件夹加入信任区；被隔离后从隔离区恢复并重新加白。
  * agent 需要能打开 \\.\H:（SPTI）。普通用户即可；若打不开，用管理员运行一次
    otiagent.ps1 -Scan 诊断。
  * 本包只装 Windows 侧；Linux/麒麟侧仍然用 re/otilink 的 otikm（不在本包内）。

七、目标机认不出线缆？
  * 设备管理器里应有 “Android+Mac USB Device”（CD-ROM）和 “Virtual Link USB Device”
    （磁盘），以及 HID 键盘/鼠标；没有黄色感叹号 = 系统驱动已就绪，不需要装驱动。
  * 如果“此电脑”只有一个 1MB 的 “Transfer line” 卷，说明线缆在 2208 人格
    （只传文件、没有 HID）。需切回 2213 人格后再拔插；切换方法见 re/RUNBOOK.md /
    re/PROTOCOL.md（厂商工具或 otiagent.ps1 的模式命令）。
  * 仍认不出：换一个 USB 2.0 口（不要经 hub），确认两端插紧；线缆是 USB2.0 设备，
    某些 USB3 口/扩展坞兼容性差。
