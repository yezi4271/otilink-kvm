# Windows 侧 agent（`otiagent.ps1`）

> **⚠️ 本文是早期文档，部分内容已过时。当前权威说明看 `re/HANDOFF.md`（一页总览）与
> `re/RUNBOOK.md §15.13`（剪贴板运维/排障）。第 34 轮（2026-09-16）的更正：**
> * 剪贴板**已真机跑通**（文本 / PNG 图片 / 文件包，双向），不是"待联调"；上限 **8MB**、单块 65000。
> * 键鼠**不再走本 agent 的注入路径**：改用厂商 HID 包通道（`otiagent2.cs` → `otiagent2.exe --edge right`）。
> * 剪贴板代理的正确启动参数：`-Cable -Clipboard -Device \\.\H:`（**不要传 `-Inject`**）。
> * 部署改动一律 `./deploy.sh [--restart]`（补 BOM / 复制到 `C:\Users\<你的用户名>\otilink\` / 解析自检 / 重启）。
> * 回归：`re/otilink/clipreg.sh`（剪贴板 17 项）+ `re/otilink/hwtest.sh`（键鼠 12 项）。

让 **Windows 端**能说和 Linux 端（`re/otilink`）**同一套协议**，从而在
**Linux(主控) ↔ Windows(被控)** 之间做键鼠共享与剪贴板共享。

单文件、零安装：内嵌 C#（PowerShell 5.1 的 `Add-Type` 自带编译器）+ PowerShell 外壳
（剪贴板用 `Get/Set-Clipboard`，注入用 `SendInput`）。

## 为什么需要它

逆向结论（见 `re/PROTOCOL.md`）：线缆的 HID 注入通道被固件/驱动**限速到 1 次/10 秒**
（`_OTi_SendHIDPacket` 里的时间比较），它的用途是"释放所有按键"这类维护动作，
**不是**键鼠数据通路。真正的键鼠与剪贴板都走**数据管道上的消息层** —— 所以两端都要有软件。

厂商的做法是两端都跑它的 App（Win/Mac）。我们的做法是两端都跑**我们自己的协议**：
Linux 用现成的 `otikm`，Windows 用本 agent。

## 已验证 / 未验证

| 项 | 状态 |
|---|---|
| 协议编解码（KEY/MOUSE/CLIP/CRC/帧布局） | ✅ `-Selftest` 7/7 通过 |
| **与 Linux 实现的线上兼容（双向）** | ✅ `make wintest`：Linux→Windows、Windows→Linux 各 2 帧，字段逐一相符 |
| SPTI（对设备发 SCSI 私有命令） | ✅ **真机验证通过，且不需要管理员**（SPTI 结构体布局修正后即通） |
| 真机读数 | ✅ 见下：设备信息块 / 分页 / 写命令被拒的 sense |
| 注入（`SendInput`）/ 剪贴板（`Set-Clipboard`） | ⚠️ 代码就绪，待与真实设备联调 |

## 用法

```powershell
# 1) 协议自检（不需要管理员）
powershell -ExecutionPolicy Bypass -File otiagent.ps1 -Selftest

# 2) 探测哪个设备能应答厂商命令（需要管理员）
powershell -ExecutionPolicy Bypass -File otiagent.ps1 -Scan

# 3) 正常运行（需要管理员；-Inject 注入收到的键鼠，-Clipboard 双向剪贴板）
powershell -ExecutionPolicy Bypass -File otiagent.ps1 -Device '\\.\H:' -Inject -Clipboard
```

`-Scan` 会依次试 `\\.\F:`、`\\.\H:`、`PhysicalDrive1..3`，对每个设备发
`0xF0/0x00`（读 64 字节设备信息块），**能应答的那个**才是可用的数据通道（记下它，传给 `-Device`）。

## 参数

| 参数 | 说明 |
|---|---|
| `-Device` | 设备路径，如 `\\.\H:`（用 `-Scan` 找） |
| `-Inject` | 把收到的 KEY/MOUSE 用 `SendInput` 注入本机 |
| `-Clipboard` | 双向剪贴板同步（哈希去重、分块 65000、上限 1MB） |
| `-ClipIntervalMs` | 剪贴板轮询间隔（默认 300ms） |
| `-DurationSec` | 运行时长（0 = 不限） |
| `-Verbose` | 打印每条消息 |
| `-Selftest` / `-Scan` / `-FrameTest` | 自检 / 设备探测 / 跨实现帧测试 |

## 真机实测结果（Windows = 被控端）

```
设备信息块 0xF0/0x00 → 00 00 22 13 00 01 30 39 30 33 30 31 00 00 01 00
                           ↑PID 0x2213    ↑ASCII "090301" = 固件日期 2009-03-01

