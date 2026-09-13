// Cross-Cluster State Fabric - adversarial hardening proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Each case is a deliberate attack on the runtime. The obligation is that the
// attack is refused with a specific structured error and that no authority,
// membership or accounting state changes as a result.

#include <string>
#include <thread>
#include <vector>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

namespace {

std::uint64_t authoritative_count(Fabric& fabric, StateId state, StateGeneration generation) {
  auto reconciliation = fabric.reconcile_replica_set(state, generation);
  return reconciliation ? reconciliation.value().replica_set.authoritative_count : 0U;
}

}  // namespace

CCSF_CASE(adversarial, transfer_completion_after_source_death_is_refused) {
  auto world = make_world(base_config(CoordinatorId::from_value(80), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 4096, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  auto seeded = seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE);
  ctx.check(seeded.has_value(), "the source replica is committed");

  ReplicationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.destination_cluster = world->cluster_b;
  request.expected_coordinator_epoch = world->epoch();
  auto authorization = world->fabric->authorize_replication(request);
  ctx.check(authorization.has_value(), "authorized");
  const TransferAuthorization auth = authorization.value();

  // The source cluster dies while the transfer is in flight.
  auto source_cluster = world->fabric->query_cluster(auth.source_cluster);
  auto fenced = world->fabric->mark_cluster_unreachable(auth.source_cluster,
                                                        source_cluster.value().incarnation,
                                                        "source died mid transfer");
  ctx.check(fenced.has_value(), "the source cluster is fenced");

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
  completion.bytes_transferred = 4096;
  completion.measured_digest = digest_of("payload");
  auto transferred = world->fabric->record_transfer_completion(completion);
  ctx.check(!transferred.has_value() || transferred.value().commit_recorded == false,
            "a completion whose source died cannot become authoritative");
  ctx.check(authoritative_count(*world->fabric, world->state, world->generation) == 0,
            "no replica counts after the only source died");
}

CCSF_CASE(adversarial, conflicting_registration_and_replica_publication) {
  auto world = make_world(base_config(CoordinatorId::from_value(81), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 1024).has_value(), "state committed");

  StateGenerationSpec conflict = world->last_spec;
  conflict.content_digest = digest_of("different-content");
  auto rejected = world->fabric->register_state(conflict);
  ctx.check(!rejected.has_value(),
            "re-registering a generation with different content is a conflict");
  ctx.check(rejected.error().code == ErrorCode::DUPLICATE_CONFLICT,
            "reported as DUPLICATE_CONFLICT");

  ReplicaRegistration first;
  first.replica_id = ReplicaId::from_value(4242);
  first.state_id = world->state;
  first.state_generation = world->generation;
  first.location.cluster_id = world->cluster_a;
  first.lifecycle = ReplicaLifecycle::TRANSFERRING;
  first.expected_coordinator_epoch = world->epoch();
  auto created = world->fabric->register_replica(first);
  ctx.check(created.has_value(), "the first publication succeeds");
  auto repeated = world->fabric->register_replica(first);
  ctx.check(repeated.has_value(), "republishing the same replica is idempotent");

  ReplicaRegistration conflicting = first;
  conflicting.location.cluster_id = world->cluster_c;
  auto clash = world->fabric->register_replica(conflicting);
  ctx.check(!clash.has_value(), "publishing the same identity elsewhere is a conflict");
  ctx.check(clash.error().code == ErrorCode::DUPLICATE_CONFLICT,
            "reported as DUPLICATE_CONFLICT");
}

CCSF_CASE(adversarial, commit_after_compatibility_change_is_refused) {
  auto world = make_world(base_config(CoordinatorId::from_value(82), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  CompatibilityDescriptor required;
  required.set("accelerator", "sm_120");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2, required)
                .has_value(),
            "state with a compatibility requirement committed");
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

  // The destination reports a different accelerator than the requirement allows.
  ReplicaVerification verification;
  verification.replica_id = auth.destination_replica;
  verification.replica_generation = auth.destination_replica_generation;
  verification.cluster_id = auth.destination_cluster;
  verification.cluster_incarnation = auth.destination_incarnation;
  verification.expected_coordinator_epoch = world->epoch();
  verification.integrity_verified = true;
  verification.measured_digest = digest_of("payload");
  verification.compatibility_verified = true;
  verification.compatibility_descriptor.set("accelerator", "sm_90");
  auto verified = world->fabric->verify_replica(verification);
  ctx.check(!verified.has_value(),
            "a destination whose compatibility evidence disagrees is refused");
  ctx.check(verified.error().code == ErrorCode::INCOMPATIBLE,
            "reported as INCOMPATIBLE");

  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(world->ids.take());
  commit.expected_coordinator_epoch = world->epoch();
  auto committed = world->fabric->commit_operation(commit);
  ctx.check(!committed.has_value(), "the incompatible destination can never commit");
  ctx.check(authoritative_count(*world->fabric, world->state, world->generation) == 1,
            "the authoritative count is unchanged");
}

CCSF_CASE(adversarial, resource_bounds_are_enforced_before_allocation) {
  auto world = make_world(base_config(CoordinatorId::from_value(83), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");

  StateGenerationSpec oversized;
  oversized.state_id = StateId::from_value(1234);
  oversized.generation = StateGeneration::from_value(1);
  oversized.logical_size = SizeValue::of(1ULL << 60U);
  auto rejected = world->fabric->register_state(oversized);
  ctx.check(!rejected.has_value(), "an absurd declared size is refused");
  ctx.check(rejected.error().code == ErrorCode::RESOURCE_EXHAUSTED,
            "reported as RESOURCE_EXHAUSTED");

  StateGenerationSpec long_text;
  long_text.state_id = StateId::from_value(1235);
  long_text.generation = StateGeneration::from_value(1);
  long_text.provenance.producer = std::string(kMaxMetadataStringBytes + 1U, 'p');
  auto text_rejected = world->fabric->register_state(long_text);
  ctx.check(!text_rejected.has_value(), "oversized metadata is refused");
  ctx.check(text_rejected.error().code == ErrorCode::INVALID_ARGUMENT,
            "reported as INVALID_ARGUMENT");

  StateGenerationSpec impossible;
  impossible.state_id = StateId::from_value(1236);
  impossible.generation = StateGeneration::from_value(1);
  impossible.replication_requirements.minimum_authoritative_replicas = 5;
  impossible.replication_requirements.maximum_replicas = 2;
  auto impossible_rejected = world->fabric->register_state(impossible);
  ctx.check(!impossible_rejected.has_value(),
            "an impossible replication requirement is refused");

  ClusterRegistration unknown_cluster;
  unknown_cluster.cluster_id = ClusterId::from_value(4242);
  (void)world->fabric->register_cluster(unknown_cluster);
  ReplicaRegistration orphan;
  orphan.state_id = StateId::from_value(999999);
  orphan.state_generation = StateGeneration::from_value(1);
  orphan.location.cluster_id = ClusterId::from_value(4242);
  auto orphan_rejected = world->fabric->register_replica(orphan);
  ctx.check(!orphan_rejected.has_value(), "a replica of an unknown state is refused");
  ctx.check(orphan_rejected.error().code == ErrorCode::UNKNOWN_STATE,
            "reported as UNKNOWN_STATE");
}

CCSF_CASE(adversarial, concurrent_migrations_of_one_state_serialize) {
  auto world = make_world(base_config(CoordinatorId::from_value(84), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 4096, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");

  ReplicationRequest first;
  first.state_id = world->state;
  first.state_generation = world->generation;
  first.destination_cluster = world->cluster_b;
  first.expected_coordinator_epoch = world->epoch();
  ReplicationRequest second = first;
  second.destination_cluster = world->cluster_c;

  auto first_auth = world->fabric->authorize_replication(first);
  auto second_auth = world->fabric->authorize_replication(second);
  ctx.check(first_auth.has_value() && second_auth.has_value(),
            "both destinations can be authorized independently");

  // Completing the second operation invalidates the first operation's fence.
  const TransferAuthorization second_authorization = second_auth.value();
  TransferCompletion completion;
  completion.replica_id = second_authorization.destination_replica;
  completion.replica_generation = second_authorization.destination_replica_generation;
  completion.transfer_id = second_authorization.transfer_id;
  completion.transfer_generation = second_authorization.transfer_generation;
  completion.operation_id = second_authorization.operation_id;
  completion.operation_generation = second_authorization.operation_generation;
  completion.cluster_id = second_authorization.destination_cluster;
  completion.cluster_incarnation = second_authorization.destination_incarnation;
  completion.expected_coordinator_epoch = world->epoch();
  completion.bytes_transferred = 4096;
  completion.measured_digest = digest_of("payload");
  ctx.check(world->fabric->record_transfer_completion(completion).has_value(), "transferred");
  ReplicaVerification verification;
  verification.replica_id = second_authorization.destination_replica;
  verification.replica_generation = second_authorization.destination_replica_generation;
  verification.cluster_id = second_authorization.destination_cluster;
  verification.cluster_incarnation = second_authorization.destination_incarnation;
  verification.expected_coordinator_epoch = world->epoch();
  verification.integrity_verified = true;
  verification.measured_digest = digest_of("payload");
  ctx.check(world->fabric->verify_replica(verification).has_value(), "verified");

  OperationCommit commit;
  commit.operation_id = second_authorization.operation_id;
  commit.operation_generation = second_authorization.operation_generation;
  commit.commit_id = CommitId::from_value(world->ids.take());
  commit.expected_coordinator_epoch = world->epoch();
  ctx.check(world->fabric->commit_operation(commit).has_value(), "the second operation commits");

  const TransferAuthorization first_authorization = first_auth.value();
  OperationCommit stale_commit;
  stale_commit.operation_id = first_authorization.operation_id;
  stale_commit.operation_generation = first_authorization.operation_generation;
  stale_commit.commit_id = CommitId::from_value(world->ids.take());
  stale_commit.expected_coordinator_epoch = world->epoch();
  auto refused = world->fabric->commit_operation(stale_commit);
  ctx.check(!refused.has_value(),
            "a migration authorized against an older membership cannot commit blindly");
  ctx.check(refused.error().code == ErrorCode::STALE_REPLICA_SET_GENERATION,
            "reported as STALE_REPLICA_SET_GENERATION");
}

CCSF_CASE(adversarial, repeated_restart_and_retire_cycles_return_to_baseline) {
  auto world = make_world(base_config(CoordinatorId::from_value(85), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");

  const FabricCounters baseline = world->fabric->status().counters;
  for (int cycle = 0; cycle < 6; ++cycle) {
    auto cluster = world->fabric->query_cluster(world->cluster_c);
    ctx.check(cluster.has_value(), "cluster C is registered");
    ClusterRegistration registration = make_cluster_registration(
        world->ids, world->cluster_c, world->region_two, FailureDomainId::from_value(3),
        WorkerBootId::from_value(world->ids.take()), ClusterHealthState::HEALTHY, {"gpu:sm_120"});
    auto reincarnated = world->fabric->register_cluster(registration);
    ctx.check(reincarnated.has_value(), "cluster C reincarnates");
    ctx.check(reincarnated.value().reincarnation_count ==
                  static_cast<std::uint32_t>(cycle) + 1U,
              "each reincarnation advances the incarnation count");
  }

  const FabricCounters after = world->fabric->status().counters;
  ctx.check(after.cluster_reincarnations - baseline.cluster_reincarnations == 6,
            "exactly six reincarnations were counted");
  ctx.check(after.active_operations == baseline.active_operations,
            "no operation was left active by the restart cycles");
  ctx.check(after.reserved_bytes == baseline.reserved_bytes,
            "reserved byte accounting returned to baseline");
  ctx.check(authoritative_count(*world->fabric, world->state, world->generation) == 1,
            "the surviving authoritative replica is unaffected by restart cycles");
}
