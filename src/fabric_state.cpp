// Cross-Cluster State Fabric - state, cluster, compatibility and policy operations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "detail/fabric_impl.hpp"

namespace ccsf {

// --- immutable read helpers -------------------------------------------------

const StateGenerationRecord* find_state(const detail::ControlPlane& plane, StateId state_id,
                                        StateGeneration generation) {
  auto it = plane.states.find(detail::StateKey{state_id, generation});
  return it == plane.states.end() ? nullptr : &it->second;
}

const StateGenerationRecord* find_current_state(const detail::ControlPlane& plane,
                                                StateId state_id) {
  for (const auto& entry : plane.states) {
    if (entry.second.state_id == state_id && entry.second.is_current()) {
      return &entry.second;
    }
  }
  return nullptr;
}

const ReplicaSetRecord* find_replica_set(const detail::ControlPlane& plane, StateId state_id,
                                         StateGeneration generation) {
  auto it = plane.replica_sets.find(detail::StateKey{state_id, generation});
  return it == plane.replica_sets.end() ? nullptr : &it->second;
}

const ClusterRecord* find_cluster(const detail::ControlPlane& plane, ClusterId cluster_id) {
  auto it = plane.clusters.find(cluster_id);
  return it == plane.clusters.end() ? nullptr : &it->second;
}

const ReplicaRecord* find_replica(const detail::ControlPlane& plane, ReplicaId replica_id) {
  auto it = plane.replicas.find(replica_id);
  return it == plane.replicas.end() ? nullptr : &it->second;
}

const OperationRecord* find_operation(const detail::ControlPlane& plane, OperationId operation_id) {
  auto it = plane.operations.find(operation_id);
  return it == plane.operations.end() ? nullptr : &it->second;
}

const OwnershipRecord* find_ownership(const detail::ControlPlane& plane, StateId state_id,
                                      StateGeneration generation) {
  auto it = plane.ownership.find(detail::StateKey{state_id, generation});
  return it == plane.ownership.end() ? nullptr : &it->second;
}

const TransferAuthorization* find_authorization(const detail::ControlPlane& plane,
                                                OperationId operation_id) {
  auto it = plane.authorizations.find(operation_id);
  return it == plane.authorizations.end() ? nullptr : &it->second;
}

const ReplicationPolicyRecord* resolve_replication_policy(const detail::ControlPlane& plane,
                                                          const FabricConfig& config,
                                                          ReplicationPolicyId policy_id) {
  if (policy_id.valid()) {
    auto it = plane.replication_policies.find(policy_id);
    return it == plane.replication_policies.end() ? nullptr : &it->second;
  }
  if (config.default_replication_policy.replication_policy_id.valid()) {
    return &config.default_replication_policy;
  }
  return nullptr;
}

const PlacementPolicyRecord* resolve_placement_policy(const detail::ControlPlane& plane,
                                                      const FabricConfig& config,
                                                      PlacementPolicyId policy_id) {
  if (policy_id.valid()) {
    auto it = plane.placement_policies.find(policy_id);
    return it == plane.placement_policies.end() ? nullptr : &it->second;
  }
  if (config.default_placement_policy.placement_policy_id.valid()) {
    return &config.default_placement_policy;
  }
  return nullptr;
}

std::vector<const ReplicaRecord*> replicas_of_state(const detail::ControlPlane& plane,
                                                    StateId state_id,
                                                    StateGeneration generation) {
  std::vector<const ReplicaRecord*> result;
  for (const auto& entry : plane.replicas) {
    const ReplicaRecord& replica = entry.second;
    if (replica.state_id == state_id && replica.state_generation == generation) {
      result.push_back(&replica);
    }
  }
  return result;
}

CompatibilityDescriptor cluster_compatibility_descriptor(const ClusterRecord& cluster) {
  CompatibilityDescriptor descriptor;
  descriptor.set("accelerator", cluster.capabilities.accelerator_architectures.empty()
                                    ? "none"
                                    : cluster.capabilities.accelerator_architectures.front());
  descriptor.set("storage", cluster.capabilities.storage_classes.empty()
                                ? "none"
                                : cluster.capabilities.storage_classes.front());
  descriptor.set("platform", cluster.location.descriptor.empty() ? "unknown"
                                                                 : cluster.location.descriptor);
  for (const std::string& tag : cluster.capabilities.tags) {
    constexpr std::string_view kPrefix = "compat:";
    if (tag.rfind(kPrefix, 0) != 0U) {
      continue;
    }
    const std::string body = tag.substr(kPrefix.size());
    const std::size_t separator = body.find('=');
    if (separator == std::string::npos || separator == 0U) {
      continue;
    }
    descriptor.set(std::string_view(body).substr(0, separator),
                   std::string_view(body).substr(separator + 1U));
  }
  return descriptor;
}

const ReplicaRecord* select_source_replica(const detail::ControlPlane& plane, StateId state_id,
                                           StateGeneration generation, ClusterId preferred,
                                           const ReplicationPolicyRecord* policy, Tick now) {
  const StateGenerationRecord* state = find_state(plane, state_id, generation);
  const ReplicaSetRecord* set = find_replica_set(plane, state_id, generation);
  const ReplicaRecord* best = nullptr;
  for (const auto& entry : plane.replicas) {
    const ReplicaRecord& replica = entry.second;
    if (replica.state_id != state_id || replica.state_generation != generation) {
      continue;
    }
    AuthorityContext context;
    context.state = state;
    context.cluster = find_cluster(plane, replica.location.cluster_id);
    context.replica_set = set;
    context.policy = policy;
    context.current_epoch = plane.coordinator_epoch;
    context.now = now;
    const ReplicaAuthorityAssessment assessment = assess_replica_authority(replica, context);
    if (!assessment.authoritative_as_replication_source) {
      continue;
    }
    if (best == nullptr) {
      best = &replica;
      continue;
    }
    const bool replica_preferred = preferred.valid() && replica.location.cluster_id == preferred;
    const bool best_preferred = preferred.valid() && best->location.cluster_id == preferred;
    if (replica_preferred != best_preferred) {
      if (replica_preferred) {
        best = &replica;
      }
      continue;
    }
    if (replica.replica_id < best->replica_id) {
      best = &replica;
    }
  }
  return best;
}

bool operation_capacity_available(const detail::ControlPlane& plane, StateId state_id,
                                  std::uint32_t maximum_in_flight) {
  std::uint32_t in_flight = 0;
  for (const auto& entry : plane.operations) {
    const OperationRecord& operation = entry.second;
    if (operation.state_id == state_id && !operation.is_terminal()) {
      ++in_flight;
    }
  }
  return in_flight < maximum_in_flight;
}

detail::Effect bump_replica_set_effect(const detail::ControlPlane& plane, StateId state_id,
                                            StateGeneration generation, Tick tick,
                                            CoordinatorEpoch epoch, bool advance_generation) {
  ReplicaSetRecord set;
  const ReplicaSetRecord* existing = find_replica_set(plane, state_id, generation);
  if (existing != nullptr) {
    // Persist the derived membership alongside the identity so that a restarted
    // coordinator observes the same counts without recomputation.
    set = rebuild_replica_set(plane, *existing);
  } else {
    set.state_id = state_id;
    set.state_generation = generation;
    set.state = ReplicaSetState::NO_AUTHORITATIVE_REPLICA;
  }
  if (advance_generation) {
    set.generation =
        set.generation.valid() ? set.generation.next() : ReplicaSetGeneration::from_value(1);
  } else if (!set.generation.valid()) {
    set.generation = ReplicaSetGeneration::from_value(1);
  }
  set.epoch = epoch;
  set.updated_at = tick;
  return detail::Effect::put_replica_set(set);
}

namespace {

Status validate_text(const std::string& text, const char* field) {
  if (text.size() > kMaxMetadataStringBytes) {
    return Error(ErrorCode::INVALID_ARGUMENT,
                 std::string("field ") + field + " exceeds the permitted metadata length");
  }
  return Status{};
}

Status validate_spec(const StateGenerationSpec& spec) {
  if (!spec.state_id.valid()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "a state identity is required");
  }
  if (!spec.generation.valid()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "a state generation is required");
  }
  if (spec.branch && !spec.branches_from.valid()) {
    return make_error(ErrorCode::INVALID_ARGUMENT,
                      "a branch generation must name the generation it branches from");
  }
  if (!spec.branch && spec.branches_from.valid()) {
    return make_error(ErrorCode::INVALID_ARGUMENT,
                      "branches_from is only legal when branch is set");
  }
  if (spec.branch && spec.branches_from == spec.generation) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "a generation cannot branch from itself");
  }
  if (spec.logical_size.known() && spec.logical_size.bytes > (1ULL << 44U)) {
    return make_error(ErrorCode::RESOURCE_EXHAUSTED, "declared logical size exceeds the bound");
  }
  CCSF_TRY(validate_text(spec.provenance.producer, "provenance.producer"));
  CCSF_TRY(validate_text(spec.provenance.verification, "provenance.verification"));
  CCSF_TRY(validate_text(spec.reconstruction.method, "reconstruction.method"));
  if (spec.replication_requirements.maximum_replicas == 0) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "maximum replica count must be at least one");
  }
  if (spec.replication_requirements.minimum_authoritative_replicas >
      spec.replication_requirements.maximum_replicas) {
    return make_error(ErrorCode::INVALID_ARGUMENT,
                      "minimum authoritative replicas exceeds the maximum replica count");
  }
  return Status{};
}

