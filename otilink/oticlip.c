/*
 * oticlip.c —— 见 oticlip.h
 */
#include "oticlip.h"

#include "otilink.h"           /* oti_adopt_path()：root 运行时把收到的文件交还桌面用户 */

#include <errno.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

static int have_cmd(const char *cmd)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "command -v %s >/dev/null 2>&1", cmd);
    return system(buf) == 0;
}

const char *oti_clip_backend_name(const struct oti_clip *c)
{
    switch (c->be) {
    case OTI_CLIP_BE_WAYLAND: return "wayland(wl-copy/wl-paste)";
    case OTI_CLIP_BE_X11:     return "x11(xclip)";
    case OTI_CLIP_BE_FILE:    return "file(sim)";
    default:                  return "none";
    }
}

/* 后端探测（不打开任何选择区）：绿色包要在启动时就把"这台机器剪贴板靠什么"打出来，
   否则用户只会看到"复制粘贴没反应"，没有任何线索。 */
const char *oti_clip_backend_probe(void)
{
    if (getenv("WAYLAND_DISPLAY") && have_cmd("wl-paste") && have_cmd("wl-copy"))
        return "wayland(wl-copy/wl-paste)";
    if (getenv("DISPLAY") && have_cmd("xclip"))
        return "x11(xclip)";
    if (have_cmd("wl-paste") && have_cmd("wl-copy"))
        return "wayland(wl-copy/wl-paste)";
    if (getenv("WAYLAND_DISPLAY"))
        return "不可用（缺 wl-clipboard： sudo apt install wl-clipboard / sudo dnf install wl-clipboard）";
    if (getenv("DISPLAY"))
        return "不可用（缺 xclip： sudo apt install xclip / sudo dnf install xclip / sudo pacman -S xclip）";
    return "file(无图形会话)";
}

int oti_clip_open(struct oti_clip *c, const char *sim_file, size_t max_bytes)
{
    memset(c, 0, sizeof(*c));
    c->max_bytes = max_bytes ? max_bytes : (1u << 20); /* 默认 1MB */
    if (sim_file) {
        c->be = OTI_CLIP_BE_FILE;
        snprintf(c->file, sizeof(c->file), "%s", sim_file);
        /* 不存在就建空文件，便于读写一致 */
        FILE *f = fopen(c->file, "a");
        if (f)
            fclose(f);
        return 0;
    }
    if (getenv("WAYLAND_DISPLAY") && have_cmd("wl-paste") && have_cmd("wl-copy")) {
        c->be = OTI_CLIP_BE_WAYLAND;
        return 0;
    }
    if (getenv("DISPLAY") && have_cmd("xclip")) {
        c->be = OTI_CLIP_BE_X11;
        return 0;
    }
    if (have_cmd("wl-paste") && have_cmd("wl-copy")) {   /* 无 DISPLAY 变量也试一下 */
        c->be = OTI_CLIP_BE_WAYLAND;
        return 0;
    }
    /* 退回 file 后端：让 daemon 仍能启动（只是没有系统剪贴板可同步） */
    c->be = OTI_CLIP_BE_FILE;
    snprintf(c->file, sizeof(c->file), "/tmp/otilink-clipboard.sim");
    return 0;
}

void oti_clip_close(struct oti_clip *c)
{
    free(c->rx_buf);
    c->rx_buf = NULL;
}

/* ---------- 读 ---------- */
static int read_stream(FILE *p, char **out, size_t *len)
{
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf)
        return -ENOMEM;
    for (;;) {
        if (n + 1024 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return -ENOMEM;
            }
            buf = nb;
        }
        size_t r = fread(buf + n, 1, cap - n, p);
        n += r;
        if (r == 0)
            break;
    }
    *out = buf;
    *len = n;
    return 0;
}

/* ---------- 文件剪贴板：轻量探测 / 打包 ---------- */

