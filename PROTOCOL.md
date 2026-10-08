# OTi WinDroid Linker / VirtualLink 对拷线协议规格（逆向结果）

设备：`0ea0:2213`，自报 `Android+Mac USB Device`，厂商 OTi / 瀚邦科技（原 Ours Technology）。
本文是**静态逆向 + 部分实测**的结论汇总，标注了每条结论的证据与可信度。
逆向对象：随线虚拟光盘里的 macOS 客户端（未加密），Windows 侧载荷 `GSDD.zz` 为 OTi 自有加密容器，未使用。

| 可信度标记 | 含义 |
|---|---|
| **[A]** | 反汇编直接读出（地址可复核） |
| **[B]** | 由多条 [A] 证据逻辑推出，未直接观测 |
| **[C]** | 假设，**需要用 `probe sweep` 实测确认** |

---

## 1. 设备结构

复合设备，3 个接口：

| 接口 | 类别 | 端点/报文 | 说明 |
|---|---|---|---|
| MI_00 | MSC `08/06/50` | 批量传输 | LUN0 = 只读 CDFS `MacKMLink`(3.84MB，随线软件)；LUN1 = 可写 FAT16 `VirtualLink`(1MB) **[A]** |
| MI_01 | HID `03/01/02` | Input=5B，**Output=0，Feature=0** | 顶层 UsagePage 0x0001 / Usage 0x0002 = 鼠标 **[A]** |
| MI_02 | HID `03/01/01` | Input=9B，**Output=0，Feature=0** | 顶层 0x0001 / 0x0006 = 键盘 **[A]** |

> HID 接口是**纯输入**：芯片向本机注入键鼠。因此"被控端"拿到的是标准 HID，OS 无需驱动。
> 发送方向不在 HID 上（Output/Feature 均为 0），走下面的 SCSI 私有通道。

---

## 2. 传输层：SCSI 私有命令

Mac 端经 `IOCreatePlugInInterfaceForService` + COM vtable 拿 `SCSITask` 接口 **[A]**；
Linux 等价做法是 `/dev/sgN` + `SG_IO`（本仓库 `otilink.c`）。

**CDB 统一为 16 字节，`CDB[14..15] = 'O','T'`（0x4F,0x54）** **[A]**

| opcode | 子命令 | 其余字段 | 方向 | 用途 | 证据 |
|---|---|---|---|---|---|
| `0xD9` | `0x2A` | `[2]=0xFF` | OUT 65536B | **帧管道写**（受发信额度门控，额度 0 时 `ASC=0x81/0x85`） | `PutData` @0x5386 **[A]** |
| `0xD9` | **`0x28`** | `[2]=0x64` | **IN 65536B** | **帧管道读（真正的 64KB 帧）** | `GetData` @0x57bc **[A]** |
| `0xD9` | `0x33`/`0x34`/`0x36` | `[2..13]` = 14B HID 缓冲的前 12B | OUT 无数据 | HID 维护包（type 1/2/3，**非实时键鼠**，真机否证见 NOTES §33.4） | `sendHIDPacket` @0x3306 **[A]** |
| `0xD9` | `0x60` | — | OUT 1B / IN 1B | **设备模式**读/写 | `GetDeviceMode` @0x74a4 / `SetDeviceMode` @0x7410 **[A]** |
| `0xD8` | `0x00` | `[2]=3`，`[3..4]` = 16 位大端"已持有帧数" | **IN 16B** | **消息管道读**（16 字节控制消息，`buf[0]` 为类型并 17 路分发；**不是帧读**） | `ProcessIdleState` @0x4ec0 **[A]** |
| `0xD8` | `0x01` | `[2]`=投递给对端的消息类型，`[3..4]` = 16 位大端值 | **无数据阶段** | 设置/投递远端主机状态（带数据阶段会 `rc=121`） | `SetRemoteHostStatus` @0x52fc **[A]** |
| `0xF0` | `0x00` | `[2]=0x00` | IN ≤64B | **设备信息块**（IC 版本、侧别、功能类型…） | `GetICVersion` @0x4597 / `GetSideType` @0x72f4 / `GetFunctionType` @0x71d8 **[A]** |
| `0xF0` | `0x00` | `[2]=0x02` | IN 16B | 物理总线类型 **与速度状态**（同一条命令两种解读） | `GetPhysicalBusType` @0x449e / `GetSpeedStatus` @0x4de1 **[A]** |
| `0xF0` | `0x05` | `[2]=0x02`，`[4]=0x0A` | NONE | USB 重启（慎用） | `USBRestart` @0x782f **[A]** |
| `0xF0` | `0x30` | — | — | 某状态查询（未细究） | @0x78da **[A]** |
| `0xF0` | `0x31` | `[2]`=锁标志，`[3]`=a2，`[4]`=a3，`[5..8]`=`TickCount()` 4 字节大端 | **OUT 2B** | 独占锁。**本代固件（2009/090301）不支持：`key=0x5 ASC=0x20`** | `LockFunction` @0x46a0 **[A]** |

