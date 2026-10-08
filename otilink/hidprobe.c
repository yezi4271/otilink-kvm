/*
 * hidprobe.c —— 从本机往对拷线写 HID 包，判定"本机 → 对端"的 HID 方向是否真的通。
 * 用法: ./hidprobe /dev/sg3 1 10      # 鼠标：右移 40px × 10 包
 *       ./hidprobe /dev/sg3 2 3 [usage]  # 键盘：usage 按/松 × 3 轮（默认 0x87=F24）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "otilink.h"
#include "otihid.h"

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/sg3";
    int type = argc > 2 ? atoi(argv[2]) : 1;
    int n = argc > 3 ? atoi(argv[3]) : 10;
    int usage = argc > 4 ? (int)strtol(argv[4], NULL, 0) : 0x87;   /* 默认 F24 */
    static struct otilink_dev d;

    if (otilink_open(&d, dev) != 0) {
        fprintf(stderr, "open %s failed\n", dev);
        return 1;
    }
    uint8_t pk[14 * 8];
    for (int i = 0; i < n; i++) {
        int rc = -1;
        if (type == OTI_HID_TYPE_MOUSE) {
            int np = otihid_mouse_packets(0, 40, 0, 0, pk, 8);
            for (int j = 0; j < np; j++)
                rc = otilink_send_hid(&d, OTI_HID_TYPE_MOUSE, pk + j * 14);
        } else {
            uint8_t k[14];
            memset(k, 0, sizeof k);
            k[2] = (uint8_t)usage;             /* F24 = 0x87 (usage 0x73) */
            rc = otilink_send_hid(&d, OTI_HID_TYPE_KBD, k);
        }
        printf("send #%d rc=%d\n", i, rc);
        fflush(stdout);
        usleep(120 * 1000);
    }
    if (type == OTI_HID_TYPE_KBD) {            /* 补一个"全松"包 */
        uint8_t k[14];
        memset(k, 0, sizeof k);
        otilink_send_hid(&d, OTI_HID_TYPE_KBD, k);
    }
    otilink_close(&d);
    return 0;
}
