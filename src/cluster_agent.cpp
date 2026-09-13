// Cross-Cluster State Fabric - cluster agent implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/cluster_agent.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "ccsf/digest.hpp"

namespace ccsf {
namespace {

constexpr std::size_t kDataChunkBytes = 256U * 1024U;

std::uint64_t current_process_identity() {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

void set_error(std::mutex& mutex, std::string& slot, const std::string& text) {
  std::lock_guard<std::mutex> guard(mutex);
  slot = text;
}

}  // namespace

void ReplicaStore::put(ReplicaId replica, StateId state_id, StateGeneration generation,
                       std::vector<std::byte> bytes, Digest digest) {
  std::lock_guard<std::mutex> guard(mutex_);
  Entry entry;
  entry.state_id = state_id;
  entry.generation = generation;
  entry.digest = digest;
  entry.bytes = std::move(bytes);
  entries_[replica] = std::move(entry);
}

bool ReplicaStore::contains(ReplicaId replica) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return entries_.find(replica) != entries_.end();
}

bool ReplicaStore::read(ReplicaId replica, std::uint64_t offset, std::uint64_t length,
                        std::vector<std::byte>* out) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = entries_.find(replica);
  if (it == entries_.end()) {
    return false;
  }
  const std::uint64_t size = static_cast<std::uint64_t>(it->second.bytes.size());
  if (offset > size) {
    return false;
  }
  const std::uint64_t available = size - offset;
  const std::uint64_t take = length == 0U ? available : std::min(length, available);
  out->assign(it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
              it->second.bytes.begin() + static_cast<std::ptrdiff_t>(offset + take));
  return true;
}

std::uint64_t ReplicaStore::size_of(ReplicaId replica) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = entries_.find(replica);
  return it == entries_.end() ? 0U : static_cast<std::uint64_t>(it->second.bytes.size());
}

Digest ReplicaStore::digest_of(ReplicaId replica) const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto it = entries_.find(replica);
  return it == entries_.end() ? Digest{} : it->second.digest;
}

std::size_t ReplicaStore::count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return entries_.size();
}

void ReplicaStore::erase(ReplicaId replica) {
  std::lock_guard<std::mutex> guard(mutex_);
  entries_.erase(replica);
}

std::vector<ReplicaId> ReplicaStore::replica_ids() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ReplicaId> ids;
  ids.reserve(entries_.size());
  for (const auto& entry : entries_) {
    ids.push_back(entry.first);
  }
  return ids;
}

ClusterAgent::~ClusterAgent() { request_stop(); }

Result<std::unique_ptr<ClusterAgent>> ClusterAgent::start(const ClusterAgentConfig& config) {
  std::unique_ptr<ClusterAgent> agent(new ClusterAgent());
  agent->config_ = config;
  agent->sockets_ = std::make_unique<SocketRuntime>();
  agent->worker_id_ = WorkerId::from_value(config.cluster_id.value());
  agent->worker_boot_id_ = WorkerBootId::from_value(current_process_identity());
  auto listener = TcpListener::bind(config.data_bind_address, 0, 4);
  if (!listener) {
    return listener.error();
  }
  agent->listener_ = std::move(listener.value());
  auto registered = agent->connect_and_register();
  if (!registered) {
    return registered.error();
  }
  return agent;
}

