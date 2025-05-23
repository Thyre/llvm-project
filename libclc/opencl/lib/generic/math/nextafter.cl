//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include <clc/clcmacro.h>
#include <clc/math/clc_nextafter.h>
#include <clc/opencl/clc.h>

_CLC_DEFINE_BINARY_BUILTIN_NO_SCALARIZE(float, nextafter, __clc_nextafter,
                                        float, float)

#ifdef cl_khr_fp64

#pragma OPENCL EXTENSION cl_khr_fp64 : enable

_CLC_DEFINE_BINARY_BUILTIN_NO_SCALARIZE(double, nextafter, __clc_nextafter,
                                        double, double)

#endif

#ifdef cl_khr_fp16

#pragma OPENCL EXTENSION cl_khr_fp16 : enable

_CLC_DEFINE_BINARY_BUILTIN_NO_SCALARIZE(half, nextafter, __clc_nextafter, half,
                                        half)

#endif
