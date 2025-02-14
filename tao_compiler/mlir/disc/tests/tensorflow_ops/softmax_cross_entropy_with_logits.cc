/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/mlir/disc/tests/mlir_feature_test.h"
#include "tensorflow/compiler/mlir/disc/tests/mlir_test.h"
#include "tensorflow/core/platform/test.h"

namespace mlir_test {

const std::string c_ft_path =
    "tensorflow/compiler/mlir/disc/tests/tensorflow_ops/data/";

// dynamic shape softmax_cross_entropy_with_logits test case
TEST(TFSoftmaxCrossEntropyWithLogitsOpTest, FullyDynamicShape3DF32) {
  EXPECT_TRUE(feature_test_main(
      /*mlir_file_path*/ c_ft_path +
          "softmax_cross_entropy_with_logits_d_f32.mlir",
      /*backend_types*/ {BackendType::kCuda, BackendType::kX86},
      /*num_inputs*/ 2,
      /*num_outputs*/ 2,
      /*input_descriptors*/ {"13x21xf32_X", "13x21xf32_X"},
      /*output_descriptors*/ {"f32_X", "f32_X"}));
}

// // partial dynamic shape softmax_cross_entropy_with_logits test case
TEST(TFSoftmaxCrossEntropyWithLogitsOpTest, PartialDynamicShape3DF32) {
  EXPECT_TRUE(feature_test_main(
      /*mlir_file_path*/ c_ft_path +
          "softmax_cross_entropy_with_logits_p_f32.mlir",
      /*backend_types*/ {BackendType::kCuda, BackendType::kX86},
      /*num_inputs*/ 2,
      /*num_outputs*/ 2,
      /*input_descriptors*/ {"13x21xf32_X", "13x21xf32_X"},
      /*output_descriptors*/ {"f32_X", "f32_X"}));
}

// static shape softmax_cross_entropy_with_logits test case
TEST(TFSoftmaxCrossEntropyWithLogitsOpTest, StaticShape3DF32) {
  EXPECT_TRUE(feature_test_main(
      /*mlir_file_path*/ c_ft_path +
          "softmax_cross_entropy_with_logits_s_f32.mlir",
      /*backend_types*/ {BackendType::kCuda, BackendType::kX86},
      /*num_inputs*/ 2,
      /*num_outputs*/ 2,
      /*input_descriptors*/ {"13x21xf32_X", "13x21xf32_X"},
      /*output_descriptors*/ {"f32_X", "f32_X"}));
}

}  // namespace mlir_test