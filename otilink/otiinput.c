/*
 * otiinput.c —— 见 otiinput.h
 */
#include "otiinput.h"

#include "otilink.h"           /* otilink_is_our_usb()：按 USB VID/PID 认对拷线 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---------- 抓取 ---------- */
int oti_capture_open(struct oti_capture *c, const char *dev, int grab)
{
    memset(c, 0, sizeof(*c));
    c->fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (c->fd < 0)
        return -errno;
    c->kind = oti_input_kind(dev);      /* 重开时用来校验"还是不是同一类设备" */
    if (ioctl(c->fd, EVIOCGNAME(sizeof(c->devname) - 1), c->devname) < 0)
        snprintf(c->devname, sizeof(c->devname), "%s", dev);
    if (grab) {
        if (ioctl(c->fd, EVIOCGRAB, 1) == 0)
            c->grabbed = 1;
        else
            return -errno; /* 抓不住就别继续（否则会出现双重输入） */
    }
    return 0;
}

void oti_capture_close(struct oti_capture *c)
{
    if (c->fd >= 0) {
        if (c->grabbed)
            ioctl(c->fd, EVIOCGRAB, 0);
        close(c->fd);
    }
    c->fd = -1;
}

int oti_capture_next(struct oti_capture *c, uint16_t *type, uint16_t *code, int32_t *value)
{
    struct input_event ev;
    ssize_t n = read(c->fd, &ev, sizeof(ev));
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    if (n != (ssize_t)sizeof(ev))
        return 0;
    *type = ev.type;
    *code = ev.code;
    *value = ev.value;
    if (ev.type == EV_KEY && ev.code >= BTN_MISC) {
        /* 鼠标按键：维护位图 */
        uint16_t bit = 0;
        switch (ev.code) {
        case BTN_LEFT:   bit = 1u << 0; break;
        case BTN_RIGHT:  bit = 1u << 1; break;
        case BTN_MIDDLE: bit = 1u << 2; break;
        default: break;
        }
        if (bit) {
            if (ev.value)
                c->buttons |= bit;
            else
                c->buttons &= (uint16_t)~bit;
        }
    }
    return 1;
}

/* ---------- 注入 ---------- */
static int uinput_set(int fd, unsigned long req, int val)
{
    return ioctl(fd, req, val);
}

int oti_inject_open(struct oti_inject *inj, const char *name)
{
    memset(inj, 0, sizeof(*inj));
    inj->fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (inj->fd < 0)
        return -errno;

    /* 键盘：给常用键码全部置位（含多媒体键），保证任意键都能注入 */
    uinput_set(inj->fd, UI_SET_EVBIT, EV_KEY);
    for (int k = 1; k <= 255; k++)
        uinput_set(inj->fd, UI_SET_KEYBIT, k);
    uinput_set(inj->fd, UI_SET_KEYBIT, KEY_MICMUTE);

    /* 鼠标：相对轴 + 按键 */
    uinput_set(inj->fd, UI_SET_EVBIT, EV_REL);
    uinput_set(inj->fd, UI_SET_RELBIT, REL_X);
    uinput_set(inj->fd, UI_SET_RELBIT, REL_Y);
    uinput_set(inj->fd, UI_SET_RELBIT, REL_WHEEL);
    uinput_set(inj->fd, UI_SET_RELBIT, REL_HWHEEL);
    uinput_set(inj->fd, UI_SET_KEYBIT, BTN_LEFT);
    uinput_set(inj->fd, UI_SET_KEYBIT, BTN_RIGHT);
    uinput_set(inj->fd, UI_SET_KEYBIT, BTN_MIDDLE);

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    snprintf(us.name, sizeof(us.name), "%s", name ? name : OTI_INJECT_NAME);
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x0ea0;
    us.id.product = 0x2213;
    us.id.version = 1;
    if (ioctl(inj->fd, UI_DEV_SETUP, &us) < 0)
        return -errno;
    if (ioctl(inj->fd, UI_DEV_CREATE) < 0)
        return -errno;
    return 0;
}

