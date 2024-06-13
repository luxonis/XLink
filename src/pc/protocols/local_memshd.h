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

int shdmemPlatformConnect(const char *devPathRead, const char *devPathWrite, void **desc);
int shdmemPlatformServer(const char *devPathRead, const char *devPathWrite, void **desc);

int shdmemPlatformRead(void *desc, void *data, int size, long *fd);
int shdmemPlatformWrite(void *desc, void *data, int size);
int shdmemPlatformWriteFd(void *desc, void *data);
 
#ifdef __cplusplus
}
#endif

#endif /* !defined(__unix__) */

#endif /* LOCAL_MEMSHD_H */
