// Cross-Cluster State Fabric - strongly typed domain identities.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Semantic identity matters in this runtime: a ReplicaId is not a StateId and a
// ReplicaGeneration is not a StateGeneration. Every domain identity below is a
// distinct C++ type; there is no implicit conversion between them and no
// implicit conversion from a raw integer.

#ifndef CCSF_IDS_HPP
#define CCSF_IDS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "ccsf/result.hpp"

namespace ccsf {

namespace detail {
std::string id_to_string(std::uint64_t value);
Result<std::uint64_t> id_from_string(std::string_view text);
}  // namespace detail

/// A strongly typed 64-bit domain identity.
///
/// Zero means "absent/unset" for every identity kind; all minted identities are
/// strictly positive. Identities are minted from a coordinator-owned monotonic
/// counter or from an explicit caller-supplied value (persistence replay,
/// protocol decode), never from address-space values.
template <class Tag>
class StrongId {
 public:
  using rep = std::uint64_t;

  constexpr StrongId() noexcept = default;
  explicit constexpr StrongId(rep value) noexcept : value_(value) {}

  static constexpr StrongId from_value(rep value) noexcept { return StrongId(value); }

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  [[nodiscard]] std::string str() const { return detail::id_to_string(value_); }

  /// The next identity in the same domain. Never valid for a zero identity.
  [[nodiscard]] constexpr StrongId next() const noexcept { return StrongId(value_ + 1U); }

  static Result<StrongId> parse(std::string_view text) {
    auto parsed = detail::id_from_string(text);
    if (!parsed) {
      return parsed.error();
    }
    return StrongId(parsed.value());
  }

  friend constexpr bool operator==(StrongId a, StrongId b) noexcept {
    return a.value_ == b.value_;
  }
  friend constexpr bool operator!=(StrongId a, StrongId b) noexcept {
    return a.value_ != b.value_;
  }
  friend constexpr bool operator<(StrongId a, StrongId b) noexcept {
    return a.value_ < b.value_;
  }
  friend constexpr bool operator>(StrongId a, StrongId b) noexcept {
    return a.value_ > b.value_;
  }
  friend constexpr bool operator<=(StrongId a, StrongId b) noexcept {
    return a.value_ <= b.value_;
  }
  friend constexpr bool operator>=(StrongId a, StrongId b) noexcept {
    return a.value_ >= b.value_;
  }

