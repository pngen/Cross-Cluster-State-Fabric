// Cross-Cluster State Fabric - placement and economics proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <utility>
#include <vector>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

namespace {

PlacementCandidate candidate_for(const World& world, ClusterId cluster, RegionId region,
                                 FailureDomainId domain, ClusterHealthState health) {
  PlacementCandidate candidate;
  candidate.cluster_id = cluster;
  candidate.cluster_incarnation = ClusterIncarnationId::from_value(cluster.value() * 10U);
  candidate.region = region;
  candidate.failure_domain = domain;
  candidate.health = health;
  candidate.capabilities.tags = {"gpu:sm_120"};
  candidate.capabilities.storage_classes = {"nvme"};
  candidate.capacity.total_logical_bytes = 1ULL << 40U;
  candidate.capacity.used_logical_bytes = 1ULL << 30U;
  candidate.capacity.inbound_bytes_per_tick = 1ULL << 28U;
  candidate.capacity.utilization_per_mille = 100;
  candidate.evidence.present = true;
  candidate.evidence.published_at = world.now();
  candidate.compatibility = CompatibilityResult::COMPATIBLE;
  candidate.integrity_evidence_available = true;
  candidate.logical_size = SizeValue::of(1ULL << 30U);
  candidate.transfer_bytes = SizeValue::of(1ULL << 30U);
  candidate.achievable_durability = ReplicaDurabilityState::DURABLE;
  candidate.expected_reuse = ReuseValue::estimated(10U, 900U);
  candidate.source_cluster_healthy = true;
  return candidate;
}

}  // namespace

