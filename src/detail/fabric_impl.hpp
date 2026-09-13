// Cross-Cluster State Fabric - runtime implementation internals.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// CONCURRENCY MODEL
// -----------------
// Every public Fabric method is safe to call concurrently.
//
//   * queries take a shared lock on mutex_ and return value copies;
//   * mutations take commit_mutex_ and then mutex_.
//
// The lock order is always commit_mutex_ -> mutex_ and never the reverse; no
// call path acquires the shared state lock and then a write lock, and no call
// path re-enters a public method while holding a lock. A mutation validates and
// computes its effects while holding the state lock, then releases the state
// lock before touching the durable log, then re-acquires it to apply. No lock
// is ever held across file I/O, network I/O, backend calls or callbacks.
//
// Mutation bodies receive a const control plane and express every change as an
// ordered list of durable effects. Nothing mutates runtime state directly, so
// live execution and journal replay are the same function of the same effects.

#ifndef CCSF_DETAIL_FABRIC_IMPL_HPP
#define CCSF_DETAIL_FABRIC_IMPL_HPP

#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "ccsf/fabric.hpp"
#include "detail/control_plane.hpp"

namespace ccsf {

/// Reserved tick window granted to an authorized transfer.
inline constexpr Tick kReservationWindowTicks = 4096;

struct FabricMutation {
  std::vector<detail::Effect> effects;
  Tick tick{0};
  CoordinatorEpoch epoch;

  /// Set by a mutation that produces an adverse but durable outcome which must
  /// still be recorded before the error is reported (for example a recorded
  /// integrity mismatch that quarantines a destination).
  bool commit_before_return{false};
};

struct MutationInputs {
  const detail::ControlPlane& plane;
  const FabricConfig& config;
  FabricMutation& mutation;
};

// --- read helpers over an immutable control plane ---------------------------

const StateGenerationRecord* find_state(const detail::ControlPlane& plane, StateId state_id,
                                        StateGeneration generation);
const StateGenerationRecord* find_current_state(const detail::ControlPlane& plane, StateId state_id);
const ReplicaSetRecord* find_replica_set(const detail::ControlPlane& plane, StateId state_id,
                                         StateGeneration generation);
const ClusterRecord* find_cluster(const detail::ControlPlane& plane, ClusterId cluster_id);
const ReplicaRecord* find_replica(const detail::ControlPlane& plane, ReplicaId replica_id);
const OperationRecord* find_operation(const detail::ControlPlane& plane, OperationId operation_id);
const OwnershipRecord* find_ownership(const detail::ControlPlane& plane, StateId state_id,
                                      StateGeneration generation);
const TransferAuthorization* find_authorization(const detail::ControlPlane& plane,
                                                OperationId operation_id);
const ReplicationPolicyRecord* resolve_replication_policy(const detail::ControlPlane& plane,
                                                          const FabricConfig& config,
                                                          ReplicationPolicyId policy_id);
const PlacementPolicyRecord* resolve_placement_policy(const detail::ControlPlane& plane,
                                                      const FabricConfig& config,
                                                      PlacementPolicyId policy_id);
std::vector<const ReplicaRecord*> replicas_of_state(const detail::ControlPlane& plane,
                                                    StateId state_id,
                                                    StateGeneration generation);

/// Descriptor a cluster publishes for compatibility gating: well-known keys plus
/// every "compat:<key>=<value>" capability tag.
CompatibilityDescriptor cluster_compatibility_descriptor(const ClusterRecord& cluster);

/// Selects the authoritative source replica for a state generation.
/// Deterministic: preferred cluster first, then lowest replica identity.
const ReplicaRecord* select_source_replica(const detail::ControlPlane& plane, StateId state_id,
                                           StateGeneration generation, ClusterId preferred,
                                           const ReplicationPolicyRecord* policy, Tick now);

/// True when the operation journal may be appended to without violating bounds.
bool operation_capacity_available(const detail::ControlPlane& plane, StateId state_id,
                                  std::uint32_t maximum_in_flight);

/// Writes a replica set record and stamps it with the current epoch. The
/// generation is advanced only when the authoritative membership actually
/// changed; staging, transfer progress and verification never fence a pending
/// operation, while commits, retirements, quarantines and fences always do.
detail::Effect bump_replica_set_effect(const detail::ControlPlane& plane, StateId state_id,
                                       StateGeneration generation, Tick tick,
                                       CoordinatorEpoch epoch, bool advance_generation = false);

/// Authorizes a replication or migration transaction: plans deterministically,
/// validates source and destination eligibility, reserves identifiers and
/// destination capacity, and records the operation plus its authorization.
Status authorize_transaction(const MutationInputs& inputs, StateId state_id,
                             StateGeneration generation, ClusterId destination_cluster,
                             ClusterId source_cluster, OperationKind kind, bool destructive_move,
                             ReplicationPolicyId replication_policy_id,
                             PlacementPolicyId placement_policy_id,
                             CoordinatorEpoch expected_epoch, TransferAuthorization* out);

/// Advances the operation that owns a replica transition. Agent reports are the
/// only evidence a transaction has progressed, so the operation's transactional
/// state is derived from them and never advanced by anything else.
void advance_operation(const MutationInputs& inputs, OperationId operation_id,
                       OperationGeneration generation, OperationState next, const char* note);

/// Plans the authoritative commit of one replica: verification gates have
/// already passed, this establishes authority, replica-set membership and (when
/// requested) single-writer ownership. Effects are appended to the mutation.
Status plan_authoritative_commit(const MutationInputs& inputs, const ReplicaRecord& target,
                                 const StateGenerationRecord& state, const ReplicaSetRecord& set,
                                 CommitId commit_id, bool make_owner, bool allow_ownership_takeover,
                                 ReplicaRecord* out, bool* ownership_written);

}  // namespace ccsf

