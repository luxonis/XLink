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

    explicit out_param_t(T& output) noexcept : wrapper(output), pRaw(nullptr) {}

    out_param_t(out_param_t&& other) noexcept(noexcept(std::declval<T>().reset(std::declval<pointer>()))) : wrapper(other.wrapper), pRaw(other.pRaw) {
        assert(other.replace);
        other.replace = false;
    }

    operator pointer*() noexcept {
        assert(replace);
        return &pRaw;
    }

    operator pointer&() noexcept {
        assert(replace);
        return pRaw;
    }

    ~out_param_t() noexcept(noexcept(std::declval<T>().reset(std::declval<pointer>()))) {
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

    explicit out_param_ptr_t(T& output) noexcept : wrapper(output), pRaw(nullptr) {}

    out_param_ptr_t(out_param_ptr_t&& other) noexcept(noexcept(std::declval<T>().reset(std::declval<pointer>()))) : wrapper(other.wrapper), pRaw(other.pRaw) {
        assert(other.replace);
        other.replace = false;
    }

    operator Tcast() noexcept {
        assert(replace);
        return reinterpret_cast<Tcast>(&pRaw);
    }

    ~out_param_ptr_t() noexcept(noexcept(std::declval<T>().reset(std::declval<pointer>()))) {
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
details::out_param_t<T> out_param(T& p) noexcept(noexcept(details::out_param_t<T>(p))) {
    return details::out_param_t<T>(p);
}

/** Use to retrieve raw out parameter pointers (with a required cast) into smart pointers that do not support the '&' operator.
Use only when the smart pointer's &handle is not equal to the output type a function requires, necessitating a cast.
Example: `dp::out_param_ptr<PSECURITY_DESCRIPTOR*>(securityDescriptor)` */
template <typename Tcast, typename T>
details::out_param_ptr_t<Tcast, T> out_param_ptr(T& p) noexcept(noexcept(details::out_param_ptr_t<Tcast, T>(p))) {
    return details::out_param_ptr_t<Tcast, T>(p);
}

#if WRAP_CPLUSPLUS < 202002L

// simple implementation of std::span for compilers older than C++20
// dpes not include every feature of C++20 std::span
template <typename T>
class span {
    T* ptr_ = nullptr;
    size_t size_ = 0;
    static constexpr const char* const throwText = "span index out of range";
    static constexpr auto dynamic_extent = static_cast<size_t>(-1);

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
    using reverse_iterator = std::reverse_iterator<iterator>;
    // no constant iterators in c++20 span https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2278r4.html
    //using const_iterator = const_pointer;
    //using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    //// constructors, copy, move, assignment ////

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

    ////////////////////////
    //// element access ////
    ////////////////////////

    pointer data() noexcept {
        return ptr_;
    }
    const_pointer data() const noexcept {
        return ptr_;
    }
    reference operator[](size_type index) noexcept {
        return *(begin() + index);
    }
    const_reference operator[](size_type index) const noexcept {
        return *(begin() + index);
    }
    reference front() noexcept {
        return *begin();
    }
    const_reference front() const noexcept {
        return *begin();
    }
    reference back() noexcept {
        return *(end() - 1);
    }
    const_reference back() const noexcept {
        return *(end() - 1);
    }
    // caution: this is non-standard and not available when compiled with C++20 or newer
    reference at(size_type index) {
        if(index >= size_) {
            throw std::out_of_range(throwText);
        }
        return *(begin() + index);
    }
    // caution: this is non-standard and not available when compiled with C++20 or newer
    const_reference at(size_type index) const {
        if(index >= size_) {
            throw std::out_of_range(throwText);
        }
        return *(begin() + index);
    }

    ///////////////////
    //// iterators ////
    ///////////////////

    iterator begin() const noexcept {
        return ptr_;
    }
    iterator end() const noexcept {
        return ptr_ + size_;
    }
    reverse_iterator rbegin() const noexcept {
        return reverse_iterator(end());
    }
    reverse_iterator rend() const noexcept {
        return reverse_iterator(begin());
    }
    // no constant iterators in c++20 span https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2278r4.html
    //const_iterator cbegin() const noexcept {
    //    return ptr_;
    //}
    //const_iterator cend() const noexcept {
    //    return ptr_ + size_;
    //}
    //const_reverse_iterator crbegin() const noexcept {
    //    return const_reverse_iterator(cend());
    //}
    //const_reverse_iterator crend() const noexcept {
    //    return const_reverse_iterator(cbegin());
    //}

    ///////////////////
    //// observers ////
    ///////////////////

    size_type size() const noexcept {
        return size_;
    }
    size_type size_bytes() const noexcept {
        return size_ * sizeof(T);
    }
    bool empty() const noexcept {
        return size_ == 0;
    }

    //////////////////
    //// subviews ////
    //////////////////

    span<T> subspan(size_type offset, size_type count = dynamic_extent) const noexcept {
        return span<T>(ptr_ + offset, count == dynamic_extent ? size_ - offset : count);
    }

    template<size_t offset, size_t count = dynamic_extent>
    span<T> subspan() const noexcept {
        return span<T>(ptr_ + offset, count == dynamic_extent ? size_ - offset : count);
    }

    span<T> first(size_type count) const noexcept {
        return subspan(0, count);
    }

    template<size_t count>
    span<T> first() const noexcept {
        return subspan<0, count>();
    }

    span<T> last(size_type count) const noexcept {
        return subspan(size_ - count, count);
    }

    template<size_t count>
    span<T> last() const noexcept {
        return span<T>{ptr_ + (size_ - count), count};
    }

    // caution: this is non-standard and not available when compiled with C++20 or newer
    span<const unsigned char> as_bytes() const noexcept {
        return {reinterpret_cast<const unsigned char*>(ptr_), size_ * sizeof(T)};
    }

    // caution: this is non-standard and not available when compiled with C++20 or newer
    span<unsigned char> as_writable_bytes() noexcept {
        return {reinterpret_cast<unsigned char*>(ptr_), size_ * sizeof(T)};
    }
};
#else

#include <span>
using std::span;

#endif

}  // namespace libusb

}  // namespace dp

#undef WRAP_CPLUSPLUS
#endif  // _WRAP_LIBUSB_DETAILS_HPP_
