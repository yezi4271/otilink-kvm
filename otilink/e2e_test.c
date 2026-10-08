/*
 * e2e_test.c —— 端到端：两个线程经 socketpair 跑真实传输抽象 + 消息层
 *
 * 验证「控制端 → 被控端」的完整数据路径在本机可跑通：
 *   键按下/抬起 → 鼠标位移 → SWITCH 交接 → 剪贴板 3 块传输并重组 → PING
 *   末了一条故意损坏的消息必须被拒收。
 *
 * 线缆后端与 fd 后端共用同一套上层代码，所以这里通过即代表协议/状态机无误，
 * 剩下的只是 SCSI 通道实测。
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "otikm_core.h"
#include "otiproto.h"
#include "otitrans.h"

#define CLIP_BYTES 140000

static oti_transport *g_tx;
static int g_fail;
static uint8_t g_clip_src[CLIP_BYTES], g_clip_dst[CLIP_BYTES];
static size_t g_clip_out;

static int send_msg(uint8_t *buf, size_t n)
{
    return oti_tr_send(g_tx, buf, n);
}

static void *sender(void *arg)
{
    (void)arg;
    uint8_t buf[OTI_HDR_SIZE + 20 + OTI_CLIP_CHUNK_MAX];
    otikm_core c;
    otikm_core_init(&c, 1920, 1080, 4);
    otikm_core_set_hotkey(&c, 29, 0);

    /* 1) 本地键盘：A 按下 / 抬起（本地控制，正常应本地消费，这里直接发以测通道） */
    oti_key_evt k = {.code = 30, .value = OTI_KEY_PRESS};
    send_msg(buf, oti_encode_key(1, &k, buf, sizeof(buf)));
    k.value = OTI_KEY_RELEASE;
    send_msg(buf, oti_encode_key(2, &k, buf, sizeof(buf)));

    /* 2) 鼠标位移（含负数，验证符号） */
    oti_mouse_evt m = {.dx = -17, .dy = 42, .wheel = -1, .buttons = 1};
    send_msg(buf, oti_encode_mouse(3, &m, buf, sizeof(buf)));

    /* 3) 状态机：撞右边缘产生 SWITCH(remote)，把该包发出去 */
    oti_switch_evt sw;
    oti_mouse_evt big = {.dx = 5000, .dy = 0};
    otikm_act a = otikm_core_local_mouse(&c, &big, &sw);
    if (a != OTI_ACT_SWITCH_TO_REMOTE)
        g_fail++;
    send_msg(buf, oti_encode_switch(4, &sw, buf, sizeof(buf)));

    /* 4) 剪贴板分块 */
    for (size_t i = 0; i < CLIP_BYTES; i++)
        g_clip_src[i] = (uint8_t)(i * 7 + 3);
    size_t off = 0;
    uint32_t seq = 10, fid = 99;
    while (off < CLIP_BYTES) {
        size_t len = CLIP_BYTES - off;
        if (len > OTI_CLIP_CHUNK_MAX)
            len = OTI_CLIP_CHUNK_MAX;
        oti_clip_hdr h = {.format = 1, .fid = fid, .offset = (uint32_t)off,
                          .total_len = CLIP_BYTES};
        h.flags = (off == 0 ? OTI_CLIP_FIRST : 0) |
                  (off + len == CLIP_BYTES ? OTI_CLIP_LAST : 0);
        size_t n = oti_encode_clip(seq++, &h, g_clip_src + off, (uint32_t)len,
                                   buf, sizeof(buf));
        if (!n || send_msg(buf, n)) {
            g_fail++;
            break;
        }
        off += len;
    }

    /* 5) PING */
    send_msg(buf, oti_encode_ping(seq++, 123456, buf, sizeof(buf)));

    /* 6) 故意损坏一条：篡改载荷一个字节（CRC 应拦住） */
    oti_key_evt k2 = {.code = 31, .value = OTI_KEY_PRESS};
    size_t n = oti_encode_key(seq++, &k2, buf, sizeof(buf));
    buf[OTI_HDR_SIZE] ^= 0xff;
    send_msg(buf, n);
    return NULL;
}

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        perror("socketpair");
        return 1;
    }
    g_tx = oti_tr_open_fd(sv[0], 1);
    oti_transport *rx = oti_tr_open_fd(sv[1], 1);
    if (!g_tx || !rx) {
        fprintf(stderr, "transport 打开失败\n");
        return 1;
    }

    pthread_t th;
    pthread_create(&th, NULL, sender, NULL);

    int got_key = 0, got_mouse = 0, got_switch = 0, got_ping = 0;
    int bad_magic_or_crc = 0, clip_chunks = 0, good = 0;
    uint32_t last_seq = 0;
    int order_ok = 1;
    uint8_t body[OTI_BODY_MAX];

    for (int i = 0; i < 12; i++) {
        size_t len = 0;
        int rc = oti_tr_recv(rx, body, &len, 3000);
        if (rc == -2)
            break;
        if (rc != 0) {
            fprintf(stderr, "recv rc=%d\n", rc);
            break;
        }
        oti_msg_hdr h;
        const uint8_t *pl;
        int dr = oti_decode(body, len, &h, &pl);
        if (dr == -4 || dr == -2) {          /* 预期只出现在最后那条损坏消息 */
            bad_magic_or_crc++;
            continue;
        }
        if (dr != 0) {
            fprintf(stderr, "decode rc=%d\n", dr);
            break;
        }
        good++;
        if (h.seq < last_seq)
            order_ok = 0;
        last_seq = h.seq;
        switch (h.type) {
        case OTI_MSG_KEY: {
            oti_key_evt k;
            if (oti_decode_key(pl, h.len, &k) == 0 && k.code == 30)
                got_key++;
            break;
        }
        case OTI_MSG_MOUSE: {
            oti_mouse_evt m;
            if (oti_decode_mouse(pl, h.len, &m) == 0 && m.dx == -17 && m.dy == 42 &&
                m.wheel == -1 && m.buttons == 1)
                got_mouse++;
            break;
        }
        case OTI_MSG_SWITCH: {
            oti_switch_evt s;
            if (oti_decode_switch(pl, h.len, &s) == 0 && s.side == OTI_SIDE_REMOTE &&
                s.edge_x == 1919)
                got_switch++;
            break;
        }
        case OTI_MSG_CLIP: {
            oti_clip_hdr ch;
            const uint8_t *data;
            uint32_t dlen;
            if (oti_decode_clip(pl, h.len, &ch, &data, &dlen) == 0) {
                if (ch.offset + dlen <= CLIP_BYTES) {
                    memcpy(g_clip_dst + ch.offset, data, dlen);
                    if (ch.offset + dlen > g_clip_out)
                        g_clip_out = ch.offset + dlen;
                }
                clip_chunks++;
            }
            break;
        }
        case OTI_MSG_PING:
            got_ping++;
            break;
        default:
            break;
        }
    }
    pthread_join(th, NULL);

    int fails = 0;
