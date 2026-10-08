/*
 * oticlip.h —— 剪贴板读写 + 分块重组
 *
 * 后端自动探测（按优先级）：
 *   wayland : wl-paste / wl-copy     需要 WAYLAND_DISPLAY
 *   x11     : xclip -selection clipboard
 *   file    : 用普通文件模拟剪贴板（无图形环境/测试用）
 *
 * 防回环：应用远端内容后把其哈希记为 last_seen_hash，本地轮询到同样内容就不再回发。
 */
#ifndef OTICLIP_H
#define OTICLIP_H

#include <stddef.h>
#include <stdint.h>

#include "otiproto.h"

/* 文件剪贴板：一次最多几个文件 / 单个路径多长 */
#define OTI_CLIP_MAX_FILES 16
#define OTI_CLIP_PATH_MAX  512

typedef enum {
    OTI_CLIP_BE_NONE = 0,
    OTI_CLIP_BE_WAYLAND,
    OTI_CLIP_BE_X11,
    OTI_CLIP_BE_FILE,
} oti_clip_backend;

struct oti_clip {
    oti_clip_backend be;
    char     file[256];     /* file 后端路径 */
    uint32_t last_seen;     /* 最近一次本地已知内容哈希（发送/应用都更新） */
    uint16_t last_format;   /* 最近一次读/写的剪贴板格式：1=UTF-8 文本，2=PNG 图片 */
    int      have_seen;
    size_t   max_bytes;     /* 超过则不发送 */

    /* 重组状态机 */
    uint32_t rx_fid;
    uint32_t rx_total;
    size_t   rx_got;
    uint8_t *rx_buf;
    int      rx_active;

    /* 文件剪贴板的指纹（路径+大小+mtime）：内容没变就不重发。
     * 以前是每次轮询都把文件整个读一遍算 CRC —— 8MB 文件每 300ms 读一次，
     * 大文件时会把磁盘和 CPU 打满，这条是 500MB 传输的前提。 */
    uint32_t last_ident;
    int      have_ident;
    /* 本轮刚收完/刚发出的文件路径（防回环：别把刚收到的文件又发回去） */
    char     self_path[OTI_CLIP_PATH_MAX];
};

/* 只探测"会用哪个后端"（不开选择区、不写剪贴板）—— 给 --version / --doctor 用。
 * 返回值同 oti_clip_backend_name() 的字符串。 */
const char *oti_clip_backend_probe(void);

/* sim_file 非空则强制 file 后端；否则自动探测。返回 0 成功 */
int oti_clip_open(struct oti_clip *c, const char *sim_file, size_t max_bytes);
void oti_clip_close(struct oti_clip *c);
const char *oti_clip_backend_name(const struct oti_clip *c);

/* 读取本地剪贴板文本。0 = 成功（*out 为 malloc 缓冲，调用者 free），
 * 1 = 空/不可用，<0 = 错误 */
int oti_clip_read(struct oti_clip *c, char **out, size_t *len);
/* 写入本地剪贴板 */
int oti_clip_write(struct oti_clip *c, const void *data, size_t len);

/* 把一组本地文件路径挂到剪贴板（X11: text/uri-list）。大文件传输收完后用。
 * 内部会记录这些路径的身份，避免轮询时又把它们发回去（防回环）。 */
int oti_clip_set_files(struct oti_clip *c, char paths[][OTI_CLIP_PATH_MAX], int n);

/* 只探测"剪贴板里是不是文件"，**不读文件内容**（大文件不能每次轮询都读一遍）。
 * 返回 1 = 是文件（files/total 已填），0 = 不是文件，<0 = 出错。
 * ident 出参 = (路径, 大小, mtime) 的指纹，用于"内容没变就不重发"。 */
int oti_clip_probe_files(struct oti_clip *c, char files[][OTI_CLIP_PATH_MAX], int max_files,
                         int *count, uint64_t *total, uint32_t *ident);

/* 把一组文件读成"文件包"（format 3 的载荷，不带头部；小文件用）。
 * 0 = 成功（*out 为 malloc，调用者 free），<0 = 错误 */
int oti_clip_pack_files(struct oti_clip *c, char paths[][OTI_CLIP_PATH_MAX], int n, char **out,
                        size_t *outlen);

/* 吃一个 CLIP 消息体；返回 1 = 一个完整对象已重组好（用 oti_clip_result 取），
 * 0 = 还在收，<0 = 错误/丢弃 */
int oti_clip_feed(struct oti_clip *c, const oti_clip_hdr *h, const uint8_t *data, uint32_t dlen);
const uint8_t *oti_clip_result(struct oti_clip *c, size_t *len);

#endif /* OTICLIP_H */