/// Two registrations of the same generation are the same only when every
/// governing field matches. Anything else is a conflicting duplicate.
bool same_governing_content(const StateGenerationRecord& existing,
                            const StateGenerationRecord& candidate) {
  return existing.lineage == candidate.lineage && existing.branch == candidate.branch &&
         existing.branches_from == candidate.branches_from &&
         existing.state_class == candidate.state_class &&
         existing.consistency == candidate.consistency &&
         existing.logical_size == candidate.logical_size &&
         existing.physical_size == candidate.physical_size &&
         existing.content_digest == candidate.content_digest &&
         existing.digest_algorithm == candidate.digest_algorithm &&
         existing.compatibility_id == candidate.compatibility_id &&
         existing.compatibility_generation == candidate.compatibility_generation &&
         existing.compatibility_required == candidate.compatibility_required &&
         existing.durability == candidate.durability &&
         existing.reuse_class == candidate.reuse_class &&
         existing.replication_requirements == candidate.replication_requirements;
}

StateGenerationRecord materialize(const StateGenerationSpec& spec, Tick tick) {
  StateGenerationRecord record;
  record.state_id = spec.state_id;
  record.generation = spec.generation;
  record.lineage =
      spec.lineage.valid() ? spec.lineage : LineageId::from_value(spec.state_id.value());
  record.branch = spec.branch;
  record.branches_from = spec.branches_from;
  record.state_class = spec.state_class;
  record.consistency = spec.consistency;
  record.lifecycle = StateGenerationLifecycle::STAGED;
  record.logical_size = spec.logical_size;
  record.physical_size = spec.physical_size;
  record.content_digest = spec.content_digest;
  record.digest_algorithm = spec.digest_algorithm;
  record.compatibility_id = spec.compatibility_id;
  record.compatibility_generation = spec.compatibility_generation;
  record.compatibility_required = spec.compatibility_required;
  record.reconstruction = spec.reconstruction;
  record.durability = spec.durability;
  record.reuse_class = spec.reuse_class;
  record.placement_constraints = spec.placement_constraints;
  record.placement_constraints.canonicalize();
  record.replication_requirements = spec.replication_requirements;
  record.provenance = spec.provenance;
  record.expected_reuse = spec.expected_reuse;
  record.transfer_cost = spec.transfer_cost;
  record.reconstruction_cost = spec.reconstruction_cost;
  record.registered_at = tick;
  return record;
}

}  // namespace

