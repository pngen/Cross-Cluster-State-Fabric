// Cross-Cluster State Fabric - benchmarks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every benchmark measures completed work: the timer surrounds the whole
// operation the runtime must finish, including verification and commit. No
// result is reported from an enqueue time.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "ccsf/fabric.hpp"
#include "ccsf/version.hpp"

namespace {

using namespace ccsf;
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void report(const std::string& name, std::uint64_t operations, double seconds) {
  const double per_operation = operations == 0U ? 0.0 : seconds / static_cast<double>(operations);
  std::printf("%-38s %10llu ops %9.3f s %12.3f us/op\n", name.c_str(),
              static_cast<unsigned long long>(operations), seconds, per_operation * 1e6);
  std::fflush(stdout);
}

FabricConfig benchmark_config() {
  FabricConfig config;
  config.coordinator_id = CoordinatorId::from_value(1);
  config.home_cluster = ClusterId::from_value(100);
  config.epoch_start = CoordinatorEpoch::from_value(1);
  config.logical_start = 1;
  config.default_placement_policy.placement_policy_id = PlacementPolicyId::from_value(1);
  config.default_placement_policy.generation = PlacementPolicyGeneration::from_value(1);
  config.default_placement_policy.cost_policy.source = CostSource::POLICY;
  config.default_placement_policy.cost_policy.transfer_micro_per_mib = 5000;
  config.default_placement_policy.cost_policy.compute_micro_per_mib = 200000;
  config.default_placement_policy.weights.expected_reuse_value = 3;
  config.default_placement_policy.weights.transfer_cost = -4;
  config.default_placement_policy.weights.destination_health = 5;
  config.default_replication_policy = make_replication_policy(
      ReplicationPolicyId::from_value(1), ReplicationPolicyGeneration::from_value(1),
      ReplicationPolicyForm::N_REPLICAS, 2);
  config.default_replication_policy.retention.maximum_replicas_per_state = 8;
  return config;
}

ClusterRegistration cluster_registration(std::uint64_t cluster, std::uint64_t region,
                                        std::uint64_t domain) {
  ClusterRegistration registration;
  registration.cluster_id = ClusterId::from_value(cluster);
  registration.region = RegionId::from_value(region);
  registration.site = SiteId::from_value(domain);
  registration.failure_domain = FailureDomainId::from_value(domain);
  registration.location_descriptor = "benchmark-site";
  registration.worker_id = WorkerId::from_value(cluster);
  registration.worker_boot_id = WorkerBootId::from_value(cluster);
  registration.health = ClusterHealthState::HEALTHY;
  registration.capabilities.tags = {"gpu:sm_120"};
  registration.capabilities.storage_classes = {"nvme"};
  registration.capabilities.accelerator_architectures = {"sm_120"};
  registration.capacity.total_logical_bytes = 1ULL << 42U;
  registration.capacity.used_logical_bytes = 1ULL << 30U;
  registration.capacity.inbound_bytes_per_tick = 1ULL << 30U;
  registration.capacity.free_transfer_slots = 64;
  registration.capacity.utilization_per_mille = 100;
  return registration;
}

StateGenerationSpec state_spec(StateId state, std::uint64_t size) {
  StateGenerationSpec spec;
  spec.state_id = state;
  spec.generation = StateGeneration::from_value(1);
  spec.lineage = LineageId::from_value(state.value());
  spec.logical_size = SizeValue::of(size);
  spec.content_digest = sha256(std::string_view("benchmark-state"));
  spec.compatibility_id = CompatibilityId::from_value(1);
  spec.compatibility_generation = CompatibilityGeneration::from_value(1);
  spec.replication_requirements =
      make_replication_policy(ReplicationPolicyId::from_value(1),
                              ReplicationPolicyGeneration::from_value(1),
                              ReplicationPolicyForm::N_REPLICAS, 2)
          .requirements;
  return spec;
}

}  // namespace

