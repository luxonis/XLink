// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#ifdef __GNUC__
#include <sys/time.h>
#include <unistd.h>
#else
#include "win_time.h"

int clock_gettime(int dummy, struct timespec *spec)
{
    dummy;	// for unreferenced formal parameter warning
    __int64 wintime; GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime -= 116444736000000000LL;  //1jan1601 to 1jan1970
    spec->tv_sec = wintime / 10000000LL;           //seconds
    spec->tv_nsec = wintime % 10000000LL * 100;      //nano-seconds
    return 0;
}

#endif // __GNUC__

