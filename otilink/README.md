# OTiLink —— 对拷线键鼠共享（Linux 主控端 ↔ Windows 被控端）

把一根 **OTi/瀚邦 USB2.0 对拷线**（`0ea0:2213`，"Virtual Link"）变成一台真正的 KVM：
一套键鼠、一份剪贴板，在两台机器之间无缝切换 —— **不需要网络，不需要在 Windows 上装驱动**。

> 逆向过程与协议细节见 `../NOTES.md` / `../PROTOCOL.md`；运维速查见 `../RUNBOOK.md`。

---

## 它有什么

| 能力 | 说明 |
|---|---|
| **Easy Mouse 撞边切换** | 指针推到屏幕边缘就把控制权交给对端；从对端边缘推回来就收回 |
| **Ctrl+Alt 热键** | `ctrl+alt+space` 切换 / `ctrl+alt+left` 拉回 / `ctrl+alt+right` 交给对端 / `ctrl+alt+l` 锁定本机 |
| **热键抑制** | 组合键不会泄漏到对端 —— 不会在对端留下"卡住的 Ctrl/Alt" |
| **角部防误切** | 指针贴角 40px 内不切换（对标 MWB 的 *Block mouse at screen corners*） |
| **修饰键防误触** | 可选"必须按住 Shift/Ctrl 才撞边切换"（对标 MWB *Easy Mouse* 选项） |
| **剪贴板双向同步** | UTF-8 文本，自动分块（实测 140KB 跨块重组一致），带回声抑制 |
| **鼠标手感可切换** | `native`（走 Windows 自己的指针速度/加速，默认）｜ `absolute`（1:1 跟手，覆盖整个虚拟桌面） |
| **永远不会被卡住** | 看门狗（对端 3 秒不消费即自动交还）+ 信号处理 + 热键兜底 |
| **两端开机自启** | 装好之后不用管；Windows 侧有**托盘图标**（状态 / 暂停 / 退出） |
| **配置文件** | `~/.config/otilink/kvm.conf`，所有策略都在里面，命令行可覆盖 |

