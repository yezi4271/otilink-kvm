/*
 * probe.c —— 对拷线标定/实验 CLI（Linux 侧）
 *
 *   ./probe list                      找属于 0ea0:2213 的 /dev/sgN
 *   ./probe selftest                  不需要硬件：帧打包/校验 + CDB 模板自检
 *   ./probe query <sub>               SCSI 0xF0 查询（sub: 0 状态 / 0x31 锁）
 *   ./probe lock <0|1>                独占锁
 *   ./probe hid-release               发厂商的“清空键鼠状态”包（0xD9/0x33 + 14B 全零）
 *   ./probe hid <type> <12字节hex>    发 HID 包（type 1/2/3）
 *   ./probe wtest [文本]              发一个数据帧
 *   ./probe rtest                     收一帧并判定（有效/空闲/非法）
 *   ./probe rloop [n]                 连续收 n 帧
 *   ./probe rdump FILE [n] [--all]    把原始帧存盘（标定/采厂商头字节）
 *   ./probe info                      厂商初始化查询（设备信息块/总线类型/模式/锁）
 *   ./probe infoall                   遍历所有 LUN，报告哪个能应答厂商命令
 *   ./probe mode [值]                 读（或写）设备模式 0xD9/0x60
 *   ./probe rstatus [v2 v4 payload]   写远端主机状态 0xD8/0x01
 *   ./probe frameout FILE            生成 2 个真实帧到文件（跨实现兼容测试用）
 *   ./probe framein FILE [win]       解码另一实现写出的帧文件（win = 校验 Windows 侧期望值）
 *   ./probe hidsend <type> <hex12>    发原始 HID 包（标定 12 字节布局用）
 *   ./probe hidseq                    依次尝试几种候选布局，观察对端反应
 *   ./probe dummy                     发一个 64KB 全零帧（厂商 SendDummyData）
 *
 * 选项: --dev /dev/sgN（不给则自动选第一个匹配设备）
 */
#include "oticlip.h"
#include "otiinput.h"
#include "otilink.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void hexdump(const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i += 16) {
        printf("  %04zx  ", i);
        for (size_t j = 0; j < 16; j++)
            printf("%02x ", i + j < n ? b[i + j] : 0);
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < n; j++)
            putchar(b[i + j] >= 32 && b[i + j] < 127 ? b[i + j] : '.');
        printf("|\n");
    }
}

static int parse_hex12(const char *s, uint8_t out[12])
{
    int n = 0;
    while (*s && n < 12) {
        while (*s == ' ' || *s == ':')
            s++;
        if (!*s)
            break;
        char tmp[3] = {s[0], s[1] ? s[1] : 0, 0};
        out[n++] = (uint8_t)strtoul(tmp, NULL, 16);
        s += s[1] ? 2 : 1;
    }
    return n;
}


/* ================= 标定：自动扫 CDB 参数 =================
 * 线缆协议里唯一还依赖硬件的部分：0xD8(读)/0xD9/0x2A(写) 的 CDB[2..4]
 * 到底怎么编码 booking/credit。厂商代码里 CDB[2]=3 / CDB[2]=0xFF 是常量，
 * CDB[3]/CDB[4] 来自运行期寄存器，所以这里"一次只变一个轴"地扫，避免组合爆炸。
 */
struct sweep_axis {
    const char *name;
    int         idx;
    int         vals[8];
    int         n;
};

static const char *lun_kind(const char *sg_path)
{
    static char buf[32];
    const char *name = strrchr(sg_path, '/');
    char p[256];
    snprintf(p, sizeof(p), "/sys/class/scsi_generic/%.48s/device/type",
             name ? name + 1 : sg_path);
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
    if (strcmp(buf, "0") == 0)
        return "disk";
    if (strcmp(buf, "5") == 0)
        return "cdrom";
    return buf;
}

static void cdb_tail(uint8_t c[16])
{
    c[14] = 'O';
    c[15] = 'T';
}

