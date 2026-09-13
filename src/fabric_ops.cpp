// Cross-Cluster State Fabric - planning, authorization and transactions.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "detail/fabric_impl.hpp"

namespace ccsf {
namespace {

/// Builds the evidence view of one candidate cluster for the policy engine.
/// Everything here is evidence; none of it is authority.
PlacementCandidate build_candidate(const detail::ControlPlane& plane, const FabricConfig& config,
                                   const ClusterRecord& cluster,
                                   const StateGenerationRecord& state,
                                   const ReplicaRecord* source) {
  PlacementCandidate candidate;
  candidate.cluster_id = cluster.cluster_id;
  candidate.cluster_incarnation = cluster.live_authority ? cluster.incarnation
                                                         : ClusterIncarnationId{};
  candidate.region = cluster.location.region;
  candidate.failure_domain = cluster.location.failure_domain;
  candidate.health = cluster.health;
  candidate.capabilities = cluster.capabilities;
  candidate.capacity = cluster.capacity;
  candidate.evidence = cluster.evidence;
  candidate.logical_size = state.logical_size;
  candidate.transfer_bytes = source != nullptr ? source->logical_size : state.logical_size;
  candidate.expected_reuse = state.expected_reuse;
  candidate.achievable_durability = state.durability >= DurabilityClass::DURABLE
                                        ? ReplicaDurabilityState::DURABLE
                                        : ReplicaDurabilityState::STANDARD;
  if (state.durability == DurabilityClass::ARCHIVAL) {
    candidate.achievable_durability = ReplicaDurabilityState::ARCHIVAL;
  }

  const CompatibilityRequirement requirement{state.compatibility_id, state.compatibility_generation,
                                             state.compatibility_required, true};
  const CompatibilityDescriptor descriptor = cluster_compatibility_descriptor(cluster);
  candidate.compatibility =
      evaluate_compatibility(requirement, &descriptor, requirement.generation, false);
  candidate.integrity_evidence_available = source != nullptr &&
                                           source->integrity == ReplicaIntegrityState::VERIFIED;

  const CostPolicy& cost_policy = config.default_placement_policy.cost_policy;
  candidate.transfer_cost = cost_policy.price_bytes(cost_policy.transfer_micro_per_mib,
                                                    candidate.transfer_bytes);
  candidate.storage_cost = cost_policy.price_bytes(cost_policy.storage_micro_per_mib_tick,
                                                   state.logical_size);
  candidate.compute_cost = cost_policy.price_bytes(cost_policy.compute_micro_per_mib,
                                                   state.logical_size);
  if (state.reconstruction.estimated_cost.known()) {
    candidate.reconstruction_cost = state.reconstruction.estimated_cost;
  } else {
    candidate.reconstruction_cost =
        cost_policy.price_bytes(cost_policy.compute_micro_per_mib, state.logical_size);
  }
  if (candidate.capacity.inbound_bytes_per_tick > 0U && candidate.transfer_bytes.known()) {
    candidate.predicted_transfer_ticks =
        (candidate.transfer_bytes.bytes + candidate.capacity.inbound_bytes_per_tick - 1U) /
        candidate.capacity.inbound_bytes_per_tick;
  }
  candidate.source_cluster_healthy = source != nullptr &&
                                     source->availability == ReplicaAvailabilityState::PRESENT;

  for (const auto& entry : plane.replicas) {
    const ReplicaRecord& replica = entry.second;
    if (replica.state_id == state.state_id && replica.location.cluster_id == cluster.cluster_id &&
        !replica.is_terminal()) {
      ++candidate.existing_replica_count;
      if (replica.commit_recorded &&
          replica.state_generation == state.generation) {
        candidate.already_hosts_current_replica = true;
      }
    }
  }
  for (const auto& entry : plane.operations) {
    if (!entry.second.is_terminal() &&
        entry.second.destination_cluster == cluster.cluster_id) {
      ++candidate.in_flight_operations;
    }
  }
  return candidate;
}

PlacementRequest build_request(const detail::ControlPlane& plane,
                               const StateGenerationRecord& state, const ReplicaSetRecord& set,
                               const ReplicationPolicyRecord& policy,
                               const PlacementPolicyRecord& placement,
                               const ReplicaRecord* source, DecisionKind intent) {
  PlacementRequest request;
  request.state_id = state.state_id;
  request.state_generation = state.generation;
  request.policy = placement;
  request.replication_policy = policy;
  request.intent = intent;
  request.now = plane.logical_time;
  request.preferred_source = source != nullptr ? source->location.cluster_id : ClusterId{};
  for (const ReplicaSetMember& member : set.authoritative_members) {
    request.current_clusters.push_back(member.cluster_id);
    request.current_regions.push_back(member.region);
    request.current_failure_domains.push_back(member.failure_domain);
  }
  return request;
}

PlacementEvaluation plan_into(const detail::ControlPlane& plane, const FabricConfig& config,
                              const StateGenerationRecord& state, const ReplicaSetRecord& set,
                              const ReplicationPolicyRecord& policy,
                              const PlacementPolicyRecord& placement, DecisionKind intent) {
  const ReplicaRecord* source =
      select_source_replica(plane, state.state_id, state.generation, ClusterId{}, &policy,
                            plane.logical_time);
  PlacementRequest request =
      build_request(plane, state, set, policy, placement, source, intent);
  for (const auto& entry : plane.clusters) {
    const ClusterRecord& cluster = entry.second;
    if (cluster.lifecycle == ClusterLifecycle::RETIRED) {
      continue;
    }
    request.candidates.push_back(build_candidate(plane, config, cluster, state, source));
  }
  return evaluate_placement(request, &state);
}

}  // namespace

Status authorize_transaction(const MutationInputs& inputs, StateId state_id,
                             StateGeneration generation, ClusterId destination_cluster,
                             ClusterId source_cluster, OperationKind kind, bool destructive_move,
                             ReplicationPolicyId replication_policy_id,
                             PlacementPolicyId placement_policy_id,
                             CoordinatorEpoch expected_epoch, TransferAuthorization* out) {
  if (expected_epoch.valid() && expected_epoch != inputs.plane.coordinator_epoch) {
    return make_error(ErrorCode::STALE_EPOCH,
                      "the request carries coordinator epoch " + expected_epoch.str() +
                          " but the current epoch is " + inputs.plane.coordinator_epoch.str());
  }
  const StateGenerationRecord* state = find_state(inputs.plane, state_id, generation);
  if (state == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
  }
  if (!state->is_current()) {
    return make_error(ErrorCode::STALE_STATE_GENERATION,
                      "only the current state generation may receive new authoritative copies");
  }
  const ReplicaSetRecord* stored_set = find_replica_set(inputs.plane, state_id, generation);
  if (stored_set == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "no replica set exists for that generation");
  }
  const ReplicaSetRecord set = detail::rebuild_replica_set(inputs.plane, *stored_set);
  const ReplicationPolicyRecord* policy =
      resolve_replication_policy(inputs.plane, inputs.config, replication_policy_id);
  if (policy == nullptr) {
    return make_error(ErrorCode::UNKNOWN_POLICY,
                      "no replication policy is configured, so eligibility cannot be evaluated");
  }
  const PlacementPolicyRecord* placement =
      resolve_placement_policy(inputs.plane, inputs.config, placement_policy_id);
  if (placement == nullptr) {
    return make_error(ErrorCode::UNKNOWN_POLICY, "no placement policy is configured");
  }
  if (!operation_capacity_available(inputs.plane, state_id,
                                    policy->destination_rules.maximum_in_flight_operations_per_state)) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED,
                      "the maximum number of in-flight operations for this state is already "
                      "reached");
  }

  const ReplicaRecord* source = select_source_replica(inputs.plane, state_id, generation,
                                                      source_cluster, policy,
                                                      inputs.plane.logical_time);
  if (source == nullptr) {
    return make_error(ErrorCode::NO_AUTHORITATIVE_REPLICA,
                      "no replica currently holds authority as a replication source for this "
                      "state generation");
  }
  if (policy->source_rules.require_healthy_source) {
    const ClusterRecord* source_cluster_record =
        find_cluster(inputs.plane, source->location.cluster_id);
    if (source_cluster_record == nullptr ||
        !cluster_admits_new_authority(*source_cluster_record, ClusterHealthState::UNKNOWN, 0U,
                                      inputs.plane.logical_time)) {
      return make_error(ErrorCode::SOURCE_INELIGIBLE,
                        "the source cluster does not hold current healthy evidence");
    }
  }

  PlacementRequest request;
  request.state_id = state_id;
  request.state_generation = generation;
  request.policy = *placement;
  request.replication_policy = *policy;
  request.preferred_source = source->location.cluster_id;
  request.now = inputs.plane.logical_time;
  request.intent = kind == OperationKind::MIGRATION ? DecisionKind::MIGRATE
                                                    : DecisionKind::REPLICATE;
  for (const ReplicaSetMember& member : set.authoritative_members) {
    request.current_clusters.push_back(member.cluster_id);
    request.current_regions.push_back(member.region);
    request.current_failure_domains.push_back(member.failure_domain);
  }
  for (const auto& entry : inputs.plane.clusters) {
    const ClusterRecord& cluster = entry.second;
    if (cluster.lifecycle == ClusterLifecycle::RETIRED) {
      continue;
    }
    if (destination_cluster.valid() && cluster.cluster_id != destination_cluster) {
      continue;
    }
    request.candidates.push_back(
        build_candidate(inputs.plane, inputs.config, cluster, *state, source));
  }
  if (request.candidates.empty()) {
    return make_error(ErrorCode::UNKNOWN_CLUSTER,
                      "the requested destination cluster is not registered");
  }
  const PlacementEvaluation evaluation = evaluate_placement(request, state);
  if (!evaluation.chosen_cluster.valid()) {
    std::string detail = "no legal destination exists";
    if (!evaluation.rejected.empty()) {
      detail += ": ";
      detail += evaluation.rejected.front().constraint;
      detail += " - ";
      detail += evaluation.rejected.front().detail;
    }
    return make_error(ErrorCode::DESTINATION_INELIGIBLE, detail);
  }
  const ClusterId chosen = evaluation.chosen_cluster;
  if (chosen == source->location.cluster_id && !policy->destination_rules.allow_same_cluster) {
    return make_error(ErrorCode::DESTINATION_INELIGIBLE,
                      "the chosen destination is the source cluster and policy forbids that");
  }
  const ClusterRecord* destination_record = find_cluster(inputs.plane, chosen);
  if (destination_record == nullptr || !destination_record->live_authority) {
    return make_error(ErrorCode::DESTINATION_INELIGIBLE,
                      "the chosen destination cluster holds no live authority");
  }

  std::uint64_t next = inputs.plane.next_id;
  OperationRecord operation;
  operation.operation_id = OperationId::from_value(next++);
  operation.operation_generation = OperationGeneration::from_value(1);
  operation.kind = kind;
  operation.state = OperationState::RESERVED;
  operation.state_id = state_id;
  operation.state_generation = generation;
  operation.source_replica = source->replica_id;
  operation.source_replica_generation = source->replica_generation;
  operation.source_cluster = source->location.cluster_id;
  operation.source_incarnation = source->location.cluster_incarnation;
  operation.destination_cluster = chosen;
  operation.destination_incarnation = destination_record->incarnation;
  operation.migration_id = MigrationId::from_value(next++);
  operation.migration_generation = MigrationGeneration::from_value(1);
  operation.transfer_id = TransferId::from_value(next++);
  operation.transfer_generation = TransferGeneration::from_value(1);
  operation.reservation_id = ReservationId::from_value(next++);
  operation.reservation_generation = ReservationGeneration::from_value(1);
  operation.source_replica_set_generation = set.generation;
  operation.placement_generation = set.placement_generation;
  operation.placement_policy_generation = placement->generation;
  operation.replication_policy_generation = policy->generation;
  operation.replication_policy_id = policy->replication_policy_id;
  operation.replication_policy_generation_recorded = policy->generation;
  operation.placement_policy_id = placement->placement_policy_id;
  operation.planned_epoch = inputs.plane.coordinator_epoch;
  operation.last_epoch = inputs.plane.coordinator_epoch;
  operation.transfer_bytes = source->logical_size.known() ? source->logical_size
                                                          : state->logical_size;
  operation.transferred_bytes = SizeValue::of(0);
  operation.expected_digest = state->content_digest;
  operation.failure_action = policy->failure_action;
  operation.planned_at = inputs.mutation.tick;
  operation.updated_at = inputs.mutation.tick;
  operation.reservation_expires_at = inputs.mutation.tick + kReservationWindowTicks;
  operation.destructive_move = destructive_move;
  operation.reason = "authorized by deterministic placement under placement policy " +
                     placement->placement_policy_id.str() + " and replication policy " +
                     policy->replication_policy_id.str();
  operation.journal.push_back("planned destination cluster " + chosen.str());
  operation.journal.push_back("reserved transfer " + operation.transfer_id.str());

  ReplicaRecord destination;
  destination.replica_id = ReplicaId::from_value(next++);
  destination.replica_generation = ReplicaGeneration::from_value(1);
  destination.state_id = state_id;
  destination.state_generation = generation;
  destination.replica_set_id = set.replica_set_id;
  destination.replica_set_generation = set.generation;
  destination.location.cluster_id = chosen;
  destination.location.cluster_incarnation = destination_record->incarnation;
  destination.location.region = destination_record->location.region;
  destination.location.site = destination_record->location.site;
  destination.location.failure_domain = destination_record->location.failure_domain;
  destination.location.descriptor = destination_record->location.descriptor;
  destination.lifecycle = ReplicaLifecycle::RESERVED;
  destination.integrity = ReplicaIntegrityState::UNVERIFIED;
  destination.compatibility = ReplicaCompatibilityState::UNKNOWN;
  destination.durability = ReplicaDurabilityState::UNKNOWN;
  destination.availability = ReplicaAvailabilityState::UNKNOWN;
  destination.authority = ReplicaAuthorityState::CANDIDATE;
  destination.content_digest = state->content_digest;
  destination.digest_algorithm = state->digest_algorithm;
  destination.compatibility_id = state->compatibility_id;
  destination.compatibility_generation = state->compatibility_generation;
  destination.logical_size = state->logical_size;
  destination.transferred_bytes = SizeValue::of(0);
  destination.provenance.producer = "cross-cluster-state-fabric";
  destination.provenance.origin_cluster = source->location.cluster_id;
  destination.provenance.origin_incarnation = source->location.cluster_incarnation;
  destination.provenance.source_generation = generation;
  destination.provenance.source_replica = source->replica_id;
  destination.provenance.operation = operation.operation_id;
  destination.provenance.worker_boot = source->worker_boot_id;
  destination.provenance.coordinator_epoch = inputs.plane.coordinator_epoch;
  destination.provenance.recorded_at = inputs.mutation.tick;
  destination.worker_id = destination_record->worker_id;
  destination.worker_boot_id = destination_record->worker_boot_id;
  destination.created_epoch = inputs.plane.coordinator_epoch;
  destination.last_epoch = inputs.plane.coordinator_epoch;
  destination.created_at = inputs.mutation.tick;
  destination.updated_at = inputs.mutation.tick;
  destination.transfer_id = operation.transfer_id;
  destination.transfer_generation = operation.transfer_generation;
  destination.operation_id = operation.operation_id;
  destination.operation_generation = operation.operation_generation;
  operation.destination_replica = destination.replica_id;
  operation.destination_replica_generation = destination.replica_generation;

  TransferAuthorization authorization;
  authorization.operation_id = operation.operation_id;
  authorization.operation_generation = operation.operation_generation;
  authorization.kind = kind;
  authorization.migration_id = operation.migration_id;
  authorization.migration_generation = operation.migration_generation;
  authorization.transfer_id = operation.transfer_id;
  authorization.transfer_generation = operation.transfer_generation;
  authorization.reservation_id = operation.reservation_id;
  authorization.reservation_generation = operation.reservation_generation;
  authorization.state_id = state_id;
  authorization.state_generation = generation;
  authorization.replica_set_id = set.replica_set_id;
  authorization.replica_set_generation = set.generation;
  authorization.source_replica = source->replica_id;
  authorization.source_replica_generation = source->replica_generation;
  authorization.source_cluster = source->location.cluster_id;
  authorization.source_incarnation = source->location.cluster_incarnation;
  authorization.destination_replica = destination.replica_id;
  authorization.destination_replica_generation = destination.replica_generation;
  authorization.destination_cluster = chosen;
  authorization.destination_incarnation = destination_record->incarnation;
  authorization.placement_id = set.placement_id;
  authorization.placement_generation = set.placement_generation;
  authorization.coordinator_epoch = inputs.plane.coordinator_epoch;
  authorization.transfer_bytes = operation.transfer_bytes;
  authorization.expected_digest = state->content_digest;
  authorization.digest_algorithm = state->digest_algorithm;
  authorization.reservation_expires_at = operation.reservation_expires_at;
  authorization.authorized_at = inputs.mutation.tick;
  authorization.destructive_move = destructive_move;

  FabricCounters counters = inputs.plane.counters;
  ++counters.operations_planned;
  ++counters.operations_reserved;
  ++counters.transfers_started;
  if (operation.transfer_bytes.known()) {
    counters.reserved_bytes += operation.transfer_bytes.bytes;
  }
  ++counters.reserved_transfer_slots;

  inputs.mutation.effects.push_back(detail::Effect::put_operation(operation));
  inputs.mutation.effects.push_back(detail::Effect::put_authorization(authorization));
  inputs.mutation.effects.push_back(detail::Effect::put_replica(destination));
  inputs.mutation.effects.push_back(bump_replica_set_effect(
      inputs.plane, state_id, generation, inputs.mutation.tick, inputs.plane.coordinator_epoch));
  inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
  inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
  *out = authorization;
  return Status{};
}

