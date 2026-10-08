/*
 * otiprobe.c —— 单文件 Linux 侧探测工具（给"主控端"那台机器用）
 *
 * 为什么单文件：只需要在 Linux 机器上有 gcc，或直接拷贝已编译好的静态二进制，
 * 不必把整个项目搬过去。
 *
 * 编译:  gcc -O2 -o otiprobe otiprobe.c
 *        gcc -O2 -static -o otiprobe-static otiprobe.c     # 拷贝到别的机器用
 *
 * 用法:
 *   sudo ./otiprobe infoall        列出 0ea0:2213 的 /dev/sgN 并读设备信息块
 *   sudo ./otiprobe pages          读 0xD8 各分页（看设备状态结构）
 *   sudo ./otiprobe read N         连读 N 个 64KB 帧并判定（有效/空闲/非法）
 *   sudo ./otiprobe write          写一个合法帧（0xD9/0x2A），打印 SCSI 状态与 sense
 *   sudo ./otiprobe ping           写一个 PING 帧（我们协议），用于两端联调
 *   sudo ./otiprobe hidsend <1|2|3> <24位hex>  发一个 HID 包（type1 键盘/type2 鼠标/type3 多媒体）
 *   sudo ./otiprobe hidseq         发候选 HID 包序列（配合对端观察以标定 12 字节格式）
 *   sudo ./otiprobe loop           持续读帧并打印（Ctrl+C 退出）
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <scsi/sg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define VID 0x0ea0
#define PID 0x2213
#define FRAME_SIZE 65536
#define FRAME_HEAD 20
#define FRAME_BODY (FRAME_SIZE - 2 * FRAME_HEAD)

struct res {
    int rc, status, host_status, driver_status, resid, sense_len;
    unsigned char sense[32];
};

static int read_hex_file(const char *path, unsigned *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = (fscanf(f, "%x", out) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

static int ids_match(char *dir)
{
    for (int depth = 0; depth < 12; depth++) {
        char p[PATH_MAX + 16];
        unsigned v = 0, pid = 0;
        snprintf(p, sizeof(p), "%s/idVendor", dir);
        if (read_hex_file(p, &v) == 0) {
            snprintf(p, sizeof(p), "%s/idProduct", dir);
            if (read_hex_file(p, &pid) == 0 && v == VID && pid == PID)
                return 1;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir)
            break;
        *slash = '\0';
    }
    return 0;
}

static int find_sg(char out[][64], int max)
{
    DIR *d = opendir("/sys/class/scsi_generic");
    if (!d)
        return -1;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < max) {
        if (strncmp(e->d_name, "sg", 2) != 0)
            continue;
        char link[PATH_MAX], real[PATH_MAX];
        snprintf(link, sizeof(link), "/sys/class/scsi_generic/%s", e->d_name);
        if (!realpath(link, real))
            continue;
        if (!ids_match(real))
            continue;
        snprintf(out[n], 64, "/dev/%.48s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

static const char *lun_type(const char *sg)
{
    static char buf[16];
    const char *nm = strrchr(sg, '/');
    char p[256];
    snprintf(p, sizeof(p), "/sys/class/scsi_generic/%.48s/device/type", nm ? nm + 1 : sg);
    FILE *f = fopen(p, "r");
    if (!f)
        return "?";
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return "?";
    }
    fclose(f);
    for (char *q = buf; *q; q++)
        if (*q == '\n')
            *q = 0;
    return buf;
}

static int exec_cdb(int fd, const unsigned char *cdb, unsigned cdb_len, void *data,
                    unsigned len, int to_dev, struct res *r)
{
    struct sg_io_hdr h;
    memset(&h, 0, sizeof(h));
    h.interface_id = 'S';
    h.cmdp = (unsigned char *)cdb;
    h.cmd_len = cdb_len;
    h.dxferp = data;
    h.dxfer_len = len;
    h.dxfer_direction = (len == 0) ? SG_DXFER_NONE : (to_dev ? SG_DXFER_TO_DEV : SG_DXFER_FROM_DEV);
    unsigned char sense[32];
    memset(sense, 0, sizeof(sense));
    h.sbp = sense;
    h.mx_sb_len = sizeof(sense);
    h.timeout = 5000;
    int io = ioctl(fd, SG_IO, &h);
    if (r) {
        memset(r, 0, sizeof(*r));
        r->status = h.status;
        r->host_status = h.host_status;
        r->driver_status = h.driver_status;
        r->resid = (int)h.resid;
        r->sense_len = h.sb_len_wr;
        memcpy(r->sense, sense, sizeof(r->sense));
        r->rc = (io < 0) ? -errno
               : (h.status || h.host_status || h.driver_status)
                     ? (0x100 | (h.status & 0xff) | (h.host_status << 8) | (h.driver_status << 16))
                     : 0;
    }
    if (io < 0)
        return -errno;
    if (h.status || h.host_status || h.driver_status)
        return 0x100 | (h.status & 0xff) | (h.host_status << 8) | (h.driver_status << 16);
    return 0;
}

static void tail16(unsigned char c[16])
{
    c[14] = 'O';
    c[15] = 'T';
}

static void show_sense(const struct res *r)
{
    int key = r->sense_len > 2 ? (r->sense[2] & 0x0f) : -1;
    int asc = r->sense_len > 12 ? r->sense[12] : -1;
    int ascq = r->sense_len > 13 ? r->sense[13] : -1;
    printf(" scale=%d key=0x%x ASC=0x%x ASCQ=0x%x", r->sense_len, key, asc, ascq);
}

static int frame_check(const unsigned char *f)
{
    int allzero = 1;
    for (int i = 0; i < FRAME_HEAD; i++)
        if (f[i]) {
            allzero = 0;
            break;
        }
    if (allzero)
        return 0;
    return memcmp(f, f + FRAME_SIZE - FRAME_HEAD, FRAME_HEAD) == 0 ? 1 : -1;
}

static void frame_pack(const void *body, size_t len, unsigned char *frame)
{
    memset(frame, 0, FRAME_SIZE);
    frame[0] = 'O'; frame[1] = 'T'; frame[2] = 'I'; frame[3] = 'L';
    uint32_t l = (uint32_t)len;
    memcpy(frame + 4, &l, 4);
    if (len) {
        if (len > FRAME_BODY)
            len = FRAME_BODY;
        memcpy(frame + FRAME_HEAD, body, len);
    }
    memcpy(frame + FRAME_SIZE - FRAME_HEAD, frame, FRAME_HEAD);
}

/* 增量 CRC32：可把"头[0..15] + 载荷"分段算出来（与另两端实现一致） */
static uint32_t crc32_upd(uint32_t crc, const unsigned char *b, size_t n)
{
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

static int do_read_frame(int fd, unsigned char *f, uint16_t held)
{
    unsigned char cdb[16] = {0};
    struct res r;
    cdb[0] = 0xD8; cdb[1] = 0x00; cdb[2] = 0x03;
    cdb[3] = (unsigned char)(held >> 8); cdb[4] = (unsigned char)(held & 0xff);
    tail16(cdb);
    return exec_cdb(fd, cdb, 16, f, FRAME_SIZE, 0, &r);
}

static int do_write_frame(int fd, const unsigned char *f, struct res *r)
{
    unsigned char cdb[16] = {0};
    cdb[0] = 0xD9; cdb[1] = 0x2A; cdb[2] = 0xFF;
    tail16(cdb);
    return exec_cdb(fd, cdb, 16, (void *)f, FRAME_SIZE, 1, r);
}

/* HID 包：0xD9/0x33|0x34|0x36，12 字节放在 CDB[2..13]，**数据阶段为空**（厂商用法） */
static int do_send_hid(int fd, int type, const unsigned char p12[12], struct res *r)
{
    unsigned char cdb[16] = {0};
    unsigned char sub;
    switch (type) {
    case 1: sub = 0x33; break;
    case 2: sub = 0x34; break;
    case 3: sub = 0x36; break;
    default: return -1;
    }
    cdb[0] = 0xD9; cdb[1] = sub;
    memcpy(cdb + 2, p12, 12);
    tail16(cdb);
    return exec_cdb(fd, cdb, 16, NULL, 0, 1, r);
}

static void hex12(const unsigned char *p, char *out)
{
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 12; i++) {
        out[i * 3] = h[p[i] >> 4];
        out[i * 3 + 1] = h[p[i] & 15];
        out[i * 3 + 2] = ' ';
    }
    out[36] = 0;
}

