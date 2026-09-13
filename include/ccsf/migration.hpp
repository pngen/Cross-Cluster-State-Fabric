// Cross-Cluster State Fabric - replication and migration transactions.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Migration and replication are explicit transactions. Every step revalidates
// the generations captured when the operation was authorized; a step that
// observes a superseded generation fails closed instead of committing.

#ifndef CCSF_MIGRATION_HPP
#define CCSF_MIGRATION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/clock.hpp"
#include "ccsf/digest.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

inline constexpr std::size_t kMaxOperationJournalEntries = 512;

/// Request to replicate a state generation into another cluster.
struct ReplicationRequest {
  StateId state_id;
  StateGeneration state_generation;
  ClusterId destination_cluster;
  ClusterId source_cluster;
  PlacementPolicyRecord placement_policy;
  ReplicationPolicyRecord replication_policy;
  CoordinatorEpoch expected_coordinator_epoch;
  std::uint32_t desired_replicas_override{0};
};

/// Request to migrate a state generation to another cluster. A migration moves
/// authority; it retires the source only when the request asks for it and the
/// destination commit is already durable.
struct MigrationRequest {
  StateId state_id;
  StateGeneration state_generation;
  ClusterId destination_cluster;
  ClusterId source_cluster;
  bool retire_source_after_commit{false};
  bool allow_source_retirement_before_commit{false};
  PlacementPolicyRecord placement_policy;
  ReplicationPolicyRecord replication_policy;
  CoordinatorEpoch expected_coordinator_epoch;
};

/// Request to hand mutable ownership to another replica.
struct OwnershipTransferRequest {
  StateId state_id;
  StateGeneration state_generation;
  ReplicaId expected_current_owner;
  ReplicaId new_owner_replica;
  ReplicaGeneration new_owner_replica_generation;
  ClusterId new_owner_cluster;
  CommitId handoff_commit;
  OperationGeneration expected_operation_generation;
  CoordinatorEpoch expected_coordinator_epoch;
};

/// The authority a planned operation carries into its execution steps.
struct TransferAuthorization {
  OperationId operation_id;
  OperationGeneration operation_generation;
  OperationKind kind{OperationKind::REPLICATION};
  MigrationId migration_id;
  MigrationGeneration migration_generation;
  TransferId transfer_id;
  TransferGeneration transfer_generation;
  ReservationId reservation_id;
  ReservationGeneration reservation_generation;

  StateId state_id;
  StateGeneration state_generation;
  ReplicaSetId replica_set_id;
  ReplicaSetGeneration replica_set_generation;

  ReplicaId source_replica;
  ReplicaGeneration source_replica_generation;
  ClusterId source_cluster;
  ClusterIncarnationId source_incarnation;

  ReplicaId destination_replica;
  ReplicaGeneration destination_replica_generation;
  ClusterId destination_cluster;
  ClusterIncarnationId destination_incarnation;

  PlacementId placement_id;
  PlacementGeneration placement_generation;
  CoordinatorEpoch coordinator_epoch;

  SizeValue transfer_bytes;
  Digest expected_digest;
  IntegrityAlgorithm digest_algorithm{IntegrityAlgorithm::SHA256};
  Tick reservation_expires_at{0};
  Tick authorized_at{0};
  bool destructive_move{false};

  friend bool operator==(const TransferAuthorization&, const TransferAuthorization&) = default;
};

/// The durable record of one governed operation.
struct OperationRecord {
  OperationId operation_id;
  OperationGeneration operation_generation;
  OperationKind kind{OperationKind::REPLICATION};
  OperationState state{OperationState::PLANNED};

  StateId state_id;
  StateGeneration state_generation;

  ReplicaId source_replica;
  ReplicaGeneration source_replica_generation;
  ClusterId source_cluster;
  ClusterIncarnationId source_incarnation;

  ReplicaId destination_replica;
  ReplicaGeneration destination_replica_generation;
  ClusterId destination_cluster;
  ClusterIncarnationId destination_incarnation;

  MigrationId migration_id;
  MigrationGeneration migration_generation;
  TransferId transfer_id;
  TransferGeneration transfer_generation;
  ReservationId reservation_id;
  ReservationGeneration reservation_generation;

  /// The policies this operation was authorized under. Commit re-resolves them
  /// so that a policy change cannot be smuggled in mid-transaction.
  ReplicationPolicyId replication_policy_id;
  ReplicationPolicyGeneration replication_policy_generation_recorded;
  PlacementPolicyId placement_policy_id;

  ReplicaSetGeneration source_replica_set_generation;
  ReplicaSetGeneration committed_replica_set_generation;
  PlacementGeneration placement_generation;
  PlacementPolicyGeneration placement_policy_generation;
  ReplicationPolicyGeneration replication_policy_generation;

  CoordinatorEpoch planned_epoch;
  CoordinatorEpoch last_epoch;
  CommitId commit_id;

  SizeValue transfer_bytes;
  SizeValue transferred_bytes;
  Digest expected_digest;
  Digest measured_digest;

  OperationFailureAction failure_action{OperationFailureAction::ABANDON_DESTINATION};
  CancellationOutcome cancellation{CancellationOutcome::OUTCOME_UNKNOWN};

  Tick planned_at{0};
  Tick updated_at{0};
  Tick reservation_expires_at{0};

  bool destructive_move{false};
  bool source_retired{false};
  bool destination_verified{false};
  bool destination_committed{false};
  bool recovery_required{false};

  std::string reason;
  std::string last_error;
  std::vector<std::string> journal;

  [[nodiscard]] bool is_terminal() const noexcept;
  [[nodiscard]] bool is_active() const noexcept;
};

/// Commit request for a prepared migration or replication.
struct OperationCommit {
  OperationId operation_id;
  OperationGeneration operation_generation;
  CommitId commit_id;
  CoordinatorEpoch expected_coordinator_epoch;
  bool retire_source{false};
};

/// Cancellation request for an in-flight operation.
struct OperationCancellation {
  OperationId operation_id;
  OperationGeneration operation_generation;
  CoordinatorEpoch expected_coordinator_epoch;
  std::string reason;
};

/// Deterministically renders an operation as an ordered explanation block.
std::vector<std::string> explain_operation(const OperationRecord& record);

}  // namespace ccsf

#endif  // CCSF_MIGRATION_HPP
