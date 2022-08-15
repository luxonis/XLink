#include "XLink/XLink.h"
#include <atomic>

static std::atomic<bool> protocolInitialized[XLINK_NMB_OF_PROTOCOLS];

extern "C" void xlinkSetProtocolInitialized(const XLinkProtocol_t protocol, int initialized) {
    if(protocol >= 0 && protocol < XLINK_NMB_OF_PROTOCOLS) {
        protocolInitialized[protocol] = initialized;
    }
}

extern "C" int XLinkIsProtocolInitialized(const XLinkProtocol_t protocol) {
    if(protocol >= 0 && protocol < XLINK_NMB_OF_PROTOCOLS) {
        return protocolInitialized[protocol];
    }
    return 0;
}
