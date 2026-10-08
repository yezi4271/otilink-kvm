# AGENTS.md —— 对拷线项目（`re/`）的 agent 运行规范

> **这是硬约束，不是建议。** 任何 agent（模型或人）在本目录下**改代码 / 部署 / 验证 / 下结论**之前，
> 必须先读本文件。它规定四件事：**谁负责什么、什么绝对不能做、改完必须跑哪些门禁、结论必须带什么证据。**
>
> 深入材料：`re/RUNBOOK.md`（运维与排障）、`re/NOTES.md`（逆向过程与复盘）、`re/PROTOCOL.md`（线缆协议）、
> `re/HANDOFF.md`（一页交接，上下文快满时先写它）。
>
> 项目：把一根 **OTi/瀚邦 USB2.0 对拷线**（`0ea0:2213`）变成真 KVM —— 麒麟 ⇄ Windows 键鼠共享 +
> 剪贴板（文本/图片/文件）+ 大文件传输（≤500MB）。**不需要网络，Windows 侧键鼠零软件。**

---

## 0. 30 秒上手（最小闭环）

```bash
# 一次性环境准备：仓库里**不含**任何凭据与内网地址，以下两个变量必须自己提供
#   （建议写进 ~/.otilink_env（chmod 600）后 source，避免进 shell 历史）
export KY=kylin@<麒麟IP>            # 或 tailscale 名，如 kylin-pc
export KY_PASS='<麒麟登录密码>'

cd re/tools
./gate.sh --pre          # ① 施工前体检：环境/拓扑/安全前提（不动任何东西）
#  ...改代码...
./gate.sh --static       # ② 静态门禁：语法/BOM/CRLF/版本指纹/文档一致性（秒级）
./gate.sh --deploy-kylin # ③ 改的是 Linux 侧？同步到麒麟 + 编译 + KySec 标签
./gate.sh --hw-clip      # ④ 按改动选硬件档：--hw-clip / --hw-km / --hw-xfer / --hw-all
./gate.sh --report       # ⑤ 贴报告（/tmp/otilink-gate.json）+ 关键日志行，才算完成
```

Windows 侧改动（`re/windows/*.cs|*.ps1`）的唯一部署姿势：

```bash
cd re/windows && ./deploy.sh --restart      # 剪贴板代理
cd re/windows && ./deploy.sh --restart-km   # 键鼠代理（先杀→编译→启动→打印版本）
```

---

## 1. 项目边界

| 位置 | 是什么 | 权威文档 |
|---|---|---|
| `re/otilink/` | **麒麟侧**：`otikm` 守护进程（键鼠共享 + 剪贴板 + 大文件）、线缆协议库、回归脚本 | `re/otilink/README.md`（部分过时，以 `re/HANDOFF.md` 为准） |
| `re/windows/` | **Windows 侧**：`otiagent2.cs`（键鼠代理）、`otiagent.ps1`（剪贴板代理）、部署与排障脚本 | `re/RUNBOOK.md §15` |
| `re/tools/` | **门禁与验证编排**（本规范配套）：`gate.sh` / `envcheck.sh` / `doccheck.sh` / `lib.sh` / `known-issues.txt` / `template-verify.sh` | `re/AGENTS.md`（本文件） |
| `re/portable/` | **免安装绿色包**（第 39 轮）：`build.sh`（按声明基线组装）/ `otilink.sh`（Linux 入口）/ `otilink.cmd`+`otilink-win.ps1`（Windows 入口）/ `mac/otilink-mac.sh` / `dist/`（产物） | `re/portable/README.md` |
| `re/mac/`、`*.py` | 早期逆向工具（Mach-O 反汇编等），**不属于运行路径**。厂商二进制（`mac/MacKMLink.app`、`WinDroid_Linker.apk`）是**逆向输入材料**，**不入库**（本地保留，见 `.gitignore`） | `re/NOTES.md` |

**机器与路径**

