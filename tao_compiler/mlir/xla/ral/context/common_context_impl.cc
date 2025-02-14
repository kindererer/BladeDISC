// Copyright 2021 The BladeDISC Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#include "tensorflow/compiler/mlir/xla/ral/context/common_context_impl.h"

#include <fcntl.h>
#include <omp.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "absl/strings/str_split.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/text_format.h"
#if defined(TAO_CPU_ONLY) && defined(PLATFORM_ALIBABA)
#include "patine/client/interface.h"
#endif
#include "tensorflow/compiler/mlir/xla/ral/context/context_util.h"
#include "tensorflow/compiler/mlir/xla/ral/device/cpu/cpu_driver.h"
#include "tensorflow/compiler/mlir/xla/ral/ral_base.h"
#include "tensorflow/compiler/mlir/xla/ral/ral_helper.h"

// If we're on gcc 4.8 or older, there's a known bug that prevents the use of
// intrinsics when the architecture is not defined in the flags. See
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=57202
#if !defined(__SSE3__) && !defined(__clang__) && \
    (defined(__GNUC__) && (__GNUC__ < 4) ||      \
     ((__GNUC__ == 4) && (__GNUC_MINOR__ < 9)))
#define GCC_WITHOUT_INTRINSICS
#endif

#if !defined(GCC_WITHOUT_INTRINSICS)
#define X86_DENORM_USE_INTRINSICS
#endif

#ifdef X86_DENORM_USE_INTRINSICS
#include <pmmintrin.h>
#endif

