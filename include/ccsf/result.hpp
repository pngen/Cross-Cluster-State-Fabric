// Cross-Cluster State Fabric - result and status plumbing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_RESULT_HPP
#define CCSF_RESULT_HPP

#include <optional>
#include <string>
#include <utility>

#include "ccsf/error.hpp"

namespace ccsf {

/// Result of an operation that produces a value. Either the value or a
/// structured Error is present; never both.
template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Error error) : error_(std::move(error)) {}      // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const T& value() const& {
    return *value_;
  }
  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }

  [[nodiscard]] const Error& error() const noexcept { return error_; }

  [[nodiscard]] T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

 private:
  std::optional<T> value_;
  Error error_{};
};

/// Result of an operation that produces no value.
template <>
class [[nodiscard]] Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool has_value() const noexcept { return error_.code == ErrorCode::OK; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

 private:
  Error error_{};
};

using Status = Result<void>;

inline Status ok_status() noexcept { return Status{}; }

}  // namespace ccsf

/// Propagate a failed Result out of the current function (which must return a Result).
#define CCSF_TRY(expr)                        \
  do {                                        \
    auto&& ccsf_try_result = (expr);          \
    if (!ccsf_try_result) {                   \
      return ccsf_try_result.error();         \
    }                                         \
  } while (false)

/// Propagate a failed Result while binding its value to a name.
#define CCSF_TRY_ASSIGN(name, expr)           \
  auto ccsf_try_##name = (expr);              \
  if (!ccsf_try_##name) {                     \
    return ccsf_try_##name.error();           \
  }                                           \
  auto& name = ccsf_try_##name.value()

#endif  // CCSF_RESULT_HPP