static int do_info(int fd, unsigned char *out, unsigned n, struct res *r)
{
    unsigned char cdb[16] = {0};
    cdb[0] = 0xF0; cdb[1] = 0x00; cdb[2] = 0x00;
    tail16(cdb);
    return exec_cdb(fd, cdb, 16, out, n, 0, r);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "infoall";
    char paths[16][64];
    int n = find_sg(paths, 16);
    if (n <= 0) {
        fprintf(stderr, "没找到 0ea0:2213 的 /dev/sgN（线缆是否插在本机？驱动是否绑定？）\n");
        return 1;
    }
    if (strcmp(cmd, "infoall") == 0) {
        printf("找到 %d 个 LUN\n", n);
        for (int i = 0; i < n; i++) {
            int fd = open(paths[i], O_RDWR);
            if (fd < 0) {
                printf("  %-10s [%s] 打不开: %s\n", paths[i], lun_type(paths[i]), strerror(errno));
                continue;
            }
            unsigned char info[64];
            memset(info, 0, sizeof(info));
            struct res r;
            int rc = do_info(fd, info, sizeof(info), &r);
            printf("  %-10s [type=%s] 信息块 rc=%d", paths[i], lun_type(paths[i]), rc);
            if (r.status || r.sense_len)
                show_sense(&r);
            if (rc == 0) {
                printf("\n      前16字节:");
                for (int k = 0; k < 16; k++)
                    printf(" %02x", info[k]);
                printf("\n      ASCII: ");
                for (int k = 0; k < 16; k++)
                    putchar(info[k] >= 32 && info[k] < 127 ? info[k] : '.');
                printf("\n");
            } else
                printf("\n");
            close(fd);
        }
        return 0;
    }

    /* 其余命令：设备 = 最后一个以 /dev/ 开头的参数（缺省用第一个 LUN）
       这样 read 3 /dev/sg3、hidsend 2 <hex> /dev/sg3 都能正确取参。 */
    const char *dev = paths[0];
    int argend = argc;
    if (argc > 2 && strncmp(argv[argc - 1], "/dev/", 5) == 0) {
        dev = argv[argc - 1];
        argend = argc - 1;
    }
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "打不开 %s: %s\n", dev, strerror(errno));
        return 1;
    }
    struct res r;
    static unsigned char frame[FRAME_SIZE];

    if (strcmp(cmd, "pages") == 0) {
        for (int sel = 0; sel <= 7; sel++) {
            unsigned char cdb[16] = {0};
            memset(frame, 0, FRAME_SIZE);
            cdb[0] = 0xD8; cdb[1] = 0x00; cdb[2] = (unsigned char)sel; cdb[4] = 1;
            tail16(cdb);
            int rc = exec_cdb(fd, cdb, 16, frame, 8192, 0, &r);
            int nz = 0;
            for (int k = 0; k < 8192; k++)
                if (frame[k])
                    nz++;
            printf("  page %d rc=%d 非零=%d 头:", sel, rc, nz);
            for (int k = 0; k < 24; k++)
                printf(" %02x", frame[k]);
            printf("\n");
            if (rc)
                show_sense(&r), printf("\n");
        }
    } else if (strcmp(cmd, "read") == 0) {
        int cnt = argend > 2 ? atoi(argv[2]) : 3;
        for (int i = 0; i < cnt; i++) {
            memset(frame, 0, FRAME_SIZE);
            int rc = do_read_frame(fd, frame, 1);
            int chk = frame_check(frame);
            int nz = 0;
            for (int k = 0; k < FRAME_SIZE; k++)
                if (frame[k])
                    nz++;
            printf("  #%d rc=%d 判定=%s 非零=%d 头:", i + 1, rc,
                   chk == 1 ? "有效" : chk == 0 ? "空闲" : "非法", nz);
            for (int k = 0; k < 16; k++)
                printf(" %02x", frame[k]);
            printf("\n");
        }
    } else if (strcmp(cmd, "write") == 0 || strcmp(cmd, "ping") == 0) {
        if (strcmp(cmd, "ping") == 0) {
            /* 组一个 PING 消息（与跨平台协议一致），再打成一个合法帧 */
            unsigned char msg[64];
            uint32_t magic = 0x314C544F, len = 4, seq = 1;
            unsigned char payload[4] = {0x44, 0x33, 0x22, 0x11};
            memset(msg, 0, sizeof(msg));
            memcpy(msg, &magic, 4);
            msg[4] = 5; /* type=PING */
            memcpy(msg + 8, &seq, 4);
            memcpy(msg + 12, &len, 4);
            memcpy(msg + 20, payload, 4);
            uint32_t c = crc32_upd(0, msg, 16);
            c = crc32_upd(c, msg + 20, 4);
            memcpy(msg + 16, &c, 4);
            frame_pack(msg, 24, frame);
        } else {
            frame_pack("otiprobe", 8, frame);
        }
        int rc = do_write_frame(fd, frame, &r);
        printf("写帧 rc=%d status=0x%02x resid=%d", rc, r.status, r.resid);
        if (r.status || r.sense_len)
            show_sense(&r);
        printf("\n");
    } else if (strcmp(cmd, "hidsend") == 0) {
        int type = argc > 2 ? atoi(argv[2]) : 1;
        unsigned char p[12] = {0};
        if (argc > 3) {
            const char *hx = argv[3];
            for (int i = 0; i < 12 && hx[i * 2] && hx[i * 2 + 1]; i++) {
                char t[3] = {hx[i * 2], hx[i * 2 + 1], 0};
                p[i] = (unsigned char)strtoul(t, NULL, 16);
            }
        }
        char hs[40];
        hex12(p, hs);
        int rc = do_send_hid(fd, type, p, &r);
        printf("HID type=%d 12字节: %s rc=%d status=0x%02x", type, hs, rc, r.status);
        if (r.status)
            show_sense(&r);
        printf("\n");
        /* 紧跟一个全零释放包，避免对端卡住按键 */
        unsigned char z[12] = {0};
        do_send_hid(fd, type, z, &r);
    } else if (strcmp(cmd, "hidseq") == 0) {
        /* 候选序列：每个候选都用"可区分的效果"，便于对端观察后反推格式。
           鼠标用大位移（对端光标会跳），键盘用不同字母（对端会出不同字符）。 */
        struct { const char *name; int type; unsigned char p[12]; } seq[] = {
            { "键盘A1 reportID=1 +保留 +usage0x04(A)", 1, {0x01,0x00,0x00,0x04,0,0,0,0,0,0,0,0} },
            { "键盘A2 无保留字节",                     1, {0x01,0x00,0x04,0x00,0,0,0,0,0,0,0,0} },
            { "键盘A3 无 reportID",                    1, {0x00,0x00,0x04,0x00,0,0,0,0,0,0,0,0} },
            { "键盘A4 usage=0x1E(win扫描码A)",         1, {0x01,0x00,0x00,0x00,0x1E,0,0,0,0,0,0,0} },
            { "鼠标M1 reportID=1 dx=+200",             2, {0x01,0x00,200,0x00,0x00,0,0,0,0,0,0,0} },
            { "鼠标M2 reportID=1 dy=+200",             2, {0x01,0x00,0x00,200,0x00,0,0,0,0,0,0,0} },
            { "鼠标M3 无reportID dx=+200",             2, {0x00,0x00,200,0x00,0x00,0,0,0,0,0,0,0} },
            { "鼠标M4 reportID=2 dx=+200",             2, {0x02,0x00,200,0x00,0x00,0,0,0,0,0,0,0} },
            { "鼠标M5 dx=200(16位LE)在第2字节起",      2, {0x01,0x00,0xC8,0x00,0,0,0,0,0,0,0,0} },
        };
        int n = (int)(sizeof(seq) / sizeof(seq[0]));
        printf("开始发送 %d 个候选 HID 包（每个间隔 2 秒，对端请观察光标/字符）\n", n);
        for (int i = 0; i < n; i++) {
            char hs[40];
            hex12(seq[i].p, hs);
            int rc = do_send_hid(fd, seq[i].type, seq[i].p, &r);
            printf("  [%2d/%d] type=%d %-38s %s rc=%d\n", i + 1, n, seq[i].type, seq[i].name, hs, rc);
            fflush(stdout);
            usleep(300 * 1000);
            unsigned char z[12] = {0};
            do_send_hid(fd, seq[i].type, z, &r);       /* 释放 */
            sleep(2);
        }
        printf("候选序列发送完毕\n");
    } else if (strcmp(cmd, "mousecal") == 0) {
        /* 鼠标格式标定：每个候选把光标推动一个**唯一**的位移量，
           对端（Windows）跑 cursorwatch.ps1，看位移量即可反推是哪个格式生效。 */
        struct { const char *name; unsigned char p[12]; } seq[] = {
            { "K1  dx@1  +50  [btn,dx,dy,wh]（标准HID报告）", {0x00,0x32,0x00,0x00,0x00,0,0,0,0,0,0,0} },
            { "K2  dx@2  +110",                               {0x00,0x00,0x6e,0x00,0x00,0,0,0,0,0,0,0} },
            { "K3  dx@3  +170",                               {0x00,0x00,0x00,0xaa,0x00,0,0,0,0,0,0,0} },
            { "K4  dx@4  +230",                               {0x00,0x00,0x00,0x00,0xe6,0,0,0,0,0,0,0} },
            { "K5  dx@5  +290",                               {0x00,0x00,0x00,0x00,0x00,0x22,0x01,0,0,0,0,0} },
            { "K6  dx@6 16位LE +350",                         {0x00,0x00,0x00,0x00,0x00,0x00,0x5e,0x01,0,0,0,0} },
            { "K7  rid=1,dx@2 +140",                          {0x01,0x00,0x8c,0x00,0x00,0,0,0,0,0,0,0} },
            { "K8  rid=1,dx@3 +200",                          {0x01,0x00,0x00,0xc8,0x00,0,0,0,0,0,0,0} },
            { "K9  rid=2,dx@3 +260",                          {0x02,0x00,0x00,0x04,0x01,0,0,0,0,0,0,0} },
            { "K10 dx@3 16位LE +320",                         {0x00,0x00,0x00,0x40,0x01,0,0,0,0,0,0,0} },
        };
        int n = (int)(sizeof(seq) / sizeof(seq[0]));
        printf("开始鼠标格式标定：%d 个候选，每个间隔 2 秒（对端请用 cursorwatch.ps1 观察光标）\n", n);
        for (int i = 0; i < n; i++) {
            char hs[40];
            hex12(seq[i].p, hs);
            printf(">>> 候选 %d/%d  %-42s 12字节: %s\n", i + 1, n, seq[i].name, hs);
            fflush(stdout);
            int rc = do_send_hid(fd, 2, seq[i].p, &r);
            printf("      rc=%d", rc);
            if (r.status || r.sense_len)
                show_sense(&r);
            printf("\n");
            fflush(stdout);
            usleep(250 * 1000);
            unsigned char z[12] = {0};
            do_send_hid(fd, 2, z, &r);       /* 释放（回中位/松开按键） */
            sleep(2);
        }
        printf("标定候选发送完毕\n");
    } else if (strcmp(cmd, "rprobe") == 0) {
        /* 用 0xAA 预填缓冲，量出设备"真正回写了多少字节"，并扫遍分页/held/长度 */
        static unsigned char buf[FRAME_SIZE];
        int sels[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        int helds[3] = {0, 1, 100};
        int lens[2] = {65536, 8192};
        for (int li = 0; li < 2; li++) {
            for (int si = 0; si < 8; si++) {
                for (int hi = 0; hi < 3; hi++) {
                    int len = lens[li], sel = sels[si], held = helds[hi];
                    memset(buf, 0xAA, sizeof buf);
                    unsigned char cdb[16] = {0};
                    cdb[0] = 0xD8; cdb[1] = 0x00; cdb[2] = (unsigned char)sel;
                    cdb[3] = (unsigned char)(held >> 8);
                    cdb[4] = (unsigned char)(held & 0xff);
                    tail16(cdb);
                    int rc = exec_cdb(fd, cdb, 16, buf, len, 0, &r);
                    int changed = 0, nz = 0;
                    for (int k = 0; k < len; k++) {
                        if (buf[k] != 0xAA) changed++;
                        if (buf[k]) nz++;
                    }
                    printf("  sel=%d held=%-3d len=%-6d rc=%-6d st=0x%02x resid=%-6d 改动=%-6d 非零=%-6d 头:",
                           sel, held, len, rc, r.status, r.resid, changed, nz);
                    for (int k = 0; k < 16; k++) printf(" %02x", buf[k]);
                    printf("\n");
                    if (rc && r.sense_len) show_sense(&r), printf("\n");
                }
            }
        }
        /* 顺带量一下"写"的 resid 与尾部吸附 */
        printf("== 写帧的 resid 与缓冲区吸附 ==\n");
        {
            static unsigned char wf[FRAME_SIZE];
            frame_pack("OTIPROBE-END-TO-END-MARKER", 26, wf);
            int rc = do_write_frame(fd, wf, &r);
            printf("  写 rc=%d st=0x%02x resid=%d\n", rc, r.status, r.resid);
            if (rc && r.sense_len) show_sense(&r), printf("\n");
        }
    } else if (strcmp(cmd, "raw") == 0) {
        /* 通用探针：raw <cdbhex> <len> <in|out|none> [设备]
           cdbhex 最多 32 个十六进制字符（≤16 字节），不足部分填 0；
           若给出的字节 <16 且 CDB[14..15] 为 0，则自动补 'OT'（厂商模板尾巴）。
           打印 rc/status/resid/sense 与缓冲区前 32 字节 + 非零计数。 */
        unsigned char cdb[16] = {0};
        int len = 16, dir = 0;   /* 0=in 1=out 2=none */
        if (argend > 2) {
            char hx[128];
            int hn = 0;
            for (const char *q = argv[2]; *q && hn < 32; q++) {
                if (isxdigit((unsigned char)*q)) hx[hn++] = *q;
            }
            hx[hn] = 0;
            int n = hn / 2;
            if (n > 16) n = 16;
            for (int i = 0; i < n; i++) {
                char t[3] = {hx[i*2], hx[i*2+1], 0};
                cdb[i] = (unsigned char)strtoul(t, NULL, 16);
            }
            if (n < 16 && cdb[14] == 0 && cdb[15] == 0) tail16(cdb);
        }
        if (argend > 3) len = atoi(argv[3]);
        if (argend > 4) {
            if (strcmp(argv[4], "in") == 0) dir = 0;
            else if (strcmp(argv[4], "out") == 0) dir = 1;
            else dir = 2;
        }
        static unsigned char buf[FRAME_SIZE];
        memset(buf, 0xAA, sizeof buf);
        if (dir == 1) memset(buf, 0x11, sizeof buf);
        int rc;
        if (dir == 2) rc = exec_cdb(fd, cdb, 16, NULL, 0, 1, &r);
        else rc = exec_cdb(fd, cdb, 16, buf, len, dir == 1, &r);
        printf("  CDB:");
        for (int k = 0; k < 16; k++) printf(" %02x", cdb[k]);
        printf("\n  方向=%s 长度=%d rc=%d status=0x%02x resid=%d", dir == 0 ? "IN" : dir == 1 ? "OUT" : "无", len, rc, r.status, r.resid);
        if (r.status || r.sense_len) show_sense(&r);
        printf("\n");
        if (dir != 2) {
            int chg = 0, nz = 0;
            unsigned char fill = (dir == 1) ? 0x11 : 0xAA;
            for (int k = 0; k < len; k++) { if (buf[k] != fill) chg++; if (buf[k]) nz++; }
            printf("  改动=%d 非零=%d 前32B:", chg, nz);
            for (int k = 0; k < 32 && k < len; k++) printf(" %02x", buf[k]);
            printf("\n");
        }
    } else if (strcmp(cmd, "vend") == 0) {
        /* 按观察到的厂商帧头形状构造一帧：
             [image_id:4][时间戳:4][0:4][1:4][载荷长度:4] + 载荷 + 头部20字节副本
           用于验证"设备是否要求帧头里带有效 image 描述"。 */
        static unsigned char f[FRAME_SIZE];
        const char *text = argend > 2 ? argv[2] : "hello-from-otilink";
        unsigned tl = (unsigned)strlen(text);
        uint32_t id = 1, ts = (uint32_t)time(NULL), z = 0, one = 1, len = tl;
        memset(f, 0, FRAME_SIZE);
        memcpy(f + 0, &id, 4);
        memcpy(f + 4, &ts, 4);
        memcpy(f + 8, &z, 4);
        memcpy(f + 12, &one, 4);
        memcpy(f + 16, &len, 4);
        memcpy(f + 20, text, tl);
        memcpy(f + FRAME_SIZE - FRAME_HEAD, f, FRAME_HEAD);
        printf("  厂商形状帧: 载荷=%u 字节 头20B:", tl);
        for (int k = 0; k < 20; k++) printf(" %02x", f[k]);
        printf("\n");
        int rc = do_write_frame(fd, f, &r);
        printf("  写 rc=%d status=0x%02x resid=%d", rc, r.status, r.resid);
        if (rc || r.status) show_sense(&r);
        printf("\n");
    } else if (strcmp(cmd, "tx") == 0) {
        /* 按厂商循环的顺序：读消息(同时上报待发帧数) → 若回 0x06/0x07(授权) → 立刻写帧。
           读与写紧挨着，中间不做任何多余操作（这是与之前测试的关键差别）。 */
        static unsigned char frame[FRAME_SIZE];
        int rounds = argend > 2 ? atoi(argv[2]) : 40;
        int ok = 0;
        for (int i = 0; i < rounds; i++) {
            unsigned char m[64];
            memset(m, 0, sizeof m);
            unsigned char cdb[16] = {0};
            cdb[0] = 0xD8; cdb[1] = 0x00; cdb[2] = 0x03;
            cdb[3] = 0x00; cdb[4] = 0x01;          /* 上报：我有 1 帧要发 */
            tail16(cdb);
            int rc = exec_cdb(fd, cdb, 16, m, 16, 0, &r);
            unsigned char t = m[0];
            if (rc == 0 && (t == 0x06 || t == 0x07)) {
                /* 授权到了 —— 立刻写，不做别的 */
                frame_pack("otilink-tx", 10, frame);
                int wrc = do_write_frame(fd, frame, &r);
                if (wrc == 0) {
                    ok++;
                    printf("  #%d 消息type=0x%02x → 写成功 rc=0（第 %d 次成功）\n", i + 1, t, ok);
                    fflush(stdout);
                } else if (i < 3) {
                    printf("  #%d 消息type=0x%02x → 写失败 rc=%d", i + 1, t, wrc);
                    show_sense(&r);
                    printf("\n");
                }
            } else if (i < 3) {
                printf("  #%d 消息type=0x%02x（未授权）\n", i + 1, t);
            }
            usleep(60 * 1000);
        }
        printf("共 %d 轮，写成功 %d 次\n", rounds, ok);
    } else if (strcmp(cmd, "sendxml") == 0) {
        /* sendxml <file>  —— 把文件里的 XML 按厂商格式打成帧发出去。
           厂商帧体格式（从抓包确认）：[1 字节 0x39][u32 LE 负载长度][XML 文本] */
        static unsigned char fr[FRAME_SIZE];
        static unsigned char body[FRAME_BODY];
        const char *path = argend > 2 ? argv[2] : NULL;
        if (!path) { printf("用法: sendxml <xml文件>\n"); return 1; }
        FILE *f = fopen(path, "rb");
        if (!f) { printf("打不开 %s\n", path); return 1; }
        size_t n = fread(body + 5, 1, FRAME_BODY - 5, f);
        fclose(f);
        body[0] = 0x39;
        uint32_t l = (uint32_t)n;
        memcpy(body + 1, &l, 4);
        memset(fr, 0, FRAME_SIZE);
        /* 用我们自己的 20 字节头（设备只要求 head==tail；对端按 body 解析） */
        fr[0] = 'O'; fr[1] = 'T'; fr[2] = 'I'; fr[3] = 'L';
        uint32_t bl = (uint32_t)(n + 5);
        memcpy(fr + 4, &bl, 4);
        memcpy(fr + FRAME_HEAD, body, n + 5);
        memcpy(fr + FRAME_SIZE - FRAME_HEAD, fr, FRAME_HEAD);
        printf("发送 XML 帧: 负载 %zu 字节 + 5 字节头 = %u\n", n, bl);
        /* 必须走厂商授权时序：读消息(上报"我有 1 帧要发") → 回 0x06/0x07 才可写 */
        int rc = -1, tries = 0;
        for (tries = 0; tries < 400; tries++) {
            unsigned char m[64];
            memset(m, 0, sizeof m);
            unsigned char cdb[16] = {0};
            cdb[0] = 0xD8; cdb[1] = 0x00; cdb[2] = 0x03; cdb[3] = 0x00; cdb[4] = 0x01;
            tail16(cdb);
            if (exec_cdb(fd, cdb, 16, m, 16, 0, &r) != 0) { usleep(1000); continue; }
            if (m[0] == 0x06 || m[0] == 0x07) {
                rc = do_write_frame(fd, fr, &r);
                break;
            }
            usleep(1000);
        }
        printf("写 rc=%d（第 %d 轮拿到授权）status=0x%02x", rc, tries + 1, r.status);
        if (rc || r.status) show_sense(&r);
        printf("\n");
    } else if (strcmp(cmd, "dumpfull") == 0) {
        /* 完整转储每一帧的帧体文本（用于抓厂商 XML 命令） */
        static unsigned char fr[FRAME_SIZE];
        int rounds = argend > 2 ? atoi(argv[2]) : 200;
        for (int i = 0; i < rounds; i++) {
            unsigned char m[64];
            memset(m, 0, sizeof m);
            unsigned char c1[16] = {0};
            c1[0] = 0xD8; c1[1] = 0x00; c1[2] = 0x03;
            tail16(c1);
            if (exec_cdb(fd, c1, 16, m, 16, 0, &r) == 0 && m[0] == 0x05) {
                unsigned n = ((unsigned)m[1] << 8) | m[2];
                if (n > 6) n = 6;
                for (unsigned k = 0; k < n; k++) {
                    unsigned char c2[16] = {0};
                    c2[0] = 0xD9; c2[1] = 0x28; c2[2] = 0x64;
                    tail16(c2);
                    memset(fr, 0, FRAME_SIZE);
                    if (exec_cdb(fd, c2, 16, fr, FRAME_SIZE, 0, &r) != 0)
                        continue;
                    if (frame_check(fr) != 1)
                        continue;
                    uint32_t l = 0;
                    memcpy(&l, fr + 4, 4);
                    if (l > FRAME_BODY)
                        l = FRAME_BODY;
                    if (l == 0)
                        continue;
                    printf("---- frame len=%u ----\n", l);
                    for (uint32_t j = 0; j < l && j < 3000; j++) {
                        unsigned char c = fr[FRAME_HEAD + j];
                        putchar(c >= 32 && c < 127 ? c : '.');
                    }
                    printf("\n---- end ----\n");
                    fflush(stdout);
                }
            }
            usleep(120 * 1000);
        }
    } else if (strcmp(cmd, "rx") == 0) {
        /* 完整接收循环（按 GetData/ProcessIdleState 反汇编语义实现）：
             1) 0xD8/0x00/0x03 读 **16 字节消息**（不是帧！）
             2) 消息[0]==0x05 → be16(消息[1..2]) = 对端已投递帧数
             3) 0xD9/0x28/0x64 + 65536 字节 IN  ← 才是真正的帧读取
        */
        static unsigned char frame[FRAME_SIZE];
        int rounds = argend > 2 ? atoi(argv[2]) : 10;
        for (int i = 0; i < rounds; i++) {
            unsigned char m[64];
            memset(m, 0, sizeof m);
            unsigned char cdb[16] = {0};
            cdb[0] = 0xD8; cdb[1] = 0x00; cdb[2] = 0x03;
            tail16(cdb);
            int rc = exec_cdb(fd, cdb, 16, m, 16, 0, &r);
            int cnt = (m[1] << 8) | m[2];
            printf("  [%d] 消息 rc=%-4d type=0x%02x  内容:", i + 1, rc, m[0]);
            for (int k = 0; k < 16; k++) printf(" %02x", m[k]);
            printf("   (be16@1=%d)", cnt);
            if (rc && r.sense_len) show_sense(&r);
            printf("\n");
            fflush(stdout);
            if (rc == 0 && m[0] == 0x05 && cnt > 0) {
                int want = cnt > 8 ? 8 : cnt;
                for (int f = 0; f < want; f++) {
                    unsigned char cdb2[16] = {0};
                    cdb2[0] = 0xD9; cdb2[1] = 0x28; cdb2[2] = 0x64;
                    tail16(cdb2);
                    memset(frame, 0, FRAME_SIZE);
                    int rc2 = exec_cdb(fd, cdb2, 16, frame, FRAME_SIZE, 0, &r);
                    int chk = frame_check(frame);
                    int nz = 0;
                    for (int k = 0; k < FRAME_SIZE; k++)
                        if (frame[k]) nz++;
                    printf("      帧#%d rc=%d 判定=%s 非零=%d 头20B:", f + 1, rc2,
                           chk == 1 ? "有效" : chk == 0 ? "空闲" : "非法", nz);
                    for (int k = 0; k < 20; k++) printf(" %02x", frame[k]);
                    printf("\n");
                    if (rc2 && r.sense_len) show_sense(&r), printf("\n");
                    /* 帧体前 96 字节 hex + ASCII */
                    printf("      体@20 hex :");
                    for (int k = 20; k < 20 + 48; k++) printf(" %02x", frame[k]);
                    printf("\n      体@20 ascii: |");
                    for (int k = 20; k < 20 + 96; k++) {
                        unsigned char c = frame[k];
                        putchar(c >= 32 && c < 127 ? c : '.');
                    }
                    printf("|\n");
                    /* 搜索常见标记 */
                    const char *marks[] = { "OTIL", "OTL1", "OTIMSG", "<?xml", "otiprobe", "OTI" };
                    for (int mi = 0; mi < 6; mi++) {
                        int ml = (int)strlen(marks[mi]);
                        for (int k = 0; k + ml < FRAME_SIZE; k++) {
                            if (memcmp(frame + k, marks[mi], ml) == 0) {
                                printf("      → offset %d 有 '%s'\n", k, marks[mi]);
                                break;
                            }
                        }
                    }
                    fflush(stdout);
                }
            }
            usleep(200 * 1000);
        }
    } else if (strcmp(cmd, "cap") == 0) {
        /* 十六进制抓帧器：被动监听线缆，把对端（厂商程序）发来的**每一个**帧
         * 连类型字节一起打出来。用于逆向厂商 KM 事件线格式。
         *   otiprobe cap [秒数] [设备]
         */
        static unsigned char fc[FRAME_SIZE];
        int secs = argend > 2 ? atoi(argv[2]) : 30;
        time_t t0 = time(NULL);
        long nm = 0, nf = 0;
        printf("cap: 监听 %d 秒（Ctrl+C 提前结束）\n", secs);
        fflush(stdout);
        while ((int)(time(NULL) - t0) < secs) {
            unsigned char m[64];
            memset(m, 0, sizeof m);
            unsigned char c1[16] = {0};
            c1[0] = 0xD8; c1[1] = 0x00; c1[2] = 0x03;
            tail16(c1);
            if (exec_cdb(fd, c1, 16, m, 16, 0, &r) != 0)
                continue;
            double ts = difftime(time(NULL), t0);
            int interesting = (m[0] != 0x00);
            if (interesting) {
                nm++;
                printf("[%6.1fs] 消息 type=0x%02x :", ts, m[0]);
                for (int k = 0; k < 16; k++) printf(" %02x", m[k]);
                printf("\n");
                fflush(stdout);
            }
            int cnt = (m[1] << 8) | m[2];
            if (m[0] == 0x05 && cnt > 0) {
                int want = cnt > 16 ? 16 : cnt;
                for (int f = 0; f < want; f++) {
                    unsigned char c2[16] = {0};
                    c2[0] = 0xD9; c2[1] = 0x28; c2[2] = 0x64;
                    tail16(c2);
                    memset(fc, 0, FRAME_SIZE);
                    if (exec_cdb(fd, c2, 16, fc, FRAME_SIZE, 0, &r) != 0)
                        break;
                    if (frame_check(fc) != 1)
                        continue;
                    nf++;
                    uint32_t l = 0;
                    memcpy(&l, fc + 4, 4);
                    if (l > FRAME_BODY) l = FRAME_BODY;
                    printf("  ==== 帧 #%ld len=%u 类型字节=0x%02x ====\n", nf, l, fc[FRAME_HEAD]);
                    printf("  hex[0..96):");
                    for (int k = FRAME_HEAD; k < FRAME_HEAD + 96; k++) {
                        if ((k - FRAME_HEAD) % 32 == 0) printf("\n    %3d:", k - FRAME_HEAD);
                        printf(" %02x", fc[k]);
                    }
                    printf("\n  ascii: |");
                    for (uint32_t k = FRAME_HEAD; k < FRAME_HEAD + (l < 600 ? l : 600); k++) {
                        unsigned char c = fc[k];
                        putchar(c >= 32 && c < 127 ? c : '.');
                    }
                    printf("|\n");
                    fflush(stdout);
                }
            }
            usleep(2000);
        }
        printf("cap 结束：收到 %ld 条非零消息、%ld 个有效帧\n", nm, nf);
    } else if (strcmp(cmd, "usbrestart") == 0) {
        /* 厂商 USBRestart（@0x782f）：0xF0/0x05/0x02，CDB[4]=0x0A。
           用途：HID 接口卡死（设备枚举 OK 但收不到报告）时重新初始化。 */
        unsigned char cdb[16] = {0};
        cdb[0] = 0xF0; cdb[1] = 0x05; cdb[2] = 0x02; cdb[4] = 0x0A;
        tail16(cdb);
        printf("USBRestart CDB:");
        for (int k = 0; k < 16; k++) printf(" %02x", cdb[k]);
        printf("\n");
        int rc = exec_cdb(fd, cdb, 16, NULL, 0, 1, &r);
        printf("rc=%d status=0x%02x", rc, r.status);
        if (r.status) show_sense(&r);
        printf("\n");
    } else if (strcmp(cmd, "loop") == 0) {
        for (;;) {
            memset(frame, 0, FRAME_SIZE);
            int rc = do_read_frame(fd, frame, 1);
            int chk = rc ? -9 : frame_check(frame);
            printf("帧 rc=%d 判定=%s 头:", rc,
                   chk == 1 ? "有效" : chk == 0 ? "空闲" : chk == -1 ? "非法" : "错误");
            for (int k = 0; k < 16; k++)
                printf(" %02x", frame[k]);
            printf("\n");
            fflush(stdout);
            usleep(500 * 1000);
        }
    } else {
        printf("用法: %s [infoall|pages|read N|write|ping|hidsend <t> <hex>|hidseq|loop] [设备]\n", argv[0]);
    }
    close(fd);
    return 0;
}
