// Cross-Cluster State Fabric - replica and replica-set operations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "detail/fabric_impl.hpp"

namespace ccsf {
namespace {

/// Rejects a replica-directed message whose authority metadata is not current.
/// The order of checks is fixed and part of the documented error semantics.
Status guard_replica(const detail::ControlPlane& plane, ReplicaId replica_id,
                     ReplicaGeneration generation, ClusterIncarnationId incarnation,
                     WorkerBootId worker_boot, CoordinatorEpoch expected_epoch,
                     ReplicaRecord* out) {
  const ReplicaRecord* existing = find_replica(plane, replica_id);
  if (existing == nullptr) {
    return make_error(ErrorCode::UNKNOWN_REPLICA, "the replica is not registered");
  }
  if (generation.valid() && generation != existing->replica_generation) {
    return make_error(ErrorCode::STALE_REPLICA_GENERATION,
                      "message names replica generation " + generation.str() +
                          " but the current generation is " +
                          existing->replica_generation.str());
  }
  if (expected_epoch.valid() && expected_epoch != plane.coordinator_epoch) {
    return make_error(ErrorCode::STALE_EPOCH,
                      "message carries coordinator epoch " + expected_epoch.str() +
                          " but the current epoch is " + plane.coordinator_epoch.str());
  }
  if (incarnation.valid() && incarnation != existing->location.cluster_incarnation) {
    return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                      "message names cluster incarnation " + incarnation.str() +
                          " but the replica was written under incarnation " +
                          existing->location.cluster_incarnation.str());
  }
  const ClusterRecord* cluster = find_cluster(plane, existing->location.cluster_id);
  if (cluster == nullptr) {
    return make_error(ErrorCode::UNKNOWN_CLUSTER, "the hosting cluster is not registered");
  }
  if (!cluster->live_authority || cluster->incarnation != existing->location.cluster_incarnation) {
    return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                      "cluster " + existing->location.cluster_id.str() +
                          " is no longer at the incarnation that hosted this replica");
  }
  if (worker_boot.valid() && existing->worker_boot_id.valid() &&
      worker_boot != existing->worker_boot_id) {
    return make_error(ErrorCode::STALE_WORKER,
                      "message names worker boot " + worker_boot.str() +
                          " but the replica was written under worker boot " +
                          existing->worker_boot_id.str());
  }
  if (existing->is_terminal()) {
    return make_error(ErrorCode::ILLEGAL_TRANSITION,
                      std::string("replica is ") + to_string(existing->lifecycle) +
                          " and accepts no further transitions");
  }
  *out = *existing;
  return Status{};
}

Status require_state(const detail::ControlPlane& plane, StateId state_id,
                     StateGeneration generation, const StateGenerationRecord** out) {
  const StateGenerationRecord* state = find_state(plane, state_id, generation);
  if (state == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
  }
  *out = state;
  return Status{};
}

}  // namespace

Result<ReplicaRecord> Fabric::register_replica(const ReplicaRegistration& registration) {
  return impl_->mutate<ReplicaRecord>(
      [&registration](const MutationInputs& inputs, ReplicaRecord* out) -> Status {
        const StateGenerationRecord* state = nullptr;
        CCSF_TRY(require_state(inputs.plane, registration.state_id, registration.state_generation,
                               &state));
        if (registration.expected_coordinator_epoch.valid() &&
            registration.expected_coordinator_epoch != inputs.plane.coordinator_epoch) {
          return make_error(ErrorCode::STALE_EPOCH, "registration carries a superseded epoch");
        }
        switch (registration.lifecycle) {
          case ReplicaLifecycle::PLANNED:
          case ReplicaLifecycle::RESERVED:
          case ReplicaLifecycle::STAGING:
          case ReplicaLifecycle::TRANSFERRING:
            break;
          default:
            return make_error(ErrorCode::ILLEGAL_TRANSITION,
                              "a replica may only be registered as planned, reserved, staging or "
                              "transferring; authority requires an explicit commit");
        }
        ClusterId cluster_id = registration.location.cluster_id;
        if (!cluster_id.valid()) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "a hosting cluster identity is required");
        }
        const ClusterRecord* cluster = find_cluster(inputs.plane, cluster_id);
        if (cluster == nullptr) {
          return make_error(ErrorCode::UNKNOWN_CLUSTER, "the hosting cluster is not registered");
        }
        if (!cluster->live_authority) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "cluster " + cluster_id.str() + " does not hold live authority");
        }
        ClusterIncarnationId incarnation = registration.location.cluster_incarnation;
        if (!incarnation.valid()) {
          incarnation = cluster->incarnation;
        }
        if (incarnation != cluster->incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "the requested cluster incarnation is not the live incarnation");
        }
        if (const ReplicaRecord* existing = find_replica(inputs.plane, registration.replica_id);
            existing != nullptr) {
          if (existing->state_id == registration.state_id &&
              existing->state_generation == registration.state_generation &&
              existing->location.cluster_id == cluster_id) {
            *out = *existing;
            return Status{};
          }
          return make_error(ErrorCode::DUPLICATE_CONFLICT,
                            "a replica with that identity already exists");
        }

        std::uint64_t next = inputs.plane.next_id;
        ReplicaRecord record;
        record.replica_id = registration.replica_id.valid()
                                ? registration.replica_id
                                : ReplicaId::from_value(next++);
        if (const ReplicaRecord* clash = find_replica(inputs.plane, record.replica_id);
            clash != nullptr) {
          return make_error(ErrorCode::DUPLICATE_CONFLICT, "replica identity collision");
        }
        record.replica_generation = registration.replica_generation.valid()
                                        ? registration.replica_generation
                                        : ReplicaGeneration::from_value(1);
        record.state_id = registration.state_id;
        record.state_generation = registration.state_generation;
        const ReplicaSetRecord* set =
            find_replica_set(inputs.plane, registration.state_id, registration.state_generation);
        record.replica_set_id = registration.replica_set_id.valid()
                                    ? registration.replica_set_id
                                    : (set != nullptr ? set->replica_set_id : ReplicaSetId{});
        record.replica_set_generation = set != nullptr
                                            ? set->generation
                                            : registration.replica_set_generation;
        record.location.cluster_id = cluster_id;
        record.location.cluster_incarnation = incarnation;
        record.location.region = registration.location.region.valid()
                                     ? registration.location.region
                                     : cluster->location.region;
        record.location.site = registration.location.site.valid() ? registration.location.site
                                                                  : cluster->location.site;
        record.location.failure_domain = registration.location.failure_domain.valid()
                                             ? registration.location.failure_domain
                                             : cluster->location.failure_domain;
        record.location.descriptor = registration.location.descriptor.empty()
                                         ? cluster->location.descriptor
                                         : registration.location.descriptor;
        record.lifecycle = registration.lifecycle;
        record.integrity = ReplicaIntegrityState::UNVERIFIED;
        record.compatibility = ReplicaCompatibilityState::UNKNOWN;
        switch (registration.durability) {
          case DurabilityClass::VOLATILE: record.durability = ReplicaDurabilityState::VOLATILE; break;
          case DurabilityClass::EPHEMERAL:
          case DurabilityClass::STANDARD: record.durability = ReplicaDurabilityState::STANDARD; break;
          case DurabilityClass::DURABLE: record.durability = ReplicaDurabilityState::DURABLE; break;
          case DurabilityClass::ARCHIVAL: record.durability = ReplicaDurabilityState::ARCHIVAL; break;
          case DurabilityClass::UNKNOWN: record.durability = ReplicaDurabilityState::UNKNOWN; break;
        }
        record.availability = ReplicaAvailabilityState::UNKNOWN;
        record.authority = ReplicaAuthorityState::CANDIDATE;
        record.content_digest = registration.content_digest.valid()
                                    ? registration.content_digest
                                    : state->content_digest;
        record.digest_algorithm = registration.digest_algorithm;
        record.compatibility_id = registration.compatibility_id.valid()
                                      ? registration.compatibility_id
                                      : state->compatibility_id;
        record.compatibility_generation = registration.compatibility_generation;
        record.compatibility_descriptor = registration.compatibility_descriptor;
        record.logical_size = registration.logical_size.known() ? registration.logical_size
                                                                : state->logical_size;
        record.transferred_bytes = SizeValue::of(0);
        record.provenance = registration.provenance;
        record.worker_id = registration.worker_id.valid() ? registration.worker_id
                                                          : cluster->worker_id;
        record.worker_boot_id = registration.worker_boot_id.valid()
                                    ? registration.worker_boot_id
                                    : cluster->worker_boot_id;
        record.created_epoch = inputs.plane.coordinator_epoch;
        record.last_epoch = inputs.plane.coordinator_epoch;
        record.created_at = inputs.mutation.tick;
        record.updated_at = inputs.mutation.tick;
        record.transfer_id = registration.transfer_id;
        record.transfer_generation = registration.transfer_generation;
        record.operation_id = registration.operation_id;
        record.operation_generation = registration.operation_generation;

        FabricCounters counters = inputs.plane.counters;
        ++counters.replicas_registered;

        inputs.mutation.effects.push_back(detail::Effect::put_replica(record));
        inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, record.state_id,
                                                   record.state_generation, inputs.mutation.tick,
                                                   inputs.plane.coordinator_epoch));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
        *out = record;
        return Status{};
      });
}

