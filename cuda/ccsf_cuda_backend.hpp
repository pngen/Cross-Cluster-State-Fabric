// Cross-Cluster State Fabric - CUDA-backed state example backend.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This backend produces a real device-resident state payload on a CUDA device
// (cudaMalloc, host-to-device copy, kernel launch, synchronization,
// device-to-host copy) and hands the resulting bytes to the fabric as opaque
// tensor state. It is an example backend: the core runtime never depends on it.

#ifndef CCSF_CUDA_BACKEND_HPP
#define CCSF_CUDA_BACKEND_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/result.hpp"

namespace ccsf::cuda {

struct DeviceInfo {
  std::string name;
  int compute_major{0};
  int compute_minor{0};
  std::size_t total_memory_bytes{0};
  std::size_t free_memory_bytes{0};
  int runtime_version{0};
  int driver_version{0};
};

/// Describes the device the proof will use. Returns an error rather than a
/// fabricated result when no usable device exists.
Result<DeviceInfo> device_info();

/// Device memory currently free, in bytes.
Result<std::size_t> free_memory_bytes();

/// Runs a deterministic FP32 matrix-vector product on the device and returns the
/// device-produced result, bit for bit, as the state payload.
///
///   x: rows x columns row-major input
///   w: columns weights
///   y: rows device outputs
Result<std::vector<std::byte>> generate_tensor_state(std::uint64_t rows,
                                                     std::uint64_t columns,
                                                     std::uint64_t seed);

/// CPU reference for the same computation, used to prove semantic parity.
std::vector<float> reference_tensor_state(std::uint64_t rows, std::uint64_t columns,
                                          std::uint64_t seed);

/// True when every element agrees within the given absolute tolerance.
bool tensors_agree(const std::vector<std::byte>& device_bytes,
                   const std::vector<float>& reference, float tolerance);

/// Releases every device allocation held by this backend and reports whether the
/// device returned to the baseline free memory within the tolerance.
Result<bool> release_all(std::size_t baseline_free_bytes, std::size_t tolerance_bytes);

}  // namespace ccsf::cuda

#endif  // CCSF_CUDA_BACKEND_HPP
