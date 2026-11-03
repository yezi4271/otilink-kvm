/*
 * otix11.c —— 读**真实**光标位置（X11），不依赖 X11 开发包。
 *
 * 为什么需要：被驱动侧的"撞边交还"以前用**累加位移**推算指针位置，但那会漂移
 *   —— 对端驱动指针时我们并不知道它的起始位置，累加值和真实位置会越差越远，
 *   实测表现就是"明明把指针推到屏幕边缘了，却没有任何反应，回不去"。
 * 用 XQueryPointer 直接读真实坐标就没有这个问题。
 *
 * 做法：dlopen("libX11.so.6")，只用到的三个函数用函数指针调用，
 *       因此**不需要装 libx11-dev**，也不需要链接期依赖。
 * 用法：otix11_pos(&x,&y) 返回 0 表示拿到坐标；失败返回 -1（调用方应退回旧逻辑）。
 */
#define _GNU_SOURCE
#include "otix11.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef int Bool;

typedef Display *(*fn_XOpenDisplay)(const char *);
typedef Window (*fn_XDefaultRootWindow)(Display *);
typedef Bool (*fn_XQueryPointer)(Display *, Window, Window *, Window *,
                                 int *, int *, int *, int *, unsigned int *);
typedef int (*fn_XWarpPointer)(Display *, Window, Window, int, int, unsigned int,
                               unsigned int, int, int);
typedef int (*fn_XSync)(Display *, int);
typedef int (*fn_XCloseDisplay)(Display *);

static void *h_lib;
static Display *dpy;
static Window root;
static fn_XQueryPointer p_XQueryPointer;
static int inited, failed;

/* 2026-10-27：本文件的 X11 调用现在来自**多个线程**（捕获线程读光标、
   消息泵线程查锁屏状态）。同一个 Display 连接不是线程安全的，
   不加锁会出现"Xlib: unexpected async reply"甚至崩。锁顺序固定 a->lock → xl，
   本文件从不反向去拿 a->lock，故不会死锁。 */
static pthread_mutex_t xl = PTHREAD_MUTEX_INITIALIZER;

static void otix11_init(void)
{
    if (inited)
        return;
    inited = 1;
    h_lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!h_lib) {
        failed = 1;
        return;
    }
    fn_XOpenDisplay p_open = (fn_XOpenDisplay)dlsym(h_lib, "XOpenDisplay");
    fn_XDefaultRootWindow p_root = (fn_XDefaultRootWindow)dlsym(h_lib, "XDefaultRootWindow");
    p_XQueryPointer = (fn_XQueryPointer)dlsym(h_lib, "XQueryPointer");
    if (!p_open || !p_root || !p_XQueryPointer) {
        failed = 1;
        return;
    }
    dpy = p_open(NULL);                 /* 用 $DISPLAY */
    if (!dpy) {
        failed = 1;
        return;
    }
    root = p_root(dpy);
}

int otix11_pos(int *x, int *y)
{
    int rc = 0;
    pthread_mutex_lock(&xl);
    if (!inited)
        otix11_init();
    if (failed || !dpy) {
        rc = -1;
    } else {
        Window r_ret = 0, c_ret = 0;
        int rx = 0, ry = 0, wx = 0, wy = 0;
        unsigned int mask = 0;
        if (!p_XQueryPointer(dpy, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask)) {
            rc = -1;                     /* 指针不在本屏 */
        } else {
            *x = rx;
            *y = ry;
        }
    }
    pthread_mutex_unlock(&xl);
    return rc;
}

int otix11_warp(int x, int y)
{
    int rc = -1;
    pthread_mutex_lock(&xl);
    if (!inited)
        otix11_init();
    if (!failed && dpy) {
        static fn_XWarpPointer p_warp;
        if (!p_warp)
            p_warp = (fn_XWarpPointer)dlsym(h_lib, "XWarpPointer");
        if (p_warp) {
            p_warp(dpy, 0 /*None*/, root, 0, 0, 0, 0, x, y);
            /* XSync 是必须的（2026-10-27 kmbret 实测）：XWarpPointer 只是**请求**，
               不同步的话紧接着的 XQueryPointer 还能读到 warp **之前**的位置 ——
               回程落点刚 warp 到 96px，下一帧"光标校准"却按旧位置 0 算，
               core 被写成 0 → 一丁点右移都算撞边 → 回程后 60ms 又交出去。
               同步一次（本地往返，开销可忽略）保证读到的就是新位置。 */
            fn_XSync p_sync = (fn_XSync)dlsym(h_lib, "XSync");
            if (p_sync)
                p_sync(dpy, 0 /*False*/);
            rc = 0;
        }
    }
    pthread_mutex_unlock(&xl);
    return rc;
}

