// Cross-Cluster State Fabric - CUDA backend implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf_cuda_backend.hpp"

#include <cuda_runtime.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ccsf::cuda {
namespace {

std::mutex g_device_mutex;
std::size_t g_live_allocations = 0;

std::string cuda_error_text(const char* what, cudaError_t status) {
  return std::string(what) + " failed: " + cudaGetErrorString(status);
}

/// Deterministic FP32 kernel: y[i] = sum_k x[i * columns + k] * w[k].
__global__ void gemv_kernel(const float* x, const float* w, float* y, unsigned long long rows,
                            unsigned long long columns) {
  const unsigned long long row = blockIdx.x * static_cast<unsigned long long>(blockDim.x) +
                                 threadIdx.x;
  if (row >= rows) {
    return;
  }
  float accumulator = 0.0f;
  for (unsigned long long column = 0; column < columns; ++column) {
    accumulator += x[row * columns + column] * w[column];
  }
  y[row] = accumulator;
}

std::vector<float> build_inputs(std::uint64_t rows, std::uint64_t columns, std::uint64_t seed) {
  std::vector<float> data(static_cast<std::size_t>(rows * columns + columns));
  std::uint64_t state = seed == 0 ? 0x2545F4914F6CDD1DULL : seed;
  auto next = [&state]() {
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
  };
  for (float& value : data) {
    const std::uint32_t raw = static_cast<std::uint32_t>(next() % 2000U);
    value = (static_cast<float>(raw) - 1000.0f) / 1000.0f;
  }
  return data;
}

}  // namespace

Result<DeviceInfo> device_info() {
  int count = 0;
  const cudaError_t counted = cudaGetDeviceCount(&count);
  if (counted != cudaSuccess) {
    return make_error(ErrorCode::NOT_SUPPORTED, cuda_error_text("cudaGetDeviceCount", counted));
  }
  if (count == 0) {
    return make_error(ErrorCode::NOT_SUPPORTED, "no CUDA device is present");
  }
  cudaDeviceProp properties{};
  const cudaError_t described = cudaGetDeviceProperties(&properties, 0);
  if (described != cudaSuccess) {
    return make_error(ErrorCode::NOT_SUPPORTED, cuda_error_text("cudaGetDeviceProperties", described));
  }
  std::size_t free_bytes = 0;
  std::size_t total_bytes = 0;
  const cudaError_t memory = cudaMemGetInfo(&free_bytes, &total_bytes);
  if (memory != cudaSuccess) {
    return make_error(ErrorCode::NOT_SUPPORTED, cuda_error_text("cudaMemGetInfo", memory));
  }
  DeviceInfo info;
  info.name = properties.name;
  info.compute_major = properties.major;
  info.compute_minor = properties.minor;
  info.total_memory_bytes = total_bytes;
  info.free_memory_bytes = free_bytes;
  cudaRuntimeGetVersion(&info.runtime_version);
  cudaDriverGetVersion(&info.driver_version);
  return info;
}

Result<std::size_t> free_memory_bytes() {
  std::size_t free_bytes = 0;
  std::size_t total_bytes = 0;
  const cudaError_t status = cudaMemGetInfo(&free_bytes, &total_bytes);
  if (status != cudaSuccess) {
    return make_error(ErrorCode::NOT_SUPPORTED, cuda_error_text("cudaMemGetInfo", status));
  }
  return free_bytes;
}

std::vector<float> reference_tensor_state(std::uint64_t rows, std::uint64_t columns,
                                          std::uint64_t seed) {
  std::vector<float> data = build_inputs(rows, columns, seed);
  const float* x = data.data();
  const float* weights = data.data() + rows * columns;
  std::vector<float> output(static_cast<std::size_t>(rows), 0.0f);
  for (std::uint64_t row = 0; row < rows; ++row) {
    float accumulator = 0.0f;
    for (std::uint64_t column = 0; column < columns; ++column) {
      accumulator += x[row * columns + column] * weights[column];
    }
    output[static_cast<std::size_t>(row)] = accumulator;
  }
  return output;
}

