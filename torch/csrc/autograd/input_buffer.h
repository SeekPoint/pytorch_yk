#pragma once

// The InputBuffer class accumulates a list of Variables for use by a
// function. It implements logic to avoid modifying the passed
// values in-place (adding an input twice will accumulate the result).
// This behaviour is needed and used only in backward graphs.

#include <memory>
#include <utility>
#include <vector>

#include <c10/core/Stream.h>
#include <c10/util/Optional.h>
#include <torch/csrc/autograd/variable.h>

namespace torch {
namespace autograd {

//因为有的节点在反向计算时候，有多个输入，所以在计算梯度的时候， grad_fn 的 输入可能从 很多条路径上累积过来，
//InputBuffer 就是用来累积 grad_fn 的输入。
struct InputBuffer {
  // size 表示有几个输入
  explicit InputBuffer(size_t size) : buffer(size) {}
  InputBuffer(const InputBuffer& other) = delete;
  InputBuffer(InputBuffer&& other) = default;
  explicit InputBuffer(variable_list&& inputs) : buffer(std::move(inputs)){};
  InputBuffer& operator=(InputBuffer&& other) = default;

  // Accumulates the variable at a specified index.
  // The optional CUDA streams determine which stream the accumulation
  // is run on and how the addition is synchronized.
  void add(
      size_t pos,
      Variable&& var,
      const c10::optional<c10::Stream>& opt_producer_stream,
      const c10::optional<c10::Stream>& opt_consumer_stream);

  at::Device device() const;

  Variable operator[](size_t pos) {
    return buffer[pos];
  }

  // Returns the inputs as a list of variables. Destroys given InputBuffer.
  static std::vector<Variable> variables(InputBuffer&& g);
  // Variables, pair 中的 int 代表 version
  std::vector<Variable> buffer;
};

} // namespace autograd
} // namespace torch
