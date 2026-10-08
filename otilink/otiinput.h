/*
 * otiinput.h —— 本地输入抓取（evdev）与注入（uinput）
 *
 * 角色：
 *   - 控制端：evdev 抓物理键鼠 → 封装成协议消息发走；
 *   - 被控端：收到协议消息 → uinput 注入（内核合成事件，普通应用可见）。
 *
 * 防回环：注入用的 uinput 设备名固定为 OTI_INJECT_NAME，抓取端按名字排除它。
 *
 * 权限：/dev/uinput 通常 root:root 0600。非 root 需要 udev 规则，例如
 *   /etc/udev/rules.d/99-otilink.rules:
 *     KERNEL=="uinput", GROUP="input", MODE="0660"
 *   再 usermod -aG input $USER
 */
#ifndef OTIINPUT_H
#define OTIINPUT_H

#include <stdint.h>

#define OTI_INJECT_NAME "otilink virtual input"

struct oti_capture {
    int fd;
    int grabbed;
    char devname[128];
    int kind;           /* 打开时按能力位判定的 OTI_IN_KBD|OTI_IN_MOUSE（重开时校验用） */
    uint16_t buttons;   /* 当前按下的 BTN_* 位图（我们自己跟踪） */
    int rel_dx, rel_dy; /* 未发送的位移累积（可选节流用） */
};

struct oti_inject {
    int fd;
    uint16_t buttons;   /* 已注入的按键状态，用于算差异 */
};

/* 打开一个 evdev 设备；grab=1 时 EVIOCGRAB 独占（本地桌面不再看到这些事件） */
int oti_capture_open(struct oti_capture *c, const char *dev, int grab);
void oti_capture_close(struct oti_capture *c);
/* 读一个事件；返回 1 有事件（type/code/value），0 无事件，<0 错误 */
int oti_capture_next(struct oti_capture *c, uint16_t *type, uint16_t *code, int32_t *value);

/* 创建 uinput 虚拟键鼠 */
int oti_inject_open(struct oti_inject *inj, const char *name);
void oti_inject_close(struct oti_inject *inj);
int oti_inject_key(struct oti_inject *inj, uint16_t code, int value);
/* 相对位移 + 轮子 + 按键位图（内部只对变化的按键发事件） */
int oti_inject_mouse(struct oti_inject *inj, int16_t dx, int16_t dy, int16_t wheel,
                     uint16_t buttons);

/* 列出 /dev/input/event* 的名字，便于挑设备 */
int oti_input_list(char names[][128], char devs[][64], int max);

/* ---------- 设备分类与自动选择（绿色包：不再依赖 by-id 命名）----------
 * 为什么：run-kylin.sh 靠 /dev/input/by-id 下的 event-kbd 通配 + "Android+Mac" 子串区分
 * "本机键鼠" 与 "线缆 HID"。那套命名随发行版/内核/线缆固件变（实测 by-id 里就没有
 * 稳定的通用模式），所以改成按**能力位**分类、按 **sysfs 里的 USB VID/PID** 认线缆。 */
#define OTI_IN_KBD   1
#define OTI_IN_MOUSE 2

/* 按 EVIOCGBIT 能力位判定；可能同时返回 KBD|MOUSE（带按键的多媒体键盘）。0 = 都不是 */
int oti_input_kind(const char *dev);
/* 该 evdev 是不是对拷线自身的 HID 接口（沿 sysfs 向上找 0ea0:2213） */
int oti_input_is_cable(const char *dev);
/* 该 evdev 是不是**可热插拔**的 USB 设备（沿 sysfs 向上找 idVendor） */
int oti_input_is_usb(const char *dev);
/* 自动挑设备：want_cable=1 挑线缆 HID（被驱动侧）；0 挑本机键鼠并排除线缆 HID（主控侧）。
 * kbd/mou 传 NULL 表示不关心；出参为 64 字节设备路径。返回挑到的个数（0~2）。
 * 返回的是 by-id 稳定路径（形如 /dev/input/by-id/...；没有链接时才退回 eventN）——
 * eventN 拔插后会变（真机实测：键盘从 event7 换成 "System Control" 接口，
 * 按旧 eventN 重开就抓错设备 → 用户"键盘用不了"）。 */
int oti_input_autoselect(int want_cable, char kbd[64], char mou[64]);

/* 把 /dev/input/eventN 换成指向同一设备的 by-id 稳定路径。0 = 成功，-1 = 没有链接。 */
int oti_input_stable_path(const char *dev, char out[64]);

/* "线缆之外、可热插拔（USB）的键鼠"设备个数（0 = 没有）。
 * 角色协商要**周期重采样**它来判断"键鼠插在哪一端"，所以：
 *   - 用栈上缓冲（不能像 autoselect 那样用 static：RX 线程与抓取线程会并发调用）；
 *   - 统计全部而不是各取第一个。判据与 oti_input_autoselect(0,..) 完全一致。 */
int oti_input_local_count(void);

#endif /* OTIINPUT_H */
