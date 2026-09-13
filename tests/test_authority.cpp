// Cross-Cluster State Fabric - authority and reuse proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

CCSF_CASE(authority, presence_is_not_authority) {
  auto world = make_world(base_config(CoordinatorId::from_value(10), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048).has_value(), "state committed");

  ReplicaRegistration registration;
  registration.state_id = world->state;
  registration.state_generation = world->generation;
  registration.location.cluster_id = world->cluster_a;
  registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
  registration.expected_coordinator_epoch = world->epoch();
  auto registered = world->fabric->register_replica(registration);
  ctx.check(registered.has_value(), "a staging replica registers");

  auto reuse = world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_a);
  ctx.check(reuse.has_value(), "reuse evaluation always answers");
  ctx.check(reuse.value().decision == DecisionKind::REJECT,
            "a replica that is merely transferring cannot serve reuse");
  ctx.check(!reuse.value().assessment.authoritative_for_reuse,
            "transfer progress confers no authority");

  TransferCompletion completion;
  completion.replica_id = registered.value().replica_id;
  completion.replica_generation = registered.value().replica_generation;
  completion.cluster_id = world->cluster_a;
  completion.expected_coordinator_epoch = world->epoch();
  completion.bytes_transferred = 2048;
  completion.measured_digest = digest_of("payload");
  ctx.check(world->fabric->record_transfer_completion(completion).has_value(),
            "the transfer completes");
  auto after_transfer =
      world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_a);
  ctx.check(after_transfer.value().decision == DecisionKind::REJECT,
            "a completed transfer is still not authority");
  ctx.check(after_transfer.value().code == ErrorCode::AUTHORITY_NOT_ESTABLISHED ||
                after_transfer.value().code == ErrorCode::INTEGRITY_UNKNOWN ||
                after_transfer.value().code == ErrorCode::STALE_REPLICA_SET_GENERATION,
            "the rejection names a specific missing prerequisite");
}

CCSF_CASE(authority, verified_committed_replica_serves_reuse) {
  auto world = make_world(base_config(CoordinatorId::from_value(11), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048).has_value(), "state committed");
  auto seeded = seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE);
  ctx.check(seeded.has_value(), "an authoritative replica is seeded in cluster A");

  auto reuse = world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_a);
  ctx.check(reuse.value().decision != DecisionKind::REJECT, "the authoritative replica serves reuse");
  ctx.check(reuse.value().assessment.integrity_verified, "integrity is verified");
  ctx.check(reuse.value().assessment.compatibility_verified, "compatibility is verified");
  ctx.check(reuse.value().assessment.current_for_state_generation,
            "the replica is current for the generation");
  ctx.check(reuse.value().assessment.member_of_current_replica_set_generation,
            "the replica is a member of the current replica set generation");

  auto other = world->fabric->evaluate_reuse(world->state, world->generation, world->cluster_b);
  ctx.check(other.value().decision == DecisionKind::REJECT,
            "a cluster without a replica cannot serve reuse");
  ctx.check(other.value().code == ErrorCode::NO_AUTHORITATIVE_REPLICA ||
                other.value().code == ErrorCode::UNKNOWN_CLUSTER,
            "the rejection names the missing authority");
}

CCSF_CASE(authority, integrity_mismatch_fences_the_destination) {
  auto world = make_world(base_config(CoordinatorId::from_value(12), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 2048).has_value(), "state committed");

  ReplicaRegistration registration;
  registration.state_id = world->state;
  registration.state_generation = world->generation;
  registration.location.cluster_id = world->cluster_b;
  registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
  registration.expected_coordinator_epoch = world->epoch();
  auto registered = world->fabric->register_replica(registration);
  ctx.check(registered.has_value(), "replica registers");

  TransferCompletion completion;
  completion.replica_id = registered.value().replica_id;
  completion.replica_generation = registered.value().replica_generation;
  completion.cluster_id = world->cluster_b;
  completion.expected_coordinator_epoch = world->epoch();
  completion.bytes_transferred = 2048;
  completion.measured_digest = digest_of("corrupted-payload");
  auto transferred = world->fabric->record_transfer_completion(completion);
  ctx.check(!transferred.has_value(), "a digest mismatch fails the transfer completion");
  ctx.check(transferred.error().code == ErrorCode::INTEGRITY_MISMATCH,
            "reported as INTEGRITY_MISMATCH");

  auto record = world->fabric->query_replica(registered.value().replica_id);
  ctx.check(record.value().integrity == ReplicaIntegrityState::MISMATCH,
            "the mismatch is durably recorded");
  ctx.check(record.value().lifecycle == ReplicaLifecycle::CORRUPT,
            "the destination is marked corrupt");

  ReplicaCommit commit;
  commit.replica_id = registered.value().replica_id;
  commit.commit_id = CommitId::from_value(world->ids.take());
  commit.expected_coordinator_epoch = world->epoch();
  auto committed = world->fabric->commit_replica(commit);
  ctx.check(!committed.has_value(), "a corrupt destination can never be committed");
  ctx.check(committed.error().code == ErrorCode::TRANSFER_INCOMPLETE ||
                committed.error().code == ErrorCode::INTEGRITY_UNKNOWN,
            "the commit rejection names a transfer or integrity prerequisite");
}

