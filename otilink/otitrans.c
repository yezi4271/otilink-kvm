/*
 * otitrans.c —— 见 otitrans.h
 */
#include "otitrans.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "otilink.h"
#include "otiproto.h"

enum { TR_CABLE, TR_FD };

/* 线缆后端需要两个队列（厂商模型：CFArray 收发各一）：
 *   txq —— 待发送的消息体，等设备发 0x06/0x07 授权后立刻写出；
 *   rxq —— 从设备读回的消息体，等上层 oti_tr_recv 取走。
 * 队列满时**不消费**设备的帧（留在设备侧），避免丢数据。 */
#define OTI_QSLOTS 32
struct oti_qslot {
    uint32_t len;
    uint32_t ts_ms;                  /* 入队时刻（单调时钟低 32 位）：HID 守卫判"饥饿"用 */
    uint8_t body[OTI_BODY_MAX];
};
struct oti_queue {
    struct oti_qslot s[OTI_QSLOTS];
    struct oti_qslot tmp;            /* 满队列踢背景帧时的交换缓冲（避免队内大 memmove） */
    int head, tail, count;
    long dropped;
};

struct oti_transport {
    int kind;
    char name[64];
    /* cable */
    struct otilink_dev dev;
    int have_dev;
    pthread_mutex_t lock;        /* 串行化对设备的交互（收发线程共用一台设备） */
    int lock_inited;
    struct oti_queue txq, rxq;
    long resets;                 /* 收到 0x01/0x00/0x08/0x10 复位的次数 */
    long tx_done, rx_done;       /* 真正写出/读回的条数（看门狗用） */
    long last_tx_ms;             /* 最近一次成功写出的时刻 */
    /* ---- 慢操作守卫（见 otitrans.h；全部在 t->lock 内读写）---- */
    long peer_slow_until_ms;     /* 写侧：在此时刻前不发起帧写（0 = 正常） */
    long peer_last_slow_ms;      /* 上一次慢帧写的时刻（判"连续慢"用） */
    int  peer_slow_streak;       /* 连续慢帧写次数（指数退避指数） */
    long slow_window_ms;         /* 当前/最近一次帧写退避窗口（0 = 已恢复） */
    unsigned long peer_slow_count;   /* 慢帧写累计次数 */
    unsigned long deferred_writes;   /* 窗口内被推迟的帧写次数 */
    long frame_write_max_ms;     /* 单次帧写最大耗时 */
    long read_slow_until_ms;     /* 读侧：在此时刻前不发起帧读 */
    long read_last_slow_ms;
    int  read_slow_streak;
    long read_window_ms;
    unsigned long read_slow_count;
    unsigned long deferred_reads;
    long frame_read_max_ms;
    long op_max_ms;              /* 线缆命令（读消息/读帧/写帧）最大单次耗时 */
    unsigned long bg_merged, bg_dropped;
    /* ---- 写入侧 HID 优先（见 otitrans.h；last_hid_ms 用原子访问，见 note 函数）---- */
    volatile long last_hid_ms;   /* 最近一次成功 HID 写的时刻（0 = 从未） */
    unsigned long deferred_hid;  /* 因 HID 活动被推迟的帧写次数 */
    int  peer_slow_seen;         /* 1 = 疑似慢对端（见过 >10ms 帧写；HID 守卫据此启用） */
    int  frame_fast_streak;      /* 连续快帧写次数（>=5 → 关掉 HID 守卫） */
    unsigned long starved;       /* HID 守卫饥饿放行次数 */
    /* fd */
    int fd;
    int owns;
};

static long now_ms(void);   /* 前向声明：q_push 要写入队时间戳 */

static int q_push(struct oti_queue *q, const void *body, size_t len)
{
    if (len > OTI_BODY_MAX)
        return -1;
    if (q->count >= OTI_QSLOTS) {
        q->dropped++;
        return -1;
    }
    struct oti_qslot *s = &q->s[q->tail];
    s->len = (uint32_t)len;
    s->ts_ms = (uint32_t)now_ms();
    if (len)
        memcpy(s->body, body, len);
    q->tail = (q->tail + 1) % OTI_QSLOTS;
    q->count++;
    return 0;
}

