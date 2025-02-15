// Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
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

#include "lite/kernels/nnadapter/engine.h"
#include <sys/time.h>
#include <time.h>
#include <utility>
#include "lite/core/op_registry.h"
#include "lite/core/subgraph/subgraph_bridge_registry.h"
#include "lite/kernels/nnadapter/converter/converter.h"
#include "lite/utils/env.h"
#include "lite/utils/md5.h"

namespace paddle {
namespace lite {
namespace kernels {
namespace nnadapter {

// A simple token for identifying the model cache is generated based on the MD5
// algorithm and the following information: 1) The valid device names 2) The
// input variable names 3) The input variable shapes
std::string GenerateModelCacheToken(
    const std::vector<std::string>& device_names,
    const std::vector<Variable>& input_vars) {
  std::ostringstream os;
  for (auto device_name : device_names) {
    os << device_name;
  }
  for (size_t i = 0; i < input_vars.size(); i++) {
    os << input_vars[i].name;
    for (auto input_shape : input_vars[i].value->dims().Vectorize()) {
      os << input_shape;
    }
  }
  return MD5(os.str());
}

void* AccessModelInput(void* memory, NNAdapterOperandType* type) {
  CHECK(memory);
  CHECK(type);
  auto tensor = static_cast<Tensor*>(memory);
  // Fill the dimensions and get the host buffer address of model inputs
  ConvertDDimToNNDimensions(
      tensor->dims(), type->dimensions.data, &type->dimensions.count);
  return tensor->raw_data();
}

void* AccessModelOutput(void* memory, NNAdapterOperandType* type) {
  CHECK(memory);
  CHECK(type);
  auto tensor = static_cast<Tensor*>(memory);
  auto precision = ConvertNNPrecisionCodeToPrecisionType(type->precision);
  auto dimensions =
      ConvertNNDimensionsToDDim(type->dimensions.data, type->dimensions.count);
  tensor->Resize(dimensions);
#define TENSOR_MUTABLE_DATA(ptype, dtype) \
  case PRECISION(ptype):                  \
    tensor->mutable_data<dtype>();        \
    break;
  switch (precision) {
    TENSOR_MUTABLE_DATA(kInt8, int8_t)
    TENSOR_MUTABLE_DATA(kInt32, int32_t)
    TENSOR_MUTABLE_DATA(kInt64, int64_t)
    TENSOR_MUTABLE_DATA(kFloat, float)
    TENSOR_MUTABLE_DATA(kBool, bool)
    default:
      LOG(FATAL) << "Failed to mutable data for the precsion type("
                 << PrecisionToStr(precision) << ") at output@0x"
                 << string_format("%x", memory) << "!";
      break;
  }
#undef TENSOR_MUTABLE_DATA
  return tensor->raw_data();
}

Program::~Program() {
  if (execution_) {
    NNAdapterExecution_destroy_invoke(execution_);
  }
  if (compilation_) {
    NNAdapterCompilation_destroy_invoke(compilation_);
  }
  if (model_) {
    NNAdapterModel_destroy_invoke(model_);
  }
}

bool Program::LoadFromCache(const std::string& model_cache_token,
                            std::vector<char>* model_cache_buffer,
                            const std::string& model_cache_dir) {
  CHECK(!model_cache_token.empty());
  // Compile the cached model buffer or file to the device-specific program
  int result = NNAdapterCompilation_create_invoke(nullptr,
                                                  model_cache_token.c_str(),
                                                  model_cache_buffer->data(),
                                                  model_cache_buffer->size(),
                                                  model_cache_dir.c_str(),
                                                  context_,
                                                  &compilation_);
  if (result != NNADAPTER_NO_ERROR) {
    LOG(WARNING)
        << "Failed to create a compilation from the model cache buffer ("
        << result << ") !";
    return false;
  }
  result = NNAdapterCompilation_finish_invoke(compilation_);
  if (result != NNADAPTER_NO_ERROR) {
    LOG(WARNING) << "Build model failed(" << result << ") !";
    return false;
  }
  return true;
}

bool Program::BuildAndCacheToFile(
    int block_idx,
    const std::shared_ptr<const cpp::ProgramDesc>& program_desc,
    Scope* exec_scope,
    const std::vector<Variable>& input_vars,
    std::vector<Variable>* output_vars,
    const std::string& model_cache_token,
    const std::string& model_cache_dir) {
  // Converting the PaddlePaddle operators and variables to the NNAdapter
  // operations and operands for building NNAdapter model(hardware-indepedent)
  CHECK(!model_cache_token.empty());
  int result = NNAdapterModel_create_invoke(&model_);
  std::vector<NNAdapterOperand *> input_operands, output_operands;
  Converter converter(model_);
  if (converter.Apply(block_idx,
                      program_desc,
                      exec_scope,
                      input_vars,
                      output_vars,
                      &input_operands,
                      &output_operands) != NO_ERROR) {
    return false;
  }
  NNAdapterModel_identifyInputsAndOutputs_invoke(model_,
                                                 input_operands.size(),
                                                 input_operands.data(),
                                                 output_operands.size(),
                                                 output_operands.data());
  result = NNAdapterModel_finish_invoke(model_);
  // Compiling the model to the device-specific binary program
  result = NNAdapterCompilation_create_invoke(model_,
                                              model_cache_token.c_str(),
                                              nullptr,
                                              0,
                                              model_cache_dir.c_str(),
                                              context_,
                                              &compilation_);
  if (result != NNADAPTER_NO_ERROR) {
    NNAdapterModel_destroy_invoke(model_);
    model_ = nullptr;
    LOG(WARNING)
        << "Failed to create a compilation by compiling the source model ("
        << result << ") !";
    return false;
  }
  result = NNAdapterCompilation_finish_invoke(compilation_);
  if (result != NNADAPTER_NO_ERROR) {
    NNAdapterModel_destroy_invoke(model_);
    model_ = nullptr;
    LOG(WARNING) << "Build model failed(" << result << ") !";
    return false;
  }
  return true;
}

bool Program::SetInputsAndOutputs(std::vector<Variable>* input_vars,
                                  std::vector<Variable>* output_vars) {
  CHECK(IsValid());
  // Query the information of inputs and outputs
  uint32_t input_count, output_count;
  int result = NNAdapterCompilation_queryInputsAndOutputs_invoke(
      compilation_, &input_count, NULL, &output_count, NULL);
  if (result != NNADAPTER_NO_ERROR) {
    LOG(WARNING) << "Failed to query the count of inputs and outputs from the "
                    "compilation("
                 << result << ") !";
    return false;
  }
  CHECK_EQ(input_count, input_vars->size());
  CHECK_EQ(output_count, output_vars->size());
  // Create an execution for executing the compiled device program
  result = NNAdapterExecution_create_invoke(compilation_, &execution_);
  if (result != NNADAPTER_NO_ERROR) {
    LOG(WARNING) << "Create execution failed(" << result << ") !";
    return false;
  }
  // Set the model input and output tensors and the functions to access them
  for (uint32_t i = 0; i < input_count; i++) {
    NNAdapterExecution_setInput_invoke(
        execution_,
        i,
        reinterpret_cast<void*>(input_vars->at(i).value),
        AccessModelInput);
  }
  for (uint32_t i = 0; i < output_count; i++) {
    NNAdapterExecution_setOutput_invoke(
        execution_,
        i,
        reinterpret_cast<void*>(output_vars->at(i).value),
        AccessModelOutput);
  }
  return true;
}

bool Program::Execute() {
  CHECK(IsReady());
  auto GetCurrentUS = []() -> double {
    struct timeval time;
    gettimeofday(&time, NULL);
    return 1e+6 * time.tv_sec + time.tv_usec;
  };
  auto start_time = GetCurrentUS();
  int result = NNAdapterExecution_compute_invoke(execution_);
  if (result != NNADAPTER_NO_ERROR) {
    LOG(WARNING) << "Failed to run the execution(" << result << ")!";
    return false;
  }
  VLOG(3) << "Process cost " << GetCurrentUS() - start_time << " us";
  return true;
}

Engine::Engine(KernelContext* ctx,
               int block_idx,
               const std::shared_ptr<const cpp::ProgramDesc>& program_desc,
               Scope* exec_scope,
               const std::vector<std::string>& input_names,
               const std::vector<std::string>& output_names,
               const std::vector<float>& input_scales,
               const std::vector<float>& output_scales)
    : ctx_(ctx),
      block_idx_(block_idx),
      program_desc_(program_desc),
      exec_scope_(exec_scope) {
  int result;
  // Obtain the same order every time by sorting the input and output names,
  // because the topological order may be different each time of the partition
  // of the subgraph(but they are equivalent)
  auto input_count = input_names.size();
  auto output_count = output_names.size();
  CHECK_EQ(input_count, input_scales.size());
  CHECK_EQ(output_count, output_scales.size());
  input_vars_.resize(input_count);
  output_vars_.resize(output_count);
  for (size_t i = 0; i < input_count; i++) {
    const auto& name = input_names[i];
    input_vars_[i].name = name;
    // input_vars_[i].dynamic_dimensions =
    // ctx->As<NNAdapterContext>().NNAdapterDynamicDimensions(exec_scope_,
    // name);
    input_vars_[i].value = exec_scope_->FindMutableTensor(input_names[i]);
    input_vars_[i].quant_scale = input_scales[i];
  }
  for (size_t i = 0; i < output_count; i++) {
    output_vars_[i].name = output_names[i];
    output_vars_[i].value = exec_scope_->FindMutableTensor(output_names[i]);
    output_vars_[i].quant_scale = output_scales[i];
  }
  auto sort_comp_func = [](const Variable& a, const Variable& b) -> bool {
    return a.name < b.name;
  };
  std::stable_sort(input_vars_.begin(), input_vars_.end(), sort_comp_func);
  std::stable_sort(output_vars_.begin(), output_vars_.end(), sort_comp_func);
  // Get the specified devices and create a context for each device to build or
  // load the device-specific program from the model or the cache file/buffer.
  const auto& device_names =
      ctx->As<NNAdapterContext>().NNAdapterDeviceNames(exec_scope_);
  CHECK_GT(device_names.size(), 0) << "No device is specified.";
  for (const auto& device_name : device_names) {
    NNAdapterDevice* device = nullptr;
    result = NNAdapterDevice_acquire_invoke(device_name.c_str(), &device);
    bool found = result == NNADAPTER_NO_ERROR && device != nullptr;
    if (found) {
      const char* name = nullptr;
      NNAdapterDevice_getName_invoke(device, &name);
      const char* vendor = nullptr;
      NNAdapterDevice_getVendor_invoke(device, &vendor);
      NNAdapterDeviceType type = 0;
      NNAdapterDevice_getType_invoke(device, &type);
      int32_t version = 0;
      NNAdapterDevice_getVersion_invoke(device, &version);
      VLOG(3) << "NNAdapter device " << name << ": vendor=" << vendor
              << " type=" << type << " version=" << version;
      devices_.push_back(device);
      // Only support the first found device.
      break;
    }
  }
  CHECK_GT(devices_.size(), 0) << "No device is found.";
  // Get the context properties from the scope
  auto context_properties =
      ctx->As<NNAdapterContext>().NNAdapterContextProperties(exec_scope_);
  VLOG(3) << "NNAdapter context_properties: " << context_properties;
  // Create a context with multiple devices
  NNAdapterContext_create_invoke(
      &devices_[0], devices_.size(), context_properties.c_str(), &context_);
  // Get the model cache dir from the scope
  model_cache_dir_ =
      ctx_->As<NNAdapterContext>().NNAdapterModelCacheDir(exec_scope_);
  VLOG(3) << "NNAdapter model_cache_dir: " << model_cache_dir_;
}

Engine::~Engine() {
  programs_.clear();
  NNAdapterContext_destroy_invoke(context_);
  for (auto* device : devices_) {
    NNAdapterDevice_release_invoke(device);
  }
}

bool Engine::Run() {
  // Update the input dimensions to generate a key to find a compiled device
  // program
  auto input_count = input_vars_.size();
  std::vector<std::vector<int64_t>> input_dims(input_count);
  for (size_t i = 0; i < input_count; i++) {
    input_dims[i] = input_vars_[i].value->dims().Vectorize();
  }
  // Find the compiled device program according to the input dimensions
  std::shared_ptr<Program> program = nullptr;
  if (!programs_.count(input_dims)) {
    // Rebuild the device program corresponding to the input dimensions if not
    // found
    std::vector<std::string> device_names;
    for (auto* device : devices_) {
      const char* name = nullptr;
      NNAdapterDevice_getName_invoke(device, &name);
      device_names.push_back(name);
    }
    program = std::make_shared<Program>(context_);
    // Take the model cache buffer from the scope
    std::vector<char> model_cache_buffer;
    // Generate a cache token based on the input names and shapes
    auto model_cache_token = GenerateModelCacheToken(device_names, input_vars_);
    VLOG(3) << "NNAdapter model_cache_token: " << model_cache_token;
    ctx_->As<NNAdapterContext>().NNAdapterModelCacheBuffers(
        exec_scope_, model_cache_token, &model_cache_buffer);
    VLOG(3) << "NNAdapter model_cache_buffer size: "
            << model_cache_buffer.size();
    // Load the compiled device program from the model cache buffer or file
    if (!program->LoadFromCache(
            model_cache_token, &model_cache_buffer, model_cache_dir_)) {
      // Compile the model online to generate the device program and cache it to
      // the file
      CHECK(program->BuildAndCacheToFile(block_idx_,
                                         program_desc_,
                                         exec_scope_,
                                         input_vars_,
                                         &output_vars_,
                                         model_cache_token,
                                         model_cache_dir_));
    }
    CHECK(program->IsValid());
    CHECK(program->SetInputsAndOutputs(&input_vars_, &output_vars_));
    programs_[input_dims] = program;
  } else {
    program = programs_[input_dims];
    CHECK(program);
    CHECK(program->IsValid());
  }
  return program->Execute();
}

}  // namespace nnadapter
}  // namespace kernels
}  // namespace lite
}  // namespace paddle
