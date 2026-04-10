#include "XLinkPrivateFields.h"

#include <cstdio>
#include <mutex>

namespace {

std::mutex globalHandlerMutex;
XLinkGlobalHandler_t* globalHandler = nullptr;

}  // namespace

extern "C" {

void XLinkGlobalHandlerAssign(XLinkGlobalHandler_t* handler) {
    std::lock_guard<std::mutex> lock(globalHandlerMutex);
    globalHandler = handler;
}

int XLinkGlobalHandlerIsValid(void) {
    std::lock_guard<std::mutex> lock(globalHandlerMutex);
    return globalHandler != nullptr;
}

void XLinkGlobalHandlerStartProfiling(void) {
    std::lock_guard<std::mutex> lock(globalHandlerMutex);
    if (globalHandler == nullptr) {
        return;
    }

    globalHandler->profEnable = 1;
    globalHandler->profilingData.totalReadBytes = 0;
    globalHandler->profilingData.totalWriteBytes = 0;
    globalHandler->profilingData.totalWriteTime = 0;
    globalHandler->profilingData.totalReadTime = 0;
    globalHandler->profilingData.totalBootCount = 0;
    globalHandler->profilingData.totalBootTime = 0;
}

void XLinkGlobalHandlerStopProfiling(void) {
    std::lock_guard<std::mutex> lock(globalHandlerMutex);
    if (globalHandler == nullptr) {
        return;
    }

    globalHandler->profEnable = 0;
}

void XLinkGlobalHandlerAccumulateRead(uint32_t bytes, float timeSeconds) {
    std::lock_guard<std::mutex> lock(globalHandlerMutex);
    if (globalHandler == nullptr || !globalHandler->profEnable) {
        return;
    }

    globalHandler->profilingData.totalReadBytes += bytes;
    globalHandler->profilingData.totalReadTime += timeSeconds;
}

void XLinkGlobalHandlerAccumulateWrite(uint32_t bytes, float timeSeconds) {
    std::lock_guard<std::mutex> lock(globalHandlerMutex);
    if (globalHandler == nullptr || !globalHandler->profEnable) {
        return;
    }

    globalHandler->profilingData.totalWriteBytes += bytes;
    globalHandler->profilingData.totalWriteTime += timeSeconds;
}

int XLinkGlobalHandlerCopyProfilingData(XLinkProf_t* prof) {
    if (prof == nullptr) {
        return 1;
    }

    std::lock_guard<std::mutex> lock(globalHandlerMutex);
    if (globalHandler == nullptr) {
        return 1;
    }

    *prof = globalHandler->profilingData;
    return 0;
}

void XLinkGlobalHandlerPrintProfilingData(void) {
    std::lock_guard<std::mutex> lock(globalHandlerMutex);
    if (globalHandler == nullptr) {
        return;
    }

    printf("XLink profiling results:\n");
    if (globalHandler->profilingData.totalWriteTime) {
        printf("Average write speed: %f MB/Sec\n",
               globalHandler->profilingData.totalWriteBytes /
                   globalHandler->profilingData.totalWriteTime / 1024.0 / 1024.0);
    }
    if (globalHandler->profilingData.totalReadTime) {
        printf("Average read speed: %f MB/Sec\n",
               globalHandler->profilingData.totalReadBytes /
                   globalHandler->profilingData.totalReadTime / 1024.0 / 1024.0);
    }
    if (globalHandler->profilingData.totalBootCount) {
        printf("Average boot speed: %f sec\n",
               globalHandler->profilingData.totalBootTime /
                   globalHandler->profilingData.totalBootCount);
    }
}

}  // extern "C"
