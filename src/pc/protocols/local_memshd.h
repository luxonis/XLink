/**
 * @file    local_memshd.h
 * @brief   Shared memory helper public header
*/

#ifndef LOCAL_MEMSHD_H
#define LOCAL_MEMSHD_H

/* **************************************************************************/
/*      Include Files                                                       */
/* **************************************************************************/
#include "XLinkPlatform.h"
#include "XLinkPublicDefines.h"


#if defined(__unix__)

#define SHDMEM_DEFAULT_SOCKET "/tmp/xlink.sock"

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Initializes Shared Memory protocol
*/
int shdmem_initialize();

int shdmemPlatformConnect(const char *devPathRead, const char *devPathWrite, void **desc);
int shdmemPlatformServer(const char *devPathRead, const char *devPathWrite, void **desc);
int shdmemPlatformClose(void **desc);

int shdmemPlatformRead(void *desc, void *data, int size, long *fd);
int shdmemPlatformWrite(void *desc, void *data, int size);
int shdmemPlatformWriteFd(void *desc, void *data, void *data2, int size2);

int shdmemSetProtocol(XLinkProtocol_t *protocol, const char* devPathRead, const char* devPathWrite);
 
#ifdef __cplusplus
}
#endif

#endif /* !defined(__unix__) */

#endif /* LOCAL_MEMSHD_H */
