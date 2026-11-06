/*
 * otikm_core.c —— 见 otikm_core.h
 *
 * 设计对标（产品级要求）：
 *   - PowerToys Mouse Without Borders：Easy Mouse（撞边切换，可用 Shift/Ctrl 修饰防误触）、
 *     角部防误切（Block mouse at screen corners）、Ctrl+Alt+<键> 切换类热键、锁定。
 *   - Barrier / Input Leap：**热键必须抑制**。否则 Ctrl+Alt+← 会既触发切换、又把组合键
 *     泄漏给对端，在对端留下"卡住的 Ctrl/Alt"。这里用修饰键缓冲实现：
 *     按下可能是热键前缀的修饰键时先压住不转发，等看清后续是热键组合（整段丢弃）
 *     还是普通输入（按原顺序整段补发）。
 */
#include "otikm_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* evdev 修饰键 → 位 */
static uint8_t mod_bit(uint16_t code)
{
    switch (code) {
    case 42:  case 54:  return OTI_MOD_SHIFT;
    case 29:  case 97:  return OTI_MOD_CTRL;
    case 56:  case 100: return OTI_MOD_ALT;
    case 125: case 126: return OTI_MOD_META;
    default:            return 0;
    }
}

/* ---------------- 热键解析： "ctrl+alt+space" / "f12" / "none" ---------------- */

static int key_from_name(const char *n, uint16_t *out)
{
    static const struct { const char *n; uint16_t c; } tab[] = {
        { "space", 57 }, { "enter", 28 }, { "return", 28 }, { "tab", 15 },
        { "esc", 1 }, { "escape", 1 }, { "home", 102 }, { "end", 107 },
        { "insert", 110 }, { "delete", 111 },
        { "left", 105 }, { "right", 106 }, { "up", 103 }, { "down", 108 },
        { "pageup", 104 }, { "pagedown", 109 }, { "pause", 119 },
        { "scrolllock", 70 }, { "printscreen", 99 },
        { "f1", 59 }, { "f2", 60 }, { "f3", 61 }, { "f4", 62 }, { "f5", 63 }, { "f6", 64 },
        { "f7", 65 }, { "f8", 66 }, { "f9", 67 }, { "f10", 68 }, { "f11", 87 }, { "f12", 88 },
        { "grave", 41 }, { "-", 12 }, { "=", 13 }, { "[", 26 }, { "]", 27 },
        { "\\", 43 }, { ";", 39 }, { "'", 40 }, { ",", 51 }, { ".", 52 }, { "/", 53 },
    };
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (strcasecmp(n, tab[i].n) == 0) {
            *out = tab[i].c;
            return 0;
        }
    if (n[0] && !n[1]) {
        /* 单字母/数字：evdev 主键区码值（与 PS/2 set-1 扫描码一致） */
        static const struct { char ch; uint16_t c; } alpha[] = {
            { 'q', 16 }, { 'w', 17 }, { 'e', 18 }, { 'r', 19 }, { 't', 20 }, { 'y', 21 },
            { 'u', 22 }, { 'i', 23 }, { 'o', 24 }, { 'p', 25 },
            { 'a', 30 }, { 's', 31 }, { 'd', 32 }, { 'f', 33 }, { 'g', 34 }, { 'h', 35 },
            { 'j', 36 }, { 'k', 37 }, { 'l', 38 },
            { 'z', 44 }, { 'x', 45 }, { 'c', 46 }, { 'v', 47 }, { 'b', 48 }, { 'n', 49 },
            { 'm', 50 },
        };
        for (size_t i = 0; i < sizeof(alpha) / sizeof(alpha[0]); i++)
            if (alpha[i].ch == n[0]) { *out = alpha[i].c; return 0; }
        if (n[0] >= '1' && n[0] <= '9') { *out = (uint16_t)(2 + (n[0] - '1')); return 0; }
        if (n[0] == '0') { *out = 11; return 0; }
    }
    return -1;
}

int otikm_hotkey_parse(const char *spec, otikm_hotkey *out)
{
    out->mods = 0;
    out->key = 0;
    if (!spec || !*spec || strcasecmp(spec, "none") == 0 || strcasecmp(spec, "off") == 0)
        return 0;                                    /* 合法：禁用该热键 */
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "+ \t", &save); tok; tok = strtok_r(NULL, "+ \t", &save)) {
        if (strcasecmp(tok, "ctrl") == 0 || strcasecmp(tok, "control") == 0)
            out->mods |= OTI_MOD_CTRL;
        else if (strcasecmp(tok, "alt") == 0)
            out->mods |= OTI_MOD_ALT;
        else if (strcasecmp(tok, "shift") == 0)
            out->mods |= OTI_MOD_SHIFT;
        else if (strcasecmp(tok, "meta") == 0 || strcasecmp(tok, "win") == 0 ||
                 strcasecmp(tok, "super") == 0)
            out->mods |= OTI_MOD_META;
        else {
            uint16_t k = 0;
            if (key_from_name(tok, &k) != 0)
                return -1;
            out->key = k;
        }
    }
    return out->key ? 0 : -1;
}

