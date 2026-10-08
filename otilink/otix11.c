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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _XDisplay Display;
typedef unsigned long Window;
typedef int Bool;

typedef Display *(*fn_XOpenDisplay)(const char *);
typedef Window (*fn_XDefaultRootWindow)(Display *);
typedef Bool (*fn_XQueryPointer)(Display *, Window, Window *, Window *,
                                 int *, int *, int *, int *, unsigned int *);
typedef int (*fn_XWarpPointer)(Display *, Window, Window, int, int, unsigned int,
                               unsigned int, int, int);
typedef int (*fn_XCloseDisplay)(Display *);

static void *h_lib;
static Display *dpy;
static Window root;
static fn_XQueryPointer p_XQueryPointer;
static int inited, failed;

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
    if (!inited)
        otix11_init();
    if (failed || !dpy)
        return -1;
    Window r_ret = 0, c_ret = 0;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!p_XQueryPointer(dpy, root, &r_ret, &c_ret, &rx, &ry, &wx, &wy, &mask))
        return -1;                       /* 指针不在本屏 */
    *x = rx;
    *y = ry;
    return 0;
}

int otix11_warp(int x, int y)
{
    if (!inited)
        otix11_init();
    if (failed || !dpy)
        return -1;
    static fn_XWarpPointer p_warp;
    if (!p_warp)
        p_warp = (fn_XWarpPointer)dlsym(h_lib, "XWarpPointer");
    if (!p_warp)
        return -1;
    p_warp(dpy, 0 /*None*/, root, 0, 0, 0, 0, x, y);
    return 0;
}

const char *otix11_status(void)
{
    if (!inited)
        otix11_init();
    if (failed)
        return "不可用（libX11 打不开或 $DISPLAY 无效）";
    return "可用（XQueryPointer）";
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

int otix11_screen(int *w, int *h)
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
