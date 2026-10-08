#!/bin/bash
# build.sh —— 组装「免安装绿色包」（产物：dist/otilink-portable-<ver>/ + .tar.gz/.zip）
#
# 目标：一条命令产出可以直接拷到 U盘/机器上的绿色包，**目标机不需要 gcc、不需要安装**。
#
# 关键决定（详见 re/portable/README.md）：
#   * Linux 二进制**必须在"声明基线"的机器上编**：默认 --baseline kylin（麒麟 glibc 2.31）
#     → 覆盖 Kylin V10/UOS/Ubuntu 20.04+/Debian 11+/RHEL9/Fedora 32+/Arch/openSUSE 15.3+。
#     在开发机（glibc 2.39）编出来的二进制拿到麒麟**根本起不来**（实测 GLIBC_2.38 未定义符号），
#     所以"就地 make"只允许 --baseline local，且 portablecheck 会拒绝把 local 产物当发布件。
#   * Windows 侧只用系统自带的 csc（.NET Framework），编出 ~27KB 的 otiagent2.exe 放进包里；
#     绿色入口 otilink.cmd 只是把它和 otiagent.ps1 拉起来 —— 无管理员、无安装、无自启。
#
# 用法：
#   ./build.sh                          默认：麒麟基线 + 本机编 Windows 载荷 + 打包
#   ./build.sh --baseline local         开发机自编（**只用于测试，不许发布**）
#   ./build.sh --baseline docker:gcc:7  容器老基线（需 Docker Desktop 打开 WSL 集成）
#   ./build.sh --no-windows             不编 Windows 载荷
#   ./build.sh --info                   只打印将要做什么（不改任何文件）
set -u

SELF_DIR=$(cd "$(dirname "$0")" && pwd) || exit 2
RE_DIR=$(cd "$SELF_DIR/.." && pwd)
OTI_DIR="$RE_DIR/otilink"
WIN_DIR="$RE_DIR/windows"
VERSION="$(cat "$SELF_DIR/VERSION" 2>/dev/null | tr -d ' \n')"
[ -n "$VERSION" ] || { echo "缺 $SELF_DIR/VERSION"; exit 2; }

. "$RE_DIR/tools/lib.sh"       # kssh / KY / KY_PASS

BASELINE=kylin
WINDOWS=1
INFO=0
while [ $# -gt 0 ]; do
    case "$1" in
        --baseline) BASELINE="${2:-kylin}"; shift || true ;;
        --no-windows) WINDOWS=0 ;;
        --info) INFO=1 ;;
        -h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数 $1"; exit 2 ;;
    esac
    shift
done

DIST="$SELF_DIR/dist/otilink-portable-$VERSION"
ARCH=$(uname -m)
BIN_BASELINE=""

log() { printf '  %s\n' "$*"; }

case "$BASELINE" in
    kylin)     BIN_BASELINE="glibc2.31" ;;
    local)     BIN_BASELINE="local" ;;
    docker:*)  BIN_BASELINE="${BASELINE#docker:}" ;;
    glibc*)    BIN_BASELINE="$BASELINE" ;;
    *) echo "--baseline 只支持 kylin|local|docker:<image>|glibc<x.y>"; exit 2 ;;
esac

echo "== 组装绿色包 =="
log "版本     : $VERSION"
log "基线     : $BASELINE（二进制标注 $BIN_BASELINE，架构 $ARCH）"
log "产物目录 : $DIST"
log "Windows  : $([ "$WINDOWS" = 1 ] && echo 编 csc 载荷 || echo 跳过)"
[ "$INFO" = 1 ] && exit 0

SRC_SHA=$(cd "$OTI_DIR" && cat $(ls *.c *.h 2>/dev/null | LC_ALL=C sort) Makefile 2>/dev/null | sha256sum | cut -c1-12)
# 包指纹 = C 源码 + Makefile + **运行期脚本**（入口/README/VERSION）。
# 为什么单列一个：改了 otilink.sh 却没重新打包时，src_sha 不变 → 门禁看不出来，
# 用户拿到的就是"旧入口 + 新二进制"（本轮真踩过：手工把 otilink.sh 补到麒麟上）。
PKG_SHA=$( { cat $(cd "$OTI_DIR" && ls *.c *.h 2>/dev/null | sed "s|^|$OTI_DIR/|" | LC_ALL=C sort) "$OTI_DIR/Makefile" \
             "$SELF_DIR/otilink.sh" "$SELF_DIR/otilink.cmd" "$SELF_DIR/otilink-win.ps1" \
             "$SELF_DIR/mac/otilink-mac.sh" "$SELF_DIR/README.md" "$SELF_DIR/VERSION" 2>/dev/null; } \
           | sha256sum | cut -c1-12)