namespace ccsf {

class Fabric::Impl {
 public:
  explicit Impl(const FabricConfig& config);
  ~Impl();

  Result<void> initialize();

  template <class Response, class Fn>
  Result<Response> mutate(Fn&& fn) {
    FabricMutation mutation;
    Response response{};
    std::unique_lock<std::mutex> commit_guard(commit_mutex_);
    {
      std::unique_lock<std::shared_mutex> guard(mutex_);
      if (shutting_down_) {
        return Error(ErrorCode::SHUTTING_DOWN, "the runtime is shutting down");
      }
      mutation.tick = plane_.logical_time + 1U;
      mutation.epoch = plane_.coordinator_epoch;
      const MutationInputs inputs{plane_, config_, mutation};
      Status status = fn(inputs, &response);
      if (!status && !mutation.commit_before_return) {
        return status.error();
      }
      pending_status_ = status;
    }
    Status persisted = persist_and_apply(mutation);
    if (!persisted) {
      return persisted.error();
    }
    if (!pending_status_) {
      return pending_status_.error();
    }
    return response;
  }

  /// Serializes, durably appends, then applies the effects. The state lock is
  /// not held across the durable append.
  Status persist_and_apply(const FabricMutation& mutation);

  /// Serializes, writes and resets the journal. Takes commit_mutex_, so no
  /// mutation can be in flight: a checkpoint must never reset the journal while
  /// a mutation is appending to it, or an acknowledged mutation would be lost.
  Status force_checkpoint();

  /// Same, for callers that already hold commit_mutex_.
  Status force_checkpoint_locked();

  Status checkpoint_image(const std::vector<std::byte>& image);

  [[nodiscard]] FabricSnapshot snapshot() const;
  [[nodiscard]] FabricStatus status() const;

  mutable std::shared_mutex mutex_;

  /// Serializes mutations. Lock order is always commit_mutex_ -> checkpoint_mutex_
  /// -> mutex_, and never the reverse.
  std::mutex commit_mutex_;

  /// Serializes durable checkpoints: two writers must never race for the same
  /// temporary snapshot file.
  std::mutex checkpoint_mutex_;
  detail::ControlPlane plane_;
  FabricConfig config_;
  std::unique_ptr<detail::PersistenceStore> store_;
  Status pending_status_;
  bool shutting_down_{false};
  bool recovered_from_durable_{false};
  std::uint64_t journal_since_checkpoint_{0};
};

}  // namespace ccsf

#endif  // CCSF_DETAIL_FABRIC_IMPL_HPP