int main() {
  using namespace ccsf;
  std::printf("Cross-Cluster State Fabric %s benchmarks\n", version_string());
  std::printf("measurement: completed work, single-threaded unless stated\n\n");

  auto fabric = Fabric::open(benchmark_config());
  if (!fabric) {
    std::fprintf(stderr, "benchmark setup failed: %s\n", fabric.error().to_string().c_str());
    return 1;
  }
  Fabric& runtime = *fabric.value();
  for (std::uint64_t index = 0; index < 8; ++index) {
    (void)runtime.register_cluster(cluster_registration(100U + index, 1U + index / 4U, index));
  }

  // State registration and commit.
  constexpr std::uint64_t kStates = 1000;
  {
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kStates; ++index) {
      const StateId state = StateId::from_value(1000000U + index);
      if (!runtime.register_state(state_spec(state, 4096))) {
        std::fprintf(stderr, "registration failed\n");
        return 1;
      }
    }
    report("state registration", kStates, seconds_since(start));
  }
  {
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kStates; ++index) {
      (void)runtime.commit_state_generation(StateId::from_value(1000000U + index),
                                            StateGeneration::from_value(1),
                                            CommitId::from_value(index + 1U));
    }
    report("state generation commit", kStates, seconds_since(start));
  }

  // Replica registration.
  constexpr std::uint64_t kReplicas = 1000;
  std::vector<ReplicaId> replicas;
  {
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kReplicas; ++index) {
      ReplicaRegistration registration;
      registration.state_id = StateId::from_value(1000000U + index);
      registration.state_generation = StateGeneration::from_value(1);
      registration.location.cluster_id = ClusterId::from_value(100U + (index % 8U));
      registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
      registration.content_digest = sha256(std::string_view("benchmark-state"));
      registration.expected_coordinator_epoch = runtime.coordinator_epoch();
      auto registered = runtime.register_replica(registration);
      if (!registered) {
        std::fprintf(stderr, "replica registration failed\n");
        return 1;
      }
      replicas.push_back(registered.value().replica_id);
    }
    report("replica registration", kReplicas, seconds_since(start));
  }

  // Placement evaluation.
  {
    PlacementRequest request;
    request.state_id = StateId::from_value(1000000U);
    request.state_generation = StateGeneration::from_value(1);
    request.policy = benchmark_config().default_placement_policy;
    request.replication_policy = benchmark_config().default_replication_policy;
    for (std::uint64_t index = 0; index < 8; ++index) {
      PlacementCandidate candidate;
      candidate.cluster_id = ClusterId::from_value(100U + index);
      candidate.cluster_incarnation = ClusterIncarnationId::from_value(100U + index);
      candidate.region = RegionId::from_value(1U + index / 4U);
      candidate.failure_domain = FailureDomainId::from_value(index);
      candidate.health = ClusterHealthState::HEALTHY;
      candidate.capabilities.accelerator_architectures = {"sm_120"};
      candidate.capabilities.storage_classes = {"nvme"};
      candidate.capacity.total_logical_bytes = 1ULL << 40U;
      candidate.capacity.used_logical_bytes = 1ULL << 30U;
      candidate.capacity.inbound_bytes_per_tick = 1ULL << 28U;
      candidate.capacity.utilization_per_mille = 100;
      candidate.compatibility = CompatibilityResult::COMPATIBLE;
      candidate.integrity_evidence_available = true;
      candidate.logical_size = SizeValue::of(1ULL << 30U);
      candidate.transfer_bytes = SizeValue::of(1ULL << 30U);
      candidate.expected_reuse = ReuseValue::estimated(10U, 900U);
      candidate.source_cluster_healthy = true;
      candidate.achievable_durability = ReplicaDurabilityState::DURABLE;
      request.candidates.push_back(candidate);
    }
    constexpr std::uint64_t kEvaluations = 5000;
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kEvaluations; ++index) {
      const PlacementEvaluation evaluation = evaluate_placement(request, nullptr);
      if (evaluation.decision == DecisionKind::UNKNOWN) {
        std::fprintf(stderr, "placement evaluation produced no decision\n");
        return 1;
      }
    }
    report("placement evaluation", kEvaluations, seconds_since(start));
  }

  // Replica lookup and reuse eligibility.
  {
    constexpr std::uint64_t kLookups = 20000;
    const auto start = Clock::now();
    std::uint64_t found = 0;
    for (std::uint64_t index = 0; index < kLookups; ++index) {
      if (runtime.query_replica(replicas[index % replicas.size()])) {
        ++found;
      }
    }
    report("replica lookup", kLookups, seconds_since(start));
    if (found != kLookups) {
      std::fprintf(stderr, "replica lookup lost records\n");
      return 1;
    }
  }
  {
    constexpr std::uint64_t kEvaluations = 5000;
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kEvaluations; ++index) {
      (void)runtime.evaluate_reuse(StateId::from_value(1000000U + (index % kStates)),
                                   StateGeneration::from_value(1),
                                   ClusterId::from_value(100U + (index % 8U)));
    }
    report("reuse eligibility", kEvaluations, seconds_since(start));
  }

  // Replica set reconciliation, migration planning and explanation.
  {
    constexpr std::uint64_t kRuns = 5000;
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kRuns; ++index) {
      (void)runtime.reconcile_replica_set(StateId::from_value(1000000U + (index % kStates)),
                                          StateGeneration::from_value(1));
    }
    report("replica set reconciliation", kRuns, seconds_since(start));
  }
  {
    constexpr std::uint64_t kRuns = 2000;
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kRuns; ++index) {
      ReplicationRequest request;
      request.state_id = StateId::from_value(1000000U + (index % 64U));
      request.state_generation = StateGeneration::from_value(1);
      (void)runtime.plan_replication(request);
    }
    report("replication planning", kRuns, seconds_since(start));
  }
  {
    constexpr std::uint64_t kRuns = 2000;
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kRuns; ++index) {
      (void)runtime.explain_replica_set_state(StateId::from_value(1000000U + (index % 64U)),
                                              StateGeneration::from_value(1));
    }
    report("deterministic explanation", kRuns, seconds_since(start));
  }

  // Integrity verification: hashing completed bytes.
  {
    std::vector<std::byte> payload(1U << 20U);
    for (std::size_t index = 0; index < payload.size(); ++index) {
      payload[index] = static_cast<std::byte>(index % 251U);
    }
    constexpr std::uint64_t kHashes = 512;
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < kHashes; ++index) {
      (void)sha256(ByteSpan(payload.data(), payload.size()));
    }
    const double seconds = seconds_since(start);
    report("integrity verification (1 MiB)", kHashes, seconds);
    std::printf("%-38s %12.2f MiB/s\n", "verified throughput",
                static_cast<double>(kHashes) / seconds);
  }

  // Persistence save and load.
  {
    FabricConfig config = benchmark_config();
    config.persistence_directory = "runtime/benchmark-persistence";
    config.require_durable_commits = true;
    auto persistent = Fabric::open(config);
    if (!persistent) {
      std::fprintf(stderr, "persistent benchmark setup failed\n");
      return 1;
    }
    for (std::uint64_t index = 0; index < 8; ++index) {
      (void)persistent.value()->register_cluster(cluster_registration(200U + index, 1, index));
    }
    constexpr std::uint64_t kRecords = 500;
    const auto commit_start = Clock::now();
    for (std::uint64_t index = 0; index < kRecords; ++index) {
      const StateId state = StateId::from_value(2000000U + index);
      (void)persistent.value()->register_state(state_spec(state, 4096));
      (void)persistent.value()->commit_state_generation(state, StateGeneration::from_value(1),
                                                        CommitId::from_value(index + 10U));
    }
    report("durable mutation (journal append)", kRecords * 2U, seconds_since(commit_start));
    const auto save_start = Clock::now();
    (void)persistent.value()->persist();
    report("checkpoint save", 1U, seconds_since(save_start));
    const auto load_start = Clock::now();
    auto reloaded = Fabric::open(config);
    if (!reloaded) {
      std::fprintf(stderr, "reload failed: %s\n", reloaded.error().to_string().c_str());
      return 1;
    }
    report("checkpoint load and recovery", 1U, seconds_since(load_start));
  }

  std::printf("\nmetadata scaling\n");
  for (std::uint64_t target : {100ULL, 1000ULL, 10000ULL}) {
    auto scaled = Fabric::open(benchmark_config());
    if (!scaled) {
      return 1;
    }
    for (std::uint64_t index = 0; index < 4; ++index) {
      (void)scaled.value()->register_cluster(cluster_registration(300U + index, 1, index));
    }
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < target; ++index) {
      const StateId state = StateId::from_value(3000000U + index);
      if (!scaled.value()->register_state(state_spec(state, 4096))) {
        std::fprintf(stderr, "scaled registration failed at %llu\n",
                     static_cast<unsigned long long>(index));
        return 1;
      }
    }
    report(std::to_string(target) + " states registered", target, seconds_since(start));
    const auto snapshot_start = Clock::now();
    const FabricSnapshot snapshot = scaled.value()->snapshot();
    report(std::to_string(target) + " states snapshot", snapshot.states.size(),
           seconds_since(snapshot_start));
  }
  return 0;
}
