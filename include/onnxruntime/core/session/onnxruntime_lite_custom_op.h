// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

// Summary
// The header has APIs to save custom op authors the trouble of defining schemas,
// which will be inferred by functions' signature, as long as their argument list has types supported here.
// Input could be:
// 1. Tensor of onnx data types.
// 2. Span of onnx data types.
// 3. Scalar of onnx data types.
// A input could be optional if indicated as std::optional<...>.
// For an output, it must be a tensor of onnx data types.
// Further, the header also has utility for a simple custom struct, where resources could be kept, to be registered as a custom op.
// For concrete examples, please search keyword "LiteCustomOpTest" under "<cloned_src_dir>/onnxruntime/test/".
// Note - all APIs in this header are ABI.

#pragma once
#include "onnxruntime_cxx_api.h"
#include <optional>
#include <numeric>
#include <unordered_set>

namespace Ort {
namespace Custom {

class TensorBase {
 public:
  TensorBase(OrtKernelContext* ctx) : ctx_(ctx) {}
  virtual ~TensorBase() {}
  operator bool() const {
    return shape_.has_value();
  }

 protected:
  struct KernelContext ctx_;
  std::optional<std::vector<int64_t>> shape_;
};

template <typename T>
struct Span {
  const T* data_ = {};
  size_t size_ = {};
  void Assign(const T* data, size_t size) {
    data_ = data;
    size_ = size;
  }
  size_t size() const { return size_; }
  T operator[](size_t indice) const {
    return data_[indice];
  }
};

template <typename T>
class Tensor : public TensorBase {
 public:
  using TT = typename std::remove_reference<T>::type;
  Tensor(OrtKernelContext* ctx, size_t indice, bool is_input) : TensorBase(ctx), indice_(indice), is_input_(is_input) {
    if (is_input_) {
      if (indice >= ctx_.GetInputCount()) {
        ORT_CXX_API_THROW("invalid indice for Ort::Custom::Tensor", OrtErrorCode::ORT_INVALID_ARGUMENT);
      }
      const_value_ = ctx_.GetInput(indice);
      auto type_shape_info = const_value_.GetTensorTypeAndShapeInfo();
      shape_ = type_shape_info.GetShape();
    }
  }
  const std::vector<int64_t>& Shape() const {
    if (!shape_.has_value()) {
      ORT_CXX_API_THROW("tensor shape is not yet initialized", OrtErrorCode::ORT_RUNTIME_EXCEPTION);
    }
    return shape_.value();
  }
  int64_t NumberOfElement() const {
    if (shape_.has_value()) {
      return std::accumulate(shape_->begin(), shape_->end(), 1LL, std::multiplies<int64_t>());
    } else {
      return 0;
    }
  }
  const TT* Data() const {
    return reinterpret_cast<const TT*>(const_value_.GetTensorRawData());
  }
  TT* Allocate(const std::vector<int64_t>& shape) {
    shape_ = shape;
    if (!data_) {
      shape_ = shape;
      data_ = ctx_.GetOutput(indice_, shape).template GetTensorMutableData<TT>();
    }
    return data_;
  }
  static TT GetT() { return (TT)0; }
  const Span<T>& AsSpan() {
    if (!shape_.has_value() || shape_->size() != 1) {
      ORT_CXX_API_THROW("invalid shape while trying to get a span out of Ort::Custom::Tensor",
                        OrtErrorCode::ORT_RUNTIME_EXCEPTION);
    }
    span_.Assign(Data(), static_cast<size_t>((*shape_)[0]));
    return span_;
  }
  const T& AsScalar() {
    if (!shape_.has_value() || shape_->size() != 1 || (*shape_)[0] != 1) {
      ORT_CXX_API_THROW("invalid shape while trying to get a scalar from Ort::Custom::Tensor",
                        OrtErrorCode::ORT_RUNTIME_EXCEPTION);
    }
    return *Data();
  }

 private:
  size_t indice_;
  bool is_input_;
  ConstValue const_value_;  // for input
  TT* data_{};              // for output
  Span<T> span_;
};

template <>
class Tensor<std::string> : public TensorBase {
 public:
  using strings = std::vector<std::string>;

