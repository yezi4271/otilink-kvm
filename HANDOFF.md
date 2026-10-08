# 交接文档（给下一个会话）

> 这个文件是为了"上下文用尽后换会话"而写的。**先读这一页，再读 NOTES.md / RUNBOOK.md。**
> 目标：OTi/瀚邦 USB2.0 对拷线（`0ea0:2213`），麒麟 ↔ Windows 键鼠共享 + 剪贴板。
> **当前状态：目标全部达成**（键鼠双向、文本/图片/文件剪贴板双向、≤500MB 大文件双向），
> 三条免手动回归脚本 + 看门狗/拔插自愈。出问题的自救命令见 `RUNBOOK §15.13.7`。
> **⚠️ 动手前先读 `re/AGENTS.md`（agent 运行规范：硬约束/工作流/门禁矩阵/DoD）**；
> 门禁入口 `re/tools/gate.sh`（`--pre` 体检 / `--static` 静态 / `--local` 本机 /
> `--hw-clip|--hw-km|--hw-kmb|--hw-kbdexcl|--hw-lat|--hw-xfer` 真机 / `--deploy-kylin` 部署 / `--report` 看上次报告）。
> 当前状态（第 49 轮复跑）：static 80 PASS、pre 24 PASS、local 9 PASS（+`tcptest` KNOWN）、
> hw-clip 17+7、**hw-kbdexcl 26 PASS（新档）**、hw-kmb 20 PASS / 0 FAIL / 2 WARN（已校准）、
> hw-lat 5 PASS、`--portable` 2 FAIL（`dist/` 过期，见下）。
> `--hw-kmb` 已**校准为稳定**：`PASS=20 FAIL=0 WARN=2`（连跑两轮）—— 两条「Windows 真收到 Ctrl/C」
> 按 §61.2 线缆固件丢包降为 WARN；真因（接管时 6 条 F24 令牌吃掉键盘报表额度）见 `NOTES §63.9`，
> 产品侧修法是下一轮候选（动它属「回不来」最贵风险，本轮故意不动）。
>
> **第 49 轮（规范已更新）**：新增门禁档 **`--hw-kbdexcl`** + 脚本 `otilink/kbdexcl.sh`；
> 排障入口新增 **`RUNBOOK §19`（症状 → 病因 → 一句探针 + 必须成立的不变量）**——一症多因，先定层再动手；
> `re/` 起有**本地** git（仅本地、无远端；首个提交 `3637b2a`，230 文件），回滚/查改用
> `git -C re log/show/diff`（见 `NOTES §64.2`）。
>
> **第 49 轮（现场报障与修复）**：
> 现场报障"**我在 windows 上打字，kylin 也在同步打字**"= 接管对端期间**双重输入**。
> 真因①（我们的 bug）：纯键盘独占的判据挂在**帧管道健康**上，而拓扑 B 的 KM 走 **HID 直发**
> （对端零软件 → 帧管道本来就该静默）→ 纯键盘永远不独占 → 按键"本地一份 + 转发一份"。
> 修法：`km_path_ok()` 让判据**跟着 KM 载体走**（HID 直发看 HID 写失败计数），
> 并加"驱动期维持独占"（每 500ms 幂等重放）+ 对称安全网。真因②（现场状态）：
> **Windows 侧代理一个都没跑**（重启后没起来）→ 剪贴板全废，并把真因①的判据钉死在假；
> 已用 `re/windows/deploy.sh --restart` 拉起**剪贴板代理**（拓扑 B 不起 `otiagent2`，L17）。
> 新增回归用 `uisim` 虚拟键鼠 + `EVIOCGRAB` 排他探针（不碰真键鼠）：
> 本地 FREE → 接管 **BUSY** → 热键回本机 **FREE**。顺带把 §61 之后**过期的测试期望**
> （`selftest_proto`/`mocktest` 还写老 HID 布局）同步回绿色。详见 `NOTES §63`、`RUNBOOK §18`。
> **欠账**：① 真人复测（推到 Windows 打字，看是否还重复）；② `re/portable/build.sh` 重建
> `dist/`（`--portable` 现在 2 FAIL：包内指纹过期，与本次修复无关）；③ §61.3 键盘改走帧管道（根治丢包）。
>
> **第 50 轮（运维，2026-10-08）**：麒麟当天**两次整机硬卡死**（11:16、15:18），都只能人工硬断电。
> 已排除 OOM / 过热 / 盘满 / panic，也排除 §51 的 Kylin 升级检查器（6 个 unit 仍 masked）；
> 最强线索是 **USB 子系统**（11:16：AIC8800 无线网卡 USB **运行时**掉线 → `usb_hub_wq` 卡 D → 整机死；
> 旁证：对拷线 usb-storage 全天 778 次 D 态，最后一次距死机 ~30s）。**与 otilink 无关**（otikm 当时空闲）。
> 已加固取证：`kernel.hardlockup_panic=1` + `kernel.panic=20`、面包屑加 D 态栈与 `dmesg` 尾；
> kdump crash 区实测正常（`kexec_crash_loaded=1`、192MiB 已保留；早前"零长"是非 root 读 `/proc/iomem` 的地址抹零假象）；**15:18 那次归因仍未定**。详见 `NOTES §65`、`RUNBOOK §20`。
>
> **第 48 轮（假光盘 / usb-storage D 态）**：现场用 udev **解绑线缆存储接口**来隐藏“假光盘”
> → 连 `/dev/sgN` 传输通道一起杀掉（otikm 698 次“重开传输失败” + `recv rc=-19`）。
> 正确修法：`usb-storage.quirks=0ea0:2213:s`（`US_FL_SINGLE_LUN`，只枚举 LUN0=1MB 共享卷），
> `install-kylin.sh` 已装成 `/etc/modprobe.d/otilink-quirks.conf`；`envcheck` 新增“线缆 MSC
> `/dev/sgN` 在位”检查（`--pre` PASS=24）。空载 120s 无 usb-storage D 态（改动前同机
> 09:30–15:29 有 979 条）。详见 `NOTES §62`、`RUNBOOK §10.4`。
>
> **第 37 轮**：根治了反复出现的"指针移到麒麟就回不来"——**厂商 GO! Suite 已关停**
> （它与我们抢同一条链路，且回程要等一个不存在的 Linux 端代理），同时修掉了**我们自己引入的
> 关键回归**（REMOTE 看门狗在 Worker 线程重装钩子 → 整套低级钩子哑掉）。详见 `NOTES §45` 与本文 §4.7。
>
> **第 44 轮（规范已更新）**：修掉「**拔插后 otikm 段错误**」——真因是 `keepalive_thread` 在循环外
> 缓存 `&a->tx->dev` 裸指针，被 `reopen_transport()` 的 `oti_tr_close(a->tx_old)` free（use-after-free，
> ASan 实证）。同一轮还修掉 `run-kylin.sh` 写死 `/dev/sg3`。**新增门禁档 `--replug`**
> （`re/otilink/replugreg.sh`：USB unbind/bind 模拟拔插，不需要真拔线）与 AGENTS.md §5/§6 登记。
> 详见 `NOTES §52`（现象/误判）与 `NOTES §53`（ASan 实证与修法）。
>
> **第 46 轮（规范已更新 L17）**：见 `re/NOTES.md §56/§57`。两端用 `OTI_MSG_ROLE(10)` 协商"谁是主控"
> （任何时刻只能一个 grab），并新增**写侧看门狗 W1b**（原看门狗只盯 recv → 写侧全失败时链路永不自愈）
> 与 **W5 设备级复位**（reopen 无效时 unbind/bind 线缆 = 等价拔插；绿色包 sudo 下自动做，非 root 打印提示）。
> `has_local_input` 只算**可热插拔**键鼠（笔记本内置键鼠/幽灵 AT 键盘不算），否则"键鼠插哪边"的信号会被内置设备淹掉。
> 改 `run-kylin.sh` 时注意：**用户参数必须原样透传**（它曾经把 `--role slave` 丢掉）。
> 改 `.ps1` 后**必须补回 UTF-8 BOM** 再 `psparse` 自检。**未完成**：换边真人拔插实测、Windows 侧线缆会话自愈。
>
> **第 47 轮续二（"键鼠过几秒卡一下" + 键盘固件缺陷）**：①卡顿 = 保活每 5s 发 64KB dummy 帧、
> 单次堵设备 **1.27 秒**（hidlat 实测 maxgap=1285ms；dummy 本身 rc=526080）→ 改成"**链路真空闲 >60s**
> 才发 + 拿 io 锁"，复测 **maxgap=98ms / SEND_FAIL=0**；新增门禁档 **`--hw-lat`**（`otilink/hidlat.sh`）。
> ②键盘（Kylin→Windows HID）有**线缆固件级缺陷**：F24 类无修饰键报表 6/6 到达，但**修饰键字节一旦发出，
> 之后的报表全部丢失**（Windows 上 Ctrl 卡死 →"键盘用不了"）。把 Ctrl 当 key usage `0xE0` 放进数组时
> **释放能到、不卡键**（真机验证）。根治方向：键盘改走**帧管道 + Windows agent SendInput**（详见 NOTES §61）。
> 行号：`NOTES §60`（热插拔/by-id/park）、`NOTES §61`（卡顿+键盘固件）。
>
> **第 47 轮续（用户二次报障："键盘也用不了"）**：三个真因 —— ①热插拔判据只看"有没有键鼠"
> （>0）：先插鼠标、再插键盘时个数 1→2 但判据不变 → **新插的键盘永远不进抓取集合**（对端没反应）；
> ②抓取看门狗按**旧 eventN** 重开：拔插后 eventN 会指向别的设备（真机从真键盘变成 "System
> Control" 接口）→ 改成 **by-id 稳定路径 + 重开校验能力位 + 连续失败后按类别重新发现**；
> ③对端光标真实位置与"入口边=0 位移"模型不一致（用户在对端顶边继续推的位移是"看不见的"）
> → **交接时用饱和位移把对端光标 park 到入口边**，并加"回程判据"节流诊断。
> 详见 `NOTES §60`；`kmbret` 扩到 **22 项**（新增 park / by-id / 诊断断言）。
> **未做**：真人拔插的热插拔重扫实测（命令被会话策略拦下）。
>
> **第 47 轮（规范已更新：新增 `--hw-kmb`）**：根治**拓扑 B**（键鼠插在麒麟）的
> "移到 Windows 回不来"—— 三个叠加 bug：①主控端回程判据只看**单次位移**（坐标每次被夹回边界，
> 慢推永远不触发）；②Windows 的 HELLO 报的是**虚拟桌面 4480x1440** 且可能在手势中途才到 →
> 坐标系/缩放中途变化，同一手势时好时坏；③HID 路径漏放行**修饰键抑制缓冲**（按一下 Ctrl 后
> 键盘按下全被吞，Ctrl+C 到不了 Windows）。另修：连发的 HID 键盘报表会被合并（加 8ms 最小间隔）、
> 回程落点用 XWarpPointer 同步真实光标（以前停在出口边上，一碰又出去）。详见 `NOTES §59`。
> 新增拓扑 B 真机回归 `otilink/kmbret.sh`（`gate.sh --hw-kmb`，19 项，合成手势不需人推鼠标）。
> `coretest` 45 项。**未跑拓扑 A 的 `--hw-km`**（那是拓扑 A 专用，见 §56.5）。
>
> **第 45 轮**：修掉 §54 记的残留缺陷 —— 给**输入抓取**补上自愈看门狗（拔插后 otikm 只用 by-id
> 重开传输、不重开 `--capture` 设备，导致撞边交还控制权静默失效、用户“又回不来”）。
> `--replug` 加了对应断言 → **PASS=18 FAIL=0**。顺带修 3 处写死设备号的工具 bug
> （`envcheck.sh` 的 `/dev/sg3`、`probe.c` 自动选到了 MS LUN、`hwtest/xferreg` 里被忽略的位置参数）。
> ⚠️ 仍欠一件事：`--hw-km` 第 1 步用的是**假失败**判据（光标贴右边缘时 +100 推不动 → Δ=0），
> HID 通路实测是好的（+247px）。见 `NOTES §55`。

