// Cross-Cluster State Fabric - canonical record encoding.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "detail/encoding.hpp"

#include <utility>

namespace ccsf::detail {
namespace {

void encode_string_list(ByteWriter& writer, const std::vector<std::string>& values) {
  encode_vector(writer, values, [](ByteWriter& w, const std::string& item) { w.str(item); });
}

bool decode_string_list(ByteReader& reader, std::vector<std::string>* out, std::size_t max_count,
                        const char* name) {
  return decode_vector<std::string>(reader, out, max_count, name,
                                    [](ByteReader& r, std::string* item) {
                                      return r.str(kMaxPolicyStringLength, item);
                                    });
}

void encode_id_list(ByteWriter& writer, const std::vector<ClusterId>& values) {
  encode_vector(writer, values, [](ByteWriter& w, const ClusterId& item) { encode_id(w, item); });
}

bool decode_cluster_id_list(ByteReader& reader, std::vector<ClusterId>* out, const char* name) {
  return decode_vector<ClusterId>(reader, out, kMaxPolicyListEntries, name,
                                  [](ByteReader& r, ClusterId* item) { return decode_id(r, item); });
}

void encode_region_list(ByteWriter& writer, const std::vector<RegionId>& values) {
  encode_vector(writer, values, [](ByteWriter& w, const RegionId& item) { encode_id(w, item); });
}

bool decode_region_id_list(ByteReader& reader, std::vector<RegionId>* out, const char* name) {
  return decode_vector<RegionId>(reader, out, kMaxPolicyListEntries, name,
                                 [](ByteReader& r, RegionId* item) { return decode_id(r, item); });
}

}  // namespace

void encode(ByteWriter& writer, const CostValue& value) {
  encode_enum(writer, value.source);
  writer.i64(value.micro_units);
}

bool decode(ByteReader& reader, CostValue* value) {
  if (!decode_enum(reader, &value->source, "CostSource")) {
    return false;
  }
  return reader.i64(&value->micro_units);
}

void encode(ByteWriter& writer, const SizeValue& value) {
  writer.boolean(value.present);
  writer.u64(value.bytes);
}

bool decode(ByteReader& reader, SizeValue* value) {
  if (!reader.boolean(&value->present)) {
    return false;
  }
  return reader.u64(&value->bytes);
}

void encode(ByteWriter& writer, const ReuseValue& value) {
  encode_enum(writer, value.source);
  writer.u64(value.expected_reuse_count);
  writer.u32(value.probability_per_mille);
}

bool decode(ByteReader& reader, ReuseValue* value) {
  if (!decode_enum(reader, &value->source, "CostSource")) {
    return false;
  }
  if (!reader.u64(&value->expected_reuse_count)) {
    return false;
  }
  if (!reader.u32(&value->probability_per_mille)) {
    return false;
  }
  if (value->probability_per_mille > 1000U) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "reuse probability exceeds 1000 per mille");
    return false;
  }
  return true;
}

void encode(ByteWriter& writer, const Digest& value) {
  writer.raw(ByteSpan(reinterpret_cast<const std::byte*>(value.bytes.data()), value.bytes.size()));
}

bool decode(ByteReader& reader, Digest* value) {
  ByteSpan view;
  if (!reader.raw(value->bytes.size(), &view)) {
    return false;
  }
  for (std::size_t index = 0; index < value->bytes.size(); ++index) {
    value->bytes[index] = std::to_integer<std::uint8_t>(view[index]);
  }
  return true;
}

void encode(ByteWriter& writer, const PlacementConstraints& value) {
  encode_id_list(writer, value.legal_clusters);
  encode_region_list(writer, value.legal_regions);
  encode_id_list(writer, value.prohibited_clusters);
  encode_region_list(writer, value.prohibited_regions);
  encode_string_list(writer, value.required_capabilities);
  encode_string_list(writer, value.required_storage_classes);
  encode_string_list(writer, value.required_sovereignty_tags);
  writer.boolean(value.require_locality);
  encode_id_list(writer, value.locality_clusters);
  writer.u32(value.minimum_cluster_diversity);
  writer.u32(value.minimum_region_diversity);
  writer.u32(value.minimum_failure_domain_diversity);
  encode_enum(writer, value.minimum_durability);
  encode_enum(writer, value.required_health);
  writer.u64(value.maximum_evidence_staleness_ticks);
  encode(writer, value.required_free_capacity);
  encode(writer, value.maximum_transfer_cost);
  writer.boolean(value.require_encryption_at_rest);
  writer.boolean(value.require_attestation);
  writer.boolean(value.require_integrity_verification);
  writer.boolean(value.require_compatibility_verification);
}

bool decode(ByteReader& reader, PlacementConstraints* value) {
  if (!decode_cluster_id_list(reader, &value->legal_clusters, "legal_clusters")) return false;
  if (!decode_region_id_list(reader, &value->legal_regions, "legal_regions")) return false;
  if (!decode_cluster_id_list(reader, &value->prohibited_clusters, "prohibited_clusters")) {
    return false;
  }
  if (!decode_region_id_list(reader, &value->prohibited_regions, "prohibited_regions")) {
    return false;
  }
  if (!decode_string_list(reader, &value->required_capabilities, kMaxClusterTagCount,
                          "required_capabilities")) {
    return false;
  }
  if (!decode_string_list(reader, &value->required_storage_classes, kMaxClusterTagCount,
                          "required_storage_classes")) {
    return false;
  }
  if (!decode_string_list(reader, &value->required_sovereignty_tags, kMaxClusterTagCount,
                          "required_sovereignty_tags")) {
    return false;
  }
  if (!reader.boolean(&value->require_locality)) return false;
  if (!decode_cluster_id_list(reader, &value->locality_clusters, "locality_clusters")) return false;
  if (!reader.u32(&value->minimum_cluster_diversity)) return false;
  if (!reader.u32(&value->minimum_region_diversity)) return false;
  if (!reader.u32(&value->minimum_failure_domain_diversity)) return false;
  if (!decode_enum(reader, &value->minimum_durability, "DurabilityClass")) return false;
  if (!decode_enum(reader, &value->required_health, "ClusterHealthState")) return false;
  if (!reader.u64(&value->maximum_evidence_staleness_ticks)) return false;
  if (!decode(reader, &value->required_free_capacity)) return false;
  if (!decode(reader, &value->maximum_transfer_cost)) return false;
  if (!reader.boolean(&value->require_encryption_at_rest)) return false;
  if (!reader.boolean(&value->require_attestation)) return false;
  if (!reader.boolean(&value->require_integrity_verification)) return false;
  if (!reader.boolean(&value->require_compatibility_verification)) return false;
  return true;
}

void encode(ByteWriter& writer, const RankingWeights& value) {
  writer.i64(value.expected_reuse_value);
  writer.i64(value.transfer_cost);
  writer.i64(value.reconstruction_cost);
  writer.i64(value.available_bandwidth);
  writer.i64(value.predicted_transfer_duration);
  writer.i64(value.storage_cost);
  writer.i64(value.compute_cost);
  writer.i64(value.region_affinity);
  writer.i64(value.workload_affinity);
  writer.i64(value.existing_replica_proximity);
  writer.i64(value.failure_domain_diversity);
  writer.i64(value.expected_future_demand);
  writer.i64(value.source_health);
  writer.i64(value.destination_health);
  writer.i64(value.pressure);
  writer.i64(value.utilization);
}

bool decode(ByteReader& reader, RankingWeights* value) {
  return reader.i64(&value->expected_reuse_value) && reader.i64(&value->transfer_cost) &&
         reader.i64(&value->reconstruction_cost) && reader.i64(&value->available_bandwidth) &&
         reader.i64(&value->predicted_transfer_duration) && reader.i64(&value->storage_cost) &&
         reader.i64(&value->compute_cost) && reader.i64(&value->region_affinity) &&
         reader.i64(&value->workload_affinity) && reader.i64(&value->existing_replica_proximity) &&
         reader.i64(&value->failure_domain_diversity) &&
         reader.i64(&value->expected_future_demand) && reader.i64(&value->source_health) &&
         reader.i64(&value->destination_health) && reader.i64(&value->pressure) &&
         reader.i64(&value->utilization);
}