  Tensor(OrtKernelContext* ctx, size_t indice, bool is_input) : TensorBase(ctx), indice_(indice), is_input_(is_input) {
    if (is_input_) {
      if (indice >= ctx_.GetInputCount()) {
        ORT_CXX_API_THROW("invalid indice for Ort::Custom::Tensor", OrtErrorCode::ORT_INVALID_ARGUMENT);
      }
      auto const_value = ctx_.GetInput(indice);
      auto type_shape_info = const_value.GetTensorTypeAndShapeInfo();
      shape_ = type_shape_info.GetShape();
      auto num_chars = const_value.GetStringTensorDataLength();
      // note - there will be copy ...
      auto num_strings = static_cast<size_t>(NumberOfElement());
      if (num_strings) {
        std::vector<char> chars(num_chars + 1, '\0');
        std::vector<size_t> offsets(num_strings);
        const_value.GetStringTensorContent(static_cast<void*>(chars.data()), num_chars, offsets.data(), offsets.size());
        auto upper_bound = num_strings - 1;
        input_strings_.resize(num_strings);
        for (size_t i = upper_bound;; --i) {
          if (i < upper_bound) {
            chars[offsets[i + 1]] = '\0';
          }
          input_strings_[i] = chars.data() + offsets[i];
          if (0 == i) {
            break;
          }
        }
      }
    }
  }
  int64_t NumberOfElement() const {
    if (shape_.has_value()) {
      return std::accumulate(shape_->begin(), shape_->end(), 1ULL, std::multiplies<int64_t>());
    } else {
      return 0;
    }
  }
  const strings& Data() const {
    return input_strings_;
  }
  void SetStringOutput(const strings& ss, const std::vector<int64_t>& dims) {
    shape_ = dims;
    std::vector<const char*> raw;
    for (const auto& s : ss) {
      raw.push_back(s.data());
    }
    auto output = ctx_.GetOutput(indice_, dims.data(), dims.size());
    // note - there will be copy ...
    output.FillStringTensor(raw.data(), raw.size());
  }
  const Span<std::string>& AsSpan() {
    ORT_CXX_API_THROW("span for TensorT of string not implemented", OrtErrorCode::ORT_RUNTIME_EXCEPTION);
  }
  const std::string& AsScalar() {
    if (input_strings_.size() != 1) {
      ORT_CXX_API_THROW("invalid shape while trying to get a scalar string from Ort::Custom::Tensor",
                        OrtErrorCode::ORT_RUNTIME_EXCEPTION);
    }
    return input_strings_[0];
  }

 private:
  size_t indice_;
  bool is_input_;
  std::vector<std::string> input_strings_;  // for input
};

template <>
class Tensor<std::string_view> : public TensorBase {
 public:
  using strings = std::vector<std::string>;
  using string_views = std::vector<std::string_view>;

