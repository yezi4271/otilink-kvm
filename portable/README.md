# 对拷线「免安装绿色包」

> 目标：**把 OTi/瀚邦 USB2.0 对拷线（$0ea0:2213）插到任何两台电脑上，拿来就能用。**
> 不装驱动、不编译、不加开机自启、不改 /etc、不进注册表；Linux 侧只在需要设备权限时**提一次权**。
>
> 这个目录负责"打包 + 开箱即用"，底层实现仍然是 `re/otilink/`（Linux）与 `re/windows/`（Windows）。
> 硬约束、门禁矩阵、验证规范见 `re/AGENTS.md`；运维排障见 `re/RUNBOOK.md`。

---

## 1. 30 秒上手

### 组装（开发机，一次性）

```bash
cd re/portable
./build.sh --baseline kylin     # 在麒麟上编 Linux 二进制（声明基线 glibc 2.31）+ 编 Windows 载荷 + 打包
./portablecheck.sh              # 门禁：新鲜度/基线/依赖/清单/入口脚本
# 产物：dist/otilink-portable-<ver>/（目录） + .tar.gz + .zip
```

把 `dist/otilink-portable-<ver>/` 整个目录（或 .zip/.tar.gz）拷到 U盘 —— **这就是"免安装"的载体**。

### Linux 侧（主控端或接收端）

```bash
cd /media/$USER/<U盘>/otilink-portable-0.1
./otilink.sh --doctor      # 先体检（不需要 root，告诉你缺什么）
sudo ./otilink.sh          # 启动（脚本自己 sudo/pkexec；会问一次密码）
```

* 第一次会问"键盘鼠标插在哪一侧"，答案记在 `$XDG_RUNTIME_DIR/otilink-role`（登出即清）。
* 日志：`/tmp/otilink-<uid>.log`。停止：Ctrl+C 或 `pkill -x otikm`。
* 想显式指定：`./otilink.sh --role slave --peer-side left`（对端屏幕在本机左边）。

### Windows 侧

双击 `otilink.cmd`（会问一次角色），或：

```bat
otilink.cmd master --edge right   ；键鼠在本机：起键鼠代理 + 剪贴板代理
otilink.cmd slave                 ；键鼠在对端：只起剪贴板代理（键鼠零软件）
otilink.cmd --stop                ；停掉本包拉起的代理
```

不需要管理员（打开对拷线的卷 `$"$H:` 本身就是普通权限）。日志在同一个目录：`km.log` / `clip.log`。

### macOS 侧（只作为接收端）

```bash
mac/otilink-mac.sh      # 只读体检：线缆是否枚举、HID 是否在位、厂商程序是否在抢
```

**Mac 作为接收端不需要装任何东西**（线缆对接收端就是真实 USB 鼠标 + 键盘）。
Mac 作为**主控端本轮不支持**（见 §6）。

---

## 2. 「免安装」到底免到什么程度（边界说清楚）

| 项 | 免安装包怎么做 | 为什么不能更省 |
|---|---|---|
| 编译 | **不需要**：包里是编好的二进制 | — |
| udev 规则 / 用户组 | **不需要**：每次运行提一次权 | `/dev/sg*`、`/dev/input/event*`、`/dev/uinput` 只对 root 或 udev 授权的组可读写 |
| 配置文件 | **不需要**：内置默认值，参数都有缺省 | — |
| 开机自启 | **没有**（这是"免安装"的代价） | 自启 = 装 systemd 服务/启动项 = 安装行为 |
| 网络 | **不需要** | 数据全走线缆 |
| 写系统文件 | **不写**：只有 `/tmp`、`$XDG_RUNTIME_DIR` | 用户目录之外一律不动 |
| 麒麟 KySec | 绿色包以 root 跑；若读 `~/文档` 报 EACCES，仍建议 `sudo re/otilink/label-kysec.sh` | KySec 禁止未信任二进制读受保护目录（历史坑 L6） |

> 如果你更想要"装一次、以后免提权"：那属于**安装**路径，仍旧用 `re/otilink/install-kylin.sh`
> （udev 规则 + 用户组），两者并存、互不影响。

---

## 3. 三种组合怎么用

| 组合 | 主控端（键鼠插这边） | 接收端 | 剪贴板 |
|---|---|---|---|
| **Linux ↔ Windows** | Linux：`sudo ./otilink.sh`（自动判定 master）<br>Windows：`otilink.cmd master --edge <边>` | 另一端 | 两端都要跑各自入口 |
| **Windows ↔ Windows** | Windows：`otilink.cmd master --edge right` | Windows：`otilink.cmd slave`（键鼠零软件） | 两端都要跑 |
| **Mac ↔ Linux/Windows** | Linux/Windows 做主控 | **Mac：零软件**（什么都不装） | ✗（macOS 侧没有代理） |

**"哪边是主控"的判据**：键鼠物理插在哪一侧，那一侧就是主控（Linux 侧 `$--auto` 会自己判断：
本机有"线缆之外的键鼠" → master，没有 → slave）。两台机器都有键盘（笔记本）时必须显式
`--role master|slave` / `otilink.cmd master|slave`。

---

## 4. 排障（按症状）

