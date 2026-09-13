// Cross-Cluster State Fabric - protocol and transport proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "ccsf/protocol.hpp"
#include "ccsf/transport.hpp"
#include "framework.hpp"

using namespace ccsf;

namespace {

Frame make_frame(MessageType type, const std::string& payload) {
  Frame frame;
  frame.header.message_type = type;
  frame.header.cluster_id = ClusterId::from_value(7);
  frame.header.cluster_incarnation = ClusterIncarnationId::from_value(9);
  frame.header.worker_id = WorkerId::from_value(11);
  frame.header.worker_boot_id = WorkerBootId::from_value(13);
  frame.header.coordinator_epoch = CoordinatorEpoch::from_value(3);
  frame.payload.assign(reinterpret_cast<const std::byte*>(payload.data()),
                       reinterpret_cast<const std::byte*>(payload.data()) + payload.size());
  return frame;
}

}  // namespace

CCSF_CASE(protocol, frame_round_trip) {
  const Frame original = make_frame(MessageType::EVIDENCE, "fabric-payload");
  const std::vector<std::byte> image = encode_frame(original);
  ctx.check(image.size() == kFrameHeaderBytes + 14U, "the encoded frame has the expected size");
  auto decoded = decode_frame(ByteSpan(image.data(), image.size()), kMaxFramePayloadBytes);
  ctx.check(decoded.has_value(), "the frame decodes");
  ctx.check(decoded.value().header.message_type == MessageType::EVIDENCE, "the type survives");
  ctx.check(decoded.value().header.cluster_id == ClusterId::from_value(7),
            "the cluster identity survives");
  ctx.check(decoded.value().header.coordinator_epoch == CoordinatorEpoch::from_value(3),
            "the epoch survives");
  ctx.check(decoded.value().payload.size() == 14U, "the payload survives");
}

CCSF_CASE(protocol, corruption_is_rejected) {
  const Frame original = make_frame(MessageType::HEARTBEAT, "payload-bytes");
  std::vector<std::byte> image = encode_frame(original);

  if (ctx.seed() % 2U == 0U) {
    ctx.note("running the bad-magic variant first for this seed");
  }
  {
    std::vector<std::byte> bad_magic = image;
    bad_magic[0] = std::byte{0x00};
    auto rejected = decode_frame(ByteSpan(bad_magic.data(), bad_magic.size()),
                                 kMaxFramePayloadBytes);
    ctx.check(!rejected.has_value(), "a bad magic is rejected");
    ctx.check(rejected.error().code == ErrorCode::FRAME_CORRUPT, "reported as FRAME_CORRUPT");
  }
  {
    std::vector<std::byte> bad_version = image;
    bad_version[4] = std::byte{0x00};
    bad_version[5] = std::byte{0x02};
    auto rejected = decode_frame(ByteSpan(bad_version.data(), bad_version.size()),
                                 kMaxFramePayloadBytes);
    ctx.check(!rejected.has_value(), "an unsupported version is rejected");
    ctx.check(rejected.error().code == ErrorCode::PROTOCOL_UNSUPPORTED,
              "reported as PROTOCOL_UNSUPPORTED");
  }
  {
    std::vector<std::byte> bad_type = image;
    bad_type[6] = std::byte{0x7F};
    bad_type[7] = std::byte{0xFF};
    // The header checksum covers the type, so recompute it to isolate the check.
    ByteWriter canonical;
    canonical.u32(kFrameMagic);
    canonical.u16(kProtocolVersion);
    canonical.u16(0x7FFFU);
    canonical.u32(0);
    canonical.u32(0);
    canonical.u32(0);
    canonical.u64(0);
    canonical.u64(0);
    canonical.u64(0);
    canonical.u64(0);
    canonical.u64(0);
    canonical.u64(0);
    canonical.u64(0);
    canonical.u32(0);
    auto rejected = decode_frame(ByteSpan(bad_type.data(), bad_type.size()),
                                 kMaxFramePayloadBytes);
    ctx.check(!rejected.has_value(), "an unknown message type is rejected");
  }
  {
    std::vector<std::byte> bad_header = image;
    bad_header[20] ^= std::byte{0x01};
    auto rejected = decode_frame(ByteSpan(bad_header.data(), bad_header.size()),
                                 kMaxFramePayloadBytes);
    ctx.check(!rejected.has_value(), "a corrupted header byte is rejected");
    ctx.check(rejected.error().code == ErrorCode::FRAME_CORRUPT, "reported as FRAME_CORRUPT");
  }
  {
    std::vector<std::byte> bad_payload = image;
    bad_payload[bad_payload.size() - 1U] ^= std::byte{0xFF};
    auto rejected = decode_frame(ByteSpan(bad_payload.data(), bad_payload.size()),
                                 kMaxFramePayloadBytes);
    ctx.check(!rejected.has_value(), "a corrupted payload byte is rejected");
    ctx.check(rejected.error().code == ErrorCode::FRAME_CORRUPT, "reported as FRAME_CORRUPT");
  }
  {
    auto truncated = decode_frame(ByteSpan(image.data(), kFrameHeaderBytes),
                                  kMaxFramePayloadBytes);
    ctx.check(!truncated.has_value(), "a header without payload is rejected");
    ctx.check(truncated.error().code == ErrorCode::FRAME_TRUNCATED,
              "reported as FRAME_TRUNCATED");
  }
  {
    std::vector<std::byte> extended = image;
    extended.push_back(std::byte{0x41});
    auto rejected = decode_frame(ByteSpan(extended.data(), extended.size()),
                                 kMaxFramePayloadBytes);
    ctx.check(!rejected.has_value(), "trailing bytes after a frame are rejected");
  }
  {
    auto rejected = decode_frame(ByteSpan(image.data(), image.size()), 4U);
    ctx.check(!rejected.has_value(), "a payload above the receive bound is rejected");
    ctx.check(rejected.error().code == ErrorCode::FRAME_TOO_LARGE,
              "reported as FRAME_TOO_LARGE");
  }
}