Result<ReplicaRecord> Fabric::record_transfer_progress(const TransferProgress& progress) {
  return impl_->mutate<ReplicaRecord>(
      [&progress](const MutationInputs& inputs, ReplicaRecord* out) -> Status {
        ReplicaRecord replica;
        CCSF_TRY(guard_replica(inputs.plane, progress.replica_id, progress.replica_generation,
                               progress.cluster_incarnation, progress.worker_boot_id,
                               progress.expected_coordinator_epoch, &replica));
        if (replica.transfer_id.valid() && progress.transfer_id.valid() &&
            progress.transfer_id != replica.transfer_id) {
          return make_error(ErrorCode::STALE_TRANSFER_GENERATION,
                            "progress names a transfer that is not the current transfer");
        }
        if (replica.transfer_generation.valid() && progress.transfer_generation.valid() &&
            progress.transfer_generation != replica.transfer_generation) {
          return make_error(ErrorCode::STALE_TRANSFER_GENERATION,
                            "progress names a superseded transfer generation");
        }
        if (replica.operation_id.valid() && progress.operation_id.valid() &&
            progress.operation_id != replica.operation_id) {
          return make_error(ErrorCode::STALE_OPERATION_GENERATION,
                            "progress names an operation that does not own this replica");
        }
        switch (replica.lifecycle) {
          case ReplicaLifecycle::PLANNED:
          case ReplicaLifecycle::RESERVED:
          case ReplicaLifecycle::STAGING:
          case ReplicaLifecycle::TRANSFERRING:
            break;
          default:
            return make_error(ErrorCode::ILLEGAL_TRANSITION,
                              std::string("a replica in lifecycle ") +
                                  to_string(replica.lifecycle) + " accepts no transfer progress");
        }

        FabricCounters counters = inputs.plane.counters;
        ReplicaRecord revised = replica;
        revised.last_epoch = inputs.plane.coordinator_epoch;
        revised.updated_at = inputs.mutation.tick;
        if (replica.lifecycle != ReplicaLifecycle::TRANSFERRING) {
          ++counters.transfers_started;
        }
        if (progress.failed) {
          revised.lifecycle = ReplicaLifecycle::ABANDONED;
          revised.authority = ReplicaAuthorityState::NONE;
          revised.counts_toward_factor = false;
          revised.availability = ReplicaAvailabilityState::ABSENT;
          revised.last_error = progress.failure_reason;
          ++counters.operations_failed;
          ++counters.replicas_abandoned;
          if (const OperationRecord* operation =
                  find_operation(inputs.plane, progress.operation_id);
              operation != nullptr && !operation->is_terminal()) {
            OperationRecord updated = *operation;
            updated.state = OperationState::FAILED;
            updated.last_error = progress.failure_reason;
            updated.last_epoch = inputs.plane.coordinator_epoch;
            updated.updated_at = inputs.mutation.tick;
            if (updated.journal.size() < kMaxOperationJournalEntries) {
              updated.journal.push_back("transfer failed: " + progress.failure_reason);
            }
            inputs.mutation.effects.push_back(detail::Effect::put_operation(updated));
          }
        } else {
          revised.lifecycle = ReplicaLifecycle::TRANSFERRING;
          revised.transferred_bytes = SizeValue::of(progress.bytes_transferred);
          advance_operation(inputs, progress.operation_id, progress.operation_generation,
                            OperationState::TRANSFERRING, "transfer in progress");
        }
        inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
        inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                   revised.state_generation, inputs.mutation.tick,
                                                   inputs.plane.coordinator_epoch));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = revised;
        if (progress.failed) {
          inputs.mutation.commit_before_return = true;
          return make_error(ErrorCode::TRANSFER_FAILED, progress.failure_reason);
        }
        return Status{};
      });
}

