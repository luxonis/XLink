// Regression test: a timed out XLinkReadMoveDataWithTimeout() must not stay
// queued in the dispatcher with a pointer back to the stack of the caller.
//
// XLinkReadMoveDataWithTimeout() keeps its xLinkEvent_t on its own stack and
// the dispatcher keeps a pointer to it (xLinkEventPriv_t::retEv) to copy the
// result back. Before the fix, on a timeout the function returned, but the
// event stayed in the local queue as EVENT_BLOCKED. A packet that came later,
// or a link down, then made the dispatcher copy the event (128 bytes) into
// the stack frame that was already returned and post the semaphore of the
// calling thread.
//
// Modes (argv[1]):
//   late-data   - after the timeout the server writes one packet. The next
//                 read on the same thread must return exactly this packet.
//   link-down   - after the timeout the client closes the link with
//                 XLinkResetRemoteTimeout(), as depthai-core does when a ping
//                 is missed.
//   peer-killed - (POSIX only) the server runs in a child process. After the
//                 timeout the child is killed (like a crashed device), so the
//                 eventReader thread of the client gets a receive error and
//                 frees the queued events.
// Without the fix, with -fsanitize=address (detect_stack_use_after_return=1,
// the default in GCC 13) all modes report "stack-use-after-return" in
// postAndMarkEventServed(). Without a sanitizer, "late-data" and "link-down"
// crash (the stale write corrupts the stack of the caller).

#undef NDEBUG
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "XLink/XLink.h"
#include "XLink/XLinkLog.h"
#include "XLink/XLinkPublicDefines.h"

using namespace std::chrono;

static constexpr const char* STREAM_NAME = "read_timeout";
static constexpr int STREAM_SIZE = 1024;
static constexpr unsigned SHORT_TIMEOUT_MS = 100;
static constexpr unsigned LONG_TIMEOUT_MS = 3000;
static const char PAYLOAD[] = "late packet";

class Flag {
   public:
    void set() {
        std::lock_guard<std::mutex> lock(mtx);
        value = true;
        cv.notify_all();
    }
    bool wait(milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, timeout, [this]() { return value; });
    }

   private:
    std::mutex mtx;
    std::condition_variable cv;
    bool value = false;
};

static Flag serverReady;
static Flag writeRequested;
static Flag serverWrote;
static Flag clientDone;
static std::atomic<bool> serverOk{true};

static XLinkGlobalHandler_t globalHandler = {};

static bool initialize() {
    globalHandler.protocol = X_LINK_TCP_IP;
    mvLogDefaultLevelSet(MVLOG_ERROR);
    return XLinkInitialize(&globalHandler) == X_LINK_SUCCESS;
}

// Server side. In "late-data" mode it writes one packet on request.
static void server(std::string path, bool writeOnRequest) {
    XLinkHandler_t handler = {};
    handler.devicePath = &path[0];
    handler.protocol = X_LINK_TCP_IP;
    serverReady.set();
    if(XLinkServerOnly(&handler) != X_LINK_SUCCESS) {
        printf("server: XLinkServerOnly failed\n");
        serverOk = false;
        return;
    }
    streamId_t s = XLinkOpenStream(handler.linkId, STREAM_NAME, STREAM_SIZE);
    if(s == INVALID_STREAM_ID) {
        printf("server: XLinkOpenStream failed\n");
        serverOk = false;
        return;
    }
    if(writeOnRequest && writeRequested.wait(milliseconds(10 * LONG_TIMEOUT_MS))) {
        if(XLinkWriteData(s, reinterpret_cast<const uint8_t*>(PAYLOAD), sizeof(PAYLOAD)) != X_LINK_SUCCESS) {
            printf("server: XLinkWriteData failed\n");
            serverOk = false;
        }
        serverWrote.set();
    }
    clientDone.wait(milliseconds(10 * LONG_TIMEOUT_MS));
}

static bool connect(std::string path, XLinkHandler_t& handler) {
    static std::string devicePath;
    devicePath = path;
    handler = {};
    handler.devicePath = &devicePath[0];
    handler.protocol = X_LINK_TCP_IP;
    for(int i = 0; i < 100; i++) {
        if(XLinkConnect(&handler) == X_LINK_SUCCESS) return true;
        std::this_thread::sleep_for(milliseconds(50));
    }
    return false;
}

// Returns the result of one XLinkReadMoveDataWithTimeout() call and the data.
static XLinkError_t readOnce(streamId_t s, unsigned timeoutMs, std::string& data) {
    streamPacketDesc_t packet = {};
    XLinkError_t rc = XLinkReadMoveDataWithTimeout(s, &packet, timeoutMs);
    if(rc == X_LINK_SUCCESS) {
        data.assign(reinterpret_cast<const char*>(packet.data), packet.length);
        XLinkDeallocateMoveData(packet.data, packet.length);
    }
    return rc;
}