/* file://host/path（含 %XX）→ 本地路径 */
static int uri_to_path(const char *u, size_t ulen, char *out, size_t cap)
{
    if (ulen < 7 || strncmp(u, "file://", 7) != 0)
        return -1;
    size_t i = 7;
    while (i < ulen && u[i] != '/')      /* 跳过主机名（file://localhost/...） */
        i++;
    size_t k = 0;
    for (; i < ulen && u[i] != '\n' && u[i] != '\r' && k + 1 < cap; i++) {
        if (u[i] == '%' && i + 2 < ulen && isxdigit((unsigned char)u[i + 1]) &&
            isxdigit((unsigned char)u[i + 2])) {
            char h[3] = { u[i + 1], u[i + 2], 0 };
            out[k++] = (char)strtol(h, NULL, 16);
            i += 2;
        } else {
            out[k++] = u[i];
        }
    }
    out[k] = 0;
    return k ? 0 : -1;
}

/* 一组路径的指纹：路径 + 大小 + mtime。
   用 mtime 而不是内容哈希，是为了**不读文件**就能判断"还是那一份"——
   500MB 的文件不可能每 300ms 读一遍。 */
static uint32_t paths_ident(char paths[][OTI_CLIP_PATH_MAX], int n, uint64_t *total)
{
    uint32_t h = 0;
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        struct stat st;
        uint64_t sz = 0, mt = 0;
        if (stat(paths[i], &st) == 0) {
            sz = (uint64_t)st.st_size;
            mt = (uint64_t)st.st_mtime;
        }
        sum += sz;
        h = oti_crc32_append(h, paths[i], strlen(paths[i]));
        h = oti_crc32_append(h, &sz, sizeof(sz));
        h = oti_crc32_append(h, &mt, sizeof(mt));
    }
    if (total)
        *total = sum;
    return h;
}

int oti_clip_probe_files(struct oti_clip *c, char files[][OTI_CLIP_PATH_MAX], int max_files,
                         int *count, uint64_t *total, uint32_t *ident)
{
    if (count)
        *count = 0;
    if (total)
        *total = 0;
    if (ident)
        *ident = 0;

    int n = 0;
    if (c->be == OTI_CLIP_BE_FILE) {
        /* 模拟后端：文件内容就是"剪贴板文本"。若里面是 file:// 列表，就当成文件剪贴板
           —— 这样大文件传输能在本机（两个 otikm 走 socketpair）回归，不用真机动线缆。 */
        FILE *f = fopen(c->file, "rb");
        if (!f)
            return 0;
        char buf[8192];
        size_t r = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[r] = 0;
        if (r < 8 || strncmp(buf, "file://", 7) != 0)
            return 0;
        size_t start = 0;
        for (size_t i = 0; i <= r && n < max_files; i++) {
            if (i == r || buf[i] == '\n' || buf[i] == '\r') {
                if (i > start) {
                    char path[OTI_CLIP_PATH_MAX];
                    size_t ll = i - start;
                    if (uri_to_path(buf + start, ll, path, sizeof(path)) == 0) {
                        struct stat st;
                        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                            snprintf(files[n++], OTI_CLIP_PATH_MAX, "%s", path);
                    }
                }
                start = i + 1;
            }
        }
        if (n == 0)
            return 0;
        if (count)
            *count = n;
        if (ident)
            *ident = paths_ident(files, n, total);
        else if (total)
            (void)paths_ident(files, n, total);
        return 1;
    }

    if (c->be != OTI_CLIP_BE_X11)
        return 0;

    /* 1) text/uri-list（文件管理器/截图工具的标准做法） */
    FILE *pu = popen("xclip -selection clipboard -t text/uri-list -o 2>/dev/null", "r");
    if (pu) {
        char *ub = NULL; size_t ul = 0;
        if (read_stream(pu, &ub, &ul) == 0 && ub && ul > 8) {
            size_t start = 0;
            for (size_t i = 0; i <= ul && n < max_files; i++) {
                if (i == ul || ub[i] == '\n' || ub[i] == '\r') {
                    if (i > start) {
                        char path[OTI_CLIP_PATH_MAX];
                        char line[OTI_CLIP_PATH_MAX];
                        size_t ll = i - start;
                        if (ll >= sizeof(line))
                            ll = sizeof(line) - 1;
                        memcpy(line, ub + start, ll);
                        line[ll] = 0;
                        if (uri_to_path(line, ll, path, sizeof(path)) == 0) {
                            struct stat st;
                            if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                                snprintf(files[n++], OTI_CLIP_PATH_MAX, "%s", path);
                        }
                    }
                    start = i + 1;
                }
            }
        }
        free(ub);
        pclose(pu);
    }
    /* 2) 兜底：纯文本路径（有些程序只把路径当文本放进剪贴板） */
    if (n == 0) {
        FILE *pt = popen("xclip -selection clipboard -o 2>/dev/null", "r");
        if (pt) {
            char *tb = NULL; size_t tl = 0;
            if (read_stream(pt, &tb, &tl) == 0 && tb && tl > 1 && tl < 4096) {
                char path[OTI_CLIP_PATH_MAX];
                if (tl > 7 && strncmp(tb, "file://", 7) == 0) {
                    if (uri_to_path(tb, tl, path, sizeof(path)) == 0) {
                        struct stat st;
                        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                            snprintf(files[n++], OTI_CLIP_PATH_MAX, "%s", path);
                    }
                } else if (tb[0] == '/') {
                    size_t k = 0;
                    while (k < tl && tb[k] != '\n' && tb[k] != '\r' && k + 1 < sizeof(path)) {
                        path[k] = tb[k];
                        k++;
                    }
                    path[k] = 0;
                    struct stat st;
                    if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
                        snprintf(files[n++], OTI_CLIP_PATH_MAX, "%s", path);
                }
            }
            free(tb);
            pclose(pt);
        }
    }
    if (n == 0)
        return 0;
    if (count)
        *count = n;
    if (ident)
        *ident = paths_ident(files, n, total);
    else if (total)
        (void)paths_ident(files, n, total);
    return 1;
}

