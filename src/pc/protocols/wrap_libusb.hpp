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

static const char *xlink_libusb_strerror(ssize_t);

// TEMPLATE WRAPPERS
template<typename T, void(*FreeFunc)(T*)>
struct unique_resource_deleter {
    inline void operator()(T* const ptr) {
        FreeFunc(ptr);
    }
};
template<typename T, void(*FreeFunc)(T*)>
using unique_resource_ptr = std::unique_ptr<T, unique_resource_deleter<T, FreeFunc>>;

// EXAMPLE LIBUSB WRAPPERS
void libusb_free_device_list_unref(libusb_device *list[]) {
    libusb_free_device_list((libusb_device **)list, 1);
}
using unique_libusb_context = unique_resource_ptr<libusb_context, libusb_exit>;
using unique_libusb_device_list = unique_resource_ptr<libusb_device*, libusb_free_device_list_unref>;


// LibusbDeviceList container class wrapper for libusb_get_device_list()
// Use LibusbDeviceList::create() to create an instance via the factory approach.
class LibusbDeviceList;
class LibusbDeviceList {
public:
    static std::unique_ptr<LibusbDeviceList> create(libusb_context* context) noexcept {
        static_assert(std::is_nothrow_constructible_v<LibusbDeviceList>, "LibusbDeviceList() errantly throws");
        auto result = std::make_unique<LibusbDeviceList>();
        if (result) {
            //const auto result = std::unique_ptr<LibusbDeviceList>(new LibusbDeviceList());
            const auto rcNum = libusb_get_device_list(context, &result->deviceList);
            if (rcNum < 0 || result->deviceList == nullptr) {
                mvLog(MVLOG_ERROR, "Unable to get USB device list: %s", xlink_libusb_strerror(rcNum));
                result.reset();
            }
            else {
                result->countDevices = static_cast<size_type>(rcNum);
            }
        }
        else {
            mvLog(MVLOG_ERROR, "Unable to allocate memory for USB device list");
        }
        return result;
    }

    // default constructors, destructor, copy, move
    LibusbDeviceList() = default; // could be private by create() using `new LibusbDeviceList()`
    ~LibusbDeviceList() noexcept {
        if (deviceList != nullptr) {
            libusb_free_device_list(deviceList, 1);
        }
    }
    LibusbDeviceList(const LibusbDeviceList&) = delete;
    LibusbDeviceList& operator=(const LibusbDeviceList&) = delete;
    LibusbDeviceList(LibusbDeviceList&& other) noexcept :
        countDevices{std::exchange(other.countDevices, 0)},
        deviceList{std::exchange(other.deviceList, nullptr)} {};
    LibusbDeviceList& operator=(LibusbDeviceList&& other) noexcept {
        if (this == &other)
            return *this;
        countDevices = std::exchange(other.countDevices, 0);
        deviceList = std::exchange(other.deviceList, nullptr);
        return *this;
    }

    explicit LibusbDeviceList(libusb_context* context) {
        const auto rcNum = libusb_get_device_list(context, &deviceList);
        if (rcNum < 0 || deviceList == nullptr) {
            mvLog(MVLOG_ERROR, "Unable to get USB device list: %s", xlink_libusb_strerror(rcNum));
            throw std::system_error(std::error_code(static_cast<int>(rcNum), std::system_category()), std::string("Unable to get USB device list: ").append(xlink_libusb_strerror(rcNum)));
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
            throw std::out_of_range("LibusbDeviceList::at");
        }
        return *(begin() + index);
    }
    const_reference at(size_type index) const {
        if (index >= size()) {
            throw std::out_of_range("LibusbDeviceList::at");
        }
        return *(cbegin() + index);
    }

private:
    size_type countDevices{0};
    pointer deviceList{nullptr};
};
