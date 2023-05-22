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
#include <iterator>
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

// checks for C++14, when not available then include definitions
#if WRAP_CPLUSPLUS >= 201402L
    #define WRAP_EXCHANGE std::exchange
#else
    // older than C++14
    #define WRAP_EXCHANGE exchange11
    template <class _Ty, class _Other = _Ty>
    _Ty exchange11(_Ty& _Val, _Other&& _New_val) noexcept(
        std::is_nothrow_move_constructible<_Ty>::value && std::is_nothrow_assignable<_Ty&, _Other>::value) {
        _Ty _Old_val = static_cast<_Ty&&>(_Val);
        _Val         = static_cast<_Other&&>(_New_val);
        return _Old_val;
    }
#endif

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
    explicit usb_error(const int libusbErrorCode) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), libusb_strerror(libusbErrorCode)} {}
    usb_error(const int libusbErrorCode, const std::string& what) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), what} {}
    usb_error(const int libusbErrorCode, const char* what) noexcept :
        std::system_error{std::error_code(libusbErrorCode, std::system_category()), what} {}
};

// tag dispatch for throwing or not throwing exceptions
inline void throw_conditional(int errCode, std::true_type) {
    throw usb_error(errCode);
}
inline void throw_conditional(int, std::false_type) {
    // do nothing
}

