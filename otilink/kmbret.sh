#!/bin/bash
# kmbret.sh —— 拓扑 B（键鼠插在**麒麟**）"移到 Windows 回不来"回归：回程手势 + 修饰键补发
#
# 为什么需要它：hwtest.sh 只覆盖拓扑 A（键鼠在 Windows、Windows 主控）。拓扑 B 下
# 指针移到 Windows 后，回程判据在**主控端（麒麟）本地**：用户在 Windows 屏幕上
# "继续往外推"。这条判据曾经只看**单次位移**是否越界（真机 bug：鼠标事件每次只有
# 1~3 个计数，坐标每次被夹回边界 → 推半天回不来，2026-09-21 用户报障）。
#
# 本脚本用 otikm 的 --sim-input 合成手势（不必人手推鼠标，也不抢本机键鼠）：
#   推出去 → 在对端继续推（**小步 2px**）→ 必须回程；
# 同时验证 HID 路径会补发被抑制的修饰键（Ctrl+C 这类组合能到对端）。
# 全程走**真实线缆**（HID 包真送到 Windows），并断言 Windows 侧看到按键/光标位移。
#
# 用法：
#   ./kmbret.sh                约 30 秒；会**短暂停止并恢复**麒麟 otikm
#   EXIT=right ./kmbret.sh     物理摆位相反（麒麟在 Windows 左边）时用
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"

EXIT=${EXIT:-left}                 # 从麒麟哪条边交给对端（默认 left：麒麟屏在 Windows 右边）
DUR=12                              # 合成脚本实例的运行时长（秒）
GEST=/tmp/kmbret-gesture.txt
KLOG=/tmp/kmbret-otikm.log
KINJ=/tmp/kmbret-inject.out
KCON=/tmp/kmbret-console.log
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

if [ "$EXIT" = left ]; then
    EXIT_DX=-50
    BACK_DX=2                       # 往左出去 → 从对端右边进来 → 往**右**推回来
    BACK_NAME="右"
else
    EXIT_DX=50
    BACK_DX=-2
    BACK_NAME="左"
fi

echo "=== 0/5 前提 ==="
khave 'echo up' && ok "麒麟 SSH 可达（$KY）" || { bad "麒麟不可达：先跑 tools/gate.sh --pre"; summary "拓扑 B 回程手势回归（kmbret）"; exit 1; }
khave 'test -x ~/otilink/otikm' && ok "麒麟 ~/otilink/otikm 存在" || bad "麒麟 ~/otilink/otikm 不存在（先 tools/gate.sh --deploy-kylin）"
NC=$(pwsh_cmd '(Get-PnpDevice -PresentOnly -EA SilentlyContinue | Where-Object { $_.InstanceId -like "*VID_0EA0*" } | Measure-Object).Count' | tail -1 | tr -d ' ')
[ "${NC:-0}" -ge 1 ] && ok "Windows 侧对拷线在场（VID_0EA0 设备 $NC 个）" || bad "Windows 看不到对拷线（VID_0EA0）"
[ "$(count_vendor)" = 0 ] && ok "厂商程序未运行（L1：一条链路只能有一个主人）" || bad "厂商程序在跑，先跑 windows/vendor-off.vbs"

echo "=== 1/5 合成手势（在麒麟上跑，经真实线缆到 Windows）==="
# 出去：从屏幕中心推到边（合成实例的 core 坐标起点=屏幕中心），只多推 ~250 计数；
# 回程：只用 2px 小步（**回归点**：小步必须靠累计才能回程）。
# 注意"推出去多少就得推回来多少"（镜像手势）：出去得越远，回来要推的事件数越多，
# 所以这里刻意只多推一点点。
{
    for _ in $(seq 1 24); do echo "mouse $EXIT_DX 0 0 0"; done
    echo "sleep 1200"
    # 在"指针在 Windows"的状态下按 Ctrl+C：验证修饰键抑制缓冲会被补发（不是吞掉）。
    # 键之间留真人级间隔（100~250ms）：线缆 HID 键盘是**状态型**的，Windows 按
    # 轮询间隔取"当前报表"，几毫秒内连发 down/up 会被合并（测试假象，不是产品 bug）。
    # ⚠️ 线缆 HID 键盘接口**本身会丢报表**（NOTES §61.2 固件缺陷）→ 同一段 Ctrl+C **连试 3 遍**，
    #    任一遍到达即算通道通（否则这条断言会随机红/绿：同二进制实测 22/0 与 19/2 两种结果）。
    for _try in 1 2 3; do
        echo "key 29 1"
        echo "sleep 150"
        echo "key 46 1"
        echo "sleep 250"
        echo "key 46 0"
        echo "sleep 150"
        echo "key 29 0"
        echo "sleep 700"
    done
    for _ in $(seq 1 300); do echo "mouse $BACK_DX 0 0 0"; done
    echo "sleep 1500"
} > "$TMP/gesture.txt"
GB64=$(base64 -w0 "$TMP/gesture.txt")
kssh "printf '%s' '$GB64' | base64 -d > $GEST && wc -l < $GEST" >/dev/null 2>&1
khave "grep -q 'mouse $BACK_DX 0 0 0' $GEST" && ok "手势文件已就位（出去 ${EXIT_DX}px/次，回来 ${BACK_DX}px/次）" || bad "手势文件没写成功"