Result<void> ClusterAgent::connect_and_register() {
  auto connection = TcpConnection::connect(config_.coordinator_host, config_.coordinator_port);
  if (!connection) {
    return connection.error();
  }
  control_ = std::move(connection.value());

  HelloPayload hello;
  hello.cluster_id = config_.cluster_id;
  hello.region = config_.region;
  hello.site = config_.site;
  hello.failure_domain = config_.failure_domain;
  hello.worker_id = worker_id_;
  hello.worker_boot_id = worker_boot_id_;
  hello.location_descriptor = config_.location_descriptor;
  hello.capabilities = config_.capabilities;
  hello.capabilities.canonicalize();
  hello.capacity = config_.capacity;
  hello.expected_coordinator_epoch = config_.expected_coordinator_epoch;
  hello.listen_port = listener_->port();

  ByteWriter writer;
  encode(writer, hello);
  Frame frame;
  frame.header.message_type = MessageType::HELLO;
  frame.header.cluster_id = config_.cluster_id;
  frame.header.worker_id = worker_id_;
  frame.header.worker_boot_id = worker_boot_id_;
  frame.payload = writer.data();
  Status sent = control_->send_frame(frame);
  if (!sent) {
    return sent.error();
  }

  auto reply = control_->receive_frame(config_.maximum_payload_bytes);
  if (!reply) {
    return reply.error();
  }
  if (reply.value().header.message_type != MessageType::HELLO_ACK) {
    return make_error(ErrorCode::PROTOCOL_UNSUPPORTED, "the coordinator did not acknowledge HELLO");
  }
  ByteReader reader(reply.value().payload_span());
  HelloAckPayload ack;
  if (!decode(reader, &ack)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed HELLO_ACK");
  }
  if (ack.accepted == 0U) {
    return Error(ErrorCode::AUTHORITY_NOT_ESTABLISHED,
                 "the coordinator refused the registration", ack.detail);
  }
  incarnation_ = ack.incarnation;
  coordinator_epoch_ = ack.coordinator_epoch;
  registered_ = true;
  if (config_.trace) {
    config_.trace("registered cluster " + config_.cluster_id.str() + " incarnation " +
                  incarnation_.str() + " worker boot " + worker_boot_id_.str() + " data port " +
                  std::to_string(listener_->port()));
  }

  EvidencePayload evidence;
  evidence.cluster_id = config_.cluster_id;
  evidence.cluster_incarnation = incarnation_;
  evidence.worker_boot_id = worker_boot_id_;
  evidence.health = ClusterHealthState::HEALTHY;
  evidence.capabilities = config_.capabilities;
  evidence.capabilities.canonicalize();
  evidence.capacity = config_.capacity;
  evidence.expected_coordinator_epoch = coordinator_epoch_;
  ByteWriter evidence_writer;
  encode(evidence_writer, evidence);
  Frame evidence_frame;
  evidence_frame.header.message_type = MessageType::EVIDENCE;
  evidence_frame.header.cluster_id = config_.cluster_id;
  evidence_frame.header.cluster_incarnation = incarnation_;
  evidence_frame.header.worker_id = worker_id_;
  evidence_frame.header.worker_boot_id = worker_boot_id_;
  evidence_frame.payload = evidence_writer.data();
  Status evidence_sent = control_->send_frame(evidence_frame);
  if (!evidence_sent) {
    return evidence_sent.error();
  }
  auto evidence_reply = control_->receive_frame(config_.maximum_payload_bytes);
  if (!evidence_reply) {
    return evidence_reply.error();
  }
  if (evidence_reply.value().header.message_type != MessageType::EVIDENCE_ACK) {
    return make_error(ErrorCode::PROTOCOL_UNSUPPORTED, "the coordinator did not acknowledge EVIDENCE");
  }
  return Status{};
}

void ClusterAgent::data_loop() {
  for (;;) {
    auto accepted = listener_->accept();
    if (!accepted) {
      return;
    }
    std::size_t active = 0;
    {
      std::lock_guard<std::mutex> guard(thread_mutex_);
      active = active_data_connections_;
      if (active >= config_.maximum_data_connections) {
        accepted.value()->close();
        continue;
      }
      ++active_data_connections_;
    }
    std::unique_ptr<TcpConnection> connection = std::move(accepted.value());
    std::thread worker([this, connection = std::move(connection)]() mutable {
      handle_data_connection(std::move(connection));
      std::lock_guard<std::mutex> guard(thread_mutex_);
      if (active_data_connections_ > 0) {
        --active_data_connections_;
      }
      ++data_connections_;
    });
    worker.detach();
  }
}

