#!/bin/sh
# run-kylin.sh —— 麒麟端启动脚本（第 46 轮起：**角色交给 otikm 协商**，本脚本只剩入口）
#
# 为什么变薄：原来这里用 /dev/input/by-id 通配单方面判"本机有没有键鼠"来决定主控/被驱动，
# 而 Windows 侧开机也无条件起 otiagent2 —— 用户把键鼠换到另一台后**两侧同时是主控**，
# 抢同一条 HID/帧管道（实测帧管道双向全丢、剪贴板两边都"未确认"）。现在：
#   * 设备发现由 otikm 的 --auto 做（按能力位 + sysfs VID/PID，不依赖发行版命名）；
#   * 角色由两端每 1s 交换 OTI_MSG_ROLE 协商（otikm_core.c 的 otikm_role_decide），
#     谁的本机键鼠（可热插拔 USB 键鼠）插在这一边、谁在用，就谁当主控，另一侧主动让位；
#     任何时刻最多一个 grab（不变式 I1/I2）。
#
# 前置：跑过一次 sudo sh install-kylin.sh 并重新登录（取得 input/uinput 权限）。
#
# 用法：
#   ./run-kylin.sh              正常启动（自动发现 + 协商角色；默认独占抓取 + 注入 + 剪贴板）
#   ./run-kylin.sh --no-grab    主控时不独占本机键鼠（便于试跑）
#   ./run-kylin.sh --role master|slave   显式指定角色（覆盖协商，对端会让位）
#   ./run-kylin.sh --dry        只做环境自检（不启动）
#
# 策略（热键 / 撞边边缘 / 角部防误切 / 看门狗 / 剪贴板开关 / 保活）全部来自
#   ~/.config/otilink/kvm.conf（可用 OTILINK_CONFIG 指定别的路径）。
cd "$(dirname "$0")" || exit 1

CFG="${OTILINK_CONFIG:-$HOME/.config/otilink/kvm.conf}"

# ⚠️ 用户参数必须**原样保留**：以前这里写 `set -- --auto --transport cable ...`，
#    会把 "$@" 整个丢掉 → `run-kylin.sh --role slave` 静默失效（第 46 轮真机踩到：
#    日志里还是"目标=auto"，强制的角色根本没传进去）。
#    现在默认参数放前面、用户参数追加在后面（otikm 后解析的覆盖前面的）。
set -- --auto --transport cable --keepalive 5000 "$@"
[ -f "$CFG" ] && set -- "$@" --config "$CFG"

for arg in "$@"; do
    case "$arg" in
        --dry) exec ./otikm --doctor ;;
    esac
done

echo "启动 otikm --auto：自动发现线缆/输入设备 → 与对端协商角色（可热插拔键鼠插在哪边/谁在用谁当主控）"
echo "  提示：角色默认由协商决定；要固定用 --role master|slave，要自检用 --dry"
exec ./otikm "$@"
