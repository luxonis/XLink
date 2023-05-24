#ifndef _WRAP_LIBUSB_HPP_
#define _WRAP_LIBUSB_HPP_

// project
#define MVLOG_UNIT_NAME xLinkUsb

// libraries
#ifdef XLINK_LIBUSB_LOCAL
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include "wrap_libusb_details.hpp"

namespace dai {

///////////////////////////////
// Helper functions and macros
///////////////////////////////

// base implementation wrapper
template<typename Resource, void(*Dispose)(Resource*)>
struct unique_resource_deleter {
    inline void operator()(Resource* const ptr) noexcept {
        if (ptr != nullptr)
            Dispose(ptr);
    }
};
template<typename Resource, void(*Dispose)(Resource*)>
using unique_resource_ptr = std::unique_ptr<Resource, unique_resource_deleter<Resource, Dispose>>;


namespace libusb {

///////////////////////////////////
// libusb error exception wrappers
///////////////////////////////////

// exception error class for libusb errors
class usb_error : public std::system_error {
public:
    explicit usb_error(int libusbErrorCode) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), libusb_strerror(libusbErrorCode)} {}
    usb_error(int libusbErrorCode, const std::string& what) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), what} {}
    usb_error(int libusbErrorCode, const char* what) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), what} {}
};

// exception error class for libusb transfer errors
class transfer_error : public usb_error {
public:
    explicit transfer_error(int libusbErrorCode, ptrdiff_t transferred) noexcept :
        usb_error{libusbErrorCode}, transferred{transferred} {}
    transfer_error(int libusbErrorCode, const std::string& what, ptrdiff_t transferred) noexcept :
        usb_error{libusbErrorCode, what}, transferred{transferred} {}
    transfer_error(int libusbErrorCode, const char* what, ptrdiff_t transferred) noexcept :
        usb_error{libusbErrorCode, what}, transferred{transferred} {}
private:
    ptrdiff_t transferred; // number of bytes transferred
};

// tag dispatch for throwing or not throwing exceptions
inline void throw_conditional_usb_error(int libusbErrorCode, std::true_type) noexcept(false) {
    throw usb_error(libusbErrorCode);
}
inline void throw_conditional_usb_error(int, std::false_type) noexcept(true) {
    // do nothing
}
inline void throw_conditional_transfer_error(int libusbErrorCode, ptrdiff_t transferred, std::true_type) noexcept(false) {
    throw transfer_error(libusbErrorCode, transferred);
}
inline void throw_conditional_transfer_error(int, ptrdiff_t, std::false_type) noexcept(true) {
    // do nothing
}

// template function that can call any libusb function passed to it
// function parameters are passed as variadic template arguments
// caution: when Throw=false, a negative return code can be returned on error; always handle such scenarios
template <mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, typename Func, typename... Args>
inline auto call_log_throw(const char* funcWithin, const int lineNumber, Func&& func, Args&&... args) noexcept(!Throw)
    -> decltype(func(std::forward<Args>(args)...)) {
    const auto rcNum = func(std::forward<Args>(args)...);
    if (rcNum < 0) {
        logprintf(MVLOGLEVEL(MVLOG_UNIT_NAME),
                  Loglevel,
                  funcWithin,
                  lineNumber,
                  "dai::libusb failed %s(): %s",
                  funcWithin,
                  libusb_strerror(static_cast<int>(rcNum)));
        throw_conditional_usb_error(static_cast<int>(rcNum), std::integral_constant<bool, Throw>{});
    }
    return rcNum;
}
#define CALL_LOG_ERROR_THROW(...) call_log_throw(__func__, __LINE__, __VA_ARGS__)

///////////////////////////////
// libusb resource wrappers
///////////////////////////////

// wraps libusb_context and automatically libusb_exit() on destruction
 using usb_context = unique_resource_ptr<libusb_context, libusb_exit>;