Result<PlacementEvaluation> Fabric::evaluate_placement(const PlacementRequest& request) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const StateGenerationRecord* state =
      find_state(impl_->plane_, request.state_id, request.state_generation);
  if (state == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
  }
  return ccsf::evaluate_placement(request, state);
}

Result<PlacementEvaluation> Fabric::plan_replication(const ReplicationRequest& request) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const StateGenerationRecord* state =
      find_state(impl_->plane_, request.state_id, request.state_generation);
  if (state == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
  }
  const ReplicaSetRecord* set =
      find_replica_set(impl_->plane_, request.state_id, request.state_generation);
  if (set == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "no replica set exists for that generation");
  }
  const ReplicationPolicyRecord* policy = resolve_replication_policy(
      impl_->plane_, impl_->config_, request.replication_policy.replication_policy_id);
  if (policy == nullptr) {
    return make_error(ErrorCode::UNKNOWN_POLICY, "no replication policy is available");
  }
  const PlacementPolicyRecord* placement = resolve_placement_policy(
      impl_->plane_, impl_->config_, request.placement_policy.placement_policy_id);
  if (placement == nullptr) {
    return make_error(ErrorCode::UNKNOWN_POLICY, "no placement policy is available");
  }
  return plan_into(impl_->plane_, impl_->config_, *state, *set, *policy, *placement,
                   DecisionKind::REPLICATE);
}