namespace tao {
namespace ral {

using cpu::CpuLaunchDims;

namespace {

bool initDebugMode() {
  const char* env = getenv("DISC_DEBUG");
  return env != nullptr && std::string(env) == "true";
}

bool initEnableGlobalConstantStore() {
  const char* env = getenv("DISC_USE_GLOBAL_CONST_STORE");
  if (!env) return false;
  std::string envStr = env;
  std::transform(envStr.begin(), envStr.end(), envStr.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return envStr == "true" || envStr == "1";
}

bool initProfileMode() {
  const char* env = getenv("DISC_CPU_ENABLE_PROFILE");
  if (!env) return false;
  std::string envStr = env;
  std::transform(envStr.begin(), envStr.end(), envStr.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return envStr == "true" || envStr == "1";
}

bool isProfilingEnabled() {
  static bool enabled = initProfileMode();
  return enabled;
}

bool initFlushDenormalMode() {
  const char* env = getenv("DISC_CPU_ENABLE_FLUSH_DENORMAL");
  if (!env) return false;
  std::string envStr = env;
  std::transform(envStr.begin(), envStr.end(), envStr.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return envStr == "true" || envStr == "1";
}

bool isFlushDenormalEnabled() {
  static bool enabled = initFlushDenormalMode();
  return enabled;
}

bool setDenormalState(bool on) {
  if (!isFlushDenormalEnabled()) return false;
#ifdef X86_DENORM_USE_INTRINSICS
  // Restore flags
  _MM_SET_FLUSH_ZERO_MODE(on ? _MM_FLUSH_ZERO_ON : _MM_FLUSH_ZERO_OFF);
  _MM_SET_DENORMALS_ZERO_MODE(on ? _MM_DENORMALS_ZERO_ON
                                 : _MM_DENORMALS_ZERO_OFF);
  return true;
#endif

  // Setting denormal handling to the provided state is not supported.
  return false;
}

bool ensureDenormalState(bool on) {
  static thread_local bool status = setDenormalState(on);
  return status;
}

struct CpuTimer {
  CpuTimer(const char* message) : message(message) {
    if (isProfilingEnabled()) {
      start = std::chrono::steady_clock::now();
    }
  }

  ~CpuTimer() { Stop(); }

  void Stop() {
    if (stopped) return;
    stopped = true;
    if (isProfilingEnabled()) {
      finish = std::chrono::steady_clock::now();
      nanoseconds =
          std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start)
              .count();
      TAO_VLOG(0) << "[[DISC]] launch " << message << " elapsed : "
                  << std::chrono::duration_cast<std::chrono::microseconds>(
                         finish - start)
                         .count()
                  << " us";
    }
  }

  size_t GetNanoSeconds() { return nanoseconds; }

  bool stopped = false;
  const char* message;
  std::chrono::steady_clock::time_point start;
  std::chrono::steady_clock::time_point finish;
  size_t nanoseconds = 0;
};

}  // namespace

bool isDebugMode() {
  static bool debug_mode = initDebugMode();
  return debug_mode;
}

bool discEnableGlobalConstantStore() {
  static bool enabled = initEnableGlobalConstantStore();
  return enabled;
}

ConstStoreRegistrar& ConstStoreRegistrar::Instance() {
  static ConstStoreRegistrar instance;
  return instance;
}

bool ConstStoreRegistrar::unregisterConstStore(ProcessLevelConstStore* store) {
  std::lock_guard<std::mutex> l(mu);
  auto it = referenceCounter.find(store);
  assert(it != referenceCounter.end());
  if (--it->second > 0) {
    return false;
  }
  pbFile2Instance.erase(store->pb_file_path);
  referenceCounter.erase(it);
  return true;
}

ProcessLevelConstStore* ConstStoreRegistrar::getConstStore(
    std::string pb_file_path) {
  std::lock_guard<std::mutex> l(mu);
  auto it = pbFile2Instance.find(pb_file_path);
  if (it == pbFile2Instance.end()) {
    ProcessLevelConstStore* const_store = new ProcessLevelConstStore;
    const_store->pb_file_path = pb_file_path;
    parseMetadataPb(pb_file_path, &(const_store->state.metadata_proto));
    it =
        pbFile2Instance.insert(std::make_pair(pb_file_path, const_store)).first;
  }
  ++referenceCounter[it->second];
  return it->second;
}

#ifdef TAO_CPU_ONLY
void RalGlobalConstantState::onContextFinish(Context* ctx) /* override */ {
  if (process_level_store) {
    bool owned = ConstStoreRegistrar::Instance().unregisterConstStore(
        process_level_store);
    if (!owned) return;
    auto cpu_driver =
        static_cast<cpu::CPUDriver*>(ctx->getDriver(cpu::CPUDriver::name()));
    for (auto& e : process_level_store->state.host_constants) {
      cpu_driver->raw_dealloc(ctx, e.second.first);
    }
    delete process_level_store;
    return;
  }
  // Skip if not process level const store since the context will free these
  // const buffer correctly.
}
#endif

const char* kRalGlobalConstantState = "ral_global_constant_state";

const std::map<std::pair<int, int>, size_t> c_CC_INDEX_MAP{
    // compute_60, reserved for fallback
    {{7, 0}, 1},  // sm_70
    {{7, 5}, 2},  // sm_75
    {{8, 0}, 3},  // sm_80
    {{8, 6}, 4}   // sm_86
};

int parseMetadataPb(const std::string& pb_file_path,
                    mlir::MetadataProto* proto) {
  int fileDescriptor = open(pb_file_path.c_str(), O_RDONLY);
  if (fileDescriptor < 0) {
    TAO_VLOG(0) << "Error file not found: " << pb_file_path;
    return 1;
  }
#ifdef RAL_NEED_PROTO3
  google::protobuf3::io::FileInputStream fileInput(fileDescriptor);
  fileInput.SetCloseOnDelete(true);
  if (!google::protobuf3::TextFormat::Parse(&fileInput, proto)) {
    TAO_VLOG(0) << "Error: parse prototxt failed: " << pb_file_path;
    return 1;
  }

#else
  google::protobuf::io::FileInputStream fileInput(fileDescriptor);
  fileInput.SetCloseOnDelete(true);
  if (!google::protobuf::TextFormat::Parse(&fileInput, proto)) {
    TAO_VLOG(0) << "Error: parse prototxt failed: " << pb_file_path;
    return 1;
  }
#endif
  return 0;
}

buffer_shape_t GetShapeFromConstUniqueName(ExecutionContext* ctx,
                                           const std::string& unique_name) {
  std::vector<std::string> splitted = absl::StrSplit(unique_name, '_');
  if (splitted.size() != 3) {
    ctx->signalError(Context::FAILURE,
                     "unexpected device const unique_name format");
  }
  buffer_shape_t shape;
  if (!splitted[2].empty()) {
    std::vector<std::string> dims_splitted = absl::StrSplit(splitted[2], 'x');
    for (auto& dim : dims_splitted) {
      shape.emplace_back(std::stoi(dim));
    }
  }
  return shape;
}

/// Interpret the given character \p C as a hexadecimal digit and return its
/// value.
///
/// If \p C is not a valid hex digit, -1U is returned.
inline unsigned hexDigitValue(char C) {
  if (C >= '0' && C <= '9')
    return C - '0';
  else if (C >= 'a' && C <= 'f')
    return C - 'a' + 10U;
  else if (C >= 'A' && C <= 'F')
    return C - 'A' + 10U;
  else
    return -1U;
}

inline uint8_t hexFromNibbles(char MSB, char LSB) {
  unsigned U1 = hexDigitValue(MSB);
  unsigned U2 = hexDigitValue(LSB);
  assert(U1 != -1U && U2 != -1U);

  return static_cast<uint8_t>((U1 << 4) | U2);
}

/// Convert hexadecimal string \p Input to its binary representation.
/// The return string is half the size of \p Input.
std::vector<uint8_t> fromHex(const std::string& Input) {
  std::vector<uint8_t> ret;
  size_t src_idx = 0;
  if (Input.size() % 2 == 1) {
    ret.push_back(hexFromNibbles('0', Input.front()));
    src_idx += 1;
  }
  while (src_idx < Input.size()) {
    assert(src_idx + 1 < Input.size());
    ret.push_back(hexFromNibbles(Input[src_idx], Input[src_idx + 1]));
    src_idx += 2;
  }
  return ret;
}

inline buffer_t ral_base_cuda_const_host_internal(ExecutionContext* ctx,
                                                  const char* unique_name,
                                                  buffer_shape_t& shape) {
  auto* state =
      ctx->getResource<RalGlobalConstantState>(kRalGlobalConstantState);
  // if process-level const store is enabled, use it instead of the context
  // level store.
  bool use_process_store = false;
  if (state->process_level_store) {
    state = &(state->process_level_store->state);
    use_process_store = true;
  }

  buffer_t ptr = nullptr;
  {
    std::lock_guard<std::recursive_mutex> lock(state->mu);
    std::string key(unique_name);
    auto it = state->host_constants.find(key);
    if (it == state->host_constants.end()) {
      buffer_shape_t dim_sizes = GetShapeFromConstUniqueName(ctx, unique_name);
      // alloc, get value from metadata file
      const auto& constants = state->metadata_proto.host_global_constants();
      if (constants.find(key) == constants.end()) {
        std::string msg =
            "const unique_name " + key + "not found in metadata file";
        ctx->signalError(Context::FAILURE, msg);
      }
      std::string hex_str = constants.at(key);
      TAO_VLOG(2) << "fromHex start:";
      auto data = fromHex(hex_str);
      auto bytes = data.size();
      auto cpu_driver = ctx->getDriver<cpu::CPUDriver>(cpu::CPUDriver::name());
      buffer_t data_ptr = use_process_store
                              ? cpu_driver->raw_alloc(ctx->getContext(), bytes)
                              : cpu_driver->alloc_persistent(ctx, bytes);
      std::memcpy(data_ptr, data.data(), bytes);

      TAO_VLOG(2) << "data.size: " << bytes;
      state->metadata_proto.mutable_host_global_constants()->erase(key);

      it = state->host_constants
               .insert(std::make_pair(key, std::make_pair(data_ptr, dim_sizes)))
               .first;
    }
    ptr = it->second.first;
    shape = it->second.second;
  }

  return ptr;
}

template <typename T, int N>
MemRefType<T, N> ral_base_cuda_const_host(ExecutionContext* ctx,
                                          void* stream_handle,
                                          const char* unique_name) {
  buffer_shape_t shape;
  buffer_t ptr = ral_base_cuda_const_host_internal(ctx, unique_name, shape);
  if (shape.size() != N) {
    ctx->signalError(Context::FAILURE,
                     "Error: unexpected shape string in unique_name");
  }

  return assignMemRef<T, N>(ptr, shape);
}

template <typename T>
MemRefType<T, 0> ral_base_cuda_const_host_0d(ExecutionContext* ctx,
                                             void* stream_handle,
                                             const char* unique_name) {
  buffer_shape_t shape;
  buffer_t ptr = ral_base_cuda_const_host_internal(ctx, unique_name, shape);
  if (shape.size() != 0) {
    ctx->signalError(Context::FAILURE,
                     "Error: unexpected shape string in unique_name");
  }

  return assignMemRef_0d<T>(ptr);
}

#define RAL_REGISTER_CONST_HOST_FUNC(T, N)                     \
  template MemRefType<T, N> ral_base_cuda_const_host<T, N>(    \
      ExecutionContext * ctx, void*, const char* unique_name); \
  TAO_RAL_API(tao::ral::kRalHostConst, "cpu", ral_base_cuda_const_host<T, N>);

#define RAL_REGISTER_CONST_HOST_FUNC_0D(T)                     \
  template MemRefType<T, 0> ral_base_cuda_const_host_0d<T>(    \
      ExecutionContext * ctx, void*, const char* unique_name); \
  TAO_RAL_API(tao::ral::kRalHostConst, "cpu", ral_base_cuda_const_host_0d<T>);

RAL_REGISTER_CONST_HOST_FUNC_0D(double);
RAL_REGISTER_CONST_HOST_FUNC_0D(float);
RAL_REGISTER_CONST_HOST_FUNC_0D(int32_t);
RAL_REGISTER_CONST_HOST_FUNC_0D(int64_t);
RAL_REGISTER_CONST_HOST_FUNC_0D(bool);
RAL_REGISTER_CONST_HOST_FUNC(double, 1);
RAL_REGISTER_CONST_HOST_FUNC(double, 2);
RAL_REGISTER_CONST_HOST_FUNC(double, 3);
RAL_REGISTER_CONST_HOST_FUNC(double, 4);
RAL_REGISTER_CONST_HOST_FUNC(double, 5);
RAL_REGISTER_CONST_HOST_FUNC(double, 6);
RAL_REGISTER_CONST_HOST_FUNC(float, 1);
RAL_REGISTER_CONST_HOST_FUNC(float, 2);
RAL_REGISTER_CONST_HOST_FUNC(float, 3);
RAL_REGISTER_CONST_HOST_FUNC(float, 4);
RAL_REGISTER_CONST_HOST_FUNC(float, 5);
RAL_REGISTER_CONST_HOST_FUNC(float, 6);
RAL_REGISTER_CONST_HOST_FUNC(int32_t, 1);
RAL_REGISTER_CONST_HOST_FUNC(int32_t, 2);
RAL_REGISTER_CONST_HOST_FUNC(int32_t, 3);
RAL_REGISTER_CONST_HOST_FUNC(int32_t, 4);
RAL_REGISTER_CONST_HOST_FUNC(int32_t, 5);
RAL_REGISTER_CONST_HOST_FUNC(int32_t, 6);
RAL_REGISTER_CONST_HOST_FUNC(int64_t, 1);
RAL_REGISTER_CONST_HOST_FUNC(int64_t, 2);
RAL_REGISTER_CONST_HOST_FUNC(int64_t, 3);
RAL_REGISTER_CONST_HOST_FUNC(int64_t, 4);
RAL_REGISTER_CONST_HOST_FUNC(int64_t, 5);
RAL_REGISTER_CONST_HOST_FUNC(int64_t, 6);
RAL_REGISTER_CONST_HOST_FUNC(bool, 1);
RAL_REGISTER_CONST_HOST_FUNC(bool, 2);
RAL_REGISTER_CONST_HOST_FUNC(bool, 3);
RAL_REGISTER_CONST_HOST_FUNC(bool, 4);
RAL_REGISTER_CONST_HOST_FUNC(bool, 5);
RAL_REGISTER_CONST_HOST_FUNC(bool, 6);

static inline void* ral_aligned_malloc(ExecutionContext* ctx, int64_t size) {
  return aligned_malloc(size);
}

static inline void ral_free(ExecutionContext* ctx, void* p) { free(p); }

TAO_RAL_API("ral_aligned_malloc", "cpu", ral_aligned_malloc);
TAO_RAL_API("ral_free", "cpu", ral_free);

#ifdef TAO_CPU_ONLY
template <typename T, int N>
int64_t GetBatchSize(MemRefType<T, N> memref) {
  int64_t batch = 1;
  for (int64_t i = 0; i < N - 2; ++i) {
    batch *= memref.sizes[i];
  }
  return batch;
}

template <typename T, int N, typename E = float>
void ral_batch_gemm(ExecutionContext* ctx, void* stream_handle,
                    MemRefType<T, N> A, MemRefType<T, N> B, MemRefType<T, N> C,
                    bool tp_a, bool tp_b) {
#ifndef PLATFORM_ALIBABA
  ctx->signalError(Context::FAILURE,
                   "ral_batch_gemm has no implementation yet, which"
                   " is on the road.");
#endif
  CpuTimer timer("ral_cpu_batch_gemm");
  if (isEmptyMemref(A) || isEmptyMemref(B) || isEmptyMemref(C)) {
    ctx->signalError(Context::FAILURE, "ral_batch_gemm input error");
    return;
  }

  // It would be better to use `static_assert` here while we need to support
  // lower gcc version in tao bridge ATM which does not support this well
  assert((N > 2) && "batch gemm requires operands with rank higher than 2");
  int64_t batch_a = GetBatchSize(A);
  int64_t batch_b = GetBatchSize(B);
  int64_t batch_c = GetBatchSize(C);

  if (batch_a != batch_b || batch_a != batch_c) {
    ctx->signalError(Context::FAILURE, "mismatch batch size");
    return;
  }

  char transa = tp_a ? 'T' : 'N';
  char transb = tp_b ? 'T' : 'N';
  long long m = tp_a ? A.sizes[N - 1] : A.sizes[N - 2];
  assert(C.sizes[N - 2] == m);
  long long n = tp_b ? B.sizes[N - 2] : B.sizes[N - 1];
  assert(C.sizes[N - 1] == n);
  long long k = tp_a ? A.sizes[N - 2] : A.sizes[N - 1];
  long long kb = tp_b ? B.sizes[N - 1] : B.sizes[N - 2];
  assert(kb == k);
  const T* pa = A.data;
  const T* pb = B.data;
  T* pc = C.data;
#ifdef PLATFORM_ALIBABA
  patine::client::batch_gemm(pa, pb, pc, batch_a, m, n, k, tp_a, tp_b);
#endif
  timer.Stop();

  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "ral_cpu_batch_gemm:\n"
                << "\tpa = " << pa << "\n"
                << "\tpb = " << pb << "\n"
                << "\tpc = " << pc << "\n"
                << "\tbatch = " << batch_a << "\n"
                << "\tm = " << m << "\n"
                << "\tn = " << n << "\n"
                << "\tk = " << k << "\n"
                << "\ttp_a = " << tp_a << "\n"
                << "\ttp_b = " << tp_b << "\n"
                << "\tMath Ops = " << 2 * batch_a * m * n * k << "\n"
                << "\tBytes = " << sizeof(T) * batch_a * (m * n + n * k + m * k)
                << "\n"
                << "\tBandwidth = "
                << double(sizeof(T) * batch_a * (m * n + n * k + m * k)) /
                       double(timer.GetNanoSeconds())
                << " GB\n"
                << "\tGFLOPS = "
                << double(2 * batch_a * m * n * k) /
                       double(timer.GetNanoSeconds())
                << "\n";
  }
}

TAO_RAL_API("ral_gemm", "cpu", ral_batch_gemm<float, 3>);
TAO_RAL_API("ral_gemm", "cpu", ral_batch_gemm<float, 4>);

template <typename T, typename E = float>
void ral_gemm(ExecutionContext* ctx, void* stream_handle, MemRefType<T, 2> A,
              MemRefType<T, 2> B, MemRefType<T, 2> C, bool tp_a, bool tp_b) {
#ifndef PLATFORM_ALIBABA
  ctx->signalError(Context::FAILURE,
                   "ral_batch_gemm has no implementation yet, which"
                   " is on the road.");
#endif
  CpuTimer timer("ral_cpu_gemm");
  long long lda = A.strides[0];
  long long ldb = B.strides[0];
  long long ldc = C.strides[0];
  long long m = tp_a ? A.sizes[1] : A.sizes[0];
  long long n = tp_b ? B.sizes[0] : B.sizes[1];
  long long k = tp_a ? A.sizes[0] : A.sizes[1];
  T* pa = A.data;
  T* pb = B.data;
  T* pc = C.data;
  assert((tp_b ? B.sizes[1] : B.sizes[0]) == k);
  assert(C.sizes[0] == m);
  assert(C.sizes[1] == n);
  char ta = tp_a ? 'T' : 'N';
  char tb = tp_b ? 'T' : 'N';
#ifdef PLATFORM_ALIBABA
  patine::client::gemm(pa, pb, pc, m, n, k, tp_a, tp_b);
#endif
  timer.Stop();

  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "ral_cpu_gemm:\n"
                << "\tpa = " << pa << "\n"
                << "\tpb = " << pb << "\n"
                << "\tpc = " << pc << "\n"
                << "\tm = " << m << "\n"
                << "\tn = " << n << "\n"
                << "\tk = " << k << "\n"
                << "\ttp_a = " << tp_a << "\n"
                << "\ttp_b = " << tp_b << "\n"
                << "\tMath Ops = " << 2 * m * n * k << "\n"
                << "\tBytes = " << sizeof(T) * (m * n + n * k + m * k) << "\n"
                << "\tBandwidth = "
                << double(sizeof(T) * (m * n + n * k + m * k)) /
                       double(timer.GetNanoSeconds())
                << " GB\n"
                << "\tGFLOPS = "
                << double(2 * m * n * k) / double(timer.GetNanoSeconds())
                << "\n";
  }
}

TAO_RAL_API("ral_gemm", "cpu", ral_gemm<float>);

bool layout_match(const std::vector<int32_t>& ref,
                  MemRefType<int32_t, 1> metadata) {
  if (ref.size() > metadata.sizes[0]) {
    return false;
  }
  for (size_t i = 0; i < ref.size(); ++i) {
    if (ref[i] != metadata.data[i]) {
      return false;
    }
  }
  return true;
}

template <typename T, int N>
void ral_conv(ExecutionContext* ctx, void* stream_handle,
              MemRefType<T, N> input, MemRefType<T, N> kernel,
              MemRefType<int32_t, 1> padding, MemRefType<T, N> output,
              MemRefType<int32_t, 1> metadata) {
#ifndef PLATFORM_ALIBABA
  ctx->signalError(Context::FAILURE,
                   "ral_conv has no implementation yet, which"
                   " is on the road.");
#endif
  static_assert(N == 4, "Only conv2d supported for now");
  CpuTimer timer("ral_cpu_conv");
  if (isEmptyMemref(input) || isEmptyMemref(kernel) || isEmptyMemref(output)) {
    TAO_VLOG(1) << "ral_conv: early return for empty tensor";
    return;
  }

  if (TAO_VLOG_IS_ON(2)) {
    print_memref(input, "input");
    print_memref(kernel, "kernel");
    print_memref(padding, "padding");
    for (int i = 0; i < N - 2; ++i) {
      TAO_VLOG(0) << "\tpadding for dim #" << i << ": (" << padding.data[2 * i]
                  << ", " << padding.data[2 * i + 1] << ")";
    }
    print_memref(output, "output");
    print_memref(metadata, "metadata");
    for (int i = 0; i < 5 * N - 4; ++i) {
      TAO_VLOG(0) << "\t#" << i << ": " << metadata.data[i];
    }
  }
  int32_t n = input.sizes[0];
  assert(n == output.sizes[0]);
  T* a_data = input.data;
  T* b_data = kernel.data;
  T* c_data = output.data;
  const std::vector<int32_t> nchw_oihw_layout = {0, 1, 2, 3, 1, 0,
                                                 2, 3, 0, 1, 2, 3};
  const std::vector<int32_t> nhwc_hwio_layout = {0, 3, 1, 2, 2, 3,
                                                 0, 1, 0, 3, 1, 2};
  const char* data_layout;
  const char* kernel_layout;
  int32_t ic = 0;
  int32_t oc = 0;
  int32_t ih = 0;
  int32_t iw = 0;
  int32_t oh = 0;
  int32_t ow = 0;
  int32_t kh = 0;
  int32_t kw = 0;
  int32_t ki = 0;
  int32_t ko = 0;
  if (layout_match(nchw_oihw_layout, metadata)) {
    data_layout = "NCHW";
    kernel_layout = "OIHW";
    ic = input.sizes[1];
    ih = input.sizes[2];
    iw = input.sizes[3];
    ko = kernel.sizes[0];
    ki = kernel.sizes[1];
    kh = kernel.sizes[2];
    kw = kernel.sizes[3];
    oc = output.sizes[1];
    oh = output.sizes[2];
    ow = output.sizes[3];
  } else if (layout_match(nhwc_hwio_layout, metadata)) {
    data_layout = "NHWC";
    kernel_layout = "HWIO";
    ih = input.sizes[1];
    iw = input.sizes[2];
    ic = input.sizes[3];
    kh = kernel.sizes[0];
    kw = kernel.sizes[1];
    ki = kernel.sizes[2];
    ko = kernel.sizes[3];
    oh = output.sizes[1];
    ow = output.sizes[2];
    oc == output.sizes[3];
  } else {
    ctx->signalError(Context::FAILURE, "layout not supported");
    return;
  }
  assert(ko == oc);
  bool is_depthwise = false;
  int32_t groups = 1;
  if (ic != ki) {
    assert(ki == 1);
    is_depthwise = true;
    groups = ic;
  }

  std::vector<int32_t> pad;
  for (int i = 0; i < 4; ++i) {
    pad.push_back(padding.data[i]);
  }
  size_t offset = N * 3;
  int32_t sh = metadata.data[offset++];
  int32_t sw = metadata.data[offset++];
  int32_t dh = metadata.data[offset++];
  int32_t dw = metadata.data[offset++];
#ifdef PLATFORM_ALIBABA
  if (is_depthwise) {
    patine::client::depthwise_conv2d(a_data, b_data, c_data, n, ic, ih, iw, oc,
                                     kh, kw, oh, ow, data_layout, kernel_layout,
                                     pad, sh, sw, dh, dw, false);
  } else {
    patine::client::conv2d(a_data, b_data, c_data, n, ic, ih, iw, oc, kh, kw,
                           oh, ow, data_layout, kernel_layout, groups, pad, sh,
                           sw, dh, dw, false);
  }
#endif
}

TAO_RAL_API("ral_conv", "cpu", ral_conv<float, 4>);

#define DISC_CPU_LAUNCH_LOOP_IMPL(i) \
  for (int64_t iv##i = lowerBound[i]; iv##i < upperBound[i]; iv##i += step[i])

#define DISC_CPU_DEFINE_TYPED_KERNEL_0(...)      \
  using kernelT = void (*)(__VA_ARGS__, void**); \
  auto typedKernel = kernelT(kernel);

#define DISC_CPU_LAUNCH_LOOP_0(...) __VA_ARGS__

#define DISC_CPU_CALL_KERNEL_0(...) typedKernel(__VA_ARGS__, params);

#define DISC_CPU_DEFINE_TYPED_KERNEL(numIVs) \
  DISC_CPU_DEFINE_TYPED_KERNEL_##numIVs()

#define DISC_CPU_LAUNCH_LOOP(numIVs) DISC_CPU_LAUNCH_LOOP_##numIVs()

#define DISC_CPU_CALL_KERNEL(numIVs) DISC_CPU_CALL_KERNEL_##numIVs()

#define DISC_CPU_LOOP_LAUNCH(numIVs)                  \
  void disc_ral_cpu_launch_##numIVs##d(               \
      ExecutionContext* ctx, const char* kernel_name, \
      const std::vector<int64_t>& lowerBound,         \
      const std::vector<int64_t>& upperBound,         \
      const std::vector<int64_t>& step, void* kernel, void** params)

#define DISC_CPU_DEFINE_TYPED_KERNEL_1_HELPER(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_0(int64_t, __VA_ARGS__)
#define DISC_CPU_DEFINE_TYPED_KERNEL_1(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_0(int64_t)
#define DISC_CPU_LAUNCH_LOOP_1(...)   \
  DISC_CPU_LAUNCH_LOOP_0(__VA_ARGS__) \
  DISC_CPU_LAUNCH_LOOP_IMPL(0)
#define DISC_CPU_CALL_KERNEL_1_HELPER(...) \
  DISC_CPU_CALL_KERNEL_0(iv0, __VA_ARGS__)
#define DISC_CPU_CALL_KERNEL_1(...) DISC_CPU_CALL_KERNEL_0(iv0)

#define DISC_CPU_DEFINE_TYPED_KERNEL_2_HELPER(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_1_HELPER(int64_t, __VA_ARGS__)
#define DISC_CPU_DEFINE_TYPED_KERNEL_2(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_1_HELPER(int64_t)
#define DISC_CPU_LAUNCH_LOOP_2(...)   \
  DISC_CPU_LAUNCH_LOOP_1(__VA_ARGS__) \
  DISC_CPU_LAUNCH_LOOP_IMPL(1)
#define DISC_CPU_CALL_KERNEL_2_HELPER(...) \
  DISC_CPU_CALL_KERNEL_1_HELPER(iv1, __VA_ARGS__)
#define DISC_CPU_CALL_KERNEL_2(...) DISC_CPU_CALL_KERNEL_1_HELPER(iv1)

#define DISC_CPU_DEFINE_TYPED_KERNEL_3_HELPER(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_2_HELPER(int64_t, __VA_ARGS__)
#define DISC_CPU_DEFINE_TYPED_KERNEL_3(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_2_HELPER(int64_t)
#define DISC_CPU_LAUNCH_LOOP_3(...)   \
  DISC_CPU_LAUNCH_LOOP_2(__VA_ARGS__) \
  DISC_CPU_LAUNCH_LOOP_IMPL(2)
#define DISC_CPU_CALL_KERNEL_3_HELPER(...) \
  DISC_CPU_CALL_KERNEL_2_HELPER(iv2, __VA_ARGS__)
#define DISC_CPU_CALL_KERNEL_3(...) DISC_CPU_CALL_KERNEL_2_HELPER(iv2)

#define DISC_CPU_DEFINE_TYPED_KERNEL_4_HELPER(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_3_HELPER(int64_t, __VA_ARGS__)
#define DISC_CPU_DEFINE_TYPED_KERNEL_4(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_3_HELPER(int64_t)
#define DISC_CPU_LAUNCH_LOOP_4(...)   \
  DISC_CPU_LAUNCH_LOOP_3(__VA_ARGS__) \
  DISC_CPU_LAUNCH_LOOP_IMPL(3)
#define DISC_CPU_CALL_KERNEL_4_HELPER(...) \
  DISC_CPU_CALL_KERNEL_3_HELPER(iv3, __VA_ARGS__)
#define DISC_CPU_CALL_KERNEL_4(...) DISC_CPU_CALL_KERNEL_3_HELPER(iv3)

#define DISC_CPU_DEFINE_TYPED_KERNEL_5_HELPER(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_4_HELPER(int64_t, __VA_ARGS__)
#define DISC_CPU_DEFINE_TYPED_KERNEL_5(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_4_HELPER(int64_t)
#define DISC_CPU_LAUNCH_LOOP_5(...)   \
  DISC_CPU_LAUNCH_LOOP_4(__VA_ARGS__) \
  DISC_CPU_LAUNCH_LOOP_IMPL(4)
#define DISC_CPU_CALL_KERNEL_5_HELPER(...) \
  DISC_CPU_CALL_KERNEL_4_HELPER(iv4, __VA_ARGS__)
#define DISC_CPU_CALL_KERNEL_5(...) DISC_CPU_CALL_KERNEL_4_HELPER(iv4)

#define DISC_CPU_DEFINE_TYPED_KERNEL_6_HELPER(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_5_HELPER(int64_t, __VA_ARGS__)
#define DISC_CPU_DEFINE_TYPED_KERNEL_6(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_5_HELPER(int64_t)
#define DISC_CPU_LAUNCH_LOOP_6(...)   \
  DISC_CPU_LAUNCH_LOOP_5(__VA_ARGS__) \
  DISC_CPU_LAUNCH_LOOP_IMPL(5)
#define DISC_CPU_CALL_KERNEL_6_HELPER(...) \
  DISC_CPU_CALL_KERNEL_5_HELPER(iv5, __VA_ARGS__)
#define DISC_CPU_CALL_KERNEL_6(...) DISC_CPU_CALL_KERNEL_5_HELPER(iv5)

#define DISC_CPU_DEFINE_TYPED_KERNEL_7_HELPER(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_6_HELPER(int64_t, __VA_ARGS__)
#define DISC_CPU_DEFINE_TYPED_KERNEL_7(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_6_HELPER(int64_t)
#define DISC_CPU_LAUNCH_LOOP_7(...)   \
  DISC_CPU_LAUNCH_LOOP_6(__VA_ARGS__) \
  DISC_CPU_LAUNCH_LOOP_IMPL(6)
#define DISC_CPU_CALL_KERNEL_7_HELPER(...) \
  DISC_CPU_CALL_KERNEL_6_HELPER(iv6, __VA_ARGS__)
#define DISC_CPU_CALL_KERNEL_7(...) DISC_CPU_CALL_KERNEL_6_HELPER(iv6)

#define DISC_CPU_DEFINE_TYPED_KERNEL_8_HELPER(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_7_HELPER(int64_t, __VA_ARGS__)
#define DISC_CPU_DEFINE_TYPED_KERNEL_8(...) \
  DISC_CPU_DEFINE_TYPED_KERNEL_7_HELPER(int64_t)
#define DISC_CPU_LAUNCH_LOOP_8(...)   \
  DISC_CPU_LAUNCH_LOOP_7(__VA_ARGS__) \
  DISC_CPU_LAUNCH_LOOP_IMPL(7)
#define DISC_CPU_CALL_KERNEL_8_HELPER(...) \
  DISC_CPU_CALL_KERNEL_7_HELPER(iv7, __VA_ARGS__)
#define DISC_CPU_CALL_KERNEL_8(...) DISC_CPU_CALL_KERNEL_7_HELPER(iv7)

DISC_CPU_LOOP_LAUNCH(1) {
  DISC_CPU_DEFINE_TYPED_KERNEL(1)
  DISC_CPU_LAUNCH_LOOP(1) { DISC_CPU_CALL_KERNEL(1) }
}

DISC_CPU_LOOP_LAUNCH(2) {
  DISC_CPU_DEFINE_TYPED_KERNEL(2)
  DISC_CPU_LAUNCH_LOOP(2) { DISC_CPU_CALL_KERNEL(2) }
}

DISC_CPU_LOOP_LAUNCH(3) {
  DISC_CPU_DEFINE_TYPED_KERNEL(3)
  DISC_CPU_LAUNCH_LOOP(3) { DISC_CPU_CALL_KERNEL(3) }
}

DISC_CPU_LOOP_LAUNCH(4) {
  DISC_CPU_DEFINE_TYPED_KERNEL(4)
  DISC_CPU_LAUNCH_LOOP(4) { DISC_CPU_CALL_KERNEL(4) }
}

DISC_CPU_LOOP_LAUNCH(5) {
  DISC_CPU_DEFINE_TYPED_KERNEL(5)
  DISC_CPU_LAUNCH_LOOP(5) { DISC_CPU_CALL_KERNEL(5) }
}

DISC_CPU_LOOP_LAUNCH(6) {
  DISC_CPU_DEFINE_TYPED_KERNEL(6)
  DISC_CPU_LAUNCH_LOOP(6) { DISC_CPU_CALL_KERNEL(6) }
}

DISC_CPU_LOOP_LAUNCH(7) {
  DISC_CPU_DEFINE_TYPED_KERNEL(7)
  DISC_CPU_LAUNCH_LOOP(7) { DISC_CPU_CALL_KERNEL(7) }
}

DISC_CPU_LOOP_LAUNCH(8) {
  DISC_CPU_DEFINE_TYPED_KERNEL(8)
  DISC_CPU_LAUNCH_LOOP(8) { DISC_CPU_CALL_KERNEL(8) }
}

struct LoopIndex {
  LoopIndex(std::vector<int64_t> multiIndices,
            const std::vector<int64_t>& dimSizes)
      : multiIndices_(std::move(multiIndices)), dimSizes_(dimSizes) {}