HID 子命令映射（`sendHIDPacket` 内 `0x00363433 >> ((type-1)*8)`）：type1→`0x33`，type2→`0x34`，type3→`0x36` **[A]**。

**统一的通道模型**（把上表串起来）：

```
0xD9 = 厂商命令 A，CDB[1] = 通道：0x2A 帧管道写 / 0x28 帧管道读 / 0x33,0x34,0x36 HID 维护包 / 0x60 设备模式
       （注意：0xD9 **不代表方向**——0xD9/0x28 是"读"、0xD9/0x2A 是"写"，方向由调用方决定）
0xD8 = 厂商命令 B，CDB[2] = 通道：0x03 **消息管道（16B）** / 0x02 物理总线类型
0xF0 = 控制，CDB[1]/CDB[2] = 子命令与选择子（0x00/0x00 IC 版本、0x00/0x02 总线类型、
       0x05/0x02 USB 重启、0x30、0x31 独占锁）
所有命令 CDB[14..15] = 'O','T'
```

**两条独立的读管道（第 25 轮真机实证，务必区分）**：

```
消息管道  0xD8/0x00/0x03 + IN 16B    ← 控制消息，buf[0] 为类型（见 NOTES §33.2 跳转表）
帧管道    0xD9/0x28/0x64 + IN 65536B ← 真正的 64KB 帧；校验规则见 §9.1
```
把消息管道当帧管道读，会永远只拿到 10~16 个非零字节（设备按 `resid` 只回 16 字节）。

`0xD8` 的 `CDB[3..4]` 语义已解（见 §4）：16 位大端的"本机已持有未消费帧数"。

**设备信息块**：`0xF0/0x00` + 全零 CDB 会返回一个信息块（厂商用 64 字节缓冲）；
IC 版本取前 12 字节，`GetSideType`/`GetFunctionType` 从同一块里取字段（侧别/功能类型），
所以这实际是**一条命令的多种解读**（本仓库 `otilink_info_read()`，12 字节子集见 `otilink_ic_version()`）。

**`0xD9/0x2A` 的 `CDB[2]=0xFF`** 在同一通道模型下是数据管道写的参数（常量，写死为 0xFF）。
`_OTi_SendDummyData()` 的实现是 `SendData(NULL, true)` —— 即**写一个 64KB 全零帧**
（@0x21c8 → @0x309c 的 `bzero` + `AppendBytes(..., 0x10000)` 分支）**[A]**。
全零帧在对端被判为"空闲"，因此这是一个**无害的保活/冲刷**操作（`otilink_send_dummy()`）。

---

## 3. 数据帧格式（65536 字节）**[A]**

```
帧(65536B) = [20B 头][65496B 载荷][20B 头的副本]
                  ↑ offset 20        ↑ offset 0xffec(65516)
                                     其中头内 dword 的副本在 0xfffc(65532)
```

接收侧校验（`GetData` @0x594b–0x59ab，常量取自 `__bss`，值为 **0**）：

```c
if (前16字节 == 0 && dword@0x10 == 0)                        → 空闲帧：丢弃
else if (memcmp(buf, buf+0xffec, 16)==0 &&
         *(u32*)(buf+0x10) == *(u32*)(buf+0xfffc))           → 有效帧：CFDataCreate(buf, 65516) 入 RX 队列
else                                                          → 非法帧：丢弃并记日志
```

发送侧构造（`PutData`）：`CFDataCreateMutable(65536)` → `AppendBytes(payload, 0xffec)` → `AppendBytes(payload, 0x14)`，
即载荷本身以 20 字节头开头，末尾再贴一份头 **[A]**。
`SendData` @0x309c 里出现的 `0xffec`(65516) 与 `0x10000`(65536) 常量与此一致；接收缓冲区与队列上限 `0xc8`(200) **[A]**。