const char *otikm_hotkey_name(const otikm_hotkey *hk, char *buf, size_t cap)
{
    if (!hk->key) {
        snprintf(buf, cap, "none");
        return buf;
    }
    uint16_t k = hk->key;
    char kn[24];
    snprintf(kn, sizeof(kn), "code%u", k);
    static const struct { uint16_t c; const char *n; } names[] = {
        { 57, "space" }, { 28, "enter" }, { 15, "tab" }, { 1, "esc" }, { 102, "home" },
        { 107, "end" }, { 110, "insert" }, { 111, "delete" }, { 105, "left" }, { 106, "right" },
        { 103, "up" }, { 108, "down" }, { 104, "pageup" }, { 109, "pagedown" }, { 119, "pause" },
        { 70, "scrolllock" }, { 99, "printscreen" }, { 59, "f1" }, { 60, "f2" }, { 61, "f3" },
        { 62, "f4" }, { 63, "f5" }, { 64, "f6" }, { 65, "f7" }, { 66, "f8" }, { 67, "f9" },
        { 68, "f10" }, { 87, "f11" }, { 88, "f12" }, { 41, "grave" },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].c == k) {
            snprintf(kn, sizeof(kn), "%s", names[i].n);
            break;
        }
    if (kn[0] == 'c' && strncmp(kn, "code", 4) == 0) {     /* 回显友好：字母/数字 */
        static const struct { uint16_t c; char ch; } al[] = {
            { 16, 'q' }, { 17, 'w' }, { 18, 'e' }, { 19, 'r' }, { 20, 't' }, { 21, 'y' },
            { 22, 'u' }, { 23, 'i' }, { 24, 'o' }, { 25, 'p' }, { 30, 'a' }, { 31, 's' },
            { 32, 'd' }, { 33, 'f' }, { 34, 'g' }, { 35, 'h' }, { 36, 'j' }, { 37, 'k' },
            { 38, 'l' }, { 44, 'z' }, { 45, 'x' }, { 46, 'c' }, { 47, 'v' }, { 48, 'b' },
            { 49, 'n' }, { 50, 'm' },
        };
        for (size_t i = 0; i < sizeof(al) / sizeof(al[0]); i++)
            if (al[i].c == k) { snprintf(kn, sizeof(kn), "%c", al[i].ch); break; }
        if (kn[0] == 'c' && strncmp(kn, "code", 4) == 0 && k >= 2 && k <= 11)
            snprintf(kn, sizeof(kn), "%c", k == 11 ? '0' : (char)('1' + (k - 2)));
    }
    snprintf(buf, cap, "%s%s%s%s%s",
             (hk->mods & OTI_MOD_CTRL) ? "ctrl+" : "",
             (hk->mods & OTI_MOD_ALT) ? "alt+" : "",
             (hk->mods & OTI_MOD_SHIFT) ? "shift+" : "",
             (hk->mods & OTI_MOD_META) ? "meta+" : "",
             kn);
    return buf;
}

static int hk_match(const otikm_hotkey *hk, uint8_t mods_down, uint16_t code)
{
    if (!hk->key || hk->key != code)
        return 0;
    return (mods_down & hk->mods) == hk->mods;
}

/* 当前按下的修饰键是否还"有可能"组成某个热键（用于决定要不要压住不转发） */
static int hk_prefix_possible(const otikm_core *c)
{
    const otikm_hotkey *hks[4] = { &c->hk_toggle, &c->hk_to_remote, &c->hk_to_local, &c->hk_lock };
    for (int i = 0; i < 4; i++) {
        const otikm_hotkey *h = hks[i];
        if (!h->key)
            continue;
        if ((h->mods & c->mods_down) == c->mods_down)
            return 1;
    }
    return 0;
}

/* 一维坐标推进 + "越界外推"累计（被驱动侧积分判据与主控侧回程判据**共用**这一份语义）。
   limit = 该轴屏幕尺寸，坐标范围 [0, limit-1]：
     * 推进后夹紧在范围内；
     * 越过下界/上界的部分累计到 over_lo / over_hi；
     * 回到边界以内就把对应侧的累计清零（避免"很久以前推过"留下陈旧计数）。
   为什么必须累计：鼠标事件每次只有 1~3 个计数，夹紧后单次永远越不了阈值
   —— 主控侧回程曾因此"推半天回不来"（2026-09-21 真机 bug，coretest 1b 回归）。 */
static void axis_advance(int *pos, int *over_lo, int *over_hi, int limit, int d)
{
    if (limit < 1)
        limit = 1;
    int n = *pos + d;
    if (n < 0)
        *over_lo += -n;
    else if (n > 0)
        *over_lo = 0;
    if (n > limit - 1)
        *over_hi += n - (limit - 1);
    else if (n < limit - 1)
        *over_hi = 0;
    if (n < 0)
        n = 0;
    if (n > limit - 1)
        n = limit - 1;
    *pos = n;
}

/* 交出去时把远端坐标放回入口边，并把"未缩放位移"清零（回程判据从入口边起算） */
static void remote_reset_disp(otikm_core *c)
{
    c->drv_x = c->drv_y = 0;
}

/* 入口边的"贴边"容差（**远端像素**）：edge_px*4。
   edge_px 是本地触边阈值（默认 4）→ 容差 16px；再兜底 ≥8px。
   为什么用乘以 4 而不是 edge_px：CURSOR 遥测是采样值，远端光标被系统夹在
   rem_w-1 附近，但虚拟桌面/DPI 换算可能差 1~2px；±16px 足够宽又不至于把
   "离边几十像素"误判成贴边（贴边只是回程的必要条件，不是充分条件）。 */
static int rem_edge_margin(const otikm_core *c)
{
    int m = c->edge_px > 0 ? c->edge_px * 4 : 16;
    if (m < 8)
        m = 8;
    return m;
}

/* 最新一次回传的光标是否贴在"从对端进来那条边"上。
   几何必须用回传自带的 rem_cur_w/h（虚拟桌面 3584x1152），**不用** rem_w/h
   （HELLO 的 10s 心跳可能是旧的 2048x1152）。 */
static int rem_cursor_at_entry_edge(const otikm_core *c)
{
    if (c->rem_cur_w <= 0 || c->rem_cur_h <= 0 || c->rem_cur_w > 65535 ||
        c->rem_cur_h > 65535)
        return 0;
    int m = rem_edge_margin(c);
    if (c->rem_entry_x == 1)
        return c->rem_cur_x >= c->rem_cur_w - 1 - m;
    if (c->rem_entry_x == 0)
        return c->rem_cur_x <= m;
    if (c->rem_entry_y == 1)
        return c->rem_cur_y >= c->rem_cur_h - 1 - m;
    if (c->rem_entry_y == 0)
        return c->rem_cur_y <= m;
    return 0;
}

