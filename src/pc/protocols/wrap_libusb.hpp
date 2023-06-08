/*
    dp::libusb - C++ wrapper for libusb-1.0 (focused on use with the XLink protocol)

    Copyright 2023 Dale Phurrough

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#ifndef _WRAP_LIBUSB_HPP_
#define _WRAP_LIBUSB_HPP_

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

/// @brief dp namespace
namespace dp {

///////////////////////////////
// Helper functions and macros
///////////////////////////////

/// @brief unique_resource_ptr deleter used to call native API to release resources
/// @tparam Resource native API resource type; the type of the resource disposed/released by this deleter
/// @tparam Dispose native API function used to release the resource; must be a function pointer
template<typename Resource, void(*Dispose)(Resource*)>
struct unique_resource_deleter {
    inline void operator()(Resource* const ptr) noexcept {
        if (ptr != nullptr)
            Dispose(ptr);
    }
};

/// @brief base unique_resource_ptr class to wrap native API resources; behavior is undefined
///        using operator->() or operator*() when get() == nullptr
/// @tparam Resource native API resource type; the type of the resource managed by this unique_resource_ptr
/// @tparam Dispose native API function used to release the resource; must be a function pointer
template<typename Resource, void(*Dispose)(Resource*)>
class unique_resource_ptr : public std::unique_ptr<Resource, unique_resource_deleter<Resource, Dispose>> {
private:
    using _base = std::unique_ptr<Resource, unique_resource_deleter<Resource, Dispose>>;

public:
    // inherit base constructors, long form due to Clang fail using _base
    using std::unique_ptr<Resource, unique_resource_deleter<Resource, Dispose>>::unique_ptr;

    // delete base unique_resource_ptr constructors that would conflict with ref counts
    unique_resource_ptr(typename _base::pointer, const typename _base::deleter_type &) = delete;
    unique_resource_ptr(typename _base::pointer, typename _base::deleter_type &&) = delete;
};


/// @brief libusb resources, structs, and functions with RAII resource management and exceptions
namespace libusb {

///////////////////////////////////
// libusb error exception wrappers
///////////////////////////////////

/// @brief exception error class for libusb errors
class usb_error : public std::system_error {
public:
    explicit usb_error(int libusbErrorCode) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), libusb_strerror(libusbErrorCode)} {}
    usb_error(int libusbErrorCode, const std::string& what) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), what} {}
    usb_error(int libusbErrorCode, const char* what) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), what} {}
};

/// @brief exception error class for libusb transfer errors
class transfer_error : public usb_error {
public:
    explicit transfer_error(int libusbErrorCode, intmax_t transferred) noexcept :
        usb_error{libusbErrorCode}, transferred{transferred} {}
    transfer_error(int libusbErrorCode, const std::string& what, intmax_t transferred) noexcept :
        usb_error{libusbErrorCode, what}, transferred{transferred} {}
    transfer_error(int libusbErrorCode, const char* what, intmax_t transferred) noexcept :
        usb_error{libusbErrorCode, what}, transferred{transferred} {}
private:
    intmax_t transferred{}; // number of bytes transferred
};

// tag dispatch for throwing or not throwing exceptions
inline void throw_conditional_usb_error(int libusbErrorCode, std::true_type) noexcept(false) {
    throw usb_error(libusbErrorCode);
}
inline void throw_conditional_usb_error(int, std::false_type) noexcept(true) {
    // do nothing
}
inline void throw_conditional_transfer_error(int libusbErrorCode, intmax_t transferred, std::true_type) noexcept(false) {
    throw transfer_error(libusbErrorCode, transferred);
}
inline void throw_conditional_transfer_error(int, intmax_t, std::false_type) noexcept(true) {
    // do nothing
}

/// @brief template function that can call any libusb function passed to it
/// @tparam Loglevel log level used when errors occur
/// @tparam Throw throw exceptions on errors or only return error codes; when false, return codes must be handled by the caller
/// @tparam Func auto-deduced libusb function pointer type
/// @tparam ...Args auto-deduced variadic template parameter pack for the function parameter(s)
/// @param funcWithin name of the function calling this function; prepended to the log message
/// @param lineNumber line number of the function calling this function; prepended to the log message
/// @param func libusb function pointer
/// @param ...args libusb function parameter(s)
/// @return return code of the libusb function
template <mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, typename Func, typename... Args>
inline auto call_log_throw(const char* funcWithin, const int lineNumber, Func&& func, Args&&... args) noexcept(!Throw)
    -> decltype(func(std::forward<Args>(args)...)) {
    const auto rcNum = func(std::forward<Args>(args)...);
    if (rcNum < 0) {
        logprintf(MVLOGLEVEL(MVLOG_UNIT_NAME),
                  Loglevel,
                  funcWithin,
                  lineNumber,
                  "dp::libusb failed %s(): %s",
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

/// @brief RAII wrapper for libusb_context; automatically calls libusb_exit() on destruction
using usb_context = unique_resource_ptr<libusb_context, libusb_exit>;

/// @brief RAII wrapper for libusb_device; automatically calls libusb_unref_device() on destruction
class usb_device;

/// @brief RAII wrapper for libusb_config_descriptor; automatically calls libusb_free_config_descriptor() on destruction
class config_descriptor : public unique_resource_ptr<libusb_config_descriptor, libusb_free_config_descriptor> {
public:
    using unique_resource_ptr<libusb_config_descriptor, libusb_free_config_descriptor>::unique_resource_ptr;

    config_descriptor(libusb_device* dev, uint8_t configIndex) noexcept(false) {
        CALL_LOG_ERROR_THROW(libusb_get_config_descriptor, dev, configIndex, out_param(*this));
    }
};

/// @brief RAII wrapper for libusb_device_handle to allow I/O on device; automatically calls libusb_close() on destruction
/// @note Can also create with usb_device::open()
class device_handle : public unique_resource_ptr<libusb_device_handle, libusb_close> {
private:
    using _base = unique_resource_ptr<libusb_device_handle, libusb_close>;

    static constexpr int DEFAULT_CHUNK_SIZE = 1024 * 1024;  // must be multiple of endpoint max packet size
    static constexpr int DEFAULT_CHUNK_SIZE_USB1 = 64;      // must be multiple of endpoint max packet size
    static constexpr decltype(libusb_endpoint_descriptor::wMaxPacketSize) DEFAULT_MAX_PACKET_SIZE = 512; // USB 1.1 = 64, USB 2.0 = 512, USB 3+ = 1024
    static constexpr std::array<decltype(libusb_endpoint_descriptor::wMaxPacketSize), 32> DEFAULT_MAX_PACKET_ARRAY{
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
        DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE, DEFAULT_MAX_PACKET_SIZE,
    };

    int chunkSize = DEFAULT_CHUNK_SIZE; // persisted for quick access by transfer methods
    std::array<decltype(libusb_endpoint_descriptor::wMaxPacketSize), 32> maxPacketSize = DEFAULT_MAX_PACKET_ARRAY;
    std::vector<int> claimedInterfaces;

    // exchange DEFAULT_MAX_PACKET_ARRAY -> val -> return
    static decltype(maxPacketSize) exchange_maxPacketSize(decltype(maxPacketSize)& val) {
        auto old = std::move(val);
        val = DEFAULT_MAX_PACKET_ARRAY;
        return old;
    }

    // get chunk size in bytes given device bcdUSB version
    static int get_chunk_size(const uint16_t bcdUSB) noexcept {
        return bcdUSB >= 0x200 ? DEFAULT_CHUNK_SIZE : DEFAULT_CHUNK_SIZE_USB1;
    }

    // given an endpoint address, return true if that address is for an IN endpoint, false for OUT
    // endpoint bit 7 = 0 is OUT host to device, bit 7 = 1 is IN device to host (Ignored for Control Endpoints)
    static bool is_direction_in(uint8_t endpoint) noexcept {
        // alternate calculation: (endpoint >> 7) & 0x01
        return static_cast<bool>(endpoint & 0x80);
    }

public:
    ////////////////
    // constructors
    ////////////////

    // inherit base constructors, long form due to Clang fail using _base
    using unique_resource_ptr<libusb_device_handle, libusb_close>::unique_resource_ptr;

    /// @brief Create a device_handle from a raw libusb_device_handle*
    /// @param handle raw libusb_device_handle* to wrap and manage ownership
    /// @note This constructor will not manage previously claimed interfaces and
    ///       will default to DEFAULT_MAX_PACKET_SIZE for all endpoints
    explicit device_handle(pointer handle) noexcept(false)
        : _base{handle}, chunkSize{handle ? get_chunk_size(get_device_descriptor().bcdUSB) : DEFAULT_CHUNK_SIZE} {}

    // move constructor
    device_handle(device_handle &&other) noexcept :
        _base{std::move(other)},
        chunkSize{std::exchange(other.chunkSize, DEFAULT_CHUNK_SIZE)},
        maxPacketSize{exchange_maxPacketSize(other.maxPacketSize)},
        claimedInterfaces{std::move(other.claimedInterfaces)}
    {}

    // move assign
    device_handle &operator=(device_handle &&other) noexcept {
        if (this != &other) {
            _base::operator=(std::move(other));
            chunkSize = std::exchange(other.chunkSize, DEFAULT_CHUNK_SIZE);
            maxPacketSize = exchange_maxPacketSize(other.maxPacketSize);
            claimedInterfaces = std::move(other.claimedInterfaces);
        }
        return *this;
    }

    // destructor
    ~device_handle() noexcept {
        reset();
    }

    /// @brief Create a device_handle from a raw libusb_device*
    /// @param device raw libusb_device* from which to open a new handle and manage its ownership
    explicit device_handle(libusb_device* device) noexcept(false) {
        if(device == nullptr) throw std::invalid_argument("device == nullptr");
        CALL_LOG_ERROR_THROW(libusb_open, device, out_param(static_cast<_base&>(*this)));

        // cache the device's bcdUSB version for use in transfer methods
        // call libusb_get_device_descriptor() directly since already have the raw libusb_device*
        libusb_device_descriptor descriptor{};
        CALL_LOG_ERROR_THROW(libusb_get_device_descriptor, device, &descriptor);
        chunkSize = get_chunk_size(descriptor.bcdUSB);
    }

    /// @brief Create a device_handle from a platform-specific system device handle
    /// @param ctx raw libusb_context* to use for wrapping the platform-specific system device handle
    /// @param sysDevHandle platform-specific system device handle to wrap
    /// @note Never use usb_device::open() or libusb_open() on this wrapped handle's underlying USB device
    device_handle(libusb_context* ctx, intptr_t sysDevHandle) noexcept(false) {
        if(ctx == nullptr || sysDevHandle == 0) throw std::invalid_argument("ctx == nullptr || sysDevHandle == 0");
        CALL_LOG_ERROR_THROW(libusb_wrap_sys_device, ctx, sysDevHandle, out_param(static_cast<_base&>(*this)));

        // cache the device's bcdUSB version
        chunkSize = get_chunk_size(get_device_descriptor().bcdUSB);
    }

    /// @brief Create a device_handle from a usb_device wrapper
    /// @param device usb_device from which to open a new handle and manage its ownership
    explicit device_handle(const usb_device& device) noexcept(noexcept(device_handle{std::declval<libusb_device*>()}));

    ///////////
    // methods
    ///////////

    /// @brief Release handle and its claimed interfaces, then replace with ptr
    /// @param ptr raw resource pointer used to replace the currently managed resource
    /// @note This method does not automatically manage previously claimed interfaces of ptr and
    ///       will default to DEFAULT_MAX_PACKET_SIZE for all endpoints of ptr
    void reset(pointer ptr = pointer{}) noexcept {
        // release all claimed interfaces and resources
        for (const auto interfaceNumber : claimedInterfaces) {
            call_log_throw<MVLOG_ERROR, false>(__func__, __LINE__, libusb_release_interface, get(), interfaceNumber);
        }
        _base::reset(ptr);

        // reset defaults
        // do not know what interfaces or endpoints are in use, therefore don't know their max packet size
        chunkSize = ptr == nullptr ? DEFAULT_CHUNK_SIZE : get_chunk_size(get_device_descriptor().bcdUSB);
        maxPacketSize = DEFAULT_MAX_PACKET_ARRAY;
        claimedInterfaces.clear();
    }

    /// @brief Release ownership of the managed device_handle and all device interfaces
    /// @return raw libusb_device_handle* that was managed by this device_handle
    /// @note Caller is responsible for calling libusb_release_interface() and libusb_close()
    device_handle::pointer release() noexcept {
        chunkSize = DEFAULT_CHUNK_SIZE;
        maxPacketSize = DEFAULT_MAX_PACKET_ARRAY;
        claimedInterfaces.clear();
        return _base::release();
    }

    /// @brief Get a usb_device for the device_handle's underlying USB device
    /// @return usb_device for the device_handle's underlying USB device
    usb_device get_device() const noexcept;

    /// @brief Claim a USB interface on the device_handle's device
    /// @param interfaceNumber USB interface number to claim
    /// @note Repeat claims of an interface with libusb are generally allowed; the interface
    ///       will be released when the device_handle is destroyed
    void claim_interface(int interfaceNumber) noexcept(false) {
        if(std::find(claimedInterfaces.begin(), claimedInterfaces.end(), interfaceNumber) != claimedInterfaces.end()) return;
        CALL_LOG_ERROR_THROW(libusb_claim_interface, get(), interfaceNumber);
        claimedInterfaces.emplace_back(interfaceNumber);
    }

    /// @brief Release a USB interface on the device_handle's device
    /// @param interfaceNumber USB interface number to release
    void release_interface(int interfaceNumber) noexcept(false) {
        auto candidate = std::find(claimedInterfaces.begin(), claimedInterfaces.end(), interfaceNumber);
        if (candidate == claimedInterfaces.end())
            return;
        CALL_LOG_ERROR_THROW(libusb_release_interface, get(), interfaceNumber);
        claimedInterfaces.erase(candidate);
    }

    /// @brief Get the active configuration value of the device_handle's device
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return error codes
    /// @return active configuration value of the device_handle's device; error returns libusb_error cast to int
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    int get_configuration() const noexcept(!Throw) {
        int configuration{};
        const int rc = call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_configuration, get(), &configuration);
        return rc >= LIBUSB_SUCCESS ? configuration : rc;
    }

    /// @brief Set the active configuration of the device_handle's device
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return error codes
    /// @tparam Force force configuration to be set even if it is already active; may cause a lightweight device reset
    /// @param configuration value of the configuration to set; put the device in unconfigured state with -1
    /// @return libusb_error result code
    /// @note Recommended to set the configuration before claiming interfaces
    template <mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, bool Force = false>
    libusb_error set_configuration(int configuration) noexcept(!Throw) {
        if(!Force) {
            const auto active = get_configuration<Loglevel, Throw>();
            if(active == configuration)
                return LIBUSB_SUCCESS;
            mvLog(MVLOG_DEBUG, "Setting configuration from %d to %d", active, configuration);
        }
        return static_cast<libusb_error>(call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_set_configuration, get(), configuration));
    }

    // wrapper for libusb_get_string_descriptor_ascii()
    // return string is size of actual number of ascii chars in descriptor

    /// @brief Get a string descriptor from the device_handle's device and convert to ASCII
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return empty string
    /// @param descriptorIndex index of the string descriptor to get
    /// @return string descriptor from the device_handle's device converted to ASCII; empty on error
    template <mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    std::string get_string_descriptor_ascii(uint8_t descriptorIndex) const noexcept(!Throw) {
        // String descriptors use UNICODE UTF16LE encodings where a bLength byte field declares the sizeBytes of the string + 2
        // and the string is not NULL-terminated. However, internet searches show some devices do null terminate.
        // The libusb api converts to ASCII
        // Therefore, the max ascii string length is: (bLength - 2) / 2 = (255 - 2) / 2 = 126.5
        std::string descriptor(127, 0);
        const auto result = call_log_throw<Loglevel, Throw>(
            __func__, __LINE__, libusb_get_string_descriptor_ascii, get(), descriptorIndex, (unsigned char*)(descriptor.data()), 127);
        if (Throw || result >= 0) {
            // when throwing enabled, then throw occurs on negative/error results before this resize(),
            // so don't need to check result and compiler can optimize this away.
            // when throwing disabled, then resize on non-error
            descriptor.resize(result);
        }
        else {
            // when throwing disabled, then return empty string on error
            descriptor.clear();
        }
        return descriptor;
    }

    /// @brief Set whether the device_handle's device should automatically detach the kernel driver when claiming interfaces
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return error codes
    /// @param enable true to automatically detach kernel driver when claiming interface then attach it when releasing interface
    /// @return libusb_error result code
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_error set_auto_detach_kernel_driver(bool enable) noexcept(!Throw) {
        return static_cast<libusb_error>(call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_set_auto_detach_kernel_driver, get(), enable));
    }

    /// @brief Set the maximum packet size for an endpoint
    /// @param endpoint USB endpoint address
    /// @param size maximum packet size for the endpoint
    void set_max_packet_size(uint8_t endpoint, decltype(libusb_endpoint_descriptor::wMaxPacketSize) size) noexcept {
        // keep endpoint bits 0-3, then move bit 7 to bit 4
        // this creates a 0-31 number representing all possible endpoint addresses
        maxPacketSize[(endpoint & 0x0F) | ((endpoint & 0x80) >> 3)] = size;
    }

    /// @brief Get the maximum packet size for an endpoint
    /// @param endpoint USB endpoint address
    /// @return value previously stored with set_max_packet_size(endpoint, value)
    decltype(libusb_endpoint_descriptor::wMaxPacketSize) get_max_packet_size(uint8_t endpoint) const noexcept {
        // keep endpoint bits 0-3, then move bit 7 to bit 4
        // this creates a 0-31 number representing all possible endpoint addresses
        return maxPacketSize[(endpoint & 0x0F) | ((endpoint & 0x80) >> 3)];
    }

    /// @brief Get the device descriptor for the device_handle's device
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return a value initialized libusb_device_descriptor
    /// @return device descriptor for the device_handle's device; value initialized on error
    /// @note Faster than calling get_device() then get_device_descriptor()
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_device_descriptor get_device_descriptor() const noexcept(!Throw) {
        libusb_device_descriptor descriptor{};
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_device_descriptor, libusb_get_device(get()), &descriptor);
        return descriptor;
    }

    /// @brief transfer with libusb_bulk_transfer(); transfers IN continue until bufferSizeBytes is full or timeout/error occurs
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return error codes
    /// @tparam ChunkTimeoutMs timeout in milliseconds for transfer of each chunk of data; 0 = unlimited
    /// @tparam ZeroLengthPacketEnding end transfers OUT having (bufferSizeBytes % maxPacketSize(ep) == 0) with zero length packet
    /// @tparam TimeoutMs timeout in milliseconds for entire transfer of data; 0 = unlimited
    /// @param endpoint USB endpoint address
    /// @param buffer buffer for transfer
    /// @param bufferSizeBytes size of buffer in bytes
    /// @return std::pair with error code and number of bytes actually transferred
    template <mvLog_t Loglevel = MVLOG_ERROR,
              bool Throw = true,
              unsigned int ChunkTimeoutMs = 0 /* unlimited */,
              bool ZeroLengthPacketEnding = false,
              unsigned int TimeoutMs = 0 /* unlimited */,
              typename BufferValueType>
    std::pair<libusb_error, intmax_t> bulk_transfer(unsigned char endpoint, BufferValueType* buffer, intmax_t bufferSizeBytes) const noexcept(!Throw);

    /// @brief transfer with libusb_bulk_transfer(); transfers IN continue until bufferSizeBytes is full or timeout/error occurs
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return error codes
    /// @tparam ChunkTimeoutMs timeout in milliseconds for transfer of each chunk of data; 0 = unlimited
    /// @tparam ZeroLengthPacketEnding end transfers OUT having (bufferSizeBytes % maxPacketSize(ep) == 0) with zero length packet
    /// @tparam TimeoutMs timeout in milliseconds for entire transfer of data; 0 = unlimited
    /// @param endpoint USB endpoint address
    /// @param buffer buffer for transfer; contiguous storage container having data() and size() methods
    /// @return std::pair with error code and number of bytes actually transferred
    template <mvLog_t Loglevel = MVLOG_ERROR,
              bool Throw = true,
              unsigned int ChunkTimeoutMs = 0 /* unlimited */,
              bool ZeroLengthPacketEnding = false,
              unsigned int TimeoutMs = 0 /* unlimited */,
              typename ContainerType>
    std::pair<libusb_error, intmax_t> bulk_transfer(const unsigned char endpoint, ContainerType& buffer) const noexcept(!Throw) {
        return bulk_transfer<Loglevel, Throw, ChunkTimeoutMs, ZeroLengthPacketEnding, TimeoutMs>(
            endpoint, buffer.data(), buffer.size() * sizeof(typename ContainerType::value_type));
    }

    /// @brief basic wrapper for libusb_bulk_transfer()
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return error codes
    /// @return libusb_error result code
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_error bulk_transfer(unsigned char endpoint, void *data, int length, int *transferred, std::chrono::milliseconds timeout) const noexcept {
        return static_cast<libusb_error>(call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_bulk_transfer, get(), endpoint, static_cast<unsigned char*>(data), length, transferred, static_cast<unsigned int>(timeout.count())));
    }

    /// @brief basic wrapper for libusb_control_transfer()
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return error codes
    /// @return libusb_error result code
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_error control_transfer(uint8_t requestType, uint8_t request, uint16_t value, uint16_t index, void *data, uint16_t length, std::chrono::milliseconds timeout) const noexcept(!Throw) {
        return static_cast<libusb_error>(call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_control_transfer, get(), requestType, request, value, index, static_cast<unsigned char*>(data), length, static_cast<unsigned int>(timeout.count())));
    }

    /// @brief basic wrapper for libusb_interrupt_transfer()
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return error codes
    /// @return libusb_error result code
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_error interrupt_transfer(unsigned char endpoint, void *data, int length, int *transferred, std::chrono::milliseconds timeout) const noexcept {
        return static_cast<libusb_error>(call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_interrupt_transfer, get(), endpoint, static_cast<unsigned char*>(data), length, transferred, static_cast<unsigned int>(timeout.count())));
    }
};

