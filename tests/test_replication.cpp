// Cross-Cluster State Fabric - replication transaction proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

CCSF_CASE(replication, destination_counts_only_after_commit) {
  auto world = make_world(base_config(CoordinatorId::from_value(20), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 8192, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  auto seeded = seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE);
  ctx.check(seeded.has_value(), "cluster A holds the authoritative source");

  auto before = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(before.value().replica_set.authoritative_count == 1,
            "the replica set starts with exactly one authoritative replica");
  const ReplicaSetGeneration first_generation = before.value().replica_set.generation;

  ReplicationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.destination_cluster = world->cluster_b;
  request.expected_coordinator_epoch = world->epoch();
  auto authorization = world->fabric->authorize_replication(request);
  ctx.check(authorization.has_value(), "replication is authorized to cluster B");
  const TransferAuthorization auth = authorization.value();
  ctx.check(auth.expected_digest == digest_of("payload"), "the authorization pins the digest");
  ctx.check(auth.destructive_move == false, "replication preserves source authority");

  TransferProgress progress;
  progress.transfer_id = auth.transfer_id;
  progress.transfer_generation = auth.transfer_generation;
  progress.replica_id = auth.destination_replica;
  progress.replica_generation = auth.destination_replica_generation;
  progress.operation_id = auth.operation_id;
  progress.operation_generation = auth.operation_generation;
  progress.cluster_id = auth.destination_cluster;
  progress.cluster_incarnation = auth.destination_incarnation;
  progress.expected_coordinator_epoch = world->epoch();
  progress.bytes_transferred = 4096;
  ctx.check(world->fabric->record_transfer_progress(progress).has_value(), "transfer starts");

  auto during = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(during.value().replica_set.authoritative_count == 1,
            "a transferring replica does not count toward the replication factor");

  TransferCompletion completion;
  completion.transfer_id = auth.transfer_id;
  completion.transfer_generation = auth.transfer_generation;
  completion.replica_id = auth.destination_replica;
  completion.replica_generation = auth.destination_replica_generation;
  completion.operation_id = auth.operation_id;
  completion.operation_generation = auth.operation_generation;
  completion.cluster_id = auth.destination_cluster;
  completion.cluster_incarnation = auth.destination_incarnation;
  completion.expected_coordinator_epoch = world->epoch();
  completion.bytes_transferred = 8192;
  completion.measured_digest = digest_of("payload");
  ctx.check(world->fabric->record_transfer_completion(completion).has_value(),
            "the transfer completes at the destination");

  auto transferred = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(transferred.value().replica_set.authoritative_count == 1,
            "a transferred but unverified replica does not count");

  ReplicaCommit premature;
  premature.replica_id = auth.destination_replica;
  premature.commit_id = CommitId::from_value(world->ids.take());
  premature.expected_coordinator_epoch = world->epoch();
  auto rejected = world->fabric->commit_replica(premature);
  ctx.check(!rejected.has_value(), "a replica cannot be committed before verification");
  ctx.check(rejected.error().code != ErrorCode::OK, "the rejection is explicit");

  ReplicaVerification verification;
  verification.replica_id = auth.destination_replica;
  verification.replica_generation = auth.destination_replica_generation;
  verification.operation_id = auth.operation_id;
  verification.operation_generation = auth.operation_generation;
  verification.cluster_id = auth.destination_cluster;
  verification.cluster_incarnation = auth.destination_incarnation;
  verification.expected_coordinator_epoch = world->epoch();
  verification.integrity_verified = true;
  verification.measured_digest = digest_of("payload");
  verification.verified_bytes = 8192;
  verification.established_durability = DurabilityClass::DURABLE;
  ctx.check(world->fabric->verify_replica(verification).has_value(),
            "the destination verifies against the authoritative digest");

  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(world->ids.take());
  commit.expected_coordinator_epoch = world->epoch();
  auto committed = world->fabric->commit_operation(commit);
  ctx.check(committed.has_value(), "the replication commits transactionally");
  ctx.check(committed.value().state == OperationState::COMPLETE, "the operation completes");

  auto after = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(after.value().replica_set.authoritative_count == 2,
            "the committed destination now counts toward the replication factor");
  ctx.check(after.value().replica_set.generation > first_generation,
            "the replica set generation advanced with the membership change");
  ctx.check(after.value().replica_set.distinct_clusters == 2,
            "the replica set now spans two clusters");

  auto reuse_b = world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_b);
  ctx.check(reuse_b.value().decision != DecisionKind::REJECT,
            "cluster B may now serve reuse");
  ctx.check(reuse_b.value().assessment.authoritative_for_reuse, "cluster B holds authority");
}