void encode(ByteWriter& writer, const CostPolicy& value) {
  encode_enum(writer, value.source);
  writer.i64(value.transfer_micro_per_mib);
  writer.i64(value.egress_micro_per_mib);
  writer.i64(value.storage_micro_per_mib_tick);
  writer.i64(value.compute_micro_per_mib);
  writer.i64(value.accelerator_micro_per_mib_tick);
  writer.i64(value.repair_micro_per_mib);
  writer.str(value.unit_label);
}

bool decode(ByteReader& reader, CostPolicy* value) {
  return decode_enum(reader, &value->source, "CostSource") &&
         reader.i64(&value->transfer_micro_per_mib) && reader.i64(&value->egress_micro_per_mib) &&
         reader.i64(&value->storage_micro_per_mib_tick) && reader.i64(&value->compute_micro_per_mib) &&
         reader.i64(&value->accelerator_micro_per_mib_tick) &&
         reader.i64(&value->repair_micro_per_mib) &&
         reader.str(kMaxPolicyStringLength, &value->unit_label);
}

void encode(ByteWriter& writer, const ReplicationRequirements& value) {
  encode_enum(writer, value.form);
  writer.u32(value.minimum_authoritative_replicas);
  writer.u32(value.desired_replicas);
  writer.u32(value.maximum_replicas);
  writer.u32(value.region_diversity);
  writer.u32(value.cluster_diversity);
  writer.u32(value.failure_domain_diversity);
  encode_enum(writer, value.minimum_durability);
}

bool decode(ByteReader& reader, ReplicationRequirements* value) {
  return decode_enum(reader, &value->form, "ReplicationPolicyForm") &&
         reader.u32(&value->minimum_authoritative_replicas) && reader.u32(&value->desired_replicas) &&
         reader.u32(&value->maximum_replicas) && reader.u32(&value->region_diversity) &&
         reader.u32(&value->cluster_diversity) && reader.u32(&value->failure_domain_diversity) &&
         decode_enum(reader, &value->minimum_durability, "DurabilityClass");
}

void encode(ByteWriter& writer, const EligibilityRules& value) {
  encode_id_list(writer, value.allowed_clusters);
  encode_id_list(writer, value.denied_clusters);
  writer.boolean(value.require_healthy_source);
  writer.boolean(value.require_healthy_destination);
  writer.boolean(value.require_available_capacity);
  writer.boolean(value.allow_same_cluster);
  writer.boolean(value.require_fresh_evidence);
  writer.u32(value.maximum_in_flight_operations_per_state);
}

bool decode(ByteReader& reader, EligibilityRules* value) {
  return decode_cluster_id_list(reader, &value->allowed_clusters, "allowed_clusters") &&
         decode_cluster_id_list(reader, &value->denied_clusters, "denied_clusters") &&
         reader.boolean(&value->require_healthy_source) &&
         reader.boolean(&value->require_healthy_destination) &&
         reader.boolean(&value->require_available_capacity) &&
         reader.boolean(&value->allow_same_cluster) &&
         reader.boolean(&value->require_fresh_evidence) &&
         reader.u32(&value->maximum_in_flight_operations_per_state);
}

void encode(ByteWriter& writer, const ReuseAuthorityRules& value) {
  writer.boolean(value.require_verified_integrity);
  writer.boolean(value.require_verified_compatibility);
  writer.boolean(value.require_current_state_generation);
  writer.boolean(value.require_replica_set_membership);
  writer.boolean(value.require_current_cluster_incarnation);
  writer.boolean(value.require_current_coordinator_epoch);
  writer.boolean(value.allow_historical_read_only);
}

bool decode(ByteReader& reader, ReuseAuthorityRules* value) {
  return reader.boolean(&value->require_verified_integrity) &&
         reader.boolean(&value->require_verified_compatibility) &&
         reader.boolean(&value->require_current_state_generation) &&
         reader.boolean(&value->require_replica_set_membership) &&
         reader.boolean(&value->require_current_cluster_incarnation) &&
         reader.boolean(&value->require_current_coordinator_epoch) &&
         reader.boolean(&value->allow_historical_read_only);
}

void encode(ByteWriter& writer, const RetentionPolicy& value) {
  writer.boolean(value.retain_historical_records);
  writer.u64(value.stale_copy_retirement_ticks);
  writer.u32(value.maximum_replicas_per_state);
  writer.u32(value.hot_history_records_per_state);
}

bool decode(ByteReader& reader, RetentionPolicy* value) {
  return reader.boolean(&value->retain_historical_records) &&
         reader.u64(&value->stale_copy_retirement_ticks) &&
         reader.u32(&value->maximum_replicas_per_state) &&
         reader.u32(&value->hot_history_records_per_state);
}

void encode(ByteWriter& writer, const RepairPolicy& value) {
  writer.boolean(value.automatic_repair_enabled);
  writer.u32(value.maximum_concurrent_repairs);
  writer.u32(value.repair_attempt_limit);
}

bool decode(ByteReader& reader, RepairPolicy* value) {
  return reader.boolean(&value->automatic_repair_enabled) &&
         reader.u32(&value->maximum_concurrent_repairs) && reader.u32(&value->repair_attempt_limit);
}

void encode(ByteWriter& writer, const RevalidationRequirements& value) {
  writer.boolean(value.require_revalidation_after_cluster_reincarnation);
  writer.boolean(value.require_revalidation_after_coordinator_restart);
  writer.boolean(value.require_fresh_health_evidence);
  writer.boolean(value.require_fresh_capability_evidence);
}

bool decode(ByteReader& reader, RevalidationRequirements* value) {
  return reader.boolean(&value->require_revalidation_after_cluster_reincarnation) &&
         reader.boolean(&value->require_revalidation_after_coordinator_restart) &&
         reader.boolean(&value->require_fresh_health_evidence) &&
         reader.boolean(&value->require_fresh_capability_evidence);
}

void encode(ByteWriter& writer, const PlacementPolicyRecord& value) {
  encode_id(writer, value.placement_policy_id);
  encode_id(writer, value.generation);
  encode(writer, value.constraints);
  encode(writer, value.weights);
  encode(writer, value.cost_policy);
  writer.u64(value.updated_at);
}

bool decode(ByteReader& reader, PlacementPolicyRecord* value) {
  return decode_id(reader, &value->placement_policy_id) && decode_id(reader, &value->generation) &&
         decode(reader, &value->constraints) && decode(reader, &value->weights) &&
         decode(reader, &value->cost_policy) && reader.u64(&value->updated_at);
}

void encode(ByteWriter& writer, const ReplicationPolicyRecord& value) {
  encode_id(writer, value.replication_policy_id);
  encode_id(writer, value.generation);
  encode(writer, value.requirements);
  encode(writer, value.source_rules);
  encode(writer, value.destination_rules);
  encode(writer, value.reuse_rules);
  encode(writer, value.retention);
  encode(writer, value.repair);
  encode(writer, value.revalidation);
  encode_enum(writer, value.degraded_behavior);
  encode_enum(writer, value.failure_action);
  writer.u64(value.updated_at);
}

bool decode(ByteReader& reader, ReplicationPolicyRecord* value) {
  return decode_id(reader, &value->replication_policy_id) && decode_id(reader, &value->generation) &&
         decode(reader, &value->requirements) && decode(reader, &value->source_rules) &&
         decode(reader, &value->destination_rules) && decode(reader, &value->reuse_rules) &&
         decode(reader, &value->retention) && decode(reader, &value->repair) &&
         decode(reader, &value->revalidation) &&
         decode_enum(reader, &value->degraded_behavior, "DegradedBehavior") &&
         decode_enum(reader, &value->failure_action, "OperationFailureAction") &&
         reader.u64(&value->updated_at);
}

void encode(ByteWriter& writer, const StateProvenance& value) {
  writer.str(value.producer);
  encode_id(writer, value.origin_cluster);
  encode_id(writer, value.origin_incarnation);
  encode_id(writer, value.source_generation);
  encode_id(writer, value.source_replica);
  encode_id(writer, value.operation);
  encode_id(writer, value.worker_boot);
  encode_id(writer, value.coordinator_epoch);
  encode_id(writer, value.commit);
  writer.str(value.verification);
  writer.u64(value.recorded_at);
  writer.boolean(value.revalidated);
  writer.u64(value.revalidated_at);
}

