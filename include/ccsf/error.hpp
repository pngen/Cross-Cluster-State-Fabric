// Cross-Cluster State Fabric - structured error semantics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef CCSF_ERROR_HPP
#define CCSF_ERROR_HPP

#include <cstdint>
#include <string>
#include <utility>

namespace ccsf {

/// Structured error codes. Normal policy outcomes are reported through these
/// codes, never through exceptions.
enum class ErrorCode : std::uint16_t {
  OK = 0,

  // Input / lookup
  INVALID_ARGUMENT = 1,
  NOT_FOUND = 2,
  UNKNOWN_STATE = 3,
  UNKNOWN_REPLICA = 4,
  UNKNOWN_CLUSTER = 5,
  UNKNOWN_OPERATION = 6,
  UNKNOWN_POLICY = 7,
  UNKNOWN_STATE_CLASS = 8,

  // Fencing
  STALE_EPOCH = 20,
  STALE_WORKER = 21,
  STALE_CLUSTER_INCARNATION = 22,
  STALE_STATE_GENERATION = 23,
  STALE_REPLICA_GENERATION = 24,
  STALE_REPLICA_SET_GENERATION = 25,
  STALE_PLACEMENT_GENERATION = 26,
  STALE_MIGRATION_GENERATION = 27,
  STALE_TRANSFER_GENERATION = 28,
  STALE_EVIDENCE = 29,
  STALE_OPERATION_GENERATION = 30,

  // Authority / safety gates
  NO_AUTHORITATIVE_REPLICA = 40,
  INTEGRITY_UNKNOWN = 41,
  INTEGRITY_MISMATCH = 42,
  INCOMPATIBLE = 43,
  COMPATIBILITY_UNKNOWN = 44,
  DESTINATION_INELIGIBLE = 45,
  SOURCE_INELIGIBLE = 46,
  UNDER_REPLICATED = 47,
  OVER_REPLICATED = 48,
  SPLIT_BRAIN_RISK = 49,
  MIGRATION_CONFLICT = 50,
  TRANSFER_FAILED = 51,
  TRANSFER_INCOMPLETE = 52,
  COMMIT_CONFLICT = 53,
  DUPLICATE_CONFLICT = 54,
  REVALIDATION_REQUIRED = 55,
  ILLEGAL_TRANSITION = 56,
  POLICY_VIOLATION = 57,
  CAPACITY_INSUFFICIENT = 58,
  FENCED = 59,
  QUARANTINED = 60,
  IMMUTABLE_STATE_CONFLICT = 61,
  AUTHORITY_NOT_ESTABLISHED = 62,

  // Persistence
  PERSISTENCE_CORRUPT = 80,
  PERSISTENCE_UNSUPPORTED_VERSION = 81,
  PERSISTENCE_IO = 82,
  PERSISTENCE_TRUNCATED = 83,

  // Transport / protocol
  FRAME_CORRUPT = 100,
  FRAME_TOO_LARGE = 101,
  FRAME_TRUNCATED = 102,
  PROTOCOL_UNSUPPORTED = 103,
  TRANSPORT_IO = 104,
  TRANSPORT_CLOSED = 105,

  // Resource / lifecycle
  RESOURCE_EXHAUSTED = 120,
  OUTCOME_UNKNOWN = 121,
  CANCELLED = 122,
  SHUTTING_DOWN = 123,
  INTERNAL_ERROR = 124,
  NOT_SUPPORTED = 125,
  UNKNOWN = 126
};

/// Stable machine readable name of an error code (never localized).
const char* to_string(ErrorCode code) noexcept;

/// True when the code reports a fencing rejection rather than a plain failure.
bool is_fencing_error(ErrorCode code) noexcept;

/// Structured error value. Carries a code and preserved human context.
struct Error {
  ErrorCode code{ErrorCode::OK};
  std::string message;
  std::string detail;

  Error() = default;
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
  Error(ErrorCode c, std::string msg, std::string det)
      : code(c), message(std::move(msg)), detail(std::move(det)) {}

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::OK; }

  /// Single-line rendering: "CODE: message (detail)".
  [[nodiscard]] std::string to_string() const;
};

inline Error make_error(ErrorCode code, std::string message) {
  return Error(code, std::move(message));
}

/// Adds context to an existing error without losing the original code.
inline Error with_detail(Error error, std::string detail) {
  if (!error.detail.empty()) {
    error.detail += "; ";
  }
  error.detail += std::move(detail);
  return error;
}

}  // namespace ccsf

#endif  // CCSF_ERROR_HPP
