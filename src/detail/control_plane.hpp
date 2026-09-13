// Cross-Cluster State Fabric - durable control plane and write-ahead log.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The durable control plane is a versioned, integrity checked snapshot plus a
// checksummed append-only journal of redo effects. A mutation is persisted
// before it is applied in memory and before it is acknowledged. Replay is
// idempotent by construction: effects are whole-record puts keyed by identity.
//
// Transactional checkpoint sequence:
//   serialize snapshot -> write temporary file -> flush -> close
//   -> atomically replace snapshot
//   -> write empty journal -> flush -> close -> atomically replace journal
// A crash between the two replacements re-applies journal records that are
// already present in the snapshot; replay is idempotent, so the state converges.

#ifndef CCSF_DETAIL_CONTROL_PLANE_HPP
#define CCSF_DETAIL_CONTROL_PLANE_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ccsf/bytes.hpp"
#include "ccsf/migration.hpp"
#include "ccsf/result.hpp"
#include "ccsf/snapshot.hpp"
#include "detail/encoding.hpp"

namespace ccsf::detail {

inline constexpr std::uint32_t kSnapshotMagic = 0x43435353U;   // "CCSS"
inline constexpr std::uint32_t kJournalMagic = 0x4343534AU;    // "CCSJ"
inline constexpr std::uint32_t kRecordMagic = 0x52454331U;     // "REC1"
inline constexpr std::uint16_t kControlPlaneFormatVersion = 1;
inline constexpr std::size_t kSnapshotHeaderBytes = 32;
inline constexpr std::size_t kJournalHeaderBytes = 12;
inline constexpr std::size_t kRecordHeaderBytes = 12;
inline constexpr std::size_t kMaxPayloadBytes = 512U * 1024U * 1024U;

/// Key identifying one state generation.
struct StateKey {
  StateId state_id;
  StateGeneration generation;

  friend bool operator<(const StateKey& a, const StateKey& b) {
    if (a.state_id != b.state_id) {
      return a.state_id < b.state_id;
    }
    return a.generation < b.generation;
  }
  friend bool operator==(const StateKey&, const StateKey&) = default;
};

enum class EffectKind : std::uint16_t {
  PUT_STATE = 1,
  PUT_CLUSTER = 2,
  PUT_REPLICA = 3,
  PUT_REPLICA_SET = 4,
  PUT_OPERATION = 5,
  PUT_AUTHORIZATION = 6,
  PUT_OWNERSHIP = 7,
  PUT_COMPATIBILITY = 8,
  PUT_PLACEMENT_POLICY = 9,
  PUT_REPLICATION_POLICY = 10,
  PUT_COUNTERS = 11,
  SET_EPOCH = 12,
  SET_NEXT_ID = 13
};

/// One redo effect. Exactly one payload member is meaningful, selected by kind.
struct Effect {
  EffectKind kind{EffectKind::PUT_STATE};
  StateGenerationRecord state;
  ClusterRecord cluster;
  ReplicaRecord replica;
  ReplicaSetRecord replica_set;
  OperationRecord operation;
  TransferAuthorization authorization;
  OwnershipRecord ownership;
  CompatibilityRecord compatibility;
  PlacementPolicyRecord placement_policy;
  ReplicationPolicyRecord replication_policy;
  FabricCounters counters;
  CoordinatorEpoch epoch;
  std::uint64_t scalar{0};

  static Effect set_next_id(std::uint64_t next_id);
  static Effect put_state(StateGenerationRecord record);
  static Effect put_cluster(ClusterRecord record);
  static Effect put_replica(ReplicaRecord record);
  static Effect put_replica_set(ReplicaSetRecord record);
  static Effect put_operation(OperationRecord record);
  static Effect put_authorization(TransferAuthorization record);
  static Effect put_ownership(OwnershipRecord record);
  static Effect put_compatibility(CompatibilityRecord record);
  static Effect put_placement_policy(PlacementPolicyRecord record);
  static Effect put_replication_policy(ReplicationPolicyRecord record);
  static Effect put_counters(FabricCounters counters);
  static Effect set_epoch(CoordinatorEpoch epoch);
};

void encode_effect(ByteWriter& writer, const Effect& effect);
bool decode_effect(ByteReader& reader, Effect* effect);

/// In-memory image of the durable control plane.
struct ControlPlane {
  CoordinatorId coordinator_id;
  CoordinatorEpoch coordinator_epoch;
  Tick logical_time{0};
  std::uint64_t durable_sequence{0};
  FabricCounters counters;

