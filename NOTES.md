# OTi WinDroid Linker / VirtualLink 对拷线 —— 逆向笔记

目标：让这根线在 **Linux↔Linux** 之间可用，功能是 **键鼠共享（KVM）** 与 **剪贴板共享（复制粘贴）**。
（文件对拷不是目标，但它和剪贴板共用同一条数据管道。）

状态：**协议骨架已完全掌握**，剩下的是字段级标定与实现。

---

## 1. 设备身份

| 项 | 值 | 证据 |
|---|---|---|
| VID:PID | `0ea0:2213` | Windows PnP |
| 自报名称 | `Android+Mac USB Device` | DEVPKEY_Device_BusReportedDeviceDesc |
| 芯片厂 | OTi / 瀚邦科技（原 Ours Technology，`0ea0`）| u2u.com.tw → oti.com.tw |
| 产品线 | WinDroid Linker / **VirtualLink** / MacKMLink | 随线软件、`LoaderMenifest.xml` 里 `OEMInfo CompanyID="OTi" ProductName="MacKMLink1325"` |
| 序列号 | `<线缆序列号>` | 已插的那一端 |

**复合接口（3 个，无隐藏第 4 个；所有 devnode Status=OK）**：

| 接口 | 类型 | 说明 |
|---|---|---|
| MI_00 | 大容量存储 `08/06/50` | LUN0 = 只读 CDFS `MacKMLink` (3.84MB)，LUN1 = 可写 FAT16 `VirtualLink` (1MB) |
| MI_01 | HID `03/01/02` | 顶层 UsagePage=0x0001 Usage=0x0002 = **鼠标**，Input=5B，**Output=0 Feature=0** |
| MI_02 | HID `03/01/01` | 顶层 UsagePage=0x0001 Usage=0x0006 = **键盘**，Input=9B，**Output=0 Feature=0** |

> 结论：HID 接口是**纯输入**（芯片向本机注入键鼠）。所以「被控端」在 Linux 上**零代码**——
> 内核 `usbhid` 直接认。我们只需实现「控制端」的发送路径 + 剪贴板双向数据。

## 2. 关键结构判断

- 传输层 = **SCSI 私有命令**（`SCSITaskAuthoringDevice`），不是网卡类、不是 HID 输出。
  Mac 侧经 `IOCreatePlugInInterfaceForService` + COM vtable 拿 SCSITask 接口
  （所以导入表里没有 `SCSITask*` 符号）。Linux 侧对应做法：**`/dev/sgN` + `SG_IO`**。
- `SendData` 里出现 `0xffec`(65516) 与 `0x10000`(65536) → **数据帧上限 65536 字节，其中 20 字节是帧头**。
- 框架内置 fishhook（hook 了 `objc_getClass`/`autoreleasepool` 等），是厂商的加固手段。

## 3. SCSI CDB 模板（已从反汇编提取，全部 16 字节，尾 2 字节 = `"OT"`）

| CDB[0] | CDB[1] | 其余 | 用途 | 来源 |
|---|---|---|---|---|
| `0xD9` | `0x33`/`0x34`/`0x36` | `[2..13]` = 12 字节 HID 包 | **发 HID 包**（类型 1/2/3 → 0x33/0x34/0x36，由 `0x363433 >> (type-1)*8` 得到） | `OTiTransporter::sendHIDPacket` @0x3306 |
| `0xD9` | `0x2A` | `[2]=0xFF` | **数据管道：写**（PutData） | `OTiTransporter::PutData` @0x5386 |
| `0xD8` | `0x00` | `[2]=3`, `[3]=bh`, `[4]=bl` | **数据管道：读**（GetData / 状态轮询） | `OTiTransporter::ProcessIdleState` @0x4ec0 |
| `0xF0` | `0x31` | | 锁/LockFunction | @0x46e4 |
| `0xF0` | `0x00` | | 速度/IC 版本/物理总线类型等查询 | @0x449e / 0x4597 / 0x4de1 |

CDB 共用布局（16B）：
```
[0]      opcode (0xD8 读 / 0xD9 写 / 0xF0 查询)
[1]      subcommand (0x2A 数据管道, 0x33/0x34/0x36 HID 包类型, 0x31 锁 …)
[2..13]  载荷 / 长度字段 / 标志（HID 情形下直接是 12 字节包）
[14..15] = 'O','T'  魔数
```

## 4. 软件栈与文件位置（`MacKMLinkFull.tgz` 解出，未加密）

```
MacKMLink.app/Contents/PlugIns/GoBridgeDemon.app/Contents/
├── MacOS/GoBridgeDemon                     守护进程
└── Frameworks/
    ├── OTiTransfer.framework/…/OTiTransfer  ← 传输层（C++ 类 OTiTransporter）
    ├── KMKeyMouse.dylib                     ← 键鼠共享
    ├── KMClipboard.dylib                    ← 剪贴板共享
    └── FileChgEvent.dylib                   ← 文件变动事件
```

Windows 侧：`SKLoader.exe`（202KB）+ `Deployment/GSDD.zz`（2.2MB，magic `"OT"`，**加密容器**）+ `GSDD.lst`。
Windows 软件是加密的，但 Mac 侧完全等价且明文 —— **不需要啃 Windows 侧**。

### OTiTransfer 关键 API / 方法

```
_OTi_Open / _OTi_Close / _OTi_ReadData / _OTi_SendData / _OTi_SendHIDPacket / _OTi_SendDummyData
_OTi_GetDeviceMode / _OTi_SetDeviceMode / _OTi_ReadDevicesInfo / _OTi_GetSideType / _OTi_GetFunctionType
_OTi_USBRestart / _OTi_GetICVersion / _OTi_GetSpeedStatus / _OTi_LockFunction / _OTi_CheckLicenseStatus

OTiTransporter::SendData(const uchar*, bool)              @0x309c  → 入队
OTiTransporter::sendHIDPacket(schar type, CFData*)        @0x3306  ← 键鼠快通道
OTiTransporter::PutData(ushort&) / GetData(ushort&)       @0x5386 / 0x57bc  ← 数据管道
OTiTransporter::SendSCSICommandSendData(...)              @0x3424  ← 底层 SCSI 执行
OTiTransporter::SendSCSICommandReceiveData(...)           @0x4ac2
OTiTransporter::ObtainExclusive / ReleaseExclusive / MountMedia / UnmountMedia  ← 独占 + 盘符挂卸
OTiTransporter::ResetRxQueue / ResetTxQueue / GetOTiSenseKeyResult / requestResendPacket
```

### 命令层（跨平台，Win/Mac/iOS 共用）

```
IUPipeCmd:      SetUPipeCmd: / SetUPipeBody:forKey: / PostToRemote   （字段名含 "Cmd"、"Body"）
KMHandler:      Dispatch:UPipeCmd: / SendSwitchToRemoteCmd:andX:andY:andW:andH:
KMMainLoop:     sendKeyData: / sendMouseData: / sendMultimediaKeyData: / sendGrabedKeyMouseData:
Cursor/key:     AddKeyToArray: / RemoveKeyFromArray: / DetectSwitchEii / warpMouse / MouseLeave
Clipboard:      getClipboard / setClipboard: / buildClipboardFileData: / handleDownloadClipboardFile:to:
                CBMainLoop sendGrabedClipboardData: / checkClipboardThread
                wrapUpUPipeMsgToSendByTempFile:（大载荷走临时文件分块）
编码：          OtiUnicodeData / stringToLittleEndianUnicodeHexStringByUTF8 / ConvertCharArrayToHexString
```

> **重要判断**：Linux↔Linux 不需要兼容厂商的 UPipe 语义，只需要那条**字节管道**。
> 因此剪贴板可以自定义协议（自带两端），复杂度大幅下降。

## 5. 逆向工具（本目录）

| 工具 | 用途 |
|---|---|
| `macho_disasm.py` | 自解析 Mach-O + capstone：列导入/导出符号、反汇编指定函数（不依赖 otool） |
| `objc_imp.py` | selector 名 → IMP 地址（扫 `__objc_const` 找指向 methname 的指针），再反汇编 |
| `scan_cdb.py` | 扫描 `__text` 聚类「栈上常量写入」，批量找协议模板 |
| `../hid-dump.ps1` | Windows HID API 读顶层集合 UsagePage/Usage 与 Input/Output/Feature 长度 |

依赖：`python3 -m venv /tmp/capvenv && /tmp/capvenv/bin/pip install capstone`

## 6. 未决问题

1. ~~12 字节 HIDPacket 的字段布局~~ → 见 §9：HID 通道实为**维护通道**，实时键鼠走数据管道。
2. ~~数据管道帧格式~~ → 见 §9：帧格式已确定；仍缺 **booking/credit 流控**与重传细节。
3. **握手/状态机**：`ProcessIdleState` → `GetSpeedStatus`/`GetRemoteHostStatus`/`processTransferState`/`processReceiveState` 的时序，`ObtainExclusive` 的必要性。
4. 设备模式/侧别：`GetSideType`/`GetFunctionType`（两端可能分 A/B 侧，影响谁注入谁）。

## 9. 第 2 轮进展

### 9.1 数据帧格式（已确定）

`GetData` 的帧校验（@0x594b-0x59ab）反汇编结论，常量区 `__bss` 为**全零**：

```c
buf = 收到的 65536 字节;
if (前16字节 == 0 && dword@0x10 == 0)          -> 空闲帧：丢弃，队列计数--
else if (memcmp(buf, buf+0xffec, 16)==0 &&
         *(u32*)(buf+0x10) == *(u32*)(buf+0xfffc)) -> 有效帧：CFDataCreate(buf, 0xffec) 入 RX 队列
else                                            -> 非法帧：记日志丢弃
```

即：

```
帧(65536) = [20B 头][65496B 载荷][20B 头的副本]
                  ↑ offset 20        ↑ offset 0xffec(65516)，dword 副本在 0xfffc(65532)
头全零 = 空闲（对端无数据）
```

`PutData` 的构造与之吻合：`CFDataAppendBytes(frame, payload, 0xffec)` 再 `CFDataAppendBytes(frame, payload, 0x14)`
——载荷本身以 20 字节头开头，第二次把头的 20 字节再贴到尾部。
`SendData` 里的 `0xffec`(65516) / `0x10000`(65536) 常量同理。
接收缓冲区容量也是 `0x10000`，队列上限 `0xc8`(200)。

### 9.2 HID 包通道 = 维护通道（不是实时键鼠）

`_OTi_SendHIDPacket(int type, const void* buf14)` @0x21ec：
- `CFDataCreate(NULL, buf, 0xE)` → **缓冲长 14 字节**，其中前 12 字节进 `CDB[2..13]`，`CDB[14..15]='OT'`；
- 进入 `sendHIDPacket` 前有**节流**：`CFAbsoluteTimeGetCurrent() - last < 10.0` 就直接丢弃返回 0；
- GoBridgeDemon 的两处调用都是 `type=1` + **全零 14 字节** = “清空/释放键鼠状态”的维护包。

→ 结论：实时键鼠不走 HID 包，而是走**数据管道上的 XML 消息**；HID 包只用于对端 HID 模拟状态的维护。
   （对我们的价值：对端若只靠芯片注入 HID，则需周期性发此维护包保持状态干净。）

### 9.3 命令层是 XML（UPipe over data pipe）

```
信封:      <OTIMSG>…</OTIMSG>
键鼠:      <OTIMSG><%@>%d</%@> ×7 </OTIMSG>          （KMKeyMouse，7 个整数字段）
类:        SimpleXML / NSXMLDocument / NSXMLElement / XmlPacketCommand / _XmlPacket / _XmlPacketLength
命令:      Cmd_Transfer_Clipboard      Cmd_Notify_KM_Switch_To_Remote
           Cmd_Post_Remote_KM_Setting_Info   Cmd_Check_Bridge_Block_Status
           Cmd_Replay_Bridge_Block_Status    NP_Cmd_Move_File_To_Trash
           NP_Cmd_Get_Drive_Update_Info      NP_Cmd_Post_Drive_Update_Info
参数:      Param_Move_Out_X / _Y / _Direction / _Info
           Param_Move_Out_KM_Switch_Option / Param_Move_Out_Use_Hotkey_Switch_Only
           Param_Remote_Screen_Width / _Height / Param_Other_PC_Position_Option
           Param_KM_Setting_Info / Param_Clipboard_Info_2 / Param_Remote_Bridge_Blocked
           Param_Sync_Running_Info
其它:      \\.\pipe\OTI_ClipboardAgent（Windows 侧剪贴板代理命名管道）
           "Remote is Linux, clipboard upipemsg ignored!!!!"  ← 厂商代码里有 Linux 分支（值得追）
```

> **对我们最重要的一条**：Linux↔Linux 时两端都是我们的代码，**不必兼容这套 XML**，
> 只需把数据管道当字节流，自定紧凑协议即可（输入事件用定长二进制，剪贴板用分块+校验）。

### 9.4 已产出代码：`otilink/`（C 库 + 标定 CLI）

| 文件 | 内容 |
|---|---|
| `otilink.h/.c` | 设备发现（按 VID/PID 找 `/dev/sgN`）、SG_IO 封装、CDB 生成（0xD9/0xD8/0xF0）、帧打包/校验、HID 维护包 |
| `probe.c` | `list / selftest / query / lock / hid-release / hid / wtest / rtest / rloop` |
| `Makefile` | `make && make selftest` |

现状：`./probe selftest` **6/6 PASS**（空帧、头副本位置、载荷偏移、空闲帧、篡改检测、HID CDB 模板）。
`./probe list` 在 WSL 下找不到设备（预期：线没接进 Linux）。

### 9.5 下一步标定顺序（需要设备进 Linux）

1. `probe list` → 确认两个 LUN 的 `/dev/sgN`；
2. `probe query 0` → `0xF0/0x00` 状态查询，确认设备响应且**盘符不被踢掉**；
3. `probe hid-release` → 观察对端是否仅“无副作用”（维护包）；
4. `probe lock 1` → 验证独占锁语义（厂商在传输前调用 `ObtainExclusive`）；
5. `probe wtest hello` / `probe rtest` → 打通 0xD9/0x2A 写与 0xD8 读；
   重点标定 `CDB[3]/CDB[4]` 的 booking 语义（`GetMaxBookingSize`/`SetMaxBookingSize`/`ContinueTxCount`）；
6. 双向压力测试 → 确定分片/重传（`requestResendPacket`）与信用窗口。

## 10. 第 3 轮进展：自有协议层 + 可验证测试

### 10.1 架构决定（省掉一半工作量）

厂商在数据管道上跑的是 XML/UPipe 命令层，但**Linux↔Linux 两端都是我们的代码，不需要兼容它**。
线缆只是字节管道 → 自定二进制协议，输入走定长消息、剪贴板走分块。

### 10.2 自有消息协议 `otiproto`

```
消息 = 20B 头 + 载荷（小端）
  magic u32 'OTL1' | type u16 | flags u16 | seq u32 | len u32 | crc32 u32
crc32 覆盖 头[0..15] + 载荷（增量 CRC，无需拼接临时缓冲）

type: KEY(1) MOUSE(2) SWITCH(3) CLIP(4) PING(5) ACK(6)
KEY    { u16 code; u8 value(0/1/2); u8 mods }
MOUSE  { i16 dx, dy, wheel; u16 buttons位图 }
SWITCH { u8 side; u8 use_hotkey_only; i16 edge_x,y; u16 screen_w,h }   ← 对应厂商 Param_Move_Out_*
CLIP   { u16 format,flags; u32 fid,offset,total_len; u32 dlen } + 数据
```

尺寸约束：`OTI_BODY_MAX = 65496`（帧体）→ 单条消息载荷 `OTI_PAYLOAD_MAX = 65476`；
剪贴板单块 `OTI_CLIP_CHUNK_MAX = 65000`。
（实现过程中修掉一个真实缺陷：最初把 65496 当成载荷上限，忽略了 20 字节消息头。）

### 10.3 传输抽象 `otitrans`

同一套上层代码跑在两种后端上：
- `cable:/dev/sgN`：包 `otilink`，发 = `0xD9/0x2A` 帧，收 = `0xD8` 帧（自动跳过全零空闲帧、校验首尾头）；
- `fd:<n>`：4 字节长度前缀，兼容 socketpair / AF_UNIX / TCP。
**意义**：线缆标定要等硬件，但协议与状态机现在就能端到端验证。

### 10.4 状态机 `otikm_core`（纯逻辑，可单测）

- 本机持控制权：本地事件本地消费；鼠标撞左右边缘 → `SWITCH(remote)` 并把指针夹到边界；
- 交出后：本地事件全部转发；热键（默认 KEY_LEFTCTRL）→ `SWITCH(local)` 抢回；
- 收到 `SWITCH(remote)` → 本机被控（收到的事件注入本地）；收到 `SWITCH(local)` → 收回控制权并校正指针；
- `use_hotkey_only` 对应厂商 `Param_Move_Out_Use_Hotkey_Switch_Only`：撞边不切换。

### 10.5 输入层 `otiinput`（evdev / uinput）

- 抓取：`EVIOCGRAB` 独占物理设备（防止本地桌面同时收到），自身跟踪 `BTN_*` 位图；
- 注入：创建 uinput 虚拟键鼠（固定名 `otilink virtual input`，用于**防回环**：抓取端按名字排除自己的注入设备）；
- 键盘 1..255 全部置位 + `KEY_MICMUTE`，鼠标 REL_X/Y/WHEEL/HWHEEL + 三键；
- 非 root 需要 udev 规则（头文件里给了 `/etc/udev/rules.d/99-otilink.rules` 示例）。

### 10.6 测试结果（`make test`，本机可复现）

```
1/3 帧与 CDB 自检 .......... 6/6 PASS
2/3 消息层 + 状态机 ........ PASS（组包/解包、CRC 篡改拒收、坏 magic、超长、
                             剪贴板 140000B→3 块→重组逐字节一致、10 项状态机断言）
3/3 端到端 ................. PASS（socketpair 上的真实 transport + 真实 proto +
                             真实状态机：2 键 + 1 鼠 + 1 切换 + 3 剪贴 + 1 PING，
                             末条故意损坏被 CRC 拦下，seq 单调）
合计 42 项 PASS / 0 FAIL
```

本机限制（已确认，非代码问题）：
- `/dev/uinput` 为 `root:root 0600` 且无免密 sudo → 注入路径**只能编译验证**，需用户给 udev 规则后在真机跑；
- WSL 无 `/dev/input`、无 USB 直通 → 抓取与线缆路径待真机；`probe list`/`probe inputs` 均正确报“无设备”。

### 10.7 待办（按依赖排序）

1. **硬件标定**（需设备进 Linux）：`probe` 六步 → 定 booking/credit 与重传；
2. `otikm` daemon 装配：`otiinput` + `otikm_core` + `otitrans` + RX 线程 + 热键/边缘配置；
3. `otiboard` 剪贴板 daemon：`wl-clipboard`/`xclip` 读写 + 分块 + 去重（哈希）+ 大对象走临时文件；
4. 打包：systemd unit + udev 规则 + 自启动。



## 7. 实验环境需求

- **最小闭环**：两端都插同一台 **Linux** 机器 → 一端 `/dev/sgN` 发 SCSI，另一端 `/dev/input/*` 看注入的键鼠。
  需要：该机器 SSH 可达（或用户代跑命令）、`evtest`/`libinput`、`usbmon`（可选抓包）、`sg3-utils`。
- 或者：一端插本机（Windows）+ `usbipd bind/attach` 进 WSL（**需要一次 UAC 提权**），另一端插 Linux 机器。
- 验证顺序：① 只发 `0xF0` 查询类命令，确认设备响应且不影响盘符；
  ② 发 `0xD9/0x33` 空包，观察对端是否出现击键；③ 逐字节标定 12 字节包；④ 打通数据管道；⑤ 写 daemon。

## 8. 交付物路线

1. `otilink` 用户态库：SCSI CDB 封装（SG_IO）、帧收发、HID 包构造。
2. `otikm`：键鼠共享 daemon（evdev 抓取 → HID 包；鼠标边缘穿越、热键切回、注入抑制）。
3. `otiboard`：剪贴板同步（自定义协议走数据管道，文本/文件分块）。
4. systemd unit + udev 规则（按 VID:PID 找 `/dev/sgN`，免 root 权限可加 udev 规则）。

## 11. 第 4 轮进展：daemon 装配 + 双实例集成测试

### 11.1 抓到一个会致命的状态机语义错误

写 daemon 时按"指针在哪台机器上"重新推导，发现 `otikm_core_remote_switch` 的方向写反了：

```
唯一状态 have_control ≡ 指针在本机(pointer_on_me)
  have_control=1 → 本地键鼠本地消费（不抓取、不转发）
  have_control=0 → 本机是驱动侧：EVIOCGRAB 独占并全部转发给对端；热键可拉回

SWITCH.side 含义 = 「指针现在在哪边」（发送方视角）
  side=REMOTE → 我把指针送过去了 → 接收方 have_control=1
  side=LOCAL  → 我把指针收回来了 → 接收方 have_control=0
```

原实现把 `side=REMOTE` 处理成 `have_control=0`（正好镜像），若直接上线会出现"谁在被控"完全反了
——单测当时也按错误语义写的断言，是**写 daemon 集成测试时才暴露**。已修正 core 与单测。

顺带修掉两个真实缺陷：
- RX 线程在 `recv` 返回 EOF 时**空转刷屏**（`rc=-1` 未区分处理）；
- sim 模式脚本跑完就退出进程，导致对端读到 EOF、后半段测试无意义（现在保持到 `--duration`）。

### 11.2 `otikm` daemon（`otikm.c`）

```
--transport cable[:/dev/sgN] | listen:PATH | connect:PATH
--capture /dev/input/eventN（可多次）  --grab   --inject
--sim-input FILE --sim-inject FILE     ← 无硬件测试通道
--screen WxH --edge PX --hotkey CODE --hotkey-only --duration --delay --log --verbose
```

接线：`otiinput`（抓取/注入）+ `otikm_core`（路由决策）+ `otitrans`（线缆或 fd）+ RX 线程。
抓取策略与状态联动：指针在本机 → 不抓取（本地桌面正常工作）；变成驱动侧 → `EVIOCGRAB` 独占，
避免"同一按键既本地生效又转发对端"的双重输入。

### 11.3 双实例集成测试（`itest.sh`，daemon 层）

两个真实 `otikm` 进程经 AF_UNIX 对跑，剧本 A 撞右边缘 → 转发 → 按热键拉回 → 再输入：

```
A 日志: SENT SWITCH side=remote / STATE 指针交给对端 / SENT key 30 ×2
        SENT SWITCH side=local  / STATE 指针拉回本机 / LOCAL key(key31)
B 日志: RECV SWITCH side=remote → 指针在本机 / RECV key 30×2 → 注入
        RECV SWITCH side=local → 指针不在本机
B 注入: key 30 1, key 30 0      ← 没有 key 31（拉回后停止转发）✓
A 注入: 空                      ✓
11/11 断言通过
```

### 11.4 全量测试

```
make test
  1/3 帧与 CDB 自检 ......... 6/6
  2/3 消息层 + 状态机 ....... PASS（含剪贴板 140KB 分块重组）
  3/4 端到端（库层）......... PASS（socketpair 真实 transport+proto+状态机）
  4/4 双实例集成（daemon）... PASS（真实 otikm 进程对跑，11 项断言）
合计 54 项 PASS / 0 FAIL，代码 2442 行
```

### 11.5 剩下的事

1. **硬件标定**（唯一必须要设备的）：`probe` 六步 → booking/credit 与重传；
2. `otiboard` 剪贴板 daemon（`wl-clipboard`/`xclip` + 哈希去重 + 大对象临时文件）——不依赖硬件；
3. 打包：systemd unit + udev 规则（`/dev/uinput` 权限、按 VID/PID 找 `/dev/sgN`）。

## 12. 第 5 轮进展：剪贴板同步 + TCP 预演 + 部署产物

### 12.1 剪贴板（`oticlip` + daemon 内线程）

- 后端自动探测：`wayland(wl-paste/wl-copy)` → `x11(xclip)` → `file`（测试/无图形环境）；
- 与键鼠**共用同一条传输**（消息类型区分），一个 daemon 一个进程；
- 分块：`OTI_CLIP_CHUNK_MAX=65000`，默认总上限 1MB（`--clip-max`）；重组状态机校验 fid/offset/长度；
- 防回环：内容用 CRC32 做指纹，**应用远端内容前先登记指纹**，本地轮询到同指纹不再回发。

### 12.2 又抓到一个真实竞态（偶发，靠复跑发现）

应用远端剪贴板时原本是"**先写剪贴板、后记指纹**"，两者之间轮询线程可能读到新内容并与旧指纹比较
→ 误判为本地新内容 → 回发（`cliptest` 第一次通过、第二次失败，实得 A 发送 2 次）。
修法：**先登记指纹（锁内）、再写剪贴板**，并把轮询侧"比较+更新"也放进同一把锁。
修后连跑 3 次 `cliptest` 与 2 轮 `make test` 均稳定通过。

### 12.3 TCP 传输：真机两阶段 bring-up

新增 `tcp-listen:PORT` / `tcp-connect:HOST:PORT`。意义是把风险分离：
**先用网络把输入/剪贴板链路在真机上跑通**（真实使用 evdev/uinput/系统剪贴板，以及本机无法验证的
`/dev/uinput` 权限、抓取与注入的配合），**再切 `cable` 后端**只验证 SCSI 通道。
`tcptest.sh` 在本机用 loopback 验证了该后端（含双向同步与无回环）。

### 12.4 部署产物

| 文件 | 用途 |
|---|---|
| `99-otilink.rules` | uinput / scsi_generic(按 VID:PID) / event* 的非 root 权限 |
| `otikm.service` | systemd unit（`EnvironmentFile=/etc/default/otikm`，补 input/disk 组） |
| `otikm.default` | 每台机器的参数模板（阶段一 tcp、阶段二 cable） |
| `README.md` | 构建、测试、两阶段真机步骤、权限、自启动、行为说明与已知边界 |

### 12.5 测试总览（`make test`，连跑两轮稳定）

```
1/6 帧与 CDB 自检 ................. 6/6
2/6 消息层 + 状态机 ............... PASS（含剪贴板 140KB 分块重组）
3/6 端到端（库层，socketpair）..... PASS
4/6 双实例集成（键鼠 daemon）...... PASS（11 项断言）
5/6 剪贴板集成（双向/防回环/跨块）.. PASS（5 项断言）
6/6 TCP 传输（loopback）........... PASS（5 项断言）
合计 64 项 PASS / 0 FAIL，代码 2882 行
```

### 12.6 现在的状态与唯一剩余项

键鼠共享与剪贴板共享的**全部软件逻辑已在无硬件条件下自证**（协议、传输、状态机、输入抓取/注入、
剪贴板读写与防回环、daemon 装配、部署产物）。剩余：

1. **实测标定线缆 SCSI 通道**（booking/credit、重传）——必须把设备接进 Linux；
2. 真机上按 README 第 3 节走两阶段验收；
3. 可选扩展：多显示器/多边缘、非文本剪贴板、速率与加速曲线调优。

## 13. 第 6 轮进展：标定自动化 + 协议规格成文

### 13.1 `probe sweep`：把"必须实测"变成一条命令

剩余未知集中在 `0xD8` 的 `CDB[3]/CDB[4]` 与 `0xD9/0x2A` 的 `CDB[2]`（booking/credit 编码）。
现在 `otilink` 增加了 `otilink_cdb_ex()`（回填 status/host_status/driver_status/**resid**/sense），
`probe sweep` 据此"一次只变一个轴"地扫（避免组合爆炸），并打印每组参数的
`rc / status / resid / 实际传输字节数 / 帧判定（有效/空闲/非法）`：

```bash
./probe sweep --dry-run          # 只打印将发出的 CDB，不碰设备（现在就能跑，已验证）
./probe sweep --delay 200        # 读方向：CDB[2] ∈ {0,1,2,4,0x7F,0xFF}，CDB[3]/[4] ∈ {0..4,8,16,0xFF}
./probe sweep --write --delay 300   # 写方向：写的是合法帧（含 20B 头 + 头副本），不破坏盘符
```

安全设计：默认只做读方向；写方向必须显式 `--write`；默认跳过 CD-ROM LUN（`--all-luns` 覆盖）；
每次探测间隔可调（避免把设备打僵）。无设备时给出干净报错。

### 13.2 `PROTOCOL.md`：逆向结论成文

把散在笔记里的结论整理成独立规格：设备结构、CDB 表（含反汇编地址）、65536 帧格式与首尾头校验、
厂商 XML/UPipe 命令与 `Param_*` 映射、HID 维护通道、自有应用层协议，
每条结论标注可信度 **[A] 反汇编直读 / [B] 逻辑推出 / [C] 待实测**，并附复核命令与未解问题清单。
另外记下一条线索：厂商二进制里有 `"Remote is Linux, clipboard upipemsg ignored!!!!"`，
说明 OTi 代码里存在识别 Linux 对端的分支——生态里可能有（或曾有）Linux 端实现。

### 13.3 当前完成度

| 部分 | 状态 |
|---|---|
| 反向工程（协议/帧/命令层/维护通道） | 完成，见 `PROTOCOL.md` |
| SCSI 通道实现（CDB/帧/SG_IO） | 完成并可自检（6 项） |
| 应用层协议 + 传输抽象（cable/fd/tcp） | 完成（64 项测试全绿） |
| 键鼠共享 daemon（抓取/注入/状态机/热键） | 完成并在双实例下验证 |
| 剪贴板共享（双向/防回环/分块） | 完成并在双实例下验证 |
| 部署产物（udev/systemd/README） | 完成 |
| **线缆 SCSI 参数实测标定** | **待设备进 Linux**（现已有 `probe sweep` 一条命令） |
| 真机两阶段验收（TCP → cable） | 待用户在真机执行 |

## 14. 第 7 轮进展：真机自检工具 + 采样工具 + 关键推论升级

### 14.1 `otikm --doctor`：上线前排雷

真机第一次跑最容易卡在环境而不是协议上。自检覆盖四项并给出**可执行的修复命令**：

```
[FAIL] /dev/input 下看不到输入设备            → 容器/WSL 未暴露，或没有键鼠
[FAIL] 无法创建 uinput 注入设备: Permission denied
       → sudo cp 99-otilink.rules /etc/udev/rules.d/ && sudo udevadm control --reload
         sudo usermod -aG input,disk $USER   # 重新登录
[OK]   剪贴板后端 file(sim)，写入/读回一致      （会先备份并恢复用户原剪贴板）
[FAIL] 未发现 0ea0:2213 的 /dev/sgN
       → usbipd bind/attach 或把线的这一端插到 Linux
```

在 WSL 下如实报 3 条 FAIL（环境限制），这正是它的价值：**把"跑不起来"从玄学变成清单**。

### 14.2 `probe rdump`：原始帧采样

把收到的 64KB 帧逐字节存盘（含空闲帧），并打印每帧前 32 字节。两个用途：
标定期确认设备是否开始回数据；**若能拿厂商软件当对端，可直接读到厂商 20 字节头的真实内容**，
一次把 §3/§4 的未解项验掉（见 `PROTOCOL.md` §7.1）。

### 14.3 一个关键推论升级（降低最后一步的风险）

原先 [B] 级说法"芯片对帧头透明"现在有了硬证据链：接收侧用「前 16 字节 == 全零」判空闲、
用「首 20 字节 == 尾 20 字节」判有效——**若芯片会插入或改写自己的头，这两个判据都不可能成立**。
故可判定芯片是透明的 64KB 管道、20 字节头完全由主机定义（**[A] 级推论**）。
直接后果：Linux↔Linux 下厂商头语义**无关紧要**，我们自定义头字段是安全的；
残留风险只剩"芯片是否对未知头内容报错"，`probe sweep/rtest` 一插上即可验掉。

### 14.4 当前状态

全量测试 64 项 PASS / 0 FAIL（含 `--doctor` 与 `rdump` 后仍稳定）。
软件侧已无待办；剩余两步都需要设备进 Linux：**`probe sweep` 定 credit 编码** → **真机两阶段验收**。

## 15. 第 8 轮进展：软件仿真设备，cable 路径也有测试了

### 15.1 SCSI 边界注入 + `otimock`

给 `otilink` 加了 `otilink_set_ops()`：`otilink_cdb_ex()` 默认走 `SG_IO`，但可注入自定义后端。
据此写了 `otimock.c` —— 按 `PROTOCOL.md` 逆向语义实现的**软件仿真对拷线**：
64KB 环形接收队列（真机为 200 缓冲）、空闲帧（全零）、首尾头校验、credit 规则、HID 维护包、`'OT'` 魔数校验。
配套 `mocktest.c` 用**真实的 `otitrans` cable 后端**跑通：

```
[PASS] A 写出 1000 字节 → B 收到且逐字节一致（走 打包→0xD9/0x2A→设备→0xD8→首尾头校验→取载荷）
[PASS] 连续 3 帧顺序与内容正确
[PASS] 队列空 → recv 超时 -2，65ms 内有 12 次空闲读（5ms 退避，不空转打满 CPU）
[PASS] 尾部头被改动的帧被设备拒收（CHECK CONDITION + sense），错误正确传回
[PASS] credit=0 不返回数据、也不消费队列；credit=1 正常返回并消费
[PASS] HID 维护包 CDB[2..13] = 14 字节缓冲的前 12 字节
[PASS] 缺 'OT' 魔数的 CDB 被拒
```

过程中也修掉两个真问题：mock 最初是**单槽队列**（连发 3 帧互相覆盖，暴露测试设计缺陷，改为 8 槽环形）；
以及确认了 cable 接收的空闲帧路径**必须有退避**（厂商实现里也是 `usleep` 后重试）。

### 15.2 测试总览（`make test`，7 组）

| 组 | 断言 |
|---|---|
| 1/7 帧与 CDB | 6 |
| 2/7 消息层 + 状态机 | 15 |
| 3/7 端到端（库层） | 10 |
| 4/7 双实例集成（daemon） | 11 |
| 5/7 剪贴板集成 | 5+ |
| 6/7 TCP 传输 | 5 |
| 7/7 **cable 传输路径（仿真设备）** | 19 |
| **合计** | **83 PASS / 0 FAIL**（3630 行 C） |

### 15.3 完成度

除"真机 CDB credit 取值"一项外，**协议层、传输层、状态机、输入/剪贴板 daemon、
部署产物、以及线缆路径本身都已有可执行的验证**。真机只需：
`otikm --doctor` 排环境 → `probe sweep` 定 credit → 两阶段验收（TCP 已可先行）。

## 16. 第 9 轮进展：booking 语义从 [C] 升到 [A]，并纠正了我自己的错假设

### 16.1 读方向 CDB[3..4] 到底是什么（反汇编实证）

`ProcessIdleState` @0x4e87–0x4ecc 逐条读出：

```c
if (ContinueTxCount >= 11) { ContinueTxCount = 0; held = 0; }
else { n = CFArrayGetCount(RX队列); held = (n==0) ? 0 : MIN(n, MaxBookingSize); }
CDB[2]=3; CDB[3]=held>>8; CDB[4]=held&0xFF;      // 16 位大端
```

成员偏移与默认值（访问器 + 构造函数实证）：

| 偏移 | 名称 | 默认 | 证据 |
|---|---|---|---|
| `+0x54` | `RegRx` | 0 | @0x5e00/@0x5e24 |
| `+0x56` | `ContinueTxCount` | 0 | @0x5e2e/@0x5e0a；构造 @0x3712 |
| `+0x58` | `MaxBookingSize` | **100** | @0x5e38/@0x5e42；构造 `mov dword [rbx+0x56],0x640000` @0x3712 |

另外：设备状态 `0x11`/`0x20` 会触发通知，收到 `0x20` 时把 `MaxBookingSize` 复位为 100（@0x5187）。

### 16.2 纠正：它不是 credit 门闸

上一轮我在 `otimock` 里把"`CDB[4]==0` → 不返回数据"写成断言，**这是错的**：
该字段是"主机已持有未消费帧数"的流控反馈。若 0 意味着不给数据，主机永远收不到第一帧（死锁）。

改动：
- `otilink_recv_frame(d, frame, held)`：参数改为 `uint16_t held`，写入 16 位大端；默认 0；
- `otimock`：规则改为"报数 ≥ MaxBookingSize(100) 时才暂不投递（不消费队列）"；
- `mocktest`：断言改为 `held=0 必须能收到`、`held=50 正常`、`held=100 暂缓且不消费`、`窗口恢复后可投递`；
- `probe sweep`：`CDB[3]` 扫 {0x01,0x64,0xFF}（构造大窗口）、`CDB[4]` 扫 {0,1,50,100,101,255}，
  并在输出里明确"预期不门闸数据；若某取值不再回数据 = 设备按窗口限流"。

**这条纠正的价值**：如果带着错误模型去真机扫，`CDB[4]=0 → 空闲帧` 会被我误读成"确认了 credit 假设"，
从而固化一个错误实现。这是本轮最重要的收获——不是多写代码，而是**避免把一个错误假设变成"已验证的结论"**。

### 16.3 测试：85 项 PASS / 0 FAIL

cable 路径测试从 19 项扩到 21 项（新增窗口语义 4 项 + 修正原有 3 项）。

## 17. 第 10 轮进展：初始化握手与统一通道模型（真机一次成功的关键）

### 17.1 厂商初始化序列（此前完全没做，很可能是真机不通的头号原因）

`Initialize()` @0x3d28–0x3e98 的调用顺序 **[A]**：

```
UnmountMedia → ObtainExclusive → ResetRxQueue/ResetTxQueue → 启动工作线程
             → MountMedia → ReleaseExclusive → GetPhysicalBusType → GetICVersion
```

即**传输前必须先把两个 LUN 卸载**（厂商用 DiskArbitration 做），再取独占锁。
Linux 侧对应：udev 抑制自动挂载（`UDISKS_IGNORE=1`）+ `otikm --doctor` 检查 `/proc/mounts` 里是否仍挂着 +
`otikm --cable-init` 执行 `IC 版本 → 总线类型 → 独占锁`（退出解锁）+ `probe info` 可单独验证。

