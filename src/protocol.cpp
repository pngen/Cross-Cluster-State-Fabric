// Cross-Cluster State Fabric - framed protocol implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/protocol.hpp"

#include <array>
#include <utility>

#include "ccsf/crc32c.hpp"

namespace ccsf {
namespace {

constexpr std::array<std::pair<MessageType, const char*>, 28> kMessageTypes{{
    {MessageType::INVALID, "INVALID"},
    {MessageType::HELLO, "HELLO"},
    {MessageType::HELLO_ACK, "HELLO_ACK"},
    {MessageType::EVIDENCE, "EVIDENCE"},
    {MessageType::EVIDENCE_ACK, "EVIDENCE_ACK"},
    {MessageType::HEARTBEAT, "HEARTBEAT"},
    {MessageType::HEARTBEAT_ACK, "HEARTBEAT_ACK"},
    {MessageType::TRANSFER_INTENT, "TRANSFER_INTENT"},
    {MessageType::TRANSFER_INTENT_ACK, "TRANSFER_INTENT_ACK"},
    {MessageType::TRANSFER_PROGRESS, "TRANSFER_PROGRESS"},
    {MessageType::TRANSFER_PROGRESS_ACK, "TRANSFER_PROGRESS_ACK"},
    {MessageType::TRANSFER_RESULT, "TRANSFER_RESULT"},
    {MessageType::TRANSFER_RESULT_ACK, "TRANSFER_RESULT_ACK"},
    {MessageType::VERIFY_RESULT, "VERIFY_RESULT"},
    {MessageType::VERIFY_RESULT_ACK, "VERIFY_RESULT_ACK"},
    {MessageType::COMMIT_NOTICE, "COMMIT_NOTICE"},
    {MessageType::COMMIT_NOTICE_ACK, "COMMIT_NOTICE_ACK"},
    {MessageType::REVALIDATE_NOTICE, "REVALIDATE_NOTICE"},
    {MessageType::REVALIDATE_NOTICE_ACK, "REVALIDATE_NOTICE_ACK"},
    {MessageType::SHUTDOWN, "SHUTDOWN"},
    {MessageType::SHUTDOWN_ACK, "SHUTDOWN_ACK"},
    {MessageType::ERROR_REPORT, "ERROR_REPORT"},
    {MessageType::DATA_REQUEST, "DATA_REQUEST"},
    {MessageType::DATA_CHUNK, "DATA_CHUNK"},
    {MessageType::HOST_STATE, "HOST_STATE"},
    {MessageType::HOST_STATE_ACK, "HOST_STATE_ACK"},
    {MessageType::CONTROL_REQUEST, "CONTROL_REQUEST"},
    {MessageType::CONTROL_RESPONSE, "CONTROL_RESPONSE"},
}};

constexpr std::size_t kMaxDetailLength = 512;
constexpr std::size_t kMaxLocationLength = 256;

void encode_header_fields(ByteWriter& writer, const FrameHeader& header) {
  writer.u32(kFrameMagic);
  writer.u16(header.protocol_version);
  writer.u16(static_cast<std::uint16_t>(header.message_type));
  writer.u32(header.flags);
  writer.u32(header.payload_length);
  writer.u32(header.payload_crc);
  writer.u64(header.coordinator_epoch.value());
  writer.u64(header.cluster_id.value());
  writer.u64(header.cluster_incarnation.value());
  writer.u64(header.worker_id.value());
  writer.u64(header.worker_boot_id.value());
  writer.u64(header.operation_generation.value());
  writer.u64(header.sequence);
}

/// Local identity codecs. The wire format stores identities as raw 64-bit
/// values; the typed wrappers are reconstructed on decode so that no identity
/// can silently cross domains.
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

}  // namespace

const char* to_string(MessageType type) noexcept {
  for (const auto& entry : kMessageTypes) {
    if (entry.first == type) {
      return entry.second;
    }
  }
  return "UNRECOGNIZED_MESSAGE_TYPE";
}

bool is_valid(MessageType type) noexcept {
  for (const auto& entry : kMessageTypes) {
    if (entry.first == type) {
      return true;
    }
  }
  return false;
}

std::vector<std::byte> encode_frame(const Frame& frame) {
  FrameHeader header = frame.header;
  header.protocol_version = kProtocolVersion;
  header.payload_length = static_cast<std::uint32_t>(frame.payload.size());
  header.payload_crc = crc32c(frame.payload_span());

  ByteWriter without_checksum;
  encode_header_fields(without_checksum, header);
  without_checksum.u32(0);
  const std::uint32_t header_crc = crc32c(without_checksum.data());

  ByteWriter image;
  encode_header_fields(image, header);
  image.u32(header_crc);
  image.raw(frame.payload_span());
  return image.data();
}

Result<FrameHeader> decode_frame_header(ByteSpan header_bytes) {
  if (header_bytes.size() != kFrameHeaderBytes) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header has the wrong length");
  }
  ByteReader reader(header_bytes);
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  std::uint16_t raw_type = 0;
  FrameHeader header;
  std::uint64_t field = 0;
  std::uint32_t checksum = 0;
  if (!reader.u32(&magic) || !reader.u16(&version) || !reader.u16(&raw_type) ||
      !reader.u32(&header.flags) || !reader.u32(&header.payload_length) ||
      !reader.u32(&header.payload_crc)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  if (magic != kFrameMagic) {
    return Error(ErrorCode::FRAME_CORRUPT, "frame magic mismatch");
  }
  if (version != kProtocolVersion) {
    return Error(ErrorCode::PROTOCOL_UNSUPPORTED, "unsupported protocol version");
  }
  header.protocol_version = version;
  if (!is_valid(static_cast<MessageType>(raw_type))) {
    return Error(ErrorCode::PROTOCOL_UNSUPPORTED, "unknown message type");
  }
  header.message_type = static_cast<MessageType>(raw_type);
  if (!reader.u64(&field)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  header.coordinator_epoch = CoordinatorEpoch::from_value(field);
  if (!reader.u64(&field)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  header.cluster_id = ClusterId::from_value(field);
  if (!reader.u64(&field)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  header.cluster_incarnation = ClusterIncarnationId::from_value(field);
  if (!reader.u64(&field)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  header.worker_id = WorkerId::from_value(field);
  if (!reader.u64(&field)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  header.worker_boot_id = WorkerBootId::from_value(field);
  if (!reader.u64(&field)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  header.operation_generation = OperationGeneration::from_value(field);
  if (!reader.u64(&header.sequence)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  if (!reader.u32(&checksum)) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame header is truncated");
  }
  if (header.payload_length > kMaxFramePayloadBytes) {
    return Error(ErrorCode::FRAME_TOO_LARGE, "declared payload exceeds the frame bound");
  }
  ByteWriter canonical;
  encode_header_fields(canonical, header);
  canonical.u32(0);
  if (crc32c(canonical.data()) != checksum) {
    return Error(ErrorCode::FRAME_CORRUPT, "frame header checksum mismatch");
  }
  return header;
}

Result<std::vector<std::byte>> decode_frame_payload(const FrameHeader& header,
                                                    ByteSpan payload_bytes) {
  if (payload_bytes.size() != header.payload_length) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame payload length mismatch");
  }
  if (crc32c(payload_bytes) != header.payload_crc) {
    return Error(ErrorCode::FRAME_CORRUPT, "frame payload checksum mismatch");
  }
  return std::vector<std::byte>(payload_bytes.begin(), payload_bytes.end());
}

Result<Frame> decode_frame(ByteSpan image, std::size_t max_payload) {
  if (image.size() < kFrameHeaderBytes) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame is shorter than its header");
  }
  auto header = decode_frame_header(image.first(kFrameHeaderBytes));
  if (!header) {
    return header.error();
  }
  if (header.value().payload_length > max_payload) {
    return Error(ErrorCode::FRAME_TOO_LARGE, "declared payload exceeds the receive bound");
  }
  const std::uint64_t expected =
      static_cast<std::uint64_t>(kFrameHeaderBytes) + header.value().payload_length;
  if (image.size() != expected) {
    return Error(ErrorCode::FRAME_TRUNCATED, "frame image does not match its declared length");
  }
  auto payload = decode_frame_payload(header.value(), image.subspan(kFrameHeaderBytes));
  if (!payload) {
    return payload.error();
  }
  Frame frame;
  frame.header = header.value();
  frame.payload = std::move(payload.value());
  return frame;
}

// --- payload codecs ---------------------------------------------------------

void encode(ByteWriter& writer, const HelloPayload& value) {
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.region);
  encode_id(writer, value.site);
  encode_id(writer, value.failure_domain);
  encode_id(writer, value.worker_id);
  encode_id(writer, value.worker_boot_id);
  encode_id(writer, value.prior_incarnation);
  writer.str(value.location_descriptor);
  writer.u32(static_cast<std::uint32_t>(value.capabilities.tags.size()));
  for (const std::string& tag : value.capabilities.tags) {
    writer.str(tag);
  }
  writer.u32(static_cast<std::uint32_t>(value.capabilities.storage_classes.size()));
  for (const std::string& storage : value.capabilities.storage_classes) {
    writer.str(storage);
  }
  writer.u32(static_cast<std::uint32_t>(value.capabilities.accelerator_architectures.size()));
  for (const std::string& architecture : value.capabilities.accelerator_architectures) {
    writer.str(architecture);
  }
  writer.boolean(value.capabilities.encryption_at_rest);
  writer.boolean(value.capabilities.attestation);
  writer.u64(value.capacity.total_logical_bytes);
  writer.u64(value.capacity.used_logical_bytes);
  writer.u64(value.capacity.inbound_bytes_per_tick);
  writer.u32(value.capacity.free_transfer_slots);
  writer.u32(value.capacity.utilization_per_mille);
  encode_id(writer, value.expected_coordinator_epoch);
  writer.u64(value.listen_port);
}

bool decode(ByteReader& reader, HelloPayload* value) {
  if (!decode_id(reader, &value->cluster_id)) return false;
  if (!decode_id(reader, &value->region)) return false;
  if (!decode_id(reader, &value->site)) return false;
  if (!decode_id(reader, &value->failure_domain)) return false;
  if (!decode_id(reader, &value->worker_id)) return false;
  if (!decode_id(reader, &value->worker_boot_id)) return false;
  if (!decode_id(reader, &value->prior_incarnation)) return false;
  if (!reader.str(kMaxLocationLength, &value->location_descriptor)) return false;
  std::uint32_t count = 0;
  if (!reader.u32(&count)) return false;
  if (count > kMaxClusterTagCount) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "capability tag count exceeds the bound");
    return false;
  }
  value->capabilities.tags.clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string tag;
    if (!reader.str(kMaxClusterTagLength, &tag)) return false;
    value->capabilities.tags.push_back(std::move(tag));
  }
  if (!reader.u32(&count)) return false;
  if (count > kMaxClusterTagCount) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "storage class count exceeds the bound");
    return false;
  }
  value->capabilities.storage_classes.clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string storage;
    if (!reader.str(kMaxClusterTagLength, &storage)) return false;
    value->capabilities.storage_classes.push_back(std::move(storage));
  }
  if (!reader.u32(&count)) return false;
  if (count > kMaxClusterTagCount) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "accelerator count exceeds the bound");
    return false;
  }
  value->capabilities.accelerator_architectures.clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string architecture;
    if (!reader.str(kMaxClusterTagLength, &architecture)) return false;
    value->capabilities.accelerator_architectures.push_back(std::move(architecture));
  }
  if (!reader.boolean(&value->capabilities.encryption_at_rest)) return false;
  if (!reader.boolean(&value->capabilities.attestation)) return false;
  if (!reader.u64(&value->capacity.total_logical_bytes)) return false;
  if (!reader.u64(&value->capacity.used_logical_bytes)) return false;
  if (value->capacity.used_logical_bytes > value->capacity.total_logical_bytes) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "reported capacity is inconsistent");
    return false;
  }
  if (!reader.u64(&value->capacity.inbound_bytes_per_tick)) return false;
  if (!reader.u32(&value->capacity.free_transfer_slots)) return false;
  if (!reader.u32(&value->capacity.utilization_per_mille)) return false;
  if (value->capacity.utilization_per_mille > 1000U) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "reported utilization exceeds 1000 per mille");
    return false;
  }
  if (!decode_id(reader, &value->expected_coordinator_epoch)) return false;
  if (!reader.u64(&value->listen_port)) return false;
  return reader.at_end();
}

