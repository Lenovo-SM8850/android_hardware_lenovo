// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifdef __ANDROID__
#define LOG_TAG "LenovoMcfgLoader"
#include <log/log.h>
#define MCFG_INFO(...) ALOGI(__VA_ARGS__)
#define MCFG_ERROR(...) ALOGE(__VA_ARGS__)
#else
#include <cstdio>
#define MCFG_INFO(...) do { std::fprintf(stderr, "LenovoMcfgLoader I: "); \
    std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define MCFG_ERROR(...) do { std::fprintf(stderr, "LenovoMcfgLoader E: "); \
    std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#endif
