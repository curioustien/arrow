// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "arrow/array/builder_nested.h"
#include "arrow/array/builder_primitive.h"
#include "arrow/array/concatenate.h"
#include "arrow/buffer_builder.h"
#include "arrow/compute/api_aggregate.h"
#include "arrow/compute/api_vector.h"
#include "arrow/compute/kernel.h"
#include "arrow/compute/kernels/aggregate_internal.h"
#include "arrow/compute/kernels/aggregate_var_std_internal.h"
#include "arrow/compute/kernels/common_internal.h"
#include "arrow/compute/kernels/pivot_internal.h"
#include "arrow/compute/kernels/util_internal.h"
#include "arrow/compute/row/grouper.h"
#include "arrow/compute/row/row_encoder_internal.h"
#include "arrow/record_batch.h"
#include "arrow/stl_allocator.h"
#include "arrow/type_traits.h"
#include "arrow/util/bit_run_reader.h"
#include "arrow/util/bit_util.h"
#include "arrow/util/bitmap_ops.h"
#include "arrow/util/bitmap_writer.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/cpu_info.h"
#include "arrow/util/int128_internal.h"
#include "arrow/util/int_util_overflow.h"
#include "arrow/util/ree_util.h"
#include "arrow/util/span.h"
#include "arrow/util/task_group.h"
#include "arrow/util/tdigest.h"
#include "arrow/util/thread_pool.h"
#include "arrow/visit_type_inline.h"

namespace arrow {

using internal::checked_cast;
using internal::FirstTimeBitmapWriter;
using util::span;

namespace compute {
namespace internal {
namespace {

/// C++ abstract base class for the HashAggregateKernel interface.
/// Implementations should be default constructible and perform initialization in
/// Init().
struct GroupedAggregator : KernelState {
  virtual Status Init(ExecContext*, const KernelInitArgs& args) = 0;

  virtual Status Resize(int64_t new_num_groups) = 0;

  virtual Status Consume(const ExecSpan& batch) = 0;

  virtual Status Merge(GroupedAggregator&& other, const ArrayData& group_id_mapping) = 0;

  virtual Result<Datum> Finalize() = 0;

  virtual std::shared_ptr<DataType> out_type() const = 0;
};

template <typename Impl>
Result<std::unique_ptr<KernelState>> HashAggregateInit(KernelContext* ctx,
                                                       const KernelInitArgs& args) {
  auto impl = std::make_unique<Impl>();
  RETURN_NOT_OK(impl->Init(ctx->exec_context(), args));
  // R build with openSUSE155 requires an explicit unique_ptr construction
  return std::unique_ptr<KernelState>(std::move(impl));
}

Status HashAggregateResize(KernelContext* ctx, int64_t num_groups) {
  return checked_cast<GroupedAggregator*>(ctx->state())->Resize(num_groups);
}
Status HashAggregateConsume(KernelContext* ctx, const ExecSpan& batch) {
  return checked_cast<GroupedAggregator*>(ctx->state())->Consume(batch);
}
Status HashAggregateMerge(KernelContext* ctx, KernelState&& other,
                          const ArrayData& group_id_mapping) {
  return checked_cast<GroupedAggregator*>(ctx->state())
      ->Merge(checked_cast<GroupedAggregator&&>(other), group_id_mapping);
}
Status HashAggregateFinalize(KernelContext* ctx, Datum* out) {
  return checked_cast<GroupedAggregator*>(ctx->state())->Finalize().Value(out);
}

Result<TypeHolder> ResolveGroupOutputType(KernelContext* ctx,
                                          const std::vector<TypeHolder>&) {
  return checked_cast<GroupedAggregator*>(ctx->state())->out_type();
}

HashAggregateKernel MakeKernel(std::shared_ptr<KernelSignature> signature,
                               KernelInit init, const bool ordered = false) {
  HashAggregateKernel kernel(std::move(signature), std::move(init), HashAggregateResize,
                             HashAggregateConsume, HashAggregateMerge,
                             HashAggregateFinalize, ordered);
  return kernel;
}

HashAggregateKernel MakeKernel(InputType argument_type, KernelInit init,
                               const bool ordered = false) {
  return MakeKernel(
      KernelSignature::Make({std::move(argument_type), InputType(Type::UINT32)},
                            OutputType(ResolveGroupOutputType)),
      std::move(init), ordered);
}

HashAggregateKernel MakeUnaryKernel(KernelInit init) {
  return MakeKernel(KernelSignature::Make({InputType(Type::UINT32)},
                                          OutputType(ResolveGroupOutputType)),
                    std::move(init));
}

using HashAggregateKernelFactory =
    std::function<Result<HashAggregateKernel>(const std::shared_ptr<DataType>&)>;

Status AddHashAggKernels(const std::vector<std::shared_ptr<DataType>>& types,
                         HashAggregateKernelFactory make_kernel,
                         HashAggregateFunction* function) {
  for (const auto& ty : types) {
    ARROW_ASSIGN_OR_RAISE(auto kernel, make_kernel(ty));
    RETURN_NOT_OK(function->AddKernel(std::move(kernel)));
  }
  return Status::OK();
}

// ----------------------------------------------------------------------
// Helpers for more easily implementing hash aggregates

template <typename T>
struct GroupedValueTraits {
  using CType = typename TypeTraits<T>::CType;

  static CType Get(const CType* values, uint32_t g) { return values[g]; }
  static void Set(CType* values, uint32_t g, CType v) { values[g] = v; }
  static Status AppendBuffers(TypedBufferBuilder<CType>* destination,
                              const uint8_t* values, int64_t offset, int64_t num_values) {
    RETURN_NOT_OK(
        destination->Append(reinterpret_cast<const CType*>(values) + offset, num_values));
    return Status::OK();
  }
};
template <>
struct GroupedValueTraits<BooleanType> {
  static bool Get(const uint8_t* values, uint32_t g) {
    return bit_util::GetBit(values, g);
  }
  static void Set(uint8_t* values, uint32_t g, bool v) {
    bit_util::SetBitTo(values, g, v);
  }
  static Status AppendBuffers(TypedBufferBuilder<bool>* destination,
                              const uint8_t* values, int64_t offset, int64_t num_values) {
    RETURN_NOT_OK(destination->Reserve(num_values));
    destination->UnsafeAppend(values, offset, num_values);
    return Status::OK();
  }
};

template <typename Type, typename ConsumeValue, typename ConsumeNull>
typename arrow::internal::call_traits::enable_if_return<ConsumeValue, void>::type
VisitGroupedValues(const ExecSpan& batch, ConsumeValue&& valid_func,
                   ConsumeNull&& null_func) {
  auto g = batch[1].array.GetValues<uint32_t>(1);
  if (batch[0].is_array()) {
    VisitArrayValuesInline<Type>(
        batch[0].array,
        [&](typename TypeTraits<Type>::CType val) { valid_func(*g++, val); },
        [&]() { null_func(*g++); });
    return;
  }
  const Scalar& input = *batch[0].scalar;
  if (input.is_valid) {
    const auto val = UnboxScalar<Type>::Unbox(input);
    for (int64_t i = 0; i < batch.length; i++) {
      valid_func(*g++, val);
    }
  } else {
    for (int64_t i = 0; i < batch.length; i++) {
      null_func(*g++);
    }
  }
}

template <typename Type, typename ConsumeValue, typename ConsumeNull>
typename arrow::internal::call_traits::enable_if_return<ConsumeValue, Status>::type
VisitGroupedValues(const ExecSpan& batch, ConsumeValue&& valid_func,
                   ConsumeNull&& null_func) {
  auto g = batch[1].array.GetValues<uint32_t>(1);
  if (batch[0].is_array()) {
    return VisitArrayValuesInline<Type>(
        batch[0].array,
        [&](typename GetViewType<Type>::T val) { return valid_func(*g++, val); },
        [&]() { return null_func(*g++); });
  }
  const Scalar& input = *batch[0].scalar;
  if (input.is_valid) {
    const auto val = UnboxScalar<Type>::Unbox(input);
    for (int64_t i = 0; i < batch.length; i++) {
      RETURN_NOT_OK(valid_func(*g++, val));
    }
  } else {
    for (int64_t i = 0; i < batch.length; i++) {
      RETURN_NOT_OK(null_func(*g++));
    }
  }
  return Status::OK();
}

template <typename Type, typename ConsumeValue>
void VisitGroupedValuesNonNull(const ExecSpan& batch, ConsumeValue&& valid_func) {
  VisitGroupedValues<Type>(batch, std::forward<ConsumeValue>(valid_func),
                           [](uint32_t) {});
}

// ----------------------------------------------------------------------
// Count implementation

// Nullary-count implementation -- COUNT(*).
struct GroupedCountAllImpl : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    counts_ = BufferBuilder(ctx->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    return counts_.Append(added_groups * sizeof(int64_t), 0);
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedCountAllImpl*>(&raw_other);

    auto* counts = counts_.mutable_data_as<int64_t>();
    const auto* other_counts = other->counts_.data_as<int64_t>();

    auto* g = group_id_mapping.GetValues<uint32_t>(1);
    for (int64_t other_g = 0; other_g < group_id_mapping.length; ++other_g, ++g) {
      counts[*g] += other_counts[other_g];
    }
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    auto* counts = counts_.mutable_data_as<int64_t>();
    auto* g_begin = batch[0].array.GetValues<uint32_t>(1);
    for (auto g_itr = g_begin, end = g_itr + batch.length; g_itr != end; g_itr++) {
      counts[*g_itr] += 1;
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto counts, counts_.Finish());
    return std::make_shared<Int64Array>(num_groups_, std::move(counts));
  }

  std::shared_ptr<DataType> out_type() const override { return int64(); }

  int64_t num_groups_ = 0;
  BufferBuilder counts_;
};

struct GroupedCountImpl : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    options_ = checked_cast<const CountOptions&>(*args.options);
    counts_ = BufferBuilder(ctx->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    return counts_.Append(added_groups * sizeof(int64_t), 0);
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedCountImpl*>(&raw_other);

    auto* counts = counts_.mutable_data_as<int64_t>();
    const auto* other_counts = other->counts_.data_as<int64_t>();

    auto* g = group_id_mapping.GetValues<uint32_t>(1);
    for (int64_t other_g = 0; other_g < group_id_mapping.length; ++other_g, ++g) {
      counts[*g] += other_counts[other_g];
    }
    return Status::OK();
  }

  template <bool count_valid>
  struct RunEndEncodedCountImpl {
    /// Count the number of valid or invalid values in a run-end-encoded array.
    ///
    /// \param[in] input the run-end-encoded array
    /// \param[out] counts the counts being accumulated
    /// \param[in] g the group ids of the values in the array
    template <typename RunEndCType>
    void DoCount(const ArraySpan& input, int64_t* counts, const uint32_t* g) {
      ree_util::RunEndEncodedArraySpan<RunEndCType> ree_span(input);
      const auto* physical_validity = ree_util::ValuesArray(input).GetValues<uint8_t>(0);
      auto end = ree_span.end();
      for (auto it = ree_span.begin(); it != end; ++it) {
        const bool is_valid = bit_util::GetBit(physical_validity, it.index_into_array());
        if (is_valid == count_valid) {
          for (int64_t i = 0; i < it.run_length(); ++i, ++g) {
            counts[*g] += 1;
          }
        } else {
          g += it.run_length();
        }
      }
    }

    void operator()(const ArraySpan& input, int64_t* counts, const uint32_t* g) {
      auto ree_type = checked_cast<const RunEndEncodedType*>(input.type);
      switch (ree_type->run_end_type()->id()) {
        case Type::INT16:
          DoCount<int16_t>(input, counts, g);
          break;
        case Type::INT32:
          DoCount<int32_t>(input, counts, g);
          break;
        default:
          DoCount<int64_t>(input, counts, g);
          break;
      }
    }
  };

  Status Consume(const ExecSpan& batch) override {
    auto* counts = counts_.mutable_data_as<int64_t>();
    auto* g_begin = batch[1].array.GetValues<uint32_t>(1);

    if (options_.mode == CountOptions::ALL) {
      for (int64_t i = 0; i < batch.length; ++i, ++g_begin) {
        counts[*g_begin] += 1;
      }
    } else if (batch[0].is_array()) {
      const ArraySpan& input = batch[0].array;
      if (options_.mode == CountOptions::ONLY_VALID) {  // ONLY_VALID
        if (input.type->id() != arrow::Type::NA) {
          const uint8_t* bitmap = input.buffers[0].data;
          if (bitmap) {
            arrow::internal::VisitSetBitRunsVoid(
                bitmap, input.offset, input.length, [&](int64_t offset, int64_t length) {
                  auto g = g_begin + offset;
                  for (int64_t i = 0; i < length; ++i, ++g) {
                    counts[*g] += 1;
                  }
                });
          } else {
            // Array without validity bitmaps require special handling of nulls.
            const bool all_valid = !input.MayHaveLogicalNulls();
            if (all_valid) {
              for (int64_t i = 0; i < input.length; ++i, ++g_begin) {
                counts[*g_begin] += 1;
              }
            } else {
              switch (input.type->id()) {
                case Type::RUN_END_ENCODED:
                  RunEndEncodedCountImpl<true>{}(input, counts, g_begin);
                  break;
                default:  // Generic and forward-compatible version.
                  for (int64_t i = 0; i < input.length; ++i, ++g_begin) {
                    counts[*g_begin] += input.IsValid(i);
                  }
                  break;
              }
            }
          }
        }
      } else {  // ONLY_NULL
        if (input.type->id() == arrow::Type::NA) {
          for (int64_t i = 0; i < batch.length; ++i, ++g_begin) {
            counts[*g_begin] += 1;
          }
        } else if (input.MayHaveLogicalNulls()) {
          if (input.HasValidityBitmap()) {
            auto end = input.offset + input.length;
            for (int64_t i = input.offset; i < end; ++i, ++g_begin) {
              counts[*g_begin] += !bit_util::GetBit(input.buffers[0].data, i);
            }
          } else {
            // Arrays without validity bitmaps require special handling of nulls.
            switch (input.type->id()) {
              case Type::RUN_END_ENCODED:
                RunEndEncodedCountImpl<false>{}(input, counts, g_begin);
                break;
              default:  // Generic and forward-compatible version.
                for (int64_t i = 0; i < input.length; ++i, ++g_begin) {
                  counts[*g_begin] += input.IsNull(i);
                }
                break;
            }
          }
        }
      }
    } else {
      const Scalar& input = *batch[0].scalar;
      if (options_.mode == CountOptions::ONLY_VALID) {
        for (int64_t i = 0; i < batch.length; ++i, ++g_begin) {
          counts[*g_begin] += input.is_valid;
        }
      } else {  // ONLY_NULL
        for (int64_t i = 0; i < batch.length; ++i, ++g_begin) {
          counts[*g_begin] += !input.is_valid;
        }
      }
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto counts, counts_.Finish());
    return std::make_shared<Int64Array>(num_groups_, std::move(counts));
  }

  std::shared_ptr<DataType> out_type() const override { return int64(); }

  int64_t num_groups_ = 0;
  CountOptions options_;
  BufferBuilder counts_;
};

// ----------------------------------------------------------------------
// Sum/Mean/Product implementation

template <typename Type, typename Impl,
          typename AccumulateType = typename FindAccumulatorType<Type>::Type>
struct GroupedReducingAggregator : public GroupedAggregator {
  using AccType = AccumulateType;
  using CType = typename TypeTraits<AccType>::CType;
  using InputCType = typename TypeTraits<Type>::CType;

  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    pool_ = ctx->memory_pool();
    options_ = checked_cast<const ScalarAggregateOptions&>(*args.options);
    reduced_ = TypedBufferBuilder<CType>(pool_);
    counts_ = TypedBufferBuilder<int64_t>(pool_);
    no_nulls_ = TypedBufferBuilder<bool>(pool_);
    out_type_ = GetOutType(args.inputs[0].GetSharedPtr());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    RETURN_NOT_OK(reduced_.Append(added_groups, Impl::NullValue(*out_type_)));
    RETURN_NOT_OK(counts_.Append(added_groups, 0));
    RETURN_NOT_OK(no_nulls_.Append(added_groups, true));
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    CType* reduced = reduced_.mutable_data();
    int64_t* counts = counts_.mutable_data();
    uint8_t* no_nulls = no_nulls_.mutable_data();

    VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, InputCType value) {
          reduced[g] = Impl::Reduce(*out_type_, reduced[g], static_cast<CType>(value));
          counts[g]++;
        },
        [&](uint32_t g) { bit_util::SetBitTo(no_nulls, g, false); });
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other =
        checked_cast<GroupedReducingAggregator<Type, Impl, AccType>*>(&raw_other);

    CType* reduced = reduced_.mutable_data();
    int64_t* counts = counts_.mutable_data();
    uint8_t* no_nulls = no_nulls_.mutable_data();

    const CType* other_reduced = other->reduced_.data();
    const int64_t* other_counts = other->counts_.data();
    const uint8_t* other_no_nulls = other->no_nulls_.data();

    auto g = group_id_mapping.GetValues<uint32_t>(1);
    for (int64_t other_g = 0; other_g < group_id_mapping.length; ++other_g, ++g) {
      counts[*g] += other_counts[other_g];
      reduced[*g] = Impl::Reduce(*out_type_, reduced[*g], other_reduced[other_g]);
      bit_util::SetBitTo(
          no_nulls, *g,
          bit_util::GetBit(no_nulls, *g) && bit_util::GetBit(other_no_nulls, other_g));
    }
    return Status::OK();
  }

  // Generate the values/nulls buffers
  static Result<std::shared_ptr<Buffer>> Finish(MemoryPool* pool,
                                                const ScalarAggregateOptions& options,
                                                const int64_t* counts,
                                                TypedBufferBuilder<CType>* reduced,
                                                int64_t num_groups, int64_t* null_count,
                                                std::shared_ptr<Buffer>* null_bitmap) {
    for (int64_t i = 0; i < num_groups; ++i) {
      if (counts[i] >= options.min_count) continue;

      if ((*null_bitmap) == nullptr) {
        ARROW_ASSIGN_OR_RAISE(*null_bitmap, AllocateBitmap(num_groups, pool));
        bit_util::SetBitsTo((*null_bitmap)->mutable_data(), 0, num_groups, true);
      }

      (*null_count)++;
      bit_util::SetBitTo((*null_bitmap)->mutable_data(), i, false);
    }
    return reduced->Finish();
  }

  Result<Datum> Finalize() override {
    std::shared_ptr<Buffer> null_bitmap = nullptr;
    const int64_t* counts = counts_.data();
    int64_t null_count = 0;

    ARROW_ASSIGN_OR_RAISE(auto values,
                          Impl::Finish(pool_, options_, counts, &reduced_, num_groups_,
                                       &null_count, &null_bitmap));

    if (!options_.skip_nulls) {
      null_count = kUnknownNullCount;
      if (null_bitmap) {
        arrow::internal::BitmapAnd(null_bitmap->data(), /*left_offset=*/0,
                                   no_nulls_.data(), /*right_offset=*/0, num_groups_,
                                   /*out_offset=*/0, null_bitmap->mutable_data());
      } else {
        ARROW_ASSIGN_OR_RAISE(null_bitmap, no_nulls_.Finish());
      }
    }

    return ArrayData::Make(out_type(), num_groups_,
                           {std::move(null_bitmap), std::move(values)}, null_count);
  }

  std::shared_ptr<DataType> out_type() const override { return out_type_; }

  template <typename T = Type>
  static enable_if_t<!is_decimal_type<T>::value, std::shared_ptr<DataType>> GetOutType(
      const std::shared_ptr<DataType>& in_type) {
    return TypeTraits<AccType>::type_singleton();
  }

  template <typename T = Type>
  static enable_if_decimal<T, std::shared_ptr<DataType>> GetOutType(
      const std::shared_ptr<DataType>& in_type) {
    return in_type;
  }

  int64_t num_groups_ = 0;
  ScalarAggregateOptions options_;
  TypedBufferBuilder<CType> reduced_;
  TypedBufferBuilder<int64_t> counts_;
  TypedBufferBuilder<bool> no_nulls_;
  std::shared_ptr<DataType> out_type_;
  MemoryPool* pool_;
};