/// @brief RAII wrapper for libusb_device; automatically calls libusb_unref_device() on destruction
class usb_device : public unique_resource_ptr<libusb_device, libusb_unref_device> {
private:
    using _base = unique_resource_ptr<libusb_device, libusb_unref_device>;

public:
    // inherit base constructors, long form due to Clang fail using _base
    using unique_resource_ptr<libusb_device, libusb_unref_device>::unique_resource_ptr;

    /// @brief construct a usb_device from a raw libusb_device* pointer and shares ownership
    /// @param ptr raw libusb_device* pointer
    explicit usb_device(pointer ptr) noexcept : _base{ptr ? libusb_ref_device(ptr) : nullptr} {}

    /// @brief open a device_handle for i/o on the device
    /// @return device_handle for i/o on the device
    device_handle open() const noexcept(false) {
        return device_handle{get()};
    }

    /// @brief Release shared ownership of usb device, then replace with ptr and take shared ownership
    /// @param ptr raw resource pointer used to replace the currently managed resource
    /// @note No exceptions are thrown. No errors are logged.
    void reset(pointer ptr = pointer{}) noexcept {
        _base::reset(ptr ? libusb_ref_device(ptr) : nullptr);
    }

    /// @brief get the USB config descriptor given the index
    /// @param configIndex index of the config descriptor to get
    /// @return config_descriptor for the USB device
    config_descriptor get_config_descriptor(uint8_t configIndex) const noexcept(noexcept(config_descriptor{get(), 0})) {
        return config_descriptor{get(), configIndex};
    }

