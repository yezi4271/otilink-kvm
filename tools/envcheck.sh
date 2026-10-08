#!/bin/bash
# envcheck.sh —— 门禁 0：施工前"体检"（环境 / 拓扑 / 安全前提）
#
# 为什么需要它：这套系统跨三台"机器"（WSL、Windows、麒麟）+ 一根线缆，
# 90% 的"假失败"和"说不清谁在转发"都源于前提没核对：
#   * 厂商程序还活着（和我们抢链路）→ 症状是"厂商在转发、我们的 agent 从没 REMOTE"
#   * 两个 otiagent2 实例（多套低级钩子）→ 行为不可预测
#   * 钩子被装到非消息泵线程（tid 不一致）→ 整套钩子哑掉（"指针回不来"）
#   * 麒麟侧 DISPLAY 无效（被驱动侧读不到真实光标）→ 撞边交还失效
#   * 重编译后没重打 KySec 标签 → 图片剪贴板变"一串路径"
#
# 用法： ./envcheck.sh [--topology driven|master|any] [--allow-no-agent]
# 退出： 0=全过；>0=失败项数（2=fatal）
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

TOPO=${TOPO:-any}
ALLOW_NO_AGENT=0
while [ $# -gt 0 ]; do
    case "$1" in
        --topology) TOPO=${2:-any}; shift 2 ;;
        --allow-no-agent) ALLOW_NO_AGENT=1; shift ;;
        *) die "未知参数 $1" ;;
    esac
done

# ------------------------------------------------------------------ 本机
stage "0/4 本机（WSL / 开发侧）"
command -v powershell.exe >/dev/null 2>&1 && ok "powershell.exe 可用（能驱动 Windows 侧）" \
    || bad "powershell.exe 不可用（必须在 WSL 里跑）"
if [ -d /mnt/c/Users/Public ]; then
    if touch /mnt/c/Users/Public/.otilink-wtest 2>/dev/null; then
        rm -f /mnt/c/Users/Public/.otilink-wtest; ok "/mnt/c 可写（部署/日志路径可用）"
    else bad "/mnt/c 不可写（部署会失败）"; fi
else bad "/mnt/c 不存在（不在 WSL？）"; fi
for s in "$OTI_DIR/Makefile" "$WIN_DIR/deploy.sh" "$WIN_DIR/otiagent2.cs"; do
    [ -f "$s" ] && ok "存在 $(basename "$s")" || bad "缺文件 $s"
done
command -v gcc >/dev/null 2>&1 && ok "gcc 可用（本地编译门禁）" || warn "没有 gcc：静态门禁只能做语法/文档检查"

# ------------------------------------------------------------------ Windows
stage "1/4 Windows 侧（主控或在线）"
CW=$(pwsh_cmd '(Get-PnpDevice -Status OK -EA SilentlyContinue | Where-Object { $_.InstanceId -match "0EA0" } | Measure-Object).Count' | tail -1 | tr -d ' ')
[ "${CW:-0}" -ge 1 ] && ok "对拷线在场（VID_0EA0 设备 ${CW} 个）" || bad "Windows 上看不到对拷线（VID_0EA0）：线缆没插/没枚举"

V=$(count_vendor)
if [ "${V:-1}" -eq 0 ]; then ok "厂商程序未运行（链路独占 ✓）"
else
    bad "厂商程序还在跑（$V 个）：MacKMLink/LinkEngKM 会和我们抢链路 → 跑 re/windows/vendor-off.vbs"
fi

A=$(count_proc otiagent2)
case "$TOPO" in
    driven)                       # 键鼠在 Windows：Windows 必须是主控
        [ "${A:-0}" -eq 1 ] && ok "otiagent2 恰好 1 个（主控端）" \
            || bad "otiagent2 数量=$A（driven 拓扑必须恰好 1 个）" ;;
    master)                       # 键鼠在麒麟：Windows 只当被驱动端，可不跑
        [ "${A:-0}" -le 1 ] && ok "otiagent2 数量=$A（master 拓扑下 ≤1，可停可留）" \
            || bad "otiagent2 数量=$A（多实例会挂多套钩子）" ;;
    any)
        # 第 46 轮起角色是**协商**出来的（麒麟 --auto 按本机键鼠 + ROLE 交换定角色），
        # 所以 Windows 侧 0 个 agent 有两种合法含义：
        #   * 拓扑 B：麒麟主控 → Windows 是零软件接收端，0 个就是对的；
        #   * 拓扑 A：Windows 主控 → 必须是 1 个。
        KROLE=$(kssh 'grep -a -m1 -o "角色引导(auto): [^（]*" /tmp/otikm.log 2>/dev/null' | tail -1)
        case "${A:-0}:$KROLE" in
            1:*)   ok "otiagent2 恰好 1 个（拓扑 A：Windows 主控）" ;;
            0:*主控*) ok "otiagent2 0 个（拓扑 B：麒麟主控 → Windows 零软件接收端，正常）" ;;
            0:*)   { [ "$ALLOW_NO_AGENT" -eq 1 ] && warn "otiagent2 数量=0（已允许）" \
                       || bad "otiagent2 数量=0 且麒麟侧无主控记录：没人转发（拓扑 A 需要 1 个）"; } ;;
            *)     bad "otiagent2 数量=$A（应为 0 或 1）" ;;
        esac ;;