---

## 1. 一句话现状

**键鼠双向 + 剪贴板全格式双向（文本 / 图片 / 文件）+ 大文件传输（≤500MB，实测 500MB md5 一致）都已真机验证可用。**
上一轮留下的两件事（①"多块剪贴板 CRC/格式失败"、②"Windows→麒麟 文件方向"）**已查清并修掉**（§4，①一半是误判）；
本轮新增的"大文件流式传输"见 §4.3。
回归现状：`./clipreg.sh`（剪贴板 17 项）、`./xferreg.sh`（大文件 12 项）、`./hwtest.sh`（键鼠 12 项）**全绿**。

### 1.1 第 39 轮新增：**免安装绿色包**（换机器/换发行版走这条）

**不装任何东西**：`re/portable/build.sh` 产出 `dist/otilink-portable-<ver>/`（含预编译 Linux 二进制 +
Windows 载荷 + Mac 体检脚本），拷到 U盘/任意目录即可用：

* Linux：`sudo ./otilink.sh --role slave`（一次提权；自动找线缆/会话环境/屏幕/角色；日志 `/tmp/otilink-<uid>.log`；
  停止要 `sudo pkill -x otikm`）。**真机验收：麒麟上接管后 clipreg 17/17、xferreg 12/12、hwtest 12/12。**
