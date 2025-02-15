// Copyright (c) 2019 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>
#include <vector>
#include "lite/core/tensor.h"

namespace paddle {
namespace lite {
namespace arm {
namespace math {

void encode_bbox_center_kernel(const int batch_num,
                               const float* loc_data,
                               const float* prior_data,
                               const float* variance,
                               const bool var_len4,
                               const int num_priors,
                               float* bbox_data);

void decode_bbox_center_kernel(const int batch_num,
                               const float* loc_data,
                               const float* prior_data,
                               const float* variance,
                               const bool var_len4,
                               const int num_priors,
                               const bool normalized,
                               float* bbox_data);

void decode_center_size_axis_1(const int var_size,
                               const int row,
                               const int col,
                               const int len,
                               const float* target_box_data,
                               const float* prior_box_data,
                               const float* prior_box_var_data,
                               const bool normalized,
                               const std::vector<float> variance,
                               float* output);
}  // namespace math
}  // namespace arm
}  // namespace lite
}  // namespace paddle
