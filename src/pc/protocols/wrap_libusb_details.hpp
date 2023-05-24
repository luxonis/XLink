//*********************************************************
//    Portions of this code are from the Microsoft WIL project.
//    Copyright (c) Microsoft. All rights reserved.
//    This code is licensed under the MIT License.
//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
//    ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
//    TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
//    PARTICULAR PURPOSE AND NONINFRINGEMENT.
//
//*********************************************************

#ifndef _WRAP_LIBUSB_DETAILS_HPP_
#define _WRAP_LIBUSB_DETAILS_HPP_


// _MSVC_LANG is the more accurate way to get the C++ version in MSVC
#if defined(_MSVC_LANG) && (_MSVC_LANG > __cplusplus)
#define WRAP_CPLUSPLUS _MSVC_LANG
#else
#define WRAP_CPLUSPLUS __cplusplus
#endif

#if WRAP_CPLUSPLUS >= 201703L
    #define WRAP_NODISCARD [[nodiscard]]
#else
    #define WRAP_NODISCARD
#endif

#include <cassert>
#include <type_traits>
#include <utility>

namespace dai {
    // subset of WIL that defines out parameters for smart pointers
    // https://github.com/microsoft/wil/wiki/RAII-resource-wrappers#wilout_param

    namespace details
    {
        // checks for C++14, when not available then include definitions
        #if WRAP_CPLUSPLUS >= 201402L
            using std::exchange;
        #else
            // implements std::exchange for compilers older than C++14
            template <class _Ty, class _Other = _Ty>
            _Ty exchange(_Ty& _Val, _Other&& _New_val) noexcept(
                std::is_nothrow_move_constructible<_Ty>::value && std::is_nothrow_assignable<_Ty&, _Other>::value) {
                _Ty _Old_val = static_cast<_Ty&&>(_Val);
                _Val         = static_cast<_Other&&>(_New_val);
                return _Old_val;
            }
        #endif

        //! Type traits class that identifies the inner type of any smart pointer.
        template <typename Ptr>
        struct smart_pointer_details
        {
            typedef typename Ptr::pointer pointer;
        };

        template <typename T>
        struct out_param_t
        {
            typedef typename smart_pointer_details<T>::pointer pointer;
            T &wrapper;
            pointer pRaw;
            bool replace = true;

            out_param_t(T &output) :
                wrapper(output),
                pRaw(nullptr)
            {
            }

            out_param_t(out_param_t&& other) noexcept :
                wrapper(other.wrapper),
                pRaw(other.pRaw)
            {
                assert(other.replace);
                other.replace = false;
            }

            operator pointer*()
            {
                assert(replace);
                return &pRaw;
            }

            operator pointer&()
            {
                assert(replace);
                return pRaw;
            }

            ~out_param_t()
            {
                if (replace)
                {
                    wrapper.reset(pRaw);
                }
            }

            out_param_t(out_param_t const &other) = delete;
            out_param_t &operator=(out_param_t const &other) = delete;
        };

        template <typename Tcast, typename T>
        struct out_param_ptr_t
        {
            typedef typename smart_pointer_details<T>::pointer pointer;
            T &wrapper;
            pointer pRaw;
            bool replace = true;

            out_param_ptr_t(T &output) :
                wrapper(output),
                pRaw(nullptr)
            {
            }

            out_param_ptr_t(out_param_ptr_t&& other) noexcept :
                wrapper(other.wrapper),
                pRaw(other.pRaw)
            {
                assert(other.replace);
                other.replace = false;
            }

            operator Tcast()
            {
                assert(replace);
                return reinterpret_cast<Tcast>(&pRaw);
            }

            ~out_param_ptr_t()
            {
                if (replace)
                {
                    wrapper.reset(pRaw);
                }
            }

            out_param_ptr_t(out_param_ptr_t const &other) = delete;
            out_param_ptr_t &operator=(out_param_ptr_t const &other) = delete;
        };
    } // details

      /** Use to retrieve raw out parameter pointers into smart pointers that do not support the '&' operator.
      This avoids multi-step handling of a raw resource to establish the smart pointer.
      Example: `GetFoo(out_param(foo));` */
    template <typename T>
    details::out_param_t<T> out_param(T& p)
    {
        return details::out_param_t<T>(p);
    }

    /** Use to retrieve raw out parameter pointers (with a required cast) into smart pointers that do not support the '&' operator.
    Use only when the smart pointer's &handle is not equal to the output type a function requires, necessitating a cast.
    Example: `dai::out_param_ptr<PSECURITY_DESCRIPTOR*>(securityDescriptor)` */
    template <typename Tcast, typename T>
    details::out_param_ptr_t<Tcast, T> out_param_ptr(T& p)
    {
        return details::out_param_ptr_t<Tcast, T>(p);
    }

}

#undef WRAP_CPLUSPLUS
#undef WRAP_NODISCARD
#endif // _WRAP_LIBUSB_DETAILS_HPP_