  std::map<StateKey, StateGenerationRecord> states;
  std::map<ClusterId, ClusterRecord> clusters;
  std::map<ReplicaId, ReplicaRecord> replicas;
  std::map<StateKey, ReplicaSetRecord> replica_sets;
  std::map<OperationId, OperationRecord> operations;
  std::map<OperationId, TransferAuthorization> authorizations;
  std::map<StateKey, OwnershipRecord> ownership;
  std::map<CompatibilityId, CompatibilityRecord> compatibility_records;
  std::map<PlacementPolicyId, PlacementPolicyRecord> placement_policies;
  std::map<ReplicationPolicyId, ReplicationPolicyRecord> replication_policies;

  std::uint64_t next_id{1};
  std::uint64_t replay_generation{1};
  std::uint64_t journal_records{0};
  std::uint64_t journal_tail_truncated{0};
  std::uint64_t snapshots_written{0};
  bool recovered{false};
  bool persistence_enabled{false};
  bool persistence_healthy{true};
  std::string persistence_detail;

  /// Applies one effect. Idempotent: applying the same effect twice is a no-op
  /// beyond the identity counter, which only moves forward.
  void apply(const Effect& effect);

  /// Mints the next identity value. Unique across every identity domain.
  std::uint64_t mint() noexcept { return next_id++; }

  [[nodiscard]] std::size_t record_count() const noexcept;

  /// Recomputes counters that are derived from live records.
  void recompute_derived_counters() noexcept;

  /// Checks structural invariants after load. Returns false and fills "reason"
  /// when the state contains impossible references.
  bool validate(std::string* reason) const;

  /// Sorts every collection for deterministic snapshot output.
  void canonicalize();

  void collect_snapshot(FabricSnapshot* snapshot) const;
};

/// Builds a set member view from a replica record.
ReplicaSetMember make_member(const ReplicaRecord& replica);

/// Rebuilds a replica set's membership from the authoritative replica records.
/// Membership is always derived, never hand-maintained, so counts cannot drift
/// and a replica set can never disagree with the replicas it describes.
ReplicaSetRecord rebuild_replica_set(const ControlPlane& plane, ReplicaSetRecord set);

void encode_control_plane(ByteWriter& writer, const ControlPlane& plane);
bool decode_control_plane(ByteReader& reader, ControlPlane* plane);

/// Encodes a complete journal record (header + payload) for the given effects.
std::vector<std::byte> encode_journal_record(const std::vector<Effect>& effects,
                                             std::uint64_t sequence, Tick tick,
                                             CoordinatorEpoch epoch);

/// One decoded journal record.
struct JournalRecordImage {
  std::vector<Effect> effects;
  std::uint64_t sequence{0};
  Tick tick{0};
};

/// Decodes one journal record payload. Returns false on corruption.
bool decode_journal_record(ByteSpan payload, JournalRecordImage* record);

struct JournalScanResult {
  std::vector<JournalRecordImage> records;
  std::size_t complete_records{0};
  std::size_t trailing_partial_bytes{0};
  std::size_t header_bytes{0};
};

/// Scans a journal file image. A single truncated tail record is reported and
/// ignored (an unacknowledged mutation). Any corruption in a complete record is
/// a hard error.
Result<JournalScanResult> scan_journal(ByteSpan image);

/// Durable store rooted at one directory.
class PersistenceStore {
 public:
  static Result<std::unique_ptr<PersistenceStore>> open(const std::string& directory);

  [[nodiscard]] const std::string& directory() const noexcept { return directory_; }
  [[nodiscard]] std::string snapshot_path() const;
  [[nodiscard]] std::string journal_path() const;

  /// Loads the durable control plane. Missing files yield an empty plane.
  Result<ControlPlane> load(std::uint64_t max_records) const;

  /// Appends a journal record and flushes it to stable storage.
  Status append(const std::vector<Effect>& effects, std::uint64_t sequence, Tick tick,
                CoordinatorEpoch epoch);

  /// Serializes a complete snapshot image (header + payload).
  static std::vector<std::byte> encode_snapshot(const ControlPlane& plane);

  /// Writes a pre-serialized snapshot and resets the journal. Callers must not
  /// hold the runtime state lock across this call.
  Status write_checkpoint(ByteSpan snapshot_image);

  /// Serialize + write in one step, for callers that already hold the image.
  Status checkpoint(const ControlPlane& plane);

 private:
  explicit PersistenceStore(std::string directory) : directory_(std::move(directory)) {}

  std::string directory_;
};

}  // namespace ccsf::detail

#endif  // CCSF_DETAIL_CONTROL_PLANE_HPP
