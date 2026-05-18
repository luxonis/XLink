#include "tcpip_host.h"
#include "local_memshd.h"
#include "tcpip_memshd.h"

#define MVLOG_UNIT_NAME tcpip_memshd
#include "XLinkLog.h"

#include <signal.h>

#include <atomic>
#include <condition_variable>
#include <thread>

#include <cstdio>

#if defined(__unix__)

int tcpipOrLocalShdmemPlatformServer(XLinkProtocol_t *protocol, const char *devPathRead, const char *devPathWrite, void **fd)
{
    std::mutex connectionMutex;
    std::condition_variable cv;

    bool isShdmemThreadFinished = false;
    bool isTcpIpThreadFinished = false;

    int retTcpIp = -1, retShdmem = -1;
    void *fdTcpIp = nullptr, *fdShdmem = nullptr;
    long tcpIpSockFd = -1, shdmemSockFd = -1;

    auto threadShdmem = std::thread([&connectionMutex,
		                     &cv,
				     &isShdmemThreadFinished,
				     &retShdmem,
				     &fdShdmem,
				     &shdmemSockFd](){
        auto ret = shdmemPlatformServer(SHDMEM_DEFAULT_SOCKET, SHDMEM_DEFAULT_SOCKET, &fdShdmem, &shdmemSockFd);
        {
            std::unique_lock<std::mutex> l(connectionMutex);
            retShdmem = ret;
            isShdmemThreadFinished = true;
        }
        cv.notify_one();
    });

    auto threadTcpip = std::thread([&connectionMutex,
		                    &cv,
				    &isTcpIpThreadFinished,
				    &retTcpIp,
				    &devPathRead,
				    &devPathWrite,
				    &fdTcpIp,
				    &tcpIpSockFd]() {
        auto ret = tcpipPlatformServer(devPathRead, devPathWrite, &fdTcpIp, &tcpIpSockFd);
        {
            std::unique_lock<std::mutex> l(connectionMutex);
            retTcpIp = ret;
            isTcpIpThreadFinished = true;
        }
        cv.notify_one();
    });

    {
        std::unique_lock<std::mutex> lock(connectionMutex);
        cv.wait(lock, [&isShdmemThreadFinished, &isTcpIpThreadFinished]{ return isShdmemThreadFinished || isTcpIpThreadFinished; });

    }

    // As soon as either one finishes, the other can be cleaned
    // Use signals, as "accept" cannot be unblocked by "close"ing the underlying socket
    if(!isTcpIpThreadFinished) {
	if(tcpIpSockFd >= 0) {
	    shutdown(tcpIpSockFd, SHUT_RDWR);
            #if defined(SO_LINGER)
        	const int set = 0;
		setsockopt(tcpIpSockFd, SOL_SOCKET, SO_LINGER, (const char*)&set, sizeof(set));
            #endif
	    close(tcpIpSockFd);
	}
	
	mvLog(MVLOG_ERROR, "Failed to start server with TCP/IP");
    }

    if(!isShdmemThreadFinished) {
	if(shdmemSockFd >= 0) {
	    shutdown(shdmemSockFd, SHUT_RDWR);
            #if defined(SO_LINGER)
        	const int set = 0;
		setsockopt(shdmemSockFd, SOL_SOCKET, SO_LINGER, (const char*)&set, sizeof(set));
            #endif
	    close(shdmemSockFd);
	}
	
	mvLog(MVLOG_ERROR, "Failed to start server with SHDMEM");
    }

    // Wait for both threads to wrap up
    if(threadTcpip.joinable()) threadTcpip.join();
    if(threadShdmem.joinable()) threadShdmem.join();

    // Asign the final protocol (once both threads finalize)
    if(retTcpIp == 0) {
        *fd = fdTcpIp;
	*protocol = X_LINK_TCP_IP;
    }

    if(retShdmem == X_LINK_SUCCESS) {
        *fd = fdShdmem;
	shdmemSetProtocol(protocol, devPathRead, devPathWrite);
    }

    // if both connected, close TCP_IP
    if(retTcpIp == 0 && retShdmem == X_LINK_SUCCESS) {
        tcpipPlatformClose(fdTcpIp);
    }

    return X_LINK_SUCCESS;
}

int tcpipOrLocalShdmemPlatformConnect(XLinkProtocol_t *protocol, const char *devPathRead, const char *devPathWrite, void **fd) {
    if(shdmemPlatformConnect(SHDMEM_DEFAULT_SOCKET, SHDMEM_DEFAULT_SOCKET, fd) == X_LINK_SUCCESS) {
	return shdmemSetProtocol(protocol, devPathRead, devPathWrite);
    }

    if (tcpipPlatformConnect(devPathRead, devPathWrite, fd) == X_LINK_SUCCESS) {
	*protocol = X_LINK_TCP_IP;
	return X_LINK_SUCCESS;
    }

    return X_LINK_ERROR;
}

#else 

int tcpipOrLocalShdmemPlatformServer(XLinkProtocol_t *protocol, const char *devPathRead, const char *devPathWrite, void **fd)
{
    *protocol = X_LINK_TCP_IP;
    return tcpipPlatformServer(devPathRead, devPathWrite, fd, nullptr);
}

int tcpipOrLocalShdmemPlatformConnect(XLinkProtocol_t *protocol, const char *devPathRead, const char *devPathWrite, void **fd)
{
    *protocol = X_LINK_TCP_IP;
    return tcpipPlatformConnect(devPathRead, devPathWrite, fd);
}

#endif