// wrap libusb_device* with RAII ref counting
class usb_device;

// device_list container class wrapper for libusb_get_device_list()
// Use constructors to create an instance as the primary approach.
// Use device_list::create() to create an instance via factory approach.
class device_list {
public:
    // default constructors, destructor, copy, move
    device_list() = default;
    ~device_list() noexcept {
        if (deviceList != nullptr) {
            libusb_free_device_list(deviceList, 1);
        }
    }
    device_list(const device_list&) = delete;
    device_list& operator=(const device_list&) = delete;
    device_list(device_list&& other) noexcept :
        countDevices{details::exchange(other.countDevices, 0)},
        deviceList{details::exchange(other.deviceList, nullptr)} {};
    device_list& operator=(device_list&& other) noexcept {
        if (this == &other)
            return *this;
        countDevices = details::exchange(other.countDevices, 0);
        deviceList = details::exchange(other.deviceList, nullptr);
        return *this;
    }

    explicit device_list(libusb_context* context) noexcept(false) {
        // libusb_get_device_list() is not thread safe!
        // multiple threads simultaneously generating device lists causes crashes and memory violations
        // within libusb itself due to incorrect libusb ref count handling, wrongly deleted devices, etc.
        // crashes occurred when libusb internally called libusb_ref_device(), XLink called libusb_unref_device(),
        // often when libusb called usbi_get_device_priv(dev) and then operated on the pointers
        // line in file libusb/os/windows_winusb.c in winusb_get_device_list() line 1741
        std::lock_guard<std::mutex> l(mtx);

        countDevices = static_cast<size_type>(CALL_LOG_ERROR_THROW(libusb_get_device_list, context, &deviceList));
    }

    // container interface
    // ideas from https://en.cppreference.com/w/cpp/named_req/SequenceContainer
    using value_type = libusb_device*;
    using allocator_type = std::allocator<value_type>;
    using size_type = allocator_type::size_type;
    using difference_type = allocator_type::difference_type;
    using reference = allocator_type::reference;
    using const_reference = allocator_type::const_reference;
    using pointer = allocator_type::pointer;
    using const_pointer = allocator_type::const_pointer;
    using iterator = pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_iterator = const_pointer;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;
    size_type size() const noexcept {
        return countDevices;
    }
    bool empty() const noexcept {
        return countDevices == 0;
    }
    pointer data() noexcept {
        return deviceList;
    }
    const_pointer data() const noexcept {
        return deviceList;
    }
    iterator begin() noexcept {
        return deviceList;
    }
    const_iterator begin() const noexcept {
        return deviceList;
    }
    const_iterator cbegin() const noexcept {
        return deviceList;
    }
    reverse_iterator rbegin() noexcept {
        return std::reverse_iterator<iterator>{end()};
    }
    const_reverse_iterator rbegin() const noexcept {
        return std::reverse_iterator<const_iterator>{cend()};
    }
    iterator end() noexcept {
        return begin() + countDevices;
    }
    const_iterator end() const noexcept {
        return cbegin() + countDevices;
    }
    const_iterator cend() const noexcept {
        return cbegin() + countDevices;
    }
    reverse_iterator rend() noexcept {
        return std::reverse_iterator<iterator>{begin()};
    }
    const_reverse_iterator rend() const noexcept {
        return std::reverse_iterator<const_iterator>{cbegin()};
    }
    reference front() noexcept {
        return *begin();
    }
    const_reference front() const noexcept {
        return *cbegin();
    }
    reference back() noexcept {
        return *(begin() + size() - 1);
    }
    const_reference back() const noexcept {
        return *(cbegin() + size() - 1);
    }
    reference operator[](size_type index) noexcept {
        return *(begin() + index);
    }
    const_reference operator[](size_type index) const noexcept {
        return *(cbegin() + index);
    }
    reference at(size_type index) {
        if (index >= size()) {
            throw std::out_of_range("device_list::at");
        }
        return *(begin() + index);
    }
    const_reference at(size_type index) const {
        if (index >= size()) {
            throw std::out_of_range("device_list::at");
        }
        return *(cbegin() + index);
    }

private:
    static std::mutex mtx;
    size_type countDevices{};
    pointer deviceList{};
};

