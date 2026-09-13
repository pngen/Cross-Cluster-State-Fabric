// Cross-Cluster State Fabric - cluster agent process entry point.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <string>
#include <vector>

#include "arguments.hpp"
#include "ccsf/cluster_agent.hpp"

int main(int argc, char** argv) {
  using namespace ccsf;
  const app::Arguments arguments = app::parse(argc, argv);

  ClusterAgentConfig config;
  config.coordinator_host = arguments.value("--coordinator-host", "127.0.0.1");
  config.coordinator_port =
      static_cast<std::uint16_t>(arguments.number("--coordinator-port", 0));
  config.data_bind_address = arguments.value("--data-bind", "127.0.0.1");
  config.cluster_id = ClusterId::from_value(arguments.number("--cluster", 0));
  config.region = RegionId::from_value(arguments.number("--region", 1));
  config.site = SiteId::from_value(arguments.number("--site", 1));
  config.failure_domain = FailureDomainId::from_value(arguments.number("--failure-domain", 1));
  config.location_descriptor = arguments.value("--descriptor", "reference-site");
  config.capabilities.tags = arguments.repeated("--tag");
  config.capabilities.storage_classes = arguments.repeated("--storage");
  config.capabilities.accelerator_architectures = arguments.repeated("--accelerator");
  config.capabilities.encryption_at_rest = arguments.has("--encryption-at-rest");
  config.capabilities.attestation = arguments.has("--attestation");
  config.capacity.total_logical_bytes = arguments.number("--capacity-bytes", 1ULL << 40U);
  config.capacity.used_logical_bytes = arguments.number("--used-bytes", 1ULL << 30U);
  config.capacity.inbound_bytes_per_tick = arguments.number("--inbound-bytes", 1ULL << 28U);
  config.capacity.free_transfer_slots =
      static_cast<std::uint32_t>(arguments.number("--transfer-slots", 8));
  config.capacity.utilization_per_mille =
      static_cast<std::uint32_t>(arguments.number("--utilization-per-mille", 100));
  app::TraceHandle trace =
      std::make_shared<app::TraceFile>(arguments.value("--log", std::string()));
  config.trace = app::trace_callback(trace);

  if (!config.cluster_id.valid()) {
    std::fprintf(stderr, "a cluster identity is required\n");
    std::fflush(stderr);
    return 2;
  }

  auto agent = ClusterAgent::start(config);
  if (!agent) {
    std::fprintf(stderr, "cluster agent failed to start: %s\n",
                 agent.error().to_string().c_str());
    std::fflush(stderr);
    return 2;
  }
  std::printf("AGENT READY cluster=%s incarnation=%s worker_boot=%s data_port=%u\n",
              config.cluster_id.str().c_str(), agent.value()->incarnation().str().c_str(),
              agent.value()->worker_boot_id().str().c_str(),
              static_cast<unsigned>(agent.value()->data_port()));
  std::fflush(stdout);

  const Result<void> served = agent.value()->serve();
  std::printf("AGENT STOPPED cluster=%s replicas=%zu connections=%llu\n",
              config.cluster_id.str().c_str(), agent.value()->store().count(),
              static_cast<unsigned long long>(agent.value()->data_connections()));
  std::fflush(stdout);
  if (!served) {
    std::fprintf(stderr, "cluster agent stopped with error: %s\n",
                 served.error().to_string().c_str());
    std::fflush(stderr);
    return 1;
  }
  return 0;
}