log "源码指纹 : $SRC_SHA（二进制）  包指纹: $PKG_SHA（含运行期脚本）"

rm -rf "$DIST"
mkdir -p "$DIST/linux/$ARCH" "$DIST/windows" "$DIST/mac"

# ---------------------------------------------------------------- Linux 二进制
case "$BASELINE" in
    local)
        echo "-- 本机编译（基线=开发机，**不许发布**）--"
        make -C "$OTI_DIR" -j4 otikm hidprobe VERSION="$VERSION" BASELINE="$BIN_BASELINE" \
             >/tmp/portable-build.log 2>&1 || { echo "本机 make 失败，见 /tmp/portable-build.log"; exit 1; }
        cp -f "$OTI_DIR/otikm" "$OTI_DIR/hidprobe" "$DIST/linux/$ARCH/"
        ;;
    kylin)
        echo "-- 麒麟编译（声明基线 $BIN_BASELINE）--"
        if ! kssh 'echo up' | grep -q up; then
            echo "SSH 不可达 $KY —— 要么先联网，要么改用 --baseline local（仅测试）"; exit 1
        fi
        # 只清自己那个专用目录里的源码（不做递归删家目录那种写法：门禁黑名单 + L9 的教训）
        kssh 'mkdir -p ~/otilink-portable-src && rm -f ~/otilink-portable-src/*.c ~/otilink-portable-src/*.h ~/otilink-portable-src/Makefile' >/dev/null 2>&1
        # 只同步源码（*.c *.h Makefile），不带仓库里的旧二进制
        FILES=()
        for f in "$OTI_DIR"/*.c "$OTI_DIR"/*.h "$OTI_DIR"/Makefile; do [ -f "$f" ] && FILES+=("$f"); done
        if ! SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force setsid -w scp -q -o StrictHostKeyChecking=no \
             "${FILES[@]}" "$KY:~/otilink-portable-src/" 2>/dev/null; then
            echo "scp 同步源码失败"; exit 1
        fi
        if ! kssh "cd ~/otilink-portable-src && make otikm hidprobe VERSION=$VERSION BASELINE=$BIN_BASELINE 2>&1 | tail -3" ; then
            echo "麒麟侧 make 失败"; exit 1
        fi
        SSH_ASKPASS="$ASKPASS" SSH_ASKPASS_REQUIRE=force setsid -w scp -q -o StrictHostKeyChecking=no \
            "$KY:~/otilink-portable-src/otikm" "$KY:~/otilink-portable-src/hidprobe" "$DIST/linux/$ARCH/" \
            || { echo "取回二进制失败"; exit 1; }
        ;;
    docker:*)
        IMG="${BASELINE#docker:}"
        echo "-- 容器编译（$IMG）--"
        if ! docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
            echo "docker 不可用（Docker Desktop 的 WSL 集成没开？）"; exit 1
        fi
        docker run --rm -v "$OTI_DIR:/src" -w /src "$IMG" sh -c \
            "make clean >/dev/null 2>&1; make otikm hidprobe VERSION=$VERSION BASELINE=$BIN_BASELINE" \
            >/tmp/portable-build.log 2>&1 || { echo "容器 make 失败，见 /tmp/portable-build.log"; exit 1; }
        cp -f "$OTI_DIR/otikm" "$OTI_DIR/hidprobe" "$DIST/linux/$ARCH/"
        ;;
esac
[ -x "$DIST/linux/$ARCH/otikm" ] || { echo "编译产物缺失"; exit 1; }
strip "$DIST/linux/$ARCH/otikm" "$DIST/linux/$ARCH/hidprobe" 2>/dev/null || true
log "Linux 二进制大小: $(du -h "$DIST/linux/$ARCH/otikm" | cut -f1)"

# ---------------------------------------------------------------- Windows 载荷
if [ "$WINDOWS" = 1 ]; then
    echo "-- Windows 载荷（csc，编到产物目录，不碰 C:\\Users\\Public 正在跑的 exe）--"
    CSC="/mnt/c/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
    if [ ! -x "$CSC" ]; then
        echo "  [warn] 找不到 csc.exe：跳过 Windows 载荷（Linux/Mac 部分照常出）"
    else
        WOUT="$(wslpath -w "$DIST/windows/otiagent2.exe")"
        WIN_SRC="$(wslpath -w "$WIN_DIR/otiagent2.cs")"
        if "$CSC" /nologo /target:exe /optimize+ "/out:$WOUT" "$WIN_SRC" >/tmp/portable-csc.log 2>&1; then
            cp -f "$WIN_DIR/otiagent.ps1" "$DIST/windows/"
            log "otiagent2.exe: $(du -h "$DIST/windows/otiagent2.exe" | cut -f1)（BuildTag $(grep -o 'BuildTag = "[^"]*"' "$WIN_DIR/otiagent2.cs" | head -1 | sed 's/.*"\(.*\)"/\1/')）"
        else
            echo "  [warn] csc 编译失败（见 /tmp/portable-csc.log）：Windows 载荷缺失"
            tail -3 /tmp/portable-csc.log | sed 's/^/    /'
        fi
    fi
fi

# ---------------------------------------------------------------- 脚本与文档
cp -f "$SELF_DIR/otilink.sh" "$DIST/"
cp -f "$SELF_DIR/otilink.cmd" "$SELF_DIR/otilink-win.ps1" "$DIST/" 2>/dev/null || true
cp -f "$SELF_DIR/mac/otilink-mac.sh" "$DIST/mac/" 2>/dev/null || true
cp -f "$SELF_DIR/README.md" "$DIST/" 2>/dev/null || true
cp -f "$SELF_DIR/VERSION" "$DIST/"
chmod +x "$DIST/otilink.sh" "$DIST/mac/otilink-mac.sh" 2>/dev/null || true

# 工具链归属：写清楚"这个二进制到底是谁编的"（麒麟编的就别记成开发机的 gcc）
case "$BASELINE" in
    kylin)    TOOLCHAIN=$(kssh 'gcc --version 2>/dev/null | head -1' | tr -d '\r' | sed 's/.*(//;s/).*//') ;;
    docker:*) TOOLCHAIN="container ${BASELINE#docker:}" ;;
    *)        TOOLCHAIN=$(gcc --version 2>/dev/null | head -1 | sed 's/.*(//;s/).*//') ;;
