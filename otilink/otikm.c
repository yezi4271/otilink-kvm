/*
 * otikm.c —— 键鼠共享 daemon（KVM over OTi 对拷线）
 *
 * 策略（have_control ≡ 指针在本机）：
 *   指针在本机  → 本地键鼠照常（不抓取、不转发）；收到对端注入的事件注入本地
 *   指针在对端  → 本机是驱动侧：EVIOCGRAB 独占本地键鼠并全部转发；热键可拉回
 *
 * 传输后端：
 *   --transport cable[:/dev/sgN]    真实线缆（SCSI 私有命令）
 *   --transport listen:/path        本机集成测试用：AF_UNIX 服务端
 *   --transport connect:/path       本机集成测试用：AF_UNIX 客户端
 *
 * 用法（测试模式）：
 *   ./otikm --transport listen:/tmp/s --sim-input a.txt --sim-inject a.out --duration 3 --log a.log
 *   ./otikm --transport connect:/tmp/s --sim-input b.txt --sim-inject b.out --duration 3 --log b.log
 *   sim-input 每行： key <code> <value> | mouse <dx> <dy> <wheel> <buttons> | sleep <ms>
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "oticlip.h"
#include "otihid.h"
#include "otix11.h"
#include "otiinput.h"
#include "otikm_core.h"
#include "otilink.h"
#include "otiproto.h"
#include "otitrans.h"
#include "otixfer.h"
#include "version.h"

#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

struct cfg {
    const char *transport;
    const char *capture[8];
    int         ncapture;
    int         grab;
    int         inject;
    const char *sim_input;
    const char *sim_inject;
    const char *log;
    int         screen_w, screen_h, edge, hotkey, hotkey_only;
    int         duration_s, delay_ms, verbose;
    int         idle_release_ms;      /* 看门狗：对端多久不消费就交还控制权（0=关） */
    /* 切换策略（对标 PowerToys Mouse Without Borders 的 Easy Mouse） */
    unsigned    edges;                /* 允许撞哪些边交出去（OTI_EDGE_* 掩码）；0=只能热键 */
    uint8_t     switch_mods;          /* 撞边需按住的修饰键（0=直接切） */
    int         corner_px;            /* 角部死区 px（防误切） */
    char        hotkey_toggle[64];
    char        hotkey_to_remote[64];
    char        hotkey_to_local[64];
    char        hotkey_lock[64];
    /* 剪贴板 */
    int         clipboard;
    const char *clip_file;
    int         clip_interval_ms;
    size_t      clip_max;
    int         doctor;
    int         cable_init;
    /* 被控端零软件模式：把键鼠直接发成 HID 包（0xD9/0x33|0x34） */
    int         peer_hid;
    int         return_on_edge;   /* 被驱动侧：撞边=把控制权还给对端（不反过来接管） */
    unsigned    return_edge;      /* 被驱动侧**只看这一条边**交还（默认左：朝向 Windows 那侧） */
    int         hid_kbd_layout;
    int         hid_mouse_layout;
    int         hid_probe;
    int         keepalive_ms;      /* >0 = 周期性保活 */
    int         selftest_seconds;  /* 输入自检时长 */
    const char *selftest_input;    /* synth / live（非空则只做输入自检） */
    /* ---- 绿色包/免安装（全部 opt-in：不传就与以前逐字节同行为）----------------
     * 这些参数把 run-kylin.sh 里的"设备发现 + 角色判定"搬进 C：
     * 那份 shell 依赖 /dev/input/by-id 命名与硬编码 /dev/sg3，换发行版/线缆固件就失效。 */
    int         version;           /* --version：打印版本/基线/特性后退出（不碰设备） */
    int         auto_mode;         /* --auto：自动判定传输/角色/设备/屏幕/剪贴板 */
    int         role;              /* 1=master 2=slave 0=未定（--role） */
    int         set_role;          /* 用户显式给了 --role（显式优先于 --auto 判定） */
    int         role_explicit;     /* --role master|slave（真·显式）；--role auto 记 0 */

    int         set_screen;        /* 用户显式给了 --screen（含 auto） */
    int         screen_auto;       /* --screen auto 或 --auto */
    int         cursor_source;     /* 被驱动侧光标来源：0=auto 1=x11 2=integrate */
    int         no_clip;           /* --no-clipboard：即使 --auto/配置打开也关掉 */
    int         no_grab;           /* --no-grab：主控侧不独占本地键鼠（试跑/自检用） */
    int         print_plan;        /* --print-plan：只打印决策，不建立链路（门禁/容器可用） */
    int         set_keepalive;     /* 用户显式给了 --keepalive（覆盖 --auto 的默认保活） */
    int         vendor_notify;     /* 撞边时是否给"厂商 Windows 端"发 XML 通知（默认关！见 §49） */
    int         edge_dwell_ms;     /* 被驱动侧交还手势的"贴边停留"要求（默认 0 = 立即，见 §50） */
    /* 角色协商（第 46 轮：键鼠插哪边都行） */
    long        role_dwell_ms;     /* 换边后最小驻留（默认 10000，防抖） */
    long        role_margin_ms;    /* 双方都有键鼠时 input_age 领先阈值（默认 2000） */
    long        role_absent_ms;    /* 多久没收到对端 ROLE 算"不在"（默认 5000，I4 兜底） */
};

struct app {
    struct cfg cfg;
    otikm_core core;
    oti_transport *tx;
    oti_transport *tx_old;        /* 上一代传输（重开后延迟关闭，避免 use-after-free） */
    pthread_mutex_t lock;
    FILE *logf, *sim_out;
    struct oti_inject inj;
    int   use_uinput;
    struct oti_capture cap[8];
    int   ncap;
    uint32_t seq;
    unsigned remote_w, remote_h;      /* 对端屏幕尺寸（HELLO 上报） */
    int   mouse_native;               /* 1 = 发相对位移（走对端系统指针速度）；0 = 发绝对坐标 */
    long  drive_started_ms;           /* 进入"驱动侧"的时刻（看门狗基准） */
    /* 鼠标位移合并：鼠标事件率(数百~上千 Hz)远高于线缆帧率，
       逐个事件发一整帧 64KB 会排队 → 表现为"慢、有惯性、停不下来"。
       这里按固定间隔(4ms)把累积位移合成一条消息发出去。 */
    int   pend_dx, pend_dy, pend_wheel, pend_buttons;
    int   pend_abs_x, pend_abs_y, pend_dirty, sent_buttons;
    long  last_mouse_tx_ms;
    int abs_last_x, abs_last_y, abs_have;  /* 收到绝对坐标时换算相对位移用 */
    long hello_last_ms;               /* 上次发 HELLO 的时间 */
    struct oti_clip clip;
    /* 剪贴板发送确认（ACK+重发）：帧通道会静默丢帧，没有确认就没有重传 */
    int      ack_active;
    uint32_t ack_crc;
    uint16_t ack_fmt;
    uint8_t *ack_buf;
    size_t   ack_len;
    long     ack_deadline_ms;
    int      ack_tries;
    unsigned long n_ack_ok, n_ack_retry, n_ack_fail;
    /* 大文件流式传输（见 otixfer.h）：tx 归剪贴板线程、rx 归收包线程 */
    struct oti_xfer_tx xfer_tx;
    struct oti_xfer_rx xfer_rx;
    pthread_mutex_t loglock;
    volatile int running;
    unsigned long n_sent, n_injected, n_switches, n_keepalive, n_ping_recv;
    struct otihid_kbd kbd;
    uint16_t mouse_buttons;
    unsigned long n_hid;
    int         hid_fail_streak;      /* HID 包连续写失败次数（链路故障信号） */
    long        hid_last_ok_ms;
    int         send_fail_streak;     /* W1b：连续发送失败次数（只用于日志节流） */
    int         send_fail_count;      /* W1b：当前统计窗口内发送失败次数（看门狗判据） */
    int         send_ok_count;        /* W1b：当前统计窗口内发送成功次数 */
    long        send_win_ms;          /* W1b：统计窗口起点（只有 RX 线程写） */
    int         send_reset_tries;     /* W5：连续"写侧故障窗口"次数（2 次就升级到 USB 复位） */
    long        send_hint_ms;         /* W5：无权限提示的节流时刻 */
    long        last_return_ms;      /* 被驱动侧撞边交还的节流时刻 */
    volatile int want_vendor_return; /* 撞边后请求主线程去通知厂商端（避免与消息泵抢授权） */
    volatile int want_token;         /* 请求消息泵线程发 F24 回程令牌（捕获线程自己发会被吞） */
    volatile int want_release_all;   /* 请求消息泵线程发"全零释放包"（切回本地前，先于令牌） */
    long        edge_at_ms;          /* 光标停在边缘区的起始时刻（真实坐标判据） */
    /* 被驱动侧的降级光标判据（Wayland 没有全局指针 API）：进边校准 + 位移积分 + 边界夹紧。
       slave_over = "贴边后继续外推了多少像素"（>= max(8, edge*4) 才算交还），
       这样即使积分漂移，判据也只取决于"用户确实在往外推"。 */
    otikm_slave_pos spos;            /* 积分坐标 + 各边外推累计（纯逻辑在 otikm_core.c） */
    int         slave_seeded;        /* 积分坐标是否已校准 */
    int         use_integrate;       /* 运行期：已降级为积分判据 */
    uint8_t     slave_mods;          /* 被驱动侧回程热键的修饰键状态 */
    /* ---- 角色协商（第 46 轮）----
       RX 线程只"决策并排队"（role_pending）；**抓取线程**是 capture fd 的唯一所有者，
       由它执行设备重建与 grab 变更 —— 避免两个线程同时 close/open 同一批 fd 的竞态。 */
    int      has_local_input;    /* 本机有线缆之外的键鼠 */
    uint32_t boot_id;            /* 本机启动标识低 32 位（平票仲裁用） */
    long     input_last_ms;      /* 本机最近一次真实键鼠事件（master 模式才有意义） */
    int      role_state;         /* 我当前角色 OTI_ROLE_NONE/MASTER/SLAVE */
    int      applied_role;       /* 抓取设备实际对应的角色 */
    long     role_state_ms;      /* 进入当前角色时刻（滞回） */
    long     role_tx_ms;         /* 上次广播 ROLE */
    int      held_grab;          /* 当前是否持有 EVIOCGRAB（上报给对端） */
    long     local_probe_ms;     /* 上次重采样"本机有无键鼠"（热插拔自愈，见 rx_thread） */
    int      local_count;        /* 上次采到的"本机键鼠"个数（增删都要重扫抓取集合） */
    volatile int cap_rescan;     /* 抓取集合需要重扫（热插拔：插了键盘/鼠标也要抓进来） */
    long     drive_log_ms;       /* 回程判据诊断日志节流 */
    int      kbd_grab_warned;    /* "键盘不独占"只提示一次 */
    long     grab_maint_ms;      /* 驱动期"维持独占"重放的节流（判据会变，见 apply_grab） */
    long     grab_err_log_ms;    /* 独占失败告警节流（维持重放反复失败时别刷屏） */
    int      role_pending, role_want_state, role_grab_ok, role_yield, role_logged_state;
    int      per_seen, per_has_input, per_want, per_state, per_flags, per_slave_streak;
    long     per_ms;
    uint32_t per_boot_id, per_input_age_ms;
};

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void say(struct app *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&a->loglock);
    if (a->logf) {
        /* 打时间戳：跨线程（收包/剪贴板/主）的时序问题只能靠它对齐
           （实测：大文件传输"到底卡在哪一段"就是靠这个定位的） */
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long t = (long)ts.tv_sec % 100000;
        fprintf(a->logf, "[%05ld.%03ld] ", t, ts.tv_nsec / 1000000);
        vfprintf(a->logf, fmt, ap);
        fputc('\n', a->logf);
        fflush(a->logf);
    }
    if (a->cfg.verbose) {
        va_list ap2;
        va_start(ap2, fmt);
        vfprintf(stderr, fmt, ap2);
        fputc('\n', stderr);
        va_end(ap2);
    }
    pthread_mutex_unlock(&a->loglock);
    va_end(ap);
}

/* ---------- 回程令牌 / 释放包：必须由消息泵线程发 ----------
 * 捕获线程（鼠标/键盘事件回调）**不能**自己去写设备：它跟 RX 消息泵线程共用
 * 同一条帧管道，泵线程正在连续读授权消息（0x06/0x07）与应答，两边同时收发会
 * 把对方的应答吃掉 —— 实测表现就是"麒麟拼命喊交还，Windows 端一声不吭"：
 * 令牌写进去了却被吞，指针永远回不来（两端都复现过）。
 * 所以捕获线程只打标记，由泵线程在下一轮优先发出去。 */
static void request_token(struct app *a)
{
    a->want_token = 1;
}

static void request_release_all(struct app *a)
{
    a->want_release_all = 1;     /* 顺序由泵线程保证：先松手，再发令牌 */
}

/* ---------- KM 载体健康度：决定"纯键盘能不能独占" ----------
   ⚠️ 判据必须对着**键鼠真正走的那条通道**，不能对着无关的通道（2026-10-08 现场 bug）。

   * HID 直发模式（键鼠在麒麟、对端零软件）：键鼠走线缆 HID 包，**帧管道本来就该静默**
     （对端没有我们的软件，没人回帧/回授权）。以前这里拿"帧管道健康"（per_seen + 5s 内
     有成功帧写）当判据 → 纯键盘永远拿不到独占 → 用户接管对端时，同一个按键**既发到
     Windows、又落在麒麟本机桌面**（用户报障原话："我在 windows 上打字，kylin 也在同步
     打字"）。HID 模式下改用 **HID 包写失败计数**（与 watchdog_check 同一个判据）。
   * 协议模式（两端都有软件）：保持原判据不变。 */
static int km_path_ok(struct app *a)
{
    if (a->cfg.peer_hid)
        return a->hid_fail_streak < 20;          /* HID 包连续写失败 = KM 通道不健康 */
    long ltx0 = a->tx ? oti_tr_cable_last_tx_ms(a->tx) : 0;
    return a->per_seen && ltx0 && (now_ms() - ltx0 < 5000);
}

/* ---------- 抓取策略：只有驱动侧(指针在对端)才独占本地键鼠 ---------- */
static void apply_grab(struct app *a, int want)
{
    if (!a->cfg.grab) {
        a->held_grab = 0;
        return;
    }
    for (int i = 0; i < a->ncap; i++) {
        struct oti_capture *c = &a->cap[i];
        if (c->fd < 0)
            continue;
        /* ⚠️ 纯键盘的独占是**有条件的**（两个方向都踩过）：
           ① 2026-09-21：独占着，但按键送不出去 → 用户打字全丢、键盘像"死了"；
           ② 2026-10-08：不敢独占 → 接管对端时**本地与远端双重输入**（用户报障）。
           所以判据 = KM 载体是否真的健康（km_path_ok）：健康就独占（防双重输入），
           不健康就放开（防锁死），并由下面的"维持/安全网"随健康度变化实时切换。
           纯键盘判据：有键盘能力、没有鼠标能力（G304 那种键鼠合一接收器仍会被独占）。 */
        if (want && (c->kind & OTI_IN_KBD) && !(c->kind & OTI_IN_MOUSE) && !km_path_ok(a)) {
            if (!a->kbd_grab_warned) {
                a->kbd_grab_warned = 1;
                say(a, "键盘不独占（%s —— 独占会把用户锁死）",
                    a->cfg.peer_hid ? "HID 包连续写失败，KM 通道不健康"
                                    : "帧管道未确认健康：对端 agent 不在或 5 秒无成功发送");
            }
            continue;
        }
        int cur = c->grabbed;
        int w = want && !cur, u = !want && cur;
        if (w) {
            if (ioctl(c->fd, EVIOCGRAB, 1) == 0) {
                c->grabbed = 1;
                a->held_grab = 1;
                say(a, "已独占 %s（驱动侧，避免本地与远端双重输入）", c->devname);
            } else {
                /* 抓不住会导致同一按键既本地生效又转发对端：必须显式告警。
                   节流 30s：驱动期会周期性"维持独占"重放（见 run_capture），
                   真的抓不住时不能每 500ms 刷一行日志。 */
                long now = now_ms();
                if (now - a->grab_err_log_ms > 30000) {
                    a->grab_err_log_ms = now;
                    say(a, "ERR 独占 %s 失败: %s —— 会出现双重输入，请检查权限/是否被他人占用",
                        c->devname, strerror(errno));
                }
            }
        } else if (u) {
            if (ioctl(c->fd, EVIOCGRAB, 0) == 0) {
                c->grabbed = 0;
                say(a, "已释放 %s（键鼠交还本机桌面）", c->devname);
            }
        }
    }
    a->held_grab = 0;
    for (int i = 0; i < a->ncap; i++)
        if (a->cap[i].grabbed)
            a->held_grab = 1;
}

/* ---------- 角色协商（第 46 轮）：抓取线程侧 ----------
   只有这个线程能 close/open capture fd（L3 同款纪律：一个 fd 一个主人）。
   角色切换 = 按目标角色重建抓取设备集合 + 切换 return_on_edge + 交还/接管。 */
static void role_rebuild_capture(struct app *a, int want_master)
{
    for (int i = 0; i < a->ncap; i++)
        oti_capture_close(&a->cap[i]);
    a->ncap = 0;
    static char kbd[64], mou[64];
    int n = oti_input_autoselect(want_master ? 0 : 1, kbd, mou);
    if (mou[0]) {
        if (oti_capture_open(&a->cap[a->ncap], mou, 0) == 0) {
            say(a, "角色(%s)：抓取 %s (%s)", want_master ? "master" : "slave", mou,
                a->cap[a->ncap].devname);
            a->ncap++;
        }
    }
    if (kbd[0] && a->ncap < 8) {
        if (oti_capture_open(&a->cap[a->ncap], kbd, 0) == 0) {
            say(a, "角色(%s)：抓取 %s (%s)", want_master ? "master" : "slave", kbd,
                a->cap[a->ncap].devname);
            a->ncap++;
        }
    }
    a->cfg.return_on_edge = want_master ? 0 : 1;
    if (!n)
        say(a, "[warn] 角色切换后看不到 %s 设备（--doctor 看输入设备）",
            want_master ? "本机键鼠" : "线缆 HID");
}

static void role_apply_pending(struct app *a)
{
    if (!a->role_pending)
        return;
    a->role_pending = 0;
    int want = a->role_want_state;
    if (a->role_yield) {
        say(a, "ROLE 让位：释放独占 + 释放按键 + 回程令牌（交给对端）");
        apply_grab(a, 0);
        request_release_all(a);
        request_token(a);
    }
    if (want != a->applied_role) {
        role_rebuild_capture(a, want == OTI_ROLE_MASTER);
        a->applied_role = want;
        a->role_state_ms = now_ms();
        if (want == OTI_ROLE_MASTER) {
            int px, py;
            pthread_mutex_lock(&a->lock);
            a->core.have_control = 1;          /* 指针归我：本地原生用，推到边缘才交出去 */
            if (otix11_pos(&px, &py) == 0) { a->core.mx = (int16_t)px; a->core.my = (int16_t)py; }
            pthread_mutex_unlock(&a->lock);
            say(a, "ROLE 生效：master（键鼠在本机，抓取已切到本机设备）");
        } else {
            pthread_mutex_lock(&a->lock);
            otikm_core_force_local(&a->core);
            pthread_mutex_unlock(&a->lock);
            say(a, "ROLE 生效：slave（被驱动侧：只监听线缆 HID，撞边交还）");
        }
    }
    a->role_state = want;
}

/* W1b：写侧看门狗计数。并发 ++ 只影响阈值精度（良性），真正的"重开"由 RX 线程独占执行。 */
static void note_send_result(struct app *a, int rc)
{
    if (rc) {
        a->send_fail_streak++;        /* 连续失败：一次成功就清零（日志节流用） */
        a->send_fail_count++;         /* 窗口内失败数：**不能**被偶发成功清零 —— 真机实测是
                                         "大部分写失败、偶尔成功"，连续计数永远到不了阈值 */
    } else {
        a->send_fail_streak = 0;
        a->send_ok_count++;
    }
}

static void send_body(struct app *a, const uint8_t *buf, size_t n, const char *what)
{
    if (!n)
        return;
    int rc = oti_tr_send(a->tx, buf, n);
    note_send_result(a, rc);
    if (rc) {
        /* 节流：故障时这里的调用很密（ROLE 1/s + PING + 剪贴板），不能每帧一行刷满日志 */
        if (a->send_fail_streak <= 3 || (a->send_fail_streak % 20) == 0)
            say(a, "ERR send %s rc=%d（连续 %d 次）", what, rc, a->send_fail_streak);
    } else {
        a->n_sent++;
        if (a->cfg.verbose)
            say(a, "SENT %s", what);
    }
}

/* ---------- 角色协商（第 46 轮）：采样与广播 ----------
   决策是纯函数（otikm_core.c 的 otikm_role_decide，coretest 覆盖）；这里只做采样/广播/排队。 */
static uint32_t role_boot_id(void)
{
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    unsigned v = 0;
    if (f) {
        if (fscanf(f, "%8x", &v) != 1)
            v = 0;
        fclose(f);
    }
    if (!v)
        v = (uint32_t)time(NULL);      /* 兜底：仍能当稳定仲裁值 */
    return v;
}

static uint32_t role_input_age(struct app *a)
{
    /* "从未用过"必须给 **uint32 最大值**，不能用 600000：
       否则一台很久没用的机器（真实 age 会涨到 >600000）反而显得"更近"，
       会把真正在用的那一端的主控抢走 —— 真机日志实测到两端来回翻
       （Windows 侧已改用同一个哨兵值，两端必须一致）。 */
    if (!a->input_last_ms)
        return 0xFFFFFFFFu;
    long d = now_ms() - a->input_last_ms;
    if (d < 0)
        return 0;
    if (d > 0xFFFFFFFFL)
        return 0xFFFFFFFFu;
    return (uint32_t)d;
}

static int role_want_of(struct app *a)
{
    /* --role auto（或未给 --role）→ AUTO：由协商按"双方谁有本机键鼠"决定。
       ⚠️ 不能用 set_role 单独判"显式"：--role auto 也置 set_role，而 resolve_auto 会把
       cfg.role 从 0 引导成 1/2；若照它返回 FORCE_*，--role auto 就变成"单方面强制"，
       与"换边协商"直接冲突（两端都会抢主控）。 */
    if (!a->cfg.role_explicit)
        return OTI_ROLE_AUTO;
    if (a->cfg.role == 1)
        return OTI_ROLE_FORCE_MASTER;
    if (a->cfg.role == 2)
        return OTI_ROLE_FORCE_SLAVE;
    return OTI_ROLE_AUTO;
}

