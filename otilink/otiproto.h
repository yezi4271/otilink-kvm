/*
 * otiproto.h —— 我们自己的上层消息协议（跑在 OTi 数据管道的 65496 字节载荷里）
 *
 * 设计取舍：Linux↔Linux 两端都是我们的代码，因此**不复刻厂商的 XML/UPipe**，
 * 用定长头 + 二进制载荷，简单、可校验、易实时。
 *
 * 消息 = 20 字节头 + 载荷（小端）
 *   magic u32 = 'OTL1'   type u16   flags u16
 *   seq   u32            len   u32   crc32 u32
 * crc 覆盖 头[0..15] + 载荷。
 */
#ifndef OTIPROTO_H
#define OTIPROTO_H

#include <stddef.h>
#include <stdint.h>

/* 一帧可携带的最大字节数：OTI_FRAME_BODY = 65536 - 2*20 */
#define OTI_BODY_MAX 65496
/* 单条消息载荷上限 = 帧体 - 消息头（20） */
#define OTI_PAYLOAD_MAX (OTI_BODY_MAX - 20)
/* 剪贴板分块留出余量后的单块上限 */
#define OTI_CLIP_CHUNK_MAX 65000

#define OTI_MSG_MAGIC 0x314C544Fu /* "OTL1" */

enum oti_msg_type {
    OTI_MSG_KEY    = 1,
    OTI_MSG_MOUSE  = 2,
    OTI_MSG_SWITCH = 3,   /* 控制权交接 */
    OTI_MSG_CLIP   = 4,   /* 剪贴板数据块 */
    OTI_MSG_PING   = 5,   /* 保活/延迟测量 */
    OTI_MSG_ACK    = 6,
    OTI_MSG_MOUSE_ABS = 7,   /* 鼠标**绝对**坐标（被控端用 MOUSEEVENTF_ABSOLUTE，
                                绕开系统指针加速，做到 1:1 手感） */
    OTI_MSG_HELLO  = 8,   /* 被控端上报自己的屏幕几何（宽高），供主控端换算绝对坐标 */
    OTI_MSG_MOUSE_MODE = 9,  /* 被控端声明它希望怎么收鼠标：
                                0 = 绝对坐标（1:1 跟手，不受系统加速影响）
                                1 = 相对位移（走系统**指针速度/加速**，即 Windows 原生手感） */
    /* 角色协商（第 46 轮新增）：解决"键鼠插哪边都行"。
       为什么需要它：原来角色是**两侧各自单方面判定**（麒麟看 by-id 有没有本机键鼠；
       Windows 开机就无条件起 otiagent2 --edge right）——用户把键鼠换到另一台后，
       **两侧同时成为主控**，抢同一条 HID/帧管道：实测帧管道双向全丢、剪贴板两边都
       "未确认"，只能人工停一侧。有了 ROLE 之后：谁的本机键鼠在用谁当主控，
       另一侧主动让位，且**任何时刻只有一个 grab**（见 otikm.c 的不变式 I1-I5）。 */
    OTI_MSG_ROLE   = 10,
};

/* ROLE 消息的三个枚举值 */
enum oti_role_want  { OTI_ROLE_AUTO = 0, OTI_ROLE_FORCE_MASTER = 1, OTI_ROLE_FORCE_SLAVE = 2 };
enum oti_role_state { OTI_ROLE_NONE = 0, OTI_ROLE_MASTER = 1, OTI_ROLE_SLAVE = 2 };

/* 角色协商载荷（12 字节，小端） */
typedef struct {
    uint8_t  has_local_input;  /* 1 = 本机有"线缆之外"的键鼠（离线缆 HID 判定） */
    uint8_t  want;             /* OTI_ROLE_AUTO / FORCE_MASTER / FORCE_SLAVE */
    uint8_t  state;            /* OTI_ROLE_NONE / MASTER / SLAVE（我当前的角色） */
    uint8_t  flags;            /* bit0 = 我正持有 EVIOCGRAB（可观测 + 冲突检测） */
    uint32_t boot_id;          /* 稳定仲裁：本机启动标识低 32 位（平票时小者优先） */
    uint32_t input_age_ms;     /* 本机最近一次真实键鼠事件距今毫秒（越小 = 刚用过这台） */
} oti_role_evt;

#define OTI_ROLE_FLAG_GRABBED 0x01

/* OTI_MSG_MOUSE_MODE 的 mode 取值 */
enum oti_mouse_mode { OTI_MOUSE_ABS = 0, OTI_MOUSE_NATIVE = 1 };

enum oti_key_value { OTI_KEY_RELEASE = 0, OTI_KEY_PRESS = 1, OTI_KEY_REPEAT = 2 };
enum oti_side      { OTI_SIDE_LOCAL  = 0, OTI_SIDE_REMOTE = 1 };

typedef struct {
    uint16_t code;    /* Linux evdev 码（KEY_* / BTN_*） */
    uint8_t  value;   /* OTI_KEY_* */
    uint8_t  mods;    /* 保留：修饰键快照 */
} oti_key_evt;

typedef struct {
    int16_t  dx, dy;  /* 相对位移 */
    int16_t  wheel;
    uint16_t buttons; /* bit0 左 / bit1 右 / bit2 中 */
} oti_mouse_evt;