  Tensor(OrtKernelContext* ctx, size_t indice, bool is_input) : TensorBase(ctx), indice_(indice), is_input_(is_input) {
    if (is_input_) {
      if (indice >= ctx_.GetInputCount()) {
        ORT_CXX_API_THROW("invalid indice for Ort::Custom::Tensor", OrtErrorCode::ORT_INVALID_ARGUMENT);
      }
      auto const_value = ctx_.GetInput(indice);
      auto type_shape_info = const_value.GetTensorTypeAndShapeInfo();
      shape_ = type_shape_info.GetShape();
      auto num_chars = const_value.GetStringTensorDataLength();
      chars_.resize(num_chars + 1, '\0');
      auto num_strings = static_cast<size_t>(NumberOfElement());
      if (num_strings) {
        std::vector<size_t> offsets(num_strings);
        const_value.GetStringTensorContent(static_cast<void*>(chars_.data()), num_chars, offsets.data(), offsets.size());
        offsets.push_back(num_chars);
        for (size_t i = 0; i < num_strings; ++i) {
          input_string_views_.emplace_back(chars_.data() + offsets[i], offsets[i + 1] - offsets[i]);
        }
      }
    }
  }
  int64_t NumberOfElement() const {
    if (shape_.has_value()) {
      return std::accumulate(shape_->begin(), shape_->end(), 1ULL, std::multiplies<int64_t>());
    } else {
      return 0;
    }
  }
  const string_views& Data() const {
    return input_string_views_;
  }
  void SetStringOutput(const strings& ss, const std::vector<int64_t>& dims) {
    shape_ = dims;
    std::vector<const char*> raw;
    for (const auto& s : ss) {
      raw.push_back(s.data());
    }
    auto output = ctx_.GetOutput(indice_, dims.data(), dims.size());
    // note - there will be copy ...
    output.FillStringTensor(raw.data(), raw.size());
  }
  const Span<std::string_view>& AsSpan() {
    ORT_CXX_API_THROW("span for TensorT of string view not implemented", OrtErrorCode::ORT_RUNTIME_EXCEPTION);
  }
  std::string_view AsScalar() {
    if (input_string_views_.size() != 1) {
      ORT_CXX_API_THROW("invalid shape while trying to get a scalar string view from Ort::Custom::Tensor",
                        OrtErrorCode::ORT_RUNTIME_EXCEPTION);
    }
    return input_string_views_[0];
  }

 private:
  size_t indice_;
  bool is_input_;
  std::vector<char> chars_;                           // for input
  std::vector<std::string_view> input_string_views_;  // for input
};

using TensorPtr = std::unique_ptr<Custom::TensorBase>;

//////////////////////////// OrtLiteCustomOp ////////////////////////////////

struct OrtLiteCustomOp : public OrtCustomOp {
  using ConstOptionalFloatTensor = std::optional<const Custom::Tensor<float>&>;
  using OptionalFloatTensor = std::optional<Custom::Tensor<float>>;

  // CreateTuple
  template <size_t ith_input, size_t ith_output, typename... Ts>
  static typename std::enable_if<sizeof...(Ts) == 0, std::tuple<>>::type
  CreateTuple(OrtKernelContext*, std::vector<TensorPtr>&, size_t, size_t, const std::string&) {
    return std::make_tuple();
  }

  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  static typename std::enable_if<std::is_same<T, OrtKernelContext*>::value, std::tuple<T, Ts...>>::type
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) {
    std::tuple<T> current = std::tuple<OrtKernelContext*>{context};
    auto next = CreateTuple<ith_input, ith_output, Ts...>(context, tensors, num_input, num_output, ep);
    return std::tuple_cat(current, next);
  }

#ifdef ORT_CUDA_CTX
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  static typename std::enable_if<std::is_same<T, CudaContext*>::value, std::tuple<T, Ts...>>::type
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) {
    thread_local CudaContext cuda_context;
    cuda_context.Init(*context);
    std::tuple<T> current = std::tuple<CudaContext*>{&cuda_context};
    auto next = CreateTuple<ith_input, ith_output, Ts...>(context, tensors, num_input, num_output, ep);
    return std::tuple_cat(current, next);
  }
#endif

#ifdef ORT_DML_CTX
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>
  static typename std::enable_if<std::is_same<T, DmlContext*>::value, std::tuple<T, Ts...>>::type
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) {
    thread_local DmlContext dml_context;
    dml_context.Init(*context);
    std::tuple<T> current = std::tuple<DmlContext*>{&dml_context};
    auto next = CreateTuple<ith_input, ith_output, Ts...>(context, tensors, num_input, num_output, ep);
    return std::tuple_cat(current, next);
  }
#endif

