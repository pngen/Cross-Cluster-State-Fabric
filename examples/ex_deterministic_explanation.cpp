// Example: deterministic explanation of a decision.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  auto example = make_example("deterministic explanation");
  if (!example) {
    return 1;
  }
  if (!publish(*example, "explained-payload", 38ULL * 1024ULL * 1024ULL * 1024ULL)) {
    fail("state publication failed");
    return 1;
  }
  if (!seed(*example, example->cluster_a)) {
    fail("seeding failed");
    return 1;
  }

  ReplicationRequest request;
  request.state_id = example->state;
  request.state_generation = example->generation;
  request.destination_cluster = example->cluster_b;
  request.expected_coordinator_epoch = example->fabric->coordinator_epoch();

  auto first = example->fabric->explain_replication_plan(request);
  auto second = example->fabric->explain_replication_plan(request);
  if (!first || !second) {
    fail("explanation failed");
    return 1;
  }
  line("--- explanation ---");
  line(first.value().render());
  line("--- identical on repetition: " +
       std::string(first.value().render() == second.value().render() ? "yes" : "no") + " ---");

  auto reuse = example->fabric->explain_reuse(example->state, example->generation,
                                              example->cluster_a);
  if (reuse) {
    line("--- reuse explanation ---");
    line(reuse.value().render());
  }
  auto replica_set = example->fabric->explain_replica_set_state(example->state,
                                                                example->generation);
  if (replica_set) {
    line("--- replica set explanation ---");
    line(replica_set.value().render());
  }
  return 0;
}