// The read on an empty stream must time out. The event of this read is the
// one that stayed queued without the fix.
static bool timedOutRead(streamId_t s) {
    std::string data;
    XLinkError_t rc = readOnce(s, SHORT_TIMEOUT_MS, data);
    if(rc != X_LINK_TIMEOUT) {
        printf("client: expected X_LINK_TIMEOUT, got %s\n", XLinkErrorToStr(rc));
        return false;
    }
    printf("client: read timed out as expected\n");
    return true;
}

static int client(std::string path, bool lateData, bool closeLink) {
    XLinkHandler_t handler;
    if(!connect(path, handler)) {
        printf("client: XLinkConnect failed\n");
        return 1;
    }
    streamId_t s = XLinkOpenStream(handler.linkId, STREAM_NAME, STREAM_SIZE);
    if(s == INVALID_STREAM_ID) {
        printf("client: XLinkOpenStream failed\n");
        return 1;
    }

    if(!timedOutRead(s)) return 1;

    int result = 0;
    if(lateData) {
        writeRequested.set();
        if(!serverWrote.wait(milliseconds(LONG_TIMEOUT_MS))) {
            printf("client: server did not write\n");
            return 1;
        }
        // Let the dispatcher process the packet (without the fix it gives the
        // packet to the stale event and writes to the returned stack frame).
        std::this_thread::sleep_for(milliseconds(200));

        std::string data;
        XLinkError_t rc = readOnce(s, LONG_TIMEOUT_MS, data);
        if(rc != X_LINK_SUCCESS) {
            printf("client: read after timeout failed: %s\n", XLinkErrorToStr(rc));
            result = 1;
        } else if(data != std::string(PAYLOAD, sizeof(PAYLOAD))) {
            printf("client: read after timeout returned wrong data\n");
            result = 1;
        } else {
            printf("client: read after timeout returned the late packet\n");
        }
    }

    if(closeLink) {
        // Like depthai-core XLinkConnection::close() after a missed ping
        XLinkError_t rc = XLinkResetRemoteTimeout(handler.linkId, 1000);
        printf("client: XLinkResetRemoteTimeout: %s\n", XLinkErrorToStr(rc));
        // Give the dispatcher threads time to free the queued events
        std::this_thread::sleep_for(milliseconds(500));
    }
    return result;
}

static int runInProcess(const std::string& path, bool lateData, bool closeLink) {
    if(!initialize()) {
        printf("XLinkInitialize failed\n");
        return 1;
    }
    std::thread ts(server, path, lateData);
    serverReady.wait(milliseconds(LONG_TIMEOUT_MS));
    int rc = client(path, lateData, closeLink);
    clientDone.set();
    ts.join();
    if(!serverOk) rc = 1;
    return rc;
}

#if !defined(_WIN32)
static int runPeerKilled(const std::string& path) {
    pid_t child = fork();
    if(child < 0) {
        perror("fork");
        return 1;
    }
    if(child == 0) {
        // Server process: serve until killed
        if(!initialize()) _exit(1);
        server(path, false);
        _exit(0);
    }

    if(!initialize()) {
        printf("XLinkInitialize failed\n");
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return 1;
    }
    XLinkHandler_t handler;
    if(!connect(path, handler)) {
        printf("client: XLinkConnect failed\n");
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return 1;
    }
    streamId_t s = XLinkOpenStream(handler.linkId, STREAM_NAME, STREAM_SIZE);
    if(s == INVALID_STREAM_ID) {
        printf("client: XLinkOpenStream failed\n");
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        return 1;
    }
    bool ok = timedOutRead(s);

    // The "device" crashes: the socket of the peer closes, the eventReader
    // thread gets an error and frees the queued local events.
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    printf("client: server process killed\n");
    std::this_thread::sleep_for(milliseconds(200));

    // Close the link, like depthai-core does after a missed ping
    XLinkError_t rc = XLinkResetRemoteTimeout(handler.linkId, 1000);
    printf("client: XLinkResetRemoteTimeout: %s\n", XLinkErrorToStr(rc));
    std::this_thread::sleep_for(milliseconds(200));
    return ok ? 0 : 1;
}
#endif

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::string mode = argc > 1 ? argv[1] : "late-data";
    std::string path = argc > 2 ? argv[2] : "127.0.0.1:11500";

    int rc = 1;
    if(mode == "late-data") {
        rc = runInProcess(path, true, true);
    } else if(mode == "link-down") {
        rc = runInProcess(path, false, true);
#if !defined(_WIN32)
    } else if(mode == "peer-killed") {
        rc = runPeerKilled(path);
#endif
    } else {
        printf("unknown mode '%s'\n", mode.c_str());
        return 2;
    }
    printf("%s: %s\n", mode.c_str(), rc == 0 ? "PASS" : "FAIL");
    return rc;
}