    /// @brief get the USB device descriptor
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return a value initialized libusb_device_descriptor
    /// @return device descriptor for the device; value initialized on error
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_device_descriptor get_device_descriptor() const noexcept(!Throw) {
        libusb_device_descriptor descriptor{};
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_device_descriptor, get(), &descriptor);
        return descriptor;
    }

    /// @brief get the USB bus number to which the device is connected
    /// @return bus number to which the device is connected
    uint8_t get_bus_number() const noexcept(false) {
        return CALL_LOG_ERROR_THROW(libusb_get_bus_number, get());
    }

    /// @brief get the USB port numbers to which the device is connected
    /// @tparam Loglevel log level used when errors occur
    /// @tparam Throw throw exceptions on errors or only return empty vector
    /// @tparam Len maximum number of port numbers to read from usb
    /// @return port numbers to which device is connected, resized to actual number read; empty on error
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
        else {
            // when throwing disabled, then return empty vector on error
            numbers.clear();
        }
        return numbers;
    }
};

/// @brief RAII wrapper for generated list of usb_device; automatically releases shared ownership of devices and frees device list on destruction
class device_list {
private:
    /// @brief iterator template internally used by device_list
    /// @tparam Val value type of the iterator
    /// @tparam Ref reference type of the iterator; non-reference transformative type returns an r-value
    ///         to prevent dangling references with operator*() and operator[]()
    template<typename Val, typename Ref>
    class iterator_xform {
      public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = Val;
        using difference_type = std::ptrdiff_t;
        using reference = Ref;
        using pointer = value_type*;