Result<std::vector<std::byte>> generate_tensor_state(std::uint64_t rows, std::uint64_t columns,
                                                     std::uint64_t seed) {
  if (rows == 0 || columns == 0) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "the tensor shape must be non-empty");
  }
  const std::uint64_t elements = rows * columns + columns;
  const std::size_t input_bytes = static_cast<std::size_t>(elements) * sizeof(float);
  const std::size_t output_bytes = static_cast<std::size_t>(rows) * sizeof(float);
  if (input_bytes > (1ULL << 30U) || output_bytes > (1ULL << 30U)) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, "the tensor shape exceeds the example bound");
  }

  const std::vector<float> host_inputs = build_inputs(rows, columns, seed);
  float* device_inputs = nullptr;
  float* device_weights = nullptr;
  float* device_outputs = nullptr;
  std::lock_guard<std::mutex> guard(g_device_mutex);

  if (cudaMalloc(&device_inputs, input_bytes) != cudaSuccess) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, "cudaMalloc for the tensor input failed");
  }
  ++g_live_allocations;
  if (cudaMalloc(&device_outputs, output_bytes) != cudaSuccess) {
    cudaFree(device_inputs);
    --g_live_allocations;
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, "cudaMalloc for the tensor output failed");
  }
  ++g_live_allocations;
  device_weights = device_inputs + rows * columns;

  cudaError_t status = cudaMemcpy(device_inputs, host_inputs.data(), input_bytes,
                                  cudaMemcpyHostToDevice);
  if (status != cudaSuccess) {
    cudaFree(device_inputs);
    cudaFree(device_outputs);
    g_live_allocations -= 2;
    return make_error(ErrorCode::INTERNAL_ERROR, cuda_error_text("cudaMemcpy (H2D)", status));
  }

  const unsigned int threads = 128;
  const unsigned int blocks =
      static_cast<unsigned int>((rows + threads - 1U) / threads);
  gemv_kernel<<<blocks, threads>>>(device_inputs, device_weights, device_outputs, rows, columns);
  status = cudaGetLastError();
  if (status != cudaSuccess) {
    cudaFree(device_inputs);
    cudaFree(device_outputs);
    g_live_allocations -= 2;
    return make_error(ErrorCode::INTERNAL_ERROR, cuda_error_text("kernel launch", status));
  }
  status = cudaDeviceSynchronize();
  if (status != cudaSuccess) {
    cudaFree(device_inputs);
    cudaFree(device_outputs);
    g_live_allocations -= 2;
    return make_error(ErrorCode::INTERNAL_ERROR, cuda_error_text("cudaDeviceSynchronize", status));
  }

  std::vector<float> host_outputs(static_cast<std::size_t>(rows), 0.0f);
  status = cudaMemcpy(host_outputs.data(), device_outputs, output_bytes,
                      cudaMemcpyDeviceToHost);
  if (status != cudaSuccess) {
    cudaFree(device_inputs);
    cudaFree(device_outputs);
    g_live_allocations -= 2;
    return make_error(ErrorCode::INTERNAL_ERROR, cuda_error_text("cudaMemcpy (D2H)", status));
  }

  cudaFree(device_inputs);
  cudaFree(device_outputs);
  g_live_allocations -= 2;

  std::vector<std::byte> payload(output_bytes);
  std::memcpy(payload.data(), host_outputs.data(), output_bytes);
  return payload;
}

bool tensors_agree(const std::vector<std::byte>& device_bytes,
                   const std::vector<float>& reference, float tolerance) {
  if (device_bytes.size() != reference.size() * sizeof(float)) {
    return false;
  }
  std::vector<float> decoded(reference.size(), 0.0f);
  std::memcpy(decoded.data(), device_bytes.data(), device_bytes.size());
  for (std::size_t index = 0; index < reference.size(); ++index) {
    const float difference = decoded[index] - reference[index];
    const float magnitude = difference < 0.0f ? -difference : difference;
    if (magnitude > tolerance) {
      return false;
    }
  }
  return true;
}

Result<bool> release_all(std::size_t baseline_free_bytes, std::size_t tolerance_bytes) {
  const cudaError_t reset = cudaDeviceSynchronize();
  if (reset != cudaSuccess) {
    return make_error(ErrorCode::INTERNAL_ERROR, cuda_error_text("cudaDeviceSynchronize", reset));
  }
  if (g_live_allocations != 0) {
    return make_error(ErrorCode::INTERNAL_ERROR,
                      "the CUDA example still holds live device allocations");
  }
  auto free_now = free_memory_bytes();
  if (!free_now) {
    return free_now.error();
  }
  const std::size_t free_bytes = free_now.value();
  const std::size_t returned =
      free_bytes >= baseline_free_bytes ? free_bytes - baseline_free_bytes
                                        : baseline_free_bytes - free_bytes;
  return returned <= tolerance_bytes;
}

}  // namespace ccsf::cuda