static void maybe_send_role(struct app *a, int force)
{
    long now = now_ms();
    if (!force && now - a->role_tx_ms < 1000)
        return;
    a->role_tx_ms = now;
    uint8_t buf[64];
    oti_role_evt e;
    memset(&e, 0, sizeof(e));
    e.has_local_input = a->has_local_input ? 1 : 0;
    e.want = (uint8_t)role_want_of(a);
    e.state = (uint8_t)a->role_state;
    e.flags = a->held_grab ? OTI_ROLE_FLAG_GRABBED : 0;
    e.boot_id = a->boot_id;
    e.input_age_ms = role_input_age(a);
    size_t n = oti_encode_role(++a->seq, &e, buf, sizeof(buf));
    if (n)
        send_body(a, buf, n, "ROLE");
}

/* RX 线程调用：算一次决策并排队给抓取线程（不在这里碰 capture fd） */
static void role_decide_and_queue(struct app *a)
{
    otikm_role_in in;
    otikm_role_out ro;
    memset(&in, 0, sizeof(in));
    in.has_local_input = a->has_local_input;
    in.want = role_want_of(a);
    in.state = a->role_state;
    in.state_since_ms = a->role_state_ms;
    in.boot_id = a->boot_id;
    in.input_age_ms = role_input_age(a);
    in.peer_seen = a->per_seen;
    in.peer_ms = a->per_ms;
    in.peer_has_input = a->per_has_input;
    in.peer_want = a->per_want;
    in.peer_state = a->per_state;
    in.peer_flags = a->per_flags;
    in.peer_boot_id = a->per_boot_id;
    in.peer_input_age_ms = a->per_input_age_ms;
    in.peer_slave_streak = a->per_slave_streak;
    in.now_ms = now_ms();
    in.dwell_ms = a->cfg.role_dwell_ms;
    in.input_margin_ms = a->cfg.role_margin_ms;
    in.peer_absent_ms = a->cfg.role_absent_ms;
    otikm_role_decide(&in, &ro);
    int prev_ok = a->role_grab_ok;
    a->role_grab_ok = ro.grab_ok;
    if (!ro.want_valid)
        return;
    a->role_want_state = ro.want_state;
    a->role_yield = ro.must_yield;
    a->role_pending = 1;
    if (a->role_logged_state != ro.want_state || prev_ok != ro.grab_ok) {
        a->role_logged_state = ro.want_state;
        say(a, "ROLE 决策: %s（%s）对端 state=%d hasIn=%d age=%u 我=%u grab=%s",
            ro.want_state == OTI_ROLE_MASTER ? "master" : "slave", ro.reason,
            a->per_state, a->per_has_input, a->per_input_age_ms, role_input_age(a),
            ro.grab_ok ? "可" : "否");
    }
}

/* W4：对端"静默"判定 —— 收到过对端消息、但已经超过 role_absent_ms 没有任何消息。
   为什么要：MASTER 把键鼠交出去之前先确认对端还活着；否则会交给一个不回应的对端
   （用户看到的就是"鼠标消失在对端、回不来"，只能靠热键）。
   注意：**从没收到过对端消息**时返回 0（不拦）—— 那是"零软件接收端/Mac"的正常形态（I4）。 */
static int peer_is_silent(struct app *a)
{
    /* ⚠️ HID 直发模式（对端允许是**零软件**接收端）绝不能用"对端发没发消息"判存活：
       那种对端根本不会说话，帧管道本来就静默 —— 真机症状是
       `[warn] 对端已 N ms 没有任何消息 → 不交出去（W4）`，指针**永远过不去 Windows**
       （用户报障原话："linux 上的鼠标移动不到 windows"；日志里交接动作其实已经发生，
       又被 W4 挡回去了）。HID 模式下链路活不活由写失败判据负责：
       hid_fail_streak 连续 20 次写失败会自动交还本机（见 watchdog_check）。 */
    if (a->cfg.peer_hid)
        return 0;
    if (!a->per_seen)
        return 0;
    return (now_ms() - a->per_ms) > a->cfg.role_absent_ms;
}

/* ⚠️ Kylin→Windows 的 HID 键盘通道有**线缆固件缺陷**（NOTES §61，2026-09-21 实测）：
   无修饰键报表（F24/字母）能到，但**修饰键字节一发出，之后的键盘报表全部丢失**，
   Windows 上 Ctrl 会永久卡住；且该接口只在前几条报表内可靠。
   在"键盘改走帧管道 + Windows agent SendInput"落地之前，**暂时关闭 HID 键盘转发**：
   本地键盘保持可用（也不独占），不会出现"按了键两边都不动"的锁死，也不会在对端卡修饰键。
   要临时打开做实验：把下面改成 1。 */
#define OTI_HID_KBD_FORWARD 1

/* HID 直发模式下发一个键盘事件（复用 otihid 的整份报表状态机）。
   抽出来是因为**抑制缓冲补发**（见 handle_local）也要走同一条路。 */
static void hid_send_state(struct app *a, struct otilink_dev *cd);

static void hid_send_key(struct app *a, struct otilink_dev *cd, uint16_t code, uint8_t value)
{
    uint8_t pkt[OTI_HID_PKT_LEN];
    if (!cd)
        return;
    if (!otihid_kbd_apply(&a->kbd, code, value))
        return;
    /* 线缆 HID 通道**会丢包**（帧通道实测丢 40%，F24 令牌因此本来就连发 3 轮 + 15ms 间隔）。
       键盘是**状态报表**：同一份报表重复发是幂等的，所以每次状态变化连发 3 遍、
       间隔 10ms（Windows 按轮询间隔取"当前报表"，间隔太小会被合并）。
       真机实测（2026-09-21，kmbret）：只发一遍时 Ctrl 到了、C 经常到不了（键盘像失灵）；
       连发 3 遍后稳定。**鼠标位移绝不能照做**（相对位移重复发 = 移动量翻倍）。 */
    hid_send_state(a, cd);
}

/* 把当前键盘状态整份发出去（3 遍，同报表幂等）。合并批次时用：修饰键+按键必须
   在**同一份报表**里到达（真机实测：分两条发时，第二条会被线缆键盘接口吞掉）。 */
static void hid_send_state(struct app *a, struct otilink_dev *cd)
{
    uint8_t pkt[OTI_HID_PKT_LEN];
    if (!cd)
        return;
    otihid_kbd_report(&a->kbd, a->cfg.hid_kbd_layout, pkt);
    int hrc = 0;
    for (int i = 0; i < 3; i++) {
        if (i)
            usleep(10 * 1000);
        hrc = otilink_send_hid(cd, OTI_HID_TYPE_KBD, pkt);
        note_send_result(a, hrc);
        if (hrc == 0) {
            a->n_hid++;
            a->hid_fail_streak = 0;
            a->hid_last_ok_ms = now_ms();
        } else if (++a->hid_fail_streak >= 20) {
            break;
        }
    }
}

/* 回程落点：把指针放在"当初出去那条边的往里 edge+8"处（另一轴保持 core 的坐标）。
   为什么不能用真实光标直接校准：grab 期间真实光标**一动不动**，回程时它通常还贴在
   出口边上 —— 拿它校准等于把指针放回边缘，用户轻轻一碰（甚至回程手势的惯性尾巴）
   就又被推出去（真机日志：拉回本机后 0.2s 又"交给对端"）。
   调用方随后用 otix11_warp() 把真实光标也挪到同一处，core 与真实桌面不再打架。 */
static void return_landing_xy(const otikm_core *c, int *ox, int *oy)
{
    int m = c->edge_px + 8;
    int x = c->mx, y = c->my;
    switch (c->edge) {
    case OTI_EDGE_RIGHT:  x = c->screen_w - 1 - m; break;
    case OTI_EDGE_LEFT:   x = m; break;
    case OTI_EDGE_TOP:    y = m; break;
    default:              y = c->screen_h - 1 - m; break;   /* BOTTOM（含默认值） */
    }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > c->screen_w - 1) x = c->screen_w - 1;
    if (y > c->screen_h - 1) y = c->screen_h - 1;
    *ox = x;
    *oy = y;
}

/* 交接时把对端光标"停"在入口边。HID 鼠标只有相对位移、不能直接定位，但可以用一段
   **饱和位移**把它顶到那条边上。
   为什么必须做（2026-09-21 真机）：状态机里的"入口边"是从**零位移**起算的，而对端光标的
   真实位置是上次用完留在那里的；更糟的是用户可能在对端把光标顶在屏幕边上继续推
   （位移被系统夹住、画面不动），这些"看不见的位移"全记进 drv_*：之后用户原路推回来
   也到不了阈值 —— 表现就是"怎么推都回不来"。顶到入口边后，"推出去多少推回来多少"
   的镜像判据就与用户眼睛看到的一致了（协议/绝对坐标路径本来就是这个语义）。
   数量：覆盖远端虚拟桌面（未知按 4480 兜底），上限 64 包（8128 计数）。 */
static void hid_park_remote_pointer(struct app *a, struct otilink_dev *cd,
                                   int entry_x, int entry_y)
{
    int rw = a->core.rem_w > 0 ? a->core.rem_w : 4480;
    int rh = a->core.rem_h > 0 ? a->core.rem_h : 1440;
    int dx = 0, dy = 0;
    if (entry_x == 0) dx = -(rw + 128);
    else if (entry_x == 1) dx = rw + 128;
    if (entry_y == 0) dy = -(rh + 128);
    else if (entry_y == 1) dy = rh + 128;
    if (!dx && !dy)
        return;
    uint8_t pkt[OTI_HID_PKT_LEN];
    int sent = 0;
    while ((dx || dy) && sent < 64) {
        int cx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
        int cy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
        otihid_mouse_report(a->mouse_buttons, (int16_t)cx, (int16_t)cy, 0, 0, pkt);
        int rc = otilink_send_hid(cd, OTI_HID_TYPE_MOUSE, pkt);
        note_send_result(a, rc);
        if (rc == 0) {
            a->n_hid++;
            a->hid_fail_streak = 0;
            a->hid_last_ok_ms = now_ms();
        } else if (++a->hid_fail_streak >= 20) {
            break;                            /* 链路故障：交给看门狗交还本机 */
        }
        dx -= cx;
        dy -= cy;
        sent++;
    }
}

/* 键盘事件走**帧管道**（OTI_MSG_KEY）而不是 HID —— 线缆的 HID 键盘接口有固件缺陷
   （修饰键字节一发出，之后报表全丢、Windows 上 Ctrl 卡死，见 NOTES §61）。
   对端在拓扑 B 跑的是我们的 otiagent.ps1（-InjectKeys 时用 SendInput 注入）；
   鼠标继续走 HID（那条通道是好的）。对端不在（零软件接收端）时退回 HID，聊胜于无。 */
static void send_key_frame(struct app *a, struct otilink_dev *cd, uint16_t code, uint8_t value)
{
    /* 默认走 HID（修饰键已改成 usage 数组编码，真机验证过 down/up 都能到、不卡键）。
       帧管道键盘（下方代码）留作备用：它依赖对端 agent 主循环的取帧节奏，实测不稳。 */
    if (OTI_HID_KBD_FORWARD) {
        hid_send_key(a, cd, code, value);
        return;
    }
    if (!a->per_seen) {
        hid_send_key(a, cd, code, value);
        return;
    }
    uint8_t buf[64];
    oti_key_evt k = {.code = code, .value = value};
    uint32_t seq = __sync_add_and_fetch(&a->seq, 1);
    char what[64];
    snprintf(what, sizeof(what), "key code=%u val=%u（帧管道→SendInput）", code, value);
    size_t n = oti_encode_key(seq, &k, buf, sizeof(buf));
    send_body(a, buf, n, what);                 /* 第一条带日志 */
    /* 帧管道会丢帧（实测 40%）→ 同 seq 重发 2 次；对端按 seq 去重，不会重复输入 */
    for (int i = 0; i < 2; i++) {
        usleep(15 * 1000);
        note_send_result(a, oti_tr_send(a->tx, buf, n));
    }
}

/* 回程落点：把 core 坐标落到"出口边往里"，写回 core 并返回落点。
   ⚠️ 调用方必须**已持有 a->lock**（handle_local 的两条路径都在锁内/锁外各有约定）。
   真实光标的同步在锁外做（见 return_landing_warp），X11 调用不能占着锁。 */
static void return_landing_store(struct app *a, int *ox, int *oy)
{
    int tx, ty;
    return_landing_xy(&a->core, &tx, &ty);
    a->core.mx = (int16_t)tx;
    a->core.my = (int16_t)ty;
    *ox = tx;
    *oy = ty;
}

/* 把真实光标同步到回程落点（不加锁；X11 不可用时什么都不做）。 */
static void return_landing_warp(struct app *a, int tx, int ty)
{
    int px = -1, py = -1;
    if (otix11_pos(&px, &py) != 0)
        return;
    /* 只在真实光标**贴在出口边**（grab 期间它不会动，回程时通常就在那儿）时，
       才把它挪到落点；如果它在别处（多屏/用户刚动过），挪动就会变成
       "光标跳一大截"（用户报障：从 Windows 回来时光标跳动、移位）。
       这时改为**用真实光标校准状态机**，谁都不动。 */
    int m = a->core.edge_px + 8;
    int near = 0;
    switch (a->core.edge) {
    case OTI_EDGE_LEFT:   near = px <= m; break;
    case OTI_EDGE_RIGHT:  near = px >= a->core.screen_w - 1 - m; break;
    case OTI_EDGE_TOP:    near = py <= m; break;
    default:              near = py >= a->core.screen_h - 1 - m; break;
    }
    if (near && (px != tx || py != ty) && otix11_warp(tx, ty) == 0) {
        say(a, "回程落点 %d,%d：真实光标 %d,%d → 已同步", tx, ty, px, py);
    } else if (!near) {
        pthread_mutex_lock(&a->lock);
        a->core.mx = (int16_t)px;
        a->core.my = (int16_t)py;
        pthread_mutex_unlock(&a->lock);
        say(a, "回程：真实光标在 %d,%d（不贴出口边）→ 不挪动，按它校准", px, py);
    }
}

/* 本地事件入口（抓取或 sim） */
/* 把累积的鼠标位移/按键合成一条消息发出去 */
static void flush_mouse(struct app *a)
{
    if (!a->pend_dirty)
        return;
    a->pend_dirty = 0;
    uint8_t buf[64];
    char what[96];
    uint32_t seq = ++a->seq;
    if (a->mouse_native) {
        oti_mouse_evt m = {.dx = (int16_t)a->pend_dx, .dy = (int16_t)a->pend_dy,
                           .wheel = (int16_t)a->pend_wheel,
                           .buttons = (uint16_t)a->pend_buttons};
        snprintf(what, sizeof(what), "mouse dx=%d dy=%d wheel=%d btn=0x%x",
                 m.dx, m.dy, m.wheel, m.buttons);
        send_body(a, buf, oti_encode_mouse(seq, &m, buf, sizeof(buf)), what);
    } else {
        oti_mouse_abs_evt ma = {.x = (int16_t)a->pend_abs_x, .y = (int16_t)a->pend_abs_y,
                                .wheel = (int16_t)a->pend_wheel,
                                .buttons = (uint16_t)a->pend_buttons};
        snprintf(what, sizeof(what), "mouse ABS x=%d y=%d wheel=%d btn=0x%x",
                 ma.x, ma.y, ma.wheel, ma.buttons);
        send_body(a, buf, oti_encode_mouse_abs(seq, &ma, buf, sizeof(buf)), what);
    }
    a->pend_dx = a->pend_dy = a->pend_wheel = 0;
    a->sent_buttons = a->pend_buttons;
    a->last_mouse_tx_ms = now_ms();
}

/*
 * 把控制权交还给"厂商 Windows 端"。
 * 实测：Windows→麒麟 的转发是**厂商程序**做的（我们自己的 agent 日志显示一个包都没发），
 * 所以只发 F24 令牌（那是给我们的 agent 的）厂商端收不到，用户看到的就是"移过去回不来"。
 * 这里按厂商线格式补发 Cmd_Notify_KM_Switch_To_Local。
 */
static void notify_vendor_switch_local(struct app *a, struct otilink_dev *cd)
{
    char xml[768];
    int n = snprintf(xml, sizeof(xml),
        "<ExtraXmlCommand>HookAppService</ExtraXmlCommand><ExtraXmlParam><OTIMSG>"
        "<NP_Cmd>Cmd_Notify_KM_Switch_To_Local</NP_Cmd>"
        "<NP_Up_Notice_NamePipe_Name>\\\\.\\pipe\\OTI_ClipboardAgent</NP_Up_Notice_NamePipe_Name>"
        "<Param_Move_In_Info><OTIMSG>"
        "<Param_Move_In_Direction>1</Param_Move_In_Direction>"
        "<Param_Move_In_X>%d</Param_Move_In_X>"
        "<Param_Move_In_Y>%d</Param_Move_In_Y>"
        "<Param_Other_PC_Position_Option>2</Param_Other_PC_Position_Option>"
        "<Param_Remote_Screen_Width>%d</Param_Remote_Screen_Width>"
        "<Param_Remote_Screen_Height>%d</Param_Remote_Screen_Height>"
        "<Param_Move_Out_KM_Switch_Option>0</Param_Move_Out_KM_Switch_Option>"
        "</OTIMSG></Param_Move_In_Info></OTIMSG></ExtraXmlParam>",
        a->cfg.screen_w, a->cfg.screen_h / 2, a->cfg.screen_w, a->cfg.screen_h);
    if (n <= 0 || (size_t)n >= sizeof(xml))
        return;
    int rc = otilink_send_vendor_xml(cd, xml, (size_t)n, 1500);
    say(a, "已通知厂商端交还控制权（Cmd_Notify_KM_Switch_To_Local rc=%d，%d 字节）", rc, n);
}

/* ---------- 屏幕尺寸自动探测 ----------
 * 顺序：X11/XRandR（含 XWayland，最准）→ DRM sysfs 的"首选模式"
 *      → 退回构造默认值并**明确告警**（撞边判据与绝对坐标都按它算）。
 * DRM 只给"显示器支持的首选模式"，不等于当前分辨率，所以排在 X11 后面。 */
static int screen_from_drm(int *w, int *h)
{
    DIR *d = opendir("/sys/class/drm");
    if (!d)
        return -1;
    struct dirent *e;
    int best_w = 0, best_h = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "card", 4) != 0 || !strchr(e->d_name, '-'))
            continue;                       /* 只看连接器目录，如 card0-HDMI-A-1 */
        char p[256], line[64];
        snprintf(p, sizeof(p), "/sys/class/drm/%.200s/status", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        int connected = (fgets(line, sizeof(line), f) && strncmp(line, "connected", 9) == 0);
        fclose(f);
        if (!connected)
            continue;
        snprintf(p, sizeof(p), "/sys/class/drm/%.200s/modes", e->d_name);
        f = fopen(p, "r");
        if (!f)
            continue;
        if (fgets(line, sizeof(line), f)) {
            int ww = 0, hh = 0;
            if (sscanf(line, "%dx%d", &ww, &hh) == 2 && ww > 0 && hh > 0 &&
                (long)ww * hh > (long)best_w * best_h) {
                best_w = ww;
                best_h = hh;
            }
        }
        fclose(f);
    }
    closedir(d);
    if (best_w > 0 && best_h > 0) {
        *w = best_w;
        *h = best_h;
        return 0;
    }
    return -1;
}

/* ---------- 被驱动侧的光标来源 ----------
 * X11/XWayland：XQueryPointer 拿**真实**坐标（精确，判据与今天完全一致）。
 * Wayland 原生会话读不到全局指针 → 降级为"进边校准 + 线缆 HID 位移积分 + 边界夹紧"：
 * 判据不看绝对位置，只看"贴边后还继续外推了多少像素"（slave_over），
 * 因此积分漂移只会让触发早晚略有差别，不会失效；热键兜底始终可用。 */
static int slave_cursor(struct app *a, int is_mouse, const oti_mouse_evt *m, int *px, int *py)
{
    if (a->cfg.cursor_source != 2 && !a->use_integrate) {
        if (otix11_pos(px, py) == 0)
            return 0;
        if (a->cfg.cursor_source == 1)          /* 显式要求 x11：不降级 */
            return -1;
        a->use_integrate = 1;
        a->slave_seeded = 0;
        say(a, "!! 读不到真实光标（DISPLAY/XAUTHORITY 无效？）→ 被驱动侧降级为"
               "位移积分判据（Wayland 场景）；回程热键仍可用");
    }
    if (!a->slave_seeded) {
        unsigned re = a->cfg.return_edge ? a->cfg.return_edge : OTI_EDGE_BIT(OTI_EDGE_LEFT);
        otikm_slave_seed(&a->spos, a->cfg.screen_w, a->cfg.screen_h, re);
        a->slave_seeded = 1;
        say(a, "被驱动侧积分判据：起点 (%d,%d)（进边校准，屏幕 %dx%d）", a->spos.x, a->spos.y,
            a->cfg.screen_w, a->cfg.screen_h);
    }
    if (is_mouse && m)
        otikm_slave_advance(&a->spos, a->cfg.screen_w, a->cfg.screen_h, m->dx, m->dy);
    *px = a->spos.x;
    *py = a->spos.y;
    return 0;
}

