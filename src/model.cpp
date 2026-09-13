// Cross-Cluster State Fabric - domain model helpers.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/authority.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>

#include "ccsf/cluster.hpp"
#include "ccsf/explain.hpp"
#include "ccsf/migration.hpp"
#include "ccsf/replica_set.hpp"
#include "ccsf/state.hpp"
#include "detail/encoding.hpp"

namespace ccsf {
namespace {

template <class T>
void sort_unique(std::vector<T>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

}  // namespace

// --- cluster ----------------------------------------------------------------

void ClusterCapabilities::canonicalize() {
  sort_unique(tags);
  sort_unique(storage_classes);
  sort_unique(accelerator_architectures);
}

bool ClusterCapabilities::has_tag(std::string_view tag) const {
  return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

bool ClusterCapabilities::has_storage_class(std::string_view storage_class) const {
  return std::find(storage_classes.begin(), storage_classes.end(), storage_class) !=
         storage_classes.end();
}

bool ClusterCapabilities::has_accelerator(std::string_view architecture) const {
  return std::find(accelerator_architectures.begin(), accelerator_architectures.end(),
                   architecture) != accelerator_architectures.end();
}

std::uint64_t ClusterCapacity::free_logical_bytes() const noexcept {
  return used_logical_bytes >= total_logical_bytes ? 0U : total_logical_bytes - used_logical_bytes;
}

bool ClusterCapacity::has_room_for(std::uint64_t bytes) const noexcept {
  return free_logical_bytes() >= bytes;
}

bool cluster_evidence_is_current(const ClusterRecord& record) noexcept {
  return record.evidence.present && !record.evidence.revalidation_required && record.live_authority;
}

bool cluster_admits_new_authority(const ClusterRecord& record, ClusterHealthState required_health,
                                  std::uint64_t maximum_evidence_staleness_ticks,
                                  Tick now) noexcept {
  if (!record.live_authority) {
    return false;
  }
  if (!record.evidence.present) {
    return false;
  }
  switch (record.health) {
    case ClusterHealthState::HEALTHY:
    case ClusterHealthState::DEGRADED:
      break;
    default:
      return false;
  }
  if (required_health == ClusterHealthState::HEALTHY &&
      record.health != ClusterHealthState::HEALTHY) {
    return false;
  }
  if (maximum_evidence_staleness_ticks != 0U) {
    if (now > record.evidence.published_at &&
        (now - record.evidence.published_at) > maximum_evidence_staleness_ticks) {
      return false;
    }
  }
  return true;
}

// --- policy -----------------------------------------------------------------

void PlacementConstraints::canonicalize() {
  sort_unique(legal_clusters);
  sort_unique(legal_regions);
  sort_unique(prohibited_clusters);
  sort_unique(prohibited_regions);
  sort_unique(required_capabilities);
  sort_unique(required_storage_classes);
  sort_unique(required_sovereignty_tags);
  sort_unique(locality_clusters);
}

void EligibilityRules::canonicalize() {
  sort_unique(allowed_clusters);
  sort_unique(denied_clusters);
}

const char* to_string(RankingFactor factor) noexcept {
  switch (factor) {
    case RankingFactor::EXPECTED_REUSE_VALUE: return "expected_reuse_value";
    case RankingFactor::TRANSFER_COST: return "transfer_cost";
    case RankingFactor::RECONSTRUCTION_COST: return "reconstruction_cost";
    case RankingFactor::AVAILABLE_BANDWIDTH: return "available_bandwidth";
    case RankingFactor::PREDICTED_TRANSFER_DURATION: return "predicted_transfer_duration";
    case RankingFactor::STORAGE_COST: return "storage_cost";
    case RankingFactor::COMPUTE_COST: return "compute_cost";
    case RankingFactor::REGION_AFFINITY: return "region_affinity";
    case RankingFactor::WORKLOAD_AFFINITY: return "workload_affinity";
    case RankingFactor::EXISTING_REPLICA_PROXIMITY: return "existing_replica_proximity";
    case RankingFactor::FAILURE_DOMAIN_DIVERSITY: return "failure_domain_diversity";
    case RankingFactor::EXPECTED_FUTURE_DEMAND: return "expected_future_demand";
    case RankingFactor::SOURCE_HEALTH: return "source_health";
    case RankingFactor::DESTINATION_HEALTH: return "destination_health";
    case RankingFactor::PRESSURE: return "pressure";
    case RankingFactor::UTILIZATION: return "utilization";
  }
  return "unrecognized_ranking_factor";
}

bool is_valid(RankingFactor factor) noexcept {
  return static_cast<std::uint16_t>(factor) <=
         static_cast<std::uint16_t>(RankingFactor::UTILIZATION);
}

std::int64_t RankingWeights::weight_of(RankingFactor factor) const noexcept {
  switch (factor) {
    case RankingFactor::EXPECTED_REUSE_VALUE: return expected_reuse_value;
    case RankingFactor::TRANSFER_COST: return transfer_cost;
    case RankingFactor::RECONSTRUCTION_COST: return reconstruction_cost;
    case RankingFactor::AVAILABLE_BANDWIDTH: return available_bandwidth;
    case RankingFactor::PREDICTED_TRANSFER_DURATION: return predicted_transfer_duration;
    case RankingFactor::STORAGE_COST: return storage_cost;
    case RankingFactor::COMPUTE_COST: return compute_cost;
    case RankingFactor::REGION_AFFINITY: return region_affinity;
    case RankingFactor::WORKLOAD_AFFINITY: return workload_affinity;
    case RankingFactor::EXISTING_REPLICA_PROXIMITY: return existing_replica_proximity;
    case RankingFactor::FAILURE_DOMAIN_DIVERSITY: return failure_domain_diversity;
    case RankingFactor::EXPECTED_FUTURE_DEMAND: return expected_future_demand;
    case RankingFactor::SOURCE_HEALTH: return source_health;
    case RankingFactor::DESTINATION_HEALTH: return destination_health;
    case RankingFactor::PRESSURE: return pressure;
    case RankingFactor::UTILIZATION: return utilization;
  }
  return 0;
}

void RankingWeights::set_weight(RankingFactor factor, std::int64_t weight) noexcept {
  switch (factor) {
    case RankingFactor::EXPECTED_REUSE_VALUE: expected_reuse_value = weight; break;
    case RankingFactor::TRANSFER_COST: transfer_cost = weight; break;
    case RankingFactor::RECONSTRUCTION_COST: reconstruction_cost = weight; break;
    case RankingFactor::AVAILABLE_BANDWIDTH: available_bandwidth = weight; break;
    case RankingFactor::PREDICTED_TRANSFER_DURATION: predicted_transfer_duration = weight; break;
    case RankingFactor::STORAGE_COST: storage_cost = weight; break;
    case RankingFactor::COMPUTE_COST: compute_cost = weight; break;
    case RankingFactor::REGION_AFFINITY: region_affinity = weight; break;
    case RankingFactor::WORKLOAD_AFFINITY: workload_affinity = weight; break;
    case RankingFactor::EXISTING_REPLICA_PROXIMITY: existing_replica_proximity = weight; break;
    case RankingFactor::FAILURE_DOMAIN_DIVERSITY: failure_domain_diversity = weight; break;
    case RankingFactor::EXPECTED_FUTURE_DEMAND: expected_future_demand = weight; break;
    case RankingFactor::SOURCE_HEALTH: source_health = weight; break;
    case RankingFactor::DESTINATION_HEALTH: destination_health = weight; break;
    case RankingFactor::PRESSURE: pressure = weight; break;
    case RankingFactor::UTILIZATION: utilization = weight; break;
  }
}

CostValue CostPolicy::price_bytes(std::int64_t micro_per_mib, SizeValue bytes) const noexcept {
  if (source == CostSource::UNKNOWN) {
    return CostValue::unknown();
  }
  if (micro_per_mib == 0) {
    return CostValue{source, 0};
  }
  if (!bytes.known()) {
    return CostValue::unknown();
  }
  constexpr std::uint64_t kMib = 1024U * 1024U;
  return cost_scale(CostValue{source, micro_per_mib}, bytes.bytes, kMib);
}

std::string PlacementPolicyRecord::fingerprint() const {
  ByteWriter writer;
  writer.str("ccsf.placement.policy.v1");
  detail::encode(writer, *this);
  return sha256(writer.data()).hex();
}

ReplicationRequirements ReplicationPolicyRecord::effective_requirements() const {
  ReplicationRequirements effective = requirements;
  switch (requirements.form) {
    case ReplicationPolicyForm::SINGLE:
      effective.minimum_authoritative_replicas = 1;
      effective.desired_replicas = 1;
      effective.maximum_replicas = 1;
      effective.region_diversity = 0;
      effective.cluster_diversity = 0;
      effective.failure_domain_diversity = 0;
      break;
    case ReplicationPolicyForm::N_REPLICAS:
      break;
    case ReplicationPolicyForm::CLUSTER_DIVERSE:
      effective.cluster_diversity = std::max<std::uint32_t>(effective.cluster_diversity, 2);
      break;
    case ReplicationPolicyForm::REGION_DIVERSE:
      effective.region_diversity = std::max<std::uint32_t>(effective.region_diversity, 2);
      effective.cluster_diversity = std::max<std::uint32_t>(effective.cluster_diversity, 2);
      break;
    case ReplicationPolicyForm::POLICY_CUSTOM:
      break;
  }
  if (effective.maximum_replicas == 0) {
    effective.maximum_replicas = 1;
  }
  if (effective.minimum_authoritative_replicas == 0) {
    effective.minimum_authoritative_replicas = 1;
  }
  if (effective.minimum_authoritative_replicas > effective.maximum_replicas) {
    effective.minimum_authoritative_replicas = effective.maximum_replicas;
  }
  if (effective.desired_replicas < effective.minimum_authoritative_replicas) {
    effective.desired_replicas = effective.minimum_authoritative_replicas;
  }
  if (effective.desired_replicas > effective.maximum_replicas) {
    effective.desired_replicas = effective.maximum_replicas;
  }
  return effective;
}

std::string ReplicationPolicyRecord::fingerprint() const {
  ByteWriter writer;
  writer.str("ccsf.replication.policy.v1");
  detail::encode(writer, *this);
  return sha256(writer.data()).hex();
}

ReplicationPolicyRecord make_replication_policy(ReplicationPolicyId id,
                                                ReplicationPolicyGeneration generation,
                                                ReplicationPolicyForm form,
                                                std::uint32_t replica_count) {
  ReplicationPolicyRecord policy;
  policy.replication_policy_id = id;
  policy.generation = generation;
  policy.requirements.form = form;
  switch (form) {
    case ReplicationPolicyForm::SINGLE:
      policy.requirements.minimum_authoritative_replicas = 1;
      policy.requirements.desired_replicas = 1;
      policy.requirements.maximum_replicas = 1;
      break;
    case ReplicationPolicyForm::N_REPLICAS: {
      const std::uint32_t count = replica_count == 0 ? 1 : replica_count;
      policy.requirements.minimum_authoritative_replicas = count;
      policy.requirements.desired_replicas = count;
      policy.requirements.maximum_replicas = std::max<std::uint32_t>(count, count);
      break;
    }
    case ReplicationPolicyForm::CLUSTER_DIVERSE:
      policy.requirements.minimum_authoritative_replicas = std::max<std::uint32_t>(replica_count, 2);
      policy.requirements.desired_replicas = std::max<std::uint32_t>(replica_count, 2);
      policy.requirements.maximum_replicas = std::max<std::uint32_t>(replica_count, 2) + 2;
      policy.requirements.cluster_diversity = 2;
      break;
    case ReplicationPolicyForm::REGION_DIVERSE:
      policy.requirements.minimum_authoritative_replicas = std::max<std::uint32_t>(replica_count, 2);
      policy.requirements.desired_replicas = std::max<std::uint32_t>(replica_count, 2);
      policy.requirements.maximum_replicas = std::max<std::uint32_t>(replica_count, 2) + 2;
      policy.requirements.region_diversity = 2;
      policy.requirements.cluster_diversity = 2;
      break;
    case ReplicationPolicyForm::POLICY_CUSTOM: {
      const std::uint32_t count = replica_count == 0 ? 1 : replica_count;
      policy.requirements.minimum_authoritative_replicas = count;
      policy.requirements.desired_replicas = count;
      policy.requirements.maximum_replicas = count;
      break;
    }
  }
  return policy;
}

// --- state ------------------------------------------------------------------

const StateGenerationRecord* LineageView::find(StateGeneration generation) const {
  for (const StateGenerationRecord& record : generations) {
    if (record.generation == generation) {
      return &record;
    }
  }
  return nullptr;
}

std::string state_generation_label(const StateId& state_id, StateGeneration generation) {
  std::string label = "S";
  label += state_id.str();
  label += " g";
  label += generation.str();
  return label;
}

// --- replica ----------------------------------------------------------------

bool replica_counts_toward_replication_factor(const ReplicaRecord& record) noexcept {
  if (!record.commit_recorded || !record.counts_toward_factor) {
    return false;
  }
  if (record.integrity != ReplicaIntegrityState::VERIFIED) {
    return false;
  }
  if (record.compatibility != ReplicaCompatibilityState::COMPATIBLE) {
    return false;
  }
  switch (record.lifecycle) {
    case ReplicaLifecycle::AUTHORITATIVE:
    case ReplicaLifecycle::DEGRADED:
      return true;
    default:
      return false;
  }
}

bool replica_survives_cluster_loss(const ReplicaRecord& record) noexcept {
  if (record.is_terminal()) {
    return false;
  }
  switch (record.durability) {
    case ReplicaDurabilityState::DURABLE:
    case ReplicaDurabilityState::ARCHIVAL:
      return record.commit_recorded;
    default:
      return false;
  }
}

// --- replica set ------------------------------------------------------------

bool ReplicaSetRecord::meets_requirements() const noexcept {
  const ReplicationRequirements effective = requirements;
  if (authoritative_count < effective.minimum_authoritative_replicas) {
    return false;
  }
  if (distinct_clusters < effective.cluster_diversity) {
    return false;
  }
  if (distinct_regions < effective.region_diversity) {
    return false;
  }
  if (distinct_failure_domains < effective.failure_domain_diversity) {
    return false;
  }
  return true;
}

bool ReplicaSetRecord::has_authoritative_owner() const noexcept {
  for (const ReplicaSetMember& member : authoritative_members) {
    if (member.authority == ReplicaAuthorityState::AUTHORITATIVE_OWNER) {
      return true;
    }
  }
  return false;
}

ReplicaSetRecord recompute_replica_set(ReplicaSetRecord record) {
  std::set<ClusterId> clusters;
  std::set<RegionId> regions;
  std::set<FailureDomainId> domains;
  std::uint32_t authoritative = 0;
  bool any_incompatible = false;
  bool any_corrupt = false;
  bool any_unverified = false;

  for (const ReplicaSetMember& member : record.authoritative_members) {
    if (!member.authoritative) {
      continue;
    }
    ++authoritative;
    clusters.insert(member.cluster_id);
    regions.insert(member.region);
    if (member.failure_domain.valid()) {
      domains.insert(member.failure_domain);
    }
    if (member.integrity == ReplicaIntegrityState::CORRUPT ||
        member.integrity == ReplicaIntegrityState::MISMATCH) {
      any_corrupt = true;
    }
    if (member.compatibility == ReplicaCompatibilityState::INCOMPATIBLE) {
      any_incompatible = true;
    }
    if (member.integrity != ReplicaIntegrityState::VERIFIED ||
        member.compatibility != ReplicaCompatibilityState::COMPATIBLE) {
      any_unverified = true;
    }
  }

  record.authoritative_count = authoritative;
  record.degraded_count = static_cast<std::uint32_t>(record.degraded_members.size());
  record.distinct_clusters = static_cast<std::uint32_t>(clusters.size());
  record.distinct_regions = static_cast<std::uint32_t>(regions.size());
  record.distinct_failure_domains = static_cast<std::uint32_t>(domains.size());

  const bool preserve_explicit =
      record.state == ReplicaSetState::SPLIT_BRAIN_RISK ||
      record.state == ReplicaSetState::REBUILDING || record.state == ReplicaSetState::RETIRING;
  if (preserve_explicit) {
    return record;
  }

  if (authoritative == 0) {
    record.state = ReplicaSetState::NO_AUTHORITATIVE_REPLICA;
  } else if (any_incompatible || any_corrupt) {
    record.state = ReplicaSetState::CORRUPT;
  } else if (record.revalidation_required || any_unverified) {
    record.state = ReplicaSetState::REVALIDATION_REQUIRED;
  } else if (authoritative > record.requirements.maximum_replicas) {
    record.state = ReplicaSetState::OVER_REPLICATED;
  } else if (authoritative < record.requirements.minimum_authoritative_replicas) {
    record.state = ReplicaSetState::UNDER_REPLICATED;
  } else if (!record.meets_requirements()) {
    record.state = ReplicaSetState::DEGRADED;
  } else {
    record.state = ReplicaSetState::HEALTHY;
  }
  return record;
}

std::vector<std::string> explain_replica_set(const ReplicaSetRecord& record) {
  std::vector<std::string> findings;
  findings.push_back(std::string("state: ") + state_generation_label(record.state_id,
                                                                    record.state_generation));
  findings.push_back(std::string("replica set generation: ") + record.generation.str());
  findings.push_back(std::string("replica set state: ") + to_string(record.state));
  findings.push_back("authoritative replicas: " + std::to_string(record.authoritative_count) +
                     " (minimum " +
                     std::to_string(record.requirements.minimum_authoritative_replicas) +
                     ", desired " + std::to_string(record.requirements.desired_replicas) +
                     ", maximum " + std::to_string(record.requirements.maximum_replicas) + ")");
  findings.push_back("distinct clusters: " + std::to_string(record.distinct_clusters) +
                     " (required " + std::to_string(record.requirements.cluster_diversity) + ")");
  findings.push_back("distinct regions: " + std::to_string(record.distinct_regions) +
                     " (required " + std::to_string(record.requirements.region_diversity) + ")");
  findings.push_back("distinct failure domains: " +
                     std::to_string(record.distinct_failure_domains) + " (required " +
                     std::to_string(record.requirements.failure_domain_diversity) + ")");
  findings.push_back("degraded members excluded from the factor: " +
                     std::to_string(record.degraded_count));
  if (record.authoritative_count < record.requirements.minimum_authoritative_replicas) {
    findings.push_back("shortfall: " +
                       std::to_string(record.requirements.minimum_authoritative_replicas -
                                      record.authoritative_count) +
                       " authoritative replica(s) required to satisfy the minimum");
  }
  if (record.state == ReplicaSetState::CORRUPT) {
    findings.push_back("finding: a member reports corrupt or incompatible state");
  }
  if (record.state == ReplicaSetState::REVALIDATION_REQUIRED) {
    findings.push_back("finding: verification evidence is stale or absent for a member");
  }
  if (record.state == ReplicaSetState::SPLIT_BRAIN_RISK) {
    findings.push_back("finding: more than one ownership claim was observed for this generation");
  }
  if (record.revalidation_required) {
    findings.push_back("finding: the replica set itself requires revalidation");
  }
  return findings;
}

// --- authority --------------------------------------------------------------

bool requires_single_authoritative_owner(ConsistencyMode mode) noexcept {
  switch (mode) {
    case ConsistencyMode::SINGLE_WRITER:
    case ConsistencyMode::SNAPSHOT:
    case ConsistencyMode::APPEND_ONLY:
      return true;
    case ConsistencyMode::IMMUTABLE:
    case ConsistencyMode::OPAQUE_EXTERNAL_CONSISTENCY:
      return false;
  }
  return true;
}

// --- operations -------------------------------------------------------------

bool OperationRecord::is_terminal() const noexcept {
  switch (state) {
    case OperationState::COMPLETE:
    case OperationState::FAILED:
    case OperationState::ABANDONED:
    case OperationState::CANCELLED:
      return true;
    default:
      return false;
  }
}

bool OperationRecord::is_active() const noexcept {
  switch (state) {
    case OperationState::PLANNED:
    case OperationState::RESERVED:
    case OperationState::STAGING_DESTINATION:
    case OperationState::TRANSFERRING:
    case OperationState::TRANSFERRED:
    case OperationState::VERIFYING:
    case OperationState::PREPARED:
    case OperationState::COMMITTED:
    case OperationState::ADDED_TO_REPLICA_SET:
    case OperationState::SOURCE_RETIRING:
    case OperationState::OUTCOME_UNKNOWN:
      return true;
    default:
      return false;
  }
}

std::vector<std::string> explain_operation(const OperationRecord& record) {
  std::vector<std::string> lines;
  lines.push_back(std::string("operation: ") + record.operation_id.str());
  lines.push_back(std::string("kind: ") + to_string(record.kind));
  lines.push_back(std::string("state: ") + to_string(record.state));
  lines.push_back(std::string("state generation: ") +
                  state_generation_label(record.state_id, record.state_generation));
  lines.push_back(std::string("source cluster: ") + record.source_cluster.str());
  lines.push_back(std::string("source replica: ") + record.source_replica.str());
  lines.push_back(std::string("destination cluster: ") + record.destination_cluster.str());
  lines.push_back(std::string("destination replica: ") + record.destination_replica.str());
  lines.push_back(std::string("coordinator epoch: ") + record.planned_epoch.str());
  lines.push_back(std::string("replica set generation: ") +
                  record.committed_replica_set_generation.str());
  lines.push_back(std::string("destructive move: ") +
                  (record.destructive_move ? "yes" : "no"));
  lines.push_back(std::string("destination verified: ") +
                  (record.destination_verified ? "yes" : "no"));
  lines.push_back(std::string("destination committed: ") +
                  (record.destination_committed ? "yes" : "no"));
  lines.push_back(std::string("source retired: ") + (record.source_retired ? "yes" : "no"));
  if (!record.reason.empty()) {
    lines.push_back(std::string("reason: ") + record.reason);
  }
  if (!record.last_error.empty()) {
    lines.push_back(std::string("last error: ") + record.last_error);
  }
  for (const std::string& entry : record.journal) {
    lines.push_back(std::string("journal: ") + entry);
  }
  return lines;
}

// --- explanation ------------------------------------------------------------

void Explanation::add_labeled(const std::string& label, const std::string& value) {
  lines.push_back(label + ": " + value);
}

std::string Explanation::render() const {
  std::string text;
  if (!title.empty()) {
    text += title;
    text += '\n';
  }
  for (const std::string& line : lines) {
    text += line;
    text += '\n';
  }
  return text;
}

// --- snapshot queries -------------------------------------------------------

const StateGenerationRecord* FabricSnapshot::find_state(StateId state_id,
                                                        StateGeneration generation) const {
  for (const StateGenerationRecord& record : states) {
    if (record.state_id == state_id && record.generation == generation) {
      return &record;
    }
  }
  return nullptr;
}

const StateGenerationRecord* FabricSnapshot::current_generation(StateId state_id) const {
  for (const StateGenerationRecord& record : states) {
    if (record.state_id == state_id && record.is_current()) {
      return &record;
    }
  }
  return nullptr;
}

const ClusterRecord* FabricSnapshot::find_cluster(ClusterId cluster_id) const {
  for (const ClusterRecord& record : clusters) {
    if (record.cluster_id == cluster_id) {
      return &record;
    }
  }
  return nullptr;
}

const ReplicaRecord* FabricSnapshot::find_replica(ReplicaId replica_id) const {
  for (const ReplicaRecord& record : replicas) {
    if (record.replica_id == replica_id) {
      return &record;
    }
  }
  return nullptr;
}

const ReplicaSetRecord* FabricSnapshot::find_replica_set(StateId state_id,
                                                         StateGeneration generation) const {
  for (const ReplicaSetRecord& record : replica_sets) {
    if (record.state_id == state_id && record.state_generation == generation) {
      return &record;
    }
  }
  return nullptr;
}

const OperationRecord* FabricSnapshot::find_operation(OperationId operation_id) const {
  for (const OperationRecord& record : operations) {
    if (record.operation_id == operation_id) {
      return &record;
    }
  }
  return nullptr;
}

std::vector<ReplicaRecord> FabricSnapshot::replicas_of(StateId state_id,
                                                       StateGeneration generation) const {
  std::vector<ReplicaRecord> result;
  for (const ReplicaRecord& record : replicas) {
    if (record.state_id == state_id && record.state_generation == generation) {
      result.push_back(record);
    }
  }
  return result;
}

std::uint32_t FabricSnapshot::authoritative_replica_count(StateId state_id,
                                                          StateGeneration generation) const {
  std::uint32_t count = 0;
  for (const ReplicaRecord& record : replicas) {
    if (record.state_id == state_id && record.state_generation == generation &&
        replica_counts_toward_replication_factor(record)) {
      ++count;
    }
  }
  return count;
}

}  // namespace ccsf
