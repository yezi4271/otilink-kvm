#!/bin/sh
# uninstall-kylin.sh —— 卸载麒麟/Ubuntu 端的安装内容（需要 sudo 密码）
#
# 会删除：udev 规则、usb-storage 复位标志（假光盘隐藏）、模块加载配置、开机自启项。
# 不会删除：程序目录本身、~/.config/otilink/kvm.conf（如要一并删，加 --purge）
set -e
if [ "$(id -u)" != "0" ]; then
    echo "需要 root：sudo sh $0 [--purge]"
    exit 1
fi
TARGET=${SUDO_USER:-$USER}
HOME_DIR=$(getent passwd "$TARGET" | cut -d: -f6)

echo "== 删除 udev 规则 =="
rm -f /etc/udev/rules.d/99-otilink.rules
udevadm control --reload || true
udevadm trigger || true

echo "== 删除 usb-storage 复位标志（下次重插/重启后恢复枚举 LUN0+LUN1）=="
rm -f /etc/modprobe.d/otilink-quirks.conf

echo "== 删除模块加载配置 =="
rm -f /etc/modules-load.d/otilink.conf

echo "== 删除开机自启 =="
rm -f "$HOME_DIR/.config/autostart/otilink-kvm.desktop" \
      "$HOME_DIR/.config/autostart/otilink-kvm.desktop.off"

if [ "$1" = "--purge" ]; then
    echo "== 删除配置 =="
    rm -rf "$HOME_DIR/.config/otilink"
fi

cat <<EOF

已卸载。组关系（input,disk）没有回退 —— 如需回退：
  sudo gpasswd -d $TARGET input
  sudo gpasswd -d $TARGET disk
正在运行的实例请先退出：pkill -x otikm
EOF