  int getRank() const { return getMultiIndices().size(); }

  int64_t& getIndex(int dim) { return multiIndices_[dim]; }

  const int64_t& getIndex(int dim) const { return multiIndices_[dim]; }

  std::vector<int64_t>& getMultiIndices() { return multiIndices_; }

  const std::vector<int64_t>& getMultiIndices() const { return multiIndices_; }

  const std::vector<int64_t>& getDimSizes() const { return dimSizes_; }

  void minusOneFromDim(int startDim) {
    for (; startDim >= 0; --startDim) {
      if (--multiIndices_[startDim] >= 0) {
        break;
      }
      multiIndices_[startDim] = dimSizes_[startDim];
    }
  }

 private:
  int64_t linearIndex_ = 0;
  std::vector<int64_t> multiIndices_;
  const std::vector<int64_t>& dimSizes_;
};

struct LoopTask {
  LoopTask(int numIVs, int64_t* lowerBound, int64_t* upperBound, int64_t* step)
      : numIVs_(numIVs),
        lowerBound(lowerBound, lowerBound + numIVs),
        upperBound(upperBound, upperBound + numIVs),
        step(step, step + numIVs) {}

  int64_t getNumUnits() {
    ensureInitialized();
    return totalNum_;
  }

  const std::vector<int64_t>& getDimSizes() {
    ensureInitialized();
    return dimSizes_;
  }

