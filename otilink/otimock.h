/*
 * otimock.h —— OTi 对拷线的软件仿真设备（用于在没有硬件时验证 cable 传输路径）
 *
 * 按 PROTOCOL.md 逆向出的语义实现：
 *   - CDB[14..15] 必须是 'O','T'，否则非法请求
 *   - 0xD9/0x2A + 65536B 数据：把帧投递给"对端"队列；可选校验首尾头一致
 *   - 0xD8：从本机队列取帧；队列空 → 返回全零（空闲帧）；可选 credit 语义（CDB[4]==0 不给数据）
 *   - 0xD9/0x33|0x34|0x36：HID 维护包（记录 CDB[2..13] 的 12 字节，无数据阶段）
 *   - 0xF0/0x00：返回固定状态块；0xF0/0x31：锁
 */
#ifndef OTIMOCK_H
#define OTIMOCK_H

#include <stdint.h>

#include "otilink.h"
#include "otiproto.h"

#define OTIMOCK_QSLOTS 8          /* 仿真设备的接收环形队列槽数（真机为 200 个缓冲） */
#define OTIMOCK_MAX_BOOKING 100   /* 厂商 MaxBookingSize 默认值（构造函数实测） */

struct otimock {
    uint8_t  q[OTIMOCK_QSLOTS][OTI_FRAME_SIZE];  /* 待本端取走的帧 */
    int      head, tail, count;                  /* 环形队列 */
    struct otimock *peer;         /* 数据要投递到哪一端 */

    int      locked;
    int      window_gate;         /* 1 = 报数 >= MaxBookingSize 时暂不投递（窗口满） */
    int      tail_check;          /* 1 = 校验首尾 20 字节头一致 */

    uint8_t  status[16];          /* 0xF0/0x00 查询返回的固定块 */
    uint8_t  last_hid[OTI_HID_CDB_COPY];
    uint8_t  last_hid_sub;       /* 最近一次 HID 包的 CDB[1]（0x33/0x34/0x36）*/

    /* 统计 */
    int      rd_idle, rd_data, wr_ok, wr_rej, queries, hid_pkts, bad_magic, q_drop;
    int      usb_restarts;
    uint8_t  dev_mode;
    int      rs_writes;          /* 0xD8/0x01 远端状态写入次数 */
    uint16_t remote_status;
};

/* 建一对互连的仿真设备，并把两个 otilink_dev 接到各自的仿真后端上 */
void otimock_pair(struct otilink_dev *a, struct otimock *ma,
                  struct otilink_dev *b, struct otimock *mb);

void otimock_init(struct otimock *m);

#endif /* OTIMOCK_H */