bool decode(ByteReader& reader, StateProvenance* value) {
  return reader.str(kMaxEncodedStringBytes, &value->producer) &&
         decode_id(reader, &value->origin_cluster) &&
         decode_id(reader, &value->origin_incarnation) &&
         decode_id(reader, &value->source_generation) &&
         decode_id(reader, &value->source_replica) && decode_id(reader, &value->operation) &&
         decode_id(reader, &value->worker_boot) && decode_id(reader, &value->coordinator_epoch) &&
         decode_id(reader, &value->commit) &&
         reader.str(kMaxEncodedStringBytes, &value->verification) &&
         reader.u64(&value->recorded_at) && reader.boolean(&value->revalidated) &&
         reader.u64(&value->revalidated_at);
}

void encode(ByteWriter& writer, const ReconstructionIdentity& value) {
  encode_id(writer, value.reconstruction_id);
  writer.str(value.method);
  encode(writer, value.output_size);
  encode(writer, value.estimated_cost);
  writer.boolean(value.deterministic);
}

bool decode(ByteReader& reader, ReconstructionIdentity* value) {
  return decode_id(reader, &value->reconstruction_id) &&
         reader.str(kMaxEncodedStringBytes, &value->method) &&
         decode(reader, &value->output_size) && decode(reader, &value->estimated_cost) &&
         reader.boolean(&value->deterministic);
}

void encode(ByteWriter& writer, const StateGenerationSpec& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.generation);
  encode_id(writer, value.lineage);
  writer.boolean(value.branch);
  encode_id(writer, value.branches_from);
  encode_enum(writer, value.state_class);
  encode_enum(writer, value.consistency);
  encode(writer, value.logical_size);
  encode(writer, value.physical_size);
  encode(writer, value.content_digest);
  encode_enum(writer, value.digest_algorithm);
  encode_id(writer, value.compatibility_id);
  encode_id(writer, value.compatibility_generation);
  value.compatibility_required.encode(writer);
  encode(writer, value.reconstruction);
  encode_enum(writer, value.durability);
  encode_enum(writer, value.reuse_class);
  encode(writer, value.placement_constraints);
  encode(writer, value.replication_requirements);
  encode(writer, value.provenance);
  encode(writer, value.expected_reuse);
  encode(writer, value.transfer_cost);
  encode(writer, value.reconstruction_cost);
}

bool decode(ByteReader& reader, StateGenerationSpec* value) {
  return decode_id(reader, &value->state_id) && decode_id(reader, &value->generation) &&
         decode_id(reader, &value->lineage) && reader.boolean(&value->branch) &&
         decode_id(reader, &value->branches_from) &&
         decode_enum(reader, &value->state_class, "StateClass") &&
         decode_enum(reader, &value->consistency, "ConsistencyMode") &&
         decode(reader, &value->logical_size) && decode(reader, &value->physical_size) &&
         decode(reader, &value->content_digest) &&
         decode_enum(reader, &value->digest_algorithm, "IntegrityAlgorithm") &&
         decode_id(reader, &value->compatibility_id) &&
         decode_id(reader, &value->compatibility_generation) &&
         value->compatibility_required.decode(reader) && decode(reader, &value->reconstruction) &&
         decode_enum(reader, &value->durability, "DurabilityClass") &&
         decode_enum(reader, &value->reuse_class, "ReuseClass") &&
         decode(reader, &value->placement_constraints) &&
         decode(reader, &value->replication_requirements) && decode(reader, &value->provenance) &&
         decode(reader, &value->expected_reuse) && decode(reader, &value->transfer_cost) &&
         decode(reader, &value->reconstruction_cost);
}

void encode(ByteWriter& writer, const StateGenerationRecord& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.generation);
  encode_id(writer, value.lineage);
  writer.boolean(value.branch);
  encode_id(writer, value.branches_from);
  encode_enum(writer, value.state_class);
  encode_enum(writer, value.consistency);
  encode_enum(writer, value.lifecycle);
  encode(writer, value.logical_size);
  encode(writer, value.physical_size);
  encode(writer, value.content_digest);
  encode_enum(writer, value.digest_algorithm);
  encode_id(writer, value.compatibility_id);
  encode_id(writer, value.compatibility_generation);
  value.compatibility_required.encode(writer);
  encode(writer, value.reconstruction);
  encode_enum(writer, value.durability);
  encode_enum(writer, value.reuse_class);
  encode(writer, value.placement_constraints);
  encode(writer, value.replication_requirements);
  encode(writer, value.provenance);
  encode(writer, value.expected_reuse);
  encode(writer, value.transfer_cost);
  encode(writer, value.reconstruction_cost);
  encode_id(writer, value.replica_set_id);
  encode_id(writer, value.replica_set_generation);
  encode_id(writer, value.commit);
  encode_id(writer, value.committed_epoch);
  writer.u64(value.registered_at);
  writer.u64(value.committed_at);
  writer.boolean(value.revalidation_required);
}

bool decode(ByteReader& reader, StateGenerationRecord* value) {
  return decode_id(reader, &value->state_id) && decode_id(reader, &value->generation) &&
         decode_id(reader, &value->lineage) && reader.boolean(&value->branch) &&
         decode_id(reader, &value->branches_from) &&
         decode_enum(reader, &value->state_class, "StateClass") &&
         decode_enum(reader, &value->consistency, "ConsistencyMode") &&
         decode_enum(reader, &value->lifecycle, "StateGenerationLifecycle") &&
         decode(reader, &value->logical_size) && decode(reader, &value->physical_size) &&
         decode(reader, &value->content_digest) &&
         decode_enum(reader, &value->digest_algorithm, "IntegrityAlgorithm") &&
         decode_id(reader, &value->compatibility_id) &&
         decode_id(reader, &value->compatibility_generation) &&
         value->compatibility_required.decode(reader) && decode(reader, &value->reconstruction) &&
         decode_enum(reader, &value->durability, "DurabilityClass") &&
         decode_enum(reader, &value->reuse_class, "ReuseClass") &&
         decode(reader, &value->placement_constraints) &&
         decode(reader, &value->replication_requirements) && decode(reader, &value->provenance) &&
         decode(reader, &value->expected_reuse) && decode(reader, &value->transfer_cost) &&
         decode(reader, &value->reconstruction_cost) && decode_id(reader, &value->replica_set_id) &&
         decode_id(reader, &value->replica_set_generation) && decode_id(reader, &value->commit) &&
         decode_id(reader, &value->committed_epoch) && reader.u64(&value->registered_at) &&
         reader.u64(&value->committed_at) && reader.boolean(&value->revalidation_required);
}

void encode(ByteWriter& writer, const ClusterCapabilities& value) {
  encode_string_list(writer, value.tags);
  encode_string_list(writer, value.storage_classes);
  encode_string_list(writer, value.accelerator_architectures);
  writer.boolean(value.encryption_at_rest);
  writer.boolean(value.attestation);
}

bool decode(ByteReader& reader, ClusterCapabilities* value) {
  return decode_string_list(reader, &value->tags, kMaxClusterTagCount, "capability tags") &&
         decode_string_list(reader, &value->storage_classes, kMaxClusterTagCount,
                            "storage classes") &&
         decode_string_list(reader, &value->accelerator_architectures, kMaxClusterTagCount,
                            "accelerator architectures") &&
         reader.boolean(&value->encryption_at_rest) && reader.boolean(&value->attestation);
}

void encode(ByteWriter& writer, const ClusterCapacity& value) {
  writer.u64(value.total_logical_bytes);
  writer.u64(value.used_logical_bytes);
  writer.u64(value.inbound_bytes_per_tick);
  writer.u32(value.free_transfer_slots);
  writer.u32(value.utilization_per_mille);
}

