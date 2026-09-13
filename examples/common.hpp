// Cross-Cluster State Fabric - shared example scaffolding.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every example compiles against the installed public API only: no internal
// headers, no private paths.

#ifndef CCSF_EXAMPLES_COMMON_HPP
#define CCSF_EXAMPLES_COMMON_HPP

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "ccsf/fabric.hpp"

namespace ccsf::examples {

inline Digest digest_of(const std::string& text) { return sha256(std::string_view(text)); }

inline void banner(const char* title) {
  std::printf("=== %s ===\n", title);
  std::fflush(stdout);
}

inline void line(const std::string& text) {
  std::printf("%s\n", text.c_str());
  std::fflush(stdout);
}

inline void fail(const std::string& text) {
  std::fprintf(stderr, "example failed: %s\n", text.c_str());
  std::fflush(stderr);
}

struct Example {
  std::unique_ptr<Fabric> fabric;
  ClusterId cluster_a;
  ClusterId cluster_b;
  ClusterId cluster_c;
  StateId state;
  StateGeneration generation;
  std::uint64_t next_id{5000};

  std::uint64_t take() { return next_id++; }
};

inline ClusterRegistration cluster_registration(std::uint64_t cluster, std::uint64_t region,
                                                std::uint64_t domain, std::uint64_t boot,
                                                std::vector<std::string> tags = {}) {
  ClusterRegistration registration;
  registration.cluster_id = ClusterId::from_value(cluster);
  registration.region = RegionId::from_value(region);
  registration.site = SiteId::from_value(domain);
  registration.failure_domain = FailureDomainId::from_value(domain);
  registration.location_descriptor = "example-site-" + std::to_string(cluster);
  registration.worker_id = WorkerId::from_value(cluster);
  registration.worker_boot_id = WorkerBootId::from_value(boot);
  registration.health = ClusterHealthState::HEALTHY;
  registration.capabilities.tags = std::move(tags);
  registration.capabilities.storage_classes = {"nvme"};
  registration.capabilities.accelerator_architectures = {"sm_120"};
  registration.capacity.total_logical_bytes = 1ULL << 40U;
  registration.capacity.used_logical_bytes = 1ULL << 30U;
  registration.capacity.inbound_bytes_per_tick = 1ULL << 28U;
  registration.capacity.free_transfer_slots = 8;
  registration.capacity.utilization_per_mille = 100;
  return registration;
}

inline PlacementPolicyRecord placement_policy(std::uint64_t id) {
  PlacementPolicyRecord policy;
  policy.placement_policy_id = PlacementPolicyId::from_value(id);
  policy.generation = PlacementPolicyGeneration::from_value(1);
  policy.cost_policy.source = CostSource::POLICY;
  policy.cost_policy.transfer_micro_per_mib = 5000;
  policy.cost_policy.compute_micro_per_mib = 200000;
  policy.cost_policy.unit_label = "units";
  policy.weights.expected_reuse_value = 3;
  policy.weights.transfer_cost = -4;
  policy.weights.destination_health = 5;
  policy.weights.failure_domain_diversity = 4;
  return policy;
}

inline ReplicationPolicyRecord replication_policy(std::uint64_t id, std::uint32_t replicas) {
  ReplicationPolicyRecord policy = make_replication_policy(
      ReplicationPolicyId::from_value(id), ReplicationPolicyGeneration::from_value(1),
      ReplicationPolicyForm::N_REPLICAS, replicas);
  policy.retention.maximum_replicas_per_state = 8;
  return policy;
}

inline std::unique_ptr<Example> make_example(const char* title) {
  banner(title);
  FabricConfig config;
  config.coordinator_id = CoordinatorId::from_value(1);
  config.home_cluster = ClusterId::from_value(100);
  config.epoch_start = CoordinatorEpoch::from_value(1);
  config.logical_start = 1;
  config.default_placement_policy = placement_policy(1);
  config.default_replication_policy = replication_policy(1, 2);
  auto fabric = Fabric::open(config);
  if (!fabric) {
    fail(fabric.error().to_string());
    return nullptr;
  }
  auto example = std::make_unique<Example>();
  example->fabric = std::move(fabric.value());
  example->cluster_a = ClusterId::from_value(100);
  example->cluster_b = ClusterId::from_value(101);
  example->cluster_c = ClusterId::from_value(102);
  for (const auto& registration :
       {cluster_registration(100, 1, 1, 11, {"gpu:sm_120", "compat:runtime_abi=ccsf-1"}),
        cluster_registration(101, 1, 2, 12, {"gpu:sm_120", "compat:runtime_abi=ccsf-1"}),
        cluster_registration(102, 2, 3, 13, {"gpu:sm_120", "compat:runtime_abi=ccsf-1"})}) {
    auto registered = example->fabric->register_cluster(registration);
    if (!registered) {
      fail(registered.error().to_string());
      return nullptr;
    }
  }
  example->state = StateId::from_value(7001);
  example->generation = StateGeneration::from_value(1);
  return example;
}

inline Status publish(Example& example, const std::string& payload, std::uint64_t size,
                      ConsistencyMode consistency = ConsistencyMode::IMMUTABLE,
                      std::uint32_t replicas = 2,
                      const CompatibilityDescriptor& required = CompatibilityDescriptor{}) {
  StateGenerationSpec spec;
  spec.state_id = example.state;
  spec.generation = example.generation;
  spec.lineage = LineageId::from_value(example.state.value());
  spec.state_class = StateClass::OPAQUE_STATE;
  spec.consistency = consistency;
  spec.logical_size = SizeValue::of(size);
  spec.content_digest = digest_of(payload);
  spec.compatibility_id = CompatibilityId::from_value(example.state.value() * 7U + 1U);
  spec.compatibility_generation = CompatibilityGeneration::from_value(1);
  spec.compatibility_required = required;
  spec.durability = DurabilityClass::DURABLE;
  spec.expected_reuse = ReuseValue::estimated(14U, 800U);
  spec.replication_requirements =
      replication_policy(1, replicas).requirements;
  spec.provenance.producer = "example";
  auto registered = example.fabric->register_state(spec);
  if (!registered) {
    return registered.error();
  }
  auto committed = example.fabric->commit_state_generation(
      example.state, example.generation, CommitId::from_value(example.take()));
  if (!committed) {
    return committed.error();
  }
  return Status{};
}

/// Registers, transfers, verifies and commits one authoritative replica.
inline Result<ReplicaId> seed(Example& example, ClusterId cluster, bool make_owner = false) {
  auto state = example.fabric->query_state(example.state, example.generation);
  if (!state) {
    return state.error();
  }
  ReplicaRegistration registration;
  registration.state_id = example.state;
  registration.state_generation = example.generation;
  registration.location.cluster_id = cluster;
  registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
  registration.content_digest = state.value().content_digest;
  registration.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  auto registered = example.fabric->register_replica(registration);
  if (!registered) {
    return registered.error();
  }
  TransferCompletion completion;
  completion.replica_id = registered.value().replica_id;
  completion.replica_generation = registered.value().replica_generation;
  completion.cluster_id = cluster;
  completion.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  completion.bytes_transferred = state.value().logical_size.bytes;
  completion.measured_digest = state.value().content_digest;
  auto transferred = example.fabric->record_transfer_completion(completion);
  if (!transferred) {
    return transferred.error();
  }
  ReplicaVerification verification;
  verification.replica_id = registered.value().replica_id;
  verification.replica_generation = registered.value().replica_generation;
  verification.cluster_id = cluster;
  verification.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  verification.integrity_verified = true;
  verification.measured_digest = state.value().content_digest;
  verification.verified_bytes = state.value().logical_size.bytes;
  verification.established_durability = DurabilityClass::DURABLE;
  auto verified = example.fabric->verify_replica(verification);
  if (!verified) {
    return verified.error();
  }
  ReplicaCommit commit;
  commit.replica_id = registered.value().replica_id;
  commit.commit_id = CommitId::from_value(example.take());
  commit.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  commit.make_authoritative_owner = make_owner;
  auto committed = example.fabric->commit_replica(commit);
  if (!committed) {
    return committed.error();
  }
  return registered.value().replica_id;
}

/// Drives one fully authorized replication transaction to completion.
inline Result<ReplicaId> replicate(Example& example, ClusterId destination,
                                   bool retire_source = false) {
  ReplicationRequest request;
  request.state_id = example.state;
  request.state_generation = example.generation;
  request.destination_cluster = destination;
  request.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  auto authorization = example.fabric->authorize_replication(request);
  if (!authorization) {
    return authorization.error();
  }
  const TransferAuthorization auth = authorization.value();
  TransferProgress progress;
  progress.transfer_id = auth.transfer_id;
  progress.transfer_generation = auth.transfer_generation;
  progress.replica_id = auth.destination_replica;
  progress.replica_generation = auth.destination_replica_generation;
  progress.operation_id = auth.operation_id;
  progress.operation_generation = auth.operation_generation;
  progress.cluster_id = auth.destination_cluster;
  progress.cluster_incarnation = auth.destination_incarnation;
  progress.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  progress.bytes_transferred = auth.transfer_bytes.known() ? auth.transfer_bytes.bytes / 2U : 0U;
  auto started = example.fabric->record_transfer_progress(progress);
  if (!started) {
    return started.error();
  }
  TransferCompletion completion;
  completion.transfer_id = auth.transfer_id;
  completion.transfer_generation = auth.transfer_generation;
  completion.replica_id = auth.destination_replica;
  completion.replica_generation = auth.destination_replica_generation;
  completion.operation_id = auth.operation_id;
  completion.operation_generation = auth.operation_generation;
  completion.cluster_id = auth.destination_cluster;
  completion.cluster_incarnation = auth.destination_incarnation;
  completion.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  completion.bytes_transferred = auth.transfer_bytes.known() ? auth.transfer_bytes.bytes : 0U;
  completion.measured_digest = auth.expected_digest;
  auto transferred = example.fabric->record_transfer_completion(completion);
  if (!transferred) {
    return transferred.error();
  }
  ReplicaVerification verification;
  verification.replica_id = auth.destination_replica;
  verification.replica_generation = auth.destination_replica_generation;
  verification.operation_id = auth.operation_id;
  verification.operation_generation = auth.operation_generation;
  verification.cluster_id = auth.destination_cluster;
  verification.cluster_incarnation = auth.destination_incarnation;
  verification.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  verification.integrity_verified = true;
  verification.measured_digest = auth.expected_digest;
  verification.verified_bytes = completion.bytes_transferred;
  verification.established_durability = DurabilityClass::DURABLE;
  auto verified = example.fabric->verify_replica(verification);
  if (!verified) {
    return verified.error();
  }
  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(example.take());
  commit.expected_coordinator_epoch = example.fabric->coordinator_epoch();
  commit.retire_source = retire_source;
  auto committed = example.fabric->commit_operation(commit);
  if (!committed) {
    return committed.error();
  }
  return auth.destination_replica;
}

inline void report_replica(Fabric& fabric, ReplicaId replica) {
  auto record = fabric.query_replica(replica);
  if (!record) {
    fail(record.error().to_string());
    return;
  }
  line("  replica " + record.value().replica_id.str() + " cluster " +
       record.value().location.cluster_id.str() + " lifecycle " +
       to_string(record.value().lifecycle) + " authority " + to_string(record.value().authority) +
       " counts " + (record.value().counts_toward_factor ? "yes" : "no"));
}

}  // namespace ccsf::examples

#endif  // CCSF_EXAMPLES_COMMON_HPP