// --- states -----------------------------------------------------------------

Result<StateGenerationRecord> Fabric::register_state(const StateGenerationSpec& spec) {
  return impl_->mutate<StateGenerationRecord>(
      [&spec](const MutationInputs& inputs, StateGenerationRecord* out) -> Status {
        CCSF_TRY(validate_spec(spec));
        if (const StateGenerationRecord* existing =
                find_state(inputs.plane, spec.state_id, spec.generation);
            existing != nullptr) {
          *out = *existing;
          const StateGenerationRecord candidate =
              materialize(spec, existing->registered_at);
          if (!same_governing_content(*existing, candidate)) {
            return make_error(ErrorCode::DUPLICATE_CONFLICT,
                              "state generation " +
                                  state_generation_label(spec.state_id, spec.generation) +
                                  " is already registered with different content");
          }
          return Status{};
        }
        const LineageId lineage =
            spec.lineage.valid() ? spec.lineage : LineageId::from_value(spec.state_id.value());
        bool first_generation = true;
        for (const auto& entry : inputs.plane.states) {
          if (entry.second.state_id != spec.state_id) {
            continue;
          }
          first_generation = false;
          if (entry.second.lineage != lineage) {
            return make_error(ErrorCode::INVALID_ARGUMENT,
                              "every generation of a state identity must share one lineage");
          }
        }
        if (spec.branch && find_state(inputs.plane, spec.state_id, spec.branches_from) == nullptr) {
          return make_error(ErrorCode::UNKNOWN_STATE,
                            "the generation this branch refers to is not registered");
        }

        std::uint64_t next = inputs.plane.next_id;
        StateGenerationRecord record = materialize(spec, inputs.mutation.tick);
        const ReplicaSetId set_id = ReplicaSetId::from_value(next++);
        record.replica_set_id = set_id;
        record.replica_set_generation = ReplicaSetGeneration::from_value(1);

        ReplicaSetRecord set;
        set.replica_set_id = set_id;
        set.generation = ReplicaSetGeneration::from_value(1);
        set.state_id = record.state_id;
        set.state_generation = record.generation;
        set.state = ReplicaSetState::NO_AUTHORITATIVE_REPLICA;
        set.requirements = record.replication_requirements;
        set.epoch = inputs.plane.coordinator_epoch;
        set.created_at = inputs.mutation.tick;
        set.updated_at = inputs.mutation.tick;

        FabricCounters counters = inputs.plane.counters;
        ++counters.generations_registered;
        if (first_generation) {
          ++counters.states_registered;
        }

        inputs.mutation.effects.push_back(detail::Effect::put_state(record));
        inputs.mutation.effects.push_back(detail::Effect::put_replica_set(set));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
        *out = record;
        return Status{};
      });
}

Result<StateGenerationRecord> Fabric::publish_state_generation(const StateGenerationSpec& spec) {
  return impl_->mutate<StateGenerationRecord>(
      [&spec](const MutationInputs& inputs, StateGenerationRecord* out) -> Status {
        CCSF_TRY(validate_spec(spec));
        const StateGenerationRecord* existing =
            find_state(inputs.plane, spec.state_id, spec.generation);
        if (existing == nullptr) {
          return make_error(ErrorCode::NOT_FOUND,
                            "register the state generation before publishing it");
        }
        const StateGenerationRecord candidate = materialize(spec, existing->registered_at);
        if (existing->lifecycle != StateGenerationLifecycle::STAGED) {
          if (same_governing_content(*existing, candidate)) {
            *out = *existing;
            return Status{};
          }
          return make_error(ErrorCode::ILLEGAL_TRANSITION,
                            "a generation that has left STAGED cannot be rewritten");
        }
        StateGenerationRecord record = candidate;
        record.replica_set_id = existing->replica_set_id;
        record.replica_set_generation = existing->replica_set_generation;
        record.revalidation_required = existing->revalidation_required;
        inputs.mutation.effects.push_back(detail::Effect::put_state(record));
        *out = record;
        return Status{};
      });
}

