// Cross-Cluster State Fabric - immutable read snapshots and accounting.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_SNAPSHOT_HPP
#define CCSF_SNAPSHOT_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/authority.hpp"
#include "ccsf/cluster.hpp"
#include "ccsf/compatibility.hpp"
#include "ccsf/migration.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/replica.hpp"
#include "ccsf/replica_set.hpp"
#include "ccsf/state.hpp"

namespace ccsf {

/// Runtime accounting. Every completed operation must return these counters to
/// their baseline; the counters are the evidence for resource-leak tests.
struct FabricCounters {
  std::uint64_t states_registered{0};
  std::uint64_t generations_registered{0};
  std::uint64_t generations_committed{0};
  std::uint64_t generations_superseded{0};
  std::uint64_t clusters_registered{0};
  std::uint64_t cluster_reincarnations{0};
  std::uint64_t cluster_incarnations_fenced{0};
  std::uint64_t replicas_registered{0};
  std::uint64_t replicas_committed{0};
  std::uint64_t replicas_retired{0};
  std::uint64_t replicas_quarantined{0};
  std::uint64_t replicas_abandoned{0};
  std::uint64_t replicas_fenced{0};
  std::uint64_t operations_planned{0};
  std::uint64_t operations_reserved{0};
  std::uint64_t operations_committed{0};
  std::uint64_t operations_failed{0};
  std::uint64_t operations_cancelled{0};
  std::uint64_t operations_abandoned{0};
  std::uint64_t operations_recovered{0};
  std::uint64_t transfers_started{0};
  std::uint64_t transfers_completed{0};
  std::uint64_t bytes_transferred{0};
  std::uint64_t integrity_verifications{0};
  std::uint64_t integrity_failures{0};
  std::uint64_t compatibility_rejections{0};
  std::uint64_t stale_rejections{0};
  std::uint64_t conflicts_rejected{0};
  std::uint64_t durable_commits{0};
  std::uint64_t journal_records_applied{0};
  std::uint64_t snapshots_written{0};
  std::uint64_t ownership_transfers{0};
  std::uint64_t active_operations{0};
  std::uint64_t active_transfers{0};
  std::uint64_t reserved_bytes{0};
  std::uint64_t reserved_transfer_slots{0};
};

/// An immutable point-in-time view of the durable control plane.
struct FabricSnapshot {
  CoordinatorId coordinator_id;
  CoordinatorEpoch coordinator_epoch;
  Tick logical_time{0};
  std::uint64_t durable_sequence{0};
  std::uint64_t journal_records{0};
  std::uint64_t replay_generation{0};

  std::vector<StateGenerationRecord> states;
  std::vector<ClusterRecord> clusters;
  std::vector<ReplicaRecord> replicas;
  std::vector<ReplicaSetRecord> replica_sets;
  std::vector<OperationRecord> operations;
  std::vector<OwnershipRecord> ownership;
  std::vector<CompatibilityRecord> compatibility_records;
  std::vector<TransferAuthorization> active_authorizations;
  FabricCounters counters;

  [[nodiscard]] const StateGenerationRecord* find_state(StateId state_id,
                                                        StateGeneration generation) const;
  [[nodiscard]] const StateGenerationRecord* current_generation(StateId state_id) const;
  [[nodiscard]] const ClusterRecord* find_cluster(ClusterId cluster_id) const;
  [[nodiscard]] const ReplicaRecord* find_replica(ReplicaId replica_id) const;
  [[nodiscard]] const ReplicaSetRecord* find_replica_set(StateId state_id,
                                                         StateGeneration generation) const;
  [[nodiscard]] const OperationRecord* find_operation(OperationId operation_id) const;
  [[nodiscard]] std::vector<ReplicaRecord> replicas_of(StateId state_id,
                                                       StateGeneration generation) const;
  /// Replicas that currently count toward the authoritative replication factor.
  [[nodiscard]] std::uint32_t authoritative_replica_count(StateId state_id,
                                                          StateGeneration generation) const;
};

}  // namespace ccsf

#endif  // CCSF_SNAPSHOT_HPP
