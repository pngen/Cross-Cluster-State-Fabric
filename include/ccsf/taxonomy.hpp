// Cross-Cluster State Fabric - domain taxonomy.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every lifecycle dimension that can be reasoned about independently is modeled
// as its own enum rather than being folded into one state machine. This keeps
// orthogonal facts orthogonal: a replica can be durable but not current, or
// compatible but not verified.

#ifndef CCSF_TAXONOMY_HPP
#define CCSF_TAXONOMY_HPP

#include <cstdint>
#include <string_view>

#include "ccsf/result.hpp"

namespace ccsf {

/// Broad kind of reusable machine state. The runtime is type-agnostic: the
/// state class drives policy selection, never hard-wired special cases.
enum class StateClass : std::uint16_t {
  OPAQUE_STATE = 0,
  TENSOR_STATE = 1,
  KV_STATE = 2,
  PREFIX_STATE = 3,
  CHECKPOINT_STATE = 4,
  COMPILED_ARTIFACT = 5,
  MODEL_COMPONENT = 6,
  EXECUTION_SNAPSHOT = 7,
  CACHE_OBJECT = 8,
  INTERMEDIATE_RESULT = 9,
  CUSTOM = 10
};

/// Consistency semantics a state generation claims. The runtime never claims a
/// stronger guarantee than the mode it was given.
enum class ConsistencyMode : std::uint16_t {
  IMMUTABLE = 0,
  APPEND_ONLY = 1,
  SINGLE_WRITER = 2,
  SNAPSHOT = 3,
  OPAQUE_EXTERNAL_CONSISTENCY = 4
};

/// Lifecycle of one state generation.
enum class StateGenerationLifecycle : std::uint16_t {
  STAGED = 0,
  CURRENT = 1,
  SUPERSEDED = 2,
  HISTORICAL = 3,
  INVALID = 4,
  CORRUPT = 5,
  REVALIDATION_REQUIRED = 6,
  RETIRED = 7
};

/// Durability obligation attached to state or to a replica.
enum class DurabilityClass : std::uint16_t {
  UNKNOWN = 0,
  VOLATILE = 1,
  EPHEMERAL = 2,
  STANDARD = 3,
  DURABLE = 4,
  ARCHIVAL = 5
};

/// How broadly a state generation may be reused.
enum class ReuseClass : std::uint16_t {
  UNKNOWN = 0,
  SHARED_READ = 1,
  SHARED_MUTABLE = 2,
  EXCLUSIVE = 3,
  ARCHIVAL_ONLY = 4
};

/// Observed health of a registered cluster.
enum class ClusterHealthState : std::uint16_t {
  UNKNOWN = 0,
  HEALTHY = 1,
  DEGRADED = 2,
  UNREACHABLE = 3,
  DRAINING = 4,
  FAILED = 5
};

/// Registration lifecycle of a cluster identity.
enum class ClusterLifecycle : std::uint16_t {
  REGISTERED = 0,
  ACTIVE = 1,
  SUSPECT = 2,
  UNREACHABLE = 3,
  REINCARNATED = 4,
  RETIRED = 5
};

/// Outcome of content integrity verification for one replica.
enum class ReplicaIntegrityState : std::uint16_t {
  UNKNOWN = 0,
  UNVERIFIED = 1,
  VERIFYING = 2,
  VERIFIED = 3,
  MISMATCH = 4,
  CORRUPT = 5
};

/// Outcome of compatibility gating for one replica.
enum class ReplicaCompatibilityState : std::uint16_t {
  UNKNOWN = 0,
  INCOMPATIBLE = 1,
  COMPATIBLE = 2,
  REVALIDATION_REQUIRED = 3
};

/// Durability actually established for one replica.
enum class ReplicaDurabilityState : std::uint16_t {
  UNKNOWN = 0,
  VOLATILE = 1,
  STANDARD = 2,
  DURABLE = 3,
  ARCHIVAL = 4
};

/// Observed availability of one replica.
enum class ReplicaAvailabilityState : std::uint16_t {
  UNKNOWN = 0,
  PRESENT = 1,
  ABSENT = 2,
  UNREACHABLE = 3,
  DRAINING = 4
};

/// Authority dimension. Authority is never inferred from presence.
enum class ReplicaAuthorityState : std::uint16_t {
  NONE = 0,
  HISTORICAL = 1,
  CANDIDATE = 2,
  AUTHORITATIVE_FOR_REUSE = 3,
  AUTHORITATIVE_SOURCE = 4,
  AUTHORITATIVE_OWNER = 5,
  FENCED = 6
};

/// Operational lifecycle of one replica.
enum class ReplicaLifecycle : std::uint16_t {
  PLANNED = 0,
  RESERVED = 1,
  STAGING = 2,
  TRANSFERRING = 3,
  TRANSFERRED = 4,
  VERIFYING = 5,
  PREPARED = 6,
  AUTHORITATIVE = 7,
  DEGRADED = 8,
  STALE = 9,
  REVALIDATION_REQUIRED = 10,
  SUPERSEDED = 11,
  QUARANTINED = 12,
  CORRUPT = 13,
  RETIRING = 14,
  RETIRED = 15,
  ABANDONED = 16
};

/// Aggregate health of one replica set generation.
enum class ReplicaSetState : std::uint16_t {
  HEALTHY = 0,
  DEGRADED = 1,
  UNDER_REPLICATED = 2,
  OVER_REPLICATED = 3,
  REVALIDATION_REQUIRED = 4,
  SPLIT_BRAIN_RISK = 5,
  NO_AUTHORITATIVE_REPLICA = 6,
  CORRUPT = 7,
  REBUILDING = 8,
  RETIRING = 9
};

/// Result of gating a candidate against a compatibility requirement.
enum class CompatibilityResult : std::uint16_t {
  COMPATIBLE = 0,
  INCOMPATIBLE = 1,
  UNKNOWN = 2,
  REVALIDATION_REQUIRED = 3
};

/// Kind of governed cross-cluster operation.
enum class OperationKind : std::uint16_t {
  REPLICATION = 0,
  MIGRATION = 1,
  OWNERSHIP_TRANSFER = 2,
  RETIREMENT = 3,
  REVALIDATION = 4,
  RECONSTRUCTION = 5
};

/// Transactional state of a governed operation.
enum class OperationState : std::uint16_t {
  PLANNED = 0,
  RESERVED = 1,
  STAGING_DESTINATION = 2,
  TRANSFERRING = 3,
  TRANSFERRED = 4,
  VERIFYING = 5,
  PREPARED = 6,
  COMMITTED = 7,
  ADDED_TO_REPLICA_SET = 8,
  SOURCE_RETIRING = 9,
  COMPLETE = 10,
  FAILED = 11,
  ABANDONED = 12,
  CANCELLED = 13,
  OUTCOME_UNKNOWN = 14
};

/// Terminal classification of a cancelled operation.
enum class CancellationOutcome : std::uint16_t {
  CANCELLED_BEFORE_TRANSFER = 0,
  CANCELLED_DURING_TRANSFER = 1,
  CANCELLED_BEFORE_COMMIT = 2,
  COMMIT_ALREADY_AUTHORITATIVE = 3,
  OUTCOME_UNKNOWN = 4
};

/// Answer produced by the placement / economics planner.
enum class DecisionKind : std::uint16_t {
  REUSE_LOCAL = 0,
  REUSE_REMOTE = 1,
  REPLICATE = 2,
  MIGRATE = 3,
  RECONSTRUCT = 4,
  DEFER = 5,
  REJECT = 6,
  UNKNOWN = 7
};

/// Named replication policy forms. The runtime implements exactly these.
enum class ReplicationPolicyForm : std::uint16_t {
  SINGLE = 0,
  N_REPLICAS = 1,
  REGION_DIVERSE = 2,
  CLUSTER_DIVERSE = 3,
  POLICY_CUSTOM = 4
};

/// Behavior permitted when a replica set is degraded.
enum class DegradedBehavior : std::uint16_t {
  REJECT_REUSE = 0,
  ALLOW_HISTORICAL_READ_ONLY = 1,
  ALLOW_VERIFIED_READ_ONLY = 2
};

/// Action taken when a destination fails eligibility or verification.
enum class OperationFailureAction : std::uint16_t {
  ABANDON_DESTINATION = 0,
  QUARANTINE_DESTINATION = 1,
  RETAIN_FOR_RETRY = 2
};

// --- Rendering -------------------------------------------------------------
const char* to_string(StateClass value) noexcept;
const char* to_string(ConsistencyMode value) noexcept;
const char* to_string(StateGenerationLifecycle value) noexcept;
const char* to_string(DurabilityClass value) noexcept;
const char* to_string(ReuseClass value) noexcept;
const char* to_string(ClusterHealthState value) noexcept;
const char* to_string(ClusterLifecycle value) noexcept;
const char* to_string(ReplicaIntegrityState value) noexcept;
const char* to_string(ReplicaCompatibilityState value) noexcept;
const char* to_string(ReplicaDurabilityState value) noexcept;
const char* to_string(ReplicaAvailabilityState value) noexcept;
const char* to_string(ReplicaAuthorityState value) noexcept;
const char* to_string(ReplicaLifecycle value) noexcept;
const char* to_string(ReplicaSetState value) noexcept;
const char* to_string(CompatibilityResult value) noexcept;
const char* to_string(OperationKind value) noexcept;
const char* to_string(OperationState value) noexcept;
const char* to_string(CancellationOutcome value) noexcept;
const char* to_string(DecisionKind value) noexcept;
const char* to_string(ReplicationPolicyForm value) noexcept;
const char* to_string(DegradedBehavior value) noexcept;
const char* to_string(OperationFailureAction value) noexcept;

// --- Validation (durable decode) -------------------------------------------
bool is_valid(StateClass value) noexcept;
bool is_valid(ConsistencyMode value) noexcept;
bool is_valid(StateGenerationLifecycle value) noexcept;
bool is_valid(DurabilityClass value) noexcept;
bool is_valid(ReuseClass value) noexcept;
bool is_valid(ClusterHealthState value) noexcept;
bool is_valid(ClusterLifecycle value) noexcept;
bool is_valid(ReplicaIntegrityState value) noexcept;
bool is_valid(ReplicaCompatibilityState value) noexcept;
bool is_valid(ReplicaDurabilityState value) noexcept;
bool is_valid(ReplicaAvailabilityState value) noexcept;
bool is_valid(ReplicaAuthorityState value) noexcept;
bool is_valid(ReplicaLifecycle value) noexcept;
bool is_valid(ReplicaSetState value) noexcept;
bool is_valid(CompatibilityResult value) noexcept;
bool is_valid(OperationKind value) noexcept;
bool is_valid(OperationState value) noexcept;
bool is_valid(CancellationOutcome value) noexcept;
bool is_valid(DecisionKind value) noexcept;
bool is_valid(ReplicationPolicyForm value) noexcept;
bool is_valid(DegradedBehavior value) noexcept;
bool is_valid(OperationFailureAction value) noexcept;

// --- Parsing (configuration and CLI) ---------------------------------------
Result<StateClass> parse_state_class(std::string_view text);
Result<ConsistencyMode> parse_consistency_mode(std::string_view text);
Result<DurabilityClass> parse_durability_class(std::string_view text);
Result<ReuseClass> parse_reuse_class(std::string_view text);
Result<ReplicationPolicyForm> parse_replication_policy_form(std::string_view text);
Result<DecisionKind> parse_decision_kind(std::string_view text);
Result<OperationState> parse_operation_state(std::string_view text);
Result<ReplicaLifecycle> parse_replica_lifecycle(std::string_view text);
Result<ClusterHealthState> parse_cluster_health_state(std::string_view text);
Result<DegradedBehavior> parse_degraded_behavior(std::string_view text);

}  // namespace ccsf

#endif  // CCSF_TAXONOMY_HPP
