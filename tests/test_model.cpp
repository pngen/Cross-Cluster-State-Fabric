// Cross-Cluster State Fabric - model proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "ccsf/compatibility.hpp"
#include "ccsf/economics.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/replica_set.hpp"
#include "framework.hpp"
#include "harness.hpp"

CCSF_CASE(compatibility, unknown_is_never_compatible) {
  using namespace ccsf;
  CompatibilityRequirement requirement;
  requirement.compatibility_id = CompatibilityId::from_value(7);
  requirement.generation = CompatibilityGeneration::from_value(3);

  ctx.check(evaluate_compatibility(requirement, nullptr, CompatibilityGeneration{},
                                   false) == CompatibilityResult::UNKNOWN,
            "a missing candidate must be UNKNOWN");

  CompatibilityDescriptor descriptor;
  ctx.check(descriptor.set("accelerator", "sm_120"), "descriptor accepts a bound attribute");
  ctx.check(!descriptor.set("", "x"), "empty attribute keys are rejected");
  ctx.check(!descriptor.set("k", std::string(kMaxCompatibilityValueLength + 1, 'v')),
            "oversized attribute values are rejected");

  requirement.required.set("accelerator", "sm_120");
  requirement.required.set("runtime_abi", "ccsf-1");
  ctx.check(evaluate_compatibility(requirement, &descriptor, CompatibilityGeneration::from_value(3),
                                   false) == CompatibilityResult::UNKNOWN,
            "a required attribute the candidate does not carry must be UNKNOWN, not compatible");

  ctx.check(descriptor.set("runtime_abi", "ccsf-1"), "attribute accepted");
  ctx.check(evaluate_compatibility(requirement, &descriptor, CompatibilityGeneration::from_value(3),
                                   false) == CompatibilityResult::COMPATIBLE,
            "a fully satisfied requirement is COMPATIBLE");

  ctx.check(descriptor.set("accelerator", "sm_90"), "attribute replaced");
  ctx.check(evaluate_compatibility(requirement, &descriptor, CompatibilityGeneration::from_value(3),
                                   false) == CompatibilityResult::INCOMPATIBLE,
            "a differing concrete value is INCOMPATIBLE");

  ctx.check(descriptor.set("accelerator", "sm_120"), "attribute restored");
  ctx.check(evaluate_compatibility(requirement, &descriptor, CompatibilityGeneration::from_value(2),
                                   false) == CompatibilityResult::REVALIDATION_REQUIRED,
            "evidence behind the required generation requires revalidation");
  ctx.check(evaluate_compatibility(requirement, &descriptor, CompatibilityGeneration::from_value(3),
                                   true) == CompatibilityResult::REVALIDATION_REQUIRED,
            "a flagged candidate requires revalidation");

  CompatibilityRequirement wildcard_requirement;
  wildcard_requirement.compatibility_id = CompatibilityId::from_value(7);
  wildcard_requirement.generation = CompatibilityGeneration::from_value(1);
  ctx.check(wildcard_requirement.required.set("accelerator", "*"), "wildcard accepted");
  CompatibilityDescriptor concrete;
  ctx.check(concrete.set("accelerator", "sm_999"), "candidate carries a different architecture");
  ctx.check(evaluate_compatibility(wildcard_requirement, &concrete,
                                   CompatibilityGeneration::from_value(9),
                                   false) == CompatibilityResult::COMPATIBLE,
            "a wildcard requirement matches any present value");
  CompatibilityDescriptor absent;
  ctx.check(evaluate_compatibility(wildcard_requirement, &absent,
                                   CompatibilityGeneration::from_value(9),
                                   false) == CompatibilityResult::UNKNOWN,
            "a wildcard still requires the attribute to be present");
}

CCSF_CASE(compatibility, fingerprint_is_canonical) {
  using namespace ccsf;
  CompatibilityDescriptor first;
  first.set("b", "2");
  first.set("a", "1");
  CompatibilityDescriptor second;
  second.set("a", "1");
  second.set("b", "2");
  ctx.check(first.fingerprint() == second.fingerprint(),
            "attribute insertion order must not change the fingerprint");
  second.set("a", "9");
  ctx.check(first.fingerprint() != second.fingerprint(),
            "different content must produce a different fingerprint");
}