Result<StateGenerationRecord> Fabric::commit_state_generation(StateId state_id,
                                                              StateGeneration generation,
                                                              CommitId commit_id) {
  return impl_->mutate<StateGenerationRecord>(
      [state_id, generation, commit_id](const MutationInputs& inputs,
                                        StateGenerationRecord* out) -> Status {
        if (!commit_id.valid()) {
          return make_error(ErrorCode::INVALID_ARGUMENT,
                            "an authoritative commit identity is required");
        }
        const StateGenerationRecord* existing = find_state(inputs.plane, state_id, generation);
        if (existing == nullptr) {
          return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
        }
        if (existing->lifecycle == StateGenerationLifecycle::CURRENT) {
          *out = *existing;
          if (existing->commit == commit_id) {
            return Status{};
          }
          return make_error(ErrorCode::DUPLICATE_CONFLICT,
                            "the state generation is already current under a different commit");
        }
        if (existing->lifecycle == StateGenerationLifecycle::INVALID ||
            existing->lifecycle == StateGenerationLifecycle::CORRUPT ||
            existing->lifecycle == StateGenerationLifecycle::RETIRED ||
            existing->lifecycle == StateGenerationLifecycle::REVALIDATION_REQUIRED) {
          return make_error(ErrorCode::ILLEGAL_TRANSITION,
                            "a state generation cannot become current from lifecycle " +
                                std::string(to_string(existing->lifecycle)));
        }

        std::vector<StateGenerationRecord> superseded;
        std::uint32_t other_current_non_branch = 0;
        for (const auto& entry : inputs.plane.states) {
          const StateGenerationRecord& candidate = entry.second;
          if (!candidate.is_current() || candidate.lineage != existing->lineage) {
            continue;
          }
          if (candidate.state_id != state_id) {
            continue;
          }
          if (!candidate.branch) {
            ++other_current_non_branch;
          }
          if (!existing->branch) {
            superseded.push_back(candidate);
          }
        }
        if (!existing->branch && other_current_non_branch > 1) {
          return make_error(ErrorCode::SPLIT_BRAIN_RISK,
                            "more than one current generation exists for this lineage");
        }

        StateGenerationRecord record = *existing;
        record.lifecycle = StateGenerationLifecycle::CURRENT;
        record.commit = commit_id;
        record.committed_epoch = inputs.plane.coordinator_epoch;
        record.committed_at = inputs.mutation.tick;
        record.revalidation_required = false;

        FabricCounters counters = inputs.plane.counters;
        ++counters.generations_committed;

        for (const StateGenerationRecord& old : superseded) {
          if (old.generation == record.generation) {
            continue;
          }
          StateGenerationRecord updated = old;
          updated.lifecycle = StateGenerationLifecycle::HISTORICAL;
          ++counters.generations_superseded;
          inputs.mutation.effects.push_back(detail::Effect::put_state(updated));

          for (const auto& replica_entry : inputs.plane.replicas) {
            const ReplicaRecord& replica = replica_entry.second;
            if (replica.state_id != old.state_id ||
                replica.state_generation != old.generation) {
              continue;
            }
            ReplicaRecord revised = replica;
            if (revised.lifecycle == ReplicaLifecycle::AUTHORITATIVE ||
                revised.lifecycle == ReplicaLifecycle::DEGRADED ||
                revised.lifecycle == ReplicaLifecycle::REVALIDATION_REQUIRED) {
              revised.lifecycle = ReplicaLifecycle::SUPERSEDED;
            }
            revised.authority = ReplicaAuthorityState::HISTORICAL;
            revised.counts_toward_factor = false;
            revised.last_epoch = inputs.plane.coordinator_epoch;
            revised.updated_at = inputs.mutation.tick;
            inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
          }

          if (const ReplicaSetRecord* old_set =
                  find_replica_set(inputs.plane, old.state_id, old.generation);
              old_set != nullptr) {
            ReplicaSetRecord revised_set = *old_set;
            revised_set.generation = revised_set.generation.next();
            revised_set.state = ReplicaSetState::RETIRING;
            revised_set.epoch = inputs.plane.coordinator_epoch;
            revised_set.updated_at = inputs.mutation.tick;
            inputs.mutation.effects.push_back(detail::Effect::put_replica_set(revised_set));
          }
        }

        inputs.mutation.effects.push_back(detail::Effect::put_state(record));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = record;
        return Status{};
      });
}

Result<StateGenerationRecord> Fabric::query_state(StateId state_id,
                                                  StateGeneration generation) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const StateGenerationRecord* record = find_state(impl_->plane_, state_id, generation);
  if (record == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
  }
  return *record;
}

Result<StateGenerationRecord> Fabric::current_state(StateId state_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const StateGenerationRecord* record = find_current_state(impl_->plane_, state_id);
  if (record == nullptr) {
    return make_error(ErrorCode::NO_AUTHORITATIVE_REPLICA,
                      "no state generation is currently authoritative for this state");
  }
  return *record;
}