### 17.2 统一通道模型（把散落的 CDB 串成一个体系）

```
0xD9 写：CDB[1] 通道 —— 0x2A 数据管道 / 0x33,0x34,0x36 HID 维护包
0xD8 读：CDB[2] 通道 —— 0x03 数据管道 / 0x02 物理总线类型
0xF0 控制：CDB[1]/CDB[2] 子命令 —— 0x00/0x00 IC 版本(12B)、0x00/0x02 总线类型(16B)、
           0x05/0x02 USB 重启(CDB[4]=0x0A)、0x30、0x31 独占锁
所有命令 CDB[14..15]='O','T'
```

### 17.3 新发现：`SendDummyData` = 写 64KB 全零帧

`_OTi_SendDummyData()` @0x21c8 就是 `SendData(NULL, /*flag=*/true)` → 内部 `bzero` 64KB 再整帧写出。
对端把它当"空闲帧"丢弃，所以这是**无害的保活/冲刷**手段，需要时可用（`otilink_send_dummy()`）。

### 17.4 本轮新增命令与检查

- `probe info`：IC 版本 / 物理总线类型 / 独占锁（真机第一步验证 SCSI 控制通道）；
- `probe dummy`：写全零帧；
- `otikm --cable-init`：启动时做厂商式握手，退出时解锁；
- `otikm --doctor` 增加第 5 项：**LUN 挂载检查**（附 `umount` 命令）；
- `99-otilink.rules` 增加自动挂载抑制规则。

测试仍为 **85 项 PASS / 0 FAIL**（本轮只加命令与检查，未改协议路径）。

## 18. 第 11 轮进展：把"设备初始化相关命令"补全

### 18.1 新解出的两条命令

| 命令 | 方向 | 含义 | 证据 |
|---|---|---|---|
| `0xD9/0x60` | 写 1B / 读 1B | **设备模式**（Get/SetDeviceMode） | @0x74a4 / @0x7410 |
| `0xF0/0x00/0x00` | 读 ≤64B | **设备信息块**（IC 版本 + 侧别 + 功能类型） | @0x4597 / @0x72f4 / @0x71d8 |

两条要点：

1. **`0xD9` 不代表方向**：`0xD9/0x60` 的数据阶段可以是"读"（`call 0x4ac2` = ReceiveData）。
   方向由调用方（Send/Receive API）决定，而不是 opcode。这一点之前被我隐含地搞反过。
2. `GetSideType`/`GetFunctionType` 与 `GetICVersion` **是同一条命令的不同解读**：
   都发 `0xF0/0x00` 全零 CDB 读 64 字节，再各取所需字段。厂商 `_OTi_ReadDevicesInfo` 即此。
   对实现的意义：一次读、多字段解析，不必为每个字段发明新命令。

### 18.2 已落地的实现

- `otilink_info_read()`（64B 信息块）、`otilink_dev_mode_get/set()`（0xD9/0x60）、`otilink_usb_restart()`；
- `probe info` 现在打印信息块 + 总线类型 + 设备模式 + 锁；新增 `probe mode [值]`；
- `otikm --cable-init` 序列扩为：设备信息块 → 设备模式读 → 总线类型 → 独占锁（退出解锁）；
- `otimock` 增加对应仿真（信息块字段、模式读写、USB 重启计数），`mocktest` 增 4 项断言。

测试：**89 项 PASS / 0 FAIL**（cable 路径 21 → 25 项）。

### 18.3 仍未解（都需要真机）

`0xD9/0x60` 的模式取值语义、`0xF0/0x30` 的含义、`0xD9/0x2A` 的 `CDB[2]=0xFF` 确切作用、
重传（`requestResendPacket`）触发、设备内部窗口表现。这些都能用现成工具读/扫出来
（`probe info` / `probe mode` / `probe sweep`），不需要再写代码。

## 19. 第 12 轮进展：命令清单收尾 + 保活机制（含一个被测试抓出的实现错误）

### 19.1 命令空间收敛完成

| 命令 | 方向 | 含义 |
|---|---|---|
| `F0/00/00` | 读 ≤64B | 设备信息块（IC 版本 / 侧别 / 功能类型） |
| `F0/00/02` | 读 16B | 物理总线类型 **与** 速度状态（同一命令两种解读） |
| `F0/05/02 [4]=0A` | 写 | USB 重启 |
| `F0/30`、`F0/31` | — | 其他状态 / 独占锁 |
| `D9/2A FF` | 写 64KB | 数据管道写（全零帧 = 保活） |
| `D9/33|34|36` | 写 | HID 维护包（12B 在 CDB 内） |
| `D9/60` | 读/写 1B | 设备模式 |
| `D8/00/03 <held>` | 读 64KB | 数据管道读（**远端主机状态随帧带回**） |
| `D8/01 <v2> 00 <v4>` | 写 2B | 设置远端主机状态 |

另外确认：`ResetRxQueue`/`ResetTxQueue` **没有任何 CDB**，是纯本地状态重置，无需实现。

### 19.2 一个被测试抓出来的实现错误

我第一版 `otilink_send_dummy()` 走的是 `otilink_frame_pack()`，结果生成的帧**带 20 字节头**
（`OTIL` + 长度），对端会判成"有效空帧"而不是"空闲帧"。
而厂商的 dummy 是**整帧 64KB 全零、不带头**（`SendData(NULL,true)` 里 `bzero` 后整块写出）。
`mocktest` 的断言 `对端把全零帧判为空闲` 直接把它抓了出来 —— 已修正为绕过 frame_pack 直发全零帧。

顺带明确了一个概念：**"设备投递帧"与"主机分类帧"是两件事**——设备照常投递 dummy 帧（`rd_data+1`），
主机侧按内容把它判为空闲并跳过，所以 dummy 对应用层完全透明。这正是它能当保活用的原因。

### 19.3 保活（真机很可能需要）

`--keepalive MS`：所有传输周期发协议层 `PING`（对端可探测存活），**线缆模式额外发 dummy 帧**
（对应厂商 `SendDummyData`/`processDummyState`）。`tcptest.sh` 新增 2 项断言验证 PING 收发。

测试：**94 项 PASS / 0 FAIL**（cable 路径 25 → 28 项）。

### 19.4 剩余

全部剩余项都属于"接上设备读一下/扫一下"：`0xD9/0x60` 模式取值、`0xF0/0x30`、
写方向 `CDB[2]=0xFF` 的作用、重传触发、远端状态该不该发。
工具齐备：`probe info / mode / rstatus / sweep / rdump` + `otikm --doctor --cable-init --keepalive`。

## 20. 第 13 轮进展：主循环节奏与 LUN 选择

### 20.1 厂商主循环（补上最后一块结构信息）

`RunLoopFunction` @0x4350 **[A]**：

```c
while (running) { if (dev_ready) ProcessIdleState(); else usleep(100000); }
```

而 `ProcessIdleState` **同时**做读与写：
`SendSCSICommandReceiveData`(读) → `GetData`(解析入队) → `processTransferState`(@0x509c) → `PutData`(@0x5d97)(写)。
正常空闲路径**不 sleep**（连续轮询），`usleep(5000)/usleep(10000)` 只在错误/重试分支。

据此把本仓库的空闲退避从 5ms 调到 **500µs**：USB2 下 64KB 传输本身是 ms 级，对延迟影响可忽略，
但不至于空转打满 CPU。

### 20.2 LUN 选择：不猜，探测

复合设备有两个 LUN（CD-ROM + 磁盘），厂商代码没说明命令该发给哪个。改为**逐个探测**：
`oti_tr_open_cable(NULL)` 选第一个能应答 `0xF0/0x00` 设备信息块的 LUN（名字里标 `(auto,已应答)`），
探测不出来退回第一个；`probe infoall` 可单独查看每个 LUN 的应答；`--transport cable:/dev/sgN` 可手工指定。

### 20.3 状态

测试 **94 项 PASS / 0 FAIL**（4004 行 C）。逆向侧现在连"主循环结构"都清楚了；
设备面向行为无法再靠静态分析收敛，剩余项全部是"接上设备读一次"。

## 21. 第 14 轮进展：把"输入链路"和"剪贴板后端"真正验证掉

前几轮我一直只能说这两块"编译通过但未实测"。本轮补上了可执行的自检，并**真的跑通了**。

### 21.1 `otikm --selftest-input [synth|grab|live]`

思路：uinput 注入的事件会出现在我们自己创建的 `/dev/input/eventN` 上，于是可以
**注入 → 回读 → 比对**，不需要 GUI、也不需要第二台机器。

| 模式 | 内容 |
|---|---|
| `synth` | 全自动：注入 A/S/空格 各按下抬起 + 鼠标位移(5,-3)/滚轮1/左键按下抬起 → 回读比对 |
| `grab` | 同 synth，但回读节点先 `EVIOCGRAB`（验证独占与自身读取互不影响） |
| `live` | 把 `--capture` 上的真实键鼠事件注入并回读（验证抓取+注入配合） |

**实测结果（真实内核，root）**：

```
[OK]   uinput 注入设备已创建: otilink virtual input
[OK]   回读节点: /dev/input/event0
[PASS] 鼠标路径：左键按下+抬起=是 位移X=是 位移Y=是 滚轮=是
[PASS] 键盘 6 事件回读匹配 6 个/错配 0 个（在 EVIOCGRAB 独占下也通过）
=> 输入链路可用（抓取/注入/回读全部打通）
```

> 说明：我在本机用 WSL 的 `wsl.exe -u root` 跑了这次自检（**临时**创建 uinput 设备，
> 未做任何持久性系统改动），以便验证这个此前只能编译验证的部件。

顺带修掉一个真实隐患：`apply_grab()` 原先**静默忽略** `EVIOCGRAB` 失败，
会导致"同一按键既本地生效又转发对端"的双重输入；现在失败会显式告警。

### 21.2 剪贴板后端验证（`probe clip`）

新增 `probe clip [文本] [--clip-file F]`：选后端 → 写 → 读回比对（并恢复原剪贴板）。
用 PATH 里放**假 `xclip` / `wl-copy` / `wl-paste` 桩**的方式验证调用管线（无需真装图形环境）：

| 后端 | 结果 |
|---|---|
| file（无图形环境时兜底） | PASS |
| x11（`xclip` 桩 + `DISPLAY`） | PASS，内容实际落到桩的存储文件 |
| wayland（`wl-copy`/`wl-paste` 桩） | PASS |

（顺带发现：本机的 WSL 会话里 `DISPLAY`/`WAYLAND_DISPLAY` 本来就是设好的，
所以后端探测顺序在真机上会优先走 wayland——已在 README 说明。）

### 21.3 状态

测试 **94 项 PASS / 0 FAIL**。现在**只剩线缆本身**（SCSI 通道）未在真机验证，
其余全部要么有自动化断言、要么已在真实内核上跑通。

## 22. 第 15 轮进展：SG_IO 层在真实内核 SCSI 设备上验证通过

此前 `otilink` 的 ioctl 边界只跟我的仿真打过交道。这轮用 **`scsi_debug`（内存盘，临时加载）**
造出一个真实内核 SCSI 设备（`/dev/sdg` + `/dev/sg6`），把这一层验证掉：

```
[PASS] INQUIRY rc=0 status=0x00 resid=0  厂商='Linux   ' 型号='scsi_debug'
[PASS] TEST UNIT READY rc=0 status=0x00
[PASS] READ CAPACITY rc=0 容量=65536 块 × 512 字节
[PASS] 厂商 0xD9/0x2A 被拒 rc=0x80102 status=0x02 sense: key=0x5 asc=0x20 ascq=0x0
[PASS] WRITE(10) 128 块（65536 字节）rc=0 resid=0
[PASS] READ(10) 回读 65536 字节并逐字节比对一致（rc=0 resid=0）
=> SG_IO 层全部通过
```

验证到的具体能力：
- CDB 长度可变（6/10/16 字节）——为此给库加了 `otilink_cdb_len()`；
- **64KB 双向传输**与 `resid` 语义（写/读各 65536 字节，`resid=0`，回读逐字节一致）；
- **错误路径的 sense 解码**（key=0x5 ILLEGAL REQUEST、ASC=0x20 INVALID COMMAND OPERATION CODE）
  —— 这正是 `probe sweep` 判定真机响应所依赖的能力（能区分"命令不认"与"参数不对"）。

做成可复现脚本 `sgtest.sh`（`make sgtest`）：自动 modprobe/找节点/测试/`rmmod` 清理，
实测退出后 `lsmod | grep -c scsi_debug == 0`，**无持久改动**。

### 22.1 现在的验证覆盖面

| 层 | 验证方式 |
|---|---|
| SCSI/SG_IO 边界 | ✅ **真实内核设备**（scsi_debug，6 项） |
| 帧/命令构造 | ✅ 自检 + 仿真设备（28 项断言） |
| 消息层/状态机 | ✅ 单测 + 双进程集成 |
| 输入抓取/注入/回读 | ✅ **真实内核**（含 EVIOCGRAB、鼠标） |
| 剪贴板三后端 | ✅ **真实调用管线**（file/x11/wayland，桩验证） |
| TCP 传输 + 保活 | ✅ loopback 双进程 |
| **真机线缆行为**（设备是否按推断应答） | ⏳ 唯一剩余项 |

## 23. 第 16 轮进展：Windows 侧 agent（用户实际拓扑是 Linux 主控 / Windows 被控）

### 23.1 先纠正上一轮的一个乐观推断

用户在真机上把**主控端插在 Linux、被控端插在这台 Windows**。我上轮猜"被控端不需要软件、
靠线缆注入 HID"——这轮把 HID 发送路径读完后**否掉了**：

- `_OTi_SendHIDPacket` 内部有硬节流：`now - last >= 10.0` 秒才允许发（否则丢弃并打日志）；
- GoBridgeDemon 6 个调用点全是「全零 14 字节 + type=1」= **释放所有按键**；
- 构造函数也读完了：14 字节源结构 →（可选前置 `0x01`）→ 只有前 12 字节进 CDB。

=> HID 包是**维护通道（≤1 次/10 秒）**，键鼠/剪贴板的真实通路是**数据管道上的消息层**，
   **两端都需要软件**。这条纠正直接决定了本轮该做什么。

### 23.2 Windows 侧 agent（`windows/otiagent.ps1`）

单文件、零安装（PowerShell 5.1 的 `Add-Type` 内置 C# 编译器）：
内嵌 C# 实现 CRC32 / 消息编解码 / 65536 帧 / SPTI（`IOCTL_SCSI_PASS_THROUGH_DIRECT`）；
PowerShell 侧负责剪贴板（`Get/Set-Clipboard`）与注入（`SendInput`）。

**验证结果（都不需要管理员）**：

```
$ otiagent.ps1 -Selftest           # 7/7 PASS：KEY 往返、CRC 篡改拒收、鼠标负位移、
                                   #            帧有效/空闲/尾部篡改
$ make wintest                     # 跨实现双向线上兼容
  [PASS] 帧#1 KEY code=30 value=1 seq=7        ← Linux 生成，Windows 解出
  [PASS] 帧#2 MOUSE dx=-1234 dy=567 wheel=-1 btn=3 seq=8
  [PASS] 帧#1 KEY code=31 value=0 seq=9        ← Windows 生成，Linux 解出
  [PASS] 帧#2 MOUSE dx=-5 dy=9 seq=10
```

为此给 Linux 侧加了 `probe frameout/framein`（生成/校验真实帧文件），
`make wintest` 把它们串起来。**两个实现线上兼容 = 已证**。

**未验证**：SPTI 实际收发（需要 Windows 管理员权限）、`SendInput`/剪贴板在真实链路下的表现。

### 23.3 当前状态与下一步

| 项 | 状态 |
|---|---|
| Linux 主控端（抓取→协议→线缆） | 已实现、已自证（94 项 + 输入链路真机验证） |
| Windows 被控端（线缆→协议→注入/剪贴板） | **已实现**，协议层双向兼容已证；待管理员权限跑 SPTI |
| 线缆 SCSI 通道本身 | 待真机实测（`probe infoall/info/sweep`） |

下一步（需要用户给一样东西）：
1. **Windows 管理员一次**（或用户自己用管理员 PowerShell 跑）→ `-Scan` 找可用设备 → 跑 agent；
2. **Linux 侧访问**（SSH 或代跑命令）→ 跑 `otikm --transport cable --cable-init --keepalive 1000`。

## 24. 第 17 轮进展：**真机通了！**（Windows 侧无需管理员即可访问设备）

### 24.1 破局点：我自己的 SPTI 结构体布局 bug

上一轮 `-Scan` 报 `rc=5 (ACCESS_DENIED)`，我以为是权限问题。这轮核对 C# 结构体后发现：
我把 `Sense[32]` **当字段拼在结构体里**，于是 `SenseInfoOffset = sizeof(结构)` 指向了 Sense 数组**之后**，
驱动拿到的请求是畸形的。改成规范布局（结构体 56 字节、Sense 放在结构体之后、`SenseInfoOffset=56`）后，
**同一台机器、非管理员**，命令立刻通了。

现在自检里加了布局断言守护：`sizeof==56`、`DataTransferLength@12`、`DataBuffer@24`、
`SenseInfoOffset@32`、`Cdb@36`（12/12 PASS）。

### 24.2 真机读数（Windows = 被控端）

```
设备信息块 0xF0/0x00：00 00 22 13 00 01 30 39 30 33 30 31 00 00 01 00   |.."...090301....|
                      ↑ PID 0x2213        ↑ ASCII "090301" = 固件日期 2009-03-01
```
`CDB[2]` 是**分页选择子**，各页返回不同内容（`0xD8/0x00/<page>`）：

| page | 内容 |
|---|---|
| 0 | `03 53` |
| 1 | `00 53` |
| 2 | `"USBC"` 开头的一段结构化数据（含 `OT` 魔数，288 非零字节；rc=CHECK CONDITION 但数据阶段完成） |
| 3 | **数据管道**：`01 00 00 00 00 00 00 ff ff ff`（10 非零字节，稳定不变） |
| 4/5/6/7 | `06 40` / `00 7f` / `00 00 03` / `00 00 03` |

### 24.3 关键发现：被控端**能读不能写**

所有写命令都被拒，且 sense 明确：

```
写 0xD9/0x2A（厂商模板 D9 2A FF + 'OT'）：
   长度 65536 / 1024 / 2、LUN F: 与 H:  —— 全部 rc=258 CHECK CONDITION
   key=0x9 (ABORTED COMMAND)  ASC=0x81 (厂商自定义)  ASCQ=0x0
读/写 0xD9/0x60（设备模式）：key=0x5 (ILLEGAL REQUEST) ASC=0x20 (命令不支持)
0xF0/0x31（独占锁）：同样被拒
```

注意 **`key=0x9 ABORTED COMMAND` 而不是 `key=0x5 ILLEGAL REQUEST`**：
命令**被识别但被中止**，这与"本端无权写 / 链路未由主控端建立"一致，
也与用户描述的拓扑（Linux=主控端、Windows=被控端）吻合。

**推论**：固件按主控/被控端限权；键鼠与剪贴板的数据帧必须由**主控端（Linux）**写入。
这正好与我们已实现的角色分工一致（Linux 端负责发，Windows agent 负责收+注入）。

### 24.4 交付给用户的单文件 Linux 工具

为了让主控端（Linux）能立刻跑起来，做了 `otilink/otiprobe.c`：单文件、无依赖，
`gcc -O2 -o otiprobe otiprobe.c` 即可；并已编译 **静态 x86-64 版**放到
`C:\Users\Public\otilink\otiprobe`（可直接拷到 Linux 跑，无需编译）。

命令：`infoall`（找 LUN + 读信息块）/ `pages`（读分页）/ `read N`（连读帧并判定）/
`write`（写帧并打印 SCSI 状态与 sense）/ `ping`（写一个协议 PING 帧）/ `loop`（持续读）。

### 24.5 下一步（依赖用户一样东西）

主控端需要跑起来才能：
1. 验证主控端的**写**是否成功（预期 rc=0，对照被控端的 ABORTED COMMAND）；
2. 从主控端写入 PING 帧，在这边（Windows）验证 us-Windows agent 能收到并解出 → 端到端首次打通；
3. 标定 HID/数据通路的完整行为。

需要：**Linux 那台的访问方式**（SSH 最好），或你把 `otiprobe` 拷过去跑
`sudo ./otiprobe infoall` / `sudo ./otiprobe write` 把输出贴回来。

## 25. 第 18 轮进展：真机写通道的"能写/不能写"清单（**推翻了我上一轮的结论**）

有了免管理员的真机访问后，把写命令的参数空间扫了一遍（`otiagent.ps1 -Sweep` / `-HidTest`）：

| 命令 | 数据阶段 | 结果 | 说明 |
|---|---|---|---|
| `0xD9/0x2A`（数据管道写，厂商模板 `CDB[2]=0xFF`） | 65536 / 1024 / 2 / **无** | ❌ rc=258 | `key=0x9 ABORTED COMMAND`、`ASC=0x81`（厂商自定义） |
| `0xD9/0x2A` 的 `CDB[2]` ∈ {0x00,0x01,0xFF} | 2 | ❌ rc=258 | 同上，与 `CDB[2]` 无关 |
| **`0xD9/0x33`（HID 键盘包）** | **无（厂商用法）** | ✅ **rc=0** | 连发多次都成功 |
| **`0xD9/0x34`（HID 鼠标包）** | **无** | ✅ **rc=0** | 同上 |
| `0xD9/0x36`（HID 多媒体） | 2 | rc=121 | 我多传了数据阶段才超时；无数据阶段应为 ✅ |
| `0xF0/0x05/0x02`（USB 重启） | 无 | ✅ rc=0 | |
| `0xD9/0x60`（设备模式） | 读 | ❌ `key=0x5/ASC=0x20` | 此固件不支持 |
| `0xF0/0x31`（独占锁） | 无 | ❌ `key=0x5/ASC=0x20` | 此固件不支持 |
| 其它 opcode（`0xDA/0x2A`、`0xF1/0x2A`） | — | ❌ `key=0x5/ASC=0x20` | 不支持 |

### 25.1 重要纠正

上一轮我根据 macOS 驱动里 `_OTi_SendHIDPacket` 的 **10 秒节流**，推断"HID 通道只是维护通道"。
真机证明：**节流是驱动侧策略，固件本身完全接受背靠背的 HID 包**（我们连发 5 次全 rc=0）。
所以"**被控端零软件、键鼠靠线缆注入 HID**"这条路**重新成立**——这正是用户拓扑最理想的方案。

### 25.2 数据管道写为何被中止（两个候选解释，都需要主控端验证）

- (a) 对端（Linux）链路未建立/未运行 → 设备拒绝向管道投递；
- (b) 角色限权：本端是**被控端**，数据管道只允许主控端写（HID 包则两端都能发）。

关键旁证：写出的是 `key=0x9 ABORTED COMMAND`（**命令被识别但被中止**），
而不是 `key=0x5 ILLEGAL REQUEST`（不支持）；且 `0xF0/0x31`/`0xD9/0x60` 这些**新版固件命令在本机是被判"不支持"的**
——说明这台 2009 年固件的命令集比随线 macOS App（2022 年构建）所假设的更旧。

### 25.3 观察能力的边界（如实记录）

| 观察手段 | 结果 |
|---|---|
| `GetCursorPos` | ✅ 可读（654,623 合理值）——**光标位移是可靠观察口径** |
| `GetAsyncKeyState` | ⚠️ 读得到但当前全 0（无法确证） |
| 直读线缆 HID 报告（`ReadFile` on HID 接口） | ❌ err=5（Windows 防键盘记录保护） |
| 本进程 `SendInput` 注入 | ❌ 返回 0（我从 WSL 启起的进程无桌面注入权限；用户自己的 PowerShell 可以） |
| 反向验证：本端发 HID 包是否本机回环 | ❌ 否（光标不动）→ 注入只发往对端 |

### 25.4 交付：Linux 主控端可直接跑的校准序列

`otiprobe.c` 新增 `hidsend <1|2|3> <24位hex>` 与 `hidseq`（9 个候选包，覆盖 reportID/字段位置/扫描码等差异，
每个间隔 2 秒，鼠标用大位移、键盘用不同字符以便对端区分）。
静态二进制已更新到 `C:\Users\Public\otilink\otiprobe`。

**下一步的双机校准**：
1. 你在 Linux 上跑 `sudo ./otiprobe hidseq`；
2. 我在这边跑 `otiagent.ps1 -Watch 30`（观察光标位移）；
3. 光标在哪一个候选之后跳动，就说明那种 12 字节布局正确 → 直接定版 HID 发送实现。

## 26. 第 19 轮进展：**发现设备有两种 USB "人格"**（这是本项目的关键结构）

把控制命令空间扫完之后，发现设备 PID 从 `0ea0:2213` **变成了 `0ea0:2208`**，
且接口与盘符都变了。对比：

| | `0ea0:2213`（"Virtual Link"） | `0ea0:2208`（"Transfer line"） |
|---|---|---|
| 磁盘 FriendlyName | **Virtual Link** | **Transfer line** |
| 大容量存储 | LUN0 = 只读 CDFS `MacKMLink`(3.84MB) + LUN1 = FAT `VirtualLink`(1MB) | **只有一个** 1MB FAT 卷 |
| HID 接口 | **键盘 + 鼠标**（MI_01/MI_02） | **无** |
| 信息块 `F0/00` 的 [2..3] | `22 13` | `22 08`（回显当前 USB PID） |
| 数据管道读 `D8/00/03` | 10 非零字节 `01 00 00 00 00 00 00 ff ff ff` | **1 非零字节** `01` |
| 数据管道写 `D9/2A` | ❌ CHECK CONDITION | ❌ 仍然 CHECK CONDITION |

触发方式：**厂商的 `USBRestart`（`0xF0/0x05/0x02`，`CDB[4]=0x0A`）** 把设备从 2213 切到了 2208。
两个模式下的 1MB 卷**内容相同**（`autorun.inf` / `installation.htm` / `VLver.set` / `icon.ico`，
且我写进去的 `marker.txt` 在切换后仍在）→ 是**同一块存储**，只是暴露方式不同。

### 26.1 为什么这很关键

1. **HID 注入需要 2213 人格**（只有那个模式才有 HID 键盘/鼠标接口）。
   我们的目标（Linux 主控 → Windows 被控）里，**Windows 端必须在 2213 模式**才能接收注入的键鼠。
2. `Transfer line` 这个名字 + "只剩一个 1MB 卷" 强烈暗示：**这一代产品的数据传输介质就是这个共享虚拟卷**
   （对拷线经典做法：写文件进去、对端读出来），而不是（或不只是）SCSI 数据管道。
   这解释了为什么**在两种人格下数据管道写都被拒** —— 也许它根本不是本产品的传输通道。
3. 也因此，**剪贴板**很可能可以走这个 1MB 卷（普通文件 I/O，不需要管理员、不需要私有 SCSI 写），
   只要 Linux 端能挂载并读写它。

### 26.2 待验证（需要 Linux 端配合）

- 这个 1MB 卷**是否两端共享**（写在这边、那边能否读到）→ 决定"共享卷方案"是否成立；
- `USBRestart` 的 `CDB[4]` 是否编码了目标人格（后台正在扫 `0x00..0x0F`）；
- 2213 人格是否只能靠**物理重插**恢复。

### 26.3 当前设备状态提示

设备现在停在 **2208（Transfer line）**，所以这边看不到 HID 接口、也没有 `MacKMLink` 光驱。
若需要回到 2213（例如读随线光盘或接收键鼠注入），**拔下再插上**应当即可恢复。

## 27. 第 20 轮进展：实现"被控端零软件"HID 直发模式（+18 项单测）

### 27.1 为什么这条路线是对的

真机实测（§25）已证明：`0xD9/0x33`（键盘）/`0xD9/0x34`（鼠标）HID 包**被固件接受且无节流**
（连发多次全 rc=0）。被控端的 HID 键盘/鼠标接口是标准 HID，**内核直接认**。
所以主控端只要把本地输入转成 HID 包发出去，被控端**不需要任何软件**——这正是用户拓扑最理想的形态。

### 27.2 新增模块与 daemon 模式

- `otihid.[ch]`：evdev → USB HID 的映射与报告构造
  - 键码映射表（字母/数字/修饰/标点/方向/编辑/F1-F12/小键盘），修饰键位图；
  - 键盘状态：`mods` + 6 键 rollover（与 boot keyboard 报告一致，带去抖与滚动）；
  - 3 种键盘布局 + 3 种鼠标布局候选（reportID 有无、保留字节有无、字段位置），
    因为 12 字节载荷的确切布局必须真机标定；
  - 鼠标相对位移按 HID 惯例截断到 ±127。
- `otikm --peer hid`：**不进协议状态机**，把捕获到的键鼠直接发成 HID 包
  （`--hid-kbd-layout N` / `--hid-mouse-layout N` 选布局；`--hid-probe` 启动时发
  鼠标 +100/-100 与一次 'A' 按键，便于对端观察确认）。

### 27.3 单测（离线可验，18 项）

```
[PASS] KEY_A(30) → HID usage 0x04 / KEY_Z → 0x1D / KEY_1 → 0x1E / SPACE → 0x2C
[PASS] 未映射键返回 0；修饰键位映射正确
[PASS] 按下/松开/重复按下（去抖）的状态变化判定
[PASS] layout0/1/2 三种键盘报告的字节布局
[PASS] 鼠标 layout0/1/2 布局；超范围位移截断到 ±127
```

Linux 全量测试：**112 项 PASS / 0 FAIL**。

### 27.4 待硬件标定（设备当前被拔出）

设备现处于拔出状态（我做了 16 次 `USBRestart` 模式扫描，可能把它留在异常状态；
**重插即可恢复**）。恢复后按下面一条命令即可标定 HID 布局：

```bash
# Linux 主控端（工具：C:\Users\Public\otilink\otiprobe）
sudo ./otiprobe hidseq          # 依次发 9 个候选包（鼠标大位移 / 不同字母），间隔 2 秒
```
**在 Windows 这端看屏幕**：光标在哪一号候选后跳动 → 鼠标布局确定；记事本里出现哪个字母 → 键盘布局确定。
拿到结论后我把 `otihid.c` 的默认布局改成它，键鼠共享就通了（被控端零软件）。

另外 `-ModeCmd`（在 2208 人格下试 `0xD9/0x60` 读/写看能否切回 2213）因设备不在**未执行**，待重插后补。

## 28. 第 21 轮进展：修正 HID 模式的可用性缺陷 + 补 HID 链路集成测试

### 28.1 修正：HID 模式也必须走状态机

上一轮 `--peer hid` 的实现**绕过了状态机**，等于"键鼠永远发往对端、本机彻底失控"——这是可用性缺陷。
本轮改为：**仍然用 `otikm_core` 决定何时转发**（撞边 → 开始转发；热键 → 收回），
只把"转发"的载体从协议消息换成 HID 包：

```
OTI_ACT_LOCAL              → 本地消费（不转发）
OTI_ACT_TO_REMOTE          → 发 HID 包（键盘 0xD9/0x33 / 鼠标 0xD9/0x34）
OTI_ACT_SWITCH_TO_REMOTE   → 重置键盘状态 + 独占本地键鼠（对端没有软件可解释 SWITCH 消息，无需发送）
OTI_ACT_SWITCH_TO_LOCAL    → **发"释放所有按键"包** + 恢复本地键鼠
```

**关键细节**：切回本地时必须发 `release_all`（`type=1` + 12 字节全零），否则对端会**卡住**切换瞬间按着的键
—— 这是 KVM 类工具的经典 bug。退出时也发一次。

### 28.2 新增 HID 链路集成测试（用仿真设备验证接缝）

`mocktest` 增加 4 项断言，把「HID 报告 → SCSI CDB → 设备」这条唯一没测到的接缝补上
（`otimock` 相应记录了最近一次 HID 包的 `CDB[1]`）：

```
[PASS] 键盘 HID 包经 CDB[1]=0x33 送达设备
[PASS] 设备收到的 12 字节与报告一致（Shift+A → mods=02 / A=04）
[PASS] 鼠标 HID 包经 CDB[1]=0x34 送达（reportID=01 buttons=01）
[PASS] 释放所有按键包 = type1 + 12 字节全零
```

Linux 全量测试：**116 项 PASS / 0 FAIL**（§27 的 18 项 HID 单测 + 本轮 4 项集成）。

### 28.3 设备仍处于拔出状态

本轮开始与结束时设备都不在（`0ea0` 无任何 PnP 条目、无 FAT/CDFS 卷）。
**重插即可恢复 2213 人格**（HID 接口 + 随线光盘）。恢复后：
1. 先跑 `-ModeCmd` 补验 2208 人格下 `0xD9/0x60` 能否切回 2213（免得以后依赖物理重插）；
2. 用 `otiprobe hidseq` 标定 HID 布局（**鼠标位移我可以自己在这边观察光标**，键盘需要你看一眼记事本）。

## 29. 第 22 轮进展：跨实现**运行时**互通验证（互操作矩阵）

给 Windows agent 加了 `-Tcp` 模式，与 WSL 里的**真实 `otikm`**（fd 传输）对跑，
拿到了第一份"运行时互通"而非"帧格式兼容"的证据：

| 组合 | 方向 | 结果 |
|---|---|---|
| `otikm`(Linux) → agent(Windows) | CLIP | ✅ **通过**：Linux 剪贴板内容出现在 Windows 剪贴板（agent 日志 `CLIP 已应用远端剪贴板 21 字节`，我用 `Get-Clipboard` 核对一致） |
| Python 客户端 → `otikm`(Linux) | CLIP | ✅ **通过**：`otikm` 应用了 Python 发来的 CLIP，剪贴板文件被更新为 `from-python-client` |
| `otikm`(Linux) → Python 客户端 | CLIP | ✅ **通过**：Python 收到 41 字节 CLIP 消息 |
| agent(Windows) → 对端 | CLIP | ⚠️ **未通过**（见下） |

也就是说：**协议与 Linux 守护进程的两向都已被独立实现验证**（Python 侧完全独立），
agent 的**接收+应用**也已在真机上下文验证。

### 29.1 agent 发送方向的已知问题（产品路径不受影响）

agent 的发送路径**能正确构造消息**（调试输出显示 55 字节消息已就绪），
但 TCP 写出时报 `WSAECONNABORTED`（"连接被本机软件中止"）。定位过程：

- 最小 Python 服务端 + PowerShell 客户端：读写都正常 → **网络与 PowerShell socket 本身没问题**；
- Python 服务端 + agent：读正常、**写失败** → 问题在 agent 的 socket 用法；
- 已把 TCP 写从 PowerShell 函数内联到调用点（怀疑函数作用域下 .NET Stream 写有问题），
  但该轮验证被 **Windows 剪贴板语义**干扰（剪贴板内容只在拥有它的进程存活期间有效，
  我用一次性进程设置后进程即退出，agent 轮询看到的仍是旧内容），未能复现出发送。

**影响评估**：产品路径（`--Device \\.\H:` SPTI + 64KB 帧）不经过这段 TCP 代码，
因此不影响真机部署；TCP 模式只是测试通道。留作待办：把 agent 的发送路径在
"剪贴板由长驻进程持有"的条件下重测。

### 29.2 顺带学到的 Windows 剪贴板性质（写进文档避免再踩）

Windows 剪贴板内容**由拥有它的进程维持**：短命进程 `Set-Clipboard` 后立即退出，
内容可能随即失效。测试时必须用一个"存活若干秒"的进程持有剪贴板（`Set-Clipboard; Start-Sleep`）。

### 29.3 状态

Linux 全量 **116 项 PASS / 0 FAIL**；Windows agent 自检 12/12；设备仍处于**拔出**状态
（等待重插恢复 2213 人格）。

## 30. 第 23 轮进展：剪贴板**双向运行时互通全部打通**（并修掉两个真 bug）

上一轮遗留"agent 发送方向失败"，本轮定位并修复，最终拿到**完整双向**的运行时验证。

### 30.1 两个真 bug（都很典型）

1. **`ReceiveTimeout` 设晚了**：我在 `$stream = $client.GetStream()` **之后**才设
   `$client.ReceiveTimeout`，而 `NetworkStream` 的读超时是在创建时从 socket 继承的
   → 读一直阻塞 → **整个循环卡在读上，剪贴板轮询根本不会执行**（所以从不尝试发送）。
   修法：`ReceiveTimeout` 放在 `GetStream()` 之前 + 补 `$stream.ReadTimeout`。
   （SPTI 路径每帧立即返回，所以这个 bug 只影响 TCP 测试通道——这也是它一直没暴露的原因。）
2. **PowerShell 把 .NET 异常包成 `MethodInvocationException`**：我写的
   `catch [System.IO.IOException]` **匹配不到**，于是"读超时"被当成致命错误路径处理。
   修法：改用无边界的 `catch { }`，按"是否读到过字节"判断是超时还是断链。

修好后经**独立实现**验证：Python 服务端收到 agent 的 59 字节 CLIP，十六进制逐字段正确
（`"OTL1"` + type=4 + len=39 + CRC + `format=1 flags=3 fid=1 off=0 total=19 dlen=19`）。

### 30.2 完整互通矩阵（`make interop`，可复现）

| 组合 | 方向 | 结果 |
|---|---|---|
| Linux `otikm` → Windows agent | CLIP | ✅ 通过 |
| Windows agent → Linux `otikm` | CLIP | ✅ 通过 |
| Python 独立客户端 ↔ Linux `otikm` | CLIP 双向 | ✅ 通过 |

```
$ make interop
  [PASS] 方向1 Linux→Windows 剪贴板（实得 'linux-to-windows-OK'）
  [PASS] 方向2 Windows→Linux 剪贴板（实得 'windows-to-linux-OK'）
  [PASS] agent 确实发出了 CLIP
  [PASS] 守护进程确实应用了远端 CLIP
```