> **关于"芯片是否解析这 20 字节头"**：接收侧用「前 16 字节 == 全零」判空闲、用「首 20 字节 == 尾 20 字节」判有效。
> 若芯片会插入或改写自己的头，这两个判据都不可能成立。由此可判定：**芯片是透明的 64KB 管道，
> 20 字节头完全由主机定义**（空闲帧全零只是"主机没写数据"的自然结果）——这是 **[A] 级推论**。
> 直接后果：Linux↔Linux 场景下**厂商头内语义无关紧要**，我们可以自定义头内字段（见 §6）；
> 残留风险仅剩"芯片是否对未知头内容报错"，`probe sweep/rtest` 一插上就能验掉。

---

## 4. 流控与状态机（部分未解）

`OTiTransporter` 中与本主题相关的方法 **[A]**：

```
ObtainExclusive / ReleaseExclusive / MountMedia / UnmountMedia     ← 传输前独占，避免与本机文件系统争用
ResetRxQueue / ResetTxQueue / ClearRxQueue / ClearTxQueue
GetMaxBookingSize / SetMaxBookingSize      ← booking 窗口
GetContinueTxCount / SetContinueTxCount    ← 续传计数
GetSendBufferCount                          ← 发送缓冲余量
requestResendPacket / RejectRemoteTX        ← 重传与拒收
GetSpeedStatus / GetRemoteHostStatus / SetRemoteHostStatus / RemoteApStatus / RemoteDevStatus
GetOTiSenseKeyResult                        ← 从 SCSI sense 解析设备状态
ProcessIdleState / processTransferState / processReceiveState / processDummyState
```

已知的连接期命令：`OTi_ReadDevicesInfo`、`OTi_GetSideType`、`OTi_GetFunctionType`、`OTi_SetDeviceMode`、
`OTi_GetPHY1Alive`、`OTi_USBRestart`、`OTi_Is5000Device`、`OTi_GetICVersion` **[A]**。

### 4.1 读方向 CDB[3..4] 已解：它是"已持有帧数"的流控反馈 **[A]**

`ProcessIdleState` @0x4e87–0x4ecc 的逻辑（逐条对应反汇编）：

```c
uint16_t held;
if (ContinueTxCount >= 11) { ContinueTxCount = 0; held = 0; }        // 0x4e87-0x4e98
else {
    n = CFArrayGetCount(RX队列);                                     // 0x4ea0 CFArrayGetCount
    held = (n == 0) ? 0 : MIN(n, MaxBookingSize);                     // 0x4eb1 cmovge
}
CDB[2] = 3;
CDB[3] = held >> 8;  CDB[4] = held & 0xFF;                            // 0x4ec9 / 0x4ecc
```

成员与默认值（由访问器与构造函数实测）：

| 偏移 | 名称（访问器） | 默认值 | 证据 |
|---|---|---|---|
| `+0x54` | `GetRegRx/SetRegRx` | 0 | @0x5e00 / @0x5e24 |
| `+0x56` | `GetContinueTxCount/SetContinueTxCount` | **0** | @0x5e2e / @0x5e0a；构造 @0x3712 |
| `+0x58` | `GetMaxBookingSize/SetMaxBookingSize` | **100 (0x64)** | @0x5e38 / @0x5e42；构造 `mov dword [rbx+0x56], 0x640000` @0x3712 |

含义与实现要点：
- 这是**主机上报"我手上还有多少帧没消费"**，供设备做流控；**不是 credit 门闸**。
  取 0 时必须仍能收到第一帧，否则握手死锁（本仓库 `otilink_recv_frame(d, frame, held)` 的 `held` 即此值，默认 0）。
- 设备状态码 `0x11`/`0x20` 会触发通知；收到 `0x20` 时把 `MaxBookingSize` 复位为 100（@0x5187）。
- 仍**未解**的部分：`CDB[2]=0xFF`（写方向）的确切含义、重传（`requestResendPacket`）触发条件、
  设备内部窗口的具体表现。→ 用 `./probe sweep [--write]` 观察（见 §7）。

---

## 4.2 初始化序列（real-device bring-up 的关键）

`Initialize()` @0x3d28–0x3e98 的调用顺序 **[A]**：

