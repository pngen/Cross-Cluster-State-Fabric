// Example: basic cross-cluster replication.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  auto example = make_example("basic cross-cluster replication");
  if (!example) {
    return 1;
  }
  if (!publish(*example, "replication-payload", 1ULL << 20U)) {
    fail("state publication failed");
    return 1;
  }
  auto source = seed(*example, example->cluster_a);
  if (!source) {
    fail(source.error().to_string());
    return 1;
  }
  line("cluster A holds the authoritative source:");
  report_replica(*example->fabric, source.value());

  auto before = example->fabric->reconcile_replica_set(example->state, example->generation);
  line("authoritative replicas before replication: " +
       std::to_string(before.value().replica_set.authoritative_count));

  auto destination = replicate(*example, example->cluster_b);
  if (!destination) {
    fail(destination.error().to_string());
    return 1;
  }
  line("cluster B replica after the transaction:");
  report_replica(*example->fabric, destination.value());

  auto after = example->fabric->reconcile_replica_set(example->state, example->generation);
  line("authoritative replicas after replication: " +
       std::to_string(after.value().replica_set.authoritative_count));
  line("replica set generation: " + after.value().replica_set.generation.str());
  line("distinct clusters: " + std::to_string(after.value().replica_set.distinct_clusters));

  auto reuse = example->fabric->evaluate_reuse(example->state, example->generation,
                                               example->cluster_b);
  line("reuse in cluster B: " + std::string(to_string(reuse.value().decision)) + " (" +
       to_string(reuse.value().code) + ")");
  return 0;
}