Result<PlacementEvaluation> Fabric::plan_migration(const MigrationRequest& request) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const StateGenerationRecord* state =
      find_state(impl_->plane_, request.state_id, request.state_generation);
  if (state == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
  }
  const ReplicaSetRecord* set =
      find_replica_set(impl_->plane_, request.state_id, request.state_generation);
  if (set == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "no replica set exists for that generation");
  }
  const ReplicationPolicyRecord* policy = resolve_replication_policy(
      impl_->plane_, impl_->config_, request.replication_policy.replication_policy_id);
  if (policy == nullptr) {
    return make_error(ErrorCode::UNKNOWN_POLICY, "no replication policy is available");
  }
  const PlacementPolicyRecord* placement = resolve_placement_policy(
      impl_->plane_, impl_->config_, request.placement_policy.placement_policy_id);
  if (placement == nullptr) {
    return make_error(ErrorCode::UNKNOWN_POLICY, "no placement policy is available");
  }
  return plan_into(impl_->plane_, impl_->config_, *state, *set, *policy, *placement,
                   DecisionKind::MIGRATE);
}

Result<ReuseEvaluation> Fabric::evaluate_reuse(StateId state_id, StateGeneration generation,
                                               ClusterId cluster_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  ReuseEvaluation evaluation;
  evaluation.state_id = state_id;
  evaluation.state_generation = generation;
  evaluation.cluster_id = cluster_id;
  evaluation.decision = DecisionKind::REJECT;

  const StateGenerationRecord* state = find_state(impl_->plane_, state_id, generation);
  if (state == nullptr) {
    evaluation.code = ErrorCode::UNKNOWN_STATE;
    evaluation.reason = "the state generation is not registered";
    evaluation.findings.push_back(evaluation.reason);
    return evaluation;
  }
  const ClusterRecord* cluster = find_cluster(impl_->plane_, cluster_id);
  if (cluster == nullptr) {
    evaluation.code = ErrorCode::UNKNOWN_CLUSTER;
    evaluation.reason = "the cluster is not registered";
    evaluation.findings.push_back(evaluation.reason);
    return evaluation;
  }
  const ReplicaSetRecord stored_set = [&]() {
    const ReplicaSetRecord* found = find_replica_set(impl_->plane_, state_id, generation);
    return found == nullptr ? ReplicaSetRecord{} : *found;
  }();
  const ReplicaSetRecord set = detail::rebuild_replica_set(impl_->plane_, stored_set);
  const ReplicationPolicyRecord* policy =
      resolve_replication_policy(impl_->plane_, impl_->config_, ReplicationPolicyId{});

  evaluation.findings.push_back("state: " + state_generation_label(state_id, generation));
  evaluation.findings.push_back(std::string("state lifecycle: ") + to_string(state->lifecycle));
  evaluation.findings.push_back("replica set generation: " + set.generation.str());
  evaluation.findings.push_back(std::string("replica set state: ") + to_string(set.state));
  evaluation.findings.push_back("authoritative replicas: " + std::to_string(set.authoritative_count) +
                                " (minimum " +
                                std::to_string(set.requirements.minimum_authoritative_replicas) +
                                ")");

  const ReplicaRecord* best = nullptr;
  ReplicaAuthorityAssessment best_assessment;
  bool have_rejection = false;
  for (const auto& entry : impl_->plane_.replicas) {
    const ReplicaRecord& replica = entry.second;
    if (replica.state_id != state_id || replica.state_generation != generation) {
      continue;
    }
    if (replica.location.cluster_id != cluster_id) {
      continue;
    }
    AuthorityContext context;
    context.state = state;
    context.cluster = cluster;
    context.replica_set = &set;
    context.policy = policy;
    context.current_epoch = impl_->plane_.coordinator_epoch;
    context.now = impl_->plane_.logical_time;
    const ReplicaAuthorityAssessment assessment = assess_replica_authority(replica, context);
    if (assessment_permits_reuse(assessment)) {
      if (best == nullptr || replica.replica_id < best->replica_id) {
        best = &replica;
        best_assessment = assessment;
      }
      continue;
    }
    if (!have_rejection || replica.replica_id < evaluation.replica) {
      best_assessment = assessment;
      evaluation.replica = replica.replica_id;
      evaluation.replica_generation = replica.replica_generation;
      have_rejection = true;
    }
  }

  if (best == nullptr) {
    evaluation.assessment = best_assessment;
    if (have_rejection) {
      evaluation.code = best_assessment.code;
      evaluation.reason = best_assessment.reason;
    } else {
      evaluation.code = ErrorCode::NO_AUTHORITATIVE_REPLICA;
      evaluation.replica = ReplicaId{};
      evaluation.replica_generation = ReplicaGeneration{};
      evaluation.reason = "no replica of " + state_generation_label(state_id, generation) +
                          " exists in cluster " + cluster_id.str();
    }
    evaluation.findings.push_back("reuse rejected: " + evaluation.reason);
    for (const auto& entry : impl_->plane_.replicas) {
      const ReplicaRecord& candidate = entry.second;
      if (candidate.state_id != state_id || candidate.state_generation != generation) {
        continue;
      }
      if (replica_survives_cluster_loss(candidate)) {
        evaluation.findings.push_back(
            "a durable historical copy exists as replica " + candidate.replica_id.str() +
            " in cluster " + candidate.location.cluster_id.str() +
            " and remains durable without being reusable");
      }
    }
    return evaluation;
  }

  evaluation.replica = best->replica_id;
  evaluation.replica_generation = best->replica_generation;
  evaluation.assessment = best_assessment;
  evaluation.code = ErrorCode::OK;
  evaluation.decision = cluster_id == impl_->config_.home_cluster ? DecisionKind::REUSE_LOCAL
                                                                  : DecisionKind::REUSE_REMOTE;
  if (!state->is_current()) {
    evaluation.findings.push_back(
        "historical generation: reuse is a historical read, not current authority");
  }
  evaluation.findings.push_back(best_assessment.reason);
  evaluation.reason = "replica " + best->replica_id.str() + " in cluster " + cluster_id.str() +
                      " is authoritative for reuse of " +
                      state_generation_label(state_id, generation);
  return evaluation;
}