#define CREATE_TUPLE_INPUT(data_type)                                                                                                   \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, const Custom::Tensor<data_type>*>::value, std::tuple<T, Ts...>>::type                  \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_input, true));                                           \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(tensors.back().get())};                                                   \
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                             \
    return std::tuple_cat(current, next);                                                                                               \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, const Custom::Tensor<data_type>&>::value, std::tuple<T, Ts...>>::type                  \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_input, true));                                           \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors.back().get())};                                                  \
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                             \
    return std::tuple_cat(current, next);                                                                                               \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, std::optional<const Custom::Tensor<data_type>*>>::value, std::tuple<T, Ts...>>::type   \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    if (ith_input < num_input) {                                                                                                        \
      tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_input, true));                                         \
      std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<data_type>*>(tensors.back().get())};                        \
      auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                           \
      return std::tuple_cat(current, next);                                                                                             \
    } else {                                                                                                                            \
      std::tuple<T> current = std::tuple<T>{};                                                                                          \
      auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                           \
      return std::tuple_cat(current, next);                                                                                             \
    }                                                                                                                                   \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, const Custom::Span<data_type>*>::value, std::tuple<T, Ts...>>::type                    \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    if ("CPUExecutionProvider" != ep) {                                                                                                 \
      ORT_CXX_API_THROW("span input could only be applied to CPU EP", OrtErrorCode::ORT_RUNTIME_EXCEPTION);                             \
    }                                                                                                                                   \
    tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_input, true));                                           \
    std::tuple<T> current = std::tuple<T>{&reinterpret_cast<Custom::Tensor<data_type>*>(tensors.back().get())->AsSpan()};               \
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                             \
    return std::tuple_cat(current, next);                                                                                               \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, const Custom::Span<data_type>&>::value, std::tuple<T, Ts...>>::type                    \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    if ("CPUExecutionProvider" != ep) {                                                                                                 \
      ORT_CXX_API_THROW("span input could only be applied to CPU EP", OrtErrorCode::ORT_RUNTIME_EXCEPTION);                             \
    }                                                                                                                                   \
    tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_input, true));                                           \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<data_type>*>(tensors.back().get())->AsSpan()};                \
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                             \
    return std::tuple_cat(current, next);                                                                                               \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, std::optional<const Custom::Span<data_type>*>>::value, std::tuple<T, Ts...>>::type     \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    if (ith_input < num_input) {                                                                                                        \
      if ("CPUExecutionProvider" != ep) {                                                                                               \
        ORT_CXX_API_THROW("span input could only be applied to CPU EP", OrtErrorCode::ORT_RUNTIME_EXCEPTION);                           \
      }                                                                                                                                 \
      tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_input, true));                                         \
      std::tuple<T> current = std::tuple<T>{&reinterpret_cast<Custom::Tensor<data_type>*>(tensors.back().get())->AsSpan()};             \
      auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                           \
      return std::tuple_cat(current, next);                                                                                             \
    } else {                                                                                                                            \
      std::tuple<T> current = std::tuple<T>{};                                                                                          \
      auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                           \
      return std::tuple_cat(current, next);                                                                                             \
    }                                                                                                                                   \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, data_type>::value, std::tuple<T, Ts...>>::type                                         \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    if ("CPUExecutionProvider" != ep) {                                                                                                 \
      ORT_CXX_API_THROW("scalar input could only be applied to CPU EP", OrtErrorCode::ORT_RUNTIME_EXCEPTION);                           \
    }                                                                                                                                   \
    tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_input, true));                                           \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<data_type>*>(tensors.back().get())->AsScalar()};              \
    auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                             \
    return std::tuple_cat(current, next);                                                                                               \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, std::optional<data_type>>::value, std::tuple<T, Ts...>>::type                          \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    if (ith_input < num_input) {                                                                                                        \
      if ("CPUExecutionProvider" != ep) {                                                                                               \
        ORT_CXX_API_THROW("scalar input could only be applied to CPU EP", OrtErrorCode::ORT_RUNTIME_EXCEPTION);                         \
      }                                                                                                                                 \
      tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_input, true));                                         \
      std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<data_type>*>(tensors.back().get())->AsScalar()};            \
      auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                           \
      return std::tuple_cat(current, next);                                                                                             \
    } else {                                                                                                                            \
      std::tuple<T> current = std::tuple<T>{};                                                                                          \
      auto next = CreateTuple<ith_input + 1, ith_output, Ts...>(context, tensors, num_input, num_output, ep);                           \
      return std::tuple_cat(current, next);                                                                                             \
    }                                                                                                                                   \
  }