Result<LineageView> Fabric::query_lineage(StateId state_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  LineageView view;
  view.state_id = state_id;
  for (const auto& entry : impl_->plane_.states) {
    const StateGenerationRecord& record = entry.second;
    if (record.state_id != state_id) {
      continue;
    }
    view.lineage = record.lineage;
    view.generations.push_back(record);
    if (record.branch) {
      view.branched = true;
    }
  }
  if (view.generations.empty()) {
    return make_error(ErrorCode::UNKNOWN_STATE, "the state identity is not registered");
  }
  std::sort(view.generations.begin(), view.generations.end(),
            [](const StateGenerationRecord& a, const StateGenerationRecord& b) {
              return a.generation < b.generation;
            });
  for (const StateGenerationRecord& record : view.generations) {
    if (!record.is_current()) {
      continue;
    }
    if (record.branch) {
      view.current_branches.push_back(record.generation);
    } else {
      view.current_generation = record.generation;
    }
  }
  return view;
}

Result<std::vector<StateGenerationRecord>> Fabric::list_states() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  std::vector<StateGenerationRecord> result;
  result.reserve(impl_->plane_.states.size());
  for (const auto& entry : impl_->plane_.states) {
    result.push_back(entry.second);
  }
  return result;
}

// --- clusters ---------------------------------------------------------------

Result<ClusterRecord> Fabric::register_cluster(const ClusterRegistration& registration) {
  return impl_->mutate<ClusterRecord>(
      [&registration](const MutationInputs& inputs, ClusterRecord* out) -> Status {
        if (!registration.cluster_id.valid()) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "a cluster identity is required");
        }
        if (registration.expected_coordinator_epoch.valid() &&
            registration.expected_coordinator_epoch != inputs.plane.coordinator_epoch) {
          return make_error(ErrorCode::STALE_EPOCH,
                            "registration carries coordinator epoch " +
                                registration.expected_coordinator_epoch.str() +
                                " but the current epoch is " +
                                inputs.plane.coordinator_epoch.str());
        }
        CCSF_TRY(validate_text(registration.location_descriptor, "location_descriptor"));
        if (registration.capabilities.tags.size() > kMaxClusterTagCount ||
            registration.capabilities.storage_classes.size() > kMaxClusterTagCount ||
            registration.capabilities.accelerator_architectures.size() > kMaxClusterTagCount) {
          return make_error(ErrorCode::RESOURCE_EXHAUSTED,
                            "cluster capability evidence exceeds the permitted bound");
        }

        std::uint64_t next = inputs.plane.next_id;
        FabricCounters counters = inputs.plane.counters;
        const ClusterRecord* existing = find_cluster(inputs.plane, registration.cluster_id);

        ClusterRecord record;
        bool reincarnated = false;
        ClusterIncarnationId previous_incarnation;
        if (existing == nullptr) {
          record = ClusterRecord{};
          record.cluster_id = registration.cluster_id;
          record.incarnation = ClusterIncarnationId::from_value(next++);
          record.worker_id = registration.worker_id.valid() ? registration.worker_id
                                                            : WorkerId::from_value(next++);
          record.worker_boot_id = registration.worker_boot_id.valid()
                                      ? registration.worker_boot_id
                                      : WorkerBootId::from_value(next++);
          record.registered_epoch = inputs.plane.coordinator_epoch;
          record.registered_at = inputs.mutation.tick;
          record.incarnation_started_at = inputs.mutation.tick;
          ++counters.clusters_registered;
        } else {
          record = *existing;
          previous_incarnation = existing->incarnation;
          reincarnated = registration.force_reincarnation ||
                         (existing->worker_boot_id.valid() && registration.worker_boot_id.valid() &&
                          existing->worker_boot_id != registration.worker_boot_id);
          if (reincarnated) {
            record.incarnation = ClusterIncarnationId::from_value(next++);
            record.incarnation_started_at = inputs.mutation.tick;
            ++record.reincarnation_count;
            ++counters.cluster_reincarnations;
            ++counters.cluster_incarnations_fenced;
          }
          record.registered_epoch = inputs.plane.coordinator_epoch;
          record.registered_at = inputs.mutation.tick;
          if (registration.worker_id.valid()) {
            record.worker_id = registration.worker_id;
          } else if (!record.worker_id.valid()) {
            record.worker_id = WorkerId::from_value(next++);
          }
          if (registration.worker_boot_id.valid()) {
            record.worker_boot_id = registration.worker_boot_id;
          } else if (reincarnated || !record.worker_boot_id.valid()) {
            record.worker_boot_id = WorkerBootId::from_value(next++);
          }
        }

        record.location.region = registration.region;
        record.location.site = registration.site;
        record.location.failure_domain = registration.failure_domain;
        record.location.descriptor = registration.location_descriptor;
        record.capabilities = registration.capabilities;
        record.capabilities.canonicalize();
        record.capacity = registration.capacity;
        record.health = registration.health;
        record.lifecycle =
            reincarnated ? ClusterLifecycle::REINCARNATED : ClusterLifecycle::ACTIVE;
        record.live_authority = true;
        record.updated_at = inputs.mutation.tick;

        ClusterEvidence evidence;
        evidence.capability_generation = CapabilityGeneration::from_value(next++);
        evidence.health_generation = HealthGeneration::from_value(next++);
        evidence.topology_generation = TopologyGeneration::from_value(next++);
        evidence.cost_generation = CostGeneration::from_value(next++);
        evidence.evidence_generation = EvidenceGeneration::from_value(next++);
        evidence.published_at = inputs.mutation.tick;
        evidence.coordinator_epoch = inputs.plane.coordinator_epoch;
        evidence.revalidation_required = false;
        evidence.present = true;
        record.evidence = evidence;
        record.last_evidence_epoch = inputs.plane.coordinator_epoch;

        inputs.mutation.effects.push_back(detail::Effect::put_cluster(record));

        if (reincarnated) {
          for (const auto& entry : inputs.plane.replicas) {
            const ReplicaRecord& replica = entry.second;
            if (replica.location.cluster_id != record.cluster_id || replica.is_terminal()) {
              continue;
            }
            if (replica.location.cluster_incarnation == record.incarnation) {
              continue;
            }
            ReplicaRecord revised = replica;
            revised.authority = ReplicaAuthorityState::FENCED;
            revised.counts_toward_factor = false;
            revised.availability = ReplicaAvailabilityState::UNKNOWN;
            revised.evidence.revalidation_required = true;
            if (revised.lifecycle == ReplicaLifecycle::AUTHORITATIVE ||
                revised.lifecycle == ReplicaLifecycle::DEGRADED) {
              revised.lifecycle = ReplicaLifecycle::REVALIDATION_REQUIRED;
            }
            revised.last_error = "fenced by cluster reincarnation to incarnation " +
                                 record.incarnation.str();
            revised.updated_at = inputs.mutation.tick;
            ++counters.replicas_fenced;
            inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
            if (const ReplicaSetRecord* set =
                    find_replica_set(inputs.plane, replica.state_id, replica.state_generation);
                set != nullptr) {
              ReplicaSetRecord revised_set = *set;
              revised_set.generation = revised_set.generation.next();
              revised_set.epoch = inputs.plane.coordinator_epoch;
              revised_set.updated_at = inputs.mutation.tick;
              inputs.mutation.effects.push_back(detail::Effect::put_replica_set(revised_set));
            }
          }
        }

        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
        *out = record;
        return Status{};
      });
}

