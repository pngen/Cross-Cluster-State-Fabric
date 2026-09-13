// Cross-Cluster State Fabric - durable and wire encoding of domain records.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This header is internal to the implementation. Every governed record has
// exactly one canonical encoding, used by persistence, by the wire protocol and
// by policy fingerprints. Decoders are total and bounded: declared counts are
// validated before allocation, enums are validated after decoding, and every
// length is checked.

#ifndef CCSF_DETAIL_ENCODING_HPP
#define CCSF_DETAIL_ENCODING_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/authority.hpp"
#include "ccsf/bytes.hpp"
#include "ccsf/cluster.hpp"
#include "ccsf/compatibility.hpp"
#include "ccsf/cost.hpp"
#include "ccsf/digest.hpp"
#include "ccsf/migration.hpp"
#include "ccsf/policy.hpp"
#include "ccsf/replica.hpp"
#include "ccsf/replica_set.hpp"
#include "ccsf/snapshot.hpp"
#include "ccsf/state.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf::detail {

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::size_t kMaxEncodedRecords = 1000000;
inline constexpr std::size_t kMaxJournalEffectsPerRecord = 64;
inline constexpr std::size_t kMaxEncodedStringBytes = 4096;

/// Bounds applied when decoding untrusted input, before any allocation.
inline constexpr std::size_t kMaxDecodedRecords = 2000000;
inline constexpr std::size_t kMaxDecodedMembers = 4096;
inline constexpr std::size_t kMaxDecodedJournalEntries = kMaxOperationJournalEntries;
inline constexpr std::size_t kMaxDecodedStates = 2000000;
inline constexpr std::size_t kMaxDecodedClusters = 65536;
inline constexpr std::size_t kMaxDecodedReplicas = 4000000;
inline constexpr std::size_t kMaxDecodedOperations = 1000000;

// --- primitive helpers ------------------------------------------------------

template <class Tag>
void encode_id(ByteWriter& writer, const StrongId<Tag>& id) {
  writer.u64(id.value());
}

template <class Tag>
bool decode_id(ByteReader& reader, StrongId<Tag>* out) {
  std::uint64_t raw = 0;
  if (!reader.u64(&raw)) {
    return false;
  }
  *out = StrongId<Tag>::from_value(raw);
  return true;
}

template <class E>
void encode_enum(ByteWriter& writer, E value) {
  writer.u16(static_cast<std::uint16_t>(value));
}

template <class E>
bool decode_enum(ByteReader& reader, E* out, const char* name) {
  std::uint16_t raw = 0;
  if (!reader.u16(&raw)) {
    return false;
  }
  const E value = static_cast<E>(raw);
  if (!is_valid(value)) {
    std::string message = "invalid ";
    message += name;
    message += " value in encoded record";
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, std::move(message));
    return false;
  }
  *out = value;
  return true;
}

template <class T, class Fn>
void encode_vector(ByteWriter& writer, const std::vector<T>& values, Fn&& encode_one) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (const T& value : values) {
    encode_one(writer, value);
  }
}

template <class T, class Fn>
bool decode_vector(ByteReader& reader, std::vector<T>* out, std::size_t max_count,
                   const char* name, Fn&& decode_one) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    return false;
  }
  if (static_cast<std::size_t>(count) > max_count) {
    std::string message = "declared ";
    message += name;
    message += " count exceeds the permitted bound";
    reader.fail(ErrorCode::PERSISTENCE_CORRUPT, std::move(message));
    return false;
  }
  out->clear();
  out->reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    T value{};
    if (!decode_one(reader, &value)) {
      return false;
    }
    out->push_back(std::move(value));
  }
  return true;
}

// --- value types ------------------------------------------------------------

void encode(ByteWriter& writer, const CostValue& value);
bool decode(ByteReader& reader, CostValue* value);

void encode(ByteWriter& writer, const SizeValue& value);
bool decode(ByteReader& reader, SizeValue* value);

void encode(ByteWriter& writer, const ReuseValue& value);
bool decode(ByteReader& reader, ReuseValue* value);

void encode(ByteWriter& writer, const Digest& value);
bool decode(ByteReader& reader, Digest* value);

// --- policy -----------------------------------------------------------------

void encode(ByteWriter& writer, const PlacementConstraints& value);
bool decode(ByteReader& reader, PlacementConstraints* value);

void encode(ByteWriter& writer, const RankingWeights& value);
bool decode(ByteReader& reader, RankingWeights* value);

void encode(ByteWriter& writer, const CostPolicy& value);
bool decode(ByteReader& reader, CostPolicy* value);

void encode(ByteWriter& writer, const ReplicationRequirements& value);
bool decode(ByteReader& reader, ReplicationRequirements* value);

void encode(ByteWriter& writer, const EligibilityRules& value);
bool decode(ByteReader& reader, EligibilityRules* value);

void encode(ByteWriter& writer, const ReuseAuthorityRules& value);
bool decode(ByteReader& reader, ReuseAuthorityRules* value);

void encode(ByteWriter& writer, const RetentionPolicy& value);
bool decode(ByteReader& reader, RetentionPolicy* value);

void encode(ByteWriter& writer, const RepairPolicy& value);
bool decode(ByteReader& reader, RepairPolicy* value);

