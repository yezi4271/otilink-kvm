/*
 * otilink.h —— OTi WinDroid Linker / VirtualLink 对拷线 (0ea0:2213) 的 Linux 用户态库
 *
 * 逆向依据见 ../NOTES.md。核心事实：
 *   - 传输层是 SCSI 私有命令（16 字节 CDB，尾两字节 'O','T'）
 *   - 0xD9 = 写方向：CDB[1]=0x2A 数据管道 / 0x33,0x34,0x36 HID 维护包(type 1/2/3)
 *   - 0xD8 = 读方向：CDB[2]=3, CDB[3]/[4] 为 booking 计数
 *   - 0xF0 = 查询/锁：CDB[1]=0x00 状态查询, 0x31 独占锁
 *   - 数据帧固定 65536 字节：20 字节头 + 65496 字节载荷 + 20 字节头副本
 *     头全零 = 空闲帧（对端无数据）
 */
#ifndef OTILINK_H
#define OTILINK_H

#include <stddef.h>
#include <stdint.h>

#define OTILINK_VID 0x0ea0
#define OTILINK_PID 0x2213

/* SCSI 私有 opcode */
#define OTI_OP_WRITE 0xD9
#define OTI_OP_READ  0xD8
#define OTI_OP_QUERY 0xF0

/* 0xD9 的子命令 */
#define OTI_SUB_DATA       0x2A  /* 帧管道**写**（PutData @0x5386）；受授权门控，见 §34 */
#define OTI_SUB_FRAME_RD   0x28  /* 帧管道**读**（GetData @0x57bc），CDB[2]=0x64 —— 真正的 64KB 帧读！ */
#define OTI_SUB_FRAME_RD_P 0x64  /* 0xD9/0x28 的固定参数 */
#define OTI_SUB_HID_TYPE1  0x33  /* HID 包 type=1 = **鼠标**（真机标定，见 §41） */
#define OTI_SUB_HID_TYPE2  0x34  /* HID 包 type=2 = **键盘** */
#define OTI_SUB_HID_TYPE3  0x36  /* HID 包 type=3 = 多媒体（未标定） */

/* 语义化别名：线缆会把 HID 包变成接收端**真实的 USB 鼠标/键盘**，接收端零软件 */
#define OTI_HID_TYPE_MOUSE 1
#define OTI_HID_TYPE_KBD   2
#define OTI_HID_TYPE_MM    3

/* 控制权令牌：键盘上的 F24（USB HID usage 0x73）。
   任一侧改变控制状态时发给对端，对端据此回到"被动"（不再吞键鼠）。
   F24 没有任何程序使用，Windows 映射为 VK_F24，安全。 */
#define OTI_HID_TOKEN_PASSIVE 0x73

/* 0xF0 的子命令 */
#define OTI_SUB_STATUS 0x00
#define OTI_SUB_LOCK   0x31
#define OTI_SUB_USB_RESTART 0x05   /* 0xF0/0x05/0x02, CDB[4]=0x0A（USBRestart @0x782f） */
#define OTI_SUB_DEV_MODE   0x60   /* 0xD9/0x60：设备模式读/写（Get/SetDeviceMode @0x74a4/@0x7410） */
#define OTI_SUB_REMOTE_ST  0x01   /* 0xD8/0x01：设置远端主机状态（SetRemoteHostStatus @0x52fc） */

/* 读方向 CDB[2] 是"通道/选择子"：3 = 数据管道，2 = 物理总线类型，0 = IC 版本 */
#define OTI_RD_CH_DATA   0x03
#define OTI_RD_CH_BUSTYPE 0x02
#define OTI_RD_CH_ICVER  0x00

#define OTI_FRAME_SIZE   65536        /* 固定帧长 */
#define OTI_FRAME_HEAD   20           /* 帧头长度（首尾各一份） */
#define OTI_FRAME_BODY   (OTI_FRAME_SIZE - 2 * OTI_FRAME_HEAD) /* 65496 */
#define OTI_HID_PKT_LEN  14           /* 厂商传给 _OTi_SendHIDPacket 的缓冲长度 */
#define OTI_HID_CDB_COPY 12           /* 其中被拷进 CDB[2..13] 的字节数 */