void encode(ByteWriter& writer, const HelloAckPayload& value) {
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.incarnation);
  encode_id(writer, value.worker_id);
  encode_id(writer, value.worker_boot_id);
  encode_id(writer, value.coordinator_id);
  encode_id(writer, value.coordinator_epoch);
  writer.u32(value.accepted);
  writer.str(value.detail);
}

bool decode(ByteReader& reader, HelloAckPayload* value) {
  return decode_id(reader, &value->cluster_id) && decode_id(reader, &value->incarnation) &&
         decode_id(reader, &value->worker_id) && decode_id(reader, &value->worker_boot_id) &&
         decode_id(reader, &value->coordinator_id) &&
         decode_id(reader, &value->coordinator_epoch) && reader.u32(&value->accepted) &&
         reader.str(kMaxDetailLength, &value->detail) && reader.at_end();
}

void encode(ByteWriter& writer, const EvidencePayload& value) {
  encode_id(writer, value.cluster_id);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.worker_boot_id);
  writer.u16(static_cast<std::uint16_t>(value.health));
  writer.u32(static_cast<std::uint32_t>(value.capabilities.tags.size()));
  for (const std::string& tag : value.capabilities.tags) {
    writer.str(tag);
  }
  writer.u32(static_cast<std::uint32_t>(value.capabilities.accelerator_architectures.size()));
  for (const std::string& architecture : value.capabilities.accelerator_architectures) {
    writer.str(architecture);
  }
  writer.u64(value.capacity.total_logical_bytes);
  writer.u64(value.capacity.used_logical_bytes);
  writer.u64(value.capacity.inbound_bytes_per_tick);
  writer.u32(value.capacity.free_transfer_slots);
  writer.u32(value.capacity.utilization_per_mille);
  encode_id(writer, value.expected_coordinator_epoch);
}

