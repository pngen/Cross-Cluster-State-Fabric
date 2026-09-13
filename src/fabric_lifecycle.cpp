// Cross-Cluster State Fabric - runtime lifecycle, recovery and persistence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <utility>
#include <vector>

#include "detail/fabric_impl.hpp"

namespace ccsf {
namespace {

/// Conservative recovery classification for operations that were in flight when
/// the coordinator died. Nothing destructive is ever reissued blindly: an
/// operation whose outcome is not certain is moved to OUTCOME_UNKNOWN and its
/// destination copy is quarantined for revalidation.
struct RecoveryOutcome {
  OperationState state{OperationState::OUTCOME_UNKNOWN};
  bool destination_trusted{false};
  bool destination_abandoned{false};
  const char* reason{""};
};

RecoveryOutcome classify_recovery(OperationState state) {
  RecoveryOutcome outcome;
  switch (state) {
    case OperationState::PLANNED:
    case OperationState::RESERVED:
    case OperationState::STAGING_DESTINATION:
    case OperationState::TRANSFERRING:
      outcome.state = OperationState::ABANDONED;
      outcome.destination_abandoned = true;
      outcome.reason =
          "abandoned during coordinator recovery: the transfer had not completed, so the "
          "destination copy carries no authority";
      return outcome;
    case OperationState::TRANSFERRED:
    case OperationState::VERIFYING:
    case OperationState::PREPARED:
      outcome.state = OperationState::OUTCOME_UNKNOWN;
      outcome.reason =
          "outcome unknown after coordinator restart: a completed transfer was never committed, "
          "so the destination requires explicit revalidation";
      return outcome;
    case OperationState::COMMITTED:
    case OperationState::ADDED_TO_REPLICA_SET:
      outcome.state = OperationState::COMPLETE;
      outcome.destination_trusted = true;
      outcome.reason = "completed: the destination commit was already durable before the restart";
      return outcome;
    case OperationState::SOURCE_RETIRING:
      // Retirement is destructive and the commit record alone does not prove
      // that the source was actually retired. Preserve the source.
      outcome.state = OperationState::OUTCOME_UNKNOWN;
      outcome.reason =
          "source retirement was interrupted: the source is preserved and the retirement "
          "requires explicit reissue";
      return outcome;
    default:
      outcome.state = state;
      outcome.reason = "terminal state preserved";
      return outcome;
  }
}

}  // namespace

Fabric::Impl::Impl(const FabricConfig& config) : config_(config) {}

Fabric::Impl::~Impl() = default;

Status Fabric::Impl::persist_and_apply(const FabricMutation& mutation) {
  if (mutation.effects.empty()) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    if (mutation.tick > plane_.logical_time) {
      plane_.logical_time = mutation.tick;
    }
    return Status{};
  }

  std::uint64_t sequence = 0;
  if (store_ != nullptr) {
    {
      std::shared_lock<std::shared_mutex> guard(mutex_);
      sequence = plane_.durable_sequence + 1U;
    }
    Status appended = store_->append(mutation.effects, sequence, mutation.tick, mutation.epoch);
    if (!appended) {
      if (config_.require_durable_commits) {
        return appended.error();
      }
      std::unique_lock<std::shared_mutex> guard(mutex_);
      plane_.persistence_healthy = false;
      plane_.persistence_detail = appended.error().to_string();
    }
  }

  std::unique_lock<std::shared_mutex> guard(mutex_);
  for (const detail::Effect& effect : mutation.effects) {
    plane_.apply(effect);
  }
  if (mutation.tick > plane_.logical_time) {
    plane_.logical_time = mutation.tick;
  }
  plane_.durable_sequence = store_ != nullptr ? sequence : plane_.durable_sequence + 1U;
  ++plane_.journal_records;
  ++journal_since_checkpoint_;
  guard.unlock();
  if (store_ != nullptr && journal_since_checkpoint_ >= config_.journal_records_before_snapshot) {
    Status checkpointed = force_checkpoint_locked();
    if (!checkpointed && config_.require_durable_commits) {
      return checkpointed.error();
    }
  }
  return Status{};
}

Status Fabric::Impl::checkpoint_image(const std::vector<std::byte>& image) {
  if (store_ == nullptr) {
    return Status{};
  }
  Status written = store_->write_checkpoint(ByteSpan(image.data(), image.size()));
  if (!written) {
    std::unique_lock<std::shared_mutex> guard(mutex_);
    plane_.persistence_healthy = false;
    plane_.persistence_detail = written.error().to_string();
    return written.error();
  }
  std::unique_lock<std::shared_mutex> guard(mutex_);
  plane_.persistence_healthy = true;
  plane_.persistence_detail.clear();
  ++plane_.snapshots_written;
  journal_since_checkpoint_ = 0;
  return Status{};
}

Status Fabric::Impl::force_checkpoint() {
  if (store_ == nullptr) {
    return Error(ErrorCode::NOT_SUPPORTED, "no persistence directory is configured");
  }
  std::unique_lock<std::mutex> commit_guard(commit_mutex_);
  return force_checkpoint_locked();
}

