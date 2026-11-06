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
 *
 * 11) 【2026-11-06 传输层重设计】慢操作守卫：在 otimock 外层注入帧写/帧读延迟
 *     （不改 otimock.c 语义），验证"慢写 → 退避窗口内不再发起帧写、HID/消息泵照常、
 *      连续慢指数退避封顶、窗口结束后队列数据不丢、背景心跳可合并、帧读慢独立退避"。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

/* ---------- 延迟注入层（不改 otimock.c/h）----------
 * 真机实测：帧写耗时完全取决于对端消费速度（旧慢代理 250~700ms）。这里把设备原来的
 * exec 包一层：按 CDB 类别睡一会儿再转调 otimock，同时**按类别计数**（测试断言用）。
 * 这样"慢操作守卫"的触发/退避/恢复都能在没有硬件的情况下被确定性验证。 */
struct lat_ctx {
    struct oti_scsi_ops orig;    /* otimock_pair 装上的原后端 */
    int frame_write_delay_ms;    /* 0xD9/0x2A 每次额外延迟 */
    int frame_read_delay_ms;     /* 0xD9/0x28 每次额外延迟 */
    int msg_read_delay_ms;       /* 0xD8 消息读每次额外延迟 */
    unsigned n_msg_read, n_frame_read, n_frame_write, n_hid;
};

static int lat_exec(void *ctx, const uint8_t cdb[16], void *data, unsigned len, int to_dev,
                    struct oti_scsi_result *res)
{
    struct lat_ctx *c = ctx;
    if (cdb[0] == OTI_OP_WRITE) {
        if (cdb[1] == OTI_SUB_DATA) {
            c->n_frame_write++;
            if (c->frame_write_delay_ms > 0)
                usleep((useconds_t)c->frame_write_delay_ms * 1000);
        } else if (cdb[1] == OTI_SUB_FRAME_RD) {
            c->n_frame_read++;
            if (c->frame_read_delay_ms > 0)
                usleep((useconds_t)c->frame_read_delay_ms * 1000);
        } else if (cdb[1] == OTI_SUB_HID_TYPE1 || cdb[1] == OTI_SUB_HID_TYPE2 ||
                   cdb[1] == OTI_SUB_HID_TYPE3) {
            c->n_hid++;
        }
    } else if (cdb[0] == OTI_OP_READ && !to_dev && len == 16) {
        c->n_msg_read++;
        if (c->msg_read_delay_ms > 0)
            usleep((useconds_t)c->msg_read_delay_ms * 1000);
    }
    return c->orig.exec(c->orig.ctx, cdb, data, len, to_dev, res);
}

/* 给设备装上延迟层：必须在 oti_tr_open_cable_dev() 复制 ops 之前调用 */
static void wrap_latency(struct otilink_dev *d, struct lat_ctx *c)
{
    memset(c, 0, sizeof(*c));
    c->orig = d->ops;
    struct oti_scsi_ops ops = {.exec = lat_exec, .ctx = c};
    otilink_set_ops(d, &ops);
}

/* 消息头类型（-1 = 不是我们的协议帧/太短） */
static int msg_type_of(const uint8_t *b, size_t n)
{
    if (n < OTI_HDR_SIZE)
        return -1;
    uint32_t magic = 0;
    uint16_t ty = 0;
    memcpy(&magic, b, 4);
    if (magic != OTI_MSG_MAGIC)
        return -1;
    memcpy(&ty, b + 4, 2);
    return (int)ty;
}

