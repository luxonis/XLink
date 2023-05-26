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

#ifndef _WRAP_LIBUSB_DETAILS_HPP_
#define _WRAP_LIBUSB_DETAILS_HPP_

// _MSVC_LANG is the more accurate way to get the C++ version in MSVC
#if defined(_MSVC_LANG) && (_MSVC_LANG > __cplusplus)
    #define WRAP_CPLUSPLUS _MSVC_LANG
#else
    #define WRAP_CPLUSPLUS __cplusplus
#endif

#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace dp {

namespace libusb {

namespace details {

//*********************************************************
//    Portions of this code are from the Microsoft WIL project
//    https://github.com/microsoft/wil/wiki/RAII-resource-wrappers#wilout_param
//
//    Copyright (c) Microsoft. All rights reserved.
//    This code is licensed under the MIT License.
//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
//    ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//    TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//    PARTICULAR PURPOSE AND NONINFRINGEMENT.
//
//*********************************************************

//! Type traits class that identifies the inner type of any smart pointer.
template <typename Ptr>
struct smart_pointer_details {
    typedef typename Ptr::pointer pointer;
};

template <typename T>
struct out_param_t {
    typedef typename smart_pointer_details<T>::pointer pointer;
    T& wrapper;
    pointer pRaw;
    bool replace = true;

    out_param_t(T& output) : wrapper(output), pRaw(nullptr) {}

    out_param_t(out_param_t&& other) noexcept : wrapper(other.wrapper), pRaw(other.pRaw) {
        assert(other.replace);
        other.replace = false;
    }

    operator pointer*() {
        assert(replace);
        return &pRaw;
    }

    operator pointer&() {
        assert(replace);
        return pRaw;
    }

    ~out_param_t() {
        if(replace) {
            wrapper.reset(pRaw);
        }
    }

    out_param_t(out_param_t const& other) = delete;
    out_param_t& operator=(out_param_t const& other) = delete;
};

template <typename Tcast, typename T>
struct out_param_ptr_t {
    typedef typename smart_pointer_details<T>::pointer pointer;
    T& wrapper;
    pointer pRaw;
    bool replace = true;

    out_param_ptr_t(T& output) : wrapper(output), pRaw(nullptr) {}

    out_param_ptr_t(out_param_ptr_t&& other) noexcept : wrapper(other.wrapper), pRaw(other.pRaw) {
        assert(other.replace);
        other.replace = false;
    }

    operator Tcast() {
        assert(replace);
        return reinterpret_cast<Tcast>(&pRaw);
    }

    ~out_param_ptr_t() {
        if(replace) {
            wrapper.reset(pRaw);
        }
    }

    out_param_ptr_t(out_param_ptr_t const& other) = delete;
    out_param_ptr_t& operator=(out_param_ptr_t const& other) = delete;
};

}  // namespace details

/** Use to retrieve raw out parameter pointers into smart pointers that do not support the '&' operator.
This avoids multi-step handling of a raw resource to establish the smart pointer.
Example: `GetFoo(out_param(foo));` */
template <typename T>
details::out_param_t<T> out_param(T& p) {
    return details::out_param_t<T>(p);
}

/** Use to retrieve raw out parameter pointers (with a required cast) into smart pointers that do not support the '&' operator.
Use only when the smart pointer's &handle is not equal to the output type a function requires, necessitating a cast.
Example: `dp::out_param_ptr<PSECURITY_DESCRIPTOR*>(securityDescriptor)` */
template <typename Tcast, typename T>
details::out_param_ptr_t<Tcast, T> out_param_ptr(T& p) {
    return details::out_param_ptr_t<Tcast, T>(p);
}

#if WRAP_CPLUSPLUS < 202002L

// simple implementation of std::span for compilers older than C++20
// includes non-standard bounds checking
template <typename T>
class span {
   public:
    using element_type = T;
    using value_type = typename std::remove_cv<T>::type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using pointer = element_type*;
    using const_pointer = const element_type*;
    using reference = element_type&;
    using const_reference = const element_type&;
    using iterator = pointer;
    using const_iterator = const_pointer;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    span() noexcept = default;
    ~span() noexcept = default;
    span(const span& other) noexcept : ptr_(other.data()), size_(other.size()) {}
    span& operator=(const span& other) noexcept {
        if(this == &other) return *this;
        ptr_ = other.ptr_;
        size_ = other.size_;
        return *this;
    }
    span(span&& other) = delete;
    span& operator=(span&& other) = delete;

