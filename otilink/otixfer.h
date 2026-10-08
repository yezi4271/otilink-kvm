/*
 * otixfer.h —— 大文件流式传输（对拷线协议管道之上的分片/确认/重传）
 *
 * 为什么需要它：剪贴板的"文件包"（format 3）是**整包驻留内存**的 ——
 * 发端要把文件全读进内存再分块，收端要把 total_len 一次性 malloc 出来。
 * 8MB 没问题，500MB 会让两侧各吃 500MB（Windows 侧实测只剩 ~1GB 可用内存），
 * 而且一旦中间丢一块，整包就得重来。
 *
 * 本模块把大文件切成 **64000 字节的片**，收端**直接落盘**（pread/pwrite，
 * 内存开销恒定 64KB），并带：
 *   · 分片位图 → 收端知道缺哪几片；
 *   · 完成校验（整文件 CRC32）→ 不一致就要求重传；
 *   · 断点续传 → 收端回 "从第 K 片重发"，发端 seek 过去接着发（不用整包重来）。
 *
 * 线路格式（复用 OTI_MSG_CLIP，只新增两个 format，两侧必须一致）：
 *   format 4 = PART：clip 头之后是
 *       u32 part_index, u32 nparts, u64 file_total, u32 name_len, name[], u32 dlen, data[]
 *   format 5 = CTRL：
 *       u16 kind, u16 flags, u64 file_total, u32 crc, u32 nparts,
 *       u32 resume_from, u32 name_len, name[]
 *   kind: 1 = DONE（发→收，带整文件 CRC）, 2 = VERDICT（收→发）
 *   flags: bit0 = OK, bit1 = RESUME（从 resume_from 片重发）, bit2 = ABORT
 *
 * 线程模型：tx 只被剪贴板线程用，rx 只被收包线程用，互不共享状态（各自的 xfer 实例）。
 */
#ifndef OTIXFER_H
#define OTIXFER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "otiproto.h"

/* 每片数据字节数（两侧必须一致；留足 clip 头 + 分片头 + 文件名的余量） */
#define OTI_XFER_PART_MAX 64000
/* 一次最多排队的文件数（多选拖拽） */
#define OTI_XFER_MAX_FILES 16
#define OTI_XFER_PATH_MAX  512

enum oti_xfer_kind { OTI_XFER_DONE = 1, OTI_XFER_VERDICT = 2 };
#define OTI_XFER_OK      0x1
#define OTI_XFER_RESUME  0x2
#define OTI_XFER_ABORT   0x4
#define OTI_XFER_HAS_MAP 0x8    /* VERDICT 里带了"缺片位图"（按位只补缺片，不用从缺口发到结尾） */

struct oti_xfer_ops {
    void *user;
    /* 发一条 clip 消息（body 是 clip 头之后的内容）。返回 0 成功、<0 失败。 */
    int  (*send_clip)(void *user, uint16_t format, uint32_t fid, const void *body, size_t len);
    /* 收完一个文件 → 挂到剪贴板（uri-list / CF_HDROP）。 */
    void (*apply_file)(void *user, const char *path);
    /* 日志（实现里带换行由调用方负责） */
    void (*log)(void *user, const char *fmt, ...);
};

/* ---------------- 发送端 ---------------- */
struct oti_xfer_tx {
    const struct oti_xfer_ops *ops;
    int      active;
    uint32_t fid;
    char     paths[OTI_XFER_MAX_FILES][OTI_XFER_PATH_MAX];
    int      npaths, cur;

    int      fd;
    char     name[256];
    uint64_t total;
    uint32_t nparts, sent_parts;
    uint32_t crc;               /* 整文件 CRC（第一遍顺序读时增量算出） */
    int      crc_done;
    int      phase;             /* 0=发片 1=等裁决 */
    long     wait_deadline_ms;
    long     t_start_ms, t_log_ms;
    int      rounds;            /* 重传轮数（护栏） */
    int      fail_streak;       /* 连续发送失败次数（退避/放弃判据） */
    /* 按位图重传：todo 非空时只发这些片号（否则从头顺序发） */
    uint32_t *todo;
    uint32_t  todo_n, todo_i;
    uint32_t  done_parts;
    int      done_files;
    uint64_t done_bytes;
    /* 裁决信箱：收包线程写、剪贴板线程读（tx 的其余字段只被剪贴板线程碰） */
    pthread_mutex_t lk;
    int      have_verdict;
    uint16_t v_flags;
    uint32_t v_resume;
    uint8_t *v_map;             /* VERDICT 带来的缺片位图（每 bit = 该片缺失） */
    uint32_t v_map_len;
    uint8_t  buf[OTI_XFER_PART_MAX];
    uint8_t  body[OTI_XFER_PART_MAX + 512];   /* 20 + 名字(≤255) + 4 + 数据 */
};

/* ---------------- 接收端 ---------------- */
struct oti_xfer_rx {
    const struct oti_xfer_ops *ops;
    int      active;
    uint32_t fid;
    int      fd;
    char     name[256];
    char     path[OTI_XFER_PATH_MAX];    /* 最终落盘路径 */
    char     part_path[OTI_XFER_PATH_MAX + 8];
    uint64_t total;
    uint32_t nparts, part_max;
    uint8_t *bitmap;                     /* nparts 位 */
    uint32_t got_parts;
    uint8_t  buf[OTI_XFER_PART_MAX];
    long     t_start_ms, t_log_ms, last_part_ms;
    int      rounds;
};

void oti_xfer_tx_init(struct oti_xfer_tx *t, const struct oti_xfer_ops *ops);
void oti_xfer_rx_init(struct oti_xfer_rx *r, const struct oti_xfer_ops *ops);

/* 开始发送一组文件（>threshold 才调用）。1 = 已启动，0 = 不需要/无法启动（已在跑等） */
int  oti_xfer_tx_begin(struct oti_xfer_tx *t, char paths[][OTI_XFER_PATH_MAX], int n,
                       uint64_t threshold);
/* 推进发送：发若干片或处理裁决。返回 1 = 还在跑，0 = 空闲/已完成 */
int  oti_xfer_tx_poll(struct oti_xfer_tx *t);
/* 收包线程收到裁决 → 投进信箱（线程安全） */
void oti_xfer_tx_verdict(struct oti_xfer_tx *t, uint16_t flags, uint32_t resume_from,
                         const uint8_t *map, uint32_t map_len);
int  oti_xfer_tx_active(const struct oti_xfer_tx *t);
void oti_xfer_tx_abort(struct oti_xfer_tx *t, const char *why);

/* 收到一片 / 收到控制消息。返回 0 = 正常，<0 = 丢弃 */
int  oti_xfer_rx_part(struct oti_xfer_rx *r, uint32_t fid, uint32_t idx, uint32_t nparts,
                      uint64_t total, const char *name, uint32_t name_len,
                      const uint8_t *data, uint32_t dlen);
int  oti_xfer_rx_ctrl(struct oti_xfer_rx *r, uint32_t fid, uint16_t kind, uint16_t flags,
                      uint64_t total, uint32_t crc, uint32_t nparts, uint32_t resume_from,
                      const char *name, uint32_t name_len);
/* 超时清理（剪贴板线程每秒调一次） */
void oti_xfer_rx_housekeep(struct oti_xfer_rx *r);
int  oti_xfer_rx_active(const struct oti_xfer_rx *r);

#endif /* OTIXFER_H */
