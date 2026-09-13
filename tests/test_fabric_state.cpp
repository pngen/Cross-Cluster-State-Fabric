// Cross-Cluster State Fabric - state lifecycle proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

CCSF_CASE(state, registration_is_staged_until_committed) {
  auto world = make_world(base_config(CoordinatorId::from_value(1), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "the test world must open");
  ctx.check(publish_state(*world, "payload", 4096).has_value(), "state publication succeeds");

  auto record = world->fabric->query_state(world->state, world->generation);
  ctx.check(record.has_value(), "the generation is queryable");
  ctx.check(record.value().lifecycle == StateGenerationLifecycle::CURRENT,
            "publication followed by an authoritative commit yields CURRENT");
  ctx.check(record.value().commit.valid(), "the current generation carries a commit identity");
  ctx.check(record.value().committed_epoch == world->epoch(),
            "the commit records the coordinator epoch that authorized it");
}

CCSF_CASE(state, a_generation_cannot_become_current_without_a_commit) {
  auto world = make_world(base_config(CoordinatorId::from_value(2), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");

  StateGenerationSpec spec;
  spec.state_id = world->state;
  spec.generation = world->generation;
  spec.lineage = LineageId::from_value(world->state.value());
  spec.logical_size = SizeValue::of(1024);
  spec.content_digest = digest_of("first");
  auto registered = world->fabric->register_state(spec);
  ctx.check(registered.has_value(), "registration succeeds");
  ctx.check(registered.value().lifecycle == StateGenerationLifecycle::STAGED,
            "registration alone must not make a generation current");
  auto current = world->fabric->current_state(world->state);
  ctx.check(!current.has_value(), "there is no current generation before an explicit commit");
  ctx.check(current.error().code == ErrorCode::NO_AUTHORITATIVE_REPLICA,
            "the absence of a current generation is reported explicitly");
}

CCSF_CASE(state, supersede_is_deterministic_and_history_survives) {
  auto world = make_world(base_config(CoordinatorId::from_value(3), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "generation-one", 2048).has_value(), "generation one committed");
  const StateGeneration first = world->generation;

  StateGenerationSpec spec;
  spec.state_id = world->state;
  spec.generation = StateGeneration::from_value(2);
  spec.lineage = LineageId::from_value(world->state.value());
  spec.logical_size = SizeValue::of(4096);
  spec.content_digest = digest_of("generation-two");
  ctx.check(world->fabric->register_state(spec).has_value(), "second generation registered");
  ctx.check(world->fabric->commit_state_generation(world->state, spec.generation,
                                                   CommitId::from_value(world->ids.take()))
                .has_value(),
            "second generation committed");

  auto lineage = world->fabric->query_lineage(world->state);
  ctx.check(lineage.has_value(), "lineage is queryable");
  ctx.check(lineage.value().current_generation == spec.generation, "the new generation is current");
  const StateGenerationRecord* older = lineage.value().find(first);
  ctx.check(older != nullptr, "the superseded generation remains in history");
  ctx.check(older->lifecycle == StateGenerationLifecycle::HISTORICAL,
            "a superseded generation becomes durable history, not current authority");
  ctx.check(!older->is_current(), "the older generation is no longer current");
  ctx.check(older->commit.valid(), "historical provenance is preserved");
}

CCSF_CASE(state, duplicate_and_conflicting_registration) {
  auto world = make_world(base_config(CoordinatorId::from_value(4), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 1024).has_value(), "state committed");

  const StateGenerationSpec duplicate = world->last_spec;
  auto repeated = world->fabric->register_state(duplicate);
  ctx.check(repeated.has_value(), "a duplicate registration of a governed generation is idempotent");
  ctx.check(repeated.value().lifecycle == StateGenerationLifecycle::CURRENT,
            "the existing record is returned unchanged");

  auto conflicting =
      world->fabric->commit_state_generation(world->state, world->generation,
                                             CommitId::from_value(world->ids.take()));
  ctx.check(!conflicting.has_value(), "a second commit for a current generation is rejected");
  ctx.check(conflicting.error().code == ErrorCode::DUPLICATE_CONFLICT,
            "the conflict is reported as DUPLICATE_CONFLICT");

  auto repeated_commit = world->fabric->commit_state_generation(
      world->state, world->generation,
      world->fabric->query_state(world->state, world->generation).value().commit);
  ctx.check(repeated_commit.has_value(), "re-committing with the same identity is idempotent");
}

CCSF_CASE(state, illegal_branch_and_lineage) {
  auto world = make_world(base_config(CoordinatorId::from_value(5), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 1024).has_value(), "state committed");

  StateGenerationSpec bad_branch;
  bad_branch.state_id = world->state;
  bad_branch.generation = StateGeneration::from_value(7);
  bad_branch.lineage = LineageId::from_value(world->state.value());
  bad_branch.branch = true;
  auto result = world->fabric->register_state(bad_branch);
  ctx.check(!result.has_value(), "a branch without a parent generation is rejected");
  ctx.check(result.error().code == ErrorCode::INVALID_ARGUMENT, "rejected as invalid input");

  StateGenerationSpec missing_parent;
  missing_parent.state_id = world->state;
  missing_parent.generation = StateGeneration::from_value(8);
  missing_parent.lineage = LineageId::from_value(world->state.value());
  missing_parent.branch = true;
  missing_parent.branches_from = StateGeneration::from_value(6);
  auto missing = world->fabric->register_state(missing_parent);
  ctx.check(!missing.has_value(), "a branch referencing an unregistered parent is rejected");
  ctx.check(missing.error().code == ErrorCode::UNKNOWN_STATE, "reported as an unknown generation");

  StateGenerationSpec other_lineage;
  other_lineage.state_id = world->state;
  other_lineage.generation = StateGeneration::from_value(9);
  other_lineage.lineage = LineageId::from_value(999999);
  auto lineage_result = world->fabric->register_state(other_lineage);
  ctx.check(!lineage_result.has_value(),
            "a second lineage for one state identity is rejected");
}

CCSF_CASE(state, legal_branching_is_explicit) {
  auto world = make_world(base_config(CoordinatorId::from_value(6), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 1024).has_value(), "state committed");

  StateGenerationSpec branch;
  branch.state_id = world->state;
  branch.generation = StateGeneration::from_value(2);
  branch.lineage = LineageId::from_value(world->state.value());
  branch.branch = true;
  branch.branches_from = world->generation;
  branch.content_digest = digest_of("branch-payload");
  ctx.check(world->fabric->register_state(branch).has_value(), "an explicit branch registers");
  ctx.check(world->fabric->commit_state_generation(world->state, branch.generation,
                                                   CommitId::from_value(world->ids.take()))
                .has_value(),
            "an explicit branch may become current alongside the trunk");

  auto lineage = world->fabric->query_lineage(world->state);
  ctx.check(lineage.value().branched, "the lineage reports that it is branched");
  ctx.check(lineage.value().current_generation == world->generation,
            "the trunk generation remains current");
  ctx.check(lineage.value().current_branches.size() == 1,
            "the branch is tracked separately from the trunk");
  ctx.check(lineage.value().current_branches.front() == branch.generation,
            "the tracked branch is the one that was committed");
}

CCSF_CASE(state, cluster_registration_and_evidence) {
  auto world = make_world(base_config(CoordinatorId::from_value(7), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  auto cluster = world->fabric->query_cluster(world->cluster_a);
  ctx.check(cluster.has_value(), "the cluster is registered");
  ctx.check(cluster.value().live_authority, "a fresh cluster holds live authority");
  ctx.check(cluster.value().incarnation.valid(), "the cluster has an incarnation");
  ctx.check(cluster.value().worker_boot_id.valid(), "the cluster has a worker boot identity");

  ClusterEvidenceUpdate update;
  update.cluster_id = world->cluster_a;
  update.incarnation = cluster.value().incarnation;
  update.expected_coordinator_epoch = world->epoch();
  update.capabilities.tags = {"gpu:sm_120", "tier:hot"};
  update.capacity.total_logical_bytes = 1ULL << 41U;
  update.capacity.inbound_bytes_per_tick = 1ULL << 29U;
  update.has_capabilities = true;
  update.has_capacity = true;
  auto published = world->fabric->publish_cluster_evidence(update);
  ctx.check(published.has_value(), "evidence publication succeeds");
  ctx.check(published.value().evidence.evidence_generation >
                cluster.value().evidence.evidence_generation,
            "evidence generations move forward");

  ClusterEvidenceUpdate stale = update;
  stale.incarnation = ClusterIncarnationId::from_value(cluster.value().incarnation.value() + 99U);
  auto rejected = world->fabric->publish_cluster_evidence(stale);
  ctx.check(!rejected.has_value(), "evidence from a stale incarnation is rejected");
  ctx.check(rejected.error().code == ErrorCode::STALE_CLUSTER_INCARNATION,
            "rejected as STALE_CLUSTER_INCARNATION");

  ClusterEvidenceUpdate old_epoch = update;
  old_epoch.expected_coordinator_epoch = CoordinatorEpoch::from_value(999);
  auto epoch_rejected = world->fabric->publish_cluster_evidence(old_epoch);
  ctx.check(!epoch_rejected.has_value(), "evidence from a stale epoch is rejected");
  ctx.check(epoch_rejected.error().code == ErrorCode::STALE_EPOCH, "rejected as STALE_EPOCH");
}