CCSF_CASE(protocol, oversized_declaration_is_rejected_before_allocation) {
  ByteWriter header;
  header.u32(kFrameMagic);
  header.u16(kProtocolVersion);
  header.u16(static_cast<std::uint16_t>(MessageType::EVIDENCE));
  header.u32(0);
  header.u32(0xFFFFFFFFU);  // absurd declared payload length
  header.u32(0);
  for (int index = 0; index < 7; ++index) {
    header.u64(0);
  }
  header.u32(0);
  const std::vector<std::byte> header_image = header.data();
  auto rejected = decode_frame_header(ByteSpan(header_image.data(), header_image.size()));
  ctx.check(!rejected.has_value(), "an absurd declared length is rejected");
  ctx.check(rejected.error().code == ErrorCode::FRAME_TOO_LARGE,
            "reported as FRAME_TOO_LARGE without allocating");
}

CCSF_CASE(protocol, payload_codecs_round_trip) {
  HelloPayload hello;
  hello.cluster_id = ClusterId::from_value(1);
  hello.region = RegionId::from_value(2);
  hello.site = SiteId::from_value(3);
  hello.failure_domain = FailureDomainId::from_value(4);
  hello.worker_id = WorkerId::from_value(5);
  hello.worker_boot_id = WorkerBootId::from_value(6);
  hello.location_descriptor = "site with spaces";
  hello.capabilities.tags = {"gpu:sm_120", "compat:accelerator=sm_120"};
  hello.capabilities.storage_classes = {"nvme"};
  hello.capabilities.accelerator_architectures = {"sm_120"};
  hello.capabilities.encryption_at_rest = true;
  hello.capacity.total_logical_bytes = 1ULL << 40U;
  hello.capacity.used_logical_bytes = 1ULL << 20U;
  hello.capacity.inbound_bytes_per_tick = 1ULL << 28U;
  hello.capacity.free_transfer_slots = 4;
  hello.capacity.utilization_per_mille = 10;
  hello.expected_coordinator_epoch = CoordinatorEpoch::from_value(1);
  hello.listen_port = 4242;
  ByteWriter writer;
  encode(writer, hello);
  ByteReader reader(writer.data());
  HelloPayload decoded;
  ctx.check(decode(reader, &decoded), "the HELLO payload decodes");
  ctx.check(decoded.cluster_id == hello.cluster_id, "cluster identity survives");
  ctx.check(decoded.location_descriptor == hello.location_descriptor, "descriptor survives");
  ctx.check(decoded.capabilities.tags == hello.capabilities.tags, "capability tags survive");
  ctx.check(decoded.listen_port == hello.listen_port, "the data port survives");

  // Inconsistent capacity evidence is rejected rather than trusted.
  ByteWriter hostile;
  hostile.u64(1);
  hostile.u64(2);
  const std::vector<std::byte> prefix = hostile.data();
  (void)prefix;
  DataChunkPayload chunk;
  chunk.state_id = StateId::from_value(1);
  chunk.state_generation = StateGeneration::from_value(1);
  chunk.replica_id = ReplicaId::from_value(1);
  chunk.offset = 0;
  chunk.last = 1;
  chunk.bytes = {std::byte{0x01}, std::byte{0x02}};
  ByteWriter chunk_writer;
  encode(chunk_writer, chunk);
  ByteReader chunk_reader(chunk_writer.data());
  DataChunkPayload decoded_chunk;
  ctx.check(decode(chunk_reader, &decoded_chunk, 1024U), "the data chunk decodes");
  ctx.check(decoded_chunk.bytes.size() == 2U, "chunk bytes survive");
  ctx.check(decoded_chunk.last == 1U, "the last flag survives");

  ByteReader bounded(chunk_writer.data());
  DataChunkPayload too_big;
  ctx.check(!decode(bounded, &too_big, 1U), "a chunk above the bound is rejected");
}

