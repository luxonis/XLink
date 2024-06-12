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

    const char *shm_name = "/my_shared_memory";
    long shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
	    perror("shm_open");
	    return 1;
    }
    ftruncate(shm_fd, 4096);

    void *addr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) {
	    perror("mmap");
	    close(shm_fd);
	    shm_unlink(shm_name);
	    return 1;
    }

    // Write a message to the shared memory
    const char *message = "Hello from Process A!";
    memcpy(addr, message, strlen(message) + 1);

    printf("Shm FD: %d\n", shm_fd);

    auto s = XLinkOpenStream(0, "test", 1024);
    assert(s != INVALID_STREAM_ID);
    auto w = XLinkWriteFd(s, &shm_fd); 
    assert(w == X_LINK_SUCCESS);

    return 0;
}
