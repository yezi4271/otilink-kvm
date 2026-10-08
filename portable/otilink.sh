#!/bin/bash
# otilink.sh —— 对拷线 Linux 侧「免安装绿色包」唯一入口
#
# 目标（见 re/portable/README.md）：
#   把线缆插到**任何** x86_64 Linux 上，一条命令就能用 ——
#   不装 udev 规则、不加用户组、不装服务、不改 /etc、不编译；
#   需要设备权限时**只提一次权**（sudo/pkexec），其他什么都由这个脚本自动搞定：
#     ① 找线缆（sysfs 里按 0ea0:2213 认，不假设 /dev/sg3）
#     ② 卸掉线缆自己的 LUN（厂商 Initialize() 的第一步就是 UnmountMedia）
#     ③ 找到用户的桌面会话环境（root 直跑时 DISPLAY/XAUTHORITY 是空的 ——
#        被驱动侧"撞边交还"靠真实光标，丢了它就会"回不去"）
#     ④ 判定角色（本机有键鼠=主控端；没有=被驱动侧）、屏幕尺寸、剪贴板后端
#     ⑤ 打印"我做了什么"，然后把 otikm 跑起来
#
# 用法：
#   ./otilink.sh                    # 自动判定角色并启动（最常用）
#   ./otilink.sh --doctor           # 只体检（不需要 root），把缺什么打出来
#   ./otilink.sh --print-plan       # 只打印判定结果与将要执行的命令，不启动
#   ./otilink.sh --role slave       # 显式指定角色（笔记本两侧都有键鼠时必须显式）
#   ./otilink.sh --peer-side left   # 对端屏幕在本机的左边（决定撞边/回程边）
#   ./otilink.sh --no-clip          # 不要剪贴板（只要键鼠）
#   ./otilink.sh -- --verbose       # '--' 之后的参数原样交给 otikm
#
# 退出码：0 正常；2 用法错误；其它 = 启动失败（原因会打在 stderr）
set -u

SELF_DIR=$(cd "$(dirname "$0")" && pwd) || exit 2
ARCH=$(uname -m)
ROLE=auto
PEER_SIDE=""
NO_CLIP=0
NO_GRAB=0
MODE=run                 # run | doctor | plan
ELEVATE=auto             # auto | sudo | pkexec | never
LOG_DEFAULT="/tmp/otilink-$(id -u).log"
LOG=""
OTIKM_EXTRA=()

usage() {
    sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'EOF'

环境变量：
  OTILINK_OTIKM        指定 otikm 路径（默认找 ./linux/<arch>/otikm）
  OTILINK_NO_UNMOUNT=1 不要自动卸载线缆的 LUN（默认会卸，厂商要求传输前卸载）
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --doctor)      MODE=doctor ;;
        --version)     MODE=version ;;
        --print-plan)  MODE=plan ;;
        --role)        ROLE="${2:-}"; shift || true ;;
        --peer-side)   PEER_SIDE="${2:-}"; shift || true ;;
        --no-clip)     NO_CLIP=1 ;;
        --no-grab)     NO_GRAB=1 ;;
        --log)         LOG="${2:-}"; shift || true ;;
        --no-elevate)  ELEVATE=never ;;
        --elevate)     ELEVATE="${2:-}"; shift || true ;;
        -h|--help)     usage; exit 0 ;;
        --)            shift; OTIKM_EXTRA=("$@"); break ;;
        *) echo "未知参数：$1（--help 看用法）" >&2; exit 2 ;;
    esac
    shift
done

case "$ROLE" in auto|master|slave) ;; *) echo "--role 只支持 auto|master|slave" >&2; exit 2 ;; esac
case "$PEER_SIDE" in ""|left|right|top|bottom) ;; *) echo "--peer-side 只支持 left|right|top|bottom" >&2; exit 2 ;; esac

# ---------------------------------------------------------------- 找二进制
find_otikm() {
    if [ -n "${OTILINK_OTIKM:-}" ] && [ -x "${OTILINK_OTIKM}" ]; then
        printf '%s\n' "$OTILINK_OTIKM"; return 0
    fi
    for c in "$SELF_DIR/linux/$ARCH/otikm" "$SELF_DIR/otikm" "$SELF_DIR/../otilink/otikm"; do
        [ -x "$c" ] && { printf '%s\n' "$c"; return 0; }
    done
    return 1
}