Status Fabric::Impl::force_checkpoint_locked() {
  if (store_ == nullptr) {
    return Error(ErrorCode::NOT_SUPPORTED, "no persistence directory is configured");
  }
  std::unique_lock<std::mutex> checkpoint_guard(checkpoint_mutex_);
  std::vector<std::byte> image;
  {
    std::shared_lock<std::shared_mutex> guard(mutex_);
    image = detail::PersistenceStore::encode_snapshot(plane_);
  }
  return checkpoint_image(image);
}

Result<void> Fabric::Impl::initialize() {
  if (!config_.persistence_directory.empty()) {
    auto store = detail::PersistenceStore::open(config_.persistence_directory);
    if (!store) {
      return store.error();
    }
    store_ = std::move(store.value());
    auto loaded = store_->load(config_.maximum_records);
    if (!loaded) {
      return loaded.error();
    }
    plane_ = std::move(loaded.value());
    recovered_from_durable_ = plane_.recovered;
    plane_.persistence_enabled = true;
  }

  if (plane_.coordinator_id.valid() && plane_.coordinator_id != config_.coordinator_id) {
    return Error(ErrorCode::PERSISTENCE_CORRUPT,
                 "the durable control plane belongs to a different coordinator identity");
  }
  plane_.coordinator_id = config_.coordinator_id;
  if (config_.epoch_start > plane_.coordinator_epoch) {
    plane_.coordinator_epoch = config_.epoch_start;
  }
  if (!plane_.coordinator_epoch.valid()) {
    plane_.coordinator_epoch = CoordinatorEpoch::from_value(1);
  }
  if (config_.logical_start > plane_.logical_time) {
    plane_.logical_time = config_.logical_start;
  }
  if (recovered_from_durable_) {
    // Coordinator restart: the epoch must advance so that every message and
    // operation minted under the previous epoch is fenced.
    plane_.coordinator_epoch = plane_.coordinator_epoch.next();
    ++plane_.replay_generation;

    for (auto& entry : plane_.clusters) {
      ClusterRecord& cluster = entry.second;
      cluster.evidence.revalidation_required = true;
      cluster.evidence.present = false;
      if (cluster.lifecycle == ClusterLifecycle::ACTIVE) {
        cluster.lifecycle = ClusterLifecycle::SUSPECT;
      }
    }

    for (auto& entry : plane_.replicas) {
      ReplicaRecord& replica = entry.second;
      if (replica.is_terminal()) {
        continue;
      }
      // Durable committed membership survives the restart; only live evidence is
      // invalidated, so reuse is withheld until the replica is revalidated.
      replica.evidence.revalidation_required = true;
      replica.availability = ReplicaAvailabilityState::UNKNOWN;
    }

    for (auto& entry : plane_.operations) {
      OperationRecord& operation = entry.second;
      if (operation.is_terminal()) {
        continue;
      }
      const RecoveryOutcome outcome = classify_recovery(operation.state);
      operation.state = outcome.state;
      operation.recovery_required = !outcome.destination_trusted;
      operation.last_epoch = plane_.coordinator_epoch;
      operation.updated_at = ++plane_.logical_time;
      if (operation.journal.size() < kMaxOperationJournalEntries) {
        operation.journal.push_back(std::string("recovery: ") + outcome.reason);
      }
      ++plane_.counters.operations_recovered;

      if (outcome.destination_abandoned) {
        ++plane_.counters.operations_abandoned;
        auto destination = plane_.replicas.find(operation.destination_replica);
        if (destination != plane_.replicas.end()) {
          ReplicaRecord& replica = destination->second;
          if (!replica.commit_recorded) {
            replica.lifecycle = ReplicaLifecycle::ABANDONED;
            replica.authority = ReplicaAuthorityState::NONE;
            replica.counts_toward_factor = false;
            replica.availability = ReplicaAvailabilityState::ABSENT;
            replica.last_error = "abandoned during coordinator recovery";
            ++plane_.counters.replicas_abandoned;
          }
        }
        operation.destination_committed = false;
      } else if (outcome.state == OperationState::OUTCOME_UNKNOWN) {
        auto destination = plane_.replicas.find(operation.destination_replica);
        if (destination != plane_.replicas.end()) {
          ReplicaRecord& replica = destination->second;
          if (!replica.commit_recorded && !replica.is_terminal()) {
            replica.lifecycle = ReplicaLifecycle::QUARANTINED;
            replica.authority = ReplicaAuthorityState::FENCED;
            replica.counts_toward_factor = false;
            replica.availability = ReplicaAvailabilityState::UNKNOWN;
            replica.evidence.revalidation_required = true;
            replica.last_error = "quarantined during coordinator recovery";
            ++plane_.counters.replicas_quarantined;
          }
        }
        operation.destination_committed = false;
      } else {
        operation.destination_committed = true;
        operation.destination_verified = true;
        ++plane_.counters.operations_committed;
      }
    }

    for (auto& entry : plane_.replica_sets) {
      ReplicaSetRecord& set = entry.second;
      set.revalidation_required = true;
      set.epoch = plane_.coordinator_epoch;
    }

    // Rebuild every derived membership from the (recovered) replica records so
    // that counts cannot drift across a restart.
    for (auto& entry : plane_.replica_sets) {
      entry.second = detail::rebuild_replica_set(plane_, entry.second);
    }

    plane_.recompute_derived_counters();
    if (store_ != nullptr) {
      plane_.persistence_detail = "control plane recovered after coordinator restart";
    }
  }

  if (store_ != nullptr) {
    Status written = force_checkpoint();
    if (!written) {
      return written.error();
    }
  }
  return Status{};
}