bool decode(ByteReader& reader, ClusterCapacity* value) {
  if (!reader.u64(&value->total_logical_bytes)) return false;
  if (!reader.u64(&value->used_logical_bytes)) return false;
  if (value->used_logical_bytes > value->total_logical_bytes) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "cluster used capacity exceeds total capacity");
    return false;
  }
  if (!reader.u64(&value->inbound_bytes_per_tick)) return false;
  if (!reader.u32(&value->free_transfer_slots)) return false;
  if (!reader.u32(&value->utilization_per_mille)) return false;
  if (value->utilization_per_mille > 1000U) {
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, "cluster utilization exceeds 1000 per mille");
    return false;
  }
  return true;
}

void encode(ByteWriter& writer, const ClusterLocation& value) {
  encode_id(writer, value.region);
  encode_id(writer, value.site);
  encode_id(writer, value.failure_domain);
  writer.str(value.descriptor);
}

bool decode(ByteReader& reader, ClusterLocation* value) {
  return decode_id(reader, &value->region) && decode_id(reader, &value->site) &&
         decode_id(reader, &value->failure_domain) &&
         reader.str(kMaxLocationDescriptorLength, &value->descriptor);
}

void encode(ByteWriter& writer, const ClusterEvidence& value) {
  encode_id(writer, value.capability_generation);
  encode_id(writer, value.health_generation);
  encode_id(writer, value.topology_generation);
  encode_id(writer, value.cost_generation);
  encode_id(writer, value.evidence_generation);
  writer.u64(value.published_at);
  encode_id(writer, value.coordinator_epoch);
  writer.boolean(value.revalidation_required);
  writer.boolean(value.present);
}

bool decode(ByteReader& reader, ClusterEvidence* value) {
  return decode_id(reader, &value->capability_generation) &&
         decode_id(reader, &value->health_generation) &&
         decode_id(reader, &value->topology_generation) &&
         decode_id(reader, &value->cost_generation) &&
         decode_id(reader, &value->evidence_generation) && reader.u64(&value->published_at) &&
         decode_id(reader, &value->coordinator_epoch) &&
         reader.boolean(&value->revalidation_required) && reader.boolean(&value->present);
}

void encode(ByteWriter& writer, const ClusterRegistration& value) {
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.region);
  encode_id(writer, value.site);
  encode_id(writer, value.failure_domain);
  writer.str(value.location_descriptor);
  encode_id(writer, value.worker_id);
  encode_id(writer, value.worker_boot_id);
  encode(writer, value.capabilities);
  encode(writer, value.capacity);
  encode_enum(writer, value.health);
  encode_id(writer, value.expected_coordinator_epoch);
  writer.boolean(value.force_reincarnation);
}

bool decode(ByteReader& reader, ClusterRegistration* value) {
  return decode_id(reader, &value->cluster_id) && decode_id(reader, &value->region) &&
         decode_id(reader, &value->site) && decode_id(reader, &value->failure_domain) &&
         reader.str(kMaxLocationDescriptorLength, &value->location_descriptor) &&
         decode_id(reader, &value->worker_id) && decode_id(reader, &value->worker_boot_id) &&
         decode(reader, &value->capabilities) && decode(reader, &value->capacity) &&
         decode_enum(reader, &value->health, "ClusterHealthState") &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         reader.boolean(&value->force_reincarnation);
}

void encode(ByteWriter& writer, const ClusterEvidenceUpdate& value) {
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.incarnation);
  encode_enum(writer, value.health);
  encode(writer, value.capabilities);
  encode(writer, value.capacity);
  writer.boolean(value.has_capabilities);
  writer.boolean(value.has_capacity);
  encode_id(writer, value.expected_coordinator_epoch);
  encode_id(writer, value.worker_boot_id);
}

bool decode(ByteReader& reader, ClusterEvidenceUpdate* value) {
  return decode_id(reader, &value->cluster_id) && decode_id(reader, &value->incarnation) &&
         decode_enum(reader, &value->health, "ClusterHealthState") &&
         decode(reader, &value->capabilities) && decode(reader, &value->capacity) &&
         reader.boolean(&value->has_capabilities) && reader.boolean(&value->has_capacity) &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         decode_id(reader, &value->worker_boot_id);
}

void encode(ByteWriter& writer, const ClusterRecord& value) {
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.incarnation);
  encode_enum(writer, value.lifecycle);
  encode_enum(writer, value.health);
  encode(writer, value.location);
  encode_id(writer, value.worker_id);
  encode_id(writer, value.worker_boot_id);
  encode(writer, value.capabilities);
  encode(writer, value.capacity);
  encode(writer, value.evidence);
  encode_id(writer, value.registered_epoch);
  encode_id(writer, value.last_evidence_epoch);
  writer.u64(value.registered_at);
  writer.u64(value.updated_at);
  writer.u64(value.incarnation_started_at);
  writer.boolean(value.live_authority);
  writer.u32(value.hosted_replica_count);
  writer.u32(value.historical_replica_count);
  writer.u32(value.reincarnation_count);
}

bool decode(ByteReader& reader, ClusterRecord* value) {
  return decode_id(reader, &value->cluster_id) && decode_id(reader, &value->incarnation) &&
         decode_enum(reader, &value->lifecycle, "ClusterLifecycle") &&
         decode_enum(reader, &value->health, "ClusterHealthState") &&
         decode(reader, &value->location) && decode_id(reader, &value->worker_id) &&
         decode_id(reader, &value->worker_boot_id) && decode(reader, &value->capabilities) &&
         decode(reader, &value->capacity) && decode(reader, &value->evidence) &&
         decode_id(reader, &value->registered_epoch) &&
         decode_id(reader, &value->last_evidence_epoch) && reader.u64(&value->registered_at) &&
         reader.u64(&value->updated_at) && reader.u64(&value->incarnation_started_at) &&
         reader.boolean(&value->live_authority) && reader.u32(&value->hosted_replica_count) &&
         reader.u32(&value->historical_replica_count) && reader.u32(&value->reincarnation_count);
}

void encode(ByteWriter& writer, const ReplicaLocation& value) {
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.region);
  encode_id(writer, value.site);
  encode_id(writer, value.failure_domain);
  writer.str(value.descriptor);
}

bool decode(ByteReader& reader, ReplicaLocation* value) {
  return decode_id(reader, &value->cluster_id) &&
         decode_id(reader, &value->cluster_incarnation) && decode_id(reader, &value->region) &&
         decode_id(reader, &value->site) && decode_id(reader, &value->failure_domain) &&
         reader.str(kMaxLocationDescriptorLength, &value->descriptor);
}

void encode(ByteWriter& writer, const ReplicaEvidence& value) {
  encode_id(writer, value.integrity_generation);
  encode_id(writer, value.compatibility_generation);
  encode_id(writer, value.evidence_generation);
  encode_id(writer, value.capability_generation);
  encode_id(writer, value.health_generation);
  encode_id(writer, value.verification_id);
  writer.u64(value.verified_at);
  encode_id(writer, value.coordinator_epoch);
  writer.boolean(value.revalidation_required);
  writer.boolean(value.integrity_evidence_present);
  writer.boolean(value.compatibility_evidence_present);
}

bool decode(ByteReader& reader, ReplicaEvidence* value) {
  return decode_id(reader, &value->integrity_generation) &&
         decode_id(reader, &value->compatibility_generation) &&
         decode_id(reader, &value->evidence_generation) &&
         decode_id(reader, &value->capability_generation) &&
         decode_id(reader, &value->health_generation) &&
         decode_id(reader, &value->verification_id) && reader.u64(&value->verified_at) &&
         decode_id(reader, &value->coordinator_epoch) &&
         reader.boolean(&value->revalidation_required) &&
         reader.boolean(&value->integrity_evidence_present) &&
         reader.boolean(&value->compatibility_evidence_present);
}