bool decode(ByteReader& reader, EvidencePayload* value) {
  std::uint16_t health = 0;
  if (!decode_id(reader, &value->cluster_id)) return false;
  if (!decode_id(reader, &value->cluster_incarnation)) return false;
  if (!decode_id(reader, &value->worker_boot_id)) return false;
  if (!reader.u16(&health)) return false;
  if (!is_valid(static_cast<ClusterHealthState>(health))) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "invalid cluster health value");
    return false;
  }
  value->health = static_cast<ClusterHealthState>(health);
  std::uint32_t count = 0;
  if (!reader.u32(&count)) return false;
  if (count > kMaxClusterTagCount) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "capability tag count exceeds the bound");
    return false;
  }
  value->capabilities.tags.clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string tag;
    if (!reader.str(kMaxClusterTagLength, &tag)) return false;
    value->capabilities.tags.push_back(std::move(tag));
  }
  if (!reader.u32(&count)) return false;
  if (count > kMaxClusterTagCount) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "accelerator count exceeds the bound");
    return false;
  }
  value->capabilities.accelerator_architectures.clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string architecture;
    if (!reader.str(kMaxClusterTagLength, &architecture)) return false;
    value->capabilities.accelerator_architectures.push_back(std::move(architecture));
  }
  if (!reader.u64(&value->capacity.total_logical_bytes)) return false;
  if (!reader.u64(&value->capacity.used_logical_bytes)) return false;
  if (value->capacity.used_logical_bytes > value->capacity.total_logical_bytes) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "reported capacity is inconsistent");
    return false;
  }
  if (!reader.u64(&value->capacity.inbound_bytes_per_tick)) return false;
  if (!reader.u32(&value->capacity.free_transfer_slots)) return false;
  if (!reader.u32(&value->capacity.utilization_per_mille)) return false;
  if (value->capacity.utilization_per_mille > 1000U) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "reported utilization exceeds 1000 per mille");
    return false;
  }
  if (!decode_id(reader, &value->expected_coordinator_epoch)) return false;
  return reader.at_end();
}

