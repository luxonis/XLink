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

const long MAXIMUM_SHM_SIZE = 4096;

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
    
    // Read the data packet containing the FD
    auto r = XLinkReadData(s, &packet);
    assert(r == X_LINK_SUCCESS);

    long receivedFd = *(long*)packet->data;
    printf("Received fd: %d\n", receivedFd);

    // Map the shared memory
    void *sharedMemAddr =
	    mmap(NULL, MAXIMUM_SHM_SIZE, PROT_READ, MAP_SHARED, receivedFd, 0);
    if (sharedMemAddr == MAP_FAILED) {
	    perror("mmap");
	    return 1;
    }

    // Read and print the message from shared memory
    printf("Message from Process A: %s\n", static_cast<char *>(sharedMemAddr));

    auto w = XLinkWriteData(s, (uint8_t*)&s, sizeof(s));
    assert(w == X_LINK_SUCCESS);

    munmap(sharedMemAddr, MAXIMUM_SHM_SIZE);

    return 0;
}