CCSF_CASE(policy, effective_requirements_are_well_formed) {
  using namespace ccsf;
  const ReplicationPolicyRecord single = make_replication_policy(
      ReplicationPolicyId::from_value(1), ReplicationPolicyGeneration::from_value(1),
      ReplicationPolicyForm::SINGLE, 4);
  const ReplicationRequirements effective = single.effective_requirements();
  ctx.check(effective.minimum_authoritative_replicas == 1 && effective.desired_replicas == 1 &&
                effective.maximum_replicas == 1,
            "SINGLE pins the replication factor to one regardless of the requested count");

  const ReplicationPolicyRecord region_diverse = make_replication_policy(
      ReplicationPolicyId::from_value(2), ReplicationPolicyGeneration::from_value(1),
      ReplicationPolicyForm::REGION_DIVERSE, 3);
  const ReplicationRequirements region_effective = region_diverse.effective_requirements();
  ctx.check(region_effective.region_diversity >= 2,
            "REGION_DIVERSE requires at least two regions");
  ctx.check(region_effective.minimum_authoritative_replicas <= region_effective.maximum_replicas,
            "minimum never exceeds maximum");

  ReplicationRequirements squeezed;
  squeezed.minimum_authoritative_replicas = 9;
  squeezed.maximum_replicas = 2;
  squeezed.desired_replicas = 1;
  ReplicationPolicyRecord custom;
  custom.requirements = squeezed;
  custom.requirements.form = ReplicationPolicyForm::POLICY_CUSTOM;
  const ReplicationRequirements clamped = custom.effective_requirements();
  ctx.check(clamped.minimum_authoritative_replicas <= clamped.maximum_replicas,
            "an impossible policy is clamped conservatively");
  ctx.check(clamped.desired_replicas >= clamped.minimum_authoritative_replicas,
            "desired never falls below the minimum");
}

CCSF_CASE(policy, cost_policy_never_invents_price) {
  using namespace ccsf;
  CostPolicy unset;
  const CostValue unknown = unset.price_bytes(1000, SizeValue::of(1U << 20U));
  ctx.check(!unknown.known(), "an unpriced policy yields UNKNOWN cost, never zero");

  CostPolicy priced;
  priced.source = CostSource::POLICY;
  priced.transfer_micro_per_mib = 5000;
  const CostValue value = priced.price_bytes(priced.transfer_micro_per_mib, SizeValue::of(1U << 20U));
  ctx.check(value.known() && value.micro_units == 5000, "one mebibyte at the policy price");
  ctx.check(!priced.price_bytes(5000, SizeValue::unknown()).known(),
            "an unknown size yields an unknown cost");
}

CCSF_CASE(economics, hard_preconditions_precede_cost) {
  using namespace ccsf;
  EconomicInputs inputs;
  inputs.transfer_bytes = SizeValue::of(38ULL * 1024ULL * 1024ULL * 1024ULL);
  inputs.transfer_cost = CostValue::from_policy(180000);
  inputs.reconstruction_cost = CostValue::from_policy(1730000);
  inputs.expected_reuse = ReuseValue::estimated(14U, 900U);
  inputs.reconstruction_available = true;
  inputs.destination_healthy = false;

  const EconomicDecision unhealthy = decide_action(inputs);
  ctx.check(unhealthy.kind == DecisionKind::REJECT,
            "an unhealthy destination rejects regardless of a cheap transfer");

  inputs.destination_healthy = true;
  const EconomicDecision transfer = decide_action(inputs);
  ctx.check(transfer.kind == DecisionKind::REPLICATE,
            "a cheaper legal transfer is chosen over reconstruction");

  inputs.transfer_cost = CostValue::from_policy(9000000);
  const EconomicDecision reconstruct = decide_action(inputs);
  ctx.check(reconstruct.kind == DecisionKind::RECONSTRUCT,
            "a cheaper reconstruction is chosen when transfer is more expensive");

  inputs.transfer_cost = CostValue::unknown();
  const EconomicDecision unknown_transfer = decide_action(inputs);
  ctx.check(unknown_transfer.kind == DecisionKind::RECONSTRUCT,
            "an unknown transfer price cannot beat a known reconstruction price");

  inputs.reconstruction_cost = CostValue::unknown();
  const EconomicDecision both_unknown = decide_action(inputs);
  ctx.check(both_unknown.kind == DecisionKind::DEFER,
            "with no price evidence at all the decision defers rather than guessing");

  inputs.transfer_cost = CostValue::from_policy(1);
  inputs.reconstruction_cost = CostValue::from_policy(1);
  inputs.expected_reuse = ReuseValue::unknown();
  const EconomicDecision unknown_reuse = decide_action(inputs);
  ctx.check(unknown_reuse.kind == DecisionKind::DEFER,
            "an unknown reuse value cannot justify a move");

  inputs.expected_reuse = ReuseValue::measured(0U, 0U);
  const EconomicDecision zero_reuse = decide_action(inputs);
  ctx.check(zero_reuse.kind == DecisionKind::REJECT,
            "zero expected reuse rejects the move");
}