Result<TransferAuthorization> Fabric::authorize_replication(const ReplicationRequest& request) {
  return impl_->mutate<TransferAuthorization>(
      [&request](const MutationInputs& inputs, TransferAuthorization* out) -> Status {
        return authorize_transaction(inputs, request.state_id, request.state_generation,
                                     request.destination_cluster, request.source_cluster,
                                     OperationKind::REPLICATION, false,
                                     request.replication_policy.replication_policy_id,
                                     request.placement_policy.placement_policy_id,
                                     request.expected_coordinator_epoch, out);
      });
}

Result<TransferAuthorization> Fabric::authorize_migration(const MigrationRequest& request) {
  return impl_->mutate<TransferAuthorization>(
      [&request](const MutationInputs& inputs, TransferAuthorization* out) -> Status {
        return authorize_transaction(inputs, request.state_id, request.state_generation,
                                     request.destination_cluster, request.source_cluster,
                                     OperationKind::MIGRATION, request.retire_source_after_commit,
                                     request.replication_policy.replication_policy_id,
                                     request.placement_policy.placement_policy_id,
                                     request.expected_coordinator_epoch, out);
      });
}

Result<OperationRecord> Fabric::commit_operation(const OperationCommit& commit) {
  return impl_->mutate<OperationRecord>(
      [&commit](const MutationInputs& inputs, OperationRecord* out) -> Status {
        const OperationRecord* existing = find_operation(inputs.plane, commit.operation_id);
        if (existing == nullptr) {
          return make_error(ErrorCode::UNKNOWN_OPERATION, "the operation is not registered");
        }
        if (commit.expected_coordinator_epoch.valid() &&
            commit.expected_coordinator_epoch != inputs.plane.coordinator_epoch) {
          return make_error(ErrorCode::STALE_EPOCH, "commit carries a superseded coordinator epoch");
        }
        if (commit.operation_generation.valid() &&
            commit.operation_generation != existing->operation_generation) {
          return make_error(ErrorCode::STALE_OPERATION_GENERATION,
                            "commit names a superseded operation generation");
        }
        if (existing->is_terminal()) {
          *out = *existing;
          if ((existing->state == OperationState::COMPLETE ||
               existing->state == OperationState::ADDED_TO_REPLICA_SET) &&
              existing->commit_id == commit.commit_id) {
            return Status{};
          }
          return make_error(ErrorCode::COMMIT_CONFLICT,
                            "the operation is already terminal under a different outcome");
        }
        if (commit.retire_source && !existing->destructive_move) {
          return make_error(ErrorCode::POLICY_VIOLATION,
                            "this operation was not authorized for destructive move semantics");
        }
        const ReplicaRecord* destination =
            find_replica(inputs.plane, existing->destination_replica);
        if (destination == nullptr) {
          return make_error(ErrorCode::UNKNOWN_REPLICA, "the destination replica is not registered");
        }
        const StateGenerationRecord* state =
            find_state(inputs.plane, existing->state_id, existing->state_generation);
        if (state == nullptr) {
          return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
        }
        const ReplicaSetRecord* set =
            find_replica_set(inputs.plane, existing->state_id, existing->state_generation);
        if (set == nullptr) {
          return make_error(ErrorCode::UNKNOWN_STATE, "the replica set for this generation is gone");
        }
        if (existing->source_replica_set_generation.valid() &&
            set->generation != existing->source_replica_set_generation) {
          return make_error(ErrorCode::STALE_REPLICA_SET_GENERATION,
                            "the replica set changed after this operation was authorized");
        }
        if (destination->lifecycle != ReplicaLifecycle::PREPARED) {
          return make_error(ErrorCode::TRANSFER_INCOMPLETE,
                            std::string("the destination is in lifecycle ") +
                                to_string(destination->lifecycle) +
                                " and cannot become authoritative");
        }
        if (destination->integrity != ReplicaIntegrityState::VERIFIED) {
          return make_error(ErrorCode::INTEGRITY_UNKNOWN,
                            "the destination has no verified integrity evidence");
        }
        if (destination->compatibility != ReplicaCompatibilityState::COMPATIBLE) {
          return make_error(ErrorCode::COMPATIBILITY_UNKNOWN,
                            "the destination has no verified compatibility evidence");
        }
        if (destination->evidence.revalidation_required) {
          return make_error(ErrorCode::REVALIDATION_REQUIRED,
                            "the destination requires revalidation before commit");
        }
        const ClusterRecord* destination_cluster =
            find_cluster(inputs.plane, destination->location.cluster_id);
        if (destination_cluster == nullptr ||
            !destination_cluster->live_authority ||
            destination_cluster->incarnation != destination->location.cluster_incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "the destination cluster incarnation is no longer live");
        }

        const ReplicationPolicyRecord* commit_policy = resolve_replication_policy(
            inputs.plane, inputs.config, existing->replication_policy_id);
        const std::uint32_t commit_maximum =
            commit_policy != nullptr ? commit_policy->retention.maximum_replicas_per_state : 8U;
        const std::uint32_t live_authoritative =
            detail::rebuild_replica_set(inputs.plane, *set).authoritative_count;
        if (live_authoritative >= commit_maximum) {
          return make_error(ErrorCode::OVER_REPLICATED,
                            "the replica set already holds the maximum permitted authoritative "
                            "replicas");
        }

        FabricCounters counters = inputs.plane.counters;
        ReplicaRecord committed;
        bool ownership_written = false;
        const bool make_owner = existing->kind == OperationKind::MIGRATION ||
                                requires_single_authoritative_owner(state->consistency);
        const bool takeover = existing->kind == OperationKind::MIGRATION ||
                              existing->destructive_move;
        CCSF_TRY(plan_authoritative_commit(inputs, *destination, *state, *set, commit.commit_id,
                                           make_owner, takeover, &committed, &ownership_written));
        ++counters.replicas_committed;
        if (ownership_written) {
          ++counters.ownership_transfers;
        }

        OperationRecord operation = *existing;
        operation.state = OperationState::COMPLETE;
        operation.last_epoch = inputs.plane.coordinator_epoch;
        operation.commit_id = commit.commit_id;
        operation.destination_verified = true;
        operation.destination_committed = true;
        operation.recovery_required = false;
        operation.updated_at = inputs.mutation.tick;
        operation.committed_replica_set_generation = committed.replica_set_generation;
        if (operation.journal.size() < kMaxOperationJournalEntries) {
          operation.journal.push_back("destination committed as authoritative");
        }
        ++counters.operations_committed;

        const bool retire_source = existing->destructive_move && commit.retire_source;
        if (retire_source) {
          const ReplicaRecord* source = find_replica(inputs.plane, existing->source_replica);
          if (source != nullptr && !source->is_terminal() &&
              source->replica_id != committed.replica_id) {
            ReplicaRecord retired = *source;
            retired.lifecycle = ReplicaLifecycle::RETIRED;
            retired.authority = ReplicaAuthorityState::HISTORICAL;
            retired.counts_toward_factor = false;
            retired.availability = ReplicaAvailabilityState::ABSENT;
            retired.last_epoch = inputs.plane.coordinator_epoch;
            retired.updated_at = inputs.mutation.tick;
            retired.last_error = "retired after a legal destination commit";
            ++counters.replicas_retired;
            inputs.mutation.effects.push_back(detail::Effect::put_replica(retired));
            inputs.mutation.effects.push_back(bump_replica_set_effect(
                inputs.plane, retired.state_id, retired.state_generation, inputs.mutation.tick,
                inputs.plane.coordinator_epoch, true));
            operation.source_retired = true;
            if (operation.journal.size() < kMaxOperationJournalEntries) {
              operation.journal.push_back("source retired after destination commit");
            }
          }
        }

        inputs.mutation.effects.push_back(detail::Effect::put_operation(operation));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = operation;
        return Status{};
      });
}