struct GroupedNullImpl : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    pool_ = ctx->memory_pool();
    options_ = checked_cast<const ScalarAggregateOptions&>(*args.options);
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    num_groups_ = new_num_groups;
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override { return Status::OK(); }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    if (options_.skip_nulls && options_.min_count == 0) {
      ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Buffer> data,
                            AllocateBuffer(num_groups_ * sizeof(int64_t), pool_));
      output_empty(data);
      return ArrayData::Make(out_type(), num_groups_, {nullptr, std::move(data)});
    } else {
      return MakeArrayOfNull(out_type(), num_groups_, pool_);
    }
  }

  virtual void output_empty(const std::shared_ptr<Buffer>& data) = 0;

  int64_t num_groups_;
  ScalarAggregateOptions options_;
  MemoryPool* pool_;
};

template <template <typename> class Impl, const char* kFriendlyName, class NullImpl>
struct GroupedReducingFactory {
  template <typename T, typename AccType = typename FindAccumulatorType<T>::Type>
  Status Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), HashAggregateInit<Impl<T>>);
    return Status::OK();
  }

  Status Visit(const Decimal128Type&) {
    kernel =
        MakeKernel(std::move(argument_type), HashAggregateInit<Impl<Decimal128Type>>);
    return Status::OK();
  }

  Status Visit(const Decimal256Type&) {
    kernel =
        MakeKernel(std::move(argument_type), HashAggregateInit<Impl<Decimal256Type>>);
    return Status::OK();
  }

  Status Visit(const NullType&) {
    kernel = MakeKernel(std::move(argument_type), HashAggregateInit<NullImpl>);
    return Status::OK();
  }

  Status Visit(const HalfFloatType& type) {
    return Status::NotImplemented("Computing ", kFriendlyName, " of type ", type);
  }

  Status Visit(const DataType& type) {
    return Status::NotImplemented("Computing ", kFriendlyName, " of type ", type);
  }

  static Result<HashAggregateKernel> Make(const std::shared_ptr<DataType>& type) {
    GroupedReducingFactory<Impl, kFriendlyName, NullImpl> factory;
    factory.argument_type = type->id();
    RETURN_NOT_OK(VisitTypeInline(*type, &factory));
    return std::move(factory.kernel);
  }

  HashAggregateKernel kernel;
  InputType argument_type;
};

// ----------------------------------------------------------------------
// Sum implementation

template <typename Type>
struct GroupedSumImpl : public GroupedReducingAggregator<Type, GroupedSumImpl<Type>> {
  using Base = GroupedReducingAggregator<Type, GroupedSumImpl<Type>>;
  using CType = typename Base::CType;
  using InputCType = typename Base::InputCType;

  // Default value for a group
  static CType NullValue(const DataType&) { return CType(0); }

  template <typename T = Type>
  static enable_if_number<T, CType> Reduce(const DataType&, const CType u,
                                           const InputCType v) {
    return static_cast<CType>(to_unsigned(u) + to_unsigned(static_cast<CType>(v)));
  }

  static CType Reduce(const DataType&, const CType u, const CType v) {
    return static_cast<CType>(to_unsigned(u) + to_unsigned(v));
  }

  using Base::Finish;
};

struct GroupedSumNullImpl final : public GroupedNullImpl {
  std::shared_ptr<DataType> out_type() const override { return int64(); }

  void output_empty(const std::shared_ptr<Buffer>& data) override {
    std::fill_n(data->mutable_data_as<int64_t>(), num_groups_, 0);
  }
};

static constexpr const char kSumName[] = "sum";
using GroupedSumFactory =
    GroupedReducingFactory<GroupedSumImpl, kSumName, GroupedSumNullImpl>;

// ----------------------------------------------------------------------
// Product implementation

template <typename Type>
struct GroupedProductImpl final
    : public GroupedReducingAggregator<Type, GroupedProductImpl<Type>> {
  using Base = GroupedReducingAggregator<Type, GroupedProductImpl<Type>>;
  using AccType = typename Base::AccType;
  using CType = typename Base::CType;
  using InputCType = typename Base::InputCType;

  static CType NullValue(const DataType& out_type) {
    return MultiplyTraits<AccType>::one(out_type);
  }

  template <typename T = Type>
  static enable_if_number<T, CType> Reduce(const DataType& out_type, const CType u,
                                           const InputCType v) {
    return MultiplyTraits<AccType>::Multiply(out_type, u, static_cast<CType>(v));
  }

  static CType Reduce(const DataType& out_type, const CType u, const CType v) {
    return MultiplyTraits<AccType>::Multiply(out_type, u, v);
  }

  using Base::Finish;
};

struct GroupedProductNullImpl final : public GroupedNullImpl {
  std::shared_ptr<DataType> out_type() const override { return int64(); }

  void output_empty(const std::shared_ptr<Buffer>& data) override {
    std::fill_n(data->mutable_data_as<int64_t>(), num_groups_, 1);
  }
};

static constexpr const char kProductName[] = "product";
using GroupedProductFactory =
    GroupedReducingFactory<GroupedProductImpl, kProductName, GroupedProductNullImpl>;

// ----------------------------------------------------------------------
// Mean implementation

template <typename T>
struct GroupedMeanAccType {
  using Type = typename std::conditional<is_number_type<T>::value, DoubleType,
                                         typename FindAccumulatorType<T>::Type>::type;
};

template <typename Type>
struct GroupedMeanImpl
    : public GroupedReducingAggregator<Type, GroupedMeanImpl<Type>,
                                       typename GroupedMeanAccType<Type>::Type> {
  using Base = GroupedReducingAggregator<Type, GroupedMeanImpl<Type>,
                                         typename GroupedMeanAccType<Type>::Type>;
  using CType = typename Base::CType;
  using InputCType = typename Base::InputCType;
  using MeanType =
      typename std::conditional<is_decimal_type<Type>::value, CType, double>::type;

  static CType NullValue(const DataType&) { return CType(0); }

  template <typename T = Type>
  static enable_if_number<T, CType> Reduce(const DataType&, const CType u,
                                           const InputCType v) {
    return static_cast<CType>(u) + static_cast<CType>(v);
  }

  static CType Reduce(const DataType&, const CType u, const CType v) {
    return static_cast<CType>(to_unsigned(u) + to_unsigned(v));
  }

  template <typename T = Type>
  static enable_if_decimal<T, Result<MeanType>> DoMean(CType reduced, int64_t count) {
    static_assert(std::is_same<MeanType, CType>::value, "");
    CType quotient, remainder;
    ARROW_ASSIGN_OR_RAISE(std::tie(quotient, remainder), reduced.Divide(count));
    // Round the decimal result based on the remainder
    remainder.Abs();
    if (remainder * 2 >= count) {
      if (reduced >= 0) {
        quotient += 1;
      } else {
        quotient -= 1;
      }
    }
    return quotient;
  }

  template <typename T = Type>
  static enable_if_t<!is_decimal_type<T>::value, Result<MeanType>> DoMean(CType reduced,
                                                                          int64_t count) {
    return static_cast<MeanType>(reduced) / count;
  }

  static Result<std::shared_ptr<Buffer>> Finish(MemoryPool* pool,
                                                const ScalarAggregateOptions& options,
                                                const int64_t* counts,
                                                TypedBufferBuilder<CType>* reduced_,
                                                int64_t num_groups, int64_t* null_count,
                                                std::shared_ptr<Buffer>* null_bitmap) {
    const CType* reduced = reduced_->data();
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Buffer> values,
                          AllocateBuffer(num_groups * sizeof(MeanType), pool));
    auto* means = values->mutable_data_as<MeanType>();
    for (int64_t i = 0; i < num_groups; ++i) {
      if (counts[i] >= options.min_count) {
        ARROW_ASSIGN_OR_RAISE(means[i], DoMean(reduced[i], counts[i]));
        continue;
      }
      means[i] = MeanType(0);

      if ((*null_bitmap) == nullptr) {
        ARROW_ASSIGN_OR_RAISE(*null_bitmap, AllocateBitmap(num_groups, pool));
        bit_util::SetBitsTo((*null_bitmap)->mutable_data(), 0, num_groups, true);
      }

      (*null_count)++;
      bit_util::SetBitTo((*null_bitmap)->mutable_data(), i, false);
    }
    return values;
  }

  std::shared_ptr<DataType> out_type() const override {
    if (is_decimal_type<Type>::value) return this->out_type_;
    return float64();
  }
};

struct GroupedMeanNullImpl final : public GroupedNullImpl {
  std::shared_ptr<DataType> out_type() const override { return float64(); }

  void output_empty(const std::shared_ptr<Buffer>& data) override {
    std::fill_n(data->mutable_data_as<double>(), num_groups_, 0);
  }
};

static constexpr const char kMeanName[] = "mean";
using GroupedMeanFactory =
    GroupedReducingFactory<GroupedMeanImpl, kMeanName, GroupedMeanNullImpl>;

// Variance/Stdev implementation

using arrow::internal::int128_t;

template <typename Type>
struct GroupedStatisticImpl : public GroupedAggregator {
  using CType = typename TypeTraits<Type>::CType;
  using SumType = typename internal::GetSumType<Type>::SumType;

  // This method is defined solely to make GroupedStatisticImpl instantiable
  // in ConsumeImpl below. It will be redefined in subclasses.
  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    return Status::NotImplemented("");
  }

  // Init helper for hash_variance and hash_stddev
  Status InitInternal(ExecContext* ctx, const KernelInitArgs& args,
                      StatisticType stat_type, const VarianceOptions& options) {
    return InitInternal(ctx, args, stat_type, options.ddof, options.skip_nulls,
                        options.min_count);
  }

  // Init helper for hash_skew and hash_kurtosis
  Status InitInternal(ExecContext* ctx, const KernelInitArgs& args,
                      StatisticType stat_type, const SkewOptions& options) {
    return InitInternal(ctx, args, stat_type, /*ddof=*/0, options.skip_nulls,
                        options.min_count);
  }

  Status InitInternal(ExecContext* ctx, const KernelInitArgs& args,
                      StatisticType stat_type, int ddof, bool skip_nulls,
                      uint32_t min_count) {
    if constexpr (is_decimal_type<Type>::value) {
      int32_t decimal_scale =
          checked_cast<const DecimalType&>(*args.inputs[0].type).scale();
      return InitInternal(ctx, stat_type, decimal_scale, ddof, skip_nulls, min_count);
    } else {
      return InitInternal(ctx, stat_type, /*decimal_scale=*/0, ddof, skip_nulls,
                          min_count);
    }
  }

  Status InitInternal(ExecContext* ctx, StatisticType stat_type, int32_t decimal_scale,
                      int ddof, bool skip_nulls, uint32_t min_count) {
    stat_type_ = stat_type;
    moments_level_ = moments_level_for_statistic(stat_type_);
    decimal_scale_ = decimal_scale;
    skip_nulls_ = skip_nulls;
    min_count_ = min_count;
    ddof_ = ddof;
    ctx_ = ctx;
    pool_ = ctx->memory_pool();
    counts_ = TypedBufferBuilder<int64_t>(pool_);
    means_ = TypedBufferBuilder<double>(pool_);
    m2s_ = TypedBufferBuilder<double>(pool_);
    m3s_ = TypedBufferBuilder<double>(pool_);
    m4s_ = TypedBufferBuilder<double>(pool_);
    no_nulls_ = TypedBufferBuilder<bool>(pool_);
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    RETURN_NOT_OK(counts_.Append(added_groups, 0));
    RETURN_NOT_OK(means_.Append(added_groups, 0));
    RETURN_NOT_OK(m2s_.Append(added_groups, 0));
    if (moments_level_ >= 3) {
      RETURN_NOT_OK(m3s_.Append(added_groups, 0));
      if (moments_level_ >= 4) {
        RETURN_NOT_OK(m4s_.Append(added_groups, 0));
      }
    }
    RETURN_NOT_OK(no_nulls_.Append(added_groups, true));
    return Status::OK();
  }

  template <typename T>
  double ToDouble(T value) const {
    return static_cast<double>(value);
  }
  double ToDouble(const Decimal32& value) const { return value.ToDouble(decimal_scale_); }
  double ToDouble(const Decimal64& value) const { return value.ToDouble(decimal_scale_); }
  double ToDouble(const Decimal128& value) const {
    return value.ToDouble(decimal_scale_);
  }
  double ToDouble(const Decimal256& value) const {
    return value.ToDouble(decimal_scale_);
  }

  Status Consume(const ExecSpan& batch) override {
    constexpr bool kCanUseIntArithmetic = std::is_integral_v<CType> && sizeof(CType) <= 4;

    if constexpr (kCanUseIntArithmetic) {
      if (moments_level_ == 2) {
        return ConsumeIntegral(batch);
      }
    }
    return ConsumeGeneric(batch);
  }

  // float/double/int64/decimal: calculate `m2` (sum((X-mean)^2)) with
  // two pass algorithm (see aggregate_var_std.cc)
  Status ConsumeGeneric(const ExecSpan& batch) {
    GroupedStatisticImpl<Type> state;
    RETURN_NOT_OK(state.InitInternal(ctx_, stat_type_, decimal_scale_, ddof_, skip_nulls_,
                                     min_count_));
    RETURN_NOT_OK(state.Resize(num_groups_));
    int64_t* counts = state.counts_.mutable_data();
    double* means = state.means_.mutable_data();
    uint8_t* no_nulls = state.no_nulls_.mutable_data();

    // XXX this uses naive summation; we should switch to pairwise summation
    // (as the scalar aggregate kernel does) or Kahan summation.
    std::vector<SumType> sums(num_groups_);
    VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, typename TypeTraits<Type>::CType value) {
          sums[g] += static_cast<SumType>(value);
          counts[g]++;
        },
        [&](uint32_t g) { bit_util::ClearBit(no_nulls, g); });

    for (int64_t i = 0; i < num_groups_; i++) {
      means[i] = ToDouble(sums[i]) / counts[i];
    }

    double* m2s = state.m2s_mutable_data();
    double* m3s = state.m3s_mutable_data();
    double* m4s = state.m4s_mutable_data();
    // Having distinct VisitGroupedValuesNonNull calls based on moments_level_
    // would increase code generation for relatively little benefit.
    VisitGroupedValuesNonNull<Type>(
        batch, [&](uint32_t g, typename TypeTraits<Type>::CType value) {
          const double d = ToDouble(value) - means[g];
          const double d2 = d * d;
          switch (moments_level_) {
            case 4:
              m4s[g] += d2 * d2;
              [[fallthrough]];
            case 3:
              m3s[g] += d2 * d;
              [[fallthrough]];
            default:
              m2s[g] += d2;
              break;
          }
        });

    return MergeSameGroups(std::move(state));
  }

  // int32/16/8: textbook one pass algorithm to compute `m2` with integer arithmetic
  // (see aggregate_var_std.cc)
  Status ConsumeIntegral(const ExecSpan& batch) {
    // max number of elements that sum will not overflow int64 (2Gi int32 elements)
    // for uint32:    0 <= sum < 2^63 (int64 >= 0)
    // for int32: -2^62 <= sum < 2^62
    constexpr int64_t max_length = 1ULL << (63 - sizeof(CType) * 8);

    const auto* g = batch[1].array.GetValues<uint32_t>(1);
    if (batch[0].is_scalar() && !batch[0].scalar->is_valid) {
      uint8_t* no_nulls = no_nulls_.mutable_data();
      for (int64_t i = 0; i < batch.length; i++) {
        bit_util::ClearBit(no_nulls, g[i]);
      }
      return Status::OK();
    }

    std::vector<IntegerVarStd> var_std(num_groups_);

    for (int64_t start_index = 0; start_index < batch.length; start_index += max_length) {
      // process in chunks that overflow will never happen

      // reset state
      var_std.clear();
      var_std.resize(num_groups_);
      GroupedStatisticImpl<Type> state;
      RETURN_NOT_OK(state.InitInternal(ctx_, stat_type_, decimal_scale_, ddof_,
                                       skip_nulls_, min_count_));
      RETURN_NOT_OK(state.Resize(num_groups_));
      int64_t* other_counts = state.counts_.mutable_data();
      double* other_means = state.means_.mutable_data();
      double* other_m2s = state.m2s_mutable_data();
      uint8_t* other_no_nulls = state.no_nulls_.mutable_data();

      if (batch[0].is_array()) {
        const ArraySpan& array = batch[0].array;
        const CType* values = array.GetValues<CType>(1);
        auto visit_values = [&](int64_t pos, int64_t len) {
          for (int64_t i = 0; i < len; ++i) {
            const int64_t index = start_index + pos + i;
            const auto value = values[index];
            var_std[g[index]].ConsumeOne(value);
          }
        };

        if (array.MayHaveNulls()) {
          arrow::internal::BitRunReader reader(
              array.buffers[0].data, array.offset + start_index,
              std::min(max_length, batch.length - start_index));
          int64_t position = 0;
          while (true) {
            auto run = reader.NextRun();
            if (run.length == 0) break;
            if (run.set) {
              visit_values(position, run.length);
            } else {
              for (int64_t i = 0; i < run.length; ++i) {
                bit_util::ClearBit(other_no_nulls, g[start_index + position + i]);
              }
            }
            position += run.length;
          }
        } else {
          visit_values(0, array.length);
        }
      } else {
        const auto value = UnboxScalar<Type>::Unbox(*batch[0].scalar);
        for (int64_t i = 0; i < std::min(max_length, batch.length - start_index); ++i) {
          const int64_t index = start_index + i;
          var_std[g[index]].ConsumeOne(value);
        }
      }

      for (int64_t i = 0; i < num_groups_; i++) {
        if (var_std[i].count == 0) continue;

        other_counts[i] = var_std[i].count;
        other_means[i] = var_std[i].mean();
        other_m2s[i] = var_std[i].m2();
      }
      RETURN_NOT_OK(MergeSameGroups(std::move(state)));
    }
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    DCHECK_EQ(group_id_mapping.length,
              checked_cast<GroupedStatisticImpl*>(&raw_other)->num_groups_);
    const uint32_t* g = group_id_mapping.GetValues<uint32_t>(1);
    return MergeInternal(std::move(raw_other),
                         [g](int64_t other_g) { return g[other_g]; });
  }

  Status MergeSameGroups(GroupedAggregator&& raw_other) {
    return MergeInternal(std::move(raw_other), [](int64_t other_g) { return other_g; });
  }

  template <typename GroupIdMapper>
  Status MergeInternal(GroupedAggregator&& raw_other, GroupIdMapper&& group_id_mapper) {
    // Combine moments from two chunks
    auto other = checked_cast<GroupedStatisticImpl*>(&raw_other);
    DCHECK_EQ(moments_level_, other->moments_level_);

    int64_t* counts = counts_.mutable_data();
    double* means = means_.mutable_data();
    double* m2s = m2s_mutable_data();
    // Moments above the current level will just be ignored.
    double* m3s = m3s_mutable_data();
    double* m4s = m4s_mutable_data();
    uint8_t* no_nulls = no_nulls_.mutable_data();

    const int64_t* other_counts = other->counts_.data();
    const double* other_means = other->means_.data();
    const double* other_m2s = other->m2s_data();
    const double* other_m3s = other->m3s_data();
    const double* other_m4s = other->m4s_data();
    const uint8_t* other_no_nulls = other->no_nulls_.data();

    const int64_t num_other_groups = other->num_groups_;

    for (int64_t other_g = 0; other_g < num_other_groups; ++other_g) {
      const auto g = group_id_mapper(other_g);
      if (!bit_util::GetBit(other_no_nulls, other_g)) {
        bit_util::ClearBit(no_nulls, g);
      }
      if (other_counts[other_g] == 0) continue;
      auto moments = Moments::Merge(
          moments_level_, Moments(counts[g], means[g], m2s[g], m3s[g], m4s[g]),
          Moments(other_counts[other_g], other_means[other_g], other_m2s[other_g],
                  other_m3s[other_g], other_m4s[other_g]));
      counts[g] = moments.count;
      means[g] = moments.mean;
      // Fill moments in reverse order, in case m3s or m4s is the same as m2s.
      m4s[g] = moments.m4;
      m3s[g] = moments.m3;
      m2s[g] = moments.m2;
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    std::shared_ptr<Buffer> null_bitmap;
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Buffer> values,
                          AllocateBuffer(num_groups_ * sizeof(double), pool_));
    int64_t null_count = 0;

    auto* results = values->mutable_data_as<double>();
    const int64_t* counts = counts_.data();
    const double* means = means_.data();
    const double* m2s = m2s_data();
    const double* m3s = m3s_data();
    const double* m4s = m4s_data();
    for (int64_t i = 0; i < num_groups_; ++i) {
      if (counts[i] > ddof_ && counts[i] >= min_count_) {
        const auto moments = Moments(counts[i], means[i], m2s[i], m3s[i], m4s[i]);
        switch (stat_type_) {
          case StatisticType::Var:
            results[i] = moments.Variance(ddof_);
            break;
          case StatisticType::Std:
            results[i] = moments.Stddev(ddof_);
            break;
          case StatisticType::Skew:
            results[i] = moments.Skew();
            break;
          case StatisticType::Kurtosis:
            results[i] = moments.Kurtosis();
            break;
          default:
            return Status::NotImplemented("Statistic type ",
                                          static_cast<int>(stat_type_));
        }
        continue;
      }

      results[i] = 0;
      if (null_bitmap == nullptr) {
        ARROW_ASSIGN_OR_RAISE(null_bitmap, AllocateBitmap(num_groups_, pool_));
        bit_util::SetBitsTo(null_bitmap->mutable_data(), 0, num_groups_, true);
      }

      null_count += 1;
      bit_util::SetBitTo(null_bitmap->mutable_data(), i, false);
    }
    if (!skip_nulls_) {
      if (null_bitmap) {
        arrow::internal::BitmapAnd(null_bitmap->data(), 0, no_nulls_.data(), 0,
                                   num_groups_, 0, null_bitmap->mutable_data());
      } else {
        ARROW_ASSIGN_OR_RAISE(null_bitmap, no_nulls_.Finish());
      }
      null_count = kUnknownNullCount;
    }

    return ArrayData::Make(float64(), num_groups_,
                           {std::move(null_bitmap), std::move(values)}, null_count);
  }

  std::shared_ptr<DataType> out_type() const override { return float64(); }

  const double* m2s_data() const { return m2s_.data(); }
  // If moments_level_ < 3, the values read from m3s_data() will be ignored,
  // but we still need to point to a valid buffer of the appropriate size.
  // The trick is to reuse m2s_, which simplifies the code.
  const double* m3s_data() const {
    return (moments_level_ >= 3) ? m3s_.data() : m2s_.data();
  }
  const double* m4s_data() const {
    return (moments_level_ >= 4) ? m4s_.data() : m2s_.data();
  }

  double* m2s_mutable_data() { return m2s_.mutable_data(); }
  double* m3s_mutable_data() {
    return (moments_level_ >= 3) ? m3s_.mutable_data() : m2s_.mutable_data();
  }
  double* m4s_mutable_data() {
    return (moments_level_ >= 4) ? m4s_.mutable_data() : m2s_.mutable_data();
  }

  StatisticType stat_type_;
  int moments_level_;
  int32_t decimal_scale_;
  bool skip_nulls_;
  uint32_t min_count_;
  int ddof_;
  int64_t num_groups_ = 0;
  // m2 = count * s2 = sum((X-mean)^2)
  TypedBufferBuilder<int64_t> counts_;
  TypedBufferBuilder<double> means_, m2s_, m3s_, m4s_;
  TypedBufferBuilder<bool> no_nulls_;
  ExecContext* ctx_;
  MemoryPool* pool_;
};

