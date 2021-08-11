#pragma once

#include <ATen/Tensor.h>
#include <c10/core/Storage.h>
#include <c10/core/TensorImpl.h>
#include <ATen/FunctionalTensorImplBase.h>

#include "torch_xla/csrc/tensor.h"

namespace torch_xla {

// Tensor implementation class used to be fed to the at::Tensor.
// Its scope is just to handle an XLATensor.
class XLATensorImpl : public at::FunctionalTensorImplBase {
 public:
  explicit XLATensorImpl(XLATensor tensor);

  const XLATensor& tensor() const { return tensor_; }

  XLATensor& tensor() { return tensor_; }

  void set_tensor(XLATensor xla_tensor);

  void force_refresh_sizes() { generation_ = 0; }

  c10::intrusive_ptr<TensorImpl> shallow_copy_and_detach(
      const c10::VariableVersion& version_counter,
      bool allow_tensor_metadata_change) const override;

  c10::intrusive_ptr<TensorImpl> shallow_copy_and_detach(
      c10::VariableVersion&& version_counter,
      bool allow_tensor_metadata_change) const override;

  void shallow_copy_from(const c10::intrusive_ptr<TensorImpl>& impl) override;

  at::IntArrayRef sizes() const override;

  int64_t dim() const override;

  int64_t numel() const override;

  bool is_contiguous(at::MemoryFormat memory_format) const override;

  int64_t size(int64_t d) const override;

  const at::Storage& storage() const override;

  bool has_storage() const override;

  // Override the FunctionalTensorImplBase method describing how to re-use a tensor in the functionalization pass.
  void replace_(const at::Tensor& other) override;

  static void AtenInitialize();

 private:
  void SetupSizeProperties();

  static caffe2::TypeMeta GetTypeMeta(const XLATensor& tensor);

  XLATensor tensor_;
  size_t generation_ = 0;
};

}  // namespace torch_xla