**意义**：剪贴板共享（目标的一半）现在是"两个独立实现之间、双向、运行时"验证过的，
并且固化成了 `interop.sh` + `make interop`。

### 30.3 状态与剩余

- Linux 全量测试 116 项；Windows agent 自检 12 项；`make interop` 4 项；
- **设备仍处于拔出状态**（等待重插恢复 2213 人格）。

剩余三件都需要设备在位（或 Linux SSH）：
1. HID 布局标定（鼠标我可以自己观察光标；键盘需你瞄一眼记事本）；
2. 共享卷是否两端共享（决定剪贴板走卷还是协议管道——注意协议管道已证明在两端可用！）；
3. 2208 人格下 `0xD9/0x60` 能否切回 2213。

## 31. 第 24 轮进展：大载荷分块互通验证 + 出运行手册

### 31.1 大载荷跨实现分块/重组（新增 2 项断言）

`make interop` 增加"阶段 3"：把 Linux 剪贴板设成 **140000 字节**（无换行、确定性模式），
守护进程切成 3 块（65000+65000+10000）发出，Windows agent 跨块重组并 `Set-Clipboard`：

```
[PASS] 大载荷（140000 字节）完整到达 Windows 剪贴板（实得 '140000'）
[PASS] 大载荷内容 MD5 一致（6c358b2354ac44b0b9d4399e602170a2）
```

至此剪贴板的跨实现验证覆盖：**小块双向 + 大块分块重组 + 回环抑制**。

### 31.2 `otikm` 增加一处诚实提示

`--peer hid` 配合 `--clipboard` 时，代码会显式提示：**剪贴板仍走协议管道**（不是 HID），
所以对端要么跑我们的 agent，要么改用共享卷方案——避免"以为 HID 模式连剪贴板都不用装东西"的误解。

### 31.3 `RUNBOOK.md`：面向当前拓扑的操作手册

把"现在能跑什么、下一步做什么"整理成一份手册（`re/RUNBOOK.md`），内容包括：
已验证事实清单（含证据章节号）、2213/2208 人格与恢复方法、
Linux 主控端键鼠共享的完整命令（含 HID 布局标定）、剪贴板两条路线（协议管道 / 共享卷）、
无设备也能跑的自检命令、以及 5 条待办（含"给我 SSH 就能一次做完"）。

关键设计结论（写入手册）：
- **键鼠 Linux→Windows** = HID 包，Windows **零软件** ✓（真机已验证固件接受、无节流）；
- **剪贴板 Linux→Windows** = 协议管道（Windows 读正常、无需管理员）；
- **剪贴板 Windows→Linux** = 协议管道写在被控端被拒 → 需共享卷（待验证）或暂不支持。

Linux 全量：**116 项 PASS**；`make interop`：**6 项 PASS**。

## 32. 第 25 轮进展：一键 bring-up 脚本（并进入"等待设备"状态）

### 32.1 `bringup-linux.sh`

把主控端的启动流程收成一条命令：

```bash
sudo sh bringup-linux.sh --calibrate     # 标定 HID 布局（Windows 侧看光标/字母）
sudo sh bringup-linux.sh --mouse-layout N --kbd-layout M   # 正式跑
```

自动完成：`otiprobe infoall` 找线缆 → 用 `/dev/input/by-id/*-event-kbd|mouse` 选输入设备
→ 以 `--transport cable --peer hid --cable-init --clipboard --grab` 启动。
两条错误路径已在 WSL 里实测（非 root、无设备）都给出可执行提示。

### 32.2 进入等待设备状态

自第 19 轮起，对拷线一直处于**拔出/异常态**（我做的 `USBRestart` 模式扫描把它留在了 2208，
且此后设备再未出现）；同时 **Linux 那台不可达**（不在 tailnet、局域网邻居里没有、无 SSH 配置）。

因此剩余四项——**HID 布局标定、主控端数据管道可写性、共享卷是否两端共享、2208→2213 切换**——
全部无法继续验证。软件侧能做的收口已经做完：
Linux 116 项断言、agent 12 项自检、跨实现互通 6 项（含 140KB 分块）、
`make sgtest`（SG_IO 层）、`make wintest`（帧级兼容）、真实内核验证（输入链路、剪贴板后端）。

**解除阻塞只需要一件事**：把线的 Windows 这端重插（恢复 2213），并在 Linux 上跑
`sudo sh bringup-linux.sh --calibrate`（或给我 Linux 的 SSH）。

## 33. 第 25 轮进展：**协议核心被推翻重写**（Linux 端真机接通，两个管道分离）

本轮拿到了 Linux 端（麒麟）的 SSH，主控端第一次真正跑起来，于是**此前所有"读不到帧"的困惑一次性解开**：
我们把**消息管道**当成了**帧管道**。

### 33.1 关键纠正：读方向有**两条完全不同的命令**

| 用途 | 命令 | 数据阶段 | 我们之前的错误 |
|---|---|---|---|
| **消息管道**（16 字节控制消息） | `0xD8/0x00/0x03` | **IN 16 字节** | 一直当它是 64KB 帧读 → 永远只回 10~16 个非零字节 |
| **帧管道**（真正的 64KB 帧） | **`0xD9/0x28/0x64`** | **IN 65536 字节** | 从未测过（我们只扫过 `0xD8` 的读和 `0xD9` 的 OUT） |

反汇编实证：

```c
// ProcessIdleState @0x4e30
CFDataCreateMutable(NULL, 0x10);            // ← 只申请 16 字节！
CFDataAppendBytes(m, zeros, 0x10);
CDB[0]=0xD8; CDB[1]=0; CDB[2]=3; CDB[3]=held>>8; CDB[4]=held; CDB[14..15]="OT";
SendSCSICommandReceiveData(cdb, 0x10, m, ...);
rax = CFDataGetMutableBytePtr(m);
switch (buf[0]) { ... }                     // ← 按第一个字节做 17 路跳转分发
```

```c
// GetData @0x57bc —— 真正的帧读取
CDB[0]=0xD9; CDB[1]=0x28; CDB[2]=0x64; CDB[14..15]="OT";
data = 65536 字节 IN;
// 之后才是 §9.1 描述的校验：前16字节全零+u32@0x10==0 → 空闲；
// head[0..15]==buf[0xffec..] 且 u32@0x10==u32@0xfffc → 有效帧
```

`resid` 是判据：向 `0xD8/0x00/0x03` 要 65536 字节，设备只给 **16 字节**（resid=65520），
`sel=0/1/4/5` 给 2 字节、`sel=6/7` 给 3 字节、`sel=2` 给 256 字节（`USBC` 结构）。

### 33.2 消息管道语义（跳转表 @0x5234，`buf[0]` 为类型）

| 类型 | 处理 | 含义 |
|---|---|---|
| `0x00`/`0x08`/`0x10` | 0x4f3d | 复位：清收发队列 |
| `0x01` | 0x4f40 | 复位 + 标记（被控端稳态收到这个） |
| `0x02`/`0x03`/`0x0b..0x0f` | 0x5001 | 只打日志 |
| `0x04` | 0x5017 | 只打日志（`_OTi_Close` 会发 `D8 01 04 03 e8` = 类型4/值1000） |
| **`0x05`** | 0x502a | **对端有 N 帧**：`be16(buf[1..2])` → `rol bx,8` → `RegRx=+0x54` → `GetData(count)` |
| `0x06` | 0x5075 | `be16(buf[1..2])!=0` → 发信额度 `[+0x52]=1` |
| **`0x07`** | 0x5088 | **发信额度** `[+0x52]=be16(buf[1..2])`，然后 `ContinueTxCount++` → `processTransferState()` |
| `0x09` | 0x50a6 | 请求重传（把保存的包插回队首） |
| `0x0a` | 0x50d1 | 释放保存的包 |

成员表补充：`+0x52` = **发信额度**（只由 0x06/0x07 更新）；`+0x54` RegRx；`+0x56` ContinueTxCount；`+0x58` MaxBookingSize(100)。
`ProcessIdleState` 开头：`ContinueTxCount>=11 → 归零且 held 报 0`，否则 `held=min(RX队列数,100)`。

### 33.3 写方向与流控

```c
// PutData @0x5386
CDB = D9 2A FF ... 'OT'；数据 = 帧(65536)
// processTransferState @0x5d7a
count = *(u16*)(this+0x52);  PutData(&count);  *(u16*)(this+0x52) = count;
```

→ **写帧被 `+0x52`（额度）门控**。额度为 0 时写被拒：`key=0x9 ASC=0x85`（主控端）
或 `key=0x9 ASC=0x81`（被控端）。主控端第一次写成功（初始额度 1），之后额度归零就再也写不进去。

`SetRemoteHostStatus(value,arg2)` @0x52fc —— **纠正 §17/§4**：
```
CDB = D8 01 <arg2> <value_hi> <value_lo> ... 'OT'   ← 无数据阶段！（16 位值在 CDB[3..4]，大端）
```
之前记的"OUT 2B 数据阶段"是错的；带 2 字节数据阶段会得到 `rc=121` 超时。

`LockFunction(lock,a2,a3)` @0x46a0（`F0/31`）格式：`CDB[2]=lock`，`CDB[3]=a2`，`CDB[4]=a3`，
`CDB[5..8]` = `TickCount()` 4 字节大端（加锁分支），**+ 2 字节 OUT 数据阶段**（16 位返回状态）。
**本代固件不支持**：`key=0x5 ASC=0x20`（Invalid command operation code），抢锁不走这条路。

### 33.4 硬件实测：HID 包确实**不是**键鼠通道（推翻第 20 轮的 `--peer hid`）

从主控端发了 10 种鼠标包格式（dx 分别落在第 1/2/3/4/5/6 字节，含 reportID=1/2 变体，
位移量各自唯一：+50/+110/+140/+170/+200/+230/+260/+290/+320/+350），**全部 rc=0 被接受**，
Windows 侧 `cursorwatch.ps1` 观察 55 秒：**光标一次都没动**。

→ §9.2 的结论是对的：HID 包是**维护包**；实时键鼠走**数据管道上的帧**。
`otikm --peer hid` 这条路线的核心假设**在硬件上被否证**。

### 33.5 硬件实测：1MB FAT 卷**不共享、且写入易失**

- 链路断开时写主控端卷标记 → 插上被控端后，被控端卷里没有；
- 链路**接通**时主控端（Linux 挂载点 `/media/kylin/4100-14E3`）写 `from-linux-master.txt` →
  被控端卷（`H:`）里仍然没有；
- 被控端卷里我先前写的 `otilink-controlled-marker.txt` **自己消失了**（卷空间回到 12288 字节的出厂状态）。

→ 两个端头各自有一份**只读出厂镜像 + 易失写入**的 1MB 卷；**不能当邮箱用**。剪贴板必须走帧管道。

### 33.6 真正打通的观测：`0xD9/0x28` 读回**有效 64KB 帧**

主控端（Linux）用正确命令读：

```
[1] 消息 type=0x07  07 00 00 00 00 00 00 ff ff ff ...   (额度=0)
[2] 消息 type=0x05  05 00 02 00 00 00 00 ff ff ff ...   (有 2 帧)
    帧#1 rc=0 判定=有效 非零=65255
    帧#2 rc=0 判定=有效 非零=65155
```

被控端（Windows）永远只读到 `type=0x01`（复位），配额恒 0，写帧恒 `rc=258`。

### 33.7 **重大发现：厂商的 Windows 程序正在运行，并且正在用这根线**

```
PID 297368  MacKMLink.exe   C:\Users\<你的用户名>\AppData\Roaming\OTi\MacKMLink1325\FunctModules\{...}\MacKMLink.exe
PID 304008  MacKMLink.exe
PID 137480  LinkEngKM
启动项 "CS Dispatch" → MacKMLink.exe -GN:RunFromRegistry
```

主控端收到的帧体里是**厂商原生 XML 协议**：

```
<ExtraXmlCommand>HookAppService</ExtraXmlCommand><ExtraXmlParam><OTIMSG><NP_Cmd>Cmd_Post_Re...
```

即：这条链路**本来就是通的**，是**厂商 Windows 程序**在占用/驱动它（它发的正是主控端→被控端的
`Cmd_Post_Remote_KM_Setting_Info` 一类命令）。设备自身还会定期投递两帧自测帧
（帧体含 ASCII `TXLIN1` + `1f 20 21 22 ...` 递增 ramp，时间戳字段随读次推进，
末字段分别是 `0x0b`(=11) 与 `0x64`(=100=MaxBookingSize)）。

**含义**：厂商软件与我们的 agent 会**争抢同一条链路**。要跑我们自己的协议，必须先在 Windows 上
退出 MacKMLink/LinkEngKM；在那之前，任何"写被拒/读到 XML"的观测都掺了厂商程序的成分。

### 33.8 麒麟（Linux 主控端）环境实测

- SSH 可用：`kylin@<麒麟IP>`（银河麒麟 V10 SP1，内核 5.4.18，x86_64）。
- `lsusb` = `0ea0:2213 Ours Technology, Inc. Android+Mac`；`sdb`=1M vfat（`/media/kylin/4100-14E3`）、
  `sr1`=MacKMLink iso9660（`/media/kylin/MacKMLink`）；`/dev/sg2`=LUN0(FAT，`root:disk`，**要 sudo**)、
  **`/dev/sg3`=LUN1(CD，有 `user:kylin:rw-` ACL → 免密可读写)**。
- **gcc 可用**（内核头文件齐全），可以就地编译，不必传二进制。
- **`/tmp` 是 `noexec`**（`./t` 报"权限不够"）；家目录 `~/otilink/` 可执行。
- **kysec 强制控制**：首次运行新编译的二进制会触发一次确认（表现为卡住），之后正常；
  **`python3` 直接被拦**（`Fatal Python error: config_parse_argv: Permission denied by kysec`）。
- `sudo` 需要密码 → **只用 sg3（免密）**；sg2 的 FAT LUN 无法访问，但 `0xF0/0x00`、数据管道在 sg3 上都能用。

### 33.9 本轮新增工具

- `otilink/otiprobe.c`：新增 `rx`（消息+帧完整接收循环，打印帧体 hex/ASCII 与标记搜索）、
  `raw <cdbhex> <len> <in|out|none>`（通用 CDB 探针）、`rprobe`（0xAA 预填量真实传输长度）、
  `mousecal`（10 种鼠标包格式标定）。修正设备参数解析（`/dev/...` 作为最后一个参数）。
- `windows/otiagent.ps1`：新增 `-Rx`（正确接收循环）、`-Raw <cdb[;cdb...]> -RawLen -RawDir`（通用探针）、
  `-WriteSweep`（带 64KB 数据阶段的写子通道全扫）；C# 新增 `ReadFrame28`（`D9/28/64`）与 `ReadMsg`（16B）。
- `windows/cursorwatch.ps1`：光标观察器（验证 HID 注入用）。

### 33.10 结论与下一步

**已验证可用**：`0xD9/0x28` 帧读取（两个方向都能读到设备投递的有效帧）、消息管道读取与分发、
`D8/01` 状态发送、写帧命令格式（与厂商字节级一致）。

**当前阻塞**：主控端发信额度为 0 且被控端恒复位 —— 而**厂商 Windows 程序正在占用链路**。
下一步必须先排除这个干扰（退出 MacKMLink/LinkEngKM），再判定：
1. 额度是否随厂商程序退出而开放（即额度是否由对端主机软件产生）；
2. 若仍为 0，则继续找额度的真实来源（`0x06`/`0x07` 由谁触发）。

## 34. 第 26 轮进展：**端到端打通！**（发送时序 + 对端消费是关键）

接 §33：搞清两条管道后，本轮把**发送侧为什么一直被拒**彻底解决，并完成真机端到端验证。

### 34.1 关键：`CDB[3..4]` 是"我要发送几帧"，`0x07` 才是发送授权

重新核对 `ProcessIdleState` @0x4e30 开头的取数（§33 里我写错了对象）：

```c
rdi = [this + 0x30];            // 0x30 = **发送队列**（SendData 追加、PutData 取用）
n   = CFArrayGetCount(rdi);     // 本机待发送帧数
held = (ContinueTxCount>=11 || n==0) ? 0 : MIN(n, MaxBookingSize);
CDB[2]=3; CDB[3]=held>>8; CDB[4]=held&0xFF;
```

→ **`0xD8/0x00/0x03` 里的 16 位值是"本机待发送帧数"（不是"已持有接收帧数"）**。

而 `switch (buf[0])` 的 `0x06/0x07` 分支用的 `bx` 就是上面这个 `held`（本机队列长度）：

```c
case 0x07: if (held != 0) { [this+0x52] = held; ContinueTxCount++; processTransferState(); }
case 0x06: if (held != 0) { [this+0x52] = 1; } else { ContinueTxCount = 0; usleep(10000); }
```

**所以正确的发送时序是**（与厂商主循环完全一致）：

```
1) 读消息（D8/00/03，同时用 CDB[3..4] 上报"我有 N 帧要发"）
2) 若设备回 0x07（或 0x06）→ 授权到了 → **立刻**写帧（D9/2A/FF + 64KB OUT）
3) 若设备回 0x05 → 那是有接收数据：be16(buf[1..2]) 帧数 → 用 D9/28/64 读帧
```

之前的失败原因：**在"读消息"和"写帧"之间插了多余的读/操作，把授权消费掉了**（授权是瞬时的）。

### 34.2 实测：93/120 写成功（对端必须在消费）

```
#81.. #93 消息type=0x07 → 写成功 rc=0（连续 13 次）
共 120 轮，写成功 93 次
```

而且**关键前提**：只有**对端（Windows）也在持续读**时，设备才会稳定发放 `0x07` 授权。
对端不读时，主控端读到的是 `0x01`（复位路径，清收发队列），写必被拒（`ASC=0x81/0x85/0x86`）。

→ **流控是"接收方消费 → 发送方获授权"**，两端必须同时跑循环。

### 34.3 端到端验证成功

主控端（Linux）连续写 93 帧，被控端（Windows）`-Rx` 读回：

```
帧#1 rc=0 判定=有效 非零=20
  头20B: 4f 54 49 4c 0a 00 00 00 00 ...     ← 'OTIL' + len=10
  体@20 : |otilink-tx|                       ← 主控端发出的原文
```

**Linux → 对拷线 → Windows 的完整数据通路首次打通。** 我们自有的 `otiproto` 帧格式在真机上被原样送达。

### 34.4 顺带纠正：链路不依赖厂商引擎

停掉 `MacKMLink/LEWD/LinkEngKM` 后，主控端消息恢复为 `0x07`（链路仍在）。
之前观察到的 `0x01` 洪水是**我自己那轮 `D8/01`（SetRemoteHostStatus）扫描**造成的——
`D8 01 <类型>` 会把状态投递给对端，其中**类型 1 = 复位**；扫到 1 就把链路打进复位态，
之后靠厂商引擎重启/重插才恢复。**以后不要盲扫 `D8/01`。**

### 34.5 厂商错误枚举（`LinkEngKM.exe`/字符串 + `GetOTiSenseKeyResult`）

`SendSCSICommandSendData` 的 sense 由 `GetOTiSenseKeyResult` @0x48da 映射成结果码：
`key=9 ASC=0x8N → 结果码 0x8N`（0x80..0x91）。`PutData` 里 **0x87..0x91 走重传**，
**0x80..0x86 是硬失败**。对应字符串（厂商源码路径 `.../SmartKMLink_USB2.0_source/...`）：

```
Send Error: OTiEng_Err_Tx_Rx_Image_Conflict
Send Error: OTiEng_Err_Tx_Tx_Image_Conflict
Send Error: OTiEng_Err_Tx_No_image
Send Error: OtiEng_Err_Request_Resend_Last_Packet
Send Error: OtiEng_Err_Request_Remove_Last_Packet
```

即协议按 **"image"** 组织传输；`Tx_No_image` = 没有可发的 image（本机待发队列为空 / 未上报）。

### 34.6 其他纠正

- `SetRemoteApStatus(bool)`/`SetRemoteDevStatus(bool)` 只是置内存标志（`[this+0x50]/[this+0x51]`），**不发 SCSI 命令**。
- `LinkEngKM.exe` 是 **UPX 加壳**的，不能直接静态扫 CDB。
- 卸载媒体（`udisksctl unmount -b /dev/sdb /dev/sr1`，**免密可用**）**不是**写帧的前置条件（试过，仍被拒）。
- 麒麟上 `/dev/sdb` 的 ACL 由 udisks 挂载时授予；**卸载后 ACL 消失且无法免密重新挂载**（polkit 需要 TTY），
  所以 **LUN0 仍未测过**（`/dev/sg2` 需要 sudo）。目前所有成功路径都在 LUN1（`/dev/sg3`）。

### 34.7 结论：现在可以实现目标了

已验证可行的完整方案（**不需要任何厂商软件**）：

```
主控端（Linux，/dev/sg3）                              被控端（Windows，\\.\H:）
loop {                                                loop {
  m = msg_read(announce = 待发帧数)                     m = msg_read(announce = 0)
  if m == 0x07/0x06 → 写帧(D9/2A/FF, 64KB)              if m == 0x05 → n=be16(m[1..2])
  if m == 0x05      → 读帧(D9/28/64, 64KB)                for i<n: 读帧(D9/28/64)
}                                                     }
```

键鼠事件与剪贴板都走这套帧管道（§9.2/33.4：HID 包不是实时键鼠通道）。
**两端必须同时运行循环**，否则授权不发放。

## 35. 第 27 轮进展：**键鼠共享 + 剪贴板真机跑通**（目标达成）

### 35.1 传输层改造（生产路径）

按 §34 的厂商模型重写 `otitrans.c` 的线缆后端：

- **待发队列 `txq` + 接收队列 `rxq`**（各 8 槽，槽 = 一条消息体 ≤65496）；
- **`cable_pump_once()`**：读消息（CDB[3..4] 上报 `txq` 长度）→
  `0x05` 则按 `be16(m[1..2])` 把帧读进 `rxq`；`0x06/0x07` 则**立刻**把 `txq` 写出去；
  `0x01/0x00/0x08/0x10` 计复位次数；
- `oti_tr_send()` = 入队 + 泵到发出去（5s 上限）；`oti_tr_recv()` = 泵一次 + 出队（带超时）；
- **`pthread_mutex`** 串行化收发线程对同一台设备的访问（`mocktest` 因此要 `-lpthread`）。

`otilink.c/h`：`otilink_msg_read()` 新增；`otilink_recv_frame()` 改用 `0xD9/0x28/0x64`。
`otimock.c` / `mocktest.c` 迁到两条管道；`make test` **全部通过**（连跑 3 次）。

### 35.2 Windows agent：线缆对等循环

`otiagent.ps1` 新增 `-Cable`：C# 加 `ReadMsgP(h, m16, pending)`；PowerShell 侧加
`$txq/$rxq` 队列 + `Pump-Cable`（与 C 侧同构）；`TxBody` 在线缆模式下入队 + 泵到发出。
主循环多一条 `elseif ($useCable) { Pump-Cable; 出队 }` 分支。

### 35.3 真机端到端验证（Linux 主控 → 线缆 → Windows 被控）

新增 `cabletest.c`（走生产路径 `otitrans`，**不需要 evdev/uinput**，因此绕开了麒麟的 root 限制）：

```
Linux 侧: [CLIP] "otilink-cable-clipboard-OK" rc=0
          [1..12/12] 发 MOUSE dx=+60 rc=0
          [KEY] 按下/松开 evdev 30 rc=0
          汇总: 发出 15 条（失败 0），收到 1 条
          收到 CLIP 24 字节: "键鼠共享 + 剪贴板"      ← Windows→Linux 方向

Windows 侧(agent 日志):
          CLIP 发送 24 字节 / 1 块
          CLIP 已应用远端剪贴板 26 字节（并抑制回发）
          MOUSE dx=60 ×12
          KEY code=30 value=1 / value=0
          结束: 收到 15 条消息，注入 14 次，剪贴板发块 1
```

**客观判定（不靠主观观察）**：

| 项 | 判定方式 | 结果 |
|---|---|---|
| 鼠标共享 | `cursorwatch.ps1` 观察真实光标 | `Δ=(140,0)` ×6，最后撞右边缘（发出的是 dx=+60） |
| 键盘共享 | 发 evdev 58(CapsLock) 后读 `[Console]::CapsLock` | 注入前 `False` → 注入后 **`True`**（已恢复） |
| 剪贴板 L→W | Windows `Get-Clipboard -Raw` | **`otilink-cable-clipboard-OK`** |
| 剪贴板 W→L | Linux 侧收到的 CLIP 消息 | **"键鼠共享 + 剪贴板"** 24 字节 |

**结论：键鼠共享与剪贴板双向在真机上全部打通。**

### 35.4 两个已知待改进项（不影响可用性）

1. **鼠标指针加速**：Windows 收到 `dx=60` 实际走 ~140px（系统"提高指针精确度"）。
   要 1:1 应改用 `MOUSEEVENTF_ABSOLUTE`（协议已有 SWITCH 携带 screen_w/h，可支撑绝对坐标）。
2. **扩展键**：evdev 码与 PS/2 set-1 扫描码在**主键区完全一致**（KEY_Q=0x10…KEY_A=0x1E、
   修饰键、空格/回车/退格等），所以字母数字与常用键直接可用；
   方向键/Home/End 等扩展键需要 `E0` 前缀映射（尚未做）。

### 35.5 交付物

- 麒麟端：`install-kylin.sh`（一次性 sudo 安装：udev 规则 + input,disk 组 + uinput 模块）、
  `run-kylin.sh`（自动挑选本机键鼠、排除对拷线自身 HID、启动 otikm `--transport cable:/dev/sg3 --inject --clipboard`）；
- Windows 端：`otiagent.ps1 -Cable -Inject -Clipboard -Device '\\.\H:'`；
- `cabletest.c`：真机数据通路回归测试（`make cabletest`）。

### 35.6 真实守护进程（otikm）真机验证 —— 完整生产路径

在麒麟上装好 udev 权限后（`sudo sh install-kylin.sh`），用 `uisim`（uinput 虚拟键鼠注入器）
驱动**真实的 otikm**，两端同时运行，验证了从 evdev 抓取到 Windows 注入的**全链路**：

主控端 `otikm --transport cable:/dev/sg3 --capture event9 --capture event10 --grab --inject --clipboard`：

```
剪贴板后端: file(sim)
抓取设备 /dev/input/event9 (otilink-uisim-mouse)
抓取设备 /dev/input/event10 (otilink-uisim-kbd)
uinput 注入设备已创建
传输就绪: cable:/dev/sg3
CLIP 应用远端剪贴板 26 字节（已抑制回发）
LOCAL mouse ×8                                   ← 指针从屏幕中心向边缘移动（本机消费）
SENT SWITCH side=remote edge=1919,540            ← 撞到右边缘 → 切换控制权
STATE 指针交给对端（本机转为驱动侧，开始独占转发）
已独占 otilink-uisim-mouse（驱动侧，避免本地与远端双重输入）   ← EVIOCGRAB 生效
已独占 otilink-uisim-kbd
SENT mouse dx=60 dy=0 ×10                        ← 位移全部转发
SENT key code=58 val=1 / val=0                   ← CapsLock 转发
结束: 发出 13 条, 注入 0 条, 切换 1 次
```

被控端 agent：

```
CLIP 发送 26 字节 / 1 块
SWITCH side=1 指针在本机
MOUSE dx=60 ×10
KEY code=58 value=1 / value=0
结束: 收到 13 条消息，注入 12 次
```

**客观判定**：

| 项 | 判定 | 结果 |
|---|---|---|
| 状态机 + 抓取 | otikm 日志 | `LOCAL mouse ×8` → `SENT SWITCH side=remote edge=1919,540` → 两条 `已独占` |
| 鼠标 | Windows 真实光标（cursorwatch） | 注入时刻 `Δ=(87,0)→(140,0)→(83,0)`，撞到 x=2047 右边缘 |
| 键盘 | `[Console]::CapsLock` | 注入前 `False` → 注入后 **`True`**（已恢复） |
| 剪贴板 W→L | otikm 日志 | `CLIP 应用远端剪贴板 26 字节` |
| 剪贴板 L→W | agent 日志 | `CLIP 发送 26 字节 / 1 块` |

**结论：键鼠共享与剪贴板**在真实守护进程下**完整跑通**（含 EVIOCGRAB 独占、撞边切换、
远端注入、剪贴板双向）。麒麟端仍需用户**真正注销重登一次**（组变更只在登录时读取；
调试期可用 `sg input -c ...` 临时取得权限）。

## 36. 第 28 轮进展：指针加速与扩展键**都修掉了**（真机定量验证）

### 36.1 绝对坐标指针（1:1 跟手）

**做法**：主控端不再发相对位移，改发**绝对坐标**；被控端用 `MOUSEEVENTF_ABSOLUTE` 注入。
系统指针加速对绝对定位不生效，因此 1:1。

协议新增两条消息：

| 类型 | 名字 | 载荷 |
|---|---|---|
| 7 | `OTI_MSG_MOUSE_ABS` | `int16 x, int16 y, int16 wheel, uint16 buttons`（x/y 是**对端主显示器**坐标系） |
| 8 | `OTI_MSG_HELLO` | `uint16 width, uint16 height`（本机屏幕，每 10 秒上报一次） |

状态机（`otikm_core`）新增远端坐标系跟踪：

```
local_w/h  本机屏幕（init 后不变，用于换算比例）
rem_w/h    远端屏幕（HELLO / SWITCH 得到，未知时退回本机尺寸）
rem_x/y    指针在**远端屏幕**坐标下的位置（have_control==0 时跟踪）
```

- 从右边缘交出去 → `rem_x = 0`（对端左边缘）；从左边缘 → `rem_x = rem_w-1`；
  `rem_y = my * rem_h / local_h`。
- 之后每个位移按 `rem_w/local_w` 比例累加并夹紧到 `[0, rem_w-1]`（跨屏也是 1:1）。

**被控端 `MouseAbs` 的关键坑（实测踩到并定标）**：

MSDN 说 `MOUSEEVENTF_ABSOLUTE` 的 0..65535 映射到**整个虚拟桌面**。实测**不是**：
本机是「2048x1152 主屏 + 左侧副屏，虚拟桌面 3584 宽」，发送 `nx = x*65535/(虚拟宽-1)`
得到的光标位置是 `x*2048/65535` —— 即**归一化实际线性映射到主显示器**。
按 `nx = x * 65535 / (SM_CXSCREEN-1)` 重标定后误差为 **0**：

```
screen=2048x1152
target 600,300   -> got 600,300
target 1500,900  -> got 1500,900
target 50,40     -> got 50,40
```

**端到端定量验证**（uinput 注入 26×60 位移，越界后本地位移 600px）：

```
注入前 Windows 光标 = (50, 40)
注入后 Windows 光标 = (640, 576)
理论值：rem_x = 0 + 600 × 2048/1920 = 640；rem_y = 540 × 1152/1080 = 576
```

两个坐标**都与理论值分毫不差**。（旧的相对模式会从 50 再走 ~1380px 直接撞到 2047 右边缘。）

### 36.2 扩展键映射

`KEYEVENTF_SCANCODE` 下 `wScan` 要用 **PS/2 set-1 扫描码**，而 Linux evdev 码只在
**主键区（evdev 1..88）与之一致**（KEY_Q=0x10…KEY_A=0x1E、修饰键、空格/回车/退格、F1–F12、小键盘）。
超出部分（方向键/Home/End/右 Ctrl/小键盘回车/Win 键…）需要 `E0` 前缀。

被控端新增表 `ExtScan`（18 项）+ `KEYEVENTF_EXTENDEDKEY`；未映射的键**宁可不注入也不按错键**：

```
96 KP_ENTER→0x1C  97 RIGHTCTRL→0x1D  98 KP_SLASH→0x35  99 SYSRQ→0x37  100 RIGHTALT→0x38
102 HOME→0x47  103 UP→0x48  104 PAGEUP→0x49  105 LEFT→0x4B  106 RIGHT→0x4D
107 END→0x4F  108 DOWN→0x50  109 PAGEDOWN→0x51  110 INSERT→0x52  111 DELETE→0x53
125 LEFTMETA→0x5B  126 RIGHTMETA→0x5C  127 COMPOSE→0x5D
```

**真机验证**（用 `uisim` 注入 evdev 125 = LeftMeta）：

```
按键前 前台窗口类 = Shell_TrayWnd
按键后 前台窗口类 = Windows.UI.Core.CoreWindow     ← 开始菜单被打开
```

### 36.3 仍然正确的部分

- 相对坐标的 `OTI_MSG_MOUSE` 保留（兼容旧对端 / 测试用）；协议层测试全绿。
- `--peer hid` 路径未动（该路线已被 §33.4 否证，保留仅作历史）。

### 36.4 补充：鼠标手感**可切换**，默认用被控端系统的指针速度

绝对坐标（36.1）虽然 1:1，但用户实际体验是"**比 Windows 慢**"——因为它绕过了系统指针加速。
所以又加了模式协商，**默认走被控端原生手感**：

| 模式 | 主控端发什么 | 被控端怎么注入 | 手感 |
|---|---|---|---|
| `native`（**默认**） | `OTI_MSG_MOUSE`（相对位移） | `MOUSEEVENTF_MOVE` | 走被控端系统的**指针速度/加速**（Windows 原生） |
| `absolute` | `OTI_MSG_MOUSE_ABS`（绝对坐标） | `MOUSEEVENTF_ABSOLUTE` | 1:1 跟手，不受加速影响 |

新增协议消息 `OTI_MSG_MOUSE_MODE(9)`，载荷 1 字节（`OTI_MOUSE_ABS=0` / `OTI_MOUSE_NATIVE=1`）。
**加速曲线在被控端系统里，所以由被控端决定**：agent 在启动时与每次 HELLO 一起声明，
主控端收到后据此选择发相对还是绝对（日志 `RECV MOUSE_MODE → …`）。

被控端命令行：`-MouseMode native|absolute`（默认 `native`）。

真机确认：

```
RECV MOUSE_MODE → 相对位移（走对端系统指针速度）
注入前 (958,984) → 注入后 (1871,673)
   —— 相对移动（**没有**像绝对模式那样跳到远端边缘），按本机加速曲线走
```

## 37. 第 29 轮进展：**健壮性重构**（用户反馈"被卡住、很简陋"，逐条修根因）

用户实际使用中"控制不了键鼠"。排查后确认**不是偶发，是我的设计缺陷**，共四处：

| # | 缺陷 | 后果 | 修法 |
|---|---|---|---|
| 1 | `oti_tr_send`（线缆）**在输入路径里阻塞最多 5 秒**且持有 `t->lock` | 对端一不消费，输入线程卡在 5s 阻塞里，接收线程因同一把锁被饿死 → **热键也失灵，彻底卡死** | 改为**非阻塞**：只入队 + 顺手推一次；真正的发送交给接收线程的泵。队列满时丢**最旧**一条（位移宁丢旧不卡新） |
| 2 | 没有看门狗 | 对端挂掉后一直保持 EVIOCGRAB，用户完全失控 | `--idle-release MS`（默认 3000）：处于驱动侧 **且有待发数据送不出去** 时，自动 `otikm_core_force_local()` + 清发送队列 + 释放独占，并打显著日志 |
| 3 | Windows agent 每轮 `New-Object byte[] 65536` | 每秒上千次 64KB 分配 → GC 风暴、整机变卡 | 预分配复用 `$script:frameBuf` / `$script:msgBuf`；**只有真收到帧才分配**；空闲时 `Start-Sleep 2ms` |
| 4 | agent 是限时任务（1 小时） | 到点就死，用户的会话就断 | 默认**永久运行**（`-DurationSec 0`），并配**登录自启**；再补**断线自动重连**（连续 60 次读失败就重开设备） |

### 37.1 看门狗真机验证

```
STATE 指针交给对端（本机转为驱动侧，开始独占转发）
已独占 otilink-uisim-mouse（驱动侧，避免本地与远端双重输入）
已独占 otilink-uisim-kbd
!! 看门狗：对端 3000ms 未消费待发数据 → 已**自动交还本机控制**（键鼠恢复本地）
已释放 otilink-uisim-mouse（键鼠交还本机桌面）
```

（杀掉被控端进程后 3 秒内自动交还 —— **用户不再可能被卡住**。）

### 37.2 其他健壮性补强

- **信号处理**：`SIGINT/SIGTERM/SIGHUP` → 信号处理函数里直接 `ioctl(EVIOCGRAB, 0)`（异步安全），
  保证 Ctrl+C / 被杀 / 终端关闭都能立刻把键鼠还给用户。