#define CREATE_TUPLE_OUTPUT(data_type)                                                                                                  \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, Custom::Tensor<data_type>*>::value, std::tuple<T, Ts...>>::type                        \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_output, false));                                         \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(tensors.back().get())};                                                   \
    auto next = CreateTuple<ith_input, ith_output + 1, Ts...>(context, tensors, num_input, num_output, ep);                             \
    return std::tuple_cat(current, next);                                                                                               \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, Custom::Tensor<data_type>&>::value, std::tuple<T, Ts...>>::type                        \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_output, false));                                         \
    std::tuple<T> current = std::tuple<T>{reinterpret_cast<T>(*tensors.back().get())};                                                  \
    auto next = CreateTuple<ith_input, ith_output + 1, Ts...>(context, tensors, num_input, num_output, ep);                             \
    return std::tuple_cat(current, next);                                                                                               \
  }                                                                                                                                     \
  template <size_t ith_input, size_t ith_output, typename T, typename... Ts>                                                            \
  static typename std::enable_if<std::is_same<T, std::optional<Custom::Tensor<data_type>*>>::value, std::tuple<T, Ts...>>::type         \
  CreateTuple(OrtKernelContext* context, std::vector<TensorPtr>& tensors, size_t num_input, size_t num_output, const std::string& ep) { \
    if (ith_output < num_output) {                                                                                                      \
      tensors.push_back(std::make_unique<Custom::Tensor<data_type>>(context, ith_output, false));                                       \
      std::tuple<T> current = std::tuple<T>{reinterpret_cast<Custom::Tensor<data_type>*>(tensors.back().get())};                        \
      auto next = CreateTuple<ith_input, ith_output + 1, Ts...>(context, tensors, num_input, num_output, ep);                           \
      return std::tuple_cat(current, next);                                                                                             \
    } else {                                                                                                                            \
      std::tuple<T> current = std::tuple<T>{};                                                                                          \
      auto next = CreateTuple<ith_input, ith_output + 1, Ts...>(context, tensors, num_input, num_output, ep);                           \
      return std::tuple_cat(current, next);                                                                                             \
    }                                                                                                                                   \
  }
#define CREATE_TUPLE(data_type) \
  CREATE_TUPLE_INPUT(data_type) \
  CREATE_TUPLE_OUTPUT(data_type)

  CREATE_TUPLE(bool)
  CREATE_TUPLE(float)
  CREATE_TUPLE(Ort::Float16_t)
  CREATE_TUPLE(Ort::BFloat16_t)
  CREATE_TUPLE(double)
  CREATE_TUPLE(int8_t)
  CREATE_TUPLE(int16_t)
  CREATE_TUPLE(int32_t)
  CREATE_TUPLE(int64_t)
  CREATE_TUPLE(uint8_t)
  CREATE_TUPLE(uint16_t)
  CREATE_TUPLE(uint32_t)
  CREATE_TUPLE(uint64_t)
  CREATE_TUPLE(std::string)
  CREATE_TUPLE_INPUT(std::string_view)

  // ParseArgs ...
  template <typename... Ts>
  static typename std::enable_if<0 == sizeof...(Ts)>::type
  ParseArgs(std::vector<ONNXTensorElementDataType>&, std::vector<ONNXTensorElementDataType>&) {
  }

  template <typename T, typename... Ts>
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, OrtKernelContext*>::value>::type
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {
    ParseArgs<Ts...>(input_types, output_types);
  }

#ifdef ORT_CUDA_CTX
  template <typename T, typename... Ts>
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, CudaContext*>::value>::type
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {
    ParseArgs<Ts...>(input_types, output_types);
  }
#endif

#ifdef ORT_DML_CTX
  template <typename T, typename... Ts>
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, DmlContext*>::value>::type
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {
    ParseArgs<Ts...>(input_types, output_types);
  }
#endif

#define PARSE_INPUT_BASE(pack_type, onnx_type)                                                                           \
  template <typename T, typename... Ts>                                                                                  \
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, pack_type>::value>::type                          \
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) { \
    input_types.push_back(onnx_type);                                                                                    \
    ParseArgs<Ts...>(input_types, output_types);                                                                         \
  }                                                                                                                      \
  template <typename T, typename... Ts>                                                                                  \
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, std::optional<pack_type>>::value>::type           \
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) { \
    input_types.push_back(onnx_type);                                                                                    \
    ParseArgs<Ts...>(input_types, output_types);                                                                         \
  }

