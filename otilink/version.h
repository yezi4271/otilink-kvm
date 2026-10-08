/*
 * version.h —— 版本与构建基线指纹（绿色包/可移植构建用）
 *
 * 为什么需要：这个项目里"跑的是哪一版、跑得起来吗"坑过很多次：
 *   - csc 覆盖运行中的 exe 会静默失败（见 AGENTS L5，Windows 侧用 BuildTag 认版本）；
 *   - WSL 编出来的二进制要求 GLIBC_2.38，拿到麒麟（2.31）根本起不来
 *     —— 所以"构建基线"必须跟着二进制走，不能靠人记。
 *
 * OTI_BUILD_TAG 由 Makefile 注入，形如：
 *   baseline=glibc2.31 arch=x86_64 src=ab12cd34ef56 date=2025-09-17
 * 门禁 re/tools/portablecheck.sh 会核对：
 *   1) src= 指纹 == 当前源码 sha256 前 12 位（防"发的是旧二进制"）；
 *   2) readelf 出来的最大 GLIBC_x.y 需求 <= baseline（防"在太新的机器上编"）。
 */
#ifndef OTILINK_VERSION_H
#define OTILINK_VERSION_H

#ifndef OTI_VERSION
#define OTI_VERSION "0.0.0-dev"
#endif
#ifndef OTI_BUILD_TAG
#define OTI_BUILD_TAG "baseline=unknown arch=unknown src=unknown date=unknown"
#endif

#endif /* OTILINK_VERSION_H */