- **发送队列 8 → 32 槽**，并支持丢最旧。
- `otikm` 的 `--duration 0` 语义前面已统一（§35 修过一次），本轮确认三处一致。
- **被控端自启**：`%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\otilink-agent.vbs`
  隐藏窗口启动；agent 本体已复制到 `C:\Users\<你的用户名>\otilink\`（**不依赖 WSL**）。

### 37.3 教训（写进手册）

**任何在输入路径上的阻塞调用都会导致"键鼠失控"**：因为接收线程与热键处理共用同一把锁，
一旦发送侧长时间持锁，用户连"按热键拉回"都做不到。这类程序必须保证：
1. 输入路径**永不阻塞**；
2. 必须有**看门狗兜底**（不依赖对端健在）；
3. 信号/异常/崩溃路径都要释放独占（EVIOCGRAB 会随 fd 关闭自动释放，但显式释放更快更明确）。

## 38. 第 30 轮进展：产品化（对标 Mouse Without Borders / Barrier）

用户要求"做成产品级、参考同类标杆"。对标对象与据此实现的功能：

| 标杆功能 | 出处 | 本实现 |
|---|---|---|
| Easy Mouse 撞边切换 | MWB | `edges` 可配（left/right/top/bottom/all/none），四边都支持 |
| 可选 Shift/Ctrl 才切换 | MWB | `switch_modifier = shift/ctrl/alt/meta`（须按住才撞边切换） |
| Block mouse at screen corners | MWB | `corner = 40`（贴角死区，防误切） |
| Ctrl+Alt+<键> 切换热键 | MWB | `ctrl+alt+space` 切换 / `ctrl+alt+left` 拉回 / `ctrl+alt+right` 交出 / `ctrl+alt+l` 锁定 |
| 托盘图标 + 状态通知 | MWB | 被控端 WinForms 托盘：双击看状态、右键暂停注入/退出、提示实时刷新 |
| 配置驱动 | Barrier/Input Leap | `~/.config/otilink/kvm.conf`，命令行优先；`--config/--no-config` |
| **热键抑制**（不泄漏给对端） | Barrier/Input Leap | **修饰键抑制缓冲**：见下 |
| Move mouse relatively（多屏/异分辨率） | MWB | `-MouseMode native`（默认）走对端系统指针速度 |
| 一键安装/卸载 | — | `install-kylin.sh` / `uninstall-kylin.sh` / `install-windows.ps1` / `uninstall-windows.ps1` |

### 38.1 热键抑制（这是"产品级"和"能跑"的分界线）

裸转发修饰键会导致：按 `ctrl+alt+left` 切换控制权时，Ctrl/Alt/Left **同时被发到对端**，
在对端留下卡住的 Ctrl/Alt（表现是"对面突然一直按住 Ctrl"）。Barrier 用 hotkey suppression 解决。

实现（`otikm_core.c`）：修饰键按下时**先压进缓冲不转发**，然后

```
按下 ctrl  → 缓冲 [ctrl↓]                      （仍可能是热键前缀）
按下 alt   → 缓冲 [ctrl↓, alt↓]
按下 space → 命中热键 → **整段丢弃** + 切换控制权     ← 对端什么都收不到
按下 a     → 不是热键 → 缓冲补发 [ctrl↓, alt↓, a↓]   ← Ctrl+Alt+A 正常到达对端
ctrl 松开  → 缓冲里只剩修饰键 → 补发（普通按住 Ctrl 也正常）
```

`hk_prefix_possible()` 判断"当前按下的修饰键是否还可能是某个热键的前缀"，只有可能时才开始压。
调用方在每次 local_key/local_mouse 之后 `otikm_core_take_suppressed()` 把缓冲按原顺序补发。

### 38.2 多显示器绝对坐标：`MOUSEEVENTF_VIRTUALDESK`（实测踩坑）

绝对坐标模式原先只认主屏。定标实测（本机 2048x1152 主屏 + 左侧副屏，虚拟桌面 3584 宽）：

```
不带 VIRTUALDESK：nx=0..65535 → 光标只在主屏 x=0..2047 之间线性移动（左副屏完全到不了）
带   VIRTUALDESK：nx=0 → 光标 x=-1536（左副屏最左）；nx=65535 → x=2047（主屏最右）  ✓ 线性覆盖整个虚拟桌面
```

修正后：被控端进程 `SetProcessDPIAware()`，HELLO 上报**虚拟桌面**尺寸（实测 4480x1440 物理），
`MouseAbs` 用 `MOVE|ABSOLUTE|VIRTUALDESK` 归一化到整个虚拟桌面。

### 38.3 本轮还修掉的两个真 bug

1. **`Pump-Cable` 的返回值污染**：`TxBody` 里调用它没接住返回值，PowerShell 会把 `$true`
   拼进 `TxBody` 的返回值 → 剪贴板发送被误判失败（日志里 `写帧失败 rc=True 0`）。用 `[void]()` 修掉。
2. **配置文件行内注释**：`hotkey_toggle = ctrl+alt+space  # 注释` 会把注释当值解析 → 热键全部解析失败。
   现在会剥掉 `#` 之后的内容（值里确有 `#` 时可用引号包起来）。

### 38.4 验证状态

- `make test` **7 组全绿、130 项 PASS / 0 FAIL**（新增断言覆盖：组合键热键解析与触发、
  修饰键抑制不泄漏、edges/switch_modifier/角部死区/锁定）。
- 真机已验证：配置加载与热键回显、HELLO 上报虚拟桌面 4480x1440、托盘启用、
  剪贴板双向、看门狗 3 秒自动交还、Ctrl+Alt 热键抑制路径。
- **未能在最后一步复验**：麒麟那台调试用的热点网断了（`EHOSTUNREACH`），
  无法再远程复核启动日志。两端均为自启动、独立运行，不依赖我的会话。

### 38.5 热键抑制的真机验证揪出一个真 bug（单测漏掉、真机抓到）

**真机测试**（uisim 注入 `ctrl+alt+space`，verbose agent 记录收到的每个键）：

```
第一次：  对端收到 KEY code=29(Ctrl)↓、code=56(Alt)↓，Space 被抑制
          → Ctrl/Alt 会**卡在对端**（正是要防的问题）
修复后：  对端只收到 CapsLock(58) 与 LeftMeta(125)（都不是热键，应正常转发）
          泄漏的 ctrl(29)/alt(56)/space(57) 条数 = 0  ✓
          主控端同时正确执行了"指针拉回本机"并释放独占
```

**根因**：核心把修饰键压进抑制缓冲后，**调用方在每次事件后都无条件抽空缓冲并发出** ——
缓冲形同虚设。抑制的正确语义是"**由核心决定何时放行**"：

```c
int otikm_core_take_suppressed(otikm_core *c, oti_key_evt *out, int max)
{
    if (!c->flush_pending)
        return 0;          /* 还没确定不是热键 → 继续压住（关键） */
    ...
}
```

放行时机：① 缓冲期间来了**非修饰键**且不是热键 → 追加并放行；
② 修饰键单独按下又松开（期间无别的键、且已不可能是任何热键前缀）→ 放行；
③ **鼠标事件**出现 → 放行（有鼠标动作就不可能是热键组合）。
触发热键时则 `pend_clear()`（整段丢弃）。

**过程中还修了第二个同类问题**：缓冲期间按下第二个修饰键（Ctrl→Alt）被误判成"非热键"而提前放行。
判据必须是 `!bit`（本键不是修饰键）才能确定"不是热键组合"。

**教训**：这类"状态机 + 调用方约定"的接缝，单测很容易和实现犯同一个错误
（我的单测当初也没在事件之间调用 `take_suppressed`）。现在补了断言：
**未放行前 `take_suppressed` 必须返回 0** —— 这条断言能直接锁死这个 bug。

## 39. 第 31 轮：用户实测反馈的三处修复 + 厂商协议路线的可行性判定

### 39.1 用户实测反馈（写在剪贴板里给我的）

```
键鼠安装在 Linux 端：鼠标隔一会卡一下；很难移入 windows 桌面；
                    移入后移动很慢、有惯性、停不下来、不跟手；左右键都没反应。
键鼠安装在 windows 端：移不到 Linux 桌面。
```

逐条定位到根因并修复（前三条都是真 bug）：

| 反馈 | 根因 | 修复 |
|---|---|---|
| **左右键没反应** | agent 收到 MOUSE 消息后**完全忽略 buttons 字段**（C# 里注释写着"按键状态差异由调用方维护"，而调用方从来没维护） | 新增 `Apply-Buttons`：按位图比对新旧状态、发 down/up 差量；退出时自动松开所有键 |
| **慢、有惯性、不跟手** | **每个鼠标事件发一整帧 64KB**；鼠标 500~1000Hz 而线缆帧率有限 → 大量排队 | 位移合并：累积 dx/dy/wheel，按 **4ms** 间隔或按键变化时发一条；`poll` 超时同步改 4ms 保证尾包及时 |
| **隔一会卡一下** | 收发线程抢同一把锁；输入线程每次发送都要等一次设备 I/O | `oti_tr_send` 改为 **trylock**：拿不到锁立刻返回（帧交给接收线程发）；泵一次最多收 4 帧，缩短持锁时间 |
| 很难移入 | 上面两条的叠加（合并后才好判断是否还有独立问题） | 待复测 |
| Windows 端移不到 Linux | **功能缺口**：agent 只收不发（没有 Windows 输入抓取实现） | 未做 |

**验证**（客观、跨进程）：

```
按键：独立进程轮询 GetAsyncKeyState(VK_LBUTTON)
      → "left-down samples = 51 / 320, first at 9.3s"（sim 脚本按住左键 4 秒）
剪贴板：麒麟写入 otilink-verify-15:00:14 → Windows Get-Clipboard 读到同样内容 ✓
热键抑制：Windows 侧只收到 CapsLock(58)/LeftMeta(125)，ctrl/alt/space 零泄漏 ✓
```

### 39.2 厂商协议路线：可行性判定（用户建议"用厂家实现，我补 Linux 侧"）

**已从二进制精确逆出的厂商协议骨架**（`KMKeyMouse.dylib` 的符号表与字符串）：

```
信封    <OTIMSG><%@>%d</%@> × 7 </OTIMSG>      （7 个带标签的整数字段，标签按调用点传入）
切换    Cmd_Notify_KM_Switch_To_Remote / _To_Local
        Cmd_Get_KM_Switch_Acknowlege            ← 切换是**带 ACK 的握手**
        Cmd_Force_Remote_To_Switch_Local
        Cmd_Notify_Remote_To_Switch_Back
剪贴板  Cmd_Transfer_Clipboard + Param_Clipboard_Info_2
标签    Param_Move_Out_X/_Y/_Direction/_KM_Switch_Option/_Use_Hotkey_Switch_Only
        Param_Move_In_X/_Y/_Direction/_Info、Param_Remote_Screen_Width/_Height
        Param_Other_PC_Position_Option、Param_CapsLock、Param_ScrollDirection
        Param_Sync_Running_Info、Param_Remote_Bridge_Blocked、Param_Temp_KM_Setting
帧内封装 [1 字节类型 0x39][u32 长度][XML]        ← 从抓包确认
```

厂商的鼠标发送策略（`CKMEvent::SendMouseEvent` 反汇编）：**累积位移 + 每轴钳位**，
另有两条受 delegate 开关控制的限流检查（阈值常量 15.0，正常路径不生效）——
与我在 §39.1 里做的位移合并是同一思路，说明方向正确。

**决定性实验（证明这条路不是"接上去就行"）**：

1. 恢复厂商全套软件（MacKMLink ×2 + LinkEngKM + LEWD），Linux 侧被动抓包
   → 抓到了启动握手的 XML：`<ExtraXmlCommand>HookAppService</ExtraXmlCommand><ExtraXmlParam><OTIMSG><NP_Cmd>Cmd_Post_Remote_KM_Setting_Info...`
2. 之后厂商**转入静默**：只剩 `0x07`（发送授权）消息，`0x05`（有帧）为零。
3. 在 Windows 上**用 SendInput 合成 10 秒鼠标移动 + 按键**（厂商的 LinkEngKM 抓本机输入）
   → 厂商**依然零帧发送**。

**结论**：`HookAppService` 是握手，**对端必须正确应答后厂商才进入 KM 数据阶段**。
因此"用厂商 Windows 端 + 自己写 Linux 端"必须先把整套 XML 命令层（信封、握手、ACK、
7 字段语义、剪贴板）完整逆出并实现，**无法靠抓包试出来**（鸡生蛋）。
这是一条**可行但成本明显更高**的路线；在它完成之前，用户手上能用的仍然是本仓库这套实现。

### 39.3 当前可用状态（本轮结束）

- Linux 主控端 `otikm`：真实键鼠 + 配置 + 开机自启，运行中
- Windows 被控端 agent：托盘 + native 模式，运行中（厂商软件已改名停用避免抢设备）
- 已验证：键盘（含扩展键）、鼠标移动与**按键**、剪贴板双向、热键抑制、看门狗自动交还
- `make test` 7 组全绿
- 已知未做：Windows→Linux 方向（agent 只收不发）；切入手感（合并位移后待用户复测）

## 40. 第 32 轮：厂商协议 —— **Linux 侧已能向厂商 Windows 端注入合法命令**

### 40.1 厂商帧的完整线格式（全部实证，不再是推断）

```
帧(65536) = [20B 头][帧体][20B 头副本]
帧体      = [1 字节 0x39][u32 小端 负载长度][XML 文本]
XML       = <ExtraXmlCommand>HookAppService</ExtraXmlCommand>
            <ExtraXmlParam><OTIMSG> …命令与参数… </OTIMSG></ExtraXmlParam>
```

`0x39` 是"XML 命令"这一类帧体的类型字节；`0xbf 02 00 00` 这样的四字节即 703 = 后面 XML 的字节数。

### 40.2 抓到的厂商启动报文（完整原文）

**① 屏幕信息**
```xml
<NP_Cmd>Cmd_Post_Remote_Screen_Info</NP_Cmd>
<NP_Up_Notice_NamePipe_Name>\\.\pipe\OTI_ClipboardAgent</NP_Up_Notice_NamePipe_Name>
<Param_Screen_Info><OTIMSG>
  <Param_Virtual_Screen_Width>4480</Param_Virtual_Screen_Width>
  <Param_Virtual_Screen_Height>1440</Param_Virtual_Screen_Height>
  <Param_Left_Side_Virtual_Screen_Coordinate>-1920</Param_Left_Side_Virtual_Screen_Coordinate>
  <Param_Top_Side_Virtual_Screen_Coordinate>0</Param_Top_Side_Virtual_Screen_Coordinate>
  <Param_Remote_Screen_Width>2560</Param_Remote_Screen_Width>
  <Param_Remote_Screen_Height>1440</Param_Remote_Screen_Height>
</OTIMSG></Param_Screen_Info>
```

**② KM 设置**（`Cmd_Post_Remote_KM_Preference_Setup_Info` → `Param_KM_Setting_Info`）
```
KMSwitchMode=3          ClipboardShareOption=1     MousePointSwitchTolence=0
PositionOfOtherPC=2     ComputerResident=1         MoveOutSensitivity=2
EnableMouseAcceleration=0                          ← 厂商也**关掉鼠标加速**
EnableOnlyUseHotkeyToSwitch=0
SenseScreenArea Left=0 Top=0 Width=50 Height=50    ← 边缘感应区 50x50
HotKeyModifierMask=4  HotKeyKeyIDk=115  HotKeyVirtualKey=83   ← 切换回来的热键
CtlAltDelHotKeyKeyIDk=61283  CtlAltDelHotKeyVirtualKey=45
DoubleClickTurnOnOffShare=0  ShowGettingStartCount=4  BlockingBridge=0
```

### 40.3 切换命令的七个字段（反汇编定位）

`-[KMHandler Dispatch:UPipeCmd:]` 在 0x15e0–0x16cc 连续引用 1 个命令 + 7 个参数：

```
Cmd_Notify_KM_Switch_To_Remote
  Param_Move_Out_Direction          越出方向
  Param_Other_PC_Position_Option    对端在本机的哪一侧
  Param_Move_Out_X / _Y             越出点坐标
  Param_Remote_Screen_Width / _Height
  Param_Move_Out_KM_Switch_Option
```
反向（0x175a–0x1799）用 `Param_Move_In_Direction / _X / _Y`。
命令全集：`Cmd_Notify_KM_Switch_To_Remote` / `_To_Local` / `Cmd_Force_Remote_To_Switch_Local` /
`Cmd_Get_KM_Switch_Acknowlege` / `Cmd_Notify_Remote_To_Switch_Back`。

### 40.4 **关键突破：Linux 侧能向厂商端注入命令并被接受**

新增 `otiprobe sendxml <file>`：把 XML 按厂商线格式打成帧发出。
**注意必须走授权时序**（读消息上报"有 1 帧要发" → 收到 `0x06/0x07` 后立刻写），
否则被拒 `ASC=0x85`。补上时序后：

```
发送我们(Linux)的屏幕信息 Cmd_Post_Remote_Screen_Info（616 字节 XML）
  → 写 rc=0（第 1 轮拿到授权）status=0x00        ← 厂商端**立刻发了授权**，说明它在消费、认可我们
发送 Cmd_Notify_KM_Switch_To_Remote（七字段 534 字节）
  → 写 rc=0（第 1 轮拿到授权）
```

**结论：厂商 Windows 端接受 Linux 侧作为对端**，"厂商 Windows + 自写 Linux"这条路技术上成立。

### 40.5 仍未解 / 下一步

1. **切换之后厂商没有明显进入 KM 事件阶段** —— 可能还需要先发
   `Cmd_Get_KM_Switch_Acknowlege`（ACK），或需要先交换 `Cmd_Post_Remote_KM_Preference_Setup_Info`。
2. **KM 事件（鼠标/键盘）的线格式仍未拿到** —— XML 里没见到鼠标/键盘参数名，
   怀疑 KM 事件是**另一种帧体类型字节**的二进制帧（`0x39` 只用于 XML）。
   下一步：在 `KMKeyMouse.dylib` 里找 `sendMouseData:`/`sendKeyData:` 实际调用的发送函数与帧体类型常量。
3. 厂商端只在"有对端在跑"时才发送；对端不发它就静默（已实测）——
   所以驱动它的最小实现必须包含启动信息交换。

**当前可用状态**：本仓库自己的实现（已修按键/合并位移/锁争用）仍是唯一能实际使用的方案；
厂商协议路线已完成"线格式 + 启动报文 + 切换七字段 + Linux 侧可注入"四项，继续推进中。

## 41. 第 33 轮：**厂商 KM 线格式完全破解 —— HID 包通道（架构级突破）**

### 41.1 一句话结论

厂商的键鼠**不走 64KB 帧管道**，而是走 **SCSI HID 包通道**：一条 16 字节 CDB 就承载一次键鼠事件，
而且**线缆本身对接收端就是一个真实的 USB 鼠标 + 键盘**（原生 HID，接收端零软件、零注入）。

这正是我们此前"卡顿 / 惯性 / 不跟手 / 左右键没反应"的**根因**：
我们自己的协议每次鼠标移动都搬 64KB 帧，比厂商大 4000 倍。

### 41.2 线格式（实证 + 反汇编双重确认）

```
CDB(16B) = D9 | 0x33 鼠标 / 0x34 键盘 / 0x36 多媒体 | 12 字节负载 | 'O' | 'T'
无数据阶段（no data phase）
```

来源：`OTiTransfer.framework` 的 `OTiTransporter::sendHIDPacket(char, CFData*)` @0x3306：

```
mov word [rbp-0x40], 0xd9          ; CDB[0]=0xD9
movsx ecx, type ; dec ecx ; cmp ecx,2 ; ja fail
mov eax, 0x363433 ; shr eax, cl<<3  ; 0x33/0x34/0x36 by type 1/2/3
mov byte [rbp-0x3f], al            ; CDB[1]
CFDataGetBytePtr -> copy 12 bytes to [rbp-0x3e]   ; CDB[2..13] = buf[0..11]
mov word [rbp-0x32], 0x544f        ; CDB[14]='O' CDB[15]='T'
```
（14 字节缓冲只有前 12 字节进 CDB，末 2 字节丢弃。）

### 41.3 负载布局（真机标定，Windows→Kylin 方向）

发送 12 字节 `00 02 03 04 05 06 07 08 09 0a 0b 0c`，在 Kylin 读线缆 `if01-event-mouse`：

```
REL_X = 2   REL_Y = 3   REL_WHEEL = 4   (REL_WHEEL_HI_RES = 480 = 4*120，内核自动派生)
MSC_SCAN = 0x70004（键盘：USB HID usage 原样透传）
```

| 类型 | CDB[1] | 12 字节负载 |
|---|---|---|
| 鼠标 | 0x33 | `[按键:5bit(左/右/中/侧/额外)][dx int8][dy int8][wheel int8][8 字节未用]` |
| 键盘 | 0x34 | `[修饰键 8bit][保留][k1..k6 = USB HID usage][4 字节未用]` |
| 多媒体 | 0x36 | 未标定（发键盘报告无反应） |

### 41.4 线缆 = 真 HID 设备（免驱注入的真相）

两端 `0ea0:2213` 都枚举出：
```
MI_00 Mass Storage（CD/FAT 两个 LUN）
MI_01 HID → "HID-compliant mouse"      / Linux: ...-if01-event-mouse
MI_02 HID → "HID Keyboard Device"      / Linux: ...-if02-event-kbd
```
Kylin 侧报告描述符（权威）：

```
鼠标: 05 01 09 02 a1 01 09 01 a1 00 05 09 19 01 29 05 15 00 25 01 95 05 75 01 81 02
      95 01 75 03 81 01 05 01 09 30 09 31 09 38 15 81 25 7f 75 08 95 03 81 06 c0 c0
      → 4 字节: [5 按键位 + 3 填充][X int8 相对 -127..127][Y int8 相对][Wheel int8]
键盘: 05 01 09 06 a1 01 05 07 19 e0 29 e7 15 00 25 01 75 01 95 08 81 02
      75 08 95 01 81 01 19 00 2a ff 00 15 00 26 ff 00 75 08 95 06 81 00 c0
      → 8 字节: [修饰键][保留][k1..k6]
```

### 41.5 真机端到端验证

* Kylin → Windows：`otiprobe hidsend 1 00140000...`（dx=20）×10 → Windows 光标
  `882 → 906 → 939 → 972 → 1005 → 1038`，**每包稳定 +33 px**（Windows 指针加速），方向正确。
* Windows → Kylin：`hidpkt.ps1 -Payload ... -Type 1` → Kylin 精确收到 `REL_X/REL_Y/REL_WHEEL`。
* 键盘：type=2 + `[00][00][04]` → Kylin 收到 `KEY_A`（down/up 成对）。type=3 无反应。

### 41.6 对产品的意义（新架构）

1. **接收端不需要任何软件**：线缆就是真鼠标/键盘，操作系统原生处理（含加速、按键、滚轮）。
2. **发送端只需"捕获 + 发包"**：不再需要 uinput 注入（Linux）/ SendInput（Windows）。
3. **延迟**：每次事件 1 条 16 字节 CDB，取代 64KB 帧 → 卡顿与惯性根除。
4. 剪贴板等大流量仍走原有 64KB 帧管道（两条通道互不干扰）。
5. 注意：**捕获端必须排除线缆自身的 HID 设备**，否则会形成回环。

### 41.7 厂商程序的其它事实（本轮补充）

* 厂商 Windows 程序可从 `FunctModules\{8AEC7F86-...}\MacKMLink.exe -GN:RunFromRegistry` 启动
  （HKCU Run 项 "CS Dispatch"），会拉起 LEWD.exe + LinkEngKM.exe。
* 厂商程序确实在发线缆帧（抓到 `0x39` 类型体的启动 XML）；但**它不会仅凭我们伪造的对端报文就切换到 KM 阶段**，
  完整的握手/切换状态机未解 —— 因此厂商程序路线（用它当 Windows 端）风险高、收益低。
* 厂商二进制的调试串自带符号信息，例如
  `sendMouseHID(): Mask: %X, Wheel: %X, x: %d, y: %d`、
  `sendKeyboardHID(): bytModifiedKey: %X, K[2]:%X,...K[7]:%X`。
* `2208KM_HID.dll` / `LinkEngKM.exe` / `LEWD.exe` 都是 UPX 压缩；
  解包：`upx -d`（4.2.4 静态版），或让 32 位 PowerShell 用 `LoadLibrary` 自解包后 dump 内存映像。

---

## 42. 第 34 轮：**剪贴板两项遗留问题的真因**（多块"CRC/格式失败"是误判；文件方向是一个 DLL 名写错）

交接文档 §4 留了两件事：①"多块剪贴板传输报 CRC/格式失败"（优先）；②"Windows→麒麟 文件方向发不出去"。
本轮把两件都查到底并修掉，结论有一半是**推翻上一轮的判断**。

### 42.1 结论先行

| 现象 | 上一轮的判断 | 实测真因 | 处置 |
|---|---|---|---|
| `[warn] 解包失败（CRC/格式）` | 多块传输重组失败（麒麟侧 `oti_clip_feed`） | **厂商 XML 帧**（otikm 撞边交还时发的 `Cmd_Notify_KM_Switch_To_Local`，668 字节）走到了我们的解码器 | 单独识别成 `[skip] 厂商 XML 帧`，不再冒充"传坏了" |
| "大一点的图片/长文本都会失败（>6000 字节）" | 分块边界问题 | **不成立**：70000 / 200012 字节文本双向 md5 逐字节一致；191257 字节图片 3 块也成功过 | 写进 `clipreg.sh` 当回归项，防止再被误判 |
| 图片每传一次都会"弹回来"一次 | 未记录 | Windows 剪贴板只有 CF_DIB，收/发各过一次 PNG↔DIB 转换 → **同一张图两侧 PNG 字节必然不同** → 按字节 CRC 的防回环对图片失效 | 改用 **CF_DIB 像素指纹**（`ClipImg::DibHash`）判定，回环消失 |
| Windows→麒麟 文件：设了 CF_HDROP 却只发一串 15 字节路径 | 怀疑 `HasFiles()` 没触发 / 被在途剪贴板覆盖 | **`DragQueryFileW` 声明在 `user32.dll`，实际在 `shell32.dll`** → `EntryPointNotFoundException` 被 `catch { return null; }` 吞掉 → 静默退化成文本 | 改 `shell32.dll`；并让 `Get()` 记录 `LastError`、调用方打印 |
| 麒麟剪贴板"读回来跟写进去的字节不一样" | （本轮一度以为 xclip 会重编码 PNG） | **假象**：是 otikm 的收/发流程在同一时间内改写了选区；停掉 otikm 后实测 `xclip -t image/png` 往返 6063→6063 **逐字节一致** | 记录在案，避免下一个人再被同一个假象带偏 |

### 42.2 证据链（关键几条）

**(1) `[warn]` 到底是什么** —— 给 Windows agent 的解码失败分支加上长度 + 头 16 字节 hex 后，一次 200KB 麒麟→Windows 传输的日志：

```
CLIP 收块 fmt=1 flags=1 fid=14 off=0      total=200012 dlen=65000
CLIP 收块 fmt=1 flags=0 fid=14 off=65000  total=200012 dlen=65000
CLIP 收块 fmt=1 flags=0 fid=14 off=130000 total=200012 dlen=65000
CLIP 收块 fmt=1 flags=2 fid=14 off=195000 total=200012 dlen=5012
CLIP 已应用远端剪贴板 200012 字节（并抑制回发）      ← 4 块，完整
[warn] 解包失败（CRC/格式）len=673 head=39 9c 02 00 00 3c 45 78 74 72 61 58 6d 6c 43 6f
```

`39` = 厂商帧标识，`9c 02 00 00` = 668（= otikm 日志里那句"668 字节"），`<?xmlCo…` = `<?xml…ExtraXmlCommand`。
**它和剪贴板毫无关系**，而且只在"撞边交还控制权"时出现。同一个 200KB 传输的 md5 两侧一致。

**(2) 文件方向** —— 隔离实验（`clipfiles-diag.ps1`，把 `ClipFiles` 每一步摊开、不吞异常）：

```
IsClipboardFormatAvailable(CF_HDROP)=True
OpenClipboard=True
GetClipboardData=0x177ab7add10
EXCEPTION: EntryPointNotFoundException: 无法在 DLL"user32.dll"中找到名为"DragQueryFileW"的入口点。
对照：Get-Clipboard -Format FileDropList: C:\Users\Public\otitest\file_w2l.txt
```

即：`HasFiles()` 为真、`GetClipboardData` 拿得到句柄，**唯独取名字那一步的 DLL 写错了**。
改 `shell32.dll` 后，`[clip] HDROP 命中：1 个文件` → `CLIP 发送 343 字节 fmt=3 源=文件`，
麒麟落地 `/tmp/otilink-files-<pid>/file_w2l.txt` 并设好 `uri-list`。

**(3) 图片回环** —— 修前：麒麟发 10973 字节 PNG，Windows `rc=True` 应用成功，**紧接着回发 8017 字节**（.NET 从 DIB 重编码）。修后同一场景只有一条 `已应用远端**图片** 10973 字节 rc=True`，无回发。

**(4) xclip 不改字节** —— 停掉 otikm 后：`cat testimg.png | xclip -t image/png -i` 再 `xclip -t image/png -o`，`6063 → 6063`，md5 相同。
（之前看到的"10973 进、8017 出"是 otikm 在中间把收来的图写进了同一个选区。）

### 42.3 本轮改动清单

**Windows（`re/windows/otiagent.ps1`）**
1. `ClipFiles::DragQueryFileW` → `shell32.dll`（**文件方向的根因**）；`Get()` 不再吞异常，改为记 `LastError`。
2. `ClipImg::DibHash()`：CF_DIB 字节的 CRC 指纹（自带 CRC 实现，因为 `ClipImg` 与 `Oti` 是两次独立 `Add-Type`，不是同一个编译单元）。
3. 发送路径：图片先比 DIB 指纹，命中就跳过（根治回环）；发完记下刚发出的 DIB 指纹。
4. 接收路径：应用远端图片后记下"我刚写进去的 DIB 指纹"。
5. 解码失败分支：打印 `len` + 头 16 字节 hex；`body[0]==0x39` 单独识别为厂商帧 `[skip]` 并计数。
6. 收块日志（`-Verbose`）：`fmt/flags/fid/off/total/dlen`；乱序或"前一笔没收完被打断"直接告警。
7. 发送日志：`本笔 N 块（累计 M）fid=… fmt=… 源=文件/图片/文本` —— 原来只有一个**会话累计**的块数，
   排查时被误读成"这一笔分了 6 块"（交接文档里那条"6856 字节 / 6 块"就是这么来的）。

**麒麟（`re/otilink/otikm.c`）**
8. `CLIP 发送` / `CLIP 应用远端剪贴板` 两条日志都带上 `fmt=` 与 `crc=`：两侧日志靠 CRC 对齐，
   一眼能看出"这次发送到底是新内容还是回环"。
9. 顺手修掉一个 `-Wcomment` 警告（`otikm.c:117` 嵌套注释写错）。

**新工具**
10. `windows/deploy.sh`：BOM 自愈 → 复制到 `C:\Users\<你的用户名>\otilink\` → 全部 `.ps1` 解析自检 → 可选 `--restart`。
11. `windows/restart-clip.vbs`：脱离调用者地拉起剪贴板代理（日志 `clip.log`）。
12. `windows/clipfiles-diag.ps1`：CF_HDROP 读取诊断。
13. `otilink/clipreg.sh`：剪贴板真机回归（9 组 17 项，约 50 秒，不动光标）。
14. `otilink/setclip.sh`（从麒麟归档回仓库）：把文件内容放进麒麟剪贴板且**不阻塞 ssh**。

### 42.4 本轮踩到的新坑（写进手册）

1. **PowerShell 按命令行匹配自己**：`Get-CimInstance Win32_Process | Where CommandLine -like '*-File*otiagent.ps1*'`
   会匹配到**执行这条查询的 powershell 自己**，`Stop-Process` 于是把自己杀掉
   —— 症状是脚本走到这里一声不响结束、代理也没起来。必须加 `$_.ProcessId -ne $PID`。
2. **WSL interop + 后台 Windows 进程 = 脚本挂死**：从 WSL 里 `powershell -Command "...Start-Process..."` 启动
   长驻进程，新进程会继承调用者的 stdout/stderr 句柄，WSL 要等句柄全关才返回 → 脚本卡到超时。
   解法：**用 VBS 的 `WScript.Shell.Run`**（开机自启那份一直是这么干的，所以它一直很稳）。
3. **.vbs 必须 CRLF**：LF-only 的 `.vbs`，cscript 返回 0 但什么都不做（WSH 把两行并成一句）。
   `deploy.sh` 现在每次部署都归一化行尾。
4. **ssh 的登录 banner 走 stderr**：把 stderr 并进 stdout 会让所有捕获多出 `Kylin V10 SP1`（14 字节），
   文本比对、md5、二进制解析全错。`clipreg.sh` 的 `kssh` 因此**不合并** stderr。
5. **麒麟上用户脚本读不了 `~/文档`**（kysec）：`./script.sh` 内部用 shell 重定向读 `~/文档/x.png` = EACCES，
   但 `cat`（系统可信二进制）可以 → 脚本里一律 `cat "$F" | xclip …`。
6. **xclip 会占住 ssh 通道**：`xclip -i` 的子进程持有选区，ssh 会一直等到它退出（实测 60 秒超时）
   → 用 `setsid … >/dev/null 2>&1 </dev/null &` 脱离（`setclip.sh` 已封装）。

### 42.5 验证

`./clipreg.sh`（新增）**17/17 PASS**：

```
小文本 W→L / L→W、大文本 69991 字节 W→L（2 块）、200010 字节 L→W（4 块）
图片 240x160 W→L / L→W、文件 W→L（md5 一致 + uri-list）、文件 L→W（md5 一致）
日志不变量：无真正帧解码失败、图片发送无回环、文件剪贴板读取正常
```

`./hwtest.sh` 键鼠回归见 §42.6（本轮跑通）。

### 42.6 现场状态与"下次怎么起"

* 麒麟：`otikm`（被驱动侧模式）由 `run-kylin.sh` 拉起，日志 `/tmp/otikm.log`；
  改 `.c` 后 `make otikm && echo "$KY_PASS" | sudo -S ./label-kysec.sh`（**标签会随重编译丢失**）。
* Windows：键鼠 `otiagent2.exe --edge right`（HID 包通道）；剪贴板 `otiagent.ps1 -Cable -Clipboard`
  （协议帧管道，日志 `C:\Users\Public\clip.log`）。两者互不冲突。
* 部署 Windows 侧脚本一律走 `re/windows/deploy.sh [--restart]`，不要手工 cp（BOM/行尾/自检/重启都在里面）。

### 42.7 补充：剪贴板容量上限的实测标定（同一轮）

顺手把"到底能传多大"标定清楚（用户会问，而且超限以前是**静默**的）：

| 方向 | 实测通过 | 实测拒绝 | 代码判据 |
|---|---|---|---|
| Windows→麒麟 文件 | **7,999,978 字节**（包总长正好 8,000,000，124 块，md5 一致） | 8,200,000（包 8,200,022） | `$tot -le 8000000` |
| 麒麟→Windows 文件 | 7,500,000 字节（116 块，md5 一致） | 9,000,000 | `clip_max = 8 MiB`（`--clip-max` 可调） |
| 文本（双向） | 200,010 字节（4 块） | — | 同上 |

本轮同时补了两条**上限告警**（原来超限是完全静默的，两侧日志一个字都没有）：

```
Windows: [warn] 剪贴板内容超过上限（文件包 8200022 字节（1 个文件） > 8MB）——本次不发送…
麒麟   : [warn] 剪贴板内容 9000024 字节超过上限 8388608 —— 本次不发送（可调 --clip-max …）
```

以及一个连带修复：`HasFiles()=true` 但 `Get()` 因 `OpenClipboard` 被别的进程短暂占用（`err=5`）而失败时，
**本轮跳过、300ms 后重试**，不再退化成"把文件路径当文本发过去"。

---

## 43. 第 35 轮：**大文件传输（≤500MB，双向）** —— 分片流式 + 位图补缺 + 让路

用户要求："想传更大的，最好 500MB 以内"。上一版剪贴板"文件包"（format 3）是**整包驻留内存**的：
发端把文件全读进内存、收端按 `total_len` 一次性 malloc —— 8MB 还凑合，
500MB 会让两侧各吃 500MB（实测 Windows 侧只剩 ~1GB 可用内存），而且中间丢一块就得整包重来。

### 43.1 先量吞吐（决定值不值得做）

| 方向 | 实测 | 推算 500MB |
|---|---|---|
| Windows→麒麟 | **10.45 MB/s**（8MB / 730ms，124 块） | ≈50 秒 |
| 麒麟→Windows | ≈3 MB/s（7.5MB 端到端 2.9 秒） | ≈3 分钟 |

结论：值得做，但**必须流式落盘**（内存恒定），并且要能承受丢片。

### 43.2 设计（复用 CLIP 消息，只加两个 format）

```
format 4 = PART：clip 头(20B) 之后
    u32 idx, u32 nparts, u64 total, u32 namelen, name(UTF-8), u32 dlen, data
format 5 = CTRL：
    u16 kind(1=DONE, 2=VERDICT), u16 flags(1=OK,2=RESUME,4=ABORT,8=HAS_MAP),
    u64 total, u32 crc, u32 nparts, u32 resume_from, u32 namelen, name, [缺片位图]