        explicit iterator_xform(pointer ptr) noexcept : ptr_{ptr} {}

        // until C++17, deref operators must return non-const reference since usb_device derives
        // from std::unique_ptr and a const unique_ptr can not be moved or copied as needed by a function return
        reference operator*() const noexcept {
            return reference{*ptr_};
        }
        reference operator[](difference_type n) const noexcept {
            return reference{*(ptr_ + n)};
        }
        // no operator->() because it would require persisting a usb_device object in the iterator_xform
        // itself and then returning a pointer to it
        // pointer operator->() const {
        //     return ptr_;
        // }

        iterator_xform& operator++() noexcept {
            ++ptr_;
            return *this;
        }
        iterator_xform operator++(int) noexcept {
            auto tmp = *this;
            operator++();
            return tmp;
        }
        iterator_xform& operator--() noexcept {
            --ptr_;
            return *this;
        }
        iterator_xform operator--(int) noexcept {
            auto tmp = *this;
            operator--();
            return tmp;
        }

        bool operator==(const iterator_xform& rhs) const noexcept {
            return ptr_ == rhs.ptr_;
        }
        bool operator!=(const iterator_xform& rhs) const noexcept {
            return !(*this == rhs);
        }
        bool operator<(const iterator_xform& rhs) const noexcept {
            return ptr_ < rhs.ptr_;
        }
        bool operator>(const iterator_xform& rhs) const noexcept {
            return rhs < *this;
        }
        bool operator<=(const iterator_xform& rhs) const noexcept {
            return !(*this > rhs);
        }
        bool operator>=(const iterator_xform& rhs) const noexcept {
            return !(*this < rhs);
        }

