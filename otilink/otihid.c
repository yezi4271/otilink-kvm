/*
 * otihid.c —— 见 otihid.h
 */
#include "otihid.h"

#include <linux/input.h>
#include <string.h>

/* evdev 键码 → USB HID usage id（常用键子集，覆盖日常键鼠共享） */
static const struct {
    uint16_t ev;
    uint8_t usage;
} map[] = {
    /* 字母 A-Z（evdev 是非连续的，HID 是连续的 0x04..0x1D） */
    {KEY_A, 0x04}, {KEY_B, 0x05}, {KEY_C, 0x06}, {KEY_D, 0x07}, {KEY_E, 0x08},
    {KEY_F, 0x09}, {KEY_G, 0x0A}, {KEY_H, 0x0B}, {KEY_I, 0x0C}, {KEY_J, 0x0D},
    {KEY_K, 0x0E}, {KEY_L, 0x0F}, {KEY_M, 0x10}, {KEY_N, 0x11}, {KEY_O, 0x12},
    {KEY_P, 0x13}, {KEY_Q, 0x14}, {KEY_R, 0x15}, {KEY_S, 0x16}, {KEY_T, 0x17},
    {KEY_U, 0x18}, {KEY_V, 0x19}, {KEY_W, 0x1A}, {KEY_X, 0x1B}, {KEY_Y, 0x1C},
    {KEY_Z, 0x1D},
    /* 数字 1..0 */
    {KEY_1, 0x1E}, {KEY_2, 0x1F}, {KEY_3, 0x20}, {KEY_4, 0x21}, {KEY_5, 0x22},
    {KEY_6, 0x23}, {KEY_7, 0x24}, {KEY_8, 0x25}, {KEY_9, 0x26}, {KEY_0, 0x27},
    /* 控制与标点 */
    {KEY_ENTER, 0x28}, {KEY_ESC, 0x29}, {KEY_BACKSPACE, 0x2A}, {KEY_TAB, 0x2B},
    {KEY_SPACE, 0x2C}, {KEY_MINUS, 0x2D}, {KEY_EQUAL, 0x2E}, {KEY_LEFTBRACE, 0x2F},
    {KEY_RIGHTBRACE, 0x30}, {KEY_BACKSLASH, 0x31}, {KEY_SEMICOLON, 0x33},
    {KEY_APOSTROPHE, 0x34}, {KEY_GRAVE, 0x35}, {KEY_COMMA, 0x36}, {KEY_DOT, 0x37},
    {KEY_SLASH, 0x38}, {KEY_CAPSLOCK, 0x39},
    /* F1..F12 */
    {KEY_F1, 0x3A}, {KEY_F2, 0x3B}, {KEY_F3, 0x3C}, {KEY_F4, 0x3D}, {KEY_F5, 0x3E},
    {KEY_F6, 0x3F}, {KEY_F7, 0x40}, {KEY_F8, 0x41}, {KEY_F9, 0x42}, {KEY_F10, 0x43},
    {KEY_F11, 0x44}, {KEY_F12, 0x45},
    /* 编辑键与方向键 */
    {KEY_SYSRQ, 0x46}, {KEY_SCROLLLOCK, 0x47}, {KEY_PAUSE, 0x48},
    {KEY_INSERT, 0x49}, {KEY_HOME, 0x4A}, {KEY_PAGEUP, 0x4B}, {KEY_DELETE, 0x4C},
    {KEY_END, 0x4D}, {KEY_PAGEDOWN, 0x4E}, {KEY_RIGHT, 0x4F}, {KEY_LEFT, 0x50},
    {KEY_DOWN, 0x51}, {KEY_UP, 0x52}, {KEY_NUMLOCK, 0x53},
    {KEY_KPSLASH, 0x54}, {KEY_KPASTERISK, 0x55}, {KEY_KPMINUS, 0x56},
    {KEY_KPPLUS, 0x57}, {KEY_KPENTER, 0x58}, {KEY_KP1, 0x59}, {KEY_KP2, 0x5A},
    {KEY_KP3, 0x5B}, {KEY_KP4, 0x5C}, {KEY_KP5, 0x5D}, {KEY_KP6, 0x5E},
    {KEY_KP7, 0x5F}, {KEY_KP8, 0x60}, {KEY_KP9, 0x61}, {KEY_KP0, 0x62},
    {KEY_KPDOT, 0x63}, {KEY_102ND, 0x64}, {KEY_MENU, 0x65},
};

uint8_t otihid_evdev_to_usage(uint16_t code)
{
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].ev == code)
            return map[i].usage;
    return 0;
}

uint8_t otihid_modifier_bit(uint16_t code)
{
    switch (code) {
    case KEY_LEFTCTRL:  return 0x01;
    case KEY_LEFTSHIFT: return 0x02;
    case KEY_LEFTALT:   return 0x04;
    case KEY_LEFTMETA:  return 0x08;
    case KEY_RIGHTCTRL: return 0x10;
    case KEY_RIGHTSHIFT:return 0x20;
    case KEY_RIGHTALT:  return 0x40;
    case KEY_RIGHTMETA: return 0x80;
    default: return 0;
    }
}