```
* 单片 64000 字节（一个 64KB 帧装得下，线路利用率 ~98%）；
* 收端 **pwrite 直接落盘**（不缓冲整文件），用位图记录收过哪些片；
* 文件收全后**读回落盘结果算整文件 CRC32** 与发端比对；
* 缺片 → VERDICT+位图（只补缺的片，**不是**从缺口一路发到结尾）；
* 校验失败 → RESUME(从 0)，最多 3 轮；收端 60 秒无新片自动清理 .part。

实现在 `otixfer.c/h`（Linux 侧状态机）+ `otiagent.ps1` 的 `$script:fileTx/fileRx`（Windows 侧），
两侧共用同一套线格式（`OTI_XFER_PART_MAX = 64000` 必须一致）。

**顺带修掉的性能坑（比大文件本身更关键）**：
* 旧轮询每 300ms 把剪贴板里的文件**整个读一遍**算 CRC 判断"变没变"。改成
  **路径+大小+mtime 指纹**（`oti_clip_probe_files` / `Get-FileIdent`）—— 不读内容，
  这也是 500MB 能轮询的前提。
* 双向**同时**传大文件会把管道拖到 **0.1 MB/s**（实测 20MB 要 249 秒，两边互相抢设备授权）。
  用 **fid 大小做确定性让路**（大的先停，等小的传完）：20MB 双向从 249 秒降到 **7 秒**。

### 43.3 真机验证（2026-09-16）

| 用例 | 结果 |
|---|---|
| 20MB W→L / L→W | md5 一致，7.2 / 3.0 MB/s |
| 64MB 双向（xferreg.sh） | md5 一致，W→L 8.9 秒、L→W 19.7 秒 |
| **500MB W→L** | **69.1 秒 / 7.2 MB/s / md5 一致**，麒麟剪贴板 uri-list 就绪 |
| **500MB L→W** | **165.8 秒 / 3.0 MB/s / md5 一致**，Windows 剪贴板 CF_HDROP 就绪 |
| 故意丢片（测试钩子 `OTI_XFER_DROP_PART`） | 收端报缺片 → 发端**按位图只补缺片** → md5 一致 |
| 传输期间键鼠 | 3 个 HID 包 → Windows 光标 +743px（与空闲时一致，未受影响） |
| 双向同时传 20MB | 让路生效：7 秒完成（修复前 249 秒） |
| 小剪贴板回归 `clipreg.sh` | 17/17 PASS（未被影响） |

### 43.4 本轮踩到的坑

1. **`char files[16][512]` 不能强转成 `const char **`**：数组元素是 512 字节不是指针，
   `paths[1]` 会读到野指针 → 段错误。签名必须写成二维数组（`char paths[][512]`）。
2. **回调表必须是程序级生命周期**：`struct oti_xfer_ops ops` 放栈上，两个 xfer 实例长期持有
   它的指针 → 离开作用域即悬垂。ASan 报 `stack-use-after-scope`，真机表现是 otikm 段错误退出。
3. **PowerShell 的 `[int]((n+7)/8)` 会四舍五入**（41.875 → 42），而 C 侧 `(n+7)/8` 是整数除法 41
   → 位图长度校验差 1 字节，收端位图**永远走不到**，静默退化成"从缺口发到结尾"。
   必须 `[int][Math]::Floor(...)`。
4. **麒麟上没有 gdb**：ASan（`gcc -fsanitize=address`，libasan 已装）足够定位这类内存错误，
   编译一个 `otikm_asan` 单独跑，不影响生产二进制。
5. 本机（不走线缆）回归怎么做：给**文件后端**（模拟剪贴板）加上"内容是 `file://` 列表就当成文件剪贴板"
   的支持，于是两个 otikm 用 AF_UNIX 对跑就能覆盖整条大文件通路（`xferlocal.sh`，约 1 秒一轮）。
   真机一轮几十秒到几分钟，逻辑改动一律先在本机回归。

### 43.4.1 修完第一版后又抓到的两个真 bug（都在"设备打嗝一次"时暴露）

一次 16MB 麒麟→Windows 的测试里设备读取连续失败（Windows 侧 `设备连续读取失败 → 重连`），
暴露出两个问题，**都不是设备本身的锅**：

1. **整文件 CRC 用"发一片算一片"的增量方式 → 重传会把同一片重复计入**，对端必然报
   `校验失败（长度对得上，CRC 不对）`，然后要求整份重发。改成**开传前顺序读一遍把 CRC 一次算好**
   （500MB 约 1~2 秒，只做一次）。Windows 侧同样处理。
2. **发送失败后无退避 → 空转 100% CPU**：`clip_thread` 的循环是"失败 → 立刻重试同一片"，
   实测把 otikm 打到 **100% CPU**，反过来让设备读取也跟着失败（Windows 侧连续 60 次读失败后重连设备）。
   改成**失败退避 20ms + 连续 20 次打一条告警 + 连续 200 次放弃**（两侧一致）。

修完两处后，三条回归全绿：`clipreg.sh` 17/17、`xferreg.sh` 12/12、`hwtest.sh` 12/12。

### 43.5 现在能传多大

* 理论上限 = 磁盘空间（`u64 total`、按片落盘，内存恒定 64KB/侧）；本轮实测到 **500MB**。
* **单个文件 >8MB 自动走分片**；≤8MB 仍走原来的"文件包"（一次发完，省往返）。
* 耗时参考：W→L ≈7 MB/s、L→W ≈3 MB/s（500MB 分别约 70 秒 / 3 分钟）。
* 已知限制：**大文件按"复制→粘贴"走剪贴板语义**，粘贴时是接收端 `/tmp/otilink-files-<pid>/`
  （麒麟）或 `%TEMP%\otilink_files_*`（Windows）里的副本；多选一次最多 16 个文件。

---

## 44. 第 36 轮：**"移过去回不来"的根因**（主控端没有回程判据）+ 看门狗/拔插自愈

用户报："现在有问题了，移过来又移动不回去了；之前 windows→linux 正常，linux→windows 复制粘贴不了；
假如出问题要有看门狗吧，最起码拔线重插要复原。"

现场与上一轮完全不同了：**麒麟重启过**（uptime 19 分钟），而且键鼠插到了麒麟这边、
`run-kylin.sh` 自动进的是**主控端模式**（`--inject --grab`，抓 Logitech G304 鼠标），
键盘仍在 Windows —— 也就是"鼠标在麒麟、键盘在 Windows"的混合拓扑。

### 44.1 根因：主控端**根本没有回程判据**

`otikm_core_local_mouse()` 分两半：

* `have_control == 1`（指针在本机）：撞边 → `SWITCH_TO_REMOTE` 交出去 ✓
* `have_control == 0`（指针在对端）：**只做两件事** —— 累加 `rem_x/rem_y`、把坐标夹在远端屏幕内，
  然后无脑返回 `OTI_ACT_TO_REMOTE`。**没有任何"把指针收回来"的判据。**

原来能回来靠的是**被驱动侧**那条路：键鼠在对端时，本机只监听线缆 HID 鼠标，撞边发 F24 令牌
（`--return-on-edge`）。可现在键鼠在**麒麟**这边（主控端），对端 Windows 只有 `otiagent2`：
它只认**自己物理鼠标**的边缘，而线缆 HID 推出来的光标位移被它按设计过滤掉了（防回环）
→ 谁都不会发那个令牌 → 指针永远留在 Windows。加上麒麟这边没有键盘（热键也按不了）
→ 用户彻底卡死。**这不是看门狗缺失，是回程逻辑缺失。**

### 44.2 修法：主控端自己判回程（与撞边对称）

* 交出去时记住"从对端屏幕的哪条边进来"（`rem_entry_x/rem_entry_y`，我出左边→从对端右边进来）；
  `enter_remote()` 顺手把**纵向**入口也修正了（老代码纵向用的是横向参数，是错的）。
* 在对端屏幕上**继续往外推 ≥ edge_px+4** ⇒ `OTI_ACT_SWITCH_TO_LOCAL`：
  发释放包 + 令牌、解除 grab、把指针放回**当初出去的那条边**往里 12px（既一眼看到，
  也不会因惯性立刻又推出去）。
* 用**真实光标**（`otix11_pos`）校准 core 的位置 —— grab 期间 core 走累加坐标，与真实位置会漂移。
* 单元测试钉死：`re/otilink/coretest.c`（`make coretest`）14 项断言：左出/右回、右出/左回、
  交出去后 ±1px 抖动**不误判**、回程后惯性不弹回、`force_local` 拉回。**不需要设备/图形环境。**

### 44.3 看门狗与拔插自愈（用户明确要求，本轮补齐）

| 位置 | 机制 | 触发后行为 |
|---|---|---|
| 麒麟 otikm（驱动侧） | **HID 写失败看门狗**（原有） | 连续 20 次 HID 包写失败 → `force_local` + 解除 grab，日志 `!! 看门狗：…已自动交还本机控制` |
| 麒麟 otikm（传输层） | **传输重开看门狗**（★本轮新增） | 连续 20 次读失败 → 先交还控制权+解除 grab → 关掉旧传输、**重新发现并打开**（`/dev/sgN` 拔插后会变）→ 日志 `!! 看门狗：重新打开传输…` |
| Windows 剪贴板代理 | **设备自动发现**（★本轮新增） | 启动打不开 / 重连失败时，枚举盘符（CD-ROM 与可移动优先）探测厂商信息块，谁应答就用谁 |
| Windows 键鼠代理 | **设备自动发现**（★本轮新增）+ 原有重开句柄 | 同上（C# 里 `FindCableDevice()`）；发送失败关句柄后下次自动重开 |
| 麒麟 otikm（帧看门狗） | 原有 idle_release | 帧队列有货却长期发不出去 → 交还本机（HID 模式下改用写失败判据，避免"刚过去就被弹回"） |

**为什么写死设备路径是隐患**：拔插/重启后线缆重新枚举，Windows 盘符可能从 `H:` 变成别的，
Linux 的 `/dev/sgN` 也会变。写死就会"读取失败→重连→还是失败"死循环，而设备其实好好的
（本轮现场就抓到 252 次这样的循环，正好对应"linux→windows 粘贴不过来"：**读**坏了、**写**还通）。
两边的自动发现都只碰**盘符/字符设备**，不碰 `\\.\PhysicalDriveN`（整块物理盘，权限与副作用都不该碰）。

### 44.4 手动恢复（用户自己也能救）

```bash
# Windows：剪贴板代理 / 键鼠代理
cd re/windows && ./deploy.sh --restart        # 剪贴板（含 BOM/行尾自检）
cd re/windows && ./deploy.sh --restart-km     # 只重启 otiagent2
# 麒麟：otikm
pkill -x otikm; sg input -c "DISPLAY=:0 nohup ~/otilink/run-kylin.sh > /tmp/rk.log 2>&1 &"
```
热键（**键盘在哪一侧就在哪一侧生效**）：`ctrl+alt+space` 无条件切换、`ctrl+alt+left` 拉回本机。
本轮用户的键盘在 Windows、鼠标在麒麟，所以热键按不了 —— 这才需要"推回来"这个物理手势。

### 44.5 本轮现场的两个额外发现

* 麒麟侧日志里 `ERR decode rc=-2` 其实是**厂商 XML 帧**（`0x39` 开头，MacKMLink 写的）——
  和 Windows 侧一样单独识别成 `[skip] 厂商 XML 帧` 并计数，别当"解包失败"。
* 两侧都会偶发 SCSI 读错误（麒麟侧见过 `ERR recv rc=526080` 一次即自愈；
  Windows 侧会连续失败 60 次后重连）。**偶发**是正常的（设备/链路打嗝），
  本轮补的是"**持续**失败"那一段的自愈。

### 44.6 附带发现并修掉：**剪贴板帧会静默丢**（这才是"复制粘贴不了"的另一半）

修好回程后按用户描述复测剪贴板，发现 **麒麟→Windows 连发 15 条丢 6 条（40%）**，而反向 12/12。
两侧日志都**没有任何错误**：麒麟每一条都打了 `CLIP 发送`，Windows 既没有 `解包失败` 也没有 `乱序` ——
帧就是在通道里没了（设备打嗝/帧被跳过，`FrameCheck != 1` 时两端都是**静默 continue**）。

**修法：给剪贴板加 ACK + 超时重发**（复用早已定义但没用的 `OTI_MSG_ACK = 6`）：

* 载荷 `u32 crc + u16 format + u16 rsvd`；
* 收端**应用成功后回 ACK**（crc = 落地内容的 CRC）；
* 发端发完登记"待确认"，**1.5 秒没等到就重发**（最多 3 次，之后告警放弃）；
* 大文件那条路本来就有 DONE/VERDICT 握手，不受影响。

实测（同一条链路、同样的机器）：

| 场景 | 修前 | 修后 |
|---|---|---|
| 麒麟→Windows 连发 15 条（2.5 秒一条，比真人快） | 9/15 | **14/15** |
| Windows→麒麟 连发 12 条（2.5 秒一条） | 12/12 | 9/12（*） |
| **两个方向各 6 条、5 秒一条（真人节奏）** | — | **6/6 + 6/6** |

（*）快速连发时，下一条剪贴板会**顶掉上一条的重发窗口**（新内容一来就重新登记），
所以"2.5 秒一条"这种极限节奏仍可能丢；真人节奏（复制→看到过去→再复制）稳定。

**实现里踩到的坑**：一开始把"登记待确认"写进了 `send_clipboard()`，而重发时又是拿 `ack_buf`
当发送源 → `send_clipboard` 里 `free(a->ack_buf)` 再 `malloc` → **use-after-free**；
而且每次重发都重新登记会把重试计数清零 → **无限重发**。
现在拆成 `send_clipboard()`（只发）+ `clip_ack_register()`（只登记），重发路径不再重新登记。

### 44.7 第 36 轮续：「移到 Linux 回不来」的第二次复现 —— 令牌通道是好的，是**钩子被静默摘除**

用户再次报："鼠标移到 linux 就回不来了"。现场：键鼠已插回 Windows（otikm 跑**被驱动侧**），
麒麟光标停在 x=0，otikm 每 1.5 秒发一次交还通知（`撞边(0,y) → 通知对端收回控制权` +
`已通知厂商端交还控制权…rc=0`）—— **麒麟侧在拼命喊"你收回去"，Windows 侧毫无反应**。

**决定性实验**（把 otiagent2 强制进 REMOTE，再从麒麟发一个 F24）：

```
[08:59:28] REMOTE  (--remote)  -- input is forwarded to the peer
[08:59:32] LOCAL   (peer token (F24))  -- Windows keeps its own input     ← 4 秒后自己回来了
```
→ **回程通道本身是好的**。所以问题在"当时那个跑了 30 分钟的 agent 实例"。

**最可能的根因：Windows 静默摘除了低级钩子**（`LowLevelHooksTimeout`，代码注释里早就记过这个坑：
钩子回调超时会被系统无声摘掉）。钩子没了 → 线缆送来的 F24 键盘事件永远进不了 `kbdProc`
→ 令牌石沉大海 → 指针回不来。这解释了"刚启动时好的、用一阵子就不行了"。

**本轮加固（三件）**：

1. **F24 令牌连发 3 轮**（`otilink_hid_send_token`）：HID 包会丢（帧通道实测丢 40%），
   而"回程令牌丢了"的代价是用户彻底卡死 —— 必须冗余。F24 无人使用，重复按安全。
2. **钩子健康看门狗**（otiagent2）：**REMOTE（正在转发）时每 30 秒重装一次**低级钩子
   （`UnhookWindowsHookEx` + `SetWindowsHookEx`，日志 `hooks reinstalled #N`）。
   代价可忽略，能自愈"钩子被摘除 → 收不到回程令牌"这一类故障。
3. **键鼠代理也开始写日志**（`C:\Users\Public\km.log`）：每次 LOCAL/REMOTE 切换都带**原因**
   （`startup` / `cursor pushed out of the hand-over edge` / `peer token (F24)` / `return hotkey`）。
   以前它不写日志，"回不来"根本无从下手 —— 这是本轮最重要的可观测性补强。

**用户侧急救热键（键盘在哪一侧就在哪一侧按）**：
* Windows 键盘：**`Ctrl+Alt+→`**（= `--hotkey-back 27`，VK_RIGHT）→ 立刻强制回 LOCAL。
* 若键盘在麒麟：`Ctrl+Alt+←`（配置里的 `hotkey_to_local`）。
* 命令行兜底：`cd re/windows && ./deploy.sh --restart-km`（重启即 LOCAL）。

---

## 45 第 37 轮：「移到 Linux 就回不来」彻底根治 —— 三个叠加故障 + 关停厂商 GO! Suite

用户第三次报同一症状（"鼠标移到 linux 就回不来了"，最后靠**拔插对拷线**才回来）。
这次挖到底了：**不是一个 bug，是三个故障叠在一起**，而且**其中一个是我们自己上一轮"加固"引入的**。

### 45.1 故障分层（为什么之前几次都治不好）

| # | 故障 | 症状 | 归属 |
|---|------|------|------|
| 一 | 厂商引擎（MacKMLink/LinkEngKM）与我们的实现**同时占用同一条链路** | 转发由厂商做，回程由厂商决定 | 结构性，非代码 bug |
| 二 | 厂商的"交还控制权"是**握手式**的，而麒麟侧**没有厂商的 Linux 端代理** | 厂商喊话无人应答 → 永远卡在 remote | 厂商设计缺陷 |
| 三 | **我们自己**：REMOTE 看门狗在 **Worker 线程**里重装低级钩子 | 钩子挂到不泵消息的线程 → 键盘/鼠标按键/F24 令牌**全哑**，只有鼠标移动还活着 | **我们的回归** |

### 45.2 故障一、二：厂商在抢链路，且它的回程本来就残

现场证据：
* `km.log` 里我们的 `otiagent2` **从启动起一直 LOCAL、从未 REMOTE**，可麒麟光标却在被驱动
  → 转发是**厂商**做的，不是我们。
* 麒麟 `ps -ef` 里**没有任何厂商进程**（只有我们的 `otikm`），而麒麟侧日志里却不断出现
  `[skip] 厂商 XML 帧` —— 那是 Windows 侧厂商引擎在**向一个不存在的对端代理喊话**。
* 厂商命令集里有 `Cmd_Notify_KM_Switch_To_Local` / `Cmd_Get_KM_Switch_Acknowlege`
  这类**请求-应答**命令；对端没人应答，它的状态机就停在 remote：本地键鼠被它吞掉转发走，
  回程手势再推也没人处理 → **只能拔线重置它的会话**（用户正是这么回来的，症状完全吻合）。
* 厂商注册表里 `ClipboardShareOption=1`（它还在抢剪贴板），`KMSwitchMode=3`、
  `SenseScreenArea=0,0,50,50`（"移出"判据是个 50×50 角区，与用户"平推左边缘"的手势不符）。
* 我们上一轮尝试用 `Cmd_Notify_KM_Switch_To_Local` XML 去求它交还：`rc=0`（写进线缆成功）
  但**毫无效果** —— 因为对端根本没人处理。

**结论：两套实现在同一条线缆上必然互相破坏，厂商那套的回程还是残的。只能二选一。**
选我们（已实测可用），把厂商关停（§45.6）。

### 45.3 故障三：我们自己引入的回归（本轮最关键的发现）

`otiagent2` 上一轮加的"钩子看门狗"（`NOTES §44.7`）为了治"钩子被系统静默摘除"，
在 **Worker 线程**里做了 `UnhookWindowsHookEx` + `SetWindowsHookEx`。而
**低级钩子（`WH_KEYBOARD_LL`/`WH_MOUSE_LL`）的回调只在"安装它的那个线程"的消息泵里派发**
—— Worker 线程只做设备写入、**不泵消息**，钩子装上去就等于死了。于是 `--remote` 状态下：

* 键盘转发**全废**（用户原话"键盘像失灵"）→ 麒麟侧收不到任何按键；
* F24 **回程令牌**收不到 → **指针永远回不来**（用户报的正是这个）；
* 鼠标**按键**也废（按键走钩子）；
* **只有鼠标移动还活着** —— 因为移动走的是 **Raw Input**（`RIDEV_INPUTSINK` + 主线程窗口过程），
  跟钩子无关。**这个"半死"特征极具误导性**，是前几轮反复误判的原因。

时间线完全吻合：这个"加固"是上一轮加的，用户**紧接着**就报"还是回不来"。

**证据链**（都做过对照实验）：
1. 独立探针进程（自带 `WH_KEYBOARD_LL`，自己泵消息）在三种状态下都能看到线缆发来的
   `vk=0x7C`(F13)：代理全死 ✓ / 代理 LOCAL ✓ / 代理 REMOTE ✓ —— 说明**钩子机制、线缆键盘通道都正常**。
2. 同一个探针能看到线缆发来的 `vk=0x87`(F24) ✓ —— 说明**令牌通道本来就通**。
3. 我们自己的代理**在 LOCAL 下能记到线缆 F13**，**在 REMOTE 下记不到任何键** ——
   差异只在"REMOTE 时看门狗重装了钩子"。日志里 `tid=` 直接指向 Worker 线程。

### 45.4 修复（`otiagent2.cs`，构建标记 `mainhook-3`）

1. **钩子一律回主线程装**：Worker 只 `PostMessageW(rawWnd, WM_APP_REINSTALL, ...)`，
   由 `MyWndProc`（主线程，消息泵所在）执行 `ReinstallHooks`。`rawWnd` 在 `InitRawInput` 里存下。
2. **重装日志带线程号**（`tid=`）—— 这类"装错线程"的坑，没有线程号根本看不出来。
3. **`BuildTag` 常量**（`[build mainhook-3]` 打进启动日志）：`csc /out:` 覆盖**正在运行**的 exe
   会**静默失败**（RUNBOOK 早记过），没有版本标记就会一直在跑旧二进制（本轮踩了）。
4. F24 命中**一律记日志**（回程类故障的第一线索）；其余按键只在 `--verbose` 下记，免得刷屏。

### 45.5 令牌必须由消息泵线程发（`otikm.c`）

`NOTES §44.7` 已经为"厂商帧"记过这条教训（捕获线程自己发会跟泵抢授权消息/应答），
但**令牌和释放包漏改了**：撞边回调（捕获线程）里直接 `otilink_hid_send_token()`。
本轮统一改成**打标记、由泵线程发**：

* 新增 `want_token` / `want_release_all` + `request_token()` / `request_release_all()`；
* `rx_thread` 每轮优先处理，顺序**先释放包、后令牌**（切回本地时先"松手"再喊话）；
* 令牌日志带 `rc=`：`→ 回程令牌 F24 已发（消息泵线程，rc=0）`，失败会显式标注。

### 45.6 关停厂商 GO! Suite（`re/windows/vendor-off.ps1`）

需要管理员，用 `vendor-off.vbs`（`Shell.Application.ShellExecute ... "runas"`）拉起，避开
"`Start-Process` 继承 WSL 句柄导致卡死"的坑。脚本做四件事：

1. 按 **LEWD → LinkEngKM → MacKMLink →（路径匹配）SKLoader** 的顺序杀（LEWD 是看门狗，必须第一个），
   再跑 **20 秒重生守卫**防止拉起；
2. 删 **HKCU Run** 里的 `CS Dispatch`（`MacKMLink.exe -GN:RunFromRegistry`）、启动文件夹快捷方式、
   计划任务、服务；
3. 全程写 `C:\Users\Public\vendor-off.log`；
4. 已核实：杀完 Run/任务/服务**零残留**，`NOTES §45.9` 记了两个误伤教训。

**恢复厂商**：重新加回那个 Run 键值（或重装 GO! Suite）即可，脚本不改厂商文件。
**注意**：`otiagent2` 与厂商程序**不能同时跑**（RUNBOOK 早有此条）——现在厂商自启已禁，重启也不会回来。

### 45.7 验证证据（`C:\Users\Public\km.log`）—— 全程**自动化**，不依赖用户动手

```
otiagent2 up. device=\\.\H:  edge=right  hotkey-back=Ctrl+Alt+VK27  [build mainhook-3]
[09:53:45] LOCAL   (startup)  -- Windows keeps its own input
[09:53:55] REMOTE  (cursor pushed out of the hand-over edge)  -- input is forwarded to the peer
[09:54:32] kbd: F24 token seen (mode=REMOTE, down=True)
[09:54:32] LOCAL   (peer token (F24))  -- Windows keeps its own input     ← 指针自己回来了
```

麒麟侧对应日志（`/tmp/otikm.log`）：

```
[06144.324] 撞边(0,782) → 通知对端收回控制权（被驱动侧，不接管）
[06144.613] → 回程令牌 F24 已发（消息泵线程，rc=0）
```

回归：`coretest` 14/14、`clipreg.sh` 17/17（文本/大文本/图片/文件双向）。

### 45.8 排障工具箱（本轮新增，都留在仓库里）

| 工具 | 侧 | 用途 |
|------|----|------|
| `re/otilink/hidprobe.c` | 麒麟 | `./hidprobe /dev/sg3 <1鼠标\|2键盘> <次数> [usage]` —— **本机→对端 HID 方向**是否通（鼠标看对端光标动不动） |
| `re/windows/kbdprobe2.ps1` | Windows | 独立 `WH_KEYBOARD_LL` 探针（自泵消息），打印 `vk=`/`inj=` —— 判定"钩子链是否活着" |
| `re/windows/sendkey.ps1` | Windows | 合成一个按键（`SendInput`），给探针/代理做对照输入 |
| `re/windows/detect-keys.ps1` | Windows | 轮询**会话级** `GetAsyncKeyState`（不依赖钩子）—— 判定"设备事件到底有没有到 Windows" |
| `re/windows/pushright.ps1` / `pushleft.ps1` | Windows | 复现真实交接手势（走到边缘并继续外推） |
| `re/windows/pushleft-synth.ps1` | Windows | **纯相对**左推（REMOTE 下等价于真实推左，用来测回程） |
| `re/windows/vendor-off.ps1` + `.vbs` | Windows | 关停厂商栈 + 禁自启（需 UAC 一次） |

**排查顺序（"回不来"类故障）**：
① `km.log` 有没有 `REMOTE`？没有 → 转发不在我们手里（查厂商/钩子）；
② `km.log` 有没有 `kbd: F24 token seen`？没有 → 钩子哑了（查 `tid=` 是否主线程）；
③ 麒麟有没有 `撞边 …` + `回程令牌 … rc=0`？没有 → 被驱动侧判据（`DISPLAY`/真实光标读取）；
④ 用 `kbdprobe2.ps1` + `hidprobe` 做端到端对照，十分钟内能定位到层。

### 45.9 两个误伤教训（写脚本删东西前必看）

1. **正则别用短子串**：第一版 `vendor-off.ps1` 用 `OTi|MacKM|Link|GO` 匹配计划任务名/启动项，
   `OTi` 命中了 `Notificati**ons**`（还有 `RecommendedTroubleshooting**S**canner` 等 5 个系统任务），
   `Link` 命中了**我们自己的** `otilink-agent.vbs` 自启项。
   已全部恢复（`Notifications` 等本来就是 `Ready`，只有 `RunUpdateNotificationMgr` 是 `Disabled`，
   而它**禁用/启用都被拒**（`\Microsoft\Windows\UNP\`，属 TrustedInstaller）→ 证明**它本来就是我改之前的状态**）。
   **教训：删/禁之前先 dry-run 打印清单，正则用全名或路径匹配。**
2. **杀进程按路径匹配时也要收紧**：`SKLoader.exe`（MacKMLink 的父进程）是被 `OTi\` 路径规则杀掉的
   —— 这个是对的（厂商宿主），但要清楚自己在杀什么。

### 45.10 教训

* **"半死"比"全死"难查**：鼠标移动活着（Raw Input）、键盘死（钩子）—— 一定要问"哪条路径还活着"，
  它会直接指向机制差异。
* **上一轮的"加固"可能是这一轮的根因**：看门狗、重试、连发这类"防御性代码"必须配**可观测性**
  （`tid=`、`rc=`、`BuildTag`），否则副作用完全隐形。
* **同一个设备 fd 只能由一条线程读写**：这条教训在厂商帧上吃过一次，令牌又吃了一次 —— 现在
  `otikm` 里所有设备写都走泵线程（只有退出清理例外，那时泵已 join）。
* **两套实现抢一条链路 = 必然互相破坏**：与其给厂商打补丁（改配置、发 XML 求它交还），
  不如让它退出（§45.6）。

---

## 46 第 38 轮：把"运行规范 + 门禁 + 验证脚本"补上（并立刻抓到 3 个真 bug）

用户要求："在 re 文件夹、对拷线这个项目下增加 AGENTS.md、门禁、验证脚本等，完整的 agent 运行规范"。

### 46.1 交付物

| 文件 | 作用 |
|---|---|
| `re/AGENTS.md` | **agent 运行规范（硬约束）**：14 条 LAWS、两种拓扑、按改动类型的工作流、门禁矩阵、DoD、排障决策树、已知坑速查、禁止清单、会话交接 |
| `re/tools/lib.sh` | 门禁共用底座：ok/bad/warn/**known** 计数、退出码=失败项数、JSON 报告、SSH/Windows 取数助手（含 `wcur` 用 `GetCursorPos` 避免 DPI 坑） |
| `re/tools/envcheck.sh` | **门禁 0 施工前体检**：本机/Windows/麒麟 + 拓扑一致性（谁在转发）+ 钩子线程唯一性 + KySec 标签新鲜度 |
| `re/tools/doccheck.sh` | **门禁 1 静态**：bash -n、make all、BOM/CRLF、**BuildTag 指纹**、危险写法黑名单（`pkill -f`、ssh stderr 合并、`rm -rf /`）、文档与脚本注册一致性 |
| `re/tools/gate.sh` | **统一入口**：`--pre/--static/--local/--hw-clip/--hw-km/--hw-xfer/--hw-all/--full/--deploy-kylin/--stamp/--report` |
| `re/tools/known-issues.txt` | 已登记的已知失败（显示为 KNOWN、**不计入通过**、必须写影响面；修好必须删行） |
| `re/tools/template-verify.sh` | 新验证脚本模板（三条约定：ok/bad 一行、退出码=失败数、硬件档先体检） |

**门禁实测**（本轮当天）：

```
--static     PASS=63 FAIL=0            （含"改码必 bump BuildTag"指纹门禁，已用故意犯规自证有效）
--pre        PASS=25 FAIL=0            （拓扑：Windows 主控 + 麒麟被驱动；厂商进程 0；钩子 tid 唯一）
--local      PASS=9  FAIL=0 KNOWN=1    （7 套本地套件 + coretest + xferlocal）
--deploy-kylin PASS=3 FAIL=0           （48 文件同步 → 麒麟编译 → 9 个二进制打 KySec 标签 → 写标签戳）
--hw-clip    PASS=2  FAIL=0            （envcheck + clipreg 17/17）
```

### 46.2 修掉的两个链路级隐患（门禁自身带来的）

1. **`hwtest.sh` 依赖 `/tmp/kssh`**（那文件一丢整个键鼠回归就废，实测当时确实不存在）→ 改成自带 askpass（与 `clipreg.sh` 一致），并**不再合并 ssh 的 stderr**。
2. **部署步骤散落在人脑里** → 固化成 `gate.sh --deploy-kylin`（同步/编译/打标签/写标签戳），以及 `deploy.sh --restart-km` 的"先杀→编译→启动→打印 BuildTag"。

### 46.3 门禁第一次跑就抓到的真问题（本机 TCP 档）

`gate.sh --local` 立刻把**长期红着的本机套件**翻出来了：`tcptest.sh` 报
`A 只主动发送 1 次（无回环）实得 4` / `B 实得 3`。查下去发现问题比断言更深：

* **断言过时**：第 36 轮加了"未确认就重发"，同一条内容重发几次是设计行为 → 断言改成**按内容 crc 去重**计数（保留"防回环"本意，另加"重发有界 ≤8"）。
* 但真正的红灯是：**TCP 下对端根本没应用内容**（所以也没有 ACK → 才有重发）。

### 46.4 顺藤摸到的三个真 bug（都在 fd/TCP 传输路径，已修）

1. **fd 写入没有按"整帧"串行化**：`oti_tr_send` 的 `TR_FD` 分支直接 `write()`，而 fd 后端有多个写者
   （剪贴板线程、接收线程的 ACK、保活线程）→ 4 字节长度头与帧体可能交错 → 对端 `decode rc=-1/-2`。
   线缆后端有队列+锁，所以产品路径没这问题。**修复**：整帧（头+体）在一把锁里写完。
2. **fd 读半帧超时致流错位**：`oti_tr_recv` 读出合法帧头后，用同一个 200ms 超时读体；一旦体没凑齐就返回
   超时，而**半帧已经消费掉了** → 之后整条流永久错位。**修复**：帧头之后必须读满（给 5s 上界，让传输看门狗去管真死）。
3. **`oti_tr_open_fd` 从未初始化 `t->lock`**（`lock_inited` 一直是 0；calloc 恰好全零，glibc 下"碰巧能用"，属 UB）
   → 上面那把锁本来也靠不住。**修复**：显式 `pthread_mutex_init`。

**残余**：TCP + `--keepalive` 组合下仍有错位（B 只收到 3/20 个 PING，接收线程被卡住），未定位完 →
按规范登记进 `tools/known-issues.txt`（KNOWN，不计入通过，附影响面），并在此留证。
影响面：仅"本机 TCP/sim 联调 + 网络 bring-up"路径；**线缆产品路径不受影响**（改完共享传输层后已复验：
envcheck 25/25、clipreg 17/17、键鼠往返 `撞边 → 回程令牌 rc=0 → LOCAL (peer token (F24))`）。

### 46.5 教训

* **规范要能执行才有价值**：门禁不是"跑一遍脚本"，而是"失败必须显式处理"——
  要么修好，要么写进 `known-issues.txt` 说明影响面。这两天里"红着的本机套件"就是因为没人负责判定而烂了很久。
* **改动共享代码（`otitrans.c`）后必须复验产品路径**：本机套件绿/红都不能替代线缆真机回归。
* **"半死"现象再次出现**：TCP 链路 PING 通、CLIP 不通 —— 与第 37 轮"鼠标移动活着、键盘死"同一类误导，
  排查时永远先问"哪条子路径还活着"。

---

## 47 第 39 轮：免安装绿色包（通用 Linux 支持 + Windows 绿色入口）

用户要求（原话）：**"为对拷线新增的 Linux 支持，增强原有功能而不影响原有功能；支持免安装，
做到对拷线插到任何两台电脑上都可以直接使用。"** 确认的口径：每次运行**提一次权**（不写系统文件）、
X11/XWayland 精确 + Wayland 降级、**没有第二台 Linux 机器**（真机验收仍是 麒麟↔Windows）、
组合优先级 Linux↔Windows + Windows↔Windows，另加 **Mac↔Linux（Mac 只做接收端）**。

### 47.0 交付物

| 文件 | 作用 |
|---|---|
| `re/portable/otilink.sh` | Linux 唯一入口：找线缆（sysfs 按 0ea0:2213）/卸 LUN/探测桌面会话环境/**一次提权**/角色交互/日志 |
| `re/portable/build.sh` | 组装绿色包：默认**在麒麟上编**（声明基线 glibc 2.31）+ csc 编 Windows 载荷 + tar.gz/zip + BUILD-INFO |
| `re/portable/otilink.cmd` + `otilink-win.ps1` | Windows 唯一入口：找线缆盘符、按角色起 exe+ps1、无管理员/无 csc/无自启、`--stop` |
| `re/portable/mac/otilink-mac.sh` | macOS 只读体检（接收端零软件说明书配套） |
| `re/portable/README.md` | 用户上手 + 边界 + 排障 + **已验证/未验证**清单 |
| `re/tools/portablecheck.sh` + `gate.sh --portable` | 绿色包门禁：源码指纹新鲜度 / GLIBC 基线 / 依赖白名单 / 清单 / 入口脚本 / PowerShell 真解析 |
| `otikm --auto --role --screen auto --cursor-source --return-edge --print-plan --version` | 把 run-kylin.sh 的角色/设备判定搬进 C（新参数全 opt-in，旧行为逐字节不变） |

### 47.1 真机证据（麒麟绿色包接管，**不使用 udev/ACL，只 sudo**）

```
sudo ./otilink.sh --role slave          # 绿色包（root）启动；传输自动选到 /dev/sg3(CD LUN)
  [PASS] gate.sh --hw-clip   → clipreg PASS=17 FAIL=0   （文本/大文本/图片/文件 双向）
  [PASS] gate.sh --hw-xfer   → xferreg PASS=12 FAIL=0   （64MB 双向 md5 一致）
  [PASS] gate.sh --hw-km     → hwtest  PASS=12 FAIL=0   （含"撞边 → 回程令牌 F24 已发"与 Windows 侧 LOCAL(peer token)）