对标参考：[PowerToys Mouse Without Borders](https://learn.microsoft.com/en-us/windows/powertoys/mouse-without-borders)
（Easy Mouse / 角部防误切 / Ctrl+Alt 热键 / 托盘 / 状态可见性）与
[Barrier / Input Leap](https://github.com/input-leap/input-leap)（配置驱动 / 热键抑制）。

## 它怎么工作（一句话）

对拷线对外是两块 USB 复合设备；真正传数据的是**厂商私有的 SCSI 命令**。
逆向结论是它有**两条独立管道**（详见 `../PROTOCOL.md`）：

```
消息管道  0xD8/0x00/0x03 + IN 16B       ← 16 字节控制消息（buf[0] 是类型）
帧管道    0xD9/0x28/0x64 + IN 65536B    ← 真正的 64KB 数据帧（读）
          0xD9/0x2A/0xFF + OUT 65536B   ← 同上（写，需要收到 0x06/0x07 授权后立刻写）
```

键鼠事件与剪贴板都封进我们自己定义的紧凑协议（`otiproto`：20 字节头 + CRC32 + 载荷）跑在帧管道里。
**Windows 侧需要运行 `otiagent.ps1`**（一个 PowerShell 脚本，普通权限即可，不需要驱动）。

---

## 安装

> **换机器 / 换发行版 / 不想装东西？用免安装绿色包：** `re/portable/`（`./build.sh` 组装 → 拷到 U盘 →
> `sudo ./otilink.sh` 一次提权即用；Windows 侧 `otilink.cmd` 双击即用）。下面这套是"装一次、以后免提权"的安装路径，
> 两者并存、互不影响。绿色包已真机验收：麒麟上 clipreg 17/17、xferreg 12/12、hwtest 12/12。

### Linux（主控端，麒麟 / Ubuntu / Debian 系）

```sh
sudo sh install-kylin.sh          # udev 规则 + 用户组 + uinput + 默认配置
# 注销并重新登录（组变更只在登录时生效）
./otikm --doctor                  # 自检：应显示"环境就绪"
./run-kylin.sh                    # 启动
```

`install-kylin.sh` 会把 udev 规则装到 `/etc/udev/rules.d/`、把当前用户加入 `input,disk` 组、
加载 `uinput`，并把默认配置写到 `~/.config/otilink/kvm.conf`。

### Windows（被控端）

```powershell
powershell -ExecutionPolicy Bypass -File ..\windows\install-windows.ps1 -Device '\\.\H:'
```

会复制到 `%USERPROFILE%\otilink\`、在"启动"文件夹放一个隐藏启动器、并立即启动一次。
之后每次登录自动运行，托盘会出现一个键盘图标。

> `-Device` 是对拷线被控端的卷（插上后通常是 `H:` 或 `F:`，可用 `-Scan` 探测）。

### 卸载

```sh
sudo sh uninstall-kylin.sh            # Linux
```
```powershell
powershell -ExecutionPolicy Bypass -File ..\windows\uninstall-windows.ps1   # Windows
```

---

## 怎么用

1. 指针推到 Linux 屏幕的**左/右边缘** → 控制权交给 Windows（Linux 指针停在边缘）
2. 在 Windows 上像本地一样用键鼠；**剪贴板自动双向同步**
3. `ctrl+alt+left`（或把 Windows 指针推到对侧边缘）→ 控制权收回 Linux
4. 临时不想被撞边打扰：`ctrl+alt+l` 锁定到本机，再按一次解锁

**万一觉得失控了**（按优先级）：

1. `ctrl+alt+left` / `ctrl+alt+space`
2. Linux 终端里 `pkill -x otikm`（独占随进程退出立刻释放）
3. 什么都不做也行 —— 看门狗 3 秒自动交还

---

## 配置

`~/.config/otilink/kvm.conf`（模板见 `kvm.conf`）：

```ini
[general]
screen = 1920x1080
edge = 4                     # 触边阈值像素
edges = left,right           # 允许撞哪些边：left,right,top,bottom | all | none
switch_modifier =            # 撞边需按住的修饰键（防误触）：空/shift/ctrl/alt/meta
corner = 40                  # 角部死区像素（防误切）；0=关
grab = true                  # 驱动侧独占本机键鼠，避免双重输入
clipboard = true
idle_release = 3000          # 看门狗毫秒数；0=关（不推荐）

[hotkeys]
hotkey_toggle    = ctrl+alt+space
hotkey_to_remote = ctrl+alt+right
hotkey_to_local  = ctrl+alt+left
hotkey_lock      = ctrl+alt+l
```

命令行参数优先于配置文件（`otikm --help` 看全部选项，例如 `--edges none`、`--switch-mod shift`、
`--hotkey-toggle f12`、`--no-config`）。

### Windows 被控端选项

| 参数 | 说明 |
|---|---|
| `-Device '\\.\H:'` | 对拷线被控端的卷（必填） |
| `-MouseMode native` | **默认**：相对位移，走 Windows 自己的指针速度/加速 |
| `-MouseMode absolute` | 绝对坐标，1:1 跟手，覆盖整个虚拟桌面（用 `MOUSEEVENTF_VIRTUALDESK`） |
| `-Inject` / `-Clipboard` | 注入键鼠 / 同步剪贴板（都用就都写上） |
| `-NoTray` | 关掉托盘图标（无桌面会话时用） |

---

## 排障

| 现象 | 处理 |
|---|---|
| `--doctor` 报输入设备 / uinput 权限 | 没重登录，或 udev 规则没装 → 重跑 `install-kylin.sh` 并注销重登 |
| 撞边不切换 | 看配置里的 `edges` / `switch_modifier` / `corner`；或已锁定（`ctrl+alt+l` 解锁） |
| 指针手感不对 | native 模式下**调 Windows 的指针速度**；或换 `-MouseMode absolute` |
| 对端无响应、控制权自动回到本机 | 看门狗生效了：检查 Windows 侧 agent 是否在运行（托盘图标） |
| 想完全停掉 | Linux：`pkill -x otikm`；Windows：托盘右键退出 |
| 日志 | Linux：`tail -f /tmp/otikm.log`；Windows：前台跑一次 agent 看输出 |

---

## 目录内容

| 文件 | 作用 |
|---|---|
| `otikm.c` / `otikm_core.c` | 主控端守护进程 / 切换状态机（热键、边缘策略、抑制缓冲） |
| `otilink.c` / `otitrans.c` | 设备层（SG_IO + 私有命令）/ 传输层（授权门控 + 收发队列） |
| `otiproto.c` / `otihid.c` | 自有消息协议 / HID 报告构造 |
| `otiinput.c` / `oticlip.c` | evdev 抓取与 uinput 注入 / 剪贴板后端（wayland/x11/file） |
| `cabletest.c` / `uisim.c` / `otiprobe.c` | 真机回归测试 / uinput 虚拟键鼠 / 单文件探针 |
| `99-otilink.rules`, `kvm.conf`, `run-kylin.sh`, `install-*.sh` | 部署件 |
| `../windows/otiagent.ps1` | Windows 被控端（托盘 + 注入 + 剪贴板 + 断线重连） |

## 构建与测试

```sh
make            # probe / otikm / cabletest / mocktest / selftest_proto / e2e_test
make test       # 全量：协议单测 + 状态机 + 仿真设备端到端 + 双实例集成
```

状态机与协议的单测覆盖：组合键热键解析与触发、**修饰键抑制不泄漏**、撞边策略
（edges / switch_modifier / 角部死区 / 锁定）、剪贴板分块重组、CRC 校验等。
