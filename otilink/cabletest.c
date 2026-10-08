/*
 * cabletest.c —— 真机线缆数据通路端到端测试（走生产路径 otitrans 队列传输）
 *
 * 用途：不依赖 evdev/uinput（麒麟上那些需要 root），直接把我们的协议消息
 *       通过线缆发出去，并打印收到的消息。用来验证：
 *         Linux → 线缆 → Windows：鼠标位移（对端光标会动）、按键、剪贴板
 *         Windows → 线缆 → Linux：剪贴板（在 Windows 上改剪贴板即可看到）
 *
 * 用法: ./cabletest [sg路径] [鼠标轮数]
 *       sg路径缺省自动发现（选能应答 0xF0/0x00 的那个 LUN）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "otilink.h"
#include "otiproto.h"
#include "otitrans.h"

static void show_rx(const uint8_t *buf, size_t n, uint32_t *rx_count)
{
    oti_msg_hdr h;
    const uint8_t *pl = NULL;
    if (oti_decode(buf, n, &h, &pl) != 0) {
        printf("    收到无法解析的消息 %zu 字节\n", n);
        return;
    }
    (*rx_count)++;
    switch (h.type) {
    case OTI_MSG_KEY: {
        oti_key_evt k;
        if (oti_decode_key(pl, h.len, &k) == 0)
            printf("    收到 KEY code=%u value=%u\n", k.code, k.value);
        break;
    }
    case OTI_MSG_MOUSE: {
        oti_mouse_evt m;
        if (oti_decode_mouse(pl, h.len, &m) == 0)
            printf("    收到 MOUSE dx=%d dy=%d wheel=%d btn=0x%x\n", m.dx, m.dy, m.wheel, m.buttons);
        break;
    }
    case OTI_MSG_CLIP: {
        oti_clip_hdr ch;
        const uint8_t *data = NULL;
        uint32_t dlen = 0;
        if (oti_decode_clip(pl, h.len, &ch, &data, &dlen) == 0) {
            printf("    收到 CLIP fid=%u off=%u/%u flags=0x%x %u 字节: \"%.*s\"\n",
                   ch.fid, ch.offset, ch.total_len, ch.flags, dlen, (int)dlen, (const char *)data);
        }
        break;
    }
    case OTI_MSG_SWITCH:
        printf("    收到 SWITCH\n");
        break;
    case OTI_MSG_PING:
        printf("    收到 PING\n");
        break;
    default:
        printf("    收到 type=%u len=%u\n", h.type, h.len);
        break;
    }
}

int main(int argc, char **argv)
{
    const char *sg = argc > 1 ? argv[1] : NULL;
    int rounds = argc > 2 ? atoi(argv[2]) : 12;
    int keycode = argc > 3 ? atoi(argv[3]) : 30;   /* evdev 键码（默认 30='A'；58=CapsLock 可客观验证） */

    oti_transport *t = oti_tr_open_cable(sg);
    if (!t) {
        fprintf(stderr, "打不开线缆（设备是否插着？）\n");
        return 1;
    }
    printf("传输: %s\n", oti_tr_name(t));

    static uint8_t buf[OTI_BODY_MAX];
    size_t n = 0;
    uint32_t seq = 0, rx_count = 0;
    int sent = 0, failed = 0;

    /* 0) 先发一段醒目的剪贴板文本，Windows 侧应能看到剪贴板被改 */
    {
        const char *txt = "otilink-cable-clipboard-OK";
        oti_clip_hdr h = {.format = 1,
                          .flags = OTI_CLIP_FIRST | OTI_CLIP_LAST,
                          .fid = 1,
                          .offset = 0,
                          .total_len = (uint32_t)strlen(txt)};
        size_t l = oti_encode_clip(++seq, &h, txt, (uint32_t)strlen(txt), buf, sizeof(buf));
        int rc = oti_tr_send(t, buf, l);
        printf("[CLIP] 发出 \"%s\" rc=%d\n", txt, rc);
        if (rc == 0)
            sent++;
        else
            failed++;
    }

    /* 1) 鼠标：每轮右移 60 像素（对端光标应持续右移） */
    for (int i = 0; i < rounds; i++) {
        oti_mouse_evt m = {.dx = 60, .dy = 0, .wheel = 0, .buttons = 0};
        size_t l = oti_encode_mouse(++seq, &m, buf, sizeof(buf));
        int rc = oti_tr_send(t, buf, l);
        printf("[%2d/%d] 发 MOUSE dx=+60 rc=%d", i + 1, rounds, rc);
        if (rc == 0)
            sent++;
        else
            failed++;
        printf("\n");
        fflush(stdout);

        /* 顺便收：对端可能在发剪贴板/按键 */
        while (oti_tr_recv(t, buf, &n, 0) == 0)
            show_rx(buf, n, &rx_count);
        fflush(stdout);
        usleep(400 * 1000);
    }

    /* 2) 键盘：按一下 'A'（evdev 30 == PS/2 set-1 扫描码 0x1E）再松开 */
    {
        oti_key_evt k = {.code = (uint16_t)keycode, .value = OTI_KEY_PRESS, .mods = 0};
        size_t l = oti_encode_key(++seq, &k, buf, sizeof(buf));
        int rc = oti_tr_send(t, buf, l);
        printf("[KEY ] 按下 evdev %d rc=%d\n", keycode, rc);
        usleep(120 * 1000);
        k.value = OTI_KEY_RELEASE;
        l = oti_encode_key(++seq, &k, buf, sizeof(buf));
        rc = oti_tr_send(t, buf, l);
        printf("[KEY ] 松开 evdev %d rc=%d\n", keycode, rc);
        if (rc == 0)
            sent += 2;
        else
            failed++;
    }

    /* 3) 再收一会儿，看对端有没有消息（在 Windows 上改剪贴板就能看到） */
    printf("继续监听 8 秒（可在 Windows 上复制文字，应看到 CLIP 到达）…\n");
    {
        long deadline = (long)time(NULL) + 8;
        while ((long)time(NULL) < deadline) {
            if (oti_tr_recv(t, buf, &n, 200) == 0)
                show_rx(buf, n, &rx_count);
        }
    }

    printf("\n汇总: 发出 %d 条（失败 %d），收到 %u 条\n", sent, failed, rx_count);
    oti_tr_close(t);
    return failed ? 1 : 0;
}