/* 本库自定义的帧头（芯片对流经的字节应当透明；若实测被拒再改成厂商语义） */
#define OTI_HDR_MAGIC0 'O'
#define OTI_HDR_MAGIC1 'T'
#define OTI_HDR_MAGIC2 'I'
#define OTI_HDR_MAGIC3 'L'

/* ---- SCSI 后端注入点（仿真设备/测试用） ---- */
struct oti_scsi_result;   /* 前向声明，供 ops 的函数指针使用 */

struct oti_scsi_ops {
    int (*exec)(void *ctx, const uint8_t cdb[16], void *data, unsigned len,
                int to_dev, struct oti_scsi_result *res);
    void *ctx;
};

struct otilink_dev {
    char sg_path[64];        /* /dev/sgN */
    int  sg;                 /* 打开的 fd，-1 = 未打开 */
    char sysfs[512];         /* sysfs 路径（调试用） */
    struct oti_scsi_ops ops; /* 非空则替代 SG_IO（测试注入） */
};

/* SCSI 执行详情（标定需要 status/resid/sense） */
struct oti_scsi_result {
    int     rc;              /* 0 = 成功 */
    int     status;          /* SCSI 状态字节 */
    int     host_status;
    int     driver_status;
    int     resid;           /* 未传输字节数 */
    int     sense_len;
    uint8_t sense[32];
};

void otilink_set_ops(struct otilink_dev *d, const struct oti_scsi_ops *ops);

/* 发现设备：把属于 0ea0:2213 的 /dev/sgN 填进 out，返回个数（每个 LUN 一个） */
int  otilink_find(char out[][64], int max);

int  otilink_open(struct otilink_dev *d, const char *sg_path);
/* 判断某 sysfs 路径（或其后代）是否属于本对拷线（0ea0:2213） */
int  otilink_is_our_usb(const char *sysfs_path);

/* 该 sysfs 路径向上是否有 USB 设备（可热插拔判定；只看 idVendor 存在与否） */
int otilink_is_usb_dev(const char *sysfs_path);
void otilink_close(struct otilink_dev *d);


/* 原始 CDB（16 字节），data 可为 NULL。返回 0 成功，否则 -errno / 打包的 SCSI 状态 */
int  otilink_cdb(struct otilink_dev *d, const uint8_t cdb[16], void *data,
                 unsigned len, int to_dev, uint8_t *sense, int sense_len);
/* 同 otilink_cdb，但回填完整结果，供 probe sweep 使用 */
int  otilink_cdb_ex(struct otilink_dev *d, const uint8_t cdb[16], void *data,
                    unsigned len, int to_dev, struct oti_scsi_result *res);
/* 允许自定义 CDB 长度（标准命令 6/10 字节；厂商命令用 16） */
int  otilink_cdb_len(struct otilink_dev *d, const uint8_t *cdb, unsigned cdb_len,
                     void *data, unsigned len, int to_dev, struct oti_scsi_result *res);

/* 便捷封装 */
int  otilink_send_hid(struct otilink_dev *d, int type, const uint8_t pkt[OTI_HID_PKT_LEN]);
int  otilink_hid_release_all(struct otilink_dev *d);
/* 发一个"控制权状态改变"令牌（见 otilink.c 注释）。usage: F24 = 0x73 */
int  otilink_hid_send_token(struct otilink_dev *d, uint8_t usage);
/* 按厂商线格式发一帧 XML（自带授权时序）。timeout_ms 内拿不到授权返回 -1。 */
int  otilink_send_vendor_xml(struct otilink_dev *d, const char *xml, size_t len, int timeout_ms);   /* type=1 + 全零（厂商的“清键”包） */
int  otilink_query(struct otilink_dev *d, uint8_t sub, void *out, unsigned outlen);
int  otilink_lock(struct otilink_dev *d, int lock);

/* 线缆设备 I/O 串行锁：同一条私有 CDB 通道上，HID 包写（捕获线程）若插进 pump 的
 * "读发送授权(0x06/0x07) → **立刻**写帧"窗口，授权会失效 → 写命令返回 CHECK_CONDITION、
 * 帧大量丢失（真机 rc=524546 / DID_NO_CONNECT；拓扑 B 特有：麒麟同时发 HID 包与帧）。 */