void encode(ByteWriter& writer, const TransferIntentPayload& value) {
  encode_id(writer, value.transfer_id);
  encode_id(writer, value.transfer_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.migration_id);
  encode_id(writer, value.migration_generation);
  encode_id(writer, value.reservation_id);
  encode_id(writer, value.reservation_generation);
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.destination_replica);
  encode_id(writer, value.destination_replica_generation);
  encode_id(writer, value.destination_cluster);
  encode_id(writer, value.destination_incarnation);
  encode_id(writer, value.source_cluster);
  encode_id(writer, value.source_incarnation);
  encode_id(writer, value.source_replica);
  writer.u64(value.payload_bytes);
  writer.raw(ByteSpan(reinterpret_cast<const std::byte*>(value.expected_digest.bytes.data()),
                      value.expected_digest.bytes.size()));
  writer.u64(value.payload_seed);
  encode_id(writer, value.coordinator_epoch);
  writer.u64(value.reservation_expires_at);
  writer.str(value.source_host);
  writer.u64(value.source_data_port);
  writer.u64(value.destination_data_port);
}

bool decode(ByteReader& reader, TransferIntentPayload* value) {
  if (!decode_id(reader, &value->transfer_id)) return false;
  if (!decode_id(reader, &value->transfer_generation)) return false;
  if (!decode_id(reader, &value->operation_id)) return false;
  if (!decode_id(reader, &value->operation_generation)) return false;
  if (!decode_id(reader, &value->migration_id)) return false;
  if (!decode_id(reader, &value->migration_generation)) return false;
  if (!decode_id(reader, &value->reservation_id)) return false;
  if (!decode_id(reader, &value->reservation_generation)) return false;
  if (!decode_id(reader, &value->state_id)) return false;
  if (!decode_id(reader, &value->state_generation)) return false;
  if (!decode_id(reader, &value->destination_replica)) return false;
  if (!decode_id(reader, &value->destination_replica_generation)) return false;
  if (!decode_id(reader, &value->destination_cluster)) return false;
  if (!decode_id(reader, &value->destination_incarnation)) return false;
  if (!decode_id(reader, &value->source_cluster)) return false;
  if (!decode_id(reader, &value->source_incarnation)) return false;
  if (!decode_id(reader, &value->source_replica)) return false;
  if (!reader.u64(&value->payload_bytes)) return false;
  if (value->payload_bytes > kMaxFramePayloadBytes) {
    reader.fail(ErrorCode::FRAME_TOO_LARGE, "declared transfer payload exceeds the bound");
    return false;
  }
  ByteSpan digest_bytes;
  if (!reader.raw(kSha256Length, &digest_bytes)) return false;
  for (std::size_t index = 0; index < kSha256Length; ++index) {
    value->expected_digest.bytes[index] = std::to_integer<std::uint8_t>(digest_bytes[index]);
  }
  if (!reader.u64(&value->payload_seed)) return false;
  if (!decode_id(reader, &value->coordinator_epoch)) return false;
  if (!reader.u64(&value->reservation_expires_at)) return false;
  if (!reader.str(kMaxLocationLength, &value->source_host)) return false;
  if (!reader.u64(&value->source_data_port)) return false;
  if (!reader.u64(&value->destination_data_port)) return false;
  return reader.at_end();
}