Result<OperationRecord> Fabric::commit_migration(const OperationCommit& commit) {
  return commit_operation(commit);
}

Result<OperationRecord> Fabric::cancel_operation(const OperationCancellation& cancellation) {
  return impl_->mutate<OperationRecord>(
      [&cancellation](const MutationInputs& inputs, OperationRecord* out) -> Status {
        const OperationRecord* existing = find_operation(inputs.plane, cancellation.operation_id);
        if (existing == nullptr) {
          return make_error(ErrorCode::UNKNOWN_OPERATION, "the operation is not registered");
        }
        if (cancellation.expected_coordinator_epoch.valid() &&
            cancellation.expected_coordinator_epoch != inputs.plane.coordinator_epoch) {
          return make_error(ErrorCode::STALE_EPOCH,
                            "cancellation carries a superseded coordinator epoch");
        }
        if (cancellation.operation_generation.valid() &&
            cancellation.operation_generation != existing->operation_generation) {
          return make_error(ErrorCode::STALE_OPERATION_GENERATION,
                            "cancellation names a superseded operation generation");
        }
        OperationRecord operation = *existing;
        if (operation.state == OperationState::COMPLETE ||
            operation.state == OperationState::ADDED_TO_REPLICA_SET ||
            operation.destination_committed) {
          operation.cancellation = CancellationOutcome::COMMIT_ALREADY_AUTHORITATIVE;
          operation.updated_at = inputs.mutation.tick;
          if (operation.journal.size() < kMaxOperationJournalEntries) {
            operation.journal.push_back(
                "cancellation refused: the destination commit is already authoritative");
          }
          inputs.mutation.effects.push_back(detail::Effect::put_operation(operation));
          *out = operation;
          return Status{};
        }
        if (operation.is_terminal()) {
          *out = operation;
          return Status{};
        }

        FabricCounters counters = inputs.plane.counters;
        switch (operation.state) {
          case OperationState::PLANNED:
          case OperationState::RESERVED:
          case OperationState::STAGING_DESTINATION:
            operation.cancellation = CancellationOutcome::CANCELLED_BEFORE_TRANSFER;
            break;
          case OperationState::TRANSFERRING:
            operation.cancellation = CancellationOutcome::CANCELLED_DURING_TRANSFER;
            break;
          case OperationState::TRANSFERRED:
          case OperationState::VERIFYING:
          case OperationState::PREPARED:
            operation.cancellation = CancellationOutcome::CANCELLED_BEFORE_COMMIT;
            break;
          default:
            operation.cancellation = CancellationOutcome::OUTCOME_UNKNOWN;
            break;
        }
        operation.state = OperationState::CANCELLED;
        operation.last_epoch = inputs.plane.coordinator_epoch;
        operation.updated_at = inputs.mutation.tick;
        operation.reason = cancellation.reason;
        operation.destination_committed = false;
        if (operation.journal.size() < kMaxOperationJournalEntries) {
          operation.journal.push_back(std::string("cancelled: ") +
                                      to_string(operation.cancellation));
        }
        ++counters.operations_cancelled;

        const ReplicaRecord* destination =
            find_replica(inputs.plane, operation.destination_replica);
        if (destination != nullptr && !destination->is_terminal() &&
            !destination->commit_recorded) {
          ReplicaRecord abandoned = *destination;
          abandoned.lifecycle = ReplicaLifecycle::ABANDONED;
          abandoned.authority = ReplicaAuthorityState::NONE;
          abandoned.counts_toward_factor = false;
          abandoned.availability = ReplicaAvailabilityState::ABSENT;
          abandoned.last_epoch = inputs.plane.coordinator_epoch;
          abandoned.updated_at = inputs.mutation.tick;
          abandoned.last_error = "the operation was cancelled";
          ++counters.replicas_abandoned;
          inputs.mutation.effects.push_back(detail::Effect::put_replica(abandoned));
          inputs.mutation.effects.push_back(bump_replica_set_effect(
              inputs.plane, abandoned.state_id, abandoned.state_generation, inputs.mutation.tick,
              inputs.plane.coordinator_epoch, true));
        }

        inputs.mutation.effects.push_back(detail::Effect::put_operation(operation));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = operation;
        return Status{};
      });
}

