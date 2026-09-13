// Cross-Cluster State Fabric - shared test harness.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_TEST_HARNESS_HPP
#define CCSF_TEST_HARNESS_HPP

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "ccsf/fabric.hpp"
#include "framework.hpp"

namespace ccsf::test {

/// Per-process scratch root. Tests never write outside the working directory.
inline std::string scratch_root() { return "runtime"; }

/// Creates a clean scratch directory for one case and returns its path.
inline long process_id() {
#if defined(_WIN32)
  return static_cast<long>(::_getpid());
#else
  return static_cast<long>(::getpid());
#endif
}

inline std::string fresh_directory(const std::string& name) {
  const std::filesystem::path path =
      std::filesystem::path(scratch_root()) / (name + "-" + std::to_string(process_id()));
  std::error_code code;
  std::filesystem::remove_all(path, code);
  std::filesystem::create_directories(path, code);
  return path.string();
}

inline Digest digest_of(const std::string& text) { return sha256(std::string_view(text)); }

/// Durable control plane artifact names. These are part of the on-disk contract.
inline std::string snapshot_file(const std::string& directory) {
  return directory + "/control.snapshot";
}
inline std::string journal_file(const std::string& directory) {
  return directory + "/control.journal";
}

/// Portable, warning-free file open. This is test scaffolding only; the runtime
/// uses its own checked platform primitives.
inline std::FILE* open_scratch_file(const std::string& path, const char* mode) {
#if defined(_WIN32)
  std::FILE* file = nullptr;
  if (::fopen_s(&file, path.c_str(), mode) != 0) {
    return nullptr;
  }
  return file;
#else
  return std::fopen(path.c_str(), mode);
#endif
}

inline std::vector<std::byte> read_all_bytes(const std::string& path) {
  std::vector<std::byte> data;
  std::FILE* file = open_scratch_file(path, "rb");
  if (file == nullptr) {
    return data;
  }
  std::byte buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    data.insert(data.end(), buffer, buffer + read);
  }
  std::fclose(file);
  return data;
}

inline void write_all_bytes(const std::string& path, const std::vector<std::byte>& data) {
  std::FILE* file = open_scratch_file(path, "wb");
  if (file == nullptr) {
    return;
  }
  if (!data.empty()) {
    std::fwrite(data.data(), 1, data.size(), file);
  }
  std::fclose(file);
}

inline void append_bytes(const std::string& path, const std::vector<std::byte>& data) {
  std::FILE* file = open_scratch_file(path, "ab");
  if (file == nullptr) {
    return;
  }
  if (!data.empty()) {
    std::fwrite(data.data(), 1, data.size(), file);
  }
  std::fclose(file);
}

inline std::uint64_t file_bytes(const std::string& path) {
  return static_cast<std::uint64_t>(read_all_bytes(path).size());
}

inline std::uint16_t read_u16_be(const std::vector<std::byte>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint8_t>(data[offset]) << 8U) |
      std::to_integer<std::uint8_t>(data[offset + 1U]));
}

/// Deterministic payload bytes used across the transfer proofs.
inline std::vector<std::byte> pattern_bytes(std::uint64_t length) {
  std::vector<std::byte> data(static_cast<std::size_t>(length));
  for (std::uint64_t index = 0; index < length; ++index) {
    data[static_cast<std::size_t>(index)] = static_cast<std::byte>((index * 31U + 7U) % 256U);
  }
  return data;
}

inline Digest payload_digest(std::uint64_t length) { return sha256(pattern_bytes(length)); }

/// Identity minting inside tests. Test-only counters, never used by the runtime.
struct IdSource {
  std::uint64_t next{1000};
  std::uint64_t take() { return next++; }
};

inline PlacementPolicyRecord default_placement_policy(std::uint64_t id) {
  PlacementPolicyRecord policy;
  policy.placement_policy_id = PlacementPolicyId::from_value(id);
  policy.generation = PlacementPolicyGeneration::from_value(1);
  policy.cost_policy.source = CostSource::POLICY;
  policy.cost_policy.transfer_micro_per_mib = 5000;
  policy.cost_policy.compute_micro_per_mib = 200000;
  policy.cost_policy.storage_micro_per_mib_tick = 10;
  policy.cost_policy.egress_micro_per_mib = 1000;
  policy.cost_policy.unit_label = "units";
  policy.weights.expected_reuse_value = 3;
  policy.weights.transfer_cost = -4;
  policy.weights.reconstruction_cost = -2;
  policy.weights.destination_health = 5;
  policy.weights.failure_domain_diversity = 4;
  policy.weights.available_bandwidth = 1;
  policy.weights.existing_replica_proximity = 2;
  policy.weights.utilization = 1;
  return policy;
}