void encode(ByteWriter& writer, const TransferReportPayload& value) {
  encode_id(writer, value.transfer_id);
  encode_id(writer, value.transfer_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.worker_boot_id);
  writer.u64(value.bytes_transferred);
  writer.raw(ByteSpan(reinterpret_cast<const std::byte*>(value.measured_digest.bytes.data()),
                      value.measured_digest.bytes.size()));
  writer.u32(value.completed);
  writer.u32(value.failed);
  writer.str(value.detail);
}

bool decode(ByteReader& reader, TransferReportPayload* value) {
  if (!decode_id(reader, &value->transfer_id)) return false;
  if (!decode_id(reader, &value->transfer_generation)) return false;
  if (!decode_id(reader, &value->operation_id)) return false;
  if (!decode_id(reader, &value->operation_generation)) return false;
  if (!decode_id(reader, &value->replica_id)) return false;
  if (!decode_id(reader, &value->replica_generation)) return false;
  if (!decode_id(reader, &value->cluster_incarnation)) return false;
  if (!decode_id(reader, &value->worker_boot_id)) return false;
  if (!reader.u64(&value->bytes_transferred)) return false;
  ByteSpan digest_bytes;
  if (!reader.raw(kSha256Length, &digest_bytes)) return false;
  for (std::size_t index = 0; index < kSha256Length; ++index) {
    value->measured_digest.bytes[index] = std::to_integer<std::uint8_t>(digest_bytes[index]);
  }
  if (!reader.u32(&value->completed)) return false;
  if (!reader.u32(&value->failed)) return false;
  if (value->completed > 1U || value->failed > 1U) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "invalid boolean flag in transfer report");
    return false;
  }
  if (!reader.str(kMaxDetailLength, &value->detail)) return false;
  return reader.at_end();
}