Result<OperationRecord> Fabric::query_operation(OperationId operation_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const OperationRecord* record = find_operation(impl_->plane_, operation_id);
  if (record == nullptr) {
    return make_error(ErrorCode::UNKNOWN_OPERATION, "the operation is not registered");
  }
  return *record;
}

Result<std::vector<OperationRecord>> Fabric::list_operations() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  std::vector<OperationRecord> result;
  result.reserve(impl_->plane_.operations.size());
  for (const auto& entry : impl_->plane_.operations) {
    result.push_back(entry.second);
  }
  return result;
}

Result<TransferAuthorization> Fabric::query_authorization(OperationId operation_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const TransferAuthorization* record = find_authorization(impl_->plane_, operation_id);
  if (record == nullptr) {
    return make_error(ErrorCode::UNKNOWN_OPERATION, "no authorization exists for that operation");
  }
  return *record;
}

Result<OwnershipRecord> Fabric::transfer_ownership(const OwnershipTransferRequest& request) {
  return impl_->mutate<OwnershipRecord>(
      [&request](const MutationInputs& inputs, OwnershipRecord* out) -> Status {
        const StateGenerationRecord* state =
            find_state(inputs.plane, request.state_id, request.state_generation);
        if (state == nullptr) {
          return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
        }
        if (!requires_single_authoritative_owner(state->consistency)) {
          return make_error(ErrorCode::NOT_SUPPORTED,
                            "this state consistency mode does not model single-writer ownership");
        }
        if (request.expected_coordinator_epoch.valid() &&
            request.expected_coordinator_epoch != inputs.plane.coordinator_epoch) {
          return make_error(ErrorCode::STALE_EPOCH, "handoff carries a superseded epoch");
        }
        const OwnershipRecord* ownership =
            find_ownership(inputs.plane, request.state_id, request.state_generation);
        if (ownership == nullptr || !ownership->active) {
          return make_error(ErrorCode::AUTHORITY_NOT_ESTABLISHED,
                            "no active authoritative owner exists to hand over");
        }
        if (request.handoff_commit.valid() && ownership->commit == request.handoff_commit &&
            ownership->owner_replica == request.new_owner_replica) {
          *out = *ownership;
          return Status{};
        }
        if (ownership->owner_replica != request.expected_current_owner) {
          return make_error(ErrorCode::SPLIT_BRAIN_RISK,
                            "the expected current owner does not match the recorded owner");
        }
        const ReplicaRecord* new_owner = find_replica(inputs.plane, request.new_owner_replica);
        if (new_owner == nullptr) {
          return make_error(ErrorCode::UNKNOWN_REPLICA, "the new owner replica is not registered");
        }
        if (request.new_owner_replica_generation.valid() &&
            new_owner->replica_generation != request.new_owner_replica_generation) {
          return make_error(ErrorCode::STALE_REPLICA_GENERATION,
                            "the new owner replica generation is superseded");
        }
        if (new_owner->integrity != ReplicaIntegrityState::VERIFIED ||
            new_owner->compatibility != ReplicaCompatibilityState::COMPATIBLE) {
          return make_error(ErrorCode::AUTHORITY_NOT_ESTABLISHED,
                            "the new owner must be a verified, compatible replica of this "
                            "generation");
        }
        if (!new_owner->commit_recorded &&
            new_owner->lifecycle != ReplicaLifecycle::PREPARED) {
          return make_error(ErrorCode::TRANSFER_INCOMPLETE,
                            "the new owner must be prepared before ownership can be handed over");
        }
        if (new_owner->state_id != request.state_id ||
            new_owner->state_generation != request.state_generation) {
          return make_error(ErrorCode::STALE_STATE_GENERATION,
                            "the new owner replica does not host the requested state generation");
        }
        const ClusterRecord* owner_cluster =
            find_cluster(inputs.plane, new_owner->location.cluster_id);
        if (owner_cluster == nullptr || !owner_cluster->live_authority) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "the new owner cluster holds no live authority");
        }
        if (new_owner->location.cluster_id != request.new_owner_cluster) {
          return make_error(ErrorCode::INVALID_ARGUMENT,
                            "the new owner cluster does not host the new owner replica");
        }
        const ClusterRecord* cluster =
            find_cluster(inputs.plane, new_owner->location.cluster_id);
        if (cluster == nullptr || !cluster->live_authority ||
            cluster->incarnation != new_owner->location.cluster_incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "the new owner cluster incarnation is not live");
        }
        if (!request.handoff_commit.valid()) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "a handoff commit identity is required");
        }

        FabricCounters counters = inputs.plane.counters;
        OwnershipRecord record = *ownership;
        record.owner_replica = new_owner->replica_id;
        record.owner_replica_generation = new_owner->replica_generation;
        record.owner_cluster = new_owner->location.cluster_id;
        record.owner_incarnation = new_owner->location.cluster_incarnation;
        record.epoch = inputs.plane.coordinator_epoch;
        record.commit = request.handoff_commit;
        record.established_at = inputs.mutation.tick;
        record.active = true;
        ++counters.ownership_transfers;

        const ReplicaRecord* old_owner = find_replica(inputs.plane, ownership->owner_replica);
        if (old_owner != nullptr && !old_owner->is_terminal() &&
            old_owner->replica_id != new_owner->replica_id) {
          ReplicaRecord fenced = *old_owner;
          fenced.authority = ReplicaAuthorityState::AUTHORITATIVE_FOR_REUSE;
          fenced.last_epoch = inputs.plane.coordinator_epoch;
          fenced.updated_at = inputs.mutation.tick;
          fenced.last_error = "ownership handed over";
          inputs.mutation.effects.push_back(detail::Effect::put_replica(fenced));
        }
        ReplicaRecord promoted = *new_owner;
        promoted.authority = ReplicaAuthorityState::AUTHORITATIVE_OWNER;
        promoted.lifecycle = ReplicaLifecycle::AUTHORITATIVE;
        promoted.commit_recorded = true;
        promoted.counts_toward_factor = true;
        promoted.commit_id = request.handoff_commit;
        promoted.provenance.commit = request.handoff_commit;
        promoted.last_epoch = inputs.plane.coordinator_epoch;
        promoted.updated_at = inputs.mutation.tick;
        inputs.mutation.effects.push_back(detail::Effect::put_replica(promoted));
        inputs.mutation.effects.push_back(bump_replica_set_effect(
            inputs.plane, promoted.state_id, promoted.state_generation, inputs.mutation.tick,
            inputs.plane.coordinator_epoch, true));
        inputs.mutation.effects.push_back(detail::Effect::put_ownership(record));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = record;
        return Status{};
      });
}