static int emit(struct oti_inject *inj, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(inj->fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        return -errno;
    return 0;
}

static int emit_sync(struct oti_inject *inj)
{
    return emit(inj, EV_SYN, SYN_REPORT, 0);
}

int oti_inject_key(struct oti_inject *inj, uint16_t code, int value)
{
    int rc = emit(inj, EV_KEY, code, value);
    if (rc)
        return rc;
    return emit_sync(inj);
}

int oti_inject_mouse(struct oti_inject *inj, int16_t dx, int16_t dy, int16_t wheel,
                     uint16_t buttons)
{
    uint16_t changed = inj->buttons ^ buttons;
    static const struct {
        uint16_t bit, code;
    } map[] = {{1u << 0, BTN_LEFT}, {1u << 1, BTN_RIGHT}, {1u << 2, BTN_MIDDLE}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (changed & map[i].bit) {
            int rc = emit(inj, EV_KEY, map[i].code, (buttons & map[i].bit) ? 1 : 0);
            if (rc)
                return rc;
        }
    }
    inj->buttons = buttons;
    if (dx)
        emit(inj, EV_REL, REL_X, dx);
    if (dy)
        emit(inj, EV_REL, REL_Y, dy);
    if (wheel)
        emit(inj, EV_REL, REL_WHEEL, wheel);
    return emit_sync(inj);
}

void oti_inject_close(struct oti_inject *inj)
{
    if (inj->fd >= 0) {
        ioctl(inj->fd, UI_DEV_DESTROY);
        close(inj->fd);
    }
    inj->fd = -1;
}

/* ---------- 分类 / 自动选择（绿色包用；不依赖 by-id 命名） ---------- */
#define BITS_PER_LONG (int)(8 * sizeof(unsigned long))
#define BIT_WORDS(n)  (((n) / BITS_PER_LONG) + 1)

static int bit_test(const unsigned long *bits, int n)
{
    return (int)((bits[n / BITS_PER_LONG] >> (n % BITS_PER_LONG)) & 1UL);
}

int oti_input_kind(const char *dev)
{
    int fd = open(dev, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    unsigned long evb[BIT_WORDS(EV_MAX)];
    memset(evb, 0, sizeof(evb));
    int kind = 0;
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evb)), evb) >= 0) {
        int have_key = bit_test(evb, EV_KEY);
        int have_rel = bit_test(evb, EV_REL);
        if (have_rel) {
            unsigned long relb[BIT_WORDS(REL_MAX)];
            memset(relb, 0, sizeof(relb));
            if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relb)), relb) >= 0 &&
                bit_test(relb, REL_X) && bit_test(relb, REL_Y))
                kind |= OTI_IN_MOUSE;                 /* 相对位移 = 鼠标 */
        }
        if (have_key) {
            unsigned long keyb[BIT_WORDS(KEY_MAX)];
            memset(keyb, 0, sizeof(keyb));
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyb)), keyb) >= 0) {
                if (bit_test(keyb, KEY_A) && bit_test(keyb, KEY_Z) && bit_test(keyb, KEY_SPACE))
                    kind |= OTI_IN_KBD;               /* 字母区齐全 = 键盘 */
                if (bit_test(keyb, BTN_LEFT) && bit_test(keyb, BTN_RIGHT))
                    kind |= OTI_IN_MOUSE;             /* 带左右键 = 鼠标（含只有按键的设备） */
            }
        }
    }
    close(fd);
    return kind;
}

int oti_input_is_cable(const char *dev)
{
    const char *bn = strrchr(dev, '/');
    char link[160], real[PATH_MAX];
    /* /dev/input/eventN → /sys/class/input/eventN/device → **realpath** → otilink_is_our_usb()
       注意：**必须先 realpath**。otilink_is_our_usb() 是沿"字符串路径"一级一级向上找
       idVendor/idProduct 的，而 /sys/class/input/eventN/device 是**符号链接**：
       不解析就只会在 /sys/class/input/... 这串假路径上打转，永远走不到 USB 设备目录
       —— 真机表现就是"看不到线缆 HID 设备"（第 39 轮实测踩到）。 */
    snprintf(link, sizeof(link), "/sys/class/input/%.48s/device", bn ? bn + 1 : dev);
    if (!realpath(link, real))
        return 0;
    return otilink_is_our_usb(real);
}

