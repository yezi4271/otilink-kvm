#!/bin/bash
# portablecheck.sh —— 门禁：免安装绿色包（re/portable/dist）能不能发布
#
# 为什么需要它（每一条都对应真实踩过的坑）：
#   * **"在开发机编的二进制拿到麒麟起不来"**：WSL glibc 2.39 编出来的东西要求 GLIBC_2.38，
#     麒麟只有 2.31 → 直接 undefined symbol。所以发布件必须"按声明基线构建"，这里用
#     readelf 的实际符号需求去核对，而不是相信人记得。
#   * **"发的是旧二进制"**：与 Windows 侧 BuildTag 同一条纪律 —— 用源码指纹核对新鲜度。
#   * 绿色包一旦硬依赖 libX11 之类就"到别的机器跑不起来"：只允许依赖 libc。
#   * 包内文件缺失只有用户插上线才会发现；这里先把清单核一遍。
#
# 用法： ./portablecheck.sh            （需要先跑 re/portable/build.sh 产出 dist）
# 退出码 = 失败项数（0 = 全过）
set -u
. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

P_DIR="$RE_DIR/portable"
VERSION="$(cat "$P_DIR/VERSION" 2>/dev/null | tr -d ' \n')"
DIST="$P_DIR/dist/otilink-portable-$VERSION"
ARCH=$(uname -m)

stage "1/5 产物存在性与新鲜度"
if [ ! -d "$DIST" ]; then
    bad "没有产物目录 $DIST（先跑 re/portable/build.sh）"
    summary "免安装绿色包（portable）"
    exit "$FAIL"
fi
ok "产物目录存在：$(realpath --relative-to="$RE_DIR" "$DIST")"

INFO="$DIST/BUILD-INFO"
if [ -f "$INFO" ]; then
    ok "BUILD-INFO 存在"
    # 注意：变量名别用 V/S/B —— lib.sh 用它们存颜色转义（stage() 就打 $B），覆盖了会把标题打花
    PKGVER=$(sed -n 's/^version=//p' "$INFO" | head -1)
    PKGSHA=$(sed -n 's/^src_sha=//p' "$INFO" | head -1)
    PKGPKG=$(sed -n 's/^pkg_sha=//p' "$INFO" | head -1)
    PKGBL=$(sed -n 's/^baseline=//p' "$INFO" | head -1)
    [ "$PKGVER" = "$VERSION" ] && ok "版本一致（$PKGVER == re/portable/VERSION）" \
        || bad "版本不一致：BUILD-INFO=$PKGVER，VERSION 文件=$VERSION（改完要重新 build.sh）"
    CUR=$(cd "$OTI_DIR" && cat $(ls *.c *.h 2>/dev/null | LC_ALL=C sort) Makefile 2>/dev/null | sha256sum | cut -c1-12)
    [ "$PKGSHA" = "$CUR" ] && ok "源码指纹一致（$PKGSHA）—— 包里的二进制就是当前源码编的" \
        || bad "源码指纹不一致：包内 $PKGSHA，当前源码 $CUR（源码改过没重新 build.sh）"
    # 包指纹：连"运行期脚本（入口/README）"一起核 —— 只核 C 源码会漏掉"改了入口没重新打包"
    CURPKG=$( { cat $(cd "$OTI_DIR" && ls *.c *.h 2>/dev/null | sed "s|^|$OTI_DIR/|" | LC_ALL=C sort) "$OTI_DIR/Makefile" \
                "$P_DIR/otilink.sh" "$P_DIR/otilink.cmd" "$P_DIR/otilink-win.ps1" \
                "$P_DIR/mac/otilink-mac.sh" "$P_DIR/README.md" "$P_DIR/VERSION" 2>/dev/null; } \
              | sha256sum | cut -c1-12)
    if [ -z "$PKGPKG" ]; then
        warn "BUILD-INFO 没有 pkg_sha（旧产物）：只核了 C 源码指纹，建议重新 build.sh"
    elif [ "$PKGPKG" = "$CURPKG" ]; then
        ok "包指纹一致（$PKGPKG）—— 入口脚本/README 与产物同步"
    else
        bad "包指纹不一致：包内 $PKGPKG，当前 $CURPKG（改了入口脚本/README 没重新 build.sh）"
    fi
    [ -n "$PKGBL" ] && ok "声明基线：$PKGBL" || bad "BUILD-INFO 缺 baseline="