static int q_pop(struct oti_queue *q, void *body, size_t *len)
{
    if (q->count == 0)
        return -1;
    struct oti_qslot *s = &q->s[q->head];
    if (s->len)
        memcpy(body, s->body, s->len);
    *len = s->len;
    q->head = (q->head + 1) % OTI_QSLOTS;
    q->count--;
    return 0;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---------------- 慢操作守卫（2026-11-06；详见 otitrans.h）----------------
 * 实测（Lead 真机）：0xD8 消息读 0~1ms；0xD9/0x28 帧读有数据 ~2ms；0xD9/0x2A 帧写
 * 耗时完全取决于**对端消费速度**（对端快 2~4ms，旧 PowerShell 代理 250~700ms）。线缆是
 * 单队列设备，慢帧写会把 HID 注入写堵在后面 → 指针冻结。因此在传输层给每条命令计时：
 * 单次帧写 > 阈值（默认 30ms）就判定"对端消费慢"并进入退避窗口，窗口内不再发起帧写。
 * 窗口曲线：base 300ms 起，连续慢 600/1200/2000ms 封顶；一次快写恢复即清零。
 * 读侧（帧读 >30ms）单独统计/退避，只推迟帧读，不影响 HID 与消息泵。 */
static int g_slowop_ms = OTI_SLOWOP_MS;
static int g_slow_base_ms = 300;
static int g_slow_max_ms = 2000;

void oti_tr_set_slowop_policy(int threshold_ms, int base_ms, int max_ms)
{
    if (threshold_ms > 0)
        g_slowop_ms = threshold_ms;
    if (base_ms > 0)
        g_slow_base_ms = base_ms;
    if (max_ms > 0)
        g_slow_max_ms = max_ms;
    if (g_slow_max_ms < g_slow_base_ms)
        g_slow_max_ms = g_slow_base_ms;
}

/* 写入侧 HID 优先窗口（见 otitrans.h）：默认 50ms，0 = 关闭。 */
static int g_hid_guard_ms = OTI_HID_GUARD_MS;
void oti_tr_set_hid_guard_ms(int ms) { g_hid_guard_ms = ms > 0 ? ms : 0; }

/* HID 写入成功后由捕获/泵线程调用：只写一个时间戳，**不拿 t->lock**（否则捕获线程会
   被正在做的设备 I/O 挡住——那正是本轮要根除的"鼠标冻结"）。用 relaxed 原子访问，
   泵线程读到的至多差几微秒，对本守卫（50ms 窗口）完全够。 */
void oti_tr_note_hid_activity(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return;
    __atomic_store_n(&t->last_hid_ms, now_ms(), __ATOMIC_RELAXED);
}

/* 连续慢的指数退避窗口：base << (streak)，封顶 max。调用者负责 streak++。 */
static long slow_window_next(int streak)
{
    if (streak > 16)
        streak = 16;                       /* 防溢出；反正早被 max 封顶 */
    long w = (long)g_slow_base_ms << streak;
    if (w <= 0 || w > g_slow_max_ms)
        w = g_slow_max_ms;
    return w;
}

/* 背景心跳帧识别：PING/HELLO/ROLE/CURSOR 这类"只有最新值有意义"的状态帧。
   返回消息类型（>0）或 0（不是背景帧/不是我们的协议帧）。
   慢对端时它们可以被**合并或丢弃**；剪贴板/键鼠数据绝不主动丢。 */
static int bg_type_of(const void *body, size_t len)
{
    if (!body || len < OTI_HDR_SIZE)
        return 0;
    uint32_t magic = 0;
    uint16_t type = 0;
    memcpy(&magic, body, 4);
    if (magic != OTI_MSG_MAGIC)
        return 0;
    memcpy(&type, (const uint8_t *)body + 4, 2);
    switch (type) {
    case OTI_MSG_PING:
    case OTI_MSG_HELLO:
    case OTI_MSG_ROLE:
    case OTI_MSG_CURSOR:
        return (int)type;
    default:
        return 0;
    }
}

/* 队列满时踢掉一条背景帧给关键数据让位：把找到的背景帧与队头交换，再丢队头。
   用队列自带的 tmp 缓冲（65KB）而不是在环里 memmove（32 槽 × 65KB 太大）。
   返回 1 = 踢掉一条；0 = 队里没有背景帧（退回"丢最旧"）。 */
static int txq_evict_bg(struct oti_queue *q)
{
    if (q->count <= 0)
        return 0;
    for (int i = 0, idx = q->head; i < q->count; i++, idx = (idx + 1) % OTI_QSLOTS) {
        if (bg_type_of(q->s[idx].body, q->s[idx].len) <= 0)
            continue;
        struct oti_qslot *h = &q->s[q->head];
        struct oti_qslot *s = &q->s[idx];
        if (s != h) {
            memcpy(&q->tmp, h, sizeof(q->tmp));
            memcpy(h, s, sizeof(q->tmp));
            memcpy(s, &q->tmp, sizeof(q->tmp));
        }
        q->head = (q->head + 1) % OTI_QSLOTS;
        q->count--;
        return 1;
    }
    return 0;
}

/* 入队策略（2026-11-06 重设计；调用者必须持有 t->lock）：
 *   - 背景心跳：队列里已有同类型 → **原地换成最新**（ROLE/HELLO/CURSOR 只有最新值有效，
 *     PING 去重）→ 返回 0；队列满 → 直接丢新（心跳可丢）→ 返回 0；
 *   - 关键数据（KEY/MOUSE/CLIP/ACK/大文件片…）：绝不主动丢；队列满时优先踢一条背景
 *     心跳让位；连背景帧都没有才返回 -1，由调用方按"丢最旧"兜底（鼠标位移宁丢旧不卡新）。
 * 返回 0 = 已接收（入队/合并/可丢的背景帧被丢）；-1 = 队列满且没有背景帧可踢。 */
static int txq_enqueue(oti_transport *t, const void *body, size_t len)
{
    struct oti_queue *q = &t->txq;
    if (len > OTI_BODY_MAX)
        return -1;
    int ty = bg_type_of(body, len);
    if (ty > 0) {
        for (int i = 0, idx = q->head; i < q->count; i++, idx = (idx + 1) % OTI_QSLOTS) {
            struct oti_qslot *s = &q->s[idx];
            if (bg_type_of(s->body, s->len) != ty)
                continue;
            s->len = (uint32_t)len;
            memcpy(s->body, body, len);
            t->bg_merged++;
            return 0;
        }
        if (q->count >= OTI_QSLOTS) {
            q->dropped++;
            t->bg_dropped++;
            return 0;                       /* 心跳：慢/满时允许丢 */
        }
        return q_push(q, body, len);
    }
    if (q->count >= OTI_QSLOTS) {
        if (txq_evict_bg(q)) {
            t->bg_dropped++;
            return q_push(q, body, len);
        }
        return -1;                          /* 没有背景帧可踢：交给"丢最旧"兜底 */
    }
    return q_push(q, body, len);
}

oti_transport *oti_tr_open_cable(const char *sg_path)
{
    char auto_path[64];
    int auto_chosen = 0;
    if (!sg_path) {
        char paths[16][64];
        int n = otilink_find(paths, 16);
        if (n <= 0)
            return NULL;
        /* 复合设备有两个 LUN（CD-ROM + 磁盘），厂商命令该发到哪个并无明文；
           这里逐个探测：谁能应答 0xF0/0x00 设备信息块就用谁，探测不出来的退回第一个。
           偏好：**优先 CD-ROM LUN**（sysfs type=5）—— 安装版 run-kylin.sh 一直用的是
           /dev/sg3（CD LUN），真机回归全在这条路上跑过；自动探测不该悄悄换到另一个 LUN。 */
        int prefer = -1, picked = -1, answered = 0;
        for (int i = 0; i < n; i++) {
            char sys[256], buf[8] = "";
            const char *nm = strrchr(paths[i], '/');
            snprintf(sys, sizeof(sys), "/sys/class/scsi_generic/%.48s/device/type",
                     nm ? nm + 1 : paths[i]);
            FILE *tf = fopen(sys, "r");
            if (tf) {
                if (fgets(buf, sizeof(buf), tf) && atoi(buf) == 5)
                    prefer = i;
                fclose(tf);
            }
        }
        if (prefer >= 0)
            picked = prefer;
        for (int i = 0; i < n; i++) {
            struct otilink_dev probe;
            if (otilink_open(&probe, paths[i]) != 0)
                continue;
            uint8_t info[16];
            int rc = otilink_info_read(&probe, info, sizeof(info));
            otilink_close(&probe);
            if (rc == 0) {
                /* 谁应答就用谁；但若偏好一个 LUN 而它不应答，仍按"能应答"优先 */
                if (picked < 0 || i == prefer || !answered) {
                    picked = i;
                    answered = 1;
                }
                if (i == prefer)
                    break;
            }
        }
        if (picked < 0)
            picked = 0;
        snprintf(auto_path, sizeof(auto_path), "%.48s", paths[picked]);
        sg_path = auto_path;
        auto_chosen = answered;      /* 1 = 该 LUN 能应答厂商命令 */
    }
    oti_transport *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->kind = TR_CABLE;
    t->fd = -1;
    snprintf(t->name, sizeof(t->name), "cable:%.40s%s", sg_path,
             auto_chosen ? "(auto,已应答)" : "");
    if (otilink_open(&t->dev, sg_path) != 0) {
        free(t);
        return NULL;
    }
    t->have_dev = 1;
    pthread_mutex_init(&t->lock, NULL);
    t->lock_inited = 1;
    return t;
}

oti_transport *oti_tr_open_cable_dev(struct otilink_dev *dev)
{
    oti_transport *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->kind = TR_CABLE;
    t->fd = -1;
    t->dev = *dev;                 /* 复制（含 ops 注入点） */
    t->have_dev = 0;               /* 不负责关闭：调用者持有 */
    pthread_mutex_init(&t->lock, NULL);
    t->lock_inited = 1;
    snprintf(t->name, sizeof(t->name), "cable:%.48s", dev->sg_path);
    return t;
}

oti_transport *oti_tr_open_fd(int fd, int owns)
{
    oti_transport *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->kind = TR_FD;
    t->fd = fd;
    t->owns = owns;
    /* 锁必须初始化：fd 后端有多个写者（剪贴板线程、接收线程的 ACK、保活线程），
       要按"整帧"串行化写入，否则帧流会被交错冲烂（表现为对端 decode rc=-1/-2）。
       历史上这里漏了初始化（calloc 恰好全零，glibc 下"碰巧能用"，属于 UB）。 */
    pthread_mutex_init(&t->lock, NULL);
    t->lock_inited = 1;
    snprintf(t->name, sizeof(t->name), "fd:%d", fd);
    return t;
}

struct otilink_dev *oti_tr_cable_dev(oti_transport *t)
{
    return (t && t->kind == TR_CABLE) ? &t->dev : NULL;
}

const char *oti_tr_name(const oti_transport *t)
{
    return t ? t->name : "(null)";
}

/* ---------- fd 后端：u32 长度前缀，兼容 SOCK_STREAM/SEQPACKET ---------- */
static int fd_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static int fd_read_all(int fd, void *buf, size_t len, int timeout_ms)
{
    uint8_t *p = buf;
    long deadline = timeout_ms < 0 ? -1 : now_ms() + timeout_ms;
    while (len) {
        if (deadline >= 0) {
            struct pollfd pf = {.fd = fd, .events = POLLIN};
            long left = deadline - now_ms();
            if (left <= 0)
                return -2;
            int pr = poll(&pf, 1, (int)left);
            if (pr == 0)
                return -2;
            if (pr < 0)
                return (errno == EINTR) ? fd_read_all(fd, p, len, (int)(deadline - now_ms())) : -errno;
        }
        ssize_t r = read(fd, p, len);
        if (r == 0)
            return -1;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN) {
                struct pollfd pf = {.fd = fd, .events = POLLIN};
                poll(&pf, 1, 100);
                continue;
            }
            return -errno;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

/* 消息读的 SG_IO 超时（浓缩自 NOTES §75.5 的历史结论）：
   线缆是单队列设备，一次阻塞的 SCSI 读会把 HID 写堵住。实测 0xD8 消息读**本身 0~1ms**
   就返回（无消息时 m0=0x01），所以超时保持默认 5000ms 即可；把它压到 200~300ms 反而
   会让设备消息读频繁超时、帧通道卡死（真机踩过）。HID 与设备操作的互斥改由
   otilink_cable_io_lock（"读授权 → 立刻写帧"整窗）+ 慢操作守卫保证。 */
static int g_msg_timeout_ms = 5000;
void oti_tr_set_msg_timeout(int ms) { g_msg_timeout_ms = ms > 0 ? ms : 5000; }

/* 【已废弃】原"HID 优先窗口"：HID 包正在写时不开始消息读。后来真机证明它害处更大：
   泵一停 CURSOR/ROLE 遥测就饿死 → 交接/回程判据失灵（2026-11-06 现场"鼠标回不来"）。
   保留空函数只为不破坏旧调用点的符号；消息泵现在**永远** 5~10ms 级轮询。 */
void oti_tr_set_hid_busy(int busy) { (void)busy; }

/* ---------------- 线缆：一次"读消息 → 分派"的设备交互 ----------------
 * 严格按厂商 ProcessIdleState 的语义（NOTES §34）：
 *   1) 读 16 字节消息，同时用 CDB[3..4] 上报"本机待发送帧数"
 *   2) 0x05 → 对端有数据：be16(m[1..2]) 帧数 → 0xD9/0x28 读帧进 rxq
 *   3) 0x06/0x07 → **发送授权** → 立刻把 txq 的帧写出去（中间不能插别的设备操作）
 *   4) 0x01/0x00/0x08/0x10 → 复位（链路未就绪/对端未消费），计数后返回
 * 2026-11-06 起每一次命令都计时：帧写 >阈值 → 慢对端 → 进退避窗口（窗口内帧写推迟），
 * 帧读 >阈值 → 退避帧读；消息泵与 HID 写永不被这两条守卫挡住。
 * 调用者必须已持有 t->lock。 */
static int cable_pump_once_locked(oti_transport *t);

/* 整段 pump（读授权 → 立刻写帧）必须与 HID 包写互斥：HID 写插进授权窗口会让授权作废，
   真机表现就是帧写 rc=524546、帧大量丢失。包一层而不是在函数里加锁，避免多个 return
   漏解锁。 */
static int cable_pump_once(oti_transport *t)
{
    otilink_cable_io_lock();
    int rc = cable_pump_once_locked(t);
    otilink_cable_io_unlock();
    return rc;
}

static int cable_pump_once_locked(oti_transport *t)
{
    static uint8_t frame[OTI_FRAME_SIZE];
    static uint8_t body[OTI_BODY_MAX];
    uint8_t m[16];
    long now = now_ms();
    long hid_last = __atomic_load_n(&t->last_hid_ms, __ATOMIC_RELAXED);
    /* HID 守卫**按需启用**：只有见过 >10ms 帧写（peer_slow_seen）才生效。
       新原生泵（帧写 2~4ms）下 peer_slow_seen 永远为 0 → 帧写不会被 HID 活动饿死。 */
    int hid_blocked = (g_hid_guard_ms > 0 && t->peer_slow_seen && hid_last > 0 &&
                       now - hid_last < g_hid_guard_ms);
    int slow_blocked = (t->peer_slow_until_ms > now);
    /* 饥饿上限：队首帧等太久（>1000ms）且仅被 HID 守卫挡住 → 放行写 1 帧，防止
       ROLE 心跳/剪贴板 ACK 在用户"连续移动鼠标 >5s"时被无限饿死（Windows 判对端不在）。 */
    int starve_allow = 0;
    if (hid_blocked && !slow_blocked && t->txq.count > 0) {
        uint32_t waited = (uint32_t)now - t->txq.s[t->txq.head].ts_ms;
        if (waited >= (uint32_t)OTI_HID_STARVE_MS) {
            starve_allow = 1;
            t->starved++;
            fprintf(stderr, "[info] SLOWOP HID 饥饿放行：队首帧等待 %ums，先写 1 帧\n",
                    (unsigned)waited);
        }
    }
    int write_blocked = slow_blocked || (hid_blocked && !starve_allow);

    /* 窗口内上报 pending=0：让设备**不要**发放帧写授权（避免空授权/帧写被拒）。
       注意只影响"写"：消息读（0xD8）与帧读照常，遥测/剪贴板接收不受影响。 */
    unsigned pending = write_blocked ? 0u : (unsigned)t->txq.count;
    if (pending > 100)
        pending = 100;
    long tu0 = now_us();
    int rc = otilink_msg_read_to(&t->dev, m, (uint16_t)pending, g_msg_timeout_ms);
    long dt_us = now_us() - tu0;
    if (dt_us / 1000 > t->op_max_ms)
        t->op_max_ms = dt_us / 1000;
    if (rc)
        return rc;

    if (m[0] == 0x05) {
        unsigned n = ((unsigned)m[1] << 8) | (unsigned)m[2];
        if (n > 4)
            n = 4;                     /* 一次最多收 4 帧：缩短持锁时间，输入线程不被长时间挡住 */
        if (t->read_slow_until_ms > now) {
            t->deferred_reads++;
            return 0;                  /* 帧读退避窗口内：数据留在设备侧，下次再取 */
        }
        for (unsigned i = 0; i < n; i++) {
            if (t->rxq.count >= OTI_QSLOTS) {
                t->rxq.dropped++;      /* 队列满：不读，留在设备侧下次再取 */
                break;
            }
            long tr0 = now_us();
            rc = otilink_recv_frame(&t->dev, frame);
            long rdt = (now_us() - tr0) / 1000;
            if (rc)
                return rc;
            if (rdt > t->frame_read_max_ms)
                t->frame_read_max_ms = rdt;
            if (rdt > t->op_max_ms)
                t->op_max_ms = rdt;
            if (otilink_frame_check(frame) != 1)
                continue;              /* 空闲/非法帧：丢弃 */
            uint32_t l = 0;
            memcpy(&l, frame + 4, 4);  /* 本库头的长度字段 */
            if (l > OTI_BODY_MAX)
                l = OTI_BODY_MAX;
            q_push(&t->rxq, frame + OTI_FRAME_HEAD, l);
            t->rx_done++;
            /* 2026-11-06 现场修正：读侧退避会**自己制造** ACK/CLIP 迟到 ——
               一旦进入读退避窗口，对端写帧被堵（SLOWOP 256ms），ACK 迟到 >1.5s 后发送端
               已放弃重发（日志 `RECV ACK ... active=0`），引发无谓的重发风暴。
               实测帧读正常 2~3ms；只有 >150ms 的极端读才值得退避。 */
            if (rdt > 150) {
                /* 帧读极慢：退避帧读（独立曲线）。只推迟帧读 —— 消息泵与 HID 写不受影响。 */
                long rnow = now_ms();
                if (t->read_last_slow_ms &&
                    rnow - t->read_last_slow_ms > (long)g_slow_base_ms * 8)
                    t->read_slow_streak = 0;      /* 很久没慢：重新从 base 起 */
                long w = slow_window_next(t->read_slow_streak);
                t->read_slow_streak++;
                t->read_window_ms = w;
                t->read_last_slow_ms = rnow;
                t->read_slow_until_ms = rnow + w;
                t->read_slow_count++;
                fprintf(stderr, "[warn] SLOWOP frame read %ldms（对端/设备慢，暂停帧读 %ldms）\n",
                        rdt, w);
                break;                 /* 本次不再读后续帧 */
            }
            if (t->read_slow_streak > 0 && t->read_slow_until_ms <= now) {
                fprintf(stderr, "[info] SLOWOP 恢复：帧读 %ldms（连续慢 %d 次后清零）\n",
                        rdt, t->read_slow_streak);
                t->read_slow_streak = 0;
                t->read_window_ms = 0;
            }
        }
        return 0;
    }

    if (m[0] == 0x06 || m[0] == 0x07) {
        if (t->txq.count == 0)
            return 0;
        if (slow_blocked) {
            /* 慢写退避窗口内：**不发起帧写**（保留 HID 与消息泵）。帧留在队列里，窗口后继续。 */
            t->deferred_writes++;
            return 0;
        }
        if (hid_blocked && !starve_allow) {
            /* 写入侧 HID 优先（仅疑似慢对端生效）：最近 50ms 内有成功的 HID 注入写 → 先不写帧。
               这样即使对端是旧慢代理（帧写 250~700ms），用户动鼠标期间也不会有长帧写
               去堵 HID；停手 50ms 后自动补发。队列保留、消息泵/帧读照常。 */
            t->deferred_writes++;
            t->deferred_hid++;
            return 0;
        }
        int max_frames = starve_allow ? 1 : 0x7fffffff;   /* 饥饿放行只写 1 帧 */
        while (t->txq.count > 0 && max_frames-- > 0) {
            size_t l = 0;
            if (q_pop(&t->txq, body, &l))
                break;
            long tw0 = now_us();
            rc = otilink_send_frame(&t->dev, body, l);
            long wdt = (now_us() - tw0) / 1000;
            if (wdt > t->frame_write_max_ms)
                t->frame_write_max_ms = wdt;
            if (wdt > t->op_max_ms)
                t->op_max_ms = wdt;
            if (rc)
                return rc;             /* 失败：该帧已丢，计数在上层 */
            t->tx_done++;
            t->last_tx_ms = now_ms();
            long wnow = now_ms();
            /* "按需启用 HID 守卫"的疑似慢对端判定：单次 >10ms → 开；连续 5 次 <10ms → 关。 */
            if (wdt > OTI_PEER_SLOW_HINT_MS) {
                t->peer_slow_seen = 1;
                t->frame_fast_streak = 0;
            } else if (t->peer_slow_seen) {
                if (++t->frame_fast_streak >= 5) {
                    t->peer_slow_seen = 0;          /* 对端已恢复快：关掉 HID 守卫 */
                    t->frame_fast_streak = 0;
                }
            }
            if (wdt > g_slowop_ms) {
                /* 对端消费慢：进入退避窗口，窗口内不再发起帧写。
                   连续慢 → 指数退避（300/600/1200/2000ms 封顶）。
                   ⚠️ 本次授权不再写后续帧：它们大概率同样慢，会把 HID 一起堵住。 */
                if (t->peer_last_slow_ms &&
                    wnow - t->peer_last_slow_ms > (long)g_slow_base_ms * 8)
                    t->peer_slow_streak = 0;      /* 很久没慢：重新从 base 起 */
                long w = slow_window_next(t->peer_slow_streak);
                t->peer_slow_streak++;
                t->slow_window_ms = w;
                t->peer_last_slow_ms = wnow;
                t->peer_slow_until_ms = wnow + w;
                t->peer_slow_count++;
                fprintf(stderr, "[warn] SLOWOP frame write %ldms（对端消费慢，暂停帧发送 %ldms）\n",
                        wdt, w);
                break;
            }
            if (t->peer_slow_streak > 0) {
                /* 退避窗口后第一次写恢复快 → 连续慢清零（"恢复后计数清零"）。 */
                fprintf(stderr, "[info] SLOWOP 恢复：帧写 %ldms（连续慢 %d 次后清零）\n",
                        wdt, t->peer_slow_streak);
                t->peer_slow_streak = 0;
                t->slow_window_ms = 0;
                t->peer_slow_until_ms = 0;
            }
        }
        return 0;
    }

    t->resets++;
    return 0;
}

/* 非阻塞推一次设备泵（单测/诊断工具用；fd 后端不需要泵） */
int oti_tr_pump_once(oti_transport *t)
{
    if (!t)
        return -EINVAL;
    if (t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    int rc = cable_pump_once(t);
    pthread_mutex_unlock(&t->lock);
    return rc;
}

int oti_tr_send(oti_transport *t, const void *body, size_t len)
{
    if (!t || len > OTI_BODY_MAX)
        return -EINVAL;
    if (t->kind == TR_FD) {
        uint8_t hdr[4] = {(uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff),
                          (uint8_t)((len >> 16) & 0xff), (uint8_t)((len >> 24) & 0xff)};
        /* 必须整帧（4 字节长度头 + 体）**一把写完**：fd 后端有多个写者
           （剪贴板线程、接收线程发 ACK/保活），不串行化就会把帧流交错冲烂 ——
           症状是对端 `ERR decode rc=-1`，之后整条流彻底错位、内容再也送不到
           （本机 TCP 回归 tcptest.sh 抓到的真 bug；线缆后端有队列+锁所以没这问题）。 */
        pthread_mutex_lock(&t->lock);
        int rc = fd_write_all(t->fd, hdr, 4);
        if (!rc)
            rc = fd_write_all(t->fd, body, len);
        pthread_mutex_unlock(&t->lock);
        return rc;
    }
    /* 线缆：**只入队，绝不在这里做设备 I/O**（2026-11-06 重设计，Lead 明确要求）。
       为什么：一次慢帧写（对端旧/慢代理时 250~700ms）如果发生在捕获/输入线程的
       内联泵里，会把这条线程整个卡住；把它交给 RX 泵线程（5~10ms 级轮询，阻塞无妨）
       才能保证"设备操作永远短、键鼠永不冻结"。历史上这里的"顺手推一次"正是隐患。
       入队策略见 txq_enqueue（背景心跳合并/可丢；关键数据不丢）。 */
    pthread_mutex_lock(&t->lock);
    int rc = txq_enqueue(t, body, len);
    if (rc != 0) {
        /* 队列满且没有背景心跳可踢：沿用历史兜底"丢最旧"（鼠标位移宁丢旧不卡新）。 */
        t->txq.head = (t->txq.head + 1) % OTI_QSLOTS;
        t->txq.count--;
        t->txq.dropped++;
        q_push(&t->txq, body, len);
        rc = 0;
    }
    pthread_mutex_unlock(&t->lock);
    return rc;
}

/* 可靠发送：队列满时**等**（而不是丢最旧的）。给大文件分片用 ——
 * 文件片丢一片虽然能被收端的位图发现并重传，但频繁丢会让 500MB 传得很惨。
 * ⚠️ 会内联推一次设备泵：只允许非输入线程（剪贴板/大文件/ACK）调用 —— 慢帧写
 * 在这里阻塞是可接受的（这些线程不碰键鼠实时性）。返回 0 成功；<0 = 超时/设备错。 */
int oti_tr_send_wait(oti_transport *t, const void *body, size_t len, int timeout_ms)
{
    if (!t || len > OTI_BODY_MAX)
        return -EINVAL;
    if (t->kind != TR_CABLE)
        return oti_tr_send(t, body, len);
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&t->lock);
        if (txq_enqueue(t, body, len) == 0) {
            int rc = cable_pump_once(t);
            pthread_mutex_unlock(&t->lock);
            return rc;
        }
        pthread_mutex_unlock(&t->lock);
        if (now_ms() >= deadline)
            return -2;                 /* 队列一直满：交给上层重传 */
        usleep(1000);
    }
}

int oti_tr_cable_pending(oti_transport *t){
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    int n = t->txq.count;
    pthread_mutex_unlock(&t->lock);
    return n;
}

long oti_tr_cable_last_tx_ms(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    long v = t->last_tx_ms;
    pthread_mutex_unlock(&t->lock);
    return v;
}

/* 清空待发队列（看门狗交还控制权时调用：陈旧位移再发出去只会让指针乱跳） */
void oti_tr_cable_drop_tx(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return;
    pthread_mutex_lock(&t->lock);
    t->txq.head = t->txq.tail = t->txq.count = 0;
    pthread_mutex_unlock(&t->lock);
}

/* ---------------- 慢操作守卫的只读统计 API（验证/单测/日志用）---------------- */
unsigned long oti_tr_cable_slow_count(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    unsigned long v = t->peer_slow_count;
    pthread_mutex_unlock(&t->lock);
    return v;
}

unsigned long oti_tr_cable_read_slow_count(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    unsigned long v = t->read_slow_count;
    pthread_mutex_unlock(&t->lock);
    return v;
}

unsigned long oti_tr_cable_deferred_writes(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    unsigned long v = t->deferred_writes;
    pthread_mutex_unlock(&t->lock);
    return v;
}

unsigned long oti_tr_cable_deferred_hid(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    unsigned long v = t->deferred_hid;
    pthread_mutex_unlock(&t->lock);
    return v;
}

unsigned long oti_tr_cable_starved(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    unsigned long v = t->starved;
    pthread_mutex_unlock(&t->lock);
    return v;
}

int oti_tr_cable_peer_slow_seen(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    int v = t->peer_slow_seen;
    pthread_mutex_unlock(&t->lock);
    return v;
}

unsigned long oti_tr_cable_bg_merged(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    unsigned long v = t->bg_merged;
    pthread_mutex_unlock(&t->lock);
    return v;
}

unsigned long oti_tr_cable_bg_dropped(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    unsigned long v = t->bg_dropped;
    pthread_mutex_unlock(&t->lock);
    return v;
}

long oti_tr_cable_frame_write_max_ms(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    long v = t->frame_write_max_ms;
    pthread_mutex_unlock(&t->lock);
    return v;
}

long oti_tr_cable_frame_read_max_ms(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    long v = t->frame_read_max_ms;
    pthread_mutex_unlock(&t->lock);
    return v;
}

long oti_tr_cable_max_latency_ms(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    long v = t->op_max_ms;
    pthread_mutex_unlock(&t->lock);
    return v;
}

long oti_tr_cable_backoff_ms(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    pthread_mutex_lock(&t->lock);
    long v = t->slow_window_ms;
    pthread_mutex_unlock(&t->lock);
    return v;
}

long oti_tr_cable_peer_slow_remaining_ms(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return 0;
    long now = now_ms();
    pthread_mutex_lock(&t->lock);
    long r = t->peer_slow_until_ms - now;
    pthread_mutex_unlock(&t->lock);
    return r > 0 ? r : 0;
}

/* 清统计（不动队列/窗口/设备）：验证脚本在"传输前"清零，测"本次传输"的慢操作。 */
void oti_tr_cable_reset_stats(oti_transport *t)
{
    if (!t || t->kind != TR_CABLE)
        return;
    pthread_mutex_lock(&t->lock);
    t->peer_slow_count = 0;
    t->deferred_writes = 0;
    t->deferred_hid = 0;
    t->starved = 0;
    t->frame_write_max_ms = 0;
    t->read_slow_count = 0;
    t->deferred_reads = 0;
    t->frame_read_max_ms = 0;
    t->op_max_ms = 0;
    t->bg_merged = t->bg_dropped = 0;
    t->txq.dropped = t->rxq.dropped = 0;
    t->resets = 0;
    pthread_mutex_unlock(&t->lock);
}

int oti_tr_recv(oti_transport *t, void *body, size_t *len, int timeout_ms)
{
    if (!t)
        return -EINVAL;
    if (t->kind == TR_FD) {
        uint8_t hdr[4];
        int rc = fd_read_all(t->fd, hdr, 4, timeout_ms);
        if (rc)
            return rc;
        uint32_t n = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        if (n > OTI_BODY_MAX) {
            fprintf(stderr, "[tr] 非法帧头 n=%u bytes=%02x %02x %02x %02x\n",
                    n, hdr[0], hdr[1], hdr[2], hdr[3]);
            return -3;
        }
        /* 已经读到**合法帧头** = 对端一定会把体发完：这里必须读满，
           绝不能因为"timeout_ms 内没凑齐"就返回超时 —— 半帧留在流里会让之后
           整条流错位（症状：对端 decode rc=-1/-2，内容再也送不到）。
           本机 TCP 回归 tcptest.sh 抓到的第二个真 bug（第一个是 fd 写没串行化）。
           给一个宽松上界：对端真死了也要能返回，让传输看门狗去重开。 */
        rc = fd_read_all(t->fd, body, n, 5000);
        if (rc)
            return rc;
        *len = n;
        return 0;
    }
    /* 线缆：泵设备（授权时发送 / 0x05 时收帧），再从 rxq 取一条消息 */
    long deadline = timeout_ms < 0 ? -1 : now_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&t->lock);
        if (t->rxq.count == 0) {
            int rc = cable_pump_once(t);
            if (rc) {
                pthread_mutex_unlock(&t->lock);
                return rc;
            }
        }
        int got = 0;
        if (t->rxq.count > 0)
            got = (q_pop(&t->rxq, body, len) == 0);
        pthread_mutex_unlock(&t->lock);
        if (got)
            return 0;
        if (deadline >= 0 && now_ms() >= deadline)
            return -2;
        /* 设备侧暂时没有数据：小睡 10ms，别把 CPU 空转打满。
           为什么是 10ms（2026-11-06 重设计保持不变）：0xD8 消息读本身 0~1ms，10ms 级轮询
           让 CURSOR/ROLE/剪贴板延迟上限 +10ms，又不会把单队列设备占满（HID 写每次只需
           等这 1ms 级读窗口）。慢对端的风险已由"帧写/帧读退避窗口"处理，不再靠停泵。 */
        usleep(10 * 1000);
    }
}

void oti_tr_close(oti_transport *t)
{
    if (!t)
        return;
    if (t->lock_inited)
        pthread_mutex_destroy(&t->lock);
    if (t->have_dev)
        otilink_close(&t->dev);
    if (t->kind == TR_FD && t->owns && t->fd >= 0)
        close(t->fd);
    free(t);
}