FabricSnapshot Fabric::Impl::snapshot() const {
  std::shared_lock<std::shared_mutex> guard(mutex_);
  FabricSnapshot result;
  plane_.collect_snapshot(&result);
  return result;
}

FabricStatus Fabric::Impl::status() const {
  std::shared_lock<std::shared_mutex> guard(mutex_);
  FabricStatus result;
  result.coordinator_id = plane_.coordinator_id;
  result.coordinator_epoch = plane_.coordinator_epoch;
  result.logical_time = plane_.logical_time;
  result.durable_sequence = plane_.durable_sequence;
  result.journal_records = plane_.journal_records;
  result.replay_generation = plane_.replay_generation;
  result.persistence_enabled = store_ != nullptr;
  result.persistence_healthy = plane_.persistence_healthy;
  result.recovered_from_durable_state = recovered_from_durable_;
  result.journal_tail_discarded = plane_.journal_tail_truncated != 0U;
  result.persistence_detail = plane_.persistence_detail;
  result.state_generations = plane_.states.size();
  result.clusters = plane_.clusters.size();
  result.replicas = plane_.replicas.size();
  result.replica_sets = plane_.replica_sets.size();
  result.operations = plane_.operations.size();
  result.counters = plane_.counters;
  result.counters.durable_commits = plane_.durable_sequence;
  result.counters.journal_records_applied = plane_.journal_records;
  result.counters.snapshots_written = plane_.snapshots_written;
  std::uint64_t active_operations = 0;
  std::uint64_t active_transfers = 0;
  std::uint64_t reserved_bytes = 0;
  for (const auto& entry : plane_.operations) {
    const OperationRecord& operation = entry.second;
    if (operation.is_terminal()) {
      continue;
    }
    ++active_operations;
    if (operation.state == OperationState::TRANSFERRING ||
        operation.state == OperationState::STAGING_DESTINATION) {
      ++active_transfers;
    }
    if (operation.transfer_bytes.known()) {
      reserved_bytes += operation.transfer_bytes.bytes;
    }
  }
  result.counters.active_operations = active_operations;
  result.counters.active_transfers = active_transfers;
  result.counters.reserved_bytes = reserved_bytes;
  result.counters.reserved_transfer_slots = active_transfers;
  return result;
}

FabricSnapshot Fabric::snapshot() const { return impl_->snapshot(); }

FabricStatus Fabric::status() const { return impl_->status(); }

Fabric::Fabric(FabricConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {}

Fabric::~Fabric() = default;

Result<std::unique_ptr<Fabric>> Fabric::open(const FabricConfig& config) {
  if (!config.coordinator_id.valid()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "a coordinator identity is required");
  }
  std::unique_ptr<Fabric> fabric(new Fabric(config));
  Status initialized = fabric->impl_->initialize();
  if (!initialized) {
    return initialized.error();
  }
  return fabric;
}

CoordinatorId Fabric::coordinator_id() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  return impl_->plane_.coordinator_id;
}

CoordinatorEpoch Fabric::coordinator_epoch() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  return impl_->plane_.coordinator_epoch;
}

Tick Fabric::logical_time() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  return impl_->plane_.logical_time;
}

Result<Tick> Fabric::advance_time(Tick delta) {
  return impl_->mutate<Tick>(
      [delta](const MutationInputs& inputs, Tick* out) -> Status {
        if (delta == 0) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "delta must be positive");
        }
        *out = inputs.mutation.tick + delta - 1U;
        inputs.mutation.tick = *out;
        return Status{};
      });
}

Result<CoordinatorEpoch> Fabric::advance_epoch() {
  return impl_->mutate<CoordinatorEpoch>(
      [](const MutationInputs& inputs, CoordinatorEpoch* out) -> Status {
        const CoordinatorEpoch next = inputs.plane.coordinator_epoch.next();
        inputs.mutation.epoch = next;
        inputs.mutation.effects.push_back(detail::Effect::set_epoch(next));
        *out = next;
        return Status{};
      });
}

Status Fabric::persist() { return impl_->force_checkpoint(); }

Status Fabric::shutdown() {
  {
    std::unique_lock<std::mutex> commit_guard(impl_->commit_mutex_);
    std::unique_lock<std::shared_mutex> guard(impl_->mutex_);
    if (impl_->shutting_down_) {
      return Status{};
    }
    impl_->shutting_down_ = true;
  }
  if (impl_->store_ != nullptr) {
    return impl_->force_checkpoint();
  }
  return Status{};
}

}  // namespace ccsf
