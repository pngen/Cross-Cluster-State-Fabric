// Cross-Cluster State Fabric - durable control plane implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "detail/control_plane.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "ccsf/crc32c.hpp"
#include "detail/platform_io.hpp"

namespace ccsf::detail {
namespace {

constexpr char kSnapshotName[] = "control.snapshot";
constexpr char kJournalName[] = "control.journal";

Error corrupt(std::string message) {
  return Error(ErrorCode::PERSISTENCE_CORRUPT, std::move(message));
}

template <class T>
bool decode_record(ByteReader& reader, T* out, const char* name) {
  if (!decode(reader, out)) {
    if (reader.ok()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, std::string("malformed ") + name + " record");
    }
    return false;
  }
  return true;
}

}  // namespace

Effect Effect::set_next_id(std::uint64_t next_id) {
  Effect effect;
  effect.kind = EffectKind::SET_NEXT_ID;
  effect.scalar = next_id;
  return effect;
}
Effect Effect::put_state(StateGenerationRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_STATE;
  effect.state = std::move(record);
  return effect;
}
Effect Effect::put_cluster(ClusterRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_CLUSTER;
  effect.cluster = std::move(record);
  return effect;
}
Effect Effect::put_replica(ReplicaRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_REPLICA;
  effect.replica = std::move(record);
  return effect;
}
Effect Effect::put_replica_set(ReplicaSetRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_REPLICA_SET;
  effect.replica_set = std::move(record);
  return effect;
}
Effect Effect::put_operation(OperationRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_OPERATION;
  effect.operation = std::move(record);
  return effect;
}
Effect Effect::put_authorization(TransferAuthorization record) {
  Effect effect;
  effect.kind = EffectKind::PUT_AUTHORIZATION;
  effect.authorization = std::move(record);
  return effect;
}
Effect Effect::put_ownership(OwnershipRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_OWNERSHIP;
  effect.ownership = std::move(record);
  return effect;
}
Effect Effect::put_compatibility(CompatibilityRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_COMPATIBILITY;
  effect.compatibility = std::move(record);
  return effect;
}
Effect Effect::put_placement_policy(PlacementPolicyRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_PLACEMENT_POLICY;
  effect.placement_policy = std::move(record);
  return effect;
}
Effect Effect::put_replication_policy(ReplicationPolicyRecord record) {
  Effect effect;
  effect.kind = EffectKind::PUT_REPLICATION_POLICY;
  effect.replication_policy = std::move(record);
  return effect;
}
Effect Effect::put_counters(FabricCounters counters) {
  Effect effect;
  effect.kind = EffectKind::PUT_COUNTERS;
  effect.counters = counters;
  return effect;
}
Effect Effect::set_epoch(CoordinatorEpoch epoch) {
  Effect effect;
  effect.kind = EffectKind::SET_EPOCH;
  effect.epoch = epoch;
  return effect;
}

void encode_effect(ByteWriter& writer, const Effect& effect) {
  encode_enum(writer, effect.kind);
  switch (effect.kind) {
    case EffectKind::PUT_STATE: encode(writer, effect.state); break;
    case EffectKind::PUT_CLUSTER: encode(writer, effect.cluster); break;
    case EffectKind::PUT_REPLICA: encode(writer, effect.replica); break;
    case EffectKind::PUT_REPLICA_SET: encode(writer, effect.replica_set); break;
    case EffectKind::PUT_OPERATION: encode(writer, effect.operation); break;
    case EffectKind::PUT_AUTHORIZATION: encode(writer, effect.authorization); break;
    case EffectKind::PUT_OWNERSHIP: encode(writer, effect.ownership); break;
    case EffectKind::PUT_COMPATIBILITY: encode(writer, effect.compatibility); break;
    case EffectKind::PUT_PLACEMENT_POLICY: encode(writer, effect.placement_policy); break;
    case EffectKind::PUT_REPLICATION_POLICY: encode(writer, effect.replication_policy); break;
    case EffectKind::PUT_COUNTERS: encode(writer, effect.counters); break;
    case EffectKind::SET_EPOCH: encode_id(writer, effect.epoch); break;
    case EffectKind::SET_NEXT_ID: writer.u64(effect.scalar); break;
  }
}

