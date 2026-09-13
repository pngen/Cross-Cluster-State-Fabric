// Cross-Cluster State Fabric - durable control plane proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <string>
#include <vector>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

namespace {

FabricConfig durable_config(CoordinatorId coordinator, ClusterId home,
                            const std::string& directory) {
  FabricConfig config = base_config(coordinator, home);
  config.persistence_directory = directory;
  config.require_durable_commits = true;
  return config;
}

}  // namespace

CCSF_CASE(persistence, committed_state_survives_a_restart) {
  const std::string directory = fresh_directory("persist-restart");
  const CoordinatorId coordinator = CoordinatorId::from_value(50);
  const ClusterId home = ClusterId::from_value(100);
  const StateId state = StateId::from_value(5001);
  const StateGeneration generation = StateGeneration::from_value(1);
  CoordinatorEpoch first_epoch;

  {
    auto world = make_world(durable_config(coordinator, home, directory));
    ctx.check(world != nullptr, "the durable world opens");
    world->state = state;
    world->generation = generation;
    ctx.check(publish_state(*world, "durable-payload", 4096, ConsistencyMode::IMMUTABLE, 2)
                  .has_value(),
              "state committed");
    ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
              "cluster A seeded");
    ctx.check(replicate(*world, world->cluster_b).has_value(), "cluster B replicated");
    first_epoch = world->epoch();

    auto reconciliation = world->fabric->reconcile_replica_set(state, generation);
    ctx.check(reconciliation.value().replica_set.authoritative_count == 2,
              "two authoritative replicas before the restart");

    const std::uint64_t journal_before = file_bytes(journal_file(directory));
    ctx.check(journal_before > 0, "journal records exist on disk before the restart");
  }

  // Reopen the control plane directly: this observes the recovered state before
  // any cluster re-publishes evidence.
  auto reopened = Fabric::open(durable_config(coordinator, home, directory));
  ctx.check(reopened.has_value(), "the durable control plane reopens");
  Fabric& fabric = *reopened.value();
  ctx.check(fabric.coordinator_epoch() > first_epoch,
            "the coordinator epoch advances across a restart so old traffic is fenced");

  auto record = fabric.query_state(state, generation);
  ctx.check(record.has_value(), "the committed state generation survived");
  ctx.check(record.value().lifecycle == StateGenerationLifecycle::CURRENT,
            "the generation is still current");
  ctx.check(record.value().content_digest == digest_of("durable-payload"),
            "the content identity survived intact");

  auto reconciliation = fabric.reconcile_replica_set(state, generation);
  ctx.check(reconciliation.has_value(), "the replica set is readable after recovery");
  ctx.check(reconciliation.value().replica_set.authoritative_count == 2,
            "committed replica set membership survives the restart");
  ctx.check(reconciliation.value().replica_set.revalidation_required,
            "the recovered replica set is explicitly marked for revalidation");

  auto clusters = fabric.list_clusters();
  ctx.check(clusters.has_value() && clusters.value().size() == 3, "all clusters survived");
  for (const ClusterRecord& cluster : clusters.value()) {
    ctx.check(cluster.evidence.revalidation_required,
              "recovered cluster evidence is conservatively marked for revalidation");
    ctx.check(!cluster.evidence.present, "recovered dynamic evidence is not treated as current");
  }

  auto reuse = fabric.evaluate_reuse(state, generation, clusters.value().front().cluster_id);
  ctx.check(reuse.value().decision == DecisionKind::REJECT,
            "live reuse authority is withheld until evidence is revalidated");
  ctx.check(reuse.value().code == ErrorCode::REVALIDATION_REQUIRED,
            "the rejection names revalidation");
}

