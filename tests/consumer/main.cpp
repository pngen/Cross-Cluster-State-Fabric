// Cross-Cluster State Fabric - independent downstream consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is deliberately outside the project's own build: it is compiled
// against an installed package with find_package(CrossClusterStateFabric CONFIG)
// and links only the exported target.

#include <cstdio>
#include <memory>
#include <string>

#include "ccsf/fabric.hpp"
#include "ccsf/version.hpp"

int main() {
  using namespace ccsf;

  FabricConfig config;
  config.coordinator_id = CoordinatorId::from_value(1);
  config.home_cluster = ClusterId::from_value(10);
  config.epoch_start = CoordinatorEpoch::from_value(1);
  config.default_replication_policy = make_replication_policy(
      ReplicationPolicyId::from_value(1), ReplicationPolicyGeneration::from_value(1),
      ReplicationPolicyForm::N_REPLICAS, 2);

  auto fabric = Fabric::open(config);
  if (!fabric) {
    std::fprintf(stderr, "downstream consumer failed to open the runtime: %s\n",
                 fabric.error().to_string().c_str());
    return 1;
  }

  ClusterRegistration registration;
  registration.cluster_id = ClusterId::from_value(10);
  registration.region = RegionId::from_value(1);
  registration.site = SiteId::from_value(1);
  registration.failure_domain = FailureDomainId::from_value(1);
  registration.worker_boot_id = WorkerBootId::from_value(1);
  registration.health = ClusterHealthState::HEALTHY;
  registration.capabilities.storage_classes = {"nvme"};
  registration.capacity.total_logical_bytes = 1ULL << 40U;
  registration.capacity.inbound_bytes_per_tick = 1ULL << 28U;
  if (!fabric.value()->register_cluster(registration)) {
    std::fprintf(stderr, "downstream consumer could not register a cluster\n");
    return 1;
  }

  StateGenerationSpec spec;
  spec.state_id = StateId::from_value(42);
  spec.generation = StateGeneration::from_value(1);
  spec.lineage = LineageId::from_value(42);
  spec.logical_size = SizeValue::of(1024);
  spec.content_digest = sha256(std::string_view("downstream-consumer"));
  if (!fabric.value()->register_state(spec)) {
    std::fprintf(stderr, "downstream consumer could not register state\n");
    return 1;
  }
  if (!fabric.value()->commit_state_generation(StateId::from_value(42),
                                               StateGeneration::from_value(1),
                                               CommitId::from_value(1))) {
    std::fprintf(stderr, "downstream consumer could not commit state\n");
    return 1;
  }

  auto record = fabric.value()->current_state(StateId::from_value(42));
  if (!record || record.value().lifecycle != StateGenerationLifecycle::CURRENT) {
    std::fprintf(stderr, "downstream consumer observed an inconsistent state\n");
    return 1;
  }

  std::printf("downstream consumer ok: Cross-Cluster State Fabric %s, state %s generation %s is %s\n",
              version_string(), record.value().state_id.str().c_str(),
              record.value().generation.str().c_str(), to_string(record.value().lifecycle));
  return 0;
}