bool decode_effect(ByteReader& reader, Effect* effect) {
  std::uint16_t raw = 0;
  if (!reader.u16(&raw)) {
    return false;
  }
  if (raw < static_cast<std::uint16_t>(EffectKind::PUT_STATE) ||
      raw > static_cast<std::uint16_t>(EffectKind::SET_NEXT_ID)) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "invalid journal effect kind");
    return false;
  }
  effect->kind = static_cast<EffectKind>(raw);
  switch (effect->kind) {
    case EffectKind::PUT_STATE: return decode_record(reader, &effect->state, "state");
    case EffectKind::PUT_CLUSTER: return decode_record(reader, &effect->cluster, "cluster");
    case EffectKind::PUT_REPLICA: return decode_record(reader, &effect->replica, "replica");
    case EffectKind::PUT_REPLICA_SET:
      return decode_record(reader, &effect->replica_set, "replica set");
    case EffectKind::PUT_OPERATION: return decode_record(reader, &effect->operation, "operation");
    case EffectKind::PUT_AUTHORIZATION:
      return decode_record(reader, &effect->authorization, "authorization");
    case EffectKind::PUT_OWNERSHIP: return decode_record(reader, &effect->ownership, "ownership");
    case EffectKind::PUT_COMPATIBILITY:
      return decode_record(reader, &effect->compatibility, "compatibility");
    case EffectKind::PUT_PLACEMENT_POLICY:
      return decode_record(reader, &effect->placement_policy, "placement policy");
    case EffectKind::PUT_REPLICATION_POLICY:
      return decode_record(reader, &effect->replication_policy, "replication policy");
    case EffectKind::PUT_COUNTERS: return decode_record(reader, &effect->counters, "counters");
    case EffectKind::SET_EPOCH: return decode_id(reader, &effect->epoch);
    case EffectKind::SET_NEXT_ID: return reader.u64(&effect->scalar);
  }
  return false;
}

void ControlPlane::apply(const Effect& effect) {
  auto bump = [this](std::uint64_t value) {
    if (value >= next_id) {
      next_id = value + 1U;
    }
  };
  switch (effect.kind) {
    case EffectKind::PUT_STATE: {
      const StateGenerationRecord& record = effect.state;
      bump(record.state_id.value());
      bump(record.generation.value());
      bump(record.compatibility_id.value());
      bump(record.replica_set_id.value());
      bump(record.reconstruction.reconstruction_id.value());
      states[StateKey{record.state_id, record.generation}] = record;
      break;
    }
    case EffectKind::PUT_CLUSTER: {
      const ClusterRecord& record = effect.cluster;
      bump(record.cluster_id.value());
      bump(record.incarnation.value());
      bump(record.worker_id.value());
      bump(record.worker_boot_id.value());
      clusters[record.cluster_id] = record;
      break;
    }
    case EffectKind::PUT_REPLICA: {
      const ReplicaRecord& record = effect.replica;
      bump(record.replica_id.value());
      bump(record.replica_generation.value());
      bump(record.transfer_id.value());
      bump(record.operation_id.value());
      replicas[record.replica_id] = record;
      break;
    }
    case EffectKind::PUT_REPLICA_SET: {
      const ReplicaSetRecord& record = effect.replica_set;
      bump(record.replica_set_id.value());
      bump(record.generation.value());
      replica_sets[StateKey{record.state_id, record.state_generation}] = record;
      break;
    }
    case EffectKind::PUT_OPERATION: {
      const OperationRecord& record = effect.operation;
      bump(record.operation_id.value());
      bump(record.migration_id.value());
      bump(record.transfer_id.value());
      bump(record.reservation_id.value());
      operations[record.operation_id] = record;
      break;
    }
    case EffectKind::PUT_AUTHORIZATION: {
      const TransferAuthorization& record = effect.authorization;
      bump(record.operation_id.value());
      bump(record.transfer_id.value());
      bump(record.reservation_id.value());
      authorizations[record.operation_id] = record;
      break;
    }
    case EffectKind::PUT_OWNERSHIP: {
      const OwnershipRecord& record = effect.ownership;
      bump(record.commit.value());
      ownership[StateKey{record.state_id, record.state_generation}] = record;
      break;
    }
    case EffectKind::PUT_COMPATIBILITY: {
      const CompatibilityRecord& record = effect.compatibility;
      bump(record.compatibility_id.value());
      bump(record.generation.value());
      compatibility_records[record.compatibility_id] = record;
      break;
    }
    case EffectKind::PUT_PLACEMENT_POLICY: {
      const PlacementPolicyRecord& record = effect.placement_policy;
      bump(record.placement_policy_id.value());
      bump(record.generation.value());
      placement_policies[record.placement_policy_id] = record;
      break;
    }
    case EffectKind::PUT_REPLICATION_POLICY: {
      const ReplicationPolicyRecord& record = effect.replication_policy;
      bump(record.replication_policy_id.value());
      bump(record.generation.value());
      replication_policies[record.replication_policy_id] = record;
      break;
    }
    case EffectKind::PUT_COUNTERS:
      counters = effect.counters;
      break;
    case EffectKind::SET_EPOCH:
      if (effect.epoch > coordinator_epoch) {
        coordinator_epoch = effect.epoch;
      }
      break;
    case EffectKind::SET_NEXT_ID:
      if (effect.scalar > next_id) {
        next_id = effect.scalar;
      }
      break;
  }
}

