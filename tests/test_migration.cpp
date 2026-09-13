// Cross-Cluster State Fabric - migration transaction proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

namespace {

struct MigrationRun {
  TransferAuthorization authorization;
  ReplicaId source_replica;
};

}  // namespace

CCSF_CASE(migration, single_writer_ownership_moves_only_through_handoff) {
  auto world = make_world(base_config(CoordinatorId::from_value(30), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 4096, ConsistencyMode::SINGLE_WRITER, 1).has_value(),
            "a single-writer state is committed");
  auto seeded = seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE, true);
  ctx.check(seeded.has_value(), "cluster A holds the authoritative owner replica");

  ReplicaRegistration plain;
  plain.state_id = world->state;
  plain.state_generation = world->generation;
  plain.location.cluster_id = world->cluster_b;
  plain.lifecycle = ReplicaLifecycle::TRANSFERRING;
  plain.expected_coordinator_epoch = world->epoch();
  auto registered = world->fabric->register_replica(plain);
  ctx.check(registered.has_value(), "a second copy may exist for migration");
  TransferCompletion completion;
  completion.replica_id = registered.value().replica_id;
  completion.replica_generation = registered.value().replica_generation;
  completion.cluster_id = world->cluster_b;
  completion.expected_coordinator_epoch = world->epoch();
  completion.bytes_transferred = 4096;
  completion.measured_digest = digest_of("payload");
  ctx.check(world->fabric->record_transfer_completion(completion).has_value(), "bytes transferred");
  ReplicaVerification verification;
  verification.replica_id = registered.value().replica_id;
  verification.replica_generation = registered.value().replica_generation;
  verification.cluster_id = world->cluster_b;
  verification.expected_coordinator_epoch = world->epoch();
  verification.integrity_verified = true;
  verification.measured_digest = digest_of("payload");
  ctx.check(world->fabric->verify_replica(verification).has_value(), "destination verified");

  ReplicaCommit no_owner;
  no_owner.replica_id = registered.value().replica_id;
  no_owner.commit_id = CommitId::from_value(world->ids.take());
  no_owner.expected_coordinator_epoch = world->epoch();
  auto rejected = world->fabric->commit_replica(no_owner);
  ctx.check(!rejected.has_value(),
            "a replica of single-writer state cannot become authoritative without ownership");
  ctx.check(rejected.error().code == ErrorCode::POLICY_VIOLATION,
            "reported as a policy violation");

  ReplicaCommit claim;
  claim.replica_id = registered.value().replica_id;
  claim.commit_id = CommitId::from_value(world->ids.take());
  claim.expected_coordinator_epoch = world->epoch();
  claim.make_authoritative_owner = true;
  auto conflict = world->fabric->commit_replica(claim);
  ctx.check(!conflict.has_value(), "a second ownership claim is refused while one is active");
  ctx.check(conflict.error().code == ErrorCode::SPLIT_BRAIN_RISK,
            "reported as SPLIT_BRAIN_RISK");

  OwnershipTransferRequest handoff;
  handoff.state_id = world->state;
  handoff.state_generation = world->generation;
  handoff.expected_current_owner = seeded.value();
  handoff.new_owner_replica = registered.value().replica_id;
  handoff.new_owner_cluster = world->cluster_b;
  handoff.handoff_commit = CommitId::from_value(world->ids.take());
  handoff.expected_coordinator_epoch = world->epoch();
  auto transferred = world->fabric->transfer_ownership(handoff);
  ctx.check(transferred.has_value(), "ownership can be handed over explicitly");
  ctx.check(transferred.value().owner_replica == registered.value().replica_id,
            "the new owner is recorded");

  auto old_owner = world->fabric->query_replica(seeded.value());
  ctx.check(old_owner.value().authority == ReplicaAuthorityState::AUTHORITATIVE_FOR_REUSE,
            "the previous owner keeps reuse authority but loses ownership");
  auto new_owner = world->fabric->query_replica(registered.value().replica_id);
  ctx.check(new_owner.value().authority == ReplicaAuthorityState::AUTHORITATIVE_OWNER,
            "the new owner holds single-writer authority");

  auto repeated = world->fabric->transfer_ownership(handoff);
  ctx.check(repeated.has_value(), "repeating the handoff with the same commit is idempotent");
}

