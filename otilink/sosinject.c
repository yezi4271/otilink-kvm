/*
 * sosinject.c —— 注入一次「左 Meta + H」（麒麟内置 SOS 组合键），仅用于验证 sosguard
 *
 * 为什么需要它：麒麟的 kysos_key 在内核 input 层挂钩所有键盘，
 * 只有真实输入事件（uinput 也算）才会触发；XTEST/xdotool 不走内核，测不出来。
 *
 * 用法：./sosinject          注入一次组合键后退出
 *       ./sosinject --wait 3 建好设备后等 3s 再注入（等 sos-handler 连上）
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

int main(int argc, char **argv)
{
    int wait_s = 1;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc)
            wait_s = atoi(argv[++i]);

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); return 1; }

    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 1; i < 128; i++)
        ioctl(fd, UI_SET_KEYBIT, i);

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_USB;
    us.id.vendor  = 0x4f54;                 /* 'OT' */
    us.id.product = 0x0003;
    snprintf(us.name, UINPUT_MAX_NAME_SIZE, "otilink-sostest");
    ioctl(fd, UI_DEV_SETUP, &us);
    if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); close(fd); return 1; }

    sleep(wait_s > 0 ? wait_s : 1);         /* 等内核 sos-handler 连上新设备 */

    emit(fd, EV_KEY, KEY_LEFTMETA, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(50000);
    emit(fd, EV_KEY, KEY_H, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(50000);
    emit(fd, EV_KEY, KEY_H, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(50000);
    emit(fd, EV_KEY, KEY_LEFTMETA, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(200000);

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    printf("sosinject: sent LEFTMETA+H\n");
    return 0;
}