Result<ReplicaRecord> Fabric::record_transfer_completion(const TransferCompletion& completion) {
  return impl_->mutate<ReplicaRecord>(
      [&completion](const MutationInputs& inputs, ReplicaRecord* out) -> Status {
        ReplicaRecord replica;
        CCSF_TRY(guard_replica(inputs.plane, completion.replica_id, completion.replica_generation,
                               completion.cluster_incarnation, completion.worker_boot_id,
                               completion.expected_coordinator_epoch, &replica));
        if (replica.transfer_id.valid() && completion.transfer_id.valid() &&
            completion.transfer_id != replica.transfer_id) {
          return make_error(ErrorCode::STALE_TRANSFER_GENERATION,
                            "completion names a transfer that is not the current transfer");
        }
        if (replica.transfer_generation.valid() && completion.transfer_generation.valid() &&
            completion.transfer_generation != replica.transfer_generation) {
          return make_error(ErrorCode::STALE_TRANSFER_GENERATION,
                            "completion names a superseded transfer generation");
        }
        if (!completion.transfer_complete) {
          return make_error(ErrorCode::TRANSFER_INCOMPLETE,
                            "a transfer completion must assert that the transfer finished");
        }
        switch (replica.lifecycle) {
          case ReplicaLifecycle::PLANNED:
          case ReplicaLifecycle::RESERVED:
          case ReplicaLifecycle::STAGING:
          case ReplicaLifecycle::TRANSFERRING:
          case ReplicaLifecycle::TRANSFERRED:
          case ReplicaLifecycle::VERIFYING:
            break;
          default:
            return make_error(ErrorCode::ILLEGAL_TRANSITION,
                              std::string("a replica in lifecycle ") +
                                  to_string(replica.lifecycle) +
                                  " accepts no transfer completion");
        }

        const StateGenerationRecord* state =
            find_state(inputs.plane, replica.state_id, replica.state_generation);
        FabricCounters counters = inputs.plane.counters;
        ReplicaRecord revised = replica;
        revised.last_epoch = inputs.plane.coordinator_epoch;
        revised.updated_at = inputs.mutation.tick;
        revised.measured_digest = completion.measured_digest;
        revised.digest_algorithm = completion.digest_algorithm;
        revised.transferred_bytes = SizeValue::of(completion.bytes_transferred);
        if (replica.lifecycle != ReplicaLifecycle::TRANSFERRED &&
            replica.lifecycle != ReplicaLifecycle::VERIFYING) {
          ++counters.transfers_completed;
          counters.bytes_transferred += completion.bytes_transferred;
        }

        const bool digest_known = state != nullptr && state->content_digest.valid();
        if (!completion.measured_digest.valid()) {
          revised.integrity = ReplicaIntegrityState::UNKNOWN;
          revised.lifecycle = ReplicaLifecycle::TRANSFERRED;
          inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
          inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                     revised.state_generation, inputs.mutation.tick,
                                                     inputs.plane.coordinator_epoch));
          inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
          *out = revised;
          inputs.mutation.commit_before_return = true;
          return make_error(ErrorCode::INTEGRITY_UNKNOWN,
                            "a transfer completion carries no measured digest; the destination "
                            "cannot be verified");
        }
        if (digest_known && completion.measured_digest != state->content_digest) {
          revised.integrity = ReplicaIntegrityState::MISMATCH;
          revised.lifecycle = ReplicaLifecycle::CORRUPT;
          revised.authority = ReplicaAuthorityState::FENCED;
          revised.counts_toward_factor = false;
          revised.availability = ReplicaAvailabilityState::PRESENT;
          revised.last_error = "measured digest does not match the state content digest";
          ++counters.integrity_failures;
          inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
          inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                     revised.state_generation, inputs.mutation.tick,
                                                     inputs.plane.coordinator_epoch));
          inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
          *out = revised;
          inputs.mutation.commit_before_return = true;
          return make_error(ErrorCode::INTEGRITY_MISMATCH,
                            "transferred bytes do not match the authoritative content digest");
        }
        revised.integrity = ReplicaIntegrityState::UNVERIFIED;
        revised.lifecycle = ReplicaLifecycle::TRANSFERRED;
        revised.availability = ReplicaAvailabilityState::PRESENT;
        advance_operation(inputs, completion.operation_id, completion.operation_generation,
                          OperationState::TRANSFERRED, "transfer complete at the destination");
        inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
        inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                   revised.state_generation, inputs.mutation.tick,
                                                   inputs.plane.coordinator_epoch));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = revised;
        return Status{};
      });
}

