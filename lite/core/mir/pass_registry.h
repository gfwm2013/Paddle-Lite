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

#include <set>
#include <string>
#include "lite/api/paddle_lite_factory_helper.h"
#include "lite/api/paddle_place.h"
#include "lite/core/mir/pass_manager.h"

namespace paddle {
namespace lite {
namespace mir {

class PassRegistry {
 public:
  PassRegistry(const std::string& name, mir::Pass* pass)
      : name_(name), pass_(pass) {
    PassManager::Global().AddNewPass(name_, pass_);
  }
  PassRegistry& SetTargets(const std::set<TargetType>& targets) {
    pass_->set_targets(targets);
    return *this;
  }
  bool Touch() const { return true; }

 private:
  std::string name_;
  mir::Pass* pass_;
};

}  // namespace mir
}  // namespace lite
}  // namespace paddle

#define REGISTER_MIR_PASS(name__, class__)                                \
  paddle::lite::mir::PassRegistry mir_pass_registry##name__(#name__,      \
                                                            new class__); \
  bool mir_pass_registry##name__##_fake() {                               \
    return mir_pass_registry##name__.Touch();                             \
  }                                                                       \
  static paddle::lite::mir::PassRegistry mir_pass_registry_func_##name__  \
      __attribute__((unused)) = mir_pass_registry##name__
