#ifndef _XLINK_TIME_H
#define _XLINK_TIME_H

#ifdef __cplusplus
extern "C"
{
#endif

void getMonotonicTimestamp(struct timespec* ts);

#ifdef __cplusplus
}
#endif
#endif