Result<ReplicaRecord> Fabric::verify_replica(const ReplicaVerification& verification) {
  return impl_->mutate<ReplicaRecord>(
      [&verification](const MutationInputs& inputs, ReplicaRecord* out) -> Status {
        ReplicaRecord replica;
        CCSF_TRY(guard_replica(inputs.plane, verification.replica_id,
                               verification.replica_generation, verification.cluster_incarnation,
                               verification.worker_boot_id,
                               verification.expected_coordinator_epoch, &replica));
        switch (replica.lifecycle) {
          case ReplicaLifecycle::TRANSFERRED:
          case ReplicaLifecycle::VERIFYING:
          case ReplicaLifecycle::PREPARED:
          case ReplicaLifecycle::AUTHORITATIVE:
          case ReplicaLifecycle::DEGRADED:
          case ReplicaLifecycle::REVALIDATION_REQUIRED:
            break;
          default:
            return make_error(ErrorCode::ILLEGAL_TRANSITION,
                              std::string("a replica in lifecycle ") +
                                  to_string(replica.lifecycle) +
                                  " cannot be verified before its bytes are present");
        }
        const StateGenerationRecord* state =
            find_state(inputs.plane, replica.state_id, replica.state_generation);
        FabricCounters counters = inputs.plane.counters;
        ++counters.integrity_verifications;
        std::uint64_t next = inputs.plane.next_id;

        ReplicaRecord revised = replica;
        revised.last_epoch = inputs.plane.coordinator_epoch;
        revised.updated_at = inputs.mutation.tick;
        revised.evidence.verification_id = verification.verification_id.valid()
                                               ? verification.verification_id
                                               : VerificationId{};
        revised.evidence.verified_at = inputs.mutation.tick;
        revised.evidence.coordinator_epoch = inputs.plane.coordinator_epoch;
        revised.measured_digest = verification.measured_digest;

        auto persist_and_fail = [&](ErrorCode code, std::string message) -> Status {
          advance_operation(inputs, verification.operation_id, verification.operation_generation,
                            OperationState::FAILED, message.c_str());
          inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
          inputs.mutation.effects.push_back(bump_replica_set_effect(
              inputs.plane, revised.state_id, revised.state_generation, inputs.mutation.tick,
              inputs.plane.coordinator_epoch));
          inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
          inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
          *out = revised;
          inputs.mutation.commit_before_return = true;
          return make_error(code, std::move(message));
        };

        if (!verification.integrity_verified) {
          revised.integrity = ReplicaIntegrityState::MISMATCH;
          revised.lifecycle = ReplicaLifecycle::CORRUPT;
          revised.authority = ReplicaAuthorityState::FENCED;
          revised.counts_toward_factor = false;
          revised.last_error = "integrity verification reported a mismatch";
          ++counters.integrity_failures;
          return persist_and_fail(ErrorCode::INTEGRITY_MISMATCH,
                                  "the destination failed integrity verification");
        }
        const Digest expected = state != nullptr ? state->content_digest : Digest{};
        if (expected.valid() && verification.measured_digest.valid() &&
            verification.measured_digest != expected) {
          revised.integrity = ReplicaIntegrityState::MISMATCH;
          revised.lifecycle = ReplicaLifecycle::CORRUPT;
          revised.authority = ReplicaAuthorityState::FENCED;
          revised.counts_toward_factor = false;
          revised.last_error = "measured digest does not match the state content digest";
          ++counters.integrity_failures;
          return persist_and_fail(ErrorCode::INTEGRITY_MISMATCH,
                                  "the destination digest does not match the authoritative digest");
        }
        if (expected.valid() && !verification.measured_digest.valid()) {
          revised.integrity = ReplicaIntegrityState::UNVERIFIED;
          revised.lifecycle = ReplicaLifecycle::TRANSFERRED;
          revised.last_error = "verification carried no measured digest";
          return persist_and_fail(ErrorCode::INTEGRITY_UNKNOWN,
                                  "verification carries no measured digest to compare");
        }
        revised.integrity = ReplicaIntegrityState::VERIFIED;
        revised.evidence.integrity_evidence_present = true;
        revised.evidence.integrity_generation = IntegrityGeneration::from_value(next++);
        revised.evidence.evidence_generation = EvidenceGeneration::from_value(next++);
        revised.evidence.capability_generation = CapabilityGeneration::from_value(next++);
        revised.durability = static_cast<ReplicaDurabilityState>(
            std::max(static_cast<std::uint16_t>(revised.durability),
                     static_cast<std::uint16_t>([&verification]() {
                       switch (verification.established_durability) {
                         case DurabilityClass::ARCHIVAL:
                           return ReplicaDurabilityState::ARCHIVAL;
                         case DurabilityClass::DURABLE:
                           return ReplicaDurabilityState::DURABLE;
                         case DurabilityClass::STANDARD:
                         case DurabilityClass::EPHEMERAL:
                           return ReplicaDurabilityState::STANDARD;
                         case DurabilityClass::VOLATILE:
                           return ReplicaDurabilityState::VOLATILE;
                         case DurabilityClass::UNKNOWN:
                           return ReplicaDurabilityState::UNKNOWN;
                       }
                       return ReplicaDurabilityState::UNKNOWN;
                     }())));

        const CompatibilityRequirement requirement =
            state != nullptr
                ? CompatibilityRequirement{state->compatibility_id, state->compatibility_generation,
                                           state->compatibility_required, true}
                : CompatibilityRequirement{};
        const CompatibilityDescriptor* descriptor = nullptr;
        CompatibilityGeneration descriptor_generation;
        if (!verification.compatibility_descriptor.empty()) {
          descriptor = &verification.compatibility_descriptor;
          descriptor_generation = verification.compatibility_generation.valid()
                                      ? verification.compatibility_generation
                                      : requirement.generation;
        } else if (!replica.compatibility_descriptor.empty()) {
          descriptor = &replica.compatibility_descriptor;
          descriptor_generation = replica.compatibility_generation;
        } else if (const ClusterRecord* cluster =
                       find_cluster(inputs.plane, replica.location.cluster_id);
                   cluster != nullptr) {
          CompatibilityDescriptor built = cluster_compatibility_descriptor(*cluster);
          if (!requirement.required.empty()) {
            const CompatibilityResult built_result = evaluate_compatibility(
                requirement, &built, requirement.generation, false);
            if (built_result == CompatibilityResult::INCOMPATIBLE) {
              revised.compatibility = ReplicaCompatibilityState::INCOMPATIBLE;
              revised.lifecycle = ReplicaLifecycle::QUARANTINED;
              revised.authority = ReplicaAuthorityState::FENCED;
              revised.counts_toward_factor = false;
              revised.last_error = "cluster capabilities are incompatible with the state";
              ++counters.compatibility_rejections;
              return persist_and_fail(ErrorCode::INCOMPATIBLE,
                                      "the destination cluster is incompatible with the state "
                                      "compatibility identity");
            }
            if (built_result != CompatibilityResult::COMPATIBLE) {
              revised.compatibility = ReplicaCompatibilityState::UNKNOWN;
              revised.lifecycle = ReplicaLifecycle::REVALIDATION_REQUIRED;
              revised.last_error = "cluster capabilities do not cover the compatibility requirement";
              return persist_and_fail(ErrorCode::COMPATIBILITY_UNKNOWN,
                                      "compatibility evidence is incomplete for the destination "
                                      "cluster");
            }
          }
          revised.compatibility_descriptor = built;
          revised.compatibility_generation = requirement.generation;
          revised.compatibility_id = state != nullptr ? state->compatibility_id : CompatibilityId{};
          revised.compatibility = ReplicaCompatibilityState::COMPATIBLE;
          revised.evidence.compatibility_evidence_present = true;
          revised.evidence.compatibility_generation = requirement.generation;
          revised.lifecycle = ReplicaLifecycle::PREPARED;
          revised.availability = ReplicaAvailabilityState::PRESENT;
          revised.evidence.revalidation_required = false;
          advance_operation(inputs, verification.operation_id, verification.operation_generation,
                            OperationState::PREPARED,
                            "destination prepared under explicit authority");
          inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
          inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                     revised.state_generation, inputs.mutation.tick,
                                                     inputs.plane.coordinator_epoch));
          inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
          inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
          *out = revised;
          return Status{};
        }

        const CompatibilityResult result =
            evaluate_compatibility(requirement, descriptor, descriptor_generation, false);
        switch (result) {
          case CompatibilityResult::COMPATIBLE:
            revised.compatibility = ReplicaCompatibilityState::COMPATIBLE;
            break;
          case CompatibilityResult::INCOMPATIBLE:
            revised.compatibility = ReplicaCompatibilityState::INCOMPATIBLE;
            revised.lifecycle = ReplicaLifecycle::QUARANTINED;
            revised.authority = ReplicaAuthorityState::FENCED;
            revised.counts_toward_factor = false;
            revised.last_error = "destination is incompatible with the state requirement";
            ++counters.compatibility_rejections;
            return persist_and_fail(
                ErrorCode::INCOMPATIBLE,
                describe_compatibility(requirement, result, descriptor));
          case CompatibilityResult::UNKNOWN:
            revised.compatibility = ReplicaCompatibilityState::UNKNOWN;
            revised.lifecycle = ReplicaLifecycle::REVALIDATION_REQUIRED;
            revised.last_error = "compatibility evidence is incomplete";
            return persist_and_fail(
                ErrorCode::COMPATIBILITY_UNKNOWN,
                describe_compatibility(requirement, result, descriptor) +
                    "; UNKNOWN is never treated as compatible");
          case CompatibilityResult::REVALIDATION_REQUIRED:
            revised.compatibility = ReplicaCompatibilityState::REVALIDATION_REQUIRED;
            revised.lifecycle = ReplicaLifecycle::REVALIDATION_REQUIRED;
            revised.last_error = "compatibility evidence requires revalidation";
            return persist_and_fail(ErrorCode::REVALIDATION_REQUIRED,
                                    "compatibility evidence requires revalidation");
        }
        revised.compatibility_generation = descriptor_generation;
        revised.evidence.compatibility_evidence_present = true;
        revised.evidence.compatibility_generation = descriptor_generation;
        if (descriptor != nullptr) {
          revised.compatibility_descriptor = *descriptor;
        }
        revised.lifecycle = ReplicaLifecycle::PREPARED;
        revised.availability = ReplicaAvailabilityState::PRESENT;
        revised.evidence.revalidation_required = false;
        revised.evidence.health_generation = HealthGeneration::from_value(next++);
        advance_operation(inputs, verification.operation_id, verification.operation_generation,
                          OperationState::PREPARED,
                          "destination prepared under explicit authority");
        inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
        inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                   revised.state_generation, inputs.mutation.tick,
                                                   inputs.plane.coordinator_epoch));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
        *out = revised;
        return Status{};
      });
}

