#!/bin/bash
# gate.sh —— 统一门禁入口（agent 提交结论前**必须**跑对应档位并贴出证据）
#
# 档位（按"这次改了什么"选，见 re/AGENTS.md §门禁）：
#   --pre            施工前体检：环境/拓扑/安全前提（不动任何东西）
#   --static         静态门禁：语法 + BOM/CRLF + BuildTag 指纹 + 文档/注册一致性（秒级）
#   --local          本机门禁（无硬件）：static + make test（7 套本地套件）+ coretest + xferlocal
#   --hw-clip        剪贴板真机回归（clipreg，约 50s，不动光标）
#   --hw-km          键鼠真机回归（hwtest，拓扑 A：Windows 主控；约 90s，**会动真实光标**）
#   --hw-kmb         键鼠真机回归（kmbret，拓扑 B：键鼠在麒麟 → 回程手势 + Ctrl 组合，约 30s）
#   --hw-kbdexcl     键盘独占真机回归（kbdexcl，拓扑 B：接管期间本地键盘必须被独占 → 防双重输入；约 40s）
#   --hw-lat         键鼠卡顿回归（hidlat，与 otikm 并发量 HID 写间隔；>200ms 即卡顿，约 20s）
#   --hw-xfer [N]    大文件真机回归（xferreg，缺省 64MB）
#   --replug         线缆拔插自愈回归（replugreg，USB unbind/bind 模拟拔插 N 轮，约 40s）
#   --hw-all         上面三档全跑
#   --full           local + hw-all（改动跨多条链路的最终验收）
#   --deploy-kylin   把 otilink 源码同步到麒麟 → 编译 → 打 KySec 标签 → 写标签戳
#   --portable       免安装绿色包门禁：产物新鲜度/GLIBC 基线/依赖/清单/入口脚本（+可选容器矩阵）
#   --stamp          更新"源码/BuildTag 指纹戳"（改了 otiagent2.cs 且 bump 过 BuildTag 后跑）
#   --report         打印上一次的 JSON 报告
#
# 用法示例：
#   ./gate.sh --pre && ./gate.sh --static
#   ./gate.sh --hw-clip
#   ./gate.sh --full
# 退出码 = 失败项数（0 = 全过）。报告固定写到 /tmp/otilink-gate.json。
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

export GATE_JSON=${GATE_JSON:-/tmp/otilink-gate.json}
HW_WARNED=0

need_hw_notice() {
    [ "$HW_WARNED" -eq 1 ] && return 0
    HW_WARNED=1
    info "注意：硬件档会真实操作键鼠/剪贴板（hwtest 会动光标约 90s，请勿同时手动操作）"
}