#define PARSE_INPUT(data_type, onnx_type)                       \
  PARSE_INPUT_BASE(const Custom::Tensor<data_type>*, onnx_type) \
  PARSE_INPUT_BASE(const Custom::Tensor<data_type>&, onnx_type) \
  PARSE_INPUT_BASE(const Custom::Span<data_type>*, onnx_type)   \
  PARSE_INPUT_BASE(const Custom::Span<data_type>&, onnx_type)   \
  PARSE_INPUT_BASE(data_type, onnx_type)

#define PARSE_OUTPUT(data_type, onnx_type)                                                                                      \
  template <typename T, typename... Ts>                                                                                         \
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<data_type>*>::value>::type                \
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {        \
    output_types.push_back(onnx_type);                                                                                          \
    ParseArgs<Ts...>(input_types, output_types);                                                                                \
  }                                                                                                                             \
  template <typename T, typename... Ts>                                                                                         \
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, Custom::Tensor<data_type>&>::value>::type                \
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {        \
    output_types.push_back(onnx_type);                                                                                          \
    ParseArgs<Ts...>(input_types, output_types);                                                                                \
  }                                                                                                                             \
  template <typename T, typename... Ts>                                                                                         \
  static typename std::enable_if<0 <= sizeof...(Ts) && std::is_same<T, std::optional<Custom::Tensor<data_type>*>>::value>::type \
  ParseArgs(std::vector<ONNXTensorElementDataType>& input_types, std::vector<ONNXTensorElementDataType>& output_types) {        \
    output_types.push_back(onnx_type);                                                                                          \
    ParseArgs<Ts...>(input_types, output_types);                                                                                \
  }