# ---------------------------------------------------------------- 找线缆（不假设 /dev/sgN）
cable_sg() {
    for s in /sys/class/scsi_generic/sg*; do
        [ -e "$s" ] || continue
        d=$(readlink -f "$s" 2>/dev/null) || continue
        n=0
        while [ -n "$d" ] && [ "$d" != "/" ] && [ "$n" -lt 12 ]; do
            if [ -r "$d/idVendor" ] && [ -r "$d/idProduct" ]; then
                if [ "$(cat "$d/idVendor" 2>/dev/null)" = "0ea0" ] && \
                   [ "$(cat "$d/idProduct" 2>/dev/null)" = "2213" ]; then
                    printf '%s\n' "/dev/$(basename "$s")"; break
                fi
            fi
            n=$((n + 1)); d=$(dirname "$d")
        done
    done
}

# 线缆的块设备分区（用于卸载）：只认 idVendor=0ea0 的 USB 设备
cable_mounts() {
    [ -r /proc/mounts ] || return 0
    while read -r dev mnt _; do
        case "$dev" in /dev/*) ;; *) continue ;; esac
        base=$(basename "$dev")
        base=$(printf '%s' "$base" | sed 's/[0-9]*$//')      # sdb1 → sdb
        [ -n "$base" ] || continue
        [ -r "/sys/block/$base/device/idVendor" ] || continue
        [ "$(cat "/sys/block/$base/device/idVendor" 2>/dev/null)" = "0ea0" ] || continue
        printf '%s %s\n' "$dev" "$mnt"
    done < /proc/mounts
}

# ---------------------------------------------------------------- 桌面会话环境
# root 直接跑时这些变量是空的；被驱动侧要靠 XQueryPointer 读真实光标，
# 没有 DISPLAY/XAUTHORITY 就会"撞边没反应、回不去"。所以从**用户的会话进程**里捞。
detect_session_env() {
    local uid="${SUDO_UID:-$(id -u)}"
    local best=""
    if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
        best=$(printf 'DISPLAY=%s\nWAYLAND_DISPLAY=%s\nXAUTHORITY=%s\nXDG_RUNTIME_DIR=%s\nXDG_SESSION_TYPE=%s\n' \
            "${DISPLAY:-}" "${WAYLAND_DISPLAY:-}" "${XAUTHORITY:-}" "${XDG_RUNTIME_DIR:-}" "${XDG_SESSION_TYPE:-}")
    fi
    if [ -z "$best" ]; then
        for p in $(ls -d /proc/[0-9]* 2>/dev/null); do
            [ -r "$p/environ" ] || continue
            owner=$(stat -c %u "$p" 2>/dev/null) || continue
            [ "$owner" = "$uid" ] || continue
            envdump=$(tr '\0' '\n' < "$p/environ" 2>/dev/null) || continue
            d=$(printf '%s\n' "$envdump" | grep -m1 '^DISPLAY=' || true)
            w=$(printf '%s\n' "$envdump" | grep -m1 '^WAYLAND_DISPLAY=' || true)
            if [ -z "$d" ] && [ -z "$w" ]; then continue; fi
            x=$(printf '%s\n' "$envdump" | grep -m1 '^XAUTHORITY=' || true)
            r=$(printf '%s\n' "$envdump" | grep -m1 '^XDG_RUNTIME_DIR=' || true)
            t=$(printf '%s\n' "$envdump" | grep -m1 '^XDG_SESSION_TYPE=' || true)
            best=$(printf '%s\n%s\n%s\n%s\n%s\n' "$d" "$w" "$x" "$r" "$t")
            break
        done
    fi
    if [ -z "${XAUTHORITY:-}" ]; then
        for c in "$HOME/.Xauthority" "/run/user/$uid/gdm/Xauthority" "/run/user/$uid/lightdm/xauthority" \
                 "/run/user/$uid/sddm/xauthority" "/run/user/$uid/.Xauthority"; do
            [ -f "$c" ] && { best="$best
XAUTHORITY=$c"; break; }
        done
    fi
    printf '%s\n' "$best" | grep -v '=$' | grep -v '^$' || true
}

session_value() {           # session_value <ENV 行列表> <KEY>
    printf '%s\n' "$1" | grep -m1 "^$2=" | cut -d= -f2- || true
}

# ---------------------------------------------------------------- 主流程
OTIKM=$(find_otikm) || {
    echo "找不到 otikm 可执行文件（架构 $ARCH）。" >&2
    echo "  期望：$SELF_DIR/linux/$ARCH/otikm" >&2
    echo "  包内容：" >&2
    ls -1 "$SELF_DIR/linux" 2>/dev/null | sed 's/^/    linux\//' >&2
    exit 1
}

VER=$("$OTIKM" --version 2>/dev/null | head -1 || true)
VBUILD=$("$OTIKM" --version 2>/dev/null | sed -n '2p' || true)
[ -n "$VER" ] || { echo "运行 $OTIKM --version 失败（架构/glibc 不匹配？）" >&2; exit 1; }

SGS=$(cable_sg || true)
MOUNTS=$(cable_mounts || true)
ENVS=$(detect_session_env || true)
SESS=$(session_value "$ENVS" XDG_SESSION_TYPE)
DISP=$(session_value "$ENVS" DISPLAY)
WDISP=$(session_value "$ENVS" WAYLAND_DISPLAY)

echo "== otilink 绿色包 =="
echo "包位置    : $SELF_DIR"
echo "二进制    : $OTIKM"
echo "版本      : $VER  [$VBUILD]"
echo "线缆      : $(printf '%s' "${SGS:-未发现（0ea0:2213 的 /dev/sgN 不存在）}" | tr '\n' ' ')"
if [ -n "$MOUNTS" ]; then
    echo "线缆卷挂载: $(printf '%s' "$MOUNTS" | tr '\n' ' ')（厂商要求传输前卸载）"
else
    echo "线缆卷挂载: 无（符合厂商 UnmountMedia 前提）"
fi
echo "会话      : type=${SESS:-未知} DISPLAY=${DISP:-无} WAYLAND_DISPLAY=${WDISP:-无}"

if [ "$MODE" = version ]; then
    echo "otilink-portable $(cat "$SELF_DIR/VERSION" 2>/dev/null)"
    "$OTIKM" --version
    exit 0
fi

if [ "$MODE" = doctor ]; then
    echo
    echo "---- 环境自检（otikm --doctor，不需要 root；FAIL 项在提权后通常可解）----"
    if [ -n "$SGS" ]; then
        "$OTIKM" --doctor --transport "cable:$SGS"
    else
        "$OTIKM" --doctor
    fi
    rc=$?
    echo
    echo "---- 脚本侧检查 ----"
    if [ -n "$SGS" ]; then echo "  [OK]   线缆可见：$SGS"
    else echo "  [FAIL] 线缆不可见：换 USB 口/确认这端插的是对拷线"; fi
    if [ -n "$DISP" ] || [ -n "$WDISP" ]; then
        echo "  [OK]   桌面会话环境可探测（被驱动侧撞边判据需要它）"
    else
        echo "  [WARN] 没探测到桌面会话：被驱动侧会退化成位移积分判据（Wayland/无 X 时的降级路径）"
    fi
    if [ "$(id -u)" = 0 ]; then
        echo "  [OK]   当前是 root：设备权限没问题"
    elif [ -n "${SUDO_UID:-}" ]; then
        echo "  [OK]   经 sudo 运行：设备权限没问题"
    else
        echo "  [INFO] 当前不是 root：设备权限等提权时自动处理（脚本会 sudo/pkexec）"
    fi
    [ "$rc" -eq 0 ] && echo "== 自检通过（提权后即可直接 ./otilink.sh 启动）==" \
                    || echo "== 自检有未通过项：多数需要提权（见上面 FAIL 的箭头提示）=="
    exit 0
fi

# 一条链路只能有一个主人（L4）：多实例会互相抢设备授权，表现是"两边都怪怪的"。
# 注意：绿色包是以 root 跑的，普通用户的 pkill -x otikm 杀不掉它 → 必须 sudo。
RUNNING=$(pgrep -x otikm 2>/dev/null | tr '\n' ' ')
if [ -n "$RUNNING" ] && [ -z "$SGS" ]; then
    # 没有对拷线 → 抢不了这条链路（多半是本机测试残留的实例）：提醒但不拦
    echo "[warn] 本机还有 otikm 在跑（pid: $RUNNING），但没检测到线缆 —— 不拦你，继续。"
    RUNNING=""
fi
if [ -n "$RUNNING" ] && [ "${OTILINK_FORCE:-0}" != 1 ]; then
    echo
    echo "[ERR] 已经有 otikm 在跑（pid: $RUNNING）—— 一条链路只能有一个主人（AGENTS L4）。" >&2
    echo "      先停掉它（绿色包是 root 起的，普通 pkill 杀不掉）：" >&2
    echo "        sudo pkill -x otikm        # 或 sudo kill $(echo $RUNNING | tr ' ' '\n' | head -1)" >&2
    echo "      确认停了再重跑本脚本；确实要强起：OTILINK_FORCE=1 ./otilink.sh" >&2
    exit 1
fi

# 角色：交互式问一次并记住，非交互用 otikm 的启发式（--role auto）
if [ "$ROLE" = auto ] && [ -t 0 ] && [ -n "$SGS" ]; then
    RT="${XDG_RUNTIME_DIR:-/tmp}/otilink-role-$(id -u)"
    CACHED=""
    [ -r "$RT" ] && CACHED=$(cat "$RT" 2>/dev/null || true)
    if [ -n "$CACHED" ]; then
        ROLE="$CACHED"
        echo "角色      : $ROLE（记住的选择：$RT；要改就删掉它或传 --role）"
    else
        echo
        echo "键盘鼠标插在哪一侧？"
        echo "  1) 本机（这台 Linux 控制对端）   → master 主控端"
        echo "  2) 对端（对端控制这台 Linux）    → slave 被驱动侧"
        printf "请选择 [1/2]（10 秒不选=自动判定）："
        read -r -t 10 ans || ans=""
        case "$ans" in
            1) ROLE=master ;;
            2) ROLE=slave ;;
            *) ROLE=auto ;;
        esac
        if [ "$ROLE" != auto ]; then printf '%s\n' "$ROLE" > "$RT" 2>/dev/null || true; fi
        echo
    fi
fi

ARGS=(--auto --role "$ROLE")
[ "$NO_CLIP" = 1 ] && ARGS+=(--no-clipboard)
[ "$NO_GRAB" = 1 ] && ARGS+=(--no-grab)
[ -n "$PEER_SIDE" ] && ARGS+=(--edges "$PEER_SIDE" --return-edge "$PEER_SIDE")
[ -n "$LOG" ] || LOG="$LOG_DEFAULT"
ARGS+=(--log "$LOG")
if [ "${#OTIKM_EXTRA[@]}" -gt 0 ]; then ARGS+=("${OTIKM_EXTRA[@]}"); fi

if [ "$MODE" = plan ]; then
    ENVLINE=""
    [ -n "$ENVS" ] && ENVLINE=$(printf '%s' "$ENVS" | tr '\n' ' ')
    echo "角色参数  : --role $ROLE"
    echo "提权方式  : $ELEVATE（当前 uid=$(id -u)）"
    echo "桌面环境  : ${ENVLINE:-（未探测到）}"
    echo "日志      : $LOG"
    echo "将执行    : ${ENVS:+env }$OTIKM ${ARGS[*]}"
    echo
    "$OTIKM" --print-plan "${ARGS[@]}" 2>&1 | grep -v '^$'
    exit 0
fi

# 卸载线缆自己的 LUN（厂商 Initialize() 第一步 = UnmountMedia）。只卸 0ea0:2213 的，
# 且先把清单打印出来 —— 绝不做"范围删除"（见 AGENTS L9 的教训）。
if [ -n "$MOUNTS" ] && [ "${OTILINK_NO_UNMOUNT:-0}" != 1 ]; then
    echo "== 需要先卸载线缆的卷（厂商前提）=="
    printf '%s\n' "$MOUNTS" | sed 's/^/  /'
fi

ENVARGS=()
if [ -n "$ENVS" ]; then
    while IFS= read -r line; do
        [ -n "$line" ] && ENVARGS+=("$line")
    done <<< "$ENVS"
fi

if [ "$(id -u)" = 0 ]; then
    if [ -n "$MOUNTS" ] && [ "${OTILINK_NO_UNMOUNT:-0}" != 1 ]; then
        printf '%s\n' "$MOUNTS" | while read -r dev mnt; do
            if umount "$mnt" 2>/dev/null; then echo "已卸载 $mnt"; else echo "[warn] 卸载 $mnt 失败（继续尝试）"; fi
        done
    fi
    echo "启动: $OTIKM ${ARGS[*]}"
    cd /          # 别让守护进程占住包目录（U盘要能拔；包目录被重建时也不会刷 getcwd 错）
    if [ "${#ENVARGS[@]}" -gt 0 ]; then
        exec env "${ENVARGS[@]}" "$OTIKM" "${ARGS[@]}"
    fi
    exec "$OTIKM" "${ARGS[@]}"
fi

if [ "$ELEVATE" = never ]; then
    echo "非 root 且 --no-elevate：只能跑到这里。" >&2
    echo "  办法一：sudo $0 $*   办法二：装 udev 规则（re/otilink/install-kylin.sh）" >&2
    exit 1
fi

# 提权：一次 sudo/pkexec；把桌面会话环境显式带过去（root 下 DISPLAY/XAUTHORITY 是空的）
SUDO=""
PKEXEC=""
if [ "$ELEVATE" != pkexec ] && command -v sudo >/dev/null 2>&1; then SUDO=sudo; fi
if [ -z "$SUDO" ] && command -v pkexec >/dev/null 2>&1; then PKEXEC=pkexec; fi
if [ -z "$SUDO" ] && [ -z "$PKEXEC" ]; then
    echo "既没有 sudo 也没有 pkexec：无法取得设备权限。" >&2
    echo "  办法一：以 root 运行本脚本   办法二：装 udev 规则（re/otilink/install-kylin.sh）" >&2
    exit 1
fi

if [ -n "$SUDO" ]; then
    if ! sudo -n true 2>/dev/null; then
        if [ ! -t 0 ]; then
            echo "需要 root 权限（读线缆/输入设备），但当前不是交互终端，sudo 问不了密码。" >&2
            echo "  请在终端里跑：sudo $0 $*" >&2
            echo "  或先执行一次 sudo -v 缓存凭据。" >&2
            exit 1
        fi
        echo "需要 root 权限读线缆/输入设备（不安装任何东西，只提权这一次）"
    fi
    echo "启动（sudo）: $OTIKM ${ARGS[*]}"
    echo "停止：sudo pkill -x otikm（绿色包以 root 跑，普通用户杀不掉）"
    if [ "${#ENVARGS[@]}" -gt 0 ]; then
        exec sudo env "${ENVARGS[@]}" sh -c '
            if [ "${OTILINK_NO_UNMOUNT:-0}" != 1 ]; then
                awk "\$1 ~ /^\/dev\// {print \$1, \$2}" /proc/mounts 2>/dev/null | while read -r dev mnt; do
                    base=$(basename "$dev"); base=$(printf "%s" "$base" | sed "s/[0-9]*$//")
                    [ -r "/sys/block/$base/device/idVendor" ] || continue
                    [ "$(cat "/sys/block/$base/device/idVendor" 2>/dev/null)" = "0ea0" ] || continue
                    umount "$mnt" 2>/dev/null && echo "已卸载 $mnt（对拷线卷）"
                done
            fi
            exec "$@"
        ' sh "$OTIKM" "${ARGS[@]}"
    fi
    exec sudo sh -c '
        if [ "${OTILINK_NO_UNMOUNT:-0}" != 1 ]; then
            awk "\$1 ~ /^\/dev\// {print \$1, \$2}" /proc/mounts 2>/dev/null | while read -r dev mnt; do
                base=$(basename "$dev"); base=$(printf "%s" "$base" | sed "s/[0-9]*$//")
                [ -r "/sys/block/$base/device/idVendor" ] || continue
                [ "$(cat "/sys/block/$base/device/idVendor" 2>/dev/null)" = "0ea0" ] || continue
                umount "$mnt" 2>/dev/null && echo "已卸载 $mnt（对拷线卷）"
            done
        fi
        exec "$@"
    ' sh "$OTIKM" "${ARGS[@]}"
fi

echo "启动（pkexec）: $OTIKM ${ARGS[*]}"
if [ "${#ENVARGS[@]}" -gt 0 ]; then
    exec env "${ENVARGS[@]}" pkexec "$OTIKM" "${ARGS[@]}"
fi
exec pkexec "$OTIKM" "${ARGS[@]}"