# 跑一个回归脚本，把它自己的 PASS=/FAIL= 折进总账
run_regression() {                     # run_regression <显示名> <脚本路径> [参数...]
    local name=$1 script=$2; shift 2
    local slug; slug=$(basename "$script" .sh)          # 档位名可能含中文/斜杠 → 用脚本名当日志名
    local log="/tmp/gate-$slug.log"
    stage "$name"
    if [ ! -x "$script" ]; then bad "$name 不存在或不可执行：$script"; stage_result "$name" 1; return 1; fi
    ( cd "$(dirname "$script")" && "./$(basename "$script")" "$@" ) > "$log" 2>&1
    local rc=$?
    local pf; pf=$(parse_pf "$log")
    local p=${pf%% *} f=${pf##* }
    tail -6 "$log" | sed 's/^/      /'
    if [ "$rc" -eq 0 ]; then ok "$name 通过（PASS=${p:-?} FAIL=${f:-0}）  日志: $log"
    elif reason=$(is_known "$slug"); then
        known "$name 失败，但已在 known-issues.txt 登记（不计入失败）  日志: $log"
        printf '        原因: %s\n' "$reason"
        stage_known "$name"
    else
        bad "$name 失败（退出码 $rc，PASS=${p:-?} FAIL=${f:-?}）  日志: $log"
    fi
    [ "$rc" -eq 0 ] || return 0
    stage_result "$name" "$rc"
    return 0
}

run_stage() {                          # run_stage <ascii-slug> <显示名> <命令...>
    local slug=$1 name=$2; shift 2
    local log="/tmp/gate-$slug.log"
    stage "$name"
    if "$@" > "$log" 2>&1; then
        tail -4 "$log" | sed 's/^/      /'
        ok "$name 通过"
        stage_result "$name" 0
    else
        if reason=$(is_known "$slug"); then
            tail -6 "$log" | sed 's/^/      /'
            known "$name 失败，但已在 known-issues.txt 登记（不计入失败）"
            printf '        原因: %s\n' "$reason"
            stage_known "$name"
        else
            tail -20 "$log" | sed 's/^/      /'
            bad "$name 失败（详见 $log）"
            stage_result "$name" 1
        fi
        return 0
    fi
}

TITLE="门禁"
DO=()
while [ $# -gt 0 ]; do
    case "$1" in
        --pre)          DO+=(pre) ;;
        --static)       DO+=(static) ;;
        --local)        DO+=(local) ;;
        --hw-clip)      DO+=(hwclip) ;;
        --hw-km)        DO+=(hwkm) ;;
        --hw-kmb)       DO+=(hwkmb) ;;
        --hw-kbdexcl)   DO+=(hwkbdexcl) ;;
        --hw-lat)       DO+=(hwlat) ;;
        --hw-xfer)      DO+=(hwxfer); XFER_BYTES=${2:-67108864}; case "${2:-}" in ''|--*) ;; *) shift ;; esac ;;
        --replug)       DO+=(replug) ;;
        --hw-all)       DO+=(hwclip hwkm hwxfer) ;;
        --full)         DO+=(local hwclip hwkm hwxfer); TITLE="完整门禁" ;;
        --deploy-kylin) DO+=(deploy) ;;
        --portable)     DO+=(portable) ;;
        --stamp)        DO+=(stamp) ;;
        --report)       DO+=(report) ;;
        -h|--help)      sed -n '2,30p' "$0"; exit 0 ;;
        *) die "未知参数：$1（--help 看用法）" ;;
    esac
    shift