#define PARSE_ARGS(data_type, onnx_type) \
  PARSE_INPUT(data_type, onnx_type)      \
  PARSE_OUTPUT(data_type, onnx_type)

  PARSE_ARGS(bool, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL)
  PARSE_ARGS(float, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
  PARSE_ARGS(Ort::Float16_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
  PARSE_ARGS(Ort::BFloat16_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_BFLOAT16)
  PARSE_ARGS(double, ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE)
  PARSE_ARGS(int8_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8)
  PARSE_ARGS(int16_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT16)
  PARSE_ARGS(int32_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32)
  PARSE_ARGS(int64_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64)
  PARSE_ARGS(uint8_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8)
  PARSE_ARGS(uint16_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT16)
  PARSE_ARGS(uint32_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT32)
  PARSE_ARGS(uint64_t, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT64)
  PARSE_ARGS(std::string, ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING)
  PARSE_ARGS(std::string_view, ONNX_TENSOR_ELEMENT_DATA_TYPE_STRING)  // todo - remove string_view output

  OrtLiteCustomOp(const char* op_name,
                  const char* execution_provider) : op_name_(op_name),
                                                    execution_provider_(execution_provider) {
    OrtCustomOp::version = ORT_API_VERSION;

    OrtCustomOp::GetName = [](const OrtCustomOp* op) { return static_cast<const OrtLiteCustomOp*>(op)->op_name_.c_str(); };
    OrtCustomOp::GetExecutionProviderType = [](const OrtCustomOp* op) { return ((OrtLiteCustomOp*)op)->execution_provider_.c_str(); };
    OrtCustomOp::GetInputMemoryType = [](const OrtCustomOp*, size_t) { return OrtMemTypeDefault; };

    OrtCustomOp::GetInputTypeCount = [](const OrtCustomOp* op) {
      auto self = reinterpret_cast<const OrtLiteCustomOp*>(op);
      return self->input_types_.size();
    };

    OrtCustomOp::GetInputType = [](const OrtCustomOp* op, size_t indice) {
      auto self = reinterpret_cast<const OrtLiteCustomOp*>(op);
      return self->input_types_[indice];
    };

    OrtCustomOp::GetOutputTypeCount = [](const OrtCustomOp* op) {
      auto self = reinterpret_cast<const OrtLiteCustomOp*>(op);
      return self->output_types_.size();
    };

    OrtCustomOp::GetOutputType = [](const OrtCustomOp* op, size_t indice) {
      auto self = reinterpret_cast<const OrtLiteCustomOp*>(op);
      return self->output_types_[indice];
    };

    OrtCustomOp::GetInputCharacteristic = [](const OrtCustomOp*, size_t) {
      return INPUT_OUTPUT_OPTIONAL;
    };

    OrtCustomOp::GetOutputCharacteristic = [](const OrtCustomOp*, size_t) {
      return INPUT_OUTPUT_OPTIONAL;
    };

    OrtCustomOp::GetVariadicInputMinArity = [](const OrtCustomOp*) { return 0; };
    OrtCustomOp::GetVariadicInputHomogeneity = [](const OrtCustomOp*) { return 0; };
    OrtCustomOp::GetVariadicOutputMinArity = [](const OrtCustomOp*) { return 0; };
    OrtCustomOp::GetVariadicOutputHomogeneity = [](const OrtCustomOp*) { return 0; };
  }

  const std::string op_name_;
  const std::string execution_provider_;

  std::vector<ONNXTensorElementDataType> input_types_;
  std::vector<ONNXTensorElementDataType> output_types_;
};

//////////////////////////// OrtLiteCustomFunc ////////////////////////////////
// The struct is to implement function-as-op.
// E.g. a function might be defined as:
//   void Filter(const Ort::Custom::Tensor<float>& floats_in, Ort::Custom::Tensor<float>& floats_out) { ... }
// It could be registered this way:
//   Ort::CustomOpDomain v2_domain{"v2"};
//   std::unique_ptr<OrtLiteCustomOp> fil_op_ptr{Ort::Custom::CreateLiteCustomOp("Filter", "CPUExecutionProvider", Filter)};
//   v2_domain.Add(fil_op_ptr.get());
//   session_options.Add(v2_domain);
// For the complete example, please search keyword "LiteCustomOpTest" under "<cloned_src_dir>/onnxruntime/test/".
template <typename... Args>
struct OrtLiteCustomFunc : public OrtLiteCustomOp {
  using ComputeFn = void (*)(Args...);
  using MyType = OrtLiteCustomFunc<Args...>;

  struct Kernel {
    size_t num_input_{};
    size_t num_output_{};
    ComputeFn compute_fn_{};
    std::string ep_{};
  };

  OrtLiteCustomFunc(const char* op_name,
                    const char* execution_provider,
                    ComputeFn compute_fn) : OrtLiteCustomOp(op_name, execution_provider),
                                            compute_fn_(compute_fn) {
    ParseArgs<Args...>(input_types_, output_types_);

    OrtCustomOp::KernelCompute = [](void* op_kernel, OrtKernelContext* context) {
      auto kernel = reinterpret_cast<Kernel*>(op_kernel);
      std::vector<TensorPtr> tensors;
      auto t = CreateTuple<0, 0, Args...>(context, tensors, kernel->num_input_, kernel->num_output_, kernel->ep_);
      std::apply([kernel](Args const&... t_args) { kernel->compute_fn_(t_args...); }, t);
    };

    OrtCustomOp::CreateKernel = [](const OrtCustomOp* this_, const OrtApi* ort_api, const OrtKernelInfo* info) {
      auto kernel = std::make_unique<Kernel>();
      kernel->compute_fn_ = static_cast<const MyType*>(this_)->compute_fn_;
      Ort::ThrowOnError(ort_api->KernelInfo_GetInputCount(info, &kernel->num_input_));
      Ort::ThrowOnError(ort_api->KernelInfo_GetOutputCount(info, &kernel->num_output_));
      auto self = static_cast<const OrtLiteCustomFunc*>(this_);
      kernel->ep_ = self->execution_provider_;
      return reinterpret_cast<void*>(kernel.release());
    };

    OrtCustomOp::KernelDestroy = [](void* op_kernel) {
      delete reinterpret_cast<Kernel*>(op_kernel);
    };
  }

  ComputeFn compute_fn_;
};  // struct OrtLiteCustomFunc

/////////////////////////// OrtLiteCustomStruct ///////////////////////////
// The struct is to implement struct-as-op.
// E.g. a struct might be defined as:
//   struct Merge {
//      Merge(const OrtApi* ort_api, const OrtKernelInfo* info) {...}
//      void Compute(const Ort::Custom::Tensor<std::string_view>& strings_in,
//                   std::string_view string_in,
//                   Ort::Custom::Tensor<std::string>* strings_out) {...}
//      bool reverse_ = false;
//   };
// It could be registered this way:
//   Ort::CustomOpDomain v2_domain{"v2"};
//   std::unique_ptr<OrtLiteCustomOp> mrg_op_ptr{Ort::Custom::CreateLiteCustomOp<Merge>("Merge", "CPUExecutionProvider")};
//   v2_domain.Add(mrg_op_ptr.get());
//   session_options.Add(v2_domain);
// For the complete example, please search keyword "LiteCustomOpTest" under "<cloned_src_dir>/onnxruntime/test/".
template <typename CustomOp>
struct OrtLiteCustomStruct : public OrtLiteCustomOp {
  template <typename... Args>
  using CustomComputeFn = void (CustomOp::*)(Args...);
  using MyType = OrtLiteCustomStruct<CustomOp>;

  struct Kernel {
    size_t num_input_{};
    size_t num_output_{};
    std::unique_ptr<CustomOp> custom_op_;
    std::string ep_{};
  };

  OrtLiteCustomStruct(const char* op_name,
                      const char* execution_provider) : OrtLiteCustomOp(op_name,
                                                                        execution_provider) {
    init(&CustomOp::Compute);
  }

  template <typename... Args>
  void init(CustomComputeFn<Args...>) {
    ParseArgs<Args...>(input_types_, output_types_);

    OrtCustomOp::KernelCompute = [](void* op_kernel, OrtKernelContext* context) {
      auto kernel = reinterpret_cast<Kernel*>(op_kernel);
      std::vector<TensorPtr> tensors;
      auto t = CreateTuple<0, 0, Args...>(context, tensors, kernel->num_input_, kernel->num_output_, kernel->ep_);
      std::apply([kernel](Args const&... t_args) { kernel->custom_op_->Compute(t_args...); }, t);
    };

    OrtCustomOp::CreateKernel = [](const OrtCustomOp* this_, const OrtApi* ort_api, const OrtKernelInfo* info) {
      auto kernel = std::make_unique<Kernel>();
      Ort::ThrowOnError(ort_api->KernelInfo_GetInputCount(info, &kernel->num_input_));
      Ort::ThrowOnError(ort_api->KernelInfo_GetOutputCount(info, &kernel->num_output_));
      kernel->custom_op_ = std::make_unique<CustomOp>(ort_api, info);
      auto self = static_cast<const OrtLiteCustomStruct*>(this_);
      kernel->ep_ = self->execution_provider_;
      return reinterpret_cast<void*>(kernel.release());
    };

    OrtCustomOp::KernelDestroy = [](void* op_kernel) {
      delete reinterpret_cast<Kernel*>(op_kernel);
    };
  }
};  // struct OrtLiteCustomStruct

/////////////////////////// CreateLiteCustomOp ////////////////////////////

template <typename... Args>
OrtLiteCustomOp* CreateLiteCustomOp(const char* op_name,
                                    const char* execution_provider,
                                    void (*custom_compute_fn)(Args...)) {
  using LiteOp = OrtLiteCustomFunc<Args...>;
  return std::make_unique<LiteOp>(op_name, execution_provider, custom_compute_fn).release();
}

template <typename CustomOp>
OrtLiteCustomOp* CreateLiteCustomOp(const char* op_name,
                                    const char* execution_provider) {
  using LiteOp = OrtLiteCustomStruct<CustomOp>;
  return std::make_unique<LiteOp>(op_name, execution_provider).release();
}

}  // namespace Custom
}  // namespace Ort
