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
 *   6) 2026-11-06 第 61 轮：回程主判据改成"真实光标贴入口边 + 继续外推 ≥16
 *      未缩放计数"（1e~1j）。重点回归：20~50ms 一条遥测时，连续"在边"的遥测
 *      不能把外推计数清零（否则慢推永远回不来）；遥测过期时冻结判定、绝不
 *      退回"计数 100 就揪回"；从未收到 CURSOR 的旧对端才走兼容计数判据。
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

/* 带时间戳的推位移：用于"对端光标回传"测试（回传新鲜度按 now_ms 判定）。 */
static otikm_act push_at(otikm_core *c, int dx, int dy, int times, long t0, long step)
{
    oti_mouse_evt m;
    oti_switch_evt sw;
    otikm_act act = OTI_ACT_LOCAL;
    for (int i = 0; i < times; i++) {
        memset(&m, 0, sizeof(m));
        m.dx = (int16_t)dx;
        m.dy = (int16_t)dy;
        otikm_core_set_now(c, t0 + (long)i * step);
        act = otikm_core_local_mouse(c, &m, &sw);
        if (act == OTI_ACT_SWITCH_TO_REMOTE || act == OTI_ACT_SWITCH_TO_LOCAL)
            return act;
    }
    return act;
}

/* 小步推位移 + 周期性插入"仍在入口边"的 CURSOR 遥测。
   2026-11-06 起回程主判据依赖 20~50ms 一条的真实光标遥测；这里用它模拟
   "原生泵"节奏，回归点：连续多条"在边"遥测**不能**把外推计数清零
   （旧实现每条遥测都重锚 → 慢推永远攒不到 16 → 真机"回不来"）。
   tel_every=0 表示不发遥测（模拟遥测断档）。 */