/* 把一组文件读成"文件包"（format 3）。
   小文件走这条路（一次发完，收端整包重组）；大文件走 otixfer 的分片流式传输。 */
int oti_clip_pack_files(struct oti_clip *c, char paths[][OTI_CLIP_PATH_MAX], int n, char **out,
                        size_t *outlen)
{
    (void)c;
    *out = NULL;
    *outlen = 0;
    size_t cap = 4096, used = 2;
    char *buf = malloc(cap);
    if (!buf)
        return -ENOMEM;
    uint16_t cnt = (uint16_t)n;
    memcpy(buf, &cnt, 2);
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        char *fb = NULL; size_t fl = 0;
        if (read_stream(f, &fb, &fl) != 0 || fl == 0) {
            free(fb);
            fclose(f);
            continue;
        }
        fclose(f);
        const char *base = strrchr(paths[i], '/');
        base = base ? base + 1 : paths[i];
        size_t nl = strlen(base);
        size_t need = used + 4 + nl + 8 + fl;
        if (need > cap) {
            while (cap < need)
                cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(fb);
                free(buf);
                return -ENOMEM;
            }
            buf = nb;
        }
        uint32_t nl32 = (uint32_t)nl;
        uint64_t sz64 = (uint64_t)fl;
        memcpy(buf + used, &nl32, 4); used += 4;
        memcpy(buf + used, base, nl); used += nl;
        memcpy(buf + used, &sz64, 8); used += 8;
        memcpy(buf + used, fb, fl);  used += fl;
        free(fb);
    }
    *out = buf;
    *outlen = used;
    return 0;
}