static void sweep_run(struct otilink_dev *d, int is_write, int delay_ms,
                      int dry, const char *lun)
{
    uint8_t cdb[16], base[16];
    static uint8_t buf[OTI_FRAME_SIZE];
    struct oti_scsi_result r;

    memset(base, 0, sizeof(base));
    if (is_write) {
        base[0] = OTI_OP_WRITE;
        base[1] = OTI_SUB_DATA;
        base[2] = 0xFF;                     /* 厂商常量 */
    } else {
        base[0] = OTI_OP_READ;
        base[1] = 0x00;
        base[2] = 3;                        /* 厂商常量 */
    }
    cdb_tail(base);

    /* CDB[3..4] 已由反汇编确认 = 16 位大端"本机已持有未消费帧数"
     * （min(RX队列长度, MaxBookingSize=100)；ContinueTxCount≥11 时清零）。
     * 预期它不决定"能否收到数据"（取 0 也必须能收到第一帧），只影响设备的限流。
     * 因此 CDB[4] 扫 0..255 的正常窗口值，CDB[3] 单独扫高位（构造 >255 的窗口测试限流）。 */
    struct sweep_axis axes[3] = {
        {"CDB[2]", 2, {0, 1, 2, 3, 4, 0x7F, 0xFF}, 7},
        {"CDB[3]", 3, {0x01, 0x64, 0xFF}, 3},
        {"CDB[4]", 4, {0, 1, 50, 100, 101, 255}, 6},
    };

    printf("\n--- %s [%s] %s方向参数扫描 ---\n", d->sg_path, lun,
           is_write ? "写(0xD9/0x2A)" : "读(0xD8)");
    printf("  提示：CDB[3..4] 是「已持有帧数」的流控反馈，预期不门闸数据；"
           "若某取值导致不再回数据，说明设备按窗口限流。\n");

    /* 基准 */
    memcpy(cdb, base, 16);
    if (dry) {
        printf("  [dry-run] 基准 CDB: ");
        for (int i = 0; i < 16; i++)
            printf("%02x ", cdb[i]);
        printf("\n");
    } else {
        memset(buf, 0x5A, sizeof(buf));
        int rc = otilink_cdb_ex(d, cdb, buf, OTI_FRAME_SIZE, is_write, &r);
        printf("  基准 CDB[2]=%d CDB[3]=%d CDB[4]=%d  rc=%d status=0x%02x resid=%d xfer=%u 帧=%s\n",
               base[2], base[3], base[4], r.rc, r.status, r.resid,
               OTI_FRAME_SIZE - r.resid,
               rc ? "-" : (otilink_frame_check(buf) == 1 ? "有效" :
                           otilink_frame_check(buf) == 0 ? "空闲" : "非法"));
        usleep(delay_ms * 1000);
    }

    for (unsigned a = 0; a < 3; a++) {
        printf("  %s 扫描:\n", axes[a].name);
        for (int i = 0; i < axes[a].n; i++) {
            int v = axes[a].vals[i];
            if (v == base[axes[a].idx])
                continue;                    /* 基准已测 */
            memcpy(cdb, base, 16);
            cdb[axes[a].idx] = (uint8_t)v;
            if (dry) {
                printf("    [dry-run] %s=%d  CDB: ", axes[a].name, v);
                for (int k = 0; k < 16; k++)
                    printf("%02x ", cdb[k]);
                printf("\n");
                continue;
            }
            memset(buf, 0x5A, sizeof(buf));
            int rc = otilink_cdb_ex(d, cdb, buf, OTI_FRAME_SIZE, is_write, &r);
            unsigned xfer = (unsigned)(OTI_FRAME_SIZE - r.resid);
            printf("    %s=%-4d rc=%-4d status=0x%02x resid=%-6d xfer=%-6u", axes[a].name,
                   v, r.rc, r.status, r.resid, xfer);
            if (!rc) {
                int chk = otilink_frame_check(buf);
                printf(" 帧=%s", chk == 1 ? "有效" : chk == 0 ? "空闲" : "非法");
            }
            if (r.rc && r.sense_len > 0)
                printf(" sense=%02x/%02x/%02x", r.sense[0], r.sense[2], r.sense[12]);
            printf("\n");
            usleep(delay_ms * 1000);
        }
    }
}

