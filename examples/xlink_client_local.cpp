#include <cstring>
#include <cstddef>
#include <cassert>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

    auto s = XLinkOpenStream(0, "test", 1024);
    assert(s != INVALID_STREAM_ID);

    long shmFd = -1;
    auto r = XLinkReadFD(s, &shmFd);
    assert(r == X_LINK_SUCCESS);

    if (shmFd <= 0) {
	printf("Invalid fd\n");
	return 1;
    }

    printf("FD: %d\r\n", shmFd);

    // Map the shared memory
    void *sharedMemAddr =
	    mmap(NULL, 4096, PROT_READ, MAP_SHARED, shmFd, 0);
    if (sharedMemAddr == MAP_FAILED) {
	    perror("mmap");
	    return 1;
    }

    // Read and print the message from shared memory
    printf("Message from Process A: %s\n", static_cast<char *>(sharedMemAddr));

    munmap(sharedMemAddr, 4096);
    close(shmFd); // Close the shared memory fd

    auto w = XLinkWriteData(s, (uint8_t*)&s, sizeof(s));
    assert(w == X_LINK_SUCCESS);

    return 0;
}
