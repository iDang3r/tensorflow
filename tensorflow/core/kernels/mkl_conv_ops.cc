/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// See docs in ../ops/nn_ops.cc.
#ifdef INTEL_MKL

#include <string.h>
#include <map>
#include <vector>
#include "tensorflow/core/framework/numeric_op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_slice.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/kernels/ops_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/gtl/array_slice.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/lib/strings/str_util.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/util/padding.h"
#include "tensorflow/core/util/tensor_format.h"

#include "tensorflow/core/common_runtime/mkl_layer_registry.h"
#include "tensorflow/core/util/mkl_util.h"
#include "third_party/mkl/include/mkl_dnn.h"
#include "third_party/mkl/include/mkl_dnn_types.h"

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

template <typename Device, typename T, bool biasEnabled>
class MklConv2DOp : public OpKernel {
 public:
  ~MklConv2DOp() {}

  explicit MklConv2DOp(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("strides", &strides_));
    string data_format;
    OP_REQUIRES_OK(context, context->GetAttr("data_format", &data_format));
    OP_REQUIRES(context, FormatFromString(data_format, &data_format_),
                errors::InvalidArgument("Invalid data format"));
    OP_REQUIRES(context, strides_.size() == 4,
                errors::InvalidArgument("Sliding window strides field must "
                                        "specify 4 dimensions"));