std::size_t ControlPlane::record_count() const noexcept {
  return states.size() + clusters.size() + replicas.size() + replica_sets.size() +
         operations.size() + authorizations.size() + ownership.size() +
         compatibility_records.size() + placement_policies.size() + replication_policies.size();
}

void ControlPlane::recompute_derived_counters() noexcept {
  std::uint64_t active_operations = 0;
  std::uint64_t active_transfers = 0;
  std::uint64_t reserved_bytes = 0;
  for (const auto& entry : operations) {
    if (!entry.second.is_terminal()) {
      ++active_operations;
      if (entry.second.state == OperationState::TRANSFERRING ||
          entry.second.state == OperationState::STAGING_DESTINATION) {
        ++active_transfers;
      }
      if (entry.second.transfer_bytes.known()) {
        reserved_bytes += entry.second.transfer_bytes.bytes;
      }
    }
  }
  counters.active_operations = active_operations;
  counters.active_transfers = active_transfers;
  counters.reserved_bytes = reserved_bytes;
  counters.reserved_transfer_slots = active_transfers;
}

bool ControlPlane::validate(std::string* reason) const {
  auto fail = [reason](std::string text) {
    if (reason != nullptr) {
      *reason = std::move(text);
    }
    return false;
  };
  for (const auto& entry : replicas) {
    const ReplicaRecord& replica = entry.second;
    if (replica.replica_id != entry.first) {
      return fail("replica map key does not match the replica identity");
    }
    if (states.find(StateKey{replica.state_id, replica.state_generation}) == states.end()) {
      return fail("replica " + replica.replica_id.str() +
                  " references an unregistered state generation");
    }
    if (clusters.find(replica.location.cluster_id) == clusters.end()) {
      return fail("replica " + replica.replica_id.str() + " references an unregistered cluster");
    }
    if (replica.replica_set_id.valid() &&
        replica_sets.find(StateKey{replica.state_id, replica.state_generation}) ==
            replica_sets.end()) {
      return fail("replica " + replica.replica_id.str() +
                  " references a missing replica set for its state generation");
    }
  }
  for (const auto& entry : replica_sets) {
    const ReplicaSetRecord& set = entry.second;
    if (states.find(entry.first) == states.end()) {
      return fail("replica set references an unregistered state generation");
    }
    for (const ReplicaSetMember& member : set.authoritative_members) {
      if (replicas.find(member.replica_id) == replicas.end()) {
        return fail("replica set member " + member.replica_id.str() +
                    " is not a registered replica");
      }
    }
    for (const ReplicaSetMember& member : set.degraded_members) {
      if (replicas.find(member.replica_id) == replicas.end()) {
        return fail("degraded replica set member " + member.replica_id.str() +
                    " is not a registered replica");
      }
    }
  }
  for (const auto& entry : operations) {
    const OperationRecord& operation = entry.second;
    if (states.find(StateKey{operation.state_id, operation.state_generation}) == states.end()) {
      return fail("operation " + operation.operation_id.str() +
                  " references an unregistered state generation");
    }
  }
  for (const auto& entry : ownership) {
    if (states.find(entry.first) == states.end()) {
      return fail("ownership record references an unregistered state generation");
    }
  }
  for (const auto& entry : states) {
    const StateGenerationRecord& state = entry.second;
    if (state.state_id != entry.first.state_id || state.generation != entry.first.generation) {
      return fail("state map key does not match the state record identity");
    }
  }
  return true;
}

void ControlPlane::canonicalize() {
  // std::map already provides deterministic ordering for every collection.
}

ReplicaSetMember make_member(const ReplicaRecord& replica) {
  ReplicaSetMember member;
  member.replica_id = replica.replica_id;
  member.replica_generation = replica.replica_generation;
  member.cluster_id = replica.location.cluster_id;
  member.cluster_incarnation = replica.location.cluster_incarnation;
  member.region = replica.location.region;
  member.failure_domain = replica.location.failure_domain;
  member.authoritative = replica_counts_toward_replication_factor(replica);
  member.lifecycle = replica.lifecycle;
  member.integrity = replica.integrity;
  member.compatibility = replica.compatibility;
  member.authority = replica.authority;
  member.commit_id = replica.commit_id;
  member.joined_at = replica.updated_at;
  return member;
}

