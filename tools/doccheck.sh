#!/bin/bash
# doccheck.sh —— 门禁 1：静态与文档一致性（不需要硬件，秒级）
#
# 每一项都对应一个**真实踩过的坑**，不是形式主义：
#   * bash -n / make all        —— 语法/编译错误（"改完没编"）
#   * .ps1 必须带 UTF-8 BOM    —— PowerShell 5.1 按 ANSI 读，中文注释会吞掉代码
#   * .vbs 必须 CRLF           —— LF-only 时 WScript 静默不执行（cscript 仍返回 0）
#   * BuildTag 指纹            —— csc 覆盖运行中的 exe 静默失败 → 一直在跑旧二进制
#   * 禁用 pkill -f            —— 会杀掉调用者自己（匹配到自己的命令行）
#   * 回归脚本必须注册进门禁   —— 新写的验证脚本不能被遗忘在角落
#   * AGENTS.md 引用的路径存在 —— 文档漂移
#
# 用法： ./doccheck.sh [--stamp]      （--stamp 只用于"我确认已 bump BuildTag"后更新指纹）
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

STAMP_ONLY=0
[ "${1:-}" = "--stamp" ] && STAMP_ONLY=1

# ---------------------------------------------------------------- 语法
stage "1/6 shell 语法（bash -n / sh -n）"
while IFS= read -r f; do
    if head -1 "$f" | grep -q '/bin/sh$'; then
        sh -n "$f" 2>/dev/null && ok "sh -n $(realpath --relative-to="$RE_DIR" "$f")" \
            || bad "sh -n 失败: $f"
    else
        bash -n "$f" 2>/dev/null && ok "bash -n $(realpath --relative-to="$RE_DIR" "$f")" \
            || bad "bash -n 失败: $f"
    fi
done < <(find "$RE_DIR" -name '*.sh' -type f | sort)

# ---------------------------------------------------------------- 编译
stage "2/6 本地编译（make all + coretest）"
if command -v gcc >/dev/null 2>&1; then
    if OUT=$(make -C "$OTI_DIR" -j4 all 2>&1); then ok "make all 通过（$(echo "$OUT" | grep -c '^cc') 个目标）"
    else bad "make all 失败"; echo "$OUT" | tail -15 | sed 's/^/      /'; fi
    if OUT=$(make -C "$OTI_DIR" coretest 2>&1) && OUT2=$(cd "$OTI_DIR" && ./coretest 2>&1); then
        echo "$OUT2" | grep -q "0 项失败" && ok "coretest 通过（交接/回程状态机）" \
            || { bad "coretest 有失败项"; echo "$OUT2" | tail -6 | sed 's/^/      /'; }
    else bad "coretest 编译/运行失败"; fi
else
    warn "没有 gcc，跳过编译门禁"
fi

