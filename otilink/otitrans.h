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

/* 发送一整个消息体（≤ OTI_BODY_MAX）。返回 0 成功 */
int oti_tr_send(oti_transport *t, const void *body, size_t len);
/* 可靠发送（只对线缆后端有意义）：队列满时等待而不是"丢最旧"。
 * 大文件分片用它，避免高频丢片导致频繁重传。返回 0；-2 = 等待超时 */
int oti_tr_send_wait(oti_transport *t, const void *body, size_t len, int timeout_ms);
/* 接收一个消息体。timeout_ms < 0 = 阻塞等待；返回 0 成功、-2 超时、其它负值为错误 */
int oti_tr_recv(oti_transport *t, void *body, size_t *len, int timeout_ms);

const char *oti_tr_name(const oti_transport *t);
/* 线缆后端的发送健康度（看门狗用）：
   pending    = 待发队列里还有多少条没送出去
   last_tx_ms = 最近一次**真正写出**成功的时刻（单调时钟，0=从未）
   看门狗判据：pending>0 且 now-last_tx_ms 超过阈值 → 对端没在消费 → 自动交还本机控制 */
int  oti_tr_cable_pending(oti_transport *t);
long oti_tr_cable_last_tx_ms(oti_transport *t);
/* 清空待发队列（交还控制权时用：陈旧位移再发出去只会让对端指针乱跳） */
void oti_tr_cable_drop_tx(oti_transport *t);
/* 线缆后端底层设备（用于初始化握手；仅 cable 后端有效） */
struct otilink_dev *oti_tr_cable_dev(oti_transport *t);
void oti_tr_close(oti_transport *t);

#endif /* OTITRANS_H */