// template function that can call any libusb function passed to it
// function parameters are passed as variadic template arguments
// caution: when Throw=false, a negative return code can be returned on error; always handle such scenarios
template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, typename Func, typename... Args>
inline auto call_log_throw(const char* func_within, const int line_number, Func&& func, Args&&... args) noexcept(!Throw) -> decltype(func(std::forward<Args>(args)...)) {
    const auto rcNum = func(std::forward<Args>(args)...);
    if (rcNum < 0) {
        logprintf(MVLOGLEVEL(MVLOG_UNIT_NAME), Loglevel, func_within, line_number, "dai::libusb failed %s(): %s", func_within, libusb_strerror(static_cast<int>(rcNum)));
        throw_conditional(static_cast<int>(rcNum), std::integral_constant<bool, Throw>{});
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
        countDevices{WRAP_EXCHANGE(other.countDevices, 0)},
        deviceList{WRAP_EXCHANGE(other.deviceList, nullptr)} {};
    device_list& operator=(device_list&& other) noexcept {
        if (this == &other)
            return *this;
        countDevices = WRAP_EXCHANGE(other.countDevices, 0);
        deviceList = WRAP_EXCHANGE(other.deviceList, nullptr);
        return *this;
    }

    explicit device_list(libusb_context* context) {
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

    config_descriptor(libusb_device* dev, uint8_t config_index) {
        CALL_LOG_ERROR_THROW(libusb_get_config_descriptor, dev, config_index, dai::out_param(*this));
    }
};

// wrap libusb_device_handle* to allow I/O on device. Create with usb_device::open(), device_handle{libusb_device*}
// or from raw platform pointers with device_handle{libusb_context*, intptr_t}
class device_handle : public unique_resource_ptr<libusb_device_handle, libusb_close> {
private:
    using _base = unique_resource_ptr<libusb_device_handle, libusb_close>;
    std::vector<int> claimedInterfaces{};
    std::array<decltype(libusb_endpoint_descriptor::wMaxPacketSize), 32> maxPacketSize{};

public:
    using unique_resource_ptr<libusb_device_handle, libusb_close>::unique_resource_ptr;

    device_handle(libusb_device* dev) {
        CALL_LOG_ERROR_THROW(libusb_open, dev, dai::out_param(*static_cast<_base*>(this)));
    }

    // wrap a platform-specific system device handle and get a libusb device_handle for it
    // never use libusb_open() on this wrapped handle's underlying device
    device_handle(libusb_context *ctx, intptr_t sys_dev_handle) {
        CALL_LOG_ERROR_THROW(libusb_wrap_sys_device, ctx, sys_dev_handle, dai::out_param(*static_cast<_base*>(this)));
    }

    // copy and move constructors and assignment operators
    device_handle(const device_handle&) = delete;
    device_handle& operator=(const device_handle&) = delete;
    device_handle(device_handle &&other) noexcept :
        _base{WRAP_EXCHANGE<_base, _base>(other, {})},
        claimedInterfaces{WRAP_EXCHANGE(other.claimedInterfaces, {})}
    {}
    device_handle &operator=(device_handle &&other) noexcept {
        if (this != &other) {
            *static_cast<_base*>(this) = WRAP_EXCHANGE<_base, _base>(other, {});
            claimedInterfaces = WRAP_EXCHANGE(other.claimedInterfaces, {});
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

    // wrapper for libusb_claim_interface()
    void claim_interface(int interface_number) {
        if (std::find(claimedInterfaces.begin(), claimedInterfaces.end(), interface_number) != claimedInterfaces.end())
            return;
        CALL_LOG_ERROR_THROW(libusb_claim_interface, get(), interface_number);
        claimedInterfaces.emplace_back(interface_number);
    }

    // wrapper for libusb_release_interface()
    void release_interface(int interface_number) {
        auto candidate = std::find(claimedInterfaces.begin(), claimedInterfaces.end(), interface_number);
        if (candidate == claimedInterfaces.end())
            return;
        CALL_LOG_ERROR_THROW(libusb_release_interface, get(), interface_number);
        claimedInterfaces.erase(candidate);
    }

    // wrapper for libusb_get_configuration()
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    int get_configuration() const {
        int configuration{};
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_configuration, get(), &configuration);
        return configuration;
    }

    // wrapper for libusb_set_configuration()
    // if skip_active_check = true, the current configuration is not checked before setting the new one
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    void set_configuration(int configuration, bool skip_active_check = false) {
        if (!skip_active_check) {
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
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, int Len = 31>
    std::string get_string_descriptor_ascii(uint8_t desc_index) const {
        std::string descriptor(Len + 1, 0);
        const auto result = call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_string_descriptor_ascii, get(), desc_index, (unsigned char*)descriptor.data(), Len + 1);
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
    void set_auto_detach_kernel_driver(bool enable) {
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

    // wrap libusb_get_device()
    usb_device get_device() const;
};

class usb_device : public unique_resource_ptr<libusb_device, libusb_unref_device> {
private:
    using _base = unique_resource_ptr<libusb_device, libusb_unref_device>;

public:
    // inherit base constructors
    using unique_resource_ptr<libusb_device, libusb_unref_device>::unique_resource_ptr;

    // stores a raw libusb_device* pointer, increments its refcount with libusb_ref_device()
    explicit usb_device(pointer p) noexcept : _base{libusb_ref_device(p)} {}

    // delete base constructors that conflict with libusb ref counts
    usb_device(pointer p, const deleter_type &d) = delete;
    usb_device(pointer p, deleter_type &&d) = delete;

    // generate a device_handle with libusb_open() to enable i/o on the device
    device_handle open() const {
        device_handle handle;
        CALL_LOG_ERROR_THROW(libusb_open, get(), dai::out_param(handle));
        return handle;
    }

    // start managing the new libusb_device* with libusb_ref_device()
    // then remove the old libusb_device* and decrement its ref count
    // No exceptions are thrown. No errors are logged.
    void reset(pointer p = pointer{}) noexcept {
        static_cast<_base*>(this)->reset(libusb_ref_device(p));
    }

    // wrapper for libusb_get_config_descriptor()
    config_descriptor get_config_descriptor(uint8_t config_index) const {
        config_descriptor descriptor;
        CALL_LOG_ERROR_THROW(libusb_get_config_descriptor, get(), config_index, dai::out_param(descriptor));
        return descriptor;
    }

    // wrapper for libusb_get_device_descriptor()
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true>
    libusb_device_descriptor get_device_descriptor() const {
        libusb_device_descriptor descriptor{};
        call_log_throw<Loglevel, Throw>(__func__, __LINE__, libusb_get_device_descriptor, get(), &descriptor);
        return descriptor;
    }

    // wrapper for libusb_get_bus_number()
    uint8_t get_bus_number() const {
        return CALL_LOG_ERROR_THROW(libusb_get_bus_number, get());
    }

    // wrapper for libusb_get_port_numbers()
    // template param Len is the maximum possible number of port-numbers read from usb
    // the return vector is resized to the actual number read
    template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, int Len = 7>
    std::vector<uint8_t> get_port_numbers() const {
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

// wrap libusb_get_device()
inline usb_device device_handle::get_device() const {
    return usb_device{libusb_get_device(get())};
}

} // namespace libusb

#undef WRAP_EXCHANGE
#undef CALL_LOG_ERROR_THROW

} // namespace dai
#endif // _WRAP_LIBUSB_HPP_
