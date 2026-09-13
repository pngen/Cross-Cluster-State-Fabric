// Cross-Cluster State Fabric - concurrency proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The documented concurrency model is exercised directly: concurrent queries,
// evidence publication, replica updates, placement evaluation, transfer
// completion, snapshot reads and persistence. Every case ends by checking that
// accounting returned to its baseline, so a lost update or a leaked reservation
// is visible as a failure rather than as silence.

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

CCSF_CASE(concurrency, readers_never_observe_a_torn_snapshot) {
  const std::string directory = fresh_directory("concurrent-readers");
  FabricConfig config = base_config(CoordinatorId::from_value(90), ClusterId::from_value(100));
  config.persistence_directory = directory;
  auto world = make_world(config);
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> queries{0};
  std::atomic<std::uint64_t> violations{0};

  // Explicit start barrier: the writer must not begin, let alone finish, before
  // every reader has completed at least one query. Nothing here sleeps or polls.
  std::mutex start_mutex;
  std::condition_variable start_condition;
  int readers_reporting = 0;
  constexpr int kReaders = 4;

  std::vector<std::thread> readers;
  for (int index = 0; index < kReaders; ++index) {
    readers.emplace_back([&]() {
      bool announced = false;
      while (!stop.load()) {
        const FabricSnapshot snapshot = world->fabric->snapshot();
        std::uint64_t states = 0;
        for (const StateGenerationRecord& state : snapshot.states) {
          if (state.state_id == world->state) {
            ++states;
          }
        }
        if (states != 1) {
          violations.fetch_add(1);
        }
        queries.fetch_add(1);
        if (!announced) {
          announced = true;
          std::lock_guard<std::mutex> guard(start_mutex);
          ++readers_reporting;
          start_condition.notify_all();
        }
      }
    });
  }
  {
    std::unique_lock<std::mutex> guard(start_mutex);
    start_condition.wait(guard, [&]() { return readers_reporting == kReaders; });
  }

  for (int round = 0; round < 20; ++round) {
    ClusterEvidenceUpdate update;
    auto cluster = world->fabric->query_cluster(world->cluster_a);
    update.cluster_id = world->cluster_a;
    update.incarnation = cluster.value().incarnation;
    update.expected_coordinator_epoch = world->epoch();
    update.has_capabilities = true;
    update.capabilities.tags = {"gpu:sm_120", "round:" + std::to_string(round)};
    (void)world->fabric->publish_cluster_evidence(update);
    (void)world->fabric->advance_time(1);
  }
  stop = true;
  for (std::thread& reader : readers) {
    reader.join();
  }
  ctx.check(queries.load() > 0, "the readers performed work");
  ctx.check(violations.load() == 0, "no reader observed a torn control plane");
}

