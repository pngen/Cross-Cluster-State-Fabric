// Cross-Cluster State Fabric - versioned framed transport protocol.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The frame header is fixed width, big-endian, and carries both the authority
// metadata the receiver must fence against and the integrity of the payload.
// The header checksum is computed over the header with its own checksum field
// canonically zeroed, so the checksum never covers itself.

#ifndef CCSF_PROTOCOL_HPP
#define CCSF_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/bytes.hpp"
#include "ccsf/cluster.hpp"
#include "ccsf/digest.hpp"
#include "ccsf/ids.hpp"
#include "ccsf/result.hpp"
#include "ccsf/taxonomy.hpp"

namespace ccsf {

inline constexpr std::uint32_t kFrameMagic = 0x43435346U;  // "CCSF"
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 80;
inline constexpr std::size_t kMaxFramePayloadBytes = 8U * 1024U * 1024U;

/// Message types. Unknown types are rejected as PROTOCOL_UNSUPPORTED rather than
/// silently ignored.
enum class MessageType : std::uint16_t {
  INVALID = 0,
  HELLO = 1,
  HELLO_ACK = 2,
  EVIDENCE = 3,
  EVIDENCE_ACK = 4,
  HEARTBEAT = 5,
  HEARTBEAT_ACK = 6,
  TRANSFER_INTENT = 10,
  TRANSFER_INTENT_ACK = 11,
  TRANSFER_PROGRESS = 12,
  TRANSFER_PROGRESS_ACK = 13,
  TRANSFER_RESULT = 14,
  TRANSFER_RESULT_ACK = 15,
  VERIFY_RESULT = 16,
  VERIFY_RESULT_ACK = 17,
  COMMIT_NOTICE = 20,
  COMMIT_NOTICE_ACK = 21,
  REVALIDATE_NOTICE = 22,
  REVALIDATE_NOTICE_ACK = 23,
  SHUTDOWN = 30,
  SHUTDOWN_ACK = 31,
  ERROR_REPORT = 40,

  /// Agent-to-agent bulk data plane. The transfer itself is a real byte stream
  /// between two independent processes.
  DATA_REQUEST = 50,
  DATA_CHUNK = 51,
  HOST_STATE = 52,
  HOST_STATE_ACK = 53,

  /// Controller-to-coordinator control plane.
  CONTROL_REQUEST = 60,
  CONTROL_RESPONSE = 61
};

const char* to_string(MessageType type) noexcept;
bool is_valid(MessageType type) noexcept;

/// Authority metadata carried by every frame.
struct FrameHeader {
  std::uint16_t protocol_version{kProtocolVersion};
  MessageType message_type{MessageType::INVALID};
  std::uint32_t flags{0};
  std::uint32_t payload_length{0};
  std::uint32_t payload_crc{0};
  CoordinatorEpoch coordinator_epoch;
  ClusterId cluster_id;
  ClusterIncarnationId cluster_incarnation;
  WorkerId worker_id;
  WorkerBootId worker_boot_id;
  OperationGeneration operation_generation;
  std::uint64_t sequence{0};
};

/// A complete frame: header plus payload bytes.
struct Frame {
  FrameHeader header;
  std::vector<std::byte> payload;

  [[nodiscard]] ByteSpan payload_span() const {
    return ByteSpan(payload.data(), payload.size());
  }
};

/// Serializes a frame, computing the payload and header checksums.
std::vector<std::byte> encode_frame(const Frame& frame);

/// Decodes and validates a fixed-size header. Rejects bad magic, unsupported
/// versions, oversized payload declarations and checksum mismatches.
Result<FrameHeader> decode_frame_header(ByteSpan header_bytes);

/// Validates a complete frame image (header + payload).
Result<Frame> decode_frame(ByteSpan image, std::size_t max_payload);

/// Decodes only the payload against an already validated header.
Result<std::vector<std::byte>> decode_frame_payload(const FrameHeader& header,
                                                    ByteSpan payload_bytes);

// --- payload codecs ---------------------------------------------------------

/// HELLO: agent registration.
struct HelloPayload {
  ClusterId cluster_id;
  RegionId region;
  SiteId site;
  FailureDomainId failure_domain;
  WorkerId worker_id;
  WorkerBootId worker_boot_id;
  ClusterIncarnationId prior_incarnation;
  std::string location_descriptor;
  ClusterCapabilities capabilities;
  ClusterCapacity capacity;
  CoordinatorEpoch expected_coordinator_epoch;
  std::uint64_t listen_port{0};
};

void encode(ByteWriter& writer, const HelloPayload& value);
bool decode(ByteReader& reader, HelloPayload* value);

/// HELLO_ACK: the coordinator's answer, carrying fresh authority.
struct HelloAckPayload {
  ClusterId cluster_id;
  ClusterIncarnationId incarnation;
  WorkerId worker_id;
  WorkerBootId worker_boot_id;
  CoordinatorId coordinator_id;
  CoordinatorEpoch coordinator_epoch;
  std::uint32_t accepted{0};
  std::string detail;
};

void encode(ByteWriter& writer, const HelloAckPayload& value);
bool decode(ByteReader& reader, HelloAckPayload* value);

/// EVIDENCE: cluster capability, capacity and health publication.
struct EvidencePayload {
  ClusterId cluster_id;
  ClusterIncarnationId cluster_incarnation;
  WorkerBootId worker_boot_id;
  ClusterHealthState health{ClusterHealthState::UNKNOWN};
  ClusterCapabilities capabilities;
  ClusterCapacity capacity;
  CoordinatorEpoch expected_coordinator_epoch;
};

void encode(ByteWriter& writer, const EvidencePayload& value);
bool decode(ByteReader& reader, EvidencePayload* value);

/// TRANSFER_INTENT: the coordinator authorizes a transfer to an agent.
struct TransferIntentPayload {
  TransferId transfer_id;
  TransferGeneration transfer_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
  MigrationId migration_id;
  MigrationGeneration migration_generation;
  ReservationId reservation_id;
  ReservationGeneration reservation_generation;
  StateId state_id;
  StateGeneration state_generation;
  ReplicaId destination_replica;
  ReplicaGeneration destination_replica_generation;
  ClusterId destination_cluster;
  ClusterIncarnationId destination_incarnation;
  ClusterId source_cluster;
  ClusterIncarnationId source_incarnation;
  ReplicaId source_replica;
  std::uint64_t payload_bytes{0};
  Digest expected_digest;
  std::uint64_t payload_seed{0};
  CoordinatorEpoch coordinator_epoch;
  Tick reservation_expires_at{0};