/* ---------- --auto：把 run-kylin.sh 的角色/设备判定搬进 C ---------- */
static void resolve_auto(struct app *a)
{
    /* 1) 传输：没显式给就自动探测线缆（oti_tr_open_cable(NULL) 会逐个 LUN 试 0xF0/0x00） */
    if (!a->cfg.transport)
        a->cfg.transport = "cable";

    /* 2) 屏幕 */
    if (!a->cfg.set_screen) {
        int w = 0, h = 0;
        const char *src = NULL;
        if (otix11_screen(&w, &h) == 0)
            src = "X11/XRandR";
        else if (screen_from_drm(&w, &h) == 0)
            src = "DRM sysfs(首选模式)";
        if (src) {
            a->cfg.screen_w = w;
            a->cfg.screen_h = h;
            say(a, "屏幕: %dx%d（来源 %s）", w, h, src);
        } else {
            say(a, "[warn] 屏幕尺寸探测失败（没有可用 X，也没有 DRM 连接器）→ 退回 %dx%d；"
                   "撞边判据按它算，如与实际不符请显式 --screen WxH",
                a->cfg.screen_w, a->cfg.screen_h);
        }
    }

    /* 3) 角色：本机有没有"线缆之外的键鼠"（按能力位 + USB VID/PID 判定，不靠 by-id 命名）。
       第 46 轮起角色**由协商决定**（otikm_core.c 的 otikm_role_decide；与对端每 1s 交换 ROLE）。
       这里只做启动引导：先按"本机有无键鼠"选一个初始角色（1~2s 内会被协商纠正），
       并备好 has_local_input / boot_id。--role 显式指定时 want=FORCE_*，对端必须让位。 */
    a->has_local_input = 0;
    {
        char kbd[64] = "", mou[64] = "";
        int n = oti_input_autoselect(0, kbd, mou);
        int nhot = oti_input_local_count();      /* 只认可热插拔的 USB 键鼠（内置/PS2 不换边） */
        a->has_local_input = nhot > 0 ? 1 : 0;
        a->local_count = nhot;                   /* 热插拔重采样基线（个数变化才重扫） */
        if (!a->cfg.set_role || a->cfg.role == 0) {
            a->cfg.role = n > 0 ? 1 : 2;         /* 引导角色仍看"任何本机键鼠"，保持老行为 */
            say(a, "角色引导(auto): %s（本机键鼠 %d 个，其中可热插拔 %d：键盘=%s 鼠标=%s）"
                   "—— 之后由对端 ROLE 协商定",
                a->cfg.role == 1 ? "主控端 master" : "被驱动侧 slave", n, nhot,
                kbd[0] ? kbd : "-", mou[0] ? mou : "-");
        } else {
            say(a, "角色(显式指定): %s（会要求对端让位）",
                a->cfg.role == 1 ? "主控端 master" : "被驱动侧 slave");
        }
    }

    /* 4) 角色 → 开关 + 抓取设备（显式 --capture 优先） */
    static char ck[64], cm[64];
    if (a->cfg.role == 2) {                       /* 被驱动侧：只监听线缆 HID，永不接管 */
        a->cfg.return_on_edge = 1;
        a->cfg.inject = 0;
        a->cfg.grab = 0;
        if (a->cfg.ncapture == 0) {
            int n = oti_input_autoselect(1, ck, cm);
            if (cm[0] && a->cfg.ncapture < 8)
                a->cfg.capture[a->cfg.ncapture++] = cm;
            if (ck[0] && a->cfg.ncapture < 8)
                a->cfg.capture[a->cfg.ncapture++] = ck;
            say(a, "抓取设备(线缆 HID): 鼠标=%s 键盘=%s", cm[0] ? cm : "-", ck[0] ? ck : "-");
            if (!n)
                say(a, "[warn] 看不到线缆 HID 设备：线缆插好了吗？（--doctor 看细节）");
        }
    } else {                                      /* 主控端：抓本机键鼠 + 注入 */
        a->cfg.inject = 1;
        if (!a->cfg.no_grab)
            a->cfg.grab = 1;
        if (a->cfg.ncapture == 0) {
            int n = oti_input_autoselect(0, ck, cm);
            if (cm[0] && a->cfg.ncapture < 8)
                a->cfg.capture[a->cfg.ncapture++] = cm;
            if (ck[0] && a->cfg.ncapture < 8)
                a->cfg.capture[a->cfg.ncapture++] = ck;
            say(a, "抓取设备(本机键鼠): 鼠标=%s 键盘=%s", cm[0] ? cm : "-", ck[0] ? ck : "-");
            if (!n)
                say(a, "[warn] 看不到本机键鼠：需要提权才能读设备能力位（sudo 再试）");
        }
    }
    if (a->cfg.no_clip)
        a->cfg.clipboard = 0;
    else if (!a->cfg.clipboard)
        a->cfg.clipboard = 1;                     /* --auto 默认带剪贴板 */

    /* 4a) 角色协商状态初始化（第 46 轮）*/
    a->boot_id = role_boot_id();
    a->role_state = (a->cfg.role == 1) ? OTI_ROLE_MASTER : OTI_ROLE_SLAVE;
    a->applied_role = a->role_state;
    a->role_state_ms = now_ms();
    a->role_grab_ok = 0;                     /* 未收到对端确认前不许 grab（I1） */
    say(a, "角色协商: boot_id=%08x 本机键鼠=%d 目标=%s（每 1s 与对端交换 ROLE）",
        a->boot_id, a->has_local_input,
        !a->cfg.role_explicit ? "auto" : (a->cfg.role == 1 ? "master" : "slave"));

    /* 4b) 保活：线缆路径长时间空闲会让设备侧会话卡死（实测：两小时后剪贴板单向不通、
       拔插无效）。--auto 默认开 5 秒保活；要关就显式 --keepalive 0。 */
    if (a->cfg.keepalive_ms == 0 && !a->cfg.set_keepalive && a->cfg.transport &&
        strncmp(a->cfg.transport, "cable", 5) == 0)
        a->cfg.keepalive_ms = 5000;

    /* 5) 把决策落成一行，便于用户/排障一眼看清 */
    say(a, "== 绿色包决策: 传输=%s 角色=%s 抓取=%d 个 注入=%s 独占=%s 剪贴板=%s 光标源=%s ==",
        a->cfg.transport, a->cfg.role == 1 ? "master" : "slave", a->cfg.ncapture,
        a->cfg.inject ? "开" : "关", a->cfg.grab ? "开" : "关",
        a->cfg.clipboard ? "开" : "关",
        a->cfg.cursor_source == 1 ? "x11" : a->cfg.cursor_source == 2 ? "integrate" : "auto");
}

/* ---------- --version：把"跑的是哪一版、基线是什么、这台机器有什么"打出来 ---------- */
static void print_version(void)
{
    printf("otikm %s\n", OTI_VERSION);
    printf("build: %s\n", OTI_BUILD_TAG);
#ifdef __GLIBC__
    printf("runtime: glibc %s\n", gnu_get_libc_version());
#else
    printf("runtime: non-glibc libc\n");
#endif
    printf("features: x11=%s\n", otix11_status());
    printf("clipboard: %s\n", oti_clip_backend_probe());
    printf("transport: cable(auto 探测 0ea0:2213) | listen:PATH | connect:PATH | tcp-*\n");
}

/* ---------- --print-plan：只打印决策（门禁/容器里无硬件也能验证探测逻辑）---------- */
static const char *edges_name(unsigned m, char *buf, size_t cap)
{
    static const char *nm[4] = {"右", "左", "上", "下"};
    size_t n = 0;
    if (!m) {
        snprintf(buf, cap, "无（只用热键）");
        return buf;
    }
    if (m == OTI_EDGE_ALL) {
        snprintf(buf, cap, "四边");
        return buf;
    }
    buf[0] = 0;
    for (int i = 0; i < 4; i++)
        if (m & OTI_EDGE_BIT(i))
            n += (size_t)snprintf(buf + n, cap > n ? cap - n : 0, "%s%s", n ? "," : "", nm[i]);
    return buf;
}

static void print_plan(struct app *a)
{
    char e1[64], e2[64];
    printf("== otikm 计划（--print-plan：只做判定，不建立链路、不碰设备）==\n");
    printf("传输        : %s\n", a->cfg.transport ? a->cfg.transport : "(未指定)");
    printf("角色        : %s\n", a->cfg.return_on_edge ? "被驱动侧 slave"
                                   : a->cfg.inject ? "主控端 master" : "(未指定)");
    printf("抓取设备    : ");
    if (a->cfg.ncapture == 0)
        printf("(无)\n");
    else {
        for (int i = 0; i < a->cfg.ncapture; i++)
            printf("%s%s", i ? ", " : "", a->cfg.capture[i]);
        printf("\n");
    }
    printf("屏幕        : %dx%d\n", a->cfg.screen_w, a->cfg.screen_h);
    printf("撞边边集    : %s\n", edges_name(a->cfg.edges, e1, sizeof(e1)));
    printf("回程边      : %s\n", edges_name(a->cfg.return_edge, e2, sizeof(e2)));
    printf("注入 uinput : %s\n", a->cfg.inject ? "开" : "关");
    printf("独占 grab   : %s\n", a->cfg.grab ? "开" : "关");
    printf("剪贴板      : %s\n", a->cfg.clipboard ? "开" : "关");
    printf("被驱动光标源: %s\n", a->cfg.cursor_source == 1 ? "x11"
                                  : a->cfg.cursor_source == 2 ? "integrate" : "auto");
    printf("按键载体    : %s\n", a->cfg.peer_hid > 0 ? "厂商 HID 包（对端零软件）"
                                  : a->cfg.peer_hid == 0 ? "协议管道（两端都要软件）" : "auto");
}

static void handle_local(struct app *a, int is_mouse, const oti_mouse_evt *m,
                         const oti_key_evt *k)
{
    uint8_t buf[64];
    /* 角色协商输入：只有"主控模式抓到的本机键鼠"才算"本机在用"（被驱动侧抓的是线缆 HID） */
    if (!a->cfg.return_on_edge)
        a->input_last_ms = now_ms();
    oti_switch_evt sw;
    memset(&sw, 0, sizeof(sw));

    /* 被控端零软件模式：**仍然用状态机决定何时转发**（边缘穿越→转发，热键→收回），
       只把"转发"的载体从协议消息换成 HID 包 —— 对端内核直接收标准 HID，无需任何软件。
       注意切回本地时必须发"释放所有按键"包，否则对端会卡住按下的键。 */
    if (a->cfg.peer_hid) {
        struct otilink_dev *cd = oti_tr_cable_dev(a->tx);
        if (!cd) {
            say(a, "ERR --peer hid 需要 cable 传输（当前 %s）", oti_tr_name(a->tx));
            return;
        }
        /* ---- 被驱动侧：撞边交还 + 回程热键 ---------------------------------
           两条路都必须留着（L8：交出去的能力一定要有对称的收回来）：
           (a) 撞边：判据优先用**真实光标坐标**（XQueryPointer，精确，避免累加位移漂移）；
               读不到时（Wayland 原生会话没有全局指针 API）降级为位移积分，见 slave_cursor()。
           (b) 回程热键：被驱动侧抓的是线缆 HID 键盘 —— 用户在共享键盘上按 ctrl+alt+left
               也能把控制权要回去（Wayland / X11 失效时的最后一道保险）。
           手势节流 1.5s（避免按住不放时刷屏）。 */
        if (a->cfg.return_on_edge) {
            /* (b) 回程热键（键盘事件，不需要光标） */
            if (!is_mouse && k) {
                uint8_t bit = 0;
                switch (k->code) {
                case 42: case 54: bit = OTI_MOD_SHIFT; break;
                case 29: case 97: bit = OTI_MOD_CTRL;  break;
                case 56: case 100: bit = OTI_MOD_ALT;  break;
                case 125: case 126: bit = OTI_MOD_META; break;
                default: break;
                }
                if (bit) {
                    if (k->value)
                        a->slave_mods |= bit;
                    else
                        a->slave_mods &= (uint8_t)~bit;
                } else if (k->value == 1 && a->core.hk_to_local.key &&
                           k->code == a->core.hk_to_local.key &&
                           (a->slave_mods & a->core.hk_to_local.mods) == a->core.hk_to_local.mods) {
                    long now = now_ms();
                    if (now - a->last_return_ms >= 1500) {
                        a->last_return_ms = now;
                        request_token(a);           /* 令牌由消息泵线程发（见 request_token） */
                        a->want_vendor_return = 1;  /* 厂商帧由 RX 线程发（见 rx_thread） */
                        char nb[64];
                        say(a, "回程热键 %s → 通知对端收回控制权（被驱动侧，不接管）",
                            otikm_hotkey_name(&a->core.hk_to_local, nb, sizeof(nb)));
                    }
                }
            }

            /* (a) 撞边交还 */
            int px = -1, py = -1;
            if (slave_cursor(a, is_mouse, m, &px, &py) == 0) {
                int mg = a->cfg.edge > 0 ? a->cfg.edge : 4;
                int dx = is_mouse && m ? m->dx : 0, dy = is_mouse && m ? m->dy : 0;
                int near = 0;
                /* 只认 return_edge 这一条边（默认左 = 朝向 Windows 那侧），且必须**还在往外推**。
                   两点都必要：
                     * 只看位置不行 —— 光标停在边缘会每隔 1.5s 反复触发；
                     * 四条边都认也不行 —— 往 Linux 深处推会被误判成"想回去"，刚过来就被弹回。 */
                unsigned re = a->cfg.return_edge ? a->cfg.return_edge : OTI_EDGE_BIT(OTI_EDGE_LEFT);
                if (a->use_integrate) {
                    /* 积分判据：完全不看绝对位置，只看"贴边之后还继续外推了多少像素"
                       （纯逻辑在 otikm_core.c，coretest 覆盖）。 */
                    if (otikm_slave_want_return(&a->spos, a->cfg.edge, re))
                        near = 1;
                } else {
                    if ((re & OTI_EDGE_BIT(OTI_EDGE_LEFT))   && px <= mg && dx < 0) near = 1;
                    if ((re & OTI_EDGE_BIT(OTI_EDGE_RIGHT))  && px >= a->cfg.screen_w - 1 - mg && dx > 0) near = 1;
                    if ((re & OTI_EDGE_BIT(OTI_EDGE_TOP))    && py <= mg && dy < 0) near = 1;
                    if ((re & OTI_EDGE_BIT(OTI_EDGE_BOTTOM)) && py >= a->cfg.screen_h - 1 - mg && dy > 0) near = 1;
                }
                long now = now_ms();
                if (!near) {
                    a->edge_at_ms = 0;
                } else if (a->edge_at_ms == 0) {
                    a->edge_at_ms = now;
                } else if (now - a->edge_at_ms >= a->cfg.edge_dwell_ms &&
                           now - a->last_return_ms >= 1500) {
                    a->edge_at_ms = 0;
                    a->last_return_ms = now;
                    a->slave_seeded = 0;            /* 下次进边重新校准积分起点 */
                    request_token(a);               /* 令牌由消息泵线程发（见 request_token） */
                    a->want_vendor_return = 1;      /* 厂商帧由 RX 线程发（见 rx_thread） */
                    say(a, "撞边(%d,%d)%s → 通知对端收回控制权（被驱动侧，不接管）", px, py,
                        a->use_integrate ? "[积分判据]" : "");
                }
            }
            return;                     /* 被驱动侧永不接管、永不转发 */
        }
        pthread_mutex_lock(&a->lock);
        otikm_act act = is_mouse ? otikm_core_local_mouse(&a->core, m, &sw)
                                 : otikm_core_local_key(&a->core, k, &sw);
        int driving_now = !a->core.have_control;
        int drv_x = a->core.drv_x, drv_y = a->core.drv_y;
        int drv_ex = a->core.rem_entry_x, drv_ey = a->core.rem_entry_y;
        /* 修饰键抑制缓冲必须在这里放行 —— HID 路径以前**漏了这一步**（真机后果：
           对端驱动时按一下 Ctrl/Alt，之后所有按键都被当"可能是热键前缀"吞掉，
           ctrl+c/ctrl+v 到不了对端；见 NOTES §59.2）。
           两个细节：① 只有"仍在驱动对端"时才补发，已经交还/收回就丢弃（这些键
           从未送达，补发会变成对端的幽灵按键）；② 补发在本次事件之前（时间上更早）。 */
        oti_key_evt sup[OTI_PEND_MAX];
        int nsup = otikm_core_take_suppressed(&a->core, sup, OTI_PEND_MAX);
        pthread_mutex_unlock(&a->lock);
        if (driving_now && nsup > 0) {
            /* 合并成**一份**报表：修饰键+按键分两条发时第二条会被吞（NOTES §61 实测） */
            for (int i = 0; i < nsup; i++)
                otihid_kbd_apply(&a->kbd, sup[i].code, sup[i].value);
            hid_send_state(a, cd);
        }
        if (driving_now) {
            if (nsup) {              /* 证据行：证明修饰键前缀被补发了（不是吞掉） */
                static unsigned long n_sup;
                if (a->cfg.verbose || ++n_sup <= 5)
                    say(a, "HID 补发被抑制的 %d 个按键事件（修饰键前缀→非热键，第 %lu 批）",
                        nsup, n_sup);
            }
        }
        switch (act) {
        case OTI_ACT_LOCAL:
            break;                            /* 指针在本机：本地消费 */
        case OTI_ACT_TO_REMOTE:
            if (is_mouse) {
                a->mouse_buttons = m->buttons;
                /* 回程判据诊断（节流 1s，只在"用户正往入口边推"时打）：
                   下次再遇到"回不来"，日志里就有证据（推回多少 / 阈值 / 入口在哪边）。 */
                if (driving_now) {
                    int need = a->core.edge_px * 4;
                    if (need < 8)
                        need = 8;
                    int prog = 0;
                    if (drv_ex == 1) prog = drv_x;
                    else if (drv_ex == 0) prog = -drv_x;
                    else if (drv_ey == 1) prog = drv_y;
                    else if (drv_ey == 0) prog = -drv_y;
                    long nw = now_ms();
                    if (prog > 0 && nw - a->drive_log_ms >= 1000) {
                        a->drive_log_ms = nw;
                        say(a, "回程判据：已把推出去的位移推回 %d/%d（入口边 x=%d y=%d）",
                            prog, need, drv_ex, drv_ey);
                    }
                }
                /* 描述符每轴只有 1 字节有符号 → 大位移必须拆成多包 */
                uint8_t pk[8 * OTI_HID_PKT_LEN];
                int np = otihid_mouse_packets(m->buttons, m->dx, m->dy, m->wheel, pk, 8);
                for (int i = 0; i < np; i++) {
                    int hrc = otilink_send_hid(cd, OTI_HID_TYPE_MOUSE, pk + i * OTI_HID_PKT_LEN);
                    note_send_result(a, hrc);
                    if (hrc == 0) {
                        a->n_hid++;
                        a->hid_fail_streak = 0;
                        a->hid_last_ok_ms = now_ms();
                    } else {
                        a->hid_fail_streak++;
                    }
                }
            } else {
                send_key_frame(a, cd, k->code, k->value);
            }
            break;
        case OTI_ACT_SWITCH_TO_REMOTE:
            if (a->cfg.return_on_edge) {
                /* 本机没有自己的键鼠（键鼠在对端），我们只是"被驱动的一侧"。
                   此时撞边不表示"我要接管对端"，而是"把控制权还给对端" ——
                   发一个 F24 令牌，对端据此恢复自己的键鼠。绝不 grab、绝不转发。 */
                /* 去抖：指针会**停在边缘**，不去抖的话每个后续事件都再触发一次
                   （实测刷出 1868 条日志、并把 F24 令牌发了上千次）。 */
                long now = now_ms();
                if (now - a->last_return_ms < 1500)
                    break;
                a->last_return_ms = now;
                request_token(a);                 /* 令牌由消息泵线程发（见 request_token） */
                /* 厂商帧必须由**主线程的消息泵**发：捕获线程自己发会跟泵抢授权消息
                   （两个线程同时读同一个 fd 的消息），实测极不可靠。 */
                a->want_vendor_return = 1;
                pthread_mutex_lock(&a->lock);
                otikm_core_force_local(&a->core);
                /* 关键：把本机指针记回屏幕中心，用户必须重新推一次才会再次触发 */
                a->core.mx = (int16_t)(a->cfg.screen_w / 2);
                a->core.my = (int16_t)(a->cfg.screen_h / 2);
                pthread_mutex_unlock(&a->lock);
                say(a, "撞边 → 已通知对端收回控制权（本机为被驱动侧，不接管）");
                break;
            }
            /* W4：对端静默时不交出去（防"交给一个不回应的对端"） */
            if (peer_is_silent(a)) {
                say(a, "[warn] 对端已 %ldms 没有任何消息 → 不交出去（W4）", now_ms() - a->per_ms);
                break;
            }
            /* I1（第 46 轮）：只有仲裁确认我是 master 才允许接管/独占，
               否则两侧可能同时 grab（现场事故：帧管道双向全丢） */
            if (a->cfg.set_role == 0 && a->role_state != OTI_ROLE_MASTER) {
                say(a, "[warn] 角色未确认为 master（对端尚未让位）→ 暂不接管（I1）");
                break;
            }
            otihid_kbd_reset(&a->kbd);        /* 新的一段转发，从干净状态开始 */
            /* 告诉对端"我接管了，你回到被动"：只有对端是**厂商/零软件端**时才需要 F24 令牌。
               对端跑我们的 agent 时它不驱动 KM，令牌纯属浪费 —— 而且真机实测线缆的键盘接口
               每个会话只送得动**几条**报表，这 6 条 F24 会把额度吃掉，导致随后用户按键全丢
               （NOTES §61）。 */
            if (!a->per_seen)
                request_token(a);
            apply_grab(a, 1);
            {
                /* 把对端光标顶到入口边：让"入口边=0 位移"的模型与用户看到的一致 */
                pthread_mutex_lock(&a->lock);
                int ex = a->core.rem_entry_x, ey = a->core.rem_entry_y;
                pthread_mutex_unlock(&a->lock);
                hid_park_remote_pointer(a, cd, ex, ey);
                say(a, "HID 模式：指针交给对端（开始把键鼠发成 HID 包；对端光标已停在入口边 x=%d y=%d）",
                    ex, ey);
            }
            break;
        case OTI_ACT_SWITCH_TO_LOCAL: {
            request_release_all(a);           /* 关键：避免对端卡键（泵线程先发这个） */
            /* 告诉对端"我放手了"：对端据此恢复自己的键鼠（否则它会一直吞着） */
            request_token(a);
            otihid_kbd_reset(&a->kbd);
            apply_grab(a, 0);
            /* 回程落点 + 真实光标同步（真机坑见 NOTES §59.3）：
               grab 期间真实光标一动不动，回程时还贴在出口边上；以前直接拿它校准
               → core 坐标又回到边上，用户轻轻一碰就再次被推出去（日志实证：
               "拉回本机"后 0.2s 又"交给对端"）。现在统一落到"出口边往里
               edge+8"，并用 XWarpPointer 把真实光标也挪过去，两边一致。 */
            int tx, ty;
            pthread_mutex_lock(&a->lock);
            return_landing_store(a, &tx, &ty);
            pthread_mutex_unlock(&a->lock);
            return_landing_warp(a, tx, ty);
            say(a, "HID 模式：指针拉回本机（落点 %d,%d；已请求释放包 + 令牌，由消息泵发出）",
                tx, ty);
            break;
        }
        }
        return;
    }

    pthread_mutex_lock(&a->lock);
    int locked_before = a->core.locked;
    otikm_act act = is_mouse ? otikm_core_local_mouse(&a->core, m, &sw)
                             : otikm_core_local_key(&a->core, k, &sw);
    int land = 0, land_tx = 0, land_ty = 0;             /* 回程落点（锁外再同步真实光标） */
    int abs_x = a->core.rem_x, abs_y = a->core.rem_y;   /* 远端坐标系下的指针位置 */
    int locked_after = a->core.locked;
    /* 先把被抑制缓冲的事件补发出去（顺序敏感：修饰键必须在本键之前到达）。
       发送本身是非阻塞入队，所以在锁内做没问题。热键组合已在核心里整段丢弃。 */
    {
        oti_key_evt sup[OTI_PEND_MAX];
        int nsup = otikm_core_take_suppressed(&a->core, sup, OTI_PEND_MAX);
        for (int i = 0; i < nsup; i++) {
            uint8_t sb[64];
            char swhat[64];
            uint32_t sq = ++a->seq;
            snprintf(swhat, sizeof(swhat), "key(supp) code=%u val=%u", sup[i].code, sup[i].value);
            send_body(a, sb, oti_encode_key(sq, &sup[i], sb, sizeof(sb)), swhat);
        }
    }
    if (locked_before != locked_after) {
        pthread_mutex_unlock(&a->lock);
        say(a, "!! %s", locked_after ? "已锁定到本机：撞边不再切换（再按一次解锁）"
                                     : "已解锁：撞边可以切换到对端");
        return;                                     /* 锁定键本身不转发给对端 */
    }
    uint32_t seq = ++a->seq;
    char what[64];

    switch (act) {
    case OTI_ACT_LOCAL:
        if (a->cfg.verbose)
            say(a, "LOCAL %s", is_mouse ? "mouse" : "key");
        break;
    case OTI_ACT_TO_REMOTE:
        if (is_mouse) {
            /* 合并位移；按键变化或到达发送间隔时才真正发一条 */
            a->pend_dx += m->dx;
            a->pend_dy += m->dy;
            a->pend_wheel += m->wheel;
            a->pend_buttons = m->buttons;
            a->pend_abs_x = abs_x;
            a->pend_abs_y = abs_y;
            a->pend_dirty = 1;
            int btn_changed = (m->buttons != a->sent_buttons);
            if (btn_changed || now_ms() - a->last_mouse_tx_ms >= 4)
                flush_mouse(a);
            break;                                  /* 注意：不往下走 send_body */
        } else {
            snprintf(what, sizeof(what), "key code=%u val=%u", k->code, k->value);
            send_body(a, buf, oti_encode_key(seq, k, buf, sizeof(buf)), what);
        }
        break;
    case OTI_ACT_SWITCH_TO_REMOTE:
        /* W4：对端静默时不交出去（同 HID 路径） */
        if (peer_is_silent(a)) {
            say(a, "[warn] 对端已 %ldms 没有任何消息 → 不交出去（W4）", now_ms() - a->per_ms);
            break;
        }
        flush_mouse(a);
        snprintf(what, sizeof(what), "SWITCH side=remote edge=%d,%d", sw.edge_x, sw.edge_y);
        send_body(a, buf, oti_encode_switch(seq, &sw, buf, sizeof(buf)), what);
        a->n_switches++;
        say(a, "STATE 指针交给对端（本机转为驱动侧，开始独占转发）");
        a->drive_started_ms = now_ms();
        apply_grab(a, 1);
        break;
    case OTI_ACT_SWITCH_TO_LOCAL:
        flush_mouse(a);
        snprintf(what, sizeof(what), "SWITCH side=local edge=%d,%d", sw.edge_x, sw.edge_y);
        send_body(a, buf, oti_encode_switch(seq, &sw, buf, sizeof(buf)), what);
        a->n_switches++;
        say(a, "STATE 指针拉回本机（恢复本地键鼠）");
        apply_grab(a, 0);
        /* 落点别停在出口边上（否则轻轻一碰又出去）。core 坐标在锁内写，
           真实光标的同步放到解锁之后（X11 调用可能慢，不能占着锁）。 */
        return_landing_store(a, &land_tx, &land_ty);
        land = 1;
        break;
    }
    pthread_mutex_unlock(&a->lock);
    if (land)
        return_landing_warp(a, land_tx, land_ty);
}

