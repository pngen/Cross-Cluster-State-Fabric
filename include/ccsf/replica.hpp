// Cross-Cluster State Fabric - replica identity, evidence and lifecycle.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A replica is modeled along independent dimensions (integrity, compatibility,
// durability, availability, authority, lifecycle). Presence is one fact among
// many and never implies authority.

#ifndef CCSF_REPLICA_HPP
#define CCSF_REPLICA_HPP

#include <cstdint>
#include <string>

#include "ccsf/clock.hpp"
#include "ccsf/compatibility.hpp"
#include "ccsf/digest.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/replica_set.hpp"
#include "ccsf/state.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

inline constexpr std::size_t kMaxFailureReasonLength = 256;

/// Where a replica physically lives, with the incarnation that hosted it.
struct ReplicaLocation {
  ClusterId cluster_id;
  ClusterIncarnationId cluster_incarnation;
  RegionId region;
  SiteId site;
  FailureDomainId failure_domain;
  std::string descriptor;
};

/// Verification evidence attached to a replica.
struct ReplicaEvidence {
  IntegrityGeneration integrity_generation;
  CompatibilityGeneration compatibility_generation;
  EvidenceGeneration evidence_generation;
  CapabilityGeneration capability_generation;
  HealthGeneration health_generation;
  VerificationId verification_id;
  Tick verified_at{0};
  CoordinatorEpoch coordinator_epoch;
  bool revalidation_required{false};
  bool integrity_evidence_present{false};
  bool compatibility_evidence_present{false};
};

/// The durable record of one replica.
struct ReplicaRecord {
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  StateId state_id;
  StateGeneration state_generation;
  ReplicaSetId replica_set_id;
  ReplicaSetGeneration replica_set_generation;

  ReplicaLocation location;
  ReplicaLifecycle lifecycle{ReplicaLifecycle::PLANNED};
  ReplicaIntegrityState integrity{ReplicaIntegrityState::UNKNOWN};
  ReplicaCompatibilityState compatibility{ReplicaCompatibilityState::UNKNOWN};
  ReplicaDurabilityState durability{ReplicaDurabilityState::UNKNOWN};
  ReplicaAvailabilityState availability{ReplicaAvailabilityState::UNKNOWN};
  ReplicaAuthorityState authority{ReplicaAuthorityState::NONE};

  Digest content_digest;
  Digest measured_digest;
  IntegrityAlgorithm digest_algorithm{IntegrityAlgorithm::SHA256};
  CompatibilityId compatibility_id;
  CompatibilityGeneration compatibility_generation;
  CompatibilityDescriptor compatibility_descriptor;

  SizeValue logical_size;
  SizeValue transferred_bytes;

  StateProvenance provenance;
  ReplicaEvidence evidence;

  WorkerId worker_id;
  WorkerBootId worker_boot_id;

  CoordinatorEpoch created_epoch;
  CoordinatorEpoch last_epoch;
  Tick created_at{0};
  Tick updated_at{0};

  TransferId transfer_id;
  TransferGeneration transfer_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
  CommitId commit_id;

  std::uint32_t attempt_count{0};
  std::string last_error;
  bool commit_recorded{false};
  bool counts_toward_factor{false};

  [[nodiscard]] bool is_terminal() const noexcept {
    return lifecycle == ReplicaLifecycle::RETIRED || lifecycle == ReplicaLifecycle::ABANDONED;
  }
};

