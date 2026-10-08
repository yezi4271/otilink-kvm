#!/bin/sh
# bringup-linux.sh —— Linux（主控端）一键 bring-up
#
# 用法（需要 root：uinput 注入与 EVIOCGRAB 独占都要权限）：
#   sudo sh bringup-linux.sh                # 默认：HID 直发模式（被控端零软件）+ 剪贴板
#   sudo sh bringup-linux.sh --calibrate    # 先标定 HID 布局（发候选包，请在 Windows 侧观察）
#   sudo sh bringup-linux.sh --proto        # 用我们自己的协议（两端都要软件）
#   sudo sh bringup-linux.sh --kbd-layout 1 --mouse-layout 2
#
# 需要同目录下有可执行文件：
#   otiprobe   单文件探测/标定工具（re/otilink/otiprobe.c 编译，或直接拷贝静态二进制）
#   otikm      键鼠/剪贴板守护进程（re/otilink/ 编译产物）
set -u
cd "$(dirname "$0")"

MODE=hid
CALIB=0
KBD=0
MOUSE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --calibrate) CALIB=1 ;;
        --proto) MODE=proto ;;
        --hid) MODE=hid ;;
        --kbd-layout) KBD=${2:-0}; shift ;;
        --mouse-layout) MOUSE=${2:-0}; shift ;;
        *) echo "未知参数: $1"; exit 2 ;;
    esac
    shift
done

err() { echo "[!] $1" >&2; }

[ "$(id -u)" = 0 ] || { err "需要 root：sudo sh bringup-linux.sh"; exit 1; }
[ -x ./otiprobe ] || { err "找不到 ./otiprobe（先编译 otiprobe.c 或拷入静态二进制）"; exit 1; }
[ -x ./otikm ] || { err "找不到 ./otikm（先在 re/otilink 里 make）"; exit 1; }

echo "== 1) 找对拷线 =="
./otiprobe infoall || { err "没找到 0ea0:2213 的 /dev/sgN —— 线是否插在本机？"; exit 1; }

if [ "$CALIB" = 1 ]; then
    echo
    echo "== 2) HID 布局标定（请在 Windows 被控端观察光标与记事本）=="
    echo "   每个候选间隔 2 秒，脚本会打印编号；"
    echo "   光标在第几号候选后跳动 -> 用 --mouse-layout；出现字母的那个 -> --kbd-layout"
    ./otiprobe hidseq
    echo
    echo "标定完成后重新运行：sudo sh bringup-linux.sh --mouse-layout N --kbd-layout M"
    exit 0
fi

echo
echo "== 2) 选输入设备 =="
KBD_DEV=""
MOU_DEV=""
for f in /dev/input/by-id/*-event-kbd; do [ -e "$f" ] && KBD_DEV=$f && break; done
for f in /dev/input/by-id/*-event-mouse; do [ -e "$f" ] && MOU_DEV=$f && break; done
if [ -z "$KBD_DEV" ] && [ -z "$MOU_DEV" ]; then
    err "没找到 /dev/input/by-id/*-event-kbd|mouse 符号链接"
    echo "    可手动指定：先看设备名（下面列出 event*），再改脚本用 --capture 形式的参数"
    ls /dev/input/ 2>/dev/null | head -20
    exit 1
fi
CAPS=""
[ -n "$KBD_DEV" ] && CAPS="$CAPS --capture $KBD_DEV"
[ -n "$MOU_DEV" ] && CAPS="$CAPS --capture $MOU_DEV"
echo "    键盘: ${KBD_DEV:-（未找到）}"
echo "    鼠标: ${MOU_DEV:-（未找到）}"

echo
echo "== 3) 启动守护进程（模式: $MODE）=="
if [ "$MODE" = hid ]; then
    echo "    指针撞屏幕边缘开始控制 Windows；热键（默认 KEY_LEFTCTRL）拉回本机"
    exec ./otikm --transport cable --peer hid --cable-init --clipboard --grab \
        --hid-kbd-layout "$KBD" --hid-mouse-layout "$MOUSE" $CAPS
else
    echo "    注意：--proto 模式要求对端也跑我们的实现（Windows 侧 otiagent.ps1）"
    exec ./otikm --transport cable --peer proto --cable-init --clipboard --grab \
        --inject $CAPS
fi