void otihid_kbd_reset(struct otihid_kbd *k)
{
    memset(k, 0, sizeof(*k));
}

int otihid_kbd_apply(struct otihid_kbd *k, uint16_t code, int value)
{
    /* ⚠️ 修饰键**不走修饰键字节**，而是当成普通 key usage 0xE0..0xE7 放进 6 键数组。
       为什么（2026-09-21 真机实证，NOTES §61）：线缆的 HID 键盘接口一旦收到**修饰键字节**，
       之后的报表全部送不出去、Windows 上 Ctrl 永久卡住；而走 usage 数组时按下和**释放都能到**。
       描述符里 0xE0..0xE7 本来就在 key array 的 usage 范围内（19 00 2a ff 00），Windows 认。 */
    static const struct { uint16_t ev; uint8_t usage; } mods[] = {
        { 29, 0xE0 }, { 42, 0xE1 }, { 56, 0xE2 }, { 125, 0xE3 },   /* 左 Ctrl/Shift/Alt/Meta */
        { 97, 0xE4 }, { 54, 0xE5 }, { 100, 0xE6 }, { 126, 0xE7 },  /* 右 … */
    };
    uint8_t usage = 0;
    for (unsigned i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
        if (mods[i].ev == code) { usage = mods[i].usage; break; }
    if (!usage)
        usage = otihid_evdev_to_usage(code);
    if (!usage)
        return 0;

    int before[6];
    memcpy(before, k->keys, sizeof(before));
    if (value) {
        /* 已在列表里则不动；否则找空槽放入 */
        for (int i = 0; i < 6; i++)
            if (k->keys[i] == usage)
                return 0;
        for (int i = 0; i < 6; i++)
            if (k->keys[i] == 0) {
                k->keys[i] = usage;
                return 1;
            }
        /* 满了：做一次滚动（丢最早的那个） */
        memmove(k->keys, k->keys + 1, 5);
        k->keys[5] = usage;
        return 1;
    }
    for (int i = 0; i < 6; i++)
        if (k->keys[i] == usage) {
            k->keys[i] = 0;
            return 1;
        }
    return 0;
}

/*
 * 以下两个函数输出的是**厂商实测布局**（第 33 轮真机标定，见 NOTES.md §41）：
 *
 *   鼠标 12 字节: [0]=按键位(bit0 左 bit1 右 bit2 中 bit3 侧 bit4 额外)
 *                 [1]=dx int8  [2]=dy int8  [3]=wheel int8  [4..11]=0
 *   键盘 12 字节: [0]=修饰键  [1]=保留  [2..7]=6 个 USB HID usage  [8..11]=0
 *
 * 线缆固件把前 12 字节塞进 CDB[2..13]，接收端得到一个**真实的 USB HID 设备**，
 * 由操作系统原生处理（含指针加速、按键、滚轮），接收端无需任何软件。
 *
 * layout 参数保留仅为兼容旧配置，现已忽略（厂商布局是唯一正确布局）。
 */
void otihid_kbd_report(const struct otihid_kbd *k, int layout, uint8_t pkt[14])
{
    (void)layout;
    memset(pkt, 0, 14);
    pkt[0] = k->mods;                        /* 修饰键位图 */
    pkt[1] = 0x00;                           /* 保留字节 */
    memcpy(pkt + 2, k->keys, 6);             /* 6 键 rollover */
}

void otihid_mouse_report(uint16_t buttons, int16_t dx, int16_t dy, int16_t wheel,
                         int layout, uint8_t pkt[14])
{
    (void)layout;
    memset(pkt, 0, 14);
    /* 描述符里每轴是 1 字节有符号（-127..127），超出必须分包，见 otihid_mouse_packets() */
    int8_t x = dx > 127 ? 127 : (dx < -127 ? -127 : (int8_t)dx);
    int8_t y = dy > 127 ? 127 : (dy < -127 ? -127 : (int8_t)dy);
    int8_t w = wheel > 127 ? 127 : (wheel < -127 ? -127 : (int8_t)wheel);
    pkt[0] = (uint8_t)(buttons & 0x1f);      /* 5 个按键位 */
    pkt[1] = (uint8_t)x;
    pkt[2] = (uint8_t)y;
    pkt[3] = (uint8_t)w;
}

/*
 * 把一次位移拆成若干个合规的 12 字节鼠标包（每轴最多 ±127）。
 * out 至少 max_pkts*14 字节；返回实际包数（0 表示无位移且无滚轮）。
 * 第一个包带滚轮，其余包滚轮为 0。
 */
int otihid_mouse_packets(uint16_t buttons, int dx, int dy, int wheel,
                         uint8_t *out, int max_pkts)
{
    int n = 0;
    for (;;) {
        if (n >= max_pkts)
            break;
        int cx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
        int cy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
        int cw = wheel > 127 ? 127 : (wheel < -127 ? -127 : wheel);
        otihid_mouse_report(buttons, (int16_t)cx, (int16_t)cy, (int16_t)cw, 0,
                            out + (size_t)n * 14);
        n++;
        dx -= cx;
        dy -= cy;
        wheel -= cw;
        if (dx == 0 && dy == 0 && wheel == 0)
            break;
    }
    return n;
}
