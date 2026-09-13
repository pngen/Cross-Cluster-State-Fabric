// Cross-Cluster State Fabric - managed state identity, generations and lineage.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_STATE_HPP
#define CCSF_STATE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/clock.hpp"
#include "ccsf/compatibility.hpp"
#include "ccsf/cost.hpp"
#include "ccsf/digest.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

inline constexpr std::size_t kMaxFreeTextLength = 256;
inline constexpr std::size_t kMaxLineageGenerations = 100000;

/// Where a state generation came from. Provenance is append-only: retiring or
/// migrating a replica never rewrites the provenance that produced it.
struct StateProvenance {
  std::string producer;
  ClusterId origin_cluster;
  ClusterIncarnationId origin_incarnation;
  StateGeneration source_generation;
  ReplicaId source_replica;
  OperationId operation;
  WorkerBootId worker_boot;
  CoordinatorEpoch coordinator_epoch;
  CommitId commit;
  std::string verification;
  Tick recorded_at{0};
  bool revalidated{false};
  Tick revalidated_at{0};
};

/// How this state generation can be rebuilt if transfer is not chosen.
struct ReconstructionIdentity {
  ReconstructionId reconstruction_id;
  std::string method;
  SizeValue output_size;
  CostValue estimated_cost;
  bool deterministic{false};

  [[nodiscard]] bool available() const noexcept { return reconstruction_id.valid(); }
};

/// Input describing a state generation to register or publish.
struct StateGenerationSpec {
  StateId state_id;
  StateGeneration generation;
  LineageId lineage;

  /// Explicit legal branch. When branch is true, branches_from must be set and
  /// the lineage is marked as branched; accidental divergence cannot be
  /// expressed as a branch.
  bool branch{false};
  StateGeneration branches_from;

  StateClass state_class{StateClass::OPAQUE_STATE};
  ConsistencyMode consistency{ConsistencyMode::IMMUTABLE};
  SizeValue logical_size;
  SizeValue physical_size;
  Digest content_digest;
  IntegrityAlgorithm digest_algorithm{IntegrityAlgorithm::SHA256};
  CompatibilityId compatibility_id;
  CompatibilityGeneration compatibility_generation;
  CompatibilityDescriptor compatibility_required;
  ReconstructionIdentity reconstruction;
  DurabilityClass durability{DurabilityClass::STANDARD};
  ReuseClass reuse_class{ReuseClass::SHARED_READ};
  PlacementConstraints placement_constraints;
  ReplicationRequirements replication_requirements;
  StateProvenance provenance;
  ReuseValue expected_reuse;
  CostValue transfer_cost;
  CostValue reconstruction_cost;
};

/// The governed record of exactly one state generation.
struct StateGenerationRecord {
  StateId state_id;
  StateGeneration generation;
  LineageId lineage;
  bool branch{false};
  StateGeneration branches_from;
  StateClass state_class{StateClass::OPAQUE_STATE};
  ConsistencyMode consistency{ConsistencyMode::IMMUTABLE};
  StateGenerationLifecycle lifecycle{StateGenerationLifecycle::STAGED};
  SizeValue logical_size;
  SizeValue physical_size;
  Digest content_digest;
  IntegrityAlgorithm digest_algorithm{IntegrityAlgorithm::SHA256};
  CompatibilityId compatibility_id;
  CompatibilityGeneration compatibility_generation;
  CompatibilityDescriptor compatibility_required;
  ReconstructionIdentity reconstruction;
  DurabilityClass durability{DurabilityClass::STANDARD};
  ReuseClass reuse_class{ReuseClass::SHARED_READ};
  PlacementConstraints placement_constraints;
  ReplicationRequirements replication_requirements;
  StateProvenance provenance;
  ReuseValue expected_reuse;
  CostValue transfer_cost;
  CostValue reconstruction_cost;
  ReplicaSetId replica_set_id;
  ReplicaSetGeneration replica_set_generation;
  CommitId commit;
  CoordinatorEpoch committed_epoch;
  Tick registered_at{0};
  Tick committed_at{0};
  bool revalidation_required{false};

  [[nodiscard]] bool is_current() const noexcept {
    return lifecycle == StateGenerationLifecycle::CURRENT;
  }
  [[nodiscard]] bool is_reusable_generation() const noexcept {
    return lifecycle == StateGenerationLifecycle::CURRENT ||
           lifecycle == StateGenerationLifecycle::HISTORICAL;
  }
};

/// The set of generations recorded for one lineage, in ascending order.
struct LineageView {
  StateId state_id;
  LineageId lineage;
  std::vector<StateGenerationRecord> generations;
  StateGeneration current_generation;
  bool branched{false};
  std::vector<StateGeneration> current_branches;

  [[nodiscard]] const StateGenerationRecord* find(StateGeneration generation) const;
};

/// Encodes a state generation fingerprint from the content digest plus the
/// compatibility identity; two generations with different content or different
/// compatibility must not collide.
std::string state_generation_label(const StateId& state_id, StateGeneration generation);

}  // namespace ccsf

#endif  // CCSF_STATE_HPP