// --- deterministic explanations ---------------------------------------------

Result<Explanation> Fabric::explain_reuse(StateId state_id, StateGeneration generation,
                                          ClusterId cluster_id) const {
  const Result<ReuseEvaluation> evaluation = evaluate_reuse(state_id, generation, cluster_id);
  if (!evaluation) {
    return evaluation.error();
  }
  const ReuseEvaluation& value = evaluation.value();
  Explanation explanation;
  explanation.title = "Cross-Cluster State Fabric reuse decision";
  explanation.add_labeled("State", state_generation_label(state_id, generation));
  explanation.add_labeled("Requesting cluster", cluster_id.str());
  explanation.add_labeled("Decision", to_string(value.decision));
  explanation.add_labeled("Result", to_string(value.code));
  explanation.add_labeled("Replica", value.replica.valid() ? value.replica.str() : "none");
  explanation.add_labeled("Reason", value.reason);
  explanation.add_labeled("Physically present", value.assessment.physically_present ? "yes" : "no");
  explanation.add_labeled("Transfer complete", value.assessment.transfer_complete ? "yes" : "no");
  explanation.add_labeled("Integrity verified", value.assessment.integrity_verified ? "yes" : "no");
  explanation.add_labeled("Compatibility verified",
                          value.assessment.compatibility_verified ? "yes" : "no");
  explanation.add_labeled("Current generation",
                          value.assessment.current_for_state_generation ? "yes" : "no");
  explanation.add_labeled("Member of current replica set",
                          value.assessment.member_of_current_replica_set_generation ? "yes" : "no");
  explanation.add_labeled("Cluster incarnation current",
                          value.assessment.cluster_incarnation_current ? "yes" : "no");
  explanation.add_labeled("Authoritative for reuse",
                          value.assessment.authoritative_for_reuse ? "yes" : "no");
  for (const std::string& finding : value.findings) {
    explanation.add(finding);
  }
  return explanation;
}

