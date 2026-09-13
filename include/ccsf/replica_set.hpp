// Cross-Cluster State Fabric - replica sets as first-class objects.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_REPLICA_SET_HPP
#define CCSF_REPLICA_SET_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/clock.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

/// One member of a replica set generation.
struct ReplicaSetMember {
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  ClusterId cluster_id;
  ClusterIncarnationId cluster_incarnation;
  RegionId region;
  FailureDomainId failure_domain;
  bool authoritative{false};
  ReplicaLifecycle lifecycle{ReplicaLifecycle::PLANNED};
  ReplicaIntegrityState integrity{ReplicaIntegrityState::UNKNOWN};
  ReplicaCompatibilityState compatibility{ReplicaCompatibilityState::UNKNOWN};
  ReplicaAuthorityState authority{ReplicaAuthorityState::NONE};
  CommitId commit_id;
  Tick joined_at{0};

  friend bool operator==(const ReplicaSetMember&, const ReplicaSetMember&) = default;
};

/// A replica set generation: the authoritative membership for one state
/// generation under one set of replication requirements.
struct ReplicaSetRecord {
  ReplicaSetId replica_set_id;
  ReplicaSetGeneration generation;
  StateId state_id;
  StateGeneration state_generation;
  ReplicaSetState state{ReplicaSetState::NO_AUTHORITATIVE_REPLICA};
  ReplicationRequirements requirements;

  std::vector<ReplicaSetMember> authoritative_members;
  std::vector<ReplicaSetMember> degraded_members;

  std::uint32_t authoritative_count{0};
  std::uint32_t degraded_count{0};
  std::uint32_t distinct_clusters{0};
  std::uint32_t distinct_regions{0};
  std::uint32_t distinct_failure_domains{0};

  PlacementId placement_id;
  PlacementGeneration placement_generation;
  CoordinatorEpoch epoch;
  CommitId last_commit;
  Tick created_at{0};
  Tick updated_at{0};
  bool revalidation_required{false};

  [[nodiscard]] bool meets_requirements() const noexcept;
  [[nodiscard]] bool has_authoritative_owner() const noexcept;
};

/// Result of reconciling a replica set against its current membership.
struct ReplicaSetReconciliation {
  ReplicaSetRecord replica_set;
  bool under_replicated{false};
  bool over_replicated{false};
  std::uint32_t authoritative_shortfall{0};
  std::uint32_t clusters_missing{0};
  std::uint32_t regions_missing{0};
  std::vector<ReplicaId> excluded_replicas;
  std::vector<std::string> findings;
};

/// Recomputes the derived fields of a replica set record from its membership.
/// The record passed in is authoritative for identity and requirements only.
ReplicaSetRecord recompute_replica_set(ReplicaSetRecord record);

/// Deterministic, human readable explanation of why a replica set is in its
/// current state. Ordering is stable: findings are appended in a fixed order.
std::vector<std::string> explain_replica_set(const ReplicaSetRecord& record);

}  // namespace ccsf

#endif  // CCSF_REPLICA_SET_HPP
