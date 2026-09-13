// Cross-Cluster State Fabric - transfer versus reconstruction economics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Economics never override safety. The economic model is invoked only after
// every hard gate passed, and it reports UNKNOWN rather than inventing
// precision when price evidence is absent.

#ifndef CCSF_ECONOMICS_HPP
#define CCSF_ECONOMICS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/cost.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

/// Inputs to the transfer-versus-reconstruction decision.
struct EconomicInputs {
  SizeValue transfer_bytes;
  CostValue transfer_cost;
  CostValue egress_cost;
  CostValue reconstruction_cost;
  CostValue storage_cost;
  CostValue compute_cost;
  ReuseValue expected_reuse;
  bool reconstruction_available{false};
  bool reconstruction_deterministic{false};
  bool destination_capacity_sufficient{true};
  bool source_healthy{true};
  bool destination_healthy{true};
  bool destination_adds_diversity{false};
  std::uint64_t predicted_transfer_ticks{0};
  std::uint64_t maximum_transfer_ticks{0};
  std::uint64_t expected_reuse_count{0};
};

/// Result of the economic decision, with a structured explanation.
struct EconomicDecision {
  DecisionKind kind{DecisionKind::UNKNOWN};
  CostValue transfer_total;
  CostValue reconstruction_total;
  CostValue repair_total;
  std::vector<std::string> factors;
  std::string reason;
};

/// Deterministic decision. Hard preconditions are checked first; a failed
/// precondition yields REJECT or DEFER, never a cost-based answer.
EconomicDecision decide_action(const EconomicInputs& inputs);

}  // namespace ccsf

#endif  // CCSF_ECONOMICS_HPP