# Windows 光标归位到中间（否则"往左推"可能已经贴在左边缘，位移看不出来）
pwsh_file "$WIN_DIR/setcur.ps1" -X 1200 -Y 500 >/dev/null 2>&1
W0=$(wcur_x); [ -n "${W0:-}" ] && ok "Windows 光标基线 x=$W0" || bad "读不到 Windows 光标"

# Windows 低级键盘钩子探针：验证 Ctrl+C 真的落到 Windows 上
pwsh_file "$WIN_DIR/kbdprobe2.ps1" -Seconds 14 > "$TMP/kbdprobe.out" 2>&1 &
KPID=$!

echo "=== 2/5 麒麟：停真机 otikm → 跑合成手势实例 ==="
kssh 'pkill -x otikm' >/dev/null 2>&1
sleep 1
khave 'pgrep -x otikm' && bad "旧 otikm 没停掉" || ok "真机 otikm 已停（本次测试独占线缆）"
kssh "rm -f $KLOG $KINJ; cd ~/otilink && sg input -c \"DISPLAY=:0 nohup ./otikm --auto --transport cable --keepalive 5000 --config \$HOME/.config/otilink/kvm.conf --role master --edges left,right --no-grab --no-clipboard --sim-input $GEST --sim-inject $KINJ --log $KLOG --delay 6 --duration $DUR > $KCON 2>&1 &\"" >/dev/null 2>&1
sleep 2
KP=$(kssh 'pgrep -x otikm | tail -1')
[ -n "${KP:-}" ] && ok "合成实例已启动（pid $KP）" || bad "合成实例没起来（看 $KCON）"
for _ in $(seq 1 30); do
    [ -n "${KP:-}" ] || break
    khave "kill -0 $KP" || break
    sleep 1
done
if [ -n "${KP:-}" ] && khave "kill -0 $KP"; then
    bad "合成实例没按时退出（>30s）"
    kssh "kill -9 $KP" >/dev/null 2>&1
else
    ok "合成实例已自行退出"
fi
kssh "cat $KLOG" > "$TMP/otikm.log" 2>/dev/null
wait $KPID 2>/dev/null || true

echo "=== 3/5 麒麟侧断言（状态机 + 证据行）==="
N_OUT=$(grep -c "HID 模式：指针交给对端" "$TMP/otikm.log" 2>/dev/null || true)
N_BACK=$(grep -c "HID 模式：指针拉回本机" "$TMP/otikm.log" 2>/dev/null || true)
[ "${N_OUT:-0}" -ge 1 ] && ok "已交给 Windows（HID 模式：指针交给对端）" || bad "没有交出去：看 $KLOG（exit 方向/edges/角部死区？）"
[ "${N_BACK:-0}" -ge 1 ] && ok "**回程成功**：小步（往${BACK_NAME} ${BACK_DX}px/次）外推触发回程" || bad "回程失败：慢推没有累计触发（本次回归的核心断言！）"
L1=$(grep -n "HID 模式：指针交给对端" "$TMP/otikm.log" | head -1 | cut -d: -f1)
L2=$(grep -n "HID 模式：指针拉回本机" "$TMP/otikm.log" | head -1 | cut -d: -f1)
[ -n "${L1:-}" ] && [ -n "${L2:-}" ] && [ "$L2" -gt "$L1" ] && ok "顺序正确：先交出去（第 $L1 行）→ 后拉回（第 $L2 行）" || bad "日志顺序不对（交 $L1 / 回 $L2）"
grep -q "回程落点" "$TMP/otikm.log" && ok "回程落点已同步真实光标（XWarpPointer）" \
    || warn "日志里没有'回程落点'：X11 可能不可用（不影响回程本身，但指针会停在边上）"