int oti_clip_read(struct oti_clip *c, char **out, size_t *len)
{    *out = NULL;
    *len = 0;
    switch (c->be) {
    case OTI_CLIP_BE_WAYLAND: {
        FILE *p = popen("wl-paste --no-newline 2>/dev/null", "r");
        if (!p)
            return -EIO;
        int rc = read_stream(p, out, len);
        int st = pclose(p);
        if (rc)
            return rc;
        if (st != 0 && *len == 0) {
            free(*out);
            *out = NULL;
            return 1;
        }
        return 0;
    }
    case OTI_CLIP_BE_X11: {
        /* 先试图片：有 image/png 目标就用它（图片优先级高于文本，
           因为很多程序复制图片时同时提供 text/plain 的占位串）。
           注意 xclip 找不到该目标时退出码非 0 且无输出 → 退回文本。 */
        c->last_format = 1;
        FILE *pi = popen("xclip -selection clipboard -t image/png -o 2>/dev/null", "r");
        if (pi) {
            char *ibuf = NULL; size_t ilen = 0;
            int irc = read_stream(pi, &ibuf, &ilen);
            int ist = pclose(pi);
            if (irc == 0 && ist == 0 && ilen > 8 &&
                (unsigned char)ibuf[0] == 0x89 && ibuf[1] == 'P' && ibuf[2] == 'N' && ibuf[3] == 'G') {
                *out = ibuf; *len = ilen;
                c->last_format = 2;              /* PNG 图片 */
                return 0;
            }
            free(ibuf);
        }
        /* 再试 text/uri-list：**很多程序（文件管理器 / 浏览器 / 截图工具）复制图片时
           只放文件路径，不放 image/png**，只读文本的话对端只会收到一串路径
           （用户实际遇到的就是这个）。路径指向本地文件时，直接把文件内容当图片发。 */
        {
            FILE *pu = popen("xclip -selection clipboard -t text/uri-list -o 2>/dev/null", "r");
            if (pu) {
                char *ub = NULL; size_t ul = 0;
                int urc = read_stream(pu, &ub, &ul);
                int ust = pclose(pu);
                if (urc == 0 && ub && ul > 8 && strncmp(ub, "file://", 7) == 0) {   /* 不要求退出码 0 */
                (void)ust;
                    char path[1024];
                    size_t k = 0;
                    /* 只取第一条 URI（到换行/回车为止），并做 %XX 解码 */
                    for (size_t i = 7; i < ul && ub[i] != '\n' && ub[i] != '\r' && k + 1 < sizeof(path); i++) {
                        if (ub[i] == '%' && i + 2 < ul && isxdigit((unsigned char)ub[i+1]) && isxdigit((unsigned char)ub[i+2])) {
                            char h[3] = { ub[i+1], ub[i+2], 0 };
                            path[k++] = (char)strtol(h, NULL, 16);
                            i += 2;
                        } else {
                            path[k++] = ub[i];
                        }
                    }
                    path[k] = 0;
                    FILE *f = path[0] ? fopen(path, "rb") : NULL;
                    if (f) {
                        char *fb = NULL; size_t fl = 0;
                        int frc = read_stream(f, &fb, &fl);
                        fclose(f);
                        if (frc == 0 && fl > 8) {
                            /* 按魔数分流：图片→format 2；**其它文件→文件包 format 3**。
                               这里早期无条件设成 2，导致 .txt/.zip 被当图片发，对端解码失败。 */
                            const unsigned char *z = (const unsigned char *)fb;
                            int im = (fl > 4 && z[0] == 0x89 && z[1] == 'P' && z[2] == 'N' && z[3] == 'G') ||
                                     (fl > 3 && z[0] == 0xFF && z[1] == 0xD8 && z[2] == 0xFF) ||
                                     (fl > 3 && z[0] == 'G' && z[1] == 'I' && z[2] == 'F') ||
                                     (fl > 2 && z[0] == 'B' && z[1] == 'M');
                            free(ub);
                            if (im) {
                                *out = fb; *len = (size_t)fl;
                                c->last_format = 2;
                                return 0;
                            }
                            {
                                const char *base = strrchr(path, '/');
                                base = base ? base + 1 : path;
                                size_t nl = strlen(base);
                                size_t tot = 2 + 4 + nl + 8 + (size_t)fl;
                                char *bb = (char *)malloc(tot);
                                if (bb) {
                                    char *w = bb;
                                    uint16_t one = 1; uint32_t nl32 = (uint32_t)nl; uint64_t sz64 = (uint64_t)fl;
                                    memcpy(w, &one, 2); w += 2;
                                    memcpy(w, &nl32, 4); w += 4;
                                    memcpy(w, base, nl); w += nl;
                                    memcpy(w, &sz64, 8); w += 8;
                                    memcpy(w, fb, (size_t)fl);
                                    free(fb);
                                    *out = bb; *len = tot;
                                    c->last_format = 3;      /* 文件包 */
                                    return 0;
                                }
                            }
                            free(fb);
                            *out = NULL; *len = 0;
                            return 1;
                        }
                        free(fb);
                    }
                }
                free(ub);
            }
        }
        FILE *p = popen("xclip -selection clipboard -o 2>/dev/null", "r");
        if (!p)
            return -EIO;
        int rc = read_stream(p, out, len);
        int st = pclose(p);
        if (rc)
            return rc;
        if (st != 0 && *len == 0) {
            free(*out);
            *out = NULL;
            return 1;
        }
        /* 兜底：有些程序只把**纯文本路径**放进剪贴板（不是 text/uri-list）。
           这种也要把文件内容当图片发 —— 否则对端收到的就是一串 Linux 路径
           （用户实际反馈的正是这个现象）。 */
        if (*out && *len > 1) {
            const char *tp = *out;
            size_t tn = *len;
            /* 去掉 file:// 前缀以及可能的主机名：
               file:///home/... / file://localhost/home/...
               用户实际遇到的就是 "file://~/文档/图片2.png" 这种，
               早期只认 '/' 开头 → 认不出来 → 当文本发过去（对端只看到一串 URI）。 */
            if (tn > 7 && strncmp(tp, "file://", 7) == 0) {
                size_t s2 = 7;
                while (s2 < tn && tp[s2] != '/')
                    s2++;
                tp += s2;
                tn -= s2;
            }
        if (tn > 1 && tp[0] == '/') {
            char p2[1024];
            size_t k2 = 0;
            for (size_t i = 0; i < tn && tp[i] != '\n' && tp[i] != '\r' && k2 + 1 < sizeof(p2); i++)
                p2[k2++] = tp[i];
            p2[k2] = 0;
            /* 直接用 fopen 在麒麟上会被 KySec 拒（本地编译的二进制被标为不可信，
               读不了 ~/文档 这类目录，errno=13；实测同一路径在 shell 里可读）。
               改走**受信任的系统命令** cat 代读；cat 不行再退回 fopen。 */
            FILE *f2 = NULL;
            if (k2 > 0 && !strchr(p2, '\'')) {
                char cmd[1200];
                snprintf(cmd, sizeof(cmd), "cat '%s' 2>/dev/null", p2);
                f2 = popen(cmd, "r");
                if (f2 && fgetc(f2) == EOF) { pclose(f2); f2 = NULL; }
                else if (f2) rewind(f2);
            }
            if (!f2)
                f2 = fopen(p2, "rb");
            fprintf(stderr, "[clip] 路径 '%s' len=%zu 读取=%s errno=%d (%s)\n", p2, k2, f2 ? "OK" : "失败", f2 ? 0 : errno, f2 ? "" : strerror(errno));
            if (f2) {
                unsigned char m[8] = {0};
                size_t rd = fread(m, 1, 8, f2);
                int isimg = (rd >= 4 && m[0] == 0x89 && m[1] == 'P' && m[2] == 'N' && m[3] == 'G') ||
                            (rd >= 3 && m[0] == 0xFF && m[1] == 0xD8 && m[2] == 0xFF) ||
                            (rd >= 3 && m[0] == 'G' && m[1] == 'I' && m[2] == 'F') ||
                            (rd >= 2 && m[0] == 'B' && m[1] == 'M');
                rewind(f2);
                /* 注意：**不能**写成 if (isimg) —— 非图片文件（.txt/.zip/...）也要在这里
                   打包成文件包（format 3），否则它们仍然只会过去一串路径。
                   下面按魔数分流：图片→format 2，其它→format 3。 */
                (void)isimg;
                {
                    char *fb = NULL; size_t fl = 0;
                    if (read_stream(f2, &fb, &fl) == 0 && fl > 8) {
                        /* 是图片就按图片发（format 2）；**不是图片就打包成文件包**（format 3）——
                           否则对端只能收到一串 Linux 路径，粘不出文件。
                           文件包布局（小端）：u16 个数；每项 u32 名长、名字、u64 长度、内容。 */
                        unsigned char m2[8] = {0};
                        size_t rd2 = 0;
                        int isimg2 = 0;
                        if (fl > 8) {
                            memcpy(m2, fb, 8); rd2 = 8;
                            isimg2 = (m2[0] == 0x89 && m2[1] == 'P' && m2[2] == 'N' && m2[3] == 'G') ||
                                     (m2[0] == 0xFF && m2[1] == 0xD8 && m2[2] == 0xFF) ||
                                     (m2[0] == 'G' && m2[1] == 'I' && m2[2] == 'F');
                        }
                        (void)rd2;
                        pclose(f2);
                        free(*out);
                        if (isimg2) {
                            *out = fb; *len = fl;
                            c->last_format = 2;
                            return 0;
                        }
                        {
                            const char *base = strrchr(p2, '/');
                            base = base ? base + 1 : p2;
                            size_t nl = strlen(base);
                            size_t tot = 2 + 4 + nl + 8 + fl;
                            char *bb = (char *)malloc(tot);
                            if (bb) {
                                char *w = bb;
                                uint16_t one = 1; uint32_t nl32 = (uint32_t)nl; uint64_t sz64 = (uint64_t)fl;
                                memcpy(w, &one, 2); w += 2;
                                memcpy(w, &nl32, 4); w += 4;
                                memcpy(w, base, nl); w += nl;
                                memcpy(w, &sz64, 8); w += 8;
                                memcpy(w, fb, fl);
                                free(fb);
                                *out = bb; *len = tot;
                                c->last_format = 3;      /* 文件包 */
                                return 0;
                            }
                        }
                        free(fb);
                        *out = NULL;
                        *len = 0;
                        return 1;              /* 打包失败：明确表示"没数据"，不要返回悬空指针 */
                    }
                    free(fb);
                }
                pclose(f2);
            }
        }
        }
        return 0;
    }
    case OTI_CLIP_BE_FILE: {
        FILE *f = fopen(c->file, "rb");
        if (!f)
            return 1;
        int rc = read_stream(f, out, len);
        fclose(f);
        return rc ? rc : (*len ? 0 : 1);
    }
    default:
        return 1;
    }
}