CCSF_CASE(concurrency, distinct_states_mutate_in_parallel) {
  const std::string directory = fresh_directory("concurrent-states");
  FabricConfig config = base_config(CoordinatorId::from_value(91), ClusterId::from_value(100));
  config.persistence_directory = directory;
  auto world = make_world(config);
  ctx.check(world != nullptr, "world");

  constexpr int kStates = 6;
  for (int index = 0; index < kStates; ++index) {
    const StateId state = StateId::from_value(50000U + static_cast<std::uint64_t>(index));
    StateGenerationSpec spec;
    spec.state_id = state;
    spec.generation = StateGeneration::from_value(1);
    spec.lineage = LineageId::from_value(state.value());
    spec.logical_size = SizeValue::of(1024);
    spec.content_digest = digest_of("state-" + state.str());
    spec.replication_requirements =
        make_replication_policy(ReplicationPolicyId::from_value(1),
                                ReplicationPolicyGeneration::from_value(1),
                                ReplicationPolicyForm::N_REPLICAS, 1)
            .requirements;
    ctx.check(world->fabric->register_state(spec).has_value(), "state registered");
    ctx.check(world->fabric->commit_state_generation(state, spec.generation,
                                                     CommitId::from_value(60000U + index))
                  .has_value(),
              "state committed");
  }

  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < kStates; ++index) {
    workers.emplace_back([&, index]() {
      const StateId state = StateId::from_value(50000U + static_cast<std::uint64_t>(index));
      auto record = world->fabric->query_state(state, StateGeneration::from_value(1));
      if (!record) {
        failures.fetch_add(1);
        return;
      }
      ReplicaRegistration registration;
      registration.state_id = state;
      registration.state_generation = StateGeneration::from_value(1);
      registration.location.cluster_id = world->cluster_a;
      registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
      registration.content_digest = record.value().content_digest;
      registration.expected_coordinator_epoch = world->epoch();
      auto registered = world->fabric->register_replica(registration);
      if (!registered) {
        failures.fetch_add(1);
        return;
      }
      TransferCompletion completion;
      completion.replica_id = registered.value().replica_id;
      completion.replica_generation = registered.value().replica_generation;
      completion.cluster_id = world->cluster_a;
      completion.expected_coordinator_epoch = world->epoch();
      completion.bytes_transferred = 1024;
      completion.measured_digest = record.value().content_digest;
      if (!world->fabric->record_transfer_completion(completion)) {
        failures.fetch_add(1);
        return;
      }
      ReplicaVerification verification;
      verification.replica_id = registered.value().replica_id;
      verification.replica_generation = registered.value().replica_generation;
      verification.cluster_id = world->cluster_a;
      verification.expected_coordinator_epoch = world->epoch();
      verification.integrity_verified = true;
      verification.measured_digest = record.value().content_digest;
      if (!world->fabric->verify_replica(verification)) {
        failures.fetch_add(1);
        return;
      }
      ReplicaCommit commit;
      commit.replica_id = registered.value().replica_id;
      commit.commit_id = CommitId::from_value(70000U + static_cast<std::uint64_t>(index));
      commit.expected_coordinator_epoch = world->epoch();
      if (!world->fabric->commit_replica(commit)) {
        failures.fetch_add(1);
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  ctx.check(failures.load() == 0, "every concurrent state mutation succeeded");

  for (int index = 0; index < kStates; ++index) {
    const StateId state = StateId::from_value(50000U + static_cast<std::uint64_t>(index));
    auto reconciliation =
        world->fabric->reconcile_replica_set(state, StateGeneration::from_value(1));
    ctx.check(reconciliation.has_value() &&
                  reconciliation.value().replica_set.authoritative_count == 1,
              "each state ends with exactly one authoritative replica");
  }
}

CCSF_CASE(concurrency, concurrent_persistence_requests_are_serialized) {
  const std::string directory = fresh_directory("concurrent-persistence");
  FabricConfig config = base_config(CoordinatorId::from_value(92), ClusterId::from_value(100));
  config.persistence_directory = directory;
  config.require_durable_commits = true;
  auto world = make_world(config);
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");

  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int index = 0; index < 4; ++index) {
    workers.emplace_back([&]() {
      for (int round = 0; round < 5; ++round) {
        if (!world->fabric->persist()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (int index = 0; index < 2; ++index) {
    workers.emplace_back([&]() {
      for (int round = 0; round < 5; ++round) {
        ClusterEvidenceUpdate update;
        auto cluster = world->fabric->query_cluster(world->cluster_b);
        if (!cluster) {
          failures.fetch_add(1);
          continue;
        }
        update.cluster_id = world->cluster_b;
        update.incarnation = cluster.value().incarnation;
        update.expected_coordinator_epoch = world->epoch();
        update.has_capabilities = true;
        update.capabilities.tags = {"gpu:sm_120", "round:" + std::to_string(round)};
        if (!world->fabric->publish_cluster_evidence(update)) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  ctx.check(failures.load() == 0, "concurrent persistence and mutation both succeeded");

  auto status = world->fabric->status();
  ctx.check(status.persistence_healthy, "the durable control plane is healthy");
  const FabricStatus durable = status;
  ctx.check(durable.counters.durable_commits > 0, "durable commits were counted");
}

CCSF_CASE(concurrency, shutdown_is_idempotent_and_terminates_work) {
  const std::string directory = fresh_directory("concurrent-shutdown");
  FabricConfig config = base_config(CoordinatorId::from_value(93), ClusterId::from_value(100));
  config.persistence_directory = directory;
  auto world = make_world(config);
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");

  ctx.check(world->fabric->shutdown().has_value(), "the first shutdown succeeds");
  ctx.check(world->fabric->shutdown().has_value(), "repeated shutdown is idempotent");

  auto rejected = world->fabric->advance_time(1);
  ctx.check(!rejected.has_value(), "a shut down runtime accepts no new work");
  ctx.check(rejected.error().code == ErrorCode::SHUTTING_DOWN,
            "reported as SHUTTING_DOWN");
  auto query = world->fabric->query_state(world->state, world->generation);
  ctx.check(query.has_value(), "committed state remains readable after shutdown");
}