Result<ClusterRecord> Fabric::publish_cluster_evidence(const ClusterEvidenceUpdate& update) {
  return impl_->mutate<ClusterRecord>(
      [&update](const MutationInputs& inputs, ClusterRecord* out) -> Status {
        const ClusterRecord* existing = find_cluster(inputs.plane, update.cluster_id);
        if (existing == nullptr) {
          return make_error(ErrorCode::UNKNOWN_CLUSTER, "the cluster is not registered");
        }
        if (update.expected_coordinator_epoch.valid() &&
            update.expected_coordinator_epoch != inputs.plane.coordinator_epoch) {
          return make_error(ErrorCode::STALE_EPOCH,
                            "evidence carries a superseded coordinator epoch");
        }
        if (!existing->live_authority || update.incarnation != existing->incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "evidence names cluster incarnation " + update.incarnation.str() +
                                " but the live incarnation is " +
                                existing->incarnation.str());
        }
        if (update.worker_boot_id.valid() && existing->worker_boot_id.valid() &&
            update.worker_boot_id != existing->worker_boot_id) {
          return make_error(ErrorCode::STALE_WORKER,
                            "evidence names worker boot " + update.worker_boot_id.str() +
                                " but the live worker boot is " +
                                existing->worker_boot_id.str());
        }

        std::uint64_t next = inputs.plane.next_id;
        ClusterRecord record = *existing;
        record.health = update.health;
        if (update.has_capabilities) {
          record.capabilities = update.capabilities;
          record.capabilities.canonicalize();
          record.evidence.capability_generation =
              CapabilityGeneration::from_value(next++);
        }
        if (update.has_capacity) {
          record.capacity = update.capacity;
        }
        record.evidence.health_generation = HealthGeneration::from_value(next++);
        record.evidence.evidence_generation = EvidenceGeneration::from_value(next++);
        record.evidence.published_at = inputs.mutation.tick;
        record.evidence.coordinator_epoch = inputs.plane.coordinator_epoch;
        record.evidence.revalidation_required = false;
        record.evidence.present = true;
        record.last_evidence_epoch = inputs.plane.coordinator_epoch;
        record.updated_at = inputs.mutation.tick;
        if (record.lifecycle == ClusterLifecycle::SUSPECT ||
            record.lifecycle == ClusterLifecycle::UNREACHABLE ||
            record.lifecycle == ClusterLifecycle::REINCARNATED) {
          record.lifecycle = ClusterLifecycle::ACTIVE;
        }

        inputs.mutation.effects.push_back(detail::Effect::put_cluster(record));
        inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
        *out = record;
        return Status{};
      });
}

