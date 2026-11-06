/*
 * hidlat.c —— 测"往线缆写 HID 包"的延迟/间隔：把"键鼠过几秒卡一下"变成可判定的数字。
 *
 * 用法： ./hidlat /dev/sgN [秒数] [最大可接受间隔 ms] [move_dx] [move_chunk]
 *        退出码 = 超过阈值的间隔个数（0 = 通过）；最后一行固定打印
 *        MAXGAP=<ms> GAPS_OVER_<阈值>=<n> SEND_FAIL=<n> [MOVE_DX=<px> CHUNK=<n>]
 *
 *        move_dx（可选，默认 0 = 历史行为：发 dy=±1 的微动包）：
 *          >0 时发 dx=**单向** move_dx 的鼠标包（每 10ms 一包），让对端（Windows）光标
 *          **持续移动**；每 move_chunk 包换一次方向（默认 1 = 每包交替）。这是 winfreeze.sh
 *          的合成驱动：光标位置持续在变，一旦 HID 写被单队列设备堵住，对端光标就"停住"
 *          → 可用 1ms 采样量出 MAXFREEZE。
 *          ⚠️ 真机实测（2026-11-06）：每包交替（chunk=1）时 Windows 光标只挪 1~2px/次
 *          （方向交替的相对位移被对端合并/加速抵消），**chunk=20、dx=10 才能产生
 *          持续可见的位移**（每次方向持续 200ms），所以别用 chunk=1 去驱动光标采样。
 *          默认 0 时行为与旧版完全一致（`./hidlat.sh [秒数] [ms]` 不受影响）。
 *
 * 为什么需要它（2026-09-21 真机）：HID 包和帧管道共用同一台**单队列**设备，任何
 * 大块/失败的设备操作都会把 HID 写堵住。实测保活里的 64KB dummy 帧单次要 ~1.3 秒
 * 且常常直接失败（rc=526080）→ 用户感知"键鼠每几秒冻结一下"。hidlat 与 otikm
 * **同时跑**（真实竞争）才能复现；修复后 maxgap 应回到个位数毫秒。
 *
 * 2026-11-06 补充：Windows 侧用原生快速读帧循环时，同场景 MAXGAP 从 372~482ms
 * 回到 14ms（见 /tmp/redesign-brief.md §1）——本工具的 GAP 行 + 对端光标采样
 * （otilink/cursortrace.cs）是这一结论的两个独立证据。
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
    int move_dx = argc > 4 ? atoi(argv[4]) : 0;   /* 0 = 旧行为（向后兼容） */
    int move_chunk = argc > 5 ? atoi(argv[5]) : 1; /* 每 N 包换方向（chunk>1 才能让对端光标持续位移） */
    if (move_dx < 0)
        move_dx = 0;
    if (move_chunk < 1)
        move_chunk = 1;
    static struct otilink_dev d;
    if (otilink_open(&d, dev) != 0) {
        fprintf(stderr, "open %s failed\n", dev);
        return 1;
    }
    if (move_dx > 0)
        fprintf(stderr, "hidlat: 合成驱动模式 move_dx=%d chunk=%d（每 10ms 一包，每 %d 包换向）\n",
                move_dx, move_chunk, move_chunk);
    uint8_t pk[14 * 8];
    long t0 = now_ms(), prev = t0, maxgap = 0;
    int ngap = 0, nfail = 0, sgn = 1, sent = 0;
    while (now_ms() - t0 < secs * 1000L) {
        int np;
        if (move_dx > 0) {
            /* 同向连续 move_chunk 包再换向：方向反复交替的相对位移会被对端合并/加速
               抵消（真机只挪 1~2px），必须让光标沿同方向走一小段才可观测。 */
            if (sent % move_chunk == 0)
                sgn = -sgn;
            np = otihid_mouse_packets(0, sgn * move_dx, 0, 0, pk, 8);
            sent++;
        } else {
            np = otihid_mouse_packets(0, 0, (int)(now_ms() % 3) - 1, 0, pk, 8);
        }
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
    if (move_dx > 0)
        printf("MAXGAP=%ld GAPS_OVER_%d=%d SEND_FAIL=%d MOVE_DX=%d CHUNK=%d\n",
               maxgap, max_ok, ngap, nfail, move_dx, move_chunk);
    else
        printf("MAXGAP=%ld GAPS_OVER_%d=%d SEND_FAIL=%d\n", maxgap, max_ok, ngap, nfail);
    otilink_close(&d);
    return ngap > 255 ? 255 : ngap;
}