Result<ReplicaRecord> Fabric::commit_replica(const ReplicaCommit& commit) {
  return impl_->mutate<ReplicaRecord>(
      [&commit](const MutationInputs& inputs, ReplicaRecord* out) -> Status {
        if (!commit.commit_id.valid()) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "a commit identity is required");
        }
        const ReplicaRecord* existing = find_replica(inputs.plane, commit.replica_id);
        if (existing == nullptr) {
          return make_error(ErrorCode::UNKNOWN_REPLICA, "the replica is not registered");
        }
        if (commit.replica_generation.valid() &&
            commit.replica_generation != existing->replica_generation) {
          return make_error(ErrorCode::STALE_REPLICA_GENERATION,
                            "commit names a superseded replica generation");
        }
        if (commit.expected_coordinator_epoch.valid() &&
            commit.expected_coordinator_epoch != inputs.plane.coordinator_epoch) {
          return make_error(ErrorCode::STALE_EPOCH, "commit carries a superseded coordinator epoch");
        }
        if (existing->commit_recorded) {
          *out = *existing;
          if (existing->commit_id == commit.commit_id) {
            return Status{};
          }
          return make_error(ErrorCode::COMMIT_CONFLICT,
                            "the replica is already committed under a different commit identity");
        }
        const StateGenerationRecord* state =
            find_state(inputs.plane, existing->state_id, existing->state_generation);
        if (state == nullptr) {
          return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
        }
        const ClusterRecord* cluster = find_cluster(inputs.plane, existing->location.cluster_id);
        if (cluster == nullptr) {
          return make_error(ErrorCode::UNKNOWN_CLUSTER, "the hosting cluster is not registered");
        }
        if (!cluster->live_authority ||
            cluster->incarnation != existing->location.cluster_incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "the hosting cluster incarnation is no longer live");
        }
        const ReplicaSetRecord* set =
            find_replica_set(inputs.plane, existing->state_id, existing->state_generation);
        if (set == nullptr) {
          return make_error(ErrorCode::UNKNOWN_STATE, "the replica set for this generation is gone");
        }
        if (commit.expected_replica_set_generation.valid() &&
            commit.expected_replica_set_generation != set->generation) {
          return make_error(ErrorCode::STALE_REPLICA_SET_GENERATION,
                            "commit names replica set generation " +
                                commit.expected_replica_set_generation.str() +
                                " but the current generation is " + set->generation.str());
        }
        switch (existing->lifecycle) {
          case ReplicaLifecycle::PREPARED:
          case ReplicaLifecycle::AUTHORITATIVE:
          case ReplicaLifecycle::DEGRADED:
            break;
          default:
            return make_error(ErrorCode::TRANSFER_INCOMPLETE,
                              std::string("a replica in lifecycle ") +
                                  to_string(existing->lifecycle) +
                                  " cannot be committed as authoritative");
        }
        if (commit.require_integrity_verified &&
            existing->integrity != ReplicaIntegrityState::VERIFIED) {
          return make_error(ErrorCode::INTEGRITY_UNKNOWN,
                            "the replica has no verified integrity evidence");
        }
        if (commit.require_compatibility_verified &&
            existing->compatibility != ReplicaCompatibilityState::COMPATIBLE) {
          return make_error(ErrorCode::COMPATIBILITY_UNKNOWN,
                            "the replica has no verified compatibility evidence");
        }
        if (existing->evidence.revalidation_required) {
          return make_error(ErrorCode::REVALIDATION_REQUIRED,
                            "the replica requires revalidation before it can be committed");
        }

        const ReplicationPolicyRecord* policy =
            resolve_replication_policy(inputs.plane, inputs.config, ReplicationPolicyId{});
        const std::uint32_t maximum =
            policy != nullptr ? policy->retention.maximum_replicas_per_state : 8U;
        if (detail::rebuild_replica_set(inputs.plane, *set).authoritative_count >= maximum) {
          return make_error(ErrorCode::OVER_REPLICATED,
                            "the replica set already holds the maximum permitted authoritative "
                            "replicas");
        }

        FabricCounters counters = inputs.plane.counters;
        ++counters.replicas_committed;
        ReplicaRecord revised;
        bool ownership_written = false;
        CCSF_TRY(plan_authoritative_commit(inputs, *existing, *state, *set, commit.commit_id,
                                           commit.make_authoritative_owner, false, &revised,
                                           &ownership_written));
        if (ownership_written) {
          ++counters.ownership_transfers;
        }
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = revised;
        return Status{};
      });
}