  LoopTask makeSubTaskFromRange(const LoopIndex& indices, int lowest_dim,
                                int64_t from, int64_t to) const {
    LoopTask subTask = *this;
    subTask.initialized = false;
    for (int i = 0; i < lowest_dim; ++i) {
      subTask.lowerBound[i] = lowerBound[i] + step[i] * indices.getIndex(i);
      subTask.upperBound[i] =
          lowerBound[i] + step[i] * (1 + indices.getIndex(i));
    }
    subTask.lowerBound[lowest_dim] =
        lowerBound[lowest_dim] + step[lowest_dim] * from;
    subTask.upperBound[lowest_dim] =
        lowerBound[lowest_dim] + step[lowest_dim] * to;
    for (int i = lowest_dim + 1; i < numIVs_; ++i) {
      subTask.lowerBound[i] = lowerBound[i];
      subTask.upperBound[i] = upperBound[i];
    }
    subTask.ensureInitialized();
    return subTask;
  }

  std::vector<int64_t> lowerBound;
  std::vector<int64_t> upperBound;
  std::vector<int64_t> step;

 private:
  bool initialized = false;
  int numIVs_ = 0;
  int64_t totalNum_ = 0;
  std::vector<int64_t> dimSizes_;

  void ensureInitialized() {
    if (initialized) return;
    initialized = true;
    if (numIVs_ < 1) return;

    totalNum_ = 1;
    dimSizes_.reserve(numIVs_);
    for (int i = 0; i < numIVs_; ++i) {
      dimSizes_.push_back(
          ((upperBound[i] - lowerBound[i] + step[i] - 1) / step[i]));
      totalNum_ *= dimSizes_.back();
    }
  }
};

struct LoopPartition {
  std::vector<LoopTask> tasks;
};

struct LoopPartitionPlan {
  std::vector<LoopPartition> partitions;
};

int get_OMP_NUM_THREADS() {
  int num_workers = 1;
  if (const char* num_cores = std::getenv("OMP_NUM_THREADS")) {
    int n = std::atoi(num_cores);
    if (n > 0) {
      num_workers = n;
    }
  }
  return num_workers;
}

int getNumAvailableCores() {
  static int numCores = get_OMP_NUM_THREADS();
  return numCores;
}

int get_DISC_MIN_NUM_UNIT_PER_CORE() {
  int min_num_unit = 1;
  if (const char* num_units = std::getenv("DISC_MIN_NUM_UNIT_PER_CORE")) {
    int n = std::atoi(num_units);
    if (n > 0) {
      min_num_unit = n;
    }
  }
  return min_num_unit;
}

int getMinNumUnitPerCore() {
  static int min_num_unit = get_DISC_MIN_NUM_UNIT_PER_CORE();
  return min_num_unit;
}

LoopPartitionPlan LoopParallelAssigner(CpuLaunchDims lowerBound,
                                       CpuLaunchDims upperBound,
                                       CpuLaunchDims step,
                                       int64_t unitWorkloadSizeHint,
                                       int64_t numAvailableCores) {
  LoopTask totalTask{static_cast<int>(lowerBound.sizes[0]), lowerBound.data,
                     upperBound.data, step.data};
  int64_t numUnits = totalTask.getNumUnits();
  const int64_t minUnitWorksPerCore = getMinNumUnitPerCore();
  int numCores =
      std::min(std::min(numAvailableCores, numUnits),
               (numUnits * unitWorkloadSizeHint) / minUnitWorksPerCore);
  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "\tnumAvailableCores: " << numAvailableCores << "\n"
                << "\tunitWorkloadSizeHint: " << unitWorkloadSizeHint << "\n"
                << "\tnumUnits: " << numUnits << "\n"
                << "\tnumCores: " << numCores;
    TAO_VLOG(0) << "\tnumIVs: " << totalTask.lowerBound.size() << ":";
    for (size_t i = 0; i < totalTask.lowerBound.size(); ++i) {
      TAO_VLOG(0) << "\t iv #" << i << " (lower, upper, step, dimSize) = ("
                  << totalTask.lowerBound[i] << ", " << totalTask.upperBound[i]
                  << ", " << totalTask.step[i] << ", "
                  << totalTask.getDimSizes()[i] << ")";
    }
  }