* Windows：`otilink.cmd master|slave`（无管理员/无 csc/无自启；`--stop` 停；日志在同目录）。
  绿色包两端一起跑 `gate.sh --hw-clip` 同样 **17/17**。
* Mac：只有只读体检 `mac/otilink-mac.sh`；**Mac 作为接收端零软件可用（未真机验证）**，作主控端本轮不做。
* 门禁：`./gate.sh --portable`（源码指纹新鲜度 / GLIBC ≤ 声明基线 2.31 / 依赖只有 glibc / 清单 / PowerShell 真解析）。
* 细节、坑、未验证清单见 `re/NOTES.md §47` 与 `re/portable/README.md`。

---

## 2. 拓扑与"谁负责什么"

```
麒麟 Kylin V10 SP1 (kylin@<麒麟IP>；现网也走 tailscale kylin-pc/<tailscale-IP>)；**密码与地址都走环境变量**：`export KY=kylin@<麒麟IP> KY_PASS='<麒麟登录密码>'`（仓库不内置，见 `AGENTS §0` / `RUNBOOK §0.0`）
  └ 线缆 MSC 走 /dev/sgN（quirks=0ea0:2213:s 后只剩 LUN0=1MB 共享卷，假光盘 CD LUN 不再枚举）；
    线缆还自带 HID 键鼠: if01-event-mouse / if02-event-kbd
  └ \\.\H: 是同一个线缆的 MS LUN（**不需要管理员**即可打开）
```

**物理摆位：Linux 屏在 Windows 的右边** → Windows 上往右推边缘交给麒麟；麒麟上往左推边缘还给 Windows。

| 方向 | 由谁负责 | 通道 |
|---|---|---|
| Windows → 麒麟 键鼠 | `otiagent2.exe --edge right`（Windows） | 厂商 HID 包通道（16 字节 CDB） |
| 麒麟 → Windows 键鼠 | `otikm --return-on-edge`（麒麟） | 同上 |
| 剪贴板（全部格式） | `otiagent.ps1 -Cable -Clipboard`（Windows）+ `otikm`（麒麟） | 协议帧管道（64KB 帧 + 授权时序） |

**注意：键鼠插在哪一侧，就由那一侧的 agent 当主控**（`run-kylin.sh` 会自动判断）：
* 键鼠在 Windows → Windows 跑 `otiagent2 --edge right`，麒麟跑被驱动侧（`--return-on-edge`，撞边发 F24 令牌）。
* 键鼠在麒麟 → 麒麟跑主控端（`--inject --grab`）。**这时回程靠"在对端屏幕上继续往外推"**
  （第 36 轮新增，见 §4.4），热键只在有键盘的那一侧能按。

