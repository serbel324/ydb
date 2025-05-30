//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___TYPE_TRAITS_IS_VOID_H
#define _LIBCPP___TYPE_TRAITS_IS_VOID_H

#include <__config>
#include <__type_traits/integral_constant.h>
#include <__type_traits/remove_cv.h>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

_LIBCPP_BEGIN_NAMESPACE_STD

template <class _Tp>
#if __has_builtin(__remove_cv)
struct _LIBCPP_TEMPLATE_VIS is_void : _BoolConstant<__is_same(__remove_cv(_Tp), void)> {};
#else
struct _LIBCPP_TEMPLATE_VIS is_void : _BoolConstant<__is_same(__remove_cv_t<_Tp>, void)> {};
#endif

#if _LIBCPP_STD_VER >= 17
template <class _Tp>
#   if __has_builtin(__remove_cv)
inline constexpr bool is_void_v = __is_same(__remove_cv(_Tp), void);
#   else
inline constexpr bool is_void_v = __is_same(__remove_cv_t<_Tp>, void);
#   endif
#endif

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___TYPE_TRAITS_IS_VOID_H
