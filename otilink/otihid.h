/*
 * otihid.h —— 把 Linux evdev 事件转成厂商的 12 字节 HID 包
 *
 * **真机标定结论（第 33 轮，见 NOTES.md §41）**：
 *   类型码 = CDB[1]：0x33=鼠标 / 0x34=键盘 / 0x36=多媒体
 *   鼠标 12 字节: [0]=按键位 [1]=dx int8 [2]=dy int8 [3]=wheel int8 [4..11]=0
 *   键盘 12 字节: [0]=修饰键 [1]=保留  [2..7]=6 个 USB HID usage [8..11]=0
 *
 * 线缆两端都把自己枚举成**真实的 USB 鼠标 + 键盘**（MI_01/MI_02），
 * 所以「发送端发 HID 包 → 接收端内核收到标准 HID」成立，
 * **接收端不需要任何软件**（Windows 侧同样如此）。
 */
#ifndef OITHID_H
#define OITHID_H

#include <stdint.h>

/* 键盘状态：修饰键位图 + 最多 6 键 rollover（与 boot keyboard 报告一致） */
struct otihid_kbd {
    uint8_t mods;
    uint8_t keys[6];
};

void otihid_kbd_reset(struct otihid_kbd *k);

/* evdev 键码 → USB HID usage id；返回 0 表示未映射 */
uint8_t otihid_evdev_to_usage(uint16_t code);
/* 是修饰键则返回其修饰位（否则 0） */
uint8_t otihid_modifier_bit(uint16_t code);

/* 更新键盘状态；返回 1 表示报告内容有变化 */
int otihid_kbd_apply(struct otihid_kbd *k, uint16_t code, int value);

/* 生成 12 字节键盘报告（写入 pkt[0..11]；pkt 至少 14 字节）
 * layout 参数已忽略（厂商布局是唯一正确布局），保留仅为兼容旧配置。 */
void otihid_kbd_report(const struct otihid_kbd *k, int layout, uint8_t pkt[14]);

/* 生成 12 字节鼠标报告；layout 参数已忽略。*/
void otihid_mouse_report(uint16_t buttons, int16_t dx, int16_t dy, int16_t wheel,
                         int layout, uint8_t pkt[14]);

/* 位移超过 ±127 时拆成多包（每包 14 字节，取前 12 字节入 CDB）。
 * out 至少 max_pkts*14 字节；返回包数。 */
int otihid_mouse_packets(uint16_t buttons, int dx, int dy, int wheel,
                         uint8_t *out, int max_pkts);

#endif /* OITHID_H */