grep -q "HID 补发被抑制的" "$TMP/otikm.log" && ok "修饰键抑制缓冲已补发（Ctrl+C 能到对端）" \
    || bad "HID 路径没有补发被抑制的按键（Ctrl/Alt 组合仍会被吞）"
grep -q "对端光标已停在入口边" "$TMP/otikm.log" && ok "交接时已把对端光标顶到入口边（消除不可见夹边位移）" \
    || bad "没有做入口边 park（回程判据仍可能与用户看到的不一致）"
grep -q "回程判据：已把推出去的位移推回" "$TMP/otikm.log" && ok "回程判据诊断有进度（日志可复盘）" \
    || warn "没有回程进度日志（可能推回得太快，或诊断被节流）"
grep -q "抓取设备 /dev/input/by-id/" "$TMP/otikm.log" && ok "抓取用的是 by-id 稳定路径（拔插后不会抓错设备）" \
    || warn "抓取路径不是 by-id（这台机器可能没有 by-id 链接；eventN 拔插后会变）"

echo "=== 4/5 Windows 侧断言（真收到 HID 才算通）==="
W1=$(wcur_x)
D=$(( ${W1:-0} - ${W0:-0} ))
# 交接时会把对端光标顶到入口边（park），所以位移方向不固定 —— 只看"确实动了"
AD=$D; [ "$AD" -lt 0 ] && AD=$(( -AD ))
[ "$AD" -gt 100 ] && ok "Windows 光标位移 ${D}px（HID 包真送到）" || bad "Windows 光标几乎没动（Δ=${D}px，HID 通道？）"
grep -q "hook=ok" "$TMP/kbdprobe.out" 2>/dev/null && ok "Windows 键盘钩子探针已挂上" || warn "键盘探针没挂上（钩子看不到按键，本条不可信）"
# 键盘到底到没到 Windows —— **分层判据**（2026-10-08 校准，见 NOTES §63.9）：
#   ① 我们的逻辑必须对：麒麟侧出现"HID 补发被抑制的…"（上面已断言，确定性）；
#   ② Windows 钩子是否真看到 Ctrl/C：受**线缆固件**影响 → **只 warn**。
# 为什么不 FAIL：线缆 HID 键盘接口每个会话只送得动约 6 条报表（NOTES §61.2），而接管时的
# **6 条 F24 令牌**恰好能把这点额度吃光（kmbret 的 --sim-input 实例永远看不到对端 ROLE →
# `per_seen=0` → 必发令牌）→ 之后的按键一条都到不了。同二进制实测 22/0 与 19/2 两种结果。
# 根治方向：键盘改走帧管道 + 对端 `SendInput`（§61.3）。
if grep -q "vk=0xA2" "$TMP/kbdprobe.out" 2>/dev/null; then
    ok "Windows 收到左 Ctrl（vk=0xA2，HID 直发）"
else
    warn "Windows 这轮没看到 Ctrl：线缆 HID 键盘接口丢报表（§61.2 固件缺陷，令牌还会先吃掉额度）—— 不计失败，见 NOTES §63.9"
fi
if grep -q "vk=0x43" "$TMP/kbdprobe.out" 2>/dev/null; then
    ok "Windows 收到 C（vk=0x43，HID 直发）"
else
    warn "Windows 这轮没看到 C：同上（固件丢包）—— 不计失败"
fi

echo "=== 5/5 恢复真机 otikm（run-kylin.sh，自动协商角色）==="
kssh 'cd ~/otilink && sg input -c "DISPLAY=:0 nohup ./run-kylin.sh > /tmp/rk.log 2>&1 &"' >/dev/null 2>&1
for _ in $(seq 1 15); do
    [ -n "$(kssh 'pgrep -x otikm')" ] && break
    sleep 1
done
[ -n "$(kssh 'pgrep -x otikm')" ] && ok "真机 otikm 已恢复（run-kylin.sh）" || bad "真机 otikm 没恢复：手动跑 ~/otilink/run-kylin.sh"

# 失败现场落盘：探测输出 + 两侧日志（排查"键盘没到"这类问题全靠它）
if [ "$FAIL" -gt 0 ]; then
    mkdir -p /tmp/kmbret-artifacts
    cp "$TMP/kbdprobe.out" /tmp/kmbret-artifacts/ 2>/dev/null
    cp "$TMP/otikm.log" /tmp/kmbret-artifacts/otikm-local.log 2>/dev/null
    kssh "cat $KLOG" > /tmp/kmbret-artifacts/otikm.log 2>/dev/null
    info "失败现场已保存到 /tmp/kmbret-artifacts/（kbdprobe.out / otikm.log）"
fi
summary "拓扑 B 回程手势回归（kmbret）"