```
UnmountMedia      (@0x3ed4)  ← 先把两个 LUN 卸载掉
ObtainExclusive   (@0x3fae)  ← 经 SCSI 0xF0/0x31 取独占
ResetRxQueue/ResetTxQueue (@0x420c / @0x42ac)
启动工作线程       (@0x3dce MPCreateTask)
MountMedia        (@0x415a)
ReleaseExclusive  (@0x43a0)
GetPhysicalBusType(@0x440e) → GetICVersion (@0x4566)
```

**Linux 侧对应做法**（本仓库）：
1. 用 udev 规则抑制自动挂载（`UDISKS_IGNORE=1`，见 `otilink/99-otilink.rules`），
   `otikm --doctor` 会检查 LUN 是否仍挂在 `/proc/mounts` 里并给出 `umount` 命令；
2. `otikm --cable-init` 执行 `0xF0/0x00/0x00`（设备信息块）→ `0xD9/0x60`（读设备模式）→
   `0xF0/0x00/0x02`（总线类型）→ `0xF0/0x31`（独占锁），退出时解锁；
   也可用 `probe info` / `probe mode` 单独验证这些命令是否通。

## 4.3 远端主机状态：读侧不需要额外实现

`GetRemoteHostStatus` @0x5278 用的 CDB 与**数据管道读完全相同**（`D8 00 03 <held>`），
也就是说"远端主机状态"是**随数据帧带回**的（由载荷/头部承载）；
只有写侧是独立命令：`SetRemoteHostStatus` @0x52fc = `D8 01 <v2> 00 <v4>` + 2 字节载荷（**写方向**）。

对实现的含义：读侧**零额外工作**（我们本来就在收帧）；写侧的 2 个状态字节语义未知，
本仓库实现为 `otilink_set_remote_status(d, v2, v4, payload)`，真机可用 `probe rstatus` 试探
（该不该发、发什么值，属于"接上设备就知道"的范畴）。

同理，`GetSpeedStatus` @0x4db0 = `F0 00 02`（与物理总线类型同一条命令），
`ResetRxQueue`/`ResetTxQueue` 是**纯本地状态**（无任何 CDB，故无需实现）**[A]**。

### 4.4 保活

厂商有 `SendDummyData()` = 写一整帧 64KB 全零（**不带头**），对端按"空闲帧"丢弃，
因此它是一条**对应用层透明**的保活/冲刷手段。本仓库：
`otilink_send_dummy()`，或在 daemon 里用 `--keepalive MS` 周期触发
（所有传输额外发协议层 `PING`，线缆模式再加 dummy 帧）。

## 4.5 主循环与轮询节奏（影响延迟与 LUN 选择）

`RunLoopFunction` @0x4350（工作线程主循环）**[A]**：

```c
while (running) {
    if (dev_ready) ProcessIdleState();   // 内部：ReceiveData(0xD8) + GetData + processTransferState→PutData
    else           usleep(100000);       // 100ms
}
```

即**同一条循环既读又写**：`ProcessIdleState` → `processTransferState`(@0x509c) → `PutData`(@0x5d97)，
读侧是 `SendSCSICommandReceiveData`(@0x4f00 附近) + `GetData`(@0x57bc)。

正常空闲路径**不 sleep**（连续轮询）；`usleep(5000)/usleep(10000)` 只出现在错误/重试分支 **[A]**。
本仓库在空闲帧之后退避 **500µs**：USB2 下一次 64KB 传输本身就是 ms 级，对输入延迟影响可忽略，
同时避免把 CPU 空转打满。

**LUN 选择**：复合设备给出两个 LUN（CD-ROM + 磁盘），厂商代码没有明文说明命令发给哪个。
本仓库不猜：`oti_tr_open_cable(NULL)` 会**逐个探测**，选第一个能应答 `0xF0/0x00` 设备信息块的 LUN
（`probe infoall` 可单独查看每个 LUN 的应答情况）；也可用 `--transport cable:/dev/sgN` 手工指定。

## 5. 厂商命令层（XML / UPipe）——仅供理解，**本实现不复刻**

`IUPipeCmd`（`Cmd`/`Body` 字段）序列化成 XML：信封 `<OTIMSG>…</OTIMSG>`，解析用 `SimpleXML`/`NSXMLDocument` **[A]**。