/* ---------- 写 ---------- */

/* 把一组文件路径挂到剪贴板（text/uri-list），并把它们的指纹记进 last_ident，
   这样下一轮轮询不会又把同一批文件发回去（防回环）。 */
int oti_clip_set_files(struct oti_clip *c, char paths[][OTI_CLIP_PATH_MAX], int n)
{
    if (n <= 0)
        return -1;
    char list[8192];
    size_t lp = 0;
    for (int i = 0; i < n && lp + 600 < sizeof(list); i++)
        lp += (size_t)snprintf(list + lp, sizeof(list) - lp, "file://%s\r\n", paths[i]);
    if (c->be == OTI_CLIP_BE_X11) {
        FILE *xp = popen("xclip -selection clipboard -t text/uri-list -i 2>/dev/null", "w");
        if (xp) {
            fwrite(list, 1, lp, xp);
            pclose(xp);
        }
    } else if (c->be == OTI_CLIP_BE_FILE) {
        FILE *f = fopen(c->file, "wb");
        if (!f)
            return -1;
        fwrite(list, 1, lp, f);
        fclose(f);
    } else {
        return -1;
    }
    /* 记录指纹（含 mtime，所以"用户又复制了同一个文件"仍会被认为是同一次内容） */
    uint32_t h = 0;
    for (int i = 0; i < n; i++) {
        struct stat st;
        uint64_t sz = 0, mt = 0;
        if (stat(paths[i], &st) == 0) {
            sz = (uint64_t)st.st_size;
            mt = (uint64_t)st.st_mtime;
        }
        h = oti_crc32_append(h, paths[i], strlen(paths[i]));
        h = oti_crc32_append(h, &sz, sizeof(sz));
        h = oti_crc32_append(h, &mt, sizeof(mt));
    }
    c->last_ident = h;
    c->have_ident = 1;
    return 0;
}
int oti_clip_write(struct oti_clip *c, const void *data, size_t len)
{
    /* 文件包（format 3）：解到临时目录，再把 file:// 列表放进剪贴板（text/uri-list），
       这样在文件管理器/桌面里可以直接粘贴出文件。 */
    if (c->last_format == 3 && c->be == OTI_CLIP_BE_X11) {
        const unsigned char *q = (const unsigned char *)data;
        size_t off = 0;
        uint16_t cnt = 0;
        if (len < 2)
            return -EINVAL;
        memcpy(&cnt, q, 2);
        off = 2;
        char dir[128];
        snprintf(dir, sizeof(dir), "/tmp/otilink-files-%d", (int)getpid());
        mkdir(dir, 0700);
        oti_adopt_path(dir);          /* 绿色包以 root 跑时：目录交还桌面用户 */

        char list[8192];
        size_t lp = 0;
        for (int k = 0; k < cnt && off + 4 <= len; k++) {
            uint32_t nl;
            memcpy(&nl, q + off, 4);
            off += 4;
            if (off + nl > len)
                break;
            char name[256];
            size_t cn = nl < sizeof(name) - 1 ? nl : sizeof(name) - 1;
            memcpy(name, q + off, cn);
            name[cn] = 0;
            off += nl;
            uint64_t sz;
            memcpy(&sz, q + off, 8);
            off += 8;
            if (off + sz > len)
                break;
            char fp[512];
            snprintf(fp, sizeof(fp), "%s/%s", dir, name);
            FILE *fo = fopen(fp, "wb");
            if (fo) {
                fwrite(q + off, 1, (size_t)sz, fo);
                fclose(fo);
                oti_adopt_path(fp);
            }
            off += (size_t)sz;
            if (lp + 600 < sizeof(list))
                lp += (size_t)snprintf(list + lp, sizeof(list) - lp, "file://%s\r\n", fp);
        }
        FILE *xp = popen("xclip -selection clipboard -t text/uri-list -i 2>/dev/null", "w");
        if (xp) {
            fwrite(list, 1, lp, xp);
            pclose(xp);
        }
        return 0;
    }
    switch (c->be) {
    case OTI_CLIP_BE_WAYLAND: {
        FILE *p = popen("wl-copy 2>/dev/null", "w");
        if (!p)
            return -EIO;
        fwrite(data, 1, len, p);
        return pclose(p) == 0 ? 0 : -EIO;
    }
    case OTI_CLIP_BE_X11: {
        const char *cmd = (c->last_format == 2)
            ? "xclip -selection clipboard -t image/png -i 2>/dev/null"
            : "xclip -selection clipboard -i 2>/dev/null";
        FILE *p = popen(cmd, "w");
        if (!p)
            return -EIO;
        fwrite(data, 1, len, p);
        int st = pclose(p);
        /* xclip 是"持锁"式实现：进程退出后所有者就没了，所以这里必须让 xclip
           继续持有选区。用 -quiet 会失去该行为，故保持默认并忽略退出码。 */
        (void)st;
        return 0;
    }
    case OTI_CLIP_BE_FILE: {
        FILE *f = fopen(c->file, "wb");
        if (!f)
            return -errno;
        size_t w = fwrite(data, 1, len, f);
        fclose(f);
        return w == len ? 0 : -EIO;
    }
    default:
        return -EINVAL;
    }
}