        iterator_xform& operator+=(difference_type n) noexcept {
            ptr_ += n;
            return *this;
        }
        iterator_xform operator+(difference_type n) const noexcept {
            auto tmp = *this;
            tmp += n;
            return tmp;
        }
        iterator_xform& operator-=(difference_type n) noexcept {
            ptr_ -= n;
            return *this;
        }
        iterator_xform operator-(difference_type n) const noexcept {
            auto tmp = *this;
            tmp -= n;
            return tmp;
        }
        difference_type operator-(const iterator_xform& rhs) const noexcept {
            return ptr_ - rhs.ptr_;
        }
      private:
        pointer ptr_ = nullptr;
    };

public:
    using value_type = libusb_device*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = usb_device;
    using const_reference = usb_device; // unable to be const usb_device until c++17 due to unique_ptr base
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using iterator = iterator_xform<libusb_device*, usb_device>;
    using const_iterator = iterator_xform<libusb_device*, usb_device>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // default constructors, destructor, copy, move
    device_list() noexcept = default;
    ~device_list() noexcept {
        // both libapi apis return when param == nullptr
        // workaround libusb bug https://github.com/libusb/libusb/issues/1287
        for (size_type i = 0; i < countDevices; ++i) {
            libusb_unref_device(deviceList[i]);
        }
        libusb_free_device_list(deviceList, 0);
    }
    device_list(const device_list&) = delete;
    device_list& operator=(const device_list&) = delete;
    device_list(device_list&& other) noexcept :
        countDevices{std::exchange(other.countDevices, 0)},
        deviceList{std::exchange(other.deviceList, nullptr)} {};
    device_list& operator=(device_list&& other) noexcept {
        if (this == &other)
            return *this;
        countDevices = std::exchange(other.countDevices, 0);
        deviceList = std::exchange(other.deviceList, nullptr);
        return *this;
    }

