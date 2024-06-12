#include <cstring>
#include <cstddef>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>  
#include <cassert>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"

XLinkGlobalHandler_t xlinkGlobalHandler = {};

int main(int argc, const char** argv){
    xlinkGlobalHandler.protocol = X_LINK_LOCAL_SHDMEM;

    mvLogDefaultLevelSet(MVLOG_ERROR);

    printf("Initializing XLink...\n");
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
    	printf("Initializing wasn't successful\n");
        return 1;
    }

    XLinkHandler_t handler;
    handler.devicePath = "/tmp/xlink.sock";
    handler.protocol = X_LINK_LOCAL_SHDMEM;
    status = XLinkConnect(&handler);
    if(X_LINK_SUCCESS != status) {
    	printf("Connecting wasn't successful\n");
        return 1;
    }

    streamPacketDesc_t *packet;

    auto s = XLinkOpenStream(0, "test", 1024);
    assert(s != INVALID_STREAM_ID);
    auto r = XLinkReadData(s, &packet);
    assert(r == X_LINK_SUCCESS);

    long received_fd = *(long*)packet->data;
    printf("Received fd: %d\n", received_fd);

    // Map the shared memory
    void *shared_mem_addr =
	    mmap(NULL, 4096, PROT_READ, MAP_SHARED, received_fd, 0);
    if (shared_mem_addr == MAP_FAILED) {
	    perror("mmap");
	    return 1;
    }

    // Read and print the message from shared memory
    printf("Message from Process A: %s\n", static_cast<char *>(shared_mem_addr));

    return 0;
}
