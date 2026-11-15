#include "usb_host.h"

#include <fcntl.h>
#include <linux/aio_abi.h>
#include <linux/dma-buf.h>
#include <linux/sync_file.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
constexpr int inputFd = 10001;
constexpr int outputFd = 10002;
constexpr int dmaFd = 10003;
constexpr int fenceFd = 10004;
struct Transfer { int32_t fd; uint32_t flags; uint64_t length; };
constexpr unsigned long attach = _IOW('g', 131, int);
constexpr unsigned long detach = _IOW('g', 132, int);
constexpr unsigned long transfer = _IOW('g', 133, Transfer);
const uint8_t metadata[] = {0xa1, 0xb2, 0xc3, 0xd4};

void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

enum class Failure { None, Attach, Transfer, Metadata, ExportFence, Fence, FenceQuery, ShortHeader, ShortTrailer, Poll, Submit, GetEvents, SecondaryResult };
struct Pending {
    bool completed = false;
};
struct Context {
    std::map<iocb*, Pending> pending;
    std::deque<io_event> events;
};
struct Input {
    std::vector<uint8_t> bytes;
    size_t offset = 0;
    explicit Input(std::vector<uint8_t> value) : bytes(std::move(value)) {}
};
struct State {
    Failure failure = Failure::None;
    unsigned packetSize = 1024;
    std::map<aio_context_t, Context> contexts;
    aio_context_t nextContext = 1;
    std::deque<Input> input;
    std::vector<uint8_t> wire;
    bool attached = false;
    bool submitted = false;
    bool completed = false;
    bool detached = false;
    bool polled = false;
    bool interrupted = false;
    bool faultInjected = false;
    bool fenceClosed = false;
    size_t writeLimit = 0;
    size_t shortAfter = 0;
    bool holdWrites = false;
    bool holdDma = false;
    bool holdCancelled = false;
    bool cancellationFails = false;
    bool dequeueFails = false;
    unsigned cancellations = 0;
    unsigned endpointCloses = 0;
    unsigned opens = 0, failOpenAt = 0;
    unsigned setups = 0, failSetupAt = 0;
    unsigned descriptorCalls = 0;
    int descriptorError = 0;
} state;
bool legacyResult = false;
std::mutex kernelMutex;
std::condition_variable kernelChanged;

void complete(Context& context, iocb* request, int64_t result) {
    auto& pending = context.pending.at(request);
    require(!pending.completed, "Kernel completed an operation twice");
    pending.completed = true;
    if(result > 0 && request->aio_lio_opcode == IOCB_CMD_PWRITE) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(request->aio_buf);
        state.wire.insert(state.wire.end(), bytes, bytes + result);
    }
    io_event event{};
    event.data = request->aio_data;
    event.obj = reinterpret_cast<uintptr_t>(request);
    event.res = result;
    event.res2 = state.failure == Failure::SecondaryResult ? 17 : legacyResult ? result : 0;
    context.events.push_back(event);
    kernelChanged.notify_all();
}

void receive(Context& context, iocb* request) {
    const size_t requested = request->aio_nbytes;
    const size_t rounded = ((requested + state.packetSize - 1) / state.packetSize) * state.packetSize;
    size_t available = 0;
    bool shortPacket = false;
    for(const auto& input : state.input) {
        available += input.bytes.size() - input.offset;
        if(input.bytes.size() % state.packetSize != 0) { shortPacket = true; break; }
        if(available >= rounded) break;
    }
    if(available < rounded && !shortPacket) return;
    const size_t consumed = std::min(rounded, available);
    auto* output = reinterpret_cast<uint8_t*>(request->aio_buf);
    size_t copied = 0;
    for(size_t remaining = consumed; remaining != 0;) {
        auto& input = state.input.front();
        const size_t amount = std::min(remaining, input.bytes.size() - input.offset);
        const size_t keep = std::min(amount, requested - copied);
        std::copy_n(input.bytes.data() + input.offset, keep, output + copied);
        copied += keep;
        input.offset += amount;
        remaining -= amount;
        if(input.offset == input.bytes.size()) state.input.pop_front();
    }
    complete(context, request, copied);
}

void finishPending(bool finishDma = true) {
    std::lock_guard<std::mutex> lock(kernelMutex);
    if(finishDma) { state.completed = true; state.holdDma = false; }
    for(auto& context : state.contexts)
        for(auto& pending : context.second.pending)
            if(!pending.second.completed) complete(context.second, pending.first, -ECANCELED);
    kernelChanged.notify_all();
}

