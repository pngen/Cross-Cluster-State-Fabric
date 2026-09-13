// Cross-Cluster State Fabric - taxonomy rendering, validation and parsing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/taxonomy.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace ccsf {
namespace {

template <class E, std::size_t N>
const char* lookup(const std::array<std::pair<E, const char*>, N>& table, E value) noexcept {
  for (const auto& entry : table) {
    if (entry.first == value) {
      return entry.second;
    }
  }
  return nullptr;
}

template <class E, std::size_t N>
bool contains(const std::array<std::pair<E, const char*>, N>& table, E value) noexcept {
  return lookup(table, value) != nullptr;
}

template <class E, std::size_t N>
Result<E> parse_with(const std::array<std::pair<E, const char*>, N>& table,
                     std::string_view text,
                     const char* enum_name) {
  for (const auto& entry : table) {
    if (text == entry.second) {
      return entry.first;
    }
  }
  std::string message = "unrecognized ";
  message += enum_name;
  message += " value: ";
  message.append(text.data(), text.size());
  return make_error(ErrorCode::INVALID_ARGUMENT, std::move(message));
}

using StateClassTable = std::array<std::pair<StateClass, const char*>, 11>;
constexpr StateClassTable kStateClass{{
    {StateClass::OPAQUE_STATE, "OPAQUE_STATE"},
    {StateClass::TENSOR_STATE, "TENSOR_STATE"},
    {StateClass::KV_STATE, "KV_STATE"},
    {StateClass::PREFIX_STATE, "PREFIX_STATE"},
    {StateClass::CHECKPOINT_STATE, "CHECKPOINT_STATE"},
    {StateClass::COMPILED_ARTIFACT, "COMPILED_ARTIFACT"},
    {StateClass::MODEL_COMPONENT, "MODEL_COMPONENT"},
    {StateClass::EXECUTION_SNAPSHOT, "EXECUTION_SNAPSHOT"},
    {StateClass::CACHE_OBJECT, "CACHE_OBJECT"},
    {StateClass::INTERMEDIATE_RESULT, "INTERMEDIATE_RESULT"},
    {StateClass::CUSTOM, "CUSTOM"},
}};

constexpr std::array<std::pair<ConsistencyMode, const char*>, 5> kConsistencyMode{{
    {ConsistencyMode::IMMUTABLE, "IMMUTABLE"},
    {ConsistencyMode::APPEND_ONLY, "APPEND_ONLY"},
    {ConsistencyMode::SINGLE_WRITER, "SINGLE_WRITER"},
    {ConsistencyMode::SNAPSHOT, "SNAPSHOT"},
    {ConsistencyMode::OPAQUE_EXTERNAL_CONSISTENCY, "OPAQUE_EXTERNAL_CONSISTENCY"},
}};

constexpr std::array<std::pair<StateGenerationLifecycle, const char*>, 8> kStateLifecycle{{
    {StateGenerationLifecycle::STAGED, "STAGED"},
    {StateGenerationLifecycle::CURRENT, "CURRENT"},
    {StateGenerationLifecycle::SUPERSEDED, "SUPERSEDED"},
    {StateGenerationLifecycle::HISTORICAL, "HISTORICAL"},
    {StateGenerationLifecycle::INVALID, "INVALID"},
    {StateGenerationLifecycle::CORRUPT, "CORRUPT"},
    {StateGenerationLifecycle::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
    {StateGenerationLifecycle::RETIRED, "RETIRED"},
}};

constexpr std::array<std::pair<DurabilityClass, const char*>, 6> kDurabilityClass{{
    {DurabilityClass::UNKNOWN, "UNKNOWN"},
    {DurabilityClass::VOLATILE, "VOLATILE"},
    {DurabilityClass::EPHEMERAL, "EPHEMERAL"},
    {DurabilityClass::STANDARD, "STANDARD"},
    {DurabilityClass::DURABLE, "DURABLE"},
    {DurabilityClass::ARCHIVAL, "ARCHIVAL"},
}};

constexpr std::array<std::pair<ReuseClass, const char*>, 5> kReuseClass{{
    {ReuseClass::UNKNOWN, "UNKNOWN"},
    {ReuseClass::SHARED_READ, "SHARED_READ"},
    {ReuseClass::SHARED_MUTABLE, "SHARED_MUTABLE"},
    {ReuseClass::EXCLUSIVE, "EXCLUSIVE"},
    {ReuseClass::ARCHIVAL_ONLY, "ARCHIVAL_ONLY"},
}};

constexpr std::array<std::pair<ClusterHealthState, const char*>, 6> kClusterHealth{{
    {ClusterHealthState::UNKNOWN, "UNKNOWN"},
    {ClusterHealthState::HEALTHY, "HEALTHY"},
    {ClusterHealthState::DEGRADED, "DEGRADED"},
    {ClusterHealthState::UNREACHABLE, "UNREACHABLE"},
    {ClusterHealthState::DRAINING, "DRAINING"},
    {ClusterHealthState::FAILED, "FAILED"},
}};

constexpr std::array<std::pair<ClusterLifecycle, const char*>, 6> kClusterLifecycle{{
    {ClusterLifecycle::REGISTERED, "REGISTERED"},
    {ClusterLifecycle::ACTIVE, "ACTIVE"},
    {ClusterLifecycle::SUSPECT, "SUSPECT"},
    {ClusterLifecycle::UNREACHABLE, "UNREACHABLE"},
    {ClusterLifecycle::REINCARNATED, "REINCARNATED"},
    {ClusterLifecycle::RETIRED, "RETIRED"},
}};

constexpr std::array<std::pair<ReplicaIntegrityState, const char*>, 6> kReplicaIntegrity{{
    {ReplicaIntegrityState::UNKNOWN, "UNKNOWN"},
    {ReplicaIntegrityState::UNVERIFIED, "UNVERIFIED"},
    {ReplicaIntegrityState::VERIFYING, "VERIFYING"},
    {ReplicaIntegrityState::VERIFIED, "VERIFIED"},
    {ReplicaIntegrityState::MISMATCH, "MISMATCH"},
    {ReplicaIntegrityState::CORRUPT, "CORRUPT"},
}};

constexpr std::array<std::pair<ReplicaCompatibilityState, const char*>, 4> kReplicaCompatibility{{
    {ReplicaCompatibilityState::UNKNOWN, "UNKNOWN"},
    {ReplicaCompatibilityState::INCOMPATIBLE, "INCOMPATIBLE"},
    {ReplicaCompatibilityState::COMPATIBLE, "COMPATIBLE"},
    {ReplicaCompatibilityState::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
}};

constexpr std::array<std::pair<ReplicaDurabilityState, const char*>, 5> kReplicaDurability{{
    {ReplicaDurabilityState::UNKNOWN, "UNKNOWN"},
    {ReplicaDurabilityState::VOLATILE, "VOLATILE"},
    {ReplicaDurabilityState::STANDARD, "STANDARD"},
    {ReplicaDurabilityState::DURABLE, "DURABLE"},
    {ReplicaDurabilityState::ARCHIVAL, "ARCHIVAL"},
}};

constexpr std::array<std::pair<ReplicaAvailabilityState, const char*>, 5> kReplicaAvailability{{
    {ReplicaAvailabilityState::UNKNOWN, "UNKNOWN"},
    {ReplicaAvailabilityState::PRESENT, "PRESENT"},
    {ReplicaAvailabilityState::ABSENT, "ABSENT"},
    {ReplicaAvailabilityState::UNREACHABLE, "UNREACHABLE"},
    {ReplicaAvailabilityState::DRAINING, "DRAINING"},
}};

constexpr std::array<std::pair<ReplicaAuthorityState, const char*>, 7> kReplicaAuthority{{
    {ReplicaAuthorityState::NONE, "NONE"},
    {ReplicaAuthorityState::HISTORICAL, "HISTORICAL"},
    {ReplicaAuthorityState::CANDIDATE, "CANDIDATE"},
    {ReplicaAuthorityState::AUTHORITATIVE_FOR_REUSE, "AUTHORITATIVE_FOR_REUSE"},
    {ReplicaAuthorityState::AUTHORITATIVE_SOURCE, "AUTHORITATIVE_SOURCE"},
    {ReplicaAuthorityState::AUTHORITATIVE_OWNER, "AUTHORITATIVE_OWNER"},
    {ReplicaAuthorityState::FENCED, "FENCED"},
}};

constexpr std::array<std::pair<ReplicaLifecycle, const char*>, 17> kReplicaLifecycle{{
    {ReplicaLifecycle::PLANNED, "PLANNED"},
    {ReplicaLifecycle::RESERVED, "RESERVED"},
    {ReplicaLifecycle::STAGING, "STAGING"},
    {ReplicaLifecycle::TRANSFERRING, "TRANSFERRING"},
    {ReplicaLifecycle::TRANSFERRED, "TRANSFERRED"},
    {ReplicaLifecycle::VERIFYING, "VERIFYING"},
    {ReplicaLifecycle::PREPARED, "PREPARED"},
    {ReplicaLifecycle::AUTHORITATIVE, "AUTHORITATIVE"},
    {ReplicaLifecycle::DEGRADED, "DEGRADED"},
    {ReplicaLifecycle::STALE, "STALE"},
    {ReplicaLifecycle::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
    {ReplicaLifecycle::SUPERSEDED, "SUPERSEDED"},
    {ReplicaLifecycle::QUARANTINED, "QUARANTINED"},
    {ReplicaLifecycle::CORRUPT, "CORRUPT"},
    {ReplicaLifecycle::RETIRING, "RETIRING"},
    {ReplicaLifecycle::RETIRED, "RETIRED"},
    {ReplicaLifecycle::ABANDONED, "ABANDONED"},
}};

constexpr std::array<std::pair<ReplicaSetState, const char*>, 10> kReplicaSetState{{
    {ReplicaSetState::HEALTHY, "HEALTHY"},
    {ReplicaSetState::DEGRADED, "DEGRADED"},
    {ReplicaSetState::UNDER_REPLICATED, "UNDER_REPLICATED"},
    {ReplicaSetState::OVER_REPLICATED, "OVER_REPLICATED"},
    {ReplicaSetState::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
    {ReplicaSetState::SPLIT_BRAIN_RISK, "SPLIT_BRAIN_RISK"},
    {ReplicaSetState::NO_AUTHORITATIVE_REPLICA, "NO_AUTHORITATIVE_REPLICA"},
    {ReplicaSetState::CORRUPT, "CORRUPT"},
    {ReplicaSetState::REBUILDING, "REBUILDING"},
    {ReplicaSetState::RETIRING, "RETIRING"},
}};

constexpr std::array<std::pair<CompatibilityResult, const char*>, 4> kCompatibilityResult{{
    {CompatibilityResult::COMPATIBLE, "COMPATIBLE"},
    {CompatibilityResult::INCOMPATIBLE, "INCOMPATIBLE"},
    {CompatibilityResult::UNKNOWN, "UNKNOWN"},
    {CompatibilityResult::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
}};

constexpr std::array<std::pair<OperationKind, const char*>, 6> kOperationKind{{
    {OperationKind::REPLICATION, "REPLICATION"},
    {OperationKind::MIGRATION, "MIGRATION"},
    {OperationKind::OWNERSHIP_TRANSFER, "OWNERSHIP_TRANSFER"},
    {OperationKind::RETIREMENT, "RETIREMENT"},
    {OperationKind::REVALIDATION, "REVALIDATION"},
    {OperationKind::RECONSTRUCTION, "RECONSTRUCTION"},
}};

constexpr std::array<std::pair<OperationState, const char*>, 15> kOperationState{{
    {OperationState::PLANNED, "PLANNED"},
    {OperationState::RESERVED, "RESERVED"},
    {OperationState::STAGING_DESTINATION, "STAGING_DESTINATION"},
    {OperationState::TRANSFERRING, "TRANSFERRING"},
    {OperationState::TRANSFERRED, "TRANSFERRED"},
    {OperationState::VERIFYING, "VERIFYING"},
    {OperationState::PREPARED, "PREPARED"},
    {OperationState::COMMITTED, "COMMITTED"},
    {OperationState::ADDED_TO_REPLICA_SET, "ADDED_TO_REPLICA_SET"},
    {OperationState::SOURCE_RETIRING, "SOURCE_RETIRING"},
    {OperationState::COMPLETE, "COMPLETE"},
    {OperationState::FAILED, "FAILED"},
    {OperationState::ABANDONED, "ABANDONED"},
    {OperationState::CANCELLED, "CANCELLED"},
    {OperationState::OUTCOME_UNKNOWN, "OUTCOME_UNKNOWN"},
}};

constexpr std::array<std::pair<CancellationOutcome, const char*>, 5> kCancellationOutcome{{
    {CancellationOutcome::CANCELLED_BEFORE_TRANSFER, "CANCELLED_BEFORE_TRANSFER"},
    {CancellationOutcome::CANCELLED_DURING_TRANSFER, "CANCELLED_DURING_TRANSFER"},
    {CancellationOutcome::CANCELLED_BEFORE_COMMIT, "CANCELLED_BEFORE_COMMIT"},
    {CancellationOutcome::COMMIT_ALREADY_AUTHORITATIVE, "COMMIT_ALREADY_AUTHORITATIVE"},
    {CancellationOutcome::OUTCOME_UNKNOWN, "OUTCOME_UNKNOWN"},
}};

constexpr std::array<std::pair<DecisionKind, const char*>, 8> kDecisionKind{{
    {DecisionKind::REUSE_LOCAL, "REUSE_LOCAL"},
    {DecisionKind::REUSE_REMOTE, "REUSE_REMOTE"},
    {DecisionKind::REPLICATE, "REPLICATE"},
    {DecisionKind::MIGRATE, "MIGRATE"},
    {DecisionKind::RECONSTRUCT, "RECONSTRUCT"},
    {DecisionKind::DEFER, "DEFER"},
    {DecisionKind::REJECT, "REJECT"},
    {DecisionKind::UNKNOWN, "UNKNOWN"},
}};

constexpr std::array<std::pair<ReplicationPolicyForm, const char*>, 5> kReplicationPolicyForm{{
    {ReplicationPolicyForm::SINGLE, "SINGLE"},
    {ReplicationPolicyForm::N_REPLICAS, "N_REPLICAS"},
    {ReplicationPolicyForm::REGION_DIVERSE, "REGION_DIVERSE"},
    {ReplicationPolicyForm::CLUSTER_DIVERSE, "CLUSTER_DIVERSE"},
    {ReplicationPolicyForm::POLICY_CUSTOM, "POLICY_CUSTOM"},
}};

constexpr std::array<std::pair<DegradedBehavior, const char*>, 3> kDegradedBehavior{{
    {DegradedBehavior::REJECT_REUSE, "REJECT_REUSE"},
    {DegradedBehavior::ALLOW_HISTORICAL_READ_ONLY, "ALLOW_HISTORICAL_READ_ONLY"},
    {DegradedBehavior::ALLOW_VERIFIED_READ_ONLY, "ALLOW_VERIFIED_READ_ONLY"},
}};

constexpr std::array<std::pair<OperationFailureAction, const char*>, 3> kFailureAction{{
    {OperationFailureAction::ABANDON_DESTINATION, "ABANDON_DESTINATION"},
    {OperationFailureAction::QUARANTINE_DESTINATION, "QUARANTINE_DESTINATION"},
    {OperationFailureAction::RETAIN_FOR_RETRY, "RETAIN_FOR_RETRY"},
}};

}  // namespace

const char* to_string(StateClass value) noexcept {
  const char* name = lookup(kStateClass, value);
  return name != nullptr ? name : "UNRECOGNIZED_STATE_CLASS";
}
const char* to_string(ConsistencyMode value) noexcept {
  const char* name = lookup(kConsistencyMode, value);
  return name != nullptr ? name : "UNRECOGNIZED_CONSISTENCY_MODE";
}
const char* to_string(StateGenerationLifecycle value) noexcept {
  const char* name = lookup(kStateLifecycle, value);
  return name != nullptr ? name : "UNRECOGNIZED_STATE_LIFECYCLE";
}
const char* to_string(DurabilityClass value) noexcept {
  const char* name = lookup(kDurabilityClass, value);
  return name != nullptr ? name : "UNRECOGNIZED_DURABILITY_CLASS";
}
const char* to_string(ReuseClass value) noexcept {
  const char* name = lookup(kReuseClass, value);
  return name != nullptr ? name : "UNRECOGNIZED_REUSE_CLASS";
}
const char* to_string(ClusterHealthState value) noexcept {
  const char* name = lookup(kClusterHealth, value);
  return name != nullptr ? name : "UNRECOGNIZED_CLUSTER_HEALTH";
}
const char* to_string(ClusterLifecycle value) noexcept {
  const char* name = lookup(kClusterLifecycle, value);
  return name != nullptr ? name : "UNRECOGNIZED_CLUSTER_LIFECYCLE";
}
const char* to_string(ReplicaIntegrityState value) noexcept {
  const char* name = lookup(kReplicaIntegrity, value);
  return name != nullptr ? name : "UNRECOGNIZED_REPLICA_INTEGRITY";
}
const char* to_string(ReplicaCompatibilityState value) noexcept {
  const char* name = lookup(kReplicaCompatibility, value);
  return name != nullptr ? name : "UNRECOGNIZED_REPLICA_COMPATIBILITY";
}
const char* to_string(ReplicaDurabilityState value) noexcept {
  const char* name = lookup(kReplicaDurability, value);
  return name != nullptr ? name : "UNRECOGNIZED_REPLICA_DURABILITY";
}
const char* to_string(ReplicaAvailabilityState value) noexcept {
  const char* name = lookup(kReplicaAvailability, value);
  return name != nullptr ? name : "UNRECOGNIZED_REPLICA_AVAILABILITY";
}
const char* to_string(ReplicaAuthorityState value) noexcept {
  const char* name = lookup(kReplicaAuthority, value);
  return name != nullptr ? name : "UNRECOGNIZED_REPLICA_AUTHORITY";
}
const char* to_string(ReplicaLifecycle value) noexcept {
  const char* name = lookup(kReplicaLifecycle, value);
  return name != nullptr ? name : "UNRECOGNIZED_REPLICA_LIFECYCLE";
}
const char* to_string(ReplicaSetState value) noexcept {
  const char* name = lookup(kReplicaSetState, value);
  return name != nullptr ? name : "UNRECOGNIZED_REPLICA_SET_STATE";
}
const char* to_string(CompatibilityResult value) noexcept {
  const char* name = lookup(kCompatibilityResult, value);
  return name != nullptr ? name : "UNRECOGNIZED_COMPATIBILITY_RESULT";
}
const char* to_string(OperationKind value) noexcept {
  const char* name = lookup(kOperationKind, value);
  return name != nullptr ? name : "UNRECOGNIZED_OPERATION_KIND";
}
const char* to_string(OperationState value) noexcept {
  const char* name = lookup(kOperationState, value);
  return name != nullptr ? name : "UNRECOGNIZED_OPERATION_STATE";
}
const char* to_string(CancellationOutcome value) noexcept {
  const char* name = lookup(kCancellationOutcome, value);
  return name != nullptr ? name : "UNRECOGNIZED_CANCELLATION_OUTCOME";
}
const char* to_string(DecisionKind value) noexcept {
  const char* name = lookup(kDecisionKind, value);
  return name != nullptr ? name : "UNRECOGNIZED_DECISION_KIND";
}
const char* to_string(ReplicationPolicyForm value) noexcept {
  const char* name = lookup(kReplicationPolicyForm, value);
  return name != nullptr ? name : "UNRECOGNIZED_REPLICATION_POLICY_FORM";
}
const char* to_string(DegradedBehavior value) noexcept {
  const char* name = lookup(kDegradedBehavior, value);
  return name != nullptr ? name : "UNRECOGNIZED_DEGRADED_BEHAVIOR";
}
const char* to_string(OperationFailureAction value) noexcept {
  const char* name = lookup(kFailureAction, value);
  return name != nullptr ? name : "UNRECOGNIZED_FAILURE_ACTION";
}

bool is_valid(StateClass value) noexcept { return contains(kStateClass, value); }
bool is_valid(ConsistencyMode value) noexcept { return contains(kConsistencyMode, value); }
bool is_valid(StateGenerationLifecycle value) noexcept { return contains(kStateLifecycle, value); }
bool is_valid(DurabilityClass value) noexcept { return contains(kDurabilityClass, value); }
bool is_valid(ReuseClass value) noexcept { return contains(kReuseClass, value); }
bool is_valid(ClusterHealthState value) noexcept { return contains(kClusterHealth, value); }
bool is_valid(ClusterLifecycle value) noexcept { return contains(kClusterLifecycle, value); }
bool is_valid(ReplicaIntegrityState value) noexcept { return contains(kReplicaIntegrity, value); }
bool is_valid(ReplicaCompatibilityState value) noexcept {
  return contains(kReplicaCompatibility, value);
}
bool is_valid(ReplicaDurabilityState value) noexcept { return contains(kReplicaDurability, value); }
bool is_valid(ReplicaAvailabilityState value) noexcept {
  return contains(kReplicaAvailability, value);
}
bool is_valid(ReplicaAuthorityState value) noexcept { return contains(kReplicaAuthority, value); }
bool is_valid(ReplicaLifecycle value) noexcept { return contains(kReplicaLifecycle, value); }
bool is_valid(ReplicaSetState value) noexcept { return contains(kReplicaSetState, value); }
bool is_valid(CompatibilityResult value) noexcept { return contains(kCompatibilityResult, value); }
bool is_valid(OperationKind value) noexcept { return contains(kOperationKind, value); }
bool is_valid(OperationState value) noexcept { return contains(kOperationState, value); }
bool is_valid(CancellationOutcome value) noexcept { return contains(kCancellationOutcome, value); }
bool is_valid(DecisionKind value) noexcept { return contains(kDecisionKind, value); }
bool is_valid(ReplicationPolicyForm value) noexcept {
  return contains(kReplicationPolicyForm, value);
}
bool is_valid(DegradedBehavior value) noexcept { return contains(kDegradedBehavior, value); }
bool is_valid(OperationFailureAction value) noexcept { return contains(kFailureAction, value); }

Result<StateClass> parse_state_class(std::string_view text) {
  return parse_with(kStateClass, text, "StateClass");
}
Result<ConsistencyMode> parse_consistency_mode(std::string_view text) {
  return parse_with(kConsistencyMode, text, "ConsistencyMode");
}
Result<DurabilityClass> parse_durability_class(std::string_view text) {
  return parse_with(kDurabilityClass, text, "DurabilityClass");
}
Result<ReuseClass> parse_reuse_class(std::string_view text) {
  return parse_with(kReuseClass, text, "ReuseClass");
}
Result<ReplicationPolicyForm> parse_replication_policy_form(std::string_view text) {
  return parse_with(kReplicationPolicyForm, text, "ReplicationPolicyForm");
}
Result<DecisionKind> parse_decision_kind(std::string_view text) {
  return parse_with(kDecisionKind, text, "DecisionKind");
}
Result<OperationState> parse_operation_state(std::string_view text) {
  return parse_with(kOperationState, text, "OperationState");
}
Result<ReplicaLifecycle> parse_replica_lifecycle(std::string_view text) {
  return parse_with(kReplicaLifecycle, text, "ReplicaLifecycle");
}
Result<ClusterHealthState> parse_cluster_health_state(std::string_view text) {
  return parse_with(kClusterHealth, text, "ClusterHealthState");
}
Result<DegradedBehavior> parse_degraded_behavior(std::string_view text) {
  return parse_with(kDegradedBehavior, text, "DegradedBehavior");
}

}  // namespace ccsf