int oti_input_is_usb(const char *dev)
{
    const char *bn = strrchr(dev, '/');
    char link[160], real[PATH_MAX];
    /* 与 oti_input_is_cable 同款细节：必须先 realpath 才能沿 sysfs 走到 USB 设备目录 */
    snprintf(link, sizeof(link), "/sys/class/input/%.48s/device", bn ? bn + 1 : dev);
    if (!realpath(link, real))
        return 0;
    return otilink_is_usb_dev(real);
}

/* /dev/input/eventN → /dev/input/by-id/... 稳定路径（见头文件说明）。
   优先带 -event-kbd / -event-mouse 的链接（它们是"主接口"，比 if01/consumer 那类可靠）；
   路径长度超过 63 字节的链接不用（调用方的缓冲区是 64 字节，截断会指向不存在的文件）。 */
int oti_input_stable_path(const char *dev, char out[64])
{
    char real[PATH_MAX];
    if (!realpath(dev, real))
        return -1;
    DIR *d = opendir("/dev/input/by-id");
    if (!d)
        return -1;
    char best[64] = "";
    int best_score = -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[192], r2[PATH_MAX];
        int n = snprintf(p, sizeof(p), "/dev/input/by-id/%.120s", e->d_name);
        if (n < 0 || n >= 64)                  /* 太长：out 放不下，跳过 */
            continue;
        if (!realpath(p, r2) || strcmp(r2, real) != 0)
            continue;
        size_t ln = strlen(e->d_name);
        int score = 0;
        if ((ln >= 10 && strcmp(e->d_name + ln - 10, "-event-kbd") == 0) ||
            (ln >= 12 && strcmp(e->d_name + ln - 12, "-event-mouse") == 0))
            score = 2;
        else if (strstr(e->d_name, "-event-kbd") || strstr(e->d_name, "-event-mouse"))
            score = 1;
        if (score > best_score) {
            best_score = score;
            snprintf(best, sizeof(best), "%.63s", p);
        }
    }
    closedir(d);
    if (!best[0])
        return -1;
    snprintf(out, 64, "%s", best);
    return 0;
}

/* 把挑选结果换成 by-id 稳定路径（有就换；没有保持 eventN） */
static void to_stable_path(char path[64])
{
    char st[64];
    if (path[0] && oti_input_stable_path(path, st) == 0)
        snprintf(path, 64, "%s", st);
}

/* 在本机设备里挑键鼠；usb_only=1 时只认可热插拔的 USB 设备。
 * 两个要点（真机实测踩到的）：
 *   1) **优先"纯"设备**：罗技 G304 这类接收器会暴露成一个"键鼠合一"的 evdev，
 *      若按顺序先撞上它，真正的键盘（SIGMACHIP USB Keyboard）就被顶掉了 —— 用户表现是
 *      "主控端键盘没反应"。所以第一遍只挑纯键盘/纯鼠标，第二遍才接受组合设备。
 *   2) **同一个设备不重复抓**：组合设备一个 fd 就能同时收键盘+鼠标事件（返回的计数是
 *      "能力个数"，但 mou 会留空表示"已被 kbd 那个 fd 覆盖"），避免同一个 eventN 开两次。 */