void encode(ByteWriter& writer, const VerifyReportPayload& value) {
  encode_id(writer, value.verification_id);
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  encode_id(writer, value.operation_id);
  encode_id(writer, value.operation_generation);
  encode_id(writer, value.cluster_incarnation);
  encode_id(writer, value.worker_boot_id);
  writer.u32(value.integrity_verified);
  writer.raw(ByteSpan(reinterpret_cast<const std::byte*>(value.measured_digest.bytes.data()),
                      value.measured_digest.bytes.size()));
  writer.u32(value.compatibility_verified);
  writer.u64(value.verified_bytes);
  writer.u32(value.durability);
  writer.str(value.detail);
}

bool decode(ByteReader& reader, VerifyReportPayload* value) {
  if (!decode_id(reader, &value->verification_id)) return false;
  if (!decode_id(reader, &value->replica_id)) return false;
  if (!decode_id(reader, &value->replica_generation)) return false;
  if (!decode_id(reader, &value->operation_id)) return false;
  if (!decode_id(reader, &value->operation_generation)) return false;
  if (!decode_id(reader, &value->cluster_incarnation)) return false;
  if (!decode_id(reader, &value->worker_boot_id)) return false;
  if (!reader.u32(&value->integrity_verified)) return false;
  ByteSpan digest_bytes;
  if (!reader.raw(kSha256Length, &digest_bytes)) return false;
  for (std::size_t index = 0; index < kSha256Length; ++index) {
    value->measured_digest.bytes[index] = std::to_integer<std::uint8_t>(digest_bytes[index]);
  }
  if (!reader.u32(&value->compatibility_verified)) return false;
  if (!reader.u64(&value->verified_bytes)) return false;
  if (!reader.u32(&value->durability)) return false;
  if (value->durability > static_cast<std::uint32_t>(DurabilityClass::ARCHIVAL)) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "invalid durability class in verify report");
    return false;
  }
  if (!reader.str(kMaxDetailLength, &value->detail)) return false;
  return reader.at_end();
}

