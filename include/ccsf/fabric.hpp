// Cross-Cluster State Fabric - public runtime API.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Cross-Cluster State Fabric governs authoritative placement, replication,
// migration, reuse, durability, integrity, compatibility and economic movement
// of reusable machine state across independent clusters and regions.
//
// The runtime answers, for any reusable state: which exact state generation
// exists, where its authoritative replicas are, which replicas are still
// current, which may be reused now, which must be fenced, when transfer beats
// reconstruction, when migration is legal, how authority survives failure and
// restart, and why the current placement remains valid.

#ifndef CCSF_FABRIC_HPP
#define CCSF_FABRIC_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ccsf/authority.hpp"
#include "ccsf/cluster.hpp"
#include "ccsf/compatibility.hpp"
#include "ccsf/economics.hpp"
#include "ccsf/explain.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/migration.hpp"
#include "ccsf/placement.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/replica.hpp"
#include "ccsf/snapshot.hpp"
#include "ccsf/state.hpp"

namespace ccsf {

inline constexpr std::size_t kDefaultMaximumRecords = 250000;
inline constexpr std::size_t kDefaultMaximumPayloadBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kDefaultJournalRecordsBeforeSnapshot = 4096;
inline constexpr std::size_t kMaxMetadataStringBytes = 4096;

/// Construction parameters for a Fabric runtime.
struct FabricConfig {
  CoordinatorId coordinator_id;

  /// The cluster this runtime physically runs in. Used for locality ranking.
  ClusterId home_cluster;

  /// Directory holding the durable control plane. Empty selects an in-memory
  /// runtime, in which case durability obligations are reported as such.
  std::string persistence_directory;

  /// When true, a mutation that cannot cross the persistence boundary fails
  /// instead of being applied in memory only.
  bool require_durable_commits{false};

  /// Journal records accumulated before a snapshot checkpoint is written.
  std::size_t journal_records_before_snapshot{kDefaultJournalRecordsBeforeSnapshot};

  /// Upper bound on the number of governed records held in memory.
  std::size_t maximum_records{kDefaultMaximumRecords};

  /// Upper bound on a single state payload moved by a reference transfer.
  std::size_t maximum_payload_bytes{kDefaultMaximumPayloadBytes};

  /// Starting logical tick and coordinator epoch.
  Tick logical_start{0};
  CoordinatorEpoch epoch_start;

  /// Default policies used when a caller does not supply one.
  PlacementPolicyRecord default_placement_policy;
  ReplicationPolicyRecord default_replication_policy;
};

/// Summary of the current runtime.
struct FabricStatus {
  CoordinatorId coordinator_id;
  CoordinatorEpoch coordinator_epoch;
  Tick logical_time{0};
  std::uint64_t durable_sequence{0};
  std::uint64_t journal_records{0};
  std::uint64_t replay_generation{0};
  bool persistence_enabled{false};
  bool persistence_healthy{false};
  bool recovered_from_durable_state{false};
  bool journal_tail_discarded{false};
  std::string persistence_detail;
  std::size_t state_generations{0};
  std::size_t clusters{0};
  std::size_t replicas{0};
  std::size_t replica_sets{0};
  std::size_t operations{0};
  FabricCounters counters;
};

/// Answer to "may this state generation be reused in this cluster right now".
struct ReuseEvaluation {
  StateId state_id;
  StateGeneration state_generation;
  ClusterId cluster_id;
  DecisionKind decision{DecisionKind::REJECT};
  ReplicaId replica;
  ReplicaGeneration replica_generation;
  ReplicaAuthorityAssessment assessment;
  ErrorCode code{ErrorCode::NO_AUTHORITATIVE_REPLICA};
  std::string reason;
  std::vector<std::string> findings;
};

/// The Cross-Cluster State Fabric runtime.
///
/// Thread safety: every public method is safe to call concurrently. Queries may
/// run in parallel with each other; mutations are serialized. No internal lock
/// is held across persistence I/O, network I/O, backend calls or callbacks.
class Fabric {
 public:
  ~Fabric();
  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;
  Fabric(Fabric&&) = delete;
  Fabric& operator=(Fabric&&) = delete;

  /// Opens a runtime, recovering the durable control plane when configured.
  static Result<std::unique_ptr<Fabric>> open(const FabricConfig& config);

  // ---- Identity -----------------------------------------------------------
  [[nodiscard]] CoordinatorId coordinator_id() const;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const;
  [[nodiscard]] Tick logical_time() const;

  /// Advances the coordinator logical clock. Decisions are made against this
  /// clock, never against wall-clock time.
  Result<Tick> advance_time(Tick delta);

  /// Advances the coordinator epoch after a restart, fencing all older epochs.
  Result<CoordinatorEpoch> advance_epoch();

  // ---- State --------------------------------------------------------------
  /// Registers a state generation in STAGED lifecycle.
  Result<StateGenerationRecord> register_state(const StateGenerationSpec& spec);

  /// Publishes (registers or updates) a state generation. Never makes it
  /// CURRENT; that requires commit_state_generation.
  Result<StateGenerationRecord> publish_state_generation(const StateGenerationSpec& spec);

