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

#include "wrap_libusb.hpp"

std::mutex dp::libusb::device_list::mtx;

// definition outside class to workaround Clang constexpr failures
constexpr int dp::libusb::device_handle::DEFAULT_CHUNK_SIZE;
constexpr int dp::libusb::device_handle::DEFAULT_CHUNK_SIZE_USB1;
constexpr decltype(libusb_endpoint_descriptor::wMaxPacketSize) dp::libusb::device_handle::DEFAULT_MAX_PACKET_SIZE;
constexpr std::array<decltype(libusb_endpoint_descriptor::wMaxPacketSize), 32> dp::libusb::device_handle::DEFAULT_MAX_PACKET_ARRAY;