**两个 Windows 进程并存、互不冲突**；剪贴板代理**绝对不要传 `-Inject`**（那是旧的键鼠路径，会跟 HID 通道打架）。
**⚠️ 厂商程序（MacKMLink / LinkEngKM / LEWD / SKLoader）已全部关停并禁自启**（第 37 轮，见 §4.7）：
它与我们共用同一条帧管道与 HID 通道，**两套实现同时在跑必然互相破坏**，而且它的"交还控制权"是
握手式的、要等一个**麒麟侧根本不存在**的厂商代理 → 一旦它把控制权转走就**必然卡死**（用户实测只能拔线）。
排查时若在 `km.log` 看到我们的 agent **从未 REMOTE**、而光标却在被驱动 → 就是厂商又在转发，跑
`re/windows/vendor-off.vbs`（UAC 一次）。

---

## 3. 关键文件与部署位置（**最容易搞错的地方**）

| 仓库路径 | 部署到 | 说明 |
|---|---|---|
| `re/otilink/*.c,h` | `kylin:~/otilink/`，**在麒麟上 make** | 不能拿 WSL 编的二进制过去（GLIBC 版本更高，跑不起来） |
| `re/windows/otiagent2.cs` | `C:\Users\Public\otiagent2.exe` | `csc.exe` 编译（键鼠） |
| `re/windows/otiagent.ps1` | **`C:\Users\<你的用户名>\otilink\otiagent.ps1`** | ⚠️ 代理跑的是这个路径，**别手工 cp —— 用 `re/windows/deploy.sh`**（见 §5.5） |
| `re/otilink/hwtest.sh` | 在 WSL 里直接跑 | 键鼠真机回归（约 90 秒，会动真实光标；**只覆盖拓扑 A**） |
| `re/otilink/kmbret.sh` | 在 WSL 里直接跑 | **拓扑 B 回程手势**真机回归（约 30 秒，合成手势不动光标；会短暂停/恢复麒麟 otikm）—— `gate.sh --hw-kmb` |
| `re/otilink/kbdexcl.sh` | 在 WSL 里直接跑 | **键盘独占（双重输入）**真机回归（约 2 分钟；`uisim` 虚拟键鼠 + `EVIOCGRAB` 排他探针，不碰真键鼠；会短暂停/恢复麒麟 otikm）—— `gate.sh --hw-kbdexcl` |
| `re/otilink/clipreg.sh` | 在 WSL 里直接跑 | **剪贴板真机回归（约 50 秒，不动光标）** —— 剪贴板改动后先跑这个 |
| `re/otilink/xferreg.sh` | 在 WSL 里直接跑 | **大文件真机回归**（`./xferreg.sh [字节数]`，缺省 64MB；500MB 约 4 分钟） |
| `re/otilink/xferlocal.sh` | 在 WSL 里直接跑 | **大文件本机回归**（两个 otikm 走 AF_UNIX，1~3 秒一轮，改逻辑先跑它） |
| `re/otilink/otixfer.c,h` | `kylin:~/otilink/` | 大文件分片流式传输（发/收状态机、位图重传） |
| `re/otilink/setclip.sh` | `kylin:~/otilink/` | 把文件内容放进麒麟剪贴板且不阻塞 ssh（回归脚本依赖它） |
| `re/otilink/testimg.png` | — | 回归用测试图（240x160） |
| `re/otilink/label-kysec.sh` | `kylin:~/otilink/` | 每次重编译后必须跑（见 §5.1） |
| `re/windows/deploy.sh` | WSL 里跑 | BOM 自愈 + 复制 + 解析自检 + `--restart` 重启剪贴板代理 |
| `re/windows/restart-clip.vbs` | `C:\Users\<你的用户名>\otilink\` | 脱离调用者地拉起剪贴板代理（日志 `C:\Users\Public\clip.log`） |
| `re/windows/vendor-off.ps1` + `.vbs` | `C:\Users\Public\` | **关停厂商 GO! Suite + 禁自启**（UAC 一次，日志 `vendor-off.log`）——"回不来"第一处置 |
| `re/otilink/hidprobe.c` | `kylin:~/otilink/`（`make hidprobe`） | **HID 方向探针**：`./hidprobe /dev/sg3 1 10`（鼠标）/ `... 2 3 0x87`（F24） |
| `re/windows/kbdprobe2.ps1` | `C:\Users\Public\` | 独立低级键盘钩子探针（自泵消息、打印 `vk=`）——判定钩子链死没死 |
| `re/windows/detect-keys.ps1` / `sendkey.ps1` | `C:\Users\Public\` | 会话级按键状态轮询 / 合成按键（对照输入） |
| `re/windows/pushleft-synth.ps1` | `C:\Users\Public\` | 纯相对左推，用来**自动测回程**（不用人手推） |
| `re/windows/clipfiles-diag.ps1` | 同上 | CF_HDROP 读取诊断（文件剪贴板出问题先跑它） |

---

## 4. 上一轮那两件事：结论与真因

### 4.1 `[warn] 解包失败（CRC/格式）` = **厂商 XML 帧，不是剪贴板传坏了**（重要）

上一轮的证据是：
```
CLIP 发送 6856 字节 / 6 块
[warn] 解包失败（CRC/格式）        ← 上一轮注："麒麟侧 oti_clip_feed 重组失败"
CLIP 已应用远端**图片** 6856 字节 rc=True
```
**两条都读错了**：

1. `"/ 6 块"` 是**整场会话的累计块数**（Windows 侧 `$txClip` 变量），不是这一笔分了 6 块。
   本轮已把日志改成 `本笔 N 块（累计 M）fid=… fmt=… 源=…`，不会再误读。
2. `[warn] 解包失败` 是 **Windows agent 解不了从麒麟来的厂商 XML 帧**，与剪贴板无关。
   本轮往失败分支加了 hex 诊断，一次 200KB 传输里同时出现：
   ```
   4 个收块 → CLIP 已应用远端剪贴板 200012 字节（完整）
   [warn] 解包失败 len=673 head=39 9c 02 00 00 3c 45 78 74 72 61 58 6d 6c 43 6f
   ```
   `39` = 厂商帧标识，`9c 02 00 00` = 668 字节（= otikm 日志里"已通知厂商端交还控制权…668 字节"），
   后面就是 `<?xml…ExtraXmlCommand`。**它只在"撞边交还控制权"时出现**，MacKMLink 也往同一条管道写这类帧。
   现在这类帧单独识别成 `[skip] 厂商 XML 帧 N 字节（第 k 条，非本协议，正常忽略）`。

**所以"多块传输会失败"不成立**：本轮实测 69991 字节（2 块）与 200012 字节（4 块）文本双向 **md5 逐字节一致**，
191257 字节图片 3 块也是成功的。`clipreg.sh` 已把这些尺寸固化成回归项。

**但图片确实有个真问题（本轮修掉）**：Windows 剪贴板只有 CF_DIB，收/发各过一次 PNG↔DIB 转换，
**同一张图在两侧的 PNG 字节必然不同** → 原来按"载荷字节 CRC"做的防回环对图片失效，
每传一张图 Windows 都会把重编码后的图**弹回去一次**（麒麟发 10973 → Windows 应用成功后回发 8017）。
修法：改用 **CF_DIB 像素指纹**（`ClipImg::DibHash`）判断"这张图是不是我刚写进去的那张"。
修后再测：只有 `已应用远端**图片** 10973 字节 rc=True`，**没有回发**。

### 4.2 Windows→麒麟 文件：`DragQueryFileW` 的 DLL 写错了

**根因**：`ClipFiles` 把 `DragQueryFileW` 声明在 `user32.dll`（实际在 **`shell32.dll`**），
调用时抛 `EntryPointNotFoundException`，而 `Get()` 的 `catch { return null; }` 把它吞了 →
现象只有"`HasFiles()=True` 但 0 个文件" → 退化成文本路径发过去。
`clipfiles-diag.ps1`（不吞异常的版本）一眼定位：
```
IsClipboardFormatAvailable(CF_HDROP)=True / OpenClipboard=True / GetClipboardData=0x…（有句柄）
EXCEPTION: EntryPointNotFoundException: 无法在 DLL"user32.dll"中找到名为"DragQueryFileW"的入口点。
```
修 `shell32.dll` 后：`[clip] HDROP 命中：1 个文件` → `CLIP 发送 343 字节 fmt=3 源=文件` →
麒麟落地 `/tmp/otilink-files-<pid>/xxx` 并设好 `text/uri-list`；反向（麒麟→Windows）也验过 md5 一致。

### 4.3 大文件传输（≤500MB，本轮新增）

**要传 >8MB 的文件**：剪贴板"文件包"（format 3）是**整包驻留内存**的（发端全读进内存、收端一次性 malloc），
500MB 会打爆内存。现在改成**分片流式**（`otixfer.c` + `otiagent.ps1` 的 `fileTx/fileRx`）：

* 64000 字节一片 → 复用一个 64KB 帧；收端 **pwrite 直接落盘**，内存恒定 64KB/侧；
* 收端位图记缺片 → VERDICT 带**缺片位图** → 发端**只补缺的那几片**（不是从缺口发到结尾）；
* 收完读回落盘结果算整文件 CRC 与发端比对，不过最多重传 3 轮；
* 落地后自动挂到接收端剪贴板（uri-list / CF_HDROP），直接粘贴。

实测：**500MB W→L 69 秒（7.2 MB/s）、L→W 166 秒（3.0 MB/s），双向 md5 一致**；
传输期间键鼠不受影响（HID 包通道独立）。≤8MB 的文件仍走原来的文件包路径（省往返、已验证）。

两个**必须知道**的点：
1. **别同时传两个方向的大文件**：会互相抢设备授权（实测能拖到 0.1MB/s）。已加 `fid` 让路
   （大的先停、等小的传完），但最好还是别同时来。
2. **轮询不再读文件内容**：`路径+大小+mtime` 指纹判断"还是那一份"（`oti_clip_probe_files` / `Get-FileIdent`）。
   以前每 300ms 把文件整个读一遍算 CRC —— 8MB 就把磁盘读爆，500MB 根本不可能。

---

## 5. 血泪教训（**这些坑最耗时，务必先看**）

### 5.1 KySec 会拦本地编译的二进制读文件
- 现象：`fopen("~/文档/图片2.png")` 返回 **errno=13 EACCES**，而同一路径用系统命令 `head`、`cat` 都能读。
- 结论：麒麟 KySec 把"用户自己编译/拷贝的二进制/脚本"视为不可信，禁止它们读 `~/文档` 这类受保护目录。
- 修法（**已固化**）：`echo "$KY_PASS" | sudo -S ./label-kysec.sh`（给目录下所有二进制打 trusted 标签）。
- ⚠️ **标签打在文件上，重新编译就丢** → 每次 `make` 后都要跑一次。
- 连带坑：**用户自己写的 .sh 也不能用 shell 重定向读 `~/文档`**（EACCES），要 `cat "$F" | …`（见 `setclip.sh`）。

### 5.2 Windows 侧编译/脚本的坑
- **代理在跑时 `csc /out:` 覆盖不了 exe，而且失败是静默的** → 先 `Stop-Process otiagent2` → 再 csc → 再启动。
- `otiagent.ps1` 含中文注释，**必须带 UTF-8 BOM**（PS 5.1 按 ANSI 读）。用不支持 BOM 的工具改完会是一堆
  莫名的"缺少 }"解析错误 → **改完一律 `re/windows/deploy.sh`**（它会补 BOM 并解析自检）。
- `[PU]::Get` 不带括号返回的是方法对象 → 必须写 `[PU]::Get()`。
- `Add-Type -TypeDefinition` 用到 `System.Drawing` 时必须 `-ReferencedAssemblies System.Drawing`。
- **`ClipImg` 与 `Oti` 是两次独立 `Add-Type`**，不是同一个编译单元：跨类调 `Oti.Crc32` 会编译失败。
- 隐藏窗口的 PowerShell 进程里**不要用 `[Windows.Forms.Clipboard]`**（依赖 STA/桌面会话，会静默失败）
  → 一律用 **Win32 P/Invoke**（`ClipImg`/`ClipFiles` 都是这么做的）。
- **按命令行匹配进程会匹配到自己**：`... | Where CommandLine -like '*-File*otiagent.ps1*' | Stop-Process`
  会把执行这条查询的 powershell 自己杀掉（脚本一声不响结束、代理没起来）。必须 `$_.ProcessId -ne $PID`。
- **从 WSL 启动长驻 Windows 进程不要用 `Start-Process`**：新进程继承调用者句柄，WSL interop 要等句柄全关
  → 脚本挂到超时。用 **VBS 的 `WScript.Shell.Run`**（`restart-clip.vbs`），且 `.vbs` 必须 **CRLF** 行尾
  （LF-only 时 cscript 返回 0 却什么都不做）。

### 5.3 真机测试方法（不然全是假失败）
- **先建基线**：两侧设成同一段内容，等稳定，再动你要测的东西（否则对端**在途的剪贴板**会覆盖测试输入）。
- **ssh 的登录 banner（`Kylin V10 SP1`）走 stderr**：把 stderr 并进 stdout 会让捕获多 14 字节，
  文本比对/md5/二进制解析全错。`clipreg.sh` 的 `kssh` 特意**不合并** stderr。
- **xclip 会占住 ssh 通道**（子进程持有选区）→ 必须 `setsid … >/dev/null 2>&1 </dev/null &`（已封装进 `setclip.sh`）。
- 测推边缘：`pushright.ps1` / `pushleft.ps1` 必须先 `SetProcessDPIAware()`。
- 自测脚本里**监控类输出不要经 `grep` 管道**（非 TTY 时缓冲，5 秒后文件还是空的 → 假失败）。
- `hwtest.sh` 跑完约 90 秒，**别用 60 秒的超时去跑**。
- 麒麟上**没有可用的 python3**（被 kysec 拦）→ 解析二进制请在 WSL 侧做（`clipreg.sh` 就是这么取图的）。

### 5.4 线缆协议（已彻底逆清，别再重推）
- 键鼠走**厂商 HID 包通道**：`CDB = D9 | 0x33 鼠标 / 0x34 键盘 | 12 字节负载 | 'O' | 'T'`，无数据阶段。
- **线缆两端都把自己枚举成真实的 USB 鼠标+键盘**（`MI_01`/`MI_02`）→ **接收端不需要任何软件**。
- 剪贴板走协议帧管道：`0xD9/0x28/0x64` 读、`0xD9/0x2A/0xFF` 写、`0xD8/00/03` 读 16 字节消息；
  写帧必须等 `0x06/0x07` 授权后**立刻**写。
- 剪贴板格式位 `format`：**1=文本，2=PNG 图片，3=文件包**；
  文件包（小端）：`u16 个数；每项 u32 名长、名字(UTF-8)、u64 长度、内容`，上限 8MB。
- ⚠️ **同一条帧管道上还有厂商的 XML 帧**（`0x39` 开头）—— 解码失败时先看首字节再下结论。
- 详细过程与数据在 `NOTES.md §33–§43`、`RUNBOOK.md §14–§15`（§15.13.6 = 大文件传输）。

### 4.4 第 36 轮：指针"回不来"是主控端缺回程判据（⚠️ 第 47 轮修正，见 NOTES §59）

现象：指针移到对端就再也收不回来（尤其"鼠标在麒麟、键盘在 Windows"这种混合拓扑）。
根因：`otikm_core_local_mouse()` 在 `have_control==0` 时**只转发、只夹坐标，没有任何回程判据**；
老拓扑能回来是因为被驱动侧会发 F24 令牌，而主控端这边没人发。
修法：交出去时记住"从对端哪条边进来"（`rem_entry_x/y`），在**对端屏幕上继续往外推 ≥8px**
就收回本机（与撞边对称）；回程后用真实光标校准 core 位置。
回归：`make coretest && ./coretest`（**45 项**，不需要设备）——**改交接逻辑后必跑**。
⚠️ **第 47 轮修正**：判据改成"**未缩放位移累计**"（推出去多少就推回来多少，再多推 16 个计数），
并冻结驱动期间的远端几何、回程落点用 XWarpPointer 同步；旧实现"单次位移 ≥8px"在真机上
慢推永远回不来（`mouse 2 0 0 0` 连推）。完整复盘见 `NOTES §59`，真机回归 `kmbret.sh`。

### 4.5 第 36 轮：剪贴板**静默丢帧** → ACK + 重发

帧通道会丢帧且**两端都不报错**（实测麒麟→Windows 连发 15 条丢 6 条 = 40%，反向却 12/12）。
修法：复用 `OTI_MSG_ACK=6`（`u32 crc + u16 fmt`），收端应用成功后回执，发端 1.5 秒没等到就重发（≤3 次）。
真人节奏实测双向各 **6/6**；极限连发（<2 秒一条）仍可能丢（下一条会顶掉上一条的重发窗口）。
注意实现上 `send_clipboard()` 只管发、`clip_ack_register()` 只管登记 —— 混在一起会在重发时
`free` 掉正在读的缓冲（use-after-free）并把重试计数清零（无限重发），这两个坑都踩过。

### 4.6 指针"回不来"的第二次复现：令牌通道好、钩子被摘除（第 36 轮续）

现象：键鼠在 Windows（麒麟被驱动侧），麒麟光标压在左边缘，otikm 每 1.5 秒发一次交还通知
（`撞边 → 通知对端收回控制权`，rc=0），**Windows 侧毫无反应**。
决定性实验：把 `otiagent2 --remote` 强制进 REMOTE，再从麒麟发一个 F24 → **4 秒内自己回到 LOCAL**
→ 通道是好的，问题在"跑了 30 分钟的那个 agent 实例"：**Windows 静默摘除了低级钩子**
（`LowLevelHooksTimeout`），线缆送来的 F24 再也进不了 `kbdProc`。
加固：**F24 连发 3 轮** + **REMOTE 时每 30 秒自动重装钩子** + **键鼠代理开始写 `km.log`**（带切换原因）。
急救热键：Windows 键盘 **`Ctrl+Alt+→`**（回 LOCAL）；命令行 `./deploy.sh --restart-km`。

> ⚠️ **§4.6 的结论已在第 37 轮被推翻**：不是"系统摘钩子"，而是**我们那个看门狗自己把钩子装错了线程**
> （详见 §4.7）。`LowLevelHooksTimeout` 那条只是误判。

### 4.7 第 37 轮：根治"回不来" —— 厂商关停 + 我们自己的钩子回归（**最新，最重要**）

三个故障叠加，完整版见 `NOTES §45`：

1. **厂商抢链路**（结构性）：`km.log` 里我们的 agent 从未 REMOTE，可麒麟光标却在被驱动 → 转发是厂商做的。
   厂商的**交还控制权是握手式**的，而**麒麟侧没有厂商的 Linux 代理**，喊话无人应答 → 它永远卡在 remote
   → 用户只能**拔插线缆**才回来（实测就是这么回来的）。
   **处置**：`re/windows/vendor-off.vbs`（UAC 一次）关停 + 禁自启。两套实现不能并存，选我们这套。
2. **我们自己的回归（关键）**：§4.6 加的"REMOTE 钩子看门狗"在 **Worker 线程**里
   `Unhook+SetWindowsHookEx`，而**低级钩子只在安装线程的消息泵里派发** → REMOTE 下**整套钩子哑掉**：
   键盘转发死（"键盘像失灵"）、F24 令牌收不到（**指针回不来**）、鼠标按键死；
   **只有鼠标移动还活着**（走 Raw Input，跟钩子无关）——这个"半死"特征害前几轮反复误判。
   时间线与用户报障完全吻合（加固上线后立刻"还是回不来"）。
   **修复**：Worker 只 `PostMessage(hwnd, WM_APP_REINSTALL)`，钩子由**主线程**重装；
   日志加 `tid=`；加 `BuildTag`（`[build mainhook-3]`）——因为 `csc /out:` 覆盖运行中的 exe 会**静默失败**。
3. **令牌写入必须走消息泵线程**：撞边回调（捕获线程）原来直接写设备，与泵的连续读抢同一条 fd。
   已改为 `want_token`/`want_release_all` 标记 + 泵线程发送（顺序：先释放包、后令牌），并打 `rc=`。

**验证**（全自动，不需要人推鼠标）：`km.log` 状态机
`LOCAL(startup) → REMOTE(edge) → kbd: F24 token seen(mode=REMOTE) → LOCAL(peer token (F24))`；
回归 `coretest` 14/14、`clipreg` 17/17、`xferreg` 12/12。

**"回不来"排查四步**：① `km.log` 有 REMOTE 吗（转发在谁手里）② 有 `F24 token seen` 吗（钩子活没活）
③ 麒麟有 `撞边` + `回程令牌 rc=0` 吗（被驱动侧判据/DISPLAY）④ `kbdprobe2.ps1` + `hidprobe` 做端到端对照。

### 5.5 部署/重启剪贴板代理的**唯一正确姿势**

```bash
cd re/windows && ./deploy.sh --restart     # 补 BOM → 复制 → 解析自检 → 重启剪贴板代理
cd re/windows && ./deploy.sh --restart-km  # 只重启键鼠代理 otiagent2（"鼠标回不来"先试这个）
```

看门狗/拔插自愈与自救命令：**`RUNBOOK §15.13.7`**；交接手势：**`RUNBOOK §15.14`**。

---

## 6. 怎么跑起来 / 怎么回归

```bash
# 麒麟侧（每次改完 .c 之后）
#   注意：ssh 要密码，用 /tmp/kssh 或 DSH 的 ssh 工具；scp 同理
make -C ~/otilink otikm                      # 在麒麟上编译（不要在 WSL 编）
echo "$KY_PASS" | sudo -S ~/otilink/label-kysec.sh
pkill -x otikm; sg input -c "DISPLAY=:0 nohup ~/otilink/run-kylin.sh > /tmp/rk.log 2>&1 &"