done
[ ${#DO[@]} -eq 0 ] && DO=(static)

# 需要麒麟的门禁档：KY/KY_PASS 没设置时**当场给提示**（否则会在 envcheck 里变成一串
# 误导性的 FAIL，比如"没人转发"）。仓库不内置这两个值（可公开），见 re/AGENTS.md §0。
NEEDS_SSH=0
for step in "${DO[@]}"; do
    case "$step" in pre|hwclip|hwkm|hwkmb|hwkbdexcl|hwlat|replug|hwxfer|deploy) NEEDS_SSH=1 ;; esac
done
if [ "$NEEDS_SSH" -eq 1 ] && { [ -z "${KY:-}" ] || [ -z "${KY_PASS:-}" ]; }; then
    { printf '%s[FATAL]%s 该档位需要连麒麟，但环境变量没设置：\n' "$R" "$N"
      printf '        export KY=kylin@<麒麟IP>      # 或 tailscale 名，如 kylin-pc\n'
      printf '        export KY_PASS=<麒麟登录密码>\n'
      printf '        （建议写进 ~/.otilink_env（chmod 600）后 source；见 re/AGENTS.md §0 / RUNBOOK §0.0）\n'; } >&2
    exit 2
fi

for step in "${DO[@]}"; do
case "$step" in
    pre)
        run_regression "环境体检" "$RE_DIR/tools/envcheck.sh" ;;

    static)
        run_regression "静态/文档门禁" "$RE_DIR/tools/doccheck.sh" ;;

    local)
        run_regression "静态/文档门禁" "$RE_DIR/tools/doccheck.sh"
        # 逐套件跑（而不是 `make test` 一把梭）：这样单个套件的已知问题不会掩盖其它套件
        for t in "selftest:帧/CDB 自检:./probe selftest" \
                 "prototest:消息层与状态机单测:./selftest_proto" \
                 "e2etest:端到端（库层）:./e2e_test" \
                 "itest:双实例集成（键鼠 daemon）:sh itest.sh" \
                 "cliptest:剪贴板集成（本机）:sh cliptest.sh" \
                 "tcptest:TCP 传输集成:sh tcptest.sh" \
                 "mocktest:cable 传输路径（软件仿真）:./mocktest"; do
            slug=${t%%:*}; rest=${t#*:}; nm=${rest%%:*}; cmd=${rest#*:}
            run_stage "$slug" "$nm" sh -c "cd '$OTI_DIR' && $cmd"
        done
        run_stage coretest "交接/回程状态机单测（coretest）" sh -c "make -C '$OTI_DIR' coretest >/dev/null && '$OTI_DIR/coretest'"
        run_regression "大文件本机回归" "$OTI_DIR/xferlocal.sh" ;;

    hwclip)
        need_hw_notice
        run_regression "环境体检" "$RE_DIR/tools/envcheck.sh"
        run_regression "剪贴板真机回归" "$OTI_DIR/clipreg.sh"
        run_regression "短内容剪贴板（1..6 字符）" "$OTI_DIR/clipshort.sh" ;;

    hwkm)
        need_hw_notice
        run_regression "环境体检" "$RE_DIR/tools/envcheck.sh"
        run_regression "键鼠真机回归" "$OTI_DIR/hwtest.sh" ;;

    hwkmb)
        need_hw_notice
        run_regression "环境体检" "$RE_DIR/tools/envcheck.sh"
        run_regression "拓扑 B 回程手势回归" "$OTI_DIR/kmbret.sh" ;;

    hwkbdexcl)
        need_hw_notice
        run_regression "环境体检" "$RE_DIR/tools/envcheck.sh"
        run_regression "键盘独占回归" "$OTI_DIR/kbdexcl.sh" ;;

    hwlat)
        need_hw_notice
        run_regression "键鼠卡顿回归" "$OTI_DIR/hidlat.sh" ;;

    replug)
        need_hw_notice
        run_regression "线缆拔插自愈回归" "$OTI_DIR/replugreg.sh" ;;

    hwxfer)
        need_hw_notice
        run_regression "环境体检" "$RE_DIR/tools/envcheck.sh"
        run_regression "大文件真机回归" "$OTI_DIR/xferreg.sh" "${XFER_BYTES:-67108864}" ;;

    deploy)
        stage "部署到麒麟（源码 → 编译 → KySec 标签）"
        ensure_kssh
        if ! kssh 'echo up' | grep -q up; then
            bad "SSH 不可达 $KY，部署中止"
        else
            FILES=()
            for f in "$OTI_DIR"/*.c "$OTI_DIR"/*.h "$OTI_DIR"/Makefile "$OTI_DIR"/*.sh "$OTI_DIR"/*.conf "$OTI_DIR"/*.rules; do
                [ -f "$f" ] && FILES+=("$f")
            done
            if scp -q -p -o StrictHostKeyChecking=no "${FILES[@]}" "$KY:~/otilink/" 2>/dev/null; then
                ok "已同步 ${#FILES[@]} 个文件到 ~/otilink/"
            else bad "scp 同步失败"; fi
            if OUT=$(kssh 'cd ~/otilink && make otikm 2>&1 | tail -3'); then
                echo "$OUT" | sed 's/^/      /'
                echo "$OUT" | grep -qi 'error' && bad "麒麟侧编译报错" || ok "麒麟侧 make otikm 通过"
            else bad "麒麟侧 make 失败"; fi
            if OUT=$(kssh "cd ~/otilink && echo $KY_PASS | sudo -S ./label-kysec.sh 2>&1 | tail -2 && touch .kysec-stamp"); then
                echo "$OUT" | sed 's/^/      /'
                kssh 'test ~/otilink/.kysec-stamp' >/dev/null 2>&1 && ok "KySec 标签已重打并写入标签戳" \
                    || bad "标签戳没写成功（KySec 未打标签？图片剪贴板会退化成路径）"
            else bad "label-kysec.sh 执行失败"; fi
            info "改完记得重启 otikm（否则跑的还是旧进程）：见 re/AGENTS.md §4.1（改完必须重启 otikm）"
        fi
        ;;

    portable)
        run_regression "免安装绿色包（portable）" "$RE_DIR/tools/portablecheck.sh" ;;

    stamp)
        run_regression "更新指纹戳" "$RE_DIR/tools/doccheck.sh" --stamp ;;

    report)
        stage "上一次门禁报告"
        if [ -f "$GATE_JSON" ]; then
            cat "$GATE_JSON"
            ok "报告读取自 $GATE_JSON"
        else bad "没有报告文件 $GATE_JSON（先跑一次门禁）"; fi ;;
esac
done

summary "$TITLE"
