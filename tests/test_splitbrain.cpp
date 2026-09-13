// Cross-Cluster State Fabric - fencing and split-brain proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

CCSF_CASE(splitbrain, coordinator_epoch_fences_every_older_request) {
  auto world = make_world(base_config(CoordinatorId::from_value(60), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  auto seeded = seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE);
  ctx.check(seeded.has_value(), "seeded");
  const CoordinatorEpoch first_epoch = world->epoch();

  ReplicationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.destination_cluster = world->cluster_b;
  request.expected_coordinator_epoch = first_epoch;
  auto authorization = world->fabric->authorize_replication(request);
  ctx.check(authorization.has_value(), "authorized under the first epoch");
  const TransferAuthorization auth = authorization.value();

  auto advanced = world->fabric->advance_epoch();
  ctx.check(advanced.has_value(), "the coordinator advanced its epoch");

  TransferCompletion completion;
  completion.replica_id = auth.destination_replica;
  completion.replica_generation = auth.destination_replica_generation;
  completion.transfer_id = auth.transfer_id;
  completion.transfer_generation = auth.transfer_generation;
  completion.operation_id = auth.operation_id;
  completion.operation_generation = auth.operation_generation;
  completion.cluster_id = auth.destination_cluster;
  completion.cluster_incarnation = auth.destination_incarnation;
  completion.expected_coordinator_epoch = first_epoch;
  completion.bytes_transferred = 2048;
  completion.measured_digest = digest_of("payload");
  auto stale = world->fabric->record_transfer_completion(completion);
  ctx.check(!stale.has_value(), "a completion from an older epoch is rejected");
  ctx.check(stale.error().code == ErrorCode::STALE_EPOCH, "rejected as STALE_EPOCH");

  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(world->ids.take());
  commit.expected_coordinator_epoch = first_epoch;
  auto stale_commit = world->fabric->commit_operation(commit);
  ctx.check(!stale_commit.has_value(), "a commit from an older epoch is rejected");
  ctx.check(stale_commit.error().code == ErrorCode::STALE_EPOCH, "rejected as STALE_EPOCH");

  auto reconciliation = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(reconciliation.value().replica_set.authoritative_count == 1,
            "the stale epoch changed no authority");
}

CCSF_CASE(splitbrain, worker_boot_restart_fences_authority) {
  auto world = make_world(base_config(CoordinatorId::from_value(61), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");

  ReplicationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.destination_cluster = world->cluster_b;
  request.expected_coordinator_epoch = world->epoch();
  auto authorization = world->fabric->authorize_replication(request);
  ctx.check(authorization.has_value(), "authorized");

  ClusterEvidenceUpdate update;
  update.cluster_id = world->cluster_b;
  update.incarnation = authorization.value().destination_incarnation;
  update.worker_boot_id = WorkerBootId::from_value(999999);
  update.expected_coordinator_epoch = world->epoch();
  auto rejected = world->fabric->publish_cluster_evidence(update);
  ctx.check(!rejected.has_value(), "evidence naming a different worker boot is rejected");
  ctx.check(rejected.error().code == ErrorCode::STALE_WORKER, "rejected as STALE_WORKER");
}

CCSF_CASE(splitbrain, stale_generations_are_rejected) {
  auto world = make_world(base_config(CoordinatorId::from_value(62), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  auto seeded = seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE);
  ctx.check(seeded.has_value(), "seeded");

  auto record = world->fabric->query_replica(seeded.value());
  ctx.check(record.has_value(), "replica recorded");

  ReplicaCommit stale_replica;
  stale_replica.replica_id = seeded.value();
  stale_replica.replica_generation =
      ReplicaGeneration::from_value(record.value().replica_generation.value() + 7U);
  stale_replica.commit_id = CommitId::from_value(world->ids.take());
  stale_replica.expected_coordinator_epoch = world->epoch();
  auto rejected = world->fabric->commit_replica(stale_replica);
  ctx.check(!rejected.has_value(), "a commit naming a superseded replica generation is rejected");
  ctx.check(rejected.error().code == ErrorCode::STALE_REPLICA_GENERATION,
            "rejected as STALE_REPLICA_GENERATION");

  ReplicationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.destination_cluster = world->cluster_b;
  request.expected_coordinator_epoch = world->epoch();
  auto authorization = world->fabric->authorize_replication(request);
  ctx.check(authorization.has_value(), "authorized");
  const TransferAuthorization auth = authorization.value();

  TransferProgress stale_transfer;
  stale_transfer.transfer_id = auth.transfer_id;
  stale_transfer.transfer_generation =
      TransferGeneration::from_value(auth.transfer_generation.value() + 5U);
  stale_transfer.replica_id = auth.destination_replica;
  stale_transfer.cluster_id = auth.destination_cluster;
  stale_transfer.expected_coordinator_epoch = world->epoch();
  stale_transfer.bytes_transferred = 1;
  auto stale_progress = world->fabric->record_transfer_progress(stale_transfer);
  ctx.check(!stale_progress.has_value(), "progress from a superseded transfer is rejected");
  ctx.check(stale_progress.error().code == ErrorCode::STALE_TRANSFER_GENERATION,
            "rejected as STALE_TRANSFER_GENERATION");

  ReplicaCommit stale_set;
  stale_set.replica_id = auth.destination_replica;
  stale_set.commit_id = CommitId::from_value(world->ids.take());
  stale_set.expected_coordinator_epoch = world->epoch();
  stale_set.expected_replica_set_generation =
      ReplicaSetGeneration::from_value(auth.replica_set_generation.value() + 11U);
  auto set_rejected = world->fabric->commit_replica(stale_set);
  ctx.check(!set_rejected.has_value(),
            "a commit naming a superseded replica set generation is rejected");
}

CCSF_CASE(splitbrain, only_one_current_generation_per_lineage) {
  auto world = make_world(base_config(CoordinatorId::from_value(63), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "one", 1024).has_value(), "generation one committed");

  StateGenerationSpec second;
  second.state_id = world->state;
  second.generation = StateGeneration::from_value(2);
  second.lineage = LineageId::from_value(world->state.value());
  second.content_digest = digest_of("two");
  ctx.check(world->fabric->register_state(second).has_value(), "second generation registered");
  ctx.check(world->fabric->commit_state_generation(world->state, second.generation,
                                                   CommitId::from_value(world->ids.take()))
                .has_value(),
            "second generation committed");

  StateGenerationSpec third;
  third.state_id = world->state;
  third.generation = StateGeneration::from_value(3);
  third.lineage = LineageId::from_value(world->state.value());
  third.content_digest = digest_of("three");
  ctx.check(world->fabric->register_state(third).has_value(), "third generation registered");
  ctx.check(world->fabric->commit_state_generation(world->state, third.generation,
                                                   CommitId::from_value(world->ids.take()))
                .has_value(),
            "third generation committed");

  auto lineage = world->fabric->query_lineage(world->state);
  ctx.check(lineage.has_value(), "lineage query");
  std::uint32_t current = 0;
  for (const StateGenerationRecord& record : lineage.value().generations) {
    if (record.is_current() && !record.branch) {
      ++current;
    }
  }
  ctx.check(current == 1, "exactly one non-branched generation is current");
  ctx.check(lineage.value().current_generation == third.generation,
            "the latest committed generation is the current one");
  ctx.check(lineage.value().current_branches.empty(), "no branch was created implicitly");
}

CCSF_CASE(splitbrain, unreachable_cluster_loses_authority_but_keeps_history) {
  auto world = make_world(base_config(CoordinatorId::from_value(64), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "cluster A seeded");
  auto replicated = replicate(*world, world->cluster_b);
  ctx.check(replicated.has_value(), "cluster B replicated");

  auto cluster = world->fabric->query_cluster(world->cluster_b);
  ctx.check(cluster.has_value(), "cluster B is registered");
  auto marked = world->fabric->mark_cluster_unreachable(world->cluster_b,
                                                        cluster.value().incarnation,
                                                        "link down");
  ctx.check(marked.has_value(), "cluster B is marked unreachable");
  ctx.check(!marked.value().live_authority, "cluster B holds no live authority");

  auto replica_b = world->fabric->query_replica(replicated.value());
  ctx.check(replica_b.value().commit_recorded,
            "the committed history of the B replica survives the loss");
  ctx.check(!replica_b.value().counts_toward_factor,
            "the B replica no longer counts toward the replication factor");

  auto set = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(set.value().replica_set.authoritative_count == 1,
            "the replica set degrades to the surviving authoritative replica");
  ctx.check(set.value().under_replicated, "under-replication is reported explicitly");

  auto reuse_b = world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_b);
  ctx.check(reuse_b.value().decision == DecisionKind::REJECT,
            "the lost cluster cannot serve reuse");
  auto reuse_a = world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_a);
  ctx.check(reuse_a.value().decision != DecisionKind::REJECT,
            "the surviving cluster remains authoritative");

  auto stale = world->fabric->mark_cluster_unreachable(
      world->cluster_b, ClusterIncarnationId::from_value(cluster.value().incarnation.value() + 3U),
      "stale claim");
  ctx.check(!stale.has_value(), "a stale incarnation cannot act on the cluster");
  ctx.check(stale.error().code == ErrorCode::STALE_CLUSTER_INCARNATION,
            "rejected as STALE_CLUSTER_INCARNATION");
}