void encode(ByteWriter& writer, const ReplicaRecord& value) {
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.replica_set_id);
  encode_id(writer, value.replica_set_generation);
  encode(writer, value.location);
  encode_enum(writer, value.lifecycle);
  encode_enum(writer, value.integrity);
  encode_enum(writer, value.compatibility);
  encode_enum(writer, value.durability);
  encode_enum(writer, value.availability);
  encode_enum(writer, value.authority);
  encode(writer, value.content_digest);
  encode(writer, value.measured_digest);
  encode_enum(writer, value.digest_algorithm);
  encode_id(writer, value.compatibility_id);
  encode_id(writer, value.compatibility_generation);
  value.compatibility_descriptor.encode(writer);
  encode(writer, value.logical_size);
  encode(writer, value.transferred_bytes);
  encode(writer, value.provenance);
  encode(writer, value.evidence);
  encode_id(writer, value.worker_id);
  encode_id(writer, value.worker_boot_id);
  encode_id(writer, value.created_epoch);
  encode_id(writer, value.last_epoch);
  writer.u64(value.created_at);
  writer.u64(value.updated_at);
  encode_id(writer, value.transfer_id);
  encode_id(writer, value.transfer_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.commit_id);
  writer.u32(value.attempt_count);
  writer.str(value.last_error);
  writer.boolean(value.commit_recorded);
  writer.boolean(value.counts_toward_factor);
}

bool decode(ByteReader& reader, ReplicaRecord* value) {
  return decode_id(reader, &value->replica_id) && decode_id(reader, &value->replica_generation) &&
         decode_id(reader, &value->state_id) && decode_id(reader, &value->state_generation) &&
         decode_id(reader, &value->replica_set_id) &&
         decode_id(reader, &value->replica_set_generation) && decode(reader, &value->location) &&
         decode_enum(reader, &value->lifecycle, "ReplicaLifecycle") &&
         decode_enum(reader, &value->integrity, "ReplicaIntegrityState") &&
         decode_enum(reader, &value->compatibility, "ReplicaCompatibilityState") &&
         decode_enum(reader, &value->durability, "ReplicaDurabilityState") &&
         decode_enum(reader, &value->availability, "ReplicaAvailabilityState") &&
         decode_enum(reader, &value->authority, "ReplicaAuthorityState") &&
         decode(reader, &value->content_digest) && decode(reader, &value->measured_digest) &&
         decode_enum(reader, &value->digest_algorithm, "IntegrityAlgorithm") &&
         decode_id(reader, &value->compatibility_id) &&
         decode_id(reader, &value->compatibility_generation) &&
         value->compatibility_descriptor.decode(reader) && decode(reader, &value->logical_size) &&
         decode(reader, &value->transferred_bytes) && decode(reader, &value->provenance) &&
         decode(reader, &value->evidence) && decode_id(reader, &value->worker_id) &&
         decode_id(reader, &value->worker_boot_id) && decode_id(reader, &value->created_epoch) &&
         decode_id(reader, &value->last_epoch) && reader.u64(&value->created_at) &&
         reader.u64(&value->updated_at) && decode_id(reader, &value->transfer_id) &&
         decode_id(reader, &value->transfer_generation) && decode_id(reader, &value->operation_id) &&
         decode_id(reader, &value->operation_generation) && decode_id(reader, &value->commit_id) &&
         reader.u32(&value->attempt_count) && reader.str(kMaxFailureReasonLength, &value->last_error) &&
         reader.boolean(&value->commit_recorded) && reader.boolean(&value->counts_toward_factor);
}

void encode(ByteWriter& writer, const ReplicaRegistration& value) {
  encode_id(writer, value.replica_id);
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.replica_set_id);
  encode_id(writer, value.replica_set_generation);
  encode_id(writer, value.replica_generation);
  encode_enum(writer, value.lifecycle);
  encode_enum(writer, value.durability);
  encode(writer, value.logical_size);
  encode(writer, value.content_digest);
  encode_enum(writer, value.digest_algorithm);
  encode_id(writer, value.compatibility_id);
  encode_id(writer, value.compatibility_generation);
  value.compatibility_descriptor.encode(writer);
  encode(writer, value.location);
  encode_id(writer, value.worker_id);
  encode_id(writer, value.worker_boot_id);
  encode_id(writer, value.expected_coordinator_epoch);
  encode(writer, value.provenance);
  encode_id(writer, value.transfer_id);
  encode_id(writer, value.transfer_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
}

bool decode(ByteReader& reader, ReplicaRegistration* value) {
  return decode_id(reader, &value->replica_id) && decode_id(reader, &value->state_id) &&
         decode_id(reader, &value->state_generation) && decode_id(reader, &value->replica_set_id) &&
         decode_id(reader, &value->replica_set_generation) &&
         decode_id(reader, &value->replica_generation) &&
         decode_enum(reader, &value->lifecycle, "ReplicaLifecycle") &&
         decode_enum(reader, &value->durability, "DurabilityClass") &&
         decode(reader, &value->logical_size) && decode(reader, &value->content_digest) &&
         decode_enum(reader, &value->digest_algorithm, "IntegrityAlgorithm") &&
         decode_id(reader, &value->compatibility_id) &&
         decode_id(reader, &value->compatibility_generation) &&
         value->compatibility_descriptor.decode(reader) && decode(reader, &value->location) &&
         decode_id(reader, &value->worker_id) && decode_id(reader, &value->worker_boot_id) &&
         decode_id(reader, &value->expected_coordinator_epoch) && decode(reader, &value->provenance) &&
         decode_id(reader, &value->transfer_id) && decode_id(reader, &value->transfer_generation) &&
         decode_id(reader, &value->operation_id) && decode_id(reader, &value->operation_generation);
}

void encode(ByteWriter& writer, const TransferProgress& value) {
  encode_id(writer, value.transfer_id);
  encode_id(writer, value.transfer_generation);
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.worker_boot_id);
  encode_id(writer, value.expected_coordinator_epoch);
  writer.u64(value.bytes_transferred);
  writer.boolean(value.failed);
  writer.str(value.failure_reason);
}

bool decode(ByteReader& reader, TransferProgress* value) {
  return decode_id(reader, &value->transfer_id) &&
         decode_id(reader, &value->transfer_generation) && decode_id(reader, &value->replica_id) &&
         decode_id(reader, &value->replica_generation) && decode_id(reader, &value->operation_id) &&
         decode_id(reader, &value->operation_generation) && decode_id(reader, &value->cluster_id) &&
         decode_id(reader, &value->cluster_incarnation) &&
         decode_id(reader, &value->worker_boot_id) &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         reader.u64(&value->bytes_transferred) && reader.boolean(&value->failed) &&
         reader.str(kMaxFailureReasonLength, &value->failure_reason);
}

void encode(ByteWriter& writer, const TransferCompletion& value) {
  encode_id(writer, value.transfer_id);
  encode_id(writer, value.transfer_generation);
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.worker_boot_id);
  encode_id(writer, value.expected_coordinator_epoch);
  writer.u64(value.bytes_transferred);
  encode(writer, value.measured_digest);
  encode_enum(writer, value.digest_algorithm);
  writer.boolean(value.transfer_complete);
  writer.str(value.detail);
}

bool decode(ByteReader& reader, TransferCompletion* value) {
  return decode_id(reader, &value->transfer_id) &&
         decode_id(reader, &value->transfer_generation) && decode_id(reader, &value->replica_id) &&
         decode_id(reader, &value->replica_generation) && decode_id(reader, &value->operation_id) &&
         decode_id(reader, &value->operation_generation) && decode_id(reader, &value->cluster_id) &&
         decode_id(reader, &value->cluster_incarnation) &&
         decode_id(reader, &value->worker_boot_id) &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         reader.u64(&value->bytes_transferred) && decode(reader, &value->measured_digest) &&
         decode_enum(reader, &value->digest_algorithm, "IntegrityAlgorithm") &&
         reader.boolean(&value->transfer_complete) &&
         reader.str(kMaxFailureReasonLength, &value->detail);
}

void encode(ByteWriter& writer, const ReplicaVerification& value) {
  encode_id(writer, value.verification_id);
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.worker_boot_id);
  encode_id(writer, value.expected_coordinator_epoch);
  writer.boolean(value.integrity_verified);
  encode(writer, value.measured_digest);
  encode_enum(writer, value.digest_algorithm);
  writer.boolean(value.compatibility_verified);
  encode_id(writer, value.compatibility_id);
  encode_id(writer, value.compatibility_generation);
  value.compatibility_descriptor.encode(writer);
  encode_enum(writer, value.established_durability);
  writer.u64(value.verified_bytes);
  writer.str(value.detail);
}