// TODO move to dedicated cpp file to avoid multiple definitions
std::mutex device_list::mtx;

// wraps libusb_config_descriptor* and automatically libusb_free_config_descriptor() on destruction
class config_descriptor : public unique_resource_ptr<libusb_config_descriptor, libusb_free_config_descriptor> {
public:
    using unique_resource_ptr<libusb_config_descriptor, libusb_free_config_descriptor>::unique_resource_ptr;

    config_descriptor(libusb_device* dev, uint8_t configIndex) noexcept(false) {
        CALL_LOG_ERROR_THROW(libusb_get_config_descriptor, dev, configIndex, dai::out_param(*this));
    }
};

// wrap libusb_device_handle* to allow I/O on device. Create with usb_device::open(), device_handle{libusb_device*}
// or from raw platform pointers with device_handle{libusb_context*, intptr_t}
class device_handle : public unique_resource_ptr<libusb_device_handle, libusb_close> {
private:
    using _base = unique_resource_ptr<libusb_device_handle, libusb_close>;

    static constexpr decltype(libusb_endpoint_descriptor::wMaxPacketSize) DEFAULT_MAX_PACKET_SIZE = 512;

    std::array<decltype(libusb_endpoint_descriptor::wMaxPacketSize), 32> maxPacketSize{
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
    };
    std::vector<int> claimedInterfaces{};

    // endpoint bit 7 = 0 is OUT host to device, bit 7 = 1 is IN device to host (Ignored for Control Endpoints)
    static bool is_direction_in(uint8_t endpoint) noexcept {
        // (endpoint >> 7) & 0x01
        return static_cast<bool>(endpoint & 0x80);
    }

public:
    using unique_resource_ptr<libusb_device_handle, libusb_close>::unique_resource_ptr;

    device_handle(libusb_device* dev) noexcept(false) {
        CALL_LOG_ERROR_THROW(libusb_open, dev, dai::out_param(*static_cast<_base*>(this)));
    }

    // wrap a platform-specific system device handle and get a libusb device_handle for it
    // never use libusb_open() on this wrapped handle's underlying device
    device_handle(libusb_context* ctx, intptr_t sysDevHandle) noexcept(false) {
        CALL_LOG_ERROR_THROW(libusb_wrap_sys_device, ctx, sysDevHandle, dai::out_param(*static_cast<_base*>(this)));
    }

    // copy and move constructors and assignment operators
    device_handle(const device_handle&) = delete;
    device_handle& operator=(const device_handle&) = delete;
    device_handle(device_handle &&other) noexcept :
        _base{details::exchange<_base, _base>(other, {})},
        claimedInterfaces{details::exchange(other.claimedInterfaces, {})}
    {}
    device_handle &operator=(device_handle &&other) noexcept {
        if (this != &other) {
            *static_cast<_base*>(this) = details::exchange<_base, _base>(other, {});
            claimedInterfaces = details::exchange(other.claimedInterfaces, {});
        }
        return *this;
    }
    ~device_handle() noexcept {
        reset();
    }

    // release all managed objects with libusb_release_interface() and libusb_close()
    // No exceptions are thrown. Errors are logged.
    void reset(pointer ptr = pointer{}) noexcept {
        for (const auto interfaceNumber : claimedInterfaces) {
            call_log_throw<MVLOG_ERROR, false>(__func__, __LINE__, libusb_release_interface, get(), interfaceNumber);
        }
        static_cast<_base*>(this)->reset(ptr);
    }