template <typename Type, typename OptionsType, StatisticType kStatType>
struct ConcreteGroupedStatisticImpl : public GroupedStatisticImpl<Type> {
  using GroupedStatisticImpl<Type>::InitInternal;

  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    const auto& options = checked_cast<const OptionsType&>(*args.options);
    return InitInternal(ctx, args, kStatType, options);
  }
};

template <typename Type>
using GroupedVarianceImpl =
    ConcreteGroupedStatisticImpl<Type, VarianceOptions, StatisticType::Var>;
template <typename Type>
using GroupedStddevImpl =
    ConcreteGroupedStatisticImpl<Type, VarianceOptions, StatisticType::Std>;
template <typename Type>
using GroupedSkewImpl =
    ConcreteGroupedStatisticImpl<Type, SkewOptions, StatisticType::Skew>;
template <typename Type>
using GroupedKurtosisImpl =
    ConcreteGroupedStatisticImpl<Type, SkewOptions, StatisticType::Kurtosis>;

template <template <typename Type> typename GroupedImpl>
Result<HashAggregateKernel> MakeGroupedStatisticKernel(
    const std::shared_ptr<DataType>& type) {
  auto make_kernel = [&](auto&& type) -> Result<HashAggregateKernel> {
    using T = std::decay_t<decltype(type)>;
    // Supporting all number types except float16
    if constexpr (is_integer_type<T>::value ||
                  (is_floating_type<T>::value && !is_half_float_type<T>::value) ||
                  is_decimal_type<T>::value) {
      return MakeKernel(InputType(T::type_id), HashAggregateInit<GroupedImpl<T>>);
    }
    return Status::NotImplemented("Computing higher-order statistic of data of type ",
                                  type);
  };

  return VisitType(*type, make_kernel);
}

Status AddHashAggregateStatisticKernels(HashAggregateFunction* func,
                                        HashAggregateKernelFactory make_kernel) {
  RETURN_NOT_OK(AddHashAggKernels(SignedIntTypes(), make_kernel, func));
  RETURN_NOT_OK(AddHashAggKernels(UnsignedIntTypes(), make_kernel, func));
  RETURN_NOT_OK(AddHashAggKernels(FloatingPointTypes(), make_kernel, func));
  RETURN_NOT_OK(AddHashAggKernels(
      {decimal32(1, 1), decimal64(1, 1), decimal128(1, 1), decimal256(1, 1)}, make_kernel,
      func));
  return Status::OK();
}

// ----------------------------------------------------------------------
// TDigest implementation

using arrow::internal::TDigest;

template <typename Type>
struct GroupedTDigestImpl : public GroupedAggregator {
  using CType = typename TypeTraits<Type>::CType;

  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    options_ = *checked_cast<const TDigestOptions*>(args.options);
    if (is_decimal_type<Type>::value) {
      decimal_scale_ = checked_cast<const DecimalType&>(*args.inputs[0].type).scale();
    } else {
      decimal_scale_ = 0;
    }
    ctx_ = ctx;
    pool_ = ctx->memory_pool();
    counts_ = TypedBufferBuilder<int64_t>(pool_);
    no_nulls_ = TypedBufferBuilder<bool>(pool_);
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    const int64_t added_groups = new_num_groups - tdigests_.size();
    tdigests_.reserve(new_num_groups);
    for (int64_t i = 0; i < added_groups; i++) {
      tdigests_.emplace_back(options_.delta, options_.buffer_size);
    }
    RETURN_NOT_OK(counts_.Append(new_num_groups, 0));
    RETURN_NOT_OK(no_nulls_.Append(new_num_groups, true));
    return Status::OK();
  }

  template <typename T>
  double ToDouble(T value) const {
    return static_cast<double>(value);
  }
  double ToDouble(const Decimal32& value) const { return value.ToDouble(decimal_scale_); }
  double ToDouble(const Decimal64& value) const { return value.ToDouble(decimal_scale_); }
  double ToDouble(const Decimal128& value) const {
    return value.ToDouble(decimal_scale_);
  }
  double ToDouble(const Decimal256& value) const {
    return value.ToDouble(decimal_scale_);
  }

  Status Consume(const ExecSpan& batch) override {
    int64_t* counts = counts_.mutable_data();
    uint8_t* no_nulls = no_nulls_.mutable_data();
    VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, CType value) {
          tdigests_[g].NanAdd(ToDouble(value));
          counts[g]++;
        },
        [&](uint32_t g) { bit_util::SetBitTo(no_nulls, g, false); });
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedTDigestImpl*>(&raw_other);

    int64_t* counts = counts_.mutable_data();
    uint8_t* no_nulls = no_nulls_.mutable_data();

    const int64_t* other_counts = other->counts_.data();
    const uint8_t* other_no_nulls = no_nulls_.mutable_data();

    auto g = group_id_mapping.GetValues<uint32_t>(1);
    for (int64_t other_g = 0; other_g < group_id_mapping.length; ++other_g, ++g) {
      tdigests_[*g].Merge(other->tdigests_[other_g]);
      counts[*g] += other_counts[other_g];
      bit_util::SetBitTo(
          no_nulls, *g,
          bit_util::GetBit(no_nulls, *g) && bit_util::GetBit(other_no_nulls, other_g));
    }

    return Status::OK();
  }

  Result<Datum> Finalize() override {
    const int64_t slot_length = options_.q.size();
    const int64_t num_values = tdigests_.size() * slot_length;
    const int64_t* counts = counts_.data();
    std::shared_ptr<Buffer> null_bitmap;
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Buffer> values,
                          AllocateBuffer(num_values * sizeof(double), pool_));
    int64_t null_count = 0;

    auto* results = values->mutable_data_as<double>();
    for (int64_t i = 0; static_cast<size_t>(i) < tdigests_.size(); ++i) {
      if (!tdigests_[i].is_empty() && counts[i] >= options_.min_count &&
          (options_.skip_nulls || bit_util::GetBit(no_nulls_.data(), i))) {
        for (int64_t j = 0; j < slot_length; j++) {
          results[i * slot_length + j] = tdigests_[i].Quantile(options_.q[j]);
        }
        continue;
      }

      if (!null_bitmap) {
        ARROW_ASSIGN_OR_RAISE(null_bitmap, AllocateBitmap(num_values, pool_));
        bit_util::SetBitsTo(null_bitmap->mutable_data(), 0, num_values, true);
      }
      null_count += slot_length;
      bit_util::SetBitsTo(null_bitmap->mutable_data(), i * slot_length, slot_length,
                          false);
      std::fill(&results[i * slot_length], &results[(i + 1) * slot_length], 0.0);
    }

    auto child = ArrayData::Make(float64(), num_values,
                                 {std::move(null_bitmap), std::move(values)}, null_count);
    return ArrayData::Make(out_type(), tdigests_.size(), {nullptr}, {std::move(child)},
                           /*null_count=*/0);
  }

  std::shared_ptr<DataType> out_type() const override {
    return fixed_size_list(float64(), static_cast<int32_t>(options_.q.size()));
  }

  TDigestOptions options_;
  int32_t decimal_scale_;
  std::vector<TDigest> tdigests_;
  TypedBufferBuilder<int64_t> counts_;
  TypedBufferBuilder<bool> no_nulls_;
  ExecContext* ctx_;
  MemoryPool* pool_;
};

struct GroupedTDigestFactory {
  template <typename T>
  enable_if_number<T, Status> Visit(const T&) {
    kernel =
        MakeKernel(std::move(argument_type), HashAggregateInit<GroupedTDigestImpl<T>>);
    return Status::OK();
  }

  template <typename T>
  enable_if_decimal<T, Status> Visit(const T&) {
    kernel =
        MakeKernel(std::move(argument_type), HashAggregateInit<GroupedTDigestImpl<T>>);
    return Status::OK();
  }

  Status Visit(const HalfFloatType& type) {
    return Status::NotImplemented("Computing t-digest of data of type ", type);
  }

  Status Visit(const DataType& type) {
    return Status::NotImplemented("Computing t-digest of data of type ", type);
  }

  static Result<HashAggregateKernel> Make(const std::shared_ptr<DataType>& type) {
    GroupedTDigestFactory factory;
    factory.argument_type = type->id();
    RETURN_NOT_OK(VisitTypeInline(*type, &factory));
    return std::move(factory.kernel);
  }

  HashAggregateKernel kernel;
  InputType argument_type;
};

HashAggregateKernel MakeApproximateMedianKernel(HashAggregateFunction* tdigest_func) {
  HashAggregateKernel kernel;
  kernel.init = [tdigest_func](
                    KernelContext* ctx,
                    const KernelInitArgs& args) -> Result<std::unique_ptr<KernelState>> {
    ARROW_ASSIGN_OR_RAISE(auto kernel, tdigest_func->DispatchExact(args.inputs));
    const auto& scalar_options =
        checked_cast<const ScalarAggregateOptions&>(*args.options);
    TDigestOptions options;
    // Default q = 0.5
    options.min_count = scalar_options.min_count;
    options.skip_nulls = scalar_options.skip_nulls;
    KernelInitArgs new_args{kernel, args.inputs, &options};
    return kernel->init(ctx, new_args);
  };
  kernel.signature = KernelSignature::Make({InputType::Any(), Type::UINT32}, float64());
  kernel.resize = HashAggregateResize;
  kernel.consume = HashAggregateConsume;
  kernel.merge = HashAggregateMerge;
  kernel.finalize = [](KernelContext* ctx, Datum* out) {
    ARROW_ASSIGN_OR_RAISE(Datum temp,
                          checked_cast<GroupedAggregator*>(ctx->state())->Finalize());
    *out = temp.array_as<FixedSizeListArray>()->values();
    return Status::OK();
  };
  return kernel;
}

// ----------------------------------------------------------------------
// MinMax implementation

template <typename CType>
struct AntiExtrema {
  static constexpr CType anti_min() { return std::numeric_limits<CType>::max(); }
  static constexpr CType anti_max() { return std::numeric_limits<CType>::min(); }
};

template <>
struct AntiExtrema<bool> {
  static constexpr bool anti_min() { return true; }
  static constexpr bool anti_max() { return false; }
};

template <>
struct AntiExtrema<float> {
  static constexpr float anti_min() { return std::numeric_limits<float>::infinity(); }
  static constexpr float anti_max() { return -std::numeric_limits<float>::infinity(); }
};

template <>
struct AntiExtrema<double> {
  static constexpr double anti_min() { return std::numeric_limits<double>::infinity(); }
  static constexpr double anti_max() { return -std::numeric_limits<double>::infinity(); }
};

template <>
struct AntiExtrema<Decimal32> {
  static constexpr Decimal32 anti_min() { return BasicDecimal32::GetMaxSentinel(); }
  static constexpr Decimal32 anti_max() { return BasicDecimal32::GetMinSentinel(); }
};

template <>
struct AntiExtrema<Decimal64> {
  static constexpr Decimal64 anti_min() { return BasicDecimal64::GetMaxSentinel(); }
  static constexpr Decimal64 anti_max() { return BasicDecimal64::GetMinSentinel(); }
};

template <>
struct AntiExtrema<Decimal128> {
  static constexpr Decimal128 anti_min() { return BasicDecimal128::GetMaxSentinel(); }
  static constexpr Decimal128 anti_max() { return BasicDecimal128::GetMinSentinel(); }
};

template <>
struct AntiExtrema<Decimal256> {
  static constexpr Decimal256 anti_min() { return BasicDecimal256::GetMaxSentinel(); }
  static constexpr Decimal256 anti_max() { return BasicDecimal256::GetMinSentinel(); }
};

template <typename Type, typename Enable = void>
struct GroupedMinMaxImpl final : public GroupedAggregator {
  using CType = typename TypeTraits<Type>::CType;
  using GetSet = GroupedValueTraits<Type>;
  using ArrType =
      typename std::conditional<is_boolean_type<Type>::value, uint8_t, CType>::type;

  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    options_ = *checked_cast<const ScalarAggregateOptions*>(args.options);
    // type_ initialized by MinMaxInit
    mins_ = TypedBufferBuilder<CType>(ctx->memory_pool());
    maxes_ = TypedBufferBuilder<CType>(ctx->memory_pool());
    has_values_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    has_nulls_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    RETURN_NOT_OK(mins_.Append(added_groups, AntiExtrema<CType>::anti_min()));
    RETURN_NOT_OK(maxes_.Append(added_groups, AntiExtrema<CType>::anti_max()));
    RETURN_NOT_OK(has_values_.Append(added_groups, false));
    RETURN_NOT_OK(has_nulls_.Append(added_groups, false));
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    auto raw_mins = mins_.mutable_data();
    auto raw_maxes = maxes_.mutable_data();

    VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, CType val) {
          GetSet::Set(raw_mins, g, std::min(GetSet::Get(raw_mins, g), val));
          GetSet::Set(raw_maxes, g, std::max(GetSet::Get(raw_maxes, g), val));
          bit_util::SetBit(has_values_.mutable_data(), g);
        },
        [&](uint32_t g) { bit_util::SetBit(has_nulls_.mutable_data(), g); });
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedMinMaxImpl*>(&raw_other);

    auto raw_mins = mins_.mutable_data();
    auto raw_maxes = maxes_.mutable_data();

    auto other_raw_mins = other->mins_.mutable_data();
    auto other_raw_maxes = other->maxes_.mutable_data();

    auto g = group_id_mapping.GetValues<uint32_t>(1);
    for (uint32_t other_g = 0; static_cast<int64_t>(other_g) < group_id_mapping.length;
         ++other_g, ++g) {
      GetSet::Set(
          raw_mins, *g,
          std::min(GetSet::Get(raw_mins, *g), GetSet::Get(other_raw_mins, other_g)));
      GetSet::Set(
          raw_maxes, *g,
          std::max(GetSet::Get(raw_maxes, *g), GetSet::Get(other_raw_maxes, other_g)));

      if (bit_util::GetBit(other->has_values_.data(), other_g)) {
        bit_util::SetBit(has_values_.mutable_data(), *g);
      }
      if (bit_util::GetBit(other->has_nulls_.data(), other_g)) {
        bit_util::SetBit(has_nulls_.mutable_data(), *g);
      }
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    // aggregation for group is valid if there was at least one value in that group
    ARROW_ASSIGN_OR_RAISE(auto null_bitmap, has_values_.Finish());

    if (!options_.skip_nulls) {
      // ... and there were no nulls in that group
      ARROW_ASSIGN_OR_RAISE(auto has_nulls, has_nulls_.Finish());
      arrow::internal::BitmapAndNot(null_bitmap->data(), 0, has_nulls->data(), 0,
                                    num_groups_, 0, null_bitmap->mutable_data());
    }

    auto mins = ArrayData::Make(type_, num_groups_, {null_bitmap, nullptr});
    auto maxes = ArrayData::Make(type_, num_groups_, {std::move(null_bitmap), nullptr});
    ARROW_ASSIGN_OR_RAISE(mins->buffers[1], mins_.Finish());
    ARROW_ASSIGN_OR_RAISE(maxes->buffers[1], maxes_.Finish());

    return ArrayData::Make(out_type(), num_groups_, {nullptr},
                           {std::move(mins), std::move(maxes)});
  }

  std::shared_ptr<DataType> out_type() const override {
    return struct_({field("min", type_), field("max", type_)});
  }

  int64_t num_groups_;
  TypedBufferBuilder<CType> mins_, maxes_;
  TypedBufferBuilder<bool> has_values_, has_nulls_;
  std::shared_ptr<DataType> type_;
  ScalarAggregateOptions options_;
};

