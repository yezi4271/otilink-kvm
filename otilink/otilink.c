/*
 * otilink.c —— 见 otilink.h。Linux 侧实现：/dev/sgN + SG_IO。
 */
#define _GNU_SOURCE
#include "otilink.h"

#include <pthread.h>

/* 进程级设备 I/O 锁（一个进程只操作一台线缆）。用静态锁而不是塞进 struct otilink_dev：
   dev 是 mmap 出来的 transport 的一部分，在 mmap 上初始化 pthread_mutex 容易踩未初始化/
   重打开的坑。见 otilink.h 的用法说明。 */
static pthread_mutex_t g_cable_io = PTHREAD_MUTEX_INITIALIZER;
void otilink_cable_io_lock(void)   { pthread_mutex_lock(&g_cable_io); }
void otilink_cable_io_unlock(void) { pthread_mutex_unlock(&g_cable_io); }

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int read_hex_file(const char *path, unsigned *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = (fscanf(f, "%x", out) == 1);
    fclose(f);
    return ok ? 0 : -1;
}

static int usb_ids_match(const char *realpath_buf)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", realpath_buf);
    for (int depth = 0; depth < 12; depth++) {
        char p[PATH_MAX + 16];
        unsigned vid = 0, pid = 0;
        snprintf(p, sizeof(p), "%s/idVendor", dir);
        if (read_hex_file(p, &vid) == 0) {
            snprintf(p, sizeof(p), "%s/idProduct", dir);
            if (read_hex_file(p, &pid) == 0 && vid == OTILINK_VID && pid == OTILINK_PID)
                return 1;
        }
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir)
            break;
        *slash = '\0';
    }
    return 0;
}

/* 该 sysfs 路径向上是否有 USB 设备（只看有没有 idVendor，不认 VID/PID）。
   用途：判断一个输入设备是不是**可热插拔**的 USB 设备 —— 台式机 i8042 会凭空造一个
   "AT Raw Set 2 keyboard"，PS/2 键盘也不会"插到另一台机器上"，所以角色仲裁只认 USB。 */
int otilink_is_usb_dev(const char *sysfs_path)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", sysfs_path);
    for (int depth = 0; depth < 12; depth++) {
        char p[PATH_MAX + 16];
        unsigned vid = 0;
        snprintf(p, sizeof(p), "%s/idVendor", dir);
        if (read_hex_file(p, &vid) == 0)
            return 1;
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir)
            break;
        *slash = '\0';
    }
    return 0;
}

int otilink_is_our_usb(const char *sysfs_path)
{
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", sysfs_path);
    return usb_ids_match(buf);
}