/* ---------- 注入 ---------- */
static void do_inject(struct app *a, int is_mouse, const oti_mouse_evt *m, const oti_key_evt *k)
{
    a->n_injected++;
    if (a->sim_out) {
        if (is_mouse)
            fprintf(a->sim_out, "mouse %d %d %d %u\n", m->dx, m->dy, m->wheel, m->buttons);
        else
            fprintf(a->sim_out, "key %u %u\n", k->code, k->value);
        fflush(a->sim_out);
        return;
    }
    if (a->use_uinput) {
        if (is_mouse)
            oti_inject_mouse(&a->inj, m->dx, m->dy, m->wheel, m->buttons);
        else
            oti_inject_key(&a->inj, k->code, k->value);
    }
}

/* ---------- 大文件流式传输（见 otixfer.h）---------- */

static uint16_t xrd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t xrd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t xrd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

/* xfer 的回调：发一条 clip 消息（body = clip 头之后的载荷） */
static int xfer_send_clip(void *user, uint16_t format, uint32_t fid, const void *body, size_t len)
{
    struct app *a = user;
    static uint8_t buf[OTI_HDR_SIZE + 20 + OTI_CLIP_CHUNK_MAX];
    if (len > OTI_CLIP_CHUNK_MAX)
        return -1;
    oti_clip_hdr h;
    memset(&h, 0, sizeof(h));
    h.format = format;
    h.fid = fid;
    h.offset = 0;
    h.total_len = (uint32_t)len;
    h.flags = OTI_CLIP_FIRST | OTI_CLIP_LAST;      /* 分片/控制消息都是单块 */
    size_t m = oti_encode_clip(fid, &h, body, (uint32_t)len, buf, sizeof(buf));
    if (!m)
        return -1;
    /* 用"可靠发送"：队列满时等一会儿，而不是丢最旧的那条 */
    return oti_tr_send_wait(a->tx, buf, m, 3000);
}

/* xfer 的回调：收完一个文件 → 挂到剪贴板（并按指纹抑制回发） */
static void xfer_apply_file(void *user, const char *path)
{
    struct app *a = user;
    static char list[1][OTI_CLIP_PATH_MAX];
    snprintf(list[0], OTI_CLIP_PATH_MAX, "%s", path);
    if (oti_clip_set_files(&a->clip, list, 1) == 0)
        say(a, "大文件已挂到剪贴板：file://%s", path);
}

static void xfer_log(void *user, const char *fmt, ...)
{
    struct app *a = user;
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    say(a, "%s", buf);
}

/* ---------- 剪贴板 ---------- */
static void send_clipboard(struct app *a, const uint8_t *data, size_t len)
{
    static uint8_t buf[OTI_HDR_SIZE + 20 + OTI_CLIP_CHUNK_MAX];
    pthread_mutex_lock(&a->lock);
    uint32_t fid = ++a->seq;
    size_t off = 0;
    int chunks = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > OTI_CLIP_CHUNK_MAX)
            n = OTI_CLIP_CHUNK_MAX;
        oti_clip_hdr h;
        memset(&h, 0, sizeof(h));
        h.format = a->clip.last_format ? a->clip.last_format : 1;   /* 文本=1 / PNG=2，由后端识别 */
        h.fid = fid;
        h.offset = (uint32_t)off;
        h.total_len = (uint32_t)len;
        h.flags = (uint16_t)((off == 0 ? OTI_CLIP_FIRST : 0) |
                             (off + n == len ? OTI_CLIP_LAST : 0));
        size_t m = oti_encode_clip(fid, &h, data + off, (uint32_t)n, buf, sizeof(buf));
        if (!m)
            break;
        send_body(a, buf, m, "CLIP");
        off += n;
        chunks++;
    }
    pthread_mutex_unlock(&a->lock);
    uint32_t crc = oti_crc32(data, len);
    /* 带上 fmt 与内容 CRC 前缀：排查"回环/回声"时，两侧日志靠这两个值对齐，
       不用再猜"这 6063 字节到底是哪一份"（实测被这个坑过一次）。 */
    say(a, "CLIP 发送 %zu 字节 / %d 块（fid=%u fmt=%u crc=%08x）", len, chunks, fid,
        (unsigned)(a->clip.last_format ? a->clip.last_format : 1), crc);

}

/* 登记"待对端确认"（首发时调用一次）。**不能放在 send_clipboard 里**：
   重发时要拿 ack_buf 当发送源，而 send_clipboard 会 free 它 → use-after-free。 */
static void clip_ack_register(struct app *a, const void *data, size_t len, uint32_t crc,
                              uint16_t fmt)
{
    free(a->ack_buf);
    a->ack_buf = malloc(len ? len : 1);
    if (!a->ack_buf)
        return;
    memcpy(a->ack_buf, data, len);
    a->ack_len = len;
    a->ack_crc = crc;
    a->ack_fmt = fmt;
    a->ack_active = 1;
    a->ack_tries = 0;
    a->ack_deadline_ms = now_ms() + 1500;
}

/* 超时未确认 → 重发（复用登记时的字节） */
static void clip_ack_check(struct app *a)
{
    if (!a->ack_active)
        return;
    if (now_ms() < a->ack_deadline_ms)
        return;
    if (a->ack_tries >= 3) {
        say(a, "[warn] 剪贴板 %zu 字节重发 3 次仍无确认（对端在吗？），放弃", a->ack_len);
        a->n_ack_fail++;
        a->ack_active = 0;
        free(a->ack_buf);
        a->ack_buf = NULL;
        return;
    }
    a->ack_tries++;
    a->n_ack_retry++;
    say(a, "剪贴板未确认 → 第 %d 次重发 %zu 字节（crc=%08x）", a->ack_tries, a->ack_len, a->ack_crc);
    send_clipboard(a, a->ack_buf, a->ack_len);     /* 只发，不重新登记 */
    a->ack_deadline_ms = now_ms() + 1500;
}

/* 保活：所有传输发 PING（可探测对端存活）；线缆模式额外发全零 dummy 帧冲刷设备
 * （对应厂商 SendDummyData / processDummyState —— 对端会把它判为空闲，故对应用层透明）。 */
static void *keepalive_thread(void *arg)
{
    struct app *a = arg;
    while (a->running) {
        for (int slept = 0; slept < a->cfg.keepalive_ms && a->running; slept += 50)
            usleep(50 * 1000);
        if (!a->running)
            break;
        uint8_t buf[64];
        pthread_mutex_lock(&a->lock);
        uint32_t seq = ++a->seq;
        pthread_mutex_unlock(&a->lock);
        long up = now_ms() % 1000000;
        size_t n = oti_encode_ping(seq, (uint32_t)up, buf, sizeof(buf));
        if (n)
            note_send_result(a, oti_tr_send(a->tx, buf, n));
        a->n_keepalive++;
        /* 每轮重新取电缆设备：reopen_transport() 会换掉 a->tx，上一代随后被
           oti_tr_close() free —— transport 约 4MB 且是 mmap 出来的，free 即 munmap，
           把 &a->tx->dev 缓存成裸指针就是 use-after-free（真机表现为段错误）。
           见 re/NOTES.md §53。 */
        struct otilink_dev *cable = oti_tr_cable_dev(a->tx);
        /* ⚠️ dummy 帧（64KB 全零）**只在链路真空闲时才发**：
           实测（2026-09-21）单次 dummy 要 ~1.3 秒且常常直接失败（rc=526080，DID_ERROR）——
           它和 HID 包共用同一台单队列设备，活跃使用期间每 5 秒发一次 = 键鼠每几秒
           完全冻结 1 秒多（用户报障原话："键鼠过几秒就会卡一下"，hidlat 实测
           maxgap=1285ms）。而链路现在根本不缺活动：ROLE 心跳每 1 秒一帧。
           dummy 的原始目的是防"长时间空闲 → 设备侧会话卡死"（NOTES §48），
           所以改成：**60 秒内对端没有任何消息、也没有帧发出**才补一次；
           并且必须和设备 I/O 互斥（它会插进"读授权→立刻写帧"的窗口，把授权作废）。 */
        if (cable) {
            long last_act = a->per_ms;
            long ctx = oti_tr_cable_last_tx_ms(a->tx);
            if (ctx > last_act)
                last_act = ctx;
            long now2 = now_ms();
            if (last_act == 0 || now2 - last_act > 60000) {
                otilink_cable_io_lock();
                int drc = otilink_send_dummy(cable);
                otilink_cable_io_unlock();
                if (a->cfg.verbose)
                    say(a, "KEEPALIVE #%lu：链路空闲 >60s → dummy 帧 rc=%d", a->n_keepalive, drc);
            }
        }
        if (a->cfg.verbose)
            say(a, "KEEPALIVE #%lu%s", a->n_keepalive, cable ? " + dummy 帧" : "");
    }
    return NULL;
}

static void *clip_thread(void *arg)
{
    struct app *a = arg;
    while (a->running) {
        clip_ack_check(a);              /* 剪贴板没被确认就重发（丢帧自愈） */
        /* 大文件传输进行中：这个线程全力推进（每轮发一批片） */
        if (oti_xfer_tx_active(&a->xfer_tx)) {
            /* 双向**同时**传大文件会把这条管道拖到 0.1MB/s（实测 20MB 要 249 秒，
               因为两边互相抢设备授权）。用 fid 大小做确定性让路：大的先停，
               等小的传完再继续 —— 无锁、无死锁（两边比较的是同一对数）。 */
            if (oti_xfer_rx_active(&a->xfer_rx) && a->xfer_tx.fid > a->xfer_rx.fid) {
                usleep(20 * 1000);
                continue;
            }
            oti_xfer_tx_poll(&a->xfer_tx);
            continue;
        }
        usleep(a->cfg.clip_interval_ms * 1000);
        if (!a->running)
            break;
        oti_xfer_rx_housekeep(&a->xfer_rx);

        /* 先做"是不是文件"的**轻量探测**：只 stat，不读内容。
           （以前是每 300ms 把文件整个读一遍算 CRC —— 8MB 文件就把磁盘读爆，
             500MB 根本不可能。现在用 路径+大小+mtime 指纹判断"还是那一份"。） */
        {
            static char files[OTI_CLIP_MAX_FILES][OTI_CLIP_PATH_MAX];
            int nf = 0;
            uint64_t ftotal = 0;
            uint32_t ident = 0;
            int isfile = oti_clip_probe_files(&a->clip, files, OTI_CLIP_MAX_FILES, &nf, &ftotal,
                                              &ident);
            if (isfile == 1) {
                if (a->clip.have_ident && ident == a->clip.last_ident)
                    continue;                        /* 同一份文件：不发 */
                /* 文件包本身要装进 8MB 的上限里（Windows 侧同一判据） */
                uint64_t budget = (a->cfg.clip_max < 8000000ull) ? (uint64_t)a->cfg.clip_max
                                                                 : 8000000ull;
                uint64_t overhead = 4096;            /* 头部/名字的粗略开销 */
                if (ftotal + overhead > budget) {
                    if (oti_xfer_rx_active(&a->xfer_rx)) {
                        /* 正在收对端的大文件：别同时开自己的发送（那条路会互相拖慢） */
                        continue;
                    }
                    char paths[OTI_XFER_MAX_FILES][OTI_XFER_PATH_MAX];
                    int k = nf < OTI_XFER_MAX_FILES ? nf : OTI_XFER_MAX_FILES;
                    for (int i = 0; i < k; i++)
                        snprintf(paths[i], OTI_XFER_PATH_MAX, "%s", files[i]);
                    if (oti_xfer_tx_begin(&a->xfer_tx, paths, k, budget - overhead)) {
                        a->clip.last_ident = ident;
                        a->clip.have_ident = 1;
                        say(a, "剪贴板里是**大文件**（%llu 字节 / %d 个）→ 分片流式传输（收端直接落盘）",
                            (unsigned long long)ftotal, k);
                    }
                } else {
                    char *pkg = NULL;
                    size_t plen = 0;
                    if (oti_clip_pack_files(&a->clip, files, nf, &pkg, &plen) == 0 &&
                        pkg && plen > 2) {
                        a->clip.last_format = OTI_CLIP_FMT_FILES;
                        send_clipboard(a, (const uint8_t *)pkg, plen);
                        clip_ack_register(a, pkg, plen, oti_crc32(pkg, plen), OTI_CLIP_FMT_FILES);
                        a->clip.last_ident = ident;
                        a->clip.have_ident = 1;
                        a->clip.last_seen = oti_crc32(pkg, plen);   /* 防回环 */
                        a->clip.have_seen = 1;
                    }
                    free(pkg);
                }
                continue;
            }
        }

        char *data = NULL;
        size_t len = 0;
        int rc = oti_clip_read(&a->clip, &data, &len);
        if (rc != 0 || !data) {
            free(data);
            continue;
        }
        if (len > a->cfg.clip_max) {
            /* 超限以前是**静默丢弃**：对端永远等不到，日志里一个字都没有，
               排查时看不出是"太大"还是"没触发"。按长度去重，避免每 300ms 刷屏。 */
            static size_t warned_len;
            if (len != warned_len) {
                warned_len = len;
                say(a, "[warn] 剪贴板内容 %zu 字节超过上限 %zu —— 本次不发送"
                       "（可调 --clip-max / config: clip_max，或改用共享卷方案）",
                    len, a->cfg.clip_max);
            }
        } else if (len > 0) {
            uint32_t h = oti_crc32(data, len);
            int need_send = 0;
            pthread_mutex_lock(&a->lock);
            if (!a->clip.have_seen || h != a->clip.last_seen) {
                a->clip.have_seen = 1;
                a->clip.last_seen = h;
                need_send = 1;
            }
            pthread_mutex_unlock(&a->lock);
            if (need_send) {
                send_clipboard(a, (const uint8_t *)data, len);
                clip_ack_register(a, data, len, h, a->clip.last_format ? a->clip.last_format : 1);
            }
        }
        free(data);
    }
    return NULL;
}

/* ---------- RX 线程 ---------- */
/* 看门狗：我们是驱动侧（独占着本机键鼠），但待发数据长时间送不出去
   —— 说明对端没在消费（程序挂了 / 链路断了 / 设备复位）。
   此时**必须**把控制权交还本机，否则用户会完全失控（这是上一版最严重的问题）。
   判据只在"确实有东西要发却发不出去"时成立，所以用户单纯不动鼠标不会误触发。 */
static void watchdog_check(struct app *a)
{
    if (a->cfg.idle_release_ms <= 0)
        return;
    pthread_mutex_lock(&a->lock);
    int driving = !a->core.have_control;
    pthread_mutex_unlock(&a->lock);
    if (!driving)
        return;

    if (a->cfg.peer_hid) {
        /* HID 包是**直接写设备**的（不进帧队列），通道里根本没有"对端消费"这个信号。
           绝不能沿用帧队列看门狗：对端只要不跑我们的 agent，帧队列就永远不消费，
           看门狗会每隔 idle_release 毫秒把控制权抢回本机一次 ——
           实测现象就是"鼠标刚移到 Windows 就被弹回来，根本过不去"。
           HID 模式下改用**写失败**作为链路故障判据。 */
        if (a->hid_fail_streak < 20)
            return;
        pthread_mutex_lock(&a->lock);
        otikm_core_force_local(&a->core);
        pthread_mutex_unlock(&a->lock);
        apply_grab(a, 0);
        say(a, "!! 看门狗：HID 包连续 %d 次写失败 → 已自动交还本机控制", a->hid_fail_streak);
        a->hid_fail_streak = 0;
        return;
    }

    if (oti_tr_cable_pending(a->tx) <= 0)
        return;                                  /* 没有待发数据：链路状态未知，不动 */
    long base = oti_tr_cable_last_tx_ms(a->tx);
    if (base < a->drive_started_ms)
        base = a->drive_started_ms;              /* 刚接管还没成功发过：从接管时刻算 */
    if (base == 0 || now_ms() - base < a->cfg.idle_release_ms)
        return;

    pthread_mutex_lock(&a->lock);
    otikm_core_force_local(&a->core);
    pthread_mutex_unlock(&a->lock);
    oti_tr_cable_drop_tx(a->tx);
    apply_grab(a, 0);
    say(a, "!! 看门狗：对端 %dms 未消费待发数据 → 已**自动交还本机控制**（键鼠恢复本地）",
        a->cfg.idle_release_ms);
}

/* 周期性把本机屏幕几何告诉对端（对端据此换算绝对坐标）。
   HELLO 很便宜（4 字节），10 秒一次足够，也避免占用授权窗口。 */
static void maybe_send_hello(struct app *a)
{
    /* HID 直发模式：对端是"线缆自带的真实 HID 设备"，没有我们的协议栈，
       HELLO（屏幕几何）无人消费，只会把帧队列堆满、并触发看门狗误判。 */
    if (a->cfg.peer_hid)
        return;
    long now = now_ms();
    if (a->hello_last_ms != 0 && now - a->hello_last_ms < 10000)
        return;
    a->hello_last_ms = now;
    oti_hello_evt he = {.width = (uint16_t)(a->core.local_w > 0 ? a->core.local_w : 1920),
                        .height = (uint16_t)(a->core.local_h > 0 ? a->core.local_h : 1080)};
    uint8_t buf[64];
    uint32_t seq = __sync_add_and_fetch(&a->seq, 1);
    size_t l = oti_encode_hello(seq, &he, buf, sizeof(buf));
    if (l)
        note_send_result(a, oti_tr_send(a->tx, buf, l));
}

static int reopen_transport(struct app *a);   /* 传输看门狗：拔插后自愈 */

/* W5：设备级复位（最后手段，需要 root）。
   为什么必须要有它：真机实测（NOTES 第 46 轮续）当线缆会话进入写侧故障后，
   **reopen 成功但写仍然全失败**（连续 5 次 reopen 都救不回来），
   而 USB unbind/bind（= 等价于拔插）之后立刻干净 —— 这正是用户要求的
   "最差的情况，对拷线的任何一端重拔插要复位"。绿色包本来就是 sudo 起的，所以能自动做。
   返回：0 = 已复位；-1 = 找不到设备/写失败；-2 = 没有 root 权限。 */
