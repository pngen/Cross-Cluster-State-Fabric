// Cross-Cluster State Fabric - cluster identity, incarnation and evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A cluster identity is durable. A cluster incarnation is not: every time a
// cluster agent process restarts, or a cluster leaves and re-registers, a fresh
// ClusterIncarnationId is minted. Durable bytes may survive an incarnation
// change; live authority never does.

#ifndef CCSF_CLUSTER_HPP
#define CCSF_CLUSTER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ccsf/clock.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

inline constexpr std::size_t kMaxClusterTagCount = 128;
inline constexpr std::size_t kMaxClusterTagLength = 128;
inline constexpr std::size_t kMaxLocationDescriptorLength = 256;

/// Capability evidence published by a cluster agent.
struct ClusterCapabilities {
  std::vector<std::string> tags;
  std::vector<std::string> storage_classes;
  std::vector<std::string> accelerator_architectures;
  bool encryption_at_rest{false};
  bool attestation{false};

  void canonicalize();
  [[nodiscard]] bool has_tag(std::string_view tag) const;
  [[nodiscard]] bool has_storage_class(std::string_view storage_class) const;
  [[nodiscard]] bool has_accelerator(std::string_view architecture) const;
};

/// Capacity and pressure evidence published by a cluster agent.
struct ClusterCapacity {
  std::uint64_t total_logical_bytes{0};
  std::uint64_t used_logical_bytes{0};
  std::uint64_t inbound_bytes_per_tick{0};
  std::uint32_t free_transfer_slots{0};
  std::uint32_t utilization_per_mille{0};

  [[nodiscard]] std::uint64_t free_logical_bytes() const noexcept;
  [[nodiscard]] bool has_room_for(std::uint64_t bytes) const noexcept;
};

/// Where a cluster physically lives. The descriptor is a bounded human label;
/// all governed decisions use the typed identities.
struct ClusterLocation {
  RegionId region;
  SiteId site;
  FailureDomainId failure_domain;
  std::string descriptor;
};

/// Evidence generations published by a cluster. Coordinator restart or cluster
/// reincarnation marks this evidence as requiring revalidation; it is never
/// silently treated as current.
struct ClusterEvidence {
  CapabilityGeneration capability_generation;
  HealthGeneration health_generation;
  TopologyGeneration topology_generation;
  CostGeneration cost_generation;
  EvidenceGeneration evidence_generation;
  Tick published_at{0};
  CoordinatorEpoch coordinator_epoch;
  bool revalidation_required{false};
  bool present{false};
};

/// Input to register_cluster / reinstate_cluster.
struct ClusterRegistration {
  ClusterId cluster_id;
  RegionId region;
  SiteId site;
  FailureDomainId failure_domain;
  std::string location_descriptor;
  WorkerId worker_id;
  WorkerBootId worker_boot_id;
  ClusterCapabilities capabilities;
  ClusterCapacity capacity;
  ClusterHealthState health{ClusterHealthState::HEALTHY};

  /// When set, the registration is rejected unless it matches the coordinator
  /// epoch currently in force. Zero means "do not check".
  CoordinatorEpoch expected_coordinator_epoch;

  /// When set, a cluster re-registering with the same incarnation is treated as
  /// a duplicate registration instead of a reincarnation.
  bool force_reincarnation{false};
};

/// Input to publish_cluster_evidence.
struct ClusterEvidenceUpdate {
  ClusterId cluster_id;
  ClusterIncarnationId incarnation;
  ClusterHealthState health{ClusterHealthState::HEALTHY};
  ClusterCapabilities capabilities;
  ClusterCapacity capacity;
  bool has_capabilities{false};
  bool has_capacity{false};
  CoordinatorEpoch expected_coordinator_epoch;
  WorkerBootId worker_boot_id;
};

/// The durable record of one cluster incarnation.
struct ClusterRecord {
  ClusterId cluster_id;
  ClusterIncarnationId incarnation;
  ClusterLifecycle lifecycle{ClusterLifecycle::REGISTERED};
  ClusterHealthState health{ClusterHealthState::UNKNOWN};
  ClusterLocation location;
  WorkerId worker_id;
  WorkerBootId worker_boot_id;
  ClusterCapabilities capabilities;
  ClusterCapacity capacity;
  ClusterEvidence evidence;
  CoordinatorEpoch registered_epoch;
  CoordinatorEpoch last_evidence_epoch;
  Tick registered_at{0};
  Tick updated_at{0};
  Tick incarnation_started_at{0};

  /// False once the incarnation is fenced; durable replica metadata survives.
  bool live_authority{false};
  std::uint32_t hosted_replica_count{0};
  std::uint32_t historical_replica_count{0};
  std::uint32_t reincarnation_count{0};

  [[nodiscard]] bool accepts_traffic(ClusterIncarnationId candidate) const noexcept {
    return live_authority && candidate == incarnation;
  }
};

/// True when the cluster may host new authoritative replicas under the given
/// health floor and evidence freshness horizon.
bool cluster_admits_new_authority(const ClusterRecord& record, ClusterHealthState required_health,
                                  std::uint64_t maximum_evidence_staleness_ticks,
                                  Tick now) noexcept;

/// True when the cluster holds no revalidation obligation.
bool cluster_evidence_is_current(const ClusterRecord& record) noexcept;


}  // namespace ccsf

#endif  // CCSF_CLUSTER_HPP