| 命令 | 参数（对应 `Param_*`） |
|---|---|
| `Cmd_Notify_KM_Switch_To_Remote` | `Param_Move_Out_X/_Y/_Direction/_Info`、`Param_Move_Out_KM_Switch_Option`、`Param_Move_Out_Use_Hotkey_Switch_Only` |
| `Cmd_Post_Remote_KM_Setting_Info` | `Param_Remote_Screen_Width/_Height`、`Param_Other_PC_Position_Option`、`Param_KM_Setting_Info` |
| `Cmd_Transfer_Clipboard` | `Param_Clipboard_Info_2` |
| `Cmd_Check_Bridge_Block_Status` / `Cmd_Replay_Bridge_Block_Status` | `Param_Remote_Bridge_Blocked` |
| `NP_Cmd_*` | `Param_Sync_Running_Info`、`Param_Move_File_To_Trash` 等文件操作 |

键鼠数据包形如 `<OTIMSG><%@>%d</%@> ×7</OTIMSG>`（`KMKeyMouse.dylib`）**[A]**；
Windows 侧剪贴板经命名管道 `\\.\pipe\OTI_ClipboardAgent` **[A]**。

**值得注意的遗留线索**：厂商二进制里有 `"Remote is Linux, clipboard upipemsg ignored!!!!"`，
说明其代码里存在识别 Linux 对端的分支——生态里可能有（或曾有）Linux 端实现，值得后续追查 **[A]**。

---

## 6. 本实现自有的应用层协议（`otiproto`）

Linux↔Linux 两端都是我们的代码，因此**不复刻 XML 层**，直接在帧载荷里跑二进制消息：

```
消息 = 20B 头 + 载荷（小端）
  magic u32 'OTL1' | type u16 | flags u16 | seq u32 | len u32 | crc32 u32
  crc32 覆盖 头[0..15] + 载荷
type: KEY(1) MOUSE(2) SWITCH(3) CLIP(4) PING(5) ACK(6)
```

载荷上限 `OTI_PAYLOAD_MAX = 65496 - 20 = 65476`；剪贴板单块 `65000`。
`SWITCH.side` 语义：`REMOTE` = 发送方把指针送出去（接收方 `have_control=1`），
`LOCAL` = 发送方把指针收回（接收方 `have_control=0`）。

---

### 6.1 角色协商 `OTI_MSG_ROLE = 10`（第 46 轮：键鼠插哪边都行）

**为什么加**：原来角色是两侧**各自单方面**判的（麒麟按 by-id 有没有本机键鼠；Windows 开机无条件起
`otiagent2 --edge right`）。用户把键鼠换到另一台后，**两侧同时成为主控**，抢同一条 HID/帧管道 ——
实测帧管道双向全丢、剪贴板两边都"未确认"，只能人工停一侧（现场事故，见 `re/NOTES.md` 第 46 轮）。

载荷 12 字节（小端）：

```
u8  has_local_input   1 = 本机有"线缆之外"的键鼠
u8  want              0=auto 1=force master 2=force slave（用户显式指定）
u8  state             本机当前角色：0=未定 1=master 2=slave
u8  flags             bit0 = 本机正持有 EVIOCGRAB（可观测 + 冲突检测）
u32 boot_id           本机启动标识低 32 位（平票仲裁用；Linux 取 /proc/sys/kernel/random/boot_id 前 8 位 hex）
u32 input_age_ms      本机最近一次真实键鼠事件距今毫秒（越小 = 刚用过这台）
```

**节奏**：状态变化立刻发 + 每 1s 心跳一条（与 HELLO/PING 同管道，量级相同）。

**仲裁规则**（两端同一套，确定性；参考实现 `re/otilink/otikm_core.c: otikm_role_decide()`，`coretest` 覆盖）：
1. 显式 `want` 优先：本机 force 最高；其次"对端 force master → 我 slave"、"对端 force slave → 我 master"。
2. 否则 **有本机键鼠的一方当主控**。
3. 双方都有：比 `input_age_ms`，小者（刚在用）当主控；领先需 ≥ `--role-margin`（默认 2000ms），
   且换边后 ≥ `--role-dwell`（默认 10s）不再换（防抖）。
4. 双方都没有：两边都进被驱动侧，日志明确报错（提示显式 `--role`）。
5. 冲突（对端也自称 master）：`boot_id` 小者胜 —— **只在双方都有本机键鼠、且本机无显式偏好时**才用这条
   （否则会把"对端显式要 master"覆盖掉，实测踩过）。