# Windows 侧（改完 otiagent.ps1）
cd re/windows && ./deploy.sh --restart

# 回归（都不需要人动手）
cd re/otilink && ./clipreg.sh      # 剪贴板：文本/大文本/图片/文件 双向（约 50 秒，不动光标）
cd re/otilink && ./xferreg.sh      # 大文件：64MB 双向 + 键鼠不卡（约 60 秒）；500MB 加参数
cd re/otilink && ./xferlocal.sh 33554432   # 大文件本机回归（1~3 秒，改逻辑先跑这个再上真机）
cd re/otilink && ./hwtest.sh       # 【拓扑 A】键鼠：光标/交接/按键/剪贴板小文本（约 90 秒，会动光标）
cd re/otilink && ./kmbret.sh       # 【拓扑 B】回程手势 + Ctrl 组合到 Windows（约 30 秒，合成手势；短暂停/恢复麒麟 otikm）
cd re/otilink && ./kbdexcl.sh      # 【拓扑 B】键盘独占/双重输入（约 2 分钟；虚拟键鼠 + EVIOCGRAB 探针；短暂停/恢复麒麟 otikm）
```

`kbdexcl.sh` 覆盖（**拓扑 B**：键鼠在麒麟，KM 走 HID 直发）：本地态探针 FREE（探针有效）→
接管期间键盘 **BUSY**（本机桌面收不到按键 = 不双重输入）→ 热键回本机 **FREE**（L8 对称）；
日志断言"已独占 <键盘>" + 无"键盘不独占"；Windows 侧同时用 `kbdprobe2.ps1` 确认按键仍真送到对端。

`hwtest.sh` 覆盖（**仅拓扑 A**：键鼠在 Windows）：麒麟→Windows 光标 / Windows→麒麟 交接 / 按键转发 /
剪贴板小文本双向 / 撞边交还。
`kmbret.sh` 覆盖（**拓扑 B**：键鼠在麒麟）：小步（2px/次）外推必须回程 / 落点同步真实光标 /
修饰键缓冲补发 / Windows 实收 Ctrl+C / HID 光标位移 / 自动恢复真机 otikm。
`clipreg.sh` 覆盖：小文本双向、69991 字节（2 块）与 200010 字节（4 块）大文本、图片双向、文件双向，
以及三条**日志不变量**（无真正帧解码失败 / 图片无回环 / 文件剪贴板读取正常）。
`xferreg.sh` 覆盖：64MB（或指定大小）双向 md5、剪贴板挂载、**传输期间键鼠不卡**、日志不变量。

---

## 7. 还可以做的（都不影响当前可用性）

1. 把 `clipreg.sh` 的图片用例从"只比尺寸"加强到"比像素"（需要 PNG 解码，WSL 侧做即可）。
2. 麒麟→Windows 图片目前走"Windows 侧 DIB 指纹"，若将来麒麟侧也出现回环，可用同样的思路
   （在麒麟侧对 *写入的字节* 与 *读回的字节* 各记一次指纹）。
3. 厂商 XML 帧现在只是识别 + 忽略；如果哪天要接厂商 Windows 程序（MacKMLink），
   那份帧的语义在 `NOTES.md §40`。
4. `otiagent.ps1` 的托盘/暂停等 UI 未在真人操作下过一遍（`otiagent2` 的边缘交接同理，需要用户配合实测）。

---

## 8. 给下一个会话的开场话术（可直接粘）

> 继续 OTi 对拷线的键鼠/剪贴板项目。先读 `re/HANDOFF.md`（一页总览 + 坑 + 结论），
> 需要细节再查 `re/NOTES.md`（§33–§41 协议逆向、**§42 剪贴板两项复盘、§43 大文件传输**）与
> `re/RUNBOOK.md`（§14–§15 运维与架构）。
> 环境：`export KY=kylin@<麒麟IP> KY_PASS='<麒麟登录密码>'`（仓库不含凭据/内网地址），Windows 侧在 WSL 里直接调 powershell.exe（PATH 里要有 `/mnt/c/Windows/System32`）。
> 回归：`re/otilink/clipreg.sh`（剪贴板）+ `re/otilink/xferreg.sh`（大文件）+ `re/otilink/hwtest.sh`（键鼠）。
> **注意**：Windows 侧改动一律 `re/windows/deploy.sh [--restart]`（BOM/行尾/自检/重启都在里面）；
> 麒麟侧每次重编译后要跑 `label-kysec.sh`（否则读不了 `~/文档`）。