static int pick_local(char names[][128], char devs[][64], int n, int usb_only,
                      char kbd[64], char mou[64])
{
    int nk = 0, nm = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            if (strstr(names[i], OTI_INJECT_NAME))
                continue;                             /* 我们自己创建的注入设备：跳过（防回环） */
            if (oti_input_is_cable(devs[i]))
                continue;
            if (usb_only && !oti_input_is_usb(devs[i]))
                continue;
            int kind = oti_input_kind(devs[i]);
            if (!kind)
                continue;
            if (pass == 0 && (kind & OTI_IN_KBD) && (kind & OTI_IN_MOUSE))
                continue;                             /* 第一遍：跳过组合设备 */
            if (!nk && kbd && (kind & OTI_IN_KBD)) {
                snprintf(kbd, 64, "%.63s", devs[i]);
                nk = 1;
            }
            if (!nm && mou && (kind & OTI_IN_MOUSE)) {
                if (nk && strcmp(kbd, devs[i]) == 0)
                    nm = 1;                          /* 同一个 fd 已覆盖鼠标，mou 留空 */
                else {
                    snprintf(mou, 64, "%.63s", devs[i]);
                    nm = 1;
                }
            }
        }
        if (nk && nm)
            break;
    }
    return nk + nm;
}

int oti_input_autoselect(int want_cable, char kbd[64], char mou[64])
{
    static char names[64][128], devs[64][64];
    int n = oti_input_list(names, devs, 64);
    if (kbd)
        kbd[0] = 0;
    if (mou)
        mou[0] = 0;
    if (want_cable) {
        int nk = 0, nm = 0;
        for (int i = 0; i < n; i++) {
            if (strstr(names[i], OTI_INJECT_NAME))
                continue;
            if (!oti_input_is_cable(devs[i]))
                continue;
            int kind = oti_input_kind(devs[i]);
            if (!kind)
                continue;
            if ((kind & OTI_IN_KBD) && kbd && !nk) { snprintf(kbd, 64, "%.63s", devs[i]); nk = 1; }
            else if ((kind & OTI_IN_MOUSE) && mou && !nm) { snprintf(mou, 64, "%.63s", devs[i]); nm = 1; }
        }
        if (kbd) to_stable_path(kbd);
        if (mou) to_stable_path(mou);
        return nk + nm;
    }
    /* 本机键鼠：**优先可热插拔的 USB 设备**。
       为什么（真机实测）：台式机 i8042 控制器即使没插 PS/2 键盘也会创建一个
       "AT Raw Set 2 keyboard"，而它排在 eventN 前面 → 会把真实 USB 键盘顶掉，
       抓取到一个永远没有事件的口（用户表现：主控端键盘不能用）。
       真的只有 PS/2 键鼠时，第二遍兜底仍会选中它们（不破坏老机器）。 */
    int got = pick_local(names, devs, n, 1, kbd, mou);
    if (!got)
        got = pick_local(names, devs, n, 0, kbd, mou);
    if (got) {
        /* 换成 by-id 稳定路径：拔插/重枚举后 eventN 会变，按它重开会抓错设备 */
        if (kbd) to_stable_path(kbd);
        if (mou) to_stable_path(mou);
    }
    return got;
}

/* ---------- 枚举 ---------- */
int oti_input_local_count(void)
{
    char names[64][128], devs[64][64];          /* 栈上：允许并发调用（见头文件说明） */
    int n = oti_input_list(names, devs, 64);
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (strstr(names[i], OTI_INJECT_NAME))
            continue;                            /* 我们自己注入的设备：跳过（防回环） */
        if (oti_input_is_cable(devs[i]))
            continue;                            /* 对拷线自身的 HID：不算"本机键鼠" */
        if (!oti_input_is_usb(devs[i]))
            continue;                            /* 内置/PS2 不会"换边"：不算（见 otikm_core 的角色规则） */
        if (oti_input_kind(devs[i]))
            cnt++;
    }
    return cnt;
}

int oti_input_list(char names[][128], char devs[][64], int max)
{
    DIR *d = opendir("/dev/input");
    if (!d)
        return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < max) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%.48s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char nm[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(nm) - 1), nm) < 0)
            snprintf(nm, sizeof(nm), "(unknown)");
        close(fd);
        snprintf(devs[n], 64, "%s", path);
        snprintf(names[n], 128, "%s", nm);
        n++;
    }
    closedir(d);
    return n;
}