void ClusterAgent::handle_data_connection(std::unique_ptr<TcpConnection> connection) {
  for (;;) {
    auto frame = connection->receive_frame(config_.maximum_payload_bytes);
    if (!frame) {
      return;
    }
    if (frame.value().header.message_type == MessageType::DATA_REQUEST) {
      ByteReader reader(frame.value().payload_span());
      DataRequestPayload request;
      if (!decode(reader, &request)) {
        return;
      }
      const Digest digest = store_.digest_of(request.replica_id);
      const std::uint64_t total = store_.size_of(request.replica_id);
      if (total == 0U) {
        DataChunkPayload chunk;
        chunk.state_id = request.state_id;
        chunk.state_generation = request.state_generation;
        chunk.replica_id = request.replica_id;
        chunk.offset = 0;
        chunk.last = 1;
        ByteWriter writer;
        encode(writer, chunk);
        Frame reply;
        reply.header.message_type = MessageType::DATA_CHUNK;
        reply.payload = writer.data();
        (void)connection->send_frame(reply);
        return;
      }
      std::uint64_t offset = request.offset;
      while (offset < total) {
        std::vector<std::byte> slice;
        if (!store_.read(request.replica_id, offset, kDataChunkBytes, &slice)) {
          return;
        }
        DataChunkPayload chunk;
        chunk.state_id = request.state_id;
        chunk.state_generation = request.state_generation;
        chunk.replica_id = request.replica_id;
        chunk.offset = offset;
        chunk.digest = digest;
        chunk.bytes = std::move(slice);
        offset += static_cast<std::uint64_t>(chunk.bytes.size());
        chunk.last = offset >= total ? 1U : 0U;
        ByteWriter writer;
        encode(writer, chunk);
        Frame reply;
        reply.header.message_type = MessageType::DATA_CHUNK;
        reply.payload = writer.data();
        Status sent = connection->send_frame(reply);
        if (!sent) {
          return;
        }
      }
      continue;
    }
    if (frame.value().header.message_type == MessageType::HOST_STATE) {
      ByteReader reader(frame.value().payload_span());
      HostStatePayload host;
      if (!decode(reader, &host, config_.maximum_payload_bytes)) {
        return;
      }
      const Digest measured = sha256(ByteSpan(host.bytes.data(), host.bytes.size()));
      SimplePayload reply_payload;
      if (measured != host.expected_digest && host.expected_digest.valid()) {
        reply_payload.ok = 0;
        reply_payload.code = ErrorCode::INTEGRITY_MISMATCH;
        reply_payload.detail = "hosted bytes do not match the declared digest";
      } else {
        store_.put(host.replica_id, host.state_id, host.state_generation, std::move(host.bytes),
                   measured);
        reply_payload.ok = 1;
        reply_payload.code = ErrorCode::OK;
        reply_payload.detail = measured.hex();
      }
      ByteWriter writer;
      encode(writer, reply_payload);
      Frame reply;
      reply.header.message_type = MessageType::HOST_STATE_ACK;
      reply.payload = writer.data();
      (void)connection->send_frame(reply);
      continue;
    }
    return;
  }
}

