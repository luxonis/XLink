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
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include "wrap_libusb_details.hpp"

namespace dai {

///////////////////////////////
// Helper functions and macros
///////////////////////////////

// checks for C++14, when not available then include definitions
#if WRAP_CPLUSPLUS >= 201402L
    #include <utility>
    #define WRAP_EXCHANGE std::exchange
#else
    // older than C++14
    #define WRAP_EXCHANGE exchange11
    #include <type_traits>
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

// template function that can call any libusb function passed to it
// function parameters are passed as variadic template arguments
template<mvLog_t Loglevel = MVLOG_ERROR, bool Throw = true, typename Func, typename... Args>
inline auto call_log_throw(const char* func_within, const int line_number, Func&& func, Args&&... args) -> decltype(func(std::forward<Args>(args)...)) {
    const auto rcNum = func(std::forward<Args>(args)...);
    if (rcNum < 0) {
        logprintf(MVLOGLEVEL(MVLOG_UNIT_NAME), Loglevel, func_within, line_number, "dai::libusb failed %s(): %s", func_within, libusb_strerror(static_cast<int>(rcNum)));
        if (Throw)
            throw usb_error(static_cast<int>(rcNum));
    }
    return rcNum;
}
#define CALL_LOG_ERROR_THROW(...) call_log_throw(__func__, __LINE__, __VA_ARGS__)

/*
// macros to check for libusb resource errors, log, and possibly throw
#define WRAP_CHECK_LOG_THROW(ERRCODE, LOGPHRASE)                                                     \
    if ((ERRCODE) < 0) {                                                                             \
        mvLog(MVLOG_ERROR, "Failed " #LOGPHRASE ": %s", libusb_strerror(static_cast<int>(ERRCODE))); \
        throw usb_error(static_cast<int>(ERRCODE));                                                  \
    }
#define WRAP_CHECK_LOG(ERRCODE, LOGPHRASE)                                                           \
    if ((ERRCODE) < 0) {                                                                             \
        mvLog(MVLOG_ERROR, "Failed " #LOGPHRASE ": %s", libusb_strerror(static_cast<int>(ERRCODE))); \
    }

// macros to call libusb resource functions, return results in a var, check for errors, log, and possibly throw
#define WRAP_CALL_LOG_THROW(FUNCNAME, ...) \
    const auto rcNum = FUNCNAME(__VA_ARGS__);    \
    WRAP_CHECK_LOG_THROW(rcNum, FUNCNAME(__VA_ARGS__))
#define WRAP_CALL_LOG(FUNCNAME, ...)    \
    const auto rcNum = FUNCNAME(__VA_ARGS__); \
    WRAP_CHECK_LOG(rcNum, FUNCNAME(__VA_ARGS__))
*/

///////////////////////////////
// libusb resource wrappers
///////////////////////////////

// wraps libusb_device and automatically libusb_unref_device() on destruction
using usb_device = unique_resource_ptr<libusb_device, libusb_unref_device>;

// wraps libusb_context and automatically libusb_exit() on destruction
// using context = unique_resource_ptr<libusb_context, libusb_exit>;

// device_list container class wrapper for libusb_get_device_list()
// Use constructors to create an instance as the primary approach.
// Use device_list::create() to create an instance via factory approach.
class device_list;
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
    size_type countDevices{};
    pointer deviceList{};
};

// wrapper for libusb_get_config_descriptor()
class config_descriptor : public unique_resource_ptr<libusb_config_descriptor, libusb_free_config_descriptor> {
public:
    using unique_resource_ptr<libusb_config_descriptor, libusb_free_config_descriptor>::unique_resource_ptr;

    config_descriptor(libusb_device* dev, uint8_t configIndex) {
        CALL_LOG_ERROR_THROW(libusb_get_config_descriptor, dev, configIndex, dai::out_param(*this));
    }
};

// wrapper for libusb_open(), libusb_wrap_sys_device()
class device_handle : public unique_resource_ptr<libusb_device_handle, libusb_close> {
private:
    using _base = unique_resource_ptr<libusb_device_handle, libusb_close>;
    std::vector<int> claimedInterfaces{};

public:
    using unique_resource_ptr<libusb_device_handle, libusb_close>::unique_resource_ptr;

    device_handle(libusb_device* dev) {
        CALL_LOG_ERROR_THROW(libusb_open, dev, dai::out_param(*static_cast<_base*>(this)));
    }
    device_handle(libusb_context *ctx, intptr_t sys_dev_handle) {
        CALL_LOG_ERROR_THROW(libusb_wrap_sys_device, ctx, sys_dev_handle, dai::out_param(*static_cast<_base*>(this)));
    }
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

    // wrapper for libusb_set_configuration()
    void set_configuration(int configuration) {
        CALL_LOG_ERROR_THROW(libusb_set_configuration, get(), configuration);
    }

    // wrapper for libusb_get_configuration()
    template<mvLog_t Loglevel = MVLOG_ERROR>
    int get_configuration() {
        int configuration{};
        call_log_throw<Loglevel>(__func__, __LINE__, libusb_get_configuration, get(), &configuration);
        return configuration;
    }
};

/*
// https://stackoverflow.com/questions/57622162/get-function-arguments-type-as-tuple
// https://stackoverflow.com/questions/36612596/tuple-to-parameter-pack
#include <tuple>
template<typename Func>
class function_traits;

// specialization for functions
template<typename Func, typename... FuncArgs>
class function_traits<Func (FuncArgs...)> {
public:
    using arguments = ::std::tuple<FuncArgs...>;
    using numargs = sizeof(...FuncArgs);
};

using foo_arguments = function_traits<decltype(libusb_get_config_descriptor)>::arguments;
using foo_argsize = function_traits<decltype(libusb_get_config_descriptor)>::numargs;

*/


} // namespace libusb

#undef WRAP_EXCHANGE
#undef CALL_LOG_ERROR_THROW

} // namespace dai
#endif // _WRAP_LIBUSB_HPP_
