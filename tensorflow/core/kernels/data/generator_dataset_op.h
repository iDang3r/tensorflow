/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_CORE_KERNELS_DATA_GENERATOR_DATASET_OP_H_
#define TENSORFLOW_CORE_KERNELS_DATA_GENERATOR_DATASET_OP_H_

#include "tensorflow/core/framework/dataset.h"

namespace tensorflow {
namespace data {

class GeneratorDatasetOp : public DatasetOpKernel {
 public:
  explicit GeneratorDatasetOp(OpKernelConstruction* ctx);

  void MakeDataset(OpKernelContext* ctx, DatasetBase** output) override;

 private:
  class Dataset;

  DataTypeVector output_types_;
  std::vector<PartialTensorShape> output_shapes_;
  NameAttrList init_func_;
  NameAttrList next_func_;
  NameAttrList finalize_func_;
  std::shared_ptr<FunctionLibraryDefinition> lib_def_;
};

}  // namespace data
}  // namespace tensorflow
#endif  // TENSORFLOW_CORE_KERNELS_DATA_GENERATOR_DATASET_OP_H_