int otilink_find(char out[][64], int max)
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
        if (!usb_ids_match(real))
            continue;
        snprintf(out[n], 64, "/dev/%.32s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

int oti_adopt_path(const char *path)
{
    if (!path || geteuid() != 0)
        return 0;                            /* 非 root：文件本来就是用户的 */
    const char *us = getenv("SUDO_UID");
    const char *gs = getenv("SUDO_GID");
    if (!us || !*us)
        return 0;                            /* 不是经 sudo 起的，不动 */
    uid_t uid = (uid_t)strtoul(us, NULL, 10);
    gid_t gid = (gs && *gs) ? (gid_t)strtoul(gs, NULL, 10) : (gid_t)uid;
    if (chown(path, uid, gid) == 0)
        return 0;
    return -1;
}

int otilink_open(struct otilink_dev *d, const char *sg_path)
{
    memset(d, 0, sizeof(*d));
    d->sg = -1;
    snprintf(d->sg_path, sizeof(d->sg_path), "%s", sg_path);
    d->sg = open(sg_path, O_RDWR);
    if (d->sg < 0)
        return -errno;
    return 0;
}

void otilink_close(struct otilink_dev *d)
{
    if (d->sg >= 0)
        close(d->sg);
    d->sg = -1;
}

void otilink_set_ops(struct otilink_dev *d, const struct oti_scsi_ops *ops)
{
    if (ops)
        d->ops = *ops;
    else
        memset(&d->ops, 0, sizeof(d->ops));
}

int otilink_cdb_len(struct otilink_dev *d, const uint8_t *cdb, unsigned cdb_len,
                    void *data, unsigned len, int to_dev, struct oti_scsi_result *res)
{
    if (d->ops.exec)                      /* 注入的后端（仿真设备/测试） */
        return d->ops.exec(d->ops.ctx, cdb, data, len, to_dev, res);
    (void)cdb_len;
    if (d->sg < 0)
        return -EBADF;
    struct sg_io_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.interface_id = 'S';
    hdr.cmdp = (unsigned char *)cdb;
    hdr.cmd_len = cdb_len;
    hdr.dxferp = data;
    hdr.dxfer_len = len;
    hdr.dxfer_direction = (len == 0) ? SG_DXFER_NONE
                                     : (to_dev ? SG_DXFER_TO_DEV : SG_DXFER_FROM_DEV);
    uint8_t sense[32];
    memset(sense, 0, sizeof(sense));
    hdr.sbp = sense;
    hdr.mx_sb_len = sizeof(sense);
    hdr.timeout = 5000;
    int io = ioctl(d->sg, SG_IO, &hdr);
    if (res) {
        memset(res, 0, sizeof(*res));
        res->status = hdr.status;
        res->host_status = hdr.host_status;
        res->driver_status = hdr.driver_status;
        res->resid = (int)hdr.resid;
        res->sense_len = hdr.sb_len_wr;
        memcpy(res->sense, sense, sizeof(res->sense));
        res->rc = (io < 0) ? -errno
                 : (hdr.status || hdr.host_status || hdr.driver_status)
                       ? (0x100 | (hdr.status & 0xff) | (hdr.host_status << 8) |
                          (hdr.driver_status << 16))
                       : 0;
    }
    if (io < 0)
        return -errno;
    if (hdr.status || hdr.host_status || hdr.driver_status)
        return 0x100 | (hdr.status & 0xff) | (hdr.host_status << 8) | (hdr.driver_status << 16);
    return 0;
}

int otilink_cdb_ex(struct otilink_dev *d, const uint8_t cdb[16], void *data,
                   unsigned len, int to_dev, struct oti_scsi_result *res)
{
    return otilink_cdb_len(d, cdb, 16, data, len, to_dev, res);
}

int otilink_cdb(struct otilink_dev *d, const uint8_t cdb[16], void *data,
                unsigned len, int to_dev, uint8_t *sense, int sense_len)
{
    struct oti_scsi_result r;
    int rc = otilink_cdb_ex(d, cdb, data, len, to_dev, &r);
    if (sense && sense_len > 0) {
        int n = r.sense_len < sense_len ? r.sense_len : sense_len;
        if (n > 0)
            memcpy(sense, r.sense, (size_t)n);
    }
    return rc;
}

static void oti_cdb_tail(uint8_t cdb[16])
{
    cdb[14] = 'O';
    cdb[15] = 'T';
}

int otilink_send_hid(struct otilink_dev *d, int type, const uint8_t pkt[OTI_HID_PKT_LEN])
{
    uint8_t cdb[16] = {0}, sense[32];
    uint8_t sub;
    switch (type) {
    case 1: sub = OTI_SUB_HID_TYPE1; break;
    case 2: sub = OTI_SUB_HID_TYPE2; break;
    case 3: sub = OTI_SUB_HID_TYPE3; break;
    default: return -EINVAL;
    }
    cdb[0] = OTI_OP_WRITE;
    cdb[1] = sub;
    memcpy(&cdb[2], pkt, OTI_HID_CDB_COPY);   /* 厂商只把前 12 字节放进 CDB */
    oti_cdb_tail(cdb);
    /* 与 pump 的"读授权 → 立刻写帧"窗口互斥（否则授权会失效 → 帧写 CHECK_CONDITION） */
    otilink_cable_io_lock();
    int rc = otilink_cdb(d, cdb, NULL, 0, 1, sense, sizeof(sense));
    otilink_cable_io_unlock();
    return rc;
}

/*
 * 控制权令牌：用一个"没人用的正常按键"（F24，USB HID usage 0x73）当作
 * "我这边改变控制状态了，你回到被动"的信号。
 * 为什么要这个：两端都靠边缘检测切换，但被驱动的一侧无法凭自身判断
 * "对面已经放手了" —— 没有信号就会一直吞着键鼠（用户实测：鼠标回不去、
 * 键盘像失灵）。F24 在 Windows 上映射为 VK_F24，没有任何程序使用它，安全。
 */
/*
 * 按厂商线格式发一帧 XML 命令，并复现厂商要求的**授权时序**：
 *   先把"我有 1 帧要发"写进消息 CDB[3..4]，读消息；只有收到 0x06/0x07（授权）
 *   才能立刻写帧，中间不能插任何别的设备操作 —— 否则被拒（ASC=0x85）。
 * 帧体格式：[1 字节 0x39][u32 小端 负载长度][XML 文本]
 */
int otilink_send_vendor_xml(struct otilink_dev *d, const char *xml, size_t len, int timeout_ms)
{
    static uint8_t body[OTI_FRAME_BODY];
    if (len + 5 > sizeof(body))
        return -2;
    body[0] = 0x39;
    uint32_t l = (uint32_t)len;
    memcpy(body + 1, &l, 4);
    memcpy(body + 5, xml, len);

    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint8_t m[16];
        memset(m, 0, sizeof m);
        if (otilink_msg_read(d, m, 1) == 0 && (m[0] == 0x06 || m[0] == 0x07))
            return otilink_send_frame(d, body, len + 5);
        clock_gettime(CLOCK_MONOTONIC, &tn);
        long el = (tn.tv_sec - t0.tv_sec) * 1000 + (tn.tv_nsec - t0.tv_nsec) / 1000000;
        if (el > timeout_ms)
            return -1;
        usleep(2000);
    }
}