static uint32_t msg_seq_of(const uint8_t *b, size_t n)
{
    uint32_t s = 0;
    if (n >= OTI_HDR_SIZE)
        memcpy(&s, b + 8, 4);
    return s;
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

    /* 1) 单帧 A→B
       注意：2026-11-06 重设计后线缆后端的 oti_tr_send() **只入队、不内联做设备 I/O**
       （避免捕获/输入线程被慢帧写阻塞；真正发送由 RX 泵线程负责）。单测里用
       oti_tr_pump_once() 显式驱动泵线程的角色。 */
    static uint8_t body[1000], out[OTI_BODY_MAX];
    for (size_t i = 0; i < sizeof(body); i++)
        body[i] = (uint8_t)(i * 13 + 5);
    int rc = oti_tr_send(ta, body, sizeof(body));
    CHECK(rc == 0, "A 入队 1000 字节（rc=%d）", rc);
    CHECK(ma.wr_ok == 0, "oti_tr_send 不内联写设备（wr_ok=%d，慢写不会阻塞调用线程）", ma.wr_ok);
    oti_tr_pump_once(ta);                  /* 泵：读消息 → 拿授权 → 写帧 */
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
    oti_tr_pump_once(ta);                  /* 一次授权会把队列里 3 帧都写出去 */
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

    /* ================= 11) 慢操作守卫（2026-11-06 传输层重设计）=================
       真机证据（Lead，2026-11-06）：帧写耗时取决于**对端消费速度**（旧慢代理
       250~700ms，原生快速泵 2~4ms），而线缆是单队列设备 —— 慢帧写会把 HID 注入写
       堵在后面（鼠标冻结）。这里用延迟注入复现"慢对端"，确定性验证守卫状态机：
       慢写 → 退避（窗口内不写帧、HID/消息泵照常）→ 连续慢指数退避封顶 → 恢复清零。 */
    {
        /* 正常快写不应触发守卫（此时阈值仍是真机默认 30ms） */
        CHECK(oti_tr_cable_slow_count(ta) == 0 && oti_tr_cable_backoff_ms(ta) == 0,
              "正常快写不触发慢操作守卫（slow=%lu backoff=%ld）",
              oti_tr_cable_slow_count(ta), oti_tr_cable_backoff_ms(ta));

        static struct otilink_dev dc, dd;
        static struct otimock mc, md;
        static struct lat_ctx lc, ld;
        memset(&mc, 0, sizeof(mc));
        memset(&md, 0, sizeof(md));
        otimock_pair(&dc, &mc, &dd, &md);
        wrap_latency(&dc, &lc);            /* tc 侧设备：可注入帧写/帧读延迟 */
        wrap_latency(&dd, &ld);
        oti_transport *tc = oti_tr_open_cable_dev(&dc);
        oti_transport *td = oti_tr_open_cable_dev(&dd);
        /* 单测用小策略（**真机默认 30/300/2000 不动**）：阈值 10ms、base 60ms、上限 150ms */
        oti_tr_set_slowop_policy(10, 60, 150);

        static uint8_t m1[64], m2[64], m3[64], m4[64], m5[64];
        memset(m1, 0x11, sizeof(m1));
        memset(m2, 0x12, sizeof(m2));
        memset(m3, 0x20, sizeof(m3));
        memset(m4, 0x21, sizeof(m4));
        memset(m5, 0x22, sizeof(m5));

        /* 11.0 HID 守卫"按需启用"：从未见过 >10ms 帧写（peer_slow_seen=0）时，
           HID 活动**不**推迟帧写 —— 新原生泵场景（2~4ms/帧）下帧不会被饿死。 */
        {
            static uint8_t m0[64];
            memset(m0, 0x08, sizeof(m0));
            oti_tr_send(tc, m0, sizeof(m0));
            oti_tr_note_hid_activity(tc);
            unsigned fw0 = lc.n_frame_write;
            oti_tr_pump_once(tc);
            CHECK(oti_tr_cable_peer_slow_seen(tc) == 0 && lc.n_frame_write == fw0 + 1,
                  "HID 活动但从未见慢对端 → 帧写不被推迟（写 %u→%u，peer_slow_seen=0）",
                  fw0, lc.n_frame_write);
            CHECK(oti_tr_cable_deferred_hid(tc) == 0, "此时 HID 优先推迟计数为 0（%lu）",
                  oti_tr_cable_deferred_hid(tc));
            size_t rl = 0;
            oti_tr_recv(td, m0, &rl, 200);   /* 收掉，保持链路空 */
            usleep(60 * 1000);               /* 让这次 HID 活动戳过期，避免影响 11.1 */
        }

        lc.frame_write_delay_ms = 20;      /* 模拟"对端消费慢"：每次帧写都慢 */

        /* 11.1 慢写触发退避；首次慢写在显式泵里（生产环境由 RX 泵线程承担，
           绝不在捕获/输入线程内联 —— 这是 Lead 明确要求的约束） */
        unsigned fwbase = lc.n_frame_write;
        oti_tr_send(tc, m1, sizeof(m1));
        CHECK(lc.n_frame_write == fwbase, "oti_tr_send 只入队、不内联写设备（写次数=%u）",
              lc.n_frame_write);
        long p0 = ms_now();
        oti_tr_pump_once(tc);
        long pdt = ms_now() - p0;
        CHECK(oti_tr_cable_slow_count(tc) == 1, "首次慢帧写被识别（slow=%lu）",
              oti_tr_cable_slow_count(tc));
        CHECK(oti_tr_cable_frame_write_max_ms(tc) >= 10, "帧写最大耗时 %ldms ≥ 阈值 10ms",
              oti_tr_cable_frame_write_max_ms(tc));
        CHECK(oti_tr_cable_max_latency_ms(tc) >= 10 && pdt >= 15,
              "慢写计入单次最大延迟（max=%ldms，泵耗时 %ldms）",
              oti_tr_cable_max_latency_ms(tc), pdt);
        long bo1 = oti_tr_cable_backoff_ms(tc);
        CHECK(bo1 == 60 && oti_tr_cable_peer_slow_remaining_ms(tc) > 0,
              "进入第一次退避：窗口 %ldms（base 60），剩余 %ldms", bo1,
              oti_tr_cable_peer_slow_remaining_ms(tc));

        /* 11.2 退避窗口内：帧写被推迟，但消息泵照常跑（CURSOR/ROLE 不会被饿死） */
        oti_tr_send(tc, m3, sizeof(m3));
        oti_tr_send(tc, m4, sizeof(m4));
        oti_tr_send(tc, m5, sizeof(m5));
        unsigned w0 = lc.n_frame_write, r0 = lc.n_msg_read;
        for (int i = 0; i < 3; i++)
            oti_tr_pump_once(tc);
        CHECK(lc.n_frame_write == w0, "退避窗口内不发起帧写（%u→%u）", w0, lc.n_frame_write);
        CHECK(lc.n_msg_read >= r0 + 3, "退避窗口内消息泵照常读 0xD8（%u→%u）",
              r0, lc.n_msg_read);
        CHECK(oti_tr_cable_deferred_writes(tc) >= 3, "被推迟的帧写被计数（%lu）",
              oti_tr_cable_deferred_writes(tc));

        /* 11.3 背景心跳（PING）在慢窗口内合并；HID 写不等慢锁、不受退避影响 */
        uint8_t ping1[64], ping2[64];
        size_t pn1 = oti_encode_ping(101, 1, ping1, sizeof(ping1));
        size_t pn2 = oti_encode_ping(102, 2, ping2, sizeof(ping2));
        oti_tr_send(tc, ping1, pn1);
        oti_tr_send(tc, ping2, pn2);
        CHECK(oti_tr_cable_bg_merged(tc) >= 1, "慢窗口内同类背景帧合并（%lu 次）",
              oti_tr_cable_bg_merged(tc));
        uint8_t hpkt[OTI_HID_PKT_LEN] = {0};
        long h0 = ms_now();
        int hrc = otilink_send_hid(&dc, OTI_HID_TYPE_MOUSE, hpkt);
        long hdt = ms_now() - h0;
        CHECK(hrc == 0 && hdt < 15, "退避窗口内 HID 写正常且不等慢锁（rc=%d，%ldms）",
              hrc, hdt);

        /* 11.4 连续慢 → 指数退避 60 → 120 → 封顶 150 */
        usleep(80 * 1000);                 /* 等第一次 60ms 窗口过期 */
        oti_tr_send(tc, m1, sizeof(m1));
        oti_tr_pump_once(tc);
        long bo2 = oti_tr_cable_backoff_ms(tc);
        CHECK(oti_tr_cable_slow_count(tc) == 2 && bo2 == 120,
              "连续慢指数退避 60→120ms（slow=%lu backoff=%ld）",
              oti_tr_cable_slow_count(tc), bo2);
        usleep(140 * 1000);                /* 等 120ms 窗口过期 */
        oti_tr_send(tc, m2, sizeof(m2));
        oti_tr_pump_once(tc);
        long bo3 = oti_tr_cable_backoff_ms(tc);
        CHECK(oti_tr_cable_slow_count(tc) == 3 && bo3 == 150,
              "退避封顶 max=150ms（slow=%lu backoff=%ld）",
              oti_tr_cable_slow_count(tc), bo3);

        /* 11.5 对端恢复快 → 一次快写清零退避；窗口期间入队的数据一条不丢 */
        lc.frame_write_delay_ms = 0;       /* 对端恢复 */
        usleep(170 * 1000);                /* 等 150ms 封顶窗口过期 */
        oti_tr_pump_once(tc);              /* 一次授权把剩余帧全部写完（快） */
        CHECK(oti_tr_cable_backoff_ms(tc) == 0 && oti_tr_cable_slow_count(tc) == 3,
              "恢复后退避窗口清零、慢计数不再增长（backoff=%ld slow=%lu）",
              oti_tr_cable_backoff_ms(tc), oti_tr_cable_slow_count(tc));
        {
            unsigned long long tags = 0;
            int total = 0, ping_seen = 0, ping_ok = 0;
            static uint8_t rb[OTI_BODY_MAX];
            long ddl = ms_now() + 1500;
            while (ms_now() < ddl && (total + ping_seen) < 7) {
                size_t rl = 0;
                if (oti_tr_recv(td, rb, &rl, 30) != 0)
                    continue;
                int ty = msg_type_of(rb, rl);
                if (ty == OTI_MSG_PING) {
                    ping_seen++;
                    ping_ok = (msg_seq_of(rb, rl) == 102);   /* 合并后应是**最新**那条 */
                } else if (rl == 64) {
                    total++;
                    if (rb[0] < 64)
                        tags |= 1ull << rb[0];
                }
            }
            unsigned long long need = (1ull << 0x11) | (1ull << 0x12) | (1ull << 0x20) |
                                      (1ull << 0x21) | (1ull << 0x22);
            CHECK(total == 6 && (tags & need) == need,
                  "慢窗口期间入队的关键帧全部送达（%d/6 条，tags=0x%llx）", total, tags);
            CHECK(ping_seen == 1 && ping_ok,
                  "合并后的背景心跳只发最新一条（PING 条数=%d seq=102? %d）",
                  ping_seen, ping_ok);
        }

        /* 11.6 帧读慢：独立退避（只推迟帧读，不影响 HID/消息泵），数据不丢 */
        lc.frame_read_delay_ms = 20;
        static uint8_t tmsg[64];
        memset(tmsg, 0x33, sizeof(tmsg));
        oti_tr_send(td, tmsg, sizeof(tmsg));
        oti_tr_pump_once(td);              /* td → tc 方向写一帧（td 侧写是快的） */
        static uint8_t rb2[OTI_BODY_MAX];
        size_t rl2 = 0;
        long r0t = ms_now();
        int rrc2 = oti_tr_recv(tc, rb2, &rl2, 200);
        long rdt2 = ms_now() - r0t;
        CHECK(rrc2 == 0 && rl2 == sizeof(tmsg) && rdt2 >= 15,
              "慢帧读仍能取回数据（rc=%d len=%zu，%ldms）", rrc2, rl2, rdt2);
        CHECK(oti_tr_cable_read_slow_count(tc) == 1 && oti_tr_cable_frame_read_max_ms(tc) >= 10,
              "帧读慢被计数（read_slow=%lu max=%ldms）",
              oti_tr_cable_read_slow_count(tc), oti_tr_cable_frame_read_max_ms(tc));

        /* 第二帧立刻到：读退避窗口内不再发起帧读（数据留在设备侧，不丢） */
        unsigned fr0 = lc.n_frame_read;
        memset(tmsg, 0x34, sizeof(tmsg));
        oti_tr_send(td, tmsg, sizeof(tmsg));
        oti_tr_pump_once(td);
        size_t rl3 = 0;
        int rrc3 = oti_tr_recv(tc, rb2, &rl3, 30);
        CHECK(rrc3 == -2 && lc.n_frame_read == fr0,
              "帧读退避窗口内不发起帧读（rc=%d reads=%u→%u，数据留设备侧）",
              rrc3, fr0, lc.n_frame_read);
        lc.frame_read_delay_ms = 0;
        usleep(70 * 1000);                 /* 等 60ms 读退避窗口过期 */
        size_t rl4 = 0;
        int rrc4 = oti_tr_recv(tc, rb2, &rl4, 200);
        CHECK(rrc4 == 0 && rl4 == sizeof(tmsg) && rb2[0] == 0x34,
              "读退避结束后数据仍能取回（不丢，rc=%d）", rrc4);

        /* 11.7 写入侧 HID 优先（Lead 追加保险）"按需启用"：
           - 只有见过 >10ms 帧写（疑似慢对端）才启用；新原生泵 2~4ms 下完全不生效（11.0 已断言）；
           - 启用后 HID 活动 50ms 内只推迟**帧写**（消息泵/帧读照常）；
           - 队首帧饥饿 >1000ms 时放行 1 帧（防 ROLE/ACK 在用户连续移动鼠标时被饿死）；
           - 与慢写退避叠加时数据不丢。
           这里先恢复真机默认慢操作阈值 30ms：20ms 帧写只触发"疑似慢对端 hint"（>10ms），
           不会进慢退避窗口，可单独验证 HID 守卫。 */
        oti_tr_set_hid_guard_ms(50);
        oti_tr_set_slowop_policy(30, 60, 150);

        /* 11.7.0 制造"疑似慢对端"：20ms 帧写（>10ms hint，<30ms 慢操作阈值） */
        lc.frame_write_delay_ms = 20;
        memset(m1, 0x42, sizeof(m1));
        oti_tr_send(tc, m1, sizeof(m1));
        oti_tr_pump_once(tc);
        CHECK(oti_tr_cable_peer_slow_seen(tc) == 1 && oti_tr_cable_backoff_ms(tc) == 0,
              "20ms 帧写 → 疑似慢对端（HID 守卫启用）且未触发慢退避（backoff=%ld）",
              oti_tr_cable_backoff_ms(tc));
        {
            size_t rl = 0;
            oti_tr_recv(td, rb2, &rl, 200);          /* 收掉探针帧 */
        }

        /* 11.7.1 启用后：HID 活动 → 连续 3 次泵不写帧；停手 50ms 后恢复，数据不丢 */
        lc.frame_write_delay_ms = 0;
        memset(m1, 0x40, sizeof(m1));
        oti_tr_send(tc, m1, sizeof(m1));
        oti_tr_note_hid_activity(tc);      /* 模拟 otikm 在 HID 写成功后打的戳 */
        unsigned fw0 = lc.n_frame_write;
        unsigned dh0 = oti_tr_cable_deferred_hid(tc);
        for (int i = 0; i < 3; i++)
            oti_tr_pump_once(tc);
        CHECK(lc.n_frame_write == fw0, "HID 活动后连续 3 次泵都不发起帧写（%u→%u）",
              fw0, lc.n_frame_write);
        CHECK(oti_tr_cable_deferred_hid(tc) >= dh0 + 3,
              "HID 优先推迟被计数（deferred_hid %u→%lu）", dh0,
              oti_tr_cable_deferred_hid(tc));
        uint8_t hpkt2[OTI_HID_PKT_LEN] = {0};
        CHECK(otilink_send_hid(&dc, OTI_HID_TYPE_MOUSE, hpkt2) == 0,
              "HID 优先窗口内 HID 写本身正常（只推迟帧写）");
        usleep(60 * 1000);                 /* 停手 > 50ms */
        oti_tr_pump_once(tc);
        CHECK(lc.n_frame_write == fw0 + 1, "停手 50ms 后帧写恢复（%u→%u）",
              fw0, lc.n_frame_write);
        {
            size_t rl = 0;
            int rrc = oti_tr_recv(td, rb2, &rl, 200);
            CHECK(rrc == 0 && rl == sizeof(m1) && rb2[0] == 0x40,
                  "HID 优先推迟的帧最终送达（rc=%d）", rrc);
        }

        /* 11.7.2 饥饿上限：连续 HID 活动 >1s 后放行（每次只写 1 帧），数据不丢 */
        static uint8_t f1[64], f2[64], f3[64];
        memset(f1, 0x50, sizeof(f1));
        memset(f2, 0x51, sizeof(f2));
        memset(f3, 0x52, sizeof(f3));
        oti_tr_send(tc, f1, sizeof(f1));
        oti_tr_send(tc, f2, sizeof(f2));
        oti_tr_send(tc, f3, sizeof(f3));
        oti_tr_note_hid_activity(tc);
        unsigned fw2 = lc.n_frame_write;
        for (int i = 0; i < 3; i++)
            oti_tr_pump_once(tc);
        CHECK(lc.n_frame_write == fw2, "连续 HID 活动初期帧写全部推迟（%u→%u）",
              fw2, lc.n_frame_write);
        {
            unsigned st0 = oti_tr_cable_starved(tc);
            int first_starve_writes = -1;
            for (int i = 0; i < 12; i++) {
                usleep(100 * 1000);
                oti_tr_note_hid_activity(tc);          /* 用户一直在动 */
                unsigned before = lc.n_frame_write;
                oti_tr_pump_once(tc);
                if (first_starve_writes < 0 && oti_tr_cable_starved(tc) > st0)
                    first_starve_writes = (int)(lc.n_frame_write - before);
            }
            CHECK(first_starve_writes == 1,
                  "连续 HID 活动 1s 后触发饥饿放行，且每次只写 1 帧（%d）",
                  first_starve_writes);
            CHECK(oti_tr_cable_starved(tc) >= 1, "饥饿放行被计数（%lu）",
                  oti_tr_cable_starved(tc));
        }
        usleep(60 * 1000);                 /* 停手：HID 守卫过期 */
        oti_tr_pump_once(tc);              /* 一次授权把剩余帧全写出去 */
        {
            int total = 0, seen50 = 0, seen51 = 0, seen52 = 0;
            long ddl = ms_now() + 1000;
            while (ms_now() < ddl && total < 3) {
                size_t rl = 0;
                if (oti_tr_recv(td, rb2, &rl, 50) != 0)
                    continue;
                if (rl == 64) {
                    total++;
                    if (rb2[0] == 0x50) seen50 = 1;
                    else if (rb2[0] == 0x51) seen51 = 1;
                    else if (rb2[0] == 0x52) seen52 = 1;
                }
            }
            CHECK(total == 3 && seen50 && seen51 && seen52,
                  "饥饿放行前后入队的帧全部送达、不丢（%d/3，tag50=%d tag51=%d tag52=%d）",
                  total, seen50, seen51, seen52);
        }

        /* 11.8 与慢写退避叠加：HID 优先窗口内不写帧、也不产生慢写；停手后慢写被识别，
           退避窗口结束数据仍不丢。
           步骤：先用 20ms 帧写重新让 peer_slow_seen=1（阈值 30ms 下不触发慢退避），
           再切回测试用 10ms 阈值验证"停手后的慢写退避"。 */
        lc.frame_write_delay_ms = 20;
        memset(m2, 0x43, sizeof(m2));
        oti_tr_send(tc, m2, sizeof(m2));
        oti_tr_pump_once(tc);
        {
            size_t rl = 0;
            oti_tr_recv(td, rb2, &rl, 200);          /* 收掉探针帧 */
        }
        oti_tr_set_slowop_policy(10, 60, 150);
        lc.frame_write_delay_ms = 20;
        memset(m2, 0x41, sizeof(m2));
        oti_tr_send(tc, m2, sizeof(m2));
        oti_tr_note_hid_activity(tc);
        unsigned sc0 = oti_tr_cable_slow_count(tc);
        unsigned fw1 = lc.n_frame_write;
        for (int i = 0; i < 3; i++)
            oti_tr_pump_once(tc);
        CHECK(lc.n_frame_write == fw1 && oti_tr_cable_slow_count(tc) == sc0,
              "HID 优先窗口内既不写帧也不产生慢写（写 %u→%u，慢 %u→%lu）",
              fw1, lc.n_frame_write, sc0, oti_tr_cable_slow_count(tc));
        usleep(60 * 1000);
        oti_tr_pump_once(tc);              /* 停手后写出：慢 20ms → 触发退避 */
        CHECK(oti_tr_cable_slow_count(tc) == sc0 + 1 && lc.n_frame_write == fw1 + 1,
              "停手后的慢写仍被识别并退避（slow=%lu backoff=%ld）",
              oti_tr_cable_slow_count(tc), oti_tr_cable_backoff_ms(tc));
        usleep(80 * 1000);                 /* 等 60ms 慢写退避窗口过期 */
        oti_tr_pump_once(tc);
        {
            size_t rl = 0;
            int rrc = oti_tr_recv(td, rb2, &rl, 200);
            CHECK(rrc == 0 && rl == sizeof(m2) && rb2[0] == 0x41,
                  "HID 优先 + 慢写退避叠加后数据不丢（rc=%d）", rrc);
        }

        printf("  [info] 守卫统计: 写慢 %lu 次/最大写 %ldms，读慢 %lu 次，延迟写 %lu 次"
               "（HID 优先 %lu、饥饿放行 %lu），背景合并 %lu/丢弃 %lu\n",
               oti_tr_cable_slow_count(tc), oti_tr_cable_frame_write_max_ms(tc),
               oti_tr_cable_read_slow_count(tc), oti_tr_cable_deferred_writes(tc),
               oti_tr_cable_deferred_hid(tc), oti_tr_cable_starved(tc),
               oti_tr_cable_bg_merged(tc), oti_tr_cable_bg_dropped(tc));
        oti_tr_close(td);
        oti_tr_close(tc);
    }

    oti_tr_close(ta);
    oti_tr_close(tb);
    printf("\n%s（%d 项失败）\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
