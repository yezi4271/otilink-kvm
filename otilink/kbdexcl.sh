#!/bin/bash
# kbdexcl.sh —— 拓扑 B「键盘独占」真机回归：接管对端期间本地键盘**必须被独占**（防双重输入），
#               回程/热键后**必须释放**（L8：交出去的能力必须有对称的"收回来"）。
#
# 为什么需要它（2026-10-08 用户报障）：
#   现场原话："我在 windows 上打字，kylin 也在同步打字"。根因不是键盘转发坏了 ——
#   键鼠在麒麟（拓扑 B）、KM 走 **HID 直发**（对端零软件），而纯键盘的独占判据当时挂在
#   "帧管道健康"（per_seen + 5s 内有成功帧写）上。HID 模式下帧管道**本来就该静默**
#   （对端没有我们的软件，没人回帧）→ 纯键盘永远拿不到 EVIOCGRAB → 接管期间同一个按键
#   既发到 Windows、又落在麒麟本机桌面 = 双重输入。
#
# 判据为什么可信：EVIOCGRAB 是**排他**的 —— 已有客户端独占时，第二个客户端再 grab 返回
#   EBUSY；而"本机桌面"正是那个第二个读者，所以 BUSY ⇔ 这些按键不会落到麒麟本机。
#   本脚本把探针当"本机桌面的替身"：比"注入+读回"更直接，也不需要 X 侧抓键。
#
# 怎么做到不碰用户的真键鼠：用 uinput 造一对虚拟键鼠（otilink/uisim）交给 otikm 抓，
#   真键鼠完全不动；但 HID 包仍走**真实线缆**送到 Windows，所以"对端真收到键"也能断言。
#
# 时序（**触发文件驱动**，不用 sleep 赌 SSH 延迟 —— 第一版就是这么翻车的）：
#   ① 起 uisim（--wait-file：先造设备并**一直保活**，等触发文件才注入）
#   ② 起后台独占探针（长采样）
#   ③ 起合成 otikm（--capture 两个虚拟设备，--role master，HID 直发）→ 断言它真抓到了
#   ④ touch 触发文件 → uisim 注入：推右边缘 → 接管（应 BUSY）→ ctrl+alt+space 回本机（应又 FREE）
#   ⑤ 收样本断言 → 杀干净 → 恢复真机 otikm
#
# 用法： ./kbdexcl.sh        约 2 分钟（每次 kssh 握手 ~2.5s，脚本有 20+ 次）；
#                            会**短暂停止并恢复**麒麟真机 otikm（本次测试独占线缆）
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"

DUR=180
KLOG=/tmp/kbdexcl-otikm.log
KCON=/tmp/kbdexcl-console.log
KINJ=/tmp/kbdexcl-inject.out
KPROBE=/tmp/kbdexcl-probe.py
KRUN=/tmp/kbdexcl-runprobe.sh
KPOUT=/tmp/kbdexcl-probe.out
UOUT=/tmp/kbdexcl-uisim.out
GO=/tmp/kbdexcl-go
TMP=$(mktemp -d)
STOPPED=0

# ⚠️ 无论怎么退出都要把用户的真机 otikm 交回去（L8：不许把用户的键鼠留在测试里）
restore_otikm() {
    [ "$STOPPED" -eq 1 ] || return 0
    STOPPED=0
    kssh 'pkill -x otikm' >/dev/null 2>&1      # 合成实例先杀（否则它会按"拔插自愈"抓真键鼠）
    kssh 'pkill -x uisim' >/dev/null 2>&1
    sleep 1
    kssh 'cd ~/otilink && sg input -c "DISPLAY=:0 nohup ./run-kylin.sh > /tmp/rk.log 2>&1 &"' >/dev/null 2>&1
    for _ in $(seq 1 15); do
        [ -n "$(kssh 'pgrep -x otikm')" ] && break
        sleep 1
    done
    [ -n "$(kssh 'pgrep -x otikm')" ] && echo "  [ok] 真机 otikm 已恢复（run-kylin.sh）" \
                                      || echo "  [FAIL] 真机 otikm 没恢复：手动跑 ~/otilink/run-kylin.sh"
}
trap 'rm -rf "$TMP"; restore_otikm' EXIT