    // release ownership of the managed libusb_device_handle and all device interfaces
    // caller is responsible for calling libusb_release_interface() and libusb_close()
    device_handle::pointer release() noexcept {
        claimedInterfaces.clear();
        return static_cast<_base*>(this)->release();
    }

    // wrap libusb_get_device() and return a ref counted usb_device
    usb_device get_device() const noexcept;

    // wrapper for libusb_claim_interface()
    void claim_interface(int interfaceNumber) noexcept(false) {
        if(std::find(claimedInterfaces.begin(), claimedInterfaces.end(), interfaceNumber) != claimedInterfaces.end()) return;
        CALL_LOG_ERROR_THROW(libusb_claim_interface, get(), interfaceNumber);
        claimedInterfaces.emplace_back(interfaceNumber);
    }

    // wrapper for libusb_release_interface()
    void release_interface(int interfaceNumber) noexcept(false) {
        auto candidate = std::find(claimedInterfaces.begin(), claimedInterfaces.end(), interfaceNumber);
        if (candidate == claimedInterfaces.end())
            return;
        CALL_LOG_ERROR_THROW(libusb_release_interface, get(), interfaceNumber);
        claimedInterfaces.erase(candidate);
    }

    // wrapper for libusb_get_configuration()
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    int get_configuration() const noexcept(!Throw) {
        int configuration{};
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_configuration, get(), &configuration);
        return configuration;
    }

    // wrapper for libusb_set_configuration()
    // if skip_active_check = true, the current configuration is not checked before setting the new one
    template <mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, bool Force = false>
    void set_configuration(int configuration) noexcept(!Throw) {
        if(!Force) {
            const auto active = get_configuration<Loglevel, Throw>();
            if(active == configuration)
                return;
            mvLog(MVLOG_DEBUG, "Setting configuration from %d to %d", active, configuration);
        }
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_set_configuration, get(), configuration);
    }

    // wrapper for libusb_get_string_descriptor_ascii()
    // template param Len is the maximum possible number of chars (without null terminator) read from the descriptor
    // the return string is resized to the actual number of chars read
    template <mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, int Len = 31>
    std::string get_string_descriptor_ascii(uint8_t descriptorIndex) const noexcept(!Throw) {
        std::string descriptor(Len + 1, 0);
        const auto result = call_log_throw<Loglevel, Throw>(
            __func__, __LINE__, libusb_get_string_descriptor_ascii, get(), descriptorIndex, (unsigned char*)descriptor.data(), Len + 1);
        if (Throw || result >= 0) {
            // when throwing enabled, then throw occurs on negative/error results before this resize(),
            // so don't need to check result and compiler can optimize this away.
            // when throwing disabled, then always prevent resize to negative/error code result
            descriptor.resize(result);
        }
        return descriptor;
    }

    // wrapper for libusb_set_auto_detach_kernel_driver()
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    void set_auto_detach_kernel_driver(bool enable) noexcept(!Throw) {
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_set_auto_detach_kernel_driver, get(), enable);
    }

    void set_max_packet_size(uint8_t endpoint, decltype(libusb_endpoint_descriptor::wMaxPacketSize) size) noexcept {
        // keep endpoint bits 0-3, then move bit 7 to bit 4
        // this creates a 0-31 number representing all possible endpoint addresses
        maxPacketSize[(endpoint & 0x0F) | ((endpoint & 0x80) >> 3)] = size;
    }

    decltype(libusb_endpoint_descriptor::wMaxPacketSize) get_max_packet_size(uint8_t endpoint) const noexcept {
        // keep endpoint bits 0-3, then move bit 7 to bit 4
        // this creates a 0-31 number representing all possible endpoint addresses
        return maxPacketSize[(endpoint & 0x0F) | ((endpoint & 0x80) >> 3)];
    }

    // wrapper for libusb_get_device_descriptor(libusb_get_device())
    // faster than calling get_device().get_device_descriptor()
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_device_descriptor get_device_descriptor() const noexcept(!Throw) {
        libusb_device_descriptor descriptor{};
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_device_descriptor, libusb_get_device(get()), &descriptor);
        return descriptor;
    }

    //template <int ChunkTimeoutMs, int TimeoutMs>
    //static constexpr unsigned int calculate_chunk_timeout() {
    //    return (TimeoutMs == 0) ? ChunkTimeoutMs : std::min(ChunkTimeoutMs, TimeoutMs);
    //}

    // wrapper for libusb_bulk_transfer(); returns std::pair with error code and number of bytes actually transferred
    // Transfers on IN endpoints will continue until the requested number of bytes has been transferred
    // TODO should a short packet on IN endpoint indicate the device is finished and this function return?
    template <mvLog_t Loglevel = MVLOG_ERROR,
              bool Throw = true,
              unsigned int ChunkTimeoutMs = 0 /* unlimited */,
              bool ZeroLengthPacketEnding = false,
              unsigned int TimeoutMs = 0 /* unlimited */,
              typename BufferValueType>
    std::pair<libusb_error, ptrdiff_t> bulk_transfer(const unsigned char endpoint, BufferValueType* buffer, intmax_t bufferSizeBytes) const noexcept(!Throw);

    // bulk_transfer() overload for contiguous storage containers having data() and size() methods
    template <mvLog_t Loglevel = MVLOG_ERROR,
              bool Throw = true,
              unsigned int ChunkTimeoutMs = 0 /* unlimited */,
              bool ZeroLengthPacketEnding = false,
              unsigned int TimeoutMs = 0 /* unlimited */,
              typename ContainerType>
    std::pair<libusb_error, ptrdiff_t> bulk_transfer(const unsigned char endpoint, ContainerType& container) const noexcept(!Throw) {
        return bulk_transfer<Loglevel, Throw, ChunkTimeoutMs, ZeroLengthPacketEnding, TimeoutMs>(
            endpoint, container.data(), container.size() * sizeof(typename ContainerType::value_type));
    }

    // basic wrapper for libusb_bulk_transfer()
    int bulk_transfer(unsigned char endpoint, void *data, int length, int *transferred, std::chrono::milliseconds timeout) const noexcept {
        return libusb_bulk_transfer(get(), endpoint, static_cast<unsigned char*>(data), length, transferred, static_cast<unsigned int>(timeout.count()));
    }
};

