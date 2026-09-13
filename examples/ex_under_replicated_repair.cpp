// Example: repairing an under-replicated replica set.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  auto example = make_example("under-replicated repair");
  if (!example) {
    return 1;
  }
  if (!publish(*example, "repair-payload", 1ULL << 20U, ConsistencyMode::IMMUTABLE, 3)) {
    fail("state publication failed");
    return 1;
  }
  if (!seed(*example, example->cluster_a)) {
    fail("seeding failed");
    return 1;
  }
  auto first = example->fabric->reconcile_replica_set(example->state, example->generation);
  line("after one replica: authoritative " +
       std::to_string(first.value().replica_set.authoritative_count) + ", state " +
       to_string(first.value().replica_set.state) + ", shortfall " +
       std::to_string(first.value().authoritative_shortfall));

  auto explanation = example->fabric->explain_replica_set_state(example->state,
                                                                example->generation);
  if (explanation) {
    for (const std::string& text : explanation.value().lines) {
      line("  " + text);
    }
  }

  for (ClusterId destination : {example->cluster_b, example->cluster_c}) {
    auto repaired = replicate(*example, destination);
    if (!repaired) {
      fail(repaired.error().to_string());
      return 1;
    }
    auto now = example->fabric->reconcile_replica_set(example->state, example->generation);
    line("after repairing into cluster " + destination.str() + ": authoritative " +
         std::to_string(now.value().replica_set.authoritative_count) + ", state " +
         to_string(now.value().replica_set.state) + ", clusters " +
         std::to_string(now.value().replica_set.distinct_clusters));
  }
  auto finished = example->fabric->reconcile_replica_set(example->state, example->generation);
  line("under-replicated: " + std::string(finished.value().under_replicated ? "yes" : "no"));
  line("over-replicated: " + std::string(finished.value().over_replicated ? "yes" : "no"));
  return 0;
}
