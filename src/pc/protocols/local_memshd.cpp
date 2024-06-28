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
    mvLog(MVLOG_DEBUG, "Shared memory initialized\n");
    return X_LINK_SUCCESS;
}

int shdmemPlatformConnect(const char *devPathRead, const char *devPathWrite, void **desc) {
    const char *socketPath = devPathWrite;

    mvLog(MVLOG_DEBUG, "Shared memory connect invoked with socket path %s\n", socketPath);

    int socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketFd < 0) {
	mvLog(MVLOG_FATAL, "Socket creation failed");
	return X_LINK_ERROR;
    }

    struct sockaddr_un sockAddr;
    memset(&sockAddr, 0, sizeof(sockAddr));
    sockAddr.sun_family = AF_UNIX;
    strcpy(sockAddr.sun_path, socketPath);

    if (connect(socketFd, (struct sockaddr *)&sockAddr, sizeof(sockAddr)) < 0) {
	mvLog(MVLOG_FATAL, "Socket connection failed");
	return X_LINK_ERROR;
    }

    // Store the socket and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *desc = createPlatformDeviceFdKey((void*) (uintptr_t) socketFd);

    return X_LINK_SUCCESS;
}

int shdmemPlatformServer(const char *devPathRead, const char *devPathWrite, void **desc) {
    const char *socketPath = devPathWrite;
    mvLog(MVLOG_DEBUG, "Shared memory server invoked with socket path %s\n", socketPath);

    int socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketFd < 0) {
	    mvLog(MVLOG_FATAL, "Socket creation failed");
	    return X_LINK_ERROR;
    }

    struct sockaddr_un addrUn;
    memset(&addrUn, 0, sizeof(addrUn));
    addrUn.sun_family = AF_UNIX;
    strcpy(addrUn.sun_path, socketPath);
    unlink(socketPath);

    if (bind(socketFd, (struct sockaddr *)&addrUn, sizeof(addrUn)) < 0) {
	    mvLog(MVLOG_FATAL, "Socket bind failed");
	    return X_LINK_ERROR;
    }

    listen(socketFd, 1);
    mvLog(MVLOG_DEBUG, "Waiting for a connection...\n");
    int clientFd = accept(socketFd, NULL, NULL);
    if (clientFd < 0) {
	    mvLog(MVLOG_FATAL, "Socket accept failed");
	    return X_LINK_ERROR;
    }

    // Store the socket and create a "unique" key instead
    // (as file descriptors are reused and can cause a clash with lookups between scheduler and link)
    *desc = createPlatformDeviceFdKey((void*) (uintptr_t) clientFd);

    return X_LINK_SUCCESS;

}

int shdmemPlatformClose(void **desc) {
    long socketFd = 0;
    if(getPlatformDeviceFdFromKey(desc, (void**)&socketFd)) {
    	mvLog(MVLOG_DEBUG, "Failed\n");
	return X_LINK_ERROR;
    }

    close(socketFd);

    return X_LINK_SUCCESS;
}

int shdmemPlatformRead(void *desc, void *data, int size, long *fd) {
    long socketFd = 0;
    if(getPlatformDeviceFdFromKey(desc, (void**)&socketFd)) {
    	mvLog(MVLOG_DEBUG, "Failed\n");
	return X_LINK_ERROR;
    }

    struct msghdr msg = {};
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = size;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char ancillaryElementBuffer[CMSG_SPACE(sizeof(long))];
    msg.msg_control = ancillaryElementBuffer;
    msg.msg_controllen = sizeof(ancillaryElementBuffer);

    int bytes;
    if(bytes = recvmsg(socketFd, &msg, 0) < 0) {
	mvLog(MVLOG_ERROR, "Failed to recieve message");
        return X_LINK_ERROR;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
	long recvFd = *((long*)CMSG_DATA(cmsg));
	mvLog(MVLOG_DEBUG, "We received ad FD: %d\n", recvFd);
	
	
	/* We have recieved a FD */
	*fd = recvFd;
    }

    return bytes;
}

int shdmemPlatformWrite(void *desc, void *data, int size) {
    long socketFd = 0;
    if(getPlatformDeviceFdFromKey(desc, (void**)&socketFd)) {
    	mvLog(MVLOG_ERROR, "Failed to get the socket FD\n");
	return X_LINK_ERROR;
    }

    struct msghdr msg = {};
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = size;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    int bytes;
    if(bytes = sendmsg(socketFd, &msg, 0) < 0) {
	mvLog(MVLOG_ERROR, "Failed to send message\n");
        return X_LINK_ERROR;
    }

    return bytes;
}

int shdmemPlatformWriteFd(void *desc, const long fd, void *data2, int size2) {
    long socketFd = 0;
    if(getPlatformDeviceFdFromKey(desc, (void**)&socketFd)) {
    	mvLog(MVLOG_ERROR, "Failed to get the socket FD\n");
	return X_LINK_ERROR;
    }

    struct msghdr msg = {};
    struct iovec iov;
    char buf[1] = {0}; // Buffer for single byte of data to send
    if (data2 != NULL && size2 > 0) {
	iov.iov_base = data2;
        iov.iov_len = size2;
    } else {
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
    }
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (fd >= 0) {
	char ancillaryElementBuffer[CMSG_SPACE(sizeof(long))];
	msg.msg_control = ancillaryElementBuffer;
	msg.msg_controllen = sizeof(ancillaryElementBuffer);

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(long));

	*((long*)CMSG_DATA(cmsg)) = fd;
    }

    int bytes;
    if(bytes = sendmsg(socketFd, &msg, 0) < 0) {
	mvLog(MVLOG_ERROR, "Failed to send message");
        return X_LINK_ERROR;
    }

    return bytes;
}

int shdmemSetProtocol(XLinkProtocol_t *protocol, const char* devPathRead, const char* devPathWrite) {
    devPathWrite = devPathRead = SHDMEM_DEFAULT_SOCKET;
    *protocol = X_LINK_LOCAL_SHDMEM;
    return X_LINK_SUCCESS;
}

#endif /* !defined(__unix__) */