Result<ClusterRecord> Fabric::mark_cluster_unreachable(ClusterId cluster_id,
                                                       ClusterIncarnationId incarnation,
                                                       std::string reason) {
  return impl_->mutate<ClusterRecord>(
      [cluster_id, incarnation, reason](const MutationInputs& inputs,
                                        ClusterRecord* out) -> Status {
        const ClusterRecord* existing = find_cluster(inputs.plane, cluster_id);
        if (existing == nullptr) {
          return make_error(ErrorCode::UNKNOWN_CLUSTER, "the cluster is not registered");
        }
        // Incarnation fencing is validated before idempotency: a caller holding a
        // stale view of the cluster must never be told that its stale request
        // succeeded.
        if (incarnation != existing->incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "cluster incarnation " + incarnation.str() +
                                " is not the current incarnation " + existing->incarnation.str());
        }
        ClusterRecord record = *existing;
        if (!existing->live_authority) {
          *out = record;
          return Status{};
        }
        CCSF_TRY(validate_text(reason, "reason"));
        record.live_authority = false;
        record.lifecycle = ClusterLifecycle::UNREACHABLE;
        record.health = ClusterHealthState::UNREACHABLE;
        record.evidence.present = false;
        record.evidence.revalidation_required = true;
        record.updated_at = inputs.mutation.tick;

        FabricCounters counters = inputs.plane.counters;
        inputs.mutation.effects.push_back(detail::Effect::put_cluster(record));

        for (const auto& entry : inputs.plane.replicas) {
          const ReplicaRecord& replica = entry.second;
          if (replica.location.cluster_id != cluster_id ||
              replica.location.cluster_incarnation != incarnation || replica.is_terminal()) {
            continue;
          }
          ReplicaRecord revised = replica;
          revised.authority = ReplicaAuthorityState::FENCED;
          revised.counts_toward_factor = false;
          revised.availability = ReplicaAvailabilityState::UNREACHABLE;
          revised.evidence.revalidation_required = true;
          if (revised.lifecycle == ReplicaLifecycle::AUTHORITATIVE ||
              revised.lifecycle == ReplicaLifecycle::DEGRADED) {
            revised.lifecycle = ReplicaLifecycle::STALE;
          }
          revised.last_error = "cluster incarnation " + incarnation.str() + " is unreachable: " +
                               reason;
          revised.updated_at = inputs.mutation.tick;
          ++counters.replicas_fenced;
          inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
          if (const ReplicaSetRecord* set =
                  find_replica_set(inputs.plane, replica.state_id, replica.state_generation);
              set != nullptr) {
            ReplicaSetRecord revised_set = *set;
            revised_set.generation = revised_set.generation.next();
            revised_set.epoch = inputs.plane.coordinator_epoch;
            revised_set.updated_at = inputs.mutation.tick;
            inputs.mutation.effects.push_back(detail::Effect::put_replica_set(revised_set));
          }
        }
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = record;
        return Status{};
      });
}

Result<ClusterRecord> Fabric::retire_cluster(ClusterId cluster_id,
                                             ClusterIncarnationId incarnation) {
  return impl_->mutate<ClusterRecord>(
      [cluster_id, incarnation](const MutationInputs& inputs, ClusterRecord* out) -> Status {
        const ClusterRecord* existing = find_cluster(inputs.plane, cluster_id);
        if (existing == nullptr) {
          return make_error(ErrorCode::UNKNOWN_CLUSTER, "the cluster is not registered");
        }
        if (incarnation != existing->incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "cluster incarnation " + incarnation.str() +
                                " is not the live incarnation " + existing->incarnation.str());
        }
        ClusterRecord record = *existing;
        record.live_authority = false;
        record.lifecycle = ClusterLifecycle::RETIRED;
        record.health = ClusterHealthState::FAILED;
        record.evidence.present = false;
        record.evidence.revalidation_required = true;
        record.updated_at = inputs.mutation.tick;

        FabricCounters counters = inputs.plane.counters;
        ++counters.cluster_incarnations_fenced;
        inputs.mutation.effects.push_back(detail::Effect::put_cluster(record));

        for (const auto& entry : inputs.plane.replicas) {
          const ReplicaRecord& replica = entry.second;
          if (replica.location.cluster_id != cluster_id ||
              replica.location.cluster_incarnation != incarnation || replica.is_terminal()) {
            continue;
          }
          ReplicaRecord revised = replica;
          revised.authority = ReplicaAuthorityState::FENCED;
          revised.counts_toward_factor = false;
          revised.availability = ReplicaAvailabilityState::ABSENT;
          revised.evidence.revalidation_required = true;
          if (revised.lifecycle == ReplicaLifecycle::AUTHORITATIVE ||
              revised.lifecycle == ReplicaLifecycle::DEGRADED) {
            revised.lifecycle = ReplicaLifecycle::STALE;
          }
          revised.last_error = "cluster retired";
          revised.updated_at = inputs.mutation.tick;
          ++counters.replicas_fenced;
          inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
          if (const ReplicaSetRecord* set =
                  find_replica_set(inputs.plane, replica.state_id, replica.state_generation);
              set != nullptr) {
            ReplicaSetRecord revised_set = *set;
            revised_set.generation = revised_set.generation.next();
            revised_set.epoch = inputs.plane.coordinator_epoch;
            revised_set.updated_at = inputs.mutation.tick;
            inputs.mutation.effects.push_back(detail::Effect::put_replica_set(revised_set));
          }
        }
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = record;
        return Status{};
      });
}

