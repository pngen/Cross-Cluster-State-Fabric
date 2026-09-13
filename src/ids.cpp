// Cross-Cluster State Fabric - identity rendering and parsing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/ids.hpp"

#include <charconv>
#include <string>

namespace ccsf {
namespace detail {

std::string id_to_string(std::uint64_t value) {
  char buffer[24] = {};
  auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
  return std::string(buffer, converted.ptr);
}

Result<std::uint64_t> id_from_string(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "empty identity text");
  }
  std::uint64_t value = 0;
  const char* first = text.data();
  const char* last = text.data() + text.size();
  auto converted = std::from_chars(first, last, value, 10);
  if (converted.ec != std::errc{} || converted.ptr != last) {
    return make_error(ErrorCode::INVALID_ARGUMENT,
                      "identity text is not a canonical unsigned decimal: " + std::string(text));
  }
  return value;
}

}  // namespace detail

}  // namespace ccsf