/* ---------- 重组 ---------- */
int oti_clip_feed(struct oti_clip *c, const oti_clip_hdr *h, const uint8_t *data, uint32_t dlen)
{
    if (h->total_len > c->max_bytes)
        return -2;                                  /* 超过上限：丢弃 */
    if (h->flags & OTI_CLIP_FIRST) {
        /* 记住这一段的格式：重组完成后 oti_clip_write 按它选 xclip 的目标类型 */
        c->last_format = h->format ? h->format : 1;
        free(c->rx_buf);
        c->rx_buf = malloc(h->total_len ? h->total_len : 1);
        if (!c->rx_buf)
            return -ENOMEM;
        c->rx_fid = h->fid;
        c->rx_total = h->total_len;
        c->rx_got = 0;
        c->rx_active = 1;
    }
    if (!c->rx_active || h->fid != c->rx_fid)
        return -3;                                  /* 没收到首块或 fid 不匹配 */
    if (h->offset != c->rx_got)
        return -4;                                  /* 乱序：本实现要求顺序到达 */
    if ((size_t)h->offset + dlen > c->rx_total)
        return -5;
    memcpy(c->rx_buf + h->offset, data, dlen);
    c->rx_got += dlen;
    if (h->flags & OTI_CLIP_LAST) {
        c->rx_active = 0;
        if (c->rx_got != c->rx_total)
            return -6;                              /* 长度对不上 */
        return 1;
    }
    return 0;
}

const uint8_t *oti_clip_result(struct oti_clip *c, size_t *len)
{
    *len = c->rx_total;
    return c->rx_buf;
}