class usb_device : public unique_resource_ptr<libusb_device, libusb_unref_device> {
private:
    using _base = unique_resource_ptr<libusb_device, libusb_unref_device>;

public:
    // inherit base constructors
    using unique_resource_ptr<libusb_device, libusb_unref_device>::unique_resource_ptr;

    // stores a raw libusb_device* pointer, increments its refcount with libusb_ref_device()
    explicit usb_device(pointer ptr) noexcept : _base{libusb_ref_device(ptr)} {}

    // delete base constructors that conflict with libusb ref counts
    usb_device(pointer, const deleter_type &) = delete;
    usb_device(pointer, deleter_type &&) = delete;

    // generate a device_handle with libusb_open() to enable i/o on the device
    device_handle open() const noexcept(false) {
        device_handle handle;
        CALL_LOG_ERROR_THROW(libusb_open, get(), dai::out_param(handle));
        return handle;
    }

    // start managing the new libusb_device* with libusb_ref_device()
    // then remove the old libusb_device* and decrement its ref count
    // No exceptions are thrown. No errors are logged.
    void reset(pointer ptr = pointer{}) noexcept {
        static_cast<_base*>(this)->reset(libusb_ref_device(ptr));
    }

    // wrapper for libusb_get_config_descriptor()
    config_descriptor get_config_descriptor(uint8_t configIndex) const noexcept(false) {
        config_descriptor descriptor;
        CALL_LOG_ERROR_THROW(libusb_get_config_descriptor, get(), configIndex, dai::out_param(descriptor));
        return descriptor;
    }

