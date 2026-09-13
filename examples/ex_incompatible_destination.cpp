// Example: an incompatible destination is refused.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  auto example = make_example("incompatible destination rejection");
  if (!example) {
    return 1;
  }
  CompatibilityDescriptor required;
  required.set("accelerator", "sm_120");
  required.set("runtime_abi", "ccsf-1");
  if (!publish(*example, "compat-payload", 1ULL << 20U, ConsistencyMode::IMMUTABLE, 2, required)) {
    fail("state publication failed");
    return 1;
  }
  auto source = seed(*example, example->cluster_a);
  if (!source) {
    fail(source.error().to_string());
    return 1;
  }

  // Cluster C advertises a different accelerator generation.
  ClusterEvidenceUpdate update;
  auto cluster_c = example->fabric->query_cluster(example->cluster_c);
  update.cluster_id = example->cluster_c;
  update.incarnation = cluster_c.value().incarnation;
  update.expected_coordinator_epoch = example->fabric->coordinator_epoch();
  update.has_capabilities = true;
  update.capabilities.tags = {"gpu:sm_90", "compat:runtime_abi=ccsf-1"};
  update.capabilities.accelerator_architectures = {"sm_90"};
  update.capabilities.storage_classes = {"nvme"};
  if (!example->fabric->publish_cluster_evidence(update)) {
    fail("evidence publication failed");
    return 1;
  }
  line("cluster C now reports accelerator sm_90 while the state requires sm_120");

  ReplicationRequest request;
  request.state_id = example->state;
  request.state_generation = example->generation;
  request.destination_cluster = example->cluster_c;
  request.expected_coordinator_epoch = example->fabric->coordinator_epoch();
  auto authorization = example->fabric->authorize_replication(request);
  if (authorization) {
    fail("an incompatible destination was authorized");
    return 1;
  }
  line("authorization refused: " + authorization.error().to_string());

  auto plan = example->fabric->plan_replication(request);
  if (plan) {
    line("plan decision: " + std::string(to_string(plan.value().decision)));
    for (const PlacementRejection& rejection : plan.value().rejected) {
      line("  cluster " + rejection.cluster_id.str() + " rejected by " + rejection.constraint +
           " (" + to_string(rejection.code) + "): " + rejection.detail);
    }
  }

  // A cluster that does not publish the required attribute at all is UNKNOWN,
  // and UNKNOWN is never treated as compatible.
  auto reuse = example->fabric->evaluate_reuse(example->state, example->generation,
                                               example->cluster_c);
  line("reuse in cluster C: " + std::string(to_string(reuse.value().decision)) + " (" +
       to_string(reuse.value().code) + ")");
  return 0;
}