/* "未缩放位移"沿**向入口边外推**方向的投影：正数 = 用户在把指针往本机方向顶。
   入口边 = 对端屏幕上离本机最近的那条边（由 enter_remote 记住 rem_entry_*）。 */
static int drv_push_out(const otikm_core *c)
{
    if (c->rem_entry_x == 1)
        return c->drv_x;
    if (c->rem_entry_x == 0)
        return -c->drv_x;
    if (c->rem_entry_y == 1)
        return c->drv_y;
    if (c->rem_entry_y == 0)
        return -c->drv_y;
    return 0;
}

/* 旧兼容判据阈值：只对**从未收到过 CURSOR** 的对端（零软件端/旧 agent）生效。
   max(100, edge_px*12) 个未缩放计数。为什么不是 16：没有真实光标反馈时无法区分
   "贴边外推"与"正常移动"，阈值必须大到只有明确的推回动作才触发（2026-10-10 真机：
   阈值 16 时在 Windows 稍微一动就被拉回，用户感受是"过不去/键盘在两机之间跳"）。 */
static int legacy_push_need(const otikm_core *c)
{
    int need = c->edge_px * 12;
    if (need < 100)
        need = 100;
    return need;
}

/* 回程落点：离出口边多远（px）。必须足够远，否则回程后的一点惯性/抖动又会立刻推出去。
   真机（2026-10-10，纯鼠标切换）：落点 edge+8=12px，用户在 Windows 上用鼠标 1~2 秒就
   回程一次 → 键盘被反复 EVIOCGRAB，"在 Kylin 上打不了字"。2026-10-27 起至少 96px。 */
int otikm_core_return_inset(const otikm_core *c)
{
    /* 2026-10-27（#3）：60 → 96。60px 在 1080p 上只有约 1.4cm，回程手势的惯性尾巴
       就能把光标再送进 edge 区（日志实证：拉回本机后 2~3 秒又"交给对端"，
       用户感知是"刚能在麒麟打字又没了"）。96px 留出余量，又不会让落点跳得太远。 */
    int m = c->edge_px + 8;
    if (m < 96)
        m = 96;
    return m;
}

static void fill_switch(const otikm_core *c, int side, oti_switch_evt *sw)
{
    memset(sw, 0, sizeof(*sw));
    sw->side = (uint8_t)side;
    sw->use_hotkey_only = (uint8_t)(c->edges == 0);
    sw->edge_x = c->mx;
    sw->edge_y = c->my;
    sw->screen_w = (uint16_t)c->screen_w;
    sw->screen_h = (uint16_t)c->screen_h;
}

/* ---------------- 撞边策略 ---------------- */

void otikm_core_set_edges(otikm_core *c, unsigned edge_mask) { c->edges = edge_mask; }
void otikm_core_set_switch_mods(otikm_core *c, uint8_t mods) { c->switch_mods = mods; }
void otikm_core_set_corner_px(otikm_core *c, int px) { c->corner_px = px > 0 ? px : 0; }

static int in_corner(const otikm_core *c, int x, int y, int edge)
{
    if (c->corner_px <= 0)
        return 0;
    int near_h = (y < c->corner_px) || (y >= c->screen_h - c->corner_px);
    if (!near_h)
        return 0;
    if (edge == OTI_EDGE_RIGHT)
        return x >= c->screen_w - c->corner_px;
    if (edge == OTI_EDGE_LEFT)
        return x < c->corner_px;
    return 0;
}

static int switch_allowed(const otikm_core *c, int edge, int x, int y)
{
    if (c->locked)
        return 0;
    if (!(c->edges & OTI_EDGE_BIT(edge)))
        return 0;
    if (c->switch_mods && (c->mods_down & c->switch_mods) != c->switch_mods)
        return 0;
    if (in_corner(c, x, y, edge))
        return 0;
    return 1;
}

/* ---------------- 初始化 / 配置 ---------------- */

void otikm_core_init(otikm_core *c, int w, int h, int edge_px)
{
    memset(c, 0, sizeof(*c));
    c->have_control = 1;
    c->screen_w = w > 0 ? w : 1920;
    c->screen_h = h > 0 ? h : 1080;
    c->edge_px = edge_px > 0 ? edge_px : 4;
    c->mx = (int16_t)(c->screen_w / 2);
    c->my = (int16_t)(c->screen_h / 2);
    c->edge = OTI_EDGE_RIGHT;
    c->local_w = c->screen_w;
    c->local_h = c->screen_h;
    c->rem_x = (int16_t)(c->screen_w / 2);
    c->rem_y = (int16_t)(c->screen_h / 2);
    c->edges = OTI_EDGE_ALL;
}

void otikm_core_set_hotkey(otikm_core *c, uint16_t code, int only_hotkey)
{
    c->hk_to_local.mods = 0;
    c->hk_to_local.key = code;
    if (only_hotkey)
        c->edges = 0;
}

void otikm_core_set_remote_screen(otikm_core *c, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    /* 正在驱动对端时**不许**换坐标系（真机 bug：Windows 的 HELLO 报 4480x1440
       虚拟桌面，它在手势中途到达 → rem_x 缩放与出口边一起变 → 回程距离翻倍，
       表现为"时好时坏、推半天回不来"）。先存为 pending，下次交出去时再生效。 */
    if (!c->have_control) {
        c->rem_w_pend = w;
        c->rem_h_pend = h;
        return;
    }
    c->rem_w = w;
    c->rem_h = h;
    if (c->rem_x > w - 1)
        c->rem_x = (int16_t)(w - 1);
    if (c->rem_y > h - 1)
        c->rem_y = (int16_t)(h - 1);
    /* 回传光标的坐标**不能**用 HELLO 的几何夹紧：CURSOR 载荷自带虚拟桌面尺寸
       （Windows 现场 3584x1152，HELLO 心跳是 10s 前的旧值/主屏尺寸 2048x1152）。
       拿旧几何夹 rem_cur_x 会把"远端右边缘 3583"夹成 1919 → 永远判不到边。 */
}