static otikm_act push_step_tel(otikm_core *c, int dx, int dy, int times,
                               long t0, long step, int tel_every,
                               int tx, int ty, int tw, int th)
{
    oti_mouse_evt m;
    oti_switch_evt sw;
    otikm_act act = OTI_ACT_LOCAL;
    for (int i = 0; i < times; i++) {
        long t = t0 + (long)i * step;
        if (tel_every > 0 && i % tel_every == 0)
            otikm_core_set_remote_cursor(c, tx, ty, tw, th, t);
        memset(&m, 0, sizeof(m));
        m.dx = (int16_t)dx;
        m.dy = (int16_t)dy;
        otikm_core_set_now(c, t);
        act = otikm_core_local_mouse(c, &m, &sw);
        if (act == OTI_ACT_SWITCH_TO_REMOTE || act == OTI_ACT_SWITCH_TO_LOCAL)
            return act;
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
    expect(push(&c, 1, 0, 5), OTI_ACT_TO_REMOTE, "贴边抖动 5×1px → 仍不回程（阈值 100）");
    expect(push(&c, 2, 0, 60), OTI_ACT_SWITCH_TO_LOCAL, "慢推 2px/次 → 累计到阈值即回程");

    /* ---- 1c) 镜像手势：往对端里推多少，就得推回来多少（再多推一点才回程）---- */
    printf("=== 1c) 镜像手势：回程距离要与推入距离对称 ===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "先交出去");
    expect(push(&c, -20, 0, 5), OTI_ACT_TO_REMOTE, "往对端里面推 100px");
    expect(push(&c, 20, 0, 5), OTI_ACT_TO_REMOTE, "推回 100px（刚好回到入口边）→ 还不回程");
    expect(push(&c, 20, 0, 6), OTI_ACT_SWITCH_TO_LOCAL, "回到入口边后再推 120 计数 → 回程");

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
    expect(push(&c, 1, 0, 120), OTI_ACT_SWITCH_TO_LOCAL, "小步推回来 → 回程（与远端几何无关）");
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "再交出去");
    checks++;
    if (c.rem_w != 4480 || c.rem_w_pend != 0) {
        fails++;
        printf("  [FAIL] 新的远端几何应在这次交出去时生效（rem_w=%d pending=%d）\n", c.rem_w, c.rem_w_pend);
    } else {
        printf("  [PASS] pending 几何在下次交出去时生效（rem_w=%d，rem_x=%d）\n", c.rem_w, c.rem_x);
    }

    /* ---- 1e) 真实光标主判据（2026-11-06 第 61 轮重写）----
       主判据 = 远端光标被确认贴在入口边（±edge_px*4）**且**用户继续向入口边
       外推的未缩放位移累计 ≥ OTI_REM_CURSOR_PUSH(16)。
       回归点：20~50ms 一条遥测时，**连续"在边"的遥测不能把累计清零**，
       否则慢推（2px/次）永远攒不到 16 —— 真机现场"回不来"的核心回归。 */
    printf("=== 1e) 真实光标主判据：贴边 + 继续外推 16 计数才回（慢推跨遥测帧累计）===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE,
           "先交出去（Kylin 左出 → Windows 右入口边）");
    checks++;
    if (c.rem_entry_x != 1) { fails++; printf("  [FAIL] 远端入口边应为右(1)，实得 %d\n", c.rem_entry_x); }
    else printf("  [PASS] 远端入口边 = Windows 右边缘（rem_entry_x=1）\n");
    /* 本段第一帧遥测：hid_park 之后 Windows 光标贴在虚拟桌面右边缘（1920x1080）。 */
    otikm_core_set_now(&c, 1000);
    otikm_core_set_remote_cursor(&c, 1920 - 1, 540, 1920, 1080, 1000);
    /* 2 计数/次、10ms/次、每 3 次一条"仍在边"遥测（30ms ≈ 原生泵节奏）。
       16 计数会跨 3 条遥测 —— 旧实现每条都清零，永远回不来。 */
    expect(push_step_tel(&c, 2, 0, 7, 1010, 10, 3, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "贴边慢推 14 计数（跨 3 条遥测帧）→ 还不回");
    expect(push_step_tel(&c, 2, 0, 1, 1080, 10, 1, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "第 15 计数 → 还不回");
    expect(push_step_tel(&c, 2, 0, 1, 1090, 10, 1, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_SWITCH_TO_LOCAL, "第 16 计数 → 回程（连续遥测帧没有重锚清零）");

    /* ---- 1f) 贴边抖动、停在边上没继续外推 → 不回程 ---- */
    printf("=== 1f) 贴边抖动、或停在边上不再外推 → 不回 ===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "先交出去");
    otikm_core_set_now(&c, 1000);
    otikm_core_set_remote_cursor(&c, 1920 - 1, 540, 1920, 1080, 1000);
    {
        otikm_act a = OTI_ACT_LOCAL;
        for (int i = 0; i < 20; i++)
            a = push_step_tel(&c, (i & 1) ? 1 : -1, 0, 1, 1010 + i * 20, 20, 1,
                              1920 - 1, 540, 1920, 1080);
        expect(a, OTI_ACT_TO_REMOTE, "贴边 ±1 抖动 20 次（净 0）→ 不回");
    }
    expect(push_step_tel(&c, 0, 0, 30, 1500, 20, 5, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "光标停在入口边、用户没继续外推（dx=0）→ 不回");

    /* ---- 1g) 遥测过期不误回（2026-11-06 现场 bug：回传过期后净计数到 100 被揪回）----
       规则：只要曾经收到过 CURSOR，过期时**冻结**判定、绝不退回计数判据；
       累计计数保留，新遥测一到就按最新值判（用户已经推够 16 → 立即回）。 */
    printf("=== 1g) 遥测过期不误回；新遥测恢复后按累计外推立即判 ===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "先交出去");
    otikm_core_set_now(&c, 1000);
    otikm_core_set_remote_cursor(&c, 1920 - 1, 540, 1920, 1080, 1000);
    expect(push_step_tel(&c, 0, 0, 1, 1010, 10, 1, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "先消费一帧新鲜遥测（确认贴边，外推计数从 0 起算）");
    expect(push_step_tel(&c, 127, 0, 1, 2000, 10, 0, 0, 0, 0, 0),
           OTI_ACT_TO_REMOTE, "遥测过期 1000ms + 贴边推 127 计数 → 不误回（计数判据已禁用）");
    expect(push_step_tel(&c, 127, 0, 1, 2010, 10, 0, 0, 0, 0, 0),
           OTI_ACT_TO_REMOTE, "过期期累计 254 计数 → 仍不回（绝不退回计数判据）");
    expect(push_step_tel(&c, 1, 0, 1, 2020, 10, 1, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_SWITCH_TO_LOCAL, "新遥测确认在入口边 → 累计外推立即触发回程");

    /* 遥测恢复但光标已不在入口边 → 旧累计作废，必须重新走到边再攒 16（入口边锚定）*/
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "再交出去");
    otikm_core_set_now(&c, 1000);
    otikm_core_set_remote_cursor(&c, 1920 - 1, 540, 1920, 1080, 1000);
    expect(push_step_tel(&c, 0, 0, 1, 1010, 10, 1, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "先消费一帧新鲜遥测（确认贴边）");
    expect(push_step_tel(&c, 127, 0, 1, 2000, 10, 0, 0, 0, 0, 0),
           OTI_ACT_TO_REMOTE, "过期期推 127 计数（暂不判）");
    expect(push_step_tel(&c, 127, 0, 1, 2010, 10, 1, 1000, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "新遥测显示光标远离入口边 → 旧累计作废、不回");
    expect(push_step_tel(&c, 127, 0, 1, 2400, 10, 1, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "刚到边的那一拍不计入（127 计数不复用）");
    expect(push_step_tel(&c, 2, 0, 7, 2410, 10, 2, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "到边后重新攒 14 计数 → 还不回");
    expect(push_step_tel(&c, 2, 0, 1, 2490, 10, 1, 1920 - 1, 540, 1920, 1080),
           OTI_ACT_SWITCH_TO_LOCAL, "重新攒到 16 计数 → 回（入口边重锚生效）");

    /* ---- 1h) 零软件/旧对端兼容：从未收到过 CURSOR → 旧计数判据 ---- */
    printf("=== 1h) 零软件/旧对端兼容：从未有 CURSOR → 旧计数判据（阈值 100）===\n");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "零软件端：先交出去");
    expect(push_at(&c, 99, 0, 1, 5000, 10),
           OTI_ACT_TO_REMOTE, "从未有回传：净推回 99 计数 → 还不回（阈值 100）");
    expect(push_at(&c, 1, 0, 1, 5010, 10),
           OTI_ACT_SWITCH_TO_LOCAL, "第 100 计数 → 旧计数判据回程（兼容路径）");

    /* ---- 1i) 左/右两方向都要对（镜像方向也走真实光标判据）---- */
    printf("=== 1i) 右出 / 左回（Windows 左入口边），同样用真实光标判 ===\n");
    setup(&c);
    c.mx = 1700;
    expect(push(&c, 40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "本机往右推到边缘 → 交出去");
    checks++;
    if (c.rem_entry_x != 0) { fails++; printf("  [FAIL] 远端入口边应为左(0)，实得 %d\n", c.rem_entry_x); }
    else printf("  [PASS] 远端入口边 = Windows 左边缘（rem_entry_x=0）\n");
    otikm_core_set_now(&c, 1000);
    otikm_core_set_remote_cursor(&c, 0, 540, 1920, 1080, 1000);
    expect(push_step_tel(&c, -2, 0, 7, 1010, 10, 3, 0, 540, 1920, 1080),
           OTI_ACT_TO_REMOTE, "贴左边缘慢推 14 计数 → 还不回");
    expect(push_step_tel(&c, -2, 0, 2, 1080, 10, 1, 0, 540, 1920, 1080),
           OTI_ACT_SWITCH_TO_LOCAL, "累计到 16 计数 → 回程（镜像方向）");

    /* ---- 1j) 贴边几何用 CURSOR 载荷自带的虚拟桌面（现场 3584x1152），不用 HELLO ---- */
    printf("=== 1j) 贴边几何/容差：用 CURSOR 载荷的虚拟桌面（3584x1152）===\n");
    setup(&c);                                   /* HELLO 初值 1920x1080 */
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "先交出去");
    otikm_core_set_now(&c, 1000);
    otikm_core_set_remote_cursor(&c, 3583, 540, 3584, 1152, 1000);
    expect(push_step_tel(&c, 2, 0, 10, 1010, 10, 4, 3583, 540, 3584, 1152),
           OTI_ACT_SWITCH_TO_LOCAL,
           "按 3584 右边缘判贴边 → 攒 16 计数回程（若用 HELLO 1920 会把 3583 当越界）");
    /* 容差 = edge_px*4 = 16 远端像素：离边 16px 算贴边，17px 不算 */
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "再交出去");
    otikm_core_set_now(&c, 1000);
    otikm_core_set_remote_cursor(&c, 3584 - 1 - 16, 540, 3584, 1152, 1000);
    expect(push_step_tel(&c, 2, 0, 10, 1010, 10, 4, 3584 - 1 - 16, 540, 3584, 1152),
           OTI_ACT_SWITCH_TO_LOCAL, "离边 16px（容差内）→ 算贴边，攒 16 计数可回");
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "再交出去");
    otikm_core_set_now(&c, 1000);
    otikm_core_set_remote_cursor(&c, 3584 - 1 - 17, 540, 3584, 1152, 1000);
    expect(push_step_tel(&c, 127, 0, 4, 1010, 10, 1, 3584 - 1 - 17, 540, 3584, 1152),
           OTI_ACT_TO_REMOTE, "离边 17px（容差外）→ 推 508 计数也不回（真实光标不到边）");

    /* 从未收到回传（零软件端/旧 agent）→ 旧计数判据仍是唯一回程方式 */
    setup(&c);
    expect(push(&c, -40, 0, 10), OTI_ACT_SWITCH_TO_REMOTE, "零软件端：先交出去");
    expect(push_at(&c, 127, 0, 1, 5000, 10),
           OTI_ACT_SWITCH_TO_LOCAL, "从未有回传 → 旧计数判据仍能回程（兼容路径）");

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