    // wrapper for libusb_get_device_descriptor()
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_device_descriptor get_device_descriptor() const noexcept(!Throw) {
        libusb_device_descriptor descriptor{};
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_device_descriptor, get(), &descriptor);
        return descriptor;
    }

    // wrapper for libusb_get_bus_number()
    uint8_t get_bus_number() const noexcept(false) {
        return CALL_LOG_ERROR_THROW(libusb_get_bus_number, get());
    }

    // wrapper for libusb_get_port_numbers()
    // template param Len is the maximum possible number of port-numbers read from usb
    // the return vector is resized to the actual number read
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, int Len = 7>
    std::vector<uint8_t> get_port_numbers() const noexcept(!Throw) {
        std::vector<uint8_t> numbers(Len, 0);
        const auto result = call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_port_numbers, get(), numbers.data(), Len);
        if (Throw || result >= 0) {
            // when throwing enabled, then throw occurs on error results before this resize(),
            // so don't need to check result and compiler can optimize this if chain away.
            // when throwing disabled, then always prevent resize of negative/error code result
            numbers.resize(result);
        }
        return numbers;
    }
};

// wrap libusb_get_device() to return a ref counted usb_device
inline usb_device device_handle::get_device() const noexcept {
    return usb_device{libusb_get_device(get())};
}