void otikm_core_set_remote_cursor(otikm_core *c, int x, int y, int w, int h, long now)
{
    if (w > 0)
        c->rem_cur_w = w;
    if (h > 0)
        c->rem_cur_h = h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (c->rem_cur_w > 0 && x > c->rem_cur_w - 1)
        x = c->rem_cur_w - 1;
    if (c->rem_cur_h > 0 && y > c->rem_cur_h - 1)
        y = c->rem_cur_h - 1;
    c->rem_cur_x = x;
    c->rem_cur_y = y;
    c->rem_cur_ms = now;
    c->rem_cur_valid = 1;
    c->cursor_ever = 1;
}

void otikm_core_set_now(otikm_core *c, long now)
{
    c->now_ms = now;
}

/* 交出去：把远端坐标放到**进来的那条边**上，并记住是哪条边。
   我出右边 → 从对端左边进来（反之亦然）；上下同理。
   （老代码只按 from_right 处理横向，纵向其实是错的；顺手一起修正。） */
static void enter_remote(otikm_core *c, int exit_edge)
{
    /* 驱动期间攒下的远端几何**先**生效（见 otikm_core_set_remote_screen）：
       必须在读 rem_w/rem_h 之前，否则入口边还是按旧尺寸算的（coretest 1d 抓到过）。 */
    if (c->rem_w_pend > 0 && c->rem_h_pend > 0) {
        c->rem_w = c->rem_w_pend;
        c->rem_h = c->rem_h_pend;
        c->rem_w_pend = c->rem_h_pend = 0;
    }
    int rw = c->rem_w > 0 ? c->rem_w : c->screen_w;
    int rh = c->rem_h > 0 ? c->rem_h : c->screen_h;
    int lw = c->local_w > 0 ? c->local_w : 1920;
    int lh = c->local_h > 0 ? c->local_h : 1080;
    c->rem_entry_x = -1;
    c->rem_entry_y = -1;
    remote_reset_disp(c);
    /* 新一段驱动：上一段的对端光标位置已无意义，等新的回传。
       若对端曾经发过 CURSOR（cursor_ever），hid_park 已经把它的光标顶到入口边
       → 首帧遥测到达前先按"在入口边"假设，允许用户立刻反悔（300ms 窗口）。 */
    c->rem_cur_valid = 0;
    c->rem_switch_ms = c->now_ms;
    c->rem_edge_anchor_ms = 0;
    c->rem_edge_confirmed = c->cursor_ever ? 1 : 0;
    if (exit_edge == OTI_EDGE_RIGHT || exit_edge == OTI_EDGE_LEFT) {
        int from_right = (exit_edge == OTI_EDGE_RIGHT);
        c->rem_x = (int16_t)(from_right ? 0 : (rw > 0 ? rw - 1 : 0));
        c->rem_entry_x = from_right ? 0 : 1;      /* 0 = 从对端左边进来 */
        int y = (int)((long)c->my * rh / (lh > 0 ? lh : 1));
        if (y < 0)
            y = 0;
        if (rh > 0 && y > rh - 1)
            y = rh - 1;
        c->rem_y = (int16_t)y;
    } else {
        int from_top = (exit_edge == OTI_EDGE_TOP);
        c->rem_y = (int16_t)(from_top ? 0 : (rh > 0 ? rh - 1 : 0));
        c->rem_entry_y = from_top ? 0 : 1;
        int x = (int)((long)c->mx * rw / (lw > 0 ? lw : 1));
        if (x < 0)
            x = 0;
        if (rw > 0 && x > rw - 1)
            x = rw - 1;
        c->rem_x = (int16_t)x;
    }
}

/* ---------------- 修饰键抑制缓冲 ---------------- */

static void pend_clear(otikm_core *c) { c->n_pend = 0; c->flush_pending = 0; }

static void pend_push(otikm_core *c, uint16_t code, uint8_t val)
{
    if (c->n_pend < OTI_PEND_MAX) {
        c->pend_key[c->n_pend] = code;
        c->pend_val[c->n_pend] = val;
        c->n_pend++;
    } else {
        /* 缓冲满：说明不是热键组合，直接放弃抑制（把最早的丢掉，保证顺序里最新的还在） */
        memmove(c->pend_key, c->pend_key + 1, sizeof(c->pend_key[0]) * (OTI_PEND_MAX - 1));
        memmove(c->pend_val, c->pend_val + 1, sizeof(c->pend_val[0]) * (OTI_PEND_MAX - 1));
        c->pend_key[OTI_PEND_MAX - 1] = code;
        c->pend_val[OTI_PEND_MAX - 1] = val;
    }
}

int otikm_core_take_suppressed(otikm_core *c, oti_key_evt *out, int max)
{
    if (!c->flush_pending)
        return 0;                       /* 还没确定不是热键：继续压住（关键！） */
    int n = c->n_pend < max ? c->n_pend : max;
    for (int i = 0; i < n; i++) {
        out[i].code = c->pend_key[i];
        out[i].value = c->pend_val[i];
        out[i].mods = 0;
    }
    c->n_pend = 0;
    c->flush_pending = 0;
    return n;
}

/* ---------------- 按键 ---------------- */