echo "=== 0/7 前提 ==="
khave 'echo up' && ok "麒麟 SSH 可达（$KY）" \
    || { bad "麒麟不可达：先跑 tools/gate.sh --pre（KY=… 见 lib.sh）"; summary "键盘独占回归（kbdexcl）"; exit 1; }
khave 'test -x ~/otilink/otikm' && ok "麒麟 ~/otilink/otikm 存在" \
    || bad "麒麟 ~/otilink/otikm 不存在（先 tools/gate.sh --deploy-kylin）"
# uisim 必须先存在且与源码同步：它是"可被独占的虚拟键鼠"唯一来源
if kssh 'cd ~/otilink && make uisim >/dev/null 2>&1 && test -x ./uisim' >/dev/null 2>&1; then
    ok "麒麟 uisim 已就绪（本次编译，与 uisim.c 同步）"
else
    bad "麒麟 uisim 编译失败（看 ~/otilink/uisim.c；需要 gcc）"
fi
khave 'test -w /dev/uinput' && ok "/dev/uinput 可写（kylin 在 input 组）" \
    || bad "/dev/uinput 不可写：跑 sudo sh ~/otilink/install-kylin.sh 并重新登录"
NC=$(pwsh_cmd '(Get-PnpDevice -PresentOnly -EA SilentlyContinue | Where-Object { $_.InstanceId -like "*VID_0EA0*" } | Measure-Object).Count' | tail -1 | tr -d ' ')
[ "${NC:-0}" -ge 1 ] && ok "Windows 侧对拷线在场（VID_0EA0 设备 $NC 个）" || bad "Windows 看不到对拷线（VID_0EA0）"
[ "$(count_vendor)" = 0 ] && ok "厂商程序未运行（L1：一条链路只能有一个主人）" || bad "厂商程序在跑，先跑 windows/vendor-off.vbs"

echo "=== 1/7 上传独占探针（EVIOCGRAB 排他性采样）==="
cat > "$TMP/probe.py" <<'PY'
#!/usr/bin/env python3
# EVIOCGRAB 排他性探针：设备已被别的客户端独占时，本客户端 grab 会 EBUSY。
# 本机桌面就是"另一个读者" —— BUSY ⇔ 这些按键不会落到本机（双重输入的直接判据）。
import errno, fcntl, os, sys, time
EVIOCGRAB = 0x40044590
kbd, mouse, secs = sys.argv[1], sys.argv[2], float(sys.argv[3])

def state(p):
    try:
        fd = os.open(p, os.O_RDONLY)
    except OSError as e:
        return "OPENFAIL(%d)" % e.errno
    try:
        try:
            fcntl.ioctl(fd, EVIOCGRAB, 1)
            fcntl.ioctl(fd, EVIOCGRAB, 0)
            return "FREE"
        except OSError as e:
            # ⚠️ EBUSY 是 **16**（11 是 EAGAIN）—— 硬编码错一次就整档假失败（真踩过）
            return "BUSY" if e.errno == errno.EBUSY else "ERR(%d)" % e.errno
    finally:
        os.close(fd)

t0 = time.time(); last = {}
while time.time() - t0 < secs:
    for name, p in (("kbd", kbd), ("mouse", mouse)):
        st = state(p)
        if last.get(name) != st:
            print("%6.2f %-5s %s" % (time.time() - t0, name, st), flush=True)
            last[name] = st
    time.sleep(0.15)
PY
cat > "$TMP/runprobe.sh" <<'SH'
#!/bin/sh
# 后台跑独占探针的壳：sg input 拿 input 组权限，输出重定向到 /tmp/kbdexcl-probe.out
exec sg input -c "python3 /tmp/kbdexcl-probe.py $1 $2 $3" > /tmp/kbdexcl-probe.out 2>&1
SH
B64=$(base64 -w0 "$TMP/probe.py"); B64R=$(base64 -w0 "$TMP/runprobe.sh")
kssh "printf '%s' '$B64' | base64 -d > $KPROBE && printf '%s' '$B64R' | base64 -d > $KRUN && chmod +x $KRUN && wc -c < $KPROBE" \
    | grep -qE '^[0-9]+' && ok "探针已就位（$KPROBE + $KRUN）" || bad "探针没写成功"

