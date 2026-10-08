/*
 * selftest_proto.c —— 消息层 + 状态机的单元测试（不需要硬件）
 *
 *   ./selftest_proto
 */
#include <stdio.h>
#include <string.h>

#include "otihid.h"
#include "otikm_core.h"
#include "otiproto.h"

static int fails;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        if (cond) {                                                            \
            printf("  [PASS] " fmt "\n", ##__VA_ARGS__);                       \
        } else {                                                               \
            printf("  [FAIL] " fmt "  (%s:%d)\n", ##__VA_ARGS__, __FILE__, __LINE__); \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static void test_proto(void)
{
    printf("== 消息层 ==\n");
    static uint8_t buf[OTI_BODY_MAX];
    oti_msg_hdr h;
    const uint8_t *pl;

    oti_key_evt k = {.code = 30 /* KEY_A */, .value = OTI_KEY_PRESS, .mods = 0};
    size_t n = oti_encode_key(7, &k, buf, sizeof(buf));
    CHECK(n == OTI_HDR_SIZE + 4, "KEY 组包长度 = %zu", n);
    CHECK(oti_decode(buf, n, &h, &pl) == 0 && h.type == OTI_MSG_KEY && h.seq == 7,
          "KEY 解包 type/seq 正确");
    oti_key_evt k2;
    CHECK(oti_decode_key(pl, h.len, &k2) == 0 && k2.code == 30 &&
              k2.value == OTI_KEY_PRESS, "KEY 载荷还原 (code=%u)", k2.code);

    /* CRC 必须覆盖载荷：改一个载荷字节应被拒 */
    uint8_t copy[64];
    memcpy(copy, buf, n);
    copy[OTI_HDR_SIZE] ^= 0x01;
    CHECK(oti_decode(copy, n, NULL, NULL) == -4, "载荷被篡改 → 拒收");

    /* 改头字段同样被拒 */
    memcpy(copy, buf, n);
    copy[8] ^= 0x01; /* seq */
    CHECK(oti_decode(copy, n, NULL, NULL) == -4, "头被篡改 → 拒收");

    /* 坏 magic */
    memcpy(copy, buf, n);
    copy[0] ^= 0xff;
    CHECK(oti_decode(copy, n, NULL, NULL) == -2, "坏 magic → 拒收");

    /* 长度超出实际 */
    memcpy(copy, buf, n);
    copy[12] = 0xff;
    copy[13] = 0xff;
    CHECK(oti_decode(copy, n, NULL, NULL) == -3, "声明长度 > 实到长度 → 拒收");

    /* 截断 */
    CHECK(oti_decode(buf, 10, NULL, NULL) == -1, "短于头 → 拒收");

    oti_mouse_evt m = {.dx = -1234, .dy = 567, .wheel = -1, .buttons = 0x0003};
    n = oti_encode_mouse(1, &m, buf, sizeof(buf));
    oti_mouse_evt m2;
    CHECK(n && oti_decode(buf, n, &h, &pl) == 0 &&
              oti_decode_mouse(pl, h.len, &m2) == 0 && m2.dx == -1234 &&
              m2.dy == 567 && m2.wheel == -1 && m2.buttons == 3,
          "鼠标负位移/按键位往返正确 (dx=%d)", m2.dx);

    oti_switch_evt s = {.side = OTI_SIDE_REMOTE, .edge_x = 1919, .edge_y = 540,
                        .screen_w = 1920, .screen_h = 1080, .use_hotkey_only = 0};
    n = oti_encode_switch(2, &s, buf, sizeof(buf));
    oti_switch_evt s2;
    CHECK(n && oti_decode(buf, n, &h, &pl) == 0 &&
              oti_decode_switch(pl, h.len, &s2) == 0 && s2.side == OTI_SIDE_REMOTE &&
              s2.edge_x == 1919 && s2.screen_w == 1920,
          "SWITCH 往返正确 (edge=%d,%d)", s2.edge_x, s2.edge_y);

    /* 载荷上限边界 */
    static uint8_t payload[OTI_PAYLOAD_MAX];
    memset(payload, 0xAB, sizeof(payload));
    n = oti_encode(OTI_MSG_CLIP, 0, 3, payload, OTI_PAYLOAD_MAX, buf, sizeof(buf));
    CHECK(n == OTI_HDR_SIZE + OTI_PAYLOAD_MAX && n <= OTI_BODY_MAX,
          "最大载荷可组包且不超帧体 (%zu)", n);
    CHECK(oti_decode(buf, n, &h, &pl) == 0 && h.len == OTI_PAYLOAD_MAX, "最大载荷可解包");
    CHECK(oti_encode(OTI_MSG_CLIP, 0, 3, payload, OTI_PAYLOAD_MAX + 1, buf, sizeof(buf)) == 0,
          "超长载荷拒绝组包");
}

static void test_clip_chunking(void)
{
    printf("== 剪贴板分块/重组 ==\n");
    static uint8_t src[140000], dst[140000];
    for (size_t i = 0; i < sizeof(src); i++)
        src[i] = (uint8_t)(i * 31 + 7);

    uint8_t msg[OTI_HDR_SIZE + 20 + OTI_CLIP_CHUNK_MAX];
    size_t off = 0, out = 0;
    unsigned fid = 12345, chunks = 0;
    while (off < sizeof(src)) {
        size_t len = sizeof(src) - off;
        if (len > OTI_CLIP_CHUNK_MAX)
            len = OTI_CLIP_CHUNK_MAX;
        oti_clip_hdr h = {.format = 1, .fid = fid, .offset = (uint32_t)off,
                          .total_len = (uint32_t)sizeof(src)};
        h.flags = (off == 0 ? OTI_CLIP_FIRST : 0) |
                  (off + len == sizeof(src) ? OTI_CLIP_LAST : 0);
        size_t n = oti_encode_clip(chunks, &h, src + off, (uint32_t)len, msg, sizeof(msg));
        if (!n)
            break;
        /* 接收侧：解包 → 按 offset 拼回 */
        oti_msg_hdr mh;
        const uint8_t *pl;
        if (oti_decode(msg, n, &mh, &pl) != 0)
            break;
        oti_clip_hdr rh;
        const uint8_t *data;
        uint32_t dlen;
        if (oti_decode_clip(pl, mh.len, &rh, &data, &dlen) != 0)
            break;
        if (rh.offset + dlen <= sizeof(dst)) {
            memcpy(dst + rh.offset, data, dlen);
            if (rh.offset + dlen > out)
                out = rh.offset + dlen;
        }
        off += len;
        chunks++;
    }
    CHECK(chunks == 3, "140000 字节被分成 %u 块（期望 3）", chunks);
    CHECK(out == sizeof(src) && memcmp(src, dst, sizeof(src)) == 0,
          "重组后与原文逐字节一致（%zu 字节）", out);
}

/* 角色协商（OTI_MSG_ROLE=10）：编解码往返 + 边界 + 仲裁纯逻辑冒烟 */
static void test_role(void)
{
    printf("== 角色协商（键鼠插哪边都行）==\n");
    uint8_t buf[128];
    oti_role_evt e, d;
    memset(&e, 0, sizeof(e));
    e.has_local_input = 1;
    e.want = OTI_ROLE_FORCE_MASTER;
    e.state = OTI_ROLE_SLAVE;
    e.flags = OTI_ROLE_FLAG_GRABBED;
    e.boot_id = 0xDEADBEEF;
    e.input_age_ms = 12345;
    size_t n = oti_encode_role(7, &e, buf, sizeof(buf));
    CHECK(n == OTI_HDR_SIZE + 12, "ROLE 编码长度 = 头 20 + 载荷 12（实得 %zu）", n);
    oti_msg_hdr h;
    const uint8_t *pl;
    CHECK(oti_decode(buf, n, &h, &pl) == 0 && h.type == OTI_MSG_ROLE,
          "ROLE 帧可解包且类型 = 10");
    memset(&d, 0, sizeof(d));
    CHECK(oti_decode_role(pl, h.len, &d) == 0 && d.has_local_input == 1 &&
          d.want == OTI_ROLE_FORCE_MASTER && d.state == OTI_ROLE_SLAVE &&
          (d.flags & OTI_ROLE_FLAG_GRABBED) && d.boot_id == 0xDEADBEEF &&
          d.input_age_ms == 12345,
          "ROLE 往返逐字段一致（want/state/flags/boot_id/input_age）");
    CHECK(oti_decode_role(pl, 11, &d) != 0, "ROLE 载荷 <12 字节被拒（防脏帧）");

    /* 仲裁冒烟：只有本机有键鼠 + 对端连续 3 次自称 slave → 我 master 且可 grab */
    otikm_role_in in;
    otikm_role_out out;
    memset(&in, 0, sizeof(in));
    in.has_local_input = 1;
    in.want = OTI_ROLE_AUTO;
    in.state = OTI_ROLE_SLAVE;
    in.boot_id = 1;
    in.input_age_ms = 600000;
    in.peer_seen = 1;
    in.peer_ms = 1000;
    in.peer_has_input = 0;
    in.peer_state = OTI_ROLE_SLAVE;
    in.peer_slave_streak = OTI_ROLE_STREAK_NEED;
    in.peer_input_age_ms = 600000;
    in.now_ms = 1000;
    in.dwell_ms = 10000;
    in.input_margin_ms = 2000;
    in.peer_absent_ms = 5000;
    otikm_role_decide(&in, &out);
    CHECK(out.want_state == OTI_ROLE_MASTER && out.grab_ok,
          "仲裁：只有本机有键鼠 + 对端确认 slave → master 且 grab 放行");
    in.peer_slave_streak = 1;
    otikm_role_decide(&in, &out);
    CHECK(!out.grab_ok, "仲裁 I1：对端确认不足 3 次 → 不放行 grab");
}

static void test_core(void)
{
    printf("== 键鼠共享状态机 ==\n");
    otikm_core c;
    otikm_core_init(&c, 1920, 1080, 4);
    otikm_core_set_hotkey(&c, 29 /* KEY_LEFTCTRL */, 0);
    CHECK(c.have_control == 1, "初始持有控制权");

    oti_switch_evt sw;
    oti_mouse_evt m = {.dx = 10, .dy = 0};
    CHECK(otikm_core_local_mouse(&c, &m, &sw) == OTI_ACT_LOCAL, "屏幕内移动 → 本地消费");

    /* 推到右边缘：应发起切换到远端 */
    m.dx = 3000;
    otikm_act a = otikm_core_local_mouse(&c, &m, &sw);
    CHECK(a == OTI_ACT_SWITCH_TO_REMOTE && c.have_control == 0 && sw.side == OTI_SIDE_REMOTE,
          "撞右边缘 → 切到远端（edge_x=%d）", sw.edge_x);
    CHECK(c.mx == 1919, "指针被夹到右边界 (mx=%d)", c.mx);

    /* 交出控制权后，本地事件应转发 */
    m.dx = -5;
    CHECK(otikm_core_local_mouse(&c, &m, &sw) == OTI_ACT_TO_REMOTE, "交出后鼠标事件转发远端");
    oti_key_evt k = {.code = 30, .value = OTI_KEY_PRESS};
    CHECK(otikm_core_local_key(&c, &k, &sw) == OTI_ACT_TO_REMOTE, "交出后键盘事件转发远端");

    /* 热键抢回 */
    k.code = 29;
    k.value = OTI_KEY_PRESS;
    a = otikm_core_local_key(&c, &k, &sw);
    CHECK(a == OTI_ACT_SWITCH_TO_LOCAL && c.have_control == 1 && sw.side == OTI_SIDE_LOCAL,
          "热键 → 抢回控制权");

    /* 对端把指针送过来：指针在本机 → 本地消费，并接收对端注入 */
    oti_switch_evt rsw = {.side = OTI_SIDE_REMOTE, .screen_w = 1280, .screen_h = 800};
    CHECK(otikm_core_remote_switch(&c, &rsw) == OTI_ACT_LOCAL && c.have_control == 1,
          "收到 SWITCH(remote) → 指针在本机");
    CHECK(otikm_core_local_mouse(&c, &m, &sw) == OTI_ACT_LOCAL,
          "指针在本机时本地事件本地消费（不转发）");

    /* 对端把指针收回去：本机转为驱动侧 */
    rsw.side = OTI_SIDE_LOCAL;
    rsw.edge_x = 100;
    rsw.edge_y = 200;
    CHECK(otikm_core_remote_switch(&c, &rsw) == OTI_ACT_LOCAL && c.have_control == 0,
          "收到 SWITCH(local) → 指针在对端（本机为驱动侧）");
    CHECK(otikm_core_local_mouse(&c, &m, &sw) == OTI_ACT_TO_REMOTE,
          "驱动侧本地事件转发对端");

    /* 仅热键模式：撞边不切换 */
    otikm_core c2;
    otikm_core_init(&c2, 1920, 1080, 4);
    otikm_core_set_hotkey(&c2, 29, 1);
    m.dx = 3000;
    CHECK(otikm_core_local_mouse(&c2, &m, &sw) == OTI_ACT_LOCAL && c2.have_control == 1,
          "仅热键模式：撞边不切换");

    printf("== 组合键热键 + 修饰键抑制（对标 Barrier/MWB）==\n");
    otikm_core c3;
    otikm_core_init(&c3, 1920, 1080, 4);
    CHECK(otikm_hotkey_parse("ctrl+alt+space", &c3.hk_toggle) == 0 && c3.hk_toggle.key == 57 &&
          c3.hk_toggle.mods == (OTI_MOD_CTRL | OTI_MOD_ALT), "解析 ctrl+alt+space");
    CHECK(otikm_hotkey_parse("f12", &c3.hk_lock) == 0 && c3.hk_lock.key == 88 && c3.hk_lock.mods == 0,
          "解析 f12（无修饰）");
    CHECK(otikm_hotkey_parse("ctrl+shift+m", &c3.hk_to_remote) == 0 && c3.hk_to_remote.key == 50,
          "解析 ctrl+shift+m（字母映射到 evdev 50）");
    CHECK(otikm_hotkey_parse("none", &c3.hk_to_local) == 0 && c3.hk_to_local.key == 0,
          "none = 禁用该热键");
    CHECK(otikm_hotkey_parse("ctr1+", &c3.hk_to_local) != 0, "非法写法被拒");

    /* 先进入驱动侧 */
    m.dx = 3000;
    (void)otikm_core_local_mouse(&c3, &m, &sw);
    CHECK(c3.have_control == 0, "已进入驱动侧");
    oti_key_evt sup[OTI_PEND_MAX];

    /* (a) 修饰键单独按下 → 被抑制，不转发 */
    oti_key_evt kc = {.code = 29, .value = OTI_KEY_PRESS};     /* LEFTCTRL */
    CHECK(otikm_core_local_key(&c3, &kc, &sw) == OTI_ACT_LOCAL, "驱动侧按下 Ctrl → 先不转发（可能成热键）");
    CHECK(c3.n_pend == 1, "Ctrl 进了抑制缓冲（n_pend=%d）", c3.n_pend);

    /* (b) 不是热键的普通键 → 连同缓冲一起补发（Ctrl+A 要正常到达对端） */
    oti_key_evt ka = {.code = 30, .value = OTI_KEY_PRESS};     /* KEY_A */
    CHECK(otikm_core_local_key(&c3, &ka, &sw) == OTI_ACT_LOCAL, "非热键：走缓冲补发路径");
    int ns = otikm_core_take_suppressed(&c3, sup, OTI_PEND_MAX);
    CHECK(ns == 2 && sup[0].code == 29 && sup[1].code == 30,
          "补发顺序正确：先 Ctrl 后 A（得到 %d 条）", ns);

    /* (c) 热键组合：整段被丢弃，不泄漏到对端 */
    kc.value = OTI_KEY_PRESS;
    (void)otikm_core_local_key(&c3, &kc, &sw);                 /* Ctrl 按下 → 缓冲 */
    ns = otikm_core_take_suppressed(&c3, sup, OTI_PEND_MAX);
    CHECK(ns == 0 && c3.n_pend == 1,
          "★未放行前不能抽出缓冲（否则 Ctrl 会立刻泄漏给对端，抑制失效）");
    oti_key_evt kal = {.code = 56, .value = OTI_KEY_PRESS};    /* LEFTALT */
    (void)otikm_core_local_key(&c3, &kal, &sw);                /* Alt 按下 → 缓冲 */
    ns = otikm_core_take_suppressed(&c3, sup, OTI_PEND_MAX);
    CHECK(ns == 0 && c3.n_pend == 2, "Alt 按下后仍压住（n_pend=%d）", c3.n_pend);
    oti_key_evt ksp = {.code = 57, .value = OTI_KEY_PRESS};    /* SPACE */
    otikm_act act = otikm_core_local_key(&c3, &ksp, &sw);
    CHECK(act == OTI_ACT_SWITCH_TO_LOCAL && c3.have_control == 1,
          "ctrl+alt+space → 切换控制权（回到本机）");
    ns = otikm_core_take_suppressed(&c3, sup, OTI_PEND_MAX);
    CHECK(ns == 0, "热键组合整段丢弃：Ctrl/Alt 不会泄漏给对端（缓冲 %d 条）", ns);

    /* (d) 撞边策略：edges=none / 角部死区 / 锁定 */
    otikm_core c4;
    otikm_core_init(&c4, 1920, 1080, 4);
    otikm_core_set_edges(&c4, 0);
    m.dx = 3000;
    CHECK(otikm_core_local_mouse(&c4, &m, &sw) == OTI_ACT_LOCAL && c4.have_control == 1,
          "--edges none：撞边不切换");

    otikm_core_init(&c4, 1920, 1080, 4);
    otikm_core_set_edges(&c4, OTI_EDGE_BIT(OTI_EDGE_RIGHT));
    otikm_core_set_corner_px(&c4, 40);
    c4.my = 10;                                    /* 贴右上角 */
    m.dx = 3000;
    CHECK(otikm_core_local_mouse(&c4, &m, &sw) == OTI_ACT_LOCAL && c4.have_control == 1,
          "角部死区：贴角不切换（防误切）");
    c4.my = 500;                                   /* 离开角部 */
    CHECK(otikm_core_local_mouse(&c4, &m, &sw) == OTI_ACT_SWITCH_TO_REMOTE,
          "离开角部后正常切换");

    otikm_core_init(&c4, 1920, 1080, 4);
    otikm_core_set_edges(&c4, OTI_EDGE_BIT(OTI_EDGE_RIGHT));
    c4.locked = 1;
    m.dx = 3000;
    CHECK(otikm_core_local_mouse(&c4, &m, &sw) == OTI_ACT_LOCAL && c4.have_control == 1,
          "锁定到本机时撞边不切换");

    otikm_core_init(&c4, 1920, 1080, 4);
    otikm_core_set_edges(&c4, OTI_EDGE_BIT(OTI_EDGE_RIGHT));
    otikm_core_set_switch_mods(&c4, OTI_MOD_SHIFT);
    m.dx = 3000;
    CHECK(otikm_core_local_mouse(&c4, &m, &sw) == OTI_ACT_LOCAL,
          "--switch-mod shift：不按 Shift 撞边不切换");
    oti_key_evt ks = {.code = 42, .value = OTI_KEY_PRESS};
    (void)otikm_core_local_key(&c4, &ks, &sw);
    CHECK(otikm_core_local_mouse(&c4, &m, &sw) == OTI_ACT_SWITCH_TO_REMOTE,
          "按住 Shift 后撞边才切换");

    char hb[64];
    CHECK(strcmp(otikm_hotkey_name(&c3.hk_toggle, hb, sizeof(hb)), "ctrl+alt+space") == 0,
          "热键回显: %s", hb);
}

static void test_hid(void)
{
    printf("== HID 报告构造（--peer hid 用）==\n");
    CHECK(otihid_evdev_to_usage(30 /*KEY_A*/) == 0x04, "KEY_A(30) → HID usage 0x04");
    CHECK(otihid_evdev_to_usage(44 /*KEY_Z*/) == 0x1D, "KEY_Z(44) → HID usage 0x1D");
    CHECK(otihid_evdev_to_usage(2 /*KEY_1*/) == 0x1E, "KEY_1(2) → HID usage 0x1E");
    CHECK(otihid_evdev_to_usage(57 /*SPACE*/) == 0x2C, "SPACE(57) → HID usage 0x2C");
    CHECK(otihid_evdev_to_usage(999) == 0, "未映射键返回 0");
    CHECK(otihid_modifier_bit(42 /*LEFTSHIFT*/) == 0x02 &&
          otihid_modifier_bit(29 /*LEFTCTRL*/) == 0x01 &&
          otihid_modifier_bit(30) == 0, "修饰键位映射正确");

    struct otihid_kbd k;
    otihid_kbd_reset(&k);
    uint8_t pkt[14];
    CHECK(otihid_kbd_apply(&k, 42, 1) == 1, "按下 Shift 报告有变化");
    CHECK(otihid_kbd_apply(&k, 30, 1) == 1, "按下 A 报告有变化");
    CHECK(otihid_kbd_apply(&k, 30, 1) == 0, "重复按下 A 无变化（去抖）");
    /* 厂商实测布局（NOTES §41）：[修饰键][保留][k1..k6][0..]
       ⚠️ 修饰键**不走修饰键字节**（mods 恒为 0），而是当 key usage 0xE0..0xE7 放进 6 键数组 ——
       线缆 HID 键盘接口一收到修饰键字节就把状态机弄死、之后报表全丢（NOTES §61.2 真机实证）。
       所以这里 Shift = k1=0xE1、A = k2=0x04。 */
    otihid_kbd_report(&k, 0, pkt);
    CHECK(pkt[0] == 0x00 && pkt[1] == 0x00 && pkt[2] == 0xE1 && pkt[3] == 0x04,
          "★键盘包 = [mods=00][保留=00][k1=E1(左Shift)][k2=04(A)]（§61：修饰键走 usage 数组）");
    CHECK(pkt[8] == 0 && pkt[9] == 0 && pkt[10] == 0 && pkt[11] == 0,
          "键盘包第 8..11 字节为 0（只前 12 字节入 CDB）");
    otihid_kbd_report(&k, 7, pkt);            /* 旧 layout 参数已忽略 */
    CHECK(pkt[0] == 0x00 && pkt[2] == 0xE1 && pkt[3] == 0x04, "layout 参数已忽略，始终输出厂商布局");
    CHECK(otihid_kbd_apply(&k, 30, 0) == 1, "松开 A 报告有变化");
    otihid_kbd_report(&k, 0, pkt);
    CHECK(pkt[2] == 0xE1 && pkt[3] == 0x00, "松开后 A 槽清空（Shift 仍在 k1）");

    /* 鼠标：真机标定 [按键][dx][dy][wheel]，dx=2→REL_X=2 等已验证 */
    otihid_mouse_report(0x0003, -12, 34, -1, 0, pkt);
    CHECK(pkt[0] == 0x03 && (int8_t)pkt[1] == -12 && (int8_t)pkt[2] == 34 &&
          (int8_t)pkt[3] == -1, "★鼠标包 = [按键=03][dx=-12][dy=34][wheel=-1]（厂商布局）");
    CHECK(pkt[4] == 0 && pkt[11] == 0, "鼠标包第 4..11 字节为 0");
    otihid_mouse_report(0x0003, 5000, -5000, 300, 0, pkt);
    CHECK((int8_t)pkt[1] == 127 && (int8_t)pkt[2] == -127 && (int8_t)pkt[3] == 127,
          "超范围位移被截断到 ±127（描述符每轴 1 字节有符号）");
    /* 分包：5000/-5000 → 40 个 ±125 包 + 1 个 0 包？不 —— 恰好 5000 = 39*127 + 47 */
    {
        uint8_t multi[8 * 14];
        int n = otihid_mouse_packets(0, 300, 0, 0, multi, 8);
        int sum = 0;
        for (int i = 0; i < n; i++) sum += (int8_t)multi[i * 14 + 1];
        CHECK(n == 3 && sum == 300, "大位移自动分包且总和守恒（300 = 127+127+46）");
        n = otihid_mouse_packets(0, 0, 0, 0, multi, 8);
        CHECK(n == 1, "零位移也发 1 包（纯按键变化必须送达对端）");
        n = otihid_mouse_packets(0x01, 0, 0, 0, multi, 8);
        CHECK(n == 1 && multi[0] == 0x01, "按下左键（无位移）也能送达");
        n = otihid_mouse_packets(0, 0, 0, -5, multi, 8);
        CHECK(n == 1 && (int8_t)multi[3] == -5, "只有滚轮时发 1 包");
        n = otihid_mouse_packets(0, 2000, 0, 0, multi, 2);
        CHECK(n == 2, "分包数受 max_pkts 限制（不会无限打印）");
    }
}

int main(void)
{
    test_proto();
    test_hid();
    test_clip_chunking();
    test_core();
    test_role();
    printf("\n%s（%d 项失败）\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