Result<ReplicaRecord> Fabric::retire_replica(const ReplicaTransition& transition) {
  return impl_->mutate<ReplicaRecord>(
      [&transition](const MutationInputs& inputs, ReplicaRecord* out) -> Status {
        ReplicaRecord replica;
        CCSF_TRY(guard_replica(inputs.plane, transition.replica_id,
                               transition.replica_generation, transition.cluster_incarnation,
                               transition.worker_boot_id, transition.expected_coordinator_epoch,
                               &replica));
        const OwnershipRecord* ownership =
            find_ownership(inputs.plane, replica.state_id, replica.state_generation);
        if (ownership != nullptr && ownership->active &&
            ownership->owner_replica == replica.replica_id) {
          return make_error(ErrorCode::AUTHORITY_NOT_ESTABLISHED,
                            "the authoritative owner must be handed over before it is retired");
        }
        FabricCounters counters = inputs.plane.counters;
        ReplicaRecord revised = replica;
        revised.lifecycle = ReplicaLifecycle::RETIRED;
        revised.authority = ReplicaAuthorityState::HISTORICAL;
        revised.counts_toward_factor = false;
        revised.availability = ReplicaAvailabilityState::ABSENT;
        revised.last_epoch = inputs.plane.coordinator_epoch;
        revised.updated_at = inputs.mutation.tick;
        revised.last_error = transition.reason;
        ++counters.replicas_retired;
        inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
        inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                   revised.state_generation, inputs.mutation.tick,
                                                   inputs.plane.coordinator_epoch, true));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = revised;
        return Status{};
      });
}

Result<ReplicaRecord> Fabric::quarantine_replica(const ReplicaTransition& transition) {
  return impl_->mutate<ReplicaRecord>(
      [&transition](const MutationInputs& inputs, ReplicaRecord* out) -> Status {
        ReplicaRecord replica;
        CCSF_TRY(guard_replica(inputs.plane, transition.replica_id,
                               transition.replica_generation, transition.cluster_incarnation,
                               transition.worker_boot_id, transition.expected_coordinator_epoch,
                               &replica));
        FabricCounters counters = inputs.plane.counters;
        ReplicaRecord revised = replica;
        revised.lifecycle = ReplicaLifecycle::QUARANTINED;
        revised.authority = ReplicaAuthorityState::FENCED;
        revised.counts_toward_factor = false;
        revised.last_epoch = inputs.plane.coordinator_epoch;
        revised.updated_at = inputs.mutation.tick;
        revised.last_error = transition.reason;
        ++counters.replicas_quarantined;
        inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
        inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                   revised.state_generation, inputs.mutation.tick,
                                                   inputs.plane.coordinator_epoch, true));
        inputs.mutation.effects.push_back(detail::Effect::put_counters(counters));
        *out = revised;
        return Status{};
      });
}