echo "=== 2/7 清场：杀残留虚拟键鼠 → 停真机 otikm → 造虚拟键鼠（保活等触发）==="
kssh 'pkill -x uisim' >/dev/null 2>&1
for _ in $(seq 1 10); do kssh 'grep -q otilink-uisim /proc/bus/input/devices' || break; sleep 0.5; done
khave 'grep -q otilink-uisim /proc/bus/input/devices' \
    && bad "上一轮的虚拟键鼠还在（残留会让路径选错 → 假失败）" || ok "没有残留的虚拟键鼠（uisim 已清干净）"
kssh 'pkill -x otikm' >/dev/null 2>&1
sleep 1
if khave 'pgrep -x otikm'; then bad "旧 otikm 没停掉（先手工 pkill -x otikm）"
else STOPPED=1; ok "真机 otikm 已停（退出时自动恢复）"; fi

kssh "rm -f /tmp/uisim.paths $KLOG $KINJ $KPOUT $UOUT $GO" >/dev/null 2>&1
# --wait-file：造好设备就**一直保活**，直到我们 touch $GO 才注入（消掉 SSH/启动延迟竞态）
kssh "cd ~/otilink && sg input -c 'nohup ./uisim --wait-file $GO --hold 600 > $UOUT 2>&1 &'" >/dev/null 2>&1
for _ in $(seq 1 30); do kssh 'test -s /tmp/uisim.paths' && break; sleep 0.5; done
MM=$(kssh 'sed -n 1p /tmp/uisim.paths')
KM=$(kssh 'sed -n 2p /tmp/uisim.paths')
[ -n "${MM:-}" ] && ok "虚拟鼠标：$MM" || bad "虚拟鼠标没造出来（看 $UOUT）"
[ -n "${KM:-}" ] && ok "虚拟键盘：$KM" || bad "虚拟键盘没造出来（看 $UOUT）"
[ -n "${KM:-}" ] || { summary "键盘独占回归（kbdexcl）"; exit 1; }
# 基线自检：探针必须能打开这两个节点（否则后面的 BUSY/FREE 结论全是假象）
BASE=$(kssh "sg input -c 'python3 $KPROBE $KM $MM 1'" 2>/dev/null | tr -d '\r' | tr '\n' ' ')
if echo "$BASE" | grep -q 'kbd *FREE'; then
    ok "基线：虚拟键盘可打开且未被独占（$BASE）"
else
    bad "探针打不开虚拟键盘（$BASE）—— 设备没造出来/编号不对"
fi

echo "=== 3/7 起后台探针 → 起合成实例 → 确认真的抓到虚拟键鼠 ==="
kssh "cd /tmp && nohup sh $KRUN $KM $MM 200 >/dev/null 2>&1 &" >/dev/null 2>&1
pwsh_file "$WIN_DIR/kbdprobe2.ps1" -Seconds 40 > "$TMP/kbdprobe.out" 2>&1 &
KPID=$!
kssh "cd ~/otilink && sg input -c \"DISPLAY=:0 nohup ./otikm --transport cable --capture $MM --capture $KM --role master --edges left,right --no-clipboard --sim-inject $KINJ --log $KLOG --duration $DUR > $KCON 2>&1 &\"" >/dev/null 2>&1
sleep 3
KP=$(kssh 'pgrep -x otikm | tail -1')
[ -n "${KP:-}" ] && ok "合成实例已启动（pid $KP）" || bad "合成实例没起来（看 $KCON）"
kssh "cat $KLOG" > "$TMP/otikm.log" 2>/dev/null
if grep -q '抓取设备 .*otilink-uisim' "$TMP/otikm.log"; then
    ok "合成实例已抓到虚拟键鼠（设备在位，接管才有意义）"
else
    bad "合成实例没抓到虚拟键鼠：$(grep -m1 'ERR' "$TMP/otikm.log" || echo "看 $KLOG")"
fi

echo "=== 4/7 触发注入（推右边缘接管 → CapsLock/LeftMeta → ctrl+alt+space 回本机）==="
kssh "touch $GO" >/dev/null 2>&1
sleep 22
kssh "cat $KPOUT" > "$TMP/probe.out" 2>/dev/null
kssh "cat $KLOG"  > "$TMP/otikm.log" 2>/dev/null
sed 's/^/      /' "$TMP/probe.out" | head -14

