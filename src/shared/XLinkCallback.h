#ifndef _X_LINK_CALLBACK_H_
#define _X_LINK_CALLBACK_H_

#include <XLink.h>

#ifdef __cplusplus
extern "C"
{
#endif

int XLinkAddLinkDownCb(void (*cb)(linkId_t));
int XLinkRemoveLinkDownCb(int cbId);
void XLinkPlatformLinkDownNotify(linkId_t linkId);

#ifdef __cplusplus
}
#endif

#endif