  /// Address of the source agent's data plane, used to pull the bytes. The
  /// reference deployment uses loopback; the mechanism is transport-neutral.
  std::string source_host;
  std::uint64_t source_data_port{0};
  std::uint64_t destination_data_port{0};
};

void encode(ByteWriter& writer, const TransferIntentPayload& value);
bool decode(ByteReader& reader, TransferIntentPayload* value);

/// TRANSFER_PROGRESS / TRANSFER_RESULT: agent reports about a live transfer.
struct TransferReportPayload {
  TransferId transfer_id;
  TransferGeneration transfer_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  ClusterIncarnationId cluster_incarnation;
  WorkerBootId worker_boot_id;
  std::uint64_t bytes_transferred{0};
  Digest measured_digest;
  std::uint32_t completed{0};
  std::uint32_t failed{0};
  std::string detail;
};

void encode(ByteWriter& writer, const TransferReportPayload& value);
bool decode(ByteReader& reader, TransferReportPayload* value);

/// VERIFY_RESULT: agent reports the outcome of destination verification.
struct VerifyReportPayload {
  VerificationId verification_id;
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  OperationId operation_id;
  OperationGeneration operation_generation;
  ClusterIncarnationId cluster_incarnation;
  WorkerBootId worker_boot_id;
  std::uint32_t integrity_verified{0};
  Digest measured_digest;
  std::uint32_t compatibility_verified{0};
  std::uint64_t verified_bytes{0};
  std::uint32_t durability{0};
  std::string detail;
};

void encode(ByteWriter& writer, const VerifyReportPayload& value);
bool decode(ByteReader& reader, VerifyReportPayload* value);

/// DATA_REQUEST: a peer asks an agent for the bytes of one hosted replica.
struct DataRequestPayload {
  StateId state_id;
  StateGeneration state_generation;
  ReplicaId replica_id;
  std::uint64_t offset{0};
  std::uint64_t length{0};
};

void encode(ByteWriter& writer, const DataRequestPayload& value);
bool decode(ByteReader& reader, DataRequestPayload* value);

/// DATA_CHUNK: a bounded slice of replica bytes plus its position.
struct DataChunkPayload {
  StateId state_id;
  StateGeneration state_generation;
  ReplicaId replica_id;
  std::uint64_t offset{0};
  std::uint32_t last{0};
  Digest digest;
  std::vector<std::byte> bytes;
};

void encode(ByteWriter& writer, const DataChunkPayload& value);
bool decode(ByteReader& reader, DataChunkPayload* value, std::size_t max_bytes);

/// HOST_STATE: pushes replica bytes into an agent's local store.
struct HostStatePayload {
  StateId state_id;
  StateGeneration state_generation;
  ReplicaId replica_id;
  ReplicaGeneration replica_generation;
  Digest expected_digest;
  std::vector<std::byte> bytes;
};

void encode(ByteWriter& writer, const HostStatePayload& value);
bool decode(ByteReader& reader, HostStatePayload* value, std::size_t max_bytes);

/// A simple acknowledgement or error report.
struct SimplePayload {
  std::uint32_t ok{0};
  ErrorCode code{ErrorCode::OK};
  std::string detail;
  std::string extra;
};

void encode(ByteWriter& writer, const SimplePayload& value);
bool decode(ByteReader& reader, SimplePayload* value);

}  // namespace ccsf

#endif  // CCSF_PROTOCOL_HPP