CCSF_CASE(persistence, journal_replay_reconstructs_state) {
  const std::string directory = fresh_directory("persist-journal");
  const CoordinatorId coordinator = CoordinatorId::from_value(51);
  const ClusterId home = ClusterId::from_value(100);
  const StateId state = StateId::from_value(5002);
  const StateGeneration generation = StateGeneration::from_value(1);

  {
    auto world = make_world(durable_config(coordinator, home, directory));
    ctx.check(world != nullptr, "world opens");
    world->state = state;
    world->generation = generation;
    ctx.check(publish_state(*world, "journal-payload", 2048, ConsistencyMode::IMMUTABLE, 2)
                  .has_value(),
              "state committed");
    ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
              "seeded");
    // No explicit checkpoint: the journal alone must reconstruct the state.
  }

  auto world = make_world(durable_config(coordinator, home, directory));
  ctx.check(world != nullptr, "the journal alone reopens the control plane");
  world->state = state;
  world->generation = generation;
  auto reconciliation = world->fabric->reconcile_replica_set(state, generation);
  ctx.check(reconciliation.has_value(), "the replica set is reconstructed from the journal");
  ctx.check(reconciliation.value().replica_set.authoritative_count == 1,
            "the authoritative replica is reconstructed from the journal");
}

CCSF_CASE(persistence, mutation_is_durable_before_acknowledgement) {
  const std::string directory = fresh_directory("persist-ack");
  auto world = make_world(durable_config(CoordinatorId::from_value(52), ClusterId::from_value(100),
                                         directory));
  ctx.check(world != nullptr, "world opens");
  ctx.check(publish_state(*world, "ack-payload", 1024, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(file_bytes(journal_file(directory)) > 12,
            "the journal already holds records while the runtime is live, so a durable mutation "
            "crossed the persistence boundary before it was acknowledged");
}

CCSF_CASE(persistence, corrupt_snapshot_is_rejected) {
  const std::string directory = fresh_directory("persist-corrupt");
  const CoordinatorId coordinator = CoordinatorId::from_value(53);
  const ClusterId home = ClusterId::from_value(100);
  {
    auto world = make_world(
        durable_config(coordinator, home, directory));
    ctx.check(world != nullptr, "world opens");
    ctx.check(publish_state(*world, "corrupt-payload", 1024).has_value(), "state committed");
    ctx.check(world->fabric->persist().has_value(), "a checkpoint is written");
  }

  const std::string snapshot = snapshot_file(directory);
  std::vector<std::byte> bytes = read_all_bytes(snapshot);
  ctx.check(bytes.size() > 40, "the snapshot has a payload to corrupt");
  bytes[bytes.size() / 2U] = static_cast<std::byte>(
      std::to_integer<std::uint8_t>(bytes[bytes.size() / 2U]) ^ 0x5AU);
  write_all_bytes(snapshot, bytes);

  auto reopened = make_world(durable_config(coordinator, home, directory));
  ctx.check(reopened == nullptr, "a corrupted snapshot must not load");
  auto direct = Fabric::open(durable_config(coordinator, home, directory));
  ctx.check(!direct.has_value(), "opening a corrupted control plane fails");
  ctx.check(direct.error().code == ErrorCode::PERSISTENCE_CORRUPT,
            "reported as PERSISTENCE_CORRUPT");
}

CCSF_CASE(persistence, truncated_and_extended_snapshots_are_rejected) {
  const std::string directory = fresh_directory("persist-truncated");
  const CoordinatorId coordinator = CoordinatorId::from_value(54);
  const ClusterId home = ClusterId::from_value(100);
  {
    auto world = make_world(durable_config(coordinator, home, directory));
    ctx.check(world != nullptr, "world opens");
    ctx.check(publish_state(*world, "truncate-payload", 1024).has_value(), "state committed");
    ctx.check(world->fabric->persist().has_value(), "checkpoint written");
  }
  const std::string snapshot = snapshot_file(directory);
  const std::vector<std::byte> original = read_all_bytes(snapshot);
  ctx.check(original.size() > 64, "snapshot is large enough to truncate");

  write_all_bytes(snapshot, std::vector<std::byte>(original.begin(), original.end() - 5));
  auto truncated = Fabric::open(durable_config(coordinator, home, directory));
  ctx.check(!truncated.has_value(), "a truncated snapshot is rejected");
  ctx.check(truncated.error().code == ErrorCode::PERSISTENCE_CORRUPT ||
                truncated.error().code == ErrorCode::PERSISTENCE_TRUNCATED,
            "reported as corruption or truncation");

  std::vector<std::byte> extended = original;
  for (int index = 0; index < 3; ++index) {
    extended.push_back(std::byte{0x41});
  }
  write_all_bytes(snapshot, extended);
  auto trailing = Fabric::open(durable_config(coordinator, home, directory));
  ctx.check(!trailing.has_value(), "trailing bytes after the declared payload are rejected");
  ctx.check(trailing.error().code == ErrorCode::PERSISTENCE_CORRUPT,
            "reported as corruption");
}

CCSF_CASE(persistence, unsupported_version_is_rejected) {
  const std::string directory = fresh_directory("persist-version");
  const CoordinatorId coordinator = CoordinatorId::from_value(55);
  const ClusterId home = ClusterId::from_value(100);
  {
    auto world = make_world(durable_config(coordinator, home, directory));
    ctx.check(world != nullptr, "world opens");
    ctx.check(publish_state(*world, "version-payload", 1024).has_value(), "state committed");
    ctx.check(world->fabric->persist().has_value(), "checkpoint written");
  }
  const std::string snapshot = snapshot_file(directory);
  std::vector<std::byte> bytes = read_all_bytes(snapshot);
  const std::uint16_t version = read_u16_be(bytes, 4);
  ctx.check(version == 1, "the snapshot declares format version one");
  bytes[4] = std::byte{0x00};
  bytes[5] = std::byte{0x09};
  write_all_bytes(snapshot, bytes);

  auto rejected = Fabric::open(durable_config(coordinator, home, directory));
  ctx.check(!rejected.has_value(), "an unknown format version is rejected");
  ctx.check(rejected.error().code == ErrorCode::PERSISTENCE_UNSUPPORTED_VERSION,
            "reported as PERSISTENCE_UNSUPPORTED_VERSION");
}

CCSF_CASE(persistence, corrupt_journal_record_is_rejected) {
  const std::string directory = fresh_directory("persist-journal-corrupt");
  const CoordinatorId coordinator = CoordinatorId::from_value(56);
  const ClusterId home = ClusterId::from_value(100);
  {
    auto world = make_world(durable_config(coordinator, home, directory));
    ctx.check(world != nullptr, "world opens");
    ctx.check(publish_state(*world, "journal-corrupt", 1024).has_value(), "state committed");
  }
  const std::string journal = journal_file(directory);
  std::vector<std::byte> bytes = read_all_bytes(journal);
  ctx.check(bytes.size() > 24, "the journal holds a record");
  bytes[bytes.size() - 1U] = static_cast<std::byte>(
      std::to_integer<std::uint8_t>(bytes[bytes.size() - 1U]) ^ 0xFFU);
  write_all_bytes(journal, bytes);

  auto rejected = Fabric::open(durable_config(coordinator, home, directory));
  ctx.check(!rejected.has_value(), "a corrupted journal record is rejected");
  ctx.check(rejected.error().code == ErrorCode::PERSISTENCE_CORRUPT,
            "reported as PERSISTENCE_CORRUPT");
}

CCSF_CASE(persistence, truncated_journal_tail_is_discarded_as_unacknowledged) {
  const std::string directory = fresh_directory("persist-journal-tail");
  const CoordinatorId coordinator = CoordinatorId::from_value(57);
  const ClusterId home = ClusterId::from_value(100);
  const StateId state = StateId::from_value(5003);
  const StateGeneration generation = StateGeneration::from_value(1);
  {
    auto world = make_world(durable_config(coordinator, home, directory));
    ctx.check(world != nullptr, "world opens");
    world->state = state;
    world->generation = generation;
    ctx.check(publish_state(*world, "tail-payload", 1024, ConsistencyMode::IMMUTABLE, 2)
                  .has_value(),
              "state committed");
  }
  const std::string journal = journal_file(directory);
  const std::uint64_t complete = file_bytes(journal);
  const std::vector<std::byte> partial{std::byte{0x52}, std::byte{0x45}, std::byte{0x43}};
  append_bytes(journal, partial);

  ctx.check(complete > 12U, "the journal held acknowledged records before the tail was appended");
  auto reopened = Fabric::open(durable_config(coordinator, home, directory));
  ctx.check(reopened.has_value(), "a truncated tail record is tolerated");
  const FabricStatus status = reopened.value()->status();
  ctx.check(status.recovered_from_durable_state, "the runtime reports that it recovered");
  ctx.check(status.journal_tail_discarded,
            "the runtime reports that an unacknowledged journal tail was discarded");
  ctx.check(file_bytes(journal) == 12U,
            "recovery rewrote the journal from the recovered control plane");
  auto record = reopened.value()->query_state(state, generation);
  ctx.check(record.has_value(), "the acknowledged records were all replayed");
}

CCSF_CASE(persistence, ambiguous_in_flight_work_is_recovered_conservatively) {
  const std::string directory = fresh_directory("persist-recovery");
  const CoordinatorId coordinator = CoordinatorId::from_value(58);
  const ClusterId home = ClusterId::from_value(100);
  const StateId state = StateId::from_value(5004);
  const StateGeneration generation = StateGeneration::from_value(1);
  OperationId in_flight;

  {
    auto world = make_world(durable_config(coordinator, home, directory));
    ctx.check(world != nullptr, "world opens");
    world->state = state;
    world->generation = generation;
    ctx.check(publish_state(*world, "recovery-payload", 4096, ConsistencyMode::IMMUTABLE, 2)
                  .has_value(),
              "state committed");
    ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
              "seeded");

    ReplicationRequest request;
    request.state_id = state;
    request.state_generation = generation;
    request.destination_cluster = world->cluster_b;
    request.expected_coordinator_epoch = world->epoch();
    auto authorization = world->fabric->authorize_replication(request);
    ctx.check(authorization.has_value(), "a transfer is authorized");
    in_flight = authorization.value().operation_id;

    TransferProgress progress;
    progress.transfer_id = authorization.value().transfer_id;
    progress.transfer_generation = authorization.value().transfer_generation;
    progress.replica_id = authorization.value().destination_replica;
    progress.replica_generation = authorization.value().destination_replica_generation;
    progress.operation_id = authorization.value().operation_id;
    progress.operation_generation = authorization.value().operation_generation;
    progress.cluster_id = authorization.value().destination_cluster;
    progress.cluster_incarnation = authorization.value().destination_incarnation;
    progress.expected_coordinator_epoch = world->epoch();
    progress.bytes_transferred = 2048;
    ctx.check(world->fabric->record_transfer_progress(progress).has_value(),
              "the transfer is in flight when the coordinator stops");
  }

  auto world = make_world(durable_config(coordinator, home, directory));
  ctx.check(world != nullptr, "the coordinator restarts");
  auto operation = world->fabric->query_operation(in_flight);
  ctx.check(operation.has_value(), "the in-flight operation survived as history");
  ctx.check(operation.value().state == OperationState::ABANDONED ||
                operation.value().state == OperationState::OUTCOME_UNKNOWN,
            "an ambiguous in-flight operation is classified honestly");
  ctx.check(!operation.value().destination_committed,
            "an ambiguous operation never claims a committed destination");

  world->state = state;
  world->generation = generation;
  auto reconciliation = world->fabric->reconcile_replica_set(state, generation);
  ctx.check(reconciliation.value().replica_set.authoritative_count == 1,
            "the ambiguous destination does not count toward the replication factor");
}
