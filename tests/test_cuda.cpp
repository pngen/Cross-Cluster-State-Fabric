// Cross-Cluster State Fabric - real CUDA device state proof.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This case is only compiled when CROSS_CLUSTER_STATE_ENABLE_CUDA is on and a
// CUDA compiler is present. It performs real device work: cudaMalloc, a
// host-to-device copy, a kernel launch, a device synchronization, a
// device-to-host copy and cudaFree. The state payload the fabric governs is the
// device-produced bytes, bit for bit.
//
// TOPOLOGY LABEL: the device work is REAL. The two clusters are SYNTHETIC
// logical clusters sharing one physical host; no cross-machine GPU transfer is
// claimed.

#if defined(CCSF_TEST_CUDA)

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf_cuda_backend.hpp"
#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

CCSF_CASE(cuda, device_state_is_generated_replicated_and_released) {
  auto info = cuda::device_info();
  ctx.check(info.has_value(), std::string("a CUDA device must be usable: ") +
                                  (info ? std::string("ok") : info.error().to_string()));
  ctx.note("device: " + info.value().name + " sm_" + std::to_string(info.value().compute_major) +
           std::to_string(info.value().compute_minor) + " runtime " +
           std::to_string(info.value().runtime_version) + " driver " +
           std::to_string(info.value().driver_version));
  const std::size_t baseline_free = info.value().free_memory_bytes;

  constexpr std::uint64_t kRows = 4096;
  constexpr std::uint64_t kColumns = 512;
  auto payload = cuda::generate_tensor_state(kRows, kColumns, 0x5EEDU);
  ctx.check(payload.has_value(), std::string("the device produces a state payload: ") +
                                     (payload ? std::string("ok") : payload.error().to_string()));
  const std::vector<std::byte> device_bytes = payload.value();
  ctx.check(device_bytes.size() == kRows * sizeof(float),
            "the payload is the device-produced FP32 output");

  const std::vector<float> reference = cuda::reference_tensor_state(kRows, kColumns, 0x5EEDU);
  ctx.check(cuda::tensors_agree(device_bytes, reference, 1e-2f),
            "the device result agrees with the CPU reference within FP32 tolerance");

  const Digest digest = sha256(ByteSpan(device_bytes.data(), device_bytes.size()));
  ctx.note("device state digest: " + digest.hex());

  auto world = make_world(base_config(CoordinatorId::from_value(95), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  const std::string architecture = "sm_" + std::to_string(info.value().compute_major) +
                                   std::to_string(info.value().compute_minor);
  auto hosting_cluster = world->fabric->query_cluster(world->cluster_a);
  ctx.check(hosting_cluster.has_value(), "the hosting cluster is registered");
  ctx.check(hosting_cluster.value().capabilities.has_accelerator(architecture),
            "the hosting cluster advertises the device architecture " + architecture);

  StateGenerationSpec spec;
  spec.state_id = world->state;
  spec.generation = world->generation;
  spec.lineage = LineageId::from_value(world->state.value());
  spec.state_class = StateClass::TENSOR_STATE;
  spec.consistency = ConsistencyMode::IMMUTABLE;
  spec.logical_size = SizeValue::of(device_bytes.size());
  spec.physical_size = SizeValue::of(device_bytes.size());
  spec.content_digest = digest;
  spec.durability = DurabilityClass::STANDARD;
  spec.reuse_class = ReuseClass::SHARED_READ;
  spec.expected_reuse = ReuseValue::estimated(64U, 900U);
  spec.provenance.producer = "cuda-gemv-example";
  spec.replication_requirements =
      make_replication_policy(ReplicationPolicyId::from_value(1),
                              ReplicationPolicyGeneration::from_value(1),
                              ReplicationPolicyForm::N_REPLICAS, 2)
          .requirements;
  world->last_spec = spec;
  ctx.check(world->fabric->register_state(spec).has_value(), "the device state is registered");
  ctx.check(world->fabric->commit_state_generation(world->state, world->generation,
                                                   CommitId::from_value(1))
                .has_value(),
            "the device state generation is committed");

  ReplicaRegistration registration;
  registration.state_id = world->state;
  registration.state_generation = world->generation;
  registration.location.cluster_id = world->cluster_a;
  registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
  registration.content_digest = digest;
  registration.expected_coordinator_epoch = world->epoch();
  auto registered = world->fabric->register_replica(registration);
  ctx.check(registered.has_value(), "a replica of the device state is registered");
  TransferCompletion completion;
  completion.replica_id = registered.value().replica_id;
  completion.replica_generation = registered.value().replica_generation;
  completion.cluster_id = world->cluster_a;
  completion.expected_coordinator_epoch = world->epoch();
  completion.bytes_transferred = device_bytes.size();
  completion.measured_digest = digest;
  ctx.check(world->fabric->record_transfer_completion(completion).has_value(),
            "the device bytes cross the runtime's transfer boundary");
  ReplicaVerification verification;
  verification.replica_id = registered.value().replica_id;
  verification.replica_generation = registered.value().replica_generation;
  verification.cluster_id = world->cluster_a;
  verification.expected_coordinator_epoch = world->epoch();
  verification.integrity_verified = true;
  verification.measured_digest = digest;
  verification.verified_bytes = device_bytes.size();
  verification.established_durability = DurabilityClass::STANDARD;
  ctx.check(world->fabric->verify_replica(verification).has_value(),
            "the device state verifies against its digest");
  ReplicaCommit commit;
  commit.replica_id = registered.value().replica_id;
  commit.commit_id = CommitId::from_value(2);
  commit.expected_coordinator_epoch = world->epoch();
  ctx.check(world->fabric->commit_replica(commit).has_value(),
            "the device state replica becomes authoritative");

  auto replicated = replicate(*world, world->cluster_b);
  ctx.check(replicated.has_value(), "the device state replicates to a second logical cluster");
  auto destination = world->fabric->query_replica(replicated.value());
  ctx.check(destination.has_value(), "the destination replica exists");
  ctx.check(destination.value().measured_digest == digest,
            "the destination independently measured the same digest");

  auto reuse_destination =
      world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_b);
  ctx.check(reuse_destination.value().assessment.authoritative_for_reuse,
            "the destination may serve reuse once it is committed");

  ReplicaTransition retire_source;
  retire_source.replica_id = registered.value().replica_id;
  retire_source.expected_coordinator_epoch = world->epoch();
  retire_source.reason = "device state freed at the source";
  ctx.check(world->fabric->retire_replica(retire_source).has_value(),
            "the source replica can be retired after the destination commit");
  auto reuse_source =
      world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_a);
  ctx.check(reuse_source.value().decision == DecisionKind::REJECT,
            "the retired source no longer serves reuse");
  auto reuse_after =
      world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_b);
  ctx.check(reuse_after.value().decision != DecisionKind::REJECT,
            "the destination remains the authoritative copy");

  payload.value().clear();
  auto released = cuda::release_all(baseline_free, 64ULL * 1024ULL * 1024ULL);
  ctx.check(released.has_value() && released.value(),
            "device memory returns to its baseline after the state is released");
}

#endif  // CCSF_TEST_CUDA
