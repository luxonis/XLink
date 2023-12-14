#ifndef _XLINK_TIME_H
#define _XLINK_TIME_H

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct XLinkTimespec {
    uint64_t tv_sec;
    uint64_t tv_nsec;
} XLinkTimespec;

void getMonotonicTimestamp(XLinkTimespec* ts);

#ifdef __cplusplus
}
#endif
#endif