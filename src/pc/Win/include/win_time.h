// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#ifdef __GNUC__
#include <sys/time.h>
#include <unistd.h>
#else
#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WIN_TIME_H
#define _WIN_TIME_H

#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif /* __STDC_VERSION__ */

#include "time.h"
#include "windows.h"
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME      0
#endif

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC     0
#endif

#define sleep(x)            Sleep((DWORD)x)
#define usleep(x)           Sleep((DWORD)(x/1000))


int clock_gettime(int, struct timespec *);

#endif // _WIN_TIME_H

#ifdef __cplusplus
}
#endif

#endif // __GNUC__