    const int64 stride_n = GetTensorDim(strides_, data_format_, 'N');
    const int64 stride_c = GetTensorDim(strides_, data_format_, 'C');
    OP_REQUIRES(
        context, stride_n == 1 && stride_c == 1,
        errors::InvalidArgument("Current implementation does not yet support "
                                "strides in the batch and depth dimensions."));
    OP_REQUIRES_OK(context, context->GetAttr("padding", &padding_));
  }

  void Compute(OpKernelContext* context) override {
    const Tensor& input = MklGetInput(context, 0);
    MklShape mkl_input_shape;
    GetMklShape(context, 0, &mkl_input_shape);
    bool input_in_mkl_format = mkl_input_shape.IsMklTensor();

    const Tensor& filter = MklGetInput(context, 1);
    MklShape mkl_filter_shape;
    GetMklShape(context, 1, &mkl_filter_shape);
    CHECK(!mkl_filter_shape.IsMklTensor())
        << "Conv filter should not be in MKL Layout";

    if (biasEnabled) {
      const Tensor& bias = MklGetInput(context, 2);
      OP_REQUIRES(context, bias.dims() == 1,
                  errors::InvalidArgument("bias must be 1-dimensional: ",
                                          bias.shape().DebugString()));
    }

    if (!input_in_mkl_format) {
      OP_REQUIRES(context, input.dims() == 4,
                  errors::InvalidArgument("input must be 4-dimensional",
                                          input.shape().DebugString()));
    }

    OP_REQUIRES(context, filter.dims() == 4,
                errors::InvalidArgument("filter must be 4-dimensional: ",
                                        filter.shape().DebugString()));

    for (int i = 0; i < 3; i++) {
      OP_REQUIRES(context, FastBoundsCheck(filter.dim_size(i),
                                           std::numeric_limits<int>::max()),
                  errors::InvalidArgument("filter too large"));
    }

    const int64 input_depth = input_in_mkl_format
                                  ? mkl_input_shape.GetSizes()[2]
                                  : GetTensorDim(input, data_format_, 'C');
    OP_REQUIRES(
        context, input_depth == filter.dim_size(2),
        errors::InvalidArgument("input and filter must have the same depth: ",
                                input_depth, " vs ", filter.dim_size(2)));
    // The last dimension for filter is out_depth.
    const int out_depth = static_cast<int>(filter.dim_size(3));

    // The second dimension for input is rows/height.
    // The first dimension for filter is rows/height.
    const int64 input_rows_raw = input_in_mkl_format
                                     ? mkl_input_shape.GetSizes()[1]
                                     : GetTensorDim(input, data_format_, 'H');
    OP_REQUIRES(context, FastBoundsCheck(input_rows_raw,
                                         std::numeric_limits<int>::max()),
                errors::InvalidArgument("Input rows too large"));
    const int input_rows = static_cast<int>(input_rows_raw);
    const int filter_rows = static_cast<int>(filter.dim_size(0));

    // The third dimension for input is columns/width.
    // The second dimension for filter is columns/width.
    const int64 input_cols_raw = input_in_mkl_format
                                     ? mkl_input_shape.GetSizes()[0]
                                     : GetTensorDim(input, data_format_, 'W');
    OP_REQUIRES(context, FastBoundsCheck(input_cols_raw,
                                         std::numeric_limits<int>::max()),
                errors::InvalidArgument("Input cols too large"));
    const int input_cols = static_cast<int>(input_cols_raw);
    const int filter_cols = static_cast<int>(filter.dim_size(1));

    // The first dimension for input is batch.
    const int64 input_batch_raw = input_in_mkl_format
                                      ? mkl_input_shape.GetSizes()[3]
                                      : GetTensorDim(input, data_format_, 'N');
    OP_REQUIRES(context, FastBoundsCheck(input_batch_raw,
                                         std::numeric_limits<int>::max()),
                errors::InvalidArgument("batch is too large"));
    const int batch = static_cast<int>(input_batch_raw);

    // For now we take the stride from the second and third dimensions only (we
    // do not support striding on the batch or depth dimension).
    const int stride_rows = GetTensorDim(strides_, data_format_, 'H');
    const int stride_cols = GetTensorDim(strides_, data_format_, 'W');

    int64 out_rows = 0, out_cols = 0, pad_rows = 0, pad_cols = 0;
    OP_REQUIRES_OK(context,
                   GetWindowedOutputSize(input_rows, filter_rows, stride_rows,
                                         padding_, &out_rows, &pad_rows));
    OP_REQUIRES_OK(context,
                   GetWindowedOutputSize(input_cols, filter_cols, stride_cols,
                                         padding_, &out_cols, &pad_cols));
    TensorShape out_shape =
        ShapeFromFormat(data_format_, batch, out_rows, out_cols, out_depth);

    // Output tensor is of the following dimensions:
    // [ in_batch, out_rows, out_cols, out_depth ]
    Tensor* output = nullptr;

    // If there is nothing to compute, return.
    if (out_shape.num_elements() == 0) {
      // TODO(jbobba): Verify correctness here
      //               Need semantics for Null MKL tensor
      return;
    }

    if (batch == 0) {
      // Nothing to do, allocate output tensor and return
      MklShape mkl_output_mkl_shape;
      mkl_output_mkl_shape.SetMklTensor(false);
      AllocateOutputSetMklshape(context, 0, &output, input.shape(),
                                mkl_output_mkl_shape);
      return;
    }

    // Create MKL convolution primitives
    dnnPrimitive_t mkl_prim_convolution_fwd = nullptr;

    int mkl_dims =
        input_in_mkl_format ? mkl_input_shape.GetDimension() : input.dims();
    size_t mkl_in_sizes[4] = {static_cast<size_t>(input_cols),
                              static_cast<size_t>(input_rows),
                              static_cast<size_t>(input_depth),
                              static_cast<size_t>(batch)};
    size_t mkl_out_sizes[4] = {static_cast<size_t>(out_cols),
                              static_cast<size_t>(out_rows),
                              static_cast<size_t>(out_depth),
                              static_cast<size_t>(batch)};
    size_t mkl_out_strides[4];
    size_t mkl_in_strides[4];
    size_t mkl_filter_size[5];
    size_t mkl_filter_strides[5];
    int mkl_input_offset[2] = {static_cast<int>(-pad_cols),
                               static_cast<int>(-pad_rows)};
    size_t mkl_conv_stride[2] = {static_cast<size_t>(stride_cols),
                                 static_cast<size_t>(stride_rows)};
    GetStridesFromSizes(data_format_, mkl_out_strides, mkl_out_sizes);
    GetStridesFromSizes(data_format_, mkl_in_strides, mkl_in_sizes);

    // TF filter (out_depth, in_depth, cols, rows) ->
    // MKL filter (of, if, H, W)
    mkl_filter_size[0] = filter.dim_size(1);
    mkl_filter_size[1] = filter.dim_size(0);
    mkl_filter_size[2] = filter.dim_size(2);
    mkl_filter_size[3] = filter.dim_size(3);

    mkl_filter_strides[0] = mkl_filter_size[2] * mkl_filter_size[3];
    mkl_filter_strides[1] =
        mkl_filter_size[0] * mkl_filter_size[2] * mkl_filter_size[3];
    mkl_filter_strides[2] = mkl_filter_size[3];
    mkl_filter_strides[3] = 1;

    if (biasEnabled) {
      CHECK_EQ(
          dnnConvolutionCreateForwardBias_F32(
              &mkl_prim_convolution_fwd, nullptr, dnnAlgorithmConvolutionDirect,
              mkl_dims, mkl_in_sizes, mkl_out_sizes, mkl_filter_size,
              mkl_conv_stride, mkl_input_offset, dnnBorderZeros),
          E_SUCCESS);
    } else {
      CHECK_EQ(
          dnnConvolutionCreateForward_F32(
              &mkl_prim_convolution_fwd, nullptr, dnnAlgorithmConvolutionDirect,
              mkl_dims, mkl_in_sizes, mkl_out_sizes, mkl_filter_size,
              mkl_conv_stride, mkl_input_offset, dnnBorderZeros),
          E_SUCCESS);
    }

    TensorShape mkl_output_tf_shape;
    MklShape mkl_output_mkl_shape;
    mkl_output_mkl_shape.SetMklTensor(true);
    mkl_output_mkl_shape.SetMklLayout(mkl_prim_convolution_fwd, dnnResourceDst);
    mkl_output_mkl_shape.SetTfLayout(mkl_dims, mkl_out_sizes, mkl_out_strides);
    mkl_output_tf_shape.AddDim(1);
    mkl_output_tf_shape.AddDim(
        dnnLayoutGetMemorySize_F32(
            static_cast<dnnLayout_t>(mkl_output_mkl_shape.GetMklLayout())) /
        sizeof(T));
    AllocateOutputSetMklshape(context, 0, &output, mkl_output_tf_shape,
                              mkl_output_mkl_shape);

    // Get input tensor layouts
    dnnLayout_t mkl_lt_filter = nullptr, mkl_lt_bias = nullptr,
                mkl_lt_input = nullptr;
    CHECK_EQ(dnnLayoutCreate_F32(&mkl_lt_filter, filter.dims(), mkl_filter_size,
                                 mkl_filter_strides),
             E_SUCCESS);

    if (biasEnabled) {
      size_t mkl_bias_size[1] = {static_cast<size_t>(
                                  MklGetInput(context, 2).dim_size(0))};
      size_t mkl_bias_stride[1] = {1};
      CHECK_EQ(
          dnnLayoutCreate_F32(&mkl_lt_bias, 1, mkl_bias_size, mkl_bias_stride),
          E_SUCCESS);
    }

    if (input_in_mkl_format) {
      mkl_lt_input = static_cast<dnnLayout_t>(mkl_input_shape.GetCurLayout());
    } else {
      CHECK_EQ(dnnLayoutCreate_F32(&mkl_lt_input, mkl_dims, mkl_in_sizes,
                                   mkl_in_strides),
               E_SUCCESS);
    }

    // Compare with internal layouts and setup conversions
    dnnPrimitive_t mkl_prim_convert_filter, mkl_prim_convert_bias,
        mkl_prim_convert_input;
    dnnLayout_t mkl_lt_internal_filter, mkl_lt_internal_bias,
        mkl_lt_internal_input;
    void *mkl_buf_convert_input, *mkl_buf_convert_filter, *mkl_buf_convert_bias;
    Tensor mkl_tmp_input_buf_tensor, mkl_tmp_filter_buf_tensor,
        mkl_tmp_bias_buf_tensor;  // Temp tensor used to allocate tmp
                                  // buffers

    mkl_prim_convert_filter = nullptr;
    mkl_prim_convert_bias = nullptr;
    mkl_prim_convert_input = nullptr;
    mkl_lt_internal_filter = nullptr;
    mkl_lt_internal_bias = nullptr;
    mkl_lt_internal_input = nullptr;
    mkl_buf_convert_input = nullptr;
    mkl_buf_convert_filter = nullptr;
    mkl_buf_convert_bias = nullptr;

    CHECK_EQ(dnnLayoutCreateFromPrimitive_F32(&mkl_lt_internal_filter,
                                              mkl_prim_convolution_fwd,
                                              dnnResourceFilter),
             E_SUCCESS);
    if (!dnnLayoutCompare_F32(mkl_lt_internal_filter, mkl_lt_filter)) {
      CHECK_EQ(dnnConversionCreate_F32(&mkl_prim_convert_filter, mkl_lt_filter,
                                       mkl_lt_internal_filter),
               E_SUCCESS);
      AllocTmpBuffer(context, &mkl_tmp_filter_buf_tensor,
                     mkl_lt_internal_filter, &mkl_buf_convert_filter);
    }

    if (biasEnabled) {
      CHECK_EQ(
          dnnLayoutCreateFromPrimitive_F32(
              &mkl_lt_internal_bias, mkl_prim_convolution_fwd, dnnResourceBias),
          E_SUCCESS);
      if (!dnnLayoutCompare_F32(mkl_lt_internal_bias, mkl_lt_bias)) {
        CHECK_EQ(dnnConversionCreate_F32(&mkl_prim_convert_bias, mkl_lt_bias,
                                         mkl_lt_internal_bias),
                 E_SUCCESS);
        AllocTmpBuffer(context, &mkl_tmp_bias_buf_tensor, mkl_lt_internal_bias,
                       &mkl_buf_convert_bias);
      }
    }

    CHECK_EQ(
        dnnLayoutCreateFromPrimitive_F32(
            &mkl_lt_internal_input, mkl_prim_convolution_fwd, dnnResourceSrc),
        E_SUCCESS);
    if (!dnnLayoutCompare_F32(mkl_lt_internal_input, mkl_lt_input)) {
      CHECK_EQ(dnnConversionCreate_F32(&mkl_prim_convert_input, mkl_lt_input,
                                       mkl_lt_internal_input),
               E_SUCCESS);
      AllocTmpBuffer(context, &mkl_tmp_input_buf_tensor, mkl_lt_internal_input,
                     &mkl_buf_convert_input);
    }

    // Execute conversions
    void* mkl_conv_res[dnnResourceNumber];
    mkl_conv_res[dnnResourceDst] = static_cast<void*>(output->flat<T>().data());

    void* mkl_buf_input =
        const_cast<void*>(static_cast<const void*>(input.flat<T>().data()));
    if (mkl_prim_convert_input != nullptr)
      // TODO(jbobba): Do we need GetConvertedFlatData here?
      CHECK_EQ(dnnConversionExecute_F32(mkl_prim_convert_input, mkl_buf_input,
                                        mkl_buf_convert_input),
               E_SUCCESS);
    mkl_conv_res[dnnResourceSrc] = (mkl_prim_convert_input == nullptr)
                                       ? mkl_buf_input
                                       : mkl_buf_convert_input;

    void* mkl_buf_filter =
        const_cast<void*>(static_cast<const void*>(filter.flat<T>().data()));
    if (mkl_prim_convert_filter != nullptr)
      CHECK_EQ(dnnConversionExecute_F32(mkl_prim_convert_filter, mkl_buf_filter,
                                        mkl_buf_convert_filter),
               E_SUCCESS);
    mkl_conv_res[dnnResourceFilter] = (mkl_prim_convert_filter == nullptr)
                                          ? mkl_buf_filter
                                          : mkl_buf_convert_filter;

    if (biasEnabled) {
      const Tensor& bias = MklGetInput(context, 2);
      void* mkl_buf_bias =
          const_cast<void*>(static_cast<const void*>(bias.flat<T>().data()));
      if (mkl_prim_convert_bias != nullptr)
        CHECK_EQ(dnnConversionExecute_F32(mkl_prim_convert_bias, mkl_buf_bias,
                                          mkl_buf_convert_bias),
                 E_SUCCESS);
      mkl_conv_res[dnnResourceBias] = (mkl_prim_convert_bias == nullptr)
                                          ? mkl_buf_bias
                                          : mkl_buf_convert_bias;
    }

    // Execute convolution
    CHECK_EQ(dnnExecute_F32(mkl_prim_convolution_fwd, mkl_conv_res), E_SUCCESS);

    // Release MKL Resources
    if (mkl_prim_convert_filter != nullptr) {
      dnnDelete_F32(mkl_prim_convert_filter);
    }

    if (biasEnabled) {
      if (mkl_prim_convert_bias != nullptr) {
        dnnDelete_F32(mkl_prim_convert_bias);
      }
    }

    if (mkl_prim_convert_input != nullptr) {
      dnnDelete_F32(mkl_prim_convert_input);
    }

    dnnDelete_F32(mkl_prim_convolution_fwd);
    dnnLayoutDelete_F32(mkl_lt_internal_input);
    dnnLayoutDelete_F32(mkl_lt_internal_filter);
    // TODO(jbobba): What if input_in_mkl_format?
    if (!input_in_mkl_format) dnnLayoutDelete_F32(mkl_lt_input);
    dnnLayoutDelete_F32(mkl_lt_filter);
    if (biasEnabled) {
      dnnLayoutDelete_F32(mkl_lt_internal_bias);
      dnnLayoutDelete_F32(mkl_lt_bias);
    }
  }

 private:
  std::vector<int32> strides_;
  Padding padding_;
  TensorFormat data_format_;
};

#define REGISTER_MKL_CPU(T)                                                \
  REGISTER_KERNEL_BUILDER(                                                 \
      Name("MklConv2D").Device(DEVICE_CPU).TypeConstraint<T>("T"),         \
      MklConv2DOp<CPUDevice, T, false>);                                   \
  REGISTER_KERNEL_BUILDER(                                                 \
      Name("MklConv2DWithBias").Device(DEVICE_CPU).TypeConstraint<T>("T"), \
      MklConv2DOp<CPUDevice, T, true>);

TF_CALL_float(REGISTER_MKL_CPU);

REGISTER_MKL_LAYER_float("MklConv2D");
REGISTER_MKL_LAYER_float("MklConv2DWithBias");

}  // namespace tensorflow
#endif  // INTEL_MKL