分页读 0xD8/0x00/<page>：
  page 0 → 03 53            page 4 → 06 40
  page 1 → 00 53            page 5 → 00 7f
  page 2 → "USBC" 开头的结构化数据（288 非零字节，含 OT 魔数）
  page 3 → 数据管道：01 00 00 00 00 00 00 ff ff ff（稳定不变）

写命令全部被拒（被控端无权写，符合主控/被控端分工）：
  写 0xD9/0x2A（厂商模板）→ rc=258 CHECK CONDITION
      key=0x9 ABORTED COMMAND, ASC=0x81（厂商自定义）   ← 命令被识别但被中止
  读 0xD9/0x60（设备模式）→ key=0x5 ILLEGAL REQUEST, ASC=0x20（命令不支持）
  0xF0/0x31（独占锁）    → 同样被拒
```

诊断模式：`-Scan`（探测哪个设备可用，**无需管理员**）、`-Dump`（信息块+连续读帧+可选写帧）、
`-Probe`（状态矩阵：锁/写/读分页/打印 sense）、`-Pages`（逐页 dump）。

## 设备有两种 USB 人格（真机实测，很重要）

| | `0ea0:2213`（"Virtual Link"） | `0ea0:2208`（"Transfer line"） |
|---|---|---|
| 大容量存储 | 只读 CDFS `MacKMLink` + FAT `VirtualLink`(1MB) | 只有一个 1MB FAT 卷 |
| HID 接口 | **有**（键盘 MI_02 / 鼠标 MI_01） | **无** |
| 信息块 [2..3] | `22 13` | `22 08` |

- 切换是**厂商的 `USBRestart`（`0xF0/0x05/0x02`，`CDB[4]=0x0A`）**触发的（`-Restart` / `-ModeSweep`）；
- 两个模式下 1MB 卷内容相同（同一块存储）；
- **2208 → 2213 用命令切不回来**（扫过 `CDB[4]=0x00..0x0F` 全部无效）→ 需要**物理重插**；
- **HID 键鼠注入需要 2213 人格**：被控端（本机）必须处于 2213 才有 HID 接口接收注入。

## 剪贴板的另一条路：1MB 共享卷（`volumeclip.ps1`）

数据管道写（`0xD9/0x2A`）在两种人格下都被 CHECK CONDITION 拒绝，
因此剪贴板改走**共享卷文件**（普通文件 I/O，不需要管理员、不需要私有命令）：

```powershell
powershell -ExecutionPolicy Bypass -File volumeclip.ps1 -Test             # 本地自检
powershell -ExecutionPolicy Bypass -File volumeclip.ps1 -Volume H:\       # 正常跑
```

约定：本机写 `otilink_clip_win.txt`、读对端写的 `otilink_clip_lin.txt`
（Linux 侧对应 `re/otilink/volumeclip-linux.sh`）。
**待验证**：该卷是否真的两端共享（需要 Linux 端配合）。

## 互操作验证现状（第 22 轮）

| 组合 | 方向 | 结果 |
|---|---|---|
| Linux otikm → agent | CLIP | ✅ 通过（Linux 剪贴板出现在 Windows 剪贴板） |
| Python 独立客户端 ↔ Linux otikm | CLIP 双向 | ✅ 通过（守护进程两向都被独立实现验证） |
| agent → Linux `otikm` | CLIP | ✅ 通过（修复 TCP 读超时/异常捕获后；`make interop` 可复现） |

-Tcp host:port 仅用于与 Linux 端 otikm 对跑的测试，不需要 SPTI/管理员。

## 注意

- **SPTI 需要管理员**（对卷/物理盘发 `IOCTL_SCSI_PASS_THROUGH_DIRECT` 要写权限）。
  没有管理员时会明确报错并提示。
- 被控端（本 agent）不抓取本地键鼠：指针在 Windows 侧时，Windows 自己的键鼠照常可用；
  若要"接管期间屏蔽本地输入"，后续可加 `LowLevelKeyboardHook` 拦截（尚未实现）。
- 与 Linux 端的角色是**对称**的：Linux 端不需要"主控模式"，它按普通对端即可
  （边缘穿越 → 发 SWITCH → 之后键鼠走数据管道；Windows 端收到即注入）。
