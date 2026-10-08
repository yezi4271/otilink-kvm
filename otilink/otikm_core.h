/*
 * otikm_core.h —— 键鼠共享的纯逻辑状态机（无 I/O，便于单测）
 *
 * 唯一状态变量：have_control ≡「指针在本机(pointer_on_me)」。任一时刻指针只在一台机器上：
 *   - have_control=1：指针在本机 → 本地事件本地消费（不抓取、不转发）；
 *                     收到对端注入的事件照常注入本地。
 *   - have_control=0：指针在对端 → 本机是驱动侧：独占(GRAB)本地键鼠并全部转发给对端；
 *                     按热键可把指针拉回本机。
 *
 * SWITCH 包的 side 字段含义 = 「指针现在在哪边」（发送方视角）：
 *   side=REMOTE → 我把指针送过去了 → 接收方 have_control=1（指针在接收方）
 *   side=LOCAL  → 我把指针收回来了 → 接收方 have_control=0（指针在发送方）
 */
#ifndef OTIKM_CORE_H
#define OTIKM_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "otiproto.h"

typedef enum {
    OTI_ACT_LOCAL = 0,          /* 本地消费（不发送） */
    OTI_ACT_TO_REMOTE,          /* 转发给对端 */
    OTI_ACT_SWITCH_TO_REMOTE,   /* 发 SWITCH(remote) 后开始转发 */
    OTI_ACT_SWITCH_TO_LOCAL,    /* 发 SWITCH(local) 并恢复本地控制 */
} otikm_act;

typedef enum { OTI_EDGE_RIGHT = 0, OTI_EDGE_LEFT, OTI_EDGE_TOP, OTI_EDGE_BOTTOM } otikm_edge;

#define OTI_EDGE_BIT(e)   (1u << (e))
#define OTI_EDGE_ALL      (OTI_EDGE_BIT(OTI_EDGE_RIGHT) | OTI_EDGE_BIT(OTI_EDGE_LEFT) | \
                           OTI_EDGE_BIT(OTI_EDGE_TOP) | OTI_EDGE_BIT(OTI_EDGE_BOTTOM))

/* 修饰键位（对应 evdev 的左右 Ctrl/Shift/Alt/Meta） */
#define OTI_MOD_SHIFT 1
#define OTI_MOD_CTRL  2
#define OTI_MOD_ALT   4
#define OTI_MOD_META  8

#define OTI_PEND_MAX 6   /* 修饰键抑制缓冲深度（按住 ctrl+alt+shift 再加 3 个键足够） */

/* 一个热键 = 修饰键位掩码 + 主键 evdev 码（0 = 未配置） */
typedef struct {
    uint8_t  mods;
    uint16_t key;
} otikm_hotkey;