template<class Predicate>
void waitFor(Predicate predicate, const char* failure) {
    std::unique_lock<std::mutex> lock(kernelMutex);
    require(kernelChanged.wait_for(lock, std::chrono::seconds(2), predicate), failure);
}

void resetState(unsigned packetSize = 1024) {
    std::lock_guard<std::mutex> lock(kernelMutex);
    require(state.contexts.empty(), "Previous connection leaked its AIO contexts");
    state = State{};
    state.packetSize = packetSize;
}

void* openServer() {
    void* key = nullptr;
    require(usbPlatformServer(nullptr, nullptr, &key) == 0 && key, "Cannot open simulated FunctionFS endpoints");
    require(usbPlatformSupportsDmaBuf(key), "Server endpoint does not support DMA-BUF");
    require(!usbPlatformSupportsDmaBuf(nullptr), "Invalid endpoint supports DMA-BUF");
    return key;
}

xLinkEvent_t frame(void* key) {
    xLinkEvent_t event{};
    event.header.type = XLINK_WRITE_REQ;
    event.header.size = 32 + sizeof(metadata);
    event.data = reinterpret_cast<void*>(dmaFd);
    event.data2 = const_cast<uint8_t*>(metadata);
    event.data2Size = sizeof(metadata);
    event.deviceHandle.protocol = X_LINK_USB_EP;
    event.deviceHandle.xLinkFD = key;
    event.dmaBuf = true;
    return event;
}

void checkDma(Failure failure) {
    resetState();
    state.failure = failure;
    state.holdDma = failure == Failure::Poll;
    void* key = openServer();
    auto event = frame(key);
    const auto result = usbPlatformWriteDmaBuf(&event);
    const bool succeeds = failure == Failure::None || failure == Failure::Submit || failure == Failure::GetEvents;
    require((result == 0) == succeeds, "Incorrect DMA transfer result");
    require(state.detached == state.attached, "Attachment leaked or unattached buffer detached");
    if(state.submitted) {
        require(state.completed, "Transfer returned before DMA completed");
        require(state.fenceClosed == (failure != Failure::ExportFence), "Exported fence leaked");
    }
    if(succeeds) {
        require(state.wire.size() == sizeof(event.header) + event.header.size, "Incorrect wire length");
        require(std::memcmp(state.wire.data(), &event.header, sizeof(event.header)) == 0, "Wire header changed");
        for(size_t i = 0; i < 32; ++i)
            require(state.wire[sizeof(event.header) + i] == i, "DMA payload changed or was copied through write");
        require(std::memcmp(state.wire.data() + sizeof(event.header) + 32, metadata, sizeof(metadata)) == 0, "Trailer is out of order");
    }
    usbPlatformClose(X_LINK_USB_EP, key);
    usbPlatformClose(X_LINK_USB_EP, key);
    require(!usbPlatformSupportsDmaBuf(key), "Closed endpoint still supports DMA-BUF");
    require(state.contexts.empty() && state.endpointCloses == 2, "Repeated close leaked or closed an endpoint twice");
}

std::vector<uint8_t> pattern(size_t size) {
    std::vector<uint8_t> bytes(size);
    for(size_t i = 0; i < size; ++i) bytes[i] = (i * 37 + 19) & 255;
    return bytes;
}

void checkLargeTrailer() {
    resetState();
    void* key = openServer();
    auto trailer = pattern(15 * 1024 * 1024 + 127);
    auto event = frame(key);
    event.header.size = 32 + trailer.size();
    event.data2 = trailer.data();
    event.data2Size = trailer.size();
    state.writeLimit = 3;
    state.shortAfter = sizeof(event.header) + 32 + 15 * 1024 * 1024;
    require(usbPlatformWriteDmaBuf(&event) == 0, "Large DMA trailer failed");
    require(state.wire.size() == sizeof(event.header) + event.header.size, "Large trailer has incorrect wire length");
    require(std::memcmp(state.wire.data(), &event.header, sizeof(event.header)) == 0, "Large trailer changed the header");
    for(size_t i = 0; i < 32; ++i) require(state.wire[sizeof(event.header) + i] == i, "Large trailer changed the DMA payload");
    require(std::equal(trailer.begin(), trailer.end(), state.wire.begin() + sizeof(event.header) + 32), "Large trailer chunks lost or reordered bytes");
    usbPlatformClose(X_LINK_USB_EP, key);
}