const char *otix11_status(void)
{
    const char *s;
    pthread_mutex_lock(&xl);
    if (!inited)
        otix11_init();
    s = failed ? "不可用（libX11 打不开或 $DISPLAY 无效）" : "可用（XQueryPointer）";
    pthread_mutex_unlock(&xl);
    return s;
}

/* ---------- 屏幕尺寸（XRandR，同样不依赖开发包）----------
 * XRandR 的公开结构体布局（Xrandr.h）在这里手工复刻：只用到 x/y/width/height 与
 * crtcs 数组，避免为了三个字段去依赖 libxrandr-dev。字段顺序/对齐必须与头文件一致。 */
typedef struct {
    unsigned long timestamp;
    int x, y;
    unsigned int width, height;
    unsigned long mode;
    unsigned short rotation;
    int noutput;
    unsigned long *outputs;
    unsigned short rotations;
    int npossible;
    unsigned long *possible;
} xrr_crtc_info;

typedef struct {
    unsigned long timestamp;
    unsigned long configTimestamp;
    int ncrtc;
    unsigned long *crtcs;
    int noutput;
    unsigned long *outputs;
    int nmode;
    void *modes;
} xrr_screen_resources;

typedef struct {
    unsigned long timestamp;
    int x, y;
    unsigned int width, height;
    unsigned int border_width, depth;
} x_window_geometry;

typedef xrr_screen_resources *(*fn_XRRGetScreenResourcesCurrent)(Display *, Window);
typedef xrr_crtc_info *(*fn_XRRGetCrtcInfo)(Display *, xrr_screen_resources *, unsigned long);
typedef void (*fn_XRRFreeCrtcInfo)(xrr_crtc_info *);
typedef void (*fn_XRRFreeScreenResources)(xrr_screen_resources *);
typedef int (*fn_XGetGeometry)(Display *, Window, Window *, int *, int *, unsigned int *,
                               unsigned int *, unsigned int *, unsigned int *);

static int screen_nolock(int *w, int *h)
{
    if (!inited)
        otix11_init();
    if (failed || !dpy)
        return -1;

    /* 1) XRandR：所有 CRTC 的并集（多显示器也正确） */
    void *hr = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
    if (hr) {
        fn_XRRGetScreenResourcesCurrent p_res =
            (fn_XRRGetScreenResourcesCurrent)dlsym(hr, "XRRGetScreenResourcesCurrent");
        fn_XRRGetCrtcInfo p_crtc = (fn_XRRGetCrtcInfo)dlsym(hr, "XRRGetCrtcInfo");
        fn_XRRFreeCrtcInfo p_fc = (fn_XRRFreeCrtcInfo)dlsym(hr, "XRRFreeCrtcInfo");
        fn_XRRFreeScreenResources p_fr =
            (fn_XRRFreeScreenResources)dlsym(hr, "XRRFreeScreenResources");
        if (p_res && p_crtc && p_fc && p_fr) {
            xrr_screen_resources *res = p_res(dpy, root);
            if (res) {
                int minx = 0, miny = 0, maxx = 0, maxy = 0, any = 0;
                for (int i = 0; i < res->ncrtc; i++) {
                    xrr_crtc_info *ci = p_crtc(dpy, res, res->crtcs[i]);
                    if (!ci)
                        continue;
                    if (ci->width > 0 && ci->height > 0) {
                        int x1 = ci->x + (int)ci->width, y1 = ci->y + (int)ci->height;
                        if (!any) {
                            minx = ci->x; miny = ci->y; maxx = x1; maxy = y1; any = 1;
                        } else {
                            if (ci->x < minx) minx = ci->x;
                            if (ci->y < miny) miny = ci->y;
                            if (x1 > maxx)   maxx = x1;
                            if (y1 > maxy)   maxy = y1;
                        }
                    }
                    p_fc(ci);
                }
                p_fr(res);
                if (any && maxx > minx && maxy > miny) {
                    *w = maxx - minx;
                    *h = maxy - miny;
                    return 0;
                }
            }
        }
    }

    /* 2) 退路：XGetGeometry（XRandR 缺失/异常时至少拿到主屏尺寸） */
    fn_XGetGeometry p_geo = (fn_XGetGeometry)dlsym(h_lib, "XGetGeometry");
    if (p_geo) {
        Window r_ret = 0;
        int x = 0, y = 0;
        unsigned int bw = 0, d = 0, hh = 0, ww = 0;
        if (p_geo(dpy, root, &r_ret, &x, &y, &ww, &hh, &bw, &d) && ww > 0 && hh > 0) {
            *w = (int)ww;
            *h = (int)hh;
            return 0;
        }
    }
    return -1;
}