echo "=== 5/7 麒麟侧断言 ==="
KSEQ=$(grep ' kbd ' "$TMP/probe.out" 2>/dev/null || true)
[ -n "$KSEQ" ] && ok "探针有采样（$(echo "$KSEQ" | wc -l) 次状态变化）" \
              || bad "探针没有任何采样（python3/权限/设备路径？看 $KPOUT）"
FIRST=$(echo "$KSEQ" | head -1 | awk '{print $3}')
LAST=$(echo "$KSEQ"  | tail -1 | awk '{print $3}')
[ "${FIRST:-?}" = FREE ] && ok "基线：本地态键盘未被独占（探针本身有效）" \
                        || warn "基线不是 FREE（${FIRST:-?}）：探针或设备状态可疑"
grep -qE ' kbd +BUSY' "$TMP/probe.out" 2>/dev/null \
    && ok "**接管期间键盘已被独占**（本机桌面收不到这些按键 = 不再双重输入）" \
    || bad "接管期间键盘仍未被独占 —— 双重输入回归了！（用户症状：两机同时打字）"
[ "${LAST:-?}" = FREE ] && ok "回到本机后键盘独占已释放（L8：能力收得回来，不会把用户锁死）" \
                        || bad "回到本机后键盘仍被独占（用户会被锁死在本地无法打字）"
grep -qE ' mouse +BUSY' "$TMP/probe.out" 2>/dev/null && ok "鼠标同样被独占（原有行为未回退）" \
                                                  || warn "没探到鼠标 BUSY（时序太紧？不影响键盘结论）"

grep -q '已独占 otilink-uisim-kbd' "$TMP/otikm.log" && ok "独占来自 otikm（日志：「已独占 otilink-uisim-kbd」）" \
    || bad "otikm 日志里没有「已独占 otilink-uisim-kbd」（真独占了吗？）"
grep -q '键盘不独占' "$TMP/otikm.log" && bad "日志出现「键盘不独占」（判据又挂到无关通道上了？）" \
                                      || ok "没有出现「键盘不独占」（判据挂在 KM 载体上）"
grep -q 'HID 模式：指针交给对端' "$TMP/otikm.log" && ok "状态机确实交出去过（断言有效）" \
    || bad "没有「指针交给对端」：手势没触发接管，本次断言不成立"
grep -q 'HID 模式：指针拉回本机' "$TMP/otikm.log" && ok "热键回程发生过（释放断言有效）" \
    || warn "没有回程记录：释放那条断言可能是「本来就没独占」导致的假通过"
# 探针每 150ms 会短暂独占一次做判定，极小概率与 otikm 的 grab 撞上 → 只 warn（"维持独占"会自愈）
grep -q 'ERR 独占 .*失败' "$TMP/otikm.log" && warn "有 EVIOCGRAB 失败告警（探针撞车或权限/被他人占用）" \
                                            || ok "没有独占失败告警"

echo "=== 6/7 Windows 侧断言（HID 键盘通道已知丢包，这里只做 warn）==="
wait $KPID 2>/dev/null || true
grep -q 'hook=ok' "$TMP/kbdprobe.out" 2>/dev/null && ok "Windows 键盘钩子探针已挂上" \
                                                  || warn "键盘探针没挂上（钩子看不到按键，本条不可信）"
if grep -q 'vk=0x14' "$TMP/kbdprobe.out" 2>/dev/null; then
    ok "Windows 收到 CapsLock（vk=0x14，独占期间按键仍真送到对端）"
else
    warn "Windows 没看到 CapsLock：线缆 HID 键盘接口会丢报表（NOTES §61）—— 本条不计失败"
fi

if [ "$FAIL" -gt 0 ]; then
    mkdir -p /tmp/kbdexcl-artifacts
    cp "$TMP/probe.out" "$TMP/otikm.log" "$TMP/kbdprobe.out" /tmp/kbdexcl-artifacts/ 2>/dev/null
    info "失败现场已保存到 /tmp/kbdexcl-artifacts/"
fi

echo "=== 7/7 恢复真机 otikm（run-kylin.sh，自动协商角色）==="
restore_otikm
summary "键盘独占回归（kbdexcl）"