ReplicaSetRecord rebuild_replica_set(const ControlPlane& plane, ReplicaSetRecord set) {
  set.authoritative_members.clear();
  set.degraded_members.clear();
  for (const auto& entry : plane.replicas) {
    const ReplicaRecord& replica = entry.second;
    if (replica.state_id != set.state_id || replica.state_generation != set.state_generation) {
      continue;
    }
    if (replica.is_terminal()) {
      continue;
    }
    if (replica_counts_toward_replication_factor(replica)) {
      set.authoritative_members.push_back(make_member(replica));
    } else {
      set.degraded_members.push_back(make_member(replica));
    }
  }
  return recompute_replica_set(std::move(set));
}

void ControlPlane::collect_snapshot(FabricSnapshot* snapshot) const {
  snapshot->coordinator_id = coordinator_id;
  snapshot->coordinator_epoch = coordinator_epoch;
  snapshot->logical_time = logical_time;
  snapshot->durable_sequence = durable_sequence;
  snapshot->replay_generation = replay_generation;
  snapshot->counters = counters;
  snapshot->counters.durable_commits = durable_sequence;
  snapshot->counters.journal_records_applied = journal_records;
  snapshot->counters.snapshots_written = snapshots_written;
  // Live operation accounting is derived from the operation records so that it
  // can never disagree with them.
  std::uint64_t active_operations = 0;
  std::uint64_t active_transfers = 0;
  std::uint64_t reserved_bytes = 0;
  for (const auto& entry : operations) {
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
  snapshot->counters.active_operations = active_operations;
  snapshot->counters.active_transfers = active_transfers;
  snapshot->counters.reserved_bytes = reserved_bytes;
  snapshot->counters.reserved_transfer_slots = active_transfers;
  snapshot->states.clear();
  snapshot->clusters.clear();
  snapshot->replicas.clear();
  snapshot->replica_sets.clear();
  snapshot->operations.clear();
  snapshot->ownership.clear();
  snapshot->compatibility_records.clear();
  snapshot->active_authorizations.clear();
  for (const auto& entry : states) {
    snapshot->states.push_back(entry.second);
  }
  for (const auto& entry : clusters) {
    snapshot->clusters.push_back(entry.second);
  }
  for (const auto& entry : replicas) {
    snapshot->replicas.push_back(entry.second);
  }
  for (const auto& entry : replica_sets) {
    snapshot->replica_sets.push_back(rebuild_replica_set(*this, entry.second));
  }
  for (const auto& entry : operations) {
    snapshot->operations.push_back(entry.second);
  }
  for (const auto& entry : ownership) {
    snapshot->ownership.push_back(entry.second);
  }
  for (const auto& entry : compatibility_records) {
    snapshot->compatibility_records.push_back(entry.second);
  }
  for (const auto& entry : authorizations) {
    snapshot->active_authorizations.push_back(entry.second);
  }
}

void encode_control_plane(ByteWriter& writer, const ControlPlane& plane) {
  encode_id(writer, plane.coordinator_id);
  encode_id(writer, plane.coordinator_epoch);
  writer.u64(plane.logical_time);
  writer.u64(plane.durable_sequence);
  writer.u64(plane.replay_generation);
  encode(writer, plane.counters);

  writer.u32(static_cast<std::uint32_t>(plane.states.size()));
  for (const auto& entry : plane.states) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.clusters.size()));
  for (const auto& entry : plane.clusters) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.replicas.size()));
  for (const auto& entry : plane.replicas) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.replica_sets.size()));
  for (const auto& entry : plane.replica_sets) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.operations.size()));
  for (const auto& entry : plane.operations) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.authorizations.size()));
  for (const auto& entry : plane.authorizations) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.ownership.size()));
  for (const auto& entry : plane.ownership) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.compatibility_records.size()));
  for (const auto& entry : plane.compatibility_records) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.placement_policies.size()));
  for (const auto& entry : plane.placement_policies) {
    encode(writer, entry.second);
  }
  writer.u32(static_cast<std::uint32_t>(plane.replication_policies.size()));
  for (const auto& entry : plane.replication_policies) {
    encode(writer, entry.second);
  }
}

