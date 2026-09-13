// Example: transfer versus reconstruction.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <vector>

#include "common.hpp"

int main() {
  using namespace ccsf;
  using namespace ccsf::examples;
  banner("transfer versus reconstruction");

  EconomicInputs inputs;
  inputs.transfer_bytes = SizeValue::of(38ULL * 1024ULL * 1024ULL * 1024ULL);
  inputs.transfer_cost = CostValue::from_policy(180000);
  inputs.egress_cost = CostValue::from_policy(20000);
  inputs.reconstruction_cost = CostValue::from_policy(1730000);
  inputs.compute_cost = CostValue::from_policy(90000);
  inputs.expected_reuse = ReuseValue::estimated(14U, 900U);
  inputs.reconstruction_available = true;
  inputs.reconstruction_deterministic = true;
  inputs.destination_adds_diversity = true;

  EconomicDecision decision = decide_action(inputs);
  line("decision: " + std::string(to_string(decision.kind)));
  line("reason: " + decision.reason);
  line("transfer total: " + describe(decision.transfer_total));
  line("reconstruction total: " + describe(decision.reconstruction_total));
  for (const std::string& factor : decision.factors) {
    line("  factor: " + factor);
  }

  line("");
  line("the same query with no price evidence at all:");
  EconomicInputs unpriced = inputs;
  unpriced.transfer_cost = CostValue::unknown();
  unpriced.egress_cost = CostValue::unknown();
  unpriced.reconstruction_cost = CostValue::unknown();
  unpriced.compute_cost = CostValue::unknown();
  const EconomicDecision deferred = decide_action(unpriced);
  line("decision: " + std::string(to_string(deferred.kind)));
  line("reason: " + deferred.reason);

  line("");
  line("the same query with an illegal destination:");
  EconomicInputs illegal = inputs;
  illegal.destination_healthy = false;
  const EconomicDecision rejected = decide_action(illegal);
  line("decision: " + std::string(to_string(rejected.kind)));
  line("reason: " + rejected.reason);
  line("a hard legality constraint is never traded away for a cheaper path");
  return 0;
}
