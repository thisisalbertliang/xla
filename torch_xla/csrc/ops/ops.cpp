#include "torch_xla/csrc/ops/ops.h"

#include <cmath>

#include "tensorflow/compiler/xla/client/lib/logdet.h"
#include "tensorflow/compiler/xla/client/lib/math.h"
#include "tensorflow/compiler/xla/client/lib/matrix.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/xla_client/debug_macros.h"
#include "tensorflow/compiler/xla/xla_client/util.h"
#include "torch/csrc/lazy/core/helpers.h"
#include "torch/csrc/lazy/core/util.h"
#include "torch_xla/csrc/convert_ops.h"
#include "torch_xla/csrc/data_ops.h"
#include "torch_xla/csrc/elementwise.h"
#include "torch_xla/csrc/helpers.h"
#include "torch_xla/csrc/lowering_context.h"
#include "torch_xla/csrc/matrix.h"
#include "torch_xla/csrc/nll_loss.h"
#include "torch_xla/csrc/ops/arithmetic_ir_ops.h"
#include "torch_xla/csrc/ops/constant.h"
#include "torch_xla/csrc/ops/expand.h"
#include "torch_xla/csrc/ops/infer_output_shape.h"
#include "torch_xla/csrc/ops/log_softmax_backward.h"
#include "torch_xla/csrc/ops/permute.h"
#include "torch_xla/csrc/ops/softmax_backward.h"
#include "torch_xla/csrc/ops/sum.h"
#include "torch_xla/csrc/ops/xla_ops.h"
#include "torch_xla/csrc/pooling.h"
#include "torch_xla/csrc/tensor_util.h"
#include "torch_xla/csrc/torch_util.h"
#include "torch_xla/csrc/xla_lower_util.h"