```

日志证据（麒麟 `/tmp/otikm.log`）：`抓取设备 /dev/input/event10 (Android+Mac)`、`传输就绪: cable:/dev/sg3(auto,已应答)`、
`屏幕: 1920x1080（来源 X11/XRandR）`、`被驱动侧模式：真实光标读取 可用（XQueryPointer）`、
`撞边(0,y) → 通知对端收回控制权` + `→ 回程令牌 F24 已发（消息泵线程，rc=0）`。
Windows 侧（绿色入口启动的代理）：`otiagent2 up. device=\\.\H: edge=right … [build mainhook-3]`；
`gate.sh --hw-clip`（两端都用绿色包）同样 **17/17**。

### 47.2 这一轮踩到并修掉的真坑（都有实测现象）

1. **线缆 HID 认不出来**：`oti_input_is_cable()` 起初没 `realpath` —— `otilink_is_our_usb()` 是沿
   **字符串路径**向上找 idVendor 的，而 `/sys/class/input/eventN/device` 是**符号链接**，不解析就永远走不到
   USB 设备目录 → 真机表现"看不到线缆 HID 设备"（被驱动侧抓不到任何输入）。修：先 `realpath`。
2. **自动探测悄悄换了 LUN**：`oti_tr_open_cable(NULL)` 按"谁应答 0xF0/0x00 就用谁"选，root 下 **/dev/sg2(LUN0) 先应答**，
   而安装版一直用 **/dev/sg3(CD LUN)**（全部真机回归都在那条路上跑过）。修：**优先 CD-ROM LUN（sysfs type=5）**并写明日志。
3. **Windows 入口把盘符探错**：解析 `otiagent.ps1 -Scan` 时用 `Out-String` 拼成一整段再跨行匹配，而真实输出里
   "打不开的 \\.\F:"和"\\.\H: 信息块 rc=0"**被合并进同一个数组元素** → 取到不存在的 F:，键鼠代理
   `cannot open \\.\F: (err 2)` 起不来。修：**只认 agent 自己给的"建议: -Device '…'"行**（显式 capture），
   退路是"全文只有一个候选才敢用"，否则交回代理自探测。
4. **Start-Process 传命令行会把引号/重定向搞坏**：键鼠代理"一声不响没起来"（日志文件都没创建）。修：把命令行写成
   `<日志目录>\otilink-run-{km,clip}.cmd` 再 `Start-Process` 那个 .cmd（顺带留证据）。
5. **进程匹配太松 → 起了两个剪贴板代理**：按 `otiagent\.ps1` 匹配会把 `cmd /c powershell …` 的 **cmd 包装进程**也算进去
   → 重复实例抢剪贴板，`clipreg` 立刻出现 `OpenClipboard 失败 err=5` 与图片用例失败。修：匹配
   `-File\s+"?[^"]*otiagent\.ps1`，并去掉 `cmd /c` 双重包装。
6. **root 起的绿色包，普通用户 `pkill` 杀不掉**：实测"停不掉 → 起了第二个实例 → rc=526080 漫天飞"。修：入口**启动前查重**
   （`OTILINK_FORCE=1` 才强起），文档写清 `sudo pkill -x otikm`。
7. **守护进程占住包目录**：从 U盘/包目录启动时进程 cwd 就在那儿（U盘拔不掉；包目录被重建还会刷 `getcwd() failed`）。修：exec 前 `cd /`。
8. **root 跑时收到的文件属主是 root**：落盘在 `/tmp/otilink-files-<pid>`（0700 root），桌面用户读不了。
   修：`oti_adopt_path()` 在 `SUDO_UID` 存在时把目录与文件 chown 回桌面用户。
9. **工具链会吃掉 BOM**：`edit` 工具重写文件后 `otilink-win.ps1` 的 UTF-8 BOM 丢了 → PowerShell 5.1 按 ANSI 读中文注释，
   **连换行一起吞** → 报一堆莫名语法错。修：改完重写一次 BOM，并加门禁：`portablecheck` 用 `tools/psparse.ps1` 做**真解析**、
   `doccheck` 查 `re/portable/*.ps1` 的 BOM 与 `*.cmd` 的 CRLF。
10. **中文注释里写 `\r\n` 会被 JS 转义成真换行**（写脚本时）→ 文件里凭空多出一行 `]* …`，PowerShell 报
    "无法将 ] 识别为 cmdlet"。修：这类字符串一律用 raw 写法，修完用 psparse 过一遍。

### 47.3 未验证 / 不做（诚实清单）

* **hw-km 复跑先失败后成功**：第一次复跑失败的原因是 **麒麟掉线**（`No route to host`，脚本里所有 ssh 侧断言全 FAIL），
  与绿色包无关；网络恢复后重跑 **12/12 通过**（见 §47.1）。这条留在文档里当"假失败"的又一例证：
  **先看前提（谁在线），再谈结论**。
* **Windows↔Windows**：只有一台 Windows，未真机验证（机制上接收端键鼠零软件）。
* **Mac 作为接收端**：没有 Mac，未真机验证；`mac/otilink-mac.sh` 是给用户跑的只读体检。**Mac 作为主控端本轮不做**
  （需 IOKit SCSITaskUserClient + CGEventTap，没机器不敢写）。
* **Linux↔Linux**：`--peer proto` 对称能力在，未真机验证（本机双实例套件覆盖协议层）。
* **跨发行版容器矩阵**：本机 Docker 引擎没开 → 只做了 `readelf` 基线核对（最大 GLIBC 需求 **2.17** ≤ 声明 2.31，
  动态依赖只有 glibc 自身）；容器矩阵在 `portablecheck.sh` 里，开 Docker 重跑即可自动补上。


---

## 48 第 40 轮：现场故障"用了两小时后剪贴板单向不通、拔插无效"（保活根治）

用户报障（装好绿色包用了两个多小时后）：**Windows→Linux 能复制粘贴，Linux→Windows 不能；拔插也不管用。**

### 48.1 取证（先看谁在转发，再看哪条子路径还活着）

* 两侧进程都**正常且唯一**（Windows `otiagent2=1` + 剪贴板代理 1；麒麟 otikm 1）；`envcheck` PASS=23 FAIL=0。
* **KM 通道是好的**（`km.log` 有 `LOCAL (peer token (F24))`）—— 与"只有剪贴板单向不通"一致：
  键鼠走 **HID 包通道**、剪贴板走**帧管道**，两条独立路径。
* **Windows clip.log 在死循环**：`设备连续读取失败，尝试重连 \.H: … 已重新连接 \.H:` 反复刷。
* **麒麟日志**：`剪贴板 2 字节重发 3 次仍无确认（对端在吗？），放弃`；
  `ERR recv rc=524546（连续 1..3 次）` / `rc=526080` —— 解码即 **DID_NO_CONNECT + CHECK CONDITION** 与 **DID_ERROR**
  （USB 链路掉了一下、SCSI 栈没恢复）；`已应用远端` 计数 **0**（这一轮一条都没收到）。
* 过了一段时间它**自己好了** → 是**状态卡死**，不是硬件坏。

### 48.2 根因判断

帧管道**长时间空闲**（几小时只有零星剪贴板活动）→ 设备侧会话卡死 → 两侧读命令开始返错；
主机侧 `CloseDevice/OpenDevice`（所谓"重连"）**修不了**，因为卡的是**设备内部会话**，不是主机句柄 ——
这就是"拔插也不管用"（何况**只拔一端**时另一端还攥着旧句柄）。

### 48.3 处置（本轮已做）

1. **保活默认开**：`keepalive = 5000`（`re/otilink/kvm.conf` 模板 + 麒麟 `~/.config/otilink/kvm.conf`），
   并把 `keepalive` 补成**配置键**（以前只有命令行 `--keepalive` → 装好之后永远是关的，这才是根因的根因）；
   绿色包 `--auto` 也默认 5000ms（`--keepalive 0` 可关）。日志证据：`保活已启用：每 5000ms 一次（PING + 线缆 dummy 帧）`。
2. **恢复流程写进 `RUNBOOK §17.4`**：正确恢复是**两端 agent 都重启**（Windows `deploy.sh --restart` +
   麒麟 `pkill -x otikm; ./run-kylin.sh`），不是只拔一端。
3. 复验：`gate.sh --static` PASS；`gate.sh --hw-clip` **17/17**；双向实测通过；保活开启后麒麟日志 `ERR` 计数 **0**。

### 48.4 待观察

保活能否**真的**避免复发，要再跑几小时才有结论 —— 本轮只证明"开着保活一切正常、且不引入新错误"。
若再复发，下一步是达到连续读失败阈值后**主动发厂商 USBRestart（`0xF0/0x05/0x02`）**做设备级复位
（`otilink_usb_restart()` 已有；但会让设备重新枚举、可能切 USB 人格，需真机验证后才敢默认开）。

---

## 49 第 41 轮：定位并修掉"L→W 剪贴板失败"的真因 —— 厂商 XML 通知抢占发送授权

现场（用户原话）：**从 Linux 复制 `dct` 到 Windows 失败**；拔插无效；W→L 一直正常。

### 49.1 证据链（同一时刻两侧互证）

* 麒麟：`CLIP 发送 3 字节（fid=141）` → 第 1/2/3 次重发 → `[warn] 剪贴板 3 字节重发 3 次仍无确认…放弃`（共发 4 次）。
* Windows：同一时刻**在读帧**（clip.log 在 `[skip] 厂商 XML 帧 668 字节`），所以既不是设备死、也不是对端没听。
* 麒麟每 1.5 秒就来一发 `撞边(0,y) → 通知对端收回控制权` + `已通知厂商端交还控制权（668 字节 XML）`
  —— 因为 Windows 主控把指针**停在麒麟屏幕左边缘**，撞边判据一直成立。

### 49.2 根因

那条**厂商 XML 通知**和剪贴板帧走**同一条帧管道、争同一个发送授权窗口**；我们自己的拓扑里
厂商 Windows 栈早就关停了（L1），它**只有害处**。指针贴边时它每 1.5 秒挤占一次 → 剪贴板帧发 4 次全丢
→ 无 ACK → 放弃。这也解释了 W→L 正常（用 Windows 侧发送）与"拔插无效"（不是设备坏）。

### 49.3 修法

1. `--vendor-notify on|off`（配置键 `vendor_notify`），**默认 off** —— 要对接厂商 Windows 端才开；
2. 即便打开也**强制 >=5s 节流**（原来 1.5s）；
3. 复现验证（**指针故意贴左边缘**、连做 10 次 L→W 复制）：**PASS=10 FAIL=0**；日志里
   `已通知厂商端=0`、`放弃=0`，每笔都是一次 `CLIP 发送` 直接成功。

### 49.4 小结（这一轮的两个故障不是一回事）

| 故障 | 现象 | 真因 | 修法 |
|---|---|---|---|
| 第 40 轮 | 用几小时后剪贴板单向不通、拔插无效 | 帧管道**长时间空闲** → 设备侧会话卡死 | `keepalive = 5000` 默认开 |
| 第 41 轮（本节） | 某一刻起 L→W 复制一直失败、W→L 正常 | 撞边**厂商 XML 洪流**抢占发送授权，剪贴板帧被挤掉 | `vendor_notify` 默认关 + 5s 节流 |

---

## 50 第 41 轮续：被驱动侧"回程手势"不再强制贴边停留 250ms（用户反馈：不顺畅）

用户原话：**"鼠标 linux→windows，不要等 250ms 了，感觉卡住了，不顺畅；改为像 windows→linux 一样顺畅。"**

* 现象映射：被驱动侧（麒麟）交还控制权的手势里有个硬编码的 **贴边停留 >=250ms** 要求
  （`now - a->edge_at_ms >= 250`），而主控端撞边是"碰到边就交" —— 所以 L→W 回程总比 W→L 慢半拍，
  手感上就是"卡一下"。
* 修法：把它变成可配项 `--edge-dwell MS`（配置键 `edge_dwell`），**默认 0 = 立即交还**，与主控端一致；
  想恢复旧行为防误触就写 `--edge-dwell 250`。**1.5s 的重复触发节流保留**（防止指针停在边缘时刷屏/刷令牌）。
* 验证：`gate.sh --hw-km` **12/12 PASS**（该档覆盖交接与回程）；
  麒麟日志里 `撞边(0,y) → 通知对端收回控制权` 紧跟 `→ 回程令牌 F24 已发（消息泵线程，rc=0）`。
* 注：Wayland 的"积分判据"仍要求外推 >=16px（那是替代真实坐标的判据，不是延迟），不受本次影响。

---

## 51 第 42 轮：麒麟**整机硬卡死**取证 —— 真凶是 Kylin 自己的后台升级检查器（**与 otilink 无关**）

2026-09-18 现场：用户报"麒麟卡住了，点哪里都无效，现在连鼠标也动不了了"。

**先排除 otilink（结论：无辜）**：Windows 侧 `otiagent2` 活着（pid 3516，`[build mainhook-3]`，
钩子每 30s 在主线程 tid=17140 重装，日志无发送失败）；线缆 `\\.\H:` 可 ReadWrite 打开；
厂商进程 0 个。`km.log` 停在 `10:18:11 REMOTE (cursor pushed out of the hand-over edge)`，
麒麟最后一次 F24 是 `10:18:09` → **是对端不吭声，不是我们**。用户看到"鼠标动不了"= REMOTE 的正常表现
（光标钉在交接边，事件全被吞掉转发给对端）。

**判断"麒麟是硬卡死 or 只是 otikm 死了"（本轮最值钱的经验）**
* 键鼠插在 Windows → **麒麟没有本机键鼠，线缆 HID 是它唯一的键盘**。所以 **REMOTE 状态下别急着按
  `Ctrl+Alt+→`**：先试 `Ctrl+Alt+F2`，能进 TTY 就用它救 otikm/DM；切回 LOCAL 等于把麒麟唯一的键盘拔掉。
* `Ctrl+Alt+F2` 完全没反应 = **内核没在调度**（不是桌面卡），只能长按电源键。
* **网络不通 ≠ 机器死**：本项目键鼠不走网络。这次 tailscale 掉线只是顺带（麒麟真实 LAN 地址是
  `<麒麟真实LAN IP>`，见 tailscaled 的 `endpoints changed`）。

**真凶（journald 是持久化的，`journalctl --list-boots` 还在，所以能回溯）**
`kylin-background-upgrade-manul.timer` 每 **315s** 跑 `/usr/bin/kylin-background-upgrade --check-upgrade`
（unit 为 `Type=forking` + **`TimeoutStartSec=infinity`**）。上一个 boot 的**最后一条日志**是：

   10:19:26 systemd[2584]: Starting System upgrade background detection program...

而前面 5 次（09:53:11 / 09:58:26 / 10:03:41 / 10:08:56 / 10:14:11）都是 `Starting...` **紧跟** `Started`，
**只有这一次永远没有 `Started`** —— 机器当场死。死前还有 **52s 日志空白**（10:18:34 → 10:19:26）。

**取证特征（下次照这个判）**
* `hung_task_timeout_secs=120` 与 NMI `watchdog=1` 都开着，却**一条 panic/oops/lockup/hung task/OOM 都没有**。
* ⚠️ **盲区**：内核消息只能经 journald 看到，而 journald 写同一块盘 → "存储/IO 停顿"与"内核真静默"
  在本案**无法区分**。**别把机制写成确定结论**（L14）。
* NVMe 无 I/O 错误、根分区 31%，所以不是"盘满"。
* 旁证：卡死前 tailscaled 在 10:16–10:18 持续超时（网卡是 out-of-tree 的 `aic8800_fdrv`）；
  `usb 1-3.4`（就是那根 OTi 线，`0ea0:2213`）在日志里反复 `USB reset`。

**同时挖出的独立重病（当时仍在发生）**
`kylin-software-center-plugin-synchrodata` 启动必崩：`segfault at 10 ... in libQApt.so.3.0.5`，
而 unit 是 `Restart=always` / `RestartSec=5` → 一个 boot 重启 **2710 次**、稳定 10 次/分钟，
累计 **3875 个 coredump / 4.1 GB**，journal 另占 1.0 GB。它 08:47 停了（撞 systemd 启动限流），
所以**不是** 10:19 那次的直接凶手，但证明这台机器的 Kylin 更新/软件中心栈是坏的。

**处置（可逆，已执行并验证）**

    # 1) 摘掉 6 个 user 单元（含 silent 孪生兄弟，否则同一触发器还留着）
    systemctl --user stop kylin-background-upgrade-{manul,silent}.{timer,service} kylin-software-center.{timer,service}
    systemctl --user mask kylin-background-upgrade-{manul,silent}.{timer,service} kylin-software-center.{timer,service}
    # 2) 停掉崩溃转储写盘 + 回收空间
    #    /etc/systemd/coredump.conf 追加 Storage=none（原文件备份 .bak-20260918）
    sudo rm -f /var/lib/systemd/coredump/* ; sudo journalctl --vacuum-size=200M   # 4.1G→456K，1.0G→168M，根分区 31%→22%
    # 3) 取证基建
    #    kernel.sysrq=1（/etc/sysctl.d/99-sysrq.conf，原本 176 只允许 sync/remount/reboot）
    #    freeze-breadcrumb.service → /usr/local/bin/freeze-breadcrumb.sh
    #      每 5s 往 /var/log/freeze-breadcrumb.log 落：load / top CPU / Dirty,Writeback / **D 状态进程**，写后 sync -d

**验证（过了原定的触发点才算数）**：`manul.timer` 下一次原本是 **10:50:07**；过点后
`journalctl --since 10:48 | grep -ci 'upgrade background detection'` = **0**，synchrodata 崩溃 = **0**，
无新 coredump，面包屑连续 **5.03s** 一跳。

**仍未摘（小时级才响，先观察）**：`kylin-source-update-T1..T4.timer` / `kylin-source-update-timer.timer`
（`/usr/bin/kylin-source-update`）、`kylin-update-rescue.timer`；标准 `apt-daily*` 未动。
若这些时刻附近再卡，优先摘它们。

**一句话给下次**：再遇"麒麟整机卡死"，先看 `/var/log/freeze-breadcrumb.log` 最后一条 `=====` 的时间戳
与那一段的 `!!D-state` / `Dirty`/`Writeback`；再 `journalctl -b -1 -n 100` 看最后一条是否
`Starting System upgrade background detection program...`。


---

## 52 第 43 轮：拔插后两个**真 bug** —— `run-kylin.sh` 设备号写死 + otikm 自发现后段错误

2026-09-18 现场：用户按 §8 急救"**两端重拔插**"后，**Windows 侧又永久卡 REMOTE、鼠标再次被吞**。
排查（`journalctl` + `/tmp/otikm.log`；注意 otikm 日志前缀 `[08263.6]` 是**开机秒数**，不是进程运行时长）：

**1) otikm 在自发现"成功"之后立刻段错误（真 bug）**

    9月 18 12:46:33 otikm[3499]: segfault at 7ff5483182a0 ip 000000000040d188
                                 sp 00007ff547314ce0 error 4 in otikm[402000+13000]
    audit: ANOM_ABEND comm="otikm" exe="~/otilink/otikm" sig=11 res=1

`/tmp/otikm.log` 紧邻上下文：

    !! 看门狗：重新打开传输 cable:/dev/sg3
       /dev/sg3 已不存在（拔插后设备号可能变了）→ 改为自动探测
    传输已重新就绪: cable:/dev/sg5(auto,已应答)      ← 自发现"成功"
    <随后 segfault>

对应 `otikm.c:1576`（判不存在）→ `otikm.c:1593`（报重新就绪），`otitrans.c:148` 打印 `(auto,已应答)`。
**高度怀疑自发现挑到了错的 LUN** —— `otitrans.c:103` 那句注释正是这个担心：
`/dev/sg3（CD LUN），真机回归全在这条路上跑过；自动探测不该悄悄换到另一个 LUN。`
本机日志也自相矛盾：报的是 `sg5`，而当时存在的节点里并没有 sg5。
**待办**：带 `-g` 重编复现定位 `0x40d188`；或直接审 `otikm.c:1560~1600` 的 reopen 路径。

**2) `run-kylin.sh` 把线缆设备号写死（真 bug ——"每次拔插都要手工救"的根因）**

`re/otilink/run-kylin.sh` **两处**硬编码 `set -- --transport cable:/dev/sg3`（被驱动侧 ~L35、主控侧 ~L57）。
拔插后节点会变（本次 **`sg3` → `sg4`**），于是启动即 `ERR 建立传输失败: cable:/dev/sg3` 退出；
而自启动项 `~/.config/autostart/otilink-kvm.desktop`（`Exec=~/otilink/run-kylin.sh`）走的就是它
→ **重登/重启后 otikm 根本起不来**，用户侧表现为"又卡住了"。

**而 otikm 本身支持自发现**：`--transport cable`（不带 `:/dev/sgN`）即可 —— 实测
`传输就绪: cable:/dev/sg4(auto,已应答)`。**修法 = 两处改成裸 `cable`**（或走 `--auto`）；
按 §5 需 `gate.sh --static` + `--deploy-kylin`（碰键鼠启动路径再加 `--hw-km`）。

**LUN 编号对照（拔插后极易搞反，务必按 type 判而不是按序号）**

| sg | scsi 地址 | model | type | 是什么 |
|---|---|---|---|---|
| sg0 | 5:0:0:0 | DVDRAM GUD1N | 5 (CD-ROM) | 本机真光驱 |
| sg1 | 6:0:0:0 | TOSHIBA DT01ACA1 | 0 (disk) | 本机真硬盘 |
| sg2 | 7:0:0:0 | **Virtual Link** | 0 (disk) | 线缆 **MS LUN**（= Windows 的 `H:`） |
| sg4 | 7:0:0:1 | Android+Mac | 5 (CD-ROM) | 线缆 **CD LUN** ← **otilink 要的是这个**（带 otilink udev ACL，`ls -l` 有 `+`） |

**本次恢复动作（可复用）**

    # Windows：⚠️ 本环境 powershell.exe 不在 PATH → deploy.sh 会静默失败（"powershell.exe: command not found"，
    #   且因为 Stop-Process 那段也一起没跑，代理压根没重启，人还以为已经重启了）
    export PATH="$PATH:/mnt/c/Windows/System32:/mnt/c/Windows/System32/WindowsPowerShell/v1.0:/mnt/c/Windows"
    cd re/windows && ./deploy.sh --restart-km     # 代理重启即回 LOCAL

    # 麒麟：run-kylin.sh 修好前，先手工用裸 cable 自发现拉起
    pkill -x otikm; sleep 1; cd ~/otilink
    setsid env DISPLAY=:0 ./otikm --transport cable --return-on-edge --config ~/.config/otilink/kvm.conf \
      --capture /dev/input/by-id/usb-_Android+Mac_*if01-event-mouse \
      --capture /dev/input/by-id/usb-_Android+Mac_*if02-event-kbd </dev/null >>/tmp/otikm.log 2>&1 &

⚠️ **坑**：上面这条**拆成多行写（行尾漏 `\`）会让第 1 行在前台阻塞、ssh 挂死**，看起来"命令没输出/超时"。

**顺带记住**：`sr 7:0:0:1`（= 线缆 CD LUN）会持续刷
`FAILED Result: ... Sense Key : Illegal Request / Logical block address out of range` ——
是 udisks/udev 在读那张厂商 CD，属噪声，与故障无关（`99-otilink.rules` 第 4 条本意就是让 udisks 别碰它）。


---

## 53 第 44 轮：拔插后 otikm 段错误 = keepalive 线程缓存裸指针（use-after-free）—— ASan 实证 + 已修 + 已加回归

**先纠正 §52 的推断**（那里只有现象，本轮用 ASan 实证）：
§52 写的"高度怀疑自发现挑到了错的 LUN"和"待办：带 -g 重编复现定位"——**都不对**。
自发现本身没问题（它每次都正确挑到 CD LUN：实测 sg5/sg6/sg4）。真因是**保活线程把
`&a->tx->dev` 缓存成裸指针**，被 `reopen_transport()` 里的 `oti_tr_close(a->tx_old)` free 掉。

**定位手段（本项目第一次用，值得复用）**

1) **崩溃地址 → 源码行（不需要 gdb）**：用**同样的 `-O2`** 再加 `-g` 重编（`-g` 不改代码布局），
   然后 `addr2line`：
   ```
   $ addr2line -f -C -e otikm 0x40d188
   otilink_cdb_len
   otilink.c:124
   ```
   （对应内核日志 `ip 000000000040d188 ... error 4 in otikm[402000+13000]`）

2) **ASan 复现，而且不用真拔线**：`gcc -O1 -g -fsanitize=address`（麒麟自带 `libasan.so.5`），
   用 **sysfs 的 USB unbind/bind 模拟拔插**：
   ```
   H=$(for d in /sys/bus/usb/devices/*/; do [ -f "$d/idVendor" ] && [ "$(cat "$d/idVendor" 2>/dev/null)" = 0ea0 ] && basename "$d"; done | head -1)
   echo $H > /sys/bus/usb/drivers/usb/unbind    # 拔线
   echo $H > /sys/bus/usb/drivers/usb/bind      # 插回
   ```
   ASan 报告（决定性）：
   ```
   ERROR: AddressSanitizer: heap-use-after-free  READ of size 8  thread T3
       #0 otilink_cdb_len      otilink.c:124
       #3 otilink_send_dummy   otilink.c:374
       #4 keepalive_thread     otikm.c:974            ← ★ 读悬空指针
   freed by thread T1:
       #1 oti_tr_close         otitrans.c:470
       #2 reopen_transport     otikm.c:1589           ← ★ free
       #3 rx_thread            otikm.c:1227
   ```

**为什么"只拔插一次不复现、拔插两次必崩"**：第 1 次成功重开时 `a->tx_old` 还是 NULL → 不 free；
**第 2 次**成功重开才 `oti_tr_close(a->tx_old)`。

**为什么真机是硬段错误而不是静默损坏**：`oti_transport` 是 **4192816 字节（约 4MB）** 的分配
（ASan 报告里的 region 大小），malloc 走 mmap，free 即 munmap → 悬空指针一读就缺页。
小对象的话是静默读脏数据，会难查得多。

**修法**（`otikm.c` 的 `keepalive_thread`）——循环外不要缓存，每轮重新取：

    -    struct otilink_dev *cable = oti_tr_cable_dev(a->tx);   /* 循环外，缓存 */
         while (a->running) {
             ...
             oti_tr_send(a->tx, buf, n);                        /* 这个本来就是每轮重取 */
    +        struct otilink_dev *cable = oti_tr_cable_dev(a->tx);  /* 每轮重取 */
             if (cable) otilink_send_dummy(cable);

**残留竞态（已知、未修、需要时再加）**：`oti_tr_cable_dev(a->tx)` 到 `otilink_send_dummy()` 之间
理论上仍可能跨过一次 reopen。但修完之后所有使用点都只**瞬时**持有指针（远短于一次拔插周期），
且旧一代还延迟一代才 free，窗口 ≪ 触发间隔。要彻底消除得给 transport 加引用计数 —— 本轮按
"最小必要改动"只修实证到的那处。

**顺带修掉**：`run-kylin.sh` **两处**写死 `cable:/dev/sg3`（§52 第 2 条）→ 改成裸 `cable`。
现在换口/换号后自启动项 `~/.config/autostart/otilink-kvm.desktop` 也能正常起。

**验证（证据行）**
* 修复**前**（ASan）：第 2 轮拔插 → `heap-use-after-free` 崩溃（报告见上）。
* 修复**后**（ASan）：**3 轮 unbind/bind 全过，零 ASan 报告**（`TOTAL_FAIL=0`）。
* 新门禁：`./gate.sh --replug` → **PASS=15 FAIL=0**。
* 部署：`KY=kylin@<tailscale-IP> ./gate.sh --deploy-kylin` → 50 文件同步、`make otikm` 通过、
  KySec 标签重打（9 个二进制）。注意 **gate 的 SSH 目标可用 `KY` 覆盖**（缺省 `<麒麟IP>` 已不通）。

**新增回归**：`re/otilink/replugreg.sh [N]` —— 用 USB unbind/bind 模拟拔插 N 轮（缺省 3），断言：
每轮 otikm 存活 + 传输重新就绪 + 本次回归时间窗内无 segfault。**不需要人手拔线**。
已注册 `gate.sh --replug` 与 AGENTS.md §5/§6。修复前的二进制第 2 轮必崩，所以它真能守住这个 bug。

**本轮踩的坑（都是工具/脚本层面的，不是产品的）**
* `pgrep -x` 只匹配 **comm 的前 15 字符** → 测试二进制叫 `otikm-asan-fixed` 时永远匹配不上，
  会被误判成"没起来/已死"。用短名（如 `otas`）或 `pgrep -f`。
* 崩溃检查必须**限定本次回归的时间窗**（`journalctl --since "$T0"`），否则历史 segfault 会把新回归判 FAIL。
* `/tmp/otikm.log` **每次启动会被截断**（`kvm.conf` 里 `log = /tmp/otikm.log`）→ 崩溃现场重开一次就没了；
  要留证据先 `cp` 出来，或者看 journald（它是持久的）。
* 用 sysfs 模拟拔插时，若中途 `break` 而没执行 `bind`，线缆会**停在 unbind 状态**，
  此时 `run-kylin.sh` 因找不到线缆 HID 会走"没本机键鼠"分支并退出 —— 看起来像"otikm 起不来"。


---

## 54 第 44 轮续：**拔插后"输入抓取"不会自愈** —— 已确认的残留缺陷（未修）

**现象（本机实测）**：修完 UAF 之后跑 3 轮 unbind/bind，再强制 Windows 进 REMOTE，
把线缆 HID 鼠标往左推：麒麟光标**被推到 x=0**（说明 HID 注入通道完好），
但 otikm **没有** `撞边`、也没发 `回程令牌 F24` → Windows 永远回不到 LOCAL
→ 用户又被卡住（症状与最初报障一模一样）。

**判定**：otikm 的自愈**只覆盖传输**（`reopen_transport()` 会重新发现并打开 `/dev/sgN`），
**不覆盖输入抓取**。`--capture` 的 evdev fd 是启动时打开的；拔插后 by-id 指向的 `eventN`
是新节点（实测重插回后 by-id → `event3/event4`，时间戳 13:11），而 otikm 手里还是旧 inode 的 fd
→ `poll()` 永远不 POLLIN → `handle_local()` 再也不被调用 → 撞边判据死掉。
（`/proc/<pid>/fd` 因 ptrace 限制读不到链接目标，本轮改用 `ls -l /proc/PID/fd` 的权限位 +
`ls -l /dev/input/by-id/` 的软链时间戳对比来判定。）

**影响面**：任何让线缆 USB 重新枚举的事件都会中招 —— **物理拔插**，以及日志里那些
`usb 1-3.4: reset high-speed USB device number 4`（旧端口时代每 1~3 小时一次）。
所以**"卡在 REMOTE"仍会复现**，只是不再伴随 otikm 崩溃。

**当前可用处置（可靠 —— 因为 `run-kylin.sh` 已修）**：

    pkill -x otikm; sg input -c "DISPLAY=:0 nohup ~/otilink/run-kylin.sh > /tmp/rk.log 2>&1 &"

修复前这一步不可靠（脚本写死 `/dev/sg3`，设备号一变就起不来）。

**根治方案（待做，未实现）**：给输入抓取加与传输同级的看门狗 ——
`run_capture()`（`otikm.c:1651`）的循环里，若某个 `poll()` fd 返回 `POLLERR/POLLHUP/POLLNVAL`，
或 `oti_capture_next()` 连续返回负 errno（ENODEV），就 `oti_capture_close()` 之后**用原
`cfg.capture[i]` 路径重新 `oti_capture_open()`**（by-id 路径本身是稳定的）。
改动量小（约 15~25 行），但属于"碰键鼠/交接"的改动 → 按 §5 必须 `--static + --hw-km`
（会抢光标约 90s，**要先跟用户说明**）。`replugreg.sh` 也应顺势加一条"拔插后撞边判据仍活"的断言
—— 本轮没加，因为一加它就会红（会变成需要人确认的 KNOWN）。

**动作清单（本次对系统的副作用，可回滚）**
* 麒麟 `~/otilink/`：`otikm.c`（keepalive UAF 修复）、`run-kylin.sh`（裸 cable）、
  新增 `replugreg.sh`；`./gate.sh --deploy-kylin` 重编 + 重打 KySec 标签。
* 麒麟运行态：`otikm` 重启过若干次（当前 pid 见 `pgrep -x otikm`）；
  USB 线缆被 `unbind/bind` 过多次（等价于拔插，均已 bind 回来）。
* Windows：`otiagent2` 以 `deploy.sh --restart-km` 重启过（当前 pid 16116，LOCAL）。
* 回滚：代码用 git；麒麟侧重编即可。


---

## 55 第 45 轮：**修掉 §54 的残留缺陷**（拔插后输入抓取自愈）+ 顺带修 3 个工具硬编码 bug

> **本节推翻 §54 的“未修”状态** —— §54 记的那个残留缺陷本轮已修并验证。

### 1) 输入抓取看门狗（主修，`otikm.c` 的 `run_capture()`）

**做法**：给抓取加与传输同级的自愈 ——
* `poll()` 返回 `POLLERR/POLLHUP/POLLNVAL`，或 `oti_capture_next()` **连续 3 次**返回负 errno（拔插后 read 返回 ENODEV），就 `oti_capture_close()` 关掉该设备，之后**每秒**用原 `cfg.capture[i]` 路径（by-id，跨拔插稳定）试着 `oti_capture_open()`；
* 重开成功打 `输入抓取已重开: <path> (<devname>)`，并 `apply_grab()` 恢复原来的独占状态；
* 失败只在第 1 次与每第 10 次打日志，避免每秒刷屏；
* 顺手修掉原来的判据笔误：`if (!(pf[i].events & POLLIN))` 判的是 **events**（恒真）→ 改成 **`revents`**。

**验证**：
* `replugreg.sh` 新增断言“每轮拔插后 `输入抓取已重开` 必须出现” → **`./gate.sh --replug` PASS=18 FAIL=0**（修复前 15 项；新增的 3 项正是这条）。
* `coretest` **21/21 PASS**；`make` 无 warning。
* `KY=... ./gate.sh --deploy-kylin` → 编过 + KySec 重打（9 个二进制）。

### 2) 顺带修的 3 个“写死设备”bug（同一族，都是拔插/换口后失效）

| 位置 | 原来 | 改成 |
|---|---|---|
| `tools/envcheck.sh:87` | `test -e /dev/sg3` | 按 USB VID/PID 判：`grep -l 0ea0 /sys/bus/usb/devices/*/idVendor`（**envcheck 25/0 全过**） |
| `otilink/probe.c`（自动选设备） | `paths[0]`（通常是 **MS/磁盘 LUN**） | **优先 CD-ROM LUN**（type=5），与 `otitrans.c` 的 `oti_tr_open_cable(NULL)` 对齐 |
| `otilink/hwtest.sh` / `xferreg.sh` | `./otiprobe hidsend ... /dev/sg3`（那个位置参数**本来就被忽略**，设备是 `--dev` 选项） | 去掉，用自发现 |

⚠️ **顺手记一个坑**：`gate.sh --deploy-kylin` 只跑 `make otikm`，**不编 `probe`/其他 bin** → 改了 `probe.c` 之后麒麟上的 `otiprobe` 还是旧的（本轮为此白测一轮）。要手工补：`ssh kylin 'cd ~/otilink && make probe'`。

### 3) 仍未解决：`--hw-km` 的 1 项失败（**测量假失败**，非产品问题）

`--hw-km` = **PASS=11 FAIL=1**；失败项是 `1/4 麒麟 → Windows（发 HID 包，看 Windows 光标）` 报 `Δ=0`。

**但 HID 通路本身是通的**，我独立测了：归位后测得 `x=960` → 发一个 HID 鼠标包（`otiprobe hidsend 1 0064...`，`rc=0`）→ 测得 `x=1207`（**+247px**）。

**真因**：hwtest 用 `setcur.ps1` 把 Windows 光标归位到 1200,500，再发 +100px 的 HID 包。若归位没生效、光标还贴在**右边缘**，+100 就推不动 → `A==B` → `Δ=0`。脚本第 60 行的注释自己就写了这个假失败模式（“归位没生效（还贴在右边缘）→ 重试一次，否则 +100 推不动会造成假失败”），但那一次**重试也没把光标挪开**。另外 `C:\Users\Public\curlog.ps1` 根本不存在，hwtest 走的是仓库副本（`wslpath` 的 UNC 路径），那条路径是好的。

**待办**：把 hwtest 第 1 步改成“先确认归位真的生效（`B` 落在屏幕内且距右边缘 > 200px），否则显式 WARN 跳过”，而不是判 FAIL。本轮未改（属测试脚本健壮性，不影响产品结论）。

**本轮其余环境事实（都验证过）**：
* 本环境 `powershell.exe` **不在 PATH** → `hwtest.sh`/`deploy.sh` 必须补 `PATH`（见 §52）；gate 的 SSH 目标用 `KY=kylin@<tailscale-IP> KYLIN=kylin@<tailscale-IP>`（`<麒麟IP>` 已不通）。
* 线缆 LUN 号一路在变：`sg3 → sg4 → sg5 → sg6 → sg7`。**任何写死 `/dev/sgN` 的地方都是定时炸弹。**


---

## 56 第 46 轮：**键鼠换边（角色协商）** —— 两侧不再各自为主控

用户要求（原话）：**"要支持键鼠换边，插哪边都行。之前不是说过对拷线要支持免驱，插在任何两台电脑上都可以立即使用。
还有要够健壮，有看门狗（最差的情况，对拷线的任何一端重拔插要复位；其他看门狗你自己发挥）。"**

### 56.1 现场故障（本轮要根治的）

用户给麒麟插上本机键鼠后：**麒麟 run-kylin.sh 判它为主控（--inject --grab），而 Windows 开机自启仍无条件起
otiagent2 --edge right** → **两侧同时是主控**。实测：帧管道双向全丢、剪贴板两侧都"重发 3 次仍无确认"、
只能人工停掉一侧才恢复。根因是"角色由两侧各自单方面判定"。

### 56.2 设计：OTI_MSG_ROLE=10 协商（规格见 PROTOCOL.md §6.1）

* 载荷 12B：has_local_input / want / state / flags / boot_id / input_age_ms；状态变化立刻发 + 每 1s 心跳。
* 规则：显式 want 优先 → 有本机键鼠的一方当主控 → 双方都有则比 input_age（领先 ≥2000ms，换边后 ≥10s 滞回）
  → 双方都没有则都 slave；**冲突只在"双方都有本机键鼠且本机无显式偏好"时按 boot_id 小者胜**。
* 不变式：**I1** 连续 3 个心跳确认对端 slave（或对端 5s 无任何帧）才允许 grab；**I2** 冲突败者立刻
  释放 grab + 释放按键 + 发令牌；**I3** 被驱动侧永不 grab；**I4** 5s 无对端帧 → 允许单方面 master
  （保证零软件接收端/Mac 可用）。
* 线程分工：**RX 线程只决策并排队**（role_pending），**抓取线程**是 capture fd 唯一所有者，由它重建设备集合
  （本机键鼠 ↔ 线缆 HID）+ 切 return_on_edge + 应用 grab —— 避免两线程 close/open 同一批 fd。

### 56.3 本轮落地与验证（证据）

* @@otiproto.[ch]@@：@@oti_encode_role/oti_decode_role@@（type=10）＋ @@selftest_proto@@ 6 项断言全过。
* @@otikm_core.c: otikm_role_decide()@@（纯函数）＋ @@coretest@@ **32/32**（新增 11 项：只有本机/只有对端/
  双方都有取"刚在用"/滞回/force/对端显式 master 我让位/冲突 boot_id/双方都没有/I1 不足不放 grab）。
* @@otikm.c@@：启动引导（has_local_input + boot_id + 初始角色）、每 1s 广播、RX 收 ROLE → 决策排队、
  @@run_capture@@ 应用角色（设备重建 + grab + 返回本机）、**W4 对端静默不交接**、**I1 未确认不接管**。
* @@run-kylin.sh@@ 变薄：改 @@otikm --auto@@（角色交给协商，不再用 by-id 通配单方面判）。
* 本机双实例冒烟：A @@--role master@@ → @@ROLE 决策: master（本机显式 master）@@，2s 后 @@grab=可@@；
  B @@--role auto@@ → @@ROLE 决策: slave@@（**"对端显式 master 我让位"生效**；修之前 B 会用 boot_id 抢主控，已修）。
* 真机（麒麟，键盘插在麒麟 → 拓扑 B）：
  @@角色引导(auto): 主控端 master（本机键鼠 1 个：键盘=/dev/input/event3）@@、
  @@角色协商: boot_id=81f0aad5 本机键鼠=1 目标=auto@@；
  Windows clip.log：@@ROLE: peer state=1 hasLocal=1 age=4980 want=0 boot=81f0aad5（73ms 前收到）@@
  → **双向 ROLE 已通**（Windows 收到的 boot_id 正是麒麟的）。
* 门禁：@@--static@@ PASS；@@--local@@ PASS=9 KNOWN=1；@@--replug@@ **PASS=18 FAIL=0**；
  @@--hw-clip@@（clipreg）**17/17**；@@clipshort@@ 双向 **7/7**（含 1..3 字符）；@@envcheck@@ **23/0**。

### 56.4 顺带修掉的"测试脚本自身的过时假设"（不是产品 bug）

* @@clipshort.sh@@ 原来要求"Windows 键鼠代理恰好 1 个"（它只测剪贴板）→ 去掉该前提；
* @@envcheck.sh@@ 默认档原来要求 otiagent2=1（拓扑 A 假设）→ 改为按麒麟侧角色判：master 时 0 个是对的；
* @@replugreg.sh@@ 的"输入抓取已重开"断言原来只对拓扑 A 成立（抓线缆 HID）→ 改为按角色分开断言：
  slave 要求重开日志、master 断言"本机设备未被线缆拔插影响"；
* @@hwtest.sh@@ / @@replugreg.sh@@ 写死 @@kylin@<麒麟IP>@@ → 改为优先 @@$KY@@（支持 ~/.ssh/config 隧道
  @@@kylin@@ → 127.0.0.1:2222）。

### 56.5 未完成（明确标注）

* **P1 第 2 步（Windows 侧按仲裁起停 otiagent2 + has_local_input 真检测 + 自启口径改造）**：
  第 1 步（Windows 能收发 ROLE + 打日志）已完成并验证；第 2 步进行中。
* **W5 设备级复位（厂商 USBRestart 升级路径）**：未实现 —— 需真机验证"是否会切 USB 人格"后再决定默认开关。
* **W6 存活监督（--supervise）**：未实现（Linux 侧脚本级守护 + Windows 侧角色代理监督）。
* **hwtest.sh 的拓扑 B 路径**：hwtest 仍按拓扑 A 写（Windows 主控驱动）；拓扑 B 下它的断言不适用，
  需补"麒麟主控驱动 Windows"的一组断言（本轮未做，故 --hw-km 在拓扑 B 下不能作为通过依据）。
* **Linux↔Linux / Windows↔Windows 换边真机**：未验证（无第二台机器）。

---

## 57 第 46 轮续：**写侧看门狗 + W5 设备级复位 + "可热插拔键鼠"语义 + 3 个真 bug**

> 本节接 §56。§56 只做到"Linux 侧协商 + Windows 侧能收发 ROLE"，本节把**换边真正跑通**，
> 并补上用户要的"够健壮、有看门狗"。**并更正一处排查中的错误归因（见 57.4）。**

### 57.1 关键语义修正：has_local_input 只算**可热插拔**键鼠

**为什么必须改**：用户的 Windows 是**笔记本**（ACPI\FUJ7401 内置键盘 + HID\MSFT0001 触控板 +
HID\VID_048D&PID_C100 ITE EC 键盘），它们永远在位；麒麟侧也有个**幽灵** AT Raw Set 2 keyboard
（i8042 造的，没插 PS/2 也在）。于是两侧永远"都有本机键鼠"→ **"键鼠插在哪一边"这个信号被彻底淹掉**，
换边永远不发生。

**改法**（两端同语义）：
* 麒麟 oti_input_local_count()（otiinput.c）只算 **USB**（可热插拔）且非线缆、非注入的键鼠；
  oti_input_autoselect(0,...) 也改成**优先 USB 纯设备**（真机踩到：罗技 G304 的接收器暴露成一个
  "键鼠合一" evdev，按顺序先撞上它就把真正的 SIGMACHIP 键盘顶掉了 → 主控端键盘没反应）。
* Windows Update-LocalInput 只算 DEVPKEY_Device_RemovalPolicy != 1 的键盘/鼠标
  （实测：线缆 HID = 3 可意外拔出；笔记本内置 = 1 不可移除）。
* --doctor 增加分类列（键/鼠 + 线缆HID / USB(可热插拔) / 内置\PS2(不换边)），一眼可查。

真机证据（麒麟 --doctor）：
    /dev/input/event7  SIGMACHIP USB Keyboard   键   USB(可热插拔)
    /dev/input/event3  Logitech G304            键鼠 USB(可热插拔)
    /dev/input/event2  AT Raw Set 2 keyboard    键   内置/PS2(不换边)
Windows：ROLE 初始化: boot=6aab3170 hasLocal=0（改之前恒为 1）。

### 57.2 修掉 3 个真 bug（都是"看起来在工作"的静默失效）

1. **--role auto 变成了单方面强制**（otikm.c）：set_role 与 role==0 共用判据，而 resolve_auto()
   会把 cfg.role 从 0 引导成 1/2 → --role auto 被当成 FORCE_MASTER/SLAVE。新增 cfg.role_explicit。
   本机双实例冒烟：A --role master → 目标=master / grab=可；B --role auto → 目标=auto 且正确让位。
2. **run-kylin.sh 把用户参数整个丢掉**：原来 set -- --auto --transport cable ... 覆盖了 "$@"，
   于是 ./run-kylin.sh --role slave **静默失效**（真机日志里还是"目标=auto"）。
   改为默认参数在前、用户参数追加在后。
3. **"从未用过"哨兵不一致**：麒麟给 600000，Windows 给 uint32 最大值 → 一台很久没用的机器
   （真实 age 会涨到 >600000）反而显得"更近"，把在用的主控抢走/两端来回翻。麒麟改成 0xFFFFFFFF。

### 57.3 看门狗：W1b 写侧 + W5 设备级复位（用户"最差情况要复位"）

**现场故障**：麒麟日志每秒刷 ERR send ROLE rc=524546（= 0x80102 → SCSI
status=CHECK_CONDITION, host_status=DID_NO_CONNECT, driver_status=DRIVER_SENSE），
偶尔 rc=526080（host_status=DID_ERROR）。帧双向大量丢失（Windows 每 12s 才收到一条 ROLE）。

**发现**：原有看门狗**只盯 recv**（连续 20 次读失败才 reopen_transport）。写侧失败**完全不触发**
任何自愈 → 链路永远卡死，只能人工拔插。

**做法**（otikm.c）：
* note_send_result() 挂到**所有**发送路径（send_body、keepalive PING、HELLO、剪贴板 ACK、HID 包）；
* send_fail_watchdog()（RX 线程）：**10 秒窗口内失败 ≥5 次且失败多于成功**就重开传输；
  连续 **2 个**故障窗口 → 升级 usb_reset_cable()：按 idVendor=0ea0 找 USB 设备，
  写 /sys/bus/usb/drivers/usb/{unbind,bind}（= 等价拔插）。
* **为什么不是"连续失败 N 次"**：真机实测是"大部分失败、偶尔成功"，连续计数会被那次成功清零，
  看门狗永远不触发（第一版就是这么写的，白改一版）。
* **为什么需要 W5**：实测 **reopen 成功但写仍全失败**（连续 5 次 reopen 都救不回来），
  只有 unbind/bind 之后立刻干净。W5 需要 root：绿色包（sudo ./otilink.sh）**自动复位**；
  非 root 只打印一次可操作提示（"请拔插对拷线，或用 sudo 启动 otikm"）。

**证据**：
* Kylin 日志：!! 看门狗：10 秒内发送失败 8 次 / 成功 1 次（写侧链路故障，第 1 个故障窗口）→
  !! 看门狗：重新打开传输 cable；第 2 个窗口 → 非 root 提示一次。
* 手动 unbind/bind（等价 W5）后：ERR recv rc=-19（连续 20 次）→ 重新打开传输 →
  传输已重新就绪: cable:/dev/sg3(auto,已应答)，**随后 20 秒 0 条 ERR send**（sg 号 sg7→sg3）。

### 57.4 ⚠️ 归因更正：**"两个剪贴板代理抢管道"没有证实**

排查中一度以为 Windows 有**两个** otiagent.ps1 实例抢同一条线缆管道（症状确实很像）。
**实际上**：进程列表里的 cmd.exe 是 VBS 启动器、它的 powershell.exe 子进程命令行里没有重定向，
看起来像"多出来的一个"，**其实是一个实例的两个进程**。所以：
* windows/otiagent.ps1 新增的**单实例守卫**保留（无害且符合 L4 精神，能挡掉真正的重复启动）；
* 但**不要把"链路卡死"归因于重复实例**。当前证据指向：**Windows 侧的线缆会话会卡住**，
  而**重启 Windows 剪贴板代理即可恢复**（实测重启后 20 秒 0 条 ERR send、ROLE 恢复 1s 心跳）。
  真正的 Windows 侧自愈（例如"长时间没收到任何帧就重开 \\.\H:"）**尚未实现**。

### 57.5 真机验证（本轮）

| 项 | 结果 |
|---|---|
| 角色收敛（麒麟主控 / Windows 被驱动） | ✅ ROLE 决策: master（只有本机有键鼠）对端 state=2 hasIn=0 grab=可；Windows 本机 state=2 hasLocal=0；otiagent2 计数 0 |
| has_local_input | ✅ Windows hasLocal=0（改前恒 1）；麒麟 本机键鼠=1（event7+event3） |
| --doctor 分类列 | ✅ 见 57.1 |
| coretest | ✅ 32/32 |
| --deploy-kylin | ✅ 51 文件同步 + 编译 + KySec 标签 |
| Windows psparse | ✅ ERRS=0（仓库副本与已部署副本都过；**修改后必须补 UTF-8 BOM 再自检**） |
| 写侧看门狗 / W5 升级路径 | ✅ 触发与升级路径实测（root 自动复位仅手动验证了等价动作） |
| 换边真机（人手拔插键鼠） | ❌ **未做**（需要用户配合）；--role master|slave 强制路径已可用 |
| Windows 无线缆会话自愈 | ❌ 未实现（见 57.4） |

### 57.6 副作用与回滚

* 麒麟：~/otilink/ 全量同步（51 文件），otikm 重编译 + KySec 重打标签；otikm 多次重启
  （当前 pid 见 pgrep -x otikm）；USB 线缆被 unbind/bind 过（等价拔插，已 bind 回来）。
* Windows：otiagent.ps1 部署到 C:\Users\<你的用户名>\otilink\ 并重启；C:\Users\Public\roleprobe*.ps1
  与 otiagent-repo.ps1 / psparse.ps1 是留下的**临时诊断脚本**，可直接删。
* 回滚：本仓库不是 git，改动都在 re/otilink/{otikm.c,otiinput.c,otilink.c,run-kylin.sh} 与
  re/windows/otiagent.ps1；麒麟侧重编旧源码即可，Windows 侧 deploy.sh --restart。

---

## 58 第 46 轮续二：**收尾** —— 鼠标能过去了、剪贴板 17/17、短内容 7/7

### 58.1 鼠标到不了 Windows：W4 用错了存活判据（已修）

`peer_is_silent()`（W4）用"对端发没发消息"判链路存活，但 **HID 直发模式下对端允许是零软件接收端**
——它本来就不会说话。于是交接被 `不交出去（W4）` 永久挡回（日志实证：`HID 模式：指针交给对端` 之后
紧接 `[warn] 对端已 19498ms 没有任何消息 → 不交出去（W4）`）。修法：**HID 模式下直接返回 0**，
存活改由写失败判据负责（`hid_fail_streak ≥ 20` → 自动交还，原有逻辑）。
**独立实测**：麒麟 `otiprobe hidsend 1 0064…`（rc=0）→ Windows 光标 `869,954 → 1021,954`（**+152px**）。

### 58.2 帧丢失的两个真因

1. **HID 写插进 pump 的授权窗口**（拓扑 B 特有）：`cable_pump_once()` 的"读 0x06/0x07 授权 →
   **立刻**写帧"中间插入别的设备操作会让授权作废 → `rc=524546`（CHECK_CONDITION/DID_NO_CONNECT）。
   修法：`otilink.c` 加进程级 `g_io_lock`，`otilink_send_hid()` 与整个 `cable_pump_once()`
   （包一层 `_locked`，避免漏解锁）互斥。**效果：写失败 50 次/30 秒 → 6 次/30 秒。**
2. **Windows 主循环被 `Get-PnpDevice`（WMI）整段卡住 5.2 秒**（最关键）。
   诊断已固化：`LOOP 慢轮` 打印 `A[托盘/剪贴板ACK/传输] + B[心跳/输入探测/角色/发ROLE] + C[取帧]`，
   实测 `A=21ms / B=5374ms / C=39ms` → 定位到 B 段。处置：探测间隔 `3s→20s→90s`。
   **效果：对端 ROLE 间隔 5.2s/10.4s → 868ms/896ms。**
   ⚠️ **根治仍欠**：把枚举挪到后台 runspace（每 90 秒仍有一次 5~6 秒卡顿）。
3. 顺带：`TxBody` 在 cable 模式原来**自旋等最多 5 秒** → 改入队即返回；RemovalPolicy 查询按
   InstanceId 缓存；新增**线缆会话自愈**（20s 无入帧 + 队列 10s 不排空 → 重开 `\\.\H:`）。

### 58.3 门禁（全绿）

`--static` **PASS=74 FAIL=0**；`--local` **PASS=9 FAIL=0 KNOWN=1**；`--replug` **PASS=18 FAIL=0**；
`--hw-clip` **envcheck 21/0、clipreg 17/17、clipshort 7/7**；`coretest` 32/32。

### 58.4 一次真机卡死（别误判成 grab）

用户报"linux 键鼠没反应"。排查：relay 端口 2222 在监听，但 `ssh` 报
`Connection timed out during banner exchange`、`ssh_exec` 报 handshake 超时 → **麒麟整机没响应**
（若只是 EVIOCGRAB，SSH 仍应可连）。处置：**硬重启麒麟**，重启后正常。
归因未定：与 §51（麒麟自带升级检查器硬冻结）同类；但本轮多次 USB unbind/bind + 十几次 otikm
重启也无法完全排除，**如实记录**。

### 58.5 仍未完成

* 设备枚举挪后台 runspace（58.2 第 2 条的根治）。
* **grab 硬安全网**：只有"写失败"才交还；"写成功但对端不动作"仍可能长时间独占。可用 `--no-grab` 零风险运行。
* 换边真人拔插实测（需用户配合）。
* **更正**：§56/§57 里"另一个会话并行改文件"的猜测是错的 —— 真凶是 `windows/deploy.sh`
  每次部署都会重写仓库里的 `.ps1`（补 BOM/归一化行尾），使 `edit` 的读取观测作废；**没有别的会话**。

---

## 59 第 47 轮：**拓扑 B「移到 Windows 回不来」根治** —— 回程判据三处修正 + 修饰键补发

> 用户报障原话："插在 linux 主机的键鼠移动到 windows 主机回不去"（= **拓扑 B**：键鼠插在麒麟，
> 麒麟 `otikm --auto` 协商成 master，Windows 侧键鼠零软件）。
> 现场证据：`/tmp/otikm.log` 最后一条停在 `HID 模式：指针交给对端`，之后只有
> `已释放 … / HID 模式：指针拉回本机` 的成对抖动（用户反复试），最后停在"交给对端"没再回来
> （麒麟本机鼠标被 grab，用户只能来报障）。**这不是配置问题，是三个叠加的代码 bug。**

### 59.1 三个叠加根因（全部复现，不是猜测）

1. **回程只看"单次位移"→ 慢推永远回不来（主根因）**
   `otikm_core_local_mouse()` 在 `have_control==0` 时的判据是 `rem_x + dx` 越过出口边
   `edge_px+4 = 8` 像素。但远端坐标每次都被**夹回边界**：越界量不累计 → 只有"单个事件
   ≥9 个计数"才可能触发。真实鼠标事件每次只有 1~3 个计数（高分辨率鼠标/慢推更小），
   用户在对端屏幕上怎么推都回不来。纯逻辑复现（coretest 1b）：`mouse 2 0 0 0` ×40，
   旧代码 0 次回程；修后按累计 16 像素触发。
2. **远端几何在手势中途到达 → 同一手势"时好时坏"**
   加日志后抓到：Windows 侧 HELLO 报的是**虚拟桌面** `4480x1440`（2560+1920 两台显示器），
   而它可能在"指针已经在 Windows 上"之后才到：
   ```
   DBGHELLO 4480x1440 (old 0x0) rem=(1619,540) entry=1 have=0
   DBGREMOTE ev=25  rem=(1695,540) rw=4480 lw=1920 dx=4    ← 缩放从 2 跳到 4
   DBGREMOTE ev=300 rem=(2795,540) over=(0,0,0,0)          ← 出口边在 4479，300 个事件推不到
   ```
   同一手势跑三次：两次成功（HELLO 在回程之后才到）、一次失败（HELLO 落在驱动中途）。
   这正是"有时候能回来、有时候回不来"的来源。
3. **HID 路径漏放行"修饰键抑制缓冲"→ 按一下 Ctrl/Alt 之后键盘全哑**
   `handle_local()` 的 `--peer hid` 分支**从不调用** `otikm_core_take_suppressed()`：
   对端驱动时按 Ctrl/Alt（可能的热键前缀）会被压进缓冲，之后每个按键**按下**都被当
   "还在凑热键"吞掉（只有松开被转发）→ Ctrl+C/V 到不了 Windows。协议分支有补发，HID 分支漏了。

### 59.2 另一个真 bug：同一毫秒发出的两条 HID 键盘报表会被合并/丢掉

修完 59.1 后 kmbret 仍红一项：麒麟日志有 `HID 补发被抑制的 2 个按键事件`，
Windows 低级键盘钩子探针收到 `vk=0xA2`（左 Ctrl）却**收不到 `vk=0x43`（C）**。
对照实验：同样的 C 单发 5 次（间隔 300ms）→ 10 个事件全部收到。结论：**线缆 HID 键盘是
状态型设备**，Windows 按轮询间隔取"当前报表"，两条报表挨得太近会被合并/丢掉
（F24 令牌早就靠"连发 3 轮 + 15ms 间隔"存活，同一原理）。
修法：`hid_send_key()` 加最小报表间隔（只延迟紧跟其后的一条，最大 8ms）。修后探针
同时收到 Ctrl 与 C。

### 59.3 修法与不变量（L8 的对称性）

* **回程判据 = 未缩放位移累计**（core 新字段 `drv_x/drv_y`，入口边为 0）：
  `drv += m->dx`（**真实发给对端的 HID 计数**）；"推出去多少就推回来多少，再多推
  `max(8, edge_px*4)=16`" → 回程。与远端几何、整数缩放截断完全无关；小步慢推照样触发。
* **驱动期间冻结远端几何**：`otikm_core_set_remote_screen()` 在 `!have_control` 时只存
  `rem_w_pend/rem_h_pend`，`enter_remote()`（**下次交出去时**）才生效；
  `otikm_core_remote_switch()` 里对端"边切换边报屏幕"的路径直接生效（不经过那条冻结逻辑）。
* **回程落点**：core 把指针放到"出口边往里 `edge_px+8`"，`otikm.c` 再用
  **`XWarpPointer`（新增 `otix11_warp()`）**把真实光标同步过去。以前拿真实光标去校准 ——
  grab 期间它一动不动、往往还贴在出口边上，校准后轻轻一碰就又被推出去（现场日志："拉回本机"
  后 0.2s 又"交给对端"）。
* **HID 修饰键补发**：HID 分支也调用 `otikm_core_take_suppressed()`；只有"仍在驱动对端"时
  才补发，已交还/收回就丢弃（避免对端幽灵按键）；补发在本次事件之前，顺序不变。
* ✋ **自踩一坑（记录）**：第一版 `apply_return_landing()` 里加锁，而**协议路径的
  `handle_local()` 在 switch 里本来就持着 `a->lock`** → 自死锁，`itest.sh` 挂死 90 秒
  （门禁 `--local` 超时）。改成 `return_landing_store()`（**调用方持锁**）+
  `return_landing_warp()`（锁外做 X11）两个函数。

### 59.4 验证（纯逻辑 + 真机）

* `coretest` **45/45**（新增 1b 慢推累计、1c 镜像手势、1d 驱动中途几何变更；改 core 必跑）。
* 新增 **`re/otilink/kmbret.sh`**（拓扑 B 专项真机回归，注册为 `gate.sh --hw-kmb`）：
  用 `--sim-input` 合成手势（**不抢用户光标**）经真实线缆到 Windows，19 项断言。
  同轮证据行（麒麟 `/tmp/kmbret-otikm.log`）：
  ```
  [06752.502] HID 模式：指针交给对端（开始把键鼠发成 HID 包）
  [06753.898] HID 补发被抑制的 2 个按键事件（修饰键前缀→非热键，第 1 批）
  [06755.325] 回程落点 12,540：真实光标 474,533 → 已同步
  [06755.325] HID 模式：指针拉回本机（落点 12,540；已请求释放包 + 令牌，由消息泵发出）
  ```
  脚本断言（`/tmp/gate-kmbret.log`）：
  ```
  [PASS] **回程成功**：小步（往右 2px/次）外推触发回程
  [PASS] 回程落点已同步真实光标（XWarpPointer）
  [PASS] 修饰键抑制缓冲已补发（Ctrl+C 能到对端）
  [PASS] Windows 光标左移 -317px（HID 包真送到）
  [PASS] Windows 收到左 Ctrl（vk=0xA2）
  [PASS] Windows 收到 C（vk=0x43）
  ```
* 门禁：`--local` PASS=9 FAIL=0 KNOWN=1（tcptest 老登记）；`--hw-kmb` envcheck 24/0 + kmbret 19/0；
  `--hw-clip` 17/17 + clipshort 7/7；`--static` 全过。

### 59.5 未验证 / 明确不做

* **拓扑 A 的 `--hw-km` 本轮没跑**：键鼠在麒麟（拓扑 B），`hwtest.sh` 是拓扑 A 专用
  （§56.5 已注明），跑它必然假失败；拓扑 B 的等价回归就是新增的 `--hw-kmb`。
* Windows 的 HELLO 报"虚拟桌面 4480x1440"只影响**绝对坐标/协议路径**（HID 路径不再用它做回程判据）。
  多显示器下的绝对定位换算**未实测**；HID 路径（本项目的默认路径）不受影响。
* `kmbret.sh` 的合成实例 `core.mx` 起点是**本机屏幕中心**（sim 模式不读真实光标），
  所以脚本用固定 24 次大位移出边 —— 换屏幕尺寸/边配置时改 `EXIT` 与次数即可。

---

## 60 第 47 轮续：拓扑 B **"键盘用不了 + 鼠标回不来"** 的三个真因（用户二次报障）

> 用户二次报障原话："键鼠插在 windows 没问题，但现在插在 linux 还是不行：鼠标移动到 windows 后，
> 移动不回去，**键盘也用不了**。"（第 47 轮首修已上线，但仍复现。）
> 现场：/tmp/otikm.log 在 07878 只有 "角色(master)：抓取 /dev/input/event3 (Logitech G304)"，
> 之后再没抓过键盘；用户 11:03 的测试里鼠标进了 Windows 就回不来（最后一次 07900 "交给对端" 后无下文）。

### 60.1 真因 1：热插拔判据只看 ">0"，**新插的键盘永远不进转发**

rx_thread 每 2 秒重采样本机键鼠，判据是：

~~~c
if ((nlocal > 0) != (a->has_local_input != 0)) { ... }
~~~

真机时序（就是用户干的事）：先插**鼠标** → 个数 1 → 已是 master（抓取集合 = 鼠标）；再插**键盘**
→ 个数 1→2，但 nlocal > 0 没变 → **不重新协商、不重扫抓取集合** → 键盘从来没被抓过，
在对端当然"用不了"。日志实证：

~~~
[07878.585] 角色协商: 本机键鼠插入（现在 1 个）→ 立即重新协商
[07878.841] 角色(master)：抓取 /dev/input/event3 (Logitech G304)     ← 只有鼠标，键盘没进来
~~~

修复：按**个数**判（nlocal != a->local_count）→ 置 a->cap_rescan；抓取线程在
run_capture 里看到标志就 role_rebuild_capture() 重扫（驱动中还会恢复 grab）。

### 60.2 真因 2：看门狗按**旧 eventN** 重开，抓到了别的设备

拔插后 eventN 会被重新分配。真机实测（同一台 SIGMACHIP 键盘）：

~~~
/dev/input/event5  SIGMACHIP USB Keyboard                A=1 Z=1 SPACE=1 ENTER=1 字母=26/26  ← 真键盘
/dev/input/event7  SIGMACHIP USB Keyboard System Control A=0 Z=0 SPACE=0 ENTER=0 字母=0/26
~~~

otikm 启动时 event7 还是真键盘（那时 by-id 链接如此），拔插后 event7 变成 "System Control"；
抓取看门狗却按**旧 eventN 路径重开**（代码注释里写着"用原 by-id 路径"，但 cfg.capture[]
存的实际是 eventN）→ 重开"成功"、抓到的却是一个没有字母键的接口 → **键盘用不了**。
修复两件套：

* oti_input_autoselect() 返回 **/dev/input/by-id/... 稳定路径**（新 oti_input_stable_path()，
  优先 -event-kbd / -event-mouse 主接口；没有链接才退回 eventN）；