#define OK(cond, fmt, ...)                                                     \
    do {                                                                       \
        printf("  [%s] " fmt "\n", (cond) ? "PASS" : "FAIL", ##__VA_ARGS__);   \
        if (!(cond))                                                           \
            fails++;                                                           \
    } while (0)

    printf("== 端到端（socketpair 上的真实 transport + proto）==\n");
    OK(good == 8, "收到并解出 8 条合法消息（2 键 + 1 鼠 + 1 切换 + 3 剪贴 + 1 PING，实得 %d）", good);
    OK(got_key == 2, "键盘按下/抬起各 1 条（实得 %d，抬起码相同计数）", got_key);
    OK(got_mouse == 1, "鼠标事件数值与符号正确");
    OK(got_switch == 1, "SWITCH 包携带右边缘坐标 1919");
    OK(clip_chunks == 3, "剪贴板 3 块（实得 %d）", clip_chunks);
    OK(g_clip_out == CLIP_BYTES && memcmp(g_clip_src, g_clip_dst, CLIP_BYTES) == 0,
       "剪贴板重组 %zu 字节逐字节一致", g_clip_out);
    OK(got_ping == 1, "PING 到达");
    OK(bad_magic_or_crc == 1, "损坏消息被 CRC 拦下（实得 %d）", bad_magic_or_crc);
    OK(order_ok, "消息顺序保持（seq 单调）");
    OK(g_fail == 0, "发送侧状态机无异常");

    oti_tr_close(g_tx);
    oti_tr_close(rx);
    printf("\n%s（%d 项失败）\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
