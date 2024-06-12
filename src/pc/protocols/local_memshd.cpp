/**
 * @file    local_memshd.c
 * @brief   Shared memory helper definitions
*/

#include "local_memshd.h"
#include "../PlatformDeviceFd.h"

#define MVLOG_UNIT_NAME local_memshd
#include "XLinkLog.h"

#if defined(__unix__)
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

int shdmem_initialize() {
    printf("Shared mem initialize function called\n");
    return 0;
}

int shdmemPlatformConnect(const char *devPathRead, const char *devPathWrite, void **fd) {
    printf("Shared mem platform connect function called\n");

    const char *socketPath = devPathWrite;
    int socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketFd < 0) {
	perror("Socket creation failed");
	return -1;
    }

    struct sockaddr_un sockAddr;
    memset(&sockAddr, 0, sizeof(sockAddr));
    sockAddr.sun_family = AF_UNIX;
    strcpy(sockAddr.sun_path, socketPath);

    if (connect(socketFd, (struct sockaddr *)&sockAddr, sizeof(sockAddr)) < 0) {
	perror("Connect failed");
	return -1;
    }

    // Store the socket and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) socketFd);

    return 0;
}

int shdmemPlatformServer(const char *devPathRead, const char *devPathWrite, void **fd) {
    printf("Shared mem platform server function called\n");

    const char *socketPath = devPathWrite;
    printf("Socket path: %s\n", socketPath);

    int socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketFd < 0) {
	    perror("socket creation failed");
	    return -1;
    }

    struct sockaddr_un addrUn;
    memset(&addrUn, 0, sizeof(addrUn));
    addrUn.sun_family = AF_UNIX;
    strcpy(addrUn.sun_path, socketPath);
    unlink(socketPath);

    if (bind(socketFd, (struct sockaddr *)&addrUn, sizeof(addrUn)) < 0) {
	    perror("bind failed");
	    return -1;
    }

    listen(socketFd, 1);
    printf("Waiting for a connection...\n");
    int clientFd = accept(socketFd, NULL, NULL);
    if (clientFd < 0) {
	    perror("accept failed");
	    return -1;
    }

    // Store the socket and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *fd = createPlatformDeviceFdKey((void*) (uintptr_t) clientFd);

    return 0;

}

int shdmemPlatformRead(void *fd, void *data, int size) {
    printf("Shared mem read function called\n");
 
    long socketFd = 0;
    if(getPlatformDeviceFdFromKey(fd, (void**)&socketFd)) {
    	printf("Failed\n");
	return -1;
    }
    printf("FD: 0x%x Data: 0x%x Size: %d\n", socketFd, data, size);

    struct msghdr msg = {};
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = size;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if(recvmsg(socketFd, &msg, 0) < 0) {
	perror("Failed to recieve message");
        return -1;
    }

    return 0;
}

int shdmemPlatformWrite(void *fd, void *data, int size) {
    printf("Shared mem write function called\n");

    long socketFd = 0;
    if(getPlatformDeviceFdFromKey(fd, (void**)&socketFd)) {
    	printf("Failed\n");
	return -1;
    }
    printf("FD: 0x%x Data: 0x%x Size: %d\n", socketFd, data, size);

    struct msghdr msg = {};
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = size;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if(sendmsg(socketFd, &msg, 0) < 0) {
	perror("Failed to send message");
        return -1;
    }

    return 0;
}

int shdmemPlatformReadFD(void *fd, long *passedFd) {
    printf("Shared mem read FD function called\n");

    while(true);

    long socketFd = 0;
    if(getPlatformDeviceFdFromKey(fd, (void**)&socketFd)) {
	printf("Failed\n");
	return -1;
    }
    printf("FD: 0x%x Data: 0x%x\n", socketFd, *passedFd);

    struct msghdr msg = {};
    struct iovec iov;
    char buf[1];
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char ancillaryElementBuffer[CMSG_SPACE(sizeof(long))];
    msg.msg_control = ancillaryElementBuffer;
    msg.msg_controllen = sizeof(ancillaryElementBuffer);

    if (recvmsg(socketFd, &msg, 0) < 0) {
	perror("Failed to receive message");
	return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
	    *passedFd = *((int *)CMSG_DATA(cmsg));
	    return 0;
    }
    while(true);

    return -1; // Return an invalid fd on failure
}

int shdmemPlatformWriteFD(void *fd, long *passedFd) {
    printf("Shared mem write FD function called\n");

    long socketFd = 0;
    if(getPlatformDeviceFdFromKey(fd, (void**)&socketFd)) {
    	printf("Failed\n");
	return -1;
    }
    printf("FD: 0x%x Data: 0x%x\n", socketFd, *passedFd);

    struct msghdr msg = {};
    struct iovec iov;
    char buf[1] = {0}; // Buffer for single byte of data to send
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char ancillaryElementBuffer[CMSG_SPACE(sizeof(long))];
    msg.msg_control = ancillaryElementBuffer;
    msg.msg_controllen = sizeof(ancillaryElementBuffer);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((long*)CMSG_DATA(cmsg)) = *passedFd;

    if (sendmsg(socketFd, &msg, 0) < 0) {
	    perror("Failed to send message");
	    return -1;
    }

    return 0;
}

#endif /* !defined(__unix__) */
