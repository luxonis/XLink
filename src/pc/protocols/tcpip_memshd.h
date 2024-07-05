#pragma once

#include "XLinkPlatform.h"
#include "XLinkPublicDefines.h"

#ifdef __cplusplus
extern "C" {
#endif

int tcpipOrLocalShdmemPlatformServer(XLinkProtocol_t *protocol, const char *devPathRead, const char *devPathWrite, void **fd);
int tcpipOrLocalShdmemPlatformConnect(XLinkProtocol_t *protocol, const char *devPathRead, const char *devPathWrite, void **fd);

#ifdef __cplusplus
}
#endif