int otilink_hid_send_token(struct otilink_dev *d, uint8_t usage)
{
    uint8_t on[OTI_HID_PKT_LEN] = {0};
    uint8_t off[OTI_HID_PKT_LEN] = {0};
    on[2] = usage;                            /* 键盘报告: [mods][rsv][k1..k6] */
    /* 连发 3 轮：HID 包**会丢**（帧通道实测都能丢 40%），而"回程令牌丢了"的代价
       是用户的指针永远回不来 —— 必须冗余。F24 没有任何程序使用，重复按也安全。 */
    int last = 0;
    for (int i = 0; i < 3; i++) {
        int rc = otilink_send_hid(d, OTI_HID_TYPE_KBD, on);
        usleep(15 * 1000);
        int rc2 = otilink_send_hid(d, OTI_HID_TYPE_KBD, off);
        usleep(15 * 1000);
        last = rc ? rc : rc2;
    }
    return last;
}

int otilink_hid_release_all(struct otilink_dev *d)
{
    /* 切回本地时必须把对端"松手"：键盘（type2）与鼠标按键（type1）各发一个全零包，
       否则对端会卡住按下的键或按下的鼠标键。 */
    uint8_t z[OTI_HID_PKT_LEN] = {0};
    int rc1 = otilink_send_hid(d, OTI_HID_TYPE_KBD, z);
    int rc2 = otilink_send_hid(d, OTI_HID_TYPE_MOUSE, z);
    return rc1 ? rc1 : rc2;
}

int otilink_query(struct otilink_dev *d, uint8_t sub, void *out, unsigned outlen)
{
    uint8_t cdb[16] = {0}, sense[32];
    cdb[0] = OTI_OP_QUERY;
    cdb[1] = sub;
    oti_cdb_tail(cdb);
    return otilink_cdb(d, cdb, out, outlen, 0, sense, sizeof(sense));
}

