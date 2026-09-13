// Example: coordinator restart with a durable control plane.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <filesystem>

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  banner("coordinator restart");

  std::error_code code;
  const std::filesystem::path directory =
      std::filesystem::absolute(std::filesystem::path("runtime/example-restart"));
  std::filesystem::remove_all(directory, code);
  std::filesystem::create_directories(directory, code);

  const StateId state = StateId::from_value(8801);
  const StateGeneration generation = StateGeneration::from_value(1);
  CoordinatorEpoch first_epoch;

  {
    FabricConfig config;
    config.coordinator_id = CoordinatorId::from_value(1);
    config.home_cluster = ClusterId::from_value(100);
    config.epoch_start = CoordinatorEpoch::from_value(1);
    config.persistence_directory = directory.string();
    config.require_durable_commits = true;
    config.default_placement_policy = placement_policy(1);
    config.default_replication_policy = replication_policy(1, 2);
    auto fabric = Fabric::open(config);
    if (!fabric) {
      fail(fabric.error().to_string());
      return 1;
    }
    for (const auto& registration : {cluster_registration(100, 1, 1, 11, {"gpu:sm_120"}),
                                     cluster_registration(101, 1, 2, 12, {"gpu:sm_120"})}) {
      if (!fabric.value()->register_cluster(registration)) {
        fail("cluster registration failed");
        return 1;
      }
    }
    Example example;
    example.fabric = std::move(fabric.value());
    example.state = state;
    example.generation = generation;
    if (!publish(example, "durable-payload", 1ULL << 20U)) {
      fail("state publication failed");
      return 1;
    }
    if (!seed(example, ClusterId::from_value(100))) {
      fail("seeding failed");
      return 1;
    }
    if (!replicate(example, ClusterId::from_value(101))) {
      fail("replication failed");
      return 1;
    }
    first_epoch = example.fabric->coordinator_epoch();
    auto set = example.fabric->reconcile_replica_set(state, generation);
    line("before the restart: epoch " + first_epoch.str() + ", authoritative replicas " +
         std::to_string(set.value().replica_set.authoritative_count));
    if (!example.fabric->persist()) {
      fail("checkpoint failed");
      return 1;
    }
  }

  FabricConfig config;
  config.coordinator_id = CoordinatorId::from_value(1);
  config.home_cluster = ClusterId::from_value(100);
  config.epoch_start = CoordinatorEpoch::from_value(1);
  config.persistence_directory = directory.string();
  config.require_durable_commits = true;
  config.default_placement_policy = placement_policy(1);
  config.default_replication_policy = replication_policy(1, 2);
  auto reopened = Fabric::open(config);
  if (!reopened) {
    fail(reopened.error().to_string());
    return 1;
  }
  Fabric& fabric = *reopened.value();
  line("after the restart: epoch " + fabric.coordinator_epoch().str() + " (was " +
       first_epoch.str() + ")");
  line("the epoch advances so that every message minted before the restart is fenced");

  auto record = fabric.query_state(state, generation);
  line("state generation survived: " + std::string(record ? "yes" : "no"));
  auto set = fabric.reconcile_replica_set(state, generation);
  line("authoritative replicas survived: " +
       std::to_string(set.value().replica_set.authoritative_count));
  line("the recovered replica set requires revalidation: " +
       std::string(set.value().replica_set.revalidation_required ? "yes" : "no"));

  auto reuse = fabric.evaluate_reuse(state, generation, ClusterId::from_value(100));
  line("reuse immediately after the restart: " + std::string(to_string(reuse.value().decision)) +
       " (" + to_string(reuse.value().code) + ")");

  ReplicaTransition revalidate;
  revalidate.replica_id = set.value().replica_set.authoritative_members.empty()
                              ? ReplicaId{}
                              : set.value().replica_set.authoritative_members.front().replica_id;
  revalidate.expected_coordinator_epoch = fabric.coordinator_epoch();
  auto revalidated = fabric.revalidate_replica(revalidate);
  line("revalidation after fresh evidence: " +
       std::string(revalidated ? "restored authority" : revalidated.error().to_string()));
  auto after = fabric.evaluate_reuse(state, generation, ClusterId::from_value(100));
  line("reuse after revalidation: " + std::string(to_string(after.value().decision)));
  std::filesystem::remove_all(directory, code);
  return 0;
}