otikm_act otikm_core_local_key(otikm_core *c, const oti_key_evt *k, oti_switch_evt *sw)
{
    uint8_t bit = mod_bit(k->code);

    if (bit) {
        if (k->value == OTI_KEY_PRESS)
            c->mods_down |= bit;
        else if (k->value == OTI_KEY_RELEASE)
            c->mods_down &= (uint8_t)~bit;
    }

    /* 1) 热键：在"按下"时判定。**不排除修饰键**——单修饰键也可以是热键（兼容旧配置），
       而组合键（ctrl+alt+space）在 Ctrl 单独按下时 hk->key 不匹配，自然不会误触发。 */
    if (k->value == OTI_KEY_PRESS) {
        if (hk_match(&c->hk_toggle, c->mods_down, k->code)) {
            c->n_hotkey++;
            pend_clear(c);
            if (c->have_control) {
                fill_switch(c, OTI_SIDE_REMOTE, sw);
                c->have_control = 0;
                c->locked = 0;
                enter_remote(c, c->edge == OTI_EDGE_LEFT ? 0 : 1);
                c->n_switch++;
                return OTI_ACT_SWITCH_TO_REMOTE;
            }
            fill_switch(c, OTI_SIDE_LOCAL, sw);
            c->have_control = 1;
            c->n_switch++;
            return OTI_ACT_SWITCH_TO_LOCAL;
        }
        if (hk_match(&c->hk_to_remote, c->mods_down, k->code)) {
            c->n_hotkey++;
            pend_clear(c);
            if (c->have_control) {
                fill_switch(c, OTI_SIDE_REMOTE, sw);
                c->have_control = 0;
                c->locked = 0;
                enter_remote(c, c->edge == OTI_EDGE_LEFT ? 0 : 1);
                c->n_switch++;
                return OTI_ACT_SWITCH_TO_REMOTE;
            }
            return OTI_ACT_LOCAL;
        }
        if (hk_match(&c->hk_to_local, c->mods_down, k->code)) {
            c->n_hotkey++;
            pend_clear(c);
            if (!c->have_control) {
                fill_switch(c, OTI_SIDE_LOCAL, sw);
                c->have_control = 1;
                c->n_switch++;
                return OTI_ACT_SWITCH_TO_LOCAL;
            }
            return OTI_ACT_LOCAL;
        }
        if (hk_match(&c->hk_lock, c->mods_down, k->code)) {
            c->n_hotkey++;
            pend_clear(c);
            c->locked = !c->locked;
            return OTI_ACT_LOCAL;
        }
        /* 不是热键：只有当本键**不是修饰键**时才能确定"不是热键组合"。
           若本键也是修饰键（Ctrl 之后又按 Alt），必须继续压住 —— 它可能还在凑热键前缀。 */
        if (!bit && c->n_pend) {
            pend_push(c, k->code, k->value);
            c->flush_pending = 1;
            c->n_remote++;
            return OTI_ACT_LOCAL;
        }
    }

    /* 2) 修饰键按下且仍可能是热键前缀 → 压住不转发 */
    if (bit && k->value == OTI_KEY_PRESS && !c->have_control && hk_prefix_possible(c)) {
        pend_push(c, k->code, k->value);
        return OTI_ACT_LOCAL;
    }

    /* 3) 修饰键松开：若还在压着，说明不是热键组合 → 补发（含松开事件） */
    if (bit && k->value == OTI_KEY_RELEASE && c->n_pend) {
        pend_push(c, k->code, k->value);
        c->mods_down &= (uint8_t)~bit;
        /* 修饰键单独按下又松开（期间没有别的键）→ 确定不是热键组合 → 放行 */
        if (!hk_prefix_possible(c))
            c->flush_pending = 1;
        return OTI_ACT_LOCAL;
    }

    if (!c->have_control) {
        c->n_remote++;
        return OTI_ACT_TO_REMOTE;
    }
    c->n_local++;
    return OTI_ACT_LOCAL;
}