bool decode_control_plane(ByteReader& reader, ControlPlane* plane) {
  if (!decode_id(reader, &plane->coordinator_id)) return false;
  if (!decode_id(reader, &plane->coordinator_epoch)) return false;
  if (!reader.u64(&plane->logical_time)) return false;
  if (!reader.u64(&plane->durable_sequence)) return false;
  if (!reader.u64(&plane->replay_generation)) return false;
  if (!decode(reader, &plane->counters)) return false;

  std::uint32_t count = 0;
  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedStates) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared state count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    StateGenerationRecord record;
    if (!decode(reader, &record)) return false;
    const StateKey key{record.state_id, record.generation};
    if (plane->states.find(key) != plane->states.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate state generation record");
      return false;
    }
    plane->states.emplace(key, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedClusters) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared cluster count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    ClusterRecord record;
    if (!decode(reader, &record)) return false;
    if (plane->clusters.find(record.cluster_id) != plane->clusters.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate cluster record");
      return false;
    }
    plane->clusters.emplace(record.cluster_id, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedReplicas) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared replica count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    ReplicaRecord record;
    if (!decode(reader, &record)) return false;
    if (plane->replicas.find(record.replica_id) != plane->replicas.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate replica record");
      return false;
    }
    plane->replicas.emplace(record.replica_id, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedRecords) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared replica set count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    ReplicaSetRecord record;
    if (!decode(reader, &record)) return false;
    const StateKey key{record.state_id, record.state_generation};
    if (plane->replica_sets.find(key) != plane->replica_sets.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate replica set record");
      return false;
    }
    plane->replica_sets.emplace(key, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedOperations) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared operation count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    OperationRecord record;
    if (!decode(reader, &record)) return false;
    if (plane->operations.find(record.operation_id) != plane->operations.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate operation record");
      return false;
    }
    plane->operations.emplace(record.operation_id, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedOperations) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared authorization count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    TransferAuthorization record;
    if (!decode(reader, &record)) return false;
    if (plane->authorizations.find(record.operation_id) != plane->authorizations.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate authorization record");
      return false;
    }
    plane->authorizations.emplace(record.operation_id, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedRecords) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared ownership count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    OwnershipRecord record;
    if (!decode(reader, &record)) return false;
    const StateKey key{record.state_id, record.state_generation};
    if (plane->ownership.find(key) != plane->ownership.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate ownership record");
      return false;
    }
    plane->ownership.emplace(key, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedClusters) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared compatibility count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    CompatibilityRecord record;
    if (!decode(reader, &record)) return false;
    if (plane->compatibility_records.find(record.compatibility_id) !=
        plane->compatibility_records.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate compatibility record");
      return false;
    }
    plane->compatibility_records.emplace(record.compatibility_id, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedClusters) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "declared placement policy count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    PlacementPolicyRecord record;
    if (!decode(reader, &record)) return false;
    if (plane->placement_policies.find(record.placement_policy_id) !=
        plane->placement_policies.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate placement policy record");
      return false;
    }
    plane->placement_policies.emplace(record.placement_policy_id, std::move(record));
  }

  if (!reader.u32(&count)) return false;
  if (static_cast<std::size_t>(count) > kMaxDecodedClusters) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT,
                "declared replication policy count exceeds the bound");
    return false;
  }
  for (std::uint32_t index = 0; index < count; ++index) {
    ReplicationPolicyRecord record;
    if (!decode(reader, &record)) return false;
    if (plane->replication_policies.find(record.replication_policy_id) !=
        plane->replication_policies.end()) {
      reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "duplicate replication policy record");
      return false;
    }
    plane->replication_policies.emplace(record.replication_policy_id, std::move(record));
  }

  return reader.ok();
}

std::vector<std::byte> encode_journal_record(const std::vector<Effect>& effects,
                                             std::uint64_t sequence, Tick tick,
                                             CoordinatorEpoch epoch) {
  ByteWriter payload;
  payload.u16(static_cast<std::uint16_t>(1));  // command: effect batch
  payload.u64(sequence);
  payload.u64(tick);
  encode_id(payload, epoch);
  payload.u16(static_cast<std::uint16_t>(effects.size()));
  for (const Effect& effect : effects) {
    encode_effect(payload, effect);
  }

  ByteWriter record;
  record.u32(kRecordMagic);
  record.u32(static_cast<std::uint32_t>(payload.size()));
  const std::uint32_t checksum = crc32c(payload.data());
  record.u32(checksum);
  record.raw(payload.data());
  return record.data();
}

