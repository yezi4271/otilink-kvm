/*
 * coretest.c —— otikm_core 交接状态机的单元测试（不需要设备/图形环境）
 *
 * 为什么需要它：交接/回程逻辑只在真机上靠"人手推边缘"验证，回归成本极高，
 * 而且"交出去回不来"这种 bug 一旦漏掉，用户当场失控（本轮就踩了）。
 * 这里用**合成鼠标事件**把状态机走一遍，覆盖：
 *   1) 本机撞边 → 交出去（SWITCH_TO_REMOTE）
 *   2) 交出去后往对端里面走 → 继续转发（TO_REMOTE）
 *   3) 在对端屏幕上**继续往外推** → 回程（SWITCH_TO_LOCAL）★ 本轮新增
 *   4) 刚交出去时的抖动（±1px）**不能**误判成回程
 *   5) 左右两个方向都要对（远端入口边 = 本机出口边的对面）
 *
 * 编译：make coretest && ./coretest
 */
#include <stdio.h>
#include <string.h>

#include "otikm_core.h"
#include "otiproto.h"

static int fails, checks;

static const char *act_name(otikm_act a)
{
    switch (a) {
    case OTI_ACT_LOCAL: return "LOCAL";
    case OTI_ACT_TO_REMOTE: return "TO_REMOTE";
    case OTI_ACT_SWITCH_TO_REMOTE: return "SWITCH_TO_REMOTE";
    case OTI_ACT_SWITCH_TO_LOCAL: return "SWITCH_TO_LOCAL";
    }
    return "?";
}

static void expect(otikm_act got, otikm_act want, const char *what)
{
    checks++;
    if (got != want) {
        fails++;
        printf("  [FAIL] %s：得到 %s，期望 %s\n", what, act_name(got), act_name(want));
    } else {
        printf("  [PASS] %s（%s）\n", what, act_name(got));
    }
}

/* 推一段位移：按 step 连续发事件，返回最后一次的动作 */
static otikm_act push(otikm_core *c, int dx, int dy, int times)
{
    oti_mouse_evt m;
    oti_switch_evt sw;
    otikm_act act = OTI_ACT_LOCAL;
    for (int i = 0; i < times; i++) {
        memset(&m, 0, sizeof(m));
        m.dx = (int16_t)dx;
        m.dy = (int16_t)dy;
        act = otikm_core_local_mouse(c, &m, &sw);
        if (act == OTI_ACT_SWITCH_TO_REMOTE || act == OTI_ACT_SWITCH_TO_LOCAL)
            return act;                     /* 交接动作只发生一次 */
    }
    return act;
}

static void setup(otikm_core *c)
{
    otikm_core_init(c, 1920, 1080, 4);
    otikm_core_set_edges(c, OTI_EDGE_BIT(OTI_EDGE_LEFT) | OTI_EDGE_BIT(OTI_EDGE_RIGHT));
    otikm_core_set_remote_screen(c, 1920, 1080);   /* 对端同尺寸，便于算 */
    /* 指针放到中间偏左，方便往左推 */
    otikm_core_force_local(c);
    c->mx = 200;
    c->my = 500;
}

