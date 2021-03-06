/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/c/tf_tensor_internal.h"
#include "tensorflow/core/common_runtime/eager/context.h"
#include "tensorflow/core/common_runtime/eager/eager_operation.h"
#include "tensorflow/core/common_runtime/eager/execute.h"
#include "tensorflow/core/common_runtime/eager/tensor_handle.h"

namespace {

bool IsCPU(
    absl::variant<tensorflow::Device*, tensorflow::CustomDevice*> variant) {
  if (VariantDeviceIsCustom(variant)) {
    return false;
  }
  tensorflow::Device* d = absl::get<tensorflow::Device*>(variant);
  return d == nullptr || d->tensorflow_gpu_device_info() == nullptr;
}

}  // namespace

namespace tensorflow {

// TODO(b/152902651): This should not depend on EagerContext. This can be
// resolved by storing ctx->HostCPU() in the TensorHandle class.
AbstractTensorInterface* TensorHandle::Resolve(Status* status) {
  if (VariantDeviceIsCustom(device())) {
    auto* custom_device = absl::get<CustomDevice*>(device());
    TensorHandle* copy;
    *status = custom_device->CopyTensorFromDevice(
        this, "/job:localhost/task:0/replica:0/device:CPU:0", &copy);
    if (status->ok()) {
      return copy->Resolve(status);
    } else {
      return nullptr;
    }
  }

  if (IsRemote()) {
    const tensorflow::Tensor* t = nullptr;
    TensorHandle* h_cpu = nullptr;
    *status = EagerCopyToDevice(this, ctx_, &ctx_->Executor(), ctx_->HostCPU(),
                                false, &h_cpu);
    if (!status->ok()) {
      return nullptr;
    }
    *status = h_cpu->Tensor(&t);
    if (!status->ok()) {
      h_cpu->Unref();
      return nullptr;
    }
    auto* retval = new TensorInterface(*t);
    h_cpu->Unref();
    return retval;
  } else {
    tensorflow::Tensor tensor;
    if (IsCPU(device()) || HasLocalMirror(nullptr)) {
      const tensorflow::Tensor* src = nullptr;
      if (HasLocalMirror(nullptr)) {
        *status = TensorFromDevice(nullptr, &src);
      } else {
        *status = Tensor(&src);
      }
      if (!status->ok()) return nullptr;
      tensor = *src;
    } else {
      *status = CopyToDevice(*ctx_, ctx_->HostCPU(), &tensor);
      if (!status->ok()) return nullptr;
      if (ImplicitMirroring()) {
        *status = AddEmptyLocalMirror(nullptr);
        if (!status->ok()) return nullptr;
        tensorflow::Tensor mirror = tensor;
        *status = SetTensor(std::move(mirror), nullptr);
        if (!status->ok()) return nullptr;
      }
    }
    return new TensorInterface(std::move(tensor));
  }
}

// TODO(b/152902651): We unfortunately need to put this EagerContext function
// here to a circular BUILD dep issue. If we move this to context.cc, then we
// will have the circular dependency of:
//   context -> tensor_handle -> remote_tensor_handle_data -> context
AbstractTensorHandleInterface* EagerContext::CreateLocalHandle(
    AbstractTensorInterface* t) {
  Tensor tensor = TensorFromInterface(t);
  return TensorHandle::CreateLocalHandle(std::move(tensor), /*d=*/HostCPU(),
                                         /*op_device=*/nullptr, this);
}

// TODO(b/152902651): We have to keep this function here since EagerOperation
// depends on EagerContext. Thus, the context build target can't depend on
// EagerOperation.
AbstractOperationInterface* EagerContext::CreateOperation() {
  return new EagerOperation(this);
}

// TODO(b/152902651): Once we move many execute.cc functions into
// eager_operation.cc we can avoid a circular dependency between them.
Status EagerOperation::Execute(
    absl::Span<AbstractTensorHandleInterface*> retvals, int* num_retvals) {
  return EagerExecute(
      this, reinterpret_cast<tensorflow::TensorHandle**>(retvals.data()),
      num_retvals);
}

}  //  namespace tensorflow