# ---------------------------------------------------------------- 文件格式
stage "3/6 部署文件格式（BOM / CRLF）"
for f in "$WIN_DIR"/*.ps1; do
    [ -f "$f" ] || continue
    if head -c3 "$f" | od -An -tx1 | grep -qi 'ef bb bf'; then ok "BOM ok $(basename "$f")"
    else bad "$(basename "$f") 缺 UTF-8 BOM（PowerShell 5.1 会按 ANSI 读 → 中文注释吞代码；deploy.sh 会补）"; fi
done
for f in "$WIN_DIR"/*.vbs; do
    [ -f "$f" ] || continue
    if grep -qU $'\r' "$f" 2>/dev/null; then ok "CRLF ok $(basename "$f")"
    else bad "$(basename "$f") 是 LF 行尾（WScript 会静默不执行；deploy.sh 会归一化）"; fi
done
for f in "$RE_DIR/portable"/*.ps1; do
    [ -f "$f" ] || continue
    if head -c3 "$f" | od -An -tx1 | grep -qi 'ef bb bf'; then ok "BOM ok portable/$(basename "$f")"
    else bad "portable/$(basename "$f") 缺 UTF-8 BOM（实测会让 PowerShell 把中文注释读成乱码并吞换行 → 解析报错）"; fi
done
for f in "$RE_DIR/portable"/*.cmd; do
    [ -f "$f" ] || continue
    if grep -qU $'\r' "$f" 2>/dev/null; then ok "CRLF ok portable/$(basename "$f")"
    else bad "portable/$(basename "$f") 是 LF 行尾（cmd.exe 多行块/标签会出怪问题）"; fi
done
grep -q 'BuildTag' "$WIN_DIR/otiagent2.cs" && ok "otiagent2.cs 有 BuildTag（能认正在运行的版本）" \
    || bad "otiagent2.cs 没有 BuildTag：无法判断跑的是哪一版"

# ---------------------------------------------------------------- 指纹门禁
stage "4/6 改码必 bump 版本（BuildTag 指纹）"
SHA_FILE="$OTI_DIR/.csbuild.sha"
CUR_SHA=$(sha256sum "$WIN_DIR/otiagent2.cs" | cut -c1-16)
TAG=$(grep -o 'BuildTag = "[^"]*"' "$WIN_DIR/otiagent2.cs" | head -1 | sed 's/.*"\(.*\)"/\1/')
if [ "$STAMP_ONLY" -eq 1 ]; then
    printf '%s %s\n' "$CUR_SHA" "$TAG" > "$SHA_FILE"
    ok "已写入指纹戳：$CUR_SHA (BuildTag=$TAG)"
else
    if [ -f "$SHA_FILE" ]; then
        OLD_SHA=$(cut -d' ' -f1 "$SHA_FILE"); OLD_TAG=$(cut -d' ' -f2- "$SHA_FILE")
        if [ "$CUR_SHA" = "$OLD_SHA" ]; then ok "otiagent2.cs 未变（指纹 $CUR_SHA，BuildTag=$TAG）"
        elif [ "$TAG" != "$OLD_TAG" ]; then ok "代码改了且 BuildTag 已 bump（$OLD_TAG → $TAG）"
        else bad "otiagent2.cs 改了但 BuildTag 还是 $TAG：必须 bump（否则分不清跑的是哪一版），然后 ./doccheck.sh --stamp"; fi
    else
        warn "首次运行，建立指纹戳（$CUR_SHA / $TAG）"; printf '%s %s\n' "$CUR_SHA" "$TAG" > "$SHA_FILE"
    fi
fi

# ---------------------------------------------------------------- 禁写清单
stage "5/6 危险写法黑名单"
# 排除检查器自身（它的模式串里就含这些词）
HITS=$(grep -rn --include='*.sh' -e 'pkill -f' "$RE_DIR" | grep -v 'tools/doccheck.sh' || true)
if [ -n "$HITS" ]; then
    bad "发现 pkill -f（会匹配到调用者自己的命令行并自杀）→ 改 pkill -x 或按 pid 杀"
    echo "$HITS" | sed 's/^/      /'
else ok "无 pkill -f（历史坑：会杀掉调用它的 shell）"; fi
# 只报**真正危险**的形态：把 ssh 客户端自己的 stderr 并进管道捕获（`ssh … 2>&1 | grep …`）
# 真正的坑形如：kssh "$@" 2>&1 | grep -v 横幅   （2>&1 属于 **ssh 客户端本身**）
# 不算的：2>&1 落在引号里的远端命令中（那是远端的管道）；以及 `2>&1 ||`（逻辑或）
# 用 python3 判引号状态（grep 做不了），python3 不可用时降级为跳过该项
if command -v python3 >/dev/null 2>&1; then
    HITS=$(RE="$RE_DIR" python3 - <<'PYEOF'
import os, re, pathlib
root = pathlib.Path(os.environ["RE"]); bad = []
for p in sorted(root.rglob("*.sh")):
    if p.name == "doccheck.sh":
        continue
    try:
        lines = p.read_text(errors="replace").splitlines()
    except Exception:
        continue
    for i, line in enumerate(lines, 1):
        for m in re.finditer(r"2>&1\s*\|", line):
            pre = line[:m.start()]
            if pre.count("'") % 2 or pre.count('"') % 2:      # 在引号内 = 远端命令的一部分
                continue
            if line[m.end():m.end() + 1] == "|":               # `||`
                continue
            if not re.search(r"\b(ssh|kssh)\b", pre):          # 不是 ssh/kssh 调用
                continue
            bad.append("%s:%d: %s" % (p, i, line.strip()))
print("\n".join(bad))
PYEOF
)
    if [ -n "$HITS" ]; then
        bad "把 ssh 的 stderr 并进了捕获管道：麒麟登录横幅会污染返回值（文本/md5 全错）"
        echo "$HITS" | sed 's/^/      /'
    else ok "ssh 捕获未把 stderr 并进管道"; fi
else warn "没有 python3，跳过 ssh stderr 检查"; fi
HITS=$(grep -rn --include='*.sh' -e 'rm -rf /' -e 'rm -rf ~' "$RE_DIR" | grep -v 'tools/doccheck.sh' || true)
if [ -n "$HITS" ]; then
    bad "发现危险的 rm -rf 绝对路径/家目录"
    echo "$HITS" | sed 's/^/      /'
else ok "无危险 rm -rf"; fi

# ---------------------------------------------------------------- 文档/注册一致性
stage "6/6 文档与脚本注册一致性"
# 1) AGENTS.md 引用的 re/ 路径必须存在
if [ -f "$RE_DIR/AGENTS.md" ]; then
    MISSING=$(grep -o 're/[A-Za-z0-9_./-]*' "$RE_DIR/AGENTS.md" | sed 's/[.,;:)]$//' | sort -u \
              | grep -v '\*' | while read -r p; do
                    [ -e "$RE_DIR/../$p" ] || echo "$p"; done)
    if [ -z "$MISSING" ]; then ok "AGENTS.md 引用的路径全部存在"
    else bad "AGENTS.md 引用了不存在的路径："; echo "$MISSING" | sed 's/^/      /'; fi
else bad "缺 re/AGENTS.md（agent 运行规范入口）"; fi
# 2) 回归脚本必须被 gate.sh 注册
for s in clipreg.sh xferreg.sh hwtest.sh kmbret.sh kbdexcl.sh hidlat.sh xferlocal.sh coretest; do
    if [ -e "$OTI_DIR/$s" ]; then
        grep -q "$s" "$RE_DIR/tools/gate.sh" && ok "$s 已注册进门禁" || bad "$s 没被 gate.sh 注册（新验证脚本不许漏注册）"
    else warn "缺少回归脚本 $s"; fi
done
# 2b) 绿色包门禁脚本也必须被注册（它不在 otilink/ 下，单独查一次）
if [ -e "$RE_DIR/tools/portablecheck.sh" ]; then
    grep -q 'portablecheck.sh' "$RE_DIR/tools/gate.sh" && ok "portablecheck.sh 已注册进门禁"         || bad "portablecheck.sh 没被 gate.sh 注册（新验证脚本不许漏注册）"
else warn "缺少 tools/portablecheck.sh"; fi
# 3) 权威文档存在
for d in HANDOFF.md NOTES.md RUNBOOK.md PROTOCOL.md AGENTS.md; do
    [ -f "$RE_DIR/$d" ] && ok "文档 $d 存在（$(wc -l < "$RE_DIR/$d") 行）" || bad "缺文档 $d"
done

summary "静态与文档门禁（doccheck）"