    /// @brief Construct a device_list from a raw libusb_context*
    /// @param context raw libusb_context* from which to generate the device list
    explicit device_list(libusb_context* context) noexcept(false) {
        // libusb_get_device_list() is not thread safe!
        // https://github.com/libusb/libusb/wiki/FAQ#what-are-the-extra-considerations-to-be-applied-to-applications-which-interact-with-libusb-from-multiple-threads
        // libusb will crash or have memory violations when multiple threads simultaneously generate device lists
        // due to errant libusb ref count handling, wrongly deleted devices, etc.
        // Testing confirmed crashes occurred when libusb internally called libusb_ref_device() or
        // called usbi_get_device_priv(dev) and then operated on the pointers
        // line in file libusb/os/windows_winusb.c in winusb_get_device_list() line 1741;
        // later code using libusb can crash when they call libusb_unref_device().
        std::lock_guard<std::mutex> lock(mtx);
        countDevices = static_cast<size_type>(CALL_LOG_ERROR_THROW(libusb_get_device_list, context, &deviceList));
    }

    /// @brief Construct a device_list from a usb_context
    /// @param context usb_context from which to generate the device list
    explicit device_list(const usb_context& context) noexcept(false) : device_list{context.get()} {}

    /// @brief Wrap an existing libusb_device** list and its count, then take ownership of it
    /// @param deviceList raw libusb_device** list
    /// @param countDevices count of libusb_device* device pointers in the list
    device_list(pointer deviceList, size_type countDevices) noexcept : deviceList{deviceList}, countDevices{countDevices} {}

