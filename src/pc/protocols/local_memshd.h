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

#ifdef __cplusplus
extern "C" {
#endif
/**
 * @brief Initializes Shared Memory protocol
*/
int shdmem_initialize();

int shdmemPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd);
int shdmemPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd);

int shdmemPlatformRead(void *fd, void *data, int size);
int shdmemPlatformWrite(void *fd, void *data, int size);

int shdmemPlatformWriteFd(void *fd, void *data);
 
#ifdef __cplusplus
}
#endif

#endif /* !defined(__unix__) */

#endif /* LOCAL_MEMSHD_H */