| 端 | 地址/路径 | 备注 |
|---|---|---|
| 开发机 | 本仓库（WSL 内），可调 `powershell.exe` | 文件在 `/mnt/c` 可见 |
| Windows | `C:\Users\Public\otiagent2.exe`、`km.log`、`clip.log` | 部署目标是 `C:\Users\<你的用户名>\otilink\` |
| 麒麟 | `kylin@<麒麟IP>`（密码由环境变量 `KY_PASS` 提供，**仓库不内置**）；现网也走 tailscale `kylin-pc`（`<tailscale-IP>`） | `~/otilink/`；线缆 MSC 走 `/dev/sgN`（`quirks=0ea0:2213:s` 后只有 LUN0），HID `by-id/usb-_Android+Mac_*` |
| Windows 线缆 | `\\.\H:`（MS LUN，无需管理员） | `--discover` 会按 `0xF0/0x00` 探测盘符 |

---

## 2. 拓扑：**谁在转发**（先判断这个，再谈任何故障）

物理摆位：**麒麟屏在 Windows 右边**。两套合法拓扑：

| 拓扑 | 键鼠插在哪 | 麒麟侧跑什么 | Windows 侧跑什么 | 回程手势 |
|---|---|---|---|---|
| **A（当前验证过）** | Windows | `otikm --return-on-edge`（被驱动，capture 线缆 HID） | `otiagent2.exe --edge right`（主控） | 把麒麟指针推向**朝 Windows 那条边**并保持 ~0.3s → 发 F24 令牌 |
| **B** | 麒麟 | `otikm --inject --grab`（主控，capture 本机键鼠） | **键鼠零软件**（剪贴板代理仍要跑） | 把指针**推回入口边**并继续推（多屏习惯，主控端本地判定） |

* 麒麟侧用 `run-kylin.sh` 启动时会**自动判断**：没有本机键鼠 → 被驱动侧；有 → 主控端。
* **判断当前是谁在转发**：`C:\Users\Public\km.log` 有没有 `REMOTE`。没有 REMOTE 而光标却在被驱动
  → **转发不在我们手里**（厂商程序复活了，见 L1）。
* **绝不允许两套实现同时在线**：厂商 `MacKMLink/LinkEngKM/LEWD/SKLoader` 与我们抢同一条帧管道与 HID 通道，
  而且它的"交还控制权"是握手式的、要等一个麒麟侧不存在的代理 → **一接管就必然卡死**（用户实测只能拔线）。

---

## 3. 硬约束（LAWS）—— 违反即回滚

| # | 约束 | 为什么（真实事故） |
|---|---|---|
| **L1** | **一条链路只能有一个主人**：厂商程序关停（`re/windows/vendor-off.vbs`），我们的 agent 与它不能并存 | 厂商在第 37 轮把用户的指针锁死在麒麟侧，只能拔线恢复 |
| **L2** | **低级钩子只能在"安装它的那个线程"（消息泵线程）上装/重装** | 看门狗在 Worker 线程重装 → 整套钩子哑掉：键盘转发与 F24 令牌全废（"键鼠失灵/回不来"），而鼠标移动仍正常（走 Raw Input）→ 极具误导性 |
| **L3** | **设备 fd 只能由一条线程读写**：捕获线程不许直接发设备命令，用 `want_token` / `want_release_all` 交给消息泵线程 | 捕获线程与泵线程抢同一条通道的授权消息 → 命令被吞（令牌石沉大海） |
| **L4** | **同时只能有一个 `otiagent2` 实例** | 多实例 = 多套低级钩子，行为不可预测（排查时被两个实例的日志互相覆盖坑过） |
| **L5** | **改了 `otiagent2.cs` 必须 bump `BuildTag` 并重新部署** | `csc /out:` 覆盖**正在运行**的 exe 会**静默失败** → 一直在跑旧二进制（为此白查半天） |
| **L6** | **麒麟侧重编译后必须重打 KySec 标签**（`label-kysec.sh`），否则图片剪贴板退化成"一串路径" | KySec 禁止未信任二进制读受保护目录（errno 13） |
| **L7** | **被驱动侧永不 grab、永不接管**（只监听 + 撞边交还） | 两侧互相抢控制权，刚过去就被弹回 |
| **L8** | **任何"交出去"的能力都必须有对称的"收回来"**：回程判据 + 急救热键 + 看门狗，三者缺一不可 | "指针回不来"是这个项目最贵的故障，用户只能拔线 |
| **L9** | **不得用宽泛正则批量删除/禁用系统对象**；先打印清单（dry-run）再动手 | `OTi` 命中了 `Notificati**ons**`（误禁 5 个系统任务），`Link` 命中了我们自己的自启项 |
| **L10** | **捕获 SSH 输出时不得合并 stderr** | 麒麟登录横幅 `Kylin V10 SP1` 在 stderr → 文本比对/md5/二进制解析全错 |
| **L11** | **杀进程用 `pkill -x` 或按 pid**，禁用 `pkill -f` | `pkill -f <pattern>` 会匹配到调用者自己的命令行 → 自杀 |
| **L12** | **Windows 文件只经 `re/windows/deploy.sh` 部署**（BOM/CRLF/解析自检/重启一条龙） | 手工 `cp` 会漏 BOM 或把 `.vbs` 写成 LF → 静默不执行 |
| **L13** | **动硬件/改协议/改钩子前先 `./gate.sh --pre`**；破坏性操作前先留证据（日志、进程、光标位置） | 90% 的"假失败"是前提没核对 |
| **L14** | **结论必须贴证据**：门禁报告 + 关键日志行（`km.log` / `/tmp/otikm.log`）。禁止"应该可以了" | 这个项目里"看起来好了"和"真的好了"差别极大 |
| **L15** | **绿色包不得依赖宿主已装的任何软件、不得写系统文件**：一切设备访问只经**一次显式提权**（sudo/pkexec），只允许写 `/tmp`、`$XDG_RUNTIME_DIR`、包自带日志目录 | 免安装是用户的硬要求；一旦依赖 xclip/udev/组，换台机器就"跑不起来但没人知道为什么" |
| **L16** | **对外发布的 Linux 二进制必须按声明基线构建**，并过 `gate.sh --portable` 的 GLIBC 需求/源码指纹/依赖白名单门禁；跨发行版容器矩阵能跑就跑，跑不了必须写明 | 开发机（glibc 2.39）编出来的二进制要求 `GLIBC_2.38`，拿到麒麟（2.31）直接起不来 —— 实测踩过 |
| **L17** | **任何时刻只能有一个主控（grab）**：角色必须由两端 $OTI_MSG_ROLE(10)$ 协商得出（或显式 $--role$），**禁止两侧各自静态为主控** | 现场事故：用户把键鼠换到另一台后，两侧同时是主控 → 帧管道双向全丢、剪贴板两边"未确认"，只能人工停一侧（$NOTES$ 第 46 轮） |

---

## 4. 标准工作流（按改动类型）

### 4.1 改 Linux 侧（`re/otilink/*.c|*.h`）

```bash
cd re/tools && ./gate.sh --pre
cd re/otilink && make -j4 all && ./coretest     # 先在开发机编过 + 状态机单测
cd re/tools && ./gate.sh --static                # 静态门禁
./gate.sh --deploy-kylin                         # 同步 → 麒麟编译 → KySec 标签 → 写标签戳
# 重启麒麟侧进程（改完不重启 = 跑的还是旧进程）：
ssh kylin@<麒麟IP> 'pkill -x otikm; cd ~/otilink && ./run-kylin.sh'   # 会自动选拓扑
./gate.sh --hw-km        # 碰了键鼠/交接：必跑
./gate.sh --hw-clip      # 碰了剪贴板：必跑
./gate.sh --hw-xfer      # 碰了传输/分片：必跑（大改动加 500MB：--hw-xfer 524288000）
```

### 4.2 改 Windows 侧（`re/windows/otiagent2.cs` / `*.ps1`）

1. **文件格式**：`.ps1` 必须 UTF-8 **BOM**；`.vbs` 必须 **CRLF**（`deploy.sh` 会补/归一化，`doccheck.sh` 会检查）。
2. 改 `otiagent2.cs` → **bump `BuildTag`** → `./deploy.sh --restart-km` → 启动日志里确认新版本：
   ```bash
   grep -a 'build ' /mnt/c/Users/Public/km.log | tail -1     # 应显示新 BuildTag
   cd re/tools && ./gate.sh --static && ./gate.sh --stamp    # 更新源码指纹戳
   ```
3. 改 `otiagent.ps1` → `./deploy.sh --restart`（**不要**传 `-Inject`，那是旧键鼠路径，会与 HID 通道打架）。
4. 验证：`./gate.sh --hw-km` / `--hw-clip`。

### 4.3 改协议（`PROTOCOL.md` + `otiproto.c/h` + `otiagent.ps1`）

* 两端**必须同版本**：先改 `re/PROTOCOL.md`（记录线上格式与理由），再改代码，最后 `--full` 全档验收。
* 新增消息类型时：`oti_encode_* / oti_decode_*` 成对实现 + 在 `re/otilink/selftest_proto.c`（或 `coretest.c`）加断言。
* **厂商 XML 帧会共存于同一条帧管道**，解码失败时先判 `body[0] == 0x39` 再报错（现有代码已处理）。

### 4.4 只改文档

`./gate.sh --static` 就够（它会检查 `AGENTS.md` 里引用的 `re/...` 路径是否存在、权威文档是否齐全）。
**改了行为却不改文档 = 没改完**（见 §7 DoD）。

---

## 5. 门禁（gate）—— 什么时候必须跑哪一档

| 改动内容 | 必跑档位 | 通过标准 |
|---|---|---|
| 文档 / 注释 | `--static` | PASS=all，FAIL=0 |
| Linux C 代码（不碰键鼠/剪贴板/传输核心） | `--static` + `--deploy-kylin` | `make` 无错、`coretest` 全过 |
| 键鼠共享 / 交接 / 回程 / 钩子 / 令牌（**拓扑 A：键鼠在 Windows**） | `--static` + `--hw-km` | `hwtest` PASS=12 FAIL=0；`km.log` 出现 `LOCAL(startup) → REMOTE → F24 token seen → LOCAL(peer token)` |
| 键鼠回程（**拓扑 B：键鼠在麒麟**，主控端本地判定） | `--static` + `--hw-kmb` | `kmbret` FAIL=0（小步外推能回程 + 落点同步 + 入口边 park + by-id 抓取 + 麒麟侧抑制缓冲补发）；Windows 侧「真收到 Ctrl/C」两条按 §61.2 线缆固件丢包**只算 WARN**（见 NOTES §63.9）；`coretest` 全过 |
| 键鼠卡顿（"过几秒卡一下" = HID 写被设备级操作堵住） | `--static` + `--hw-lat` | `hidlat` PASS：与 otikm 并发时 HID 写最大间隔 ≤ 200ms、`SEND_FAIL=0` |
| 键盘独占 / 双重输入（"我在 Windows 上打字，麒麟也在同步打字"） | `--static` + `--hw-kbdexcl` | `kbdexcl` FAIL=0：接管期间虚拟键盘探针 **BUSY**（本机桌面收不到按键）、回程/热键后 **FREE**（能力收得回来），日志无「键盘不独占」 |
| 线缆拔插自愈（拔插后 otikm 不崩 + 自发现重开） | `--static` + `--deploy-kylin` + `--replug` | `replugreg` FAIL=0（N 轮拔插后 otikm 存活、传输重新就绪、无 segfault） |
| 剪贴板（任一格式） | `--static` + `--hw-clip` | `clipreg` PASS=17 FAIL=0 |
| 大文件 / 分片 / 位图重传 | `--static` + `--local` + `--hw-xfer` | `xferlocal` 通过；`xferreg` PASS=12 FAIL=0 |
| 跨链路（协议 / 拓扑 / 厂商相关 / 收尾交付） | `--full` | 全档 FAIL=0 |
| 部署到新机器 / 复现故障 / 动厂商程序 | 先 `--pre` | `envcheck` FAIL=0（WARN 需逐条解释） |
| 绿色包 / 可移植二进制 / 打包脚本（`re/portable/**`） | `--static` + `--portable`（**产物不入库**：先跑 `re/portable/build.sh`，否则该档会明确报「没有产物目录」；改到 otikm 时按上面各行选真机档） | 产物源码指纹与当前源码一致、GLIBC 需求 ≤ 声明基线、依赖只有 glibc、包内清单齐全、Windows 脚本过 PowerShell 真解析 |

**门禁失败不许"绕过"**。两种合法处理，二选一：

1. **修好**；
2. 写进 `re/tools/known-issues.txt`（格式 `<阶段slug> = 现象 + 影响面 + 为什么暂不阻塞 + 证据指向`）——
   门禁会把它显示成 `KNOWN`（**不计入通过**，汇总里单独计数），并在每次运行时把原因打出来。
   修好后**必须删掉那一行**，否则门禁会一直替你遮掩。新增失败**不会**自动变成 KNOWN —— 改这个文件是
   一个需要人确认的动作。

当前登记：`tcptest`（TCP+keepalive 残余错位，只影响本机网络联调路径；见 `re/NOTES.md §46.4`）。

---

## 6. 验证脚本：新增与登记

* 复现一个 bug 或加一条保障 → **写成脚本**，别只写进聊天记录。
* 复制模板：`cp re/tools/template-verify.sh re/otilink/<你的脚本>.sh && chmod +x`，遵守三条约定：
  1. 每项检查 `ok/bad/warn` 一行；结束时 `summary`；
  2. **退出码 = 失败项数**（唯一可信判定）；
  3. 硬件相关先经 `envcheck`。
* **必须在 `re/tools/gate.sh` 里加一档调用它**，并在本文件 §5 表格里登记 —— `doccheck.sh` 会检查
  `clipreg.sh / xferreg.sh / hwtest.sh / kmbret.sh / kbdexcl.sh / hidlat.sh / xferlocal.sh / coretest / portablecheck.sh` 是否被 gate 注册，漏注册直接 FAIL。

**常用验证工具**（都在 `re/` 下）：

| 工具 | 侧 | 用途 |
|---|---|---|
| `otilink/clipreg.sh` | 双向 | 剪贴板真机回归（文本/大文本/图片/文件 × 双向，17 项，不动光标） |
| `otilink/xferreg.sh [N]` | 双向 | 大文件真机回归（缺省 64MB，12 项） |
| `otilink/hwtest.sh` | 双向 | 键鼠真机回归（12 项，**会动真实光标**；**只覆盖拓扑 A**） |
| `otilink/kmbret.sh` | 麒麟主控 | **拓扑 B 回程手势**真机回归（22 项 = 20 硬断言 + 2 WARN）：`--sim-input` 合成手势经真实线缆 → 小步（2px/次）外推必须回程、落点同步、入口边 park、by-id 抓取、抑制缓冲补发；Windows 侧「真收到 Ctrl/C」受 §61.2 固件丢包影响**只 warn**；会短暂停/恢复麒麟 otikm（`gate.sh --hw-kmb`） |
| `otilink/hidlat.sh [秒数] [ms]` | 麒麟主控 | **键鼠卡顿**回归（20 项左右）：与 otikm **并发**量 HID 写间隔，>200ms 即 FAIL（`gate.sh --hw-lat`）—— 真机案例：保活 dummy 帧 64KB 单次堵设备 ~1.3s |
| `otilink/kbdexcl.sh` | 麒麟主控 | **键盘独占**真机回归（拓扑 B，约 40s）：用 `uisim` 造 uinput 虚拟键鼠给 otikm 抓（不碰真键鼠），探针按 **EVIOCGRAB 排他性**判定 —— 接管期间必须 **BUSY**（本机桌面收不到按键 = 不双重输入）、热键回本机后必须 **FREE**（L8）；会短暂停/恢复麒麟 otikm（`gate.sh --hw-kbdexcl`） |
| `otilink/replugreg.sh [N]` | 双向 | 线缆拔插自愈回归：用 USB unbind/bind **模拟拔插** N 轮（缺省 3），不需人手拔线；修复前二进制第 2 轮必崩 |
| `otilink/xferlocal.sh` | 本机 | 大文件本机回归（AF_UNIX，无硬件，秒级） |
| `otilink/coretest` | 本机 | 交接/回程状态机单测（45 项，改 core 必跑） |
| `otilink/hidprobe.c` | 麒麟 | HID 方向探针：`./hidprobe /dev/sg3 1 10`（鼠标）/ `… 2 3 0x87`（F24） |
| `windows/kbdprobe2.ps1` | Windows | 独立低级键盘钩子探针（打印 `vk=`）——判定钩子链死没死 |
| `windows/detect-keys.ps1` | Windows | 会话级按键状态轮询（不依赖钩子） |
| `windows/sendkey.ps1` | Windows | 合成按键（对照输入） |
| `windows/pushright.ps1` / `pushleft-synth.ps1` | Windows | 复现/自动测"交接"与"回程"手势 |
| `portable/build.sh` + `otilink.sh` | 绿色包 | **免安装**：按声明基线组装绿色包（麒麟编 Linux 二进制 + csc 编 Windows 载荷 + 打包） |
| `tools/portablecheck.sh` | 门禁 | 绿色包门禁：源码指纹新鲜度 / GLIBC ≤ 基线 / 依赖白名单 / 清单 / PowerShell 真解析 |
| `tools/psparse.ps1` | 门禁 | 用 PowerShell 自己的解析器查 `.ps1` 语法（BOM 丢了会"假语法错"，这个能抓到） |

---

## 7. 完成定义（DoD）—— 交付时必须包含

1. **门禁报告**：`/tmp/otilink-gate.json`（`./gate.sh --report` 的输出）+ PASS/FAIL 计数。
2. **证据行**：从日志里贴出**能证明行为**的原文（例：`km.log` 的 `[hh:mm:ss] REMOTE …` 与
   `[hh:mm:ss] LOCAL (peer token (F24))`；麒麟侧 `撞边(0,y) → 通知对端收回控制权` + `回程令牌 F24 已发（消息泵线程，rc=0）`）。
3. **文档同步**：
   * 新事实/新坑 → `re/NOTES.md`（**新开一节，带日期与证据**）；
   * 用户可操作的步骤/排障 → `re/RUNBOOK.md`；
   * 状态变化（拓扑、文件表、已知限制）→ `re/HANDOFF.md`；
   * 规范变化（新硬约束/新门禁）→ **本文件**。
4. **未验证的东西必须标注**（"未复现/待观察"），不许混在结论里当成功。
5. **副作用清单**：动过哪些进程/文件/注册表/计划任务，以及**怎么回滚**。

---

## 8. 排障决策树（"回不来 / 键鼠失灵 / 剪贴板不通"）

```
症状：指针移到麒麟回不来
├─ ① km.log 有 REMOTE 吗？
│    └─ 没有 → 转发不在我们手里：厂商程序在跑？（vendor-off.vbs）→ 再跑 --pre
├─ ② km.log 有 `kbd: F24 token seen` 吗？
│    └─ 没有 → 钩子哑了：看 `hooks reinstalled … tid=` 是否**只有同一个线程**（L2）
├─ ③ 麒麟 /tmp/otikm.log 有 `撞边…` + `回程令牌 F24 已发…rc=0` 吗？
│    └─ 没有 → 被驱动侧判据：DISPLAY 是否有效（真实光标读得到）、线缆 HID 鼠标是否在位
└─ ④ 分层对照：windows/kbdprobe2.ps1（钩子）+ otilink/hidprobe（线缆方向）
     └─ 钩子看得到、线缆方向通 → 问题在我们的模式判定；否则定位到具体一层
```

> ⚠️ **一症多因**：上面这棵树只覆盖「回不来」。「指针回不来」在历史上有 **5 个**不同真因、
> 「键盘有问题」有 **4 个**（`NOTES §44/44.7/45/59/60/61/63`）——**别按记忆改代码**。
> 完整索引（8 类症状 × 病因 × 一句可跑的探针 + 必须成立的不变量）见 **`RUNBOOK §19`**：
> 先定层，再动手，改完按 §5 矩阵跑对应门禁。

**用户侧急救（背下来，卡死时先做这个）**

| 场景 | 动作 |
|---|---|
| 指针卡在麒麟 | Windows 键盘 **`Ctrl+Alt+→`**（强制回 LOCAL）；或 `cd re/windows && ./deploy.sh --restart-km` |
| 键盘在麒麟侧 | 麒麟键盘 **`Ctrl+Alt+←`**（拉回本机）；`Ctrl+Alt+Space`（切换）；`Ctrl+Alt+L`（锁定） |
| 完全没反应 / 两侧都不动 | **拔插对拷线**（重置线缆会话）→ 然后 `./deploy.sh --restart-km` + 重启麒麟 `otikm` |
| 怀疑厂商在抢 | `cmd.exe /c "cscript //B //NoLogo C:\\Users\\Public\\vendor-off.vbs"`（UAC 一次） |

---

## 9. 已知坑速查（改代码前扫一眼）

| 坑 | 规避 |
|---|---|
| `csc` 覆盖运行中的 exe **静默失败** | 先 `Stop-Process otiagent2` 再编译（`deploy.sh --restart-km` 已内置） |
| `.ps1` 无 BOM / `.vbs` 用 LF | `deploy.sh` 自动修；`doccheck.sh` 拦截 |
| `[int]((n+7)/8)` 在 PS 里是**四舍五入** | 用 `[Math]::Floor`（位图长度曾因此静默失效） |
| `0xFFFFFFFF` 变 Int32 `-1` | 用 `0xFFFFFFFFu` |
| `DragQueryFileW` 在 **shell32.dll**（不是 user32） | 文件剪贴板取路径失败先查这个 |
| WinForms `Cursor.Position` 受 DPI 虚拟化 | 读光标用 `GetCursorPos`（`re/tools/lib.sh` 的 `wcur`） |
| 自匹配进程过滤会把自己杀掉 | 停进程时排除 `$PID`（`deploy.sh --restart` 已处理） |
| WSL 里 `Start-Process` 会继承句柄导致挂死 | 用 `cscript` + `.vbs`（`WScript.Shell.Run` 完全脱离） |
| 麒麟 `python3` 受 KySec 限制 / 本地二进制读不了 `~/文档` | 重编译后打 KySec 标签（L6） |
| 大文件分片：整文件 CRC 必须**预先算** | 增量算会被重传片重复计入（历史 bug） |
| 发送失败空转 100% CPU | 退避（20ms）+ 连续失败阈值（20/200） |
| 剪贴板突发 <2s 会丢一条 | 已知限制：ACK+重发只能救"未被覆盖"的那条 |
| 拔插后 `eventN` 会被重新分配（键盘的 event7 可能变成 "System Control" 接口） | 抓取必须用 **by-id 稳定路径**（`oti_input_stable_path()`）；重开要**校验能力位**（`oti_capture.kind`），不符就按类别重新发现（L 见 `NOTES §60`） |
| 热插拔判据只看"有没有键鼠"（>0）会漏掉**新插的第二个设备** | 按**个数**判（`nlocal != a->local_count`）→ `cap_rescan` → 重扫抓取集合（真机："先插鼠标再插键盘"键盘永远用不了） |
| 任何**大块/会失败的设备写**都会堵住 HID（单队列设备）→ "键鼠每几秒冻结一下" | 保活 dummy 帧（64KB）只在**链路真空闲 >60s** 时发，且必须拿 `otilink_cable_io_lock()`；判据用 `gate.sh --hw-lat`（hidlat 量 maxgap） |
| **健康判据不能挂到与"这条能力"无关的通道上**：纯键盘独占曾用"帧管道健康"当判据，而 HID 直发模式（对端零软件）下帧管道**本来就该静默** → 纯键盘永远不独占 → 接管期间**两机同时打字** | 判据必须对着**这条能力真正走的通道**（`otikm.c` 的 `km_path_ok()`：HID 直发看 HID 写失败计数，协议模式才看帧管道）；且"维持独占"要周期性幂等重放（判据会变，见 `gate.sh --hw-kbdexcl`） |
| 线缆"假光盘"（LUN1/CD）读不出来会让 usb-storage 卡 D 状态、拖卡整机 | **不要解绑 usb-storage 接口**（会连带杀掉 `/dev/sgN` 传输通道，otikm 只剩 rc=-19）；用 `usb-storage.quirks=0ea0:2213:s`（SINGLE_LUN，只枚举 LUN0），`install-kylin.sh` 已装，见 NOTES §62 |

---

## 10. 禁止事项（Never）

1. **不要**同时运行厂商程序与我们的 agent（L1）。
2. **不要**在 Worker/捕获线程里装钩子（L2）或直接写设备（L3）。
3. **不要**手工 `cp` 到 `C:\Users\<你的用户名>\otilink\`（L12）。
4. **不要**在没 bump `BuildTag` 的情况下编译部署 `otiagent2.cs`（L5）。
5. **不要**把 ssh 的 stderr 合并进捕获值（L10）；**不要**用 `pkill -f`（L11）。
6. **不要**在没有 `--pre` 体检的情况下动硬件/协议/钩子（L13）。
7. **不要**用宽泛正则批量删/禁系统对象（L9）。
8. **不要**在报告里写"应该/大概/可能好了"（L14）。
9. **不要**把任何凭据写进仓库或日志：`KY` / `KY_PASS` 一律走环境变量（§0），内网地址与用户名用占位符。
10. **不要**在用户正在用机器时跑 `--hw-km`（会抢光标 ~90s）——先说明再跑。

---

## 11. 会话交接

* 上下文将满或任务切换：更新 `re/HANDOFF.md`（一页：现状 / 拓扑 / 文件表 / 本轮结论 / 下一步）。
* 每轮结束把**新事实与新坑**写进 `re/NOTES.md`（新开 `## NN 第 N 轮：…`，含证据与失败结论更正；
  被推翻的旧结论要**显式标注**，不要让下一个会话再踩一次）。
* 用户可操作的命令/排障步骤 → `re/RUNBOOK.md` 对应小节。
* 规范/门禁变化 → 本文件（并在 `re/HANDOFF.md` 里提一句"规范已更新"）。
