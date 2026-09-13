// Cross-Cluster State Fabric - standalone CUDA state proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Runs the real device path outside the test harness: allocate, compute on the
// device, copy back, hash, release, and report device memory accounting.
//
// TOPOLOGY LABEL: device work is REAL; the cross-cluster placement shown by the
// test suite is SYNTHETIC (logical clusters on one host).

#include <cstdio>
#include <vector>

#include "ccsf/bytes.hpp"
#include "ccsf/digest.hpp"
#include "ccsf_cuda_backend.hpp"

int main() {
  using namespace ccsf;
  auto info = cuda::device_info();
  if (!info) {
    std::fprintf(stderr, "no usable CUDA device: %s\n", info.error().to_string().c_str());
    return 2;
  }
  std::printf("device: %s sm_%d%d total=%zu MiB free=%zu MiB runtime=%d driver=%d\n",
              info.value().name.c_str(), info.value().compute_major, info.value().compute_minor,
              info.value().total_memory_bytes >> 20U, info.value().free_memory_bytes >> 20U,
              info.value().runtime_version, info.value().driver_version);
  const std::size_t baseline = info.value().free_memory_bytes;

  constexpr std::uint64_t kRows = 8192;
  constexpr std::uint64_t kColumns = 1024;
  auto payload = cuda::generate_tensor_state(kRows, kColumns, 0xC0FFEEU);
  if (!payload) {
    std::fprintf(stderr, "device state generation failed: %s\n",
                 payload.error().to_string().c_str());
    return 1;
  }
  const Digest digest = sha256(ByteSpan(payload.value().data(), payload.value().size()));
  const std::vector<float> reference = cuda::reference_tensor_state(kRows, kColumns, 0xC0FFEEU);
  const bool parity = cuda::tensors_agree(payload.value(), reference, 1e-2f);
  std::printf("device state: %zu bytes digest=%s cpu_parity=%s\n", payload.value().size(),
              digest.hex().c_str(), parity ? "pass" : "FAIL");

  auto released = cuda::release_all(baseline, 64ULL * 1024ULL * 1024ULL);
  std::printf("device memory restored: %s\n",
              (released && released.value()) ? "yes" : "no");
  if (!parity || !released || !released.value()) {
    return 1;
  }
  return 0;
}