int main(void)
{
    otikm_core c;

    printf("=== 1) 左出 / 右回（Kylin 在 Windows 右边 → 往左交给对端）===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "本机往左推到边缘 → 交出去");
    checks++;
    if (c.have_control) { fails++; printf("  [FAIL] 交出去后 have_control 应为 0\n"); }
    else printf("  [PASS] 交出去后 have_control=0\n");
    checks++;
    if (c.rem_entry_x != 1) { fails++; printf("  [FAIL] 远端入口边应为右(rem_entry_x=1)，实得 %d\n", c.rem_entry_x); }
    else printf("  [PASS] 远端入口边 = 右边（rem_entry_x=1，rem_x=%d）\n", c.rem_x);

    expect(push(&c, -40, 0, 3), OTI_ACT_TO_REMOTE, "继续往对端里面走 → 继续转发");
    expect(push(&c, 1, 0, 3), OTI_ACT_TO_REMOTE, "贴着入口边抖动 ±1px → 不误判回程");
    expect(push(&c, 40, 0, 60), OTI_ACT_SWITCH_TO_LOCAL, "在对端屏幕上往右推回去 → 回程");
    checks++;
    if (!c.have_control) { fails++; printf("  [FAIL] 回程后 have_control 应为 1\n"); }
    else printf("  [PASS] 回程后 have_control=1（本机指针 x=%d）\n", c.mx);

    /* ---- 1b) 慢推回程（2026-09-21 真机 bug："键鼠插在 Linux、移到 Windows 回不来"）----
       真机上的鼠标事件每次只有 1~3 个计数；旧实现只看**单次位移**是否越过边界，
       而坐标每次都夹回边界 → 越界量永远不累计 → 推半天回不来（用户报障）。
       回归要求：小步（2px/次）也要能累计到阈值并回程。 */
    printf("=== 1b) 慢推回程：小步位移必须累计触发（真机 bug 回归）===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "先交出去");
    expect(push(&c, 1, 0, 5), OTI_ACT_TO_REMOTE, "贴边抖动 5×1px → 仍不回程（阈值 16）");
    expect(push(&c, 2, 0, 40), OTI_ACT_SWITCH_TO_LOCAL, "慢推 2px/次 → 累计到阈值即回程");

    /* ---- 1c) 镜像手势：往对端里推多少，就得推回来多少（再多推一点才回程）---- */
    printf("=== 1c) 镜像手势：回程距离要与推入距离对称 ===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "先交出去");
    expect(push(&c, -20, 0, 5), OTI_ACT_TO_REMOTE, "往对端里面推 100px");
    expect(push(&c, 20, 0, 5), OTI_ACT_TO_REMOTE, "推回 100px（刚好回到入口边）→ 还不回程");
    expect(push(&c, 20, 0, 1), OTI_ACT_SWITCH_TO_LOCAL, "再多推一点 → 回程");

    /* ---- 1d) 远端几何在手势中途到达（2026-09-21 真机 bug：Windows 侧的 HELLO
       报的是 4480x1440 的**虚拟桌面**，它可能在"指针已经在 Windows 上"时才到）----
       旧实现把 rem_x 按 rem_w/local_w 缩放：几何一变，缩放从 2 跳到 4、出口边从 1919
       跳到 4479 → 同一手势时好时坏、推半天回不来。现在回程只看**未缩放位移**，
       且驱动期间不允许换坐标系（先存 pending，下次交出去时生效）。 */
    printf("=== 1d) 驱动中途收到远端几何（不许换坐标系，回程不受影响）===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "先交出去（此时远端几何未知）");
    expect(push(&c, -1, 0, 10), OTI_ACT_TO_REMOTE, "往对端里面推 10px");
    otikm_core_set_remote_screen(&c, 4480, 1440);        /* HELLO 中途到达 */
    checks++;
    if (c.rem_w != 1920 || c.rem_w_pend != 4480) {
        fails++;
        printf("  [FAIL] 驱动期间不该换坐标系（rem_w=%d pending=%d）\n", c.rem_w, c.rem_w_pend);
    } else {
        printf("  [PASS] 驱动期间几何被暂存（rem_w 仍 %d，pending %d）\n", c.rem_w, c.rem_w_pend);
    }
    expect(push(&c, 1, 0, 40), OTI_ACT_SWITCH_TO_LOCAL, "小步推回来 → 回程（与远端几何无关）");
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "再交出去");
    checks++;
    if (c.rem_w != 4480 || c.rem_w_pend != 0) {
        fails++;
        printf("  [FAIL] 新的远端几何应在这次交出去时生效（rem_w=%d pending=%d）\n", c.rem_w, c.rem_w_pend);
    } else {
        printf("  [PASS] pending 几何在下次交出去时生效（rem_w=%d，rem_x=%d）\n", c.rem_w, c.rem_x);
    }

    printf("=== 2) 右出 / 左回（镜像方向）===\n");
    setup(&c);
    c.mx = 1700;
    expect(push(&c, 40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "本机往右推到边缘 → 交出去");
    checks++;
    if (c.rem_entry_x != 0) { fails++; printf("  [FAIL] 远端入口边应为左(rem_entry_x=0)，实得 %d\n", c.rem_entry_x); }
    else printf("  [PASS] 远端入口边 = 左边（rem_entry_x=0，rem_x=%d）\n", c.rem_x);
    expect(push(&c, 40, 0, 3), OTI_ACT_TO_REMOTE, "继续往对端里面走 → 继续转发");
    expect(push(&c, -40, 0, 60), OTI_ACT_SWITCH_TO_LOCAL, "在对端屏幕上往左推回去 → 回程");

    printf("=== 3) 回程后不能立刻又被推出去（惯性防抖）===\n");
    expect(push(&c, -8, 0, 1), OTI_ACT_LOCAL, "回程瞬间的惯性小位移 → 仍在本机");

    printf("=== 4) 热键拉回本机（键盘路径不受影响）===\n");
    setup(&c);
    c.mx = 200;
    push(&c, -40, 0, 10);                    /* 交出去 */
    checks++;
    if (c.have_control) { fails++; printf("  [FAIL] 前置：应先交出去\n"); }
    else printf("  [PASS] 前置：已交出去\n");
    otikm_core_force_local(&c);
    checks++;
    if (!c.have_control) { fails++; printf("  [FAIL] force_local 应把控制权拿回\n"); }
    else printf("  [PASS] force_local 拉回本机（看门狗/热键走这条）\n");

    /* ---- 5) 被驱动侧"位移积分"判据（Wayland 降级路径，第 39 轮新增）----
       Wayland 原生会话拿不到全局指针，只能按位移积分 + "贴边外推累计"判定交还。
       这里把它（otikm_slave_*，纯逻辑）当成状态机走一遍。 */
    printf("=== 5) 被驱动侧积分判据（Wayland 降级）===\n");
    otikm_slave_pos sp;
    otikm_slave_seed(&sp, 1920, 1080, OTI_EDGE_BIT(OTI_EDGE_LEFT));
    checks++;
    if (sp.x != 0 || sp.y != 540) {
        fails++;
        printf("  [FAIL] 进边校准：回程边=左 → 起点应为 (0,540)，实得 (%d,%d)\n", sp.x, sp.y);
    } else {
        printf("  [PASS] 进边校准：起点 (0,540)（回程边=左）\n");
    }

    /* 往里走（dx>0）不应该想回去 */
    for (int i = 0; i < 20; i++)
        otikm_slave_advance(&sp, 1920, 1080, 20, 0);
    checks++;
    if (otikm_slave_want_return(&sp, 4, OTI_EDGE_BIT(OTI_EDGE_LEFT))) {
        fails++;
        printf("  [FAIL] 往屏幕里走却判定要交还（x=%d over_l=%d）\n", sp.x, sp.over_l);
    } else {
        printf("  [PASS] 往屏幕里走 → 不交还（x=%d）\n", sp.x);
    }

    /* 一路推回左边缘：**到位**（x=0）但还没越过边界 → 仍不该交还 */
    for (int i = 0; i < 20; i++)
        otikm_slave_advance(&sp, 1920, 1080, -20, 0);
    checks++;
    if (sp.x != 0 || sp.over_l != 0 || otikm_slave_want_return(&sp, 4, OTI_EDGE_BIT(OTI_EDGE_LEFT))) {
        fails++;
        printf("  [FAIL] 推到边缘但未越界就交还（x=%d over_l=%d）\n", sp.x, sp.over_l);
    } else {
        printf("  [PASS] 推到边缘（x=0）但未越界 → 不交还\n");
    }

    /* 越过边界但**不足阈值**（阈值 = max(8, edge*4) = 16px）→ 防抖，不交还 */
    otikm_slave_advance(&sp, 1920, 1080, -8, 0);
    checks++;
    if (otikm_slave_want_return(&sp, 4, OTI_EDGE_BIT(OTI_EDGE_LEFT))) {
        fails++;
        printf("  [FAIL] 只外推 8px 就交还（阈值 16px，抖动会误触发）\n");
    } else {
        printf("  [PASS] 外推 8px → 还不交还（阈值 16px，防抖）\n");
    }
    for (int i = 0; i < 5; i++)
        otikm_slave_advance(&sp, 1920, 1080, -4, 0);
    checks++;
    if (!otikm_slave_want_return(&sp, 4, OTI_EDGE_BIT(OTI_EDGE_LEFT))) {
        fails++;
        printf("  [FAIL] 贴边继续外推 28px 却没判定交还（over_l=%d）\n", sp.over_l);
    } else {
        printf("  [PASS] 贴边继续外推 28px → 判定交还（over_l=%d）\n", sp.over_l);
    }

    /* 离开边缘后，陈旧的外推计数必须清零 */
    otikm_slave_advance(&sp, 1920, 1080, 300, 0);
    checks++;
    if (sp.over_l != 0 || otikm_slave_want_return(&sp, 4, OTI_EDGE_BIT(OTI_EDGE_LEFT))) {
        fails++;
        printf("  [FAIL] 离开边缘后 over_l 没清零（=%d）\n", sp.over_l);
    } else {
        printf("  [PASS] 离开边缘 → 外推计数清零\n");
    }

    /* 回程边是右时，往左推不能触发（只看配置那条边） */
    otikm_slave_seed(&sp, 1920, 1080, OTI_EDGE_BIT(OTI_EDGE_RIGHT));
    for (int i = 0; i < 50; i++)
        otikm_slave_advance(&sp, 1920, 1080, -20, 0);
    checks++;
    if (otikm_slave_want_return(&sp, 4, OTI_EDGE_BIT(OTI_EDGE_RIGHT))) {
        fails++;
        printf("  [FAIL] 回程边=右，却按左边判定要交还\n");
    } else {
        printf("  [PASS] 回程边=右 → 左边外推不触发（只认配置那条边）\n");
    }


    /* ---- 6) 角色协商（第 46 轮：键鼠换边）---- */
    printf("=== 6) 角色协商（键鼠插哪边都行）===\n");
    otikm_role_in ri;
    otikm_role_out ro;
#define RRESET() do { memset(&ri, 0, sizeof(ri)); ri.want = OTI_ROLE_AUTO; \
    ri.state = OTI_ROLE_NONE; ri.boot_id = 1000; ri.input_age_ms = 600000; \
    ri.peer_input_age_ms = 600000; ri.dwell_ms = 10000; ri.input_margin_ms = 2000; \
    ri.peer_absent_ms = 5000; ri.now_ms = 100000; ri.peer_ms = 100000; } while (0)

    /* a) 只有本机有键鼠 + 对端连续 3 次自称 slave → 我 master 且允许 grab */
    RRESET(); ri.has_local_input = 1; ri.state = OTI_ROLE_SLAVE;
    ri.peer_seen = 1; ri.peer_has_input = 0; ri.peer_state = OTI_ROLE_SLAVE; ri.peer_slave_streak = 3;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_state != OTI_ROLE_MASTER || !ro.grab_ok) { fails++; printf("  [FAIL] a) 只有本机有键鼠 → 应 master+grab（实得 state=%d grab=%d）\n", ro.want_state, ro.grab_ok); }
    else printf("  [PASS] a) 只有本机有键鼠 → master + grab_ok（%s）\n", ro.reason);

    /* b) I1：对端说 slave 但连续次数不足 → 不许 grab */
    RRESET(); ri.has_local_input = 1;
    ri.peer_seen = 1; ri.peer_state = OTI_ROLE_SLAVE; ri.peer_slave_streak = 1;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.grab_ok) { fails++; printf("  [FAIL] b) 对端只确认 1 次就 grab（违反 I1）\n"); }
    else printf("  [PASS] b) I1：连续确认不足 → 不 grab\n");

    /* c) I4：对端不在 → 允许单方面 master（零软件接收端/没跑代理的对端） */
    RRESET(); ri.has_local_input = 1; ri.peer_seen = 0;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_state != OTI_ROLE_MASTER || !ro.grab_ok || !ro.peer_absent) { fails++; printf("  [FAIL] c) 对端不在应单方面 master（实得 state=%d grab=%d absent=%d）\n", ro.want_state, ro.grab_ok, ro.peer_absent); }
    else printf("  [PASS] c) I4：对端不在 → 单方面 master（%s）\n", ro.reason);

    /* d) I2：双方都自称 master，对端 boot_id 更小 → 我必须让位且不许 grab */
    RRESET(); ri.has_local_input = 1; ri.state = OTI_ROLE_MASTER;
    ri.peer_seen = 1; ri.peer_state = OTI_ROLE_MASTER; ri.peer_has_input = 1; ri.peer_boot_id = 500;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (!ro.must_yield || ro.grab_ok) { fails++; printf("  [FAIL] d) 冲突应 boot_id 小者胜（实得 yield=%d grab=%d）\n", ro.must_yield, ro.grab_ok); }
    else printf("  [PASS] d) I2：冲突 → 让位（%s）\n", ro.reason);

    /* e) 双方都有键鼠、本机刚在用 → 我 master */
    RRESET(); ri.has_local_input = 1; ri.peer_seen = 1; ri.peer_has_input = 1;
    ri.peer_state = OTI_ROLE_SLAVE; ri.peer_slave_streak = 3;
    ri.input_age_ms = 100; ri.peer_input_age_ms = 9000;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_state != OTI_ROLE_MASTER) { fails++; printf("  [FAIL] e) 本机刚在用应 master（实得 %d）\n", ro.want_state); }
    else printf("  [PASS] e) 双方都有键鼠 → 刚在用的当 master（%s）\n", ro.reason);

    /* f) 双方都有键鼠、对端刚在用 → 我让位 */
    RRESET(); ri.has_local_input = 1; ri.state = OTI_ROLE_MASTER; ri.peer_seen = 1;
    ri.peer_has_input = 1; ri.peer_state = OTI_ROLE_SLAVE;
    ri.input_age_ms = 9000; ri.peer_input_age_ms = 100;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_state != OTI_ROLE_SLAVE || !ro.must_yield) { fails++; printf("  [FAIL] f) 对端刚在用应让位（实得 %d yield=%d）\n", ro.want_state, ro.must_yield); }
    else printf("  [PASS] f) 对端刚在用 → 让位（%s）\n", ro.reason);

    /* g) 滞回：刚进 master 才 1s（<dwell 10s），即便对端更近也不换（防抖） */
    RRESET(); ri.has_local_input = 1; ri.state = OTI_ROLE_MASTER; ri.peer_seen = 1;
    ri.peer_has_input = 1; ri.peer_state = OTI_ROLE_SLAVE;
    ri.input_age_ms = 9000; ri.peer_input_age_ms = 100;
    ri.now_ms = 100000; ri.state_since_ms = 99000;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_valid && ro.want_state != OTI_ROLE_MASTER) { fails++; printf("  [FAIL] g) dwell 内不该换边（实得 want=%d valid=%d）\n", ro.want_state, ro.want_valid); }
    else printf("  [PASS] g) 滞回：dwell 内保持（%s）\n", ro.reason);

    /* h) 显式 force slave：从 master 让位 */
    RRESET(); ri.has_local_input = 1; ri.state = OTI_ROLE_MASTER; ri.want = OTI_ROLE_FORCE_SLAVE;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_state != OTI_ROLE_SLAVE || !ro.must_yield) { fails++; printf("  [FAIL] h) force slave 应让位（实得 %d yield=%d）\n", ro.want_state, ro.must_yield); }
    else printf("  [PASS] h) --role slave → 让位\n");

    /* i) 对端显式要 master：自动的我让位 */
    RRESET(); ri.has_local_input = 1; ri.peer_seen = 1; ri.peer_has_input = 0;
    ri.peer_want = OTI_ROLE_FORCE_MASTER; ri.peer_state = OTI_ROLE_MASTER;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_state != OTI_ROLE_SLAVE) { fails++; printf("  [FAIL] i) 对端显式 master 我该让（实得 %d）\n", ro.want_state); }
    else printf("  [PASS] i) 对端显式 master → 我 slave（%s）\n", ro.reason);

    /* j) 双方都没有本机键鼠 → 都 slave + 明确原因 */
    RRESET(); ri.has_local_input = 0; ri.peer_seen = 1; ri.peer_has_input = 0;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_state != OTI_ROLE_SLAVE) { fails++; printf("  [FAIL] j) 双方都没键鼠应 slave（实得 %d）\n", ro.want_state); }
    else printf("  [PASS] j) 双方都没有键鼠 → slave + 提示（%s）\n", ro.reason);

    /* k) 只有对端有键鼠 → 我 slave 且永不 grab（L7） */
    RRESET(); ri.has_local_input = 0; ri.peer_seen = 1; ri.peer_has_input = 1; ri.peer_state = OTI_ROLE_MASTER;
    otikm_role_decide(&ri, &ro);
    checks++;
    if (ro.want_state != OTI_ROLE_SLAVE || ro.grab_ok) { fails++; printf("  [FAIL] k) 只有对端有键鼠应 slave 且不 grab（实得 %d grab=%d）\n", ro.want_state, ro.grab_ok); }
    else printf("  [PASS] k) 只有对端有键鼠 → slave（%s）\n", ro.reason);

    printf("\n=== 结果：%d 项检查，%d 项失败 ===\n", checks, fails);
    return fails ? 1 : 0;
}