otikm_act otikm_core_local_mouse(otikm_core *c, const oti_mouse_evt *m, oti_switch_evt *sw)
{
    if (c->n_pend)
        c->flush_pending = 1;           /* 鼠标动了 ⇒ 不是热键组合 ⇒ 放行修饰键 */
    if (!c->have_control) {
        int rw = c->rem_w > 0 ? c->rem_w : c->screen_w;
        int rh = c->rem_h > 0 ? c->rem_h : c->screen_h;
        int lw = c->local_w > 0 ? c->local_w : 1920;
        int lh = c->local_h > 0 ? c->local_h : 1080;
        /* ---- 对端真实光标回传（2026-11-06 重写）----
           新鲜窗口 300ms（Windows 原生泵 20~50ms/条）；"贴边"必须用回传自带的
           虚拟桌面几何 rem_cur_w/h。 */
        int cur_fresh = c->rem_cur_valid && c->rem_cur_w > 0 && c->rem_cur_h > 0 &&
                        c->now_ms >= c->rem_cur_ms &&
                        c->now_ms - c->rem_cur_ms <= OTI_REM_CURSOR_TTL_MS;
        int cur_at_edge = cur_fresh ? rem_cursor_at_entry_edge(c) : 0;
        /* 首帧假设窗口：刚交出去、本段还没收到第一帧遥测，但 hid_park 已把远端
           光标顶到入口边 → 300ms 内先按"在入口边"处理（用户立刻反悔能马上回来）。
           一旦发现用户往对端里面走（外推投影变负）立即作废这个假设。 */
        long since_switch = c->now_ms - c->rem_switch_ms;
        int assume_edge = c->cursor_ever && !c->rem_cur_valid && since_switch >= 0 &&
                          since_switch <= OTI_REM_CURSOR_WAIT_MS;
        int dx = (int)((long)m->dx * rw / lw);
        int dy = (int)((long)m->dy * rh / lh);
        if (dx == 0 && m->dx)
            dx = m->dx > 0 ? 1 : -1;
        if (dy == 0 && m->dy)
            dy = m->dy > 0 ? 1 : -1;
        /* 夹紧的远端坐标（用于协议/绝对坐标路径；本机 HID 路径其实只发相对位移） */
        int nx = c->rem_x, ny = c->rem_y;
        int o1 = 0, o2 = 0;              /* 只要夹紧，不要越界累计（回程用 drv_*） */
        axis_advance(&nx, &o1, &o2, rw, dx);
        c->rem_x = (int16_t)nx;
        axis_advance(&ny, &o1, &o2, rh, dy);
        c->rem_y = (int16_t)ny;

        /* ---- 回程判据（2026-11-06 第 61 轮重写）：真实光标为主，计数只负责防抖 ----
           规则（与 NOTES §76.2 的验收一致）：
             * 主判据：远端**真实光标**被确认贴在入口边（±edge_px*4）之后，
               用户继续向入口边外推的**未缩放位移累计** ≥ OTI_REM_CURSOR_PUSH(16)
               → 回程。计数用未缩放值：它正好等于我们真发给对端的 HID 位移，
               与远端几何/指针加速无关（镜像手势）。
             * 连续"在入口边"的遥测**不重锚/不清零**：20~50ms 一条遥测时，
               如果每条都清零，慢推（2px/次）永远攒不到 16 → 真机"回不来"。
               只有 0→1 跃迁（从"不在边"到"在边"）才重锚。
             * 遥测过期（>300ms）：**冻结判定**，但累计计数保留；新遥测一到就按
               最新值判（用户已经推够 16 的话立刻回）。**绝不**退回"计数 100 就揪回"
               （2026-11-06 现场 bug：回传过期 + 正常移动累积 100 计数被揪回）。
             * 从未收到过 CURSOR 的对端（零软件/旧 agent）：保留旧计数判据
               max(100, edge_px*12) 作为唯一的回程方式（兼容路径）。
           为什么用"未缩放位移"而不是远端像素：远端几何可能在手势中途才到
           （HELLO 报的是虚拟桌面 4480x1440），缩放/出口边一起变 → 回程距离翻倍，
           真机表现为"同一手势时好时坏"（coretest 1d）。 */
        {
            int anchored_now = 0;
            /* ① 状态更新：只在**新鲜遥测**上更新"是否在入口边"；过期时保持上次
                  确认（这样断档期间累计的外推计数不会丢，恢复后立即生效）。 */
            if (cur_fresh) {
                if (cur_at_edge) {
                    if (!c->rem_edge_confirmed) {
                        /* 0→1 跃迁：以这次确认为零点重锚（之前在路上推的计数不算；
                           本拍事件的位移也不计入 —— 它很可能正是"走到边"的那一步）。 */
                        c->drv_x = c->drv_y = 0;
                        c->rem_edge_anchor_ms = c->rem_cur_ms;
                        anchored_now = 1;
                    }
                    c->rem_edge_confirmed = 1;
                } else {
                    /* 对端光标**不在**入口边：计数与像素可能已漂移，清零；
                       这一段不按计数回程（等下一次回传确认真到边）。 */
                    c->rem_edge_confirmed = 0;
                    c->rem_edge_anchor_ms = 0;
                    c->drv_x = c->drv_y = 0;
                }
            }
            if (!anchored_now && (c->rem_edge_confirmed || !c->cursor_ever)) {
                c->drv_x += m->dx;
                c->drv_y += m->dy;
            }
            /* ② 首帧假设只对"立刻继续往外顶"成立：一旦用户明显往对端里面走
               （外推投影为负），说明远端光标大概率已被带离入口边 → 作废假设。 */
            if (assume_edge && c->rem_edge_confirmed && drv_push_out(c) < 0) {
                c->rem_edge_confirmed = 0;
                c->rem_edge_anchor_ms = 0;
                c->drv_x = c->drv_y = 0;
            }
            /* ③ 判定 */
            int back = 0;
            if (c->cursor_ever) {
                if (c->rem_edge_confirmed && (cur_fresh || assume_edge) &&
                    drv_push_out(c) >= OTI_REM_CURSOR_PUSH)
                    back = 1;
            } else if (drv_push_out(c) >= legacy_push_need(c)) {
                /* 兼容路径：从未有过真实光标回传（零软件端/旧 agent）。 */
                back = 1;
            }
            if (back) {
                /* 指针放回"当初出去的那条边"往里一点点：
                   既让用户一眼看到指针回来了，又不会因为惯性立刻又推出去。 */
                int inset = otikm_core_return_inset(c);
                if (c->edge == OTI_EDGE_RIGHT) {
                    c->mx = (int16_t)(c->screen_w - 1 - inset);
                    c->my = (int16_t)((long)ny * c->screen_h / (rh > 0 ? rh : 1));
                } else if (c->edge == OTI_EDGE_LEFT) {
                    c->mx = (int16_t)inset;
                    c->my = (int16_t)((long)ny * c->screen_h / (rh > 0 ? rh : 1));
                } else if (c->edge == OTI_EDGE_TOP) {
                    c->my = (int16_t)inset;
                    c->mx = (int16_t)((long)nx * c->screen_w / (rw > 0 ? rw : 1));
                } else {
                    c->my = (int16_t)(c->screen_h - 1 - inset);
                    c->mx = (int16_t)((long)nx * c->screen_w / (rw > 0 ? rw : 1));
                }
                if (c->mx < 0)
                    c->mx = 0;
                if (c->my < 0)
                    c->my = 0;
                if (c->mx > c->screen_w - 1)
                    c->mx = (int16_t)(c->screen_w - 1);
                if (c->my > c->screen_h - 1)
                    c->my = (int16_t)(c->screen_h - 1);
                c->have_control = 1;
                c->rem_entry_x = c->rem_entry_y = -1;
                remote_reset_disp(c);
                c->rem_edge_confirmed = 0;
                c->rem_edge_anchor_ms = 0;
                fill_switch(c, OTI_SIDE_LOCAL, sw);
                c->n_switch++;
                return OTI_ACT_SWITCH_TO_LOCAL;
            }
        }

        c->n_remote++;
        return OTI_ACT_TO_REMOTE;
    }

    int x = c->mx + m->dx;
    int y = c->my + m->dy;
    int w = c->screen_w, h = c->screen_h, e = c->edge_px;

    if (x >= w - e && switch_allowed(c, OTI_EDGE_RIGHT, x, y)) {
        c->mx = (int16_t)(w - 1);
        c->my = (int16_t)(y < 0 ? 0 : (y >= h ? h - 1 : y));
        c->edge = OTI_EDGE_RIGHT;
        fill_switch(c, OTI_SIDE_REMOTE, sw);
        c->have_control = 0;
        enter_remote(c, OTI_EDGE_RIGHT);
        c->n_switch++;
        return OTI_ACT_SWITCH_TO_REMOTE;
    }
    if (x < e && switch_allowed(c, OTI_EDGE_LEFT, x, y)) {
        c->mx = 0;
        c->my = (int16_t)(y < 0 ? 0 : (y >= h ? h - 1 : y));
        c->edge = OTI_EDGE_LEFT;
        fill_switch(c, OTI_SIDE_REMOTE, sw);
        c->have_control = 0;
        enter_remote(c, OTI_EDGE_LEFT);
        c->n_switch++;
        return OTI_ACT_SWITCH_TO_REMOTE;
    }
    if (y < e && switch_allowed(c, OTI_EDGE_TOP, x, y)) {
        c->my = 0;
        c->mx = (int16_t)(x < 0 ? 0 : (x >= w ? w - 1 : x));
        c->edge = OTI_EDGE_TOP;
        fill_switch(c, OTI_SIDE_REMOTE, sw);
        c->have_control = 0;
        enter_remote(c, OTI_EDGE_TOP);
        c->n_switch++;
        return OTI_ACT_SWITCH_TO_REMOTE;
    }
    if (y >= h - e && switch_allowed(c, OTI_EDGE_BOTTOM, x, y)) {
        c->my = (int16_t)(h - 1);
        c->mx = (int16_t)(x < 0 ? 0 : (x >= w ? w - 1 : x));
        c->edge = OTI_EDGE_BOTTOM;
        fill_switch(c, OTI_SIDE_REMOTE, sw);
        c->have_control = 0;
        enter_remote(c, OTI_EDGE_BOTTOM);
        c->n_switch++;
        return OTI_ACT_SWITCH_TO_REMOTE;
    }
    c->mx = (int16_t)(x < 0 ? 0 : (x >= w ? w - 1 : x));
    c->my = (int16_t)(y < 0 ? 0 : (y >= h ? h - 1 : y));
    c->n_local++;
    return OTI_ACT_LOCAL;
}

