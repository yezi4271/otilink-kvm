/*
 * hidmon.c —— 监听线缆自带的 HID 键鼠接口，打印原始 input_event。
 * 用途：标定"厂商 HID 包（CDB D9 33 + 12 字节）→ 实际 HID 报告"的映射。
 *
 * 编译: gcc -O2 -o hidmon hidmon.c
 * 用法: sg input -c './hidmon /dev/input/by-id/usb-_Android+Mac_*-if01-event-mouse 20'
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char *evname(unsigned short t)
{
    switch (t) {
    case EV_SYN: return "SYN";
    case EV_KEY: return "KEY";
    case EV_REL: return "REL";
    case EV_ABS: return "ABS";
    case EV_MSC: return "MSC";
    case EV_LED: return "LED";
    default: return "?";
    }
}

static const char *codename(unsigned short t, unsigned short c)
{
    static char b[32];
    if (t == EV_REL) {
        if (c == REL_X) return "REL_X";
        if (c == REL_Y) return "REL_Y";
        if (c == REL_WHEEL) return "REL_WHEEL";
        if (c == REL_HWHEEL) return "REL_HWHEEL";
    } else if (t == EV_KEY) {
        if (c == BTN_LEFT) return "BTN_LEFT";
        if (c == BTN_RIGHT) return "BTN_RIGHT";
        if (c == BTN_MIDDLE) return "BTN_MIDDLE";
        if (c == BTN_SIDE) return "BTN_SIDE";
        if (c == BTN_EXTRA) return "BTN_EXTRA";
        if (c == KEY_A) return "KEY_A";
        if (c == KEY_LEFTSHIFT) return "KEY_LEFTSHIFT";
        if (c == KEY_LEFTCTRL) return "KEY_LEFTCTRL";
    }
    snprintf(b, sizeof b, "%u", c);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "用法: %s <event设备> [秒数]\n", argv[0]);
        return 1;
    }
    int secs = argc > 2 ? atoi(argv[2]) : 20;
    int fd = open(argv[1], O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    printf("hidmon: 监听 %s %d 秒\n", argv[1], secs);
    fflush(stdout);
    struct timeval t0;
    gettimeofday(&t0, NULL);
    struct pollfd p = { fd, POLLIN, 0 };
    for (;;) {
        struct timeval tn;
        gettimeofday(&tn, NULL);
        double el = (tn.tv_sec - t0.tv_sec) + (tn.tv_usec - t0.tv_usec) / 1e6;
        if (el > secs) break;
        int r = poll(&p, 1, 200);
        if (r <= 0) continue;
        struct input_event ev[64];
        ssize_t n = read(fd, ev, sizeof ev);
        if (n <= 0) continue;
        int cnt = (int)(n / sizeof(struct input_event));
        for (int i = 0; i < cnt; i++) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            double e2 = (tv.tv_sec - t0.tv_sec) + (tv.tv_usec - t0.tv_usec) / 1e6;
            printf("[%7.3fs] %-4s %-12s = %d\n", e2, evname(ev[i].type),
                   codename(ev[i].type, ev[i].code), ev[i].value);
        }
        fflush(stdout);
    }
    close(fd);
    printf("hidmon: 结束\n");
    return 0;
}
