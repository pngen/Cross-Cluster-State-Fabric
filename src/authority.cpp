// Cross-Cluster State Fabric - replica authority assessment.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/authority.hpp"

#include <string>

namespace ccsf {
namespace {

void deny(ReplicaAuthorityAssessment* assessment, ErrorCode code, std::string reason) {
  assessment->code = code;
  assessment->reason = std::move(reason);
}

}  // namespace

ReplicaAuthorityAssessment assess_replica_authority(const ReplicaRecord& replica,
                                                    const AuthorityContext& context) {
  ReplicaAuthorityAssessment assessment;

  const bool terminal = replica.lifecycle == ReplicaLifecycle::RETIRED ||
                        replica.lifecycle == ReplicaLifecycle::ABANDONED;
  const bool staging = replica.lifecycle == ReplicaLifecycle::PLANNED ||
                       replica.lifecycle == ReplicaLifecycle::RESERVED ||
                       replica.lifecycle == ReplicaLifecycle::STAGING;

  assessment.physically_present = !terminal && !staging &&
                                  replica.availability != ReplicaAvailabilityState::ABSENT;
  assessment.transfer_complete =
      !terminal &&
      (replica.lifecycle == ReplicaLifecycle::TRANSFERRED ||
       replica.lifecycle == ReplicaLifecycle::VERIFYING ||
       replica.lifecycle == ReplicaLifecycle::PREPARED ||
       replica.lifecycle == ReplicaLifecycle::AUTHORITATIVE ||
       replica.lifecycle == ReplicaLifecycle::DEGRADED || replica.commit_recorded);
  assessment.integrity_verified = replica.integrity == ReplicaIntegrityState::VERIFIED &&
                                  replica.evidence.integrity_evidence_present;
  assessment.compatibility_verified = replica.compatibility == ReplicaCompatibilityState::COMPATIBLE;
  assessment.cluster_incarnation_current =
      context.cluster != nullptr &&
      context.cluster->incarnation == replica.location.cluster_incarnation &&
      context.cluster->live_authority;
  assessment.coordinator_epoch_current =
      !replica.last_epoch.valid() || replica.last_epoch <= context.current_epoch;
  assessment.evidence_current = !replica.evidence.revalidation_required &&
                                replica.evidence.evidence_generation.valid() &&
                                (context.cluster == nullptr ||
                                 cluster_evidence_is_current(*context.cluster));

  assessment.current_for_state_generation =
      context.state != nullptr && context.state->generation == replica.state_generation &&
      context.state->state_id == replica.state_id && context.state->is_current() &&
      !context.state->revalidation_required &&
      replica.lifecycle != ReplicaLifecycle::SUPERSEDED;

  // Replica-set membership is bound to the replica set's stable identity and to
  // the replica's own committed membership state, not to the generation the
  // replica happened to join at: a later membership change elsewhere in the set
  // must not retroactively unmask an older committed member.
  assessment.member_of_current_replica_set_generation =
      context.replica_set != nullptr && replica.replica_set_id.valid() &&
      replica.replica_set_id == context.replica_set->replica_set_id &&
      context.replica_set->state_generation == replica.state_generation &&
      replica.commit_recorded && replica.counts_toward_factor;

  assessment.fenced = replica.authority == ReplicaAuthorityState::FENCED ||
                      replica.lifecycle == ReplicaLifecycle::QUARANTINED ||
                      replica.lifecycle == ReplicaLifecycle::STALE ||
                      (context.cluster != nullptr && !context.cluster->live_authority);

  assessment.historical_only = !terminal && !assessment.current_for_state_generation;

  if (terminal) {
    deny(&assessment, ErrorCode::NOT_FOUND,
         "replica " + replica.replica_id.str() + " is " + to_string(replica.lifecycle) +
             " and holds no authority");
    return assessment;
  }
  if (staging) {
    deny(&assessment, ErrorCode::AUTHORITY_NOT_ESTABLISHED,
         "replica " + replica.replica_id.str() + " is still " + to_string(replica.lifecycle) +
             " and has not been committed");
    return assessment;
  }
  if (assessment.fenced) {
    deny(&assessment, ErrorCode::FENCED,
         "replica " + replica.replica_id.str() +
             " is fenced: its cluster incarnation or lifecycle no longer holds authority");
    return assessment;
  }
  if (context.cluster == nullptr) {
    deny(&assessment, ErrorCode::UNKNOWN_CLUSTER,
         "cluster " + replica.location.cluster_id.str() + " is not registered");
    return assessment;
  }
  if (!assessment.cluster_incarnation_current) {
    deny(&assessment, ErrorCode::STALE_CLUSTER_INCARNATION,
         "replica " + replica.replica_id.str() + " was written under cluster incarnation " +
             replica.location.cluster_incarnation.str() + " but cluster " +
             replica.location.cluster_id.str() + " is now at incarnation " +
             context.cluster->incarnation.str());
    return assessment;
  }
  if (!assessment.coordinator_epoch_current) {
    deny(&assessment, ErrorCode::STALE_EPOCH,
         "replica " + replica.replica_id.str() + " carries coordinator epoch " +
             replica.last_epoch.str() + " ahead of the current epoch " +
             context.current_epoch.str());
    return assessment;
  }
  if (replica.lifecycle == ReplicaLifecycle::REVALIDATION_REQUIRED ||
      replica.evidence.revalidation_required) {
    deny(&assessment, ErrorCode::REVALIDATION_REQUIRED,
         "replica " + replica.replica_id.str() +
             " requires revalidation of its live evidence before reuse");
    return assessment;
  }
  if (!assessment.physically_present) {
    deny(&assessment, ErrorCode::NO_AUTHORITATIVE_REPLICA,
         "replica " + replica.replica_id.str() + " is not physically present (availability " +
             to_string(replica.availability) + ")");
    return assessment;
  }
  if (context.state == nullptr) {
    deny(&assessment, ErrorCode::UNKNOWN_STATE,
         "state generation " + replica.state_generation.str() + " is not registered");
    return assessment;
  }
  if (context.state->state_id != replica.state_id ||
      context.state->generation != replica.state_generation) {
    deny(&assessment, ErrorCode::STALE_STATE_GENERATION,
         "replica " + replica.replica_id.str() + " does not reference the requested state generation");
    return assessment;
  }
  if (replica.lifecycle == ReplicaLifecycle::SUPERSEDED) {
    deny(&assessment, ErrorCode::STALE_STATE_GENERATION,
         "replica " + replica.replica_id.str() + " is superseded");
    return assessment;
  }
  if (replica.lifecycle == ReplicaLifecycle::CORRUPT ||
      replica.integrity == ReplicaIntegrityState::CORRUPT) {
    deny(&assessment, ErrorCode::INTEGRITY_MISMATCH,
         "replica " + replica.replica_id.str() + " is corrupt");
    return assessment;
  }
  if (replica.integrity == ReplicaIntegrityState::MISMATCH) {
    deny(&assessment, ErrorCode::INTEGRITY_MISMATCH,
         "replica " + replica.replica_id.str() + " failed integrity verification");
    return assessment;
  }
  if (replica.compatibility == ReplicaCompatibilityState::INCOMPATIBLE) {
    deny(&assessment, ErrorCode::INCOMPATIBLE,
         "replica " + replica.replica_id.str() + " is incompatible with the state requirement");
    return assessment;
  }
  if (replica.lifecycle == ReplicaLifecycle::RETIRING) {
    deny(&assessment, ErrorCode::FENCED,
         "replica " + replica.replica_id.str() + " is retiring and may not serve reuse");
    return assessment;
  }

  const bool integrity_gate = context.policy == nullptr ||
                              !context.policy->reuse_rules.require_verified_integrity ||
                              assessment.integrity_verified;
  const bool compatibility_gate = context.policy == nullptr ||
                                  !context.policy->reuse_rules.require_verified_compatibility ||
                                  assessment.compatibility_verified;
  const bool generation_gate = context.policy == nullptr ||
                               !context.policy->reuse_rules.require_current_state_generation ||
                               assessment.current_for_state_generation;
  const bool membership_gate = context.policy == nullptr ||
                               !context.policy->reuse_rules.require_replica_set_membership ||
                               assessment.member_of_current_replica_set_generation;
  const bool evidence_gate = context.policy == nullptr ||
                             (!context.policy->revalidation.require_fresh_health_evidence &&
                              !context.policy->revalidation.require_fresh_capability_evidence) ||
                             assessment.evidence_current;

  if (!integrity_gate) {
    deny(&assessment, ErrorCode::INTEGRITY_UNKNOWN,
         "replica " + replica.replica_id.str() +
             " has no verified integrity evidence for the required generation");
    return assessment;
  }
  if (!compatibility_gate) {
    deny(&assessment, ErrorCode::COMPATIBILITY_UNKNOWN,
         "replica " + replica.replica_id.str() +
             " has no verified compatibility evidence");
    return assessment;
  }
  if (!generation_gate) {
    deny(&assessment, ErrorCode::STALE_STATE_GENERATION,
         "replica " + replica.replica_id.str() +
             " is not current for the authoritative state generation");
    return assessment;
  }
  if (!membership_gate) {
    deny(&assessment, ErrorCode::STALE_REPLICA_SET_GENERATION,
         "replica " + replica.replica_id.str() +
             " is not a member of the current replica set generation");
    return assessment;
  }
  if (!evidence_gate) {
    deny(&assessment, ErrorCode::REVALIDATION_REQUIRED,
         "replica " + replica.replica_id.str() +
             " has stale dynamic evidence and must be revalidated");
    return assessment;
  }
  if (replica.lifecycle != ReplicaLifecycle::AUTHORITATIVE &&
      replica.lifecycle != ReplicaLifecycle::DEGRADED) {
    deny(&assessment, ErrorCode::AUTHORITY_NOT_ESTABLISHED,
         "replica " + replica.replica_id.str() + " is in lifecycle " +
             to_string(replica.lifecycle) + " and holds no committed authority");
    return assessment;
  }

  assessment.authoritative_for_reuse = true;
  assessment.authoritative_as_replication_source = true;
  assessment.authoritative_for_migration_source_retirement = replica.commit_recorded;
  assessment.code = ErrorCode::OK;
  assessment.reason = "replica " + replica.replica_id.str() +
                      " is authoritative for reuse of " +
                      state_generation_label(replica.state_id, replica.state_generation);
  return assessment;
}

bool assessment_permits_reuse(const ReplicaAuthorityAssessment& assessment) noexcept {
  return assessment.authoritative_for_reuse && assessment.code == ErrorCode::OK;
}

std::string describe_authority(const ReplicaRecord& replica,
                               const ReplicaAuthorityAssessment& assessment) {
  std::string text = "replica ";
  text += replica.replica_id.str();
  text += " in cluster ";
  text += replica.location.cluster_id.str();
  text += ": ";
  text += to_string(assessment.code);
  text += " - ";
  text += assessment.reason;
  return text;
}

}  // namespace ccsf