void encode(ByteWriter& writer, const DataRequestPayload& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.replica_id);
  writer.u64(value.offset);
  writer.u64(value.length);
}

bool decode(ByteReader& reader, DataRequestPayload* value) {
  return decode_id(reader, &value->state_id) && decode_id(reader, &value->state_generation) &&
         decode_id(reader, &value->replica_id) && reader.u64(&value->offset) &&
         reader.u64(&value->length) && reader.at_end();
}

void encode(ByteWriter& writer, const DataChunkPayload& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.replica_id);
  writer.u64(value.offset);
  writer.u32(value.last);
  writer.raw(ByteSpan(reinterpret_cast<const std::byte*>(value.digest.bytes.data()),
                      value.digest.bytes.size()));
  writer.blob(ByteSpan(value.bytes.data(), value.bytes.size()));
}

bool decode(ByteReader& reader, DataChunkPayload* value, std::size_t max_bytes) {
  if (!decode_id(reader, &value->state_id)) return false;
  if (!decode_id(reader, &value->state_generation)) return false;
  if (!decode_id(reader, &value->replica_id)) return false;
  if (!reader.u64(&value->offset)) return false;
  if (!reader.u32(&value->last)) return false;
  if (value->last > 1U) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "invalid last flag in a data chunk");
    return false;
  }
  ByteSpan digest_bytes;
  if (!reader.raw(kSha256Length, &digest_bytes)) return false;
  for (std::size_t index = 0; index < kSha256Length; ++index) {
    value->digest.bytes[index] = std::to_integer<std::uint8_t>(digest_bytes[index]);
  }
  if (!reader.blob(max_bytes, &value->bytes)) return false;
  return reader.at_end();
}

void encode(ByteWriter& writer, const HostStatePayload& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.replica_id);
  encode_id(writer, value.replica_generation);
  writer.raw(ByteSpan(reinterpret_cast<const std::byte*>(value.expected_digest.bytes.data()),
                      value.expected_digest.bytes.size()));
  writer.blob(ByteSpan(value.bytes.data(), value.bytes.size()));
}

bool decode(ByteReader& reader, HostStatePayload* value, std::size_t max_bytes) {
  if (!decode_id(reader, &value->state_id)) return false;
  if (!decode_id(reader, &value->state_generation)) return false;
  if (!decode_id(reader, &value->replica_id)) return false;
  if (!decode_id(reader, &value->replica_generation)) return false;
  ByteSpan digest_bytes;
  if (!reader.raw(kSha256Length, &digest_bytes)) return false;
  for (std::size_t index = 0; index < kSha256Length; ++index) {
    value->expected_digest.bytes[index] = std::to_integer<std::uint8_t>(digest_bytes[index]);
  }
  if (!reader.blob(max_bytes, &value->bytes)) return false;
  return reader.at_end();
}

void encode(ByteWriter& writer, const SimplePayload& value) {
  writer.u32(value.ok);
  writer.u16(static_cast<std::uint16_t>(value.code));
  writer.str(value.detail);
  writer.str(value.extra);
}

bool decode(ByteReader& reader, SimplePayload* value) {
  std::uint16_t code = 0;
  if (!reader.u32(&value->ok)) return false;
  if (!reader.u16(&code)) return false;
  value->code = static_cast<ErrorCode>(code);
  if (!reader.str(kMaxDetailLength, &value->detail)) return false;
  if (!reader.str(kMaxDetailLength, &value->extra)) return false;
  return reader.at_end();
}

}  // namespace ccsf