CCSF_CASE(migration, source_is_not_retired_before_commit) {
  auto world = make_world(base_config(CoordinatorId::from_value(31), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::SINGLE_WRITER, 1).has_value(),
            "single-writer state committed");
  auto seeded = seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE, true);
  ctx.check(seeded.has_value(), "owner seeded");

  ReplicaTransition retire_owner;
  retire_owner.replica_id = seeded.value();
  retire_owner.expected_coordinator_epoch = world->epoch();
  retire_owner.reason = "premature retirement attempt";
  auto refused = world->fabric->retire_replica(retire_owner);
  ctx.check(!refused.has_value(), "the authoritative owner cannot be retired in place");
  ctx.check(refused.error().code == ErrorCode::AUTHORITY_NOT_ESTABLISHED,
            "the operator must hand over ownership first");

  MigrationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.destination_cluster = world->cluster_b;
  request.retire_source_after_commit = true;
  request.expected_coordinator_epoch = world->epoch();
  auto authorization = world->fabric->authorize_migration(request);
  ctx.check(authorization.has_value(), "migration authorized");
  const TransferAuthorization auth = authorization.value();
  ctx.check(auth.destructive_move, "the authorization records destructive move semantics");

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
  completion.bytes_transferred = 2048;
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

  auto source_before = world->fabric->query_replica(auth.source_replica);
  ctx.check(source_before.value().lifecycle != ReplicaLifecycle::RETIRED,
            "the source is still live before the destination commit");
  auto reuse_before =
      world->fabric->evaluate_reuse(world->state, world->generation, auth.source_cluster);
  ctx.check(reuse_before.value().decision != DecisionKind::REJECT,
            "the source remains authoritative before the commit");

  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(world->ids.take());
  commit.expected_coordinator_epoch = world->epoch();
  commit.retire_source = true;
  auto committed = world->fabric->commit_operation(commit);
  ctx.check(committed.has_value(), "the migration commits");
  ctx.check(committed.value().source_retired, "the source is retired as part of the commit");

  auto source_after = world->fabric->query_replica(auth.source_replica);
  ctx.check(source_after.value().lifecycle == ReplicaLifecycle::RETIRED,
            "the source is retired only after the destination commit");
  ctx.check(source_after.value().authority == ReplicaAuthorityState::HISTORICAL,
            "the retired source keeps historical provenance");
  auto reuse_after =
      world->fabric->evaluate_reuse(world->state, world->generation, auth.destination_cluster);
  ctx.check(reuse_after.value().decision != DecisionKind::REJECT,
            "the destination serves reuse after the commit");
}

CCSF_CASE(migration, stale_completion_after_membership_change_is_refused) {
  auto world = make_world(base_config(CoordinatorId::from_value(32), ClusterId::from_value(100)));
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
  const TransferAuthorization auth = authorization.value();

  // A later authoritative membership change moves the replica set generation on.
  auto competing = replicate(*world, world->cluster_c);
  ctx.check(competing.has_value(), "a later replication commits into cluster C");
  auto moved = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(moved.value().replica_set.generation > auth.replica_set_generation,
            "the replica set generation advanced with the authoritative membership change");

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
  completion.bytes_transferred = 2048;
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
  ctx.check(!committed.has_value(),
            "a completion whose replica set generation moved on cannot commit");
  ctx.check(committed.error().code == ErrorCode::STALE_REPLICA_SET_GENERATION,
            "reported as STALE_REPLICA_SET_GENERATION");
}

CCSF_CASE(migration, cancellation_is_real_and_never_undoes_a_commit) {
  auto world = make_world(base_config(CoordinatorId::from_value(33), ClusterId::from_value(100)));
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
  const TransferAuthorization auth = authorization.value();

  OperationCancellation cancellation;
  cancellation.operation_id = auth.operation_id;
  cancellation.operation_generation = auth.operation_generation;
  cancellation.expected_coordinator_epoch = world->epoch();
  cancellation.reason = "operator cancelled";
  auto cancelled = world->fabric->cancel_operation(cancellation);
  ctx.check(cancelled.has_value(), "cancellation is accepted");
  ctx.check(cancelled.value().state == OperationState::CANCELLED, "the operation is cancelled");
  ctx.check(cancelled.value().cancellation == CancellationOutcome::CANCELLED_BEFORE_TRANSFER,
            "the cancellation outcome names the phase");

  auto destination = world->fabric->query_replica(auth.destination_replica);
  ctx.check(destination.value().lifecycle == ReplicaLifecycle::ABANDONED,
            "the destination copy is abandoned");

  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(world->ids.take());
  commit.expected_coordinator_epoch = world->epoch();
  auto committed = world->fabric->commit_operation(commit);
  ctx.check(!committed.has_value(), "a cancelled replication never joins the replica set");
  ctx.check(committed.error().code == ErrorCode::COMMIT_CONFLICT,
            "reported as COMMIT_CONFLICT");

  auto set = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(set.value().replica_set.authoritative_count == 1,
            "the cancelled destination never counted");
}

CCSF_CASE(migration, cancellation_after_commit_cannot_undo_authority) {
  auto world = make_world(base_config(CoordinatorId::from_value(34), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");
  auto destination = replicate(*world, world->cluster_b);
  ctx.check(destination.has_value(), "replicated");

  auto operations = world->fabric->list_operations();
  ctx.check(operations.has_value() && !operations.value().empty(), "operations recorded");
  const OperationRecord completed = operations.value().back();

  OperationCancellation cancellation;
  cancellation.operation_id = completed.operation_id;
  cancellation.operation_generation = completed.operation_generation;
  cancellation.expected_coordinator_epoch = world->epoch();
  cancellation.reason = "late cancellation";
  auto result = world->fabric->cancel_operation(cancellation);
  ctx.check(result.has_value(), "the cancellation request is classified rather than failing");
  ctx.check(result.value().cancellation == CancellationOutcome::COMMIT_ALREADY_AUTHORITATIVE,
            "the outcome records that authority was already committed");
  ctx.check(result.value().state == OperationState::COMPLETE,
            "committed authority is not silently undone");

  auto set = world->fabric->reconcile_replica_set(world->state, world->generation);
  ctx.check(set.value().replica_set.authoritative_count == 2,
            "the committed replica still counts");
}