static int usb_reset_cable(void)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d)
        return -1;
    char name[64] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[256], v[16] = "";
        snprintf(p, sizeof(p), "/sys/bus/usb/devices/%.64s/idVendor", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        int hit = (fgets(v, sizeof(v), f) && strncmp(v, "0ea0", 4) == 0);
        fclose(f);
        if (hit) {
            snprintf(name, sizeof(name), "%.63s", e->d_name);
            break;
        }
    }
    closedir(d);
    if (!name[0])
        return -1;
    if (geteuid() != 0)
        return -2;
    char p[256];
    snprintf(p, sizeof(p), "/sys/bus/usb/drivers/usb/unbind");
    int fd = open(p, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t w1 = write(fd, name, strlen(name));
    close(fd);
    if (w1 != (ssize_t)strlen(name))
        return -1;
    usleep(800 * 1000);
    snprintf(p, sizeof(p), "/sys/bus/usb/drivers/usb/bind");
    fd = open(p, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t w2 = write(fd, name, strlen(name));
    close(fd);
    if (w2 != (ssize_t)strlen(name))
        return -1;
    usleep(1500 * 1000);              /* 等重新枚举（sg 号通常会变，靠 auto 探测找回来） */
    return 0;
}

/* W1b：**发送侧**看门狗。原来的传输看门狗只盯 recv；线缆会话一旦进入"写全失败、读偶尔成功"
   的状态（SCSI status=CHECK_CONDITION / host_status=DID_NO_CONNECT，rc=0x80102，真机实测
   每秒 ROLE 全是 rc=524546），读侧看门狗永远不触发 → 链路再也不会自愈，只能人工拔插。
   这里补上：连续 N 次写失败就重开线缆会话（与读失败共用同一条自愈路径，仍在 RX 线程执行）。 */
static void send_fail_watchdog(struct app *a)
{
    /* HID 直发模式下**不要**用帧写失败去 reopen/复位：帧通道与 HID 通道是两条路径，帧写失败
       不代表鼠标过不去；而 reopen_transport() 会 apply_grab(0)+force_local() —— 真机症状就是
       "鼠标正移到 Windows 就被拽回 Linux"。HID 的链路故障由 hid_fail_streak 负责
       （连续 20 次写失败 → 自动交还本机）。 */
    if (a->cfg.peer_hid) {
        a->send_fail_count = 0; a->send_ok_count = 0; a->send_reset_tries = 0;
        a->send_win_ms = now_ms();
        return;
    }
    long now = now_ms();
    if (!a->send_win_ms) {
        a->send_win_ms = now;
        return;
    }
    if (now - a->send_win_ms < 10000)
        return;
    int f = a->send_fail_count, ok = a->send_ok_count;
    a->send_win_ms = now;
    a->send_fail_count = 0;
    a->send_ok_count = 0;
    /* 判据：**10 秒窗口内一次都没成功过（ok==0）且失败 ≥10 次** = 链路真的死了。
       ⚠️ 早期版本用"失败 ≥5 且失败多于成功"，太激进：线缆忙/对端 drain 慢时会偶发失败，
       而 reopen_transport() 会 apply_grab(0) + force_local() —— 等于**把用户正驱动到对端的
       指针硬拉回本机**（真机症状："鼠标移到 Windows 就弹回来/根本过不去"）。所以现在只在
       "整窗零成功"时才认定故障；recv 侧的真实拔插仍由原有 err_streak 看门狗负责。
       为什么不是"连续失败 N 次"：真机实测写失败是"大部分失败、偶尔成功"（CLIP 失败而某次
       PING 成功），连续计数会被那次成功清零 → 看门狗永远不触发，链路永远不自愈。
       为什么要求 f > ok：链路正常但偶发失败（设备忙）不能重开，否则自己制造抖动。 */
    if (f >= 10 && ok == 0) {
        a->send_reset_tries++;
        say(a, "!! 看门狗：10 秒内发送失败 %d 次 / 成功 %d 次（写侧链路故障，第 %d 个故障窗口）",
            f, ok, a->send_reset_tries);
        if (a->send_reset_tries >= 2) {
            /* reopen 只重开 sg 设备；真机实测对这个故障**无效** → 升级到设备级复位 */
            int rr = usb_reset_cable();
            if (rr == 0) {
                a->send_reset_tries = 0;
                say(a, "!! 看门狗：reopen 无效 → 已做 **USB 设备级复位**（等价拔插），重新探测传输");
            } else if (rr == -2) {
                a->send_reset_tries = 1;     /* 留着计数，下次仍会尝试升级 */
                if (!a->send_hint_ms || now - a->send_hint_ms > 60000) {
                    a->send_hint_ms = now;
                    say(a, "!! 看门狗：写侧链路故障且 reopen 无效；当前无 root 权限做 USB 复位 —— "
                           "请**拔插对拷线**，或用 sudo 启动 otikm（绿色包本来就是 sudo 起的）");
                }
            } else {
                a->send_reset_tries = 0;
                say(a, "!! 看门狗：USB 复位失败（找不到 0ea0 设备或 sysfs 不可写）");
            }
        }
        reopen_transport(a);
    } else {
        a->send_reset_tries = 0;          /* 窗口干净：故障计数归零 */
    }
}

static void *rx_thread(void *arg)
{
    struct app *a = arg;
    static uint8_t body[OTI_BODY_MAX];
    int err_streak = 0;                /* 连续读失败计数（传输看门狗用） */
    while (a->running) {
        size_t len = 0;
        maybe_send_hello(a);
        /* ---- 热插拔重采样（第 46 轮续）：has_local_input 决定"谁该当主控"，而用户会**物理搬键鼠**。
           只在启动时采一次的话，换边后两侧认知都过期 → 永远换不过去（用户要的"插哪边都行"）。
           每 2 秒只读枚举一次（不碰抓取 fd），变化就立刻广播 ROLE 并重新决策。
           ⚠️ 2026-09-21 修正：判据必须看**个数**，不能只看 >0。
           真机 bug：先插鼠标（个数 1，已经是 master）再插键盘（个数 1→2），
           ">0" 不变 → 不重扫抓取集合 → **新插的键盘永远不被抓、在对端用不了**
           （用户报障原话："键盘也用不了"）。现在个数一变就 cap_rescan。 ---- */
        if (now_ms() - a->local_probe_ms >= 2000) {
            a->local_probe_ms = now_ms();
            int nlocal = oti_input_local_count();
            if (nlocal != a->local_count) {
                int prev = a->local_count;
                a->local_count = nlocal;
                a->has_local_input = nlocal > 0;
                say(a, "角色协商: 本机键鼠%s（现在 %d 个，之前 %d）→ 立即重新协商并重扫抓取集合",
                    nlocal > prev ? "插入" : "移除", nlocal, prev);
                a->cap_rescan = 1;
                maybe_send_role(a, 1);
                role_decide_and_queue(a);
            }
        }
        maybe_send_role(a, 0);
        /* 回程令牌 / 释放包：必须由**本线程（消息泵所在线程）**发。
           捕获线程自己发会跟这里抢同一条帧管道上的授权消息与应答，实测令牌被吞
           → 对端收不到"交还控制权"，指针永远回不来。顺序：先松手，再发令牌。 */
        if (a->want_release_all || a->want_token) {
            struct otilink_dev *tcd = oti_tr_cable_dev(a->tx);
            if (a->want_release_all) {
                a->want_release_all = 0;
                int rrc = tcd ? otilink_hid_release_all(tcd) : -1;
                if (rrc)
                    say(a, "ERR 释放包 rc=%d", rrc);
            }
            if (a->want_token) {
                a->want_token = 0;
                int trc = tcd ? otilink_hid_send_token(tcd, OTI_HID_TOKEN_PASSIVE) : -1;
                say(a, "→ 回程令牌 F24 已发（消息泵线程，rc=%d%s）", trc,
                    trc ? " <-- 写设备失败，对端收不到！" : "");
            }
        }
        /* 撞边交还：厂商帧必须由**本线程（消息泵所在线程）**发。
           捕获线程自己发会跟这里抢同一条消息管道上的授权消息，实测极不可靠。 */
        if (a->want_vendor_return) {
            a->want_vendor_return = 0;
            /* 第 40 轮现场教训：这条厂商 XML（668 字节）每 1.5 秒一帧，和剪贴板帧走同一条
               帧管道、争同一个发送授权窗口 —— 指针停在边缘时它会把剪贴板帧挤掉，
               表现为"L→W 复制粘贴失败、发 4 次全无确认后放弃"，而 W→L 一直正常。
               它只对"对端跑厂商 Windows 程序"有意义（L1 已把厂商栈关停），故**默认关闭**，
               要接厂商端时才 --vendor-notify on，并强制 >=5s 节流。 */
            static long last_vendor_notify_ms;
            long vnow = now_ms();
            if (!a->cfg.vendor_notify) {
                /* 关：什么都不发（默认路径） */
            } else if (vnow - last_vendor_notify_ms >= 5000) {
                last_vendor_notify_ms = vnow;
                struct otilink_dev *vcd = oti_tr_cable_dev(a->tx);
                if (vcd)
                    notify_vendor_switch_local(a, vcd);
            }
        }
        watchdog_check(a);
        send_fail_watchdog(a);
        int rc = oti_tr_recv(a->tx, body, &len, 200);
        if (rc == -2) {
            err_streak = 0;                /* 超时 = 链路正常（只是暂时没数据） */
            continue;
        }
        if (rc == -1) {                    /* 对端关闭 */
            say(a, "对端断开，退出接收线程");
            a->running = 0;
            break;
        }
        if (rc) {
            err_streak++;
            if (err_streak <= 3 || (err_streak % 20) == 0)
                say(a, "ERR recv rc=%d（连续 %d 次）", rc, err_streak);
            /* 连续读失败 = 设备没了/被复位/设备号变了 → 自愈重开（拔插复原先靠它） */
            if (err_streak >= 20) {
                err_streak = 0;
                reopen_transport(a);
            }
            usleep(50 * 1000);             /* 避免错误空转刷屏 */
            continue;
        }
        oti_msg_hdr h;
        const uint8_t *pl;
        int dr = oti_decode(body, len, &h, &pl);
        if (dr) {
            /* 同一条帧管道上还有**厂商 XML 帧**（帧体 [0x39][u32 长度][XML]）：
               otikm 撞边交还时会按厂商线格式发，厂商程序 MacKMLink 也在发。
               它们不是我们的协议消息，别当"解包失败"报警（Windows 侧同样处理）。 */
            if (len >= 5 && body[0] == 0x39) {
                static unsigned long n_vendor;
                n_vendor++;
                if (a->cfg.verbose || n_vendor <= 3)
                    say(a, "[skip] 厂商 XML 帧（第 %lu 条，非本协议，正常忽略）", n_vendor);
            } else {
                static unsigned long n_bad;
                n_bad++;
                if (n_bad <= 5 || (n_bad % 100) == 0)
                    say(a, "ERR decode rc=%d（丢弃，累计 %lu 条）", dr, n_bad);
            }
            continue;
        }
        switch (h.type) {
        case OTI_MSG_KEY: {
            oti_key_evt k;
            if (oti_decode_key(pl, h.len, &k) == 0) {
                do_inject(a, 0, NULL, &k);
                if (a->cfg.verbose)
                    say(a, "RECV key code=%u val=%u → 注入", k.code, k.value);
            }
            break;
        }
        case OTI_MSG_MOUSE: {
            oti_mouse_evt m;
            if (oti_decode_mouse(pl, h.len, &m) == 0) {
                do_inject(a, 1, &m, NULL);
                if (a->cfg.verbose)
                    say(a, "RECV mouse dx=%d dy=%d → 注入", m.dx, m.dy);
            }
            break;
        }
        case OTI_MSG_MOUSE_ABS: {
            /* 对端在驱动本机：uinput 只能相对注入，用上一次位置换算成相对位移 */
            oti_mouse_abs_evt ma;
            if (oti_decode_mouse_abs(pl, h.len, &ma) == 0) {
                if (!a->abs_have) {
                    a->abs_last_x = ma.x;
                    a->abs_last_y = ma.y;
                    a->abs_have = 1;
                }
                oti_mouse_evt m = {.dx = (int16_t)(ma.x - a->abs_last_x),
                                   .dy = (int16_t)(ma.y - a->abs_last_y),
                                   .wheel = ma.wheel, .buttons = ma.buttons};
                a->abs_last_x = ma.x;
                a->abs_last_y = ma.y;
                do_inject(a, 1, &m, NULL);
                if (a->cfg.verbose)
                    say(a, "RECV mouse ABS x=%d y=%d → 注入 Δ(%d,%d)", ma.x, ma.y, m.dx, m.dy);
            }
            break;
        }
        case OTI_MSG_ROLE: {
            oti_role_evt re;
            if (oti_decode_role(pl, h.len, &re) == 0) {
                a->per_seen = 1;
                a->per_ms = now_ms();
                a->per_has_input = re.has_local_input;
                a->per_want = re.want;
                a->per_state = re.state;
                a->per_flags = re.flags;
                a->per_boot_id = re.boot_id;
                a->per_input_age_ms = re.input_age_ms;
                if (re.state == OTI_ROLE_SLAVE)
                    a->per_slave_streak++;
                else
                    a->per_slave_streak = 0;
                role_decide_and_queue(a);
            }
            break;
        }
        case OTI_MSG_HELLO: {
            oti_hello_evt he;
            if (oti_decode_hello(pl, h.len, &he) == 0) {
                pthread_mutex_lock(&a->lock);
                otikm_core_set_remote_screen(&a->core, he.width, he.height);
                pthread_mutex_unlock(&a->lock);
                a->remote_w = he.width;
                a->remote_h = he.height;
                if (a->cfg.verbose)
                    say(a, "RECV HELLO 对端屏幕 %ux%u（绝对坐标按此换算）", he.width, he.height);
            }
            break;
        }
        case OTI_MSG_MOUSE_MODE: {
            uint8_t md = 0;
            if (oti_decode_mouse_mode(pl, h.len, &md) == 0) {
                a->mouse_native = (md == OTI_MOUSE_NATIVE);
                if (a->cfg.verbose)
                    say(a, "RECV MOUSE_MODE → %s", a->mouse_native
                             ? "相对位移（走对端系统指针速度）" : "绝对坐标（1:1，无加速）");
            }
            break;
        }
        case OTI_MSG_SWITCH: {
            oti_switch_evt sw;
            if (oti_decode_switch(pl, h.len, &sw) == 0) {
                pthread_mutex_lock(&a->lock);
                otikm_core_remote_switch(&a->core, &sw);
                int hc = a->core.have_control;
                pthread_mutex_unlock(&a->lock);
                a->n_switches++;
                say(a, "RECV SWITCH side=%s → 指针%s本机",
                    sw.side == OTI_SIDE_REMOTE ? "remote" : "local", hc ? "在" : "不在");
                if (!hc)
                    a->drive_started_ms = now_ms();
                apply_grab(a, hc ? 0 : 1);
            }
            break;
        }
        case OTI_MSG_CLIP: {
            oti_clip_hdr ch;
            const uint8_t *data;
            uint32_t dlen;
            if (oti_decode_clip(pl, h.len, &ch, &data, &dlen) == 0) {
                /* 大文件分片 / 传输控制：先于普通剪贴板重组分派（见 otixfer.h） */
                if (ch.format == OTI_CLIP_FMT_PART) {
                    /* u32 idx, u32 nparts, u64 total, u32 namelen, name, u32 dlen2, data */
                    if (dlen >= 24) {
                        uint32_t idx = xrd32(data), nparts = xrd32(data + 4);
                        uint64_t total = xrd64(data + 8);
                        uint32_t nl = xrd32(data + 16);
                        if (20u + nl + 4u <= dlen) {
                            char name[256];
                            uint32_t cn = nl < sizeof(name) - 1 ? nl : sizeof(name) - 1;
                            memcpy(name, data + 20, cn);
                            name[cn] = 0;
                            uint32_t dl2 = xrd32(data + 20 + nl);
                            if (20u + nl + 4u + dl2 <= dlen)
                                oti_xfer_rx_part(&a->xfer_rx, ch.fid, idx, nparts, total, name, nl,
                                                 data + 20 + nl + 4, dl2);
                        }
                    }
                    break;
                }
                if (ch.format == OTI_CLIP_FMT_CTRL) {
                    /* u16 kind, u16 flags, u64 total, u32 crc, u32 nparts, u32 resume_from,
                       u32 namelen, name */
                    if (dlen >= 28) {
                        uint16_t kind = xrd16(data), flags = xrd16(data + 2);
                        uint64_t total = xrd64(data + 4);
                        uint32_t crc = xrd32(data + 12), nparts = xrd32(data + 16);
                        uint32_t resume = xrd32(data + 20), nl = xrd32(data + 24);
                        char name[256];
                        uint32_t cn = nl < sizeof(name) - 1 ? nl : sizeof(name) - 1;
                        if (28u + cn <= dlen)
                            memcpy(name, data + 28, cn);
                        else
                            cn = 0;
                        name[cn] = 0;
                        if (kind == OTI_XFER_VERDICT) {
                            /* 对端对我们发出的大文件的裁决（可能带缺片位图） */
                            say(a, "RECV 裁决 fid=%u flags=0x%x resume=%u（tx %s）", ch.fid, flags,
                                resume, oti_xfer_tx_active(&a->xfer_tx) ? "进行中" : "已空闲");
                            const uint8_t *map = NULL;
                            uint32_t map_len = 0;
                            if (28u + cn < dlen) {
                                map = data + 28 + cn;
                                map_len = dlen - 28 - cn;
                            }
                            oti_xfer_tx_verdict(&a->xfer_tx, flags, resume, map, map_len);
                        } else {
                            oti_xfer_rx_ctrl(&a->xfer_rx, ch.fid, kind, flags, total, crc, nparts,
                                             resume, name, cn);
                        }
                    }
                    break;
                }
                int fr = oti_clip_feed(&a->clip, &ch, data, dlen);
                if (fr == 1) {
                    size_t rlen = 0;
                    const uint8_t *res = oti_clip_result(&a->clip, &rlen);
                    /* 顺序很重要：先登记哈希（锁内），再写剪贴板。
                       否则轮询线程可能在"写入完成、哈希未记"的窗口里把远端内容
                       误判成本地新内容而回发（实测偶发）。 */
                    uint32_t rh = oti_crc32(res, rlen);
                    pthread_mutex_lock(&a->lock);
                    a->clip.have_seen = 1;
                    a->clip.last_seen = rh;
                    pthread_mutex_unlock(&a->lock);
                    int wr = oti_clip_write(&a->clip, res, rlen);
                    say(a, "CLIP 应用远端剪贴板 %zu 字节（fmt=%u crc=%08x write rc=%d，已抑制回发）",
                        rlen, (unsigned)(ch.format ? ch.format : 1), rh, wr);
                    /* 回执：让对端知道这一笔真的落地了（没收到就重发） */
                    {
                        uint8_t ab[64];
                        size_t an = oti_encode_ack(++a->seq, rh, ch.format ? ch.format : 1, ab,
                                                   sizeof(ab));
                        if (an) {
                            /* 走**可靠**发送：ACK 很小，可它一丢，对端就会把整笔内容重发 3 轮
                               （实测主控模式下 6KB 图片被重发 8 次；8MB 包更亏）。
                               超时给短一点，别把接收线程拖住。 */
                            int arc = oti_tr_send_wait(a->tx, ab, an, 300);
                            note_send_result(a, arc);
                            if (arc)
                                say(a, "ERR send ACK rc=%d", arc);
                            else {
                                a->n_sent++;
                                if (a->cfg.verbose)
                                    say(a, "→ 回执 ACK crc=%08x（fmt=%u）", rh,
                                        (unsigned)(ch.format ? ch.format : 1));
                            }
                        }
                    }
                } else if (fr < 0) {
                    say(a, "CLIP 重组丢弃 rc=%d fid=%u off=%u", fr, ch.fid, ch.offset);
                }
            }
            break;
        }
        case OTI_MSG_ACK: {
            uint32_t acrc = 0;
            uint16_t afmt = 0;
            if (oti_decode_ack(pl, h.len, &acrc, &afmt) == 0) {
                if (a->cfg.verbose)
                    say(a, "RECV ACK crc=%08x（待确认=%08x active=%d）", acrc, a->ack_crc,
                        a->ack_active);
                if (a->ack_active && acrc == a->ack_crc) {
                    a->ack_active = 0;
                    a->n_ack_ok++;
                    free(a->ack_buf);
                    a->ack_buf = NULL;
                } else if (a->cfg.verbose) {
                    say(a, "RECV ACK crc=%08x（与待确认 %08x 不符，忽略）", acrc, a->ack_crc);
                }
            }
            break;
        }
        case OTI_MSG_PING:
            a->n_ping_recv++;
            if (a->cfg.verbose)
                say(a, "RECV PING #%lu", a->n_ping_recv);
            break;
        default:
            say(a, "RECV 未知消息类型 %u", h.type);
            break;
        }
    }
    return NULL;
}

/* ---------- 传输建立 ---------- */
static oti_transport *open_transport(const char *spec)
{
    if (strncmp(spec, "cable", 5) == 0) {
        const char *p = strchr(spec, ':');
        return oti_tr_open_cable(p ? p + 1 : NULL);
    }
    if (strncmp(spec, "listen:", 7) == 0) {
        const char *path = spec + 7;
        unlink(path);
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (s < 0)
            return NULL;
        struct sockaddr_un sa;
        memset(&sa, 0, sizeof(sa));
        sa.sun_family = AF_UNIX;
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
        if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) || listen(s, 1)) {
            close(s);
            return NULL;
        }
        int c = accept(s, NULL, NULL);
        close(s);
        if (c < 0)
            return NULL;
        return oti_tr_open_fd(c, 1);
    }
    /* TCP 后端：先在两台机器上用网络验证输入/剪贴板链路，再切换线缆 */
    if (strncmp(spec, "tcp-listen:", 11) == 0) {
        int port = atoi(spec + 11);
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0)
            return NULL;
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons((uint16_t)port);
        if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) || listen(s, 1)) {
            close(s);
            return NULL;
        }
        int c = accept(s, NULL, NULL);
        close(s);
        return c < 0 ? NULL : oti_tr_open_fd(c, 1);
    }
    if (strncmp(spec, "tcp-connect:", 12) == 0) {
        char host[128] = "127.0.0.1";
        int port = 0;
        if (sscanf(spec + 12, "%127[^:]:%d", host, &port) != 2)
            return NULL;
        for (int i = 0; i < 50; i++) {
            struct addrinfo hints, *res = NULL;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            char ps[8];
            snprintf(ps, sizeof(ps), "%d", port);
            if (getaddrinfo(host, ps, &hints, &res) == 0 && res) {
                int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
                if (s >= 0 && connect(s, res->ai_addr, res->ai_addrlen) == 0) {
                    freeaddrinfo(res);
                    return oti_tr_open_fd(s, 1);
                }
                if (s >= 0)
                    close(s);
                freeaddrinfo(res);
            }
            usleep(100 * 1000);
        }
        return NULL;
    }
    if (strncmp(spec, "connect:", 8) == 0) {
        const char *path = spec + 8;
        for (int i = 0; i < 50; i++) {
            int s = socket(AF_UNIX, SOCK_STREAM, 0);
            if (s < 0)
                return NULL;
            struct sockaddr_un sa;
            memset(&sa, 0, sizeof(sa));
            sa.sun_family = AF_UNIX;
            snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
            if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0)
                return oti_tr_open_fd(s, 1);
            close(s);
            usleep(100 * 1000);
        }
        return NULL;
    }
    return NULL;
}