template <mvLog_t Loglevel, bool Throw, unsigned int ChunkTimeoutMs, bool ZeroLengthPacketEnding, unsigned int TimeoutMs, typename BufferValueType>
inline std::pair<libusb_error, ptrdiff_t> device_handle::bulk_transfer(const unsigned char endpoint,
                                                                       BufferValueType* const buffer,
                                                                       intmax_t bufferSizeBytes) const noexcept(!Throw) {
    static constexpr auto FAIL_FORMAT =    "dai::libusb failed bulk_transfer(%u %s): %td/%jd bytes transmit; %s";
    static constexpr auto START_FORMAT =   "dai::libusb starting bulk_transfer(%u %s): 0/%jd bytes transmit";
    static constexpr auto ZLP_FORMAT =     "dai::libusb zerolp bulk_transfer(%u %s): %td/%jd bytes transmit";
    static constexpr auto SUCCESS_FORMAT = "dai::libusb success bulk_transfer(%u %s): %td/%jd bytes transmit in %lf ms (%lf MB/s)";
    static constexpr std::array<const char*, 2> DIRECTION_TEXT{"out", "in"};
    static constexpr int DEFAULT_CHUNK_SIZE = 1024 * 1024;  // must be multiple of endpoint max packet size
    static constexpr int DEFAULT_CHUNK_SIZE_USB1 = 64;      // must be multiple of endpoint max packet size
    static constexpr bool BUFFER_IS_CONST = static_cast<bool>(std::is_const<BufferValueType>::value);
    static constexpr auto CHUNK_TIMEOUT = (TimeoutMs == 0) ? ChunkTimeoutMs : std::min(ChunkTimeoutMs, TimeoutMs);

    std::pair<libusb_error, ptrdiff_t> result{LIBUSB_ERROR_INVALID_PARAM, 0};

    // verify parameters and that buffer for specific endpoint is non-const for IN endpoints
    if((BUFFER_IS_CONST && is_direction_in(endpoint)) || buffer == nullptr || bufferSizeBytes < 0) {
        mvLog(MVLOG_FATAL, FAIL_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], 0, bufferSizeBytes, "invalid buffer const, pointer, or size");
        throw_conditional_transfer_error(LIBUSB_ERROR_INVALID_PARAM, 0, std::integral_constant<bool, Throw>{});
    }

    // start transfer
    else {
        const bool transmitZeroLengthPacket = ZeroLengthPacketEnding; // && (bufferSizeBytes % get_max_packet_size(endpoint) == 0);
        const auto chunkSize = get_device_descriptor().bcdUSB >= 0x200 ? DEFAULT_CHUNK_SIZE : DEFAULT_CHUNK_SIZE_USB1;
        const auto completeSizeBytes = bufferSizeBytes;
        auto& rcNum = result.first;
        auto& transferredBytes = result.second;
        mvLog(MVLOG_DEBUG, START_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], bufferSizeBytes);

        // loop until all data is transferred
        const auto t1 = std::chrono::steady_clock::now();
        auto iterationBuffer = reinterpret_cast<unsigned char*>(const_cast<typename std::remove_cv<BufferValueType>::type*>(buffer));
        while(bufferSizeBytes || transmitZeroLengthPacket) {
            // calculate the number of bytes to transfer in this iteration; never more than chunkSize
            int iterationBytesToTransfer = static_cast<int>(std::min<intmax_t>(bufferSizeBytes, chunkSize));

            // Low-level usb packets sized at the endpoint's max packet size will prevent overflow errors.
            // This must be handled during the last chunk. If that chunk isn't exactly a multiple of the
            // endpoint's max packet size, then need to transfer the largest multiple of max packet size
            // and then create a overflow buffer to receive the final "partial" packet of that final chunk
            // and then copy that into the outgoing buffer.
            std::vector<unsigned char> overflowBuffer;
            if (is_direction_in(endpoint) && (iterationBytesToTransfer % get_max_packet_size(endpoint) != 0)) {
                // adjust down the iterationBytesToTransfer to the largest multiple of max packet size
                // which lets this iteration complete without overflow errors, and setup the next
                // iteration to handle the final "partial" packet
                iterationBytesToTransfer -= iterationBytesToTransfer % get_max_packet_size(endpoint);

                // if this is the final packet, then use the local overflow buffer to receive
                // the final "partial" packet and copy the transmitted bytes into the outgoing buffer
                if (iterationBytesToTransfer == 0) {
                    overflowBuffer.resize(get_max_packet_size(endpoint));
                    iterationBuffer = overflowBuffer.data();
                    iterationBytesToTransfer = static_cast<int>(overflowBuffer.size());
                }
            }

            // transfer the data
            int iterationTransferredBytes{0};
            rcNum = static_cast<libusb_error>(libusb_bulk_transfer(get(), endpoint, iterationBuffer, iterationBytesToTransfer, &iterationTransferredBytes, CHUNK_TIMEOUT));

            // validate if too much data received; likely corrupts memory
            // caution: libusb docs writes when error is LIBUSB_ERROR_OVERFLOW
            // then behaviour is largely undefined: actual_length out variable may or may
            // not be accurate, the chunk of data that can fit in the buffer (before overflow)
            // may or may not have been transferred.
            // http://billauer.co.il/blog/2019/12/usb-bulk-overflow-halt-reset/
            assert(iterationTransferredBytes <= iterationBytesToTransfer);

            // update the transferred bytes except for LIBUSB_ERROR_OVERFLOW because other
            // result codes can still transfer data
            if(rcNum != LIBUSB_ERROR_OVERFLOW) {
                // did the transfer happen from the overflow buffer?
                if(iterationBuffer == overflowBuffer.data()) {
                    // validate the data transmitted into the overflow buffer is not larger than remaining space in outgoing buffer
                    // However in testing, if I start an 84 byte incoming transfer, so it is all in one packet, so I use the overflow buffer to receive
                    // since it is not %=0, so I tell the libusbapi 512 bytes buffer to avoid overflows, then libusb comes back with 512 bytes instead
                    // of the 84 I would expect. Either libusb or the device sent the data. When I inspected the 512 packet, I could see
                    // the first 84 bytes seemed formatted and regular, and then the 85 bytes and later seemed patterned. Maybe that is
                    // uninitialized data.
                    //if(iterationTransferredBytes > bufferSizeBytes) {
                    //    rcNum = LIBUSB_ERROR_OVERFLOW;
                    //    mvLog(Loglevel, FAIL_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], transferredBytes, completeSizeBytes, "final packet won't fit into outgoing buffer");
                    //    throw_conditional_transfer_error(LIBUSB_ERROR_OVERFLOW, transferredBytes, std::integral_constant<bool, Throw>{});
                    //    break;
                    //}

                    // copy the final "partial" packet from the overflow buffer into the outgoing buffer
                    std::copy_n(overflowBuffer.data(),
                                bufferSizeBytes,
                                reinterpret_cast<unsigned char*>(const_cast<typename std::remove_cv<BufferValueType>::type*>(buffer)) + transferredBytes);
                    iterationTransferredBytes = bufferSizeBytes;
                }
                transferredBytes += iterationTransferredBytes;
            }

            // handle error codes, or if the number of bytes transferred != number bytes requested
            // note: Scenarios of receiving data from an IN endpoint, and the usb device sends less data than the bufferSizeBytes are
            //       specifically handled. An example is getLibusbDeviceMxId(). Older code call this function would send a too large
            //       buffer which causes this scenario. When too large then this while loop would keep looping until LIBUSB_ERROR_TIMEOUT.
            //       However, if the caller provides a buffer that is exactly the size needed, (getLibusbDeviceMxId() does know this size,
            //       then this loop will quickly exit.
            // note: previous logic ORd (iterationTransferredBytes != iterationBytesToTransfer) with (rcNum < 0)
            if(transferredBytes != completeSizeBytes /* logic is superset of ZLP: (iterationBytesToTransfer != 0) */ && rcNum < 0) {
                mvLog(Loglevel, FAIL_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], transferredBytes, completeSizeBytes, libusb_strerror(static_cast<int>(rcNum)));
                throw_conditional_transfer_error(static_cast<int>(rcNum), transferredBytes, std::integral_constant<bool, Throw>{});
                break;
            }

            // handle zero length packet transfer (the last packet)
            if(iterationBytesToTransfer == 0) {
                mvLog(MVLOG_DEBUG, ZLP_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], transferredBytes, completeSizeBytes);
                rcNum = LIBUSB_SUCCESS;  // simulating behavior in previous send_file() function
                break;
            }

            // successfully and fully transferred this iteration's buffer
            bufferSizeBytes -= iterationTransferredBytes;
            iterationBuffer += iterationTransferredBytes;

            // check for timeout; when TimeoutMs is non-zero, and bufferSizeBytes is non-zero (all bytes not yet transferred),
            // and the duration since started has exceeded TimeoutMs milliseconds
            if(static_cast<bool>(TimeoutMs) && bufferSizeBytes && (std::chrono::steady_clock::now() - t1 > std::chrono::milliseconds{TimeoutMs})) {
                rcNum = LIBUSB_ERROR_TIMEOUT;
                mvLog(Loglevel, FAIL_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], transferredBytes, completeSizeBytes, libusb_strerror(static_cast<int>(rcNum)));
                throw_conditional_transfer_error(static_cast<int>(rcNum), transferredBytes, std::integral_constant<bool, Throw>{});
                break;
            }
        }
#ifndef NDEBUG
        if(rcNum == LIBUSB_SUCCESS) {
            // calculate the transfer rate (MB/s) and log the result
            using double_msec = std::chrono::duration<double, std::chrono::milliseconds::period>;
            const double elapsedMs = std::chrono::duration_cast<double_msec>(std::chrono::steady_clock::now() - t1).count();
            const double MBpS = (static_cast<double>(transferredBytes) / 1048576.) / (elapsedMs * 0.001);
            mvLog(MVLOG_DEBUG, SUCCESS_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], transferredBytes, completeSizeBytes, elapsedMs, MBpS);
        }
#endif
    }
    return result;
}

} // namespace libusb

#undef CALL_LOG_ERROR_THROW

} // namespace dai
#endif // _WRAP_LIBUSB_HPP_
