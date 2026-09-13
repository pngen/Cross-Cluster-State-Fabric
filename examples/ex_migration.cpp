// Example: migration with explicit ownership handoff.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  auto example = make_example("migration of single-writer state");
  if (!example) {
    return 1;
  }
  if (!publish(*example, "mutable-payload", 1ULL << 19U, ConsistencyMode::SINGLE_WRITER, 1)) {
    fail("state publication failed");
    return 1;
  }
  auto owner = seed(*example, example->cluster_a, true);
  if (!owner) {
    fail(owner.error().to_string());
    return 1;
  }
  line("cluster A holds the authoritative owner:");
  report_replica(*example->fabric, owner.value());

  MigrationRequest request;
  request.state_id = example->state;
  request.state_generation = example->generation;
  request.destination_cluster = example->cluster_b;
  request.retire_source_after_commit = true;
  request.expected_coordinator_epoch = example->fabric->coordinator_epoch();
  auto authorization = example->fabric->authorize_migration(request);
  if (!authorization) {
    fail(authorization.error().to_string());
    return 1;
  }
  const TransferAuthorization auth = authorization.value();
  line("migration authorized: operation " + auth.operation_id.str() + ", destructive move " +
       (auth.destructive_move ? "yes" : "no"));

  TransferCompletion completion;
  completion.replica_id = auth.destination_replica;
  completion.replica_generation = auth.destination_replica_generation;
  completion.transfer_id = auth.transfer_id;
  completion.transfer_generation = auth.transfer_generation;
  completion.operation_id = auth.operation_id;
  completion.operation_generation = auth.operation_generation;
  completion.cluster_id = auth.destination_cluster;
  completion.cluster_incarnation = auth.destination_incarnation;
  completion.expected_coordinator_epoch = example->fabric->coordinator_epoch();
  completion.bytes_transferred = 1ULL << 19U;
  completion.measured_digest = auth.expected_digest;
  if (!example->fabric->record_transfer_completion(completion)) {
    fail("transfer completion failed");
    return 1;
  }
  ReplicaVerification verification;
  verification.replica_id = auth.destination_replica;
  verification.replica_generation = auth.destination_replica_generation;
  verification.cluster_id = auth.destination_cluster;
  verification.cluster_incarnation = auth.destination_incarnation;
  verification.expected_coordinator_epoch = example->fabric->coordinator_epoch();
  verification.integrity_verified = true;
  verification.measured_digest = auth.expected_digest;
  if (!example->fabric->verify_replica(verification)) {
    fail("destination verification failed");
    return 1;
  }
  auto source_before = example->fabric->query_replica(auth.source_replica);
  line("source lifecycle before the commit: " + std::string(to_string(source_before.value().lifecycle)));

  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(example->take());
  commit.expected_coordinator_epoch = example->fabric->coordinator_epoch();
  commit.retire_source = true;
  auto migrated = example->fabric->commit_operation(commit);
  if (!migrated) {
    fail(migrated.error().to_string());
    return 1;
  }
  line("migration committed; source retired: " +
       std::string(migrated.value().source_retired ? "yes" : "no"));
  auto source_after = example->fabric->query_replica(auth.source_replica);
  line("source lifecycle after the commit: " +
       std::string(to_string(source_after.value().lifecycle)));
  line("historical provenance of the retired source is preserved: " +
       std::string(source_after.value().commit_recorded ? "yes" : "no"));

  auto reuse_source =
      example->fabric->evaluate_reuse(example->state, example->generation, example->cluster_a);
  line("reuse at the retired source: " + std::string(to_string(reuse_source.value().decision)));
  auto reuse_destination =
      example->fabric->evaluate_reuse(example->state, example->generation, example->cluster_b);
  line("reuse at the destination: " + std::string(to_string(reuse_destination.value().decision)));
  return 0;
}
