/*
 * otitrans.h —— 传输抽象：同一套上层协议跑在「线缆」或「任意 fd」上
 *
 * 为什么抽象：线缆标定要等硬件，但协议/状态机必须现在就能验证。
 * fd 后端（socketpair / unix socket）让两端协议可以在本机做端到端测试。
 */
#ifndef OTITRANS_H
#define OTITRANS_H

#include <stddef.h>

#include "otilink.h"

typedef struct oti_transport oti_transport;

/* 线缆后端：sg_path = /dev/sgN（NULL 则自动发现） */
oti_transport *oti_tr_open_cable(const char *sg_path);
/* 线缆后端（已配置好的 dev，含注入的仿真后端）；dev 生命周期由调用者持有 */
oti_transport *oti_tr_open_cable_dev(struct otilink_dev *dev);
/* fd 后端：fd 需已连接（socketpair/AF_UNIX/TCP 均可）。owns=1 时 close 会关掉它 */
oti_transport *oti_tr_open_fd(int fd, int owns);

/* 发送一整个消息体（≤ OTI_BODY_MAX）。返回 0 成功（含"背景心跳被合并/丢弃"，
 * 见下方入队策略）。⚠️ 线缆后端**只入队，绝不在这里做设备 I/O**：
 * 所有设备 I/O 只在 RX 泵线程（oti_tr_recv 内部，5~10ms 级）与非输入线程的
 * oti_tr_send_wait 内联泵里发生 —— 捕获/输入线程调本函数绝不会被一次慢帧写（可达
 * 700ms）阻塞（2026-11-06 重设计，Lead 明确要求）。 */
int oti_tr_send(oti_transport *t, const void *body, size_t len);
/* 可靠发送（只对线缆后端有意义）：队列满时等待而不是"丢最旧"。
 * 大文件分片用它，避免高频丢片导致频繁重传。返回 0；-2 = 等待超时。
 * ⚠️ 会内联推一次设备泵：只允许非输入线程（剪贴板/大文件/ACK）调用。 */
int oti_tr_send_wait(oti_transport *t, const void *body, size_t len, int timeout_ms);
/* 接收一个消息体。timeout_ms < 0 = 阻塞等待；返回 0 成功、-2 超时、其它负值为错误 */
int oti_tr_recv(oti_transport *t, void *body, size_t *len, int timeout_ms);
/* 非阻塞推一次线缆设备泵（读消息/授权→发帧↔收帧）。给单测与诊断工具用：
   没有 RX 线程时用它驱动设备；fd 后端为空操作。返回 0/-2(无数据)/错误码。 */
int oti_tr_pump_once(oti_transport *t);

const char *oti_tr_name(const oti_transport *t);
/* 线缆后端的发送健康度（看门狗用）：
   pending    = 待发队列里还有多少条没送出去
   last_tx_ms = 最近一次**真正写出**成功的时刻（单调时钟，0=从未）
   看门狗判据：pending>0 且 now-last_tx_ms 超过阈值 → 对端没在消费 → 自动交还本机控制 */
int  oti_tr_cable_pending(oti_transport *t);
long oti_tr_cable_last_tx_ms(oti_transport *t);
/* 清空待发队列（交还控制权时用：陈旧位移再发出去只会让对端指针乱跳） */
void oti_tr_cable_drop_tx(oti_transport *t);
/* 线缆消息读的 SG_IO 超时（毫秒）。默认 5000；不随 HID 状态调整（见 2026-11-06 重设计）。 */
void oti_tr_set_msg_timeout(int ms);
/* 【已废弃 / 2026-11-06】原"HID 优先窗口"开关：HID 包正在写时让消息泵**不开始**新的读。
   这套逻辑已被证明有害：泵停 → CURSOR/ROLE 遥测饿死 → 交接/回程判据失灵（真机现场
   "鼠标回不来"）。现在消息泵持续跑（0xD8 本身 0~1ms），HID 与帧写之间的互斥只靠
   otilink_cable_io_lock 的"读授权→写帧"窗口保证。函数保留为空操作，避免旧调用点符号断。 */
void oti_tr_set_hid_busy(int busy);
/* 线缆后端底层设备（用于初始化握手；仅 cable 后端有效） */
struct otilink_dev *oti_tr_cable_dev(oti_transport *t);
void oti_tr_close(oti_transport *t);

