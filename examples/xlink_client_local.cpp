#if defined(__unix__)

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
const char *SHARED_MEMORY_NAME = "/xlink_shared_memory_b";

XLinkGlobalHandler_t xlinkGlobalHandler = {};

int main(int argc, const char** argv){
    xlinkGlobalHandler.protocol = X_LINK_TCP_IP;

    mvLogDefaultLevelSet(MVLOG_ERROR);

    printf("Initializing XLink...\n");
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
    	printf("Initializing wasn't successful\n");
        return 1;
    }

    XLinkHandler_t handler;
    handler.devicePath = "127.0.0.1";
    handler.protocol = X_LINK_TCP_IP;
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

    long receivedFd = packet->fd;
    if (receivedFd < 0) {
	printf("Not a valid FD, data streamed through message\n");
	return 1;
    }
    
    // Map the shared memory
    void *sharedMemAddr =
	    mmap(NULL, MAXIMUM_SHM_SIZE, PROT_READ, MAP_SHARED, receivedFd, 0);
    if (sharedMemAddr == MAP_FAILED) {
	    perror("mmap");
	    return 1;
    }

    // Read and print the message from shared memory
    printf("Message from Process A: %s\n", static_cast<char *>(sharedMemAddr));

    const char *normalMessage = "Normal message from Process B";
    auto w = XLinkWriteData(s, (uint8_t*)normalMessage, strlen(normalMessage) + 1);
    assert(w == X_LINK_SUCCESS);

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
    const char *message = "Shared message from Process B!";
    memcpy(addr, message, strlen(message) + 1);

    // Send the FD through the XLinkWriteFd function
    w = XLinkWriteFd(s, shmFd); 
    assert(w == X_LINK_SUCCESS);

    r = XLinkReadData(s, &packet);
    assert(w == X_LINK_SUCCESS);

    printf("Message from Process A: %s\n", (char *)(packet->data));


    munmap(sharedMemAddr, MAXIMUM_SHM_SIZE);

    munmap(addr, MAXIMUM_SHM_SIZE);
    close(shmFd);
    unlink(shmName);

    return 0;
}

#else 

int main() {
    return -1;
}

#endif