static int selftest(void)
{
    int fail = 0;
    uint8_t frame[OTI_FRAME_SIZE];

    /* 1) 空载荷帧：合法、且头尾一致 */
    otilink_frame_pack(NULL, 0, frame);
    int r = otilink_frame_check(frame);
    printf("  [%s] 空帧判定 = %d（期望 1=有效）\n", r == 1 ? "PASS" : "FAIL", r);
    fail += (r != 1);

    /* 2) 头副本位于 65516，且 dword@0x10 与 dword@65532 相同（与厂商校验一致） */
    int hdr_tail_ok = memcmp(frame, frame + 0xffec, 16) == 0 &&
                      memcmp(frame + 0x10, frame + 0xfffc, 4) == 0;
    printf("  [%s] 头副本 @0xffec/0xfffc 与厂商校验一致\n", hdr_tail_ok ? "PASS" : "FAIL");
    fail += !hdr_tail_ok;

    /* 3) 载荷写入位置 = offset 20，长度 65496 上限 */
    const char *msg = "hello-otilink";
    otilink_frame_pack(msg, strlen(msg), frame);
    int body_ok = memcmp(frame + 20, msg, strlen(msg)) == 0;
    printf("  [%s] 载荷位于 offset %d\n", body_ok ? "PASS" : "FAIL", OTI_FRAME_HEAD);
    fail += !body_ok;

    /* 4) 全零 = 空闲帧 */
    memset(frame, 0, sizeof(frame));
    r = otilink_frame_check(frame);
    printf("  [%s] 全零帧判定 = %d（期望 0=空闲）\n", r == 0 ? "PASS" : "FAIL", r);
    fail += (r != 0);

    /* 5) 破坏尾头 = 非法 */
    otilink_frame_pack(msg, strlen(msg), frame);
    frame[0xffec] ^= 0xff;
    r = otilink_frame_check(frame);
    printf("  [%s] 尾部头被篡改判定 = %d（期望 -1=非法）\n", r == -1 ? "PASS" : "FAIL", r);
    fail += (r != -1);

    /* 6) HID CDB 模板：0xD9, sub, 前12字节, 'O','T' */
    uint8_t pkt[14] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
    uint8_t cdb[16] = {0};
    cdb[0] = OTI_OP_WRITE;
    cdb[1] = OTI_SUB_HID_TYPE1;
    memcpy(&cdb[2], pkt, OTI_HID_CDB_COPY);
    cdb[14] = 'O';
    cdb[15] = 'T';
    int cdb_ok = cdb[0] == 0xd9 && cdb[1] == 0x33 && cdb[13] == 12 &&
                 cdb[14] == 'O' && cdb[15] == 'T';
    printf("  [%s] HID CDB 模板: %02x %02x .. %02x %02x '%c%c'\n", cdb_ok ? "PASS" : "FAIL",
           cdb[0], cdb[1], cdb[13], cdb[14], cdb[14], cdb[15]);
    fail += !cdb_ok;

    printf("  => %s（%d 项失败）\n", fail ? "有失败" : "全部通过", fail);
    return fail;
}