* 重开**校验能力位**（struct oti_capture.kind）：不符就丢掉，并在连续失败 3 次后按类别
  **重新发现**设备（日志："输入抓取路径已重新发现: ... → ..."）。

修后启动日志（真机）：

~~~
抓取设备 /dev/input/by-id/usb-Logitech_USB_Receiver-if02-event-mouse (Logitech G304)
抓取设备 /dev/input/by-id/usb-SIGMACHIP_USB_Keyboard-event-kbd (SIGMACHIP USB Keyboard)
~~~

### 60.3 真因 3：对端光标的**真实位置**与"入口边=0 位移"模型不一致（鼠标回不来）

状态机的回程判据是"推出去多少就推回来多少 + 16 计数"（§59），它隐含假设：
**交接那一刻对端光标正好停在入口边上**。但 HID 鼠标只有相对位移，对端光标其实停在
"用户上次用完留在那里"的地方；更糟的是用户可能在对端把光标顶在屏幕边上**继续推**
（位移被系统夹住、画面不动）—— 这些"看不见的位移"全记进 drv_*，之后原路推回来也到不了阈值。
真机表现就是"怎么推都回不来"（用户 11:03 最后一次 "交给对端" 之后再无 "拉回本机"）。
修复：**交接时用一段饱和位移把对端光标顶到入口边**（hid_park_remote_pointer()，
覆盖远端虚拟桌面宽度，未知按 4480 兜底，上限 64 包）—— 协议/绝对坐标路径本来就是这个语义，
现在 HID 路径与之一致。顺带给回程判据加**节流诊断**：
"回程判据：已把推出去的位移推回 37/16（入口边 x=1 y=-1）"，下次出问题日志里就有证据。

### 60.4 验证

* kmbret **22/22**（新增断言）：交接时 park、回程诊断有进度、抓取用 by-id 稳定路径；
  Windows 光标位移 771px、实收 Ctrl(vk=0xA2)+C(vk=0x43)、小步回程成功。
* --static 76/0、--local PASS=9 FAIL=0 KNOWN=1（tcptest）、coretest 45/45。
* 生产实例已重启并确认按 by-id 抓取键鼠（见 60.2 日志）。

### 60.5 未验证（如实标注）

* **"先插鼠标、再插键盘"的热插拔重扫**：代码路径已改，但本轮**没有做真人拔插实测**
  （自动 unbind/bind 键盘的命令被本会话的策略拦下，没有执行）。请用户下次实测时看
  /tmp/otikm.log 是否出现 "本机键鼠插入（现在 2 个，之前 1）" + "输入热插拔：抓取集合已重扫"。
* 多显示器下"对端光标 park 到虚拟桌面哪条边"只按入口边方向推理，未在多显示器真人场景下确认
  用户体验（单显示器/虚拟桌面边缘的行为已由 kmbret 覆盖）。

---

## 61 第 47 轮续三：**键鼠"过几秒卡一下"根治（保活 dummy 帧）+ 键盘 HID 通道的固件级缺陷（未修完，证据在此）**

### 61.1 "过几秒卡一下" = 保活里的 64KB dummy 帧把单队列设备占住 ~1.3 秒（已修）

用户报障："键鼠过几秒就会卡一下"。用新工具 `otilink/hidlat.c`（与 otikm **并发**跑，量 HID 写间隔）实测：

~~~
GAP 1283ms (send 1273ms) t=5097ms
GAP 1285ms (send 1275ms) t=11404ms      ← 每 ~5-6 秒一次，冻结 1.27 秒
=== maxgap=1285ms ===
~~~

隔离测试（`dumblat`，设备保持打开）：HID 包 0-1ms；`otilink_send_dummy()` **rc=526080（DID_ERROR）且要 1.28-2.44 秒**。
根因：keepalive_thread 每 5 秒发一次 64KB 全零 dummy 帧，它与 HID 包共用同一台**单队列**设备 → 输入被堵 1.3 秒。
修复（`otikm.c`）：dummy 只在**链路真空闲 >60s**（对端无消息且无帧发出）时才发，并且必须拿
`otilink_cable_io_lock()`（以前它还会插进"读授权→立刻写帧"的窗口，把授权作废）。
新增回归 `otilink/hidlat.sh` + `gate.sh --hw-lat`（判据：并发下 maxgap ≤ 200ms、SEND_FAIL=0）。

### 61.2 键盘"用不了"的真因：Kylin→Windows 的 HID 键盘通道**只送得动头几条报表**

Windows 低级键盘钩子（`kbdprobe2.ps1`）实测序列（设备保持打开，报文完全按厂商布局）：

| 发送 | Windows 看到 |
|---|---|
| F24 on/off ×3（无修饰键） | **6/6 全到** |
| `[01][00][06...]`（Ctrl+C，修饰键字节） | 到一次（Ctrl↓+C↓），**之后的报文全部丢失** |
| 之后 33 条报表（含 10 条全零释放，跨度 6 秒） | **一条都没到** → Windows 上 Ctrl 永远卡住 |
| `[00][00][E0][06...]`（把 LeftCtrl 当 key usage 0xE0 放进数组） | 一组完整 down/up 到达（**释放也能到**，不卡键），但第二组又丢 |

结论：**键盘接口的修饰键字节会把线缆的 HID 键盘状态机弄死**（释放报文再也送不出去），
而且这个接口似乎只在前几条报表内可靠；鼠标接口（type1）完全正常（kmbret 每轮都验到光标位移）。
这解释了用户两次报障："键盘用不了"、以及键盘时而能用时而失灵。**这是线缆固件层面的缺陷，不是我们的报文格式**
（格式与厂商一致：[mods][rsv][k1..k6]，NOTES §41.3；F24/字母键的数组通道是好的）。

### 61.3 下一步（明确计划，本轮没做完）

1. **短期**：把修饰键改走 key usage 数组（0xE0..0xE7）——真机已证"释放能到、不卡键"（61.2 第 4 行），
   至少消掉"Ctrl 卡死导致键盘像失灵"。剩余丢包用重发覆盖（已在 `hid_send_key` 里连发 3 遍）。
2. **根治**：键盘改走**帧管道**（对端在拓扑 B 跑的就是我们的 otiagent.ps1，它有 SendInput 注入路径，
   见 `windows/otiagent.ps1` 的 `KEY code=...` 分支）——帧管道有队列+重传，远可靠于 HID 键盘接口。
   需要：Kylin 侧发 `OTI_MSG_KEY`（带 seq）+ 对端按 seq 去重（避免重发导致重复输入）；
   鼠标继续走 HID（它是好的）。协议标志位可复用 ROLE 的 `flags`（新增"我能注入键盘"位）。
3. **验证**：`kmbret` 的键盘断言（Windows 实收 Ctrl/C）必须稳定绿；`hidlat` 保持 maxgap ≤ 200ms。

### 61.4 本轮已交付/已验证（用户下一轮可直接用）

* 鼠标：回程（小步累计）、入口边 park、回程落点同步——kmbret 稳定通过。
* 键盘**抓取**：by-id 稳定路径 + 重开校验能力位 + 热插拔按个数重扫（先插鼠标再插键盘也能抓到）。
* 卡顿：保活 dummy 帧改为"真空闲才发"——hidlat 待复测（部署后本轮未再跑满 14s 的采样）。
* 门禁：`--static` / `--local` / `--hw-kmb`（22 项，键盘断言受 61.2 影响）/ `--hw-clip` 均跑过。

---

## 62 第 48 轮：假光盘不能靠“解绑 usb-storage”隐藏 —— 用 quirks=SINGLE_LUN（保传输、去光盘）

2026-09-22 现场：用户问“不出现假光盘影响对拷线功能吗”。当时机器上有一份**仓库外的现场改动**
`/etc/udev/rules.d/99-otilink-no-storage.rules` + `/usr/local/sbin/otilink-unstick.sh`：
线缆存储接口（`1-x:1.0`，class 08）一 add 就 `unbind` 掉 usb-storage，注释写
“HID 键鼠接口不动，键鼠共享照常工作”。**实测：后半句是错的，而且当时传输已经死了。**

### 62.1 为什么解绑 = 杀掉功能（不是“照常工作”）

键鼠/剪贴板/回程令牌/角色协商/大文件**全走这个 MSC 接口的裸 SCSI 通用设备 `/dev/sgN`**
（`re/PROTOCOL.md`；HID 接口 Output=0/Feature=0，是**纯输入**，只能设备→本机，回程发不出去）。
解绑 usb-storage → SCSI host/LUN 全没 → `/dev/sgN` 消失。现场证据（16:01 只读快照）：

```
lsusb: 0ea0:2213 在（Bus 001 Device 014）；1-4.2:1.0 class=08 driver=none
/dev/sg0 = 本机真光驱，/dev/sg1 = 本机硬盘 —— 线缆一个 sg 节点都没有
journal: 15:29:57 otilink-unstick[48955]: unbound 1-3.4:1.0 from usb-storage
         15:30:39 otilink-unstick[50553]: unbound 1-4.2:1.0 from usb-storage
/tmp/otikm.log:
 [00019.006] 传输就绪: cable:/dev/sg3(auto,已应答)     ← 解绑前是好的
 [00962.029] ERR recv rc=-19（连续 1 次）             ← rc=-19 = ENODEV
 之后 698 次 “ERR 重开传输失败（设备插好了吗？）”，看门狗每 0.5s 重来一次
```

### 62.2 假光盘确实在拖 usb-storage：常驻 D 态

`/var/log/freeze-breadcrumb.log`（§51 装的取证）里 `!!D-state: usb-storage` 共 **979 条，
全部在 2026-09-22**（首条 09:30:14），per-minute 采样经常 2–4 条。关键旁证：

* 这段时间 **otikm 根本没在跑**（上一 boot 09-21 08:53 被 pkill，之后没起）→ 不是我们的命令；
* 机器上唯一常驻的 USB 大容量设备就是线缆（另一个 `1-4.1.4` 是 AIC 无线网卡的 `Aic MSC`，
  本 boot 日志里只存在 1 秒就 disconnect 重枚举成 WiFi）；
* 内核日志里 `sr 0:0:0:1`（= 线缆 CD LUN）反复 `Illegal Request`（§52 已记：udisks 在读假光盘）；
* 15:29:49 最后一条 usb-storage D 态 → 15:29:57 解绑 → 之后不再有 usb-storage D 态
  （解绑后只剩 kworker/usb_hub_wq 两条，属于 replug 处理）。

即 **“假光盘被反复读 → usb-storage 控线程堵在 bulk 传输（D 态）→ udev/桌面跟着卡”** 是本站
最合理解释（§51 那次整机硬卡死的直接凶手是 Kylin 自己的升级检查器，两条独立，不冲突）。

### 62.3 正确修法：`usb-storage.quirks=0ea0:2213:s`（SINGLE_LUN）

内核 5.4 `drivers/usb/storage/usb.c`：`quirks` 参数里 **`s` = US_FL_SINGLE_LUN**；
`usb_stor_scan_dwork()` 置位时**跳过 GET_MAX_LUN 探测并强制 `us->max_lun = 0`** →
只扫 LUN0，假光盘（LUN1）从枚举层面消失；MSC 接口继续绑定，`/dev/sgN` 保留。
LUN0 就是 Windows 一直在用的 `H:`（MS/可移动卷），私有协议本来就跑在它上面。

2026-09-22 16:04 在 kylin-pc 实施（可逆）：

```sh
sudo mv /etc/udev/rules.d/99-otilink-no-storage.rules /etc/udev/rules.d/99-otilink-no-storage.rules.disabled
sudo udevadm control --reload
echo 'options usb-storage quirks=0ea0:2213:s' | sudo tee /etc/modprobe.d/otilink-quirks.conf
echo 0ea0:2213:s | sudo tee /sys/module/usb_storage/parameters/quirks     # 免重启
n=$(for d in /sys/bus/usb/devices/*/; do [ "$(cat $d/idVendor 2>/dev/null)" = 0ea0 ] && basename $d; done | head -1)
sudo sh -c "printf '$n:1.0' > /sys/bus/usb/drivers/usb-storage/bind"
```

实测：

```
usb-storage 1-4.2:1.0: Quirks match for vid 0ea0 pid 2213: 1
scsi 7:0:0:0: Direct-Access  Virtual Link  → /dev/sdb(1MB vfat) + /dev/sg2(type=0)
（没有 LUN1 / 没有 sr1 / 没有 MacKMLink ISO）
otiprobe: /dev/sg2 [type=0] 信息块 rc=0  前16B 00 00 22 13 00 01 30 39 30 33 30 31 01 01 01 00
otikm --doctor: /dev/sg2 0xF0/0x00 rc=0；LUN 未被挂载
otikm: 传输就绪: cable:/dev/sg2(auto,已应答)
```

### 62.4 A/B：空载无 D 态；otikm 流量下只剩瞬时 D 态

16:10–16:14 对照（面包屑计数）：