otikm_act otikm_core_remote_switch(otikm_core *c, const oti_switch_evt *sw)
{
    c->n_switch++;
    if (sw->side == OTI_SIDE_REMOTE) {
        c->have_control = 1;
        c->rem_edge_confirmed = 0;
        c->rem_edge_anchor_ms = 0;
        remote_reset_disp(c);
        pend_clear(c);
        return OTI_ACT_LOCAL;
    }
    c->have_control = 0;
    remote_reset_disp(c);            /* 远端几何/坐标被重设：位移累计不能沿用旧的 */
    /* 对端把指针交给我们（它刚撞边）：入口边未知，不做"已在入口边"的假设，
       等它自己的 CURSOR 遥测来确认（cursor_ever=1 时判据只在新鲜遥测上判）。 */
    c->rem_cur_valid = 0;
    c->rem_switch_ms = c->now_ms;
    c->rem_edge_confirmed = 0;
    c->rem_edge_anchor_ms = 0;
    if (sw->screen_w && sw->screen_h) {
        /* 对端刚把指针交给我们并**同时**报上它的屏幕：立即生效。
           不能走 set_remote_screen() —— 此刻 have_control 已经是 0（正在驱动），
           那条路会把它当成"驱动中途的更新"存成 pending。 */
        c->rem_w = sw->screen_w;
        c->rem_h = sw->screen_h;
        c->rem_w_pend = c->rem_h_pend = 0;
        c->mx = sw->edge_x;
        c->my = sw->edge_y;
    }
    if (sw->edge_x > 0)
        c->mx = sw->edge_x;
    if (sw->edge_y > 0)
        c->my = sw->edge_y;
    c->rem_x = sw->edge_x;
    c->rem_y = sw->edge_y;
    if (c->rem_w > 0 && c->rem_x > c->rem_w - 1)
        c->rem_x = (int16_t)(c->rem_w - 1);
    if (c->rem_h > 0 && c->rem_y > c->rem_h - 1)
        c->rem_y = (int16_t)(c->rem_h - 1);
    return OTI_ACT_LOCAL;
}

void otikm_core_force_local(otikm_core *c)
{
    c->n_pend = 0;
    c->flush_pending = 0;
    c->mods_down = 0;
    if (c->have_control)
        return;
    c->have_control = 1;
    c->edge = OTI_EDGE_RIGHT;
    c->rem_entry_x = c->rem_entry_y = -1;
    c->rem_edge_confirmed = 0;
    c->rem_edge_anchor_ms = 0;
    remote_reset_disp(c);
    c->n_watchdog++;
}

/* ---------- 被驱动侧"位移积分"判据（见 otikm_core.h 的说明）---------- */