  LoopPartitionPlan plan;
  if (numCores < 2) {
    LoopPartition partition;
    partition.tasks.emplace_back(std::move(totalTask));
    plan.partitions.emplace_back(std::move(partition));
    return plan;
  }

  int rank = lowerBound.sizes[0];
  plan.partitions.resize(numCores);
  auto& dimSizes = totalTask.getDimSizes();
  LoopIndex indices(dimSizes, dimSizes);
  int unassigned_dim = 0;
  int64_t unassigned_units = 1;
  for (; unassigned_dim < rank && unassigned_units; ++unassigned_dim) {
    unassigned_units *= dimSizes[unassigned_dim];
    if (unassigned_units < numCores) {
      indices.minusOneFromDim(unassigned_dim);
      continue;
    }
    int64_t num_unit_per_core = unassigned_units / numCores;
    unassigned_units %= numCores;

    auto& idx = indices.getIndex(unassigned_dim);
    for (int coreIdx = 0; coreIdx < numCores; ++coreIdx) {
      auto& partition = plan.partitions[coreIdx];
      if (idx >= num_unit_per_core) {
        int64_t from = idx - num_unit_per_core;
        partition.tasks.emplace_back(
            totalTask.makeSubTaskFromRange(indices, unassigned_dim, from, idx));
        idx = from;
      } else {
        int64_t left = num_unit_per_core - idx;
        if (idx > 0) {
          partition.tasks.emplace_back(
              totalTask.makeSubTaskFromRange(indices, unassigned_dim, 0, idx));
        }
        idx = 0;
        indices.minusOneFromDim(unassigned_dim);
        int64_t from = idx - left;
        partition.tasks.emplace_back(
            totalTask.makeSubTaskFromRange(indices, unassigned_dim, from, idx));
        idx = from;
      }
    }
    if (unassigned_units > 0) {
      indices.minusOneFromDim(unassigned_dim);
    }
  }