bool decode_journal_record(ByteSpan payload, JournalRecordImage* record) {
  ByteReader reader(payload);
  std::uint16_t command = 0;
  if (!reader.u16(&command)) {
    return false;
  }
  if (command != 1U) {
    reader.fail(ErrorCode::PERSISTENCE_UNSUPPORTED_VERSION, "unknown journal command");
    return false;
  }
  CoordinatorEpoch epoch;
  if (!reader.u64(&record->sequence) || !reader.u64(&record->tick) ||
      !decode_id(reader, &epoch)) {
    return false;
  }
  std::uint16_t count = 0;
  if (!reader.u16(&count)) {
    return false;
  }
  if (static_cast<std::size_t>(count) > kMaxJournalEffectsPerRecord) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "journal record declares too many effects");
    return false;
  }
  record->effects.clear();
  record->effects.reserve(count);
  for (std::uint16_t index = 0; index < count; ++index) {
    Effect effect;
    if (!decode_effect(reader, &effect)) {
      return false;
    }
    record->effects.push_back(std::move(effect));
  }
  return reader.require_end();
}

Result<JournalScanResult> scan_journal(ByteSpan image) {
  JournalScanResult result;
  if (image.size() < kJournalHeaderBytes) {
    if (image.empty()) {
      return result;
    }
    return Error(ErrorCode::PERSISTENCE_TRUNCATED, "journal header is truncated");
  }
  ByteReader header(image.first(kJournalHeaderBytes));
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t reserved = 0;
  std::uint32_t checksum = 0;
  if (!header.u32(&magic) || !header.u16(&version) || !header.u16(&reserved) ||
      !header.u32(&checksum)) {
    return Error(ErrorCode::PERSISTENCE_TRUNCATED, "journal header is truncated");
  }
  if (magic != kJournalMagic) {
    return Error(ErrorCode::PERSISTENCE_CORRUPT, "journal magic mismatch");
  }
  if (version != kControlPlaneFormatVersion) {
    return Error(ErrorCode::PERSISTENCE_UNSUPPORTED_VERSION, "unsupported journal format version");
  }
  ByteWriter header_bytes;
  header_bytes.u32(magic);
  header_bytes.u16(version);
  header_bytes.u16(reserved);
  header_bytes.u32(0);
  if (crc32c(header_bytes.data()) != checksum) {
    return Error(ErrorCode::PERSISTENCE_CORRUPT, "journal header checksum mismatch");
  }
  result.header_bytes = kJournalHeaderBytes;

  std::size_t offset = kJournalHeaderBytes;
  while (offset < image.size()) {
    const std::size_t remaining = image.size() - offset;
    if (remaining < kRecordHeaderBytes) {
      result.trailing_partial_bytes = remaining;
      break;
    }
    ByteReader record_header(image.subspan(offset, kRecordHeaderBytes));
    std::uint32_t record_magic = 0;
    std::uint32_t length = 0;
    std::uint32_t payload_checksum = 0;
    if (!record_header.u32(&record_magic) || !record_header.u32(&length) ||
        !record_header.u32(&payload_checksum)) {
      result.trailing_partial_bytes = remaining;
      break;
    }
    const std::uint64_t available = static_cast<std::uint64_t>(remaining - kRecordHeaderBytes);
    if (record_magic != kRecordMagic) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT,
                   "journal record magic mismatch at offset " + std::to_string(offset));
    }
    if (length > kMaxPayloadBytes) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT, "journal record declares an oversized payload");
    }
    if (available < length) {
      result.trailing_partial_bytes = remaining;
      break;
    }
    const ByteSpan payload = image.subspan(offset + kRecordHeaderBytes, length);
    if (crc32c(payload) != payload_checksum) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT,
                   "journal record checksum mismatch at offset " + std::to_string(offset));
    }
    JournalRecordImage record;
    if (!decode_journal_record(payload, &record)) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT,
                   "journal record payload is malformed at offset " + std::to_string(offset));
    }
    result.records.push_back(std::move(record));
    ++result.complete_records;
    offset += kRecordHeaderBytes + length;
  }
  return result;
}

std::string PersistenceStore::snapshot_path() const {
  return join_path(directory_, kSnapshotName);
}

std::string PersistenceStore::journal_path() const {
  return join_path(directory_, kJournalName);
}

Result<std::unique_ptr<PersistenceStore>> PersistenceStore::open(const std::string& directory) {
  if (directory.empty()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "persistence directory must not be empty");
  }
  Status created = ensure_directory(directory);
  if (!created) {
    return created.error();
  }
  return std::unique_ptr<PersistenceStore>(new PersistenceStore(directory));
}