esac

C=$(count_clip)
[ "${C:-0}" -ge 1 ] && ok "剪贴板代理在跑（${C} 个）" || bad "剪贴板代理没跑：cd re/windows && ./deploy.sh --restart"

BUILD=$(winlog | grep -ao '\[build [^]]*\]' | tail -1)
[ -n "$BUILD" ] && ok "键鼠代理版本标记 $BUILD（csc 覆盖运行中的 exe 会静默失败，必须认这个）" \
                || warn "km.log 里没有 [build …]：跑的可能是不带标记的旧二进制"

# 钩子线程一致性：同一次运行里所有重装记录的 tid 必须相同
TIDS=$(winlog | awk '/otiagent2 up\./{buf=""} {buf=buf $0 "\n"} END{print buf}' \
       | grep -o 'reinstalled #[0-9]* .*tid=[0-9]*' | grep -o 'tid=[0-9]*' | sort -u | tr '\n' ' ')
NT=$(echo "$TIDS" | wc -w)
case "$NT" in
    0) warn "km.log 本轮没有 hooks reinstalled 记录（未进过 REMOTE 属正常）" ;;
    1) ok "钩子重装线程唯一（$TIDS）—— 低级钩子必须装在消息泵线程上" ;;
    *) bad "钩子出现在多个线程上（$TIDS）：低级钩子装错线程 = 整套钩子哑掉（键盘/令牌全废）" ;;
esac

# ------------------------------------------------------------------ 麒麟
stage "2/4 麒麟侧（被驱动或主控）"
if kssh 'echo up' | grep -q up; then ok "SSH 可达 $KY"; else
    bad "SSH 不可达 $KY（检查网络/密码 KY_PASS）"; summary "环境体检" ; exit $((FAIL > 0))
fi
# 不要写死 /dev/sgN：拔插/换口/重启后设备号会变（实测 sg2→sg3→sg4→…→sg7）。
# 判"线缆在不在"应按 USB VID/PID，而不是某个固定的 sg 号。
CABLE_N=$(kssh 'grep -l 0ea0 /sys/bus/usb/devices/*/idVendor 2>/dev/null | wc -l' | tr -d ' ')
case "${CABLE_N:-}" in ''|*[!0-9]*) CABLE_N=0 ;; esac
[ "$CABLE_N" -ge 1 ] && ok "线缆已枚举（USB 0ea0:2213）" || bad "麒麟侧看不到线缆 0ea0:2213：没插/没枚举"
# 传输通道：线缆 MSC 接口的 /dev/sgN 必须在。反例（2026-09-22 真机踩过）：用 udev 解绑
# usb-storage 来"隐藏假光盘" → /dev/sgN 一起消失，otikm 永远建不起传输（重开传输失败 /
# recv rc=-19，见 NOTES §62）。不要写死 sg 号：从 sysfs 往上层走，认 idVendor=0ea0。
CSG=$(kssh 'for d in /sys/class/scsi_generic/sg*; do [ -e "$d" ] || continue; b=$(basename "$d"); u=$(readlink -f "$d"); while [ -n "$u" ] && [ "$u" != "/" ]; do if [ -f "$u/idVendor" ] && grep -q 0ea0 "$u/idVendor" 2>/dev/null; then echo "/dev/$b"; break; fi; u=$(dirname "$u"); done; done' | head -1 | tr -d ' ')
[ -n "$CSG" ] && ok "线缆 MSC 通用设备在位（$CSG）" \
    || bad "线缆 MSC 没有 /dev/sgN（存储接口被解绑/未枚举）→ 传输建不起来；查 99-otilink-no-storage.rules 之类（NOTES §62）"
NK=$(kstatus | wc -w)
[ "$NK" -eq 1 ] && ok "otikm 恰好 1 个（pid $(kstatus)）" || bad "otikm 数量=$NK（应为 1）"
# otikm 的启动方式有两种：安装版（run-kylin.sh：--transport/--inject/--return-on-edge）
# 与绿色包（--auto --role master|slave）。两种都要能认出来，否则会误报"参数异常"。
KCMD=$(kssh 'ps -ef | grep "[o]tikm" | grep -v grep | head -1')
KMODE=""
case "$KCMD" in
    *--return-on-edge*|*"--role slave"*)  KMODE=slave ;;
    *--inject*|*"--role master"*)         KMODE=master ;;
esac
if [ -z "$KMODE" ]; then            # 兜底：绿色包会在日志里写角色判定结果
    kssh 'grep -q "被驱动侧 slave" /tmp/otikm.log' >/dev/null 2>&1 && KMODE=slave
    kssh 'grep -q "主控端 master" /tmp/otikm.log' >/dev/null 2>&1 && KMODE=master
