# RUNBOOK —— Linux（主控端）↔ Windows（被控端）键鼠 + 剪贴板

面向你当前的实际拓扑：**对拷线主控端插在 Linux，被控端插在这台 Windows**。
本文只写"现在能跑什么、下一步做什么"，每条都标注**已验证 / 待验证**。

---

## 0.0 环境准备（仓库里不含凭据与内网地址）

所有脚本都从环境变量取「哪台麒麟、什么密码」，**仓库不内置**：

```bash
export KY=kylin@<麒麟IP>          # 或 tailscale 名，如 kylin-pc
export KY_PASS='<麒麟登录密码>'    # 建议写进 ~/.otilink_env（chmod 600）后 source
```

缺了会当场报错（`KY … 请先 export KY=`），不会静默连到别的机器。

---

## 0. 已验证的事实（真机实测，别重复怀疑）

| 事实 | 证据 |
|---|---|
| 设备有两种 USB 人格：`0ea0:2213`（"Virtual Link"：CD + 1MB 卷 + **HID 键鼠**）与 `0ea0:2208`（"Transfer line"：仅 1MB 卷，无 HID） | §26 |
| 切换由厂商 `USBRestart`（`0xF0/0x05/0x02`）触发；**2208 → 2213 需物理重插** | §26 |
| **HID 包通道可用且无固件节流**（`0xD9/0x33` 键盘 / `0xD9/0x34` 鼠标，数据阶段为空） | §25 |
| 数据管道**写**（`0xD9/0x2A`）在**被控端（Windows）被拒**：`key=0x9 ABORTED / ASC=0x81` | §25 |
| Windows 端**读**数据管道正常（无管理员即可 SPTI） | §24 |
| 协议与 Linux 守护进程**双向**已被独立实现（Python）验证；剪贴板跨实现**双向 + 140KB 分块**全部通过 | §29/§30 |

**推论**：
- **键鼠 Linux→Windows**：走 HID 包，Windows **零软件**（推荐）。
- **剪贴板 Linux→Windows**：走协议管道（Linux 写 → Windows agent 读并 `Set-Clipboard`）。
- **剪贴板 Windows→Linux**：协议管道写在被控端被拒 → 需走**共享卷**（待验证）或暂不支持。

---

## 1. 前置：让设备处于 2213 人格

```powershell
# Windows 侧看一眼（0 表示设备不在）
(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match '0EA0' }).InstanceId
```
- 若显示 `PID_2208` 或为空：**把线的 Windows 这端拔下再插上**（2208 无法用命令切回 2213）。
- 期望看到 `PID_2213` 且带 `MI_01`(鼠标) / `MI_02`(键盘) 两个 HID 接口。

---

## 2. 一键 bring-up（推荐）

把 `otiprobe`（静态二进制）和 `otikm` 放到 Linux 同一目录，然后：

```bash
sudo sh bringup-linux.sh --calibrate     # ① 标定 HID 布局（在 Windows 侧看光标/字母）
sudo sh bringup-linux.sh --mouse-layout N --kbd-layout M   # ② 用标定结果正式跑
```

脚本会自动：找线缆（`otiprobe infoall`）→ 用 `/dev/input/by-id/*-event-kbd|mouse` 选输入设备
→ 以 `--transport cable --peer hid --cable-init --clipboard --grab` 启动。
（`--proto` 可切换成"两端都跑我们实现"的协议模式。）

## 3. Linux 主控端：键鼠共享（被控端零软件）

工具：`otiprobe`（单文件静态二进制，已放在 `C:\Users\Public\otilink\otiprobe`，
拷到 Linux 后 `chmod +x` 即可；或 `gcc -O2 -o otiprobe otiprobe.c`）。

```bash
# 2.1 找设备（应看到一个或多个 /dev/sgN）
sudo ./otiprobe infoall

# 2.2 ★ 标定 HID 12 字节布局（只需一次）
sudo ./otiprobe hidseq
```
`hidseq` 会依次发 9 个候选包（间隔 2 秒，每个印编号）：
- **鼠标候选**：Windows 侧光标会跳动 → 用 `GetCursorPos`/肉眼看**光标跳到第几号**；
- **键盘候选**：在 Windows 记事本里会出现字母 → 看是**第几号候选出现的字母**。

把"第 N 号"告诉我，我把 `otihid.c` 的默认布局定版。

```bash
# 2.3 布局定版后（或先用默认 layout 试），跑键鼠共享：
sudo ./otikm --transport cable --peer hid --hid-probe \
     --capture /dev/input/event3 --capture /dev/input/event4 --grab
```
- `--peer hid`：键鼠直接发成 HID 包（对端无需软件）；
- 指针**撞到屏幕边缘**开始控制 Windows，按热键（默认 `KEY_LEFTCTRL`）拉回本机；
- `--hid-kbd-layout N` / `--hid-mouse-layout N`：切换候选布局；`--hid-probe` 启动时发一组探针。

---

## 4. 剪贴板

### 4.1 方案 A（推荐先试）：协议管道 + Windows agent

```bash
# Linux：与键鼠同一个进程即可（--peer hid 时剪贴板仍走协议管道）
sudo ./otikm --transport cable --peer hid --clipboard --capture ... --grab
```
```powershell
# Windows：被控端 agent（读数据管道 -> Set-Clipboard / SendInput）
powershell -ExecutionPolicy Bypass -File otiagent.ps1 -Device '\\.\H:' -Clipboard -Inject
```
- **已证明**：Windows 侧**读**数据管道无需管理员；`Set-Clipboard` 正常。
- **待验证**：Linux（主控端）**写**数据管道是否 rc=0（被控端被拒是已知的；主控端预期可以）。
  验证命令：在 Linux 上 `sudo ./otiprobe write`，若 `rc=0` → 主控端可写 ✓。

### 4.2 方案 B（备用）：1MB 共享卷

```powershell
# Windows（不需要管理员）
powershell -ExecutionPolicy Bypass -File volumeclip.ps1 -Volume H:\
```
```bash
# Linux
sudo mount -t vfat /dev/sdX1 /mnt/otilink     # lsblk 找 label 为 VirtualLink 的 1MB 分区
sh volumeclip-linux.sh /mnt/otilink
```
**待验证**：该卷是否真的**两端共享**（Windows 写的文件 Linux 能否读到）。这是方案 B 的前提。

---

## 5. 自检与回归（不需要设备）

```bash
make test        # Linux 侧 116 项断言（协议/状态机/剪贴板/HID 报告/仿真设备…）
make interop     # 跨实现运行时互通（剪贴板双向 + 140KB 分块），6 项
make sgtest      # 需要 root：用 scsi_debug 验证 SG_IO 层
```
```powershell
powershell -ExecutionPolicy Bypass -File otiagent.ps1 -Selftest   # 12 项
```

---

## 6. 待办清单（按优先级）

1. **重插线**恢复 2213（当前设备被我上轮的模式扫描留在拔出/异常态）；
2. `otiprobe hidseq` 标定 HID 布局 → 键鼠共享收口；
3. `otiprobe write` 验证主控端数据管道**可写** → 剪贴板方案 A 收口；
4. `mkfifo`/文件方式验证共享卷是否两端共享 → 方案 B 是否可用；
5. `-ModeCmd`（2208 下试 `0xD9/0x60`）→ 看能否免物理重插切回 2213。

若给我 **Linux 的 SSH**，以上 2–5 我可以一次做完。

---

## 10. 真机协议已打通（第 26 轮）—— 代码现状与剩余工程

**已验证可用（真机实测，NOTES §34）**：Linux 主控端 → 对拷线 → Windows 被控端的 64KB 帧通路。
关键点只有三条：

1. **消息管道**：`0xD8/0x00/0x03` + **IN 16 字节**。CDB[3..4]（16 位大端）= **本机待发送帧数**。
2. **帧管道**：读 `0xD9/0x28/0x64` + IN 65536；写 `0xD9/0x2A/0xFF` + OUT 65536。
3. **发送时序**：读消息 → 若回 `0x06/0x07`（授权）→ **立刻**写帧；中间别插任何其他操作。
   设备回 `0x05` 表示"对端有数据"（`be16(m[1..2])` = 帧数）→ 用帧管道读。
   **两端必须同时在跑循环**，否则设备不发授权（主控端只会读到 `0x01` 复位）。

### 10.1 本轮已落地的代码修正

| 文件 | 修正 |
|---|---|
| `otilink.c/h` | 新增 `otilink_msg_read()`；`otilink_recv_frame()` 改用 `0xD9/0x28/0x64`（原来错用 `0xD8/0x00/0x03`）；常量 `OTI_SUB_FRAME_RD` |
| `otitrans.c` | 线缆接收路径改为"先读消息 → 0x05 才读帧" |
| `otimock.c` | 仿真设备迁移到两条管道（16B 消息 / 64KB 帧） |
| `mocktest.c` | 旧 booking 断言（已被真机否证）标注废弃 |
| `probe.c` | 调用点适配 |

`make test` 三次连跑 **全部通过**（剪贴板"回显抑制"那条有已知偶发，偶现 1 项失败）。

### 10.2 剩余工程（按依赖排序）

1. **发送侧要做"授权门控 + 待发队列"**：`otilink_send_frame()` 目前是裸写，
   真机上必须在收到 `0x06/0x07` 后立刻调用才会成功（否则 `key=0x9 ASC=0x81/0x85/0x86`）。
   与厂商一致的做法：`SendData` 入队 → 循环读消息时上报队列长度 → 收到 `0x07` 后
   `PutData` 从队列取帧写出。
2. **发送途中收到的帧不能丢**：读消息时若回 `0x05`，必须把帧读进**内部 RX 队列**，
   否则发送与接收互相饿死（厂商就是这么做的：`CFArray` 收发两个队列）。
3. 上面两条做完，`otikm --transport cable` 就能在真机上跑键鼠共享 + 剪贴板；
   协议层（`otiproto`/`otikm_core`/`oticlip`）无需改动，它们已经在 TCP 路径上验证过。
4. 未解/未测：`F0/31` 独占锁（本代固件 `ASC=0x20` 不支持）、LUN0（`/dev/sg2` 需 sudo，从未测过）。