Result<ControlPlane> PersistenceStore::load(std::uint64_t max_records) const {
  ControlPlane plane;
  auto exists = file_exists(snapshot_path());
  if (!exists) {
    return exists.error();
  }
  if (exists.value()) {
    auto image = read_file(snapshot_path());
    if (!image) {
      return image.error();
    }
    const std::vector<std::byte>& bytes = image.value();
    if (bytes.size() < kSnapshotHeaderBytes) {
      return Error(ErrorCode::PERSISTENCE_TRUNCATED, "snapshot file is smaller than its header");
    }
    ByteReader header(ByteSpan(bytes.data(), kSnapshotHeaderBytes));
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint64_t sequence = 0;
    std::uint64_t payload_length = 0;
    std::uint32_t payload_checksum = 0;
    std::uint32_t header_checksum = 0;
    if (!header.u32(&magic) || !header.u16(&version) || !header.u16(&reserved) ||
        !header.u64(&sequence) || !header.u64(&payload_length) ||
        !header.u32(&payload_checksum) || !header.u32(&header_checksum)) {
      return Error(ErrorCode::PERSISTENCE_TRUNCATED, "snapshot header is truncated");
    }
    if (magic != kSnapshotMagic) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT, "snapshot magic mismatch");
    }
    if (version != kControlPlaneFormatVersion) {
      return Error(ErrorCode::PERSISTENCE_UNSUPPORTED_VERSION,
                   "unsupported snapshot format version");
    }
    ByteWriter header_bytes;
    header_bytes.u32(magic);
    header_bytes.u16(version);
    header_bytes.u16(reserved);
    header_bytes.u64(sequence);
    header_bytes.u64(payload_length);
    header_bytes.u32(payload_checksum);
    header_bytes.u32(0);
    if (crc32c(header_bytes.data()) != header_checksum) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT, "snapshot header checksum mismatch");
    }
    std::uint64_t expected_size = 0;
    if (!checked_add(kSnapshotHeaderBytes, payload_length, &expected_size)) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT, "snapshot declares an impossible size");
    }
    if (expected_size != bytes.size()) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT,
                   "snapshot size does not match its declared payload length");
    }
    if (payload_length > kMaxPayloadBytes) {
      return Error(ErrorCode::RESOURCE_EXHAUSTED,
                   "snapshot payload exceeds the bounded read limit");
    }
    const ByteSpan payload(bytes.data() + kSnapshotHeaderBytes,
                           static_cast<std::size_t>(payload_length));
    if (crc32c(payload) != payload_checksum) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT, "snapshot payload checksum mismatch");
    }
    ByteReader reader(payload);
    if (!decode_control_plane(reader, &plane)) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT,
                   "snapshot payload is malformed: " + reader.error().to_string());
    }
    if (!reader.require_end()) {
      return Error(ErrorCode::PERSISTENCE_CORRUPT, "snapshot payload has trailing bytes");
    }
    plane.durable_sequence = sequence;
  }

  auto journal_exists = file_exists(journal_path());
  if (!journal_exists) {
    return journal_exists.error();
  }
  if (journal_exists.value()) {
    auto image = read_file(journal_path());
    if (!image) {
      return image.error();
    }
    auto scanned = scan_journal(ByteSpan(image.value().data(), image.value().size()));
    if (!scanned) {
      return scanned.error();
    }
    const JournalScanResult& scan = scanned.value();
    plane.journal_records = scan.complete_records;
    plane.journal_tail_truncated = scan.trailing_partial_bytes == 0 ? 0U : 1U;
    for (const JournalRecordImage& record : scan.records) {
      for (const Effect& effect : record.effects) {
        plane.apply(effect);
      }
      if (record.tick > plane.logical_time) {
        plane.logical_time = record.tick;
      }
      if (record.sequence > plane.durable_sequence) {
        plane.durable_sequence = record.sequence;
      }
    }
    if (plane.journal_tail_truncated != 0U) {
      plane.persistence_detail =
          "journal contained a truncated tail record which was discarded as an "
          "unacknowledged mutation";
    }
  }

  plane.persistence_enabled = true;
  plane.recovered = true;
  plane.next_id = 1;
  std::uint64_t highest = 0;
  for (const auto& entry : plane.states) {
    highest = std::max(highest, entry.second.state_id.value());
    highest = std::max(highest, entry.second.generation.value());
    highest = std::max(highest, entry.second.replica_set_id.value());
    highest = std::max(highest, entry.second.compatibility_id.value());
  }
  for (const auto& entry : plane.clusters) {
    highest = std::max(highest, entry.second.cluster_id.value());
    highest = std::max(highest, entry.second.incarnation.value());
    highest = std::max(highest, entry.second.worker_id.value());
    highest = std::max(highest, entry.second.worker_boot_id.value());
  }
  for (const auto& entry : plane.replicas) {
    highest = std::max(highest, entry.second.replica_id.value());
    highest = std::max(highest, entry.second.replica_generation.value());
  }
  for (const auto& entry : plane.replica_sets) {
    highest = std::max(highest, entry.second.replica_set_id.value());
    highest = std::max(highest, entry.second.generation.value());
  }
  for (const auto& entry : plane.operations) {
    highest = std::max(highest, entry.second.operation_id.value());
    highest = std::max(highest, entry.second.migration_id.value());
    highest = std::max(highest, entry.second.transfer_id.value());
    highest = std::max(highest, entry.second.reservation_id.value());
  }
  for (const auto& entry : plane.authorizations) {
    highest = std::max(highest, entry.second.operation_id.value());
    highest = std::max(highest, entry.second.transfer_id.value());
  }
  for (const auto& entry : plane.compatibility_records) {
    highest = std::max(highest, entry.second.compatibility_id.value());
  }
  for (const auto& entry : plane.placement_policies) {
    highest = std::max(highest, entry.second.placement_policy_id.value());
  }
  for (const auto& entry : plane.replication_policies) {
    highest = std::max(highest, entry.second.replication_policy_id.value());
  }
  plane.next_id = highest + 1U;

  if (plane.record_count() > max_records) {
    return Error(ErrorCode::RESOURCE_EXHAUSTED,
                 "durable control plane holds more records than the configured bound");
  }
  std::string reason;
  if (!plane.validate(&reason)) {
    return Error(ErrorCode::PERSISTENCE_CORRUPT, "control plane invariants violated: " + reason);
  }
  return plane;
}

