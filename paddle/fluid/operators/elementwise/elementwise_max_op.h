/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <cmath>
#include "paddle/fluid/operators/elementwise/elementwise_op.h"

namespace paddle {
namespace operators {

template <typename DeviceContext, typename T>
class ElementwiseMaxKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    auto* x = ctx.Input<framework::LoDTensor>("X");
    auto* y = ctx.Input<framework::LoDTensor>("Y");
    auto* z = ctx.Output<framework::LoDTensor>("Out");

    z->mutable_data<T>(ctx.GetPlace());
    int axis = ctx.Attr<int>("axis");
    ElementwiseComputeEx<MaxFunctor<T>, DeviceContext, T>(ctx, x, y, axis,
                                                          MaxFunctor<T>(), z);
  }
};

template <typename DeviceContext, typename T>
class ElementwiseFMaxKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    auto* x = ctx.Input<framework::LoDTensor>("X");
    auto* y = ctx.Input<framework::LoDTensor>("Y");
    auto* z = ctx.Output<framework::LoDTensor>("Out");

    z->mutable_data<T>(ctx.GetPlace());
    int axis = ctx.Attr<int>("axis");
    ElementwiseComputeEx<FMaxFunctor<T>, DeviceContext, T>(ctx, x, y, axis,
                                                           FMaxFunctor<T>(), z);
  }
};

template <typename T>
struct MaxGradDx {
  HOSTDEVICE T operator()(T x, T y, T out, T dout) const {
    return dout * static_cast<T>(x > y);
  }
};

template <typename T>
struct MaxGradDy {
  HOSTDEVICE T operator()(T x, T y, T out, T dout) const {
    return dout * static_cast<T>(x <= y);
  }
};

template <typename DeviceContext, typename T>
typename std::enable_if<
    std::is_same<DeviceContext, platform::CPUDeviceContext>::value>::type
ElementwiseMaxGrad(const framework::ExecutionContext& ctx,
                   const framework::Tensor* x, const framework::Tensor* y,
                   const framework::Tensor* out, const framework::Tensor* dout,
                   framework::Tensor* dx, framework::Tensor* dy) {
  int axis = ctx.Attr<int>("axis");
  ElemwiseGradCompute<DeviceContext, T, MaxGradDx<T>, MaxGradDy<T>>(
      ctx, *x, *y, *out, *dout, axis, dx, dy, MaxGradDx<T>(), MaxGradDy<T>());
}

#if defined(PADDLE_WITH_CUDA) || defined(PADDLE_WITH_HIP)
template <typename DeviceContext, typename T>
typename std::enable_if<
    std::is_same<DeviceContext, platform::CUDADeviceContext>::value>::type
ElementwiseMaxGrad(const framework::ExecutionContext& ctx,
                   const framework::Tensor* x, const framework::Tensor* y,
                   const framework::Tensor* out, const framework::Tensor* dout,
                   framework::Tensor* dx, framework::Tensor* dy);
#endif

template <typename DeviceContext, typename T>
class ElementwiseMaxGradKernel : public ElemwiseGradKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    ElemwiseGradKernel<T>::Compute(ctx);
    using Tensor = framework::Tensor;

    auto* x = ctx.Input<Tensor>("X");
    auto* y = ctx.Input<Tensor>("Y");
    auto* dout = ctx.Input<Tensor>(framework::GradVarName("Out"));
    auto* out = dout;  // out is not necessary
    auto* dx = ctx.Output<Tensor>(framework::GradVarName("X"));
    auto* dy = ctx.Output<Tensor>(framework::GradVarName("Y"));

    ElementwiseMaxGrad<DeviceContext, T>(ctx, x, y, out, dout, dx, dy);
  }
};

template <typename T>
struct FMaxGradDx {
  HOSTDEVICE T operator()(T x, T y, T out, T dout) const {
    return dout * static_cast<T>((x >= y) || isnan(y));
  }
};

template <>
struct FMaxGradDx<paddle::platform::float16> {
  HOSTDEVICE paddle::platform::float16 operator()(
      paddle::platform::float16 x, paddle::platform::float16 y,
      paddle::platform::float16 out, paddle::platform::float16 dout) const {
    return dout * static_cast<paddle::platform::float16>(
                      (x >= y) || paddle::platform::isnan(y));
  }
};

template <>
struct FMaxGradDx<int> {
  HOSTDEVICE int operator()(int x, int y, int out, int dout) const {
    return dout * static_cast<int>((x >= y));
  }
};

template <>
struct FMaxGradDx<int64_t> {
  HOSTDEVICE int64_t operator()(int64_t x, int64_t y, int64_t out,
                                int64_t dout) const {
    return dout * static_cast<int64_t>((x >= y));
  }
};

template <typename T>
struct FMaxGradDy {
  HOSTDEVICE T operator()(T x, T y, T out, T dout) const {
    return dout * static_cast<T>(!((x >= y) || isnan(y)));
  }
};

template <>
struct FMaxGradDy<paddle::platform::float16> {
  HOSTDEVICE paddle::platform::float16 operator()(
      paddle::platform::float16 x, paddle::platform::float16 y,
      paddle::platform::float16 out, paddle::platform::float16 dout) const {
    return dout * static_cast<paddle::platform::float16>(
                      !((x >= y) || paddle::platform::isnan(y)));
  }
};

template <>
struct FMaxGradDy<int64_t> {
  HOSTDEVICE int64_t operator()(int64_t x, int64_t y, int64_t out,
                                int64_t dout) const {
    return dout * static_cast<int64_t>(!((x >= y)));
  }
};

template <>
struct FMaxGradDy<int> {
  HOSTDEVICE int operator()(int x, int y, int out, int dout) const {
    return dout * static_cast<int>(!((x >= y)));
  }
};

template <typename DeviceContext, typename T>
class ElementwiseFMaxGradKernel : public ElemwiseGradKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    ElemwiseGradKernel<T>::Compute(ctx);
    using Tensor = framework::Tensor;

    auto* x = ctx.Input<Tensor>("X");
    auto* y = ctx.Input<Tensor>("Y");
    auto* dout = ctx.Input<Tensor>(framework::GradVarName("Out"));
    auto* dx = ctx.Output<Tensor>(framework::GradVarName("X"));
    auto* dy = ctx.Output<Tensor>(framework::GradVarName("Y"));
    auto* out = dout;  // Fake out, not used
    int axis = ctx.Attr<int>("axis");
    ElemwiseGradCompute<DeviceContext, T, FMaxGradDx<T>, FMaxGradDy<T>>(
        ctx, *x, *y, *out, *dout, axis, dx, dy, FMaxGradDx<T>(),
        FMaxGradDy<T>());
  }
};
}  // namespace operators
}  // namespace paddle