void otilink_cable_io_lock(void);
void otilink_cable_io_unlock(void);
/* 0xF0 控制读：CDB[1]=sub, CDB[2]=selector（IC 版本 / 物理总线类型） */
int  otilink_ctrl_read(struct otilink_dev *d, uint8_t sub, uint8_t selector,
                       void *out, unsigned outlen);
int  otilink_info_read(struct otilink_dev *d, void *out, unsigned outlen); /* 0xF0/0x00/0x00 设备信息块（≤64B） */
int  otilink_ic_version(struct otilink_dev *d, void *out12);        /* 设备信息块前 12 字节 */
/* 设备模式：0xD9/0x60（厂商 GetDeviceMode/SetDeviceMode）。语义未定，真机可读出来看 */
int  otilink_dev_mode_get(struct otilink_dev *d, uint8_t *mode);
int  otilink_dev_mode_set(struct otilink_dev *d, uint8_t mode);
int  otilink_bus_type(struct otilink_dev *d, void *out);            /* 0xF0/0x00/0x02 */
int  otilink_usb_restart(struct otilink_dev *d);                    /* 0xF0/0x05/0x02 */
/* 设置远端主机状态：0xD8/0x01（CDB[2]/CDB[4] 携带状态字节，附带 2 字节载荷）。
 * 注意读取端（GetRemoteHostStatus @0x5278）用的是**数据管道读通道**（0xD8/0x03），
 * 即状态是随帧带回的，所以读侧不需要额外实现。 */
int  otilink_set_remote_status(struct otilink_dev *d, uint8_t v2, uint8_t v4, uint16_t payload);
/* 厂商的 SendDummyData：写一个 64KB 全零帧（对端视为空闲帧，可作保活/冲刷） */
int  otilink_send_dummy(struct otilink_dev *d);

/* 数据帧 */
void otilink_frame_pack(const void *body, size_t len, uint8_t frame[OTI_FRAME_SIZE]);
int  otilink_frame_check(const uint8_t frame[OTI_FRAME_SIZE]);  /* 1=有效数据 0=空闲 -1=非法 */

/* 消息管道：0xD8/0x00/0x03 + **IN 16 字节**（厂商 ProcessIdleState @0x4e30）。
 * 注意：这**不是**帧读取！以前把它当 64KB 帧读是错的（设备只回 16 字节）。
 * `pending_tx` 写进 CDB[3..4]（16 位大端）= **本机待发送帧数**（min(队列长度, 100)），
 * 不是"已持有接收帧数"。设备据此决定是否回 0x07（发送授权）。
 * 返回 0 成功，m16[0] 为消息类型：
 *   0x05 = 对端有数据，be16(m16[1..2]) = 帧数 → 紧跟 otilink_recv_frame()
 *   0x06/0x07 = **发送授权**（0x07 时授权数 = 本机上报的待发帧数）
 *               → 必须**立刻** otilink_send_frame()，中间不要插别的操作
 *   0x01/0x00/0x08/0x10 = 复位（清收发队列），通常是链路未就绪/对端未消费 */
int  otilink_msg_read(struct otilink_dev *d, uint8_t m16[16], uint16_t pending_tx);

/* 帧写：0xD9/0x2A/0xFF + 64KB OUT。**必须在收到 0x06/0x07 授权后立刻调用**，
 * 否则设备返回 key=0x9 ASC=0x81/0x85/0x86（厂商语义：Tx_No_image / Image_Conflict）。 */
int  otilink_send_frame(struct otilink_dev *d, const void *body, size_t len);

/* 帧读：0xD9/0x28/0x64 + **64KB IN**（厂商 GetData @0x57bc）——真正的帧读取。
 * 只应在 otilink_msg_read() 返回 0x05 之后调用。 */
int  otilink_recv_frame(struct otilink_dev *d, uint8_t frame[OTI_FRAME_SIZE]);

/* 绿色包以 root 运行（一次提权）时，收到的文件/目录属主会变成 root，
   桌面用户读不了（实测：/tmp/otilink-files-<pid> 是 0700 root）。
   这个助手在 root 且存在 SUDO_UID 时把 path chown 回桌面用户；否则什么都不做。
   返回 0 = 已交还 / 无需交还，-1 = chown 失败（调用方只记日志，不中断）。 */
int  oti_adopt_path(const char *path);

#endif /* OTILINK_H */