int otilink_lock(struct otilink_dev *d, int lock)
{
    uint8_t cdb[16] = {0}, sense[32];
    cdb[0] = OTI_OP_QUERY;
    cdb[1] = OTI_SUB_LOCK;
    cdb[2] = lock ? 1 : 0;
    oti_cdb_tail(cdb);
    return otilink_cdb(d, cdb, NULL, 0, 1, sense, sizeof(sense));
}

int otilink_ctrl_read(struct otilink_dev *d, uint8_t sub, uint8_t selector,
                      void *out, unsigned outlen)
{
    uint8_t cdb[16] = {0}, sense[32];
    cdb[0] = OTI_OP_QUERY;
    cdb[1] = sub;
    cdb[2] = selector;
    oti_cdb_tail(cdb);
    return otilink_cdb(d, cdb, out, outlen, 0, sense, sizeof(sense));
}

int otilink_info_read(struct otilink_dev *d, void *out, unsigned outlen)
{
    return otilink_ctrl_read(d, 0x00, OTI_RD_CH_ICVER, out, outlen);
}

int otilink_ic_version(struct otilink_dev *d, void *out12)
{
    return otilink_info_read(d, out12, 12);
}

int otilink_dev_mode_get(struct otilink_dev *d, uint8_t *mode)
{
    uint8_t cdb[16] = {0}, sense[32], buf[16] = {0};
    cdb[0] = OTI_OP_WRITE;              /* 0xD9，但数据阶段是"读" */
    cdb[1] = OTI_SUB_DEV_MODE;
    cdb[2] = 2;                         /* 厂商 GetDeviceMode 用 CDB[2]=2 */
    oti_cdb_tail(cdb);
    int rc = otilink_cdb(d, cdb, buf, sizeof(buf), 0, sense, sizeof(sense));
    if (mode)
        *mode = buf[0];
    return rc;
}

int otilink_dev_mode_set(struct otilink_dev *d, uint8_t mode)
{
    uint8_t cdb[16] = {0}, sense[32], buf[1] = {mode};
    cdb[0] = OTI_OP_WRITE;
    cdb[1] = OTI_SUB_DEV_MODE;
    oti_cdb_tail(cdb);
    return otilink_cdb(d, cdb, buf, sizeof(buf), 1, sense, sizeof(sense));
}

int otilink_bus_type(struct otilink_dev *d, void *out)
{
    return otilink_ctrl_read(d, 0x00, OTI_RD_CH_BUSTYPE, out, 16);
}

int otilink_usb_restart(struct otilink_dev *d)
{
    uint8_t cdb[16] = {0}, sense[32];
    cdb[0] = OTI_OP_QUERY;
    cdb[1] = OTI_SUB_USB_RESTART;
    cdb[2] = 0x02;
    cdb[4] = 0x0A;
    oti_cdb_tail(cdb);
    return otilink_cdb(d, cdb, NULL, 0, 1, sense, sizeof(sense));
}

int otilink_set_remote_status(struct otilink_dev *d, uint8_t v2, uint8_t v4, uint16_t payload)
{
    uint8_t cdb[16] = {0}, sense[32], buf[2];
    cdb[0] = OTI_OP_READ;                 /* 0xD8，但数据阶段是"写" */
    cdb[1] = OTI_SUB_REMOTE_ST;
    cdb[2] = v2;
    cdb[4] = v4;
    oti_cdb_tail(cdb);
    buf[0] = (uint8_t)(payload & 0xff);
    buf[1] = (uint8_t)(payload >> 8);
    return otilink_cdb(d, cdb, buf, sizeof(buf), 1, sense, sizeof(sense));
}

int otilink_send_dummy(struct otilink_dev *d)
{
    /* 厂商 SendDummyData = SendData(NULL, true)：整帧 64KB 全零（**不带头**），
       与"空闲帧"逐字节相同 → 对端判为空闲并丢弃。因此这里不能用 frame_pack。 */
    static uint8_t zero[OTI_FRAME_SIZE];
    uint8_t cdb[16] = {0}, sense[32];
    cdb[0] = OTI_OP_WRITE;
    cdb[1] = OTI_SUB_DATA;
    cdb[2] = 0xFF;
    oti_cdb_tail(cdb);
    return otilink_cdb(d, cdb, zero, OTI_FRAME_SIZE, 1, sense, sizeof(sense));
}