else
    bad "缺 BUILD-INFO（不要手工拼包，跑 build.sh）"
    PKGBL=""
fi

BIN="$DIST/linux/$ARCH/otikm"
if [ -x "$BIN" ]; then
    ok "Linux 主程序存在：linux/$ARCH/otikm（$(du -h "$BIN" | cut -f1)）"
else
    bad "缺 linux/$ARCH/otikm"
fi

stage "2/5 发布件必须能在别的发行版上跑（基线/依赖）"
if [ -x "$BIN" ]; then
    if [ "$PKGBL" = "local" ]; then
        warn "基线是 local（开发机自编）：**不能对外发布**（换台老 glibc 的机器就跑不起来）"
    elif [ -n "$PKGBL" ]; then
        BASEV="$(printf '%s' "$PKGBL" | sed 's/^glibc//;s/^gcc[0-9]*//')"
        MAXV=$(readelf --version-info "$BIN" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
        if [ -z "$MAXV" ]; then
            warn "readelf 读不到 GLIBC 版本需求（静态链接？）—— 人工确认"
        elif [ "$(printf '%s\n%s\n' "$MAXV" "$BASEV" | sort -V | tail -1)" = "$BASEV" ]; then
            ok "GLIBC 需求 $MAXV <= 声明基线 $BASEV（老发行版也能跑）"
            # 覆盖矩阵提示（写清楚，避免"以为到处都能跑"）
            info "基线 $BASEV 覆盖：Kylin V10 / UOS / Ubuntu 20.04+ / Debian 11+ / RHEL9 / Fedora 32+ / Arch / openSUSE 15.3+"
        else
            bad "GLIBC 需求 $MAXV 超过声明基线 $BASEV —— 这份二进制在基线机器上跑不起来（换 --baseline 重编）"
        fi
    fi
    # 依赖白名单：只允许 glibc 自己的组成部分（老 glibc 把 pthread/dl 拆成独立 .so，
    # glibc>=2.34 合并进 libc —— 两者都是"任何 glibc 系统都必然有"的，不算外部依赖）。
    DEPS=$(ldd "$BIN" 2>/dev/null | grep -o '=> [^ ]*' | awk '{print $2}' | xargs -r -n1 basename 2>/dev/null | sort -u | tr '\n' ' ')
    EXTRA=""
    for d in $DEPS; do
        case "$d" in
            libc.so.6|libdl.so.2|libpthread.so.0|librt.so.1|libm.so.6|libgcc_s.so.1|ld-linux*) ;;
            *) EXTRA="$EXTRA $d" ;;
        esac
    done
    if [ -z "$DEPS" ]; then
        warn "ldd 读不到依赖（静态链接？）"
    elif [ -z "$EXTRA" ]; then
        ok "动态依赖只有 glibc 自身（$DEPS）—— X11/XRandR 都是 dlopen，不会拖垮别的发行版"
    else
        bad "有外部动态依赖：$EXTRA（到别的发行版会缺库；应该改成 dlopen 或静态链接）"
    fi
fi

stage "3/5 包内文件清单"
want() {   # want <相对路径> [可执行]
    local f="$DIST/$1"
    if [ -f "$f" ]; then
        if [ "${2:-}" = x ] && [ ! -x "$f" ]; then bad "$1 存在但没有可执行位"; return; fi
        ok "$1"
    else
        bad "缺 $1"
    fi
}
want otilink.sh x
want VERSION
want BUILD-INFO
want README.md
want linux/$ARCH/otikm x
want linux/$ARCH/hidprobe x
want windows/otiagent2.exe
want windows/otiagent.ps1
want mac/otilink-mac.sh x
if [ -f "$DIST/windows/otiagent.ps1" ]; then
    head -c3 "$DIST/windows/otiagent.ps1" | od -An -tx1 | grep -qi 'ef bb bf' \
        && ok "windows/otiagent.ps1 带 UTF-8 BOM（PS 5.1 按 ANSI 读会吞中文注释）" \
        || bad "windows/otiagent.ps1 缺 BOM"
fi
if [ -f "$DIST/windows/otiagent2.exe" ]; then
    TAG=$(grep -o 'BuildTag = "[^"]*"' "$WIN_DIR/otiagent2.cs" | head -1 | sed 's/.*"\(.*\)"/\1/')
    if strings -el "$DIST/windows/otiagent2.exe" 2>/dev/null | grep -qF "$TAG" || grep -aq "$TAG" "$DIST/windows/otiagent2.exe"; then
        ok "otiagent2.exe 里有 BuildTag $TAG（与源码一致）"
    else
        bad "otiagent2.exe 里找不到 BuildTag $TAG（编的不是这份源码？）"
    fi