**安全不变式**：
* **I1**：只有**连续 3 个心跳**收到对端 `state=slave`（或对端 `--role-absent` 默认 5s 内没有任何消息）之后，
  本机才允许 `grab`/注入 —— 这是"任何时刻最多一个 grab"的根；
* **I2**：自己在 master 且收到对端 `state=master` → `boot_id` 小者胜，败者**立刻**释放 grab + 释放所有按键 + 发 F24 令牌；
* **I3**：被驱动侧永不 grab（L7）；
* **I4**：5s 内完全没有对端帧（HELLO/PING/ROLE 都没有）→ 允许单方面 master（保证零软件接收端/Mac 仍可用）。

**兼容**：新增 type=10 → **两端必须同版本**（AGENTS §4.3）。旧对端不认 type 10 时会忽略它（不致命），
新端对旧端退化为 I4 单方面 master。

**门禁**：`coretest`（纯逻辑 11 项）、`selftest_proto`（编解码 6 项）、`--replug`（拔插后角色/抓取仍有效）、
`--hw-km` / `--hw-clip`。

---

## 7. 复核与标定方法

```bash
# 复现静态结论（工具在本仓库 re/ 下）
python macho_disasm.py <OTiTransfer> --disasm __ZN14OTiTransporter13sendHIDPacketEaPK8__CFData
python macho_disasm.py <OTiTransfer> --disasm __ZN14OTiTransporter7GetDataERt
python objc_imp.py <KMKeyMouse.dylib> --list sendKeyData: sendMouseData:
python find_calls.py <GoBridgeDemon> --symbol _OTi_SendHIDPacket --ctx 30

# 设备接进 Linux 后标定 SCSI 参数
./probe list                    # 找 /dev/sgN（按 VID:PID）
./probe sweep --dry-run         # 先看要打哪些 CDB（不碰设备）
./probe sweep --delay 200       # 读方向扫描：CDB[2]/[3]/[4] 各扫一遍
./probe sweep --write --delay 300   # 写方向（写的是合法帧，含首尾头）
./probe query 0                 # 0xF0/0x00 状态查询
./probe lock 1                  # 独占锁
```

判定：`rc=0 且 status=0x00` 且 `xfer=65536` 且帧判定为"有效/空闲"的那组参数就是正确编码；
若只有基准组满足，说明 `CDB[2]=3`(读)/`0xFF`(写) 已是全部信息，`CDB[3]/[4]` 只是可选的窗口值。

---

### 7.1 采样法：拿厂商软件当对端样本

若能接受在一台 Windows 机器上跑厂商软件，可以把**厂商实际发出的帧**采下来核对 §3/§4：

```bash
# Windows 侧: 运行随线软件（虚拟光盘里的 SKLoader.exe），另一端插到 Linux
./probe rdump /tmp/vendor.bin 5 --all     # 逐字节存盘（含空闲帧），并打印每帧前 32 字节
```

这能一次确认：20 字节头的真实内容、空闲帧是否全零、以及设备在厂商软件驱动下的 credit 行为。
若厂商软件要求对端也必须是 Windows，则退化为在 Linux 侧先观察空闲帧与 `0xF0/0x00` 状态返回。

### 7.2 软件仿真设备（`otimock`）

`otilink` 的 SCSI 边界做了后端注入（`otilink_set_ops()`），因此可以用**软件仿真设备**在没有硬件时
验证整条电缆路径：`otimock.c` 按本文语义实现 0xD9/0xD8/0xF0/HID 维护包、64KB 环形接收队列、
空闲帧（全零）、首尾头校验与 credit 规则；`mocktest` 覆盖 19 项断言（含"credit=0 不返回数据也不消费队列"）。
这既是对本文结论的可执行化，也让真机排障时能把"帧/协议层"与"设备层"分开定位。

## 8. 未解问题清单

1. `0xD8` 的 `CDB[3]/CDB[4]`、`0xD9/0x2A` 的 `CDB[2]` 精确语义与 credit 窗口（→ `probe sweep`）；
2. 重传机制（`requestResendPacket`）的触发与线上表现；
3. 厂商 20 字节头内部字段（对芯片是否透明需实测）；
4. `GetSideType`/`GetFunctionType`（两端是否区分 A/B 侧，是否影响 HID 注入方向）；
5. HID 维护包（`0xD9/0x33`+全零）是否为对端 HID 模拟所必需、周期多长；
6. 厂商二进制里的 Linux 分支指向哪个实现。