/* ---------- sim 输入脚本 ---------- */

/* 重开传输：拔插线缆、/dev/sgN 变化、设备被复位之后自愈。
   两个前提：
     1) 先把控制权还给本机 —— 设备没了还攥着用户的键鼠 = 用户完全失控；
     2) 旧 transport **延迟关闭**（留到下一次重开时再关）—— 捕获线程可能正拿着
        旧指针在发 HID 包，立刻 free 就是 use-after-free。 */
static int reopen_transport(struct app *a)
{
    const char *spec = a->cfg.transport;
    if (!spec)
        return -1;
    say(a, "!! 看门狗：重新打开传输 %s", spec);
    pthread_mutex_lock(&a->lock);
    otikm_core_force_local(&a->core);
    pthread_mutex_unlock(&a->lock);
    apply_grab(a, 0);                       /* 别攥着用户的键鼠 */

    oti_transport *nt = NULL;
    if (strncmp(spec, "cable", 5) == 0) {
        const char *p = strchr(spec, ':');
        if (p && p[1] && access(p + 1, R_OK | W_OK) != 0) {
            say(a, "   %s 已不存在（拔插后设备号可能变了）→ 改为自动探测", p + 1);
            nt = oti_tr_open_cable(NULL);
        } else {
            nt = oti_tr_open_cable(p && p[1] ? p + 1 : NULL);
        }
    } else {
        nt = open_transport(spec);
    }
    if (!nt) {
        say(a, "ERR 重开传输失败（设备插好了吗？）");
        return -1;
    }
    if (a->tx_old)
        oti_tr_close(a->tx_old);            /* 关掉上一代（早没人用了） */
    a->tx_old = a->tx;
    a->tx = nt;
    a->hid_fail_streak = 0;
    say(a, "传输已重新就绪: %s", oti_tr_name(a->tx));
    return 0;
}

static void run_sim(struct app *a)
{
    FILE *f = fopen(a->cfg.sim_input, "r");
    if (!f) {
        say(a, "ERR 打不开 sim-input %s", a->cfg.sim_input);
        return;
    }
    char line[256];
    long deadline = a->cfg.duration_s > 0 ? now_ms() + a->cfg.duration_s * 1000L : -1;
    while (a->running && fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (strncmp(line, "sleep", 5) == 0) {
            int ms = atoi(line + 5);
            usleep(ms * 1000);
            continue;
        }
        if (strncmp(line, "key", 3) == 0) {
            unsigned code = 0, val = 0;
            if (sscanf(line + 3, "%u %u", &code, &val) == 2) {
                oti_key_evt k = {.code = (uint16_t)code, .value = (uint8_t)val};
                handle_local(a, 0, NULL, &k);
            }
        } else if (strncmp(line, "mouse", 5) == 0) {
            int dx = 0, dy = 0, wh = 0;
            unsigned btn = 0;
            if (sscanf(line + 5, "%d %d %d %u", &dx, &dy, &wh, &btn) >= 2) {
                oti_mouse_evt m = {.dx = (int16_t)dx, .dy = (int16_t)dy,
                                   .wheel = (int16_t)wh, .buttons = (uint16_t)btn};
                handle_local(a, 1, &m, NULL);
            }
        }
        if (deadline >= 0 && now_ms() > deadline)
            break;
        usleep(a->cfg.delay_ms * 1000);
    }
    fclose(f);

    /* 脚本跑完不代表该退出：继续运行到 duration，保持与对端的连接 */
    while (a->running && (deadline < 0 || now_ms() < deadline))
        usleep(50 * 1000);
}

/* ---------- 真实抓取 ---------- */
static void run_capture(struct app *a)
{
    /* duration_s == 0 表示"不限时"（与 main 里无输入源那条路径的语义一致）。
       注意：这里不能写成 now_ms() + 0，否则循环条件立刻为假、进程秒退。 */
    long deadline = a->cfg.duration_s > 0 ? now_ms() + a->cfg.duration_s * 1000L : -1;
    struct pollfd pf[8];
    const long cap_retry_ms = 1000;      /* 抓取看门狗：重开失败后的重试间隔 */
    long retry_at[8];
    int  fail_n[8], tries[8], grabdes[8], want_kind[8];
    for (int i = 0; i < a->ncap && i < 8; i++) {
        retry_at[i] = 0; fail_n[i] = 0; tries[i] = 0; grabdes[i] = a->cap[i].grabbed;
        want_kind[i] = a->cap[i].kind;
    }
    while (a->running && (deadline < 0 || now_ms() < deadline)) {
        long now = now_ms();
        /* ---- 角色切换 / 输入热插拔重扫（第 46/47 轮）：设备集合会被重建，看门狗状态跟着重置 ---- */
        {
            int role_before = a->applied_role;
            role_apply_pending(a);
            int rebuilt = (a->applied_role != role_before);
            /* 真机 bug（2026-09-21）：先插鼠标（个数 1 → 已是 master）再插键盘（个数 1→2）时
               角色不变，但抓取集合必须补上键盘 —— 否则新插的设备永远不进转发，
               用户表现就是"键鼠插在 linux，鼠标能过去、**键盘用不了**"。 */
            if (a->cap_rescan) {
                a->cap_rescan = 0;
                rebuilt = 1;
                pthread_mutex_lock(&a->lock);
                int driving = !a->core.have_control;
                pthread_mutex_unlock(&a->lock);
                role_rebuild_capture(a, a->applied_role == OTI_ROLE_MASTER);
                say(a, "输入热插拔：抓取集合已重扫（%d 个设备%s）", a->ncap,
                    driving ? "，恢复独占" : "");
                if (driving && a->applied_role == OTI_ROLE_MASTER)
                    apply_grab(a, 1);
            }
            if (rebuilt) {
                for (int i = 0; i < 8; i++) {
                    retry_at[i] = 0;
                    fail_n[i] = 0;
                    tries[i] = 0;
                    grabdes[i] = (i < a->ncap) ? a->cap[i].grabbed : 0;
                    want_kind[i] = (i < a->ncap) ? a->cap[i].kind : 0;
                }
            }
        }
        /* ---- 键盘独占：维持 + 安全网（2026-10-08 用户报障 + 2026-09-21 现场事故）------
           * 维持：apply_grab(1) 原来只在**状态切换那一刻**调用，而健康判据是会变的
             （对端 agent 起来/掉线、HID 写失败率升降）→ "那一刻没独占上就再也不独占"，
             用户接管对端期间**每一个按键都双重输入**（Windows 与麒麟各打一遍）。
             现在驱动期间周期性幂等重放（节流 500ms），判据一恢复就补上独占。
           * 安全网：通道不健康时立刻放开键盘独占（本地键盘恢复）。宁可短暂双输入，
             也绝不让用户打不了字。
           ⚠️ 两边的判据都必须跟着 **KM 载体**（km_path_ok）：HID 直发模式下帧管道静默
           是正常形态 —— 沿用"3 秒无成功帧写"会让独占每隔几秒被误放一次。 ---- */
        {
            pthread_mutex_lock(&a->lock);
            int drv = !a->core.have_control;
            pthread_mutex_unlock(&a->lock);
            if (drv) {
                if (!km_path_ok(a)) {
                    for (int i = 0; i < a->ncap; i++) {
                        if (a->cap[i].grabbed && (a->cap[i].kind & OTI_IN_KBD) &&
                            !(a->cap[i].kind & OTI_IN_MOUSE)) {
                            if (ioctl(a->cap[i].fd, EVIOCGRAB, 0) == 0) {
                                a->cap[i].grabbed = 0;
                                say(a, "!! KM 通道不健康（%s）→ 放开键盘独占（本地键盘恢复，防锁死）",
                                    a->cfg.peer_hid ? "HID 包连续写失败" : "帧管道 3 秒无成功发送");
                            }
                        }
                    }
                } else if (now - a->grab_maint_ms >= 500) {
                    a->grab_maint_ms = now;
                    apply_grab(a, 1);            /* 幂等：只在状态真的变化时打日志 */
                }
            }
        }
        /* ---- 输入抓取看门狗 ------------------------------------------------
           拔插 / 线缆 USB 重新枚举之后，/dev/input/by-id/... 会指向**新的** eventN，
           而 otikm 手里还是旧 inode 的 fd：poll 不再 POLLIN、read 返回 ENODEV →
           "撞边交还控制权"（回程手势）与本地热键**全部失效**，用户表现为
           "又卡在对端回不来"（真机实测见 re/NOTES.md §54）。
           传输层早有 reopen_transport() 自愈，这里给输入抓取补上同级的：
           报错就关掉，之后每秒用原 by-id 路径试着重开（by-id 跨拔插是稳定的）。 */
        for (int i = 0; i < a->ncap && i < 8; i++) {
            if (a->cap[i].fd >= 0 || now < retry_at[i])
                continue;
            const char *path = a->cfg.capture[i];
            if (!path || !path[0]) {
                retry_at[i] = now + 10000;          /* 没有原始路径可重开（正常不该发生） */
                continue;
            }
            if (oti_capture_open(&a->cap[i], path, 0) == 0 &&
                (want_kind[i] == 0 || (a->cap[i].kind & want_kind[i]))) {
                say(a, "输入抓取已重开: %s (%s)", path, a->cap[i].devname);
                want_kind[i] = a->cap[i].kind;
                if (grabdes[i])
                    apply_grab(a, 1);               /* 恢复原来的独占状态 */
                retry_at[i] = 0; fail_n[i] = 0; tries[i] = 0;
            } else {
                /* 真机踩过（2026-09-21）：eventN 拔插后可能被**别的设备**占用
                   （键盘的 event7 变成了 "SIGMACHIP USB Keyboard System Control"），
                   按旧 eventN 重开会"成功"但抓到错的设备 → 用户"键盘用不了"。
                   所以重开后必须校验能力位；不符就丢掉，并按类别重新发现设备。 */
                if (a->cap[i].fd >= 0) {
                    say(a, "[warn] 输入抓取重开到了不同设备：%s (%s kind=0x%x，期望 0x%x) → 丢弃重试",
                        path, a->cap[i].devname, a->cap[i].kind, want_kind[i]);
                    oti_capture_close(&a->cap[i]);
                }
                if (++tries[i] == 1 || (tries[i] % 10) == 0)
                    say(a, "ERR 输入抓取重开失败（第 %d 次）: %s（设备还没回来/编号变了？）", tries[i], path);
                if (tries[i] >= 3) {
                    /* 路径过期：按类别重新发现（autoselect 返回 by-id 稳定路径） */
                    char nk[64] = "", nm[64] = "";
                    oti_input_autoselect(a->applied_role == OTI_ROLE_MASTER ? 0 : 1, nk, nm);
                    const char *fresh = (want_kind[i] & OTI_IN_KBD) ? nk : nm;
                    if (fresh[0] && strcmp(fresh, path) != 0) {
                        static char heal[8][64];
                        snprintf(heal[i], sizeof(heal[i]), "%.63s", fresh);
                        a->cfg.capture[i] = heal[i];
                        say(a, "输入抓取路径已重新发现: %s → %s（第 %d 次失败后）",
                            path, fresh, tries[i]);
                        retry_at[i] = 0;
                        continue;
                    }
                }
                retry_at[i] = now + cap_retry_ms;
            }
        }
        for (int i = 0; i < a->ncap; i++) {
            pf[i].fd = a->cap[i].fd;                /* fd < 0 → 本轮不监听 */
            pf[i].events = a->cap[i].fd >= 0 ? POLLIN : 0;
        }
        int pr = poll(pf, a->ncap, 4);      /* 4ms：与位移合并间隔一致，保证尾包及时发出 */
        if (pr < 0)
            continue;
        if (pr == 0) {
            flush_mouse(a);
            continue;
        }
        for (int i = 0; i < a->ncap; i++) {
            if (a->cap[i].fd < 0)
                continue;
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                say(a, "!! 输入抓取 %s 报错 revents=0x%x → 关闭，等待重开",
                    a->cap[i].devname, pf[i].revents);
                grabdes[i] = a->cap[i].grabbed;
                oti_capture_close(&a->cap[i]);
                retry_at[i] = now_ms() + cap_retry_ms;
                fail_n[i] = 0;
                continue;
            }
            if (!(pf[i].revents & POLLIN))
                continue;
            uint16_t type, code;
            int32_t value;
            int nr = 0;
            while ((nr = oti_capture_next(&a->cap[i], &type, &code, &value)) == 1) {
                if (type == EV_KEY && code < BTN_MISC) {
                    oti_key_evt k = {.code = code, .value = (uint8_t)value};
                    handle_local(a, 0, NULL, &k);
                } else if (type == EV_KEY) {
                    oti_mouse_evt m = {.buttons = a->cap[i].buttons};
                    handle_local(a, 1, &m, NULL);
                } else if (type == EV_REL) {
                    oti_mouse_evt m = {.buttons = a->cap[i].buttons};
                    if (code == REL_X)
                        m.dx = (int16_t)value;
                    else if (code == REL_Y)
                        m.dy = (int16_t)value;
                    else if (code == REL_WHEEL)
                        m.wheel = (int16_t)value;
                    else
                        continue;
                    handle_local(a, 1, &m, NULL);
                }
            }
            if (nr < 0) {
                /* 抖动容忍几次，连续失败才判定"设备没了"（拔插后 read 返回 ENODEV） */
                if (++fail_n[i] >= 3) {
                    say(a, "!! 输入抓取 %s 连续读失败 rc=%d → 关闭，等待重开",
                        a->cap[i].devname, nr);
                    grabdes[i] = a->cap[i].grabbed;
                    oti_capture_close(&a->cap[i]);
                    retry_at[i] = now_ms() + cap_retry_ms;
                    fail_n[i] = 0;
                }
            } else {
                fail_n[i] = 0;
            }
        }
    }
}



/* ---------- 输入链路自检：验证 evdev 抓取 + uinput 注入真的能用 ----------
 * 思路：uinput 注入的事件会出现在我们自己创建的 /dev/input/eventN 上，
 * 因此可以"注入→回读→比对"，不需要 GUI，也不需要第二台机器。
 *   synth : 注入固定键序列再回读比对（全自动）
 *   grab  : 同 synth，但回读节点先 EVIOCGRAB 独占（验证抓取不影响自身读取）
 *   live  : 把 --capture 上的真实键鼠事件注入并回读（需要你敲键/动鼠标）
 */
static int find_inject_node(char *out, size_t cap)
{
    for (int tries = 0; tries < 40; tries++) {          /* uinput 节点可能稍后出现 */
        static char names[32][128], devs[32][64];
        int n = oti_input_list(names, devs, 32);
        for (int i = 0; i < n; i++) {
            if (strcmp(names[i], OTI_INJECT_NAME) == 0) {
                snprintf(out, cap, "%s", devs[i]);
                return 0;
            }
        }
        usleep(50 * 1000);
    }
    return -1;
}

static int selftest_input(const struct cfg *cfg, const char *mode)
{
    printf("== 输入链路自检（%s）==\n", mode);
    struct oti_inject inj;
    inj.fd = -1;
    int rc = oti_inject_open(&inj, OTI_INJECT_NAME);
    if (rc) {
        printf("  [FAIL] 无法创建 uinput 注入设备: %s\n", strerror(-rc));
        printf("         → sudo cp 99-otilink.rules /etc/udev/rules.d/ && sudo udevadm control --reload\n");
        printf("           sudo udevadm trigger && sudo usermod -aG input,disk $USER（重新登录）\n");
        return 1;
    }
    printf("  [OK]   uinput 注入设备已创建: %s\n", OTI_INJECT_NAME);

    char node[64];
    if (find_inject_node(node, sizeof(node)) != 0) {
        printf("  [FAIL] 未在 /dev/input 下找到注入设备节点（内核未生成？）\n");
        oti_inject_close(&inj);
        return 1;
    }
    printf("  [OK]   回读节点: %s\n", node);

    int want_grab = (strcmp(mode, "grab") == 0);
    struct oti_capture rb;
    if (oti_capture_open(&rb, node, want_grab) != 0) {
        printf("  [FAIL] 打不开回读节点 %s（权限？）\n", node);
        oti_inject_close(&inj);
        return 1;
    }
    if (want_grab) {
        printf("  [%s] EVIOCGRAB 独占回读节点%s\n", rb.grabbed ? "OK" : "FAIL",
               rb.grabbed ? "" : "（失败，检查权限）");
        if (!rb.grabbed) {
            oti_capture_close(&rb);
            oti_inject_close(&inj);
            return 1;
        }
    }

    int sent = 0, got = 0, mismatch = 0, fails_mouse = 0;
    const uint16_t codes[3] = {30 /*A*/, 31 /*S*/, 57 /*SPACE*/};

    if (strcmp(mode, "live") == 0) {
        struct oti_capture src[8];
        int ns = 0;
        for (int i = 0; i < cfg->ncapture && ns < 8; i++)
            if (oti_capture_open(&src[ns], cfg->capture[i], 0) == 0)
                ns++;
        if (!ns) {
            printf("  [FAIL] live 模式需要 --capture 指定真实键鼠设备（先用 --doctor 列出来）\n");
            oti_capture_close(&rb);
            oti_inject_close(&inj);
            return 1;
        }
        printf("  请在 %d 秒内敲几个键或动一下鼠标…\n", cfg->selftest_seconds);
        long end = now_ms() + cfg->selftest_seconds * 1000L;
        while (now_ms() < end) {
            for (int i = 0; i < ns; i++) {
                uint16_t t, c;
                int32_t v;
                while (oti_capture_next(&src[i], &t, &c, &v) == 1) {
                    if (t == EV_KEY && c < BTN_MISC) {
                        oti_inject_key(&inj, c, v);
                        sent++;
                    } else if (t == EV_KEY) {
                        oti_inject_mouse(&inj, 0, 0, 0, src[i].buttons);
                        sent++;
                    } else if (t == EV_REL) {
                        oti_inject_mouse(&inj, c == REL_X ? (int16_t)v : 0,
                                         c == REL_Y ? (int16_t)v : 0,
                                         c == REL_WHEEL ? (int16_t)v : 0, src[i].buttons);
                        sent++;
                    }
                }
            }
            uint16_t t, c;
            int32_t v;
            while (oti_capture_next(&rb, &t, &c, &v) == 1)
                if (t == EV_KEY || t == EV_REL)
                    got++;
            usleep(2 * 1000);
        }
        for (int i = 0; i < ns; i++)
            oti_capture_close(&src[i]);
    } else {
        /* 键盘 6 个事件 + 鼠标：位移(5,-3)/滚轮 1 + 左键按下抬起 */
        for (int i = 0; i < 3; i++) {
            oti_inject_key(&inj, codes[i], 1);
            oti_inject_key(&inj, codes[i], 0);
            sent += 2;
        }
        oti_inject_mouse(&inj, 5, -3, 1, 0);
        oti_inject_mouse(&inj, 0, 0, 0, 1);        /* 左键按下 */
        oti_inject_mouse(&inj, 0, 0, 0, 0);        /* 左键抬起 */
        sent += 5;
        usleep(300 * 1000);

        uint16_t t, c;
        int32_t v;
        int idx = 0, key_bad = 0, btn_ok = 0, rel_x = 0, rel_y = 0, rel_w = 0;
        while (oti_capture_next(&rb, &t, &c, &v) == 1) {
            if (t == EV_KEY && c < BTN_MISC) {
                if (idx < 6 && c == codes[idx / 2] && v == (idx % 2 ? 0 : 1))
                    got++;
                else
                    key_bad++;
                idx++;
            } else if (t == EV_KEY && c == BTN_LEFT) {
                if (v == 1)
                    btn_ok = 1;
                else if (v == 0 && btn_ok == 1)
                    btn_ok = 2;                    /* 按下后又抬起 */
            } else if (t == EV_REL) {
                if (c == REL_X && v == 5)
                    rel_x = 1;
                if (c == REL_Y && v == -3)
                    rel_y = 1;
                if (c == REL_WHEEL && v == 1)
                    rel_w = 1;
            }
        }
        mismatch += key_bad;
        int mouse_ok = (btn_ok == 2 && rel_x && rel_y && rel_w);
        printf("  [%s] 鼠标路径：左键按下+抬起=%s 位移X=%s 位移Y=%s 滚轮=%s\n",
               mouse_ok ? "PASS" : "FAIL", btn_ok == 2 ? "是" : "否",
               rel_x ? "是" : "否", rel_y ? "是" : "否", rel_w ? "是" : "否");
        fails_mouse = !mouse_ok;
    }

    oti_capture_close(&rb);
    oti_inject_close(&inj);

    int fails = 0;
    if (strcmp(mode, "live") == 0) {
        int ok = sent > 0 && got >= sent / 2;
        printf("  [%s] 源事件 %d 个，回读 %d 个（>=50%% 视为通过；被动丢事件正常）\n",
               ok ? "PASS" : "FAIL", sent, got);
        if (!ok)
            printf("         → 若 sent=0：没抓到输入（--doctor 看设备名/权限）；若 got 远小于 sent：注入异常\n");
        fails += !ok;
    } else {
        int ok = (sent == 11 && got == 6 && mismatch == 0 && !fails_mouse);
        printf("  [%s] 键盘 6 事件回读匹配 %d 个/错配 %d 个%s\n",
               (got == 6 && mismatch == 0) ? "PASS" : "FAIL", got, mismatch,
               want_grab ? "（在 EVIOCGRAB 独占下）" : "");
        fails += !ok;
    }
    printf("  => %s\n", fails ? "输入链路有问题（见上面提示）" : "输入链路可用（抓取/注入/回读全部打通）");
    return fails;
}

