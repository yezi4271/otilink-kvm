#!/bin/bash
# otilink-mac.sh —— macOS 侧**只读体检**（对拷线「免安装」的一部分）
#
# macOS 在本项目里的定位（先说结论，避免误解）：
#   * **Mac 作为"接收端"（被驱动侧）：不需要装任何东西** —— 线缆对接收端就是
#     一个真实的 USB 鼠标 + 键盘（原生 HID，任何有 USB HID 驱动的系统都认），
#     键鼠由对端（Linux 主控端 otikm / Windows 主控端 otiagent2.exe）发 HID 包驱动。
#   * **Mac 作为"主控端"（把 Mac 的键鼠送给对端）：本轮不支持** —— 需要 IOKit
#     SCSITaskUserClient 直通（私有 SCSI 命令）+ CGEventTap（需要辅助功能授权）
#     + NSPasteboard，是一块独立工作量，没有 Mac 真机无法验证（见 re/portable/README.md）。
#   * **剪贴板/大文件**：需要两端都跑我们的代理；macOS 侧没有代理，所以**没有**剪贴板同步。
#
# 这个脚本只做"看"：不安装、不提权、不改任何设置、不杀任何进程。
#
# 用法： ./otilink-mac.sh
set -u

echo "== otilink macOS 只读体检 =="
echo "系统: $(sw_vers -productVersion 2>/dev/null || echo 未知) ($(uname -m))"

# ---------------------------------------------------------------- 1) 线缆在不在
echo
echo "-- 1) 线缆是否枚举（USB 0ea0:2213 / 厂商产品名）--"
USB=$(system_profiler SPUSBDataType 2>/dev/null || true)
if [ -z "$USB" ]; then
    echo "  [warn] system_profiler 没有输出（沙箱/权限？）"
fi
HIT=$(printf '%s\n' "$USB" | grep -iE '0ea0|2213|VirtualLink|Android\+Mac|WinDroid|MacKMLink|Transfer line' || true)
if [ -n "$HIT" ]; then
    echo "  [OK] 找到线缆相关条目："
    printf '%s\n' "$HIT" | sed 's/^/       /' | head -20
else
    echo "  [FAIL] 没看到线缆：换 USB 口（直插，别过 hub）、确认另一端插在开机的主控端"
fi

# ---------------------------------------------------------------- 2) HID 接口
echo
echo "-- 2) 线缆的 HID 接口（接收端零软件的前提）--"
HID=$(ioreg -c IOHIDDevice -r -l 2>/dev/null | grep -iE '0xea0|"Product" = "(Android|VirtualLink|MacKM|Transfer)' || true)
if [ -n "$HID" ]; then
    echo "  [OK] HID 设备在位（对端发 HID 包时，macOS 会像用本地鼠标/键盘一样响应）"
    printf '%s\n' "$HID" | head -8 | sed 's/^/       /'
else
    echo "  [warn] 没在 IOHIDDevice 里认出线缆（可能只是命名不同）—— 看 1) 是否枚举成功"
fi

# ---------------------------------------------------------------- 3) 厂商程序
echo
echo "-- 3) 厂商程序（有的话必须关掉：一条链路只能有一个主人）--"
VP=$(pgrep -fl 'MacKMLink|LEWD|LinkEngKM|SKLoader|OTiLink|WinDroid' 2>/dev/null || true)
if [ -n "$VP" ]; then
    echo "  [WARN] 厂商进程在跑（会和我们的实现抢同一条链路，抢起来只能拔线）："
    printf '%s\n' "$VP" | sed 's/^/       /'
    echo "         → 退出厂商 App / 在 Dock 上退出，再重跑本体检"
else
    echo "  [OK] 没看到厂商进程"
fi
if ls /Applications 2>/dev/null | grep -qiE 'MacKM|OTi|WinDroid|VirtualLink|Transfer'; then
    echo "  [info] /Applications 里有厂商 App（装过就建议别同时开）"
fi

# ---------------------------------------------------------------- 4) 怎么用
echo
echo "-- 4) 作为接收端怎么用（零软件）--"
cat <<'EOF'
   1. 把线缆这一端插到 Mac（另一端插到主控端：Linux 跑 otikm --auto，或 Windows 跑 otilink.cmd master）
   2. 主控端把指针推到自己屏幕的边缘 → 控制权交给 Mac：Mac 上应该出现一个可用的鼠标 + 键盘
   3. 想收回：在主控端按热键（Linux: Ctrl+Alt+←；Windows: Ctrl+Alt+→）
      —— 主控端会检测"在对端屏幕上继续往外推"，所以把指针往 Mac 屏幕外侧推也行
   4. 剪贴板/大文件：macOS 侧没有我们的代理，**本轮不支持**
EOF

echo
echo "== 体检结束（本脚本没有改动任何东西）=="
echo "把上面的输出贴回给会话，就能判断这台 Mac 能不能当接收端用。"