 private:
  rep value_{0};
};

// ---------------------------------------------------------------------------
// Identity tags. Each tag names exactly one semantic domain.
// ---------------------------------------------------------------------------
struct CoordinatorIdTag {};
struct CoordinatorEpochTag {};
struct WorkerIdTag {};
struct WorkerBootIdTag {};
struct ClusterIdTag {};
struct ClusterIncarnationIdTag {};
struct RegionIdTag {};
struct SiteIdTag {};
struct StateIdTag {};
struct StateGenerationTag {};
struct LineageIdTag {};
struct ReplicaIdTag {};
struct ReplicaGenerationTag {};
struct ReplicaSetIdTag {};
struct ReplicaSetGenerationTag {};
struct PlacementIdTag {};
struct PlacementGenerationTag {};
struct PlacementPolicyIdTag {};
struct PlacementPolicyGenerationTag {};
struct ReplicationPolicyIdTag {};
struct ReplicationPolicyGenerationTag {};
struct MigrationIdTag {};
struct MigrationGenerationTag {};
struct TransferIdTag {};
struct TransferGenerationTag {};
struct ReservationIdTag {};
struct ReservationGenerationTag {};
struct CompatibilityIdTag {};
struct CompatibilityGenerationTag {};
struct IntegrityRecordIdTag {};
struct IntegrityGenerationTag {};
struct EvidenceGenerationTag {};
struct TopologyGenerationTag {};
struct CapabilityGenerationTag {};
struct CostGenerationTag {};
struct HealthGenerationTag {};
struct CommitIdTag {};
struct AttemptIdTag {};
struct OperationIdTag {};
struct OperationGenerationTag {};
struct VerificationIdTag {};
struct SnapshotIdTag {};
struct FailureDomainIdTag {};
struct ReconstructionIdTag {};
struct PolicyFingerprintTag {};

using CoordinatorId = StrongId<CoordinatorIdTag>;
using CoordinatorEpoch = StrongId<CoordinatorEpochTag>;
using WorkerId = StrongId<WorkerIdTag>;
using WorkerBootId = StrongId<WorkerBootIdTag>;
using ClusterId = StrongId<ClusterIdTag>;
using ClusterIncarnationId = StrongId<ClusterIncarnationIdTag>;
using RegionId = StrongId<RegionIdTag>;
using SiteId = StrongId<SiteIdTag>;
using StateId = StrongId<StateIdTag>;
using StateGeneration = StrongId<StateGenerationTag>;
using LineageId = StrongId<LineageIdTag>;
using ReplicaId = StrongId<ReplicaIdTag>;
using ReplicaGeneration = StrongId<ReplicaGenerationTag>;
using ReplicaSetId = StrongId<ReplicaSetIdTag>;
using ReplicaSetGeneration = StrongId<ReplicaSetGenerationTag>;
using PlacementId = StrongId<PlacementIdTag>;
using PlacementGeneration = StrongId<PlacementGenerationTag>;
using PlacementPolicyId = StrongId<PlacementPolicyIdTag>;
using PlacementPolicyGeneration = StrongId<PlacementPolicyGenerationTag>;
using ReplicationPolicyId = StrongId<ReplicationPolicyIdTag>;
using ReplicationPolicyGeneration = StrongId<ReplicationPolicyGenerationTag>;
using MigrationId = StrongId<MigrationIdTag>;
using MigrationGeneration = StrongId<MigrationGenerationTag>;
using TransferId = StrongId<TransferIdTag>;
using TransferGeneration = StrongId<TransferGenerationTag>;
using ReservationId = StrongId<ReservationIdTag>;
using ReservationGeneration = StrongId<ReservationGenerationTag>;
using CompatibilityId = StrongId<CompatibilityIdTag>;
using CompatibilityGeneration = StrongId<CompatibilityGenerationTag>;
using IntegrityRecordId = StrongId<IntegrityRecordIdTag>;
using IntegrityGeneration = StrongId<IntegrityGenerationTag>;
using EvidenceGeneration = StrongId<EvidenceGenerationTag>;
using TopologyGeneration = StrongId<TopologyGenerationTag>;
using CapabilityGeneration = StrongId<CapabilityGenerationTag>;
using CostGeneration = StrongId<CostGenerationTag>;
using HealthGeneration = StrongId<HealthGenerationTag>;
using CommitId = StrongId<CommitIdTag>;
using AttemptId = StrongId<AttemptIdTag>;
using OperationId = StrongId<OperationIdTag>;
using OperationGeneration = StrongId<OperationGenerationTag>;
using VerificationId = StrongId<VerificationIdTag>;
using SnapshotId = StrongId<SnapshotIdTag>;
using FailureDomainId = StrongId<FailureDomainIdTag>;
using ReconstructionId = StrongId<ReconstructionIdTag>;
using PolicyFingerprint = StrongId<PolicyFingerprintTag>;

// ---------------------------------------------------------------------------
// A generation/authority pair: identifies a specific revision of a governed
// object together with the authority epoch under which it was minted.
// ---------------------------------------------------------------------------
struct AuthorityStamp {
  CoordinatorEpoch coordinator_epoch;
  ClusterIncarnationId cluster_incarnation;
  WorkerBootId worker_boot;
  OperationGeneration operation_generation;

  friend bool operator==(const AuthorityStamp&, const AuthorityStamp&) = default;
};

}  // namespace ccsf

namespace std {
template <class Tag>
struct hash<ccsf::StrongId<Tag>> {
  size_t operator()(const ccsf::StrongId<Tag>& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};
}  // namespace std

#endif  // CCSF_IDS_HPP
