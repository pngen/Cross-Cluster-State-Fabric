// Cross-Cluster State Fabric - deterministic explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_EXPLAIN_HPP
#define CCSF_EXPLAIN_HPP

#include <string>
#include <vector>

#include "ccsf/ids.hpp"

namespace ccsf {

/// A structured explanation. Lines are appended in a fixed, documented order so
/// that the same canonical inputs always render the same text.
struct Explanation {
  std::string title;
  std::vector<std::string> lines;

  void add(std::string line) { lines.push_back(std::move(line)); }
  void add_labeled(const std::string& label, const std::string& value);

  [[nodiscard]] std::string render() const;
  [[nodiscard]] bool empty() const noexcept { return lines.empty(); }
};

}  // namespace ccsf

#endif  // CCSF_EXPLAIN_HPP