Result<std::vector<std::byte>> ClusterAgent::pull_from_peer(const TransferIntentPayload& intent,
                                                            Digest* digest) {
  auto connection = TcpConnection::connect(intent.source_host,
                                           static_cast<std::uint16_t>(intent.source_data_port));
  if (!connection) {
    return connection.error();
  }
  DataRequestPayload request;
  request.state_id = intent.state_id;
  request.state_generation = intent.state_generation;
  request.replica_id = intent.source_replica;
  request.offset = 0;
  request.length = 0;
  ByteWriter writer;
  encode(writer, request);
  Frame frame;
  frame.header.message_type = MessageType::DATA_REQUEST;
  frame.header.coordinator_epoch = coordinator_epoch_;
  frame.header.cluster_id = config_.cluster_id;
  frame.header.cluster_incarnation = incarnation_;
  frame.header.worker_id = worker_id_;
  frame.header.worker_boot_id = worker_boot_id_;
  frame.payload = writer.data();
  Status sent = connection.value()->send_frame(frame);
  if (!sent) {
    return sent.error();
  }

  if (config_.trace) {
    config_.trace("pulling replica " + intent.source_replica.str() + " from " +
                  intent.source_host + ":" + std::to_string(intent.source_data_port));
  }
  std::vector<std::byte> bytes;
  Sha256 hasher;
  bool reported_progress = false;
  for (;;) {
    auto chunk_frame = connection.value()->receive_frame(config_.maximum_payload_bytes);
    if (!chunk_frame) {
      return chunk_frame.error();
    }
    if (chunk_frame.value().header.message_type != MessageType::DATA_CHUNK) {
      return make_error(ErrorCode::PROTOCOL_UNSUPPORTED, "expected a data chunk");
    }
    ByteReader reader(chunk_frame.value().payload_span());
    DataChunkPayload chunk;
    if (!decode(reader, &chunk, config_.maximum_payload_bytes)) {
      return make_error(ErrorCode::FRAME_CORRUPT, "malformed data chunk");
    }
    if (chunk.replica_id != intent.source_replica) {
      return make_error(ErrorCode::FRAME_CORRUPT, "data chunk names a different replica");
    }
    if (chunk.bytes.empty() && chunk.last == 1U) {
      return make_error(ErrorCode::TRANSFER_FAILED,
                        "the source does not host the requested replica bytes");
    }
    hasher.update(ByteSpan(chunk.bytes.data(), chunk.bytes.size()));
    bytes.insert(bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
    if (!reported_progress) {
      reported_progress = true;
      TransferReportPayload progress;
      progress.transfer_id = intent.transfer_id;
      progress.transfer_generation = intent.transfer_generation;
      progress.operation_id = intent.operation_id;
      progress.operation_generation = intent.operation_generation;
      progress.replica_id = intent.destination_replica;
      progress.replica_generation = intent.destination_replica_generation;
      progress.cluster_incarnation = incarnation_;
      progress.worker_boot_id = worker_boot_id_;
      progress.bytes_transferred = static_cast<std::uint64_t>(bytes.size());
      progress.detail = "receiving";
      ByteWriter progress_writer;
      encode(progress_writer, progress);
      Frame progress_frame;
      progress_frame.header.message_type = MessageType::TRANSFER_PROGRESS;
      progress_frame.header.coordinator_epoch = coordinator_epoch_;
      progress_frame.header.cluster_id = config_.cluster_id;
      progress_frame.header.cluster_incarnation = incarnation_;
      progress_frame.header.worker_id = worker_id_;
      progress_frame.header.worker_boot_id = worker_boot_id_;
      progress_frame.payload = progress_writer.data();
      std::lock_guard<std::mutex> guard(send_mutex_);
      Status progress_sent = control_->send_frame(progress_frame);
      if (!progress_sent) {
        return progress_sent.error();
      }
    }
    if (chunk.last == 1U) {
      break;
    }
    if (bytes.size() > config_.maximum_payload_bytes) {
      return make_error(ErrorCode::FRAME_TOO_LARGE, "the transfer exceeded the payload bound");
    }
  }
  if (intent.payload_bytes != 0U && bytes.size() != intent.payload_bytes) {
    return make_error(ErrorCode::TRANSFER_INCOMPLETE,
                      "the received byte count does not match the authorized length");
  }
  *digest = hasher.finalize();
  if (config_.trace) {
    config_.trace("pulled " + std::to_string(bytes.size()) + " bytes");
  }
  return bytes;
}

Result<Frame> ClusterAgent::await_acknowledgement(MessageType expected) {
  constexpr int kMaxFrames = 8;
  for (int attempt = 0; attempt < kMaxFrames; ++attempt) {
    auto frame = control_->receive_frame(config_.maximum_payload_bytes);
    if (!frame) {
      return frame.error();
    }
    if (frame.value().header.message_type == expected) {
      return frame;
    }
    if (frame.value().header.message_type == MessageType::SHUTDOWN) {
      return make_error(ErrorCode::SHUTTING_DOWN, "the coordinator is shutting down");
    }
  }
  return make_error(ErrorCode::PROTOCOL_UNSUPPORTED,
                    "the expected acknowledgement did not arrive");
}

Result<void> ClusterAgent::handle_transfer_intent(const Frame& frame) {
  ByteReader reader(frame.payload_span());
  TransferIntentPayload intent;
  if (!decode(reader, &intent)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed transfer intent");
  }
  Digest measured;
  auto bytes = pull_from_peer(intent, &measured);
  if (!bytes) {
    TransferReportPayload report;
    report.transfer_id = intent.transfer_id;
    report.transfer_generation = intent.transfer_generation;
    report.operation_id = intent.operation_id;
    report.operation_generation = intent.operation_generation;
    report.replica_id = intent.destination_replica;
    report.replica_generation = intent.destination_replica_generation;
    report.cluster_incarnation = incarnation_;
    report.worker_boot_id = worker_boot_id_;
    report.failed = 1;
    report.detail = bytes.error().message;
    ByteWriter writer;
    encode(writer, report);
    Frame report_frame;
    report_frame.header.message_type = MessageType::TRANSFER_RESULT;
    report_frame.header.coordinator_epoch = coordinator_epoch_;
    report_frame.header.cluster_id = config_.cluster_id;
    report_frame.header.cluster_incarnation = incarnation_;
    report_frame.header.worker_id = worker_id_;
    report_frame.header.worker_boot_id = worker_boot_id_;
    report_frame.payload = writer.data();
    std::lock_guard<std::mutex> guard(send_mutex_);
    (void)control_->send_frame(report_frame);
    set_error(error_mutex_, last_error_, bytes.error().to_string());
    return bytes.error();
  }

  const std::size_t received = bytes.value().size();
  store_.put(intent.destination_replica, intent.state_id, intent.state_generation,
             std::move(bytes.value()), measured);

  TransferReportPayload result;
  result.transfer_id = intent.transfer_id;
  result.transfer_generation = intent.transfer_generation;
  result.operation_id = intent.operation_id;
  result.operation_generation = intent.operation_generation;
  result.replica_id = intent.destination_replica;
  result.replica_generation = intent.destination_replica_generation;
  result.cluster_incarnation = incarnation_;
  result.worker_boot_id = worker_boot_id_;
  result.bytes_transferred = static_cast<std::uint64_t>(received);
  result.measured_digest = measured;
  result.completed = 1;
  result.detail = "transfer complete";

  VerifyReportPayload verification;
  verification.verification_id = VerificationId::from_value(intent.operation_id.value());
  verification.replica_id = intent.destination_replica;
  verification.replica_generation = intent.destination_replica_generation;
  verification.operation_id = intent.operation_id;
  verification.operation_generation = intent.operation_generation;
  verification.cluster_incarnation = incarnation_;
  verification.worker_boot_id = worker_boot_id_;
  verification.measured_digest = measured;
  verification.verified_bytes = static_cast<std::uint64_t>(received);
  verification.durability = static_cast<std::uint32_t>(DurabilityClass::DURABLE);
  const bool digest_matches = !intent.expected_digest.valid() || measured == intent.expected_digest;
  verification.integrity_verified = digest_matches ? 1U : 0U;
  verification.compatibility_verified = 1U;
  verification.detail = digest_matches ? "digest matches the authorized digest"
                                       : "digest does not match the authorized digest";

  {
    std::lock_guard<std::mutex> guard(send_mutex_);
    ByteWriter result_writer;
    encode(result_writer, result);
    Frame result_frame;
    result_frame.header.message_type = MessageType::TRANSFER_RESULT;
    result_frame.header.coordinator_epoch = coordinator_epoch_;
    result_frame.header.cluster_id = config_.cluster_id;
    result_frame.header.cluster_incarnation = incarnation_;
    result_frame.header.worker_id = worker_id_;
    result_frame.header.worker_boot_id = worker_boot_id_;
    result_frame.payload = result_writer.data();
    Status sent = control_->send_frame(result_frame);
    if (!sent) {
      return sent.error();
    }
    auto acknowledged = await_acknowledgement(MessageType::TRANSFER_RESULT_ACK);
    if (!acknowledged) {
      return acknowledged.error();
    }
    ByteWriter verify_writer;
    encode(verify_writer, verification);
    Frame verify_frame;
    verify_frame.header.message_type = MessageType::VERIFY_RESULT;
    verify_frame.header.coordinator_epoch = coordinator_epoch_;
    verify_frame.header.cluster_id = config_.cluster_id;
    verify_frame.header.cluster_incarnation = incarnation_;
    verify_frame.header.worker_id = worker_id_;
    verify_frame.header.worker_boot_id = worker_boot_id_;
    verify_frame.payload = verify_writer.data();
    Status verify_sent = control_->send_frame(verify_frame);
    if (!verify_sent) {
      return verify_sent.error();
    }
    auto verify_ack = await_acknowledgement(MessageType::VERIFY_RESULT_ACK);
    if (!verify_ack) {
      return verify_ack.error();
    }
  }
  return Status{};
}

Result<void> ClusterAgent::serve() {
  std::thread data_worker([this]() { data_loop(); });
  data_thread_ = 1;
  Result<void> outcome = Status{};
  for (;;) {
    auto frame = control_->receive_frame(config_.maximum_payload_bytes);
    if (!frame) {
      if (!stopping_) {
        set_error(error_mutex_, last_error_, frame.error().to_string());
        outcome = frame.error();
      }
      break;
    }
    const MessageType type = frame.value().header.message_type;
    if (config_.trace) {
      config_.trace(std::string("received ") + to_string(type) + " sequence " +
                    std::to_string(frame.value().header.sequence));
    }
    if (type == MessageType::SHUTDOWN) {
      Frame ack;
      ack.header.message_type = MessageType::SHUTDOWN_ACK;
      ack.header.cluster_id = config_.cluster_id;
      ack.header.cluster_incarnation = incarnation_;
      std::lock_guard<std::mutex> guard(send_mutex_);
      (void)control_->send_frame(ack);
      break;
    }
    if (type == MessageType::HEARTBEAT) {
      Frame ack;
      ack.header.message_type = MessageType::HEARTBEAT_ACK;
      ack.header.cluster_id = config_.cluster_id;
      ack.header.cluster_incarnation = incarnation_;
      ack.header.worker_boot_id = worker_boot_id_;
      std::lock_guard<std::mutex> guard(send_mutex_);
      Status sent = control_->send_frame(ack);
      if (!sent) {
        outcome = sent.error();
        break;
      }
      continue;
    }
    if (type == MessageType::TRANSFER_INTENT) {
      Frame ack;
      ack.header.message_type = MessageType::TRANSFER_INTENT_ACK;
      ack.header.cluster_id = config_.cluster_id;
      ack.header.cluster_incarnation = incarnation_;
      {
        std::lock_guard<std::mutex> guard(send_mutex_);
        Status sent = control_->send_frame(ack);
        if (!sent) {
          outcome = sent.error();
          break;
        }
      }
      if (config_.trace) {
        config_.trace("transfer intent acknowledged; starting the pull");
      }
      Result<void> handled = handle_transfer_intent(frame.value());
      if (config_.trace) {
        config_.trace(handled ? "transfer intent handled"
                              : ("transfer intent failed: " + handled.error().to_string()));
      }
      if (!handled) {
        set_error(error_mutex_, last_error_, handled.error().to_string());
      }
      continue;
    }
    if (type == MessageType::HOST_STATE) {
      ByteReader reader(frame.value().payload_span());
      HostStatePayload host;
      if (decode(reader, &host, config_.maximum_payload_bytes)) {
        const Digest measured = sha256(ByteSpan(host.bytes.data(), host.bytes.size()));
        store_.put(host.replica_id, host.state_id, host.state_generation, std::move(host.bytes),
                   measured);
      }
      continue;
    }
  }
  stopping_ = true;
  if (listener_ != nullptr) {
    listener_->close();
  }
  data_worker.join();
  return outcome;
}

void ClusterAgent::request_stop() {
  stopping_ = true;
  if (control_ != nullptr) {
    control_->close();
  }
  if (listener_ != nullptr) {
    listener_->close();
  }
}

ClusterIncarnationId ClusterAgent::incarnation() const { return incarnation_; }
WorkerBootId ClusterAgent::worker_boot_id() const { return worker_boot_id_; }
WorkerId ClusterAgent::worker_id() const { return worker_id_; }
std::uint16_t ClusterAgent::data_port() const {
  return listener_ == nullptr ? 0 : listener_->port();
}
bool ClusterAgent::registered() const { return registered_; }

std::string ClusterAgent::last_error() const {
  std::lock_guard<std::mutex> guard(error_mutex_);
  return last_error_;
}

std::uint64_t ClusterAgent::data_connections() const {
  std::lock_guard<std::mutex> guard(thread_mutex_);
  return data_connections_;
}

}  // namespace ccsf
