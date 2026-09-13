// Cross-Cluster State Fabric - transfer versus reconstruction economics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/economics.hpp"

#include <string>

namespace ccsf {

EconomicDecision decide_action(const EconomicInputs& inputs) {
  EconomicDecision decision;
  decision.transfer_total = cost_sum(inputs.transfer_cost, inputs.egress_cost);
  decision.reconstruction_total = cost_sum(inputs.reconstruction_cost, inputs.compute_cost);
  decision.repair_total = inputs.storage_cost;

  decision.factors.push_back(std::string("transfer cost: ") + describe(decision.transfer_total));
  decision.factors.push_back(std::string("reconstruction cost: ") +
                             describe(decision.reconstruction_total));
  decision.factors.push_back(std::string("expected reuse: ") + describe(inputs.expected_reuse));
  decision.factors.push_back(std::string("transfer bytes: ") + describe(inputs.transfer_bytes));

  // Hard preconditions first: economics may never legitimize an illegal move.
  if (!inputs.destination_healthy) {
    decision.kind = DecisionKind::REJECT;
    decision.reason = "destination cluster is not healthy";
    return decision;
  }
  if (!inputs.source_healthy) {
    decision.kind = DecisionKind::REJECT;
    decision.reason = "source cluster is not healthy";
    return decision;
  }
  if (!inputs.destination_capacity_sufficient) {
    decision.kind = DecisionKind::REJECT;
    decision.reason = "destination has insufficient capacity for the state";
    return decision;
  }
  if (inputs.maximum_transfer_ticks != 0U &&
      inputs.predicted_transfer_ticks > inputs.maximum_transfer_ticks) {
    decision.kind = DecisionKind::DEFER;
    decision.reason = "predicted transfer duration exceeds the policy horizon";
    return decision;
  }
  if (!inputs.expected_reuse.known()) {
    decision.kind = DecisionKind::DEFER;
    decision.reason = "expected reuse value is unknown; the transfer cannot be justified";
    return decision;
  }
  if (inputs.expected_reuse.expected_value_milli() == 0U) {
    decision.kind = DecisionKind::REJECT;
    decision.reason = "expected reuse value is zero; moving the state would be waste";
    return decision;
  }

  const bool transfer_known = decision.transfer_total.known();
  const bool reconstruction_known = decision.reconstruction_total.known();

  if (!inputs.reconstruction_available) {
    if (!transfer_known) {
      decision.kind = DecisionKind::DEFER;
      decision.reason =
          "neither transfer nor reconstruction has known cost evidence; failing closed";
      return decision;
    }
    decision.kind = DecisionKind::REPLICATE;
    if (inputs.destination_adds_diversity) {
      decision.factors.push_back("destination adds failure-domain diversity");
    }
    decision.reason = "reconstruction is unavailable; transfer is the only legal path";
    return decision;
  }

  if (!transfer_known && !reconstruction_known) {
    decision.kind = DecisionKind::DEFER;
    decision.reason = "both transfer and reconstruction cost evidence are unknown";
    return decision;
  }
  if (!transfer_known && reconstruction_known) {
    decision.kind = DecisionKind::RECONSTRUCT;
    decision.reason =
        "transfer cost evidence is unknown while reconstruction cost is known";
    return decision;
  }
  if (transfer_known && !reconstruction_known) {
    decision.kind = DecisionKind::REPLICATE;
    decision.reason =
        "reconstruction cost evidence is unknown while transfer cost is known";
    return decision;
  }

  const int comparison = cost_compare(decision.transfer_total, decision.reconstruction_total);
  if (comparison < 0) {
    decision.kind = DecisionKind::REPLICATE;
    decision.reason = "legal destination and transfer is cheaper than reconstruction";
  } else if (comparison > 0) {
    decision.kind = DecisionKind::RECONSTRUCT;
    decision.reason = "reconstruction is cheaper than transfer to a legal destination";
  } else {
    decision.kind = DecisionKind::REPLICATE;
    decision.reason = "costs are equal; transfer is chosen to preserve the exact state bytes";
  }
  if (inputs.destination_adds_diversity) {
    decision.factors.push_back("destination adds failure-domain diversity");
  }
  if (inputs.reconstruction_deterministic) {
    decision.factors.push_back("reconstruction identity is marked deterministic");
  }
  return decision;
}

}  // namespace ccsf