Result<ClusterRecord> Fabric::query_cluster(ClusterId cluster_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const ClusterRecord* record = find_cluster(impl_->plane_, cluster_id);
  if (record == nullptr) {
    return make_error(ErrorCode::UNKNOWN_CLUSTER, "the cluster is not registered");
  }
  return *record;
}

Result<std::vector<ClusterRecord>> Fabric::list_clusters() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  std::vector<ClusterRecord> result;
  result.reserve(impl_->plane_.clusters.size());
  for (const auto& entry : impl_->plane_.clusters) {
    result.push_back(entry.second);
  }
  return result;
}

// --- compatibility and policy ----------------------------------------------

Result<CompatibilityRecord> Fabric::publish_compatibility(const CompatibilityRecord& record) {
  return impl_->mutate<CompatibilityRecord>(
      [&record](const MutationInputs& inputs, CompatibilityRecord* out) -> Status {
        if (!record.compatibility_id.valid()) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "a compatibility identity is required");
        }
        CCSF_TRY(validate_text(record.producer, "producer"));
        CompatibilityRecord stored = record;
        if (!stored.generation.valid()) {
          stored.generation = CompatibilityGeneration::from_value(1);
        }
        if (const auto existing = inputs.plane.compatibility_records.find(record.compatibility_id);
            existing != inputs.plane.compatibility_records.end()) {
          if (existing->second.generation > stored.generation) {
            return make_error(ErrorCode::STALE_EVIDENCE,
                              "a newer compatibility generation is already published");
          }
        }
        stored.published_at = inputs.mutation.tick;
        inputs.mutation.effects.push_back(detail::Effect::put_compatibility(stored));
        *out = stored;
        return Status{};
      });
}

Result<CompatibilityRecord> Fabric::query_compatibility(CompatibilityId compatibility_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  auto it = impl_->plane_.compatibility_records.find(compatibility_id);
  if (it == impl_->plane_.compatibility_records.end()) {
    return make_error(ErrorCode::NOT_FOUND, "no compatibility record is published for that identity");
  }
  return it->second;
}

Result<PlacementPolicyRecord> Fabric::publish_placement_policy(
    const PlacementPolicyRecord& policy) {
  return impl_->mutate<PlacementPolicyRecord>(
      [&policy](const MutationInputs& inputs, PlacementPolicyRecord* out) -> Status {
        if (!policy.placement_policy_id.valid()) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "a placement policy identity is required");
        }
        PlacementPolicyRecord stored = policy;
        stored.constraints.canonicalize();
        if (!stored.generation.valid()) {
          stored.generation = PlacementPolicyGeneration::from_value(1);
        }
        if (const auto existing = inputs.plane.placement_policies.find(policy.placement_policy_id);
            existing != inputs.plane.placement_policies.end()) {
          if (existing->second.generation > stored.generation) {
            return make_error(ErrorCode::STALE_EVIDENCE,
                              "a newer placement policy generation is already published");
          }
        }
        stored.updated_at = inputs.mutation.tick;
        inputs.mutation.effects.push_back(detail::Effect::put_placement_policy(stored));
        *out = stored;
        return Status{};
      });
}

Result<ReplicationPolicyRecord> Fabric::publish_replication_policy(
    const ReplicationPolicyRecord& policy) {
  return impl_->mutate<ReplicationPolicyRecord>(
      [&policy](const MutationInputs& inputs, ReplicationPolicyRecord* out) -> Status {
        if (!policy.replication_policy_id.valid()) {
          return make_error(ErrorCode::INVALID_ARGUMENT,
                            "a replication policy identity is required");
        }
        ReplicationPolicyRecord stored = policy;
        stored.source_rules.canonicalize();
        stored.destination_rules.canonicalize();
        if (!stored.generation.valid()) {
          stored.generation = ReplicationPolicyGeneration::from_value(1);
        }
        if (const auto existing =
                inputs.plane.replication_policies.find(policy.replication_policy_id);
            existing != inputs.plane.replication_policies.end()) {
          if (existing->second.generation > stored.generation) {
            return make_error(ErrorCode::STALE_EVIDENCE,
                              "a newer replication policy generation is already published");
          }
        }
        stored.updated_at = inputs.mutation.tick;
        inputs.mutation.effects.push_back(detail::Effect::put_replication_policy(stored));
        *out = stored;
        return Status{};
      });
}

Result<PlacementPolicyRecord> Fabric::placement_policy(PlacementPolicyId policy_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const PlacementPolicyRecord* policy =
      resolve_placement_policy(impl_->plane_, impl_->config_, policy_id);
  if (policy == nullptr) {
    return make_error(ErrorCode::UNKNOWN_POLICY, "no placement policy is available");
  }
  return *policy;
}

Result<ReplicationPolicyRecord> Fabric::replication_policy(ReplicationPolicyId policy_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const ReplicationPolicyRecord* policy =
      resolve_replication_policy(impl_->plane_, impl_->config_, policy_id);
  if (policy == nullptr) {
    return make_error(ErrorCode::UNKNOWN_POLICY, "no replication policy is available");
  }
  return *policy;
}

}  // namespace ccsf