// For binary-like types
// In principle, FixedSizeBinary could use base implementation
template <typename Type>
struct GroupedMinMaxImpl<Type,
                         enable_if_t<is_base_binary_type<Type>::value ||
                                     std::is_same<Type, FixedSizeBinaryType>::value>>
    final : public GroupedAggregator {
  using Allocator = arrow::stl::allocator<char>;
  using StringType = std::basic_string<char, std::char_traits<char>, Allocator>;

  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    ctx_ = ctx;
    allocator_ = Allocator(ctx->memory_pool());
    options_ = *checked_cast<const ScalarAggregateOptions*>(args.options);
    // type_ initialized by MinMaxInit
    has_values_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    has_nulls_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    DCHECK_GE(added_groups, 0);
    num_groups_ = new_num_groups;
    mins_.resize(new_num_groups);
    maxes_.resize(new_num_groups);
    RETURN_NOT_OK(has_values_.Append(added_groups, false));
    RETURN_NOT_OK(has_nulls_.Append(added_groups, false));
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    return VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, std::string_view val) {
          if (!mins_[g] || val < *mins_[g]) {
            mins_[g].emplace(val.data(), val.size(), allocator_);
          }
          if (!maxes_[g] || val > *maxes_[g]) {
            maxes_[g].emplace(val.data(), val.size(), allocator_);
          }
          bit_util::SetBit(has_values_.mutable_data(), g);
          return Status::OK();
        },
        [&](uint32_t g) {
          bit_util::SetBit(has_nulls_.mutable_data(), g);
          return Status::OK();
        });
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedMinMaxImpl*>(&raw_other);
    auto g = group_id_mapping.GetValues<uint32_t>(1);
    for (uint32_t other_g = 0; static_cast<int64_t>(other_g) < group_id_mapping.length;
         ++other_g, ++g) {
      if (!mins_[*g] ||
          (mins_[*g] && other->mins_[other_g] && *mins_[*g] > *other->mins_[other_g])) {
        mins_[*g] = std::move(other->mins_[other_g]);
      }
      if (!maxes_[*g] || (maxes_[*g] && other->maxes_[other_g] &&
                          *maxes_[*g] < *other->maxes_[other_g])) {
        maxes_[*g] = std::move(other->maxes_[other_g]);
      }

      if (bit_util::GetBit(other->has_values_.data(), other_g)) {
        bit_util::SetBit(has_values_.mutable_data(), *g);
      }
      if (bit_util::GetBit(other->has_nulls_.data(), other_g)) {
        bit_util::SetBit(has_nulls_.mutable_data(), *g);
      }
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    // aggregation for group is valid if there was at least one value in that group
    ARROW_ASSIGN_OR_RAISE(auto null_bitmap, has_values_.Finish());

    if (!options_.skip_nulls) {
      // ... and there were no nulls in that group
      ARROW_ASSIGN_OR_RAISE(auto has_nulls, has_nulls_.Finish());
      arrow::internal::BitmapAndNot(null_bitmap->data(), 0, has_nulls->data(), 0,
                                    num_groups_, 0, null_bitmap->mutable_data());
    }

    auto mins = ArrayData::Make(type_, num_groups_, {null_bitmap, nullptr});
    auto maxes = ArrayData::Make(type_, num_groups_, {std::move(null_bitmap), nullptr});
    RETURN_NOT_OK(MakeOffsetsValues(mins.get(), mins_));
    RETURN_NOT_OK(MakeOffsetsValues(maxes.get(), maxes_));
    return ArrayData::Make(out_type(), num_groups_, {nullptr},
                           {std::move(mins), std::move(maxes)});
  }

  template <typename T = Type>
  enable_if_base_binary<T, Status> MakeOffsetsValues(
      ArrayData* array, const std::vector<std::optional<StringType>>& values) {
    using offset_type = typename T::offset_type;
    ARROW_ASSIGN_OR_RAISE(
        auto raw_offsets,
        AllocateBuffer((1 + values.size()) * sizeof(offset_type), ctx_->memory_pool()));
    auto* offsets = raw_offsets->mutable_data_as<offset_type>();
    offsets[0] = 0;
    offsets++;
    const uint8_t* null_bitmap = array->buffers[0]->data();
    offset_type total_length = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        if (value->size() >
                static_cast<size_t>(std::numeric_limits<offset_type>::max()) ||
            arrow::internal::AddWithOverflow(
                total_length, static_cast<offset_type>(value->size()), &total_length)) {
          return Status::Invalid("Result is too large to fit in ", *array->type,
                                 " cast to large_ variant of type");
        }
      }
      offsets[i] = total_length;
    }
    ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(total_length, ctx_->memory_pool()));
    int64_t offset = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        std::memcpy(data->mutable_data() + offset, value->data(), value->size());
        offset += value->size();
      }
    }
    array->buffers[1] = std::move(raw_offsets);
    array->buffers.push_back(std::move(data));
    return Status::OK();
  }

  template <typename T = Type>
  enable_if_same<T, FixedSizeBinaryType, Status> MakeOffsetsValues(
      ArrayData* array, const std::vector<std::optional<StringType>>& values) {
    const uint8_t* null_bitmap = array->buffers[0]->data();
    const int32_t slot_width =
        checked_cast<const FixedSizeBinaryType&>(*array->type).byte_width();
    int64_t total_length = values.size() * slot_width;
    ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(total_length, ctx_->memory_pool()));
    int64_t offset = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        std::memcpy(data->mutable_data() + offset, value->data(), slot_width);
      } else {
        std::memset(data->mutable_data() + offset, 0x00, slot_width);
      }
      offset += slot_width;
    }
    array->buffers[1] = std::move(data);
    return Status::OK();
  }

  std::shared_ptr<DataType> out_type() const override {
    return struct_({field("min", type_), field("max", type_)});
  }

  ExecContext* ctx_;
  Allocator allocator_;
  int64_t num_groups_;
  std::vector<std::optional<StringType>> mins_, maxes_;
  TypedBufferBuilder<bool> has_values_, has_nulls_;
  std::shared_ptr<DataType> type_;
  ScalarAggregateOptions options_;
};

struct GroupedNullMinMaxImpl final : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs&) override { return Status::OK(); }

  Status Resize(int64_t new_num_groups) override {
    num_groups_ = new_num_groups;
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override { return Status::OK(); }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    return ArrayData::Make(
        out_type(), num_groups_, {nullptr},
        {
            ArrayData::Make(null(), num_groups_, {nullptr}, num_groups_),
            ArrayData::Make(null(), num_groups_, {nullptr}, num_groups_),
        });
  }

  std::shared_ptr<DataType> out_type() const override {
    return struct_({field("min", null()), field("max", null())});
  }

  int64_t num_groups_;
};

template <typename T>
Result<std::unique_ptr<KernelState>> MinMaxInit(KernelContext* ctx,
                                                const KernelInitArgs& args) {
  ARROW_ASSIGN_OR_RAISE(auto impl, HashAggregateInit<GroupedMinMaxImpl<T>>(ctx, args));
  static_cast<GroupedMinMaxImpl<T>*>(impl.get())->type_ = args.inputs[0].GetSharedPtr();
  return impl;
}

template <MinOrMax min_or_max>
HashAggregateKernel MakeMinOrMaxKernel(HashAggregateFunction* min_max_func) {
  HashAggregateKernel kernel;
  kernel.init = [min_max_func](
                    KernelContext* ctx,
                    const KernelInitArgs& args) -> Result<std::unique_ptr<KernelState>> {
    std::vector<TypeHolder> inputs = args.inputs;
    ARROW_ASSIGN_OR_RAISE(auto kernel, min_max_func->DispatchExact(args.inputs));
    KernelInitArgs new_args{kernel, inputs, args.options};
    return kernel->init(ctx, new_args);
  };
  kernel.signature =
      KernelSignature::Make({InputType::Any(), Type::UINT32}, OutputType(FirstType));
  kernel.resize = HashAggregateResize;
  kernel.consume = HashAggregateConsume;
  kernel.merge = HashAggregateMerge;
  kernel.finalize = [](KernelContext* ctx, Datum* out) {
    ARROW_ASSIGN_OR_RAISE(Datum temp,
                          checked_cast<GroupedAggregator*>(ctx->state())->Finalize());
    *out = temp.array_as<StructArray>()->field(static_cast<uint8_t>(min_or_max));
    return Status::OK();
  };
  return kernel;
}

struct GroupedMinMaxFactory {
  template <typename T>
  enable_if_physical_integer<T, Status> Visit(const T&) {
    using PhysicalType = typename T::PhysicalType;
    kernel = MakeKernel(std::move(argument_type), MinMaxInit<PhysicalType>);
    return Status::OK();
  }

  // MSVC2015 apparently doesn't compile this properly if we use
  // enable_if_floating_point
  Status Visit(const FloatType&) {
    kernel = MakeKernel(std::move(argument_type), MinMaxInit<FloatType>);
    return Status::OK();
  }

  Status Visit(const DoubleType&) {
    kernel = MakeKernel(std::move(argument_type), MinMaxInit<DoubleType>);
    return Status::OK();
  }

  template <typename T>
  enable_if_decimal<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), MinMaxInit<T>);
    return Status::OK();
  }

  template <typename T>
  enable_if_base_binary<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), MinMaxInit<T>);
    return Status::OK();
  }

  Status Visit(const FixedSizeBinaryType&) {
    kernel = MakeKernel(std::move(argument_type), MinMaxInit<FixedSizeBinaryType>);
    return Status::OK();
  }

  Status Visit(const BooleanType&) {
    kernel = MakeKernel(std::move(argument_type), MinMaxInit<BooleanType>);
    return Status::OK();
  }

  Status Visit(const NullType&) {
    kernel =
        MakeKernel(std::move(argument_type), HashAggregateInit<GroupedNullMinMaxImpl>);
    return Status::OK();
  }

  Status Visit(const HalfFloatType& type) {
    return Status::NotImplemented("Computing min/max of data of type ", type);
  }

  Status Visit(const DataType& type) {
    return Status::NotImplemented("Computing min/max of data of type ", type);
  }

  static Result<HashAggregateKernel> Make(const std::shared_ptr<DataType>& type) {
    GroupedMinMaxFactory factory;
    factory.argument_type = type->id();
    RETURN_NOT_OK(VisitTypeInline(*type, &factory));
    return std::move(factory.kernel);
  }

  HashAggregateKernel kernel;
  InputType argument_type;
};

// ----------------------------------------------------------------------
// FirstLast implementation

template <typename Type, typename Enable = void>
struct GroupedFirstLastImpl final : public GroupedAggregator {
  using CType = typename TypeTraits<Type>::CType;
  using GetSet = GroupedValueTraits<Type>;
  using ArrType =
      typename std::conditional<is_boolean_type<Type>::value, uint8_t, CType>::type;

  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    options_ = *checked_cast<const ScalarAggregateOptions*>(args.options);

    // First and last non-null values
    firsts_ = TypedBufferBuilder<CType>(ctx->memory_pool());
    lasts_ = TypedBufferBuilder<CType>(ctx->memory_pool());

    // Whether the first/last element is null
    first_is_nulls_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    last_is_nulls_ = TypedBufferBuilder<bool>(ctx->memory_pool());

    has_values_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    has_any_values_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    // Reusing AntiExtrema as uninitialized value here because it doesn't
    // matter what the value is. We never output the uninitialized
    // first/last value.
    RETURN_NOT_OK(firsts_.Append(added_groups, AntiExtrema<CType>::anti_min()));
    RETURN_NOT_OK(lasts_.Append(added_groups, AntiExtrema<CType>::anti_max()));
    RETURN_NOT_OK(has_values_.Append(added_groups, false));
    RETURN_NOT_OK(first_is_nulls_.Append(added_groups, false));
    RETURN_NOT_OK(last_is_nulls_.Append(added_groups, false));
    RETURN_NOT_OK(has_any_values_.Append(added_groups, false));
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    auto raw_firsts = firsts_.mutable_data();
    auto raw_lasts = lasts_.mutable_data();
    auto raw_has_values = has_values_.mutable_data();
    auto raw_has_any_values = has_any_values_.mutable_data();
    auto raw_first_is_nulls = first_is_nulls_.mutable_data();
    auto raw_last_is_nulls = last_is_nulls_.mutable_data();

    VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, CType val) {
          if (!bit_util::GetBit(raw_has_values, g)) {
            GetSet::Set(raw_firsts, g, val);
            bit_util::SetBit(raw_has_values, g);
            bit_util::SetBit(raw_has_any_values, g);
          }
          // No not need to set first_is_nulls because
          // Once first_is_nulls is set to true it never
          // changes
          bit_util::SetBitTo(raw_last_is_nulls, g, false);
          GetSet::Set(raw_lasts, g, val);
          DCHECK(bit_util::GetBit(raw_has_values, g));
        },
        [&](uint32_t g) {
          // We update first_is_null to true if this is called
          // before we see any non-null values
          if (!bit_util::GetBit(raw_has_values, g)) {
            bit_util::SetBit(raw_first_is_nulls, g);
            bit_util::SetBit(raw_has_any_values, g);
          }
          bit_util::SetBit(raw_last_is_nulls, g);
        });
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    // The merge is asymmetric. "first" from this state gets pick over "first" from other
    // state. "last" from other state gets pick over from this state. This is so that when
    // using with segmented aggregation, we still get the correct "first" and "last"
    // value for the entire segment.
    auto other = checked_cast<GroupedFirstLastImpl*>(&raw_other);

    auto raw_firsts = firsts_.mutable_data();
    auto raw_lasts = lasts_.mutable_data();
    auto raw_has_values = has_values_.mutable_data();
    auto raw_has_any_values = has_any_values_.mutable_data();
    auto raw_first_is_nulls = first_is_nulls_.mutable_data();
    auto raw_last_is_nulls = last_is_nulls_.mutable_data();

    auto other_raw_firsts = other->firsts_.mutable_data();
    auto other_raw_lasts = other->lasts_.mutable_data();
    auto other_raw_has_values = other->has_values_.mutable_data();
    auto other_raw_has_any_values = other->has_values_.mutable_data();
    auto other_raw_last_is_nulls = other->last_is_nulls_.mutable_data();

    auto g = group_id_mapping.GetValues<uint32_t>(1);

    for (uint32_t other_g = 0; static_cast<int64_t>(other_g) < group_id_mapping.length;
         ++other_g, ++g) {
      if (!bit_util::GetBit(raw_has_values, *g)) {
        if (bit_util::GetBit(other_raw_has_values, other_g)) {
          GetSet::Set(raw_firsts, *g, GetSet::Get(other_raw_firsts, other_g));
        }
      }
      if (bit_util::GetBit(other_raw_has_values, other_g)) {
        GetSet::Set(raw_lasts, *g, GetSet::Get(other_raw_lasts, other_g));
      }
      // If the current state doesn't have any nulls (null or non-null), then
      // We take the "first_is_null" from rhs
      if (!bit_util::GetBit(raw_has_any_values, *g)) {
        bit_util::SetBitTo(raw_first_is_nulls, *g,
                           bit_util::GetBit(other->first_is_nulls_.data(), other_g));
      }
      if (bit_util::GetBit(other_raw_last_is_nulls, other_g)) {
        bit_util::SetBit(raw_last_is_nulls, *g);
      }

      if (bit_util::GetBit(other_raw_has_values, other_g)) {
        bit_util::SetBit(raw_has_values, *g);
      }

      if (bit_util::GetBit(other_raw_has_any_values, other_g)) {
        bit_util::SetBit(raw_has_any_values, *g);
      }
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    // We initialize the null bitmap with first_is_nulls and last_is_nulls
    // then update it depending on has_values
    ARROW_ASSIGN_OR_RAISE(auto first_null_bitmap, first_is_nulls_.Finish());
    ARROW_ASSIGN_OR_RAISE(auto last_null_bitmap, last_is_nulls_.Finish());
    ARROW_ASSIGN_OR_RAISE(auto has_values, has_values_.Finish());

    auto raw_first_null_bitmap = first_null_bitmap->mutable_data();
    auto raw_last_null_bitmap = last_null_bitmap->mutable_data();
    auto raw_has_values = has_values->data();

    if (options_.skip_nulls) {
      for (int i = 0; i < num_groups_; i++) {
        const bool has_value = bit_util::GetBit(has_values->data(), i);
        bit_util::SetBitTo(raw_first_null_bitmap, i, has_value);
        bit_util::SetBitTo(raw_last_null_bitmap, i, has_value);
      }
    } else {
      for (int i = 0; i < num_groups_; i++) {
        // If first is null, we set the mask to false to output null
        if (bit_util::GetBit(raw_first_null_bitmap, i)) {
          bit_util::SetBitTo(raw_first_null_bitmap, i, false);
        } else {
          bit_util::SetBitTo(raw_first_null_bitmap, i,
                             bit_util::GetBit(raw_has_values, i));
        }
      }
      for (int i = 0; i < num_groups_; i++) {
        // If last is null, we set the mask to false to output null
        if (bit_util::GetBit(raw_last_null_bitmap, i)) {
          bit_util::SetBitTo(raw_last_null_bitmap, i, false);
        } else {
          bit_util::SetBitTo(raw_last_null_bitmap, i,
                             bit_util::GetBit(raw_has_values, i));
        }
      }
    }

    auto firsts =
        ArrayData::Make(type_, num_groups_, {std::move(first_null_bitmap), nullptr});
    auto lasts =
        ArrayData::Make(type_, num_groups_, {std::move(last_null_bitmap), nullptr});
    ARROW_ASSIGN_OR_RAISE(firsts->buffers[1], firsts_.Finish());
    ARROW_ASSIGN_OR_RAISE(lasts->buffers[1], lasts_.Finish());

    return ArrayData::Make(out_type(), num_groups_, {nullptr},
                           {std::move(firsts), std::move(lasts)});
  }

  std::shared_ptr<DataType> out_type() const override {
    return struct_({field("first", type_), field("last", type_)});
  }

  int64_t num_groups_;
  TypedBufferBuilder<CType> firsts_, lasts_;
  // has_values is true if there is non-null values
  // has_any_values is true if there is either null or non-null values
  TypedBufferBuilder<bool> has_values_, has_any_values_, first_is_nulls_, last_is_nulls_;
  std::shared_ptr<DataType> type_;
  ScalarAggregateOptions options_;
};