esac
[ -n "$TOOLCHAIN" ] || TOOLCHAIN=unknown

# ---------------------------------------------------------------- BUILD-INFO（门禁据此判定"新鲜度/基线"）
cat > "$DIST/BUILD-INFO" <<EOF
version=$VERSION
src_sha=$SRC_SHA
pkg_sha=$PKG_SHA
baseline=$BIN_BASELINE
baseline_source=$BASELINE
arch=$ARCH
built_at=$(date -Iseconds)
built_on=$(hostname)
toolchain=$TOOLCHAIN
EOF
log "BUILD-INFO:"; sed 's/^/    /' "$DIST/BUILD-INFO"

# ---------------------------------------------------------------- 打包
echo "-- 打包 --"
OUT="$SELF_DIR/dist"
tar -czf "$OUT/otilink-portable-$VERSION.tar.gz" -C "$OUT" "otilink-portable-$VERSION" \
    && log "otilink-portable-$VERSION.tar.gz"
if command -v zip >/dev/null 2>&1; then
    (cd "$OUT" && zip -qr "otilink-portable-$VERSION.zip" "otilink-portable-$VERSION") \
        && log "otilink-portable-$VERSION.zip"
elif command -v powershell.exe >/dev/null 2>&1; then
    # WSL 里通常没装 zip；Windows 用户更需要 .zip（解压后双击 otilink.cmd）
    powershell.exe -NoProfile -Command \
        "Compress-Archive -Path '$(wslpath -w "$DIST")' -DestinationPath '$(wslpath -w "$OUT/otilink-portable-$VERSION.zip")' -Force" \
        >/dev/null 2>&1 && log "otilink-portable-$VERSION.zip（Compress-Archive）" \
        || log "[warn] 打 zip 失败：Windows 用户可直接复制目录"
else
    log "[warn] 没有 zip 也没有 powershell.exe：只出了 tar.gz（Windows 用户可直接复制目录）"
fi
echo
echo "== 完成 =="
echo "  下一步：./portablecheck.sh（门禁）或 cd re/tools && ./gate.sh --portable"