void checkShortWrites() {
    resetState();
    state.writeLimit = 3;
    void* key = openServer();
    auto bytes = pattern(37);
    require(usbPlatformWrite(X_LINK_USB_EP, key, bytes.data(), bytes.size()) == static_cast<int>(bytes.size()), "Short write did not finish the caller span");
    require(state.wire == bytes, "Positive short writes lost or duplicated bytes");
    usbPlatformClose(X_LINK_USB_EP, key);
}

void enqueue(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(kernelMutex);
    state.input.emplace_back(bytes);
    for(auto& context : state.contexts)
        for(auto& pending : context.second.pending)
            if(!pending.second.completed && pending.first->aio_lio_opcode == IOCB_CMD_PREAD)
                receive(context.second, pending.first);
}

std::vector<uint8_t> readBytes(void* key, size_t size) {
    std::vector<uint8_t> bytes(size, 0xee);
    auto result = std::async(std::launch::async, [&] { return usbPlatformRead(X_LINK_USB_EP, key, bytes.data(), bytes.size()); });
    const bool ready = result.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    if(!ready) {
        auto close = std::async(std::launch::async, [&] { return usbPlatformClose(X_LINK_USB_EP, key); });
        finishPending();
        close.get();
    }
    require(result.get() == static_cast<int>(size) && ready, "Read lost bytes or waited for a nonexistent packet/ZLP");
    return bytes;
}

void checkReads(unsigned packetSize) {
    resetState(packetSize);
    void* key = openServer();
    auto expected = pattern(packetSize);
    enqueue(expected);
    require(readBytes(key, packetSize) == expected, "Exact packet read changed bytes");
    enqueue(expected);
    auto first = readBytes(key, 5);
    auto tail = readBytes(key, packetSize - 5);
    first.insert(first.end(), tail.begin(), tail.end());
    require(first == expected, "Small read discarded the rest of a USB packet");
    expected = pattern(packetSize * 3);
    enqueue(expected);
    first = readBytes(key, packetSize * 2 + 17);
    tail = readBytes(key, packetSize - 17);
    first.insert(first.end(), tail.begin(), tail.end());
    require(first == expected, "Aligned prefix and carried suffix changed byte order");
    enqueue({1, 2, 3});
    enqueue({4, 5, 6, 7});
    require(readBytes(key, 7) == std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7}), "Repeated short transfers lost bytes");
    enqueue(pattern(packetSize));
    require(readBytes(key, 1) == std::vector<uint8_t>({19}), "Carry setup failed");
    usbPlatformClose(X_LINK_USB_EP, key);
    void* replacement = openServer();
    require(replacement != key, "New connection reused a stale key");
    enqueue(std::vector<uint8_t>(packetSize, 0x42));
    uint8_t stale = 0;
    require(usbPlatformRead(X_LINK_USB_EP, key, &stale, 1) < 0, "Stale read key reached a replacement connection");
    require(readBytes(replacement, packetSize) == std::vector<uint8_t>(packetSize, 0x42), "Reconnect leaked old carry bytes");
    usbPlatformClose(X_LINK_USB_EP, replacement);
}

void checkOpenFailures() {
    for(unsigned failure = 1; failure <= 4; ++failure) {
        resetState();
        if(failure <= 2) state.failOpenAt = failure;
        else state.failSetupAt = failure - 2;
        void* key = nullptr;
        require(usbPlatformServer(nullptr, nullptr, &key) < 0, "Failed endpoint/context creation returned a connection");
        require(state.contexts.empty() && state.endpointCloses == (failure <= 2 ? 1u : 2u), "Failed open leaked kernel resources");
        state.failOpenAt = state.failSetupAt = 0;
        key = openServer();
        auto event = frame(key);
        require(usbPlatformWriteDmaBuf(&event) == 0, "Cannot transfer after failed connection creation");
        usbPlatformClose(X_LINK_USB_EP, key);
    }
}

void checkDescriptorCancellation(int error) {
    resetState();
    state.descriptorError = error;
    void* key = openServer();
    uint8_t byte = 0x6d;
    auto read = std::async(std::launch::async, [&] { return usbPlatformRead(X_LINK_USB_EP, key, &byte, 1); });
    waitFor([] { return state.descriptorCalls != 0; }, "No endpoint descriptor query");
    usbPlatformClose(X_LINK_USB_EP, key);
    require(read.get() < 0 && byte == 0x6d, "Close did not stop an unconfigured endpoint read");
    require(state.contexts.empty() && state.endpointCloses == 2, "Descriptor admission leaked the connection");
}