template <typename Type>
struct GroupedFirstLastImpl<Type,
                            enable_if_t<is_base_binary_type<Type>::value ||
                                        std::is_same<Type, FixedSizeBinaryType>::value>>
    final : public GroupedAggregator {
  using Allocator = arrow::stl::allocator<char>;
  using StringType = std::basic_string<char, std::char_traits<char>, Allocator>;

  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    ctx_ = ctx;
    allocator_ = Allocator(ctx->memory_pool());
    options_ = *checked_cast<const ScalarAggregateOptions*>(args.options);
    // type_ initialized by FirstLastInit
    // Whether the first/last element is null
    first_is_nulls_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    last_is_nulls_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    has_values_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    has_any_values_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    DCHECK_GE(added_groups, 0);
    num_groups_ = new_num_groups;
    firsts_.resize(new_num_groups);
    lasts_.resize(new_num_groups);
    RETURN_NOT_OK(has_values_.Append(added_groups, false));
    RETURN_NOT_OK(has_any_values_.Append(added_groups, false));
    RETURN_NOT_OK(first_is_nulls_.Append(added_groups, false));
    RETURN_NOT_OK(last_is_nulls_.Append(added_groups, false));
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    auto raw_has_values = has_values_.mutable_data();
    auto raw_has_any_values = has_any_values_.mutable_data();
    auto raw_first_is_nulls = first_is_nulls_.mutable_data();
    auto raw_last_is_nulls = last_is_nulls_.mutable_data();

    return VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, std::string_view val) {
          if (!firsts_[g]) {
            firsts_[g].emplace(val.data(), val.size(), allocator_);
            bit_util::SetBit(raw_has_values, g);
            bit_util::SetBit(raw_has_any_values, g);
          }
          bit_util::SetBitTo(raw_last_is_nulls, g, false);
          lasts_[g].emplace(val.data(), val.size(), allocator_);
          return Status::OK();
        },
        [&](uint32_t g) {
          if (!bit_util::GetBit(raw_has_values, g)) {
            bit_util::SetBit(raw_first_is_nulls, g);
            bit_util::SetBit(raw_has_any_values, g);
          }
          bit_util::SetBit(raw_last_is_nulls, g);
          return Status::OK();
        });
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedFirstLastImpl*>(&raw_other);
    auto g = group_id_mapping.GetValues<uint32_t>(1);
    for (uint32_t other_g = 0; static_cast<int64_t>(other_g) < group_id_mapping.length;
         ++other_g, ++g) {
      if (!firsts_[*g]) {
        firsts_[*g] = std::move(other->firsts_[other_g]);
      }
      lasts_[*g] = std::move(other->lasts_[other_g]);

      if (!bit_util::GetBit(has_any_values_.data(), *g)) {
        bit_util::SetBitTo(first_is_nulls_.mutable_data(), *g,
                           bit_util::GetBit(other->first_is_nulls_.data(), other_g));
      }
      if (bit_util::GetBit(other->last_is_nulls_.data(), other_g)) {
        bit_util::SetBit(last_is_nulls_.mutable_data(), *g);
      }
      if (bit_util::GetBit(other->has_values_.data(), other_g)) {
        bit_util::SetBit(has_values_.mutable_data(), *g);
      }
      if (bit_util::GetBit(other->has_any_values_.data(), other_g)) {
        bit_util::SetBit(has_any_values_.mutable_data(), *g);
      }
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto first_null_bitmap, first_is_nulls_.Finish());
    ARROW_ASSIGN_OR_RAISE(auto last_null_bitmap, last_is_nulls_.Finish());
    ARROW_ASSIGN_OR_RAISE(auto has_values, has_values_.Finish());

    if (!options_.skip_nulls) {
      for (int i = 0; i < num_groups_; i++) {
        const bool first_is_null = bit_util::GetBit(first_null_bitmap->data(), i);
        const bool has_value = bit_util::GetBit(has_values->data(), i);
        if (first_is_null) {
          bit_util::SetBitTo(first_null_bitmap->mutable_data(), i, false);
        } else {
          bit_util::SetBitTo(first_null_bitmap->mutable_data(), i, has_value);
        }
      }

      for (int i = 0; i < num_groups_; i++) {
        const bool last_is_null = bit_util::GetBit(last_null_bitmap->data(), i);
        const bool has_value = bit_util::GetBit(has_values->data(), i);
        if (last_is_null) {
          bit_util::SetBitTo(last_null_bitmap->mutable_data(), i, false);
        } else {
          bit_util::SetBitTo(last_null_bitmap->mutable_data(), i, has_value);
        }
      }
    } else {
      for (int i = 0; i < num_groups_; i++) {
        const bool has_value = bit_util::GetBit(has_values->data(), i);
        bit_util::SetBitTo(first_null_bitmap->mutable_data(), i, has_value);
        bit_util::SetBitTo(last_null_bitmap->mutable_data(), i, has_value);
      }
    }

    auto firsts =
        ArrayData::Make(type_, num_groups_, {std::move(first_null_bitmap), nullptr});
    auto lasts =
        ArrayData::Make(type_, num_groups_, {std::move(last_null_bitmap), nullptr});
    RETURN_NOT_OK(MakeOffsetsValues(firsts.get(), firsts_));
    RETURN_NOT_OK(MakeOffsetsValues(lasts.get(), lasts_));
    return ArrayData::Make(out_type(), num_groups_, {nullptr},
                           {std::move(firsts), std::move(lasts)});
  }

  template <typename T = Type>
  enable_if_base_binary<T, Status> MakeOffsetsValues(
      ArrayData* array, const std::vector<std::optional<StringType>>& values) {
    using offset_type = typename T::offset_type;
    ARROW_ASSIGN_OR_RAISE(
        auto raw_offsets,
        AllocateBuffer((1 + values.size()) * sizeof(offset_type), ctx_->memory_pool()));
    auto* offsets = raw_offsets->mutable_data_as<offset_type>();
    offsets[0] = 0;
    offsets++;
    const uint8_t* null_bitmap = array->buffers[0]->data();
    offset_type total_length = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        if (value->size() >
                static_cast<size_t>(std::numeric_limits<offset_type>::max()) ||
            arrow::internal::AddWithOverflow(
                total_length, static_cast<offset_type>(value->size()), &total_length)) {
          return Status::Invalid("Result is too large to fit in ", *array->type,
                                 " cast to large_ variant of type");
        }
      }
      offsets[i] = total_length;
    }
    ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(total_length, ctx_->memory_pool()));
    int64_t offset = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        std::memcpy(data->mutable_data() + offset, value->data(), value->size());
        offset += value->size();
      }
    }
    array->buffers[1] = std::move(raw_offsets);
    array->buffers.push_back(std::move(data));
    return Status::OK();
  }

  template <typename T = Type>
  enable_if_same<T, FixedSizeBinaryType, Status> MakeOffsetsValues(
      ArrayData* array, const std::vector<std::optional<StringType>>& values) {
    const uint8_t* null_bitmap = array->buffers[0]->data();
    const int32_t slot_width =
        checked_cast<const FixedSizeBinaryType&>(*array->type).byte_width();
    int64_t total_length = values.size() * slot_width;
    ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(total_length, ctx_->memory_pool()));
    int64_t offset = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        std::memcpy(data->mutable_data() + offset, value->data(), slot_width);
      } else {
        std::memset(data->mutable_data() + offset, 0x00, slot_width);
      }
      offset += slot_width;
    }
    array->buffers[1] = std::move(data);
    return Status::OK();
  }

  std::shared_ptr<DataType> out_type() const override {
    return struct_({field("first", type_), field("last", type_)});
  }

  ExecContext* ctx_;
  Allocator allocator_;
  int64_t num_groups_;
  std::vector<std::optional<StringType>> firsts_, lasts_;
  TypedBufferBuilder<bool> has_values_, has_any_values_, first_is_nulls_, last_is_nulls_;
  std::shared_ptr<DataType> type_;
  ScalarAggregateOptions options_;
};

template <typename T>
Result<std::unique_ptr<KernelState>> FirstLastInit(KernelContext* ctx,
                                                   const KernelInitArgs& args) {
  ARROW_ASSIGN_OR_RAISE(auto impl, HashAggregateInit<GroupedFirstLastImpl<T>>(ctx, args));
  static_cast<GroupedFirstLastImpl<T>*>(impl.get())->type_ =
      args.inputs[0].GetSharedPtr();
  return impl;
}

template <FirstOrLast first_or_last>
HashAggregateKernel MakeFirstOrLastKernel(HashAggregateFunction* first_last_func) {
  HashAggregateKernel kernel;
  kernel.init = [first_last_func](
                    KernelContext* ctx,
                    const KernelInitArgs& args) -> Result<std::unique_ptr<KernelState>> {
    std::vector<TypeHolder> inputs = args.inputs;
    ARROW_ASSIGN_OR_RAISE(auto kernel, first_last_func->DispatchExact(args.inputs));
    KernelInitArgs new_args{kernel, inputs, args.options};
    return kernel->init(ctx, new_args);
  };

  kernel.signature =
      KernelSignature::Make({InputType::Any(), Type::UINT32}, OutputType(FirstType));
  kernel.resize = HashAggregateResize;
  kernel.consume = HashAggregateConsume;
  kernel.merge = HashAggregateMerge;
  kernel.finalize = [](KernelContext* ctx, Datum* out) {
    ARROW_ASSIGN_OR_RAISE(Datum temp,
                          checked_cast<GroupedAggregator*>(ctx->state())->Finalize());
    *out = temp.array_as<StructArray>()->field(static_cast<uint8_t>(first_or_last));
    return Status::OK();
  };
  kernel.ordered = true;
  return kernel;
}

struct GroupedFirstLastFactory {
  template <typename T>
  enable_if_physical_integer<T, Status> Visit(const T&) {
    using PhysicalType = typename T::PhysicalType;
    kernel = MakeKernel(std::move(argument_type), FirstLastInit<PhysicalType>,
                        /*ordered*/ true);
    return Status::OK();
  }

  Status Visit(const FloatType&) {
    kernel =
        MakeKernel(std::move(argument_type), FirstLastInit<FloatType>, /*ordered*/ true);
    return Status::OK();
  }

  Status Visit(const DoubleType&) {
    kernel =
        MakeKernel(std::move(argument_type), FirstLastInit<DoubleType>, /*ordered*/ true);
    return Status::OK();
  }

  template <typename T>
  enable_if_base_binary<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), FirstLastInit<T>);
    return Status::OK();
  }

  Status Visit(const FixedSizeBinaryType&) {
    kernel = MakeKernel(std::move(argument_type), FirstLastInit<FixedSizeBinaryType>);
    return Status::OK();
  }

  Status Visit(const BooleanType&) {
    kernel = MakeKernel(std::move(argument_type), FirstLastInit<BooleanType>);
    return Status::OK();
  }

  Status Visit(const HalfFloatType& type) {
    return Status::NotImplemented("Computing first/last of data of type ", type);
  }

  Status Visit(const DataType& type) {
    return Status::NotImplemented("Computing first/last of data of type ", type);
  }

  static Result<HashAggregateKernel> Make(const std::shared_ptr<DataType>& type) {
    GroupedFirstLastFactory factory;
    factory.argument_type = type->id();
    RETURN_NOT_OK(VisitTypeInline(*type, &factory));
    return factory.kernel;
  }

  HashAggregateKernel kernel;
  InputType argument_type;
};

// ----------------------------------------------------------------------
// Any/All implementation

template <typename Impl>
struct GroupedBooleanAggregator : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    options_ = checked_cast<const ScalarAggregateOptions&>(*args.options);
    pool_ = ctx->memory_pool();
    reduced_ = TypedBufferBuilder<bool>(pool_);
    no_nulls_ = TypedBufferBuilder<bool>(pool_);
    counts_ = TypedBufferBuilder<int64_t>(pool_);
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    RETURN_NOT_OK(reduced_.Append(added_groups, Impl::NullValue()));
    RETURN_NOT_OK(no_nulls_.Append(added_groups, true));
    return counts_.Append(added_groups, 0);
  }

  Status Consume(const ExecSpan& batch) override {
    uint8_t* reduced = reduced_.mutable_data();
    uint8_t* no_nulls = no_nulls_.mutable_data();
    int64_t* counts = counts_.mutable_data();
    auto g = batch[1].array.GetValues<uint32_t>(1);

    if (batch[0].is_array()) {
      const ArraySpan& input = batch[0].array;
      const uint8_t* bitmap = input.buffers[1].data;
      if (input.MayHaveNulls()) {
        arrow::internal::VisitBitBlocksVoid(
            input.buffers[0].data, input.offset, input.length,
            [&](int64_t position) {
              counts[*g]++;
              Impl::UpdateGroupWith(reduced, *g, bit_util::GetBit(bitmap, position));
              g++;
            },
            [&] { bit_util::SetBitTo(no_nulls, *g++, false); });
      } else {
        arrow::internal::VisitBitBlocksVoid(
            bitmap, input.offset, input.length,
            [&](int64_t) {
              Impl::UpdateGroupWith(reduced, *g, true);
              counts[*g++]++;
            },
            [&]() {
              Impl::UpdateGroupWith(reduced, *g, false);
              counts[*g++]++;
            });
      }
    } else {
      const Scalar& input = *batch[0].scalar;
      if (input.is_valid) {
        const bool value = UnboxScalar<BooleanType>::Unbox(input);
        for (int64_t i = 0; i < batch.length; i++) {
          Impl::UpdateGroupWith(reduced, *g, value);
          counts[*g++]++;
        }
      } else {
        for (int64_t i = 0; i < batch.length; i++) {
          bit_util::SetBitTo(no_nulls, *g++, false);
        }
      }
    }
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedBooleanAggregator<Impl>*>(&raw_other);

    uint8_t* reduced = reduced_.mutable_data();
    uint8_t* no_nulls = no_nulls_.mutable_data();
    int64_t* counts = counts_.mutable_data();

    const uint8_t* other_reduced = other->reduced_.mutable_data();
    const uint8_t* other_no_nulls = other->no_nulls_.mutable_data();
    const int64_t* other_counts = other->counts_.mutable_data();

    auto g = group_id_mapping.GetValues<uint32_t>(1);
    for (int64_t other_g = 0; other_g < group_id_mapping.length; ++other_g, ++g) {
      counts[*g] += other_counts[other_g];
      Impl::UpdateGroupWith(reduced, *g, bit_util::GetBit(other_reduced, other_g));
      bit_util::SetBitTo(
          no_nulls, *g,
          bit_util::GetBit(no_nulls, *g) && bit_util::GetBit(other_no_nulls, other_g));
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    std::shared_ptr<Buffer> null_bitmap;
    const int64_t* counts = counts_.data();
    int64_t null_count = 0;

    for (int64_t i = 0; i < num_groups_; ++i) {
      if (counts[i] >= options_.min_count) continue;

      if (null_bitmap == nullptr) {
        ARROW_ASSIGN_OR_RAISE(null_bitmap, AllocateBitmap(num_groups_, pool_));
        bit_util::SetBitsTo(null_bitmap->mutable_data(), 0, num_groups_, true);
      }

      null_count += 1;
      bit_util::SetBitTo(null_bitmap->mutable_data(), i, false);
    }

    ARROW_ASSIGN_OR_RAISE(auto reduced, reduced_.Finish());
    if (!options_.skip_nulls) {
      null_count = kUnknownNullCount;
      ARROW_ASSIGN_OR_RAISE(auto no_nulls, no_nulls_.Finish());
      Impl::AdjustForMinCount(no_nulls->mutable_data(), reduced->data(), num_groups_);
      if (null_bitmap) {
        arrow::internal::BitmapAnd(null_bitmap->data(), /*left_offset=*/0,
                                   no_nulls->data(), /*right_offset=*/0, num_groups_,
                                   /*out_offset=*/0, null_bitmap->mutable_data());
      } else {
        null_bitmap = std::move(no_nulls);
      }
    }

    return ArrayData::Make(out_type(), num_groups_,
                           {std::move(null_bitmap), std::move(reduced)}, null_count);
  }

  std::shared_ptr<DataType> out_type() const override { return boolean(); }

  int64_t num_groups_ = 0;
  ScalarAggregateOptions options_;
  TypedBufferBuilder<bool> reduced_, no_nulls_;
  TypedBufferBuilder<int64_t> counts_;
  MemoryPool* pool_;
};

struct GroupedAnyImpl : public GroupedBooleanAggregator<GroupedAnyImpl> {
  // The default value for a group.
  static bool NullValue() { return false; }

  // Update the value for a group given an observation.
  static void UpdateGroupWith(uint8_t* seen, uint32_t g, bool value) {
    if (!bit_util::GetBit(seen, g) && value) {
      bit_util::SetBit(seen, g);
    }
  }

  // Combine the array of observed nulls with the array of group values.
  static void AdjustForMinCount(uint8_t* no_nulls, const uint8_t* seen,
                                int64_t num_groups) {
    arrow::internal::BitmapOr(no_nulls, /*left_offset=*/0, seen, /*right_offset=*/0,
                              num_groups, /*out_offset=*/0, no_nulls);
  }
};

struct GroupedAllImpl : public GroupedBooleanAggregator<GroupedAllImpl> {
  static bool NullValue() { return true; }

  static void UpdateGroupWith(uint8_t* seen, uint32_t g, bool value) {
    if (!value) {
      bit_util::ClearBit(seen, g);
    }
  }

  static void AdjustForMinCount(uint8_t* no_nulls, const uint8_t* seen,
                                int64_t num_groups) {
    arrow::internal::BitmapOrNot(no_nulls, /*left_offset=*/0, seen, /*right_offset=*/0,
                                 num_groups, /*out_offset=*/0, no_nulls);
  }
};

// ----------------------------------------------------------------------
// CountDistinct/Distinct implementation

struct GroupedCountDistinctImpl : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    ctx_ = ctx;
    pool_ = ctx->memory_pool();
    options_ = checked_cast<const CountOptions&>(*args.options);
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    num_groups_ = new_num_groups;
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    ARROW_ASSIGN_OR_RAISE(std::ignore, grouper_->Consume(batch));
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedCountDistinctImpl*>(&raw_other);

    // Get (value, group_id) pairs, then translate the group IDs and consume them
    // ourselves
    ARROW_ASSIGN_OR_RAISE(ExecBatch uniques, other->grouper_->GetUniques());
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Buffer> remapped_g,
                          AllocateBuffer(uniques.length * sizeof(uint32_t), pool_));

    const auto* g_mapping = group_id_mapping.buffers[1]->data_as<uint32_t>();
    const auto* other_g = uniques[1].array()->buffers[1]->data_as<uint32_t>();
    auto* g = remapped_g->mutable_data_as<uint32_t>();

    for (int64_t i = 0; i < uniques.length; i++) {
      g[i] = g_mapping[other_g[i]];
    }

    ExecSpan uniques_span(uniques);
    uniques_span.values[1].array.SetBuffer(1, remapped_g);
    return Consume(uniques_span);
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<Buffer> values,
                          AllocateBuffer(num_groups_ * sizeof(int64_t), pool_));
    auto* counts = values->mutable_data_as<int64_t>();
    std::fill(counts, counts + num_groups_, 0);

    ARROW_ASSIGN_OR_RAISE(auto uniques, grouper_->GetUniques());
    auto* g = uniques[1].array()->GetValues<uint32_t>(1);
    const auto& items = *uniques[0].array();
    const auto* valid = items.GetValues<uint8_t>(0, 0);
    if (options_.mode == CountOptions::ALL ||
        (options_.mode == CountOptions::ONLY_VALID && !valid)) {
      for (int64_t i = 0; i < uniques.length; i++) {
        counts[g[i]]++;
      }
    } else if (options_.mode == CountOptions::ONLY_VALID) {
      for (int64_t i = 0; i < uniques.length; i++) {
        counts[g[i]] += bit_util::GetBit(valid, items.offset + i);
      }
    } else if (valid) {  // ONLY_NULL
      for (int64_t i = 0; i < uniques.length; i++) {
        counts[g[i]] += !bit_util::GetBit(valid, items.offset + i);
      }
    }

    return ArrayData::Make(int64(), num_groups_, {nullptr, std::move(values)},
                           /*null_count=*/0);
  }

  std::shared_ptr<DataType> out_type() const override { return int64(); }

  ExecContext* ctx_;
  MemoryPool* pool_;
  int64_t num_groups_;
  CountOptions options_;
  std::unique_ptr<Grouper> grouper_;
  std::shared_ptr<DataType> out_type_;
};