  while (unassigned_units > 0) {
    auto& partition = plan.partitions[unassigned_units];
    auto& idx = indices.getIndex(unassigned_dim);
    partition.tasks.emplace_back(totalTask.makeSubTaskFromRange(
        indices, rank - 1, unassigned_units - 1, unassigned_units));
    --unassigned_units;
  }
  return plan;
}

void taskRunner(ExecutionContext* ctx, const char* kernel_name,
                const std::vector<int64_t>& lowerBound,
                const std::vector<int64_t>& upperBound,
                const std::vector<int64_t>& step, void* kernel, void** params) {
  using kernel_type =
      void (*)(const int64_t*, const int64_t*, const int64_t*, void**);
  auto typed_kernel = (kernel_type)(kernel);
  typed_kernel(lowerBound.data(), upperBound.data(), step.data(), params);
  return;
#define CALL_LAUNCH(N)                                                       \
  disc_ral_cpu_launch_##N##d(ctx, kernel_name, lowerBound, upperBound, step, \
                             kernel, params)

  switch (lowerBound.size()) {
    case 1:
      CALL_LAUNCH(1);
      break;
    case 2:
      CALL_LAUNCH(2);
      break;
    case 3:
      CALL_LAUNCH(3);
      break;
    case 4:
      CALL_LAUNCH(4);
      break;
    case 5:
      CALL_LAUNCH(5);
      break;
    case 6:
      CALL_LAUNCH(6);
      break;
    case 7:
      CALL_LAUNCH(7);
      break;
    case 8:
      CALL_LAUNCH(8);
      break;
    default:
      ctx->signalError(Context::FAILURE, "not supported ivs num");
  }
#undef CALL_LAUNCH
}