CCSF_CASE(replica_set, membership_recomputes_deterministically) {
  using namespace ccsf;
  ReplicaSetRecord set;
  set.replica_set_id = ReplicaSetId::from_value(5);
  set.generation = ReplicaSetGeneration::from_value(1);
  set.state_id = StateId::from_value(9);
  set.state_generation = StateGeneration::from_value(1);
  set.requirements = make_replication_policy(ReplicationPolicyId::from_value(1),
                                             ReplicationPolicyGeneration::from_value(1),
                                             ReplicationPolicyForm::N_REPLICAS, 2)
                         .requirements;

  const ReplicaSetRecord empty = recompute_replica_set(set);
  ctx.check(empty.state == ReplicaSetState::NO_AUTHORITATIVE_REPLICA,
            "an empty authoritative membership is explicitly marked");

  ReplicaSetMember member;
  member.replica_id = ReplicaId::from_value(11);
  member.cluster_id = ClusterId::from_value(2);
  member.region = RegionId::from_value(1);
  member.failure_domain = FailureDomainId::from_value(1);
  member.authoritative = true;
  member.integrity = ReplicaIntegrityState::VERIFIED;
  member.compatibility = ReplicaCompatibilityState::COMPATIBLE;
  member.lifecycle = ReplicaLifecycle::AUTHORITATIVE;
  set.authoritative_members.push_back(member);

  const ReplicaSetRecord single = recompute_replica_set(set);
  ctx.check(single.authoritative_count == 1, "one authoritative member is counted");
  ctx.check(single.state == ReplicaSetState::UNDER_REPLICATED,
            "one replica against a minimum of two is under replicated");

  ReplicaSetMember second = member;
  second.replica_id = ReplicaId::from_value(12);
  second.cluster_id = ClusterId::from_value(3);
  second.region = RegionId::from_value(2);
  second.failure_domain = FailureDomainId::from_value(2);
  set.authoritative_members.push_back(second);
  const ReplicaSetRecord healthy = recompute_replica_set(set);
  ctx.check(healthy.authoritative_count == 2 && healthy.state == ReplicaSetState::HEALTHY,
            "two authoritative members in two clusters satisfy the minimum");
  ctx.check(healthy.distinct_clusters == 2 && healthy.distinct_regions == 2,
            "diversity is derived from member locations");

  set.authoritative_members[1].integrity = ReplicaIntegrityState::CORRUPT;
  const ReplicaSetRecord corrupt = recompute_replica_set(set);
  ctx.check(corrupt.state == ReplicaSetState::CORRUPT,
            "a corrupt member marks the replica set as corrupt");

  set.authoritative_members[1].integrity = ReplicaIntegrityState::VERIFIED;
  set.authoritative_members[1].compatibility = ReplicaCompatibilityState::INCOMPATIBLE;
  const ReplicaSetRecord incompatible = recompute_replica_set(set);
  ctx.check(incompatible.state == ReplicaSetState::CORRUPT,
            "an incompatible member marks the replica set as corrupt");

  set.authoritative_members[1].compatibility = ReplicaCompatibilityState::COMPATIBLE;
  set.revalidation_required = true;
  const ReplicaSetRecord revalidation = recompute_replica_set(set);
  ctx.check(revalidation.state == ReplicaSetState::REVALIDATION_REQUIRED,
            "an explicit revalidation obligation is reported");
}