Result<Explanation> Fabric::explain_operation_record(OperationId operation_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const OperationRecord* record = find_operation(impl_->plane_, operation_id);
  if (record == nullptr) {
    return make_error(ErrorCode::UNKNOWN_OPERATION, "the operation is not registered");
  }
  Explanation explanation;
  explanation.title = "Cross-Cluster State Fabric operation";
  for (const std::string& line : explain_operation(*record)) {
    explanation.add(line);
  }
  return explanation;
}

Result<Explanation> Fabric::explain_replica_set_state(StateId state_id,
                                                      StateGeneration generation) const {
  const Result<ReplicaSetReconciliation> reconciliation =
      reconcile_replica_set(state_id, generation);
  if (!reconciliation) {
    return reconciliation.error();
  }
  Explanation explanation;
  explanation.title = "Cross-Cluster State Fabric replica set";
  for (const std::string& finding : reconciliation.value().findings) {
    explanation.add(finding);
  }
  explanation.add_labeled("Under replicated",
                          reconciliation.value().under_replicated ? "yes" : "no");
  explanation.add_labeled("Over replicated",
                          reconciliation.value().over_replicated ? "yes" : "no");
  explanation.add_labeled("Authoritative shortfall",
                          std::to_string(reconciliation.value().authoritative_shortfall));
  for (const ReplicaId& id : reconciliation.value().excluded_replicas) {
    explanation.add("excluded replica: " + id.str());
  }
  return explanation;
}

namespace {

Explanation render_placement(const PlacementEvaluation& value) {
  Explanation explanation;
  explanation.title = "Cross-Cluster State Fabric placement decision";
  explanation.add_labeled("State", state_generation_label(value.state_id, value.state_generation));
  explanation.add_labeled("Decision", to_string(value.decision));
  explanation.add_labeled("Placement policy generation", value.placement_policy_generation.str());
  explanation.add_labeled("Chosen cluster",
                          value.chosen_cluster.valid() ? value.chosen_cluster.str() : "none");
  explanation.add_labeled("Score", std::to_string(value.chosen_score));
  for (const std::string& reason : value.reasons) {
    explanation.add(reason);
  }
  return explanation;
}

}  // namespace

Result<Explanation> Fabric::explain_replication_plan(const ReplicationRequest& request) const {
  const Result<PlacementEvaluation> evaluation = plan_replication(request);
  if (!evaluation) {
    return evaluation.error();
  }
  return render_placement(evaluation.value());
}

Result<Explanation> Fabric::explain_migration_plan(const MigrationRequest& request) const {
  const Result<PlacementEvaluation> evaluation = plan_migration(request);
  if (!evaluation) {
    return evaluation.error();
  }
  return render_placement(evaluation.value());
}

Result<Explanation> Fabric::explain_placement(const PlacementRequest& request) const {
  const Result<PlacementEvaluation> evaluation = evaluate_placement(request);
  if (!evaluation) {
    return evaluation.error();
  }
  return render_placement(evaluation.value());
}

}  // namespace ccsf
