// project
#define MVLOG_UNIT_NAME xLinkUsb

// libraries
#ifdef XLINK_LIBUSB_LOCAL
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace dai {

// TEMPLATE WRAPPERS
template<typename Resource, void(*Dispose)(Resource*)>
struct unique_resource_deleter {
    inline void operator()(Resource* const ptr) noexcept {
        Dispose(ptr);
    }
};
template<typename Resource, void(*Dispose)(Resource*)>
using unique_resource_ptr = std::unique_ptr<Resource, unique_resource_deleter<Resource, Dispose>>;


namespace libusb {

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

// wraps libusb_device and automatically libusb_unref_device() on destruction
using usb_device = unique_resource_ptr<libusb_device, libusb_unref_device>;

//using context = unique_resource_ptr<libusb_context, libusb_exit>;
//using config_descriptor = unique_resource_ptr<libusb_config_descriptor, libusb_free_config_descriptor>;

// device_list container class wrapper for libusb_get_device_list()
// Use constructors to create an instance as the primary approach.
// Use device_list::create() to create an instance via factory approach.
class device_list;
class device_list {
public:
    // default constructors, destructor, copy, move
    device_list() = default; // could be private by create() using `new device_list()`
    ~device_list() noexcept {
        if (deviceList != nullptr) {
            libusb_free_device_list(deviceList, 1);
        }
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

    explicit device_list(libusb_context* context) {
        const auto rcNum = libusb_get_device_list(context, &deviceList);
        if (rcNum < 0 || deviceList == nullptr) {
            mvLog(MVLOG_ERROR, "Unable to get USB device list: %s", libusb_strerror(rcNum));
            throw usb_error(static_cast<int>(rcNum));
        }
        countDevices = static_cast<size_type>(rcNum);
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
        return std::make_reverse_iterator(end());
    }
    const_reverse_iterator rbegin() const noexcept {
        return std::make_reverse_iterator(cend());
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
        return std::make_reverse_iterator(begin());
    }
    const_reverse_iterator rend() const noexcept {
        return std::make_reverse_iterator(cbegin());
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
        libusb_config_descriptor *resource{};
        const auto rcNum = libusb_get_config_descriptor(dev, configIndex, &resource);
        if (rcNum < 0) {
            mvLog(MVLOG_ERROR, "Unable to get libusb config_descriptor: %s", libusb_strerror(rcNum));
            throw usb_error(rcNum);
        }
        reset(resource);
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
        libusb_device_handle *resource{};
        const auto rcNum = libusb_open(dev, &resource);
        if (rcNum < 0) {
            mvLog(MVLOG_ERROR, "Unable to get libusb device_handle: %s", libusb_strerror(rcNum));
            throw usb_error(rcNum);
        }
        _base::reset(resource);
    }
    device_handle(libusb_context *ctx, intptr_t sys_dev_handle) {
        libusb_device_handle *resource{};
        const auto rcNum = libusb_wrap_sys_device(ctx, sys_dev_handle, &resource);
        if (rcNum < 0) {
            mvLog(MVLOG_ERROR, "Unable to get libusb device_handle: %s", libusb_strerror(rcNum));
            throw usb_error(rcNum);
        }
        _base::reset(resource);
    }
    device_handle(const device_handle&) = delete;
    device_handle& operator=(const device_handle&) = delete;
    device_handle(device_handle &&other) noexcept :
        _base{std::exchange<_base, _base>(other, {})},
        claimedInterfaces{std::exchange(other.claimedInterfaces, {})}
    {}
    device_handle &operator=(device_handle &&other) noexcept {
        if (this != &other) {
            _base::operator=(std::exchange<_base, _base>(other, {}));
            claimedInterfaces = std::exchange(other.claimedInterfaces, {});
        }
        return *this;
    }
    ~device_handle() noexcept {
        reset();
    }

    // release all managed objects with libusb_release_interface() and libusb_close()
    // No exceptions are thrown. Errors are logged.
    void reset() noexcept {
        for (const auto interfaceNumber : claimedInterfaces) {
            const auto rcNum = libusb_release_interface(get(), interfaceNumber);
            if (rcNum < 0) {
                mvLog(MVLOG_ERROR, "Unable to release libusb interface %d: %s", interfaceNumber, libusb_strerror(rcNum));
            }
        }
        _base::reset();
    }

    // release ownership of the managed libusb_device_handle and all device interfaces
    // caller is responsible for calling libusb_release_interface() and libusb_close()
    device_handle::pointer release() noexcept {
        claimedInterfaces.clear();
        return _base::release();
    }

    // wrapper for libusb_claim_interface()
    void claim_interface(int interface_number) {
        if (std::find(claimedInterfaces.begin(), claimedInterfaces.end(), interface_number) != claimedInterfaces.end())
            return;
        const auto rcNum = libusb_claim_interface(get(), interface_number);
        if (rcNum < 0) {
            mvLog(MVLOG_ERROR, "Unable to claim libusb interface %d: %s", interface_number, libusb_strerror(rcNum));
            throw usb_error(rcNum);
        }
        claimedInterfaces.emplace_back(interface_number);
    }

    // wrapper for libusb_release_interface()
    void release_interface(int interface_number) {
        auto candidate = std::find(claimedInterfaces.begin(), claimedInterfaces.end(), interface_number);
        if (candidate == claimedInterfaces.end())
            return;
        const auto rcNum = libusb_release_interface(get(), interface_number);
        if (rcNum < 0) {
            mvLog(MVLOG_ERROR, "Unable to release libusb interface %d: %s", interface_number, libusb_strerror(rcNum));
            throw usb_error(rcNum);
        }
        claimedInterfaces.erase(candidate);
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
} // namespace dai