CCSF_CASE(placement, hard_constraints_dominate_ranking) {
  auto world = make_world(base_config(CoordinatorId::from_value(40), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  StateGenerationRecord state;
  state.state_id = world->state;
  state.generation = world->generation;
  state.logical_size = SizeValue::of(1ULL << 30U);
  state.durability = DurabilityClass::DURABLE;
  state.registered_at = world->now();

  PlacementRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.policy = default_placement_policy(world->ids.take());
  request.policy.constraints.prohibited_clusters.push_back(world->cluster_c);

  PlacementCandidate preferred =
      candidate_for(*world, world->cluster_c, world->region_two, FailureDomainId::from_value(3),
                    ClusterHealthState::HEALTHY);
  preferred.capacity.inbound_bytes_per_tick = 1ULL << 32U;
  preferred.affinity_score = 1000000;
  preferred.expected_reuse = ReuseValue::estimated(1000U, 1000U);

  PlacementCandidate fallback =
      candidate_for(*world, world->cluster_b, world->region_one, FailureDomainId::from_value(2),
                    ClusterHealthState::HEALTHY);
  fallback.capacity.inbound_bytes_per_tick = 1ULL << 20U;
  fallback.affinity_score = 0;
  fallback.expected_reuse = ReuseValue::estimated(1U, 100U);

  request.candidates = {preferred, fallback};
  const PlacementEvaluation evaluation = evaluate_placement(request, &state);
  ctx.check(evaluation.chosen_cluster == world->cluster_b,
            "a candidate that fails a hard constraint can never win on score");
  ctx.check(!evaluation.rejected.empty(), "the rejected candidate is reported");
  ctx.check(evaluation.rejected.front().constraint == "prohibited_cluster",
            "the rejection names the violated constraint");
}

CCSF_CASE(placement, deterministic_decision_and_stable_tie_break) {
  auto world = make_world(base_config(CoordinatorId::from_value(41), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  StateGenerationRecord state;
  state.state_id = world->state;
  state.generation = world->generation;
  state.logical_size = SizeValue::of(1ULL << 30U);
  state.registered_at = world->now();

  PlacementRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.policy = default_placement_policy(world->ids.take());
  request.candidates = {
      candidate_for(*world, world->cluster_b, world->region_one, FailureDomainId::from_value(2),
                    ClusterHealthState::HEALTHY),
      candidate_for(*world, world->cluster_c, world->region_two, FailureDomainId::from_value(3),
                    ClusterHealthState::HEALTHY)};

  const PlacementEvaluation first = evaluate_placement(request, &state);
  PlacementRequest reversed = request;
  std::swap(reversed.candidates[0], reversed.candidates[1]);
  const PlacementEvaluation second = evaluate_placement(reversed, &state);

  ctx.check(first.chosen_cluster == second.chosen_cluster,
            "candidate ordering must not change the decision");
  ctx.check(first.chosen_score == second.chosen_score, "the score is identical");
  ctx.check(first.reasons == second.reasons, "the explanation is byte-for-byte identical");
  ctx.check(first.chosen_cluster == world->cluster_b,
            "an exact tie is broken by the lower cluster identity");

  const PlacementEvaluation third = evaluate_placement(request, &state);
  ctx.check(third.chosen_cluster == first.chosen_cluster,
            "repeating the same canonical inputs yields the same decision");
}

CCSF_CASE(placement, unknown_cost_never_looks_best) {
  auto world = make_world(base_config(CoordinatorId::from_value(42), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  StateGenerationRecord state;
  state.state_id = world->state;
  state.generation = world->generation;
  state.logical_size = SizeValue::of(1ULL << 30U);
  state.registered_at = world->now();

  PlacementRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.policy = default_placement_policy(world->ids.take());
  request.policy.weights = RankingWeights{};
  request.policy.weights.transfer_cost = -1;

  PlacementCandidate priced =
      candidate_for(*world, world->cluster_c, world->region_two, FailureDomainId::from_value(3),
                    ClusterHealthState::HEALTHY);
  priced.transfer_cost = CostValue::from_policy(500000);
  PlacementCandidate unpriced =
      candidate_for(*world, world->cluster_b, world->region_one, FailureDomainId::from_value(2),
                    ClusterHealthState::HEALTHY);
  unpriced.transfer_cost = CostValue::unknown();

  request.candidates = {unpriced, priced};
  const PlacementEvaluation evaluation = evaluate_placement(request, &state);
  ctx.check(evaluation.chosen_cluster == world->cluster_c,
            "an unknown cost is scored pessimistically and loses to a known cost");
  bool saw_unknown = false;
  for (const RankingContribution& contribution : evaluation.ranked.front().contributions) {
    if (contribution.factor == RankingFactor::TRANSFER_COST && contribution.unknown) {
      saw_unknown = true;
    }
  }
  ctx.check(!saw_unknown, "the chosen candidate has no unknown cost evidence");
}

CCSF_CASE(placement, diversity_floor_requires_a_new_domain) {
  auto world = make_world(base_config(CoordinatorId::from_value(43), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  StateGenerationRecord state;
  state.state_id = world->state;
  state.generation = world->generation;
  state.logical_size = SizeValue::of(1ULL << 30U);
  state.registered_at = world->now();

  PlacementRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.policy = default_placement_policy(world->ids.take());
  request.policy.constraints.minimum_region_diversity = 2;
  request.current_regions = {world->region_one};
  request.current_clusters = {world->cluster_a};
  request.current_failure_domains = {FailureDomainId::from_value(1)};
  request.candidates = {
      candidate_for(*world, world->cluster_b, world->region_one, FailureDomainId::from_value(2),
                    ClusterHealthState::HEALTHY),
      candidate_for(*world, world->cluster_c, world->region_two, FailureDomainId::from_value(3),
                    ClusterHealthState::HEALTHY)};

  const PlacementEvaluation evaluation = evaluate_placement(request, &state);
  ctx.check(evaluation.chosen_cluster == world->cluster_c,
            "with a region diversity floor only a new region is eligible");
  bool rejected_for_diversity = false;
  for (const PlacementRejection& rejection : evaluation.rejected) {
    if (rejection.constraint == "region_diversity") {
      rejected_for_diversity = true;
    }
  }
  ctx.check(rejected_for_diversity, "the same-region candidate is rejected by the diversity floor");
}

CCSF_CASE(placement, compatibility_and_capacity_are_hard_gates) {
  auto world = make_world(base_config(CoordinatorId::from_value(44), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  StateGenerationRecord state;
  state.state_id = world->state;
  state.generation = world->generation;
  state.logical_size = SizeValue::of(1ULL << 30U);
  state.registered_at = world->now();

  PlacementRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.policy = default_placement_policy(world->ids.take());

  PlacementCandidate incompatible =
      candidate_for(*world, world->cluster_b, world->region_one, FailureDomainId::from_value(2),
                    ClusterHealthState::HEALTHY);
  incompatible.compatibility = CompatibilityResult::INCOMPATIBLE;
  PlacementCandidate unknown_compatibility =
      candidate_for(*world, world->cluster_c, world->region_two, FailureDomainId::from_value(3),
                    ClusterHealthState::HEALTHY);
  unknown_compatibility.compatibility = CompatibilityResult::UNKNOWN;

  request.candidates = {incompatible, unknown_compatibility};
  const PlacementEvaluation evaluation = evaluate_placement(request, &state);
  ctx.check(evaluation.decision == DecisionKind::REJECT,
            "no candidate survives when compatibility is unknown or incompatible");
  ctx.check(evaluation.rejected.size() == 2, "both candidates are rejected");
  bool saw_incompatible = false;
  bool saw_unknown = false;
  for (const PlacementRejection& rejection : evaluation.rejected) {
    if (rejection.code == ErrorCode::INCOMPATIBLE) saw_incompatible = true;
    if (rejection.code == ErrorCode::COMPATIBILITY_UNKNOWN) saw_unknown = true;
  }
  ctx.check(saw_incompatible, "INCOMPATIBLE is reported distinctly");
  ctx.check(saw_unknown, "UNKNOWN compatibility is reported distinctly and never treated as legal");

  PlacementCandidate too_small =
      candidate_for(*world, world->cluster_b, world->region_one, FailureDomainId::from_value(2),
                    ClusterHealthState::HEALTHY);
  too_small.capacity.total_logical_bytes = 1ULL << 20U;
  too_small.capacity.used_logical_bytes = 1ULL << 19U;
  request.candidates = {too_small};
  const PlacementEvaluation capacity = evaluate_placement(request, &state);
  ctx.check(capacity.decision == DecisionKind::REJECT, "a full destination is rejected");
  ctx.check(capacity.rejected.front().code == ErrorCode::CAPACITY_INSUFFICIENT,
            "reported as insufficient capacity");
}

CCSF_CASE(placement, plan_replication_selects_a_legal_destination) {
  auto world = make_world(base_config(CoordinatorId::from_value(45), ClusterId::from_value(100)));
  ctx.check(world != nullptr, "world");
  ctx.check(publish_state(*world, "payload", 1ULL << 20U, ConsistencyMode::IMMUTABLE, 2).has_value(),
            "state committed");
  ctx.check(seed_replica(*world, world->cluster_a, ReplicaDurabilityState::DURABLE).has_value(),
            "seeded");

  ReplicationRequest request;
  request.state_id = world->state;
  request.state_generation = world->generation;
  request.expected_coordinator_epoch = world->epoch();
  auto plan = world->fabric->plan_replication(request);
  ctx.check(plan.has_value(), "a replication plan is produced");
  ctx.check(plan.value().decision == DecisionKind::REPLICATE, "the plan replans a replication");
  ctx.check(plan.value().chosen_cluster.valid(), "a destination cluster is chosen");
  ctx.check(plan.value().chosen_cluster != world->cluster_a,
            "the chosen destination is not the source cluster");

  auto planned = world->fabric->plan_replication(request);
  ctx.check(planned.value().chosen_cluster == plan.value().chosen_cluster,
            "planning the same request twice yields the same destination");

  auto explanation = world->fabric->explain_replication_plan(request);
  ctx.check(explanation.has_value(), "the placement decision is explainable");
  ctx.check(explanation.value().render().find("Decision: REPLICATE") != std::string::npos,
            "the explanation states the decision");
}
