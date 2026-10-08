/* otix11.h —— 读真实光标位置 + 屏幕尺寸（见 otix11.c 注释）。返回 0 成功。 */
#ifndef OTIX11_H
#define OTIX11_H
int otix11_pos(int *x, int *y);
/* 把真实光标挪到 (x,y)（XWarpPointer）。回程落点用：grab 期间真实光标不动、
   回程时还贴在"出去时那条边"上，必须主动把它放到"边往里一点"，
   否则 core 坐标与真实光标打架，用户轻轻一碰就又推出去。返回 0 成功。 */
int otix11_warp(int x, int y);
const char *otix11_status(void);
/* 屏幕（虚拟桌面）尺寸：优先 XRandR 求所有 CRTC 并集，退 XGetGeometry。
   为什么要它：撞边判据与绝对坐标都按屏幕尺寸算，而 --screen 默认写死 1920x1080
   —— 换一台机器（2560x1440 / 1366x768 / 笔记本内屏）判据就偏了。
   返回 0 成功（w/h 出参已填），-1 失败（没有可用 X 显示）。 */
int otix11_screen(int *w, int *h);
#endif