/* ---------- 角色协商决策（纯逻辑；见 otikm_core.h）---------- */
void otikm_role_decide(const otikm_role_in *in, otikm_role_out *out)
{
    memset(out, 0, sizeof(*out));
    out->want_state = in->state;
    out->want_valid = 0;
    out->reason = "保持现状";

    int peer_recent = in->peer_seen && (in->now_ms - in->peer_ms <= in->peer_absent_ms);
    out->peer_absent = !peer_recent;

    /* 1) 显式优先：本机 want 最高；其次对端显式 want */
    int want = in->want;
    if (want == OTI_ROLE_AUTO && peer_recent) {
        if (in->peer_want == OTI_ROLE_FORCE_MASTER)
            want = OTI_ROLE_FORCE_SLAVE;
        else if (in->peer_want == OTI_ROLE_FORCE_SLAVE)
            want = OTI_ROLE_FORCE_MASTER;
    }
    if (want == OTI_ROLE_FORCE_MASTER) {
        out->want_state = OTI_ROLE_MASTER; out->want_valid = 1;
        out->reason = "本机显式 master";
    } else if (want == OTI_ROLE_FORCE_SLAVE) {
        out->want_state = OTI_ROLE_SLAVE; out->want_valid = 1;
        out->reason = "本机/对端显式 slave";
    } else {
        /* 2) 自动：有本机键鼠的一方当主控 */
        int my_has = in->has_local_input;
        int pe_has = peer_recent && in->peer_has_input;
        if (my_has && !pe_has) {
            out->want_state = OTI_ROLE_MASTER; out->want_valid = 1;
            out->reason = "只有本机有键鼠";
        } else if (!my_has && pe_has) {
            out->want_state = OTI_ROLE_SLAVE; out->want_valid = 1;
            out->reason = "只有对端有键鼠";
        } else if (my_has && pe_has) {
            int i_master = (in->state == OTI_ROLE_MASTER);
            int peer_master = (in->peer_state == OTI_ROLE_MASTER);
            long dwell_ok = (in->state == OTI_ROLE_NONE) ||
                            (in->now_ms - in->state_since_ms >= in->dwell_ms);
            uint32_t margin = (uint32_t)(in->input_margin_ms > 0 ? in->input_margin_ms : 0);
            if (!i_master && !peer_master) {
                if (in->input_age_ms + margin < in->peer_input_age_ms) {
                    out->want_state = OTI_ROLE_MASTER; out->want_valid = 1;
                    out->reason = "双方都有键鼠：本机刚在用";
                } else if (in->peer_input_age_ms + margin < in->input_age_ms) {
                    out->want_state = OTI_ROLE_SLAVE; out->want_valid = 1;
                    out->reason = "双方都有键鼠：对端刚在用";
                }
            } else if (dwell_ok) {
                if (in->input_age_ms + margin < in->peer_input_age_ms) {
                    out->want_state = OTI_ROLE_MASTER; out->want_valid = 1;
                    out->reason = i_master ? "保持 master（本机更近）" : "抢 master（本机更近）";
                } else if (in->peer_input_age_ms + margin < in->input_age_ms) {
                    out->want_state = OTI_ROLE_SLAVE; out->want_valid = 1;
                    out->reason = "让 master（对端更近）";
                }
            }
        } else {
            out->want_state = OTI_ROLE_SLAVE; out->want_valid = 1;
            out->reason = "双方都没有本机键鼠（请显式 --role）";
        }
    }

    /* 3) 冲突（对端也自称 master）：boot_id 小者胜。
       只在**真正有歧义**时用这条：我自己是 auto、且双方都有本机键鼠。
       为什么必须加这两个前提（冒烟测试实测）：
         * 对端显式 FORCE_MASTER 时，我在第 1 步已经把目的角色置成 slave —— 用 in->want(=AUTO)
           再走平票会把结论覆盖成 master（实测 B 抢了主控）；
         * 我本机没有键鼠时（not my_has），无论如何都不该靠 boot_id 抢成 master。 */
    int my_has_input = in->has_local_input;
    int peer_has_input2 = peer_recent && in->peer_has_input;
    if (peer_recent && in->peer_state == OTI_ROLE_MASTER && want == OTI_ROLE_AUTO &&
        my_has_input && peer_has_input2) {
        int peer_wins = (in->peer_boot_id < in->boot_id) ||
                        (in->peer_boot_id == in->boot_id && in->peer_input_age_ms < in->input_age_ms);
        if (peer_wins) {
            out->want_state = OTI_ROLE_SLAVE; out->want_valid = 1;
            out->reason = "冲突：对端 boot_id 更小（我让位）";
        } else {
            out->want_state = OTI_ROLE_MASTER; out->want_valid = 1;
            out->reason = "冲突：本机 boot_id 更小（对端该让）";
        }
    }

    /* 4) 让位：我在 master 但目标不是 master → 调用方必须立刻释放 */
    out->must_yield = (in->state == OTI_ROLE_MASTER) && out->want_valid &&
                      out->want_state != OTI_ROLE_MASTER;

    /* 5) grab 许可（I1） */
    int peer_says_slave = peer_recent && in->peer_state == OTI_ROLE_SLAVE &&
                          in->peer_slave_streak >= OTI_ROLE_STREAK_NEED;
    out->grab_ok = (out->want_state == OTI_ROLE_MASTER) && (peer_says_slave || out->peer_absent);
    if (out->must_yield)
        out->grab_ok = 0;
}

void otikm_slave_seed(otikm_slave_pos *p, int w, int h, unsigned return_edge)
{
    p->x = w / 2;
    p->y = h / 2;
    p->over_l = p->over_r = p->over_t = p->over_b = 0;
    if (return_edge & OTI_EDGE_BIT(OTI_EDGE_LEFT))
        p->x = 0;                              /* 从对端进来的那条边 */
    if (return_edge & OTI_EDGE_BIT(OTI_EDGE_RIGHT))
        p->x = w - 1;
    if (return_edge & OTI_EDGE_BIT(OTI_EDGE_TOP))
        p->y = 0;
    if (return_edge & OTI_EDGE_BIT(OTI_EDGE_BOTTOM))
        p->y = h - 1;
}

void otikm_slave_advance(otikm_slave_pos *p, int w, int h, int dx, int dy)
{
    /* 与主控侧回程判据（otikm_core_local_mouse）共用同一份"夹紧 + 越界累计"语义 */
    axis_advance(&p->x, &p->over_l, &p->over_r, w, dx);
    axis_advance(&p->y, &p->over_t, &p->over_b, h, dy);
}

int otikm_slave_want_return(const otikm_slave_pos *p, int edge_px, unsigned return_edge)
{
    int need = edge_px > 0 ? edge_px * 4 : 16;   /* 默认 4px 阈值 → 需外推 16px */
    if (need < 8)
        need = 8;
    if ((return_edge & OTI_EDGE_BIT(OTI_EDGE_LEFT)) && p->over_l >= need)
        return 1;
    if ((return_edge & OTI_EDGE_BIT(OTI_EDGE_RIGHT)) && p->over_r >= need)
        return 1;
    if ((return_edge & OTI_EDGE_BIT(OTI_EDGE_TOP)) && p->over_t >= need)
        return 1;
    if ((return_edge & OTI_EDGE_BIT(OTI_EDGE_BOTTOM)) && p->over_b >= need)
        return 1;
    return 0;
}