void checkCancellation(bool input, bool failCancel) {
    resetState();
    state.holdWrites = true;
    state.holdCancelled = true;
    state.cancellationFails = failCancel;
    void* key = openServer();
    std::vector<uint8_t> bytes(1024, 0x6d);
    auto io = std::async(std::launch::async, [&] {
        return input ? usbPlatformRead(X_LINK_USB_EP, key, bytes.data(), bytes.size())
                     : usbPlatformWrite(X_LINK_USB_EP, key, bytes.data(), bytes.size());
    });
    waitFor([] { for(const auto& c : state.contexts) if(!c.second.pending.empty()) return true; return false; }, "No pending I/O to cancel");
    auto close = std::async(std::launch::async, [&] { return usbPlatformClose(X_LINK_USB_EP, key); });
    waitFor([] { return state.cancellations != 0; }, "Close did not cancel pending I/O");
    const bool heldIo = io.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout;
    const bool heldClose = close.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout;
    unsigned closed;
    { std::lock_guard<std::mutex> lock(kernelMutex); closed = state.endpointCloses; }
    finishPending();
    const int result = io.get();
    close.get();
    require(heldIo && heldClose && closed == 0, "Cancellation released caller storage or descriptors before completion");
    require(result < 0, "Cancelled I/O reported success");
    require(bytes == std::vector<uint8_t>(1024, 0x6d), "Cancelled read corrupted destination");
    require(state.contexts.empty() && state.endpointCloses == 2, "Cancelled connection did not drain and close");
}

void checkDmaCancellation(bool failDequeue) {
    resetState();
    state.holdDma = true;
    state.holdWrites = true;
    state.holdCancelled = true;
    state.dequeueFails = failDequeue;
    if(failDequeue) state.failure = Failure::Poll;
    void* key = openServer();
    auto event = frame(key);
    auto io = std::async(std::launch::async, [&] { return usbPlatformWriteDmaBuf(&event); });
    waitFor([] { return state.submitted; }, "No DMA transfer queued");
    auto close = std::async(std::launch::async, [&] { return usbPlatformClose(X_LINK_USB_EP, key); });
    waitFor([] { return state.detached; }, "Close did not request DMA cancellation");
    void* refused = nullptr;
    const int reopened = usbPlatformServer(nullptr, nullptr, &refused);
    const bool heldIo = io.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout;
    const bool heldClose = close.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout;
    unsigned closed;
    { std::lock_guard<std::mutex> lock(kernelMutex); closed = state.endpointCloses; }
    finishPending(false);
    const bool heldDma = !failDequeue || (io.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout
                                        && close.wait_for(std::chrono::milliseconds(80)) == std::future_status::timeout);
    finishPending();
    const int result = io.get();
    close.get();
    require(reopened < 0 && !refused, "Reopened endpoints while previous connection was stopping");
    require(heldDma, "Failed DMA dequeue released the frame after only metadata completion");
    require(heldIo && heldClose && closed == 0, "DMA cancellation released the frame before terminal evidence");
    require(result < 0, "Cancelled DMA reported success");
    require(state.fenceClosed && state.contexts.empty(), "Cancelled DMA leaked its fence or context");
}
}