fi

stage "4/5 入口脚本能在本机跑（不需要硬件/root）"
if [ -x "$DIST/otilink.sh" ]; then
    OUT="$(cd "$DIST" && ./otilink.sh --version 2>&1)"
    echo "$OUT" | grep -q 'otikm' && ok "otilink.sh 能加载包内二进制并打印版本" \
        || bad "otilink.sh 跑不起来（缺二进制/架构不匹配？）"
    # FORCE=1：这是 dry-run（不建链路），别被本机残留的 otikm 实例拦住
    OUT="$(cd "$DIST" && OTILINK_FORCE=1 ./otilink.sh --print-plan --no-clip 2>&1)"
    if echo "$OUT" | grep -q 'otikm 计划'; then
        ok "otilink.sh --print-plan 能输出决策（无硬件也能验证探测逻辑）"
        R=$(echo "$OUT" | sed -n 's/^角色 *: //p' | head -1)
        info "本机（无对拷线）判定：角色=$R（容器/无设备机器上应为 被驱动侧 slave）"
    else
        bad "otilink.sh --print-plan 没输出计划（见 /tmp/gate-portablecheck.log）"
    fi
    bash -n "$DIST/otilink.sh" && ok "otilink.sh 语法自检" || bad "otilink.sh 语法错误"
fi
if [ -f "$DIST/otilink-win.ps1" ]; then
    head -c3 "$DIST/otilink-win.ps1" | od -An -tx1 | grep -qi 'ef bb bf' \
        && ok "otilink-win.ps1 带 UTF-8 BOM（PS 5.1 按 ANSI 读会把中文注释读成乱码并吞掉换行）" \
        || bad "otilink-win.ps1 缺 BOM（改文件时被工具吃掉了；实测会导致 PowerShell 解析报错）"
    if grep -qU $'\r' "$DIST/otilink.cmd" 2>/dev/null; then
        ok "otilink.cmd 是 CRLF（cmd.exe 的多行块在 LF-only 下会出怪问题）"
    else
        bad "otilink.cmd 不是 CRLF"
    fi
    if command -v powershell.exe >/dev/null 2>&1; then
        WPSP=$(wslpath -w "$DIST/otilink-win.ps1" 2>/dev/null)
        if OUT=$(powershell.exe -NoProfile -ExecutionPolicy Bypass \
                  -File "$(wslpath -w "$RE_DIR/tools/psparse.ps1")" "$WPSP" 2>&1 | tr -d '\r'); then
            ok "otilink-win.ps1 通过 PowerShell 真解析（$(printf '%s' "$OUT" | tail -1)）"
        else
            bad "otilink-win.ps1 PowerShell 解析报错：$(printf '%s' "$OUT" | head -3 | tr '\n' ' ')"
            info "最常见原因：BOM 被编辑器/工具吃掉（中文注释被按 ANSI 读 → 吞换行 → 假语法错误）"
        fi
    else
        warn "本机没有 powershell.exe：跳过 otilink-win.ps1 的解析检查"
    fi
fi
if [ -f "$DIST/mac/otilink-mac.sh" ]; then
    bash -n "$DIST/mac/otilink-mac.sh" && ok "mac/otilink-mac.sh 语法自检" || bad "mac 脚本语法错误"
fi

stage "5/5 跨发行版矩阵（可选：需要 Docker）"
if docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
    for img in ubuntu:20.04 ubuntu:24.04 debian:11 debian:12 rockylinux:9 fedora:latest archlinux:latest; do
        if OUT=$(timeout 180 docker run --rm -v "$DIST:/pkg:ro" "$img" /pkg/linux/$ARCH/otikm --version 2>&1); then
            echo "$OUT" | grep -q 'otikm' && ok "容器 $img：二进制可运行（$(echo "$OUT" | head -1)）" \
                || bad "容器 $img：跑起来了但没输出预期内容"
        else
            bad "容器 $img：跑不起来（见日志）"
        fi
    done
else
    warn "Docker 不可用 → **本次没有做跨发行版实测**；已完成的是 readelf 基线核对（静态保证），"
    info "如果要更强证据：打开 Docker Desktop 的 WSL 集成后重跑 ./portablecheck.sh"
fi

summary "免安装绿色包（portable）"