CCSF_CASE(replication, duplicate_commit_is_idempotent_and_conflicts_reject) {
  auto world = make_world(base_config(CoordinatorId::from_value(21), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 4096, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");
  auto destination = replicate(*world, world->cluster_b);
  ctx.check(destination.has_value(), "replication succeeds");

  auto replicas = world->fabric->list_replicas(world->state, world->generation);
  ctx.check(replicas.has_value() && replicas.value().size() == 2, "two replicas exist");
  ReplicaId second;
  for (const ReplicaRecord& record : replicas.value()) {
    if (record.replica_id != replicas.value().front().replica_id) {
      second = record.replica_id;
    }
  }
  auto record = world->fabric->query_replica(second);
  ctx.check(record.has_value(), "the destination replica is queryable");

  ReplicaCommit repeated;
  repeated.replica_id = record.value().replica_id;
  repeated.commit_id = record.value().commit_id;
  repeated.expected_coordinator_epoch = world->epoch();
  auto idempotent = world->fabric->commit_replica(repeated);
  ctx.check(idempotent.has_value(), "re-committing with the same commit identity is idempotent");

  ReplicaCommit conflicting;
  conflicting.replica_id = record.value().replica_id;
  conflicting.commit_id = CommitId::from_value(world->ids.take());
  conflicting.expected_coordinator_epoch = world->epoch();
  auto conflict = world->fabric->commit_replica(conflicting);
  ctx.check(!conflict.has_value(), "a conflicting duplicate commit is rejected");
  ctx.check(conflict.error().code == ErrorCode::COMMIT_CONFLICT,
            "reported as COMMIT_CONFLICT");

  auto set = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(set.value().replica_set.authoritative_count == 2,
            "the conflict did not change the authoritative count");
}

CCSF_CASE(replication, ineligible_destination_is_rejected_by_hard_constraint) {
  auto world = make_world(base_config(CoordinatorId::from_value(22), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 4096, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");

  ClusterRegistration unhealthy = make_cluster_registration(
      world->ids, world->cluster_c, world->region_two, FailureDomainId::from_value(3),
      WorkerBootId::from_value(world->ids.take()), ClusterHealthState::FAILED);
  ctx.check(world->fabric->register_cluster(unhealthy).has_value(), "cluster C registered unhealthy");

  ReplicationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.destination_cluster = world->cluster_c;
  request.expected_coordinator_epoch = world->epoch();
  auto authorization = world->fabric->authorize_replication(request);
  ctx.check(!authorization.has_value(), "an unhealthy destination is refused");
  ctx.check(authorization.error().code == ErrorCode::DESTINATION_INELIGIBLE ||
                authorization.error().code == ErrorCode::STALE_CLUSTER_INCARNATION,
            "refused as an ineligible destination");
}

CCSF_CASE(replication, destination_never_exceeds_the_per_state_bound) {
  auto world = make_world(base_config(CoordinatorId::from_value(23), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 1024, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");
  ctx.check(replicate(*world, world->cluster_b).has_value(), "first replica added");

  ReplicationPolicyRecord policy = default_replication_policy(world->ids.take(), 4);
  policy.retention.maximum_replicas_per_state = 2;
  ctx.check(world->fabric->publish_replication_policy(policy).has_value(), "policy published");

  ReplicationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.destination_cluster = world->cluster_c;
  request.expected_coordinator_epoch = world->epoch();
  request.replication_policy = policy;
  auto authorization = world->fabric->authorize_replication(request);
  ctx.check(authorization.has_value(), "the third replica is authorized");
  const TransferAuthorization auth = authorization.value();
  TransferCompletion completion;
  completion.replica_id = auth.destination_replica;
  completion.replica_generation = auth.destination_replica_generation;
  completion.transfer_id = auth.transfer_id;
  completion.transfer_generation = auth.transfer_generation;
  completion.operation_id = auth.operation_id;
  completion.operation_generation = auth.operation_generation;
  completion.cluster_id = auth.destination_cluster;
  completion.cluster_incarnation = auth.destination_incarnation;
  completion.expected_coordinator_epoch = world->epoch();
  completion.bytes_transferred = 1024;
  completion.measured_digest = digest_of("payload");
  ctx.check(world->fabric->record_transfer_completion(completion).has_value(), "transferred");
  ReplicaVerification verification;
  verification.replica_id = auth.destination_replica;
  verification.replica_generation = auth.destination_replica_generation;
  verification.cluster_id = auth.destination_cluster;
  verification.cluster_incarnation = auth.destination_incarnation;
  verification.expected_coordinator_epoch = world->epoch();
  verification.integrity_verified = true;
  verification.measured_digest = digest_of("payload");
  ctx.check(world->fabric->verify_replica(verification).has_value(), "verified");
  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(world->ids.take());
  commit.expected_coordinator_epoch = world->epoch();
  auto committed = world->fabric->commit_operation(commit);
  ctx.check(!committed.has_value(), "exceeding the per-state replica bound is refused");
  ctx.check(committed.error().code == ErrorCode::OVER_REPLICATED,
            "reported as OVER_REPLICATED");
}

CCSF_CASE(replication, under_replication_is_explicit) {
  auto world = make_world(base_config(CoordinatorId::from_value(24), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 1024, ConsistencyMode::IMMUTABLE, 3).has_value(),
            "state committed with a minimum of three replicas");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");
  auto reconciliation = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(reconciliation.has_value(), "reconciliation is available");
  ctx.check(reconciliation.value().under_replicated, "one replica against three is under replicated");
  ctx.check(reconciliation.value().authoritative_shortfall == 2,
            "the shortfall is reported exactly");
  ctx.check(reconciliation.value().replica_set.state == ReplicaSetState::UNDER_REPLICATED,
            "the replica set state is explicitly under replicated");
}
