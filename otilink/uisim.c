/*
 * uisim.c —— 创建 uinput 虚拟键鼠并注入事件（无物理键鼠时的端到端验证用）
 *
 * 用途：麒麟上没有物理键鼠可操作时，用它在 /dev/input 下造一对虚拟键鼠，
 *       让 otikm --capture 它们，然后注入事件 → 走完整生产路径（evdev 抓取 +
 *       EVIOCGRAB + 状态机 + 协议 + 线缆）→ 到 Windows 被注入。
 *
 * 用法: ./uisim [--delay 秒] [--moves N] [--hold 秒] [--wait-file F]
 *   启动后把两个 event 节点路径写到 /tmp/uisim.paths（每行一个），
 *   等 --delay 秒后开始注入：鼠标向右推 N×60（会越过屏幕边缘触发切换），
 *   然后按一下 CapsLock（可用 [Console]::CapsLock 客观验证是否注入成功）。
 */
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void emit(int fd, int type, int code, int val)
{
    struct input_event ie;
    memset(&ie, 0, sizeof(ie));
    ie.type = (unsigned short)type;
    ie.code = (unsigned short)code;
    ie.value = val;
    if (write(fd, &ie, sizeof(ie)) < 0)
        perror("write input_event");
}

static int make_dev(int mouse)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    if (mouse) {
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        ioctl(fd, UI_SET_RELBIT, REL_X);
        ioctl(fd, UI_SET_RELBIT, REL_Y);
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
        ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
        ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    } else {
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (int i = 1; i < 128; i++)
            ioctl(fd, UI_SET_KEYBIT, i);
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB;
    us.id.vendor = 0x4f54;                       /* 'OT' */
    us.id.product = mouse ? 0x0002 : 0x0001;
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "otilink-uisim-%s", mouse ? "mouse" : "kbd");
    ioctl(fd, UI_DEV_SETUP, &us);
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* 扫 /dev/input/event* 找名字匹配的设备节点 */
static int find_by_name(const char *want, char *out, size_t outcap)
{
    for (int i = 0; i < 64; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 &&
            strcmp(name, want) == 0) {
            close(fd);
            snprintf(out, outcap, "%s", p);
            return 0;
        }
        close(fd);
    }
    return -1;
}

int main(int argc, char **argv)
{
    int delay = 10, moves = 30, hold = 6;
    const char *wait_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--delay") == 0 && i + 1 < argc)
            delay = atoi(argv[++i]);
        else if (strcmp(argv[i], "--moves") == 0 && i + 1 < argc)
            moves = atoi(argv[++i]);
        else if (strcmp(argv[i], "--hold") == 0 && i + 1 < argc)
            hold = atoi(argv[++i]);
        else if (strcmp(argv[i], "--wait-file") == 0 && i + 1 < argc)
            wait_file = argv[++i];
    }

    int m = make_dev(1), k = make_dev(0);
    if (m < 0 || k < 0) {
        fprintf(stderr, "创建 uinput 设备失败（/dev/uinput 权限？）\n");
        return 1;
    }
    usleep(400 * 1000);

    char pm[64] = {0}, pk[64] = {0};
    int okm = (find_by_name("otilink-uisim-mouse", pm, sizeof pm) == 0);
    int okk = (find_by_name("otilink-uisim-kbd", pk, sizeof pk) == 0);
    FILE *f = fopen("/tmp/uisim.paths", "w");
    if (f) {
        if (okm) fprintf(f, "%s\n", pm);
        if (okk) fprintf(f, "%s\n", pk);
        fclose(f);
    }
    printf("虚拟鼠标: %s\n虚拟键盘: %s\n", okm ? pm : "(未找到)", okk ? pk : "(未找到)");
    if (wait_file) {
        /* 等触发文件出现再注入（kbdexcl 用：消掉"起探针/起 otikm"的 SSH 延迟竞态） */
        printf("等待触发文件 %s 出现…（注入结束后保持设备 %d 秒）\n", wait_file, hold);
        fflush(stdout);
        for (int i = 0; i < 600; i++) {           /* 最多 120 秒 */
            if (access(wait_file, F_OK) == 0)
                break;
            usleep(200 * 1000);
        }
    } else {
        printf("等待 %d 秒后开始注入…（注入结束后保持设备 %d 秒）\n", delay, hold);
        fflush(stdout);
        sleep((unsigned)delay);
    }

    printf("注入：鼠标向右 %d×60（越过屏幕边缘即切换到对端）\n", moves);
    fflush(stdout);
    for (int i = 0; i < moves; i++) {
        emit(m, EV_REL, REL_X, 60);
        emit(m, EV_SYN, SYN_REPORT, 0);
        usleep(120 * 1000);
    }
    printf("注入：CapsLock 按下+松开\n");
    fflush(stdout);
    emit(k, EV_KEY, KEY_CAPSLOCK, 1);
    emit(k, EV_SYN, SYN_REPORT, 0);
    usleep(80 * 1000);
    emit(k, EV_KEY, KEY_CAPSLOCK, 0);
    emit(k, EV_SYN, SYN_REPORT, 0);
    usleep(500 * 1000);
    /* 扩展键验证：LeftMeta（evdev 125）需要 E0 前缀，主键区之外 */
    printf("注入：LeftMeta（扩展键）按下+松开\n");
    fflush(stdout);
    emit(k, EV_KEY, KEY_LEFTMETA, 1);
    emit(k, EV_SYN, SYN_REPORT, 0);
    usleep(80 * 1000);
    emit(k, EV_KEY, KEY_LEFTMETA, 0);
    emit(k, EV_SYN, SYN_REPORT, 0);
    usleep(500 * 1000);
    /* 热键抑制验证：注入 ctrl+alt+space（默认的"切换控制权"热键）。
       抑制生效的话，对端**一个按键都不该收到**；泄漏的话对端会看到 Ctrl/Alt/Space。 */
    printf("注入：ctrl+alt+space（热键，应被整体抑制、不泄漏给对端）\n");
    fflush(stdout);
    emit(k, EV_KEY, KEY_LEFTCTRL, 1); emit(k, EV_SYN, SYN_REPORT, 0); usleep(40 * 1000);
    emit(k, EV_KEY, KEY_LEFTALT, 1);  emit(k, EV_SYN, SYN_REPORT, 0); usleep(40 * 1000);
    emit(k, EV_KEY, KEY_SPACE, 1);    emit(k, EV_SYN, SYN_REPORT, 0); usleep(60 * 1000);
    emit(k, EV_KEY, KEY_SPACE, 0);    emit(k, EV_SYN, SYN_REPORT, 0); usleep(40 * 1000);
    emit(k, EV_KEY, KEY_LEFTALT, 0);  emit(k, EV_SYN, SYN_REPORT, 0); usleep(40 * 1000);
    emit(k, EV_KEY, KEY_LEFTCTRL, 0); emit(k, EV_SYN, SYN_REPORT, 0);
    printf("注入完成\n");
    fflush(stdout);

    sleep((unsigned)hold);
    ioctl(m, UI_DEV_DESTROY);
    ioctl(k, UI_DEV_DESTROY);
    close(m);
    close(k);
    return 0;
}