    ////////////////////
    // container methods
    ////////////////////

    size_type size() const noexcept {
        return countDevices;
    }
    constexpr size_type max_size() const noexcept {
        constexpr auto MAX = std::numeric_limits<uintptr_t>::max() / sizeof(value_type);
        return MAX;
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
        return iterator{deviceList};
    }
    const_iterator begin() const noexcept {
        return const_iterator{deviceList};
    }
    const_iterator cbegin() const noexcept {
        return const_iterator{deviceList};
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
    void swap(device_list& other) noexcept {
        std::swap(countDevices, other.countDevices);
        std::swap(deviceList, other.deviceList);
    }
    friend void swap(device_list& left, device_list& right) noexcept {
        left.swap(right);
    }
    bool operator==(const device_list& other) const noexcept {
        // short circuit if same pointer list
        return countDevices == other.countDevices && deviceList == other.deviceList;
    }
    bool operator!=(const device_list& other) const noexcept {
        return !(*this == other);
    }

private:
    static std::mutex mtx;
    size_type countDevices{};
    pointer deviceList{};
};

inline device_handle::device_handle(const usb_device& device) noexcept(noexcept(device_handle{device.get()})) : device_handle{device.get()} {}

// wrap libusb_get_device() and return a usb_device
inline usb_device device_handle::get_device() const noexcept {
    return usb_device{libusb_get_device(get())};
}

// TODO should a short packet on IN endpoint indicate the device is finished and this function quickly return vs. wait for timeout/error?
template <mvLog_t Loglevel, bool Throw, unsigned int ChunkTimeoutMs, bool ZeroLengthPacketEnding, unsigned int TimeoutMs, typename BufferValueType>
inline std::pair<libusb_error, intmax_t> device_handle::bulk_transfer(const unsigned char endpoint,
                                                                       BufferValueType* const buffer,
                                                                       intmax_t bufferSizeBytes) const noexcept(!Throw) {
    static constexpr auto FAIL_FORMAT =    "dp::libusb failed bulk_transfer(%u %s): %td/%jd bytes transmit; %s";
    static constexpr auto START_FORMAT =   "dp::libusb starting bulk_transfer(%u %s): 0/%jd bytes transmit";
    static constexpr auto ZLP_FORMAT =     "dp::libusb zerolp bulk_transfer(%u %s): %td/%jd bytes transmit";
    static constexpr auto SUCCESS_FORMAT = "dp::libusb success bulk_transfer(%u %s): %td/%jd bytes transmit in %lf ms (%lf MB/s)";
    static constexpr std::array<const char*, 2> DIRECTION_TEXT{"out", "in"};
    static constexpr bool BUFFER_IS_CONST = static_cast<bool>(std::is_const<BufferValueType>::value);
    static constexpr auto CHUNK_TIMEOUT = (TimeoutMs == 0) ? ChunkTimeoutMs : std::min(ChunkTimeoutMs, TimeoutMs);

    std::pair<libusb_error, intmax_t> result{LIBUSB_ERROR_INVALID_PARAM, 0};

    // verify parameters and that buffer for specific endpoint is non-const for IN endpoints
    if((BUFFER_IS_CONST && is_direction_in(endpoint)) || buffer == nullptr || bufferSizeBytes < 0) {
        mvLog(MVLOG_FATAL, FAIL_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], 0, bufferSizeBytes, "invalid buffer const, pointer, or size");
        throw_conditional_transfer_error(LIBUSB_ERROR_INVALID_PARAM, 0, std::integral_constant<bool, Throw>{});
    }