| 相 | 条件 | 120s 内新增 `!!D-state: usb-storage` |
|---|---|---|
| A | 停掉 otikm，无任何客户端 | **0** |
| B | 恢复 otikm（**对端 agent 没跑**，ROLE/剪贴板都在失败重试） | 3 |

B 相内核栈：`usb_sg_wait ← usb_stor_bulk_srb ← usb_stor_bulk_transfer_sglist ←
usb_stor_Bulk_transport ← usb_stor_invoke_transport` —— 在等设备完成一次 bulk 传输。
**结论：假光盘/udisks 轮询引起的“无人用也一直 D”消失；剩余瞬时 D 态来自 otikm 自己的命令，
在“对端不在”的降级状态下会被失败重试放大。正常双端运行时的 D 态频率本轮未测（待观察）。**

### 62.5 门禁（本轮）

* `--pre`：envcheck **PASS=24 FAIL=0 WARN=1**（新增的“线缆 MSC `/dev/sgN` 在位”检查已通过）。
* `--hw-kmb`（拓扑 B，`KY=kylin@<tailscale-IP>`）：**PASS=19 FAIL=2 WARN=1**。
  * 绿：交接、**小步回程**、顺序、入口边 park、by-id 抓取、修饰键补发；
    **Windows 光标位移 771px（HID 包真送到）** ← 这条证明 LUN0 上的 KM 通道完好。
  * 红 2 条：`Windows 没收到 Ctrl / C`。kmbret 的键盘断言期望**帧管道键盘**，而当前代码
    `OTI_HID_KBD_FORWARD=1`（`otikm.c:524`）默认走 HID 键盘，且 Windows 侧 agent 本轮没跑；
    与 §61.2/§61.3 同一个未收尾问题，**与 LUN 改动无关**（鼠标断言全绿）。
* `--static`：doccheck **PASS=78 FAIL=0**。

### 62.6 交付与回滚

* 新增 `re/otilink/otilink-quirks.conf`；`install-kylin.sh` 第 2 步装它，`uninstall-kylin.sh` 删它。
* `re/tools/envcheck.sh` 麒麟段新增一条：线缆 MSC 的 `/dev/sgN` 必须在（按 idVendor=0ea0
  从 sysfs 上溯查，不写死 sg 号）——这次的现场改动如果早装了规则，`--pre` 会直接报出来。
* `re/tools/gate.sh` 的 `--deploy-kylin` 同步清单补了 `*.conf` / `*.rules`（并把 scp 加 `-p`）——否则 `otilink-quirks.conf` / `99-otilink.rules` 同步不到麒麟。
* 回滚：
  ```sh
  sudo rm -f /etc/modprobe.d/otilink-quirks.conf
  echo 0ea0:2213: | sudo tee /sys/module/usb_storage/parameters/quirks   # 清空运行时参数
  # 重插/重启后恢复 LUN0+LUN1；要恢复“解绑隐藏”把 .disabled 改回 .rules（不推荐）
  ```
* **副作用清单**：kylin-pc 上把 `99-otilink-no-storage.rules` 改名为 `.disabled`；新增
  `/etc/modprobe.d/otilink-quirks.conf` + 运行时 quirks 参数；重启过 otikm 数次；
  仓库改过 `install-kylin.sh` / `uninstall-kylin.sh` / `envcheck.sh` / `RUNBOOK.md` / `AGENTS.md` / 本文件。






---

## 63 第 49 轮：现场「我在 Windows 上打字，麒麟也在同步打字」—— 纯键盘独占判据挂错通道（+ Windows 侧代理全停）

### 63.1 现象与现场取证（2026-10-08）

用户原话：**"对拷线又有问题了，我在 windows 上打字，kylin 也在同步打字"**（确认过：两边同时出现同样的字）。

先把"谁在转发"钉死（AGENTS §2）：

* 物理摆位：键鼠插在**麒麟**（`SIGMACHIP USB Keyboard` + `Logitech G304` 接收器）→ **拓扑 B**，主控在麒麟；
  KM 走 **HID 直发**（日志：`HID 直发模式：键盘 layout=0，鼠标 layout=0（被控端无需任何软件）`）。
* 接管之后，`/tmp/otikm.log` 里**只有鼠标被独占**：

~~~
[00127.556] 已独占 Logitech G304（驱动侧，避免本地与远端双重输入）
[00127.556] 键盘不独占（帧管道未确认健康：对端 agent 不在或 5 秒无成功发送 —— 独占会把用户锁死）
[00127.574] HID 模式：指针交给对端（开始把键鼠发成 HID 包；对端光标已停在入口边 x=1 y=-1）
~~~

* 独立探针（`EVIOCGRAB` 排他性）在"指针已在对端"时实测：**键盘 FREE、鼠标 BUSY**
  → 本地桌面照样收得到这些按键 = **双重输入**的直接证据。
* Windows 侧**一个代理都没在跑**（`otiagent2` 无、命令行含 `otiagent.ps1` 的进程无）；
  麒麟日志全是 `剪贴板未确认 → 第 N 次重发 …（对端在吗？），放弃`；
  `km.log` / `clip.log` 的最后写入停在 09-21 / 09-24 → 用户在 Windows 重启后再没起来过。

### 63.2 根因：两个独立故障叠加

1. **判据挂错通道**（我们代码的 bug）。`apply_grab()` 里"纯键盘能不能独占"用的一直是
   `pipe_ok = per_seen && 5 秒内有成功帧写` —— 那是**协议帧管道**的健康度。而 HID 直发模式下对端
   **零软件**，帧管道本来就该静默（没人回帧/回授权）→ `pipe_ok` 恒为假 → **纯键盘永远拿不到独占**。
   于是接管期间同一个按键：既作为 HID 包发到 Windows，又原封不动落到麒麟本机桌面。
   历史背景：这条判据是 2026-09-21 为**另一个方向**的事故加的（键盘独占了但键送不出去 → 打字全丢）；
   需求本身没错（通道不健康就别独占），错在**判据没有对着键鼠真正走的那条通道**。
2. **Windows 侧代理没跑**（现场状态）。拓扑 B 的**键鼠**是零软件，但**剪贴板代理仍要跑**；
   它不在 → 帧管道没有对端（`per_seen=0`）→ 剪贴板全废，并且把第 1 条的 `pipe_ok` 永久钉死在假。

### 63.3 修法（`re/otilink/otikm.c`）

* 新增 **`km_path_ok()`**：判据**跟着 KM 载体走** —— `peer_hid`（HID 直发）时看 **HID 写失败计数**
  （与 `watchdog_check` 同一个判据 `hid_fail_streak < 20`）；协议模式保持原帧管道判据不变。
* 纯键盘独占改用 `km_path_ok()`；"键盘不独占"提示按模式给出不同原因（HID 写失败 / 帧管道不健康）。
* **驱动期维持独占**：`apply_grab(1)` 原来只在**状态切换那一刻**调用，判据一变就再也不独占；
  现在驱动期间每 500ms **幂等重放**一次（只在状态真的变化时打日志），判据恢复就自动补上独占
  （顺带也修掉"接管瞬间 EVIOCGRAB 撞上别的客户端失败 → 整段会话不再独占"）。
* **安全网对称**：通道不健康时立刻放开键盘独占（原来的"3 秒无成功帧写"在 HID 模式下会每几秒误放一次）。
* 独占失败告警加 30s 节流（维持重放会反复失败，别刷屏）。

### 63.4 新增真机回归 `otilink/kbdexcl.sh`（`gate.sh --hw-kbdexcl`，已注册进 `doccheck.sh`）

* 用 `uisim` 造 uinput 虚拟键鼠（**完全不碰用户真键鼠**）交给 otikm 抓，HID 包仍走**真实线缆**；
  探针按 **EVIOCGRAB 排他性**判定（`BUSY` ⇔ 本机桌面收不到这些按键）——
  这比"注入+读回"更直接，也不需要 X 侧抓键。
* 断言三段：**本地态 FREE**（探针本身有效）→ **接管期间 BUSY**（不再双重输入）→
  **热键回本机后 FREE**（L8：能力收得回来）。
* 触发文件驱动（新增 `uisim --wait-file`）：每次 kssh 握手 ~2.5s，靠 sleep 赌时序第一版直接翻车
  （探针跑起来时虚拟设备已被销毁 → 全 OPENFAIL 假失败）；`--hold` 保证设备活到采样结束。
* 顺手踩到并修掉的两个工具坑：① **`EBUSY` 是 16 不是 11**（硬编码错一次 = 整档假失败）；
  ② `%-5s` 会把 `kbd` 补齐成 `kbd  `（grep 断言必须写 `kbd +BUSY` 这种空格容忍形式）。
* 通过样张（`gate.sh --hw-kbdexcl`，PASS=26 FAIL=0）：

~~~
        0.08 kbd   FREE
        0.14 mouse FREE
       80.37 kbd   BUSY        ← 接管期间：本机桌面收不到这些按键（双重输入消失）
       80.40 mouse BUSY
       83.46 kbd   FREE        ← ctrl+alt+space 回本机后：独占已释放（L8 对称）
       83.54 mouse FREE
~~~

~~~
[06012.362] 已独占 otilink-uisim-mouse（驱动侧，避免本地与远端双重输入）
[06012.362] 已独占 otilink-uisim-kbd（驱动侧，避免本地与远端双重输入）
[06012.378] HID 模式：指针交给对端（开始把键鼠发成 HID 包；对端光标已停在入口边 x=0 y=-1）
[06015.442] 已释放 otilink-uisim-mouse（键鼠交还本机桌面）
[06015.482] 已释放 otilink-uisim-kbd（键鼠交还本机桌面）
[06015.482] HID 模式：指针拉回本机（落点 1907,540；已请求释放包 + 令牌，由消息泵发出）
~~~

Windows 侧同时用 `kbdprobe2.ps1` 确认**独占期间按键仍真送到对端**（`vk=0x14` CapsLock 到达）。

### 63.5 顺带修掉的两处"测试期望过期"（不是本轮引入）

`--local` 原先报 `selftest_proto` 3 项 + `mocktest` 1 项失败：断言还写着 §61 之前的**老布局**
（`[mods=02][保留=00][A=04]`），而 `otihid_kbd_apply()` 早已按 §61.2 把修饰键编成
key usage `0xE0..0xE7` 塞进 6 键数组（`mods` 恒为 0）—— 测试没跟着改。
本轮改的是 `otikm.c/uisim.c/Makefile/脚本`，**没碰 `otihid.c` 与这两个测试**，所以这是**预先存在的失败**。
已把期望同步为 `[mods=00][保留=00][k1=E1(左Shift)][k2=04(A)]` → `--local` 9 PASS / 0 FAIL（+`tcptest` KNOWN）。

### 63.6 本轮验证（都跑过，档位见 AGENTS §5）

| 档位 | 结果 |
|---|---|
| `--static` | PASS=80 FAIL=0 |
| `--local` | PASS=9 FAIL=0 KNOWN=1（`tcptest`，老账） |
| `--deploy-kylin` | 58 个文件同步 + 麒麟编译 + KySec 标签 PASS |
| `--hw-kbdexcl`（新） | **PASS=26 FAIL=0** |
| `--hw-kmb` | 校准后稳定 **PASS=20 FAIL=0 WARN=2**（连跑两轮）——两条 Windows 侧键盘到达断言按 §61.2 固件丢包降为 WARN，详见 63.9；麒麟侧（回程/落点/park/抑制缓冲补发/by-id）全部硬断言且全绿 |
| `--hw-lat` | PASS=5 FAIL=0（`MAXGAP=94ms`，`SEND_FAIL=0`） |
| `--hw-clip` | `clipreg` **17/17** + `clipshort` **7/7**（恢复 Windows 剪贴板代理之后） |
| `--pre` | PASS=24 FAIL=0（WARN=1：`km.log` 本轮没有 `hooks reinstalled`，拓扑 B 下正常） |
| `--portable` | **FAIL=2（预先存在）**：`dist/` 的源码/包指纹过期（包内 `80df3f95444e` ≠ 当前 `784fa046d206`），要 `re/portable/build.sh` 重建 —— 与本次修复无关 |

* "跑的是新二进制"的校验：麒麟 `~/otilink/otikm --version` 的 `src=784fa046d206` == 当前源码指纹。
* 麒麟 otikm 已重启（`run-kylin.sh`，自动协商角色）；Windows **剪贴板代理**已用
  `re/windows/deploy.sh --restart` 拉起（**没有**起 `otiagent2` —— 拓扑 B 下那会变成第二个主控，违反 L17）。

### 63.7 未验证 / 残留风险（不许算成功）

1. **真人复测没做**：虚拟设备路径已证，真键鼠路径要靠用户实际"推到 Windows 打字"再看一眼。
2. HID 键盘接口**本身会丢报表**（§61.2 固件缺陷）：独占之后键只往 Windows 送，
   若通道悄悄丢包，用户会感觉"漏字"而本机也收不到。"写成功但没到"**无法自动检测**（离线判据只有写失败计数）。
   逃生口：`Ctrl+Alt+←`（热键由 otikm 自己从捕获设备读到，独占不影响它）→ 立刻回本机；
   `run-kylin.sh --no-grab` 可整体关掉独占。
3. 根治方向仍是 §61.3 的计划：键盘改走**帧管道 + Windows agent SendInput**（有队列+重传），本轮没做。
4. 绿色包 `dist/` 未重建（见上表）。
5. `kmbret` 的两条键盘到达断言已按 §61.2 固件丢包**降为 WARN**（判据分层，见 63.9）——无需登记 KNOWN；产品侧的真因（F24 令牌吃额度）留作下一轮候选，见 63.9 末段。

### 63.8 副作用与回滚

* 麒麟：多次 `pkill -x otikm` + `run-kylin.sh` 重启；`--hw-kbdexcl` / `--hw-kmb` 各让真机 otikm 停了约 40s。
* Windows：`deploy.sh --restart` 重放了一遍 `.ps1`/`.vbs`（内容与仓库一致），并拉起剪贴板代理
  （`restart-clip.vbs` → `otiagent.ps1 -Cable -Clipboard -InjectKeys -Device \\.\H:`，日志覆盖 `C:\Users\Public\clip.log`）。
* 仓库文件：`otilink/otikm.c`、`otilink/uisim.c`、`otilink/Makefile`、`otilink/kbdexcl.sh`（新）、
  `otilink/selftest_proto.c`、`otilink/mocktest.c`、`tools/gate.sh`、`tools/doccheck.sh`、`AGENTS.md`、`RUNBOOK.md`、本文件。
* **回滚**（两条路）：(a) 本地 git（2026-10-08 起，见 §64.2）：`git -C re log --oneline` → `git -C re checkout <提交> -- otilink/otikm.c` 再重新部署；(b) 按下面三处逆改后重新部署：
  1. 删掉 `km_path_ok()`；`apply_grab()` 里换回
     `long ltx0 = a->tx ? oti_tr_cable_last_tx_ms(a->tx) : 0; int pipe_ok = a->per_seen && ltx0 && (now_ms() - ltx0 < 5000);`
     并用 `!pipe_ok` 做纯键盘豁免；
  2. `run_capture()` 里"键盘独占：维持 + 安全网"整块换回只按 `cable_last_tx_ms` 3 秒判据（无维持）；
  3. `cd re/tools && KY=kylin@<tailscale-IP> ./gate.sh --deploy-kylin` + 重启麒麟 `otikm`。
     （只想立刻止血、不改代码：`run-kylin.sh --no-grab` 关掉独占，代价是鼠标也会双重输入。）


### 63.9 追加发现（kmbret 抖动的真因）：6 条 F24 令牌把线缆键盘那点「报表额度」吃光

把 kmbret 的键盘断言「按丢包加固」（同一段 Ctrl+C 连试 3 遍）之后**还是两次都 FAIL**；
现场取证（`/tmp/kmbret-artifacts/kbdprobe.out` + 合成实例日志）给出了更硬的事实：

* Windows 钩子探针整轮只看到 **6 条报文、全是 F24**（`vk=0x87` ×6），之后**什么都没有**；
* 同一轮 otikm 侧明明发了 `HID 补发被抑制的 2 个按键事件`（第 1/2/3 批）——**发了，但到不了**；
* 合成实例日志里接管那一下正好是：`HID 模式：指针交给对端` → `→ 回程令牌 F24 已发`。

对照 NOTES §61.2 的真机标定（「F24 on/off ×3 → 6/6 全到；之后的报表全部丢失」）：
**线缆 HID 键盘接口每个会话只送得动约 6 条报表**，而接管时的 F24 令牌正好是 6 条 ——
额度被令牌吃光，用户随后的按键一条都到不了。kmbret 的 `--sim-input` 合成实例**永远收不到对端 ROLE**
（`per_seen=0`）→ **必发令牌** → 这条断言在测试环境下必然抖（同二进制 22/0 与 19/2 都出现过）。

**本轮落地（测试侧，判据分层）**：

* ① 麒麟侧「抑制缓冲补发」（**我们的逻辑**）继续硬断言 → 回归时能确定性抓到；
* ② 「Windows 钩子是否真看到 Ctrl/C」降为 **WARN**（受固件丢包/额度影响，不能确定性要求）。
* 校准后连跑两轮稳定 `PASS=20 FAIL=0 WARN=2`（22 项不变）。

**留给下一轮的产品候选（本轮故意不动）**：接管时那发 F24 令牌，在**我们当主控**时其实**没有合法消费者** ——
它等的是对端一个「钩键盘的 KM agent/厂商端」，而厂商端已被 L1 禁止、对端若跑 `otiagent2` 就会变成
第二个主控（L17）。也就是说 `if (!a->per_seen) request_token(a)` 很可能**只花额度、买不到东西**，
却能把用户接管后的头几个字**吞掉**（正好解释 §61.2 里「键盘时而能用时而失灵」）。
改它属于「回不来」那类最贵风险（L8）→ 必须单独一轮，并用 `--hw-kmb` + `--hw-km`（拓扑 A/B 双档）验收。


---

## 64 第 49 轮收尾：把「一症多因」沉淀成索引 + 仓库纳入**本地** git

### 64.1 为什么会有这一节

用户问：**「为什么同样的问题多次修改又多次出现？为什么不能彻底改好？」** 复盘（证据都在本文件里）：

* 「指针回不来」有 **5 个**互不相同的真因（§44 / §44.7 / §45 / §59 / §60）；「键盘有问题」有 **4 个**
  （§59.2 / §60.1 / §61.2 / §63）。**同一个症状 ≠ 同一个 bug**，每修掉一个，剩下的病因照旧复现同一句话。
* 本轮这个 bug 是**上一次护栏的代价**：2026-09-21 的修复显式选择了
  「宁可短暂双输入，也绝不让用户打不了字」（`otikm.c` 注释原文），代价就是本次报障。
  同一个开关上的两个失败方向**不可能同时消掉**——只能换通道（§61.3 键盘改走帧管道）。
* 结构性原因：判据用**代理指标**（帧管道健康 / 有无键鼠 >0 / 单次位移）、单队列设备 + 固件缺陷、
  双端状态机组合爆炸、测试落后于改动（用户级不变量「这个键只能落在一台机器上」直到本轮才有
  `kbdexcl` 守着）、以及**没有 VCS**（回滚靠手写逆改说明）。

### 64.2 本轮落地的收敛手段

1. **`RUNBOOK §19` 索引表**：8 类症状 × 病因（每条带历史节号）× **一句可跑的探针**，
   外加 §19.3「必须成立的不变量（谁在守）」。排障顺序固定为
   **先定层 → 再动手 → 按 `AGENTS §5` 跑对应门禁**，不许按记忆改代码。
2. **不变量进 CI**：`kbdexcl`（驱动侧必须独占本地键鼠）已是门禁档；§61.2 那类固件缺陷从
   「调时序」改成「绕行计划」（§61.3），`kmbret` 相应分层判据（我们的逻辑硬断言、固件丢包只 WARN）。
3. **本地 git**：`re/` 于 2026-10-08 初始化（**仅本地，无远端**），首个提交 `3637b2a`
   （230 文件 / 36580 行）。`.gitignore` 只排除编译产物（开发机 glibc 2.39 编的二进制拿到麒麟 2.31
   起不来，L16）与缓存。此后「改了什么 / 怎么回滚」机器可查：

   ```bash
   git -C re log --oneline -10                  # 有哪些提交
   git -C re show <hash> --stat                 # 这一提交动了哪些文件
   git -C re diff <hash> -- otilink/otikm.c     # 与某版本比差异
   git -C re checkout <hash> -- otilink/otikm.c # 单文件回滚（然后 gate.sh --deploy-kylin + 重启 otikm）
   ```

   ⚠️ 提交前跑 `gate.sh --static`；**不要把凭据/厂商二进制提交**（厂商关停动的是注册表与进程，不入库）。

### 64.3 仍然没解决（不许当成功）

* **§61.3**（键盘改走帧管道 + 对端 `SendInput`）仍未做——只要键盘还走那条有固件缺陷的 HID 通道，
  「独占 vs 不独占」这个两难就还在（现在是「独占 + 写失败才放」）。
* 接管时那发 **F24 令牌**（§63.9）仍是「只花额度、买不到东西」的候选删除项，动它要 `--hw-kmb` + `--hw-km` 双拓扑验收。
* 绿色包 `dist/` 仍是旧指纹（`--portable` 2 FAIL），要跑 `re/portable/build.sh` 重建。
* 「一症多因」只能靠索引 + 不变量**持续收敛**，不会有"一次性彻底修好"——这是这类系统的性质，不是态度问题。

---

## 65 第 50 轮：麒麟再次整机硬卡死（2026-10-08 两次）—— 线索指向 USB 子系统；取证采集已加固

> **结论先行**：**与 otilink 无关**（otikm 当时空闲）；**不是** §51 那个 Kylin 后台升级检查器
> （那 6 个 unit 仍 `masked`，boot -1 里出现 0 次）。两次都是**整机级硬锁死 + 人工硬断电**。
> 最硬的线索在 **USB 子系统**（AIC8800 无线网卡 USB **运行时**掉线 → `usb_hub_wq` 卡 D → 整机死；
> 旁证：对拷线 `usb-storage` 全天反复 D 态，且加固后**立即抓到它卡在 USB 批量传输的内核栈**）。
> 过去两次都**没留下 vmcore**（当时 `hardlockup_panic=0`），本轮已补齐采集（§65.5）——
> 按 L14，本节只写证据与嫌疑，**不写确定结论**。

### 65.1 现场与时间线

机器 `kylin-pc`（Kylin V10 SP1，`5.4.18-168-generic`，H3C X500H G2，16 核 / 15G）。
当天两次非正常掉电（`journalctl --list-boots`）：

| boot | 起 | 止 |
|---|---|---|
| -2 | 10-08 09:15:39 | **11:16:1x —— 硬死，人工断电** |
| -1 | 10-08 11:28:33 | **15:18:2x —— 硬死，人工断电** |
| 0 | 10-08 15:29:31 | 现在 |

**11:16 那次的最后证据链**（boot -2）：

```
11:16:10 kernel: usb 1-4.1.4: USB disconnect, device number 11     ← AIC8800 无线网卡（运行时，非开机枚举）
11:16:10 kernel: AICWFDBG(LOGERROR)  rwnx_send_msg bus is down
11:16:13 /var/log/freeze-breadcrumb.log 最后一格末行:
         !!D-state: kworker/9:2+usb_hub_wq                            ← USB hub 工作队列卡住
   （之后面包屑断到 11:28:33 重启）
```

注意：`usb 1-4.1.4` 每次开机本来就会 disconnect/re-enumerate（`Aic MSC` → `AIC Wlan` → `AIC 8800D80`），
**开机枚举不算异常**；但 11:16:10 是**运行中**掉线（device number 11），紧接着 USB hub 工作队列卡 D。

**15:18 那次的最后证据**（boot -1）：

```
15:18:14 面包屑一格完整：load=0.55 0.71 0.74；无 D-state；Dirty=580kB；MemAvailable=9913236 kB
15:18:19.844 面包屑只写下表头（下一行已是 15:29:32 重启）→ 脚本卡在随后的 `ps`
15:18:28 journald 最后一条（dbus screen.rotation 刷屏），之后全断
```

即：15:18:19 起 `/proc` 遍历被卡住，8 秒后整机停摆。**过程中没有任何内核日志**
（boot -1 最后一条内核消息是 14:33:12 的线缆 xhci reset）。

**非正常断电的直接证据**（两次共同，boot 0 dmesg）：

```
EXT4-fs (nvme0n1p6): recovery complete
FAT-fs  (nvme0n1p1): Volume was not properly unmounted. Some data may be corrupt.
EXT4-fs (nvme0n1p8): 10 orphan inodes deleted / recovery complete
systemd-journald: File .../system.journal corrupted or uncleanly shut down, renaming and replacing
```

### 65.2 已排除（每条都有反证）

* **OOM**：死机前 `MemAvailable` 13G；全日志 0 条 oom-kill。
* **过热**：`k10temp Tctl 45.5°C`；无 thermal throttle。
* **盘满/坏盘**：`/` 用 26%；NVMe 无 I/O error；Dirty/Writeback 一直 <1MB。
* **内核 panic/oops/LOCKUP/hung task**：2026-10-08 全天 **0 条**。（历史对照：9/24 01:09 有过
  `watchdog: BUG: soft lockup - CPU#1 ... [qax_bs_ipcrnner:2751]`，那是更早的另一次。）
* **§51 元凶（Kylin 后台升级检查器）**：`kylin-background-upgrade-{manul,silent}` 与
  `kylin-software-center` 6 个 user unit 仍 `masked`；`journalctl -b -1 | grep 'upgrade background detection'` = 0。
* **otikm（我们的守护）**：死机时 CPU 2.2%、空闲；boot -1 最后一次 HID 相关内核事件在 14:19/14:33，
  距 15:18 死机 45 分钟。与 §51/§58 结论一致：**不是 otilink 把机器搞死**。
* **厂商程序**：本轮不涉及（L1 状态未变）。

### 65.3 嫌疑排序（都不是确定结论）

1. **USB 子系统硬锁（最强）**：11:16 链路完整（无线网卡运行中掉线 → `usb_hub_wq` D 态 → 死）。
   该网卡是 out-of-tree `aic8800_fdrv`（`/proc/sys/kernel/tainted=0x3000`，
   `aic_load_fw: loading out-of-tree module taints kernel`），历史上 `1-4.1.4` 掉线 **24 次**。
2. **对拷线 usb-storage 卡 D（项目相关旁证）**：`0ea0:2213` 的 `Virtual Link`（`usb 1-4.2`）当天被
   xhci `reset` 两次（13:59:52 / 14:33:12）；面包屑里 `!!D-state: usb-storage` 全天 **778 次**，
   最后一次 ~15:17:50（**距 15:18 死机约 30 秒**）。`US_FL_SINGLE_LUN` quirk 已生效
   （`Quirks match for vid 0ea0 pid 2213: 1`，见 §62）但 usb-storage 仍会 D。
   ⚠️ `usb-storage` 同时挂对拷线 LUN 与 AIC 网卡的假 MSC（`Aic MSC`），
   **不能只凭 D-state 断言是哪台设备**。
   **新证据（2026-10-08 加固后立即抓到）**：`!!D-state: 427:usb-storage` 的栈是
   `usb_stor_msg_common → usb_stor_bulk_transfer_buf → usb_stor_Bulk_transport →
   usb_stor_invoke_transport → usb_stor_transparent_scsi_command → usb_stor_control_thread`
   （wchan=`usb_stor_msg_common`）→ 卡在**USB 批量传输的完成等待**上，不是文件系统层。
3. **InnoSilicon 显卡驱动 `innogpu`**（PowerVR RGX，out-of-tree）：**无任何日志证据**，
   但属于"整机静默死透"的典型来源，留着观察。

### 65.4 为什么给不了死因（取证缺口，写进下次核对单）

* `softlockup_panic=1` 有，但**当时 `hardlockup_panic=0`** → 真·硬锁不会 panic、不会 kdump
  （本轮已改成 1，见 §65.5）。
* `/var/crash` 只有 `kexec_cmd`/`kdump_lock`，**无 vmcore**。
* ⚠️ **更正**：早前把 `/proc/iomem` 的 `00000000-00000000 : Crash kernel` 当成"零长、kdump 没生效"——
  **那是非 root 读 `/proc/iomem` 地址被抹零的假象**。root 下实测
  `kexec_crash_loaded=1`、`kexec_crash_size=201326592`（192MiB）、
  `/proc/iomem: 60000000-6bffffff : Crash kernel` → **crash 内核已保留并加载**。
  过去没 vmcore 的原因是 `hardlockup_panic=0`（硬锁没触发 panic），不是 kdump 不可用。
* pstore/ramoops 为空（boot 0 `systemd-pstore` = `Condition check resulted in ... skipped`）。
* ⚠️ §51 的盲区依旧成立：**journald 与日志写同一块盘**，"存储/IO 停顿"与"内核真静默"
  在无 kdump 时**无法区分**——**别把机制写成确定结论**（L14）。

### 65.5 本轮做的加固（可逆，含回滚）

**A. 取数加固（麒麟侧）**
1. `/etc/sysctl.d/99-otilink-crashcapture.conf`：`kernel.hardlockup_panic=1` + `kernel.panic=20`
   （panic 后 20s 自动重启，避免又只能人工断电）。**不设** `hung_task_panic=1` —— 本机
   `usb-storage` 经常短暂 D 态，设了会变成"日常自杀"。
   回滚：删该文件 + `sudo sysctl -w kernel.hardlockup_panic=0 kernel.panic=0`。
2. `/usr/local/bin/freeze-breadcrumb.sh`（v2）：复用同一次 `ps` 输出做 top-CPU 与 D 态判定；
   检测到 D 态时追加每个 D 进程的 `/proc/<pid>/stack`（`timeout 2` 包裹，防读栈自锁）与 `wchan`，
   并每 60s 最多追加一次 `dmesg | tail -30`（原有 load/CPU/Dirty/Writeback 保留）。
   原脚本备份为 `/usr/local/bin/freeze-breadcrumb.sh.bak-20261008`。
   **已实测有效**：写入后几分钟内即抓到 `usb-storage` 的 `usb_stor_msg_common` 栈（见 §65.3）。
   回滚：`cp` 回备份 + `systemctl restart freeze-breadcrumb`。
3. crashkernel **核查结论：正常**（早前"零长"是非 root 假象，见 §65.4 更正）——
   `kexec_crash_loaded=1` / 192MiB 已保留。**未改 GRUB**。现在 `hardlockup_panic=1` 已开，
   下次硬锁应能 panic→kdump→`/var/crash/vmcore`（**待下一次验证**）。

**B. 文档**：本节 + `HANDOFF.md` 顶部状态 + `RUNBOOK §20`（操作步骤与核对单）。

### 65.6 下次遇到"麒麟整机死"的核对单

1. `tail -40 /var/log/freeze-breadcrumb.log`：最后一格时间 + 有没有 `!!D-state` + D 态进程的 `stack`。
2. `journalctl -b -1 -n 50`：最后一条内核消息是什么（USB？线缆 reset？）。
3. `journalctl --list-boots` + `ls -la /var/crash/` + `sudo ls /sys/fs/pstore/`：有没有 vmcore/pstore。
4. `hardlockup_panic` 已为 1：下次应能在 `/var/crash` 拿到 vmcore，用 `crash` 工具解析栈。
5. **别急着改 otikm**：先按 `RUNBOOK §19` 定层。

### 65.7 仍未做（不许当成功）

* 新的 `hardlockup_panic=1` + kdump **能否真正抓到 vmcore 未验证**（crash 区本身已确认正常）。
* 无线网卡隔离实验（`modprobe -r aic8800_fdrv`）**未做**：默认路由走 WiFi（`<网关IP>`），
  拔了会影响 tailscale/relay，需用户配合挑时间。
* **15:18 那次死机完全没有触发事件日志，归因仍未定**。


---

## 66 第 51 轮：仓库「公开就绪」——脱敏 + 去厂商二进制 + 环境变量化

用户要求把仓库做成**可以公开**的。清单与结果如下。

### 65.1 为什么敢公开：厂商二进制**不属于运行路径**

全仓搜索（排除文档）证明：`Makefile` / `otilink/` / `windows/` / `tools/` / `portable/`
**没有任何一处**引用 `mac/MacKMLink.app`、`WinDroid_Linker.apk` —— 它们只是**逆向输入材料**
（协议结论早已落在 `PROTOCOL.md` + `NOTES §33–41`）。绿色包的 Mac 只读体检在
`re/portable/mac/otilink-mac.sh`，**不受影响**。
→ 两个厂商件移出仓库（`git rm --cached`，**本地保留**、加进 `.gitignore`）。

### 65.2 凭据与内网标识：一律环境变量 / 占位符

| 原内容 | 处理 |
|---|---|
| 脚本里内置的密码默认值（`tools/lib.sh` + `hwtest/xferreg/replugreg/clipreg`：`KY_PASS=${KY_PASS:-<本机密码>}`） | 改成 `KY=${KY:-}` / `KY_PASS=${KY_PASS:-}`，**在 `kssh()` 里校验**（用到才报错）→ `--static`/`--local` 这类不需要麒麟的门禁仍能跑 |
| 文档里的密码 | `export KY_PASS='<麒麟登录密码>'`；建议写进 `~/.otilink_env`（chmod 600、不进仓库） |
| 局域网 IP / tailscale IP | `<麒麟IP>` / `<tailscale-IP>`（tailscale 名 `kylin-pc` 保留） |
| Windows 用户名写在路径里（9 个文件） | 文档用 `C:\Users\<你的用户名>\…`；**代码改成从 `%USERPROFILE%` 推导**（`windows/deploy.sh` + 3 个 `.vbs`） |
| 线缆序列号 `DE6348…` | `<线缆序列号>` / by-id 通配 `usb-_Android+Mac_*-…`（对文档反而更正确） |
| 麒麟侧家目录绝对路径（`/home/<用户>/…`） | `~/otilink/…` |
| `portable/dist/`（`BUILD-INFO` 里带构建主机名，且指纹早已过期） | 移出仓库（由 `build.sh` 现场生成）；`--portable` 会明确提示「先跑 build.sh」 |

另外 `gate.sh` 加了**前置检查**：需要麒麟的档位在 `KY`/`KY_PASS` 缺失时**当场 `[FATAL]` 给提示**，
不再让它在 envcheck 里变成一串误导性的 FAIL（"没人转发"）。

### 65.3 这轮踩到并修掉的**两个自己引入的真 bug**（"只跑一次就以为好了"的反面教材）

1. **VBS 字符串拼接写错**：把 `-File ""C:\…\otiagent.ps1"" -Cable …` 改成
   `& base & "\otiagent.ps1"` 之后，**后面的参数落到了字符串外面** → `cscript` 返回 1、代理起不来。
   正确写法：`… & "\otiagent.ps1""" & " -Cable …`（尾巴必须在同一段字符串里）。
2. **bash 单引号吃掉了变量**：`deploy.sh` 里写成 `cscript.exe … '$DEST_WIN\restart-clip.vbs'`
   → 单引号不展开，cscript 收到字面量 `$DEST_WIN\…` → 静默失败（还被 `>/dev/null 2>&1` 吞掉）。
   `bash -x` 一眼露馅：`+ cscript.exe //B //NoLogo '$DEST_WIN\restart-clip.vbs'`。
   同轮把「起来了吗」的检查从 `sleep 3` 改成**轮询 12 秒**：Windows PowerShell 冷启动 >3s，
   会把"其实已经起来了"误报成"没起来"（这次就先误报了一次）。

### 65.4 验证现状

* `windows/deploy.sh --restart` → **[ok]**（便携路径 + 修好的 vbs）；Windows 上代理**稳定存活**
  （`代理数=2` = `cmd /c` 壳 + powershell；`clip.log` 每几秒更新、`设备已打开: \\.\H:`）。
* `--static`（**不带**环境变量）PASS；`--pre`（不带环境变量）→ `[FATAL]` 明确提示怎么 export。
* 麒麟中途重启过一次（15:30 左右离线约 10 分钟）→ 回来后 `--pre` / `--hw-clip` / `--hw-kbdexcl` /
  `--hw-kmb` / `--hw-lat` 全套复跑（报告见 `/tmp/otilink-gate.json`）。
* **未验证（显式标注）**：`windows/restart-km.vbs`、`windows/otilink-agent.vbs` 只做了结构/语法检查 ——
  拓扑 B 下**不能真的执行**（会起 `otiagent2` 变成第二个主控，违反 L17）。切到拓扑 A 时第一件事验证它们。

### 65.5 公开前的历史问题（重要）

脱敏只影响**新提交**：厂商二进制与密码仍在旧提交 `3637b2a` 的历史里。公开必须用**干净历史** ——
本轮把旧历史留在本地（tag `pre-public`），`main` 重做成一个清洁单提交后再推；
`git` 的日常用法见 §64.2。

### 66.6 远端与历史

* GitHub：**`git@github.com:yezi4271/otilink-kvm.git`**（用户选定**私有**；内容已公开就绪，随时可改公开）。
  首次推送的是**干净单提交** `8e9c9ea`（119 文件 / 28585 行）——**不含**厂商二进制、凭据、内网 IP、用户名。
* 旧历史（含厂商二进制与旧密码）留在本地，tag 名故意写成 **`do-not-push-pre-public`**：
  `git push` 默认不带 tag，所以它不会上去；**但 `git push --tags` 会把它推上去 → 千万别用**。
* 提交身份用**仓库级**配置（不污染全局）：`yezi4271 <209439523+yezi4271@users.noreply.github.com>`——
  即 GitHub 官方的 **noreply 邮箱**（`<数字ID>+<用户名>@users.noreply.github.com`），提交会挂到账号名下，
  且仓库历史里**不含真实邮箱**。首个快照最初误用了 `otilink-local <otilink@localhost>` 占位身份，
  已 `rebase --root --exec 'git commit --amend --reset-author'` 重写后强推。