struct GroupedDistinctImpl : public GroupedCountDistinctImpl {
  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto uniques, grouper_->GetUniques());
    ARROW_ASSIGN_OR_RAISE(
        auto groupings, Grouper::MakeGroupings(*uniques[1].array_as<UInt32Array>(),
                                               static_cast<uint32_t>(num_groups_), ctx_));
    ARROW_ASSIGN_OR_RAISE(
        auto list, Grouper::ApplyGroupings(*groupings, *uniques[0].make_array(), ctx_));
    const auto& values = list->values();
    DCHECK_EQ(values->offset(), 0);
    auto* offsets = list->value_offsets()->mutable_data_as<int32_t>();
    if (options_.mode == CountOptions::ALL ||
        (options_.mode == CountOptions::ONLY_VALID && values->null_count() == 0)) {
      return list;
    } else if (options_.mode == CountOptions::ONLY_VALID) {
      int32_t prev_offset = offsets[0];
      for (int64_t i = 0; i < list->length(); i++) {
        const int32_t slot_length = offsets[i + 1] - prev_offset;
        const int64_t null_count =
            slot_length - arrow::internal::CountSetBits(values->null_bitmap()->data(),
                                                        prev_offset, slot_length);
        DCHECK_LE(null_count, 1);
        const int32_t offset = null_count > 0 ? slot_length - 1 : slot_length;
        prev_offset = offsets[i + 1];
        offsets[i + 1] = offsets[i] + offset;
      }
      auto filter =
          std::make_shared<BooleanArray>(values->length(), values->null_bitmap());
      ARROW_ASSIGN_OR_RAISE(
          auto new_values,
          Filter(std::move(values), filter, FilterOptions(FilterOptions::DROP), ctx_));
      return std::make_shared<ListArray>(list->type(), list->length(),
                                         list->value_offsets(), new_values.make_array());
    }
    // ONLY_NULL
    if (values->null_count() == 0) {
      std::fill(offsets + 1, offsets + list->length() + 1, offsets[0]);
    } else {
      int32_t prev_offset = offsets[0];
      for (int64_t i = 0; i < list->length(); i++) {
        const int32_t slot_length = offsets[i + 1] - prev_offset;
        const int64_t null_count =
            slot_length - arrow::internal::CountSetBits(values->null_bitmap()->data(),
                                                        prev_offset, slot_length);
        const int32_t offset = null_count > 0 ? 1 : 0;
        prev_offset = offsets[i + 1];
        offsets[i + 1] = offsets[i] + offset;
      }
    }
    ARROW_ASSIGN_OR_RAISE(
        auto new_values,
        MakeArrayOfNull(out_type_,
                        list->length() > 0 ? offsets[list->length()] - offsets[0] : 0,
                        pool_));
    return std::make_shared<ListArray>(list->type(), list->length(),
                                       list->value_offsets(), std::move(new_values));
  }

  std::shared_ptr<DataType> out_type() const override { return list(out_type_); }
};

template <typename Impl>
Result<std::unique_ptr<KernelState>> GroupedDistinctInit(KernelContext* ctx,
                                                         const KernelInitArgs& args) {
  ARROW_ASSIGN_OR_RAISE(auto impl, HashAggregateInit<Impl>(ctx, args));
  auto instance = static_cast<Impl*>(impl.get());
  instance->out_type_ = args.inputs[0].GetSharedPtr();
  ARROW_ASSIGN_OR_RAISE(instance->grouper_,
                        Grouper::Make(args.inputs, ctx->exec_context()));
  return impl;
}

// ----------------------------------------------------------------------
// One implementation

template <typename Type, typename Enable = void>
struct GroupedOneImpl final : public GroupedAggregator {
  using CType = typename TypeTraits<Type>::CType;
  using GetSet = GroupedValueTraits<Type>;

  Status Init(ExecContext* ctx, const KernelInitArgs&) override {
    // out_type_ initialized by GroupedOneInit
    ones_ = TypedBufferBuilder<CType>(ctx->memory_pool());
    has_one_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    RETURN_NOT_OK(ones_.Append(added_groups, static_cast<CType>(0)));
    RETURN_NOT_OK(has_one_.Append(added_groups, false));
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    auto raw_ones_ = ones_.mutable_data();

    return VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, CType val) -> Status {
          if (!bit_util::GetBit(has_one_.data(), g)) {
            GetSet::Set(raw_ones_, g, val);
            bit_util::SetBit(has_one_.mutable_data(), g);
          }
          return Status::OK();
        },
        [&](uint32_t g) -> Status { return Status::OK(); });
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedOneImpl*>(&raw_other);

    auto raw_ones = ones_.mutable_data();
    auto other_raw_ones = other->ones_.mutable_data();

    auto g = group_id_mapping.GetValues<uint32_t>(1);
    for (uint32_t other_g = 0; static_cast<int64_t>(other_g) < group_id_mapping.length;
         ++other_g, ++g) {
      if (!bit_util::GetBit(has_one_.data(), *g)) {
        if (bit_util::GetBit(other->has_one_.data(), other_g)) {
          GetSet::Set(raw_ones, *g, GetSet::Get(other_raw_ones, other_g));
          bit_util::SetBit(has_one_.mutable_data(), *g);
        }
      }
    }

    return Status::OK();
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto null_bitmap, has_one_.Finish());
    ARROW_ASSIGN_OR_RAISE(auto data, ones_.Finish());
    return ArrayData::Make(out_type_, num_groups_,
                           {std::move(null_bitmap), std::move(data)});
  }

  std::shared_ptr<DataType> out_type() const override { return out_type_; }

  int64_t num_groups_;
  TypedBufferBuilder<CType> ones_;
  TypedBufferBuilder<bool> has_one_;
  std::shared_ptr<DataType> out_type_;
};

struct GroupedNullOneImpl : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs&) override { return Status::OK(); }

  Status Resize(int64_t new_num_groups) override {
    num_groups_ = new_num_groups;
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override { return Status::OK(); }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    return ArrayData::Make(null(), num_groups_, {nullptr}, num_groups_);
  }

  std::shared_ptr<DataType> out_type() const override { return null(); }

  int64_t num_groups_;
};

template <typename Type>
struct GroupedOneImpl<Type, enable_if_t<is_base_binary_type<Type>::value ||
                                        std::is_same<Type, FixedSizeBinaryType>::value>>
    final : public GroupedAggregator {
  using Allocator = arrow::stl::allocator<char>;
  using StringType = std::basic_string<char, std::char_traits<char>, Allocator>;

  Status Init(ExecContext* ctx, const KernelInitArgs&) override {
    ctx_ = ctx;
    allocator_ = Allocator(ctx->memory_pool());
    // out_type_ initialized by GroupedOneInit
    has_one_ = TypedBufferBuilder<bool>(ctx->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    DCHECK_GE(added_groups, 0);
    num_groups_ = new_num_groups;
    ones_.resize(new_num_groups);
    RETURN_NOT_OK(has_one_.Append(added_groups, false));
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    return VisitGroupedValues<Type>(
        batch,
        [&](uint32_t g, std::string_view val) -> Status {
          if (!bit_util::GetBit(has_one_.data(), g)) {
            ones_[g].emplace(val.data(), val.size(), allocator_);
            bit_util::SetBit(has_one_.mutable_data(), g);
          }
          return Status::OK();
        },
        [&](uint32_t g) -> Status { return Status::OK(); });
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedOneImpl*>(&raw_other);
    auto g = group_id_mapping.GetValues<uint32_t>(1);
    for (uint32_t other_g = 0; static_cast<int64_t>(other_g) < group_id_mapping.length;
         ++other_g, ++g) {
      if (!bit_util::GetBit(has_one_.data(), *g)) {
        if (bit_util::GetBit(other->has_one_.data(), other_g)) {
          ones_[*g] = std::move(other->ones_[other_g]);
          bit_util::SetBit(has_one_.mutable_data(), *g);
        }
      }
    }
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto null_bitmap, has_one_.Finish());
    auto ones =
        ArrayData::Make(out_type(), num_groups_, {std::move(null_bitmap), nullptr});
    RETURN_NOT_OK(MakeOffsetsValues(ones.get(), ones_));
    return ones;
  }

  template <typename T = Type>
  enable_if_base_binary<T, Status> MakeOffsetsValues(
      ArrayData* array, const std::vector<std::optional<StringType>>& values) {
    using offset_type = typename T::offset_type;
    ARROW_ASSIGN_OR_RAISE(
        auto raw_offsets,
        AllocateBuffer((1 + values.size()) * sizeof(offset_type), ctx_->memory_pool()));
    auto* offsets = raw_offsets->mutable_data_as<offset_type>();
    offsets[0] = 0;
    offsets++;
    const uint8_t* null_bitmap = array->buffers[0]->data();
    offset_type total_length = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        if (value->size() >
                static_cast<size_t>(std::numeric_limits<offset_type>::max()) ||
            arrow::internal::AddWithOverflow(
                total_length, static_cast<offset_type>(value->size()), &total_length)) {
          return Status::Invalid("Result is too large to fit in ", *array->type,
                                 " cast to large_ variant of type");
        }
      }
      offsets[i] = total_length;
    }
    ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(total_length, ctx_->memory_pool()));
    int64_t offset = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        std::memcpy(data->mutable_data() + offset, value->data(), value->size());
        offset += value->size();
      }
    }
    array->buffers[1] = std::move(raw_offsets);
    array->buffers.push_back(std::move(data));
    return Status::OK();
  }

  template <typename T = Type>
  enable_if_same<T, FixedSizeBinaryType, Status> MakeOffsetsValues(
      ArrayData* array, const std::vector<std::optional<StringType>>& values) {
    const uint8_t* null_bitmap = array->buffers[0]->data();
    const int32_t slot_width =
        checked_cast<const FixedSizeBinaryType&>(*array->type).byte_width();
    int64_t total_length = values.size() * slot_width;
    ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(total_length, ctx_->memory_pool()));
    int64_t offset = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        std::memcpy(data->mutable_data() + offset, value->data(), slot_width);
      } else {
        std::memset(data->mutable_data() + offset, 0x00, slot_width);
      }
      offset += slot_width;
    }
    array->buffers[1] = std::move(data);
    return Status::OK();
  }

  std::shared_ptr<DataType> out_type() const override { return out_type_; }

  ExecContext* ctx_;
  Allocator allocator_;
  int64_t num_groups_;
  std::vector<std::optional<StringType>> ones_;
  TypedBufferBuilder<bool> has_one_;
  std::shared_ptr<DataType> out_type_;
};

template <typename T>
Result<std::unique_ptr<KernelState>> GroupedOneInit(KernelContext* ctx,
                                                    const KernelInitArgs& args) {
  ARROW_ASSIGN_OR_RAISE(auto impl, HashAggregateInit<GroupedOneImpl<T>>(ctx, args));
  auto instance = static_cast<GroupedOneImpl<T>*>(impl.get());
  instance->out_type_ = args.inputs[0].GetSharedPtr();
  return impl;
}

struct GroupedOneFactory {
  template <typename T>
  enable_if_physical_integer<T, Status> Visit(const T&) {
    using PhysicalType = typename T::PhysicalType;
    kernel = MakeKernel(std::move(argument_type), GroupedOneInit<PhysicalType>);
    return Status::OK();
  }

  template <typename T>
  enable_if_floating_point<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), GroupedOneInit<T>);
    return Status::OK();
  }

  template <typename T>
  enable_if_decimal<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), GroupedOneInit<T>);
    return Status::OK();
  }

  template <typename T>
  enable_if_base_binary<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), GroupedOneInit<T>);
    return Status::OK();
  }

  Status Visit(const FixedSizeBinaryType&) {
    kernel = MakeKernel(std::move(argument_type), GroupedOneInit<FixedSizeBinaryType>);
    return Status::OK();
  }

  Status Visit(const BooleanType&) {
    kernel = MakeKernel(std::move(argument_type), GroupedOneInit<BooleanType>);
    return Status::OK();
  }

  Status Visit(const NullType&) {
    kernel = MakeKernel(std::move(argument_type), HashAggregateInit<GroupedNullOneImpl>);
    return Status::OK();
  }

  Status Visit(const HalfFloatType& type) {
    return Status::NotImplemented("Outputting one of data of type ", type);
  }

  Status Visit(const DataType& type) {
    return Status::NotImplemented("Outputting one of data of type ", type);
  }

  static Result<HashAggregateKernel> Make(const std::shared_ptr<DataType>& type) {
    GroupedOneFactory factory;
    factory.argument_type = type->id();
    RETURN_NOT_OK(VisitTypeInline(*type, &factory));
    return std::move(factory.kernel);
  }

  HashAggregateKernel kernel;
  InputType argument_type;
};

// ----------------------------------------------------------------------
// List implementation

template <typename Type, typename Enable = void>
struct GroupedListImpl final : public GroupedAggregator {
  using CType = typename TypeTraits<Type>::CType;
  using GetSet = GroupedValueTraits<Type>;

  Status Init(ExecContext* ctx, const KernelInitArgs&) override {
    ctx_ = ctx;
    has_nulls_ = false;
    // out_type_ initialized by GroupedListInit
    values_ = TypedBufferBuilder<CType>(ctx_->memory_pool());
    groups_ = TypedBufferBuilder<uint32_t>(ctx_->memory_pool());
    values_bitmap_ = TypedBufferBuilder<bool>(ctx_->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    num_groups_ = new_num_groups;
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    const ArraySpan& values_array_data = batch[0].array;
    const ArraySpan& groups_array_data = batch[1].array;

    int64_t num_values = values_array_data.length;
    const auto* groups = groups_array_data.GetValues<uint32_t>(1, 0);
    DCHECK_EQ(groups_array_data.offset, 0);
    RETURN_NOT_OK(groups_.Append(groups, num_values));

    int64_t offset = values_array_data.offset;
    const uint8_t* values = values_array_data.buffers[1].data;
    RETURN_NOT_OK(GetSet::AppendBuffers(&values_, values, offset, num_values));

    if (batch[0].null_count() > 0) {
      if (!has_nulls_) {
        has_nulls_ = true;
        RETURN_NOT_OK(values_bitmap_.Append(num_args_, true));
      }
      const uint8_t* values_bitmap = values_array_data.buffers[0].data;
      RETURN_NOT_OK(GroupedValueTraits<BooleanType>::AppendBuffers(
          &values_bitmap_, values_bitmap, offset, num_values));
    } else if (has_nulls_) {
      RETURN_NOT_OK(values_bitmap_.Append(num_values, true));
    }
    num_args_ += num_values;
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedListImpl*>(&raw_other);
    const auto* other_raw_groups = other->groups_.data();
    const auto* g = group_id_mapping.GetValues<uint32_t>(1);

    for (uint32_t other_g = 0; static_cast<int64_t>(other_g) < other->num_args_;
         ++other_g) {
      RETURN_NOT_OK(groups_.Append(g[other_raw_groups[other_g]]));
    }

    const auto* values = reinterpret_cast<const uint8_t*>(other->values_.data());
    RETURN_NOT_OK(GetSet::AppendBuffers(&values_, values, 0, other->num_args_));

    if (other->has_nulls_) {
      if (!has_nulls_) {
        has_nulls_ = true;
        RETURN_NOT_OK(values_bitmap_.Append(num_args_, true));
      }
      const uint8_t* values_bitmap = other->values_bitmap_.data();
      RETURN_NOT_OK(GroupedValueTraits<BooleanType>::AppendBuffers(
          &values_bitmap_, values_bitmap, 0, other->num_args_));
    } else if (has_nulls_) {
      RETURN_NOT_OK(values_bitmap_.Append(other->num_args_, true));
    }
    num_args_ += other->num_args_;
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto values_buffer, values_.Finish());
    ARROW_ASSIGN_OR_RAISE(auto groups_buffer, groups_.Finish());
    ARROW_ASSIGN_OR_RAISE(auto null_bitmap_buffer, values_bitmap_.Finish());

    auto groups = UInt32Array(num_args_, groups_buffer);
    ARROW_ASSIGN_OR_RAISE(
        auto groupings,
        Grouper::MakeGroupings(groups, static_cast<uint32_t>(num_groups_), ctx_));

    auto values_array_data = ArrayData::Make(
        out_type_, num_args_,
        {has_nulls_ ? std::move(null_bitmap_buffer) : nullptr, std::move(values_buffer)});
    auto values = MakeArray(values_array_data);
    return Grouper::ApplyGroupings(*groupings, *values);
  }

  std::shared_ptr<DataType> out_type() const override { return list(out_type_); }

  ExecContext* ctx_;
  int64_t num_groups_, num_args_ = 0;
  bool has_nulls_ = false;
  TypedBufferBuilder<CType> values_;
  TypedBufferBuilder<uint32_t> groups_;
  TypedBufferBuilder<bool> values_bitmap_;
  std::shared_ptr<DataType> out_type_;
};

template <typename Type>
struct GroupedListImpl<Type, enable_if_t<is_base_binary_type<Type>::value ||
                                         std::is_same<Type, FixedSizeBinaryType>::value>>
    final : public GroupedAggregator {
  using Allocator = arrow::stl::allocator<char>;
  using StringType = std::basic_string<char, std::char_traits<char>, Allocator>;
  using GetSet = GroupedValueTraits<Type>;

  Status Init(ExecContext* ctx, const KernelInitArgs&) override {
    ctx_ = ctx;
    allocator_ = Allocator(ctx_->memory_pool());
    // out_type_ initialized by GroupedListInit
    groups_ = TypedBufferBuilder<uint32_t>(ctx_->memory_pool());
    values_bitmap_ = TypedBufferBuilder<bool>(ctx_->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    num_groups_ = new_num_groups;
    return Status::OK();
  }

  Status Consume(const ExecSpan& batch) override {
    const ArraySpan& values_array_data = batch[0].array;
    int64_t num_values = values_array_data.length;
    int64_t offset = values_array_data.offset;

    const ArraySpan& groups_array_data = batch[1].array;
    const uint32_t* groups = groups_array_data.GetValues<uint32_t>(1, 0);
    DCHECK_EQ(groups_array_data.offset, 0);
    RETURN_NOT_OK(groups_.Append(groups, num_values));

    if (batch[0].null_count() == 0) {
      RETURN_NOT_OK(values_bitmap_.Append(num_values, true));
    } else {
      const uint8_t* values_bitmap = values_array_data.buffers[0].data;
      RETURN_NOT_OK(GroupedValueTraits<BooleanType>::AppendBuffers(
          &values_bitmap_, values_bitmap, offset, num_values));
    }
    num_args_ += num_values;
    return VisitGroupedValues<Type>(
        batch,
        [&](uint32_t group, std::string_view val) -> Status {
          values_.emplace_back(StringType(val.data(), val.size(), allocator_));
          return Status::OK();
        },
        [&](uint32_t group) -> Status {
          values_.emplace_back("");
          return Status::OK();
        });
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedListImpl*>(&raw_other);
    const auto* other_raw_groups = other->groups_.data();
    const auto* g = group_id_mapping.GetValues<uint32_t>(1);

    for (uint32_t other_g = 0; static_cast<int64_t>(other_g) < other->num_args_;
         ++other_g) {
      RETURN_NOT_OK(groups_.Append(g[other_raw_groups[other_g]]));
    }

    values_.insert(values_.end(), other->values_.begin(), other->values_.end());

    const uint8_t* values_bitmap = other->values_bitmap_.data();
    RETURN_NOT_OK(GroupedValueTraits<BooleanType>::AppendBuffers(
        &values_bitmap_, values_bitmap, 0, other->num_args_));
    num_args_ += other->num_args_;
    return Status::OK();
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto groups_buffer, groups_.Finish());
    ARROW_ASSIGN_OR_RAISE(auto null_bitmap_buffer, values_bitmap_.Finish());

    auto groups = UInt32Array(num_args_, groups_buffer);
    ARROW_ASSIGN_OR_RAISE(
        auto groupings,
        Grouper::MakeGroupings(groups, static_cast<uint32_t>(num_groups_), ctx_));

    auto values_array_data =
        ArrayData::Make(out_type_, num_args_, {std::move(null_bitmap_buffer), nullptr});
    RETURN_NOT_OK(MakeOffsetsValues(values_array_data.get(), values_));
    auto values = MakeArray(values_array_data);
    return Grouper::ApplyGroupings(*groupings, *values);
  }

  template <typename T = Type>
  enable_if_base_binary<T, Status> MakeOffsetsValues(
      ArrayData* array, const std::vector<std::optional<StringType>>& values) {
    using offset_type = typename T::offset_type;
    ARROW_ASSIGN_OR_RAISE(
        auto raw_offsets,
        AllocateBuffer((1 + values.size()) * sizeof(offset_type), ctx_->memory_pool()));
    auto* offsets = raw_offsets->mutable_data_as<offset_type>();
    offsets[0] = 0;
    offsets++;
    const uint8_t* null_bitmap = array->buffers[0]->data();
    offset_type total_length = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        if (value->size() >
                static_cast<size_t>(std::numeric_limits<offset_type>::max()) ||
            arrow::internal::AddWithOverflow(
                total_length, static_cast<offset_type>(value->size()), &total_length)) {
          return Status::Invalid("Result is too large to fit in ", *array->type,
                                 " cast to large_ variant of type");
        }
      }
      offsets[i] = total_length;
    }
    ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(total_length, ctx_->memory_pool()));
    int64_t offset = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        std::memcpy(data->mutable_data() + offset, value->data(), value->size());
        offset += value->size();
      }
    }
    array->buffers[1] = std::move(raw_offsets);
    array->buffers.push_back(std::move(data));
    return Status::OK();
  }

  template <typename T = Type>
  enable_if_same<T, FixedSizeBinaryType, Status> MakeOffsetsValues(
      ArrayData* array, const std::vector<std::optional<StringType>>& values) {
    const uint8_t* null_bitmap = array->buffers[0]->data();
    const int32_t slot_width =
        checked_cast<const FixedSizeBinaryType&>(*array->type).byte_width();
    int64_t total_length = values.size() * slot_width;
    ARROW_ASSIGN_OR_RAISE(auto data, AllocateBuffer(total_length, ctx_->memory_pool()));
    int64_t offset = 0;
    for (size_t i = 0; i < values.size(); i++) {
      if (bit_util::GetBit(null_bitmap, i)) {
        const std::optional<StringType>& value = values[i];
        DCHECK(value.has_value());
        std::memcpy(data->mutable_data() + offset, value->data(), slot_width);
      } else {
        std::memset(data->mutable_data() + offset, 0x00, slot_width);
      }
      offset += slot_width;
    }
    array->buffers[1] = std::move(data);
    return Status::OK();
  }

  std::shared_ptr<DataType> out_type() const override { return list(out_type_); }

  ExecContext* ctx_;
  Allocator allocator_;
  int64_t num_groups_, num_args_ = 0;
  std::vector<std::optional<StringType>> values_;
  TypedBufferBuilder<uint32_t> groups_;
  TypedBufferBuilder<bool> values_bitmap_;
  std::shared_ptr<DataType> out_type_;
};