int main(int argc, char **argv)
{
    const char *dev = NULL, *cmd = NULL;
    const char **rest = calloc(argc, sizeof(char *));
    int nrest = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dev") == 0 && i + 1 < argc)
            dev = argv[++i];
        else if (!cmd)
            cmd = argv[i];
        else
            rest[nrest++] = argv[i];
    }
    if (!cmd) {
        printf("用法见 probe.c 头部注释\n");
        return 2;
    }
    if (strcmp(cmd, "selftest") == 0)
        return selftest() ? 1 : 0;

    if (strcmp(cmd, "clip") == 0) {
        /* 剪贴板后端自检：写一段文本再读回比对。真机上先跑这个确认后端可用。 */
        const char *sim_file = NULL, *text = "otilink-clip-probe";
        for (int i = 0; i < nrest; i++) {
            if (strcmp(rest[i], "--clip-file") == 0 && i + 1 < nrest)
                sim_file = rest[++i];
            else if (rest[i][0] != '-')
                text = rest[i];
        }
        struct oti_clip c;
        if (oti_clip_open(&c, sim_file, 1u << 20) != 0) {
            printf("剪贴板初始化失败\n");
            return 1;
        }
        printf("后端: %s\n", oti_clip_backend_name(&c));
        char *orig = NULL;
        size_t olen = 0;
        int had = (oti_clip_read(&c, &orig, &olen) == 0 && orig);
        int w = oti_clip_write(&c, text, strlen(text));
        char *back = NULL;
        size_t blen = 0;
        int r = oti_clip_read(&c, &back, &blen);
        int ok = (w == 0 && r == 0 && blen == strlen(text) &&
                  memcmp(back, text, blen) == 0);
        printf("  [%s] 写入 %zu 字节 -> 读回 %zu 字节\n", ok ? "PASS" : "FAIL",
               strlen(text), blen);
        if (had)
            oti_clip_write(&c, orig, olen);      /* 恢复原剪贴板 */
        free(orig);
        free(back);
        oti_clip_close(&c);
        return ok ? 0 : 1;
    }

    if (strcmp(cmd, "sweep") == 0) {
        int is_write = 0, dry = 0, delay_ms = 150, all_luns = 0;
        for (int i = 0; i < nrest; i++) {
            if (strcmp(rest[i], "--write") == 0)
                is_write = 1;
            else if (strcmp(rest[i], "--dry-run") == 0)
                dry = 1;
            else if (strcmp(rest[i], "--all-luns") == 0)
                all_luns = 1;
            else if (strcmp(rest[i], "--delay") == 0 && i + 1 < nrest)
                delay_ms = atoi(rest[++i]);
        }
        if (!is_write && !dry)
            printf("提示：默认只做读方向扫描（安全）。写方向请显式加 --write，\n"
                   "      它会向设备写一个合法帧（含 20 字节头与头副本），不会破坏盘符。\n");
        if (dry) {
            struct otilink_dev dummy;
            memset(&dummy, 0, sizeof(dummy));
            dummy.sg = -1;
            snprintf(dummy.sg_path, sizeof(dummy.sg_path), "%s", dev ? dev : "(auto)");
            sweep_run(&dummy, is_write, delay_ms, 1, "dry");
            return 0;
        }
        char paths[16][64];
        int np = 0;
        if (dev) {
            snprintf(paths[0], 64, "%s", dev);
            np = 1;
        } else {
            np = otilink_find(paths, 16);
        }
        if (np <= 0) {
            fprintf(stderr, "没找到设备（先解决 usbipd/直连）\n");
            return 1;
        }
        for (int i = 0; i < np; i++) {
            const char *kind = lun_kind(paths[i]);
            if (!all_luns && strcmp(kind, "cdrom") == 0) {
                printf("跳过 %s（CD-ROM LUN；要扫请加 --all-luns）\n", paths[i]);
                continue;
            }
            struct otilink_dev dd;
            int rc = otilink_open(&dd, paths[i]);
            if (rc) {
                fprintf(stderr, "打开 %s 失败: %s\n", paths[i], strerror(-rc));
                continue;
            }
            sweep_run(&dd, is_write, delay_ms, 0, kind);
            otilink_close(&dd);
        }
        return 0;
    }

    if (strcmp(cmd, "frameout") == 0) {
        /* 生成两个真实帧写到文件（供另一实现解码，做跨实现线上兼容测试） */
        const char *file = nrest > 0 ? rest[0] : "/tmp/otilink.frames";
        FILE *f = fopen(file, "wb");
        if (!f) { fprintf(stderr, "打不开 %s\n", file); return 1; }
        static uint8_t frame[OTI_FRAME_SIZE], msg[128];
        oti_key_evt k = {.code = 30, .value = OTI_KEY_PRESS};
        size_t n = oti_encode_key(7, &k, msg, sizeof(msg));
        otilink_frame_pack(msg, n, frame);
        fwrite(frame, 1, OTI_FRAME_SIZE, f);
        oti_mouse_evt m = {.dx = -1234, .dy = 567, .wheel = -1, .buttons = 3};
        n = oti_encode_mouse(8, &m, msg, sizeof(msg));
        otilink_frame_pack(msg, n, frame);
        fwrite(frame, 1, OTI_FRAME_SIZE, f);
        fclose(f);
        printf("已写入 2 帧（KEY code=30 seq=7 / MOUSE dx=-1234 dy=567 wheel=-1 btn=3）到 %s\n", file);
        return 0;
    } else if (strcmp(cmd, "framein") == 0) {
        /* 解码另一实现写出的帧文件并校验内容 */
        const char *file = nrest > 0 ? rest[0] : "/tmp/otilink.frames";
        int expect_win = (nrest > 1 && strcmp(rest[1], "win") == 0);
        FILE *f = fopen(file, "rb");
        if (!f) { fprintf(stderr, "打不开 %s\n", file); return 1; }
        static uint8_t frame[OTI_FRAME_SIZE];
        int idx = 0, fails = 0;
        while (fread(frame, 1, OTI_FRAME_SIZE, f) == OTI_FRAME_SIZE) {
            int chk = otilink_frame_check(frame);
            oti_msg_hdr h;
            const uint8_t *pl;
            uint32_t len = 0;
            memcpy(&len, frame + 4, 4);
            int dr = oti_decode(frame + OTI_FRAME_HEAD, len, &h, &pl);
            printf("  帧#%d 帧判定=%s 解包=%s", ++idx,
                   chk == 1 ? "有效" : chk == 0 ? "空闲" : "非法", dr == 0 ? "OK" : "失败");
            int ok = (chk == 1 && dr == 0);
            if (ok && h.type == OTI_MSG_KEY) {
                oti_key_evt k;
                oti_decode_key(pl, h.len, &k);
                printf("  KEY code=%u value=%u seq=%u", k.code, k.value, h.seq);
                if (expect_win)
                    ok = (k.code == 31 && k.value == OTI_KEY_RELEASE && h.seq == 9);
            } else if (ok && h.type == OTI_MSG_MOUSE) {
                oti_mouse_evt m;
                oti_decode_mouse(pl, h.len, &m);
                printf("  MOUSE dx=%d dy=%d wheel=%d btn=%u seq=%u", m.dx, m.dy, m.wheel,
                       m.buttons, h.seq);
                if (expect_win)
                    ok = (m.dx == -5 && m.dy == 9 && h.seq == 10);
            }
            printf("  -> %s\n", ok ? "符合预期" : "不符预期");
            fails += !ok;
        }
        fclose(f);
        printf("  共 %d 帧，%d 帧不符预期\n", idx, fails);
        return (idx > 0 && fails == 0) ? 0 : 1;
    }

    if (strcmp(cmd, "inputs") == 0) {
        static char names[32][128], devs[32][64];
        int n = oti_input_list(names, devs, 32);
        if (n < 0) {
            printf("/dev/input 不可用（本机没有输入设备可见：%s）\n", strerror(errno));
            return 1;
        }
        printf("发现 %d 个输入设备：\n", n);
        for (int i = 0; i < n; i++)
            printf("  %-20s %s\n", devs[i], names[i]);
        return 0;
    }

    if (strcmp(cmd, "list") == 0) {
        char paths[16][64];
        int n = otilink_find(paths, 16);
        printf("找到 %d 个匹配 0ea0:2213 的 sg 设备\n", n);
        for (int i = 0; i < n; i++) {
            char v[128], m[128], l[128];
            FILE *f;
            char p[256];
            snprintf(p, sizeof(p), "/sys/class/scsi_generic/%s/device/vendor", strrchr(paths[i], '/') + 1);
            f = fopen(p, "r"); if (f) { if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f); } else v[0] = 0;
            snprintf(p, sizeof(p), "/sys/class/scsi_generic/%s/device/model", strrchr(paths[i], '/') + 1);
            f = fopen(p, "r"); if (f) { if (!fgets(m, sizeof(m), f)) m[0] = 0; fclose(f); } else m[0] = 0;
            snprintf(p, sizeof(p), "/sys/class/scsi_generic/%s/device/type", strrchr(paths[i], '/') + 1);
            f = fopen(p, "r"); if (f) { if (!fgets(l, sizeof(l), f)) l[0] = 0; fclose(f); } else l[0] = 0;
            for (char *q = v; *q; q++) if (*q == '\n') *q = 0;
            for (char *q = m; *q; q++) if (*q == '\n') *q = 0;
            printf("  %-10s vendor=%-10s model=%-20s scsi_type=%s", paths[i], v, m, l);
        }
        return n > 0 ? 0 : 1;
    }

    if (!dev) {
        static char auto_dev[64];
        char paths[16][64];
        int n = otilink_find(paths, 16);
        if (n <= 0) {
            fprintf(stderr, "没找到设备（线插在 Windows 上？Linux 侧需要 usbipd/直连）\n");
            return 1;
        }
        /* 与 otitrans.c 的 oti_tr_open_cable(NULL) 保持一致：**优先 CD-ROM LUN**（sysfs type=5）。
           原先取 paths[0]，而 paths[0] 通常是 MS/磁盘 LUN → 标定/回归用的 HID 包会发到错的 LUN
           （表现为 hwtest 第 1 步"Windows 光标没动"的假失败）。 */
        int pick = 0;
        for (int i = 0; i < n; i++) {
            if (strcmp(lun_kind(paths[i]), "cdrom") == 0) {
                pick = i;
                break;
            }
        }
        snprintf(auto_dev, sizeof(auto_dev), "%s", paths[pick]);
        dev = auto_dev;
        fprintf(stderr, "自动选择 %s\n", dev);
    }

    struct otilink_dev d;
    int rc = otilink_open(&d, dev);
    if (rc) {
        fprintf(stderr, "打开 %s 失败: %s\n", dev, strerror(-rc));
        return 1;
    }

    if (strcmp(cmd, "query") == 0) {
        uint8_t buf[512];
        memset(buf, 0, sizeof(buf));
        unsigned sub = nrest > 0 ? (unsigned)strtoul(rest[0], NULL, 0) : 0;
        rc = otilink_query(&d, (uint8_t)sub, buf, sizeof(buf));
        printf("0xF0/%02x 返回 %d\n", sub, rc);
        if (!rc) {
            printf("数据区前 64 字节：\n");
            hexdump(buf, 64);
        }
    } else if (strcmp(cmd, "lock") == 0) {
        rc = otilink_lock(&d, nrest > 0 && atoi(rest[0]) != 0);
        printf("锁操作返回 %d\n", rc);
    } else if (strcmp(cmd, "hid-release") == 0) {
        rc = otilink_hid_release_all(&d);
        printf("清键包返回 %d\n", rc);
    } else if (strcmp(cmd, "hidsend") == 0) {
        /* 原始 HID 包发送：hidsend <type> <最多12字节hex> [--pad N]
           用于标定 12 字节载荷布局（被控端会看到什么由观察决定）。 */
        int type = nrest > 0 ? atoi(rest[0]) : 1;
        uint8_t pkt[OTI_HID_PKT_LEN];
        memset(pkt, 0, sizeof(pkt));
        int nb = 0;
        if (nrest > 1)
            nb = parse_hex12(rest[1], pkt);
        printf("发送 HID 包 type=%d（给了 %d 字节），CDB[2..13] = ", type, nb);
        for (int i = 0; i < OTI_HID_CDB_COPY; i++)
            printf("%02x ", pkt[i]);
        printf("\n");
        rc = otilink_send_hid(&d, type, pkt);
        printf("返回 %d\n", rc);
    } else if (strcmp(cmd, "hidseq") == 0) {
        /* 校准序列：同一按键用几种候选布局各发一次（间隔 1s），
           观察被控端屏幕出现了什么，即可反推布局。 */
        const char *layouts[] = {
            "01 00 04 00 00 00 00 00 00 00 00 00",   /* [reportID=1][mods][rsvd][key=0x04(A)][...] */
            "00 00 04 00 00 00 00 00 00 00 00 00",   /* 无 reportID 变体 */
            "04 00 00 00 00 00 00 00 00 00 00 00",   /* 直接 keycode 打头 */
        };
        for (unsigned i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
            uint8_t pkt[OTI_HID_PKT_LEN];
            memset(pkt, 0, sizeof(pkt));
            parse_hex12(layouts[i], pkt);
            printf("候选 %u: ", i + 1);
            for (int k = 0; k < OTI_HID_CDB_COPY; k++)
                printf("%02x ", pkt[k]);
            rc = otilink_send_hid(&d, 1, pkt);
            printf(" -> rc=%d（观察对端是否出现按键）\n", rc);
            sleep(1);
            /* 再发一次“全零”作为释放 */
            memset(pkt, 0, sizeof(pkt));
            otilink_send_hid(&d, 1, pkt);
            sleep(1);
        }
    } else if (strcmp(cmd, "hid") == 0) {
        uint8_t pkt[14] = {0};
        int type = nrest > 0 ? atoi(rest[0]) : 1;
        if (nrest > 1)
            parse_hex12(rest[1], pkt);
        rc = otilink_send_hid(&d, type, pkt);
        printf("HID type=%d 返回 %d\n", type, rc);
    } else if (strcmp(cmd, "info") == 0) {
        /* 厂商初始化序列里的查询：IC 版本、物理总线类型。
           真实设备上先跑这个，能确认 SCSI 控制通道是否通。 */
        static uint8_t info[64];
        memset(info, 0, sizeof(info));
        uint8_t bus[16] = {0};
        int r1 = otilink_info_read(&d, info, sizeof(info));
        printf("0xF0/0x00/0x00 设备信息块（64B）: rc=%d\n", r1);
        if (!r1) {
            printf("  前 16 字节: ");
            for (int i = 0; i < 16; i++)
                printf("%02x ", info[i]);
            printf("\n  （厂商从这块里取 IC 版本/侧别 GetSideType/功能类型 GetFunctionType）\n");
        }
        uint8_t mode = 0xFF;
        int r0 = otilink_dev_mode_get(&d, &mode);
        printf("0xD9/0x60 设备模式: rc=%d mode=%u\n", r0, mode);
        int r2 = otilink_bus_type(&d, bus);
        printf("0xF0/0x00/0x02 物理总线类型: rc=%d\n", r2);
        if (!r2) {
            printf("  16 字节: ");
            for (int i = 0; i < 16; i++)
                printf("%02x ", bus[i]);
            printf("\n");
        }
        int r3 = otilink_lock(&d, 1);
        printf("0xF0/0x31 独占锁: rc=%d\n", r3);
        int r4 = otilink_lock(&d, 0);
        printf("0xF0/0x31 解锁: rc=%d\n", r4);
        return (r1 || r2) ? 1 : 0;
    } else if (strcmp(cmd, "sgtest") == 0) {
        /* SG_IO 层验证：标准 SCSI 命令 + 64KB 双向 + 厂商命令的错误路径，
           可对着 /dev/sgN（真实内核设备，如 scsi_debug 造的）跑。 */
        int fails = 0;
        struct oti_scsi_result r;
        static uint8_t buf[OTI_FRAME_SIZE];

        /* 1) INQUIRY（6 字节 CDB，36 字节 IN） */
        uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
        memset(buf, 0, 36);
        int rc1 = otilink_cdb_len(&d, inq, 6, buf, 36, 0, &r);
        printf("  [%s] INQUIRY rc=%d status=0x%02x resid=%d  厂商='%.8s' 型号='%.16s'\n",
               (rc1 == 0) ? "PASS" : "FAIL", rc1, r.status, r.resid, buf + 8, buf + 16);
        fails += (rc1 != 0);

        /* 2) TEST UNIT READY */
        uint8_t tur[6] = {0};
        int rc2 = otilink_cdb_len(&d, tur, 6, NULL, 0, 1, &r);
        printf("  [%s] TEST UNIT READY rc=%d status=0x%02x\n", (rc2 == 0) ? "PASS" : "FAIL",
               rc2, r.status);
        fails += (rc2 != 0);

        /* 3) READ CAPACITY(10) */
        uint8_t rc10[10] = {0x25};
        uint8_t cap[8] = {0};
        int rc3 = otilink_cdb_len(&d, rc10, 10, cap, 8, 0, &r);
        uint32_t last_lba = ((uint32_t)cap[0] << 24) | ((uint32_t)cap[1] << 16) |
                            ((uint32_t)cap[2] << 8) | cap[3];
        uint32_t bs = ((uint32_t)cap[4] << 24) | ((uint32_t)cap[5] << 16) |
                      ((uint32_t)cap[6] << 8) | cap[7];
        printf("  [%s] READ CAPACITY rc=%d 容量=%u 块 × %u 字节\n",
               (rc3 == 0) ? "PASS" : "FAIL", rc3, last_lba + 1, bs);
        fails += (rc3 != 0);

        /* 4) 厂商命令的错误路径（本设备当然不认 0xD9/0x2A）*/
        uint8_t vcdb[16] = {0};
        vcdb[0] = 0xD9; vcdb[1] = 0x2A; vcdb[2] = 0xFF; vcdb[14] = 'O'; vcdb[15] = 'T';
        memset(buf, 0, OTI_FRAME_SIZE);
        int rc4 = otilink_cdb_len(&d, vcdb, 16, buf, OTI_FRAME_SIZE, 1, &r);
        int key = r.sense_len > 2 ? (r.sense[2] & 0x0f) : -1;
        int asc = r.sense_len > 12 ? r.sense[12] : -1;
        int ascq = r.sense_len > 13 ? r.sense[13] : -1;
        printf("  [%s] 厂商 0xD9/0x2A 被拒 rc=0x%x status=0x%02x sense: key=0x%x asc=0x%x ascq=0x%x\n",
               (rc4 != 0) ? "PASS" : "FAIL", rc4, r.status, key, asc, ascq);
        fails += (rc4 == 0);

        /* 5) 64KB 双向：写入再读回逐字节比对（LBA 从 2048 开始，避开分区表） */
        uint32_t bs512 = bs ? bs : 512;
        uint32_t lba = 2048;
        uint16_t blocks = (uint16_t)(OTI_FRAME_SIZE / bs512);
        for (unsigned i = 0; i < OTI_FRAME_SIZE; i++)
            buf[i] = (uint8_t)(i * 7 + 1);
        uint8_t w10[10] = {0x2A, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        w10[2] = (uint8_t)(lba >> 24); w10[3] = (uint8_t)(lba >> 16);
        w10[4] = (uint8_t)(lba >> 8);  w10[5] = (uint8_t)lba;
        w10[7] = (uint8_t)(blocks >> 8); w10[8] = (uint8_t)blocks;
        int rc5 = otilink_cdb_len(&d, w10, 10, buf, OTI_FRAME_SIZE, 1, &r);
        printf("  [%s] WRITE(10) %u 块（%u 字节）rc=%d resid=%d\n",
               (rc5 == 0 && r.resid == 0) ? "PASS" : "FAIL", blocks, OTI_FRAME_SIZE, rc5, r.resid);
        fails += !(rc5 == 0 && r.resid == 0);

        static uint8_t back[OTI_FRAME_SIZE];
        uint8_t r10[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        memcpy(r10 + 2, w10 + 2, 4);
        r10[7] = w10[7];
        r10[8] = w10[8];
        memset(back, 0, sizeof(back));
        int rc6 = otilink_cdb_len(&d, r10, 10, back, OTI_FRAME_SIZE, 0, &r);
        int same = (rc6 == 0 && memcmp(buf, back, OTI_FRAME_SIZE) == 0);
        printf("  [%s] READ(10) 回读 %u 字节并逐字节比对一致（rc=%d resid=%d）\n",
               same ? "PASS" : "FAIL", OTI_FRAME_SIZE, rc6, r.resid);
        fails += !same;

        printf("  => %s（%d 项失败）\n", fails ? "有失败" : "SG_IO 层全部通过", fails);
        return fails ? 1 : 0;
    } else if (strcmp(cmd, "mode") == 0) {
        uint8_t mode = 0;
        rc = otilink_dev_mode_get(&d, &mode);
        printf("设备模式读: rc=%d mode=%u\n", rc, mode);
        if (nrest > 0) {
            uint8_t want = (uint8_t)strtoul(rest[0], NULL, 0);
            rc = otilink_dev_mode_set(&d, want);
            printf("设备模式写 %u: rc=%d\n", want, rc);
        }
    } else if (strcmp(cmd, "rstatus") == 0) {
        unsigned v2 = nrest > 0 ? (unsigned)strtoul(rest[0], NULL, 0) : 1;
        unsigned v4 = nrest > 1 ? (unsigned)strtoul(rest[1], NULL, 0) : 2;
        unsigned pl = nrest > 2 ? (unsigned)strtoul(rest[2], NULL, 0) : 0x1234;
        rc = otilink_set_remote_status(&d, (uint8_t)v2, (uint8_t)v4, (uint16_t)pl);
        printf("远端主机状态写（0xD8/0x01，CDB[2]=%u CDB[4]=%u payload=0x%04x）返回 %d\n",
               v2, v4, pl, rc);
    } else if (strcmp(cmd, "dummy") == 0) {
        rc = otilink_send_dummy(&d);
        printf("全零帧（dummy）发送返回 %d\n", rc);
    } else if (strcmp(cmd, "wtest") == 0) {
        const char *msg = nrest > 0 ? rest[0] : "otilink-wtest";
        rc = otilink_send_frame(&d, msg, strlen(msg));
        printf("发送数据帧 %zu 字节，返回 %d\n", strlen(msg), rc);
    } else if (strcmp(cmd, "rdump") == 0) {
        /* 采样：把收到的原始 64KB 帧逐字节存盘。
           用途 1：标定期确认设备是否开始回数据；
           用途 2：若对端跑的是厂商软件，可直接拿到厂商 20 字节头的真实内容。 */
        const char *file = nrest > 0 ? rest[0] : "/tmp/otiframes.bin";
        int want = (nrest > 1 && rest[1][0] != '-') ? atoi(rest[1]) : 3;
        int all = 0;
        for (int i = 0; i < nrest; i++)
            if (strcmp(rest[i], "--all") == 0)
                all = 1;
        FILE *out = fopen(file, "wb");
        if (!out) {
            fprintf(stderr, "打不开 %s: %s\n", file, strerror(errno));
            return 1;
        }
        static uint8_t frame[OTI_FRAME_SIZE];
        int kept = 0;
        printf("采样 %d 次到 %s（%s空闲帧）\n", want, file, all ? "包含" : "跳过");
        for (int i = 0; i < want; i++) {
            memset(frame, 0, sizeof(frame));
            int rc = otilink_recv_frame(&d, frame);
            int chk = rc ? -2 : otilink_frame_check(frame);
            printf("  #%d rc=%d 判定=%s  前 32 字节: ", i + 1, rc,
                   chk == 1 ? "有效" : chk == 0 ? "空闲" : chk == -1 ? "非法" : "未收到");
            for (int k = 0; k < 32; k++)
                printf("%02x", frame[k]);
            printf("\n");
            if (!rc && (all || chk == 1)) {
                fwrite(frame, 1, OTI_FRAME_SIZE, out);
                kept++;
            }
            if (rc)
                break;
        }
        fclose(out);
        printf("已写入 %d 帧（每帧 %d 字节）到 %s\n", kept, OTI_FRAME_SIZE, file);
        return 0;
    } else if (strcmp(cmd, "rtest") == 0 || strcmp(cmd, "rloop") == 0) {
        int n = (strcmp(cmd, "rloop") == 0 && nrest > 0) ? atoi(rest[0]) : 1;
        static uint8_t frame[OTI_FRAME_SIZE];
        for (int i = 0; i < n; i++) {
            memset(frame, 0, sizeof(frame));
            rc = otilink_recv_frame(&d, frame);
            int chk = rc ? -2 : otilink_frame_check(frame);
            printf("第 %d 帧: scsi_rc=%d 判定=%s", i + 1, rc,
                   chk == 1 ? "有效" : chk == 0 ? "空闲" : chk == -1 ? "非法" : "未收到");
            if (chk == 1) {
                uint32_t l = 0;
                memcpy(&l, frame + 4, 4);
                printf(" 头=%c%c%c%c len=%u", frame[0], frame[1], frame[2], frame[3], l);
            }
            printf("\n");
            if (chk == 1)
                hexdump(frame + OTI_FRAME_HEAD, 64);
        }
    } else {
        fprintf(stderr, "未知命令 %s\n", cmd);
        rc = 2;
    }
    otilink_close(&d);
    return rc ? 1 : 0;
}