Status PersistenceStore::append(const std::vector<Effect>& effects, std::uint64_t sequence,
                                Tick tick, CoordinatorEpoch epoch) {
  const std::vector<std::byte> record = encode_journal_record(effects, sequence, tick, epoch);
  auto exists = file_exists(journal_path());
  if (!exists) {
    return exists.error();
  }
  if (!exists.value()) {
    ByteWriter header;
    header.u32(kJournalMagic);
    header.u16(kControlPlaneFormatVersion);
    header.u16(0);
    header.u32(0);
    ByteWriter finalized;
    finalized.u32(kJournalMagic);
    finalized.u16(kControlPlaneFormatVersion);
    finalized.u16(0);
    finalized.u32(crc32c(header.data()));
    Status written = append_file_durable(journal_path(), finalized.data());
    if (!written) {
      return written.error();
    }
  }
  return append_file_durable(journal_path(), record);
}

std::vector<std::byte> PersistenceStore::encode_snapshot(const ControlPlane& plane) {
  ByteWriter payload;
  encode_control_plane(payload, plane);

  ByteWriter header;
  header.u32(kSnapshotMagic);
  header.u16(kControlPlaneFormatVersion);
  header.u16(0);
  header.u64(plane.durable_sequence);
  header.u64(static_cast<std::uint64_t>(payload.size()));
  header.u32(crc32c(payload.data()));
  header.u32(0);
  ByteWriter finalized_header;
  finalized_header.u32(kSnapshotMagic);
  finalized_header.u16(kControlPlaneFormatVersion);
  finalized_header.u16(0);
  finalized_header.u64(plane.durable_sequence);
  finalized_header.u64(static_cast<std::uint64_t>(payload.size()));
  finalized_header.u32(crc32c(payload.data()));
  finalized_header.u32(crc32c(header.data()));

  ByteWriter image;
  image.raw(finalized_header.data());
  image.raw(payload.data());
  return image.data();
}

Status PersistenceStore::write_checkpoint(ByteSpan snapshot_image) {
  const std::string snapshot_tmp = snapshot_path() + ".tmp";
  Status written = write_file_durable(snapshot_tmp, snapshot_image);
  if (!written) {
    return written.error();
  }
  Status replaced = atomic_replace(snapshot_tmp, snapshot_path());
  if (!replaced) {
    return replaced.error();
  }

  ByteWriter journal_header;
  journal_header.u32(kJournalMagic);
  journal_header.u16(kControlPlaneFormatVersion);
  journal_header.u16(0);
  journal_header.u32(0);
  ByteWriter journal_image;
  journal_image.u32(kJournalMagic);
  journal_image.u16(kControlPlaneFormatVersion);
  journal_image.u16(0);
  journal_image.u32(crc32c(journal_header.data()));

  const std::string journal_tmp = journal_path() + ".tmp";
  Status journal_written = write_file_durable(journal_tmp, journal_image.data());
  if (!journal_written) {
    return journal_written.error();
  }
  return atomic_replace(journal_tmp, journal_path());
}

Status PersistenceStore::checkpoint(const ControlPlane& plane) {
  return write_checkpoint(encode_snapshot(plane));
}

}  // namespace ccsf::detail