void partitionRunner(ExecutionContext* ctx, const char* kernel_name,
                     const LoopPartition& partition, void* kernel,
                     void** params) {
  for (const auto& task : partition.tasks) {
    taskRunner(ctx, kernel_name, task.lowerBound, task.upperBound, task.step,
               kernel, params);
  }
}

// cpu kernel launch
void ompLaunchKernel(ExecutionContext* ctx, const char* kernel_name,
                     CpuLaunchDims lowerBound, CpuLaunchDims upperBound,
                     CpuLaunchDims step, int64_t unitWorkloadSizeHint,
                     void* kernel, void** params /* kernel params */) {
  int numIVs = lowerBound.sizes[0];
  TAO_VLOG(1) << "ompLaunchKernel: " << kernel_name << "@" << numIVs;
  ensureDenormalState(true);
  CpuTimer timer(kernel_name);
  if (!numIVs) {
    return;
  }

  // Basic idea:
  // 1, estimate #num core needed
  //    numCore = estimate(...)
  // 2, partition the ivs
  //    partitions = partition(lowerBound, upperBound, step);
  // 3, parallel launch.

  auto plan =
      LoopParallelAssigner(lowerBound, upperBound, step, unitWorkloadSizeHint,
                           getNumAvailableCores());
  if (TAO_VLOG_IS_ON(1)) {
    TAO_VLOG(0) << "loop partition plan w/ " << plan.partitions.size()
                << " partitions";
    for (size_t i = 0; i < plan.partitions.size(); ++i) {
      TAO_VLOG(0) << " partition #" << i << ":";
      for (size_t taskId = 0; taskId < plan.partitions[i].tasks.size();
           ++taskId) {
        auto& task = plan.partitions[i].tasks[taskId];
        TAO_VLOG(0) << "  task@" << taskId << " w/ " << task.lowerBound.size()
                    << " ivs: ";
        for (size_t ivId = 0; ivId < task.lowerBound.size(); ++ivId) {
          TAO_VLOG(0) << "   iv #" << ivId
                      << " (lower, upper, step, dimSize) = ("
                      << task.lowerBound[ivId] << ", " << task.upperBound[ivId]
                      << ", " << task.step[ivId] << ", "
                      << task.getDimSizes()[ivId] << ")";
        }
      }
    }
  }
  if (plan.partitions.size() < 2) {
    TAO_VLOG(2) << "# loop partitions is less than two, use single thread.";
    partitionRunner(ctx, kernel_name, plan.partitions[0], kernel, params);
    return;
  }

#pragma omp parallel num_threads(plan.partitions.size())
  {
    ensureDenormalState(true);
    int idx = omp_get_thread_num();
    TAO_VLOG(1) << "parallel runner #" << idx << " start";
    partitionRunner(ctx, kernel_name, plan.partitions[idx], kernel, params);
    TAO_VLOG(1) << "parallel runner #" << idx << " finish";
  }
  TAO_VLOG(1) << "parallel runner finish";
}

TAO_RAL_API(cpu::kRalCpuLaunch, "cpu", ompLaunchKernel);

#endif  // TAO_CPU_ONLY
}  // namespace ral
}  // namespace tao

#ifdef TAO_RAL_USE_STREAM_EXECUTOR
#include "tensorflow/compiler/mlir/xla/ral/context/stream_executor_based_impl.h"
namespace tao {
namespace ral {
RAL_REGISTER_CONST_HOST_FUNC_0D(Eigen::half);
RAL_REGISTER_CONST_HOST_FUNC(Eigen::half, 1);
RAL_REGISTER_CONST_HOST_FUNC(Eigen::half, 2);
RAL_REGISTER_CONST_HOST_FUNC(Eigen::half, 3);
RAL_REGISTER_CONST_HOST_FUNC(Eigen::half, 4);
RAL_REGISTER_CONST_HOST_FUNC(Eigen::half, 5);
RAL_REGISTER_CONST_HOST_FUNC(Eigen::half, 6);
}  // namespace ral
}  // namespace tao
#endif  // TAO_RAL_USE_STREAM_EXECUTOR
