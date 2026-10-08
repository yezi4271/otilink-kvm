#!/bin/bash
# clipshort.sh —— 短内容剪贴板回归：1..6 个字符逐长度测 Linux → Windows
#
# 为什么单独写：用户现场发现"1/2/3 个字符复制不过去，>=4 个就行"（第 41 轮）。
# 按规范：复现出来的 bug 必须变成脚本，不能再靠聊天记录。
#
# 用法： ./clipshort.sh [方向 l2w|w2l]      退出码 = 失败项数
set -u
. "$(dirname "${BASH_SOURCE[0]}")/../tools/lib.sh"

DIR=${1:-l2w}
stage "1/2 前提"
kssh 'pgrep -x otikm >/dev/null && echo yes' | grep -q yes && ok "麒麟 otikm 在跑" || bad "麒麟 otikm 没跑"
# 本脚本只测剪贴板：**不要求**键鼠代理在跑（拓扑 B 下它是 0；第 46 轮起角色由协商定）
NA=$(count_proc otiagent2)
[ "${NA:-0}" -ge 1 ] && ok "Windows 键鼠代理 ${NA} 个（本脚本不依赖它）" \
    || info "Windows 键鼠代理 0 个（拓扑 B：麒麟主控，正常）"

stage "2/2 短内容逐长度（$DIR）"
for N in 1 2 3 4 5 6; do
    MARK=$(printf '%*s' "$N" '' | tr ' ' 'x')          # x / xx / xxx / xxxx ...
    if [ "$DIR" = l2w ]; then
        kssh "setsid sh -c 'printf %s $MARK | DISPLAY=:0 xclip -selection clipboard -i' >/dev/null 2>&1 </dev/null & sleep 1" >/dev/null 2>&1
        sleep 3
        GOT=$(pwsh_cmd '(Get-Clipboard -Raw)' | head -1)
    else
        pwsh_cmd "Set-Clipboard -Value '$MARK'" >/dev/null 2>&1
        sleep 3
        GOT=$(kssh 'DISPLAY=:0 xclip -selection clipboard -o 2>/dev/null' | head -1)
    fi
    if [ "$GOT" = "$MARK" ]; then ok "$N 字符（$MARK）到对端"
    else bad "$N 字符（$MARK）没到对端（实得 '<$GOT>'）"; fi
done
summary "短内容剪贴板回归（clipshort $DIR）"
