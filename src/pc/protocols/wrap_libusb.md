# `dp::libusb` - C++ wrap for libusb-1.0

Copyright 2023 Dale Phurrough  
Licensed under the Apache License, Version 2.0 http://www.apache.org/licenses/LICENSE-2.0

## Overview

`dp::libusb` wraps the C apis of [libusb](https://libusb.info/) with C++ unique resource classes
that follow RAII semantics for ownership and automatic disposal of resources. A simple class
`dp::unique_resource_ptr` derives from `std::unique_ptr` with an additional template
parameter for a C-function pointer to dispose the resource. Most `dp::libusb` classes derive
from this base resource type.

`dp::libusb` is currently focused on the needs of XLink; the protocol to communicate with
Intel Movidius/Myriad devices. A `std::shared_ptr` base for resources was considered for
some use cases. It was not implemented due to the limited needs of XLink.

Many raw libusb types, e.g. `libusb_device_descriptor`, are used in their simple POD form.
Some libusb resources are not implemented, e.g. asynchronous device I/O.

## Rules

Most behaviors of `dp::libusb` resources derive from `std::unique_ptr`.

1. `dp::libusb` resources permit empty/nullptr contents.
   `dp::libusb::device_handle handle{nullptr};` and `handle.reset()` are both valid code.
2. Behavior is undefined when code uses or operates on an empty resource.
   `handle.claim_interface(1);` and `*handle` would both likely crash your application.
3. Behavior is undefined when code transforms an empty resource.
   `libusb_device* device = nullptr; dp::libusb::device_handle handle{device};` will likely crash your application.
4. `dp::libusb::device_list` follows the behaviors and apis of STL containers. It can
   be used in places an STL container can be used.
5. Raw libusb return codes for failures are transformed into `dp::libusb::usb_error` exceptions
   with error text sourced from `libusb_strerror()`.
6. Raw resource pointers can be retrieved with `get()` to use with libusb C apis.

## Types Overview and Hierarchy

mermaid tbd

* `usb_context` enhances raw `libusb_context*`
* `device_list` container exposes iterators to raw `libusb_device*`
* `usb_device` enhances raw `libusb_device*`
* `device_handle` enhances raw `libusb_device_handle*`
* `config_descriptor` enhances raw `libusb_config_descriptor*`
* remaining libusb types are used unchanged
