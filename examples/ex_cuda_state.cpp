// Example: CUDA-backed state.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// REAL: the device work (allocation, kernel, synchronization, copy back) runs
// on an actual CUDA device.
// SYNTHETIC: the two clusters are logical clusters on one physical host; no
// cross-machine GPU transfer is claimed.

#if defined(CCSF_EXAMPLE_CUDA)

#include <vector>

#include "common.hpp"
#include "ccsf_cuda_backend.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  banner("CUDA-backed state");

  auto info = cuda::device_info();
  if (!info) {
    fail(info.error().to_string());
    return 1;
  }
  line("device: " + info.value().name + " sm_" + std::to_string(info.value().compute_major) +
       std::to_string(info.value().compute_minor));
  const std::size_t baseline = info.value().free_memory_bytes;

  constexpr std::uint64_t kRows = 4096;
  constexpr std::uint64_t kColumns = 512;
  auto payload = cuda::generate_tensor_state(kRows, kColumns, 0xA11CEU);
  if (!payload) {
    fail(payload.error().to_string());
    return 1;
  }
  const std::vector<float> reference = cuda::reference_tensor_state(kRows, kColumns, 0xA11CEU);
  line("device produced " + std::to_string(payload.value().size()) + " bytes; cpu parity " +
       (cuda::tensors_agree(payload.value(), reference, 1e-2f) ? "pass" : "FAIL"));

  const Digest digest = sha256(ByteSpan(payload.value().data(), payload.value().size()));
  line("device state digest: " + digest.hex());

  auto example = make_example("CUDA-backed state (fabric half)");
  if (!example) {
    return 1;
  }
  example->state = StateId::from_value(9901);
  StateGenerationSpec spec;
  spec.state_id = example->state;
  spec.generation = example->generation;
  spec.lineage = LineageId::from_value(example->state.value());
  spec.state_class = StateClass::TENSOR_STATE;
  spec.logical_size = SizeValue::of(payload.value().size());
  spec.content_digest = digest;
  spec.replication_requirements = replication_policy(1, 2).requirements;
  spec.provenance.producer = "cuda-example";
  if (!example->fabric->register_state(spec)) {
    fail("device state registration failed");
    return 1;
  }
  if (!example->fabric->commit_state_generation(example->state, example->generation,
                                                CommitId::from_value(example->take()))) {
    fail("device state commit failed");
    return 1;
  }
  auto source = seed(*example, example->cluster_a);
  if (!source) {
    fail(source.error().to_string());
    return 1;
  }
  auto destination = replicate(*example, example->cluster_b);
  if (!destination) {
    fail(destination.error().to_string());
    return 1;
  }
  auto record = example->fabric->query_replica(destination.value());
  line("destination digest matches the device digest: " +
       std::string(record.value().measured_digest == digest ? "yes" : "no"));
  auto reuse = example->fabric->evaluate_reuse(example->state, example->generation,
                                               example->cluster_b);
  line("destination reuse: " + std::string(to_string(reuse.value().decision)));

  payload.value().clear();
  auto released = cuda::release_all(baseline, 64ULL * 1024ULL * 1024ULL);
  line("device memory restored to baseline: " +
       std::string((released && released.value()) ? "yes" : "no"));
  return 0;
}

#else

#include <cstdio>

int main() {
  std::printf("this example requires CROSS_CLUSTER_STATE_ENABLE_CUDA=ON\n");
  return 0;
}

#endif  // CCSF_EXAMPLE_CUDA