bool decode(ByteReader& reader, ReplicaVerification* value) {
  return decode_id(reader, &value->verification_id) && decode_id(reader, &value->replica_id) &&
         decode_id(reader, &value->replica_generation) && decode_id(reader, &value->operation_id) &&
         decode_id(reader, &value->operation_generation) && decode_id(reader, &value->cluster_id) &&
         decode_id(reader, &value->cluster_incarnation) &&
         decode_id(reader, &value->worker_boot_id) &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         reader.boolean(&value->integrity_verified) && decode(reader, &value->measured_digest) &&
         decode_enum(reader, &value->digest_algorithm, "IntegrityAlgorithm") &&
         reader.boolean(&value->compatibility_verified) &&
         decode_id(reader, &value->compatibility_id) &&
         decode_id(reader, &value->compatibility_generation) &&
         value->compatibility_descriptor.decode(reader) &&
         decode_enum(reader, &value->established_durability, "DurabilityClass") &&
         reader.u64(&value->verified_bytes) && reader.str(kMaxFailureReasonLength, &value->detail);
}

void encode(ByteWriter& writer, const ReplicaCommit& value) {
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.commit_id);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.expected_coordinator_epoch);
  encode_id(writer, value.expected_replica_set_generation);
  writer.boolean(value.make_authoritative_owner);
  writer.boolean(value.add_to_replica_set);
  writer.boolean(value.require_integrity_verified);
  writer.boolean(value.require_compatibility_verified);
}

bool decode(ByteReader& reader, ReplicaCommit* value) {
  return decode_id(reader, &value->replica_id) && decode_id(reader, &value->replica_generation) &&
         decode_id(reader, &value->commit_id) && decode_id(reader, &value->operation_id) &&
         decode_id(reader, &value->operation_generation) &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         decode_id(reader, &value->expected_replica_set_generation) &&
         reader.boolean(&value->make_authoritative_owner) &&
         reader.boolean(&value->add_to_replica_set) &&
         reader.boolean(&value->require_integrity_verified) &&
         reader.boolean(&value->require_compatibility_verified);
}

void encode(ByteWriter& writer, const ReplicaTransition& value) {
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.expected_coordinator_epoch);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.worker_boot_id);
  writer.str(value.reason);
}

bool decode(ByteReader& reader, ReplicaTransition* value) {
  return decode_id(reader, &value->replica_id) && decode_id(reader, &value->replica_generation) &&
         decode_id(reader, &value->operation_id) && decode_id(reader, &value->operation_generation) &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         decode_id(reader, &value->cluster_incarnation) && decode_id(reader, &value->worker_boot_id) &&
         reader.str(kMaxFailureReasonLength, &value->reason);
}

void encode(ByteWriter& writer, const ReplicaSetMember& value) {
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.region);
  encode_id(writer, value.failure_domain);
  writer.boolean(value.authoritative);
  encode_enum(writer, value.lifecycle);
  encode_enum(writer, value.integrity);
  encode_enum(writer, value.compatibility);
  encode_enum(writer, value.authority);
  encode_id(writer, value.commit_id);
  writer.u64(value.joined_at);
}

bool decode(ByteReader& reader, ReplicaSetMember* value) {
  return decode_id(reader, &value->replica_id) && decode_id(reader, &value->replica_generation) &&
         decode_id(reader, &value->cluster_id) && decode_id(reader, &value->cluster_incarnation) &&
         decode_id(reader, &value->region) && decode_id(reader, &value->failure_domain) &&
         reader.boolean(&value->authoritative) &&
         decode_enum(reader, &value->lifecycle, "ReplicaLifecycle") &&
         decode_enum(reader, &value->integrity, "ReplicaIntegrityState") &&
         decode_enum(reader, &value->compatibility, "ReplicaCompatibilityState") &&
         decode_enum(reader, &value->authority, "ReplicaAuthorityState") &&
         decode_id(reader, &value->commit_id) && reader.u64(&value->joined_at);
}

void encode(ByteWriter& writer, const ReplicaSetRecord& value) {
  encode_id(writer, value.replica_set_id);
  encode_id(writer, value.generation);
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_enum(writer, value.state);
  encode(writer, value.requirements);
  encode_vector(writer, value.authoritative_members,
                [](ByteWriter& w, const ReplicaSetMember& member) { encode(w, member); });
  encode_vector(writer, value.degraded_members,
                [](ByteWriter& w, const ReplicaSetMember& member) { encode(w, member); });
  writer.u32(value.authoritative_count);
  writer.u32(value.degraded_count);
  writer.u32(value.distinct_clusters);
  writer.u32(value.distinct_regions);
  writer.u32(value.distinct_failure_domains);
  encode_id(writer, value.placement_id);
  encode_id(writer, value.placement_generation);
  encode_id(writer, value.epoch);
  encode_id(writer, value.last_commit);
  writer.u64(value.created_at);
  writer.u64(value.updated_at);
  writer.boolean(value.revalidation_required);
}

bool decode(ByteReader& reader, ReplicaSetRecord* value) {
  if (!decode_id(reader, &value->replica_set_id)) return false;
  if (!decode_id(reader, &value->generation)) return false;
  if (!decode_id(reader, &value->state_id)) return false;
  if (!decode_id(reader, &value->state_generation)) return false;
  if (!decode_enum(reader, &value->state, "ReplicaSetState")) return false;
  if (!decode(reader, &value->requirements)) return false;
  if (!decode_vector<ReplicaSetMember>(
          reader, &value->authoritative_members, kMaxDecodedMembers, "authoritative members",
          [](ByteReader& r, ReplicaSetMember* member) { return decode(r, member); })) {
    return false;
  }
  if (!decode_vector<ReplicaSetMember>(
          reader, &value->degraded_members, kMaxDecodedMembers, "degraded members",
          [](ByteReader& r, ReplicaSetMember* member) { return decode(r, member); })) {
    return false;
  }
  if (!reader.u32(&value->authoritative_count)) return false;
  if (!reader.u32(&value->degraded_count)) return false;
  if (!reader.u32(&value->distinct_clusters)) return false;
  if (!reader.u32(&value->distinct_regions)) return false;
  if (!reader.u32(&value->distinct_failure_domains)) return false;
  if (!decode_id(reader, &value->placement_id)) return false;
  if (!decode_id(reader, &value->placement_generation)) return false;
  if (!decode_id(reader, &value->epoch)) return false;
  if (!decode_id(reader, &value->last_commit)) return false;
  if (!reader.u64(&value->created_at)) return false;
  if (!reader.u64(&value->updated_at)) return false;
  if (!reader.boolean(&value->revalidation_required)) return false;
  return true;
}

void encode(ByteWriter& writer, const TransferAuthorization& value) {
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_enum(writer, value.kind);
  encode_id(writer, value.migration_id);
  encode_id(writer, value.migration_generation);
  encode_id(writer, value.transfer_id);
  encode_id(writer, value.transfer_generation);
  encode_id(writer, value.reservation_id);
  encode_id(writer, value.reservation_generation);
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.replica_set_id);
  encode_id(writer, value.replica_set_generation);
  encode_id(writer, value.source_replica);
  encode_id(writer, value.source_replica_generation);
  encode_id(writer, value.source_cluster);
  encode_id(writer, value.source_incarnation);
  encode_id(writer, value.destination_replica);
  encode_id(writer, value.destination_replica_generation);
  encode_id(writer, value.destination_cluster);
  encode_id(writer, value.destination_incarnation);
  encode_id(writer, value.placement_id);
  encode_id(writer, value.placement_generation);
  encode_id(writer, value.coordinator_epoch);
  encode(writer, value.transfer_bytes);
  encode(writer, value.expected_digest);
  encode_enum(writer, value.digest_algorithm);
  writer.u64(value.reservation_expires_at);
  writer.u64(value.authorized_at);
  writer.boolean(value.destructive_move);
}

