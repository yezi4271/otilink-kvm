#!/bin/sh
# install-kylin.sh —— 麒麟/Ubuntu 端一次性安装（需要 sudo 密码）
#
# 做五件事：
#   1) 装 udev 规则：允许普通用户抓取键鼠、创建 uinput 注入设备、访问对拷线 sg 设备，
#      并抑制对拷线两个 LUN 的自动挂载（厂商传输前提是 UnmountMedia）
#   2) 装 usb-storage 复位标志（/etc/modprobe.d/otilink-quirks.conf，quirks=0ea0:2213:s）：
#      让内核**只枚举 LUN0**（1MB 共享卷），不枚举 LUN1（那张读不出来的“假光盘”）。
#      ⚠️ 绝对不要用“解绑 usb-storage 接口”的办法隐藏假光盘 —— 传输走的裸 SCSI 通用设备
#      /dev/sgN 就在同一个接口上，解绑会把它一起干掉（2026-09-22 真机踩过，见 NOTES §62）。
#   3) 把当前用户加入 input,disk 组（注销重登后生效）
#   4) 加载 uinput 模块
#   5) 安装默认配置文件 ~/.config/otilink/kvm.conf（已存在则不覆盖）
#
# 运行：sudo sh install-kylin.sh
set -e
D=$(cd "$(dirname "$0")" && pwd)

if [ "$(id -u)" != "0" ]; then
    echo "需要 root：sudo sh $0"
    exit 1
fi
TARGET=${SUDO_USER:-$USER}
HOME_DIR=$(getent passwd "$TARGET" | cut -d: -f6)

echo "== 1/5 安装 udev 规则 =="
[ -f "$D/99-otilink.rules" ] || { echo "缺少 $D/99-otilink.rules"; exit 1; }
cp "$D/99-otilink.rules" /etc/udev/rules.d/
udevadm control --reload
udevadm trigger

echo "== 2/5 安装 usb-storage 复位标志（只枚举 LUN0，隐藏假光盘）=="
if [ -f "$D/otilink-quirks.conf" ]; then
    cp "$D/otilink-quirks.conf" /etc/modprobe.d/otilink-quirks.conf
    echo "    已装 /etc/modprobe.d/otilink-quirks.conf"
    if lsmod | grep -q '^usb_storage'; then
        echo "    ⚠️ usb-storage 已加载：本参数要重插线缆/重启才对已有设备生效"
        echo "       免重启生效：① echo 0ea0:2213:s | sudo tee /sys/module/usb_storage/parameters/quirks"
        echo "                    ② 重绑线缆的 1.0 接口（现查 USB 路径，步骤见 re/NOTES.md §62）"
    fi
else
    echo "    缺少 $D/otilink-quirks.conf（跳过）"
fi

echo "== 3/5 把 $TARGET 加入 input,disk 组 =="
usermod -aG input,disk "$TARGET"

echo "== 4/5 加载 uinput 模块 =="
modprobe uinput 2>/dev/null || true
echo 'uinput' > /etc/modules-load.d/otilink.conf

echo "== 5/5 安装默认配置 =="
mkdir -p "$HOME_DIR/.config/otilink"
if [ -f "$HOME_DIR/.config/otilink/kvm.conf" ]; then
    echo "    已存在 $HOME_DIR/.config/otilink/kvm.conf，保持不动"
elif [ -f "$D/kvm.conf" ]; then
    cp "$D/kvm.conf" "$HOME_DIR/.config/otilink/kvm.conf"
    chown -R "$TARGET" "$HOME_DIR/.config/otilink"
    echo "    已安装到 $HOME_DIR/.config/otilink/kvm.conf"
else
    echo "    未找到 kvm.conf 模板（将使用内置默认值）"
fi

cat <<EOF

安装完成。还需要两步：

  1) **注销并重新登录**（组变更只在登录时读取；或重启一次让 usb-storage 复位标志生效）
  2) 验证：cd $D && ./otikm --doctor

然后启动主控端：
  cd $D && ./run-kylin.sh

默认热键（可在 ~/.config/otilink/kvm.conf 里改）：
  ctrl+alt+space  切换控制权        ctrl+alt+left   拉回本机
  ctrl+alt+right  交给对端          ctrl+alt+l      锁定/解锁到本机
另外把指针推到屏幕左/右边缘也会交出控制权（角部 40px 有死区，防误切）。
EOF

# 给二进制打 KySec 信任标签（否则读不了 ~/文档 里的图片，见 label-kysec.sh 注释）
[ -x ./label-kysec.sh ] && ./label-kysec.sh