typedef struct {
    int      have_control;     /* 本机是否持有控制权 */
    /* 撞边切换策略（对标 PowerToys Mouse Without Borders 的 Easy Mouse） */
    unsigned edges;            /* 允许从哪些边交出去（OTI_EDGE_BIT 掩码）；0 = 只能热键 */
    uint8_t  switch_mods;      /* 撞边时必须按住的修饰键（0 = 直接切，防误触用 SHIFT/CTRL） */
    int      corner_px;        /* 角部死区像素：贴角时不切换（防误切）；0 = 关 */
    int      locked;           /* 锁定在本机：撞边一律不切换，直到解锁 */
    uint8_t  mods_down;        /* 当前按下的修饰键位（热键匹配用） */
    otikm_hotkey hk_toggle;    /* 无条件切换控制权 */
    otikm_hotkey hk_to_remote; /* 交给对端 */
    otikm_hotkey hk_to_local;  /* 拉回本机 */
    otikm_hotkey hk_lock;      /* 锁定/解锁本机 */
    unsigned long n_hotkey;    /* 热键触发次数 */
    /* 修饰键抑制缓冲：按下的修饰键先不转发，等看清是否为热键组合。
       非热键时整段按原顺序补发（否则 Ctrl+Alt+← 会既切换、又在对端留下卡住的修饰键）。 */
    uint16_t pend_key[OTI_PEND_MAX];
    uint8_t  pend_val[OTI_PEND_MAX];
    int      n_pend;
    int      flush_pending;    /* 1 = 缓冲已确定不是热键，可以放行给对端；0 = 继续压住 */
    int      screen_w, screen_h;
    /* 绝对坐标定位（修掉"系统指针加速导致手感不对"）：
       local_* = 本机屏幕（用于把本机位移换算到远端坐标系），init 后不变；
       rem_*   = 远端屏幕（由 HELLO / SWITCH 得到，未知时退回本机尺寸）；
       rem_x/y = 指针在**远端屏幕**坐标系中的位置（have_control==0 时跟踪）。 */
    int      local_w, local_h;
    int      rem_w, rem_h;
    /* 驱动期间收到的新远端几何先存这里，**下一次交给对端时**才生效 ——
       手势中途换坐标系会直接把回程判据撕掉（见上面 drv_x/drv_y 的说明）。 */
    int      rem_w_pend, rem_h_pend;
    int16_t  rem_x, rem_y;
    /* 交出去时"从对端屏幕的哪条边进来"：0=左/上，1=右/下，-1=该轴不适用。
       回程判据就靠它：在对端屏幕上**继续往外推** = 用户想把指针收回本机
       （在这之前，主控端交出去就再也回不来 —— 被驱动侧撞边发令牌那条路
        只在"键鼠在对端"的老拓扑里成立）。 */
    int      rem_entry_x, rem_entry_y;
    /* 交出去之后累计的**未缩放位移**（入口边 = 0；往对端里面为负/正见下）。
       回程判据只看它：用户必须把"推出去的距离"推回来，再多推 max(8, edge_px*4)。
       为什么不用 rem_x：rem_x 是**远端像素**坐标，要按 rem_w/local_w 缩放，而远端
       几何可能在手势中途才到（真机实测：Windows 的 HELLO 报的是 4480x1440 虚拟桌面，
       它在驱动中途到达 → 缩放从 2 跳到 4、出口边从 1919 跳到 4479，用户推回去的
       距离瞬间翻倍 → 永远回不来，且时快时慢、看起来像随机）。
       未缩放的计数正好是**我们真发给对端的 HID 位移**：推出去多少、推回来多少，
       与远端几何无关，也不会被整数截断吃掉对称性（coretest 1d 回归）。 */
    int      drv_x, drv_y;
    int      edge_px;          /* 触边阈值（像素/单位位移） */
    int16_t  mx, my;           /* 本地指针位置（本地控制时跟踪） */
    uint16_t hotkey_code;      /* 抢回控制权的 evdev 键码，0 = 关闭 */
    int      use_hotkey_only;  /* 1 = 只允许热键切换（对应 Param_Move_Out_Use_Hotkey_Switch_Only） */
    otikm_edge edge;           /* 交给对端时用的边缘 */
    /* 统计，便于诊断 */
    unsigned long n_local, n_remote, n_switch;
    unsigned long n_watchdog;      /* 被看门狗交还控制的次数 */
} otikm_core;

void otikm_core_init(otikm_core *c, int w, int h, int edge_px);
/* 热键与撞边策略（配置解析后调用；字符串形如 "ctrl+alt+space"） */
void otikm_core_set_edges(otikm_core *c, unsigned edge_mask);
void otikm_core_set_switch_mods(otikm_core *c, uint8_t mods);
void otikm_core_set_corner_px(otikm_core *c, int px);
int  otikm_hotkey_parse(const char *spec, otikm_hotkey *out);   /* 0 成功；"" 或 "none" = 清空 */
const char *otikm_hotkey_name(const otikm_hotkey *hk, char *buf, size_t cap);
void otikm_core_set_hotkey(otikm_core *c, uint16_t code, int only_hotkey);
/* 对端上报屏幕几何（0 表示未知，忽略） */
void otikm_core_set_remote_screen(otikm_core *c, int w, int h);

/* 本地鼠标事件 → 动作；发生切换时填 sw */
otikm_act otikm_core_local_mouse(otikm_core *c, const oti_mouse_evt *m, oti_switch_evt *sw);
/* 本地键盘事件 → 动作；热键抢回时填 sw */
otikm_act otikm_core_local_key(otikm_core *c, const oti_key_evt *k, oti_switch_evt *sw);
/* 收到对端 SWITCH：返回本机接下来该做什么 */
otikm_act otikm_core_remote_switch(otikm_core *c, const oti_switch_evt *sw);
/* 强制把控制权交还本机（看门狗/信号/故障兜底用）：保证用户永远不会被卡在对端 */
void otikm_core_force_local(otikm_core *c);
/* 取出**已放行**的抑制事件（每次 local_key/local_mouse 之后调用；核心未放行时返回 0，
   此时必须继续压住 —— 这正是"热键组合不会泄漏给对端"的关键）。顺序即数组顺序。 */