CCSF_CASE(authority, incompatible_destination_is_quarantined) {
  auto world = make_world(base_config(CoordinatorId::from_value(13), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  CompatibilityDescriptor required;
  required.set("accelerator", "sm_120");
  ctx.check(publish_state(*world, "payload", 2048, ConsistencyMode::IMMUTABLE, 2, required)
                .has_value(),
            "state with a compatibility requirement is committed");

  auto cluster = world->fabric->query_cluster(world->cluster_b);
  ClusterEvidenceUpdate update;
  update.cluster_id = world->cluster_b;
  update.incarnation = cluster.value().incarnation;
  update.expected_coordinator_epoch = world->epoch();
  update.capabilities.tags = {"gpu:sm_90"};
  update.capabilities.accelerator_architectures = {"sm_90"};
  update.has_capabilities = true;
  ctx.check(world->fabric->publish_cluster_evidence(update).has_value(), "evidence published");

  ReplicaRegistration registration;
  registration.state_id = world->state;
  registration.state_generation = world->generation;
  registration.location.cluster_id = world->cluster_b;
  registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
  registration.expected_coordinator_epoch = world->epoch();
  auto registered = world->fabric->register_replica(registration);
  ctx.check(registered.has_value(), "replica registers");

  TransferCompletion completion;
  completion.replica_id = registered.value().replica_id;
  completion.replica_generation = registered.value().replica_generation;
  completion.cluster_id = world->cluster_b;
  completion.expected_coordinator_epoch = world->epoch();
  completion.bytes_transferred = 2048;
  completion.measured_digest = digest_of("payload");
  ctx.check(world->fabric->record_transfer_completion(completion).has_value(),
            "the bytes transfer correctly");

  ReplicaVerification verification;
  verification.replica_id = registered.value().replica_id;
  verification.replica_generation = registered.value().replica_generation;
  verification.cluster_id = world->cluster_b;
  verification.expected_coordinator_epoch = world->epoch();
  verification.integrity_verified = true;
  verification.measured_digest = digest_of("payload");
  auto verified = world->fabric->verify_replica(verification);
  ctx.check(!verified.has_value(), "an incompatible destination fails verification");
  ctx.check(verified.error().code == ErrorCode::INCOMPATIBLE,
            "reported as INCOMPATIBLE, never as compatible");

  auto record = world->fabric->query_replica(registered.value().replica_id);
  ctx.check(record.value().compatibility == ReplicaCompatibilityState::INCOMPATIBLE,
            "the incompatibility is durably recorded");
  ctx.check(record.value().lifecycle == ReplicaLifecycle::QUARANTINED,
            "the destination is quarantined");
}

CCSF_CASE(authority, historical_generation_is_not_current_authority) {
  auto world = make_world(base_config(CoordinatorId::from_value(14), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "generation-one", 1024).has_value(), "first generation");
  auto seeded = seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE);
  ctx.check(seeded.has_value(), "first generation has an authoritative replica");
  const StateGeneration first = world->generation;

  StateGenerationSpec spec;
  spec.state_id = world->state;
  spec.generation = StateGeneration::from_value(2);
  spec.lineage = LineageId::from_value(world->state.value());
  spec.logical_size = SizeValue::of(2048);
  spec.content_digest = digest_of("generation-two");
  ctx.check(world->fabric->register_state(spec).has_value(), "second generation registered");
  ctx.check(world->fabric->commit_state_generation(world->state, spec.generation,
                                                   CommitId::from_value(world->ids.take()))
                .has_value(),
            "second generation committed");

  auto old_replica = world->fabric->query_replica(seeded.value());
  ctx.check(old_replica.value().lifecycle == ReplicaLifecycle::SUPERSEDED,
            "the replica of the superseded generation loses its authority");
  ctx.check(old_replica.value().authority == ReplicaAuthorityState::HISTORICAL,
            "the replica retains historical provenance");
  ctx.check(!old_replica.value().counts_toward_factor,
            "a superseded replica no longer counts toward the replication factor");

  auto reuse = world->fabric->evaluate_reuse(world->state, first, world->cluster_a);
  ctx.check(reuse.value().decision == DecisionKind::REJECT,
            "a historical generation does not serve current reuse");
  ctx.check(reuse.value().code == ErrorCode::STALE_STATE_GENERATION,
            "reported as a stale state generation");
}