/// Input to register_replica.
struct ReplicaRegistration {
  ReplicaId replica_id;
  StateId state_id;
  StateGeneration state_generation;
  ReplicaSetId replica_set_id;
  ReplicaSetGeneration replica_set_generation;
  ReplicaGeneration replica_generation;
  ReplicaLifecycle lifecycle{ReplicaLifecycle::PLANNED};
  DurabilityClass durability{DurabilityClass::STANDARD};
  SizeValue logical_size;
  Digest content_digest;
  IntegrityAlgorithm digest_algorithm{IntegrityAlgorithm::SHA256};
  CompatibilityId compatibility_id;
  CompatibilityGeneration compatibility_generation;
  CompatibilityDescriptor compatibility_descriptor;
  ReplicaLocation location;
  WorkerId worker_id;
  WorkerBootId worker_boot_id;
  CoordinatorEpoch expected_coordinator_epoch;
  StateProvenance provenance;
  TransferId transfer_id;
  TransferGeneration transfer_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
};

/// Input to record_transfer_progress.
struct TransferProgress {
  TransferId transfer_id;
  TransferGeneration transfer_generation;
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
  ClusterId cluster_id;
  ClusterIncarnationId cluster_incarnation;
  WorkerBootId worker_boot_id;
  CoordinatorEpoch expected_coordinator_epoch;
  std::uint64_t bytes_transferred{0};
  bool failed{false};
  std::string failure_reason;
};

/// Input to record_transfer_completion.
struct TransferCompletion {
  TransferId transfer_id;
  TransferGeneration transfer_generation;
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
  ClusterId cluster_id;
  ClusterIncarnationId cluster_incarnation;
  WorkerBootId worker_boot_id;
  CoordinatorEpoch expected_coordinator_epoch;
  std::uint64_t bytes_transferred{0};
  Digest measured_digest;
  IntegrityAlgorithm digest_algorithm{IntegrityAlgorithm::SHA256};
  bool transfer_complete{true};
  std::string detail;
};

/// Input to verify_replica.
struct ReplicaVerification {
  VerificationId verification_id;
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
  ClusterId cluster_id;
  ClusterIncarnationId cluster_incarnation;
  WorkerBootId worker_boot_id;
  CoordinatorEpoch expected_coordinator_epoch;
  bool integrity_verified{false};
  Digest measured_digest;
  IntegrityAlgorithm digest_algorithm{IntegrityAlgorithm::SHA256};
  bool compatibility_verified{false};
  CompatibilityId compatibility_id;
  CompatibilityGeneration compatibility_generation;
  CompatibilityDescriptor compatibility_descriptor;
  DurabilityClass established_durability{DurabilityClass::STANDARD};
  std::uint64_t verified_bytes{0};
  std::string detail;
};

/// Input to commit_replica.
struct ReplicaCommit {
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  CommitId commit_id;
  OperationId operation_id;
  OperationGeneration operation_generation;
  CoordinatorEpoch expected_coordinator_epoch;
  ReplicaSetGeneration expected_replica_set_generation;
  bool make_authoritative_owner{false};
  bool add_to_replica_set{true};
  bool require_integrity_verified{true};
  bool require_compatibility_verified{true};
};

/// Input to retire_replica / quarantine_replica / revalidate_replica.
struct ReplicaTransition {
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
  CoordinatorEpoch expected_coordinator_epoch;
  ClusterIncarnationId cluster_incarnation;
  WorkerBootId worker_boot_id;
  std::string reason;
};

/// True when the replica is counted toward the authoritative replication factor.
///
/// A replica counts only when it is committed, member of the current replica set
/// generation, and carries verified integrity and compatibility. Staging,
/// transferring, verifying, stale, corrupt and revalidation-pending replicas
/// never count.
///
/// Transient availability is deliberately not part of this predicate: a
/// committed replica that is momentarily unreachable is still a committed
/// member of the replica set. Explicit fencing events (cluster loss,
/// reincarnation, quarantine, retirement) move the replica out of the counting
/// lifecycles instead.
bool replica_counts_toward_replication_factor(const ReplicaRecord& record) noexcept;

/// True when the replica is durable enough to keep historical metadata alive
/// after its hosting cluster disappears.
bool replica_survives_cluster_loss(const ReplicaRecord& record) noexcept;

}  // namespace ccsf

#endif  // CCSF_REPLICA_HPP