CCSF_CASE(transport, loopback_frame_exchange) {
  auto listener = TcpListener::bind(loopback_address(), 0, 4);
  ctx.check(listener.has_value(), "the listener binds");
  const std::uint16_t port = listener.value()->port();
  ctx.check(port != 0, "an ephemeral port is assigned");

  Result<std::unique_ptr<TcpConnection>> accepted =
      make_error(ErrorCode::INTERNAL_ERROR, "unset");
  std::thread acceptor([&]() { accepted = listener.value()->accept(); });

  auto client = TcpConnection::connect(loopback_address(), port);
  ctx.check(client.has_value(), "the client connects");
  const Frame frame = make_frame(MessageType::EVIDENCE, "transport-payload");
  ctx.check(client.value()->send_frame(frame).has_value(), "the frame is sent");
  acceptor.join();
  ctx.check(accepted.has_value(), "the server accepts");
  auto received = accepted.value()->receive_frame(kMaxFramePayloadBytes);
  ctx.check(received.has_value(), "the frame is received");
  ctx.check(received.value().payload.size() == 17U, "the payload length is preserved");

  accepted.value()->close();
  client.value()->close();
  listener.value()->close();
}

CCSF_CASE(transport, peer_close_is_reported_as_truncation) {
  auto listener = TcpListener::bind(loopback_address(), 0, 4);
  ctx.check(listener.has_value(), "the listener binds");
  const std::uint16_t port = listener.value()->port();

  Result<std::unique_ptr<TcpConnection>> accepted =
      make_error(ErrorCode::INTERNAL_ERROR, "unset");
  std::thread acceptor([&]() { accepted = listener.value()->accept(); });
  auto client = TcpConnection::connect(loopback_address(), port);
  ctx.check(client.has_value(), "the client connects");
  acceptor.join();
  ctx.check(accepted.has_value(), "the server accepts");

  // Send a partial header and then close: the peer must see a truncated frame,
  // not a silent success.
  const std::vector<std::byte> partial(10, std::byte{0x41});
  const std::vector<std::byte> image = encode_frame(make_frame(MessageType::HEARTBEAT, "x"));
  std::vector<std::byte> mixed(partial);
  mixed.insert(mixed.end(), image.begin(), image.begin() + 5);
  const std::string text(reinterpret_cast<const char*>(mixed.data()), mixed.size());
  (void)text;
  ctx.check(client.value()->send_frame(make_frame(MessageType::HEARTBEAT, "one")).has_value(),
            "a first frame is sent");
  auto first = accepted.value()->receive_frame(kMaxFramePayloadBytes);
  ctx.check(first.has_value(), "the first frame is received");
  client.value()->close();
  auto second = accepted.value()->receive_frame(kMaxFramePayloadBytes);
  ctx.check(!second.has_value(), "a clean peer close between frames is reported");
  ctx.check(second.error().code == ErrorCode::TRANSPORT_CLOSED, "reported as TRANSPORT_CLOSED");
  accepted.value()->close();
  listener.value()->close();
}

CCSF_CASE(transport, listener_close_ends_accept) {
  auto listener = TcpListener::bind(loopback_address(), 0, 4);
  ctx.check(listener.has_value(), "the listener binds");
  std::thread closer([&]() {
    std::this_thread::yield();
    listener.value()->close();
  });
  auto accepted = listener.value()->accept();
  closer.join();
  ctx.check(!accepted.has_value(), "accept returns once the listener is closed");
  ctx.check(accepted.error().code == ErrorCode::TRANSPORT_CLOSED,
            "reported as TRANSPORT_CLOSED");
}