inline ReplicationPolicyRecord default_replication_policy(std::uint64_t id,
                                                          std::uint32_t replicas) {
  ReplicationPolicyRecord policy = make_replication_policy(
      ReplicationPolicyId::from_value(id), ReplicationPolicyGeneration::from_value(1),
      ReplicationPolicyForm::N_REPLICAS, replicas);
  policy.retention.maximum_replicas_per_state = 8;
  policy.destination_rules.maximum_in_flight_operations_per_state = 4;
  policy.reuse_rules.require_verified_integrity = true;
  policy.reuse_rules.require_verified_compatibility = true;
  policy.reuse_rules.require_replica_set_membership = true;
  policy.revalidation.require_fresh_health_evidence = true;
  policy.revalidation.require_fresh_capability_evidence = true;
  return policy;
}

/// A fully wired single-process fabric with three clusters registered.
struct World {
  std::unique_ptr<Fabric> fabric;
  IdSource ids;
  ClusterId cluster_a;
  ClusterId cluster_b;
  ClusterId cluster_c;
  RegionId region_one;
  RegionId region_two;
  StateId state;
  StateGeneration generation;
  StateGenerationSpec last_spec;

  CoordinatorEpoch epoch() const { return fabric->coordinator_epoch(); }
  Tick now() const { return fabric->logical_time(); }
};

inline ClusterRegistration make_cluster_registration(IdSource& ids, ClusterId cluster,
                                                     RegionId region, FailureDomainId domain,
                                                     WorkerBootId boot, ClusterHealthState health,
                                                     std::vector<std::string> tags = {}) {
  ClusterRegistration registration;
  registration.cluster_id = cluster;
  registration.region = region;
  registration.site = SiteId::from_value(ids.take());
  registration.failure_domain = domain;
  registration.location_descriptor = "test-site-" + cluster.str();
  registration.worker_id = WorkerId::from_value(ids.take());
  registration.worker_boot_id = boot;
  registration.health = health;
  registration.capabilities.tags = std::move(tags);
  registration.capabilities.storage_classes.push_back("nvme");
  registration.capabilities.accelerator_architectures.push_back("sm_120");
  registration.capabilities.encryption_at_rest = true;
  registration.capacity.total_logical_bytes = 1ULL << 40U;
  registration.capacity.used_logical_bytes = 1ULL << 30U;
  registration.capacity.inbound_bytes_per_tick = 1ULL << 28U;
  registration.capacity.free_transfer_slots = 8;
  registration.capacity.utilization_per_mille = 100;
  return registration;
}

inline std::unique_ptr<World> make_world(const FabricConfig& config) {
  auto world = std::make_unique<World>();
  FabricConfig effective = config;
  effective.default_placement_policy = default_placement_policy(world->ids.take());
  effective.default_replication_policy = default_replication_policy(world->ids.take(), 2);
  auto fabric = Fabric::open(effective);
  if (!fabric) {
    return nullptr;
  }
  world->fabric = std::move(fabric.value());
  world->cluster_a = ClusterId::from_value(world->ids.take());
  world->cluster_b = ClusterId::from_value(world->ids.take());
  world->cluster_c = ClusterId::from_value(world->ids.take());
  world->region_one = RegionId::from_value(1);
  world->region_two = RegionId::from_value(2);
  auto a = world->fabric->register_cluster(make_cluster_registration(
      world->ids, world->cluster_a, world->region_one, FailureDomainId::from_value(1),
      WorkerBootId::from_value(world->ids.take()), ClusterHealthState::HEALTHY, {"gpu:sm_120"}));
  auto b = world->fabric->register_cluster(make_cluster_registration(
      world->ids, world->cluster_b, world->region_one, FailureDomainId::from_value(2),
      WorkerBootId::from_value(world->ids.take()), ClusterHealthState::HEALTHY, {"gpu:sm_120"}));
  auto c = world->fabric->register_cluster(make_cluster_registration(
      world->ids, world->cluster_c, world->region_two, FailureDomainId::from_value(3),
      WorkerBootId::from_value(world->ids.take()), ClusterHealthState::HEALTHY, {"gpu:sm_120"}));
  if (!a || !b || !c) {
    return nullptr;
  }
  world->state = StateId::from_value(world->ids.take());
  world->generation = StateGeneration::from_value(1);
  return world;
}

inline FabricConfig base_config(CoordinatorId coordinator, ClusterId home) {
  FabricConfig config;
  config.coordinator_id = coordinator;
  config.home_cluster = home;
  config.epoch_start = CoordinatorEpoch::from_value(1);
  config.logical_start = 1;
  return config;
}