Result<ReplicaRecord> Fabric::revalidate_replica(const ReplicaTransition& transition) {
  return impl_->mutate<ReplicaRecord>(
      [&transition](const MutationInputs& inputs, ReplicaRecord* out) -> Status {
        // Revalidation is deliberately allowed to cross a cluster incarnation or
        // worker boot boundary: the durable bytes survive, live authority does
        // not. The replica is rebound to the incarnation that is current now and
        // only then may it count as authoritative again.
        const ReplicaRecord* existing = find_replica(inputs.plane, transition.replica_id);
        if (existing == nullptr) {
          return make_error(ErrorCode::UNKNOWN_REPLICA, "the replica is not registered");
        }
        if (transition.replica_generation.valid() &&
            transition.replica_generation != existing->replica_generation) {
          return make_error(ErrorCode::STALE_REPLICA_GENERATION,
                            "revalidation names a superseded replica generation");
        }
        if (transition.expected_coordinator_epoch.valid() &&
            transition.expected_coordinator_epoch != inputs.plane.coordinator_epoch) {
          return make_error(ErrorCode::STALE_EPOCH,
                            "revalidation carries a superseded coordinator epoch");
        }
        if (existing->is_terminal()) {
          return make_error(ErrorCode::ILLEGAL_TRANSITION,
                            std::string("replica is ") + to_string(existing->lifecycle) +
                                " and cannot be revalidated");
        }
        const ClusterRecord* bound_cluster =
            find_cluster(inputs.plane, existing->location.cluster_id);
        if (bound_cluster == nullptr || !bound_cluster->live_authority) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "the hosting cluster holds no live authority");
        }
        if (transition.cluster_incarnation.valid() &&
            transition.cluster_incarnation != bound_cluster->incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "revalidation names a cluster incarnation that is not live");
        }
        ReplicaRecord replica = *existing;
        const bool rebinding =
            replica.location.cluster_incarnation != bound_cluster->incarnation ||
            (replica.worker_boot_id.valid() && bound_cluster->worker_boot_id.valid() &&
             replica.worker_boot_id != bound_cluster->worker_boot_id);
        if (rebinding) {
          replica.location.cluster_incarnation = bound_cluster->incarnation;
          replica.worker_boot_id = bound_cluster->worker_boot_id;
        }
        if (replica.integrity == ReplicaIntegrityState::MISMATCH ||
            replica.integrity == ReplicaIntegrityState::CORRUPT ||
            replica.lifecycle == ReplicaLifecycle::CORRUPT) {
          return make_error(ErrorCode::INTEGRITY_MISMATCH,
                            "a corrupt replica must be re-verified, not revalidated");
        }
        if (replica.integrity != ReplicaIntegrityState::VERIFIED) {
          return make_error(ErrorCode::INTEGRITY_UNKNOWN,
                            "a replica without verified integrity cannot be revalidated into "
                            "authority");
        }
        const ClusterRecord* cluster = find_cluster(inputs.plane, replica.location.cluster_id);
        if (cluster == nullptr || !cluster->live_authority ||
            cluster->incarnation != replica.location.cluster_incarnation) {
          return make_error(ErrorCode::STALE_CLUSTER_INCARNATION,
                            "the hosting cluster incarnation is not live");
        }
        const StateGenerationRecord* state =
            find_state(inputs.plane, replica.state_id, replica.state_generation);
        if (state == nullptr) {
          return make_error(ErrorCode::UNKNOWN_STATE, "the state generation is not registered");
        }
        if (replica.compatibility != ReplicaCompatibilityState::COMPATIBLE) {
          const CompatibilityRequirement requirement{state->compatibility_id,
                                                     state->compatibility_generation,
                                                     state->compatibility_required, true};
          const CompatibilityDescriptor built = cluster_compatibility_descriptor(*cluster);
          if (evaluate_compatibility(requirement, &built, requirement.generation, false) !=
              CompatibilityResult::COMPATIBLE) {
            return make_error(ErrorCode::COMPATIBILITY_UNKNOWN,
                              "compatibility evidence is not current for this cluster");
          }
        }

        const bool was_committed = replica.commit_recorded;
        std::uint64_t next = inputs.plane.next_id;
        ReplicaRecord revised = replica;
        revised.evidence.revalidation_required = false;
        revised.evidence.evidence_generation = EvidenceGeneration::from_value(next++);
        revised.evidence.capability_generation = CapabilityGeneration::from_value(next++);
        revised.evidence.verified_at = inputs.mutation.tick;
        revised.evidence.coordinator_epoch = inputs.plane.coordinator_epoch;
        revised.availability = ReplicaAvailabilityState::PRESENT;
        revised.last_epoch = inputs.plane.coordinator_epoch;
        revised.updated_at = inputs.mutation.tick;
        revised.last_error = rebinding
                                 ? "revalidated and rebound to cluster incarnation " +
                                       bound_cluster->incarnation.str()
                                 : std::string();
        if (rebinding) {
          revised.authority = ReplicaAuthorityState::AUTHORITATIVE_FOR_REUSE;
        }
        if (was_committed) {
          revised.lifecycle = ReplicaLifecycle::AUTHORITATIVE;
          revised.counts_toward_factor = true;
          const OwnershipRecord* ownership =
              find_ownership(inputs.plane, revised.state_id, revised.state_generation);
          revised.authority = (ownership != nullptr && ownership->active &&
                               ownership->owner_replica == revised.replica_id)
                                  ? ReplicaAuthorityState::AUTHORITATIVE_OWNER
                                  : ReplicaAuthorityState::AUTHORITATIVE_FOR_REUSE;
        } else {
          revised.lifecycle = ReplicaLifecycle::PREPARED;
          revised.authority = ReplicaAuthorityState::CANDIDATE;
          revised.counts_toward_factor = false;
        }
        inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
        inputs.mutation.effects.push_back(bump_replica_set_effect(inputs.plane, revised.state_id,
                                                   revised.state_generation, inputs.mutation.tick,
                                                   inputs.plane.coordinator_epoch, true));
        inputs.mutation.effects.push_back(detail::Effect::set_next_id(next));
        *out = revised;
        return Status{};
      });
}

Result<ReplicaRecord> Fabric::query_replica(ReplicaId replica_id) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const ReplicaRecord* record = find_replica(impl_->plane_, replica_id);
  if (record == nullptr) {
    return make_error(ErrorCode::UNKNOWN_REPLICA, "the replica is not registered");
  }
  return *record;
}

Result<std::vector<ReplicaRecord>> Fabric::list_replicas(StateId state_id,
                                                         StateGeneration generation) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  std::vector<ReplicaRecord> result;
  for (const auto& entry : impl_->plane_.replicas) {
    if (entry.second.state_id == state_id && entry.second.state_generation == generation) {
      result.push_back(entry.second);
    }
  }
  return result;
}

// --- replica sets -----------------------------------------------------------

Result<ReplicaSetRecord> Fabric::query_replica_set(StateId state_id,
                                                   StateGeneration generation) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const ReplicaSetRecord* set = find_replica_set(impl_->plane_, state_id, generation);
  if (set == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "no replica set exists for that generation");
  }
  return detail::rebuild_replica_set(impl_->plane_, *set);
}

