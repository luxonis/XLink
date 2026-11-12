#ifndef XLINK_TCPIP_RETAINED_SEND_SUPPORT_H
#define XLINK_TCPIP_RETAINED_SEND_SUPPORT_H

#ifdef __linux__
#include <sys/socket.h>
#include <linux/errqueue.h>
#endif

#if defined(__linux__) && defined(SO_ZEROCOPY) && defined(MSG_ZEROCOPY) && \
    defined(SO_EE_ORIGIN_ZEROCOPY) && defined(SO_EE_CODE_ZEROCOPY_COPIED)
#define XLINK_LINUX_TCP_RETAINED_SEND 1
#else
#define XLINK_LINUX_TCP_RETAINED_SEND 0
#endif

#endif
