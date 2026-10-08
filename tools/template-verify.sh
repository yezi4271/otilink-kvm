#!/bin/bash
# template-verify.sh —— 新验证脚本的模板（复制走改，别直接用它当测试）
#
# 用法：
#   cp tools/template-verify.sh otilink/mycheck.sh && chmod +x otilink/mycheck.sh
#   # 改：头部注释 / 检查项 / 名字
#   # **必须**在 tools/gate.sh 里加一档调用它，并在 AGENTS.md §验证脚本 里登记
#   # （doccheck.sh 会检查"回归脚本有没有被 gate.sh 注册"，漏了会 FAIL）
#
# 约定（三条，全部是硬要求）：
#   1) 每项检查用 ok/bad/warn 报一行；结束时 summary 打印汇总
#   2) **退出码 = 失败项数**（0 = 全过）—— 唯一可信的判定信号，别用 echo 里的词
#   3) 硬件相关检查前先跑 envcheck（`--pre`），否则"假失败"会浪费大量时间
#
# 依赖（按需保留）：WSL 侧 powershell.exe；SSH 到麒麟（KY_PASS）；麒麟 xdotool/xclip
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"      # otilink/ 下的脚本用这个相对路径

stage "1/2 前提"
if kssh 'pgrep -x otikm >/dev/null && echo yes' | grep -q yes; then ok "麒麟 otikm 在跑"
else bad "麒麟 otikm 没跑（先跑 tools/gate.sh --pre 看体检结果）"; fi
[ "$(count_proc otiagent2)" = 1 ] && ok "Windows 键鼠代理恰好 1 个" || bad "Windows 键鼠代理数量不对"

stage "2/2 我的检查"
# --- 示例：在麒麟放一段文本到剪贴板，等 3 秒，看 Windows 是否收到 -----------
kssh 'printf "TEMPLATE-1" | DISPLAY=:0 xclip -selection clipboard' >/dev/null 2>&1
sleep 3
if winclip | grep -q 'CLIP'; then ok "剪贴板通道有活动记录"
else bad "剪贴板通道没有活动（看 /tmp/gate-<name>.log 与 clip.log）"; fi

# --- 示例：读 Windows 光标（GetCursorPos，不用 WinForms：它受 DPI 虚拟化影响）--
X=$(wcur_x); [ -n "$X" ] && ok "读到 Windows 光标 x=$X" || bad "读不到 Windows 光标"

summary "我的验证（template）"
