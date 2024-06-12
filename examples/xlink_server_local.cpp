#include <cstring>
#include <cstddef>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>  
#include <cassert>

#include "XLink/XLink.h"
#include "XLink/XLinkPublicDefines.h"
#include "XLink/XLinkLog.h"
    
const long MAXIMUM_SHM_SIZE = 4096;
const char *SHARED_MEMORY_NAME = "/xlink_shared_memory";

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
    status = XLinkServerOnly(&handler);
    if(X_LINK_SUCCESS != status) {
    	printf("Connecting wasn't successful\n");
        return 1;
    }

    const char *shmName = SHARED_MEMORY_NAME;
    long shmFd = shm_open(shmName, O_CREAT | O_RDWR, 0666);
    if (shmFd < 0) {
	    perror("shm_open");
	    return 1;
    }

    ftruncate(shmFd, MAXIMUM_SHM_SIZE);

    void *addr = mmap(NULL, MAXIMUM_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
    if (addr == MAP_FAILED) {
	    perror("mmap");
	    close(shmFd);
	    shm_unlink(shmName);
	    return 1;
    }

    // Write a message to the shared memory
    const char *message = "Hello from Process A!";
    memcpy(addr, message, strlen(message) + 1);

    printf("Shm FD: %d\n", shmFd);

    auto s = XLinkOpenStream(0, "test", 1024);
    assert(s != INVALID_STREAM_ID);

    // Send the FD through the XLinkWriteFd function
    auto w = XLinkWriteFd(s, &shmFd); 
    assert(w == X_LINK_SUCCESS);
    
    streamPacketDesc_t *packet;
    auto r = XLinkReadData(s, &packet);
    assert(w == X_LINK_SUCCESS);

    munmap(addr, MAXIMUM_SHM_SIZE);
    close(shmFd);
    unlink(shmName);

    return 0;
}
