/*
 * hidlat.c —— 测"往线缆写 HID 包"的延迟/间隔：把"键鼠过几秒卡一下"变成可判定的数字。
 *
 * 用法： ./hidlat /dev/sgN [秒数] [最大可接受间隔 ms]
 *        退出码 = 超过阈值的间隔个数（0 = 通过）；最后一行固定打印 MAXGAP=<ms>。
 *
 * 为什么需要它（2026-09-21 真机）：HID 包和帧管道共用同一台**单队列**设备，任何
 * 大块/失败的设备操作都会把 HID 写堵住。实测保活里的 64KB dummy 帧单次要 ~1.3 秒
 * 且常常直接失败（rc=526080）→ 用户感知"键鼠每几秒冻结一下"。hidlat 与 otikm
 * **同时跑**（真实竞争）才能复现；修复后 maxgap 应回到个位数毫秒。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "otilink.h"
#include "otihid.h"

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/sg3";
    int secs = argc > 2 ? atoi(argv[2]) : 14;
    int max_ok = argc > 3 ? atoi(argv[3]) : 200;
    static struct otilink_dev d;
    if (otilink_open(&d, dev) != 0) {
        fprintf(stderr, "open %s failed\n", dev);
        return 1;
    }
    uint8_t pk[14 * 8];
    long t0 = now_ms(), prev = t0, maxgap = 0;
    int ngap = 0, nfail = 0;
    while (now_ms() - t0 < secs * 1000L) {
        int np = otihid_mouse_packets(0, (int)(now_ms() % 3) - 1, 0, 0, pk, 8);
        long a = now_ms();
        int rc = 0;
        for (int j = 0; j < np; j++)
            rc = otilink_send_hid(&d, OTI_HID_TYPE_MOUSE, pk + j * 14);
        long b = now_ms();
        long gap = b - prev;
        prev = b;
        if (gap > maxgap)
            maxgap = gap;
        if (gap > 30)
            printf("GAP %4ldms (send %ldms rc=%d) t=%ldms\n", gap, b - a, rc, b - t0);
        if (gap > max_ok)
            ngap++;
        if (rc)
            nfail++;
        usleep(10000);
    }
    printf("MAXGAP=%ld GAPS_OVER_%d=%d SEND_FAIL=%d\n", maxgap, max_ok, ngap, nfail);
    otilink_close(&d);
    return ngap > 255 ? 255 : ngap;
}
