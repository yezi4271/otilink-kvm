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
    uint8_t body[OTI_BODY_MAX];
};
struct oti_queue {
    struct oti_qslot s[OTI_QSLOTS];
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
    /* fd */
    int fd;
    int owns;
};

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
    if (len)
        memcpy(s->body, body, len);
    q->tail = (q->tail + 1) % OTI_QSLOTS;
    q->count++;
    return 0;
}

static uint8_t q_scratch[OTI_BODY_MAX];   /* 仅用于"队列满丢最旧"时接住被丢的条目 */

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

/* ---------------- 线缆：一次"读消息 → 分派"的设备交互 ----------------
 * 严格按厂商 ProcessIdleState 的语义（NOTES §34）：
 *   1) 读 16 字节消息，同时用 CDB[3..4] 上报"本机待发送帧数"
 *   2) 0x05 → 对端有数据：be16(m[1..2]) 帧数 → 0xD9/0x28 读帧进 rxq
 *   3) 0x06/0x07 → **发送授权** → 立刻把 txq 的帧写出去（中间不能插别的设备操作）
 *   4) 0x01/0x00/0x08/0x10 → 复位（链路未就绪/对端未消费），计数后返回
 * 调用者必须已持有 t->lock。 */
static int cable_pump_once_locked(oti_transport *t);

/* 整段 pump（读授权 → 立刻写帧）必须与 HID 包写互斥：HID 写插进授权窗口会让授权作废，
   真机表现就是帧写 rc=524546、帧大量丢失。包一层而不是在函数里加锁，避免 5 个 return
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

    unsigned pending = (unsigned)t->txq.count;
    if (pending > 100)
        pending = 100;
    int rc = otilink_msg_read(&t->dev, m, (uint16_t)pending);
    if (rc)
        return rc;

    if (m[0] == 0x05) {
        unsigned n = ((unsigned)m[1] << 8) | (unsigned)m[2];
        if (n > 4)
            n = 4;                     /* 一次最多收 4 帧：缩短持锁时间，输入线程不被长时间挡住 */
        for (unsigned i = 0; i < n; i++) {
            if (t->rxq.count >= OTI_QSLOTS) {
                t->rxq.dropped++;      /* 队列满：不读，留在设备侧下次再取 */
                break;
            }
            rc = otilink_recv_frame(&t->dev, frame);
            if (rc)
                return rc;
            if (otilink_frame_check(frame) != 1)
                continue;              /* 空闲/非法帧：丢弃 */
            uint32_t l = 0;
            memcpy(&l, frame + 4, 4);  /* 本库头的长度字段 */
            if (l > OTI_BODY_MAX)
                l = OTI_BODY_MAX;
            q_push(&t->rxq, frame + OTI_FRAME_HEAD, l);
            t->rx_done++;
        }
        return 0;
    }

    if (m[0] == 0x06 || m[0] == 0x07) {
        while (t->txq.count > 0) {
            size_t l = 0;
            if (q_pop(&t->txq, body, &l))
                break;
            rc = otilink_send_frame(&t->dev, body, l);
            if (rc)
                return rc;             /* 失败：该帧已丢，计数在上层 */
            t->tx_done++;
            t->last_tx_ms = now_ms();
        }
        return 0;
    }

    t->resets++;
    return 0;
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
    /* 线缆：**绝不阻塞输入路径**。
       只入队 + 顺手推一次；真正的发送由接收线程的泵负责。
       （教训：之前在这里等最多 5 秒且持有互斥锁，对端一不消费就把输入线程和
         接收线程一起卡死 —— 用户表现为"键鼠完全失控、热键也失灵"。） */
    pthread_mutex_lock(&t->lock);
    if (q_push(&t->txq, body, len)) {
        size_t dl = 0;
        q_pop(&t->txq, q_scratch, &dl);      /* 丢最旧的一条：鼠标位移宁丢旧不卡新 */
        q_push(&t->txq, body, len);
    }
    pthread_mutex_unlock(&t->lock);

    /* **尝试**推一次；拿不到锁（接收线程正在读设备）就立刻返回 ——
       帧已经在队列里，接收线程下一次泵会把它发出去。
       绝不在输入路径上等设备 I/O：那会造成"鼠标隔一会卡一下"。 */
    if (pthread_mutex_trylock(&t->lock) == 0) {
        int rc = cable_pump_once(t);
        pthread_mutex_unlock(&t->lock);
        return rc;
    }
    return 0;
}

/* 可靠发送：队列满时**等**（而不是丢最旧的）。给大文件分片用 ——
 * 文件片丢一片虽然能被收端的位图发现并重传，但频繁丢会让 500MB 传得很惨。
 * 返回 0 成功；<0 = 超时/设备错（上层按"收端位图裁决"重传）。 */
int oti_tr_send_wait(oti_transport *t, const void *body, size_t len, int timeout_ms)
{
    if (!t || len > OTI_BODY_MAX)
        return -EINVAL;
    if (t->kind != TR_CABLE)
        return oti_tr_send(t, body, len);
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&t->lock);
        if (q_push(&t->txq, body, len) == 0) {
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
        /* 设备侧暂时没有数据：小睡一下，别把 CPU 空转打满 */
        usleep(300);
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