bool decode(ByteReader& reader, TransferAuthorization* value) {
  if (!decode_id(reader, &value->operation_id)) return false;
  if (!decode_id(reader, &value->operation_generation)) return false;
  if (!decode_enum(reader, &value->kind, "OperationKind")) return false;
  if (!decode_id(reader, &value->migration_id)) return false;
  if (!decode_id(reader, &value->migration_generation)) return false;
  if (!decode_id(reader, &value->transfer_id)) return false;
  if (!decode_id(reader, &value->transfer_generation)) return false;
  if (!decode_id(reader, &value->reservation_id)) return false;
  if (!decode_id(reader, &value->reservation_generation)) return false;
  if (!decode_id(reader, &value->state_id)) return false;
  if (!decode_id(reader, &value->state_generation)) return false;
  if (!decode_id(reader, &value->replica_set_id)) return false;
  if (!decode_id(reader, &value->replica_set_generation)) return false;
  if (!decode_id(reader, &value->source_replica)) return false;
  if (!decode_id(reader, &value->source_replica_generation)) return false;
  if (!decode_id(reader, &value->source_cluster)) return false;
  if (!decode_id(reader, &value->source_incarnation)) return false;
  if (!decode_id(reader, &value->destination_replica)) return false;
  if (!decode_id(reader, &value->destination_replica_generation)) return false;
  if (!decode_id(reader, &value->destination_cluster)) return false;
  if (!decode_id(reader, &value->destination_incarnation)) return false;
  if (!decode_id(reader, &value->placement_id)) return false;
  if (!decode_id(reader, &value->placement_generation)) return false;
  if (!decode_id(reader, &value->coordinator_epoch)) return false;
  if (!decode(reader, &value->transfer_bytes)) return false;
  if (!decode(reader, &value->expected_digest)) return false;
  if (!decode_enum(reader, &value->digest_algorithm, "IntegrityAlgorithm")) return false;
  if (!reader.u64(&value->reservation_expires_at)) return false;
  if (!reader.u64(&value->authorized_at)) return false;
  if (!reader.boolean(&value->destructive_move)) return false;
  return true;
}

void encode(ByteWriter& writer, const OperationRecord& value) {
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_enum(writer, value.kind);
  encode_enum(writer, value.state);
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.source_replica);
  encode_id(writer, value.source_replica_generation);
  encode_id(writer, value.source_cluster);
  encode_id(writer, value.source_incarnation);
  encode_id(writer, value.destination_replica);
  encode_id(writer, value.destination_replica_generation);
  encode_id(writer, value.destination_cluster);
  encode_id(writer, value.destination_incarnation);
  encode_id(writer, value.migration_id);
  encode_id(writer, value.migration_generation);
  encode_id(writer, value.transfer_id);
  encode_id(writer, value.transfer_generation);
  encode_id(writer, value.reservation_id);
  encode_id(writer, value.reservation_generation);
  encode_id(writer, value.replication_policy_id);
  encode_id(writer, value.replication_policy_generation_recorded);
  encode_id(writer, value.placement_policy_id);
  encode_id(writer, value.source_replica_set_generation);
  encode_id(writer, value.committed_replica_set_generation);
  encode_id(writer, value.placement_generation);
  encode_id(writer, value.placement_policy_generation);
  encode_id(writer, value.replication_policy_generation);
  encode_id(writer, value.planned_epoch);
  encode_id(writer, value.last_epoch);
  encode_id(writer, value.commit_id);
  encode(writer, value.transfer_bytes);
  encode(writer, value.transferred_bytes);
  encode(writer, value.expected_digest);
  encode(writer, value.measured_digest);
  encode_enum(writer, value.failure_action);
  encode_enum(writer, value.cancellation);
  writer.u64(value.planned_at);
  writer.u64(value.updated_at);
  writer.u64(value.reservation_expires_at);
  writer.boolean(value.destructive_move);
  writer.boolean(value.source_retired);
  writer.boolean(value.destination_verified);
  writer.boolean(value.destination_committed);
  writer.boolean(value.recovery_required);
  writer.str(value.reason);
  writer.str(value.last_error);
  encode_string_list(writer, value.journal);
}

bool decode(ByteReader& reader, OperationRecord* value) {
  if (!decode_id(reader, &value->operation_id)) return false;
  if (!decode_id(reader, &value->operation_generation)) return false;
  if (!decode_enum(reader, &value->kind, "OperationKind")) return false;
  if (!decode_enum(reader, &value->state, "OperationState")) return false;
  if (!decode_id(reader, &value->state_id)) return false;
  if (!decode_id(reader, &value->state_generation)) return false;
  if (!decode_id(reader, &value->source_replica)) return false;
  if (!decode_id(reader, &value->source_replica_generation)) return false;
  if (!decode_id(reader, &value->source_cluster)) return false;
  if (!decode_id(reader, &value->source_incarnation)) return false;
  if (!decode_id(reader, &value->destination_replica)) return false;
  if (!decode_id(reader, &value->destination_replica_generation)) return false;
  if (!decode_id(reader, &value->destination_cluster)) return false;
  if (!decode_id(reader, &value->destination_incarnation)) return false;
  if (!decode_id(reader, &value->migration_id)) return false;
  if (!decode_id(reader, &value->migration_generation)) return false;
  if (!decode_id(reader, &value->transfer_id)) return false;
  if (!decode_id(reader, &value->transfer_generation)) return false;
  if (!decode_id(reader, &value->reservation_id)) return false;
  if (!decode_id(reader, &value->reservation_generation)) return false;
  if (!decode_id(reader, &value->replication_policy_id)) return false;
  if (!decode_id(reader, &value->replication_policy_generation_recorded)) return false;
  if (!decode_id(reader, &value->placement_policy_id)) return false;
  if (!decode_id(reader, &value->source_replica_set_generation)) return false;
  if (!decode_id(reader, &value->committed_replica_set_generation)) return false;
  if (!decode_id(reader, &value->placement_generation)) return false;
  if (!decode_id(reader, &value->placement_policy_generation)) return false;
  if (!decode_id(reader, &value->replication_policy_generation)) return false;
  if (!decode_id(reader, &value->planned_epoch)) return false;
  if (!decode_id(reader, &value->last_epoch)) return false;
  if (!decode_id(reader, &value->commit_id)) return false;
  if (!decode(reader, &value->transfer_bytes)) return false;
  if (!decode(reader, &value->transferred_bytes)) return false;
  if (!decode(reader, &value->expected_digest)) return false;
  if (!decode(reader, &value->measured_digest)) return false;
  if (!decode_enum(reader, &value->failure_action, "OperationFailureAction")) return false;
  if (!decode_enum(reader, &value->cancellation, "CancellationOutcome")) return false;
  if (!reader.u64(&value->planned_at)) return false;
  if (!reader.u64(&value->updated_at)) return false;
  if (!reader.u64(&value->reservation_expires_at)) return false;
  if (!reader.boolean(&value->destructive_move)) return false;
  if (!reader.boolean(&value->source_retired)) return false;
  if (!reader.boolean(&value->destination_verified)) return false;
  if (!reader.boolean(&value->destination_committed)) return false;
  if (!reader.boolean(&value->recovery_required)) return false;
  if (!reader.str(kMaxFailureReasonLength, &value->reason)) return false;
  if (!reader.str(kMaxFailureReasonLength, &value->last_error)) return false;
  if (!decode_string_list(reader, &value->journal, kMaxDecodedJournalEntries,
                          "operation journal")) {
    return false;
  }
  return true;
}

void encode(ByteWriter& writer, const ReplicationRequest& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.destination_cluster);
  encode_id(writer, value.source_cluster);
  encode(writer, value.placement_policy);
  encode(writer, value.replication_policy);
  encode_id(writer, value.expected_coordinator_epoch);
  writer.u32(value.desired_replicas_override);
}

bool decode(ByteReader& reader, ReplicationRequest* value) {
  return decode_id(reader, &value->state_id) && decode_id(reader, &value->state_generation) &&
         decode_id(reader, &value->destination_cluster) && decode_id(reader, &value->source_cluster) &&
         decode(reader, &value->placement_policy) && decode(reader, &value->replication_policy) &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         reader.u32(&value->desired_replicas_override);
}

void encode(ByteWriter& writer, const MigrationRequest& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.destination_cluster);
  encode_id(writer, value.source_cluster);
  writer.boolean(value.retire_source_after_commit);
  writer.boolean(value.allow_source_retirement_before_commit);
  encode(writer, value.placement_policy);
  encode(writer, value.replication_policy);
  encode_id(writer, value.expected_coordinator_epoch);
}