/* ---------- 环境自检（真机第一次跑之前的排雷） ---------- */
static int doctor(const char *transport)
{
    int fails = 0;
    printf("== otikm 环境自检 ==\n");

    /* 1) 抓取设备 */
    static char names[32][128], devs[32][64];
    int n = oti_input_list(names, devs, 32);
    if (n <= 0) {
        printf("  [FAIL] /dev/input 下看不到输入设备（%s）\n", strerror(errno));
        printf("         → 本机没有可抓取的键鼠，或容器/WSL 未暴露输入设备\n");
        fails++;
    } else {
        printf("  [OK]   发现 %d 个输入设备（用 --capture 指定要抓的）:\n", n);
        for (int i = 0; i < n; i++) {
            /* 分类列直接对应角色协商的判据：只有"USB(可热插拔)"的键鼠才算"本机键鼠" */
            int kind = oti_input_kind(devs[i]);
            const char *cls = oti_input_is_cable(devs[i]) ? "线缆HID"
                            : !oti_input_is_usb(devs[i])     ? "内置/PS2(不换边)"
                            : kind ? "USB(可热插拔)" : "USB(非键鼠)";
            printf("           %-22s %-40s %s%s %s\n", devs[i], names[i],
                   (kind & OTI_IN_KBD) ? "键" : "  ",
                   (kind & OTI_IN_MOUSE) ? "鼠" : "  ", cls);
        }
    }

    /* 2) uinput 注入 */
    struct oti_inject inj;
    inj.fd = -1;
    int rc = oti_inject_open(&inj, OTI_INJECT_NAME);
    if (rc) {
        printf("  [FAIL] 无法创建 uinput 注入设备: %s\n", strerror(-rc));
        printf("         → 需要权限（/dev/uinput 通常 root:root 0600）。修复：\n");
        printf("             sudo cp 99-otilink.rules /etc/udev/rules.d/\n");
        printf("             sudo udevadm control --reload && sudo udevadm trigger\n");
        printf("             sudo usermod -aG input,disk $USER   # 重新登录生效\n");
        fails++;
    } else {
        printf("  [OK]   uinput 注入设备可创建（%s）\n", OTI_INJECT_NAME);
        oti_inject_close(&inj);
    }

    /* 3) 剪贴板后端 + 往返校验（会先备份再恢复原内容） */
    struct oti_clip clip;
    if (oti_clip_open(&clip, NULL, 1u << 20) == 0) {
        char *orig = NULL;
        size_t olen = 0;
        int had = (oti_clip_read(&clip, &orig, &olen) == 0 && orig);
        const char *mark = "otilink-doctor-probe-42";
        int wr = oti_clip_write(&clip, mark, strlen(mark));
        char *back = NULL;
        size_t blen = 0;
        int rd = oti_clip_read(&clip, &back, &blen);
        int ok = (wr == 0 && rd == 0 && blen == strlen(mark) && memcmp(back, mark, blen) == 0);
        if (ok)
            printf("  [OK]   剪贴板后端 %s，写入/读回一致\n", oti_clip_backend_name(&clip));
        else
            printf("  [WARN] 剪贴板后端 %s，往返回失败（wr=%d rd=%d len=%zu）\n",
                   oti_clip_backend_name(&clip), wr, rd, blen);
        if (had)
            oti_clip_write(&clip, orig, olen);      /* 恢复用户原剪贴板 */
        free(orig);
        free(back);
        oti_clip_close(&clip);
    } else {
        printf("  [WARN] 剪贴板后端初始化失败\n");
    }

    /* 4) 线缆可见性 */
    char paths[16][64];
    int np = otilink_find(paths, 16);
    if (np <= 0) {
        printf("  [FAIL] 未发现 0ea0:2213 的 /dev/sgN —— 线缆对 Linux 不可见\n");
        printf("         → Windows 侧: usbipd bind --busid 1-1 && usbipd attach --wsl --busid 1-1\n");
        printf("         → 或把线的这一端直接插到 Linux 机器上\n");
        fails++;
    } else {
        printf("  [OK]   发现 %d 个 sg 设备:\n", np);
        for (int i = 0; i < np; i++) {
            char p[256], v[64] = "?", m[64] = "?", t[16] = "?";
            const char *nm = strrchr(paths[i], '/');
            FILE *f;
            snprintf(p, sizeof(p), "/sys/class/scsi_generic/%.48s/device/vendor", nm ? nm + 1 : paths[i]);
            f = fopen(p, "r"); if (f) { if (!fgets(v, sizeof(v), f)) v[0] = 0; fclose(f); }
            snprintf(p, sizeof(p), "/sys/class/scsi_generic/%.48s/device/model", nm ? nm + 1 : paths[i]);
            f = fopen(p, "r"); if (f) { if (!fgets(m, sizeof(m), f)) m[0] = 0; fclose(f); }
            snprintf(p, sizeof(p), "/sys/class/scsi_generic/%.48s/device/type", nm ? nm + 1 : paths[i]);
            f = fopen(p, "r"); if (f) { if (!fgets(t, sizeof(t), f)) t[0] = 0; fclose(f); }
            for (char *q = v; *q; q++) if (*q == '\n') *q = 0;
            for (char *q = m; *q; q++) if (*q == '\n') *q = 0;
            for (char *q = t; *q; q++) if (*q == '\n') *q = 0;
            printf("           %-10s type=%-5s vendor=%-8s model=%s\n", paths[i], t, v, m);
        }
        /* 顺带做一个安全的 0xF0/0x00 查询 */
        struct otilink_dev d;
        if (otilink_open(&d, paths[0]) == 0) {
            uint8_t buf[64] = {0};
            int qr = otilink_query(&d, OTI_SUB_STATUS, buf, sizeof(buf));
            printf("           %s 0xF0/0x00 查询: rc=%d\n", paths[0], qr);
            otilink_close(&d);
        } else {
            printf("  [WARN] 打开 %s 失败（权限？）\n", paths[0]);
        }
    }

    /* 5) 媒体挂载：厂商 Initialize() 第一步就是 UnmountMedia，传输期间两个 LUN 不应被挂载 */
    {
        FILE *mnt = fopen("/proc/mounts", "r");
        int mounted = 0;
        if (mnt) {
            char line[1024];
            while (fgets(line, sizeof(line), mnt)) {
                char dev[256], dir[512];
                if (sscanf(line, "%255s %511s", dev, dir) != 2)
                    continue;
                if (strncmp(dev, "/dev/sd", 7) != 0)
                    continue;
                char base[64], sys[256];
                const char *bn = strrchr(dev, '/');
                snprintf(base, sizeof(base), "%.48s", bn ? bn + 1 : dev);
                /* 去掉分区号：sdb1 → sdb */
                size_t bl = strlen(base);
                while (bl > 3 && base[bl - 1] >= '0' && base[bl - 1] <= '9')
                    base[--bl] = 0;
                snprintf(sys, sizeof(sys), "/sys/block/%s/device", base);
                if (otilink_is_our_usb(sys)) {
                    printf("  [WARN] 对拷线的分区 %s 已挂载在 %s\n", dev, dir);
                    printf("         → 厂商初始化第一步就是 UnmountMedia；建议先卸载：sudo umount %s\n", dir);
                    mounted++;
                }
            }
            fclose(mnt);
        }
        if (!mounted)
            printf("  [OK]   对拷线的 LUN 未被挂载（符合厂商 UnmountMedia 前提）\n");
    }

    if (transport)
        printf("  [INFO] 传输将使用: %s（自检不建立连接）\n", transport);
    printf("  => %s\n", fails ? "有问题需要先修复（见上面 → 提示）" : "环境就绪");
    return fails;
}

/* ---------------- 配置文件 ----------------
 * 格式：`键 = 值`，`#` 注释，段名 `[section]` 只为可读性会被忽略。
 * 默认读 $XDG_CONFIG_HOME/otilink/kvm.conf（或 ~/.config/otilink/kvm.conf）。
 * 命令行在配置文件之后解析 → **命令行优先**。 */
static const char *default_config_path(void)
{
    static char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(path, sizeof(path), "%s/otilink/kvm.conf", xdg);
    else if (home && *home)
        snprintf(path, sizeof(path), "%s/.config/otilink/kvm.conf", home);
    else
        return NULL;
    return path;
}

static int parse_edges(const char *v)
{
    if (strcasecmp(v, "none") == 0 || strcasecmp(v, "off") == 0)
        return 0;
    if (strcasecmp(v, "all") == 0)
        return (int)OTI_EDGE_ALL;
    int m = 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", v);
    char *save = NULL;
    for (char *t = strtok_r(buf, ", |", &save); t; t = strtok_r(NULL, ", |", &save)) {
        if (strcasecmp(t, "left") == 0)        m |= OTI_EDGE_BIT(OTI_EDGE_LEFT);
        else if (strcasecmp(t, "right") == 0)  m |= OTI_EDGE_BIT(OTI_EDGE_RIGHT);
        else if (strcasecmp(t, "top") == 0)    m |= OTI_EDGE_BIT(OTI_EDGE_TOP);
        else if (strcasecmp(t, "bottom") == 0) m |= OTI_EDGE_BIT(OTI_EDGE_BOTTOM);
    }
    return m;
}

static uint8_t parse_mods(const char *v)
{
    uint8_t m = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", v);
    char *save = NULL;
    for (char *t = strtok_r(buf, "+ ,|", &save); t; t = strtok_r(NULL, "+ ,|", &save)) {
        if (strcasecmp(t, "shift") == 0)      m |= OTI_MOD_SHIFT;
        else if (strcasecmp(t, "ctrl") == 0 || strcasecmp(t, "control") == 0) m |= OTI_MOD_CTRL;
        else if (strcasecmp(t, "alt") == 0)   m |= OTI_MOD_ALT;
        else if (strcasecmp(t, "meta") == 0 || strcasecmp(t, "win") == 0) m |= OTI_MOD_META;
    }
    return m;
}

static void apply_kv(struct app *a, const char *k, const char *v)
{
    if (strcmp(k, "screen") == 0)
        sscanf(v, "%dx%d", &a->cfg.screen_w, &a->cfg.screen_h);
    else if (strcmp(k, "edge") == 0)
        a->cfg.edge = atoi(v);
    else if (strcmp(k, "edges") == 0)
        a->cfg.edges = (unsigned)parse_edges(v);
    else if (strcmp(k, "switch_modifier") == 0 || strcmp(k, "switch_mod") == 0)
        a->cfg.switch_mods = parse_mods(v);
    else if (strcmp(k, "corner") == 0 || strcmp(k, "corner_px") == 0)
        a->cfg.corner_px = atoi(v);
    else if (strcmp(k, "grab") == 0)
        a->cfg.grab = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
    else if (strcmp(k, "return_edge") == 0)
        a->cfg.return_edge = (unsigned)parse_edges(v);
    else if (strcmp(k, "return_on_edge") == 0)
        a->cfg.return_on_edge = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
    else if (strcmp(k, "peer") == 0)
        a->cfg.peer_hid = (strcmp(v, "proto") != 0);
    else if (strcmp(k, "clipboard") == 0)
        a->cfg.clipboard = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
    else if (strcmp(k, "inject") == 0)
        a->cfg.inject = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
    else if (strcmp(k, "idle_release") == 0 || strcmp(k, "idle_release_ms") == 0)
        a->cfg.idle_release_ms = atoi(v);
    /* 保活：之前只有命令行 --keepalive，配置里写不进去 → 装好之后就一直是"关"。
       实测代价：帧管道空闲几小时后设备侧会话卡死，剪贴板单向不通且拔插无效（第 40 轮）。
       这里补上配置键，并把它写进 kvm.conf 模板。 */
    else if (strcmp(k, "keepalive") == 0 || strcmp(k, "keepalive_ms") == 0)
        a->cfg.keepalive_ms = atoi(v);
    /* 撞边时是否给厂商 Windows 端发 XML（默认关：它会挤掉剪贴板帧，见 NOTES §49） */
    else if (strcmp(k, "edge_dwell") == 0 || strcmp(k, "edge_dwell_ms") == 0)
        a->cfg.edge_dwell_ms = atoi(v);
    else if (strcmp(k, "vendor_notify") == 0)
        a->cfg.vendor_notify = (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0);
    else if (strcmp(k, "log") == 0)
        a->cfg.log = strdup(v);
    else if (strcmp(k, "transport") == 0)
        a->cfg.transport = strdup(v);
    else if (strcmp(k, "hotkey_toggle") == 0 || strcmp(k, "toggle") == 0)
        snprintf(a->cfg.hotkey_toggle, sizeof(a->cfg.hotkey_toggle), "%s", v);
    else if (strcmp(k, "hotkey_to_remote") == 0 || strcmp(k, "to_remote") == 0)
        snprintf(a->cfg.hotkey_to_remote, sizeof(a->cfg.hotkey_to_remote), "%s", v);
    else if (strcmp(k, "hotkey_to_local") == 0 || strcmp(k, "to_local") == 0)
        snprintf(a->cfg.hotkey_to_local, sizeof(a->cfg.hotkey_to_local), "%s", v);
    else if (strcmp(k, "hotkey_lock") == 0 || strcmp(k, "lock") == 0)
        snprintf(a->cfg.hotkey_lock, sizeof(a->cfg.hotkey_lock), "%s", v);
    else if (strcmp(k, "capture") == 0) {
        if (a->cfg.ncapture < 8)
            a->cfg.capture[a->cfg.ncapture++] = strdup(v);
    }
}

static void load_config(struct app *a, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == 0 || *p == '[')
            continue;
        char *eq = strchr(p, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = p, *v = eq + 1;
        char *ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t'))
            *--ke = 0;
        while (*v == ' ' || *v == '\t')
            v++;
        /* 去掉行内注释（值里若确有 # 可用引号包起来） */
        if (*v != '"' && *v != '\'') {
            char *hash = strchr(v, '#');
            if (hash) {
                char *he = hash;
                while (he > v && (he[-1] == ' ' || he[-1] == '\t'))
                    he--;
                *he = 0;
            }
        } else {
            char q = *v++;
            char *end = strchr(v, q);
            if (end)
                *end = 0;
        }
        char *ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t'))
            *--ve = 0;
        if (*k && *v) {
            apply_kv(a, k, v);
            n++;
        }
    }
    fclose(f);
    fprintf(stderr, "配置: %s（%d 项）\n", path, n);
}

static void usage(void)
{
    printf("用法: otikm --transport cable[:/dev/sgN]|listen:PATH|connect:PATH [选项]\n"
           "  --capture DEV        evdev 设备（可多次，如 /dev/input/event3）\n"
           "  --grab               驱动侧独占本地键鼠（EVIOCGRAB）\n"
           "  --inject             用 uinput 注入（默认写 sim-inject 或丢弃）\n"
           "  --sim-input FILE     脚本输入（key/mouse/sleep），用于无硬件测试\n"
           "  --sim-inject FILE    把注入的事件写入文件（替代 uinput）\n"
           "  --screen WxH         屏幕尺寸（默认 1920x1080）\n"
           "  --edge PX            触边阈值（默认 4）\n"
           "  --hotkey CODE        拉回指针的 evdev 键码（旧写法，等价于 hotkey_to_local = 单键）\n"
           "  --hotkey-only        只允许热键切换，撞边不切换\n"
           "  --duration SEC       运行时长（默认 0=不限）\n"
           "  --idle-release MS    看门狗：驱动侧时对端超过该时长未消费待发数据，\n"
           "                       就自动交还本机控制（默认 3000；0=关）。防卡死。\n"
           "  --delay MS           sim 模式每步间隔（默认 80）\n"
           "  --log FILE           日志文件（默认 stderr）\n"
           "  --doctor             只做环境自检（uinput/输入设备/剪贴板/线缆），不启动\n"
           "  --selftest-input [synth|grab|live]  输入链路自检：注入→回读→比对\n"
           "                       synth=全自动; grab=同时验证 EVIOCGRAB; live=用真实键鼠(需 --capture)\n"
           "  --selftest-seconds N  live 模式时长（默认 10）\n"
           "  --cable-init         线缆模式启动时做厂商式握手（查询 IC/总线 + 独占锁）\n"
           "  --peer proto|hid     对端接收方式：proto=我们自己的协议（两端都要软件，默认）；\n"
           "                       hid=直接发 HID 包（被控端零软件，需 cable 传输）\n"
           "  --hid-kbd-layout N   键盘 12 字节布局候选 0/1/2（默认 0）\n"
           "  --hid-mouse-layout N 鼠标 12 字节布局候选 0/1/2（默认 0）\n"
           "  --hid-probe          启动时发一组探针（鼠标+100/-100、按一下 A）\n"
           "  --keepalive MS       周期性保活（PING；线缆模式额外发全零 dummy 帧），0=关\n"
           "  --version            打印版本/构建基线/特性后退出（不碰设备）\n"
           "  --auto               绿色包：自动判定传输/角色/设备/屏幕/剪贴板\n"
           "  --role auto|master|slave   显式指定角色（显式优先于 --auto 判定）\n"
           "  --screen auto|WxH    屏幕尺寸；auto = XRandR → DRM → 默认值（告警）\n"
           "  --cursor-source auto|x11|integrate  被驱动侧光标来源（integrate = Wayland 降级）\n"
           "  --return-edge E      被驱动侧交还边：left|right|top|bottom|all\n"
           "  --no-clipboard       关掉剪贴板（即使 --auto/配置打开了）\n"
           "  --no-grab            主控侧不独占本机键鼠（试跑/自检用）\n"
           "  --print-plan         只打印决策（角色/设备/边/屏幕），不建立链路\n"
           "  --edge-dwell MS      被驱动侧交还手势要求贴边停留多少毫秒（缺省 0 = 立即，\n"
           "                       与主控端撞边一致；设 250 可恢复旧行为防误触）\n"
           "  --vendor-notify on|off  撞边时是否给厂商 Windows 端发 XML 通知（缺省 off：\n"
           "                        它会和剪贴板帧抢发送授权，实测把 L→W 剪贴板挤掉）\n"
           "  --verbose            详细日志\n"
           "  --clipboard          启用剪贴板同步（与键鼠共用一条传输）\n"
           "  --clip-file FILE     用文件模拟剪贴板（无图形环境/测试）\n"
           "  --clip-interval MS   轮询间隔（默认 300）\n"
           "  --clip-max BYTES     超过此大小不同步（默认 1MB）\n"
           "\n传输后端:\n"
           "  cable[:/dev/sgN]     真实对拷线（SCSI 私有命令）\n"
           "  listen:PATH / connect:PATH    AF_UNIX（本机集成测试）\n"
           "  tcp-listen:PORT / tcp-connect:HOST:PORT   先借网络验证链路，再切线缆\n");
}

static struct app *g_app_for_signal;

static void on_signal(int sig)
{
    (void)sig;
    if (g_app_for_signal)
        g_app_for_signal->running = 0;   /* 主循环退出后会在收尾处释放独占 */
    /* 立刻把键鼠还给本机（信号处理里只做异步安全的事：ioctl） */
    if (g_app_for_signal)
        for (int i = 0; i < g_app_for_signal->ncap; i++) {
            struct oti_capture *c = &g_app_for_signal->cap[i];
            if (c->fd >= 0 && c->grabbed) {
                ioctl(c->fd, EVIOCGRAB, 0);
                c->grabbed = 0;
            }
        }
}