### 10.3 现场注意事项

- **不要盲扫 `0xD8/0x01`**：那是 `SetRemoteHostStatus`，其 `CDB[2]` 是投递给对端的**消息类型**，
  其中 **类型 1 = 复位**，会把链路打进复位态（我踩过，靠重插/厂商引擎重启才恢复）。
- **厂商软件会抢占并干扰**：`MacKMLink.exe` / `LEWD.exe` / `LinkEngKM.exe`
  （都在 `C:\Users\<你的用户名>\AppData\Roaming\OTi\MacKMLink1325\FunctModules\{...}\`）。
  它们有看门狗（杀掉会重启），**把 .exe 改名**即可（可逆）。
  实测停掉后链路仍然可用（§34.4），只是不再有厂商流量干扰。
- 麒麟侧 `/dev/sg3`（LUN1/CD）有 `user:kylin:rw-` ACL，**免密可读写**；
  `/dev/sg2` 与 `/dev/sdb`（LUN0/FAT）需要 sudo。`udisksctl unmount` 免密可用，
  但 **unmount 后 ACL 消失且无法免密重新挂载**（polkit 需 TTY）——别随手卸载。

### 10.4 “假光盘”（厂商 CD LUN）怎么处理：只隐藏枚举/挂载，**别解绑 usb-storage**

对拷线的 MSC 接口有两个 LUN：**LUN0 = 1MB 共享卷（Windows 的 `H:`）**、
**LUN1 = 厂商虚拟光盘 `MacKMLink`（3.84MB CDFS，麒麟上读不出来）**。
我们的传输（键鼠 / 剪贴板 / 回程令牌 / 大文件）走的是**同一个接口**的裸 SCSI 通用设备
`/dev/sgN`（HID 接口是 Output=0 的纯输入，只能设备→本机）。所以：

* ✅ **只抑制挂载/桌面显示**：`99-otilink.rules` 第 4 条 `UDISKS_IGNORE=1`（`install-kylin.sh` 会装）。
* ✅ **让内核不枚举那张光盘**：`usb-storage.quirks=0ea0:2213:s`（`US_FL_SINGLE_LUN`，只留 LUN0）。
  `install-kylin.sh` 会装成 `/etc/modprobe.d/otilink-quirks.conf`；改完**重插线缆或重启**生效。
  免重启（真机 2026-09-22 验证过）：
  ```sh
  echo 0ea0:2213:s | sudo tee /sys/module/usb_storage/parameters/quirks
  # USB 路径现查（不要写死 1-4.2）：
  n=$(for d in /sys/bus/usb/devices/*/; do [ "$(cat $d/idVendor 2>/dev/null)" = 0ea0 ] && basename $d; done | head -1)
  sudo sh -c "printf '$n:1.0' > /sys/bus/usb/drivers/usb-storage/bind"
  dmesg | tail          # 期望：Quirks match for vid 0ea0 pid 2213: 1
  ```
* ❌ **绝对不要解绑 usb-storage**（`echo <iface> > /sys/bus/usb/drivers/usb-storage/unbind`，
  或 udev `RUN+=` 脚本）：`/dev/sgN` 会一起消失，otikm 只会刷
  `ERR 重开传输失败（设备插好了吗？）` + `recv rc=-19`（2026-09-22 真机踩过，见 NOTES §62）。
* 回滚：删 `/etc/modprobe.d/otilink-quirks.conf` → 重插/重启后恢复 LUN0+LUN1。

---

## 11. 现在怎么用（第 27 轮：真机已跑通）

### 11.1 拓扑

```
麒麟（主控端）                          这台 Windows（被控端）
/dev/sg3  ── 对拷线 ──  \\.\H:
本机键鼠（evdev 抓取）                     otiagent.ps1 -Cable 收帧
剪贴板                                    SendInput 注入 + 剪贴板
```

### 11.2 一次性安装（麒麟上，需要 sudo 密码）

```sh
cd ~/otilink
sudo sh install-kylin.sh          # 装 udev 规则 + 加入 input,disk 组 + 加载 uinput
# 然后注销重新登录（组变更生效）
./otikm --doctor                  # 应显示输入设备可读、uinput 可创建
```

### 11.3 启动

麒麟（主控端）：

```sh
cd ~/otilink
./run-kylin.sh                    # 独占本机键鼠 + 注入 + 剪贴板
./run-kylin.sh --no-grab          # 先试跑：本机键鼠照常用，只是镜像过去
```

Windows（被控端，普通权限即可）：

```powershell
cd <本仓库>\re\windows
powershell -ExecutionPolicy Bypass -File .\otiagent.ps1 -Cable -Inject -Clipboard -Device '\\.\H:'
```

**两端必须同时运行**：设备只在"对端在消费"时才发放发送授权（0x06/0x07）。

### 11.4 已验证的行为

| 项 | 结果 |
|---|---|
| 鼠标共享 | 光标跟随（有系统指针加速，见 §11.6） |
| 键盘共享 | CapsLock 按键翻转可被 `[Console]::CapsLock` 读到 |
| 剪贴板 L→W | Windows `Get-Clipboard` 读到 Linux 发出的文本 |
| 剪贴板 W→L | Linux 侧收到 Windows 剪贴板内容 |

回归测试：`make cabletest && ./cabletest /dev/sg3 12`（不需要 evdev/uinput 权限）。

### 11.5 现场注意

- 两端跑之前先确认 **厂商软件已停用**（`MacKMLink/LEWD/LinkEngKM`；见 §10.3）。
- **别盲扫 `0xD8/0x01`**（`CDB[2]` 是投递给对端的消息类型，**1 = 复位**，会把链路打进复位态）。
- 麒麟上 `udisksctl unmount` 免密可用，但卸载后 **无法免密重新挂载**（polkit 要 TTY）——别随手卸。
  传输期间两个 LUN 应当保持未挂载（`--doctor` 会检查）。

### 11.6 已解决（第 28 轮）

1. ~~鼠标指针加速~~ → **已修，且手感可切换**（`-MouseMode`，默认 `native`）：
   - `native`（默认）：发相对位移，走 **Windows 自己的指针速度/加速**；
   - `absolute`：发绝对坐标（`MOUSEEVENTF_ABSOLUTE` + `OTI_MSG_HELLO` 几何握手），
     实测 1:1 —— 目标 (600,300)/(1500,900)/(50,40) → 光标**分毫不差**。
   模式由被控端决定（加速曲线在它系统里），主控端日志会显示
   `RECV MOUSE_MODE → 相对位移（走对端系统指针速度）`。
   **觉得快慢不对就先调 Windows 的指针速度设置**（本模式完全遵循它）。
2. ~~扩展键~~ → **已修**：被控端新增 evdev→PS/2 set-1 扫描码表 + `KEYEVENTF_EXTENDEDKEY`
   （方向键/Home/End/右Ctrl/小键盘回车/Win 键等 18 个），未映射的键跳过。
   实测：注入 evdev 125(LeftMeta) 后前台窗口类由 `Shell_TrayWnd` 变为
   `Windows.UI.Core.CoreWindow`（开始菜单打开）。

> **被控端 `MouseAbs` 的坑（务必保留这个写法）**：MSDN 说 `MOUSEEVENTF_ABSOLUTE` 的
> 0..65535 映射到**虚拟桌面**，实测是**主显示器**。本机是 2048x1152 主屏 + 左侧副屏
> （虚拟桌面 3584 宽），用 `x*65535/(SM_CXSCREEN-1)` 才准确。


---

## 12. 健壮性保障（第 29 轮）

### 12.1 永远不会被卡住

| 机制 | 说明 |
|---|---|
| 输入路径非阻塞 | 发送只入队，绝不等待；对端不消费也不会卡住热键 |
| **看门狗** | 驱动侧且待发数据 3 秒送不出去 → **自动交还本机**（`--idle-release` 可调，0=关） |
| 信号处理 | Ctrl+C / kill / 关终端 → 立刻 `EVIOCGRAB` 释放，键鼠回到本机桌面 |
| 崩溃兜底 | 进程死亡时 fd 关闭，evdev 独占自动解除 |

**万一还是被卡住**：按一下**左 Ctrl**（默认热键）；或直接 `pkill -x otikm`。

### 12.2 两端都开机自启，不用手动管

- 麒麟：`~/.config/autostart/otilink-kvm.desktop` → 登录跑 `~/otilink/run-kylin.sh`
- Windows：`启动` 文件夹里的 `otilink-agent.vbs` → 隐藏窗口跑
  `C:\Users\<你的用户名>\otilink\otiagent.ps1 -Cable -Inject -Clipboard -MouseMode native`
  （agent 副本在 Windows 本地，**不依赖 WSL**；拔插线会自动重连）

### 12.3 状态可见性

```sh
tail -f /tmp/otikm.log            # 麒麟侧状态（切换/独占/释放/看门狗/HELLO）
```
Windows 侧日志：agent 是隐藏窗口，若要排查可手动前台运行一次看输出。


---

## 13. 产品化配置（第 30 轮）

### 13.1 全部策略在配置文件里

`~/.config/otilink/kvm.conf`（模板见 `otilink/kvm.conf`）：

```ini
[general]
edges = left,right           # 撞哪些边交出去：left,right,top,bottom | all | none
switch_modifier =            # 撞边需按住的修饰键（防误触）：空/shift/ctrl/alt/meta
corner = 40                  # 角部死区（防误切）
idle_release = 3000          # 看门狗
[hotkeys]
hotkey_toggle    = ctrl+alt+space
hotkey_to_remote = ctrl+alt+right
hotkey_to_local  = ctrl+alt+left
hotkey_lock      = ctrl+alt+l
```

命令行优先，例如临时只用热键：`./run-kylin.sh` 之外直接
`./otikm --transport cable:/dev/sg3 --edges none --capture ...`

### 13.2 日常操作

| 想做什么 | 怎么做 |
|---|---|
| 切换控制权 | `ctrl+alt+space`，或把指针推到屏幕边缘 |
| 只交给对端 / 只拉回 | `ctrl+alt+right` / `ctrl+alt+left` |
| 临时不被撞边打扰 | `ctrl+alt+l` 锁定（再按解锁） |
| 改热键/边缘/防误触 | 编辑 `~/.config/otilink/kvm.conf` 后重启 `run-kylin.sh` |
| 看状态（Windows） | 托盘图标双击；右键可暂停注入/退出 |
| 看状态（Linux） | `tail -f /tmp/otikm.log` |
| 完全停掉 | Linux `pkill -x otikm`；Windows 托盘退出 |

### 13.3 安装 / 卸载

```sh
sudo sh install-kylin.sh      # Linux：udev + 用户组 + uinput + 默认配置
sudo sh uninstall-kylin.sh    # 卸载（--purge 连配置一起删）
```
```powershell
powershell -ExecutionPolicy Bypass -File install-windows.ps1 -Device '\\.\H:'
powershell -ExecutionPolicy Bypass -File uninstall-windows.ps1
```

---

## 14. 【新】键鼠走厂商 HID 包通道（默认，务必先读这一节）

第 33 轮把厂商的键鼠线格式彻底破解了（全过程见 `NOTES.md` §41）。结论改变了整个产品的
键鼠路径，**这是解决"卡顿 / 惯性 / 不跟手 / 左右键没反应"的根本手段**。

### 14.1 为什么以前的实现会卡

以前键鼠和剪贴板共用 64KB 帧管道：**鼠标动一下就搬 64KB**。厂商不是这么做的 ——
键鼠走的是 **SCSI HID 包通道**，一次事件只有 **16 字节 CDB**，小了 4000 倍。

### 14.2 线格式（真机标定，可直接照抄）

```
CDB(16B) = D9 | 0x33 鼠标 / 0x34 键盘 / 0x36 多媒体 | 12 字节负载 | 'O' | 'T'
无数据阶段
鼠标负载: [按键:bit0 左 bit1 右 bit2 中 bit3 侧 bit4 额外][dx int8][dy int8][wheel int8][4..11]=0
键盘负载: [修饰键][保留][k1..k6 = USB HID usage][8..11]=0
```
修饰键位：bit0 LCtrl bit1 LShift bit2 LAlt bit3 LWin bit4 RCtrl bit5 RShift bit6 RAlt bit7 RWin。

### 14.3 关键性质：接收端不需要任何软件

线缆两端都把自己枚举成**真实的 USB 鼠标 + 键盘**：

```
MI_00 Mass Storage（CD/FAT）
MI_01 HID -> "HID-compliant mouse"     Linux: /dev/input/by-id/usb-_Android+Mac_*-if01-event-mouse
MI_02 HID -> "HID Keyboard Device"     Linux: /dev/input/by-id/usb-_Android+Mac_*-if02-event-kbd
```
所以发出去的 HID 包在对面变成**真正的 USB 输入**，由操作系统原生处理
（指针加速、按键、滚轮、CapsLock 全对），**对面不用装任何东西**。

### 14.4 用法

* Linux 端（本仓库）**默认就走这条路**，无需额外参数。判断依据：`--transport cable:*` 自动启用，
  其它传输（TCP/AF_UNIX）自动退回协议管道。日志里会打印
  `键鼠载体：厂商 HID 包通道（--peer 可强制）`。
  强制用法：`--peer hid` / `--peer proto`，配置文件键 `peer = hid|proto`。
* 若要拿回旧行为用于对比：`--peer proto`。

### 14.5 现场排查工具（都在 `otilink/`）

| 工具 | 用途 |
|---|---|
| `otiprobe cap [秒] [设备]` | 抓线缆上对端发来的每个帧（十六进制），逆向厂商协议用 |
| `otiprobe hidsend <1\|2> <24位hex>` | 直接发一个 HID 包（1=鼠标 2=键盘），标定/验证用 |
| `otiprobe sendxml <file>` | 按厂商格式发 XML 帧（对端须是厂商程序） |
| `hidmon <event设备> [秒]` | 监听**线缆自带**的 HID 键鼠接口，看收到的原始 input_event |

Linux 侧读线缆 HID 需要 input 组权限：
`sg input -c './hidmon /dev/input/by-id/usb-_Android+Mac_*D21-if01-event-mouse 10'`

### 14.6 Windows 侧（`windows/` 目录）

| 文件 | 说明 |
|---|---|
| `otiagent2.cs` | **新的捕获代理**（C#，低级钩子 → 直接发 HID 包）。编译：`csc.exe /target:exe /out:otiagent2.exe otiagent2.cs` |
| `hidpkt.ps1` | 从 Windows 发一个 HID 包（标定/自检用） |
| `otiagent.ps1` | 旧代理（协议管道 + SendInput 注入）。**已被 `otiagent2` 取代**，仅在 `--peer proto` 兼容路径下还需要 |
| `curlog.ps1` / `winmove.ps1` | 光标记录 / 合成鼠标位移（调试用） |

`otiagent2.exe` 用法：

```
otiagent2.exe --edge left [--device \\.\H:] [--hotkey-back 27] [--verbose] [--status]
otiagent2.exe --probe          # 发一组鼠标+键盘包然后退出（不需要钩子，最快的通路自检）
```

设计要点（踩过的坑，改代码前务必读）：

1. **钩子里绝不做 SPTI**：低级钩子有 `LowLevelHooksTimeout`（默认 300ms），超时会被系统
   **静默摘除**（表现为"用一会儿就再也不转发"）。现在钩子只入队（`Enqueue`，微秒级），
   工作线程 `Worker()` 负责真正的 SCSI 发送。
2. **鼠标移动事件不能吞**：吞掉光标就不动了，`pt` 恒定 → 再也算不出位移。
   现在放行移动、用 `ShowCursor(false)` 让它不可见、漂移超过 220px 才回中一次
   （回中后忽略紧随的 2 次事件，否则会与系统取整误差形成 ±1px 乒乓风暴）。
3. **必须 `SetProcessDPIAware()`**：否则 `GetSystemMetrics/SetCursorPos` 用逻辑坐标，
   而钩子报物理坐标，两者混用会产生巨大的假位移（实测出现 ±127 洪流）。
4. **不能把线缆自己的 HID 设备也捕获进来**，否则形成回环。所以发送端与接收端
   必须"同一时刻只有一侧在控制"，这与厂商的切换语义一致。

### 14.7 已验证 / 未完成

**已在真机验证：**
* Linux→Windows：`otiprobe hidsend 1 00140000…`（dx=20）×10 → Windows 光标
  `882→906→939→972→1005→1038`，**每包稳定 +33px**。
* Linux→Windows 全链路：`otikm`（HID 模式）+ `uisim` 注入 → Windows 光标从 -781 平移到
  +2047（**dy 恒为 377，纯水平**），HID 包 28 个；CapsLock 也成功翻转。
* Windows→Linux：`hidpkt.ps1` / `otiagent2 --probe` → Linux 线缆 HID 精确收到
  `REL_X=±40`、`KEY_A`；20 个包连发全部到达（**设备无速率限制、无丢包**）。

**未完成：**
* `otiagent2` 的**边缘交接 / 热键回归**策略还没有在真人操作下调过
  （合成输入在本机测试里不稳定，需要用户配合实测）。
* Windows→Linux 目前需要手动跑 `otiagent2.exe`；尚未做开机自启与托盘 GUI。

---

## 15. 当前实机架构（键鼠在 Windows 端时）与真机自测

### 15.1 分工（**单一转发方**，这是关键）

```
Windows → 麒麟 ：otiagent2.exe 捕获 + 发厂商 HID 包 → 线缆在麒麟侧变成真 USB 鼠标/键盘
麒麟 → Windows ：otikm（被驱动侧）用 X11 真实坐标检测撞边 → 通知对端交还控制权
```

**同一时刻只能有一个转发方。** 踩过的坑：厂商程序与 `otiagent2` 会同时抢着转发
（双份位移 = 用户说的"光标乱飞"），两个厂商实例同时跑也一样。所以：
* Windows 上要么只跑 `otiagent2.exe`，要么只跑厂商程序，**不要都跑**；
* 厂商程序若在跑，最好让它保持"未接管"状态（发给它 `Cmd_Notify_KM_Switch_To_Local`）。

### 15.2 两条链路的关键实现点（改代码前必读）

| 点 | 说明 |
|---|---|
| 撞边判据必须用**真实光标坐标** | 用 X11 `XQueryPointer`（`otix11.c`，`dlopen` 不需要 libx11-dev）。之前用"累加位移"推算，会和真实位置漂移，表现就是"推到边缘了却没反应、回不去" |
| 撞边必须**还在往边缘外推** | 只看位置不行：光标停在边缘会每 1.5s 反复触发（实测刷过 1868 条） |
| 厂商帧必须由 **RX 线程**发 | 捕获线程自己发会跟消息泵抢同一条管道上的授权消息 |
| Windows 钩子**不能吞 WM_MOUSEMOVE** | 吞掉光标就不动，`pt` 恒定 → 再也算不出位移 |
| 钩子里**不做 SPTI** | 有 `LowLevelHooksTimeout`，超时会静默摘除钩子 |
| 必须 `SetProcessDPIAware()` | 否则 `GetCursorPos` 逻辑坐标与钩子物理坐标混用 → ±127 假位移洪流 |
| 卡键自愈 | 键盘修饰键/鼠标键会卡在按下（症状：Windows 点什么都没反应）。裸发 up 清不掉，必须补一次 **down+up 循环**；`otiagent2` 在每次交还控制权时自动做 |

### 15.3 真机自测（回归用，**不需要人动手**）

```bash
cd re/otilink && ./hwtest.sh          # 完整往返，逐项 PASS/FAIL
./hwtest.sh check                      # 只查两侧进程/设备
```

它用**程序化输入 + 真实光标坐标**验四件事：

1. 麒麟 → Windows：发 HID 包，看 Windows 光标是否移动；
2. Windows → 麒麟 交接：推 Windows 光标到左边缘，看麒麟光标是否跟动；
3. 麒麟 → Windows 交还：用 HID 包把麒麟光标推到右边缘，看 otikm 是否撞边、代理是否回 LOCAL；
4. 结果汇总。

当前实机结果：**PASS=8 FAIL=0**。


### 15.5 交接手势失效的三个真坑（都在真机自测里复现过）

用户报"鼠标从 windows 移动不到 linux"，排查出来是**三个叠加的判据 bug**，
而且前两个只有"用程序复现真实手势"才能暴露（我的第一次自测因为把光标推到了左显示器、
那里还有位移空间，所以全都没踩到）：

| # | 坑 | 现象 | 修法 |
|---|---|---|---|
| 1 | 判据用了**主屏**的 0/宽 | 用户有左显示器时 `x <= 0` 表示"光标落在左显示器上"，随手一动就误判 | 改用**虚拟桌面**边界 `SM_XVIRTUALSCREEN/CXVIRTUALSCREEN` |
| 2 | 光标被边缘**夹住**后 `dx==0` | 代码把"没有向外位移"当成"用户没在推"，计时器立刻清零 → 推到边缘一停就永远攒不满 | `dx==0` 视为"仍按住不放"，继续计时；只有明确向内（>`JIT`）才清零 |
| 3 | 边缘有 **±1px 抖动**，且 `outward` 分支每次都**重新赋值**计时器 | 抖动每隔一个事件产生一次 outward → 计时器无限重启（日志里 elapsed 永远 ~93ms） | 加抖动容差 `JIT=3`；开始计时改成"只在未开始时启动" |

**教训**：回归测试必须复现**用户的真实手势**（推到边缘并被夹住），
否则测的是"还有余量的推"，恰好绕开所有边界 bug。`pushleft.ps1` 现在会先走到虚拟桌面最左边界，
再持续外推，专门制造 clamped + 抖动 + outward 交替的事件序列。

### 15.6 代理的编译陷阱

`otiagent2.exe` 正在运行时，`csc /out:` **无法覆盖它，而且失败是静默的**
（看起来像编译成功，其实还在跑旧二进制 —— 我因此白折腾了两轮）。
固定流程：**先 `Stop-Process otiagent2` → 再 csc → 最后启动**。


### 15.7 交接方向必须和**实际物理摆位**一致

用户的实际布局是 **Linux 屏在 Windows 右边**，所以 Windows→麒麟 的交接边缘是**右边缘**：

```
otiagent2.exe --edge right      # Linux 在右边 → 往右推出屏幕即交给 Linux
```

（我一开始默认成 `--edge left`，方向反了 —— 这类"默认值"必须按用户现场确认，不能猜。）
回程相应地是：把**麒麟**光标推到**左**边缘（Windows 在 Linux 左边）。

### 15.8 REMOTE 转发改用**原始输入（Raw Input）**，不再靠光标位置差

这是本轮最重要的一次实现替换。早期做法是"让光标正常移动、取前后位置差、漂移过大就 SetCursorPos 回中"，
但它有三个致命问题：

1. `SetCursorPos` 在**低级钩子内部不生效** —— 回中根本没发生；
2. 光标随即被屏幕边缘**夹住**，位置差恒为 0（只剩 ±1px 抖动），转发出去的就是"乱抖"；
3. 位置差还受 DPI 虚拟化、指针加速影响。

现在改成：**REMOTE 下钩子把鼠标事件全部吞掉（本机光标完全不动），位移与按键一律取自
`WM_INPUT` 的 `RAWMOUSE`** —— 那是**设备级相对位移**，不受边缘夹取、DPI、加速影响，
因此**完全不需要回中**。`lLastX/lLastY` 紧跟在 24 字节 `RAWINPUTHEADER` 之后（x64）。

三个必须同时满足的 Raw Input 前提（缺一个就是"注册成功但一个 WM_INPUT 都收不到"）：

| 前提 | 说明 |
|---|---|
| `RegisterClassW` 必须 **CharSet.Unicode** | 否则和 `CreateWindowExW` 要的 Unicode 窗口类对不上，`CreateWindowExW` 直接失败 |
| 消息循环必须 **`DispatchMessage`** | 低级钩子是系统取消息时回调的，所以只 `GetMessage` 也能跑；但 `WM_INPUT` 是**窗口消息**，不 dispatch 永远到不了窗口过程 |
| 窗口用 `HWND_MESSAGE` + `RIDEV_INPUTSINK` | 消息专用窗口，且非前台也能收到输入 |

### 15.9 回归自测的两个"前置动作"

`hwtest.sh` 里有两处看起来多余、其实必需的准备，删掉就会产生假失败：

* **先把 Windows 光标归位到屏幕中间**（`setcur.ps1`）：它可能停在上一次测试留下的屏幕边缘，
  那样 +100 的包推不动，步骤 1 会假失败；
* **步骤 1 之后 sleep 2 再进步骤 2**：步骤 1 发的 HID 包会让代理看到"线缆鼠标在动"，
  从而抑制交接 1.5s（防对端驱动时被误判），不等它过期步骤 2 也会假失败。


### 15.10 按键/滚轮必须走**低级钩子**，位移才走原始输入

实测：**合成点击不会产生 `RAWMOUSE.usButtonFlags`**（移动会），所以按键不能依赖原始输入；
而低级钩子对按键是 100% 可靠的。最终分工：

| 事件 | 来源 | 原因 |
|---|---|---|
| 位移 | `WM_INPUT` 的 `RAWMOUSE.lLastX/lLastY` | 设备级位移，不受屏幕边缘夹取 / DPI / 加速影响 |
| 按键、滚轮 | `WH_MOUSE_LL` 钩子（`WM_LBUTTONDOWN` 等） | 钩子对按键可靠；原始输入拿不到合成点击的按键位 |
| 是否放行 | REMOTE 下一律吞掉 | 本机光标不动，也就没有"撞边/夹取"问题 |

### 15.11 被驱动侧的交还边必须**朝向控制端**

用户布局是 Linux 在 Windows 右边，所以麒麟的交还边是**左边缘**（朝向 Windows），
用配置键 `return_edge = left`（默认）。

教训：早期四条边都当交还边 → 用户从 Windows 往右移进来后**继续往右推**，
麒麟光标撞到自身右边缘就被判定成"想回去"，**刚过来就被弹回 Windows**
（自测里表现为"交接成功但按键检查失败"，因为控制权已经被还回去了）。

### 15.12 自测脚本里的两个"工具陷阱"

* **`grep` 非 TTY 时会缓冲**：`hidmon` 的输出若经 `| grep -v ...` 再落文件，
  5 秒后文件里还是空的 → 按键检查假失败。监控类输出必须直连 ssh 落盘。
* **测试耗时约 90 秒**，别用 60 秒的超时去跑，会被中途 SIGTERM（看起来像失败，其实是没跑完）。


### 15.13 剪贴板：**两个代理并存**（各占一条通道，互不冲突）

| 进程 | 通道 | 负责 |
|---|---|---|
| `otiagent2.exe --edge right` | 厂商 HID 包通道（16 字节 CDB） | 键鼠（含点击、滚轮） |
| `otiagent.ps1 -Cable -Clipboard` | 协议帧管道（64KB 帧 + 授权时序） | **剪贴板** |

**注意 `-Inject` 不要传**：那是旧代理的键鼠注入路径，会与 HID 通道打架。只留 `-Clipboard` 即可。
两者分别读写设备的不同通道，可以同时跑（实测无冲突）。

故障排查顺序（"复制粘贴不能用"）：
1. `Get-Process otiagent2` → 键鼠代理在不在；
2. `Get-CimInstance Win32_Process | ? { $_.CommandLine -like '*-File*otiagent.ps1*' }` → **剪贴板代理**在不在
   （最常见的原因就是它根本没启动 —— 新键鼠代理不含剪贴板功能）；
3. 麒麟侧 `DISPLAY=:0 xclip -selection clipboard -o` 看有没有内容。

自测已覆盖：`hwtest.sh` 的第 2c 步会双向写读剪贴板（用 xclip + Set/Get-Clipboard）。

#### 15.13.1 剪贴板专项回归 `clipreg.sh`（第 34 轮新增）

```bash
cd re/otilink && ./clipreg.sh          # 约 50 秒，**不动光标**，可反复跑
cd re/otilink && ./clipreg.sh check    # 只查环境
```

覆盖 9 组 17 项：小文本双向、69991 字节（2 块）与 200010 字节（4 块）大文本、图片 240x160 双向、
文件双向（含落地 md5 与 `text/uri-list`），外加三条**日志不变量**：
① 无真正的帧解码失败（厂商 XML 帧应显示为 `[skip]`）；② 图片发送无回环；③ 文件剪贴板读取正常。

依赖麒麟侧 `~/otilink/setclip.sh`（仓库里有，重新装麒麟侧时别漏）。

#### 15.13.2 剪贴板改动后的部署姿势（第 34 轮固化）

```bash
cd re/windows && ./deploy.sh --restart
```

它做四件事：**补 UTF-8 BOM** → 复制 `.ps1/.vbs` 到 `C:\Users\<你的用户名>\otilink\` → **所有 `.ps1` 解析自检**
→ `--restart` 时用 `restart-clip.vbs` 重启剪贴板代理（日志 `C:\Users\Public\clip.log`）。
**不要再手工 cp**（BOM 丢失、`.vbs` 行尾、忘了重启，三个坑它都堵上了）。

麒麟侧改完 `.c`：`make otikm && echo "$KY_PASS" | sudo -S ./label-kysec.sh && pkill -x otikm`，
再用 `sg input -c "DISPLAY=:0 nohup ./run-kylin.sh > /tmp/rk.log 2>&1 &"` 拉起。

#### 15.13.3 日志读法（**先读这一条再看日志**）

* Windows `clip.log`：
  - `CLIP 发送 69991 字节 / 本笔 2 块（累计 4）fid=3 fmt=1 源=文本` —— **本笔**才是这一笔的块数，
    "累计"是整场会话的（历史上只看累计值，把一次单块传输误读成"6 块"）。
  - `[skip] 厂商 XML 帧 668 字节（第 k 条，非本协议，正常忽略）` —— 厂商/otikm 的 XML 通知帧，
    **与剪贴板无关**（撞边交还时出现）。以前它显示为 `[warn] 解包失败（CRC/格式）`，误导过一整轮排查。
  - `[warn] 解包失败（CRC/格式）len=… head=…` —— 这个才是**真**问题，head 前几字节能看出是谁的帧。
  - `[warn] CLIP 乱序/缺口 …` / `[warn] CLIP 前一笔未收完 …` —— 分块重组异常（正常情况下不该出现）。
  - `[warn] 剪贴板有文件却读不出来：[ClipFiles]::LastError=…` —— 文件方向回归的第一现场。
* 麒麟 `/tmp/otikm.log`：`CLIP 发送 …（fid=… fmt=… crc=…）` 与
  `CLIP 应用远端剪贴板 …（fmt=… crc=… write rc=…）` —— **两侧靠 CRC 对齐**：
  如果"应用"和随后的"发送"CRC 相同，就是回环（第 34 轮已把图片回环修掉）。

#### 15.13.4 三个最容易踩的坑（第 34 轮实测）

1. **别按命令行匹配进程后无脑 Stop-Process**：`... CommandLine -like '*-File*otiagent.ps1*'` 会匹配到
   **执行这条查询的 powershell 自己** → 脚本把自己杀掉，表现为"一声不响、代理没起来"。必须加
   `$_.ProcessId -ne $PID`。
2. **从 WSL 启动长驻 Windows 进程别用 `Start-Process`**：新进程继承调用者句柄，WSL interop 等句柄全关
   → 脚本挂到超时。用 `cscript //B restart-clip.vbs`（`WScript.Shell.Run` 完全脱离调用者）。
   并且 **`.vbs` 必须 CRLF**：LF-only 时 cscript 返回 0 却什么都不做。
3. **ssh 的 banner 走 stderr**：把 stderr 并进 stdout 会让捕获多出 `Kylin V10 SP1`（14 字节），
   文本/md5/二进制全错；`clipreg.sh` 的 `kssh` 因此不合并 stderr。
   另外 **麒麟上没有可用 python3**（kysec 拦），二进制解析放 WSL 侧做。

#### 15.13.5 剪贴板容量上限（实测标定，第 34 轮）

| 项 | 上限 | 位置 |
|---|---|---|
| **文件 >8MB** | **走分片流式传输**（第 35 轮新增，见 §15.13.6）——实测到 500MB | `otixfer.c` / `otiagent.ps1` |
| Windows 发**文件包**（≤8MB 的小文件） | 包总长 ≤ **8,000,000** 字节（= 2 + 4 + 名字长 + 8 + 文件内容） | `otiagent.ps1`（`$tot -le 8000000`） |
| Windows 发文本/图片 | ≤ **8,388,608**（8 MiB） | `otiagent.ps1` |
| Windows 单次文件个数 | ≤ **16** | `otiagent.ps1`（`$fs.Count -le 16`） |
| 麒麟发/收文本/图片（非文件） | ≤ **8,388,608**（`clip_max`，可调） | `otikm.c`；`--clip-max N` 或 `kvm.conf` 的 `clip_max = N` |
| 线上单块 | 65,000 字节/块（64KB 帧 − 帧头/消息头） | 两侧一致 |
| 大文件单片 | **64,000 字节/片**（`OTI_XFER_PART_MAX`，两侧必须一致） | `otixfer.h` / `otiagent.ps1` |

超限行为（第 34 轮起）：**打印一条上限告警，然后不发**（按内容去重，不会刷屏）。
以前是静默丢弃 —— 对端永远等不到、日志里一个字都没有，排查时完全看不出"太大"还是"没触发"。

#### 15.13.6 大文件传输（≤500MB，第 35 轮新增）

**行为**：剪贴板里放一个 >8MB 的文件（或合计 >8MB 的多选）→ 自动走**分片流式**：
`64000` 字节一片、收端**边收边落盘**（内存恒定 64KB/侧）、收完读回算整文件 CRC 与发端比对；
缺片按**位图**只补缺的那几片（不是从缺口一路发到结尾）；校验不过最多重传 3 轮。
收端落盘后自动把文件挂到自己的剪贴板（麒麟 `text/uri-list` / Windows `CF_HDROP`），直接粘贴即可。

实测（2026-09-16）：**500MB** W→L 69 秒（7.2 MB/s）、L→W 166 秒（3.0 MB/s），双向 md5 一致；
传输期间键鼠不受影响（HID 包通道独立）。

**回归**：

```bash
cd re/otilink && ./xferlocal.sh 33554432   # 本机（不走线缆、不动光标）：协议+状态机，约 1~3 秒
cd re/otilink && ./xferreg.sh              # 真机 64MB 双向，约 60 秒
cd re/otilink && ./xferreg.sh 524288000    # 真机 500MB 双向，约 4 分钟
DROP_PART=100 ./xferlocal.sh 33554432      # 故意丢第 100 片，验证位图补缺
OTI_XFER_DROP_PART=100 ./otikm ...         # 真机上故意丢片（麒麟侧，需重启 otikm）
```

**排障读法**：`clip.log` / `/tmp/otikm.log` 里带 `[xfer]` 的行就是这条链路 ——
`开始接收`/`片（N%）`/`缺 N 片，回缺片位图`/`按位图补发`/`传送完成`/`接收完成`。
两侧日志现在都带**单调时钟时间戳**（`[秒.毫秒]`），跨线程时序问题靠它对齐。

**两个已修的坑（改这条链路前必读）**：
1. 整文件 CRC **必须在开传前算好**（顺序读一遍）。按片增量累加的话，一次发送失败+重传
   就会把同一片重复计入 → 对端 `校验失败` → 整份重发。
2. 发送失败**必须退避**：不推进游标地立刻重试会把 otikm 打成 **100% CPU** 空转，
   连带把设备读取拖挂（Windows 侧会连续读失败然后重连设备）。现在的策略是
   退避 20ms、连续 20 次告警、连续 200 次放弃。

**已知限制**：
* 双向**同时**传大文件会互相抢设备授权（曾实测拖到 0.1MB/s）→ 已用 **fid 让路**缓解（一边先走）；
  仍然建议**别同时传两个大文件**。
* 大文件走剪贴板语义，粘贴的是接收端临时目录里的副本（`/tmp/otilink-files-<pid>/`、
  `%TEMP%\otilink_files_<fid>/`），不是"原地同步"。
* 一次多选最多 16 个文件；单文件大小只受磁盘空间限制（`u64` 长度 + 按片落盘）。

### 15.13.7 看门狗与拔插自愈（第 36 轮）

**先记住三条"救命"操作**（出问题不用重启机器）：

```bash
# Windows：剪贴板代理 / 键鼠代理（键鼠代理卡住时鼠标会"回不来"，先重启它）
cd re/windows && ./deploy.sh --restart        # 剪贴板代理
cd re/windows && ./deploy.sh --restart-km     # 键鼠代理 otiagent2
# 麒麟：otikm
pkill -x otikm; sg input -c "DISPLAY=:0 nohup ~/otilink/run-kylin.sh > /tmp/rk.log 2>&1 &"
```

**自动看门狗（不需要人管）**：

| 位置 | 判据 | 动作 |
|---|---|---|
| otikm 驱动侧 | HID 包连续 20 次写失败 | 交还本机 + 解除 grab（拔线后你的鼠标立刻能用） |
| otikm 传输层 | 连续 20 次读失败 | 交还本机 → **重新发现并打开**线缆设备（`/dev/sgN` 拔插后会变） |
| otikm 帧通道 | 待发数据长期发不出去（`idle_release`，默认 3000ms） | 交还本机 |
| Windows 剪贴板代理 | 连续 60 次读失败 | 关闭并重开设备；重开失败 → **枚举盘符自动发现**线缆 |
| Windows 键鼠代理 | 发送失败 / 打不开 | 关句柄下次重开；重开失败 → 自动发现 |

**设备路径不要写死**：拔插/重启后 Windows 盘符（`H:`）与 Linux `/dev/sgN` 都可能变。
两边的代理现在都会自动发现（只碰盘符/字符设备，不碰 `\\.\PhysicalDriveN`）。
`./deploy.sh`（不带参数）会把 `restart-km.vbs` / `restart-clip.vbs` 一起部署到位。

**拔插自愈回归（第 44 轮新增）**：`cd re/tools && ./gate.sh --replug`
（麒麟地址不是缺省时：`KY=kylin@<ip> ./gate.sh --replug`）。
它用 USB `unbind/bind` **模拟拔插** N 轮，断言 otikm 不崩 + 传输能自发现重开 —— **不需要真拔线**，
也不用重启机器。修复前（`NOTES §53` 那个 use-after-free）第 2 轮必崩。
另外：`run-kylin.sh` 现在用**裸 `cable` 自发现**，换口/设备号变化都不再需要手工改脚本；
手工拉起 otikm 仍可用：`pkill -x otikm; sg input -c "DISPLAY=:0 nohup ~/otilink/run-kylin.sh > /tmp/rk.log 2>&1 &"`。

**指针"回不来"时**（按顺序试）：
1. **在有键盘的那一侧按急救热键**：Windows 侧 **`Ctrl+Alt+→`**（`--hotkey-back`，VK_RIGHT）、
   麒麟侧 `ctrl+alt+←`（`hotkey_to_local`）。热键由**捕获侧**识别，所以在哪一侧按是确定的。
2. `cd re/windows && ./deploy.sh --restart-km`（键鼠代理重启即回 LOCAL）。
3. 被驱动侧（键鼠在对端）：把指针推到**朝向控制端那条边**并继续推 ~0.3 秒（默认左边缘）。
4. 主控侧（**键鼠插在这一侧**，例如拓扑 B 的麒麟）：在对端屏幕上**继续往外推**（§15.14）。
   要点：推出去多少、就得推回来多少，再多推一点 —— 回程判据累计的是**未缩放的位移**，
   所以小步慢推（每个事件 1~3 个计数）照样会触发；另外落点会同步真实光标（不会停在边上）。
   仍回不来时：**在有键盘的那侧按 Ctrl+Alt+←**（hotkey_to_local，由捕获侧识别，一定有效），
   或看 /tmp/otikm.log 有没有 "HID 模式：指针拉回本机"。
5. 拓扑 B 专项回归（自动，不需人推鼠标）：cd re/tools && KY=kylin ./gate.sh --hw-kmb
   （re/otilink/kmbret.sh：合成手势经真实线缆 → 断言小步回程 + park + by-id 抓取 + Ctrl 组合到 Windows）。
6. **拓扑 B 键盘用不了**：看麒麟 /tmp/otikm.log 的"抓取设备"行 —— 正常应是
   /dev/input/by-id/usb-...-event-kbd（by-id 稳定路径）。如果是 /dev/input/eventN，或设备名是
   "... System Control"/"... Consumer Control"，说明按**旧 eventN** 抓到了错设备
   （2026-09-21 已修：by-id 稳定路径 + 重开校验能力位 + 按类别重新发现）。
   另外"**先插鼠标、再插键盘**"以前不会把键盘加进抓取集合（判据只看"有没有"，现在按个数），
   现在 2 秒内会自动重扫，日志出现：
   `本机键鼠插入（现在 2 个，之前 1）` + `输入热插拔：抓取集合已重扫（2 个设备）`。
   如果键盘仍然没反应，先跑 `cd re/tools && KY=kylin ./gate.sh --hw-kmb`，
   再看日志里有没有 `抓取设备 /dev/input/by-id/...`。

排障看两个日志：`C:\Users\Public\km.log`（键鼠代理，每次 LOCAL/REMOTE 切换都带原因）、
`/tmp/otikm.log`（麒麟，`撞边(…) → 通知对端收回控制权` 表示它在喊对端收回）。
**"麒麟一直喊、Windows 不动"最常见的原因是 Windows 静默摘除了低级钩子**
（`LowLevelHooksTimeout`）→ 收不到 F24 回程令牌。第 36 轮起：令牌连发 3 轮 +
**REMOTE 期间每 30 秒自动重装钩子**（`hooks reinstalled #N` 会出现在 km.log 里）。

**剪贴板丢内容（"复制粘贴不了"）**：帧通道会**静默丢帧**（实测连发 15 条丢 6 条，两侧日志都不报错），
第 36 轮加了 **ACK + 1.5 秒超时重发**（最多 3 次）。日志里看 `未确认 → 第 N 次重发`；
连续 3 次仍无确认会打 `[warn] …重发 3 次仍无确认，放弃`（对端不在/链路坏）。
真人节奏（复制→确认过去→再复制）实测两个方向各 6/6；
**极限连发（<2 秒一条）会顶掉上一条的重发窗口**，仍可能丢 —— 这是已知限制。

### 15.14 交接手势（**改这块逻辑前必读**）

* **交出去**：把指针推到本机屏幕的允许边（配置 `edges`）→ 抓取本地键鼠、开始转发 HID 包。
* **收回来**（第 36 轮新增，第 47 轮修正）：在**对端屏幕上把推出去的距离推回来、再多推一点**
  （镜像手势）→ 发释放包 + F24 令牌、解除 grab，指针落到"当初出去的那条边"往里 `edge_px+8`，
  并用 `XWarpPointer` 把真实光标同步过去（以前它停在边上，轻轻一碰就又被推出去）。
  判据 = **未缩放位移累计** ≥ `max(8, edge_px*4)`（2026-09-21 修正：以前只看单次位移、
  慢推永远回不来；且远端几何中途到达会换坐标系 —— 现在驱动期间冻结几何，见 NOTES §59）。
  `rem_entry_x/y` 记录"从对端哪条边进来的"。
* **热键**：`ctrl+alt+space` 切换、`ctrl+alt+left` 拉回本机（**键盘在哪一侧就在哪一侧按**）。
* 逻辑回归：`cd re/otilink && make coretest && ./coretest`（45 项断言，不需要设备/图形环境）。
  **改 `otikm_core.c` 的交接/回程逻辑后必须跑它** —— 这条路径真机验证要靠人手推边缘，成本极高。
  拓扑 B（键鼠在麒麟）真机回归：`re/otilink/kmbret.sh`（合成手势，不抢用户光标；注册在 `gate.sh --hw-kmb`）。

### 15.4 Windows 自启

`%APPDATA%\...\Startup\otilink-agent.vbs` → 隐藏窗口启动两个进程：

| 进程 | 通道 | 日志 |
|---|---|---|
| `C:\Users\Public\otiagent2.exe --edge right` | 厂商 HID 包通道 | 无（若要观察：`--status` / 手工前台跑） |
| `powershell.exe -File C:\Users\<你的用户名>\otilink\otiagent.ps1 -Cable -Clipboard` | 协议帧管道 | `C:\Users\Public\clip.log`（`clip.err` 收错误） |

（第 34 轮起自启也给剪贴板代理带日志：不开日志的话，用户报"粘贴没反应"时无从下手。）
仓库里的 `re/windows/otilink-agent.vbs` 与现场那份保持一致，**改完用 `deploy.sh` 部署**。
`.vbs` 是 CRLF 行尾，LF-only 时 WScript 会静默不执行。

麒麟侧自启 `~/.config/autostart/otilink-kvm.desktop` → `run-kylin.sh`，
它会**自动判断**本机有没有自己的键鼠：没有则进"被驱动侧模式"（`--return-on-edge`，不 grab、不注入）。

---

## 16. 【第 37 轮】"回不来/键鼠失灵"根治：厂商关停 + 钩子线程修正

> 完整复盘见 `NOTES §45`，一页版见 `HANDOFF §4.7`。**遇到"移到麒麟就回不来"先看这一节。**

### 16.1 第一处置：关停厂商 GO! Suite（一次 UAC）

厂商（`MacKMLink` / `LinkEngKM` / `LEWD` / `SKLoader`）与我们**共用同一条帧管道和 HID 通道**，
而且它的"交还控制权"是**握手式**的、要等一个**麒麟侧不存在**的代理应答 → 它一接管就**必然卡死**。

```bash
# WSL 侧（会弹一次 UAC，点"是"）
cmd.exe /c "cscript //B //NoLogo C:\\Users\\Public\\vendor-off.vbs"
cat /mnt/c/Users/Public/vendor-off.log      # 全过程日志
```
脚本：按 `LEWD → LinkEngKM → MacKMLink →（路径匹配）SKLoader` 杀（**LEWD 是看门狗，必须第一个**），
随后 20 秒重生守卫，并删 `HKCU Run\CS Dispatch`、启动项、计划任务、服务。
**恢复厂商**：把那个 Run 键值加回去（或重装 GO! Suite）。**厂商与我们的 agent 不能同时跑。**

### 16.2 "回不来"的四步定位（10 分钟内能定到层）

| 步 | 看什么 | 结论 |
|---|---|---|
| ① | `C:\Users\Public\km.log` 有没有 `REMOTE` | 没有 → 转发不在我们手里（厂商复活？钩子死了？） |
| ② | `km.log` 有没有 `kbd: F24 token seen` | 没有 → **钩子哑了**；看上一行 `hooks reinstalled … tid=` 是不是主线程 |
| ③ | 麒麟 `/tmp/otikm.log` 有没有 `撞边…` + `回程令牌 F24 已发…rc=0` | 没有 → 被驱动侧判据问题（`DISPLAY`/真实光标读取，见 §14） |
| ④ | 端到端对照：`kbdprobe2.ps1`（Windows）+ `hidprobe`（麒麟） | 分层定位到"钩子/线缆 HID/协议"哪一层 |

```bash
# ④ 的两个探针
cd re/otilink && make hidprobe && scp hidprobe.c kylin@<麒麟IP>:~/otilink/   # 麒麟侧再 make hidprobe
# 麒麟：往线缆写鼠标包（看 Windows 光标动不动）
ssh kylin@<麒麟IP> 'cd ~/otilink && ./hidprobe /dev/sg3 1 10'
# 麒麟：往线缆写 F24（看 Windows 代理是否回 LOCAL）
ssh kylin@<麒麟IP> 'cd ~/otilink && ./hidprobe /dev/sg3 2 3 0x87'
# Windows：独立钩子探针（在跑的时候从麒麟发 F13/F24，应看到 vk=0x7C / vk=0x87）
powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'C:\Users\Public\kbdprobe2.ps1' -Seconds 8
```

### 16.3 `km.log` 的读法（状态机，一行一次切换）

```
otiagent2 up. device=\\.\H:  edge=right  hotkey-back=Ctrl+Alt+VK27  [build mainhook-3]
[09:53:45] LOCAL   (startup)  -- Windows keeps its own input
[09:53:55] REMOTE  (cursor pushed out of the hand-over edge)  -- input is forwarded to the peer
[09:54:32] kbd: F24 token seen (mode=REMOTE, down=True)
[09:54:32] LOCAL   (peer token (F24))  -- Windows keeps its own input
```
* `[build …]`：**认版本用**。`csc /out:` 覆盖**正在运行**的 exe 会**静默失败** → 没有它就会一直跑旧二进制（踩过）。
* `hooks reinstalled #N (… ) tid=`：**tid 必须是主线程**。低级钩子只在安装线程的消息泵里派发，
  被别的线程装上去 = 整套钩子哑掉（本轮根因）。
* `kbd: F24 token seen`：令牌到达钩子。缺它 = 键盘钩子没在跑。
* **同时只能有一个 `otiagent2` 实例**（多开会出现多套钩子，行为不可预测；`deploy.sh --restart-km` 会先杀旧的）。

### 16.4 编译/重启键鼠代理的正确姿势

**推荐：`cd re/windows && ./deploy.sh --restart-km`** —— 它现在是一条龙
（**先杀 → 再编译 → 再启动 → 最后打印 `[build …]` 版本**），不会再出现"跑的还是旧 exe"。
手工流程（脚本不存在时）：

```bash
# 1) 先杀（运行中覆盖会静默失败）  2) 再编译  3) 最后启动
powershell.exe -NoProfile -Command "Get-Process otiagent2 -EA SilentlyContinue | Stop-Process -Force"
powershell.exe -NoProfile -Command "& \"C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe\" /nologo /target:exe /out:C:\Users\Public\otiagent2.exe C:\Users\Public\otiagent2.cs"
cmd.exe /c "cscript //B //NoLogo C:\\Users\\<你的用户名>\\otilink\\restart-km.vbs"    # 正常模式(LOCAL)
# 强制进 REMOTE 做回程测试：re/windows/restart-km-remote.vbs（带 --remote）
```

---

## 17. 【第 39 轮】免安装绿色包（换机器/换发行版时用这个）

> 一句话：**不装任何东西**，把 `re/portable/dist/otilink-portable-<ver>/` 拷到 U盘，插上线就能用。
> 细节与"已验证/未验证"清单见 `re/portable/README.md`；门禁 `./gate.sh --portable`。

### 17.1 Linux 侧

```bash
cd <绿色包目录>
./otilink.sh --doctor          # 体检（不需要 root）：线缆/会话/剪贴板后端/输入设备
sudo ./otilink.sh             # 启动（脚本自己提权一次；会问"键鼠插在哪一侧"）
sudo ./otilink.sh --role slave --peer-side left    # 显式指定角色与对端方位
./otilink.sh --print-plan     # 只看判定（角色/设备/边/屏幕/剪贴板），不启动
```

* 日志默认 `/tmp/otilink-<uid>.log`；`--log FILE` 可指定（跑门禁时指到 `/tmp/otikm.log` 以复用现有断言）。
* **停止要 sudo**：`sudo pkill -x otikm`（绿色包以 root 跑，普通用户杀不掉）。
* 已经有 otikm 在跑时入口会**拒绝启动**（一条链路只能有一个主人）；确认要强起用 `OTILINK_FORCE=1`。
* 启动时会打 `== 绿色包决策: 传输=… 角色=… 屏幕=… ==` 与角色判定依据（排障先看这行）。

### 17.2 Windows 侧

```bat
otilink.cmd                     ；交互问角色后启动
otilink.cmd master --edge right ；键鼠在本机：起键鼠代理 + 剪贴板代理
otilink.cmd slave               ；键鼠在对端：只起剪贴板代理（键鼠零软件）
otilink.cmd --scan              ；只探测线缆盘符（应打印"可作数据通道: \\.\X:"）
otilink.cmd --stop              ；停掉本包拉起的代理
```

* 日志在同目录（`--log-dir` 可改）：`km.log` / `clip.log`；启动用的 `otilink-run-*.cmd` 也在那里（证据）。
* 不需要管理员、不写注册表、不加开机自启（免安装的代价：每次插线点一次）。
* **别同时跑安装版代理**：绿色入口发现已有 otiagent2 不会重复起；要切换先 `otilink.cmd --stop`。

### 17.3 出问题先看这三行

| 症状 | 先看 |
|---|---|
| 起了但没反应 | `otilink.sh --doctor` 的"线缆/会话"两行；Windows 看 `otilink.cmd --scan` |
| 指针回不来 | 被驱动侧日志有没有 `撞边…回程令牌 F24 已发…rc=0`；接收端热键（Linux Ctrl+Alt+← / Windows Ctrl+Alt+→） |
| 剪贴板不动 | `--doctor` 的"剪贴板"那一行（Wayland 要 wl-clipboard，X11 要 xclip） |

### 17.4 用久了之后"剪贴板单向不通、拔插无效"（第 40 轮现场故障）

症状：**W→L 能粘、L→W 粘不出来**（或反过来），**键鼠一直正常**；拔插也不管用。

判据（一眼定位）：

1. 麒麟：`grep -a "重发\|放弃\|ERR recv" /tmp/otikm.log | tail` → 出现 `重发 3 次仍无确认…放弃` + `ERR recv rc=524546/526080`
2. Windows：`Get-Content C:\Users\Public\clip.log -Tail 20` → 出现 **`设备连续读取失败，尝试重连`** 反复刷

**恢复（两端都要重启，一步到位）**：

`@bash
cd re/windows && ./deploy.sh --restart            # ① Windows 剪贴板代理
ssh kylin@<麒麟IP> 'pkill -x otikm; cd ~/otilink && ./run-kylin.sh'   # ② 麒麟 otikm
`@

> ⚠️ 卡死的是**设备侧会话**：只拔一端、或只重启一端，另一端还攥着旧句柄 → 看起来"拔插没用"。

**预防（已默认开启）**：`keepalive = 5000`（麒麟 `~/.config/otilink/kvm.conf`；绿色包 `--auto` 默认 5000ms）：
每 5 秒一次 PING + 一个全零 dummy 帧，让帧管道别空闲到卡死。见 `re/NOTES.md §48`。

**补充（第 41 轮）**：如果"某一个方向一直失败、另一个方向正常"，先看麒麟日志有没有
`已通知厂商端交还控制权（…668 字节）` 在**每 1.5 秒刷屏**（指针停在边缘就会）—— 那是厂商 XML 通知
在抢发送授权。修法已落地：`vendor_notify` 默认 off（`--vendor-notify on` 才发，且 >=5s 节流）。
另一个立刻可用的绕过办法：**把指针从左边缘挪开**再复制。

**手感调优（第 41 轮续）**：被驱动侧把指针推出边缘交还控制权时，旧版要求"贴边停留 250ms"，
用户反馈"卡一下"。现在默认 **立即交还**（`--edge-dwell 0`，与主控端撞边一致）；
如果想避免误触，可以在麒麟侧加 `--edge-dwell 250`（或配置 `edge_dwell = 250`）。

---

## 18. 【第 49 轮】"我在 Windows 上打字，麒麟也在同步打字"（双重输入）与"剪贴板全废"

### 18.1 双重输入：主控端没独占本地键盘

**症状**：指针推到 Windows（已接管对端）后，同一个按键**既进 Windows、又落在麒麟本机桌面**。

**原理**：主控端（麒麟）在驱动对端时必须 `EVIOCGRAB` **独占**本地键盘，否则按键会"本地一份 + 转发一份"。
能不能独占只看一件事：**键鼠真正走的那条通道**是否健康（HID 直发模式看 HID 写失败计数；
详见 `re/NOTES.md §63`）。旧版本用"协议帧管道健康"当判据，而 HID 直发时对端**零软件**、
帧管道本来就该静默 → 纯键盘永远不独占 → 必然双重输入。

**现场三步定位**：

```bash
# ① 麒麟日志：接管期间键盘独占了吗？（要看到 "已独占 <你的键盘>"；出现 "键盘不独占" = 会双重输入）
grep -a "独占\|键盘不独占" /tmp/otikm.log | tail -5

# ② 独立探针（不依赖日志）：指针在对端时键盘应为 BUSY（EBUSY=16）
ssh kylin@<麒麟IP> "python3 - <<'PY'
import fcntl, os, errno
p = '/dev/input/by-id/usb-SIGMACHIP_USB_Keyboard-event-kbd'   # 换成你机器上的键盘
fd = os.open(p, os.O_RDONLY)
try:
    fcntl.ioctl(fd, 0x40044590, 1); fcntl.ioctl(fd, 0x40044590, 0)
    print('FREE：没被独占 → 会双重输入')
except OSError as e:
    print('BUSY：已独占 ✓' if e.errno == errno.EBUSY else e)
PY"

# ③ 完整回归（约 2 分钟；用 uinput 虚拟键鼠，不碰真键鼠；会短暂停/起麒麟 otikm）
cd re/tools && KY=kylin@<tailscale-IP> ./gate.sh --hw-kbdexcl
```

**修（本轮已落地）**：`otikm.c` 的 `km_path_ok()` + 驱动期"维持独占"。
部署与重启：

```bash
cd re/tools && KY=kylin@<tailscale-IP> ./gate.sh --deploy-kylin
ssh kylin@<麒麟IP> 'pkill -x otikm; cd ~/otilink && sg input -c "DISPLAY=:0 nohup ./run-kylin.sh > /tmp/rk.log 2>&1 &"'
grep -a "已独占" /tmp/otikm.log | tail -3      # 推到 Windows 后再看一眼
```

**临时逃生**：麒麟键盘 **`Ctrl+Alt+←`** 立刻拉回本机（热键由 otikm 自己读捕获设备，独占不影响它）；
或 `run-kylin.sh --no-grab` 整体关掉独占（代价：鼠标也会双重输入）。

### 18.2 剪贴板双向全废、日志刷 "（对端在吗？）"：先查 Windows 侧代理在不在

拓扑 B 的**键鼠**是零软件，但**剪贴板代理仍要跑**。它不在 → 帧管道没有对端 → 剪贴板全废
（**并且**会把 §18.1 的旧判据永久钉死在假，顺手制造双重输入）。

```bash
cd re/windows && ./deploy.sh --restart          # 拉起剪贴板代理（不要传 -Inject）
# 确认进程：Get-CimInstance Win32_Process | ? { $_.CommandLine -like '*otiagent.ps1*' }
# 麒麟侧应随即出现：CLIP 应用远端剪贴板 N 字节（… write rc=0，已抑制回发）
```

> 2026-10-08 现场就是这一条：Windows 重启后两个代理都没起来（`km.log` 停在 09-21、`clip.log` 停在 09-24）。

### 18.3 接管期间"偶尔漏字"是线缆固件缺陷，不是你的错觉

线缆 HID 键盘接口**会丢报表**（`re/NOTES.md §61`，真机实证）；而且"写成功但没到"**无法自动检测**。
本轮之后键盘在接管期间**只**往对端送（本机不再重复），所以丢包表现为"偶尔少一个字"。
根治方向：键盘改走帧管道 + Windows agent `SendInput`（`§61.3`，尚未落地）。

---

## 19. 【索引】症状 → 病因 → 一句探针（排障先看这一节）

### 19.1 为什么需要这张表（**别按记忆改代码**）

同一个症状在本项目里对应过**好几个互不相同**的真因，每轮只修掉一个，剩下的病因会照旧复现
**同一句话**——用户看到的是"又坏了"，我们看到的是一次次"根治"：

* 「指针回不来」有 **5 个**真因（`NOTES §44 / §44.7 / §45 / §59 / §60`）；
* 「键盘有问题」有 **4 个**真因（`§59.2 / §60.1 / §61.2 / §63`）。

所以顺序永远是：**① 先跑探针定到"是哪一条病因" → ② 再动手 → ③ 改完跑对应门禁**。
先看 `AGENTS.md §5` 的"改了什么 → 跑哪档"矩阵。

**三条命令覆盖 80% 的现场**（三条都不改任何东西）：

```bash
cd re/tools
KY=kylin@<tailscale-IP> ./gate.sh --pre                       # ① 谁在转发/厂商在不在/线缆/代理在不在
ssh kylin@<tailscale-IP> 'grep -a "独占\|键盘不独占\|令牌\|看门狗\|抓取\|重开" /tmp/otikm.log | tail -25'   # ② 麒麟侧
powershell.exe -NoProfile -Command 'Get-Content C:\Users\Public\km.log  -Tail 20; Get-Content C:\Users\Public\clip.log -Tail 10'   # ③ Windows 侧
```

### 19.2 症状 → 病因 → 探针

| 症状（用户原话） | 可能的病因（按历史出现次数） | 一句探针（怎么区分） | 细则在哪 |
|---|---|---|---|
| **「指针移到对端回不来」** | ① 主控端回程判据缺失／只看单次位移 | 麒麟日志有没有 `回程判据：已把推出去的位移推回…`；**小步（2px）外推**能不能回 | `NOTES §44/§59`、`§15.14` |
| | ② 厂商程序抢链路（转发根本不在我们手里） | `km.log` **没有** `REMOTE` 却光标被驱动 | `§16.1`（先关厂商） |
| | ③ 低级钩子被 Worker 线程重装 → 整套哑掉 | `km.log` 无 `kbd: F24 token seen`；看 `hooks reinstalled … tid=` 是否同一线程 | `§16.3`、L2 |
| | ④ 拔插后 `eventN` 变号，抓取 fd 失效 | 麒麟日志 `输入抓取…报错 revents=` / `输入抓取重开失败` | `NOTES §54/§55` |
| | ⑤ 对端光标真实位置与「入口边=0 位移」模型不一致 | 缺 `对端光标已停在入口边`；回程要"推很久" | `NOTES §60.3` |
| | ⑥ 看门狗把控制权交还了（链路坏） | 日志 `看门狗` / `reopen_transport` / `写侧链路故障` | `§15.13.7` |
| **「键盘用不了／按键到不了对端」** | ① 热插拔判据只看 `>0`，新插的键盘没进抓取集合 | 日志 `抓取设备(本机键鼠)` 里有没有**你那把新键盘** | `NOTES §60.1` |
| | ② 线缆 HID 键盘接口固件缺陷（只送得动约 6 条报表） | `kbdprobe2.ps1` 里只有 `vk=0x87`（F24），之后一片空白 | `NOTES §61.2` |
| | ③ 接管时 6 条 F24 令牌把那点额度吃光 | 日志 `指针交给对端` 紧跟 `→ 回程令牌 F24 已发`，而随后按键全丢 | `NOTES §63.9` |
| | ④ 修饰键抑制缓冲没放行（Ctrl 类组合被吞） | 日志有没有 `HID 补发被抑制的 N 个按键事件` | `NOTES §59.2` |
| | ⑤ 键盘被独占但键送不出去（"死了"，本机也打不了） | 日志有 `已独占 <键盘>` 但 Windows 收不到、本机也收不到 | `NOTES §61`（根治看 §61.3） |
| **「两机同时打字」（双重输入）** | 判据挂错通道 → 纯键盘没独占 | `grep -a "已独占\|键盘不独占" /tmp/otikm.log`：要看到 **`已独占 <你的键盘>`**；独立探针应 EBUSY(16) | `NOTES §63`、`§18.1`、`gate.sh --hw-kbdexcl` |
| **「键鼠过几秒卡一下」** | 保活 dummy 帧（64KB）堵住单队列设备 ~1.3s | `gate.sh --hw-lat`：`MAXGAP` 必须 ≤200ms | `NOTES §61.1` |
| **「剪贴板单向／双向不通」** | ① Windows 剪贴板代理没跑（重启后最常见） | 查进程：`Get-CimInstance Win32_Process \| ? { $_.CommandLine -like '*otiagent.ps1*' }` 为空 → `deploy.sh --restart` | `§18.2` |
| | ② 设备侧会话卡死（用久了/拔插无效） | 麒麟 `重发 3 次仍无确认…放弃` + `clip.log` `设备连续读取失败，尝试重连` | `§15.13.7` |
| | ③ 厂商 XML 通知抢发送授权 | 麒麟日志 `已通知厂商端交还控制权` 每 1.5 秒刷屏 | `NOTES §49` |
| | ④ 2 秒内连续两次复制（已知限制，只救一条） | 复现节奏对得上 | `NOTES §42` |
| **「拔插后失效 / otikm 崩」** | keepalive 裸指针 use-after-free（已修）+ 抓取自愈缺失（已修） | `gate.sh --replug`（USB unbind/bind 模拟拔插 N 轮） | `NOTES §52/§53/§54/§55` |
| **「线缆冒出个假光盘 / 机器被拖卡」** | 假光盘 LUN 读不出来 → usb-storage 卡 D 态 | `gate.sh --pre` 的「线缆 MSC 在位」检查；**不要解绑 usb-storage 接口** | `NOTES §62`、`§10.4` |
| **「两侧都没反应」** | 线缆会话彻底死 | 拔插对拷线重置会话 → 重启两端 | `AGENTS §8` 急救表 |

### 19.3 必须成立的不变量（违反了就一定会出现上表症状）

| 不变量 | 违反后的症状 | 谁在守 |
|---|---|---|
| 任何时刻**只有一个主控**（grab） | 帧管道双向全丢、剪贴板两边"未确认" | `L17` + 角色协商 + `--hw-kmb` / `--hw-km` |
| **驱动侧必须独占**本地键鼠 | 两机同时打字 / 鼠标双动 | `gate.sh --hw-kbdexcl`（本轮新增） |
| 交出去的**必须收得回来**（热键 + 看门狗 + 回程） | 用户被锁死在对端，只能拔线 | `L8` + `coretest` + `--hw-kmb` |
| 判据必须与它**保护的通道同源** | 静默反向行为（本轮双重输入就是这条被违反） | `AGENTS §9` 的坑表（人工 review + 双档真机回归） |
| 一条链路**只有一个主人**（厂商/我们） | 一接管就卡死，只能拔线 | `L1` + `vendor-off.vbs` + `--pre` |

> 新增症状/新真因时：**在 19.2 表里加一行**，并把探针写成能跑的命令（而不是"看一眼"）。
> 条件允许的话直接落成脚本 + `gate.sh` 一档（见 `AGENTS §6`）——**没有断言守着的不变量，一定会再犯**。

---

## 20. 【运维】麒麟整机硬卡死：取证与处置（2026-10-08 两次实战）

> 判断标准：**SSH 连不上**（`Connection timed out during banner exchange`）、**Ctrl+Alt+F2 切 TTY 也没反应**
> = 内核没在调度（整机硬锁），不是桌面卡、更不是我们的 `EVIOCGRAB`（只是 grab 时 SSH 仍应可连）。
> 完整取证记录见 `NOTES §65`。

### 20.1 死机后**先取证，再硬重启**

```bash
# ① 面包屑：最后一格时间戳 + 有没有 D 态（含本轮新增的 /proc/<pid>/stack）
ssh kylin 'tail -60 /var/log/freeze-breadcrumb.log'

# ② 上一个 boot 的最后 50 条日志（内核最后一条消息是什么？USB？线缆 reset？）
ssh kylin 'journalctl -b -1 -n 50 --no-pager'

# ③ 有没有留下崩溃产物（vmcore / pstore）
ssh kylin 'journalctl --list-boots --no-pager | tail -4; ls -la /var/crash/ 2>&1'
echo "$KY_PASS" | ssh kylin "sudo -S -p '' ls -la /sys/fs/pstore/"
```

判定：
* 面包屑最后有 **`!!D-state: kworker/*+usb_hub_wq`** → **USB 子系统**嫌疑（2026-10-08 11:16 就是这条）。
* 面包屑最后有 **`!!D-state: usb-storage`** → 对拷线/AIC 假光盘卡 D（见 `§10.4`、`NOTES §62`），
  **不要解绑 usb-storage 接口**。
* 上一 boot 最后一条内核消息是 **`usb … USB disconnect`**（运行时，不是开机枚举）→ 查是哪台 USB 设备。
* 什么都没有、journald 直接断 → 硬锁或 IO 停顿，**别急着改 otikm**（先 `§19` 定层）。

### 20.2 处置（取证之后）

```bash
# 整机硬锁只能长按电源键；重启后确认键鼠/剪贴板恢复
cd re/windows && ./deploy.sh --restart      # 剪贴板代理
ssh kylin 'pkill -x otikm; cd ~/otilink && ./run-kylin.sh'   # 麒麟侧（自动选拓扑）
```

### 20.3 本机已装的取证基建（2026-10-08）

| 件 | 内容 | 位置 / 回滚 |
|---|---|---|
| 面包屑 v2 | 每 5s：load / top CPU / Dirty,Writeback / D 态进程 + **每个 D 进程的 `/proc/<pid>/stack`（`timeout 2`）、`wchan`**；`dmesg` 尾 30 行每 60s 最多一次 | `/usr/local/bin/freeze-breadcrumb.sh`（备份 `.bak-20261008`）+ `freeze-breadcrumb.service` |
| 硬锁必 panic | `kernel.hardlockup_panic=1`、`kernel.panic=20`（panic 后自动重启） | `/etc/sysctl.d/99-otilink-crashcapture.conf`；删文件即可回滚 |
| kdump | 实测正常：`kexec_crash_loaded=1`、`kexec_crash_size=201326592`（192MiB），`/proc/iomem: 60000000-6bffffff : Crash kernel`。⚠️ **非 root 读 `/proc/iomem` 会把地址抹成 `00000000-00000000`**，别据此判断 | 见 `NOTES §65.4` |

> ⚠️ **不要**设 `hung_task_panic=1`：本机 `usb-storage` 经常短暂 D 态，设了会变成"日常自杀"。
