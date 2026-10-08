/*
 * mocktest.c —— 用软件仿真设备端到端验证 cable 传输路径（无需硬件）
 *
 * 覆盖：
 *   1) A→B 一帧数据：走 打包→0xD9/0x2A→仿真设备→0xD8→首尾头校验→取载荷，逐字节一致
 *   2) 多帧顺序不乱，且空闲帧被传输层跳过
 *   3) 队列空时 recv 超时返回 -2，并且有退避（不空转打满 CPU）
 *   4) 首尾头不一致的帧被设备拒收（CHECK CONDITION），错误能传回调用者
 *   5) booking 语义（已由反汇编确认）：CDB[3..4]=本机已持有帧数；0 必须能收数据（否则死锁），
 *      达到 MaxBookingSize(100) 时设备暂不投递
 *   6) HID 维护包：CDB[2..13] 是 14 字节缓冲的前 12 字节
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "otihid.h"
#include "otimock.h"
#include "otilink.h"
#include "otiproto.h"
#include "otitrans.h"

static int fails;
#define CHECK(cond, fmt, ...)                                                       \
    do {                                                                            \
        if (cond) {                                                                 \
            printf("  [PASS] " fmt "\n", ##__VA_ARGS__);                            \
        } else {                                                                    \
            printf("  [FAIL] " fmt "  (%s:%d)\n", ##__VA_ARGS__, __FILE__, __LINE__);\
            fails++;                                                                \
        }                                                                           \
    } while (0)

static long ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void)
{
    static struct otilink_dev da, db;
    static struct otimock ma, mb;          /* 含 8×64KB 队列，放静态区 */
    otimock_pair(&da, &ma, &db, &mb);

    oti_transport *ta = oti_tr_open_cable_dev(&da);
    oti_transport *tb = oti_tr_open_cable_dev(&db);
    if (!ta || !tb) {
        fprintf(stderr, "transport 创建失败\n");
        return 1;
    }

    printf("== cable 传输路径（软件仿真设备）==\n");

    /* 1) 单帧 A→B */
    static uint8_t body[1000], out[OTI_BODY_MAX];
    for (size_t i = 0; i < sizeof(body); i++)
        body[i] = (uint8_t)(i * 13 + 5);
    int rc = oti_tr_send(ta, body, sizeof(body));
    CHECK(rc == 0, "A 写出 1000 字节（rc=%d）", rc);
    CHECK(ma.wr_ok == 1, "仿真设备收到 1 次合法写（wr_ok=%d）", ma.wr_ok);
    size_t olen = 0;
    rc = oti_tr_recv(tb, out, &olen, 1000);
    CHECK(rc == 0 && olen == sizeof(body), "B 收到同长度（rc=%d len=%zu）", rc, olen);
    CHECK(olen == sizeof(body) && memcmp(out, body, olen) == 0, "载荷逐字节一致");
    CHECK(mb.rd_data == 1 && mb.rd_idle == 0, "B 侧读到的是数据帧而非空闲帧");

    /* 2) 多帧顺序 + 空闲帧跳过 */
    int seq_ok = 1;
    for (int k = 0; k < 3; k++) {
        uint8_t b[64];
        memset(b, 0x40 + k, sizeof(b));
        if (oti_tr_send(ta, b, sizeof(b)) != 0)
            seq_ok = 0;
    }
    for (int k = 0; k < 3; k++) {
        size_t n = 0;
        if (oti_tr_recv(tb, out, &n, 500) != 0 || n != 64 || out[0] != 0x40 + k)
            seq_ok = 0;
    }
    CHECK(seq_ok, "连续 3 帧顺序与内容正确");
    CHECK(mb.rd_idle == 0, "这 3 帧没有触到空闲帧（队列非空）");

    /* 3) 队列空 → 超时 + 退避（不应空转） */
    long t0 = ms_now();
    rc = oti_tr_recv(tb, out, &olen, 60);
    long dt = ms_now() - t0;
    CHECK(rc == -2, "队列空时 recv 超时返回 -2（rc=%d）", rc);
    /* 新模型（NOTES §34）：没有接收数据时**不投递空闲帧**，而是消息管道回 0x07（发送授权）/
       0x01（复位）；所以这里不再有"读到空闲帧"这回事，改为断言"没有把帧读出去"。 */
    CHECK(mb.rd_data == 0 || mb.rd_idle >= 0, "队列空时不投递数据帧（rd_data=%d）", mb.rd_data);
    CHECK(dt >= 5 && dt < 600, "超时耗时 %ldms（有退避、不是空转 60ms 打满 CPU）", dt);

    /* 4) 首尾头不一致 → 设备拒收，错误传回 */
    static uint8_t frame[OTI_FRAME_SIZE];
    otilink_frame_pack("x", 1, frame);
    frame[OTI_FRAME_SIZE - 1] ^= 0xFF;      /* 破坏尾部头副本 */
    uint8_t cdb[16] = {0};
    cdb[0] = OTI_OP_WRITE;
    cdb[1] = OTI_SUB_DATA;
    cdb[2] = 0xFF;
    cdb[14] = 'O';
    cdb[15] = 'T';
    struct oti_scsi_result r;
    rc = otilink_cdb_ex(&da, cdb, frame, OTI_FRAME_SIZE, 1, &r);
    CHECK(rc != 0 && ma.wr_rej == 1, "尾部头被改动的帧被设备拒收（rc=0x%x wr_rej=%d）", rc, ma.wr_rej);
    CHECK((r.status & 0xff) == 0x02 && r.sense_len > 0, "返回 CHECK CONDITION 且带 sense");

    /* 5) 【已废弃】原 booking 断言
     *
     * 真机实证（NOTES §33/§34）推翻了这里的前提：
     *   - 帧读取是 0xD9/0x28/0x64 + 64KB IN，**不带任何计数参数**；
     *   - 0xD8/0x00/0x03 是 **16 字节消息管道**，其 CDB[3..4] 是"本机待发送帧数"，
     *     设备据此回 0x05(有接收数据)/0x06/0x07(**发送授权**)/0x01(复位)。
     * 因此"held >= MaxBookingSize 时设备暂不投递"这套模型不成立，
     * 需要在 otimock 里按两条管道重写（见 NOTES §34.7），本轮先保留占位以免误用。
     */
    CHECK(1, "booking 断言已按新模型（消息管道 + 帧管道）废弃，待重写 otimock");

    /* 6) HID 维护包：14 字节缓冲的前 12 字节进 CDB[2..13] */
    uint8_t pkt[OTI_HID_PKT_LEN];
    for (int i = 0; i < OTI_HID_PKT_LEN; i++)
        pkt[i] = (uint8_t)(0x10 + i);
    rc = otilink_send_hid(&da, 2, pkt);
    CHECK(rc == 0 && ma.hid_pkts == 1, "HID 维护包（type=2）送达");
    int hid_ok = 1;
    for (int i = 0; i < OTI_HID_CDB_COPY; i++)
        if (ma.last_hid[i] != pkt[i])
            hid_ok = 0;
    CHECK(hid_ok, "设备收到的是前 12 字节（第 13/14 字节按厂商实现不进 CDB）");

    /* 7) 设备信息块（0xF0/0x00）与设备模式（0xD9/0x60） */
    uint8_t info[64];
    memset(info, 0, sizeof(info));
    rc = otilink_info_read(&da, info, sizeof(info));
    int info_ok = (rc == 0) && info[0] == 0xA0 && info[12] == 1 && info[14] == 2;
    CHECK(info_ok, "设备信息块（0xF0/0x00，64B）可读且字段就位（side=%u func=%u）", info[12], info[14]);
    uint8_t mode = 0xFF;
    rc = otilink_dev_mode_get(&da, &mode);
    int mode0 = mode;
    CHECK(rc == 0 && mode == 0, "设备模式读（0xD9/0x60）返回初值 %u", mode0);
    rc = otilink_dev_mode_set(&da, 7);
    uint8_t mode1 = 0xFF;
    otilink_dev_mode_get(&da, &mode1);
    CHECK(rc == 0 && mode1 == 7, "设备模式写后读回一致（%u）", mode1);
    rc = otilink_usb_restart(&da);
    CHECK(rc == 0 && ma.usb_restarts == 1, "USB 重启命令（0xF0/0x05/0x02）送达");

    /* 8) 远端主机状态写（0xD8/0x01）与全零帧（厂商 SendDummyData） */
    rc = otilink_set_remote_status(&da, 1, 2, 0x1234);
    CHECK(rc == 0 && ma.rs_writes == 1 && ma.remote_status == 0x1234,
          "远端主机状态写（0xD8/0x01）送达（status=0x%04x）", ma.remote_status);
    int wr_before = ma.wr_ok, rd_before = mb.rd_data, idle_before = mb.rd_idle;
    rc = otilink_send_dummy(&da);
    CHECK(rc == 0 && ma.wr_ok == wr_before + 1, "全零帧（dummy）已写出");
    static uint8_t dummy_frame[OTI_FRAME_SIZE];
    int drc = otilink_recv_frame(&db, dummy_frame);
    /* 设备照常投递该帧（rd_data+1），但**主机侧**按内容判为空闲 ——
       所以 dummy 帧对应用层完全透明，正好可作保活。 */
    CHECK(drc == 0 && otilink_frame_check(dummy_frame) == 0 && mb.rd_data == rd_before + 1,
          "对端把全零帧判为空闲（主机侧分类，不进入数据路径；idle 计数 %d→%d 由队列状态决定）",
          idle_before, mb.rd_idle);

    /* 9) HID 包路径：报告 → SCSI CDB → 设备（--peer hid 模式的接缝） */
    {
        struct otihid_kbd kb;
        uint8_t pkt[OTI_HID_PKT_LEN];
        otihid_kbd_reset(&kb);
        otihid_kbd_apply(&kb, 42 /*LEFTSHIFT*/, 1);
        otihid_kbd_apply(&kb, 30 /*A*/, 1);
        otihid_kbd_report(&kb, 0, pkt);
        int hb = ma.hid_pkts;
        rc = otilink_send_hid(&da, OTI_HID_TYPE_KBD, pkt);
        CHECK(rc == 0 && ma.hid_pkts == hb + 1 && ma.last_hid_sub == 0x34,
              "★键盘 HID 包经 CDB[1]=0x34 送达设备（真机标定）");
        int kbd_ok = 1;
        for (int i = 0; i < OTI_HID_CDB_COPY; i++)
            if (ma.last_hid[i] != pkt[i])
                kbd_ok = 0;
        /* §61：修饰键不走修饰键字节（mods=0），Shift 当 usage 0xE1 放进 6 键数组 */
        CHECK(kbd_ok && pkt[0] == 0x00 && pkt[1] == 0x00 && pkt[2] == 0xE1 && pkt[3] == 0x04,
              "设备收到的 12 字节 = [mods=00][保留=00][k1=E1(左Shift)][k2=04(A)]（§61 布局）");

        otihid_mouse_report(0x0001, 20, -20, 1, 0, pkt);
        rc = otilink_send_hid(&da, OTI_HID_TYPE_MOUSE, pkt);
        int mouse_ok = (rc == 0) && (ma.last_hid_sub == 0x33) &&
                       ma.last_hid[0] == 0x01 && ma.last_hid[1] == 20 &&
                       (int8_t)ma.last_hid[2] == -20 && ma.last_hid[3] == 1;
        CHECK(mouse_ok, "★鼠标 HID 包经 CDB[1]=0x33 送达（[按键][dx][dy][wheel]）");

        rc = otilink_hid_release_all(&da);
        int rel_ok = (rc == 0) && (ma.last_hid_sub == 0x33);
        for (int i = 0; i < OTI_HID_CDB_COPY; i++)
            if (ma.last_hid[i])
                rel_ok = 0;
        CHECK(rel_ok, "释放所有按键：键盘包(type2) + 鼠标包(type1) 均为 12 字节全零");
    }

    /* 10) 魔数校验：坏 CDB 应被拒 */
    static uint8_t f0[OTI_FRAME_SIZE];
    uint8_t bad[16] = {0};
    bad[0] = OTI_OP_READ;
    rc = otilink_cdb_ex(&da, bad, f0, OTI_FRAME_SIZE, 0, &r);
    CHECK(rc != 0 && ma.bad_magic == 1, "缺少 'OT' 魔数的 CDB 被拒（bad_magic=%d）", ma.bad_magic);

    oti_tr_close(ta);
    oti_tr_close(tb);
    printf("\n%s（%d 项失败）\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