int main(int argc, char **argv)
{
    struct app a;
    memset(&a, 0, sizeof(a));
    a.cfg.screen_w = 1920;
    a.cfg.screen_h = 1080;
    a.cfg.edge = 4;
    a.cfg.hotkey = 0;      /* 0 = 未显式指定；新写法用配置文件里的 hotkey_* */
    a.cfg.delay_ms = 80;
    a.cfg.duration_s = 0;
    a.cfg.idle_release_ms = 3000;     /* 看门狗：对端 3 秒不消费就把控制权交还本机 */
    a.cfg.role_dwell_ms = 10000;      /* 角色协商：换边后最小驻留（防抖） */
    a.cfg.role_margin_ms = 2000;      /* 双方都有键鼠时的领先阈值 */
    a.cfg.role_absent_ms = 5000;      /* 多久没收到对端 ROLE 算"不在"（I4 兜底） */
    /* 切换策略默认值（对标 PowerToys Mouse Without Borders 的 Easy Mouse） */
    a.cfg.edges = OTI_EDGE_BIT(OTI_EDGE_RIGHT) | OTI_EDGE_BIT(OTI_EDGE_LEFT);
    a.cfg.switch_mods = 0;            /* 直接撞边即切换；想防误触可设 shift/ctrl */
    a.cfg.corner_px = 40;             /* 贴角 40px 不切换，避免误切 */
    snprintf(a.cfg.hotkey_toggle, sizeof(a.cfg.hotkey_toggle), "ctrl+alt+space");
    snprintf(a.cfg.hotkey_to_remote, sizeof(a.cfg.hotkey_to_remote), "ctrl+alt+right");
    snprintf(a.cfg.hotkey_to_local, sizeof(a.cfg.hotkey_to_local), "ctrl+alt+left");
    snprintf(a.cfg.hotkey_lock, sizeof(a.cfg.hotkey_lock), "ctrl+alt+l");
    a.cfg.clip_interval_ms = 300;
    a.cfg.clip_max = 8u << 20;   /* 8MB：图片常常远大于 1MB */
    a.cfg.selftest_seconds = 10;
    a.cfg.hid_mouse_layout = 0;
    a.cfg.hid_kbd_layout = 0;
    /* 键鼠载体：-1 = 自动（线缆传输用厂商 HID 包，其它传输用协议管道）
       厂商 HID 包通道见 NOTES §41：接收端是**真实 USB 鼠标/键盘**，每次事件只有
       16 字节 CDB，不再需要 64KB 帧，也不需要被控端任何软件。 */
    a.cfg.peer_hid = -1;
    /* 被驱动侧的交还边：必须是"朝向控制端"的那条边。
       实测教训：如果左右都当交还边，用户从 Windows 往右移进来后**继续往右推**
       （麒麟光标撞到右边缘）就会被判定成"想回去"，刚过来就被弹回去。 */
    a.cfg.return_edge = OTI_EDGE_BIT(OTI_EDGE_LEFT);
    a.inj.fd = -1;
    for (int i = 0; i < 8; i++)
        a.cap[i].fd = -1;

    /* 先找 --config / --no-config，再解析其余参数（命令行优先于配置文件） */
    {
        const char *cp = default_config_path();
        int nocfg = 0;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--no-config") == 0)
                nocfg = 1;
            else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
                cp = argv[i + 1];
        }
        if (!nocfg && cp)
            load_config(&a, cp);
    }
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (strcmp(s, "--transport") == 0 && i + 1 < argc)
            a.cfg.transport = argv[++i];
        else if (strcmp(s, "--capture") == 0 && i + 1 < argc && a.cfg.ncapture < 8)
            a.cfg.capture[a.cfg.ncapture++] = argv[++i];
        else if (strcmp(s, "--grab") == 0)
            a.cfg.grab = 1;
        else if (strcmp(s, "--inject") == 0)
            a.cfg.inject = 1;
        else if (strcmp(s, "--sim-input") == 0 && i + 1 < argc)
            a.cfg.sim_input = argv[++i];
        else if (strcmp(s, "--sim-inject") == 0 && i + 1 < argc)
            a.cfg.sim_inject = argv[++i];
        else if (strcmp(s, "--screen") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            a.cfg.set_screen = 1;
            if (strcmp(v, "auto") == 0) {
                a.cfg.screen_auto = 1;              /* 让 --auto 之外也能单独要自动探测 */
            } else {
                a.cfg.screen_auto = 0;
                sscanf(v, "%dx%d", &a.cfg.screen_w, &a.cfg.screen_h);
            }
        }
        else if (strcmp(s, "--edge") == 0 && i + 1 < argc)
            a.cfg.edge = atoi(argv[++i]);
        else if (strcmp(s, "--hotkey") == 0 && i + 1 < argc)
            a.cfg.hotkey = atoi(argv[++i]);
        else if (strcmp(s, "--hotkey-only") == 0)
            a.cfg.hotkey_only = 1;
        else if (strcmp(s, "--duration") == 0 && i + 1 < argc)
            a.cfg.duration_s = atoi(argv[++i]);
        else if (strcmp(s, "--idle-release") == 0 && i + 1 < argc)
            a.cfg.idle_release_ms = atoi(argv[++i]);   /* 0 = 关掉看门狗（不推荐） */
        else if (strcmp(s, "--config") == 0 && i + 1 < argc)
            i++;                                        /* 已在预扫描里处理 */
        else if (strcmp(s, "--no-config") == 0)
            ;
        else if (strcmp(s, "--edges") == 0 && i + 1 < argc)
            a.cfg.edges = (unsigned)parse_edges(argv[++i]);
        else if (strcmp(s, "--switch-mod") == 0 && i + 1 < argc)
            a.cfg.switch_mods = parse_mods(argv[++i]);
        else if (strcmp(s, "--corner") == 0 && i + 1 < argc)
            a.cfg.corner_px = atoi(argv[++i]);
        else if (strcmp(s, "--hotkey-toggle") == 0 && i + 1 < argc)
            snprintf(a.cfg.hotkey_toggle, sizeof(a.cfg.hotkey_toggle), "%s", argv[++i]);
        else if (strcmp(s, "--hotkey-to-remote") == 0 && i + 1 < argc)
            snprintf(a.cfg.hotkey_to_remote, sizeof(a.cfg.hotkey_to_remote), "%s", argv[++i]);
        else if (strcmp(s, "--hotkey-to-local") == 0 && i + 1 < argc)
            snprintf(a.cfg.hotkey_to_local, sizeof(a.cfg.hotkey_to_local), "%s", argv[++i]);
        else if (strcmp(s, "--hotkey-lock") == 0 && i + 1 < argc)
            snprintf(a.cfg.hotkey_lock, sizeof(a.cfg.hotkey_lock), "%s", argv[++i]);
        else if (strcmp(s, "--delay") == 0 && i + 1 < argc)
            a.cfg.delay_ms = atoi(argv[++i]);
        else if (strcmp(s, "--log") == 0 && i + 1 < argc)
            a.cfg.log = argv[++i];
        else if (strcmp(s, "--clipboard") == 0)
            a.cfg.clipboard = 1;
        else if (strcmp(s, "--clip-file") == 0 && i + 1 < argc)
            a.cfg.clip_file = argv[++i];
        else if (strcmp(s, "--clip-interval") == 0 && i + 1 < argc)
            a.cfg.clip_interval_ms = atoi(argv[++i]);
        else if (strcmp(s, "--clip-max") == 0 && i + 1 < argc)
            a.cfg.clip_max = (size_t)strtoul(argv[++i], NULL, 0);
        else if (strcmp(s, "--doctor") == 0)
            a.cfg.doctor = 1;
        else if (strcmp(s, "--cable-init") == 0)
            a.cfg.cable_init = 1;
        else if (strcmp(s, "--peer") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "hid") == 0)
                a.cfg.peer_hid = 1;
            else if (strcmp(v, "proto") == 0)
                a.cfg.peer_hid = 0;
            else { fprintf(stderr, "--peer 只支持 proto|hid\n"); return 2; }
        }
        else if (strcmp(s, "--hid-kbd-layout") == 0 && i + 1 < argc)
            a.cfg.hid_kbd_layout = atoi(argv[++i]);
        else if (strcmp(s, "--hid-mouse-layout") == 0 && i + 1 < argc)
            a.cfg.hid_mouse_layout = atoi(argv[++i]);
        else if (strcmp(s, "--return-on-edge") == 0)
            a.cfg.return_on_edge = 1;
        else if (strcmp(s, "--hid-probe") == 0)
            a.cfg.hid_probe = 1;
        else if (strcmp(s, "--keepalive") == 0 && i + 1 < argc) {
            a.cfg.keepalive_ms = atoi(argv[++i]);
            a.cfg.set_keepalive = 1;
        }
        else if (strcmp(s, "--selftest-input") == 0)
            a.cfg.selftest_input = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : "synth";
        else if (strcmp(s, "--selftest-seconds") == 0 && i + 1 < argc)
            a.cfg.selftest_seconds = atoi(argv[++i]);
        else if (strcmp(s, "--verbose") == 0)
            a.cfg.verbose = 1;
        /* ---- 绿色包/免安装（opt-in；不传 = 与以前逐字节同行为）---------------- */
        else if (strcmp(s, "--version") == 0)
            a.cfg.version = 1;
        else if (strcmp(s, "--auto") == 0)
            a.cfg.auto_mode = 1;
        else if (strcmp(s, "--role") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            a.cfg.set_role = 1;
            if (strcmp(v, "auto") == 0)         a.cfg.role = 0;
            else if (strcmp(v, "master") == 0)  a.cfg.role = 1;
            else if (strcmp(v, "slave") == 0)   a.cfg.role = 2;
            else { fprintf(stderr, "--role 只支持 auto|master|slave\n"); return 2; }
            a.cfg.role_explicit = (a.cfg.role != 0);   /* auto 不是"显式强制" */
        }
        else if (strcmp(s, "--cursor-source") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "auto") == 0)           a.cfg.cursor_source = 0;
            else if (strcmp(v, "x11") == 0)       a.cfg.cursor_source = 1;
            else if (strcmp(v, "integrate") == 0) a.cfg.cursor_source = 2;
            else { fprintf(stderr, "--cursor-source 只支持 auto|x11|integrate\n"); return 2; }
        }
        else if (strcmp(s, "--return-edge") == 0 && i + 1 < argc)
            a.cfg.return_edge = (unsigned)parse_edges(argv[++i]);
        else if (strcmp(s, "--no-clipboard") == 0)
            a.cfg.no_clip = 1;
        else if (strcmp(s, "--no-grab") == 0)
            a.cfg.no_grab = 1;
        else if (strcmp(s, "--print-plan") == 0)
            a.cfg.print_plan = 1;
        else if (strcmp(s, "--edge-dwell") == 0 && i + 1 < argc)
            a.cfg.edge_dwell_ms = atoi(argv[++i]);
        else if (strcmp(s, "--role-dwell") == 0 && i + 1 < argc)
            a.cfg.role_dwell_ms = atol(argv[++i]);
        else if (strcmp(s, "--role-margin") == 0 && i + 1 < argc)
            a.cfg.role_margin_ms = atol(argv[++i]);
        else if (strcmp(s, "--role-absent") == 0 && i + 1 < argc)
            a.cfg.role_absent_ms = atol(argv[++i]);
        else if (strcmp(s, "--vendor-notify") == 0 && i + 1 < argc) {
            const char *v = argv[++i];
            if (strcmp(v, "on") == 0)       a.cfg.vendor_notify = 1;
            else if (strcmp(v, "off") == 0) a.cfg.vendor_notify = 0;
            else { fprintf(stderr, "--vendor-notify 只支持 on|off\n"); return 2; }
        }
        else {
            usage();
            return 2;
        }
    }
    if (a.cfg.no_grab)
        a.cfg.grab = 0;
    if (a.cfg.version) {
        print_version();
        return 0;
    }
    if (a.cfg.selftest_input)
        return selftest_input(&a.cfg, a.cfg.selftest_input) ? 1 : 0;
    if (a.cfg.doctor)
        return doctor(a.cfg.transport) ? 1 : 0;
    if (!a.cfg.transport && !a.cfg.auto_mode && !a.cfg.print_plan) {
        usage();
        return 2;
    }

    a.logf = a.cfg.log ? fopen(a.cfg.log, "w") : NULL;
    if (a.cfg.sim_inject)
        a.sim_out = fopen(a.cfg.sim_inject, "w");
    pthread_mutex_init(&a.lock, NULL);
    pthread_mutex_init(&a.loglock, NULL);

    /* 绿色包：先做角色/设备/屏幕判定（say() 需要 logf 已就绪），
       再做剪贴板/core/抓取设备初始化 —— 顺序不能反，否则 --auto 打开的剪贴板不生效。 */
    if (a.cfg.auto_mode || a.cfg.set_role || a.cfg.screen_auto || a.cfg.cursor_source ||
        a.cfg.print_plan)
        resolve_auto(&a);
    if (a.cfg.print_plan) {
        print_plan(&a);
        return 0;
    }
    if (!a.cfg.transport) {
        usage();
        return 2;
    }
    if (a.cfg.clipboard) {
        if (oti_clip_open(&a.clip, a.cfg.clip_file, a.cfg.clip_max) != 0) {
            fprintf(stderr, "剪贴板后端初始化失败\n");
            return 1;
        }
        say(&a, "剪贴板后端: %s（间隔 %dms，上限 %zu 字节）",
            oti_clip_backend_name(&a.clip), a.cfg.clip_interval_ms, a.cfg.clip_max);
        /* 大文件流式传输（见 otixfer.h）：小文件仍走"文件包"，超过上限的走分片。
           注意：这个 ops 必须**静态**（程序级生命周期）—— 两个 xfer 实例会长期持有它的
           指针，收包线程之后还会用；放在栈上离开作用域就是悬垂指针
           （实测：ASan 报 stack-use-after-scope，真机表现是 otikm 段错误退出）。 */
        static struct oti_xfer_ops ops;
        ops.user = &a;
        ops.send_clip = xfer_send_clip;
        ops.apply_file = xfer_apply_file;
        ops.log = xfer_log;
        oti_xfer_tx_init(&a.xfer_tx, &ops);
        oti_xfer_rx_init(&a.xfer_rx, &ops);
        say(&a, "大文件传输: 已启用（单片 %d 字节，>8MB 的文件自动走分片流式）",
            OTI_XFER_PART_MAX);
    }
    otikm_core_init(&a.core, a.cfg.screen_w, a.cfg.screen_h, a.cfg.edge);
    otikm_core_set_edges(&a.core, a.cfg.edges);
    otikm_core_set_switch_mods(&a.core, a.cfg.switch_mods);
    otikm_core_set_corner_px(&a.core, a.cfg.corner_px);
    {
        struct { const char *spec; otikm_hotkey *hk; const char *name; } hs[4] = {
            { a.cfg.hotkey_toggle,    &a.core.hk_toggle,    "切换控制权" },
            { a.cfg.hotkey_to_remote, &a.core.hk_to_remote, "交给对端" },
            { a.cfg.hotkey_to_local,  &a.core.hk_to_local,  "拉回本机" },
            { a.cfg.hotkey_lock,      &a.core.hk_lock,      "锁定本机" },
        };
        char nb[64];
        for (int i = 0; i < 4; i++) {
            if (otikm_hotkey_parse(hs[i].spec, hs[i].hk) != 0) {
                say(&a, "ERR 热键 '%s'（%s）无法解析，已禁用", hs[i].spec, hs[i].name);
                hs[i].hk->key = 0;
            } else if (hs[i].hk->key) {
                say(&a, "热键 %-10s = %s", hs[i].name, otikm_hotkey_name(hs[i].hk, nb, sizeof(nb)));
            }
        }
    }
    if (a.cfg.hotkey) {                  /* 兼容旧参数 --hotkey CODE（裸键、无修饰；显式传才生效） */
        a.core.hk_to_local.mods = 0;
        a.core.hk_to_local.key = (uint16_t)a.cfg.hotkey;
        say(&a, "热键 拉回本机 = code%u（来自 --hotkey）", a.cfg.hotkey);
    }
    if (a.cfg.hotkey_only)
        otikm_core_set_edges(&a.core, 0);
    say(&a, "撞边切换: %s%s%s", a.cfg.edges == 0 ? "关闭（只用热键）" :
        (a.cfg.edges == OTI_EDGE_ALL ? "四边" : "部分边"),
        a.cfg.switch_mods ? "，需按住修饰键" : "", a.cfg.corner_px ? "，角部防误切" : "");

    for (int i = 0; i < a.cfg.ncapture; i++) {
        int rc = oti_capture_open(&a.cap[i], a.cfg.capture[i], 0);
        if (rc) {
            say(&a, "ERR 打开 %s 失败: %s", a.cfg.capture[i], strerror(-rc));
            return 1;
        }
        a.ncap++;
        say(&a, "抓取设备 %s (%s)", a.cfg.capture[i], a.cap[i].devname);
    }
    if (a.cfg.inject && !a.sim_out) {
        int rc = oti_inject_open(&a.inj, OTI_INJECT_NAME);
        if (rc) {
            say(&a, "ERR 打不开 /dev/uinput: %s（需要 udev 规则或 root）", strerror(-rc));
            return 1;
        }
        a.use_uinput = 1;
        say(&a, "uinput 注入设备已创建");
    }

    if (a.cfg.peer_hid < 0) {
        /* 自动：只有线缆传输才可能用 HID 包通道（TCP/AF_UNIX 没有线缆固件） */
        a.cfg.peer_hid = (a.cfg.transport && strncmp(a.cfg.transport, "cable", 5) == 0);
        say(&a, "键鼠载体：%s（--peer 可强制）",
            a.cfg.peer_hid ? "厂商 HID 包通道" : "协议管道");
    }
    if (a.cfg.return_on_edge) {
        /* 被驱动侧撞边依赖真实光标坐标：启动时把可用性打出来，
           否则出问题只能看到"推到边缘没反应"，无从判断。 */
        int px = -1, py = -1;
        int ok = otix11_pos(&px, &py);
        say(&a, "被驱动侧模式：真实光标读取 %s（当前 %d,%d）%s",
            otix11_status(), px, py,
            ok == 0 ? "" : "  <-- 读不到！撞边交还会失效，请检查 DISPLAY");
    }
    a.tx = open_transport(a.cfg.transport);
    if (!a.tx) {
        say(&a, "ERR 建立传输失败: %s", a.cfg.transport);
        return 1;
    }
    say(&a, "传输就绪: %s", oti_tr_name(a.tx));
    maybe_send_hello(&a);          /* 先把自己的屏幕几何告诉对端 */
    g_app_for_signal = &a;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    /* 线缆模式的初始化握手（对应厂商 Initialize(): 卸载媒体 → 独占 → 查询 → 传输 → 释放）。
       媒体卸载这一条在 Linux 上靠 udev 规则抑制自动挂载 + doctor 检查来保证。 */
    if (a.cfg.cable_init && strncmp(a.cfg.transport, "cable", 5) == 0) {
        struct otilink_dev *cd = oti_tr_cable_dev(a.tx);
        static uint8_t info[64];
        uint8_t bus[16] = {0}, mode = 0;
        int r1 = otilink_info_read(cd, info, sizeof(info));
        say(&a, "cable-init: 设备信息块 rc=%d（%02x %02x %02x %02x / side=%u func=%u）", r1,
            info[0], info[1], info[2], info[3], info[12], info[14]);
        int r0 = otilink_dev_mode_get(cd, &mode);
        say(&a, "cable-init: 设备模式 rc=%d mode=%u", r0, mode);
        int r2 = otilink_bus_type(cd, bus);
        say(&a, "cable-init: 物理总线类型 rc=%d（%02x %02x %02x %02x ...）", r2,
            bus[0], bus[1], bus[2], bus[3]);
        int r3 = otilink_lock(cd, 1);
        say(&a, "cable-init: 独占锁 rc=%d", r3);
    }
    a.running = 1;

    pthread_t th, cth, kth;
    int clip_started = 0, ka_started = 0;
    pthread_create(&th, NULL, rx_thread, &a);
    if (a.cfg.clipboard) {
        pthread_create(&cth, NULL, clip_thread, &a);
        clip_started = 1;
    }
    if (a.cfg.peer_hid) {
        struct otilink_dev *cd = oti_tr_cable_dev(a.tx);
        if (!cd) {
            say(&a, "ERR --peer hid 需要 cable 传输；请用 --transport cable[:/dev/sgN]");
            return 1;
        }
        say(&a, "HID 直发模式：键盘 layout=%d，鼠标 layout=%d（被控端无需任何软件）",
            a.cfg.hid_kbd_layout, a.cfg.hid_mouse_layout);
        if (a.cfg.clipboard)
            say(&a, "注意：剪贴板仍走**协议管道**（不是 HID），所以对端要么跑我们的 agent，"
                    "要么改用共享卷方案（windows/volumeclip.ps1 + volumeclip-linux.sh）");
        if (a.cfg.hid_probe) {
            uint8_t pkt[OTI_HID_PKT_LEN];
            say(&a, "hid-probe: 先发鼠标 +100/-100 再按一下 'A'，请在被控端观察");
            otihid_mouse_report(0, 100, 0, 0, a.cfg.hid_mouse_layout, pkt);
            otilink_send_hid(cd, OTI_HID_TYPE_MOUSE, pkt);
            usleep(400 * 1000);
            otihid_mouse_report(0, -100, 0, 0, a.cfg.hid_mouse_layout, pkt);
            otilink_send_hid(cd, OTI_HID_TYPE_MOUSE, pkt);
            usleep(400 * 1000);
            struct otihid_kbd kb;
            otihid_kbd_reset(&kb);
            otihid_kbd_apply(&kb, 30 /*KEY_A*/, 1);
            otihid_kbd_report(&kb, a.cfg.hid_kbd_layout, pkt);
            otilink_send_hid(cd, OTI_HID_TYPE_KBD, pkt);
            usleep(150 * 1000);
            otihid_kbd_reset(&kb);
            otihid_kbd_report(&kb, a.cfg.hid_kbd_layout, pkt);
            otilink_send_hid(cd, OTI_HID_TYPE_KBD, pkt);
            say(&a, "hid-probe 结束");
        }
    }
    if (a.cfg.keepalive_ms > 0) {
        pthread_create(&kth, NULL, keepalive_thread, &a);
        ka_started = 1;
        say(&a, "保活已启用：每 %dms 一次（%s）", a.cfg.keepalive_ms,
            oti_tr_cable_dev(a.tx) ? "PING；dummy 帧只在链路空闲 >60s 时发（它会独占设备 ~1.3s）"
                                   : "PING");
    }

    if (a.cfg.sim_input)
        run_sim(&a);
    else if (a.ncap)
        run_capture(&a);
    else {
        /* 没有输入源：只当中继（仅注入收到的远端事件）。
           但不能死睡 —— 角色仲裁可能因为**热插拔**（用户刚把键鼠插到本机）或对端让位
           而要求切换设备集合；切换后 ncap 会变成 >0，必须立刻转入真正的抓取循环。 */
        long deadline = a.cfg.duration_s > 0 ? now_ms() + a.cfg.duration_s * 1000L : -1;
        while (a.running && a.ncap == 0 && (deadline < 0 || now_ms() < deadline)) {
            role_apply_pending(&a);
            if (a.ncap)
                break;
            usleep(100 * 1000);
        }
        if (a.running && a.ncap)
            run_capture(&a);          /* 热插拔之后：设备集合已重建，转入抓取 */
    }

    a.running = 0;
    pthread_join(th, NULL);
    if (clip_started)
        pthread_join(cth, NULL);
    if (ka_started)
        pthread_join(kth, NULL);
    say(&a, "结束: 发出 %lu 条, 注入 %lu 条, 切换 %lu 次, 保活 %lu 次, 收到 PING %lu 次, HID 包 %lu 个",
        a.n_sent, a.n_injected, a.n_switches, a.n_keepalive, a.n_ping_recv, a.n_hid);

    if (a.cfg.peer_hid) {                      /* 退出前释放对端可能按住的键 */
        if (oti_tr_cable_dev(a.tx))
            otilink_hid_send_token(oti_tr_cable_dev(a.tx), OTI_HID_TOKEN_PASSIVE);
        struct otilink_dev *cd = oti_tr_cable_dev(a.tx);
        if (cd)
            otilink_hid_release_all(cd);
    }
    if (a.cfg.cable_init && strncmp(a.cfg.transport, "cable", 5) == 0) {
        int r = otilink_lock(oti_tr_cable_dev(a.tx), 0);
        say(&a, "cable-init: 释放独占锁 rc=%d", r);
    }
    if (a.use_uinput)
        oti_inject_close(&a.inj);
    for (int i = 0; i < a.ncap; i++)
        oti_capture_close(&a.cap[i]);
    oti_tr_close(a.tx);
    if (a.cfg.clipboard) {
        oti_clip_close(&a.clip);
        free(a.ack_buf);
        a.ack_buf = NULL;
    }
    if (a.sim_out)
        fclose(a.sim_out);
    if (a.logf)
        fclose(a.logf);
    return 0;
}
