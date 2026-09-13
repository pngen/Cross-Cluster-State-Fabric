// Example: cluster disappearance and reincarnation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  auto example = make_example("cluster reincarnation");
  if (!example) {
    return 1;
  }
  if (!publish(*example, "incarnation-payload", 1ULL << 20U)) {
    fail("state publication failed");
    return 1;
  }
  if (!seed(*example, example->cluster_a)) {
    fail("seeding failed");
    return 1;
  }
  auto replicated = replicate(*example, example->cluster_b);
  if (!replicated) {
    fail(replicated.error().to_string());
    return 1;
  }
  const ClusterIncarnationId previous = [&]() {
    auto cluster = example->fabric->query_cluster(example->cluster_b);
    return cluster.value().incarnation;
  }();
  line("cluster B incarnation before the restart: " + previous.str());

  ClusterRegistration restart = cluster_registration(example->cluster_b.value(), 1, 2,
                                                     example->take(), {"gpu:sm_120"});
  auto reincarnated = example->fabric->register_cluster(restart);
  if (!reincarnated) {
    fail(reincarnated.error().to_string());
    return 1;
  }
  line("cluster B incarnation after the restart: " + reincarnated.value().incarnation.str());
  line("reincarnations recorded: " + std::to_string(reincarnated.value().reincarnation_count));

  auto replica = example->fabric->query_replica(replicated.value());
  line("the durable copy still records the incarnation that wrote it: " +
       replica.value().location.cluster_incarnation.str());
  line("it counts toward the replication factor: " +
       std::string(replica.value().counts_toward_factor ? "yes" : "no"));

  auto reuse = example->fabric->evaluate_reuse(example->state, example->generation,
                                               example->cluster_b);
  line("reuse before revalidation: " + std::string(to_string(reuse.value().decision)) + " (" +
       to_string(reuse.value().code) + ")");

  ReplicaTransition revalidate;
  revalidate.replica_id = replicated.value();
  revalidate.expected_coordinator_epoch = example->fabric->coordinator_epoch();
  auto revalidated = example->fabric->revalidate_replica(revalidate);
  if (!revalidated) {
    fail(revalidated.error().to_string());
    return 1;
  }
  line("after explicit revalidation the replica is rebound to incarnation " +
       revalidated.value().location.cluster_incarnation.str());
  auto reuse_after = example->fabric->evaluate_reuse(example->state, example->generation,
                                                     example->cluster_b);
  line("reuse after revalidation: " + std::string(to_string(reuse_after.value().decision)));
  line("a returned cluster never reclaims authority merely because old bytes survived");
  return 0;
}