struct GroupedNullListImpl : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs&) override {
    ctx_ = ctx;
    counts_ = TypedBufferBuilder<int64_t>(ctx_->memory_pool());
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    auto added_groups = new_num_groups - num_groups_;
    num_groups_ = new_num_groups;
    return counts_.Append(added_groups, 0);
  }

  Status Consume(const ExecSpan& batch) override {
    int64_t* counts = counts_.mutable_data();
    const auto* g_begin = batch[1].array.GetValues<uint32_t>(1);
    for (int64_t i = 0; i < batch.length; ++i, ++g_begin) {
      counts[*g_begin] += 1;
    }
    return Status::OK();
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedNullListImpl*>(&raw_other);

    int64_t* counts = counts_.mutable_data();
    const int64_t* other_counts = other->counts_.data();

    const auto* g = group_id_mapping.GetValues<uint32_t>(1);
    for (int64_t other_g = 0; other_g < group_id_mapping.length; ++other_g, ++g) {
      counts[*g] += other_counts[other_g];
    }

    return Status::OK();
  }

  Result<Datum> Finalize() override {
    std::unique_ptr<ArrayBuilder> builder;
    RETURN_NOT_OK(MakeBuilder(ctx_->memory_pool(), list(null()), &builder));
    auto list_builder = checked_cast<ListBuilder*>(builder.get());
    auto value_builder = checked_cast<NullBuilder*>(list_builder->value_builder());
    const int64_t* counts = counts_.data();

    for (int64_t group = 0; group < num_groups_; ++group) {
      RETURN_NOT_OK(list_builder->Append(true));
      RETURN_NOT_OK(value_builder->AppendNulls(counts[group]));
    }
    return list_builder->Finish();
  }

  std::shared_ptr<DataType> out_type() const override { return list(null()); }

  ExecContext* ctx_;
  int64_t num_groups_ = 0;
  TypedBufferBuilder<int64_t> counts_;
};

template <typename T>
Result<std::unique_ptr<KernelState>> GroupedListInit(KernelContext* ctx,
                                                     const KernelInitArgs& args) {
  ARROW_ASSIGN_OR_RAISE(auto impl, HashAggregateInit<GroupedListImpl<T>>(ctx, args));
  auto instance = static_cast<GroupedListImpl<T>*>(impl.get());
  instance->out_type_ = args.inputs[0].GetSharedPtr();
  return impl;
}

struct GroupedListFactory {
  template <typename T>
  enable_if_physical_integer<T, Status> Visit(const T&) {
    using PhysicalType = typename T::PhysicalType;
    kernel = MakeKernel(std::move(argument_type), GroupedListInit<PhysicalType>);
    return Status::OK();
  }

  template <typename T>
  enable_if_floating_point<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), GroupedListInit<T>);
    return Status::OK();
  }

  template <typename T>
  enable_if_decimal<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), GroupedListInit<T>);
    return Status::OK();
  }

  template <typename T>
  enable_if_base_binary<T, Status> Visit(const T&) {
    kernel = MakeKernel(std::move(argument_type), GroupedListInit<T>);
    return Status::OK();
  }

  Status Visit(const FixedSizeBinaryType&) {
    kernel = MakeKernel(std::move(argument_type), GroupedListInit<FixedSizeBinaryType>);
    return Status::OK();
  }

  Status Visit(const BooleanType&) {
    kernel = MakeKernel(std::move(argument_type), GroupedListInit<BooleanType>);
    return Status::OK();
  }

  Status Visit(const NullType&) {
    kernel = MakeKernel(std::move(argument_type), HashAggregateInit<GroupedNullListImpl>);
    return Status::OK();
  }

  Status Visit(const HalfFloatType& type) {
    return Status::NotImplemented("Outputting list of data of type ", type);
  }

  Status Visit(const DataType& type) {
    return Status::NotImplemented("Outputting list of data of type ", type);
  }

  static Result<HashAggregateKernel> Make(const std::shared_ptr<DataType>& type) {
    GroupedListFactory factory;
    factory.argument_type = type->id();
    RETURN_NOT_OK(VisitTypeInline(*type, &factory));
    return std::move(factory.kernel);
  }

  HashAggregateKernel kernel;
  InputType argument_type;
};

// ----------------------------------------------------------------------
// Pivot implementation

struct GroupedPivotAccumulator {
  Status Init(ExecContext* ctx, std::shared_ptr<DataType> value_type,
              const PivotWiderOptions* options) {
    ctx_ = ctx;
    value_type_ = std::move(value_type);
    num_keys_ = static_cast<int>(options->key_names.size());
    num_groups_ = 0;
    columns_.resize(num_keys_);
    scratch_buffer_ = BufferBuilder(ctx_->memory_pool());
    return Status::OK();
  }

  Status Consume(span<const uint32_t> groups, span<const PivotWiderKeyIndex> keys,
                 const ArraySpan& values) {
    // To dispatch the values into the right (group, key) coordinates,
    // we first compute a vector of take indices for each output column.
    //
    // For each index #i, we set take_indices[keys[#i]][groups[#i]] = #i.
    // Unpopulated take_indices entries are null.
    //
    // For example, assuming we get:
    //   groups  |  keys
    // ===================
    //    1      |   0
    //    3      |   1
    //    1      |   1
    //    0      |   1
    //
    // We are going to compute:
    // - take_indices[key = 0] = [null, 0, null, null]
    // - take_indices[key = 1] = [3, 2, null, 1]
    //
    // Then each output column is computed by taking the values with the
    // respective take_indices for the column's keys.
    //

    DCHECK_EQ(groups.size(), keys.size());
    DCHECK_EQ(groups.size(), static_cast<size_t>(values.length));

    std::shared_ptr<DataType> take_index_type;
    std::vector<std::shared_ptr<Buffer>> take_indices(num_keys_);
    std::vector<std::shared_ptr<Buffer>> take_bitmaps(num_keys_);

    // A generic lambda that computes the take indices with the desired integer width
    auto compute_take_indices = [&](auto typed_index) {
      ARROW_UNUSED(typed_index);
      using TakeIndex = std::decay_t<decltype(typed_index)>;
      take_index_type = CTypeTraits<TakeIndex>::type_singleton();

      const int64_t take_indices_size =
          bit_util::RoundUpToMultipleOf64(num_groups_ * sizeof(TakeIndex));
      const int64_t take_bitmap_size =
          bit_util::RoundUpToMultipleOf64(bit_util::BytesForBits(num_groups_));
      const int64_t total_scratch_size =
          num_keys_ * (take_indices_size + take_bitmap_size);
      RETURN_NOT_OK(scratch_buffer_.Resize(total_scratch_size, /*shrink_to_fit=*/false));

      // Slice the scratch space into individual buffers for each output column's
      // take_indices array.
      std::vector<TakeIndex*> take_indices_data(num_keys_);
      std::vector<uint8_t*> take_bitmap_data(num_keys_);
      int64_t offset = 0;
      for (int i = 0; i < num_keys_; ++i) {
        take_indices[i] = std::make_shared<MutableBuffer>(
            scratch_buffer_.mutable_data() + offset, take_indices_size);
        take_indices_data[i] = take_indices[i]->mutable_data_as<TakeIndex>();
        offset += take_indices_size;
        take_bitmaps[i] = std::make_shared<MutableBuffer>(
            scratch_buffer_.mutable_data() + offset, take_bitmap_size);
        take_bitmap_data[i] = take_bitmaps[i]->mutable_data();
        memset(take_bitmap_data[i], 0, take_bitmap_size);
        offset += take_bitmap_size;
      }
      DCHECK_LE(offset, scratch_buffer_.capacity());

      // Populate the take_indices for each output column
      for (int64_t i = 0; i < values.length; ++i) {
        const PivotWiderKeyIndex key = keys[i];
        if (key != kNullPivotKey && !values.IsNull(i)) {
          DCHECK_LT(static_cast<int>(key), num_keys_);
          const uint32_t group = groups[i];
          if (bit_util::GetBit(take_bitmap_data[key], group)) {
            return DuplicateValue();
          }
          // For row #group in column #key, we are going to take the value at index #i
          bit_util::SetBit(take_bitmap_data[key], group);
          take_indices_data[key][group] = static_cast<TakeIndex>(i);
        }
      }
      return Status::OK();
    };

    // Call compute_take_indices with the optimal integer width
    if (values.length <= static_cast<int64_t>(std::numeric_limits<uint8_t>::max())) {
      RETURN_NOT_OK(compute_take_indices(uint8_t{}));
    } else if (values.length <=
               static_cast<int64_t>(std::numeric_limits<uint16_t>::max())) {
      RETURN_NOT_OK(compute_take_indices(uint16_t{}));
    } else if (values.length <=
               static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
      RETURN_NOT_OK(compute_take_indices(uint32_t{}));
    } else {
      RETURN_NOT_OK(compute_take_indices(uint64_t{}));
    }

    // Use take_indices to compute the output columns for this batch
    auto values_data = values.ToArrayData();
    ArrayVector new_columns(num_keys_);
    TakeOptions take_options(/*boundscheck=*/false);
    for (int i = 0; i < num_keys_; ++i) {
      auto indices_data =
          ArrayData::Make(take_index_type, num_groups_,
                          {std::move(take_bitmaps[i]), std::move(take_indices[i])});
      // If indices_data is all nulls, we can just ignore this column.
      if (indices_data->GetNullCount() != indices_data->length) {
        ARROW_ASSIGN_OR_RAISE(Datum grouped_column,
                              Take(values_data, indices_data, take_options, ctx_));
        new_columns[i] = grouped_column.make_array();
      }
    }
    // Merge them with the previous columns
    return MergeColumns(std::move(new_columns));
  }

  Status Consume(span<const uint32_t> groups, const PivotWiderKeyIndex key,
                 const ArraySpan& values) {
    if (key == kNullPivotKey) {
      // Nothing to update
      return Status::OK();
    }
    DCHECK_LT(static_cast<int>(key), num_keys_);
    DCHECK_EQ(groups.size(), static_cast<size_t>(values.length));

    // The algorithm is simpler than in the array-taking version of Consume()
    // below, since only the column #key needs to be updated.
    std::shared_ptr<DataType> take_index_type;
    std::shared_ptr<Buffer> take_indices;
    std::shared_ptr<Buffer> take_bitmap;

    // A generic lambda that computes the take indices with the desired integer width
    auto compute_take_indices = [&](auto typed_index) {
      ARROW_UNUSED(typed_index);
      using TakeIndex = std::decay_t<decltype(typed_index)>;
      take_index_type = CTypeTraits<TakeIndex>::type_singleton();

      const int64_t take_indices_size =
          bit_util::RoundUpToMultipleOf64(num_groups_ * sizeof(TakeIndex));
      const int64_t take_bitmap_size =
          bit_util::RoundUpToMultipleOf64(bit_util::BytesForBits(num_groups_));
      const int64_t total_scratch_size = take_indices_size + take_bitmap_size;
      RETURN_NOT_OK(scratch_buffer_.Resize(total_scratch_size, /*shrink_to_fit=*/false));

      take_indices = std::make_shared<MutableBuffer>(scratch_buffer_.mutable_data(),
                                                     take_indices_size);
      take_bitmap = std::make_shared<MutableBuffer>(
          scratch_buffer_.mutable_data() + take_indices_size, take_bitmap_size);
      auto take_indices_data = take_indices->mutable_data_as<TakeIndex>();
      auto take_bitmap_data = take_bitmap->mutable_data();
      memset(take_bitmap_data, 0, take_bitmap_size);

      for (int64_t i = 0; i < values.length; ++i) {
        const uint32_t group = groups[i];
        if (!values.IsNull(i)) {
          if (bit_util::GetBit(take_bitmap_data, group)) {
            return DuplicateValue();
          }
          bit_util::SetBit(take_bitmap_data, group);
          take_indices_data[group] = static_cast<TakeIndex>(i);
        }
      }
      return Status::OK();
    };

    // Call compute_take_indices with the optimal integer width
    if (values.length <= static_cast<int64_t>(std::numeric_limits<uint8_t>::max())) {
      RETURN_NOT_OK(compute_take_indices(uint8_t{}));
    } else if (values.length <=
               static_cast<int64_t>(std::numeric_limits<uint16_t>::max())) {
      RETURN_NOT_OK(compute_take_indices(uint16_t{}));
    } else if (values.length <=
               static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
      RETURN_NOT_OK(compute_take_indices(uint32_t{}));
    } else {
      RETURN_NOT_OK(compute_take_indices(uint64_t{}));
    }

    // Use take_indices to update column #key
    auto values_data = values.ToArrayData();
    auto indices_data = ArrayData::Make(
        take_index_type, num_groups_, {std::move(take_bitmap), std::move(take_indices)});
    TakeOptions take_options(/*boundscheck=*/false);
    ARROW_ASSIGN_OR_RAISE(Datum grouped_column,
                          Take(values_data, indices_data, take_options, ctx_));
    return MergeColumn(&columns_[key], grouped_column.make_array());
  }

  Status Resize(int64_t new_num_groups) {
    if (new_num_groups > std::numeric_limits<int32_t>::max()) {
      return Status::NotImplemented("Pivot with more 2**31 groups");
    }
    return ResizeColumns(new_num_groups);
  }

  Status Merge(GroupedPivotAccumulator&& other, const ArrayData& group_id_mapping) {
    // To merge `other` into `*this`, we simply merge their respective columns.
    // However, we must first transpose `other`'s rows using `group_id_mapping`.
    // This is a logical "scatter" operation.
    //
    // Since `scatter(indices)` is implemented as `take(inverse_permutation(indices))`,
    // we can save time by computing `inverse_permutation(indices)` once for all
    // columns.

    // Scatter/InversePermutation only accept signed indices. We checked
    // in Resize() above that we were inside the limites for int32.
    auto scatter_indices = group_id_mapping.Copy();
    scatter_indices->type = int32();
    std::shared_ptr<DataType> take_indices_type;
    if (num_groups_ - 1 <= std::numeric_limits<int8_t>::max()) {
      take_indices_type = int8();
    } else if (num_groups_ - 1 <= std::numeric_limits<int16_t>::max()) {
      take_indices_type = int16();
    } else {
      DCHECK_GE(num_groups_ - 1, std::numeric_limits<int32_t>::max());
      take_indices_type = int32();
    }
    InversePermutationOptions options(/*max_index=*/num_groups_ - 1, take_indices_type);
    ARROW_ASSIGN_OR_RAISE(auto take_indices,
                          InversePermutation(scatter_indices, options, ctx_));
    auto scatter_column =
        [&](const std::shared_ptr<Array>& column) -> Result<std::shared_ptr<Array>> {
      TakeOptions take_options(/*boundscheck=*/false);
      ARROW_ASSIGN_OR_RAISE(auto scattered,
                            Take(column, take_indices, take_options, ctx_));
      return scattered.make_array();
    };
    return MergeColumns(std::move(other.columns_), std::move(scatter_column));
  }

  Result<ArrayVector> Finalize() {
    // Ensure that columns are allocated even if num_groups_ == 0
    RETURN_NOT_OK(ResizeColumns(num_groups_));
    return std::move(columns_);
  }

 protected:
  Status ResizeColumns(int64_t new_num_groups) {
    if (new_num_groups == num_groups_ && num_groups_ != 0) {
      return Status::OK();
    }
    ARROW_ASSIGN_OR_RAISE(
        auto array_suffix,
        MakeArrayOfNull(value_type_, new_num_groups - num_groups_, ctx_->memory_pool()));
    for (auto& column : columns_) {
      if (num_groups_ != 0) {
        DCHECK_NE(column, nullptr);
        ARROW_ASSIGN_OR_RAISE(
            column, Concatenate({std::move(column), array_suffix}, ctx_->memory_pool()));
      } else {
        column = array_suffix;
      }
      DCHECK_EQ(column->length(), new_num_groups);
    }
    num_groups_ = new_num_groups;
    return Status::OK();
  }

  using ColumnTransform =
      std::function<Result<std::shared_ptr<Array>>(const std::shared_ptr<Array>&)>;

  Status MergeColumns(ArrayVector&& other_columns,
                      const ColumnTransform& transform = {}) {
    DCHECK_EQ(columns_.size(), other_columns.size());
    for (int i = 0; i < num_keys_; ++i) {
      if (other_columns[i]) {
        RETURN_NOT_OK(MergeColumn(&columns_[i], std::move(other_columns[i]), transform));
      }
    }
    return Status::OK();
  }

  Status MergeColumn(std::shared_ptr<Array>* column, std::shared_ptr<Array> other_column,
                     const ColumnTransform& transform = {}) {
    if (other_column->null_count() == other_column->length()) {
      // Avoid paying for the transform step below, since merging will be a no-op anyway.
      return Status::OK();
    }
    if (transform) {
      ARROW_ASSIGN_OR_RAISE(other_column, transform(other_column));
    }
    DCHECK_EQ(num_groups_, other_column->length());
    if (!*column) {
      *column = other_column;
      return Status::OK();
    }
    if ((*column)->null_count() == (*column)->length()) {
      *column = other_column;
      return Status::OK();
    }
    int64_t expected_non_nulls = (num_groups_ - (*column)->null_count()) +
                                 (num_groups_ - other_column->null_count());
    ARROW_ASSIGN_OR_RAISE(auto coalesced,
                          CallFunction("coalesce", {*column, other_column}, ctx_));
    // Check that all non-null values in other_column and column were kept in the result.
    if (expected_non_nulls != num_groups_ - coalesced.null_count()) {
      DCHECK_GT(expected_non_nulls, num_groups_ - coalesced.null_count());
      return DuplicateValue();
    }
    *column = coalesced.make_array();
    return Status::OK();
  }

