/*
 * otixfer.c —— 见 otixfer.h
 */
#include "otixfer.h"

#include "otilink.h"           /* oti_adopt_path()：root 运行时把收到的文件交还桌面用户 */

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---------- 小端读写 ---------- */
static void w16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xff; }
static void w64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xff; }
static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void xlog(struct oti_xfer_tx *t, const char *fmt, ...)
{
    if (!t->ops || !t->ops->log)
        return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    t->ops->log(t->ops->user, "%s", buf);
}
/* rx 用同一个日志回调 */
static void rlog(struct oti_xfer_rx *r, const char *fmt, ...)
{
    if (!r->ops || !r->ops->log)
        return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    r->ops->log(r->ops->user, "%s", buf);
}

static const char *base_name(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* 把文件名洗成安全的 basename（防 ".."/绝对路径/反斜杠） */
static void sanitize_name(char *dst, size_t cap, const char *src, size_t slen)
{
    size_t k = 0;
    for (size_t i = 0; i < slen && k + 1 < cap; i++) {
        char c = src[i];
        if (c == '/' || c == '\\' || c == '\0')
            c = '_';
        dst[k++] = c;
    }
    dst[k] = 0;
    if (k == 0 || strcmp(dst, ".") == 0 || strcmp(dst, "..") == 0)
        snprintf(dst, cap, "unnamed");
}

/* ============================ 发送端 ============================ */

static pthread_mutex_t tx_mailbox_init_lock = PTHREAD_MUTEX_INITIALIZER;

void oti_xfer_tx_init(struct oti_xfer_tx *t, const struct oti_xfer_ops *ops)
{
    memset(t, 0, sizeof(*t));
    t->ops = ops;
    t->fd = -1;
    pthread_mutex_init(&t->lk, NULL);
    (void)tx_mailbox_init_lock;
}

int oti_xfer_tx_active(const struct oti_xfer_tx *t) { return t->active; }

static void tx_close_current(struct oti_xfer_tx *t)
{
    if (t->fd >= 0) {
        close(t->fd);
        t->fd = -1;
    }
}

void oti_xfer_tx_abort(struct oti_xfer_tx *t, const char *why)
{
    if (!t->active)
        return;
    tx_close_current(t);
    xlog(t, "[warn] 大文件传输中止：%s（已发 %u/%u 片，%s）", why, t->sent_parts, t->nparts,
         t->paths[t->cur]);
    t->active = 0;
    t->npaths = 0;
    t->cur = 0;
}

int oti_xfer_tx_begin(struct oti_xfer_tx *t, char paths[][OTI_XFER_PATH_MAX], int n,
                      uint64_t threshold)
{
    if (t->active || n <= 0)
        return 0;
    if (n > OTI_XFER_MAX_FILES)
        n = OTI_XFER_MAX_FILES;
    uint64_t total = 0;
    int ok = 0;
    for (int i = 0; i < n; i++) {
        struct stat st;
        if (stat(paths[i], &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        snprintf(t->paths[ok], OTI_XFER_PATH_MAX, "%s", paths[i]);
        total += (uint64_t)st.st_size;
        ok++;
    }
    if (ok == 0)
        return 0;
    if (total <= threshold)
        return 0;                     /* 小文件走原来的"文件包"路径 */
    t->npaths = ok;
    t->cur = 0;
    t->active = 1;
    t->done_files = 0;
    t->done_bytes = 0;
    t->rounds = 0;
    t->phase = 0;
    t->fid = (uint32_t)(now_ms() & 0x7fffffff) | 1u;
    t->t_start_ms = now_ms();
    t->t_log_ms = t->t_start_ms;
    return 1;
}

static int tx_open_current(struct oti_xfer_tx *t)
{
    if (t->fd >= 0)
        return 0;
    const char *p = t->paths[t->cur];
    t->fd = open(p, O_RDONLY);
    if (t->fd < 0) {
        xlog(t, "[warn] 打不开 %s（errno=%d）", p, errno);
        return -1;
    }
    struct stat st;
    if (fstat(t->fd, &st) != 0) {
        xlog(t, "[warn] fstat %s 失败（errno=%d）", p, errno);
        return -1;
    }
    t->total = (uint64_t)st.st_size;
#if defined(POSIX_FADV_SEQUENTIAL)
    (void)posix_fadvise(t->fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    snprintf(t->name, sizeof(t->name), "%s", base_name(p));
    t->nparts = (uint32_t)((t->total + OTI_XFER_PART_MAX - 1) / OTI_XFER_PART_MAX);
    if (t->nparts == 0)
        t->nparts = 1;                /* 空文件也发一片（dlen=0），收端据此建文件 */
    t->sent_parts = 0;
    t->done_parts = 0;
    t->fail_streak = 0;
    /* 整文件 CRC **一次算好**（顺序读一遍）。
       不要在发片时增量累加：发失败重传会把同一片重复计入，对端校验必然失败
       （实测：设备打嗝一次 → CRC 全错 → 对端要求整份重发）。 */
    {
        uint32_t crc = 0;
        ssize_t r;
        lseek(t->fd, 0, SEEK_SET);
        while ((r = read(t->fd, t->buf, sizeof(t->buf))) > 0)
            crc = oti_crc32_append(crc, t->buf, (size_t)r);
        t->crc = crc;
        t->crc_done = 1;
    }
    xlog(t, "开始发送大文件 %s（%llu 字节 / %u 片，crc=%08x，fid=%u）", t->name,
         (unsigned long long)t->total, t->nparts, t->crc, t->fid);
    return 0;
}

/* 发第 idx 片；返回 0 成功 */
static int tx_send_part(struct oti_xfer_tx *t, uint32_t idx)
{
    uint64_t off = (uint64_t)idx * OTI_XFER_PART_MAX;
    size_t n = (size_t)(t->total - off);
    if (n > OTI_XFER_PART_MAX)
        n = OTI_XFER_PART_MAX;
    if (n > 0) {
        ssize_t r = pread(t->fd, t->buf, n, (off_t)off);
        if (r != (ssize_t)n) {
            xlog(t, "[warn] 读 %s 偏移 %llu 失败（%zd/%zu，errno=%d）", t->name,
                 (unsigned long long)off, r, n, errno);
            return -1;
        }
    }
    size_t nl = strlen(t->name);
    uint8_t *b = t->body;
    w32(b + 0, idx);
    w32(b + 4, t->nparts);
    w64(b + 8, t->total);
    w32(b + 16, (uint32_t)nl);
    memcpy(b + 20, t->name, nl);
    uint8_t *q = b + 20 + nl;
    w32(q, (uint32_t)n);
    if (n)
        memcpy(q + 4, t->buf, n);
    size_t blen = 20 + nl + 4 + n;

    return t->ops->send_clip(t->ops->user, OTI_CLIP_FMT_PART, t->fid, b, blen);
}

static void tx_send_done(struct oti_xfer_tx *t)
{
    uint8_t b[64];
    size_t nl = strlen(t->name);
    w16(b + 0, OTI_XFER_DONE);
    w16(b + 2, 0);
    w64(b + 4, t->total);
    w32(b + 12, t->crc);
    w32(b + 16, t->nparts);
    w32(b + 20, 0);
    w32(b + 24, (uint32_t)nl);
    if (nl)
        memcpy(b + 28, t->name, nl);
    t->ops->send_clip(t->ops->user, OTI_CLIP_FMT_CTRL, t->fid, b, 28 + nl);
    t->phase = 1;
    t->wait_deadline_ms = now_ms() + 30000;
    xlog(t, "  %s 全部 %u 片已发出，等待对端校验（crc=%08x）…", t->name, t->nparts, t->crc);
}

void oti_xfer_tx_verdict(struct oti_xfer_tx *t, uint16_t flags, uint32_t resume_from,
                         const uint8_t *map, uint32_t map_len)
{
    pthread_mutex_lock(&t->lk);
    t->have_verdict = 1;
    t->v_flags = flags;
    t->v_resume = resume_from;
    free(t->v_map);
    t->v_map = NULL;
    t->v_map_len = 0;
    if (map && map_len) {
        t->v_map = malloc(map_len);
        if (t->v_map) {
            memcpy(t->v_map, map, map_len);
            t->v_map_len = map_len;
        }
    }
    pthread_mutex_unlock(&t->lk);
}

int oti_xfer_tx_poll(struct oti_xfer_tx *t)
{
    if (!t->active)
        return 0;

    /* 裁决到达？ */
    int have = 0;
    uint16_t flags = 0;
    uint32_t resume = 0;
    uint8_t *map = NULL;
    uint32_t map_len = 0;
    pthread_mutex_lock(&t->lk);
    if (t->have_verdict) {
        have = 1;
        flags = t->v_flags;
        resume = t->v_resume;
        map = t->v_map;                 /* 所有权移交本线程 */
        map_len = t->v_map_len;
        t->v_map = NULL;
        t->v_map_len = 0;
        t->have_verdict = 0;
    }
    pthread_mutex_unlock(&t->lk);

    if (have) {
        if (flags & OTI_XFER_OK) {
            double sec = (now_ms() - t->t_start_ms) / 1000.0;
            double mb = (t->total / 1048576.0);
            xlog(t, "✔ %s 传送完成（%llu 字节，%.1f 秒，%.1f MB/s）", t->name,
                 (unsigned long long)t->total, sec, sec > 0 ? mb / sec : 0);
            tx_close_current(t);
            free(t->todo);
            t->todo = NULL;
            t->todo_n = t->todo_i = 0;
            t->done_files++;
            t->done_bytes += t->total;
            t->cur++;
            t->rounds = 0;
            if (t->cur >= t->npaths) {
                xlog(t, "✔ 本批大文件全部完成：%d 个文件 / %llu 字节", t->done_files,
                     (unsigned long long)t->done_bytes);
                t->active = 0;
                t->npaths = 0;
                t->cur = 0;
                return 0;
            }
            t->t_start_ms = now_ms();
            return 1;
        }
        if (flags & OTI_XFER_ABORT) {
            oti_xfer_tx_abort(t, "对端放弃");
            return 0;
        }
        if (flags & OTI_XFER_RESUME) {
            if (t->rounds++ >= 3) {
                oti_xfer_tx_abort(t, "重传 3 轮仍未通过校验");
                return 0;
            }
            if (t->fd < 0 && tx_open_current(t) != 0) {
                oti_xfer_tx_abort(t, "重传时打不开源文件");
                return 0;
            }
            free(t->todo);
            t->todo = NULL;
            t->todo_n = t->todo_i = 0;
            if ((flags & OTI_XFER_HAS_MAP) && map && map_len >= (t->nparts + 7) / 8) {
                /* 按位图只补缺片：500MB 时这一条能把"缺口靠前 → 重发几百 MB"变成"只补几片" */
                uint32_t cnt = 0;
                for (uint32_t i = 0; i < t->nparts; i++)
                    if (map[i >> 3] & (1u << (i & 7)))
                        cnt++;
                if (cnt == 0) {
                    xlog(t, "  对端说没有缺片（可能校验失败要求整份重发）");
                    t->todo = NULL;
                    t->sent_parts = 0;
                    t->rounds--;
                } else {
                    t->todo = malloc(sizeof(uint32_t) * cnt);
                    if (t->todo) {
                        uint32_t k = 0;
                        for (uint32_t i = 0; i < t->nparts; i++)
                            if (map[i >> 3] & (1u << (i & 7)))
                                t->todo[k++] = i;
                        t->todo_n = k;
                        xlog(t, "  对端缺 %u 片，按位图补发（第 %d 轮）", k, t->rounds);
                    } else {
                        t->sent_parts = (resume < t->nparts) ? resume : 0;
                        xlog(t, "  内存不足，退化为从第 %u 片顺序重发（第 %d 轮）", t->sent_parts,
                             t->rounds);
                    }
                }
            } else {
                if (resume >= t->nparts)
                    resume = (t->nparts > 0) ? t->nparts - 1 : 0;
                t->sent_parts = resume;
                xlog(t, "  对端要求从第 %u 片重发（第 %d 轮）", resume, t->rounds);
            }
            free(map);
            map = NULL;
            t->phase = 0;
        }
    }

    free(map);
    map = NULL;

    if (t->phase == 1) {                     /* 等裁决 */
        if (now_ms() > t->wait_deadline_ms) {
            oti_xfer_tx_abort(t, "等待对端校验超时（30 秒）");
            return 0;
        }
        return 1;
    }

    if (t->fd < 0 && tx_open_current(t) != 0) {
        oti_xfer_tx_abort(t, "打开源文件失败");
        return 0;
    }

    /* 一批片（约 1MB）后再回来，便于上层检查 running 标志 */
    for (int i = 0; i < 16; i++) {
        uint32_t idx;
        if (t->todo) {                       /* 按位图只补缺片 */
            if (t->todo_i >= t->todo_n)
                break;
            idx = t->todo[t->todo_i];
        } else {                             /* 顺序全发 */
            if (t->sent_parts >= t->nparts)
                break;
            idx = t->sent_parts;
        }
        int rc = tx_send_part(t, idx);
        if (rc != 0) {
            /* 发失败：不推进游标（下一轮重试同一片）。但必须**退避**：
               立刻重试会把这个线程打成 100% CPU 空转，反过来把设备读也拖挂
               （实测：设备连续读取失败 → Windows 侧重连设备）。 */
            t->fail_streak++;
            if (t->fail_streak == 1 || (t->fail_streak % 20) == 0)
                xlog(t, "[warn] 第 %u 片发送失败 rc=%d（连续 %d 次），退避重试", idx, rc,
                     t->fail_streak);
            if (t->fail_streak >= 200) {
                oti_xfer_tx_abort(t, "连续 200 次发送失败（设备/链路异常）");
                return 0;
            }
            usleep(20 * 1000);
            break;
        }
        t->fail_streak = 0;
        if (t->todo)
            t->todo_i++;
        else
            t->sent_parts++;
        t->done_parts++;
    }

    long now = now_ms();
    if (now - t->t_log_ms >= 1000) {
        t->t_log_ms = now;
        double pct = t->nparts ? 100.0 * t->sent_parts / t->nparts : 100.0;
        double sec = (now - t->t_start_ms) / 1000.0;
        double done_mb = (double)t->done_parts * OTI_XFER_PART_MAX / 1048576.0;
        xlog(t, "  发送 %s：%u/%u 片（%.1f%%）%.1f MB/s", t->name, t->sent_parts, t->nparts, pct,
             sec > 0 ? done_mb / sec : 0);
    }

    int all_sent = t->todo ? (t->todo_i >= t->todo_n) : (t->sent_parts >= t->nparts);
    if (all_sent && t->phase == 0)
        tx_send_done(t);
    return 1;
}

/* ============================ 接收端 ============================ */

void oti_xfer_rx_init(struct oti_xfer_rx *r, const struct oti_xfer_ops *ops)
{
    memset(r, 0, sizeof(*r));
    r->ops = ops;
    r->fd = -1;
}

int oti_xfer_rx_active(const struct oti_xfer_rx *r) { return r->active; }

static void rx_reset(struct oti_xfer_rx *r)
{
    if (r->fd >= 0) {
        close(r->fd);
        r->fd = -1;
    }
    free(r->bitmap);
    r->bitmap = NULL;
    r->active = 0;
    r->got_parts = 0;
}

/* 起始路径：/tmp/otilink-files-<pid>/<name>（与文件包路径一致，便于统一/清理） */
static void rx_paths(struct oti_xfer_rx *r)
{
    char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/otilink-files-%d", (int)getpid());
    mkdir(dir, 0700);
    oti_adopt_path(dir);          /* 绿色包以 root 跑时：目录交还桌面用户（否则 0700 root 读不了） */
    snprintf(r->path, sizeof(r->path), "%s/%s", dir, r->name);
    snprintf(r->part_path, sizeof(r->part_path), "%s.part", r->path);
}

static void rx_reply(struct oti_xfer_rx *r, uint16_t flags, uint32_t resume_from)
{
    size_t nl = strlen(r->name);
    size_t maplen = ((size_t)r->nparts + 7) / 8;
    int with_map = (flags & OTI_XFER_RESUME) && r->bitmap;
    size_t blen = 28 + nl + (with_map ? maplen : 0);
    uint8_t *b = malloc(blen);
    if (!b)
        return;
    memset(b, 0, blen);
    w16(b + 0, OTI_XFER_VERDICT);
    w16(b + 2, (uint16_t)(flags | (with_map ? OTI_XFER_HAS_MAP : 0)));
    w64(b + 4, r->total);
    w32(b + 12, 0);
    w32(b + 16, r->nparts);
    w32(b + 20, resume_from);
    w32(b + 24, (uint32_t)nl);
    if (nl)
        memcpy(b + 28, r->name, nl);
    if (with_map) {
        /* 送"缺片"位图（收端位图是"已收"，这里取反，语义更直观） */
        uint8_t *m = b + 28 + nl;
        for (uint32_t i = 0; i < r->nparts; i++)
            if (!(r->bitmap[i >> 3] & (1u << (i & 7))))
                m[i >> 3] |= (uint8_t)(1u << (i & 7));
    }
    rlog(r, "  回复裁决：flags=0x%x%s（缺片位图 %zu 字节）", flags | (with_map ? OTI_XFER_HAS_MAP : 0),
         with_map ? " 含位图" : " 无位图", with_map ? maplen : 0);
    r->ops->send_clip(r->ops->user, OTI_CLIP_FMT_CTRL, r->fid, b, blen);
    free(b);
}

int oti_xfer_rx_part(struct oti_xfer_rx *r, uint32_t fid, uint32_t idx, uint32_t nparts,
                     uint64_t total, const char *name, uint32_t name_len,
                     const uint8_t *data, uint32_t dlen)
{
    if (!r->ops || !r->ops->send_clip)
        return -1;

    if (r->active && r->fid != fid) {
        rlog(r, "[warn] 大文件接收被新的一笔打断（旧 fid=%u 收 %u/%u 片，已丢弃）", r->fid,
             r->got_parts, r->nparts);
        if (r->part_path[0])
            unlink(r->part_path);
        rx_reset(r);
    }

    if (!r->active) {
        if (nparts == 0 || nparts > 1000000)
            return -1;
        sanitize_name(r->name, sizeof(r->name), name, name_len);
        r->fid = fid;
        r->nparts = nparts;
        r->total = total;
        r->part_max = OTI_XFER_PART_MAX;
        r->bitmap = calloc((nparts + 7) / 8, 1);
        if (!r->bitmap)
            return -1;
        rx_paths(r);
        r->fd = open(r->part_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (r->fd < 0) {
            rlog(r, "[warn] 建不了接收文件 %s（errno=%d）", r->part_path, errno);
            free(r->bitmap);
            r->bitmap = NULL;
            return -1;
        }
        if (ftruncate(r->fd, (off_t)total) != 0)
            rlog(r, "[warn] 预分配 %llu 字节失败（errno=%d），继续（写到哪算哪）",
                 (unsigned long long)total, errno);
        r->got_parts = 0;
        r->t_start_ms = now_ms();
        r->t_log_ms = r->t_start_ms;
        r->last_part_ms = r->t_start_ms;
        r->active = 1;
        rlog(r, "开始接收大文件 %s（%llu 字节 / %u 片，fid=%u）", r->name,
             (unsigned long long)total, nparts, fid);
    }

    if (idx >= r->nparts)
        return -1;
    /* 测试钩子：OTI_XFER_DROP_PART=<idx> 时故意丢掉这一片，用来验证"位图 + 断点重传"。
       否则这条路径只能靠真机随机丢帧来验，几乎不可复现。 */
    {
        static int drop_idx = -2;
        if (drop_idx == -2) {
            const char *e = getenv("OTI_XFER_DROP_PART");
            drop_idx = (e && *e) ? atoi(e) : -1;   /* 空串=关 */
        }
        if (drop_idx >= 0 && (uint32_t)drop_idx == idx) {
            drop_idx = -1;
            rlog(r, "[test] 故意丢弃第 %u 片（验证收端位图与重传）", idx);
            return 0;
        }
    }
    uint64_t off = (uint64_t)idx * r->part_max;
    if (off + dlen > r->total && r->total != 0)
        return -1;
    if (dlen > 0) {
        ssize_t w = pwrite(r->fd, data, dlen, (off_t)off);
        if (w != (ssize_t)dlen) {
            rlog(r, "[warn] 写 %s 偏移 %llu 失败（%zd/%u，errno=%d）", r->part_path,
                 (unsigned long long)off, w, dlen, errno);
            return -1;
        }
    }
    uint8_t *bit = &r->bitmap[idx >> 3];
    uint8_t mask = (uint8_t)(1u << (idx & 7));
    if (!(*bit & mask)) {
        *bit |= mask;
        r->got_parts++;
    }
    r->last_part_ms = now_ms();
    if (r->last_part_ms - r->t_log_ms >= 2000) {
        r->t_log_ms = r->last_part_ms;
        double sec = (r->last_part_ms - r->t_start_ms) / 1000.0;
        double mb = (double)r->got_parts * r->part_max / 1048576.0;
        rlog(r, "  接收 %s：%u/%u 片（%.1f%%）%.1f MB/s", r->name, r->got_parts, r->nparts,
             100.0 * r->got_parts / r->nparts, sec > 0 ? mb / sec : 0);
    }
    return 0;
}

/* 整文件校验：把落盘结果读一遍算 CRC（500MB 约 1~2 秒，只在收全后做一次） */
static int rx_verify(struct oti_xfer_rx *r, uint32_t expect_crc)
{
    lseek(r->fd, 0, SEEK_SET);
    uint32_t crc = 0;
    ssize_t n;
    uint64_t read_total = 0;
    while ((n = read(r->fd, r->buf, sizeof(r->buf))) > 0) {
        crc = oti_crc32_append(crc, r->buf, (size_t)n);
        read_total += (uint64_t)n;
    }
    if (read_total != r->total)
        return -1;
    return (crc == expect_crc) ? 0 : -2;
}

static uint32_t rx_first_missing(const struct oti_xfer_rx *r)
{
    for (uint32_t i = 0; i < r->nparts; i++)
        if (!(r->bitmap[i >> 3] & (1u << (i & 7))))
            return i;
    return r->nparts;
}

int oti_xfer_rx_ctrl(struct oti_xfer_rx *r, uint32_t fid, uint16_t kind, uint16_t flags,
                     uint64_t total, uint32_t crc, uint32_t nparts, uint32_t resume_from,
                     const char *name, uint32_t name_len)
{
    (void)flags;
    (void)resume_from;      /* 裁决方向才用它；收端只需 kind/total/crc/nparts */
    (void)name;
    (void)name_len;
    if (!r->ops || !r->ops->send_clip)
        return -1;
    if (kind != OTI_XFER_DONE)
        return -1;
    if (!r->active || r->fid != fid) {
        rlog(r, "[warn] 收到未知传输的 DONE（fid=%u），回 ABORT", fid);
        return -1;
    }
    if (nparts != r->nparts || total != r->total) {
        rlog(r, "[warn] DONE 的元数据不一致（片 %u/%u，长度 %llu/%llu），要求从头发", nparts,
             r->nparts, (unsigned long long)total, (unsigned long long)r->total);
        rx_reply(r, OTI_XFER_RESUME, 0);
        return 0;
    }

    if (r->got_parts < r->nparts) {
        uint32_t miss = rx_first_missing(r);
        rlog(r, "[warn] %s 缺 %u 片，要求从第 %u 片重发", r->name, r->nparts - r->got_parts, miss);
        rx_reply(r, OTI_XFER_RESUME, miss);
        return 0;
    }

    int v = rx_verify(r, crc);
    if (v != 0) {
        rlog(r, "[warn] %s 校验失败（%s），要求整份重发", r->name,
             v == -1 ? "长度对不上" : "CRC 不一致");
        rx_reply(r, OTI_XFER_RESUME, 0);
        return 0;
    }

    fsync(r->fd);
    close(r->fd);
    r->fd = -1;
    unlink(r->path);
    if (rename(r->part_path, r->path) != 0) {
        rlog(r, "[warn] 重命名 %s → %s 失败（errno=%d）", r->part_path, r->path, errno);
        rx_reply(r, OTI_XFER_ABORT, 0);
        rx_reset(r);
        return -1;
    }
    oti_adopt_path(r->path);
    double sec = (now_ms() - r->t_start_ms) / 1000.0;
    double mb = r->total / 1048576.0;
    rlog(r, "✔ %s 接收完成（%llu 字节，%.1f 秒，%.1f MB/s）→ %s", r->name,
         (unsigned long long)r->total, sec, sec > 0 ? mb / sec : 0, r->path);
    if (r->ops->apply_file)
        r->ops->apply_file(r->ops->user, r->path);
    rx_reply(r, OTI_XFER_OK, r->nparts);
    rx_reset(r);
    return 1;
}

void oti_xfer_rx_housekeep(struct oti_xfer_rx *r)
{
    if (!r->active)
        return;
    if (now_ms() - r->last_part_ms > 60000) {
        rlog(r, "[warn] 大文件接收超时（60 秒无新片），丢弃 %s（%u/%u 片）", r->name, r->got_parts,
             r->nparts);
        if (r->part_path[0])
            unlink(r->part_path);
        rx_reset(r);
    }
}