    // start transfer
    else {
        const bool transmitZeroLengthPacket = ZeroLengthPacketEnding && !is_direction_in(endpoint) && (bufferSizeBytes % get_max_packet_size(endpoint) == 0);
        const auto completeSizeBytes = bufferSizeBytes;
        auto& rcNum = result.first;
        auto& transferredBytes = result.second;
        mvLog(MVLOG_DEBUG, START_FORMAT, endpoint, DIRECTION_TEXT[is_direction_in(endpoint)], bufferSizeBytes);

        // loop until all data is transferred
        const auto t1 = std::chrono::steady_clock::now();
        auto *iterationBuffer = reinterpret_cast<unsigned char*>(const_cast<typename std::remove_cv<BufferValueType>::type*>(buffer));
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

            // update the transferred bytes in most cases since some error codes can still transfer data.
            // LIBUSB_ERROR_OVERFLOW is special case and libusb docs writes behavior is undefined and
            // actual_length out variable is unreliable, data may or may not have been transferred.
            // http://billauer.co.il/blog/2019/12/usb-bulk-overflow-halt-reset/
            if(rcNum != LIBUSB_ERROR_OVERFLOW) {

                // adjust this iteration's transferred bytes to minimum of:
                // * number of bytes actually transmitted (IN transfers often larger than requested, overflow buffer use, etc.)
                // * number of bytes remining for storage in the outgoing buffer
                iterationTransferredBytes = static_cast<int>(std::min<intmax_t>(iterationTransferredBytes, bufferSizeBytes));

                // did the transfer happen with the overflow buffer?
                if(iterationBuffer == overflowBuffer.data()) {
                    // copy the "partial" packet from the overflow buffer into the outgoing buffer
                    std::copy_n(overflowBuffer.data(),
                                iterationTransferredBytes,
                                reinterpret_cast<unsigned char*>(const_cast<typename std::remove_cv<BufferValueType>::type*>(buffer)) + transferredBytes);
                }

                // increment the total count of transferred bytes
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

} // namespace dp
#endif // _WRAP_LIBUSB_HPP_