/* ================= 慢操作守卫（2026-11-06 传输层重设计）=================
 * 真机实测（Lead，2026-11-06；见 /tmp/redesign-brief.md §1）：
 *   - 线缆是**单队列**设备（queue_depth=1）：一条阻塞的 SCSI 命令会把 HID 注入写堵住；
 *   - 0xD8 消息读 0~1ms（不阻塞）；0xD9/0x28 帧读有数据 ~2ms；
 *   - 0xD9/0x2A 帧写耗时完全取决于**对端消费速度**：对端快 2~4ms，旧慢代理 250~700ms。
 * 策略：单次帧写 > 30ms 判定"对端消费慢" → peer_slow_until = now + 退避窗口
 *   （300ms 起，连续慢指数退避 600/1200/2000ms 封顶；一次快写恢复即清零）。
 *   窗口内**不发起帧写**（队列保留、消息泵/HID 照常），避免反复把键鼠写堵住。
 *   帧读慢（>30ms）同样统计并对帧读退避（窗口独立），不影响 HID。
 * 统计/验证 API（只读快照；无设备或 fd 后端返回 0）。
 * 真机证据行（stderr，run-kylin.sh 会重定向进日志）：
 *   [warn] SLOWOP frame write Nms（对端消费慢，暂停帧发送 Wms）
 *   [warn] SLOWOP frame read Nms（对端/设备慢，暂停帧读 Wms）
 *   [info] SLOWOP 恢复：帧写 Nms（连续慢 K 次后清零）
 * 阈值默认 30/300/2000；单测可调小（见 oti_tr_set_slowop_policy）。 */
void oti_tr_set_slowop_policy(int threshold_ms, int base_ms, int max_ms);
unsigned long oti_tr_cable_slow_count(oti_transport *t);       /* 帧写慢事件累计 */
unsigned long oti_tr_cable_read_slow_count(oti_transport *t);  /* 帧读慢事件累计 */
unsigned long oti_tr_cable_deferred_writes(oti_transport *t);  /* 退避窗口内被推迟的帧写次数 */
unsigned long oti_tr_cable_bg_merged(oti_transport *t);        /* 背景心跳帧合并次数 */
unsigned long oti_tr_cable_bg_dropped(oti_transport *t);       /* 背景心跳帧丢弃次数 */
long oti_tr_cable_frame_write_max_ms(oti_transport *t);        /* 单次帧写最大耗时 */
long oti_tr_cable_frame_read_max_ms(oti_transport *t);         /* 单次帧读最大耗时 */
long oti_tr_cable_max_latency_ms(oti_transport *t);            /* 线缆命令（读消息/读帧/写帧）最大单次耗时 */
long oti_tr_cable_backoff_ms(oti_transport *t);                /* 当前/最近一次慢事件的帧写退避窗口 */
long oti_tr_cable_peer_slow_remaining_ms(oti_transport *t);    /* >0 = 正在退避帧写（剩余 ms） */
void oti_tr_cable_reset_stats(oti_transport *t);               /* 清统计（不动队列/窗口/设备） */

/* ================= 写入侧 HID 优先（2026-11-06 Lead 追加的保险）=================
 * 上一轮的"HID 优先窗口"暂停的是**消息泵读** → CURSOR/ROLE 饿死、交接判据失灵（已删除）。
 * 这里只推迟**帧写**：疑似慢对端 + 最近有 HID 活动时，传输层不发起帧写（txq 保留、
 * 消息泵/帧读照常）。理由：即使对端还是旧慢代理（帧写 250~700ms），只要用户还在动
 * 鼠标/打字就绝不发长帧写 → HID 永不冻结；用户停手 50ms 后剪贴板/心跳自动补发。
 *
 * "按需启用"（Lead，2026-11-06）：守卫**初始关闭**。单次帧写 >10ms（OTI_PEER_SLOW_HINT_MS）
 * 判定"疑似慢对端"→ 开启；连续 5 次帧写都 <10ms → 关闭。新原生泵（2~4ms/帧）下
 * 守卫完全不会生效（帧不会被 HID 活动饿死）；只有旧慢对端才进入保护模式。
 *
 * 饥饿上限（防守卫把自己饿死）：队首帧等待 > OTI_HID_STARVE_MS(1000ms) 且仅被 HID
 * 守卫挡住 → 放行写 1 帧（若慢，慢操作退避接管）。保证 ROLE 心跳/剪贴板 ACK 在
 * 用户"连续移动鼠标 >1s"时也不会被无限饿死（Windows 侧 peer_absent=5000ms）。
 * 计数：deferred_hid = 因 HID 活动被推迟的帧写次数；starved = 饥饿放行次数。 */
#define OTI_HID_GUARD_MS     50    /* HID 活动后推迟帧写的窗口（需疑似慢对端才生效） */
#define OTI_HID_STARVE_MS    1000  /* 队首帧最长饥饿时间：超过就放行 1 帧 */
#define OTI_PEER_SLOW_HINT_MS 10   /* 单次帧写超过它 = 疑似慢对端（开启 HID 守卫） */
void oti_tr_set_hid_guard_ms(int ms);                 /* 调窗口（0 = 关闭守卫）；默认 50 */
/* HID 写入成功后调用（捕获/泵线程都可能调）。⚠️ 不加 t->lock：捕获线程绝不能被
   设备 I/O 的锁挡住（否则又回到"鼠标冻结"）。内部用 relaxed 原子写时间戳。 */
void oti_tr_note_hid_activity(oti_transport *t);
unsigned long oti_tr_cable_deferred_hid(oti_transport *t);
unsigned long oti_tr_cable_starved(oti_transport *t);        /* 饥饿放行次数 */
int oti_tr_cable_peer_slow_seen(oti_transport *t);           /* 1 = HID 守卫已启用（疑似慢对端） */

#endif /* OTITRANS_H */
