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
const char *SHARED_MEMORY_NAME = "/xlink_shared_memory_a";

XLinkGlobalHandler_t xlinkGlobalHandler = {};

int main(int argc, const char** argv){
    xlinkGlobalHandler.protocol = X_LINK_TCP_IP_OR_LOCAL_SHDMEM;

    mvLogDefaultLevelSet(MVLOG_ERROR);

    printf("Initializing XLink...\n");
    auto status = XLinkInitialize(&xlinkGlobalHandler);
    if(X_LINK_SUCCESS != status) {
    	printf("Initializing wasn't successful\n");
        return 1;
    }

    XLinkHandler_t handler;
    handler.devicePath = "0.0.0.0";
    handler.protocol = X_LINK_TCP_IP_OR_LOCAL_SHDMEM;
    status = XLinkServerOnly(&handler);
    if(X_LINK_SUCCESS != status) {
    	printf("Connecting wasn't successful\n");
        return 1;
    }

    auto s = XLinkOpenStream(0, "test", 1024 * 1024);
    assert(s != INVALID_STREAM_ID);

    const char *shmName = SHARED_MEMORY_NAME;
    long shmFd = memfd_create(shmName, 0);
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
    const char *message = "Shared message from Process A!";
    memcpy(addr, message, strlen(message) + 1);

    // Send the FD through the XLinkWriteFd function
    auto w = XLinkWriteFd(s, shmFd); 
    assert(w == X_LINK_SUCCESS);
    
    streamPacketDesc_t *packet;
    auto r = XLinkReadData(s, &packet);
    assert(r == X_LINK_SUCCESS);

    printf("Message from Process B: %s\n", (char *)(packet->data));

    // Read the data packet containing the FD
    r = XLinkReadData(s, &packet);
    assert(r == X_LINK_SUCCESS);

    void *sharedMemAddr;
    long receivedFd = packet->fd;
    if (receivedFd < 0) {
	printf("Not a valid FD, data streamed through message\n");
	sharedMemAddr = packet->data;
    } else { 
        // Map the shared memory
        sharedMemAddr = mmap(NULL, MAXIMUM_SHM_SIZE, PROT_READ, MAP_SHARED, receivedFd, 0);
	if (sharedMemAddr == MAP_FAILED) {
	    perror("mmap");
	    return 1;
	}
    }

    // Read and print the message from shared memory
    printf("Message from Process B: %s\n", static_cast<char *>(sharedMemAddr));

    const char *normalMessage = "Normal message from Process A";
    w = XLinkWriteData(s, (uint8_t*)normalMessage, strlen(normalMessage) + 1);
    assert(w == X_LINK_SUCCESS);

    if (receivedFd >= 0) {
        munmap(sharedMemAddr, MAXIMUM_SHM_SIZE);
    }

    munmap(addr, MAXIMUM_SHM_SIZE);
    close(shmFd);
    unlink(shmName);

    return 0;
}