int otix11_screen(int *w, int *h)
{
    int rc;
    pthread_mutex_lock(&xl);
    rc = screen_nolock(w, h);
    pthread_mutex_unlock(&xl);
    return rc;
}

/* ---------- 锁屏/屏保检测（2026-10-27）----------
 * 真机事故：18:02 屏幕自动锁定，otikm 当时正把控制权交给 Windows（键盘 EVIOCGRAB 独占），
 * 用户在锁屏密码框里怎么打都没反应（按键被转成 HID 发去 Windows 了），最后只能靠 SSH
 * 解锁。L8 要求"交出去的能力必须收得回来"，锁屏就是最硬的一种收回：**锁屏期间不独占**。
 *
 * 判据：输入焦点窗口 → 向上找到顶层 → 看 WM_CLASS。
 *   只认类名（screensaver / lock-screen），不认标题：普通窗口叫 "lock.txt" 不能误判。
 *   取焦点而不是遍历所有顶层窗口：一次 XGetInputFocus + 一次 XGetClassHint，
 *   每 500ms 一次也不心疼；Kylin 的锁屏窗是 override-redirect 全屏，一定拿焦点。
 * 返回：1=锁屏/屏保在前台，0=没有，-1=判断不了（无 X / 缺符号 → 调用方保持原行为）。 */
typedef struct {
    char *res_name;
    char *res_class;
} x_class_hint;

typedef int (*fn_XGetInputFocus)(Display *, Window *, int *);
typedef int (*fn_XGetClassHint)(Display *, Window, x_class_hint *);
typedef int (*fn_XQueryTree)(Display *, Window, Window *, Window *, Window **, unsigned int *);
typedef int (*fn_XFree)(void *);

static int name_looks_like_lock(const char *s)
{
    if (!s)
        return 0;
    return strcasestr(s, "screensaver") || strcasestr(s, "lockscreen") ||
           strcasestr(s, "lock-screen") || strcasestr(s, "screen-lock") ||
           strcasestr(s, "screenlock");
}

int otix11_lock_screen_active(void)
{
    int rc = -1;
    pthread_mutex_lock(&xl);
    if (!inited)
        otix11_init();
    if (!failed && dpy) {
        fn_XGetInputFocus p_focus = (fn_XGetInputFocus)dlsym(h_lib, "XGetInputFocus");
        fn_XGetClassHint p_class = (fn_XGetClassHint)dlsym(h_lib, "XGetClassHint");
        fn_XQueryTree p_tree = (fn_XQueryTree)dlsym(h_lib, "XQueryTree");
        fn_XFree p_free = (fn_XFree)dlsym(h_lib, "XFree");
        if (p_focus && p_class && p_free) {
            rc = 0;
            Window w = 0;
            int revert = 0;
            p_focus(dpy, &w, &revert);
            /* None(0) / PointerRoot(1) 都不是锁屏窗 */
            if (w > 1 && p_tree) {
                /* 焦点常在锁屏对话框的子控件上，WM_CLASS 只在顶层 → 向上爬 */
                for (int depth = 0; depth < 8 && w > 1; depth++) {
                    Window r_ret = 0, parent = 0;
                    Window *kids = NULL;
                    unsigned int nkids = 0;
                    if (!p_tree(dpy, w, &r_ret, &parent, &kids, &nkids))
                        break;
                    if (kids)
                        p_free(kids);
                    if (parent == 0 || parent == root)
                        break;          /* w 已是顶层 */
                    w = parent;
                }
            }
            if (w > 1) {
                x_class_hint ch;
                memset(&ch, 0, sizeof(ch));
                if (p_class(dpy, w, &ch)) {
                    if (name_looks_like_lock(ch.res_name) ||
                        name_looks_like_lock(ch.res_class))
                        rc = 1;
                    if (ch.res_name)
                        p_free(ch.res_name);
                    if (ch.res_class)
                        p_free(ch.res_class);
                }
            }
        }
    }
    pthread_mutex_unlock(&xl);
    return rc;
}
