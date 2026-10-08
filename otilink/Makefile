CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11
BINS     = probe selftest_proto e2e_test mocktest otikm cabletest hidprobe uisim

# ---- 版本指纹与构建基线（绿色包发布用；见 re/portable/README.md）------------
# 发布二进制必须用"声明基线"编译（越老的 glibc 越通用）：
#   BASELINE=kylin      麒麟 glibc 2.31（覆盖 Kylin/UOS/Ubuntu 20.04+/Debian 11+/RHEL9/Fedora 32+）
#   BASELINE=gcc7       容器 gcc:7 → glibc 2.24（覆盖 Ubuntu 16.10+/Debian 9+）
#   BASELINE=local      开发机（**只用于测试，不许发布**）
# VERSION 与 re/portable/VERSION 保持一致（portablecheck.sh 会核对）。
VERSION    ?= 0.1
BASELINE   ?= local
ARCH       ?= $(shell uname -m)
BUILD_DATE ?= $(shell date +%Y-%m-%d)
SRC_SHA    ?= $(shell cat $(sort $(wildcard *.c *.h)) Makefile 2>/dev/null | sha256sum | cut -c1-12)
override CFLAGS += -DOTI_VERSION='"$(VERSION)"' \
                   -DOTI_BUILD_TAG='"baseline=$(BASELINE) arch=$(ARCH) src=$(SRC_SHA) date=$(BUILD_DATE)"'

all: $(BINS)

# 标定 CLI（线缆通道）
probe: probe.c otilink.c otiinput.c oticlip.c otiproto.c otilink.h otiinput.h oticlip.h otiproto.h
	$(CC) $(CFLAGS) -o $@ probe.c otilink.c otiinput.c oticlip.c otiproto.c

# 消息层 + 状态机单测（无硬件）
selftest_proto: selftest_proto.c otiproto.c otikm_core.c otihid.c otiproto.h otikm_core.h otihid.h
	$(CC) $(CFLAGS) -o $@ selftest_proto.c otiproto.c otikm_core.c otihid.c

# cable 传输路径（软件仿真设备，无硬件）
mocktest: mocktest.c otimock.c otilink.c otitrans.c otiproto.c otihid.c \
          otimock.h otilink.h otitrans.h otiproto.h otihid.h
	$(CC) $(CFLAGS) -o $@ mocktest.c otimock.c otilink.c otitrans.c otiproto.c otihid.c otix11.c -lpthread -ldl

# 真机线缆数据通路端到端测试（不需要 evdev/uinput）
cabletest: cabletest.c otilink.c otitrans.c otiproto.c otilink.h otitrans.h otiproto.h
	$(CC) $(CFLAGS) -o $@ cabletest.c otilink.c otitrans.c otiproto.c -lpthread

# 键鼠共享 daemon
otikm: otikm.c otix11.c otix11.h otiproto.c otitrans.c otilink.c otikm_core.c otiinput.c oticlip.c otihid.c otixfer.c \
       otiproto.h otitrans.h otilink.h otikm_core.h otiinput.h oticlip.h otihid.h otixfer.h
	$(CC) $(CFLAGS) -o $@ otikm.c otiproto.c otitrans.c otilink.c otikm_core.c otiinput.c oticlip.c otihid.c otix11.c otixfer.c -lpthread -ldl

# uinput 虚拟键鼠（无物理键鼠时走完整生产路径做端到端验证）。
# kbdexcl.sh 用它造一对"可被独占"的抓取设备：既不碰用户的真键鼠，
# 又能真实验证 EVIOCGRAB 独占/释放（双重输入这条链路的唯一硬件不变量）。
uisim: uisim.c
	$(CC) $(CFLAGS) -o $@ uisim.c

# HID 方向探针：判定"本机 → 对端"的 HID 通道是否通（"回不来"类故障的第二层定位）
#   ./hidprobe /dev/sg3 1 10        # 鼠标右移 40px × 10 → 看对端光标动不动
#   ./hidprobe /dev/sg3 2 3 0x87    # 键盘 usage 0x87(F24) 按/松 × 3 → 看对端钩子/令牌
hidprobe: hidprobe.c otilink.c otiproto.c otitrans.c otihid.c \
          otilink.h otiproto.h otitrans.h otihid.h
	$(CC) $(CFLAGS) -o $@ hidprobe.c otilink.c otiproto.c otitrans.c otihid.c -lpthread

# 端到端：真实 transport（fd 后端）+ 真实 proto + 真实状态机
e2e_test: e2e_test.c otiproto.c otitrans.c otilink.c otikm_core.c \
          otiproto.h otitrans.h otilink.h otikm_core.h
	$(CC) $(CFLAGS) -o $@ e2e_test.c otiproto.c otitrans.c otilink.c otikm_core.c -lpthread

itest: otikm
	@sh itest.sh

cliptest: otikm
	@sh cliptest.sh

tcptest: otikm
	@sh tcptest.sh

# Windows 侧 agent 的跨实现线上兼容测试（用 Linux 生成/校验帧，经 powershell.exe 互操作）
wintest: probe
	@./probe frameout /tmp/lin.frames
	@powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$$(wslpath -w ../windows/otiagent.ps1)" -FrameTest "$$(wslpath -w /tmp/lin.frames)" -FrameOut "$$(wslpath -w /tmp/win.frames)"
	@./probe framein /tmp/win.frames win

# 跨实现运行时互通（Linux otikm ↔ Windows agent，经 TCP 测试通道）
interop: otikm
	@sh interop.sh

# SG_IO 层验证（需要 root + scsi_debug 模块）
sgtest: probe
	@sh sgtest.sh

test: $(BINS)
	@echo "===== 1/7 帧与 CDB 自检 ====="
	@./probe selftest
	@echo "===== 2/7 消息层与状态机 ====="
	@./selftest_proto
	@echo "===== 3/7 端到端（库层）====="
	@./e2e_test
	@echo "===== 4/7 双实例集成（键鼠 daemon 层）====="
	@sh itest.sh
	@echo "===== 5/7 剪贴板集成（双向 + 防回环 + 跨块）====="
	@sh cliptest.sh
	@echo "===== 6/7 TCP 传输（真机 bring-up 预演）====="
	@sh tcptest.sh
	@echo "===== 7/7 cable 传输路径（软件仿真设备）====="
	@./mocktest

clean:
	rm -f $(BINS)

.PHONY: all clean test itest cliptest tcptest sgtest wintest interop

# 交接状态机单元测试（不需要设备/图形环境）—— 交接/回程逻辑的回归
coretest: coretest.c otikm_core.c otikm_core.h otiproto.c otiproto.h
	$(CC) $(CFLAGS) -o $@ coretest.c otikm_core.c otiproto.c