Result<ReplicaSetReconciliation> Fabric::reconcile_replica_set(StateId state_id,
                                                              StateGeneration generation) const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  const ReplicaSetRecord* set = find_replica_set(impl_->plane_, state_id, generation);
  if (set == nullptr) {
    return make_error(ErrorCode::UNKNOWN_STATE, "no replica set exists for that generation");
  }
  ReplicaSetReconciliation reconciliation;
  reconciliation.replica_set = detail::rebuild_replica_set(impl_->plane_, *set);
  const ReplicationRequirements& requirements = reconciliation.replica_set.requirements;
  if (reconciliation.replica_set.authoritative_count <
      requirements.minimum_authoritative_replicas) {
    reconciliation.under_replicated = true;
    reconciliation.authoritative_shortfall = requirements.minimum_authoritative_replicas -
                                             reconciliation.replica_set.authoritative_count;
  }
  if (reconciliation.replica_set.authoritative_count > requirements.maximum_replicas) {
    reconciliation.over_replicated = true;
  }
  if (reconciliation.replica_set.distinct_clusters < requirements.cluster_diversity) {
    reconciliation.clusters_missing =
        requirements.cluster_diversity - reconciliation.replica_set.distinct_clusters;
  }
  if (reconciliation.replica_set.distinct_regions < requirements.region_diversity) {
    reconciliation.regions_missing =
        requirements.region_diversity - reconciliation.replica_set.distinct_regions;
  }
  for (const ReplicaSetMember& member : reconciliation.replica_set.degraded_members) {
    reconciliation.excluded_replicas.push_back(member.replica_id);
  }
  reconciliation.findings = explain_replica_set(reconciliation.replica_set);
  return reconciliation;
}

Result<std::vector<ReplicaSetRecord>> Fabric::list_replica_sets() const {
  std::shared_lock<std::shared_mutex> guard(impl_->mutex_);
  std::vector<ReplicaSetRecord> result;
  result.reserve(impl_->plane_.replica_sets.size());
  for (const auto& entry : impl_->plane_.replica_sets) {
    result.push_back(detail::rebuild_replica_set(impl_->plane_, entry.second));
  }
  return result;
}

void advance_operation(const MutationInputs& inputs, OperationId operation_id,
                       OperationGeneration generation, OperationState next, const char* note) {
  const OperationRecord* existing = find_operation(inputs.plane, operation_id);
  if (existing == nullptr || existing->is_terminal()) {
    return;
  }
  if (generation.valid() && existing->operation_generation != generation) {
    return;
  }
  OperationRecord updated = *existing;
  updated.state = next;
  updated.last_epoch = inputs.plane.coordinator_epoch;
  updated.updated_at = inputs.mutation.tick;
  if (next == OperationState::PREPARED) {
    updated.destination_verified = true;
  }
  if (updated.journal.size() < kMaxOperationJournalEntries) {
    updated.journal.push_back(note);
  }
  inputs.mutation.effects.push_back(detail::Effect::put_operation(updated));
}

Status plan_authoritative_commit(const MutationInputs& inputs, const ReplicaRecord& target,
                                 const StateGenerationRecord& state, const ReplicaSetRecord& set,
                                 CommitId commit_id, bool make_owner, bool allow_ownership_takeover,
                                 ReplicaRecord* out, bool* ownership_written) {
  const OwnershipRecord* ownership =
      find_ownership(inputs.plane, target.state_id, target.state_generation);
  const bool single_owner = requires_single_authoritative_owner(state.consistency);
  if (single_owner && !make_owner) {
    return make_error(ErrorCode::POLICY_VIOLATION,
                      "state consistency mode " + std::string(to_string(state.consistency)) +
                          " requires an explicit authoritative owner commit");
  }

  ReplicaRecord revised = target;
  revised.lifecycle = ReplicaLifecycle::AUTHORITATIVE;
  revised.integrity = ReplicaIntegrityState::VERIFIED;
  revised.compatibility = ReplicaCompatibilityState::COMPATIBLE;
  revised.availability = ReplicaAvailabilityState::PRESENT;
  revised.commit_recorded = true;
  revised.counts_toward_factor = true;
  revised.commit_id = commit_id;
  revised.replica_set_generation =
      set.generation.valid() ? set.generation.next() : ReplicaSetGeneration::from_value(1);
  revised.last_epoch = inputs.plane.coordinator_epoch;
  revised.updated_at = inputs.mutation.tick;
  revised.last_error.clear();
  revised.provenance.commit = commit_id;

  if (make_owner) {
    if (ownership != nullptr && ownership->active &&
        ownership->owner_replica != target.replica_id) {
      if (!allow_ownership_takeover) {
        return make_error(ErrorCode::SPLIT_BRAIN_RISK,
                          "another replica currently holds authoritative ownership of this state "
                          "generation");
      }
      const ReplicaRecord* previous = find_replica(inputs.plane, ownership->owner_replica);
      if (previous != nullptr && !previous->is_terminal()) {
        ReplicaRecord demoted = *previous;
        demoted.authority = ReplicaAuthorityState::AUTHORITATIVE_FOR_REUSE;
        demoted.last_epoch = inputs.plane.coordinator_epoch;
        demoted.updated_at = inputs.mutation.tick;
        demoted.last_error = "ownership handed over by an authorized migration";
        inputs.mutation.effects.push_back(detail::Effect::put_replica(demoted));
      }
    }
    OwnershipRecord record;
    record.state_id = revised.state_id;
    record.state_generation = revised.state_generation;
    record.owner_replica = revised.replica_id;
    record.owner_replica_generation = revised.replica_generation;
    record.owner_cluster = revised.location.cluster_id;
    record.owner_incarnation = revised.location.cluster_incarnation;
    record.epoch = inputs.plane.coordinator_epoch;
    record.commit = commit_id;
    record.placement_id = set.placement_id;
    record.placement_generation = set.placement_generation;
    record.established_at = inputs.mutation.tick;
    record.active = true;
    revised.authority = ReplicaAuthorityState::AUTHORITATIVE_OWNER;
    inputs.mutation.effects.push_back(detail::Effect::put_ownership(record));
    *ownership_written = true;
  } else {
    revised.authority = ReplicaAuthorityState::AUTHORITATIVE_FOR_REUSE;
    *ownership_written = false;
  }

  inputs.mutation.effects.push_back(detail::Effect::put_replica(revised));
  inputs.mutation.effects.push_back(bump_replica_set_effect(
      inputs.plane, revised.state_id, revised.state_generation, inputs.mutation.tick,
      inputs.plane.coordinator_epoch, true));
  *out = revised;
  return Status{};
}

}  // namespace ccsf