void encode(ByteWriter& writer, const RevalidationRequirements& value);
bool decode(ByteReader& reader, RevalidationRequirements* value);

void encode(ByteWriter& writer, const PlacementPolicyRecord& value);
bool decode(ByteReader& reader, PlacementPolicyRecord* value);

void encode(ByteWriter& writer, const ReplicationPolicyRecord& value);
bool decode(ByteReader& reader, ReplicationPolicyRecord* value);

// --- state ------------------------------------------------------------------

void encode(ByteWriter& writer, const StateProvenance& value);
bool decode(ByteReader& reader, StateProvenance* value);

void encode(ByteWriter& writer, const ReconstructionIdentity& value);
bool decode(ByteReader& reader, ReconstructionIdentity* value);

void encode(ByteWriter& writer, const StateGenerationSpec& value);
bool decode(ByteReader& reader, StateGenerationSpec* value);

void encode(ByteWriter& writer, const StateGenerationRecord& value);
bool decode(ByteReader& reader, StateGenerationRecord* value);

// --- cluster ----------------------------------------------------------------

void encode(ByteWriter& writer, const ClusterCapabilities& value);
bool decode(ByteReader& reader, ClusterCapabilities* value);

void encode(ByteWriter& writer, const ClusterCapacity& value);
bool decode(ByteReader& reader, ClusterCapacity* value);

void encode(ByteWriter& writer, const ClusterLocation& value);
bool decode(ByteReader& reader, ClusterLocation* value);

void encode(ByteWriter& writer, const ClusterEvidence& value);
bool decode(ByteReader& reader, ClusterEvidence* value);

void encode(ByteWriter& writer, const ClusterRegistration& value);
bool decode(ByteReader& reader, ClusterRegistration* value);

void encode(ByteWriter& writer, const ClusterEvidenceUpdate& value);
bool decode(ByteReader& reader, ClusterEvidenceUpdate* value);

void encode(ByteWriter& writer, const ClusterRecord& value);
bool decode(ByteReader& reader, ClusterRecord* value);

// --- replica ----------------------------------------------------------------

void encode(ByteWriter& writer, const ReplicaLocation& value);
bool decode(ByteReader& reader, ReplicaLocation* value);

void encode(ByteWriter& writer, const ReplicaEvidence& value);
bool decode(ByteReader& reader, ReplicaEvidence* value);

void encode(ByteWriter& writer, const ReplicaRecord& value);
bool decode(ByteReader& reader, ReplicaRecord* value);

void encode(ByteWriter& writer, const ReplicaRegistration& value);
bool decode(ByteReader& reader, ReplicaRegistration* value);

void encode(ByteWriter& writer, const TransferProgress& value);
bool decode(ByteReader& reader, TransferProgress* value);

void encode(ByteWriter& writer, const TransferCompletion& value);
bool decode(ByteReader& reader, TransferCompletion* value);

void encode(ByteWriter& writer, const ReplicaVerification& value);
bool decode(ByteReader& reader, ReplicaVerification* value);

void encode(ByteWriter& writer, const ReplicaCommit& value);
bool decode(ByteReader& reader, ReplicaCommit* value);

void encode(ByteWriter& writer, const ReplicaTransition& value);
bool decode(ByteReader& reader, ReplicaTransition* value);

// --- replica set ------------------------------------------------------------

void encode(ByteWriter& writer, const ReplicaSetMember& value);
bool decode(ByteReader& reader, ReplicaSetMember* value);

void encode(ByteWriter& writer, const ReplicaSetRecord& value);
bool decode(ByteReader& reader, ReplicaSetRecord* value);

// --- operations -------------------------------------------------------------

void encode(ByteWriter& writer, const TransferAuthorization& value);
bool decode(ByteReader& reader, TransferAuthorization* value);

void encode(ByteWriter& writer, const OperationRecord& value);
bool decode(ByteReader& reader, OperationRecord* value);

void encode(ByteWriter& writer, const ReplicationRequest& value);
bool decode(ByteReader& reader, ReplicationRequest* value);

void encode(ByteWriter& writer, const MigrationRequest& value);
bool decode(ByteReader& reader, MigrationRequest* value);

void encode(ByteWriter& writer, const OwnershipTransferRequest& value);
bool decode(ByteReader& reader, OwnershipTransferRequest* value);

void encode(ByteWriter& writer, const OperationCommit& value);
bool decode(ByteReader& reader, OperationCommit* value);

void encode(ByteWriter& writer, const OperationCancellation& value);
bool decode(ByteReader& reader, OperationCancellation* value);

// --- ownership --------------------------------------------------------------

void encode(ByteWriter& writer, const OwnershipRecord& value);
bool decode(ByteReader& reader, OwnershipRecord* value);

// --- compatibility ----------------------------------------------------------

void encode(ByteWriter& writer, const CompatibilityRecord& value);
bool decode(ByteReader& reader, CompatibilityRecord* value);

void encode(ByteWriter& writer, const CompatibilityRequirement& value);
bool decode(ByteReader& reader, CompatibilityRequirement* value);

// --- counters ---------------------------------------------------------------

void encode(ByteWriter& writer, const FabricCounters& value);
bool decode(ByteReader& reader, FabricCounters* value);

}  // namespace ccsf::detail

#endif  // CCSF_DETAIL_ENCODING_HPP