typedef struct {
    uint8_t  side;            /* 谁拿到控制权（OTI_SIDE_*） */
    uint8_t  use_hotkey_only;
    int16_t  edge_x, edge_y;  /* 本机坐标下的穿越点 */
    uint16_t screen_w, screen_h;
} oti_switch_evt;

typedef struct {
    uint16_t format;     /* 1 = UTF-8 文本 */
    uint16_t flags;      /* bit0 = 首块；bit1 = 末块 */
    uint32_t fid;        /* 一次剪贴板传输的 id */
    uint32_t offset;
    uint32_t total_len;
} oti_clip_hdr;

#define OTI_CLIP_FIRST 0x1
#define OTI_CLIP_LAST  0x2

/* 剪贴板格式位（oti_clip_hdr.format）—— 两侧必须一致 */
#define OTI_CLIP_FMT_TEXT  1   /* UTF-8 文本 */
#define OTI_CLIP_FMT_PNG   2   /* PNG 图片 */
#define OTI_CLIP_FMT_FILES 3   /* 文件包（小文件，整包驻留内存） */
#define OTI_CLIP_FMT_PART  4   /* 大文件分片（见 otixfer.c，流式落盘） */
#define OTI_CLIP_FMT_CTRL  5   /* 大文件传输控制（DONE / VERDICT） */

/* 绝对坐标鼠标事件：x,y 是**对端屏幕**坐标系下的像素位置 */
typedef struct {
    int16_t  x, y;
    int16_t  wheel;
    uint16_t buttons;
} oti_mouse_abs_evt;

/* 几何握手：发送方屏幕尺寸（像素） */
typedef struct {
    uint16_t width, height;
} oti_hello_evt;

typedef struct {
    uint32_t magic;
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
    uint32_t len;     /* 载荷长度 */
    uint32_t crc;
} oti_msg_hdr;

#define OTI_HDR_SIZE 20

uint32_t oti_crc32(const void *data, size_t len);
/* 增量 CRC：crc 为前一段的终值（即 oti_crc32 的结果），继续追加 data */
uint32_t oti_crc32_append(uint32_t crc, const void *data, size_t len);

/* 组包：返回总长（头+载荷），失败返回 0 */
size_t oti_encode(uint16_t type, uint16_t flags, uint32_t seq,
                  const void *payload, uint32_t len, uint8_t *out, size_t outcap);

/* 解包：成功返回 0 并填 hdr/payload（payload 指向 buf 内部，不做拷贝） */
int oti_decode(const uint8_t *buf, size_t len, oti_msg_hdr *hdr, const uint8_t **payload);

/* 便捷封装（返回总长，0 = 失败） */
size_t oti_encode_key(uint32_t seq, const oti_key_evt *e, uint8_t *out, size_t cap);
size_t oti_encode_mouse(uint32_t seq, const oti_mouse_evt *e, uint8_t *out, size_t cap);
size_t oti_encode_switch(uint32_t seq, const oti_switch_evt *e, uint8_t *out, size_t cap);
size_t oti_encode_clip(uint32_t seq, const oti_clip_hdr *h, const void *data,
                       uint32_t dlen, uint8_t *out, size_t cap);
size_t oti_encode_ping(uint32_t seq, uint32_t stamp_ms, uint8_t *out, size_t cap);
/* 剪贴板确认（type=6）：载荷 = u32 crc + u16 format + u16 rsvd。
   为什么要它：帧通道**会静默丢帧**（设备打嗝/帧被跳过，两侧日志都不报错）——
   实测麒麟→Windows 的剪贴板连发 15 条丢 6 条（40%），而反向 12/12 全到。
   没有确认就没有重传，用户看到的就是"复制粘贴不了"。 */
size_t oti_encode_ack(uint32_t seq, uint32_t crc, uint16_t format, uint8_t *out, size_t cap);
int oti_decode_ack(const uint8_t *payload, uint32_t len, uint32_t *crc, uint16_t *format);
size_t oti_encode_mouse_abs(uint32_t seq, const oti_mouse_abs_evt *e, uint8_t *out, size_t cap);
size_t oti_encode_hello(uint32_t seq, const oti_hello_evt *e, uint8_t *out, size_t cap);
size_t oti_encode_mouse_mode(uint32_t seq, uint8_t mode, uint8_t *out, size_t cap);

int oti_decode_key(const uint8_t *payload, uint32_t len, oti_key_evt *out);
int oti_decode_mouse(const uint8_t *payload, uint32_t len, oti_mouse_evt *out);
int oti_decode_switch(const uint8_t *payload, uint32_t len, oti_switch_evt *out);
int oti_decode_clip(const uint8_t *payload, uint32_t len, oti_clip_hdr *h,
                    const uint8_t **data, uint32_t *dlen);
int oti_decode_mouse_abs(const uint8_t *payload, uint32_t len, oti_mouse_abs_evt *out);
int oti_decode_hello(const uint8_t *payload, uint32_t len, oti_hello_evt *out);
int oti_decode_mouse_mode(const uint8_t *payload, uint32_t len, uint8_t *mode_out);

/* 角色协商（type=10）：载荷 = 上表 12 字节 */
size_t oti_encode_role(uint32_t seq, const oti_role_evt *e, uint8_t *out, size_t cap);
int    oti_decode_role(const uint8_t *payload, uint32_t len, oti_role_evt *out);

#endif /* OTIPROTO_H */