  /// Authoritative commit: makes the generation CURRENT and deterministically
  /// supersedes the prior current generation of the same lineage.
  Result<StateGenerationRecord> commit_state_generation(StateId state_id,
                                                        StateGeneration generation,
                                                        CommitId commit_id);

  Result<StateGenerationRecord> query_state(StateId state_id,
                                            StateGeneration generation) const;
  Result<StateGenerationRecord> current_state(StateId state_id) const;
  Result<LineageView> query_lineage(StateId state_id) const;
  Result<std::vector<StateGenerationRecord>> list_states() const;

  // ---- Clusters -----------------------------------------------------------
  Result<ClusterRecord> register_cluster(const ClusterRegistration& registration);
  Result<ClusterRecord> publish_cluster_evidence(const ClusterEvidenceUpdate& update);
  Result<ClusterRecord> mark_cluster_unreachable(ClusterId cluster_id,
                                                 ClusterIncarnationId incarnation,
                                                 std::string reason);
  Result<ClusterRecord> retire_cluster(ClusterId cluster_id, ClusterIncarnationId incarnation);
  Result<ClusterRecord> query_cluster(ClusterId cluster_id) const;
  Result<std::vector<ClusterRecord>> list_clusters() const;

  // ---- Compatibility and policy ------------------------------------------
  Result<CompatibilityRecord> publish_compatibility(const CompatibilityRecord& record);
  Result<CompatibilityRecord> query_compatibility(CompatibilityId compatibility_id) const;
  Result<PlacementPolicyRecord> publish_placement_policy(const PlacementPolicyRecord& policy);
  Result<ReplicationPolicyRecord> publish_replication_policy(const ReplicationPolicyRecord& policy);
  Result<PlacementPolicyRecord> placement_policy(PlacementPolicyId policy_id) const;
  Result<ReplicationPolicyRecord> replication_policy(ReplicationPolicyId policy_id) const;

  // ---- Replicas -----------------------------------------------------------
  Result<ReplicaRecord> register_replica(const ReplicaRegistration& registration);
  Result<ReplicaRecord> record_transfer_progress(const TransferProgress& progress);
  Result<ReplicaRecord> record_transfer_completion(const TransferCompletion& completion);
  Result<ReplicaRecord> verify_replica(const ReplicaVerification& verification);
  Result<ReplicaRecord> commit_replica(const ReplicaCommit& commit);
  Result<ReplicaRecord> retire_replica(const ReplicaTransition& transition);
  Result<ReplicaRecord> quarantine_replica(const ReplicaTransition& transition);
  Result<ReplicaRecord> revalidate_replica(const ReplicaTransition& transition);
  Result<ReplicaRecord> query_replica(ReplicaId replica_id) const;
  Result<std::vector<ReplicaRecord>> list_replicas(StateId state_id,
                                                   StateGeneration generation) const;

  // ---- Replica sets -------------------------------------------------------
  Result<ReplicaSetRecord> query_replica_set(StateId state_id,
                                             StateGeneration generation) const;
  Result<ReplicaSetReconciliation> reconcile_replica_set(StateId state_id,
                                                         StateGeneration generation) const;
  Result<std::vector<ReplicaSetRecord>> list_replica_sets() const;

  // ---- Decisions ----------------------------------------------------------
  Result<ReuseEvaluation> evaluate_reuse(StateId state_id, StateGeneration generation,
                                         ClusterId cluster_id) const;
  Result<PlacementEvaluation> evaluate_placement(const PlacementRequest& request) const;
  Result<PlacementEvaluation> plan_replication(const ReplicationRequest& request) const;
  Result<PlacementEvaluation> plan_migration(const MigrationRequest& request) const;

  Result<TransferAuthorization> authorize_replication(const ReplicationRequest& request);
  Result<TransferAuthorization> authorize_migration(const MigrationRequest& request);
  Result<OperationRecord> commit_operation(const OperationCommit& commit);
  Result<OperationRecord> commit_migration(const OperationCommit& commit);
  Result<OperationRecord> cancel_operation(const OperationCancellation& cancellation);
  Result<OperationRecord> query_operation(OperationId operation_id) const;
  Result<std::vector<OperationRecord>> list_operations() const;
  Result<TransferAuthorization> query_authorization(OperationId operation_id) const;
  Result<OwnershipRecord> transfer_ownership(const OwnershipTransferRequest& request);

  // ---- Explanation, snapshot, persistence --------------------------------
  Result<Explanation> explain_operation_record(OperationId operation_id) const;
  Result<Explanation> explain_reuse(StateId state_id, StateGeneration generation,
                                    ClusterId cluster_id) const;
  Result<Explanation> explain_replica_set_state(StateId state_id,
                                                StateGeneration generation) const;
  Result<Explanation> explain_placement(const PlacementRequest& request) const;
  Result<Explanation> explain_replication_plan(const ReplicationRequest& request) const;
  Result<Explanation> explain_migration_plan(const MigrationRequest& request) const;

  [[nodiscard]] FabricSnapshot snapshot() const;
  [[nodiscard]] FabricStatus status() const;

  /// Forces a durable checkpoint. Returns an error when persistence is not
  /// configured.
  Status persist();

  /// Signals that no further work is accepted. Idempotent.
  Status shutdown();

 private:
  explicit Fabric(FabricConfig config);
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ccsf

#endif  // CCSF_FABRIC_HPP