namespace torch_xla {
namespace ir {
namespace ops {

#define PTXLA_UNARY_OP(name, sym, xla_fn)                                  \
  torch::lazy::NodePtr name(const Value& input) {                          \
    auto lower_fn = [](const Node& node,                                   \
                       LoweringContext* loctx) -> XlaOpVector {            \
      xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));          \
      return node.ReturnOp(xla_fn(xla_input), loctx);                      \
    };                                                                     \
    return GenericOp(torch::lazy::OpKind(sym), {input}, input.xla_shape(), \
                     std::move(lower_fn));                                 \
  }

#define PTXLA_BINARY_OP(name, sym, xla_fn)                                     \
  torch::lazy::NodePtr name(const Value& input0, const Value& input1) {        \
    auto shape_fn = [&](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp { \
      auto promoted = XlaHelpers::Promote(operands[0], operands[1]);           \
      return xla_fn(promoted.first, promoted.second);                          \
    };                                                                         \
    auto lower_fn = [](const Node& node,                                       \
                       LoweringContext* loctx) -> XlaOpVector {                \
      xla::XlaOp xla_input0 = loctx->GetOutputOp(node.operand(0));             \
      xla::XlaOp xla_input1 = loctx->GetOutputOp(node.operand(1));             \
      auto promoted = XlaHelpers::Promote(xla_input0, xla_input1);             \
      return node.ReturnOp(xla_fn(promoted.first, promoted.second), loctx);    \
    };                                                                         \
    return GenericOp(torch::lazy::OpKind(sym), {input0, input1},               \
                     [&]() {                                                   \
                       return InferOutputShape(                                \
                           {input0.xla_shape(), input1.xla_shape()},           \
                           shape_fn);                                          \
                     },                                                        \
                     std::move(lower_fn));                                     \
  }

PTXLA_UNARY_OP(Acos, at::aten::acos, xla::Acos);
PTXLA_UNARY_OP(Acosh, at::aten::acosh, xla::Acosh);
PTXLA_UNARY_OP(Cos, at::aten::cos, xla::Cos);
PTXLA_UNARY_OP(Cosh, at::aten::cosh, xla::Cosh);
PTXLA_UNARY_OP(Asin, at::aten::asin, xla::Asin);
PTXLA_UNARY_OP(Asinh, at::aten::asinh, xla::Asinh);
PTXLA_UNARY_OP(Sin, at::aten::sin, xla::Sin);
PTXLA_UNARY_OP(Sinh, at::aten::sinh, xla::Sinh);
PTXLA_UNARY_OP(Atan, at::aten::atan, xla::Atan);
PTXLA_UNARY_OP(Atanh, at::aten::atanh, xla::Atanh);
PTXLA_UNARY_OP(Tan, at::aten::tan, xla::Tan);
PTXLA_UNARY_OP(Tanh, at::aten::tanh, xla::Tanh);
PTXLA_UNARY_OP(Neg, at::aten::neg, xla::Neg);
PTXLA_UNARY_OP(Exp, at::aten::exp, xla::Exp);
PTXLA_UNARY_OP(Expm1, at::aten::expm1, xla::Expm1);
PTXLA_UNARY_OP(Log, at::aten::log, xla::Log);
PTXLA_UNARY_OP(Log1p, at::aten::log1p, xla::Log1p);
PTXLA_UNARY_OP(Erf, at::aten::erf, xla::Erf);
PTXLA_UNARY_OP(Erfc, at::aten::erfc, xla::Erfc);
PTXLA_UNARY_OP(Erfinv, at::aten::erfinv, xla::ErfInv);
PTXLA_UNARY_OP(Sqrt, at::aten::sqrt, xla::Sqrt);
PTXLA_UNARY_OP(Rsqrt, at::aten::rsqrt, xla::Rsqrt);
PTXLA_UNARY_OP(Ceil, at::aten::ceil, xla::Ceil);
PTXLA_UNARY_OP(Floor, at::aten::floor, xla::Floor);
PTXLA_UNARY_OP(Round, at::aten::round, xla::RoundToEven);
PTXLA_UNARY_OP(Not, at::aten::bitwise_not, xla::Not);
PTXLA_UNARY_OP(IsNan, at::aten::isnan, xla::IsNan);

PTXLA_BINARY_OP(Min, at::aten::min, xla::Min);
PTXLA_BINARY_OP(Max, at::aten::max, xla::Max);
PTXLA_BINARY_OP(Pow, at::aten::pow, xla::Pow);
PTXLA_BINARY_OP(Fmod, at::aten::fmod, xla::Rem);
PTXLA_BINARY_OP(Atan2, at::aten::atan2, xla::Atan2);

torch::lazy::NodePtr Trunc(const Value& input) {
  return Floor(Abs(input)) * SignOp(input);
}

torch::lazy::NodePtr FracOp(const Value& input) { return input - Trunc(input); }

torch::lazy::NodePtr LogBase(const Value& input, torch::lazy::OpKind op,
                             double base) {
  auto lower_fn = [base](const Node& node,
                         LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp result = xla::Log(xla_input);
    xla::XlaOp ln_base = XlaHelpers::ScalarValue<float>(
        1.0 / std::log(base), node.xla_shape().element_type(),
        xla_input.builder());
    return node.ReturnOp(result * ln_base, loctx);
  };
  return GenericOp(op, {input}, input.xla_shape(), std::move(lower_fn),
                   /*num_outputs=*/1, torch::lazy::MHash(base));
}

torch::lazy::NodePtr ReciprocalOp(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(BuildReciprocal(xla_input), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::reciprocal), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr SgnOp(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(BuildSgn(xla_input), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::sgn), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr SignOp(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(BuildSign(xla_input), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::sign), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr Abs(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(BuildAbs(xla_input), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::abs), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr ReluOp(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_output = BuildRelu(xla_input);
    return node.ReturnOp(xla_output, loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    XLA_CHECK_EQ(operands.size(), 1) << "Unexpected number of operands";
    return BuildRelu(operands[0]);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::relu), {input},
                   [&]() {
                     return InferOutputShape({input.xla_shape()},
                                             lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr Prelu(const Value& input, const Value& weight) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_weight = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_output = BuildPrelu(xla_input, xla_weight);
    return node.ReturnOp(xla_output, loctx);
  };

  return GenericOp(torch::lazy::OpKind(at::aten::prelu), {input, weight},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr HardSigmoid(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(BuildHardSigmoid(xla_input), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::hardsigmoid), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr HardSigmoidBackward(const Value& grad_output,
                                         const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_grad_output = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(1));
    return node.ReturnOp(BuildHardSigmoidBackward(xla_grad_output, xla_input),
                         loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::hardsigmoid_backward),
                   {grad_output, input}, input.xla_shape(),
                   std::move(lower_fn));
}

torch::lazy::NodePtr HardSwish(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(BuildHardSwish(xla_input), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::hardswish), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr HardSwishBackward(const Value& grad_output,
                                       const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_grad_output = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(1));
    return node.ReturnOp(BuildHardSwishBackward(xla_grad_output, xla_input),
                         loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::hardswish_backward),
                   {grad_output, input}, input.xla_shape(),
                   std::move(lower_fn));
}

std::tuple<torch::lazy::NodePtr, torch::lazy::NodePtr> LogSigmoid(
    const Value& input) {
  ScopePusher ir_scope(at::aten::log_sigmoid.toQualString());
  // Use log-sum-exp trick to avoid overflow.
  torch::lazy::NodePtr neg_input = Neg(input);
  torch::lazy::NodePtr max_elem =
      Max(ScalarOp(0, input.xla_shape()), neg_input);
  torch::lazy::NodePtr buffer = Exp(Neg(max_elem)) + Exp(neg_input - max_elem);
  torch::lazy::NodePtr output = Neg(max_elem + Log(buffer));
  return std::make_tuple(output, buffer);
}

torch::lazy::NodePtr LogSigmoidBackward(const Value& grad_output,
                                        const Value& input,
                                        const Value& buffer) {
  ScopePusher ir_scope(at::aten::log_sigmoid_backward.toQualString());
  torch::lazy::NodePtr zero = ScalarOp(0, input.xla_shape());
  torch::lazy::NodePtr one = ScalarOp(1, input.xla_shape());
  torch::lazy::NodePtr minus_one = ScalarOp(-1, input.xla_shape());
  torch::lazy::NodePtr max_deriv =
      Where(ComparisonOp(at::aten::lt, input, zero), minus_one, zero);
  torch::lazy::NodePtr sign =
      Where(ComparisonOp(at::aten::lt, input, zero), one, minus_one);
  return grad_output * (Neg(max_deriv) - sign * (buffer - one) / buffer);
}

torch::lazy::NodePtr SiLU(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(xla_input * BuildSigmoid(xla_input), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::silu), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr SiLUBackward(const Value& grad_output,
                                  const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_grad_output = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(1));
    return node.ReturnOp(BuildSiLUBackward(xla_grad_output, xla_input), loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return BuildSiLUBackward(operands[0], operands[1]);
  };
  return GenericOp(
      torch::lazy::OpKind(at::aten::silu_backward), {grad_output, input},
      [&]() {
        return InferOutputShape({grad_output.xla_shape(), input.xla_shape()},
                                lower_for_shape_fn);
      },
      std::move(lower_fn));
}

torch::lazy::NodePtr Sigmoid(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(BuildSigmoid(xla_input), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::sigmoid), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr SigmoidBackward(const Value& grad_output,
                                     const Value& output) {
  return grad_output * (ScalarOp(1, output.xla_shape()) - output) * output;
}

torch::lazy::NodePtr LogSoftmaxBackwardOp(const Value& grad_output,
                                          const Value& output, int64_t dim) {
  return ir::MakeNode<LogSoftmaxBackward>(
      grad_output, output,
      torch::lazy::GetCanonicalDimensionIndex(dim,
                                              grad_output.xla_shape().rank()));
}

torch::lazy::NodePtr SoftmaxBackwardOp(const Value& grad_output,
                                       const Value& output, int64_t dim) {
  return ir::MakeNode<SoftmaxBackward>(
      grad_output, output,
      torch::lazy::GetCanonicalDimensionIndex(dim,
                                              grad_output.xla_shape().rank()));
}

torch::lazy::NodePtr Clamp(const Value& input, const Value& min,
                           const Value& max) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_min = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_max = loctx->GetOutputOp(node.operand(2));
    xla::PrimitiveType input_type = XlaHelpers::TypeOfXlaOp(xla_input);
    xla_min = ConvertTo(xla_min, XlaHelpers::TypeOfXlaOp(xla_min), input_type,
                        /*device=*/nullptr);
    xla_max = ConvertTo(xla_max, XlaHelpers::TypeOfXlaOp(xla_max), input_type,
                        /*device=*/nullptr);
    return node.ReturnOp(xla::Clamp(xla_min, xla_input, xla_max), loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::clamp), {input, min, max},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr Ger(const Value& input, const Value& other) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_other = loctx->GetOutputOp(node.operand(1));
    return node.ReturnOp(BuildGer(xla_input, xla_other), loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return BuildGer(operands[0], operands[1]);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::ger), {input, other},
                   [&]() {
                     return InferOutputShape(
                         {input.xla_shape(), other.xla_shape()},
                         lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr AddMatMulOp(const Value& input, const Value& weight,
                                 const Value& bias) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    XLA_CHECK_EQ(node.operands().size(), 3) << "Unexpected number of operands";
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_weight = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_bias = loctx->GetOutputOp(node.operand(2));
    return node.ReturnOp(BuildMatMul(xla_input, xla_weight, xla_bias), loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return BuildMatMul(operands[0], operands[1], operands[2]);
  };
  return GenericOp(
      torch::lazy::OpKind(at::aten::addmm), {input, weight, bias},
      [&]() {
        return InferOutputShape(
            {input.xla_shape(), weight.xla_shape(), bias.xla_shape()},
            lower_for_shape_fn);
      },
      std::move(lower_fn));
}

torch::lazy::NodePtr Dot(const Value& input, const Value& weight) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_weight = loctx->GetOutputOp(node.operand(1));
    return node.ReturnOp(BuildDot(xla_input, xla_weight), loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return BuildDot(operands[0], operands[1]);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::mm), {input, weight},
                   [&]() {
                     return InferOutputShape(
                         {input.xla_shape(), weight.xla_shape()},
                         lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr MatMul(const Value& lhs, const Value& rhs) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_lhs = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_rhs = loctx->GetOutputOp(node.operand(1));
    std::tie(xla_lhs, xla_rhs) = XlaHelpers::PromoteValues(xla_lhs, xla_rhs);

    return node.ReturnOp(CreateMatMul(xla_lhs, xla_rhs), loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return CreateMatMul(operands[0], operands[1]);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::matmul), {lhs, rhs},
                   [&]() {
                     return InferOutputShape({lhs.xla_shape(), rhs.xla_shape()},
                                             lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr AdaptiveMaxPool2dBackward(const Value& grad_output,
                                               const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp grad_output = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp input = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_output = BuildAdaptiveMaxPoolNdBackward(
        /*out_backprop=*/grad_output, /*input=*/input, /*pool_dim=*/2);
    return node.ReturnOp(xla_output, loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    XLA_CHECK_EQ(operands.size(), 2);
    return BuildAdaptiveMaxPoolNdBackward(/*out_backprop=*/operands[0],
                                          /*input=*/operands[1],
                                          /*pool_dim=*/2);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::adaptive_max_pool2d_backward),
                   {grad_output, input},
                   [&]() {
                     return InferOutputShape(
                         {grad_output.xla_shape(), input.xla_shape()},
                         lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr AdaptiveAvgPool3dBackward(const Value& grad_output,
                                               const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp grad_output = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp input = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_output = BuildAdaptiveAvgPool3dBackward(
        /*out_backprop=*/grad_output, /*input=*/input);
    return node.ReturnOp(xla_output, loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    XLA_CHECK_EQ(operands.size(), 2);
    return BuildAdaptiveAvgPool3dBackward(/*out_backprop=*/operands[0],
                                          /*input=*/operands[1]);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::adaptive_avg_pool3d_backward),
                   {grad_output, input},
                   [&]() {
                     return InferOutputShape(
                         {grad_output.xla_shape(), input.xla_shape()},
                         lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr AdaptiveAvgPool2dBackward(const Value& grad_output,
                                               const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp grad_output = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp input = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_output = BuildAdaptiveAvgPool2dBackward(
        /*out_backprop=*/grad_output, /*input=*/input);
    return node.ReturnOp(xla_output, loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    XLA_CHECK_EQ(operands.size(), 2);
    return BuildAdaptiveAvgPool2dBackward(/*out_backprop=*/operands[0],
                                          /*input=*/operands[1]);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::adaptive_avg_pool2d_backward),
                   {grad_output, input},
                   [&]() {
                     return InferOutputShape(
                         {grad_output.xla_shape(), input.xla_shape()},
                         lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr ComparisonOp(c10::Symbol kind, const Value& input,
                                  const Value& other) {
  auto lower_fn = [kind](const Node& node,
                         LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_other = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_output = BuildComparisonOp(kind, xla_input, xla_other);
    return node.ReturnOp(xla_output, loctx);
  };
  auto lower_for_shape_fn =
      [kind](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return BuildComparisonOp(kind, operands[0], operands[1]);
  };
  return GenericOp(torch::lazy::OpKind(kind), {input, other},
                   [&]() {
                     return InferOutputShape(
                         {input.xla_shape(), other.xla_shape()},
                         lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr Where(const Value& condition, const Value& input,
                           const Value& other) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_condition = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_other = loctx->GetOutputOp(node.operand(2));
    xla::XlaOp pred_condition =
        ConvertTo(xla_condition, XlaHelpers::TypeOfXlaOp(xla_condition),
                  xla::PrimitiveType::PRED, /*device=*/nullptr);
    auto promoted_branches = XlaHelpers::PromoteShapes(xla_input, xla_other);
    return node.ReturnOp(xla::Select(pred_condition, promoted_branches.first,
                                     promoted_branches.second),
                         loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::where),
                   {condition, input, other}, input.xla_shape(),
                   std::move(lower_fn));
}

torch::lazy::NodePtr ARange(const at::Scalar& start, const at::Scalar& end,
                            const at::Scalar& step,
                            at::ScalarType scalar_type) {
  xla::PrimitiveType type = MakeXlaPrimitiveType(scalar_type,
                                                 /*device=*/nullptr);
  XLA_CHECK_NE(step.toDouble(), 0.0);
  XLA_CHECK(!std::isnan(start.toDouble()) && !std::isnan(end.toDouble()))
      << "unsupported range: " << start.toDouble() << " -> " << end.toDouble();
  XLA_CHECK((start.toDouble() <= end.toDouble() && step.toDouble() > 0.0) ||
            (start.toDouble() >= end.toDouble() && step.toDouble() < 0.0));
  xla::Literal values;
  switch (type) {
    case xla::PrimitiveType::BF16:
      values = XlaHelpers::Range<tensorflow::bfloat16>(
          static_cast<tensorflow::bfloat16>(start.toFloat()),
          static_cast<tensorflow::bfloat16>(end.toFloat()),
          static_cast<tensorflow::bfloat16>(step.toFloat()));
      break;
    case xla::PrimitiveType::F16:
      values =
          XlaHelpers::Range<xla::half>(static_cast<xla::half>(start.toHalf()),
                                       static_cast<xla::half>(end.toHalf()),
                                       static_cast<xla::half>(step.toHalf()));
      break;
    case xla::PrimitiveType::F32:
      values = XlaHelpers::Range<float>(start.toFloat(), end.toFloat(),
                                        step.toFloat());
      break;
    case xla::PrimitiveType::F64:
      values = XlaHelpers::Range<double>(start.toDouble(), end.toDouble(),
                                         step.toDouble());
      break;
    case xla::PrimitiveType::U8:
      values = XlaHelpers::Range<uint8_t>(start.toByte(), end.toByte(),
                                          step.toByte());
      break;
    case xla::PrimitiveType::S8:
      values = XlaHelpers::Range<int8_t>(start.toChar(), end.toChar(),
                                         step.toChar());
      break;
    case xla::PrimitiveType::S16:
      values = XlaHelpers::Range<int16_t>(start.toShort(), end.toShort(),
                                          step.toShort());
      break;
    case xla::PrimitiveType::U16:
      values =
          XlaHelpers::Range<uint16_t>(start.toInt(), end.toInt(), step.toInt());
      break;
    case xla::PrimitiveType::S32:
      values =
          XlaHelpers::Range<int32_t>(start.toInt(), end.toInt(), step.toInt());
      break;
    case xla::PrimitiveType::U32:
      values = XlaHelpers::Range<uint32_t>(start.toLong(), end.toLong(),
                                           step.toLong());
      break;
    case xla::PrimitiveType::S64:
      values = XlaHelpers::Range<int64_t>(start.toLong(), end.toLong(),
                                          step.toLong());
      break;
    case xla::PrimitiveType::U64:
      values = XlaHelpers::Range<uint64_t>(start.toLong(), end.toLong(),
                                           step.toLong());
      break;
    default:
      XLA_ERROR() << "XLA type not supported: " << type;
  }
  return ir::MakeNode<Constant>(std::move(values));
}

torch::lazy::NodePtr BroadcastTensors(absl::Span<const Value> tensors) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    std::vector<xla::XlaOp> xla_operands;
    for (const torch::lazy::Output& operand : node.operands()) {
      xla_operands.push_back(loctx->GetOutputOp(operand));
    }
    return node.ReturnOps(CreateBroadcastTensors(xla_operands), loctx);
  };
  std::vector<xla::Shape> tensor_shapes;
  for (const Value& tensor : tensors) {
    tensor_shapes.push_back(tensor.xla_shape());
  }
  auto lower_for_shape_fn =
      [&](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    auto results = CreateBroadcastTensors(operands);
    return xla::Tuple(results.front().builder(), results);
  };
  return GenericOp(
      torch::lazy::OpKind(at::aten::broadcast_tensors), tensors,
      [&]() { return InferOutputShape(tensor_shapes, lower_for_shape_fn); },
      std::move(lower_fn), /*num_outputs=*/tensors.size());
}

torch::lazy::NodePtr Norm(const Value& input,
                          const c10::optional<at::Scalar>& p,
                          c10::optional<at::ScalarType> dtype,
                          absl::Span<const int64_t> dims, bool keepdim) {
  ScopePusher ir_scope(at::aten::norm.toQualString());
  auto dimensions = torch::lazy::ToVector<int64_t>(dims);
  if (dimensions.empty()) {
    dimensions = torch::lazy::Iota<int64_t>(input.xla_shape().rank());
  }
  if (!p.has_value() || p->toDouble() == 2.0) {
    torch::lazy::NodePtr square = input * input;
    torch::lazy::NodePtr result =
        ir::MakeNode<Sum>(square, dimensions, keepdim, dtype);
    return Sqrt(result);
  }
  double norm_value = p->toDouble();
  if (norm_value == 1.0) {
    // Contrary to documentation, norm(p=1) has nothing to do with traces and
    // standard mathematical definitions of nuclear norms:
    //
    //   >>> import torch
    //   >>> x = torch.randn(4, 4)
    //   >>> print(torch.norm(x, 1))
    //   tensor(11.9437)
    //   >>> print(torch.trace(x.abs()))
    //   tensor(3.1235)
    //   >>> print(x.abs().sum())
    //   tensor(11.9437)
    return ir::MakeNode<Sum>(Abs(input), dimensions, keepdim, dtype);
  }
  // Generic sum(x^p)^(1/p) norms.
  torch::lazy::NodePtr norm_exp =
      ScalarOp(norm_value, input.xla_shape().element_type());
  torch::lazy::NodePtr norm_exp_inv =
      ScalarOp(1.0 / norm_value, input.xla_shape().element_type());
  torch::lazy::NodePtr exp = Pow(Abs(input), norm_exp);
  torch::lazy::NodePtr result =
      ir::MakeNode<Sum>(exp, dimensions, keepdim, dtype);
  return Pow(result, norm_exp_inv);
}

torch::lazy::NodePtr Identity(int64_t lines, int64_t cols,
                              xla::PrimitiveType element_type) {
  auto lower_fn = [=](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    return node.ReturnOp(
        xla::IdentityMatrix(loctx->builder(), element_type, lines, cols),
        loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::eye),
                   xla::ShapeUtil::MakeShape(element_type, {lines, cols}),
                   std::move(lower_fn), /*num_outputs=*/1,
                   torch::lazy::MHash(lines, cols));
}

torch::lazy::NodePtr Elu(const Value& input, const at::Scalar& alpha,
                         const at::Scalar& scale,
                         const at::Scalar& input_scale) {
  ScopePusher ir_scope(at::aten::elu.toQualString());
  const xla::Shape& shape = input.xla_shape();
  torch::lazy::NodePtr scaled_input = input * ScalarOp(input_scale, shape);
  torch::lazy::NodePtr zero = ScalarOp(0, shape);
  torch::lazy::NodePtr one = ScalarOp(1, shape);
  torch::lazy::NodePtr alpha_scalar = ScalarOp(alpha, shape);
  return Where(ComparisonOp(at::aten::le, input, zero),
               alpha_scalar * (Exp(scaled_input) - one), input) *
         ScalarOp(scale, shape);
}

torch::lazy::NodePtr EluBackward(const Value& grad_output, const Value& output,
                                 const at::Scalar& alpha,
                                 const at::Scalar& scale,
                                 const at::Scalar& input_scale) {
  ScopePusher ir_scope(at::aten::elu_backward.toQualString());
  const xla::Shape& shape = grad_output.xla_shape();
  torch::lazy::NodePtr negative_output_branch =
      ScalarOp(input_scale, shape) *
      (output + ScalarOp(alpha, shape) * ScalarOp(scale, shape));
  torch::lazy::NodePtr positive_output_branch = ScalarOp(scale, shape);
  return grad_output *
         Where(ComparisonOp(at::aten::gt, output, ScalarOp(0, shape)),
               positive_output_branch, negative_output_branch);
}

torch::lazy::NodePtr Gelu(const Value& input) {
  ScopePusher ir_scope("aten::gelu");
  const xla::Shape& shape = input.xla_shape();
  // input * 0.5 * (1.0 + torch.erf(input / math.sqrt(2.0)))
  return input * ScalarOp(0.5, shape) *
         (Erf(input * ScalarOp(M_SQRT1_2, shape)) + ScalarOp(1.0, shape));
}

torch::lazy::NodePtr GeluBackward(const Value& grad, const Value& input) {
  ScopePusher ir_scope("aten::gelu_backward");
  const xla::Shape& shape = input.xla_shape();
  constexpr float kAlpha = M_2_SQRTPI * M_SQRT1_2 * 0.5;
  torch::lazy::NodePtr scratch = Erf(input * ScalarOp(M_SQRT1_2, shape));
  torch::lazy::NodePtr dinput = Exp(input * input * ScalarOp(-0.5, shape));
  return grad * (ScalarOp(0.5, shape) * (ScalarOp(1.0, shape) + scratch) +
                 input * dinput * ScalarOp(kAlpha, shape));
}

torch::lazy::NodePtr Lshift(const Value& input, const at::Scalar& other) {
  ScopePusher ir_scope(at::aten::__lshift__.toQualString());
  return input * ScalarOp(pow(2, other.to<double>()), input.xla_shape());
}

torch::lazy::NodePtr Lshift(const Value& input, const Value& other) {
  ScopePusher ir_scope(at::aten::__lshift__.toQualString());
  return input * Pow(ScalarOp(2, input.xla_shape()), other);
}

torch::lazy::NodePtr Rshift(const Value& input, const at::Scalar& other) {
  ScopePusher ir_scope(at::aten::__rshift__.toQualString());
  return input / ScalarOp(pow(2, other.to<double>()), input.xla_shape());
}

torch::lazy::NodePtr Rshift(const Value& input, const Value& other) {
  ScopePusher ir_scope(at::aten::__rshift__.toQualString());
  return input / Pow(ScalarOp(2, input.xla_shape()), other);
}

torch::lazy::NodePtr Remainder(const Value& input, const Value& divisor) {
  ScopePusher ir_scope(at::aten::remainder.toQualString());
  torch::lazy::NodePtr f = Fmod(input, Abs(divisor));
  return f + divisor * ComparisonOp(at::aten::lt, SignOp(f) * SignOp(divisor),
                                    ScalarOp(0, input.xla_shape()));
}

torch::lazy::NodePtr MaxUnary(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    const xla::Shape& input_shape = XlaHelpers::ShapeOfXlaOp(xla_input);
    xla::PrimitiveType element_type = input_shape.element_type();
    XlaHelpers::MinMax min_max = XlaHelpers::MinMaxValues(element_type);
    xla::XlaOp init_value =
        XlaHelpers::ScalarValue(min_max.min, element_type, loctx->builder());
    xla::XlaOp result = xla::Reduce(
        xla_input, init_value, XlaHelpers::CreateMaxComputation(element_type),
        torch::lazy::Iota<int64_t>(input_shape.rank()));
    return node.ReturnOp(xla::Reshape(result, {}), loctx);
  };
  XLA_CHECK_GT(xla::ShapeUtil::ElementsIn(input.xla_shape()), 0);
  return GenericOp(
      torch::lazy::OpKind(at::aten::max), {input},
      xla::ShapeUtil::MakeShape(input.xla_shape().element_type(), {}),
      std::move(lower_fn));
}

torch::lazy::NodePtr MinUnary(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    const xla::Shape& input_shape = XlaHelpers::ShapeOfXlaOp(xla_input);
    xla::PrimitiveType element_type = input_shape.element_type();
    XlaHelpers::MinMax min_max = XlaHelpers::MinMaxValues(element_type);
    xla::XlaOp init_value =
        XlaHelpers::ScalarValue(min_max.max, element_type, loctx->builder());
    xla::XlaOp result = xla::Reduce(
        xla_input, init_value, XlaHelpers::CreateMinComputation(element_type),
        torch::lazy::Iota<int64_t>(input_shape.rank()));
    return node.ReturnOp(xla::Reshape(result, {}), loctx);
  };
  XLA_CHECK_GT(xla::ShapeUtil::ElementsIn(input.xla_shape()), 0);
  return GenericOp(
      torch::lazy::OpKind(at::aten::min), {input},
      xla::ShapeUtil::MakeShape(input.xla_shape().element_type(), {}),
      std::move(lower_fn));
}

torch::lazy::NodePtr Take(const Value& input, const Value& index) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_index = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp result = BuildTake(xla_input, xla_index);
    return node.ReturnOp(result, loctx);
  };
  xla::Shape result_shape = index.xla_shape();
  result_shape.set_element_type(input.xla_shape().element_type());
  return GenericOp(torch::lazy::OpKind(at::aten::take), {input, index},
                   std::move(result_shape), std::move(lower_fn));
}

torch::lazy::NodePtr TanhGelu(const Value& input) {
  // TODO: add proper lowering function
  ScopePusher ir_scope("aten::tanh_gelu");
  const xla::Shape& shape = input.xla_shape();
  // inner = math.sqrt(2 / math.pi) * (x + 0.044715 * torch.pow(input, 3))
  // input * 0.5 * (1.0 + torch.tanh(inner))
  const static float kBeta = M_SQRT2 * M_2_SQRTPI * 0.5;
  torch::lazy::NodePtr beta = ScalarOp(kBeta, shape);
  torch::lazy::NodePtr kappa = ScalarOp(0.044715, shape);
  torch::lazy::NodePtr three = ScalarOp(3, shape);
  torch::lazy::NodePtr one = ScalarOp(1, shape);
  torch::lazy::NodePtr half = ScalarOp(0.5, shape);
  torch::lazy::NodePtr inner = beta * (input + kappa * Pow(input, three));
  return half * input * (one + Tanh(inner));
}

torch::lazy::NodePtr TanhGeluBackward(const Value& grad, const Value& input) {
  // TODO: add proper lowering function
  ScopePusher ir_scope("aten::tanh_gelu_backward");
  const xla::Shape& shape = input.xla_shape();
  constexpr float kBeta = M_SQRT2 * M_2_SQRTPI * 0.5;
  torch::lazy::NodePtr beta = ScalarOp(kBeta, shape);
  torch::lazy::NodePtr kappa = ScalarOp(0.044715, shape);
  torch::lazy::NodePtr one = ScalarOp(1, shape);
  torch::lazy::NodePtr two = ScalarOp(2, shape);
  torch::lazy::NodePtr three = ScalarOp(3, shape);
  torch::lazy::NodePtr half = ScalarOp(0.5, shape);
  torch::lazy::NodePtr inner = beta * (input + kappa * Pow(input, three));
  torch::lazy::NodePtr tanh_inner = Tanh(inner);

  torch::lazy::NodePtr left = half * input;
  torch::lazy::NodePtr right = one + tanh_inner;

  torch::lazy::NodePtr left_derivative = half * right;

  torch::lazy::NodePtr tanh_derivative = one - tanh_inner * tanh_inner;
  torch::lazy::NodePtr inner_derivative =
      beta * (one + three * kappa * Pow(input, two));
  torch::lazy::NodePtr right_derivative =
      left * tanh_derivative * inner_derivative;

  return grad * (left_derivative + right_derivative);
}

torch::lazy::NodePtr LogDet(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp result = xla::LogDet(xla_input);
    return node.ReturnOp(result, loctx);
  };
  const xla::Shape& input_shape = input.xla_shape();
  XLA_CHECK_GE(input_shape.rank(), 2) << input_shape;
  // The input tensor is ...,N,N
  xla::Shape logdet_shape(input_shape);
  logdet_shape.DeleteDimension(input_shape.rank() - 1);
  logdet_shape.DeleteDimension(input_shape.rank() - 2);
  return GenericOp(torch::lazy::OpKind(at::aten::logdet), {input}, logdet_shape,
                   std::move(lower_fn));
}

torch::lazy::NodePtr Inverse(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp result = BuildInverse(xla_input);
    return node.ReturnOp(result, loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::inverse), {input},
                   input.xla_shape(), std::move(lower_fn));
}

torch::lazy::NodePtr BaddBmm(const Value& lhs, const Value& rhs,
                             const Value& bias, const Value& product_multiplier,
                             const Value& bias_multiplier) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_lhs = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_rhs = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_bias = loctx->GetOutputOp(node.operand(2));
    xla::XlaOp xla_product_multiplier = loctx->GetOutputOp(node.operand(3));
    xla::XlaOp xla_bias_multiplier = loctx->GetOutputOp(node.operand(4));
    std::tie(xla_lhs, xla_rhs) = XlaHelpers::PromoteValues(xla_lhs, xla_rhs);

    return node.ReturnOp(
        BuildMatMulWithMultiplier(xla_lhs, xla_rhs, xla_bias,
                                  xla_product_multiplier, xla_bias_multiplier),
        loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return BuildMatMulWithMultiplier(operands[0], operands[1], operands[2],
                                     operands[3], operands[4]);
  };
  return GenericOp(
      torch::lazy::OpKind(at::aten::baddbmm),
      {lhs, rhs, bias, product_multiplier, bias_multiplier},
      [&]() {
        return InferOutputShape(
            {lhs.xla_shape(), rhs.xla_shape(), bias.xla_shape(),
             product_multiplier.xla_shape(), bias_multiplier.xla_shape()},
            lower_for_shape_fn);
      },
      std::move(lower_fn));
}

torch::lazy::NodePtr Lerp(const Value& start, const Value& end,
                          const Value& weight) {
  ScopePusher ir_scope(at::aten::lerp.toQualString());
  return start + weight * (end - start);
}

torch::lazy::NodePtr LogicalNot(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp op = loctx->GetOutputOp(node.operand(0));
    return node.ReturnOp(XlaHelpers::PromotedLogicalUnaryOp(
                             op, [](xla::XlaOp lhs) { return xla::Not(lhs); }),
                         loctx);
  };
  auto shape_fn = [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return XlaHelpers::PromotedLogicalUnaryOp(
        operands[0], [](xla::XlaOp lhs) { return xla::Not(lhs); });
  };
  return GenericOp(
      torch::lazy::OpKind(at::aten::logical_not), {input},
      [&]() { return InferOutputShape({input.xla_shape()}, shape_fn); },
      std::move(lower_fn));
}

torch::lazy::NodePtr LogicalXor(const Value& input, const Value& other) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp op1 = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp op2 = loctx->GetOutputOp(node.operand(1));
    return node.ReturnOp(
        XlaHelpers::PromotedLogicalBinaryOp(
            op1, op2,
            [](xla::XlaOp lhs, xla::XlaOp rhs) { return xla::Xor(lhs, rhs); }),
        loctx);
  };
  auto shape_fn = [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return XlaHelpers::PromotedLogicalBinaryOp(
        operands[0], operands[1],
        [](xla::XlaOp lhs, xla::XlaOp rhs) { return xla::Xor(lhs, rhs); });
  };
  return GenericOp(torch::lazy::OpKind(at::aten::logical_xor), {input, other},
                   [&]() {
                     return InferOutputShape(
                         {input.xla_shape(), other.xla_shape()}, shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr LogicalAnd(const Value& input, const Value& other) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp op1 = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp op2 = loctx->GetOutputOp(node.operand(1));
    return node.ReturnOp(
        XlaHelpers::PromotedLogicalBinaryOp(
            op1, op2,
            [](xla::XlaOp lhs, xla::XlaOp rhs) { return xla::And(lhs, rhs); }),
        loctx);
  };
  auto shape_fn = [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return XlaHelpers::PromotedLogicalBinaryOp(
        operands[0], operands[1],
        [](xla::XlaOp lhs, xla::XlaOp rhs) { return xla::And(lhs, rhs); });
  };
  return GenericOp(torch::lazy::OpKind(at::aten::logical_and), {input, other},
                   [&]() {
                     return InferOutputShape(
                         {input.xla_shape(), other.xla_shape()}, shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr LogicalOr(const Value& input, const Value& other) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp op1 = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp op2 = loctx->GetOutputOp(node.operand(1));
    return node.ReturnOp(
        XlaHelpers::PromotedLogicalBinaryOp(
            op1, op2,
            [](xla::XlaOp lhs, xla::XlaOp rhs) { return xla::Or(lhs, rhs); }),
        loctx);
  };
  auto shape_fn = [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    return XlaHelpers::PromotedLogicalBinaryOp(
        operands[0], operands[1],
        [](xla::XlaOp lhs, xla::XlaOp rhs) { return xla::Or(lhs, rhs); });
  };
  return GenericOp(torch::lazy::OpKind(at::aten::logical_or), {input, other},
                   [&]() {
                     return InferOutputShape(
                         {input.xla_shape(), other.xla_shape()}, shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr XLogY(const Value& input, const Value& other) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_other = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_output = BuildXLogY(xla_input, xla_other);
    return node.ReturnOp(xla_output, loctx);
  };
  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    XLA_CHECK_EQ(operands.size(), 2) << "Unexpected number of operands";
    return BuildXLogY(operands[0], operands[1]);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::xlogy), {input, other},
                   [&]() {
                     return InferOutputShape(
                         {input.xla_shape(), other.xla_shape()},
                         lower_for_shape_fn);
                   },
                   std::move(lower_fn));
}

torch::lazy::NodePtr NanToNum(const Value& input, const Value& nan,
                              const Value& posinf, const Value& neginf) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp nan_replacement = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp posinf_replacement = loctx->GetOutputOp(node.operand(2));
    xla::XlaOp neginf_replacement = loctx->GetOutputOp(node.operand(3));
    xla::XlaOp result =
        xla::Select(xla::IsNan(xla_input), nan_replacement,
                    xla::Select(xla::IsPosInf(xla_input), posinf_replacement,
                                xla::Select(xla::IsNegInf(xla_input),
                                            neginf_replacement, xla_input)));
    return node.ReturnOp(result, loctx);
  };
  return GenericOp(torch::lazy::OpKind(at::aten::nan_to_num),
                   {input, nan, posinf, neginf}, input.xla_shape(),
                   std::move(lower_fn));
}

torch::lazy::NodePtr SLogDet(const Value& input) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::SignAndLogDet result = xla::SLogDet(xla_input);
    return node.ReturnOps({result.sign, result.logdet}, loctx);
  };

  auto lower_for_shape_fn =
      [](absl::Span<const xla::XlaOp> operands) -> xla::XlaOp {
    xla::SignAndLogDet result = xla::SLogDet(operands[0]);
    return xla::Tuple(operands[0].builder(), {result.sign, result.logdet});
  };

  return GenericOp(torch::lazy::OpKind(at::aten::slogdet), {input},
                   [&]() {
                     return InferOutputShape({input.xla_shape()},
                                             lower_for_shape_fn);
                   },
                   std::move(lower_fn), /*num_outputs=*/2);
}

torch::lazy::NodePtr Softplus(const Value& input, const Value& beta,
                              const Value& threshold) {
  auto lower_fn = [](const Node& node, LoweringContext* loctx) -> XlaOpVector {
    xla::XlaOp xla_input = loctx->GetOutputOp(node.operand(0));
    xla::XlaOp xla_beta = loctx->GetOutputOp(node.operand(1));
    xla::XlaOp xla_threshold = loctx->GetOutputOp(node.operand(2));
    xla::XlaOp xla_output = BuildSoftplus(xla_input, xla_beta, xla_threshold);
    return node.ReturnOp(xla_output, loctx);
  };

  return GenericOp(torch::lazy::OpKind(at::aten::softplus),
                   {input, beta, threshold}, input.xla_shape(),
                   std::move(lower_fn));
}

}  // namespace ops
}  // namespace ir
}  // namespace torch_xla
