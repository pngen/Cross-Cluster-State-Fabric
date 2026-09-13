// Cross-Cluster State Fabric - replica authority assessment.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Existence is not authority. This header models every authority question the
// runtime answers separately, so that no caller can collapse them into a single
// boolean "exists".

#ifndef CCSF_AUTHORITY_HPP
#define CCSF_AUTHORITY_HPP

#include <string>

#include "ccsf/cluster.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/replica.hpp"
#include "ccsf/replica_set.hpp"
#include "ccsf/state.hpp"

namespace ccsf {

/// Everything authority assessment is allowed to consult.
struct AuthorityContext {
  const StateGenerationRecord* state{nullptr};
  const ClusterRecord* cluster{nullptr};
  const ReplicaSetRecord* replica_set{nullptr};
  const ReplicationPolicyRecord* policy{nullptr};
  CoordinatorEpoch current_epoch;
  Tick now{0};
};

/// The full authority answer for one replica. Every field is evaluated
/// independently and never inferred from another.
struct ReplicaAuthorityAssessment {
  bool physically_present{false};
  bool transfer_complete{false};
  bool integrity_verified{false};
  bool compatibility_verified{false};
  bool current_for_state_generation{false};
  bool member_of_current_replica_set_generation{false};
  bool cluster_incarnation_current{false};
  bool coordinator_epoch_current{false};
  bool evidence_current{false};
  bool authoritative_for_reuse{false};
  bool authoritative_as_replication_source{false};
  bool authoritative_for_migration_source_retirement{false};
  bool historical_only{false};
  bool fenced{false};

  ErrorCode code{ErrorCode::OK};
  std::string reason;
};

/// Assesses one replica. The assessment is total: it always returns a value,
/// and a false authority field always carries a non-OK code and a reason.
ReplicaAuthorityAssessment assess_replica_authority(const ReplicaRecord& replica,
                                                    const AuthorityContext& context);

/// True when the assessment permits serving reuse of the state generation.
bool assessment_permits_reuse(const ReplicaAuthorityAssessment& assessment) noexcept;

/// One-line deterministic rendering of an assessment.
std::string describe_authority(const ReplicaRecord& replica,
                               const ReplicaAuthorityAssessment& assessment);

/// Single-writer ownership record for mutable state. Exactly one active record
/// may exist per (state id, state generation) at a time.
struct OwnershipRecord {
  StateId state_id;
  StateGeneration state_generation;
  ReplicaId owner_replica;
  ReplicaGeneration owner_replica_generation;
  ClusterId owner_cluster;
  ClusterIncarnationId owner_incarnation;
  CoordinatorEpoch epoch;
  CommitId commit;
  PlacementId placement_id;
  PlacementGeneration placement_generation;
  Tick established_at{0};
  bool active{true};
};

/// True when a state's consistency mode demands single-writer authority.
bool requires_single_authoritative_owner(ConsistencyMode mode) noexcept;

}  // namespace ccsf

#endif  // CCSF_AUTHORITY_HPP
