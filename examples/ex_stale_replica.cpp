// Example: a stale replica is rejected for reuse.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  auto example = make_example("stale replica rejection");
  if (!example) {
    return 1;
  }
  if (!publish(*example, "stale-payload", 1ULL << 20U)) {
    fail("state publication failed");
    return 1;
  }
  auto source = seed(*example, example->cluster_a);
  if (!source) {
    fail(source.error().to_string());
    return 1;
  }
  auto replicated = replicate(*example, example->cluster_b);
  if (!replicated) {
    fail(replicated.error().to_string());
    return 1;
  }

  auto cluster_b = example->fabric->query_cluster(example->cluster_b);
  if (!example->fabric->mark_cluster_unreachable(example->cluster_b, cluster_b.value().incarnation,
                                                 "example: link down")) {
    fail("marking the cluster unreachable failed");
    return 1;
  }
  line("cluster B is unreachable under incarnation " + cluster_b.value().incarnation.str());

  auto replica = example->fabric->query_replica(replicated.value());
  line("the B replica is now: " + std::string(to_string(replica.value().lifecycle)) +
       ", authority " + to_string(replica.value().authority) + ", counts " +
       (replica.value().counts_toward_factor ? "yes" : "no"));
  line("its committed history is preserved: " +
       std::string(replica.value().commit_recorded ? "yes" : "no"));

  auto reuse_b = example->fabric->evaluate_reuse(example->state, example->generation,
                                                 example->cluster_b);
  line("reuse in cluster B: " + std::string(to_string(reuse_b.value().decision)) + " (" +
       to_string(reuse_b.value().code) + ")");
  line("reason: " + reuse_b.value().reason);

  auto reuse_a = example->fabric->evaluate_reuse(example->state, example->generation,
                                                 example->cluster_a);
  line("reuse in cluster A: " + std::string(to_string(reuse_a.value().decision)));

  auto set = example->fabric->reconcile_replica_set(example->state, example->generation);
  line("replica set state: " + std::string(to_string(set.value().replica_set.state)) +
       ", authoritative " + std::to_string(set.value().replica_set.authoritative_count) +
       ", under-replicated " + (set.value().under_replicated ? "yes" : "no"));
  return 0;
}