    span(T* ptr, std::size_t size) noexcept : ptr_(ptr), size_(size) {}

    template <std::size_t N>
    span(std::array<T, N>& arr) noexcept : ptr_(arr.data()), size_(N) {}

    template <std::size_t N>
    span(const std::array<T, N>& arr) noexcept : ptr_(arr.data()), size_(N) {}

    template <typename Iterator>
    span(Iterator begin, Iterator end) noexcept : ptr_(&(*begin)), size_(std::distance(begin, end)) {}

    pointer data() noexcept {
        return ptr_;
    }
    const_pointer data() const noexcept {
        return ptr_;
    }
    size_type size() const noexcept {
        return size_;
    }
    size_type size_bytes() const noexcept {
        return size_ * sizeof(T);
    }

    // includes non-standard bounds checking
    reference operator[](size_type index) const {
        if(index >= size_) {
            throw std::out_of_range(throwText);
        }
        return ptr_[index];
    }

    iterator begin() noexcept {
        return ptr_;
    }
    const_iterator begin() const noexcept {
        return ptr_;
    }
    iterator end() noexcept {
        return ptr_ + size_;
    }
    const_iterator end() const noexcept {
        return ptr_ + size_;
    }
    const_iterator cbegin() const noexcept {
        return ptr_;
    }
    const_iterator cend() const noexcept {
        return ptr_ + size_;
    }

    reverse_iterator rbegin() noexcept {
        return reverse_iterator(end());
    }
    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }
    reverse_iterator rend() noexcept {
        return reverse_iterator(begin());
    }
    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }
    const_reverse_iterator crbegin() const noexcept {
        return const_reverse_iterator(cend());
    }
    const_reverse_iterator crend() const noexcept {
        return const_reverse_iterator(cbegin());
    }

    bool empty() const noexcept {
        return size_ == 0;
    }

    // includes non-standard bounds checking
    reference front() const {
        if(size_ == 0) {
            throw std::out_of_range(throwText);
        }
        return ptr_[0];
    }

    // includes non-standard bounds checking
    reference back() const {
        if(size_ == 0) {
            throw std::out_of_range(throwText);
        }
        return ptr_[size_ - 1];
    }

    // includes non-standard bounds checking
    span<T> subspan(size_type offset, size_type count = size_type(-1)) const {
        if(offset > size_) {
            throw std::out_of_range(throwText);
        }
        if(count == size_type(-1)) {
            count = size_ - offset;
        }
        if(offset + count > size_) {
            throw std::out_of_range(throwText);
        }
        return span<T>(ptr_ + offset, count);
    }

    span<const unsigned char> as_bytes() const noexcept {
        return {reinterpret_cast<const unsigned char*>(ptr_), size_ * sizeof(T)};
    }

    span<unsigned char> as_writable_bytes() const noexcept {
        return {reinterpret_cast<unsigned char*>(ptr_), size_ * sizeof(T)};
    }

   private:
    element_type* ptr_ = nullptr;
    const size_type size_ = 0;
    const char* const throwText = "span index out of range";
};
#else

#include <span>
using std::span;

#endif

}  // namespace libusb

}  // namespace dp

#undef WRAP_CPLUSPLUS
#endif  // _WRAP_LIBUSB_DETAILS_HPP_
