#include "XLink/XLink.h"
#include <atomic>

static std::atomic<bool> protocolInitialized[X_LINK_NMB_OF_PROTOCOLS];

extern "C" void xlinkSetProtocolInitialized(const XLinkProtocol_t protocol, int initialized) {
    if(protocol >= 0 && protocol < X_LINK_NMB_OF_PROTOCOLS) {
        protocolInitialized[protocol] = initialized;
    }
}

int XLinkIsProtocolInitialized(const XLinkProtocol_t protocol) {
    if(protocol >= 0 && protocol < X_LINK_NMB_OF_PROTOCOLS) {
        return protocolInitialized[protocol];
    }
    return 0;
}
