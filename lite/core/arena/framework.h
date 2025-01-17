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
#include <gtest/gtest.h>
#include <time.h>
#include <algorithm>
#include <chrono>  // NOLINT
#include <iomanip>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <cmath>
#include "lite/core/op_registry.h"
#include "lite/core/program.h"
#include "lite/core/scope.h"
#include "lite/core/types.h"
#include "lite/model_parser/cpp/op_desc.h"

namespace paddle {
namespace lite {
namespace arena {

/*
 * Init data and prepare the op.
 */
class TestCase {
 public:
  explicit TestCase(const Place& place, const std::string& alias)
      : place_(place), scope_(new Scope), alias_(alias) {
    ctx_ = ContextScheduler::Global().NewContext(place_.target);
  }

  void Prepare() {
    PrepareScopes();
    PrepareData();
    op_desc_.reset(new cpp::OpDesc);
    PrepareOpDesc(op_desc_.get());

    PrepareOutputsForInstruction();
    CreateInstruction();
    PrepareInputsForInstruction();
  }

  /// Run the target instruction, that is run the test operator.
  void RunInstruction() { instruction_->Run(); }

  KernelContext* context() { return ctx_.get(); }

  /// The baseline should be implemented, which acts similar to an operator,
  /// that is take several tensors as input and output several tensors as
  /// output.
  virtual void RunBaseline(Scope* scope) = 0;

  /// Check the precision of the output tensors. It will compare the same tensor
  /// in two scopes, one of the instruction execution, and the other for the
  /// baseline.
  template <typename T>
  bool CheckPrecision(const std::string& var_name, float abs_error);

  const cpp::OpDesc& op_desc() { return *op_desc_; }

  // Check whether the output tensor is consistent with the output definition in
  // kernel registry.
  void CheckKernelConsistWithDefinition() {}

  Scope& scope() { return *scope_; }

  Scope* baseline_scope() { return base_scope_; }
  Scope* inst_scope() { return inst_scope_; }

 protected:
  // Prepare inputs in scope() for Tester.
  virtual void PrepareData() = 0;

  /// Prepare a tensor in host. The tensors will be created in scope_.
  /// Need to specify the targets other than X86 or ARM.
  template <typename T>
  void SetCommonTensor(const std::string& var_name,
                       const DDim& ddim,
                       const T* data,
                       const LoD& lod = {}) {
    auto* tensor = scope_->NewTensor(var_name);
    tensor->Resize(ddim);
    auto* d = tensor->mutable_data<T>();
    memcpy(d, data, ddim.production() * sizeof(T));

    // set lod
    if (!lod.empty()) *tensor->mutable_lod() = lod;
  }

  // Prepare for the operator.
  virtual void PrepareOpDesc(cpp::OpDesc* op_desc) = 0;

 public:
  const Instruction& instruction() { return *instruction_; }

 private:
  std::unique_ptr<KernelContext> ctx_;
  void CreateInstruction();

  void PrepareScopes() {
    inst_scope_ = &scope_->NewScope();
    base_scope_ = &scope_->NewScope();
  }

  // Check shape
  // TODO(Superjomn) Move this method to utils or DDim?
  bool ShapeEquals(const DDim& a, const DDim& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }

  /// Copy the input tensors to target devices needed by the instruction.
  void PrepareInputsForInstruction();

  // Create output tensors and variables.
  void PrepareOutputsForInstruction() {
    for (auto x : op_desc().output_vars()) {
      inst_scope_->NewTensor(x);
      base_scope_->NewTensor(x);
    }
  }

 private:
  std::shared_ptr<Scope> scope_;
  // The workspace for the Instruction.
  Scope* inst_scope_{};
  // The workspace for the baseline implementation.
  Scope* base_scope_{};
  std::unique_ptr<cpp::OpDesc> op_desc_;
  std::unique_ptr<Instruction> instruction_;
  Place place_;
  std::string alias_;
};

class Arena {
  float abs_error_{};

 public:
  Arena(std::unique_ptr<TestCase>&& tester,
        const Place& place,
        float abs_error = 1e-5)
      : tester_(std::move(tester)), place_(place), abs_error_(abs_error) {
    tester_->Prepare();
  }

  bool TestPrecision() {
    tester_->RunBaseline(tester_->baseline_scope());
    tester_->RunInstruction();

    bool success = true;
    for (auto& out : tester_->op_desc().OutputArgumentNames()) {
      for (auto& var : tester_->op_desc().Output(out)) {
        success = success && CompareTensor(out, var);
      }
    }
    LOG(INFO) << "done";
    return success;
  }

  void TestPerformance(int times = 100) {
    auto timer = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < times; i++) {
      tester_->RunInstruction();
    }
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - timer);
    LOG(INFO) << "average duration: " << duration.count() << " ms";
  }

 private:
  // input_name: X
  bool CompareTensor(const std::string& arg_name, const std::string& var_name) {
    // get tensor type.
    const Type* type =
        tester_->instruction().kernel()->GetOutputDeclType(arg_name);

    switch (type->precision()) {
      case PRECISION(kFloat):
        return tester_->CheckPrecision<float>(var_name, abs_error_);
      case PRECISION(kInt8):
        return tester_->CheckPrecision<int8_t>(var_name, abs_error_);
      case PRECISION(kInt32):
        return tester_->CheckPrecision<int32_t>(var_name, abs_error_);
      case PRECISION(kBool):
        return tester_->CheckPrecision<bool>(var_name, abs_error_);

      default:
        LOG(FATAL) << "not support type " << PrecisionToStr(type->precision());
    }
  }

 private:
  std::unique_ptr<TestCase> tester_;
  Place place_;
};

template <typename T>
bool TestCase::CheckPrecision(const std::string& var_name, float abs_error) {
  auto a_tensor = inst_scope_->FindTensor(var_name);
  auto b_tensor = base_scope_->FindTensor(var_name);
  CHECK(a_tensor);
  CHECK(b_tensor);

  CHECK(ShapeEquals(a_tensor->dims(), b_tensor->dims()));

  CHECK(a_tensor->lod() == b_tensor->lod()) << "lod not match";

  // The baseline should output in host devices.
  CHECK(b_tensor->target() == TARGET(kHost) ||
        b_tensor->target() == TARGET(kX86) ||
        b_tensor->target() == TARGET(kARM));

  const T* a_data{};
  switch (a_tensor->target()) {
    case TARGET(kX86):
    case TARGET(kHost):
    case TARGET(kARM):
      a_data = static_cast<const T*>(a_tensor->raw_data());
      break;

    default:
      // Before compare, need to copy data from `target` device to host.
      LOG(FATAL) << "Not supported";
  }

  CHECK(a_data);

  const T* b_data = static_cast<const T*>(b_tensor->raw_data());

  bool success = true;
  for (int i = 0; i < a_tensor->dims().production(); i++) {
    EXPECT_NEAR(a_data[i], b_data[i], abs_error);
    if (fabsf(a_data[i] - b_data[i]) > abs_error) {
      success = false;
    }
  }
  return success;
}

}  // namespace arena
}  // namespace lite
}  // namespace paddle
