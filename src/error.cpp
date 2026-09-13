// Cross-Cluster State Fabric - error code rendering.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/error.hpp"

#include <array>
#include <utility>

namespace ccsf {
namespace {

struct CodeName {
  ErrorCode code;
  const char* name;
};

constexpr std::array<CodeName, 62> kCodeNames{{
    {ErrorCode::OK, "OK"},
    {ErrorCode::INVALID_ARGUMENT, "INVALID_ARGUMENT"},
    {ErrorCode::NOT_FOUND, "NOT_FOUND"},
    {ErrorCode::UNKNOWN_STATE, "UNKNOWN_STATE"},
    {ErrorCode::UNKNOWN_REPLICA, "UNKNOWN_REPLICA"},
    {ErrorCode::UNKNOWN_CLUSTER, "UNKNOWN_CLUSTER"},
    {ErrorCode::UNKNOWN_OPERATION, "UNKNOWN_OPERATION"},
    {ErrorCode::UNKNOWN_POLICY, "UNKNOWN_POLICY"},
    {ErrorCode::UNKNOWN_STATE_CLASS, "UNKNOWN_STATE_CLASS"},
    {ErrorCode::STALE_EPOCH, "STALE_EPOCH"},
    {ErrorCode::STALE_WORKER, "STALE_WORKER"},
    {ErrorCode::STALE_CLUSTER_INCARNATION, "STALE_CLUSTER_INCARNATION"},
    {ErrorCode::STALE_STATE_GENERATION, "STALE_STATE_GENERATION"},
    {ErrorCode::STALE_REPLICA_GENERATION, "STALE_REPLICA_GENERATION"},
    {ErrorCode::STALE_REPLICA_SET_GENERATION, "STALE_REPLICA_SET_GENERATION"},
    {ErrorCode::STALE_PLACEMENT_GENERATION, "STALE_PLACEMENT_GENERATION"},
    {ErrorCode::STALE_MIGRATION_GENERATION, "STALE_MIGRATION_GENERATION"},
    {ErrorCode::STALE_TRANSFER_GENERATION, "STALE_TRANSFER_GENERATION"},
    {ErrorCode::STALE_EVIDENCE, "STALE_EVIDENCE"},
    {ErrorCode::STALE_OPERATION_GENERATION, "STALE_OPERATION_GENERATION"},
    {ErrorCode::NO_AUTHORITATIVE_REPLICA, "NO_AUTHORITATIVE_REPLICA"},
    {ErrorCode::INTEGRITY_UNKNOWN, "INTEGRITY_UNKNOWN"},
    {ErrorCode::INTEGRITY_MISMATCH, "INTEGRITY_MISMATCH"},
    {ErrorCode::INCOMPATIBLE, "INCOMPATIBLE"},
    {ErrorCode::COMPATIBILITY_UNKNOWN, "COMPATIBILITY_UNKNOWN"},
    {ErrorCode::DESTINATION_INELIGIBLE, "DESTINATION_INELIGIBLE"},
    {ErrorCode::SOURCE_INELIGIBLE, "SOURCE_INELIGIBLE"},
    {ErrorCode::UNDER_REPLICATED, "UNDER_REPLICATED"},
    {ErrorCode::OVER_REPLICATED, "OVER_REPLICATED"},
    {ErrorCode::SPLIT_BRAIN_RISK, "SPLIT_BRAIN_RISK"},
    {ErrorCode::MIGRATION_CONFLICT, "MIGRATION_CONFLICT"},
    {ErrorCode::TRANSFER_FAILED, "TRANSFER_FAILED"},
    {ErrorCode::TRANSFER_INCOMPLETE, "TRANSFER_INCOMPLETE"},
    {ErrorCode::COMMIT_CONFLICT, "COMMIT_CONFLICT"},
    {ErrorCode::DUPLICATE_CONFLICT, "DUPLICATE_CONFLICT"},
    {ErrorCode::REVALIDATION_REQUIRED, "REVALIDATION_REQUIRED"},
    {ErrorCode::ILLEGAL_TRANSITION, "ILLEGAL_TRANSITION"},
    {ErrorCode::POLICY_VIOLATION, "POLICY_VIOLATION"},
    {ErrorCode::CAPACITY_INSUFFICIENT, "CAPACITY_INSUFFICIENT"},
    {ErrorCode::FENCED, "FENCED"},
    {ErrorCode::QUARANTINED, "QUARANTINED"},
    {ErrorCode::IMMUTABLE_STATE_CONFLICT, "IMMUTABLE_STATE_CONFLICT"},
    {ErrorCode::AUTHORITY_NOT_ESTABLISHED, "AUTHORITY_NOT_ESTABLISHED"},
    {ErrorCode::PERSISTENCE_CORRUPT, "PERSISTENCE_CORRUPT"},
    {ErrorCode::PERSISTENCE_UNSUPPORTED_VERSION, "PERSISTENCE_UNSUPPORTED_VERSION"},
    {ErrorCode::PERSISTENCE_IO, "PERSISTENCE_IO"},
    {ErrorCode::PERSISTENCE_TRUNCATED, "PERSISTENCE_TRUNCATED"},
    {ErrorCode::FRAME_CORRUPT, "FRAME_CORRUPT"},
    {ErrorCode::FRAME_TOO_LARGE, "FRAME_TOO_LARGE"},
    {ErrorCode::FRAME_TRUNCATED, "FRAME_TRUNCATED"},
    {ErrorCode::PROTOCOL_UNSUPPORTED, "PROTOCOL_UNSUPPORTED"},
    {ErrorCode::TRANSPORT_IO, "TRANSPORT_IO"},
    {ErrorCode::TRANSPORT_CLOSED, "TRANSPORT_CLOSED"},
    {ErrorCode::RESOURCE_EXHAUSTED, "RESOURCE_EXHAUSTED"},
    {ErrorCode::OUTCOME_UNKNOWN, "OUTCOME_UNKNOWN"},
    {ErrorCode::CANCELLED, "CANCELLED"},
    {ErrorCode::SHUTTING_DOWN, "SHUTTING_DOWN"},
    {ErrorCode::INTERNAL_ERROR, "INTERNAL_ERROR"},
    {ErrorCode::NOT_SUPPORTED, "NOT_SUPPORTED"},
    {ErrorCode::UNKNOWN, "UNKNOWN"},
}};

}  // namespace

const char* to_string(ErrorCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "UNRECOGNIZED_ERROR_CODE";
}

bool is_fencing_error(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::STALE_EPOCH:
    case ErrorCode::STALE_WORKER:
    case ErrorCode::STALE_CLUSTER_INCARNATION:
    case ErrorCode::STALE_STATE_GENERATION:
    case ErrorCode::STALE_REPLICA_GENERATION:
    case ErrorCode::STALE_REPLICA_SET_GENERATION:
    case ErrorCode::STALE_PLACEMENT_GENERATION:
    case ErrorCode::STALE_MIGRATION_GENERATION:
    case ErrorCode::STALE_TRANSFER_GENERATION:
    case ErrorCode::STALE_EVIDENCE:
    case ErrorCode::STALE_OPERATION_GENERATION:
      return true;
    default:
      return false;
  }
}

std::string Error::to_string() const {
  std::string result = ccsf::to_string(code);
  result += ": ";
  result += message;
  if (!detail.empty()) {
    result += " (";
    result += detail;
    result += ")";
  }
  return result;
}

}  // namespace ccsf