  Status DuplicateValue() {
    return Status::Invalid(
        "Encountered more than one non-null value for the same grouped pivot key");
  }

  ExecContext* ctx_;
  std::shared_ptr<DataType> value_type_;
  int num_keys_;
  int64_t num_groups_;
  ArrayVector columns_;
  // A persistent scratch buffer to store the take indices in Consume
  BufferBuilder scratch_buffer_;
};

struct GroupedPivotImpl : public GroupedAggregator {
  Status Init(ExecContext* ctx, const KernelInitArgs& args) override {
    DCHECK_EQ(args.inputs.size(), 3);
    key_type_ = args.inputs[0].GetSharedPtr();
    options_ = checked_cast<const PivotWiderOptions*>(args.options);
    DCHECK_NE(options_, nullptr);
    auto value_type = args.inputs[1].GetSharedPtr();
    FieldVector fields;
    fields.reserve(options_->key_names.size());
    for (const auto& key_name : options_->key_names) {
      fields.push_back(field(key_name, value_type));
    }
    out_type_ = struct_(std::move(fields));
    out_struct_type_ = checked_cast<const StructType*>(out_type_.get());
    ARROW_ASSIGN_OR_RAISE(key_mapper_, PivotWiderKeyMapper::Make(*key_type_, options_));
    RETURN_NOT_OK(accumulator_.Init(ctx, value_type, options_));
    return Status::OK();
  }

  Status Resize(int64_t new_num_groups) override {
    num_groups_ = new_num_groups;
    return accumulator_.Resize(new_num_groups);
  }

  Status Merge(GroupedAggregator&& raw_other,
               const ArrayData& group_id_mapping) override {
    auto other = checked_cast<GroupedPivotImpl*>(&raw_other);
    return accumulator_.Merge(std::move(other->accumulator_), group_id_mapping);
  }

  Status Consume(const ExecSpan& batch) override {
    DCHECK_EQ(batch.values.size(), 3);
    auto groups = batch[2].array.GetSpan<const uint32_t>(1, batch.length);
    if (!batch[1].is_array()) {
      return Status::NotImplemented("Consuming scalar pivot value");
    }
    if (batch[0].is_array()) {
      ARROW_ASSIGN_OR_RAISE(span<const PivotWiderKeyIndex> keys,
                            key_mapper_->MapKeys(batch[0].array));
      return accumulator_.Consume(groups, keys, batch[1].array);
    } else {
      ARROW_ASSIGN_OR_RAISE(PivotWiderKeyIndex key,
                            key_mapper_->MapKey(*batch[0].scalar));
      return accumulator_.Consume(groups, key, batch[1].array);
    }
  }

  Result<Datum> Finalize() override {
    ARROW_ASSIGN_OR_RAISE(auto columns, accumulator_.Finalize());
    DCHECK_EQ(columns.size(), static_cast<size_t>(out_struct_type_->num_fields()));
    return std::make_shared<StructArray>(out_type_, num_groups_, std::move(columns),
                                         /*null_bitmap=*/nullptr,
                                         /*null_count=*/0);
  }

  std::shared_ptr<DataType> out_type() const override { return out_type_; }

  std::shared_ptr<DataType> key_type_;
  std::shared_ptr<DataType> out_type_;
  const StructType* out_struct_type_;
  const PivotWiderOptions* options_;
  std::unique_ptr<PivotWiderKeyMapper> key_mapper_;
  GroupedPivotAccumulator accumulator_;
  int64_t num_groups_ = 0;
};

// ----------------------------------------------------------------------
// Docstrings

const FunctionDoc hash_count_doc{
    "Count the number of null / non-null values in each group",
    ("By default, only non-null values are counted.\n"
     "This can be changed through ScalarAggregateOptions."),
    {"array", "group_id_array"},
    "CountOptions"};

const FunctionDoc hash_count_all_doc{"Count the number of rows in each group",
                                     ("Not caring about the values of any column."),
                                     {"group_id_array"}};

const FunctionDoc hash_sum_doc{"Sum values in each group",
                               ("Null values are ignored."),
                               {"array", "group_id_array"},
                               "ScalarAggregateOptions"};

const FunctionDoc hash_product_doc{
    "Compute the product of values in each group",
    ("Null values are ignored.\n"
     "On integer overflow, the result will wrap around as if the calculation\n"
     "was done with unsigned integers."),
    {"array", "group_id_array"},
    "ScalarAggregateOptions"};

const FunctionDoc hash_mean_doc{
    "Compute the mean of values in each group",
    ("Null values are ignored.\n"
     "For integers and floats, NaN is emitted if min_count = 0 and\n"
     "there are no values in a group. For decimals, null is emitted instead."),
    {"array", "group_id_array"},
    "ScalarAggregateOptions"};

const FunctionDoc hash_stddev_doc{
    "Compute the standard deviation of values in each group",
    ("The number of degrees of freedom can be controlled using VarianceOptions.\n"
     "By default (`ddof` = 0), the population standard deviation is calculated.\n"
     "Nulls are ignored.  If there are not enough non-null values in a group\n"
     "to satisfy `ddof`, null is emitted."),
    {"array", "group_id_array"}};

const FunctionDoc hash_variance_doc{
    "Compute the variance of values in each group",
    ("The number of degrees of freedom can be controlled using VarianceOptions.\n"
     "By default (`ddof` = 0), the population variance is calculated.\n"
     "Nulls are ignored.  If there are not enough non-null values in a group\n"
     "to satisfy `ddof`, null is emitted."),
    {"array", "group_id_array"}};

const FunctionDoc hash_skew_doc{
    "Compute the skewness of values in each group",
    ("Nulls are ignored by default.  If there are not enough non-null values\n"
     "in a group to satisfy `min_count`, null is emitted.\n"
     "The behavior of nulls and the `min_count` parameter can be changed\n"
     "in SkewOptions."),
    {"array", "group_id_array"}};

const FunctionDoc hash_kurtosis_doc{
    "Compute the kurtosis of values in each group",
    ("Nulls are ignored by default.  If there are not enough non-null values\n"
     "in a group to satisfy `min_count`, null is emitted.\n"
     "The behavior of nulls and the `min_count` parameter can be changed\n"
     "in SkewOptions."),
    {"array", "group_id_array"}};

const FunctionDoc hash_tdigest_doc{
    "Compute approximate quantiles of values in each group",
    ("The T-Digest algorithm is used for a fast approximation.\n"
     "By default, the 0.5 quantile (i.e. median) is emitted.\n"
     "Nulls and NaNs are ignored.\n"
     "Nulls are returned if there are no valid data points."),
    {"array", "group_id_array"},
    "TDigestOptions"};

const FunctionDoc hash_approximate_median_doc{
    "Compute approximate medians of values in each group",
    ("The T-Digest algorithm is used for a fast approximation.\n"
     "Nulls and NaNs are ignored.\n"
     "Nulls are returned if there are no valid data points."),
    {"array", "group_id_array"},
    "ScalarAggregateOptions"};

const FunctionDoc hash_first_last_doc{
    "Compute the first and last of values in each group",
    ("Null values are ignored by default.\n"
     "If skip_nulls = false, then this will return the first and last values\n"
     "regardless if it is null"),
    {"array", "group_id_array"},
    "ScalarAggregateOptions"};

const FunctionDoc hash_first_doc{
    "Compute the first value in each group",
    ("Null values are ignored by default.\n"
     "If skip_nulls = false, then this will return the first and last values\n"
     "regardless if it is null"),
    {"array", "group_id_array"},
    "ScalarAggregateOptions"};

const FunctionDoc hash_last_doc{
    "Compute the first value in each group",
    ("Null values are ignored by default.\n"
     "If skip_nulls = false, then this will return the first and last values\n"
     "regardless if it is null"),
    {"array", "group_id_array"},
    "ScalarAggregateOptions"};

const FunctionDoc hash_min_max_doc{
    "Compute the minimum and maximum of values in each group",
    ("Null values are ignored by default.\n"
     "This can be changed through ScalarAggregateOptions."),
    {"array", "group_id_array"},
    "ScalarAggregateOptions"};

const FunctionDoc hash_min_or_max_doc{
    "Compute the minimum or maximum of values in each group",
    ("Null values are ignored by default.\n"
     "This can be changed through ScalarAggregateOptions."),
    {"array", "group_id_array"},
    "ScalarAggregateOptions"};

const FunctionDoc hash_any_doc{"Whether any element in each group evaluates to true",
                               ("Null values are ignored."),
                               {"array", "group_id_array"},
                               "ScalarAggregateOptions"};

const FunctionDoc hash_all_doc{"Whether all elements in each group evaluate to true",
                               ("Null values are ignored."),
                               {"array", "group_id_array"},
                               "ScalarAggregateOptions"};

const FunctionDoc hash_count_distinct_doc{
    "Count the distinct values in each group",
    ("Whether nulls/values are counted is controlled by CountOptions.\n"
     "NaNs and signed zeroes are not normalized."),
    {"array", "group_id_array"},
    "CountOptions"};

const FunctionDoc hash_distinct_doc{
    "Keep the distinct values in each group",
    ("Whether nulls/values are kept is controlled by CountOptions.\n"
     "NaNs and signed zeroes are not normalized."),
    {"array", "group_id_array"},
    "CountOptions"};

const FunctionDoc hash_one_doc{"Get one value from each group",
                               ("Null values are also returned."),
                               {"array", "group_id_array"}};

const FunctionDoc hash_list_doc{"List all values in each group",
                                ("Null values are also returned."),
                                {"array", "group_id_array"}};

const FunctionDoc hash_pivot_doc{
    "Pivot values according to a pivot key column",
    ("Output is a struct array with as many fields as `PivotWiderOptions.key_names`.\n"
     "All output struct fields have the same type as `pivot_values`.\n"
     "Each pivot key decides in which output field the corresponding pivot value\n"
     "is emitted. If a pivot key doesn't appear in a given group, null is emitted.\n"
     "If more than one non-null value is encountered in the same group for a\n"
     "given pivot key, Invalid is raised.\n"
     "Behavior of unexpected pivot keys is controlled by `unexpected_key_behavior`\n"
     "in PivotWiderOptions."),
    {"pivot_keys", "pivot_values", "group_id_array"},
    "PivotWiderOptions"};

}  // namespace

void RegisterHashAggregateBasic(FunctionRegistry* registry) {
  static const auto default_count_options = CountOptions::Defaults();
  static const auto default_scalar_aggregate_options = ScalarAggregateOptions::Defaults();
  static const auto default_tdigest_options = TDigestOptions::Defaults();
  static const auto default_variance_options = VarianceOptions::Defaults();
  static const auto default_skew_options = SkewOptions::Defaults();

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_count", Arity::Binary(), hash_count_doc, &default_count_options);

    DCHECK_OK(func->AddKernel(
        MakeKernel(InputType::Any(), HashAggregateInit<GroupedCountImpl>)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>("hash_count_all", Arity::Unary(),
                                                        hash_count_all_doc, NULLPTR);

    DCHECK_OK(func->AddKernel(MakeUnaryKernel(HashAggregateInit<GroupedCountAllImpl>)));
    auto status = registry->AddFunction(std::move(func));
    DCHECK_OK(status);
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_sum", Arity::Binary(), hash_sum_doc, &default_scalar_aggregate_options);
    DCHECK_OK(AddHashAggKernels({boolean()}, GroupedSumFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels(SignedIntTypes(), GroupedSumFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels(UnsignedIntTypes(), GroupedSumFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(FloatingPointTypes(), GroupedSumFactory::Make, func.get()));
    // Type parameters are ignored
    DCHECK_OK(AddHashAggKernels({decimal128(1, 1), decimal256(1, 1)},
                                GroupedSumFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels({null()}, GroupedSumFactory::Make, func.get()));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_product", Arity::Binary(), hash_product_doc,
        &default_scalar_aggregate_options);
    DCHECK_OK(AddHashAggKernels({boolean()}, GroupedProductFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(SignedIntTypes(), GroupedProductFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(UnsignedIntTypes(), GroupedProductFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(FloatingPointTypes(), GroupedProductFactory::Make, func.get()));
    // Type parameters are ignored
    DCHECK_OK(AddHashAggKernels({decimal128(1, 1), decimal256(1, 1)},
                                GroupedProductFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels({null()}, GroupedProductFactory::Make, func.get()));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_mean", Arity::Binary(), hash_mean_doc, &default_scalar_aggregate_options);
    DCHECK_OK(AddHashAggKernels({boolean()}, GroupedMeanFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels(SignedIntTypes(), GroupedMeanFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(UnsignedIntTypes(), GroupedMeanFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(FloatingPointTypes(), GroupedMeanFactory::Make, func.get()));
    // Type parameters are ignored
    DCHECK_OK(AddHashAggKernels({decimal128(1, 1), decimal256(1, 1)},
                                GroupedMeanFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels({null()}, GroupedMeanFactory::Make, func.get()));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_stddev", Arity::Binary(), hash_stddev_doc, &default_variance_options);
    DCHECK_OK(AddHashAggregateStatisticKernels(
        func.get(), MakeGroupedStatisticKernel<GroupedStddevImpl>));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_variance", Arity::Binary(), hash_variance_doc, &default_variance_options);
    DCHECK_OK(AddHashAggregateStatisticKernels(
        func.get(), MakeGroupedStatisticKernel<GroupedVarianceImpl>));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_skew", Arity::Binary(), hash_skew_doc, &default_skew_options);
    DCHECK_OK(AddHashAggregateStatisticKernels(
        func.get(), MakeGroupedStatisticKernel<GroupedSkewImpl>));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_kurtosis", Arity::Binary(), hash_kurtosis_doc, &default_skew_options);
    DCHECK_OK(AddHashAggregateStatisticKernels(
        func.get(), MakeGroupedStatisticKernel<GroupedKurtosisImpl>));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  HashAggregateFunction* tdigest_func = nullptr;
  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_tdigest", Arity::Binary(), hash_tdigest_doc, &default_tdigest_options);
    DCHECK_OK(
        AddHashAggKernels(SignedIntTypes(), GroupedTDigestFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(UnsignedIntTypes(), GroupedTDigestFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(FloatingPointTypes(), GroupedTDigestFactory::Make, func.get()));
    // Type parameters are ignored
    DCHECK_OK(AddHashAggKernels({decimal128(1, 1), decimal256(1, 1)},
                                GroupedTDigestFactory::Make, func.get()));
    tdigest_func = func.get();
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_approximate_median", Arity::Binary(), hash_approximate_median_doc,
        &default_scalar_aggregate_options);
    DCHECK_OK(func->AddKernel(MakeApproximateMedianKernel(tdigest_func)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  HashAggregateFunction* first_last_func = nullptr;
  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_first_last", Arity::Binary(), hash_first_last_doc,
        &default_scalar_aggregate_options);

    DCHECK_OK(
        AddHashAggKernels(NumericTypes(), GroupedFirstLastFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(TemporalTypes(), GroupedFirstLastFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(BaseBinaryTypes(), GroupedFirstLastFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels({boolean(), fixed_size_binary(1)},
                                GroupedFirstLastFactory::Make, func.get()));

    first_last_func = func.get();
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_first", Arity::Binary(), hash_first_doc, &default_scalar_aggregate_options);
    DCHECK_OK(
        func->AddKernel(MakeFirstOrLastKernel<FirstOrLast::First>(first_last_func)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_last", Arity::Binary(), hash_last_doc, &default_scalar_aggregate_options);
    DCHECK_OK(func->AddKernel(MakeFirstOrLastKernel<FirstOrLast::Last>(first_last_func)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  HashAggregateFunction* min_max_func = nullptr;
  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_min_max", Arity::Binary(), hash_min_max_doc,
        &default_scalar_aggregate_options);
    DCHECK_OK(AddHashAggKernels(NumericTypes(), GroupedMinMaxFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels(TemporalTypes(), GroupedMinMaxFactory::Make, func.get()));
    DCHECK_OK(
        AddHashAggKernels(BaseBinaryTypes(), GroupedMinMaxFactory::Make, func.get()));
    // Type parameters are ignored
    DCHECK_OK(AddHashAggKernels({null(), boolean(), decimal128(1, 1), decimal256(1, 1),
                                 month_interval(), fixed_size_binary(1)},
                                GroupedMinMaxFactory::Make, func.get()));
    min_max_func = func.get();
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_min", Arity::Binary(), hash_min_or_max_doc,
        &default_scalar_aggregate_options);
    DCHECK_OK(func->AddKernel(MakeMinOrMaxKernel<MinOrMax::Min>(min_max_func)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_max", Arity::Binary(), hash_min_or_max_doc,
        &default_scalar_aggregate_options);
    DCHECK_OK(func->AddKernel(MakeMinOrMaxKernel<MinOrMax::Max>(min_max_func)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_any", Arity::Binary(), hash_any_doc, &default_scalar_aggregate_options);
    DCHECK_OK(func->AddKernel(MakeKernel(boolean(), HashAggregateInit<GroupedAnyImpl>)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_all", Arity::Binary(), hash_all_doc, &default_scalar_aggregate_options);
    DCHECK_OK(func->AddKernel(MakeKernel(boolean(), HashAggregateInit<GroupedAllImpl>)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_count_distinct", Arity::Binary(), hash_count_distinct_doc,
        &default_count_options);
    DCHECK_OK(func->AddKernel(
        MakeKernel(InputType::Any(), GroupedDistinctInit<GroupedCountDistinctImpl>)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>(
        "hash_distinct", Arity::Binary(), hash_distinct_doc, &default_count_options);
    DCHECK_OK(func->AddKernel(
        MakeKernel(InputType::Any(), GroupedDistinctInit<GroupedDistinctImpl>)));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>("hash_one", Arity::Binary(),
                                                        hash_one_doc);
    DCHECK_OK(AddHashAggKernels(NumericTypes(), GroupedOneFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels(TemporalTypes(), GroupedOneFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels(BaseBinaryTypes(), GroupedOneFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels({null(), boolean(), decimal128(1, 1), decimal256(1, 1),
                                 month_interval(), fixed_size_binary(1)},
                                GroupedOneFactory::Make, func.get()));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>("hash_list", Arity::Binary(),
                                                        hash_list_doc);
    DCHECK_OK(AddHashAggKernels(NumericTypes(), GroupedListFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels(TemporalTypes(), GroupedListFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels(BaseBinaryTypes(), GroupedListFactory::Make, func.get()));
    DCHECK_OK(AddHashAggKernels({null(), boolean(), decimal128(1, 1), decimal256(1, 1),
                                 month_interval(), fixed_size_binary(1)},
                                GroupedListFactory::Make, func.get()));
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }

  {
    auto func = std::make_shared<HashAggregateFunction>("hash_pivot_wider",
                                                        Arity::Ternary(), hash_pivot_doc);
    for (auto key_type : BaseBinaryTypes()) {
      // Anything that scatter() (i.e. take()) accepts can be passed as values
      auto sig = KernelSignature::Make(
          {key_type->id(), InputType::Any(), InputType(Type::UINT32)},
          OutputType(ResolveGroupOutputType));
      DCHECK_OK(func->AddKernel(
          MakeKernel(std::move(sig), HashAggregateInit<GroupedPivotImpl>)));
    }
    DCHECK_OK(registry->AddFunction(std::move(func)));
  }
}

}  // namespace internal
}  // namespace compute
}  // namespace arrow