fi
case "$KMODE" in
    slave)
        ok "麒麟=被驱动侧（--return-on-edge 或 绿色包 --role slave）"
        kssh 'DISPLAY=:0 xdotool getmouselocation >/dev/null 2>&1' && ok "真实光标可读（DISPLAY 有效：撞边交还的前提）" \
            || bad "读不到真实光标（DISPLAY/XAUTHORITY 无效）→ 撞边交还会失效，见 RUNBOOK §14"
        # 线缆 HID 鼠标在不在位：安装版按 by-id 命名看，绿色包按"设备分类"选（日志里有"抓取设备 /dev/input/eventN"）
        if kssh 'ls /dev/input/by-id/*Android+Mac*event-mouse >/dev/null 2>&1' >/dev/null 2>&1 \
           || kssh 'grep -q "抓取设备 /dev/input/event" /tmp/otikm.log' >/dev/null 2>&1; then
            ok "线缆 HID 鼠标设备在位（被驱动侧的输入源）"
        else
            bad "看不到线缆 HID 鼠标设备（by-id *Android+Mac*event-mouse，或日志里的抓取设备）"
        fi
        [ "$TOPO" = master ] && warn "拓扑参数是 master，但麒麟实际跑的是被驱动侧" || true ;;
    master)
        ok "麒麟=主控端（--inject 或 绿色包 --role master）"
        LOC=$(kssh 'ls /dev/input/by-id/* 2>/dev/null | grep -vc "Android+Mac"')
        [ "${LOC:-0}" -ge 1 ] && ok "本机键鼠设备在位（${LOC} 个非线缆设备）" \
            || bad "主控模式但看不到本机键鼠设备（键鼠真的插在麒麟上吗？）"
        [ "$TOPO" = driven ] && warn "拓扑参数是 driven，但麒麟实际跑的是主控端" || true ;;
    *) bad "无法判断 otikm 模式（未运行或参数异常）" ;;
esac
kssh 'DISPLAY=:0 xclip -selection clipboard -o >/dev/null 2>&1; echo y' | grep -q y \
    && ok "xclip 可用（剪贴板回归需要）" || warn "xclip 不可用：剪贴板类回归会失败"
if kssh 'test -f ~/otilink/.kysec-stamp' >/dev/null 2>&1; then
    NEWER=$(kssh 'test ~/otilink/otikm -nt ~/otilink/.kysec-stamp && echo stale || echo fresh')
    [ "$NEWER" = fresh ] && ok "KySec 标签是最新的" \
        || warn "otikm 比 KySec 标签新（重编译后没重打标签）→ cd ~/otilink && sudo ./label-kysec.sh"
else warn "麒麟侧没有 KySec 标签戳：跑 ./gate.sh --deploy-kylin（它会重打标签并写戳）"; fi

# ------------------------------------------------------------------ 拓扑一致性
stage "3/4 拓扑一致性（谁在转发）"
case "$KMODE:$A" in
    slave:1)  ok "Windows 主控 + 麒麟被驱动：本仓库当前验证过的拓扑" ;;
    slave:0)  bad "麒麟被驱动，但 Windows 没有主控代理 → 没人转发（键鼠会卡在麒麟侧）" ;;
    master:1) warn "麒麟是主控，Windows 的 otiagent2 还在跑：它 LOCAL 时会放行、且线缆驱动的光标 1.5s 内不触发交接，通常无害；要绝对干净就停掉它" ;;
    master:0) ok "麒麟主控 + Windows 无 KM 代理：对端零软件拓扑" ;;
    *) ;;
esac
BUILD_SHA_FILE="$OTI_DIR/.csbuild.sha"
if [ -f "$BUILD_SHA_FILE" ]; then
    CUR=$(sha256sum "$WIN_DIR/otiagent2.cs" | cut -c1-16)
    OLD=$(cut -d' ' -f1 "$BUILD_SHA_FILE")
    [ "$CUR" = "$OLD" ] && ok "otiagent2.cs 与已部署版本指纹一致（$CUR）" \
        || warn "otiagent2.cs 改过（$OLD → $CUR）：按规范必须 bump BuildTag 并重新 deploy.sh --restart-km"
else warn "没有 .csbuild.sha 指纹戳：跑 gate.sh --stamp 建立（用于「改了代码但忘了改 BuildTag」门禁）"; fi

stage "4/4 日志可用性"
winlog | grep -q 'otiagent2 up' && ok "km.log 可读且有启动记录" || warn "km.log 里没有启动记录"
winclip | grep -q 'CLIP\|clip' && ok "clip.log 可读" || warn "clip.log 里没有内容"
kssh 'test -s /tmp/otikm.log' >/dev/null 2>&1 && ok "麒麟 /tmp/otikm.log 可读" || warn "麒麟 /tmp/otikm.log 为空或不存在"

summary "环境体检（envcheck）"
