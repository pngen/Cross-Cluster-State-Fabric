// Cross-Cluster State Fabric - coordinator process entry point.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <string>

#include "arguments.hpp"
#include "ccsf/coordinator.hpp"
#include "ccsf/taxonomy.hpp"

int main(int argc, char** argv) {
  using namespace ccsf;
  const app::Arguments arguments = app::parse(argc, argv);

  CoordinatorConfig config;
  config.bind_address = arguments.value("--bind", "127.0.0.1");
  config.port = static_cast<std::uint16_t>(arguments.number("--port", 0));
  config.coordinator_id = CoordinatorId::from_value(arguments.number("--coordinator-id", 1));
  config.home_cluster = ClusterId::from_value(arguments.number("--home-cluster", 0));
  config.persistence_directory = arguments.value("--persistence", std::string());
  config.require_durable_commits = !arguments.has("--in-memory");
  if (!config.persistence_directory.empty() && arguments.has("--ephemeral")) {
    config.persistence_directory.clear();
  }
  config.placement_policy.placement_policy_id =
      PlacementPolicyId::from_value(arguments.number("--placement-policy-id", 1));
  config.placement_policy.generation = PlacementPolicyGeneration::from_value(1);
  config.placement_policy.cost_policy.source = CostSource::POLICY;
  config.placement_policy.cost_policy.transfer_micro_per_mib = 5000;
  config.placement_policy.cost_policy.compute_micro_per_mib = 200000;
  config.placement_policy.cost_policy.egress_micro_per_mib = 1000;
  config.placement_policy.weights.expected_reuse_value = 3;
  config.placement_policy.weights.transfer_cost = -4;
  config.placement_policy.weights.destination_health = 5;
  config.placement_policy.weights.failure_domain_diversity = 4;
  config.placement_policy.weights.available_bandwidth = 1;

  const std::uint32_t replica_count =
      static_cast<std::uint32_t>(arguments.number("--replication-factor", 2));
  config.replication_policy = make_replication_policy(
      ReplicationPolicyId::from_value(arguments.number("--replication-policy-id", 1)),
      ReplicationPolicyGeneration::from_value(1), ReplicationPolicyForm::N_REPLICAS,
      replica_count);
  config.replication_policy.retention.maximum_replicas_per_state = 8;
  config.replication_policy.destination_rules.maximum_in_flight_operations_per_state = 4;
  app::TraceHandle trace =
      std::make_shared<app::TraceFile>(arguments.value("--log", std::string()));
  config.trace = app::trace_callback(trace);

  auto coordinator = Coordinator::start(config);
  if (!coordinator) {
    std::fprintf(stderr, "coordinator failed to start: %s\n",
                 coordinator.error().to_string().c_str());
    std::fflush(stderr);
    return 2;
  }
  const FabricStatus status = coordinator.value()->status();
  std::printf("COORDINATOR READY port=%u coordinator=%s epoch=%s persistence=%s\n",
              static_cast<unsigned>(coordinator.value()->port()),
              status.coordinator_id.str().c_str(), status.coordinator_epoch.str().c_str(),
              status.persistence_enabled ? "enabled" : "disabled");
  std::fflush(stdout);

  const Result<void> served = coordinator.value()->serve();
  std::printf("COORDINATOR STOPPED epoch=%s\n",
              coordinator.value()->epoch().str().c_str());
  std::fflush(stdout);
  if (!served) {
    std::fprintf(stderr, "coordinator stopped with error: %s\n",
                 served.error().to_string().c_str());
    std::fflush(stderr);
    return 1;
  }
  return 0;
}