bool decode(ByteReader& reader, MigrationRequest* value) {
  return decode_id(reader, &value->state_id) && decode_id(reader, &value->state_generation) &&
         decode_id(reader, &value->destination_cluster) && decode_id(reader, &value->source_cluster) &&
         reader.boolean(&value->retire_source_after_commit) &&
         reader.boolean(&value->allow_source_retirement_before_commit) &&
         decode(reader, &value->placement_policy) && decode(reader, &value->replication_policy) &&
         decode_id(reader, &value->expected_coordinator_epoch);
}

void encode(ByteWriter& writer, const OwnershipTransferRequest& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.expected_current_owner);
  encode_id(writer, value.new_owner_replica);
  encode_id(writer, value.new_owner_replica_generation);
  encode_id(writer, value.new_owner_cluster);
  encode_id(writer, value.handoff_commit);
  encode_id(writer, value.expected_operation_generation);
  encode_id(writer, value.expected_coordinator_epoch);
}

bool decode(ByteReader& reader, OwnershipTransferRequest* value) {
  return decode_id(reader, &value->state_id) && decode_id(reader, &value->state_generation) &&
         decode_id(reader, &value->expected_current_owner) &&
         decode_id(reader, &value->new_owner_replica) &&
         decode_id(reader, &value->new_owner_replica_generation) &&
         decode_id(reader, &value->new_owner_cluster) && decode_id(reader, &value->handoff_commit) &&
         decode_id(reader, &value->expected_operation_generation) &&
         decode_id(reader, &value->expected_coordinator_epoch);
}

void encode(ByteWriter& writer, const OperationCommit& value) {
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.commit_id);
  encode_id(writer, value.expected_coordinator_epoch);
  writer.boolean(value.retire_source);
}

bool decode(ByteReader& reader, OperationCommit* value) {
  return decode_id(reader, &value->operation_id) &&
         decode_id(reader, &value->operation_generation) && decode_id(reader, &value->commit_id) &&
         decode_id(reader, &value->expected_coordinator_epoch) && reader.boolean(&value->retire_source);
}

void encode(ByteWriter& writer, const OperationCancellation& value) {
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.expected_coordinator_epoch);
  writer.str(value.reason);
}

bool decode(ByteReader& reader, OperationCancellation* value) {
  return decode_id(reader, &value->operation_id) &&
         decode_id(reader, &value->operation_generation) &&
         decode_id(reader, &value->expected_coordinator_epoch) &&
         reader.str(kMaxFailureReasonLength, &value->reason);
}

void encode(ByteWriter& writer, const OwnershipRecord& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.owner_replica);
  encode_id(writer, value.owner_replica_generation);
  encode_id(writer, value.owner_cluster);
  encode_id(writer, value.owner_incarnation);
  encode_id(writer, value.epoch);
  encode_id(writer, value.commit);
  encode_id(writer, value.placement_id);
  encode_id(writer, value.placement_generation);
  writer.u64(value.established_at);
  writer.boolean(value.active);
}

bool decode(ByteReader& reader, OwnershipRecord* value) {
  return decode_id(reader, &value->state_id) && decode_id(reader, &value->state_generation) &&
         decode_id(reader, &value->owner_replica) &&
         decode_id(reader, &value->owner_replica_generation) &&
         decode_id(reader, &value->owner_cluster) && decode_id(reader, &value->owner_incarnation) &&
         decode_id(reader, &value->epoch) && decode_id(reader, &value->commit) &&
         decode_id(reader, &value->placement_id) && decode_id(reader, &value->placement_generation) &&
         reader.u64(&value->established_at) && reader.boolean(&value->active);
}

void encode(ByteWriter& writer, const CompatibilityRecord& value) {
  encode_id(writer, value.compatibility_id);
  encode_id(writer, value.generation);
  value.descriptor.encode(writer);
  writer.str(value.producer);
  writer.u64(value.published_at);
  writer.boolean(value.revalidation_required);
}

bool decode(ByteReader& reader, CompatibilityRecord* value) {
  return decode_id(reader, &value->compatibility_id) && decode_id(reader, &value->generation) &&
         value->descriptor.decode(reader) && reader.str(kMaxEncodedStringBytes, &value->producer) &&
         reader.u64(&value->published_at) && reader.boolean(&value->revalidation_required);
}

void encode(ByteWriter& writer, const CompatibilityRequirement& value) {
  encode_id(writer, value.compatibility_id);
  encode_id(writer, value.generation);
  value.required.encode(writer);
  writer.boolean(value.require_verified_evidence);
}

bool decode(ByteReader& reader, CompatibilityRequirement* value) {
  return decode_id(reader, &value->compatibility_id) && decode_id(reader, &value->generation) &&
         value->required.decode(reader) && reader.boolean(&value->require_verified_evidence);
}

void encode(ByteWriter& writer, const FabricCounters& value) {
  writer.u64(value.states_registered);
  writer.u64(value.generations_registered);
  writer.u64(value.generations_committed);
  writer.u64(value.generations_superseded);
  writer.u64(value.clusters_registered);
  writer.u64(value.cluster_reincarnations);
  writer.u64(value.cluster_incarnations_fenced);
  writer.u64(value.replicas_registered);
  writer.u64(value.replicas_committed);
  writer.u64(value.replicas_retired);
  writer.u64(value.replicas_quarantined);
  writer.u64(value.replicas_abandoned);
  writer.u64(value.replicas_fenced);
  writer.u64(value.operations_planned);
  writer.u64(value.operations_reserved);
  writer.u64(value.operations_committed);
  writer.u64(value.operations_failed);
  writer.u64(value.operations_cancelled);
  writer.u64(value.operations_abandoned);
  writer.u64(value.operations_recovered);
  writer.u64(value.transfers_started);
  writer.u64(value.transfers_completed);
  writer.u64(value.bytes_transferred);
  writer.u64(value.integrity_verifications);
  writer.u64(value.integrity_failures);
  writer.u64(value.compatibility_rejections);
  writer.u64(value.stale_rejections);
  writer.u64(value.conflicts_rejected);
  writer.u64(value.durable_commits);
  writer.u64(value.journal_records_applied);
  writer.u64(value.snapshots_written);
  writer.u64(value.ownership_transfers);
  writer.u64(value.active_operations);
  writer.u64(value.active_transfers);
  writer.u64(value.reserved_bytes);
  writer.u64(value.reserved_transfer_slots);
}

bool decode(ByteReader& reader, FabricCounters* value) {
  return reader.u64(&value->states_registered) && reader.u64(&value->generations_registered) &&
         reader.u64(&value->generations_committed) && reader.u64(&value->generations_superseded) &&
         reader.u64(&value->clusters_registered) && reader.u64(&value->cluster_reincarnations) &&
         reader.u64(&value->cluster_incarnations_fenced) &&
         reader.u64(&value->replicas_registered) && reader.u64(&value->replicas_committed) &&
         reader.u64(&value->replicas_retired) && reader.u64(&value->replicas_quarantined) &&
         reader.u64(&value->replicas_abandoned) && reader.u64(&value->replicas_fenced) &&
         reader.u64(&value->operations_planned) && reader.u64(&value->operations_reserved) &&
         reader.u64(&value->operations_committed) && reader.u64(&value->operations_failed) &&
         reader.u64(&value->operations_cancelled) && reader.u64(&value->operations_abandoned) &&
         reader.u64(&value->operations_recovered) && reader.u64(&value->transfers_started) &&
         reader.u64(&value->transfers_completed) && reader.u64(&value->bytes_transferred) &&
         reader.u64(&value->integrity_verifications) && reader.u64(&value->integrity_failures) &&
         reader.u64(&value->compatibility_rejections) && reader.u64(&value->stale_rejections) &&
         reader.u64(&value->conflicts_rejected) && reader.u64(&value->durable_commits) &&
         reader.u64(&value->journal_records_applied) && reader.u64(&value->snapshots_written) &&
         reader.u64(&value->ownership_transfers) && reader.u64(&value->active_operations) &&
         reader.u64(&value->active_transfers) && reader.u64(&value->reserved_bytes) &&
         reader.u64(&value->reserved_transfer_slots);
}

}  // namespace ccsf::detail