| 症状 | 先看什么 |
|---|---|
| `$--doctor` 说"未发现线缆" | 换 USB 口（直插不过 hub）、确认这端插的是对拷线；Linux 看 `ls /sys/class/scsi_generic/` |
| 提权后仍说打不开 `/dev/sg*` | 用 `$sudo ./otilink.sh`；或看 `$dmesg | tail` 有没有 USB 复位 |
| 指针移过去**回不来** | 1) Linux 日志有没有 `$撞边…回程令牌 F24 已发…rc=0`；2) 接收端热键：Linux Ctrl+Alt+←、Windows Ctrl+Alt+→；3) 都不行就 `$pkill -x otikm` 重来 |
| 接收端是 Wayland，撞边不灵 | 日志会打"降级为位移积分判据"：需要"贴边后继续外推 ≥16px"才交还（比 X11 钝一点，但不会失效）；热键始终可用 |
| 剪贴板不通 | `$otilink.sh --doctor` 看剪贴板后端那一行（Wayland 需要 `$wl-clipboard`，X11 需要 `$xclip`）；Windows 看 `$clip.log` |
| Windows 端"指针卡在对面" | `$otilink.cmd --stop` 后重来；确认没有厂商软件在抢（`re/windows/vendor-off.vbs`） |
| 两台机器都在转发/互相抢 | 一条链路只能有一个主人：确认厂商程序（MacKMLink/LinkEngKM/LEWD/SKLoader）没在跑 |

---

## 5. 已验证 / 未验证（诚实清单）

| 项 | 状态 | 证据 |
|---|---|---|
| Linux 绿色包在麒麟（glibc 2.31）上**免安装接管**：键鼠 + 剪贴板 + 大文件 | 见 `re/NOTES.md` 第 39 轮 | `$gate.sh --hw-clip/--hw-km/--hw-xfer` 的 PASS 数 + 两侧日志行 |
| 二进制可移植性（声明基线 + 只依赖 glibc） | **已核对** | `$gate.sh --portable`：`readelf` 最大 GLIBC 需求 ≤ 基线；`ldd` 只有 glibc 自身 |
| Windows 绿色载荷（预编译 exe + ps1，无管理员/无安装） | 见 `re/NOTES.md` 第 39 轮 | `$gate.sh --hw-km/--hw-clip`（以绿色包代理运行时） |
| **Windows ↔ Windows** 真机 | **未真机验证**（本仓库只有一台 Windows） | 机制上：接收端键鼠零软件（线缆对接收端就是真实 USB 键鼠） |
| **Mac 作为接收端** | **未真机验证**（没有 Mac） | 需要用户跑 `$mac/otilink-mac.sh` 并把输出贴回来 |
| Mac 作为主控端 | **未实现** | 见 §6 |
| Linux ↔ Linux | 机制支持（`--peer proto` 对称），**未真机验证** | 本机双实例套件 `$itest/cliptest/tcptest` 覆盖协议层 |
| 跨发行版容器矩阵 | **未做**（本机 Docker 引擎没开） | 打开 Docker Desktop 的 WSL 集成后 `$./portablecheck.sh` 会自动补上 |

---

## 6. 明确不做（以及为什么）

* **macOS 作为主控端**：需要 IOKit `$SCSITaskUserClient` 直通（发厂商私有 SCSI 命令）+
  `$CGEventTap`（抓本机键鼠，需要辅助功能授权）+ `$NSPasteboard`（剪贴板）。
  没有 Mac 真机无法验证，按本项目规范（结论必须带证据）不写"应该能行"的代码。
* **aarch64 等非 x86_64**：`build.sh` 留了扩展点，但没有机器验证，`otilink.sh` 在非 x86_64 上
  会明确报错而不是装作能用。
* **把绿色包放进线缆自带的 1MB 卷**：实测那个卷**两端不共享、写入易失**（`re/NOTES.md` §33.5），
  当不了"随线软件"载体 —— 请用 U盘/目录分发。
* **Wayland 下的进程内剪贴板**：需要 compositor 的 data-control 协议，本轮用 `$wl-clipboard`。

---

## 7. 设计取舍（为什么长这样）

1. **二进制必须"按声明基线"构建**：开发机（Ubuntu 24.04 / glibc 2.39）编出来的 `$otikm` 要求
   GLIBC_2.38，拿到麒麟（2.31）直接起不来 —— 所以默认 `$--baseline kylin`，且门禁用
   `$readelf` 的**实际符号需求**核对基线，而不是相信人记得。
2. **角色/设备判定搬进 C**（`otikm --auto`）：原来在 `$run-kylin.sh` 里靠
   `/dev/input/by-id` 通配 + 硬编码 `/dev/sg3`，换个发行版/线缆固件就失效。
   现在按**设备能力位**（EVIOCGBIT）分类、按 **sysfs 里的 USB VID/PID** 认线缆。
3. **屏幕尺寸自动探测**：原来写死 1920x1080，换台机器撞边判据就偏；现在 X11/XRandR → DRM sysfs → 默认值（告警）。
4. **提权只做一次，且把桌面会话环境带过去**：root 直跑时 `$DISPLAY/XAUTHORITY` 是空的，
   被驱动侧要靠 `$XQueryPointer` 读真实光标 —— 丢了它就"撞边没反应、回不去"（实测过）。
5. **被驱动侧加回程热键 + Wayland 降级判据**：交出去的能力必须有对称的收回来（L8）。
6. **两条路都留着**：绿色包（免安装、每次提权）与安装版（udev 规则、免提权）并存，
   同一份源码、同一套协议，互不影响。

---

## 8. 门禁与发布

```bash
cd re/tools
./gate.sh --portable      # 绿色包门禁（产物新鲜度/GLIBC 基线/依赖/清单/入口脚本/PS 解析 + 可选容器矩阵）
./gate.sh --static        # 静态与文档一致性（含 BOM/CRLF/脚本注册检查）
./gate.sh --full          # 本机 + 三条真机链路（改协议/拓扑时跑）
```

`gate.sh --portable` 会拦住这几类事故：**发的是旧二进制**（源码指纹不符）、**在太新的机器上编**
（GLIBC 需求超过声明基线）、**包里缺文件**、**Windows 脚本丢了 BOM**（实测会让 PowerShell 解析报错）。