/// Registers a state generation with a known content digest.
inline Status publish_state(World& world, const std::string& payload, std::uint64_t size,
                            ConsistencyMode consistency = ConsistencyMode::IMMUTABLE,
                            std::uint32_t replicas = 2,
                            const CompatibilityDescriptor& required = {}) {
  StateGenerationSpec spec;
  spec.state_id = world.state;
  spec.generation = world.generation;
  spec.lineage = LineageId::from_value(world.state.value());
  spec.state_class = StateClass::OPAQUE_STATE;
  spec.consistency = consistency;
  spec.logical_size = SizeValue::of(size);
  spec.physical_size = SizeValue::of(size);
  spec.content_digest = digest_of(payload);
  spec.compatibility_id = CompatibilityId::from_value(world.ids.take());
  spec.compatibility_generation = CompatibilityGeneration::from_value(1);
  spec.compatibility_required = required;
  spec.durability = DurabilityClass::DURABLE;
  spec.reuse_class = ReuseClass::SHARED_READ;
  spec.expected_reuse = ReuseValue::estimated(14U, 800U);
  spec.replication_requirements = make_replication_policy(
                                       ReplicationPolicyId::from_value(1),
                                       ReplicationPolicyGeneration::from_value(1),
                                       ReplicationPolicyForm::N_REPLICAS, replicas)
                                       .requirements;
  spec.provenance.producer = "test-harness";
  world.last_spec = spec;
  auto registered = world.fabric->register_state(spec);
  if (!registered) {
    return registered.error();
  }
  auto committed =
      world.fabric->commit_state_generation(world.state, world.generation,
                                            CommitId::from_value(world.ids.take()));
  if (!committed) {
    return committed.error();
  }
  return Status{};
}

/// Establishes one authoritative replica of the current generation in a cluster.
inline Result<ReplicaId> seed_replica(World& world, ClusterId cluster,
                                      ReplicaDurabilityState durability,
                                      bool make_owner = false) {
  const Digest digest = digest_of("payload");
  (void)digest;
  auto state = world.fabric->query_state(world.state, world.generation);
  if (!state) {
    return state.error();
  }
  ReplicaRegistration registration;
  registration.state_id = world.state;
  registration.state_generation = world.generation;
  registration.location.cluster_id = cluster;
  registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
  registration.digest_algorithm = IntegrityAlgorithm::SHA256;
  registration.expected_coordinator_epoch = world.epoch();
  registration.content_digest = state.value().content_digest;
  auto registered = world.fabric->register_replica(registration);
  if (!registered) {
    return registered.error();
  }
  const ReplicaRecord record = registered.value();
  TransferCompletion completion;
  completion.replica_id = record.replica_id;
  completion.replica_generation = record.replica_generation;
  completion.transfer_id = record.transfer_id;
  completion.cluster_id = cluster;
  completion.expected_coordinator_epoch = world.epoch();
  completion.bytes_transferred = state.value().logical_size.bytes;
  completion.measured_digest = state.value().content_digest;
  auto completed = world.fabric->record_transfer_completion(completion);
  if (!completed) {
    return completed.error();
  }
  ReplicaVerification verification;
  verification.replica_id = record.replica_id;
  verification.replica_generation = record.replica_generation;
  verification.expected_coordinator_epoch = world.epoch();
  verification.integrity_verified = true;
  verification.measured_digest = state.value().content_digest;
  verification.compatibility_verified = true;
  verification.compatibility_id = state.value().compatibility_id;
  verification.compatibility_generation = state.value().compatibility_generation;
  verification.compatibility_descriptor = state.value().compatibility_required;
  verification.established_durability = DurabilityClass::DURABLE;
  verification.verified_bytes = state.value().logical_size.bytes;
  auto verified = world.fabric->verify_replica(verification);
  if (!verified) {
    return verified.error();
  }
  ReplicaCommit commit;
  commit.replica_id = record.replica_id;
  commit.replica_generation = record.replica_generation;
  commit.commit_id = CommitId::from_value(world.ids.take());
  commit.expected_coordinator_epoch = world.epoch();
  commit.make_authoritative_owner = make_owner;
  auto committed = world.fabric->commit_replica(commit);
  if (!committed) {
    return committed.error();
  }
  (void)durability;
  return record.replica_id;
}

/// Drives a full authorized replication to completion through the public API.
inline Result<ReplicaId> replicate(World& world, ClusterId destination) {
  ReplicationRequest request;
  request.state_id = world.state;
  request.state_generation = world.generation;
  request.destination_cluster = destination;
  request.expected_coordinator_epoch = world.epoch();
  auto authorization = world.fabric->authorize_replication(request);
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
  progress.expected_coordinator_epoch = world.epoch();
  progress.bytes_transferred = auth.transfer_bytes.known() ? auth.transfer_bytes.bytes / 2U : 0U;
  auto started = world.fabric->record_transfer_progress(progress);
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
  completion.expected_coordinator_epoch = world.epoch();
  completion.bytes_transferred = auth.transfer_bytes.known() ? auth.transfer_bytes.bytes : 0U;
  completion.measured_digest = auth.expected_digest;
  auto transferred = world.fabric->record_transfer_completion(completion);
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
  verification.expected_coordinator_epoch = world.epoch();
  verification.integrity_verified = true;
  verification.measured_digest = auth.expected_digest;
  verification.compatibility_verified = true;
  verification.verified_bytes = completion.bytes_transferred;
  verification.established_durability = DurabilityClass::DURABLE;
  auto verified = world.fabric->verify_replica(verification);
  if (!verified) {
    return verified.error();
  }
  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(world.ids.take());
  commit.expected_coordinator_epoch = world.epoch();
  auto committed = world.fabric->commit_operation(commit);
  if (!committed) {
    return committed.error();
  }
  return auth.destination_replica;
}

}  // namespace ccsf::test

#endif  // CCSF_TEST_HARNESS_HPP