int  otikm_core_take_suppressed(otikm_core *c, oti_key_evt *out, int max);

/* ---------- 被驱动侧的"位移积分"判据（Wayland 降级；纯逻辑，coretest 覆盖）----------
 * 背景：Wayland 原生会话拿不到全局指针坐标（没有 XQueryPointer 那种 API），
 *       而"撞边交还"必须有个判据，否则用户被卡在对端只能靠对端热键。
 * 办法：进边校准一个起点 → 之后按**线缆 HID 的相对位移**推进并夹紧到屏幕内 →
 *       交还判据只看"贴到回程边之后还继续往外推了多少像素"（over_*），不看绝对位置。
 * 这样积分漂移只会让触发**早晚差一点**，不会失效（漂移量被"外推累计"吸收掉）。
 * 放在 core 里是因为它是纯逻辑：可以在没有设备/图形环境的机器上单测。 */
typedef struct {
    int x, y;                              /* 夹紧到屏幕内的积分坐标 */
    int over_l, over_r, over_t, over_b;    /* 贴到各边后继续外推的累计像素 */
} otikm_slave_pos;

/* 进边校准：把起点放到"朝向对端那条边"上（屏幕中心为兜底） */
void otikm_slave_seed(otikm_slave_pos *p, int w, int h, unsigned return_edge);
/* 推进一次相对位移：夹紧到 [0,w-1]x[0,h-1]，累计越过边界的外推量，
   离开某条边时把该边的累计清零（避免"很久以前推过"留下陈旧计数） */
void otikm_slave_advance(otikm_slave_pos *p, int w, int h, int dx, int dy);
/* 是否该交还：只看配置的回程边上累计外推 >= max(8, edge_px*4) 像素 */
int  otikm_slave_want_return(const otikm_slave_pos *p, int edge_px, unsigned return_edge);

/* ---------- 角色协商（第 46 轮）：纯逻辑，coretest 覆盖 ----------
 * 要解决的现场故障：两侧各自单方面判角色（麒麟看本机有没有 by-id 键鼠；Windows 开机无条件起
 * otiagent2 --edge right）→ 键鼠换到另一台后**两侧同时是主控**，抢同一条 HID/帧管道：
 * 实测帧管道双向全丢、剪贴板两边都"未确认"，只能人工停一侧。
 *
 * 这里只做**决策**（不碰 I/O）：给定"本机 + 对端最近一次 ROLE 快照 + 时间"，算出
 *   want_state（我该是 master 还是 slave）、grab_ok（现在允不允许独占/注入）、
 *   must_yield（我正持有主控但应立刻让位）。
 * 不变式 I1：grab_ok 只在"目标 master 且对端**连续 N 个心跳**说自己是 slave（或对端根本不在）"
 * 时为真 —— 这是"任何时刻最多一个 grab"的根。
 * 不变式 I2：双方都自称 master 时 boot_id 小者胜；败者 must_yield=1（调用方必须立刻释放）。 */
#define OTI_ROLE_STREAK_NEED 3

typedef struct {
    int      has_local_input;   /* 本机有"线缆之外"的键鼠 */
    int      want;              /* OTI_ROLE_AUTO / FORCE_MASTER / FORCE_SLAVE */
    int      state;             /* 我当前的角色 */
    uint32_t boot_id;           /* 本机启动标识低 32 位（平票小者优先） */
    uint32_t input_age_ms;      /* 本机最近一次真实键鼠事件距今（无输入=600000） */
    long     state_since_ms;    /* 进入当前状态的时刻（滞回） */
    int      peer_seen;
    long     peer_ms;
    int      peer_has_input, peer_want, peer_state, peer_flags;
    uint32_t peer_boot_id, peer_input_age_ms;
    int      peer_slave_streak; /* 连续收到 peer=slave 的次数（I1） */
    long     now_ms;
    long     dwell_ms;          /* 换边后最小驻留（默认 10000） */
    long     input_margin_ms;   /* 双方都有键鼠时的领先阈值（默认 2000） */
    long     peer_absent_ms;    /* 多久没收到对端 ROLE 算"不在"（默认 5000，I4 兜底） */
} otikm_role_in;

typedef struct {
    int         want_state;
    int         want_valid;
    int         grab_ok;
    int         must_yield;
    int         peer_absent;
    const char *reason;
} otikm_role_out;

void otikm_role_decide(const otikm_role_in *in, otikm_role_out *out);

#endif /* OTIKM_CORE_H */