extern "C" int __real_open(const char*, int, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    if(std::strcmp(path, "/dev/usb-ffs/device/ep1") == 0 || std::strcmp(path, "/dev/usb-ffs/device/ep2") == 0) {
        std::lock_guard<std::mutex> lock(kernelMutex);
        if(++state.opens == state.failOpenAt) { errno = ENODEV; return -1; }
        require((flags & O_NONBLOCK) != 0, "AIO submission may block on an unconfigured endpoint");
        return path[std::strlen(path) - 1] == '1' ? outputFd : inputFd;
    }
    mode_t mode = 0;
    if(flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    return __real_open(path, flags, mode);
}

extern "C" long __wrap_syscall(long number, ...) {
    std::unique_lock<std::mutex> lock(kernelMutex);
    va_list args;
    va_start(args, number);
    long result = -1;
    if(number == SYS_io_setup) {
        (void)va_arg(args, unsigned);
        auto* context = va_arg(args, aio_context_t*);
        if(++state.setups == state.failSetupAt) errno = ENOMEM;
        else {
            *context = state.nextContext++;
            state.contexts.emplace(*context, Context{});
            result = 0;
        }
    } else if(number == SYS_io_submit) {
        auto& context = state.contexts.at(va_arg(args, aio_context_t));
        const long count = va_arg(args, long);
        auto** requests = va_arg(args, iocb**);
        require(count == 1, "Unexpected batched AIO submission");
        auto* request = requests[0];
        if(state.failure == Failure::Submit && !state.faultInjected) {
            state.faultInjected = true;
            errno = EINTR;
        } else if(state.failure == Failure::Metadata && state.submitted && request->aio_lio_opcode == IOCB_CMD_PWRITE) {
            errno = ESHUTDOWN;
        } else {
            require(context.pending.emplace(request, Pending{}).second, "Reused an I/O slot before draining its completion");
            if(request->aio_lio_opcode == IOCB_CMD_PREAD) {
                require(request->aio_fildes == inputFd, "Read reached the wrong endpoint");
                receive(context, request);
            } else {
                require(request->aio_lio_opcode == IOCB_CMD_PWRITE && request->aio_fildes == outputFd, "Write reached the wrong endpoint");
                if(!state.holdWrites) {
                    const bool shortNative = (state.failure == Failure::ShortHeader && !state.submitted)
                                             || (state.failure == Failure::ShortTrailer && state.submitted);
                    const auto count = state.writeLimit && state.wire.size() >= state.shortAfter
                                           ? std::min(request->aio_nbytes, static_cast<decltype(request->aio_nbytes)>(state.writeLimit))
                                           : request->aio_nbytes - (shortNative ? 1 : 0);
                    complete(context, request, count);
                }
            }
            result = 1;
            kernelChanged.notify_all();
        }
    } else if(number == SYS_io_getevents) {
        auto& context = state.contexts.at(va_arg(args, aio_context_t));
        const long minimum = va_arg(args, long);
        const long maximum = va_arg(args, long);
        auto* events = va_arg(args, io_event*);
        (void)va_arg(args, timespec*);
        if(state.failure == Failure::GetEvents && !state.faultInjected) {
            state.faultInjected = true;
            errno = EINTR;
        } else {
            kernelChanged.wait(lock, [&] { return context.events.size() >= static_cast<size_t>(minimum); });
            result = 0;
            while(!context.events.empty() && result < maximum) {
                events[result] = context.events.front();
                context.events.pop_front();
                context.pending.erase(reinterpret_cast<iocb*>(events[result].obj));
                ++result;
            }
        }
    } else if(number == SYS_io_cancel) {
        auto& context = state.contexts.at(va_arg(args, aio_context_t));
        auto* request = va_arg(args, iocb*);
        (void)va_arg(args, io_event*);
        ++state.cancellations;
        auto pending = context.pending.find(request);
        if(pending == context.pending.end() || pending->second.completed) errno = EINVAL;
        else if(state.cancellationFails) errno = EIO;
        else {
            if(!state.holdCancelled) complete(context, request, -ECANCELED);
            errno = EINPROGRESS;
        }
        kernelChanged.notify_all();
    } else if(number == SYS_io_destroy) {
        const auto context = va_arg(args, aio_context_t);
        require(state.contexts.at(context).pending.empty(), "Destroyed a context with unconsumed kernel completions");
        state.contexts.erase(context);
        result = 0;
    } else {
        throw std::runtime_error("Unexpected direct syscall " + std::to_string(number));
    }
    va_end(args);
    return result;
}

extern "C" int __wrap_ioctl(int fd, unsigned long command, ...) {
    std::lock_guard<std::mutex> lock(kernelMutex);
    va_list args;
    va_start(args, command);
    void* argument = va_arg(args, void*);
    va_end(args);
    if(command == FUNCTIONFS_ENDPOINT_DESC) {
        require(fd == inputFd, "Requested packet size from wrong endpoint");
        ++state.descriptorCalls;
        kernelChanged.notify_all();
        if(state.descriptorError) { errno = state.descriptorError; return -1; }
        auto& descriptor = *static_cast<usb_endpoint_descriptor*>(argument);
        descriptor = usb_endpoint_descriptor{};
        descriptor.bLength = sizeof(descriptor);
        descriptor.bDescriptorType = USB_DT_ENDPOINT;
        descriptor.bEndpointAddress = USB_DIR_OUT | 2;
        descriptor.bmAttributes = USB_ENDPOINT_XFER_BULK;
        descriptor.wMaxPacketSize = state.packetSize;
    } else if(command == attach) {
        require(fd == outputFd && *static_cast<int*>(argument) == dmaFd, "Wrong DMA attachment");
        if(!state.interrupted) { state.interrupted = true; errno = EINTR; return -1; }
        if(state.failure == Failure::Attach) { errno = ENOTTY; return -1; }
        state.attached = true;
    } else if(command == transfer) {
        const auto& request = *static_cast<Transfer*>(argument);
        require(fd == outputFd && request.fd == dmaFd && request.flags == 0 && request.length == 32, "Wrong DMA payload range");
        if(state.failure == Failure::Transfer) { errno = ESHUTDOWN; return -1; }
        state.submitted = true;
        state.completed = !state.holdDma;
        if(!state.holdDma) for(uint8_t i = 0; i < 32; ++i) state.wire.push_back(i);
        kernelChanged.notify_all();
    } else if(command == DMA_BUF_IOCTL_EXPORT_SYNC_FILE) {
        auto& sync = *static_cast<dma_buf_export_sync_file*>(argument);
        require(fd == dmaFd && sync.flags == DMA_BUF_SYNC_WRITE, "Fence does not include device readers");
        if(state.failure == Failure::ExportFence) { errno = ENOMEM; return -1; }
        sync.fd = fenceFd;
    } else if(command == SYNC_IOC_FILE_INFO) {
        require(fd == fenceFd, "Wrong completion fence queried");
        if(state.failure == Failure::FenceQuery) { errno = EIO; return -1; }
        static_cast<sync_file_info*>(argument)->status = !state.completed ? 0 : state.failure == Failure::Fence ? -ESHUTDOWN : 1;
    } else if(command == detach) {
        require(fd == outputFd && *static_cast<int*>(argument) == dmaFd, "Wrong DMA detachment");
        require(state.attached && !state.detached, "Attachment detached twice");
        state.detached = true;
        if(!state.dequeueFails) state.completed = true;
        kernelChanged.notify_all();
    } else {
        throw std::runtime_error("Unexpected ioctl " + std::to_string(command));
    }
    return 0;
}

extern "C" int __wrap_poll(pollfd* fds, nfds_t count, int timeout) {
    std::unique_lock<std::mutex> lock(kernelMutex);
    require(count == 1 && state.submitted, "Invalid DMA completion wait");
    const bool fallback = state.failure == Failure::ExportFence || state.failure == Failure::FenceQuery || (state.failure == Failure::Poll && state.faultInjected);
    require(fds[0].fd == (fallback ? dmaFd : fenceFd) && fds[0].events == (fallback ? POLLOUT : POLLIN), "Wrong DMA completion fence");
    state.polled = true;
    if(state.failure == Failure::Poll && !state.faultInjected) {
        state.faultInjected = true;
        errno = EIO;
        return -1;
    }
    if(timeout < 0) kernelChanged.wait(lock, [] { return state.completed; });
    else if(!kernelChanged.wait_for(lock, std::chrono::milliseconds(timeout), [] { return state.completed; })) return 0;
    fds[0].revents = fds[0].events;
    return 1;
}

extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
    std::lock_guard<std::mutex> lock(kernelMutex);
    if(fd == fenceFd) {
        require(state.completed, "Fence closed before DMA completion");
        require(!state.fenceClosed, "Fence closed twice");
        state.fenceClosed = true;
        return 0;
    }
    if(fd == inputFd || fd == outputFd) {
        for(const auto& context : state.contexts) require(context.second.pending.empty(), "Endpoint closed while kernel owns caller memory");
        require(!state.submitted || state.completed, "Endpoint closed while DMA is active");
        ++state.endpointCloses;
        return 0;
    }
    return __real_close(fd);
}

int main() {
    try {
        for(bool legacy : {false, true}) {
            legacyResult = legacy;
            for(auto failure : {Failure::None, Failure::Attach, Failure::Transfer, Failure::Metadata, Failure::ExportFence,
                                Failure::Fence, Failure::FenceQuery, Failure::ShortHeader, Failure::ShortTrailer, Failure::Poll, Failure::Submit, Failure::GetEvents, Failure::SecondaryResult}) checkDma(failure);
            checkShortWrites();
            checkLargeTrailer();
            for(auto packetSize : {64u, 512u, 1024u}) checkReads(packetSize);
            checkOpenFailures();
            for(int error : {EAGAIN, ESHUTDOWN}) checkDescriptorCancellation(error);
            for(bool input : {false, true}) for(bool failCancel : {false, true}) checkCancellation(input, failCancel);
            for(bool failDequeue : {false, true}) checkDmaCancellation(failDequeue);
        }
        std::cout << "USB DMA ordering, cancelled I/O lifetime and packet boundary reads passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