void otilink_frame_pack(const void *body, size_t len, uint8_t frame[OTI_FRAME_SIZE])
{
    uint8_t hdr[OTI_FRAME_HEAD];
    memset(frame, 0, OTI_FRAME_SIZE);
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = OTI_HDR_MAGIC0;
    hdr[1] = OTI_HDR_MAGIC1;
    hdr[2] = OTI_HDR_MAGIC2;
    hdr[3] = OTI_HDR_MAGIC3;
    uint32_t l = (uint32_t)len;
    memcpy(&hdr[4], &l, 4);                 /* 载荷长度（本库自定义） */
    memcpy(frame, hdr, sizeof(hdr));                     /* 头 */
    if (body && len) {
        if (len > OTI_FRAME_BODY)
            len = OTI_FRAME_BODY;
        memcpy(frame + OTI_FRAME_HEAD, body, len);
    }
    memcpy(frame + OTI_FRAME_SIZE - OTI_FRAME_HEAD, hdr, sizeof(hdr)); /* 头副本 */
}

int otilink_frame_check(const uint8_t frame[OTI_FRAME_SIZE])
{
    static const uint8_t zero[OTI_FRAME_HEAD] = {0};
    if (memcmp(frame, zero, OTI_FRAME_HEAD) == 0)
        return 0;                            /* 空闲帧：对端无数据 */
    if (memcmp(frame, frame + OTI_FRAME_SIZE - OTI_FRAME_HEAD, OTI_FRAME_HEAD) != 0)
        return -1;                           /* 首尾头不一致 = 非法 */
    return 1;
}

int otilink_send_frame(struct otilink_dev *d, const void *body, size_t len)
{
    uint8_t cdb[16] = {0}, sense[32];
    static uint8_t frame[OTI_FRAME_SIZE];    /* 64KB，静态避免爆栈 */
    otilink_frame_pack(body, len, frame);
    cdb[0] = OTI_OP_WRITE;
    cdb[1] = OTI_SUB_DATA;
    cdb[2] = 0xFF;                           /* 厂商固定值 */
    oti_cdb_tail(cdb);
    return otilink_cdb(d, cdb, frame, OTI_FRAME_SIZE, 1, sense, sizeof(sense));
}

int otilink_msg_read(struct otilink_dev *d, uint8_t m16[16], uint16_t pending_tx)
{
    /* 消息管道：0xD8/0x00/0x03 + IN **16 字节**（厂商 ProcessIdleState @0x4e30）。
       CDB[3..4] = 本机待发送帧数（16 位大端），设备据此决定是否发放 0x07 发送授权。 */
    uint8_t cdb[16] = {0}, sense[32];
    cdb[0] = OTI_OP_READ;
    cdb[1] = 0x00;
    cdb[2] = OTI_RD_CH_DATA;
    cdb[3] = (uint8_t)(pending_tx >> 8);
    cdb[4] = (uint8_t)(pending_tx & 0xff);
    oti_cdb_tail(cdb);
    memset(m16, 0, 16);
    return otilink_cdb(d, cdb, m16, 16, 0, sense, sizeof(sense));
}

int otilink_recv_frame(struct otilink_dev *d, uint8_t frame[OTI_FRAME_SIZE])
{
    /* 帧读：0xD9/0x28/0x64 + IN 65536（厂商 GetData @0x57bc）。
       以前这里是 0xD8/0x00/0x03 —— 那是消息管道，设备只回 16 字节（真机实证，NOTES §33.1/§34）。 */
    uint8_t cdb[16] = {0}, sense[32];
    cdb[0] = OTI_OP_WRITE;
    cdb[1] = OTI_SUB_FRAME_RD;
    cdb[2] = OTI_SUB_FRAME_RD_P;
    oti_cdb_tail(cdb);
    return otilink_cdb(d, cdb, frame, OTI_FRAME_SIZE, 0, sense, sizeof(sense));
}
