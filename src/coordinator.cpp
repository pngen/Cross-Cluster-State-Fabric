// Cross-Cluster State Fabric - coordinator process implementation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "ccsf/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "detail/encoding.hpp"

namespace ccsf {

using detail::decode_id;
using detail::encode_id;

namespace {

constexpr std::size_t kMaxFindings = 256;
constexpr std::size_t kMaxReasonBytes = 4096;

void encode_string_list(ByteWriter& writer, const std::vector<std::string>& values) {
  writer.u32(static_cast<std::uint32_t>(values.size()));
  for (const std::string& value : values) {
    writer.str(value);
  }
}

bool decode_string_list(ByteReader& reader, std::vector<std::string>* out, std::size_t max_count) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    return false;
  }
  if (count > max_count) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "declared list exceeds the bound");
    return false;
  }
  out->clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    std::string value;
    if (!reader.str(kMaxReasonBytes, &value)) {
      return false;
    }
    out->push_back(std::move(value));
  }
  return true;
}

std::string host_of(const std::string& peer) {
  const std::size_t separator = peer.find_last_of(':');
  return separator == std::string::npos ? peer : peer.substr(0, separator);
}

void encode_status(ByteWriter& writer, const FabricStatus& status) {
  encode_id(writer, status.coordinator_id);
  encode_id(writer, status.coordinator_epoch);
  writer.u64(status.logical_time);
  writer.u64(status.durable_sequence);
  writer.u64(status.journal_records);
  writer.u64(status.replay_generation);
  writer.boolean(status.persistence_enabled);
  writer.boolean(status.persistence_healthy);
  writer.boolean(status.recovered_from_durable_state);
  writer.boolean(status.journal_tail_discarded);
  writer.str(status.persistence_detail);
  writer.u64(status.state_generations);
  writer.u64(status.clusters);
  writer.u64(status.replicas);
  writer.u64(status.replica_sets);
  writer.u64(status.operations);
  detail::encode(writer, status.counters);
}

bool decode_status(ByteReader& reader, FabricStatus* status) {
  return decode_id(reader, &status->coordinator_id) &&
         decode_id(reader, &status->coordinator_epoch) && reader.u64(&status->logical_time) &&
         reader.u64(&status->durable_sequence) && reader.u64(&status->journal_records) &&
         reader.u64(&status->replay_generation) &&
         reader.boolean(&status->persistence_enabled) &&
         reader.boolean(&status->persistence_healthy) &&
         reader.boolean(&status->recovered_from_durable_state) &&
         reader.boolean(&status->journal_tail_discarded) &&
         reader.str(kMaxReasonBytes, &status->persistence_detail) &&
         reader.u64(&status->state_generations) && reader.u64(&status->clusters) &&
         reader.u64(&status->replicas) && reader.u64(&status->replica_sets) &&
         reader.u64(&status->operations) && detail::decode(reader, &status->counters) &&
         reader.at_end();
}

void encode_reuse(ByteWriter& writer, const ReuseEvaluation& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  encode_id(writer, value.cluster_id);
  writer.u16(static_cast<std::uint16_t>(value.decision));
  encode_id(writer, value.replica);
  encode_id(writer, value.replica_generation);
  writer.u16(static_cast<std::uint16_t>(value.code));
  writer.str(value.reason);
  writer.boolean(value.assessment.authoritative_for_reuse);
  writer.boolean(value.assessment.integrity_verified);
  writer.boolean(value.assessment.compatibility_verified);
  writer.boolean(value.assessment.current_for_state_generation);
  writer.boolean(value.assessment.member_of_current_replica_set_generation);
  writer.boolean(value.assessment.physically_present);
  encode_string_list(writer, value.findings);
}

bool decode_reuse(ByteReader& reader, ReuseEvaluation* value) {
  std::uint16_t decision = 0;
  std::uint16_t code = 0;
  if (!decode_id(reader, &value->state_id)) return false;
  if (!decode_id(reader, &value->state_generation)) return false;
  if (!decode_id(reader, &value->cluster_id)) return false;
  if (!reader.u16(&decision)) return false;
  if (!decode_id(reader, &value->replica)) return false;
  if (!decode_id(reader, &value->replica_generation)) return false;
  if (!reader.u16(&code)) return false;
  value->decision = static_cast<DecisionKind>(decision);
  value->code = static_cast<ErrorCode>(code);
  if (!is_valid(value->decision)) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "invalid decision kind");
    return false;
  }
  if (!reader.str(kMaxReasonBytes, &value->reason)) return false;
  if (!reader.boolean(&value->assessment.authoritative_for_reuse)) return false;
  if (!reader.boolean(&value->assessment.integrity_verified)) return false;
  if (!reader.boolean(&value->assessment.compatibility_verified)) return false;
  if (!reader.boolean(&value->assessment.current_for_state_generation)) return false;
  if (!reader.boolean(&value->assessment.member_of_current_replica_set_generation)) return false;
  if (!reader.boolean(&value->assessment.physically_present)) return false;
  return decode_string_list(reader, &value->findings, kMaxFindings) && reader.at_end();
}

void encode_reconciliation(ByteWriter& writer, const ReplicaSetReconciliation& value) {
  const ReplicaSetRecord& set = value.replica_set;
  encode_id(writer, set.replica_set_id);
  encode_id(writer, set.generation);
  encode_id(writer, set.state_id);
  encode_id(writer, set.state_generation);
  writer.u16(static_cast<std::uint16_t>(set.state));
  writer.u32(set.authoritative_count);
  writer.u32(set.degraded_count);
  writer.u32(set.distinct_clusters);
  writer.u32(set.distinct_regions);
  writer.u32(set.distinct_failure_domains);
  writer.u32(set.requirements.minimum_authoritative_replicas);
  writer.u32(set.requirements.desired_replicas);
  writer.u32(set.requirements.maximum_replicas);
  writer.boolean(set.revalidation_required);
  writer.boolean(value.under_replicated);
  writer.boolean(value.over_replicated);
  writer.u32(value.authoritative_shortfall);
  encode_string_list(writer, value.findings);
}

bool decode_reconciliation(ByteReader& reader, ReplicaSetReconciliation* value) {
  std::uint16_t set_state = 0;
  ReplicaSetRecord& set = value->replica_set;
  if (!decode_id(reader, &set.replica_set_id)) return false;
  if (!decode_id(reader, &set.generation)) return false;
  if (!decode_id(reader, &set.state_id)) return false;
  if (!decode_id(reader, &set.state_generation)) return false;
  if (!reader.u16(&set_state)) return false;
  if (!is_valid(static_cast<ReplicaSetState>(set_state))) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "invalid replica set state");
    return false;
  }
  set.state = static_cast<ReplicaSetState>(set_state);
  if (!reader.u32(&set.authoritative_count)) return false;
  if (!reader.u32(&set.degraded_count)) return false;
  if (!reader.u32(&set.distinct_clusters)) return false;
  if (!reader.u32(&set.distinct_regions)) return false;
  if (!reader.u32(&set.distinct_failure_domains)) return false;
  if (!reader.u32(&set.requirements.minimum_authoritative_replicas)) return false;
  if (!reader.u32(&set.requirements.desired_replicas)) return false;
  if (!reader.u32(&set.requirements.maximum_replicas)) return false;
  if (!reader.boolean(&set.revalidation_required)) return false;
  if (!reader.boolean(&value->under_replicated)) return false;
  if (!reader.boolean(&value->over_replicated)) return false;
  if (!reader.u32(&value->authoritative_shortfall)) return false;
  return decode_string_list(reader, &value->findings, kMaxFindings) && reader.at_end();
}

void encode_placement(ByteWriter& writer, const PlacementEvaluation& value) {
  encode_id(writer, value.state_id);
  encode_id(writer, value.state_generation);
  writer.u16(static_cast<std::uint16_t>(value.decision));
  encode_id(writer, value.chosen_cluster);
  writer.i64(value.chosen_score);
  encode_string_list(writer, value.reasons);
}

bool decode_placement(ByteReader& reader, PlacementEvaluation* value) {
  std::uint16_t decision = 0;
  if (!decode_id(reader, &value->state_id)) return false;
  if (!decode_id(reader, &value->state_generation)) return false;
  if (!reader.u16(&decision)) return false;
  if (!is_valid(static_cast<DecisionKind>(decision))) {
    reader.fail(ErrorCode::FRAME_CORRUPT, "invalid decision kind");
    return false;
  }
  value->decision = static_cast<DecisionKind>(decision);
  if (!decode_id(reader, &value->chosen_cluster)) return false;
  if (!reader.i64(&value->chosen_score)) return false;
  return decode_string_list(reader, &value->reasons, kMaxFindings) && reader.at_end();
}

void encode_explanation(ByteWriter& writer, const Explanation& value) {
  writer.str(value.title);
  encode_string_list(writer, value.lines);
}

bool decode_explanation(ByteReader& reader, Explanation* value) {
  return reader.str(kMaxReasonBytes, &value->title) &&
         decode_string_list(reader, &value->lines, kMaxFindings) && reader.at_end();
}

bool decode_id_list(ByteReader& reader, std::vector<ReplicaId>* out) {
  std::uint32_t count = 0;
  if (!reader.u32(&count)) {
    return false;
  }
  if (count > kMaxFindings) {
    reader.fail(ErrorCode::RESOURCE_EXHAUSTED, "declared replica list exceeds the bound");
    return false;
  }
  out->clear();
  for (std::uint32_t index = 0; index < count; ++index) {
    ReplicaId id;
    if (!detail::decode_id(reader, &id)) {
      return false;
    }
    out->push_back(id);
  }
  return true;
}

}  // namespace

const char* to_string(ControlOperation operation) noexcept {
  switch (operation) {
    case ControlOperation::INVALID: return "INVALID";
    case ControlOperation::STATUS: return "STATUS";
    case ControlOperation::REGISTER_STATE: return "REGISTER_STATE";
    case ControlOperation::COMMIT_STATE: return "COMMIT_STATE";
    case ControlOperation::QUERY_STATE: return "QUERY_STATE";
    case ControlOperation::LIST_CLUSTERS: return "LIST_CLUSTERS";
    case ControlOperation::REGISTER_REPLICA: return "REGISTER_REPLICA";
    case ControlOperation::TRANSFER_COMPLETION: return "TRANSFER_COMPLETION";
    case ControlOperation::VERIFY_REPLICA: return "VERIFY_REPLICA";
    case ControlOperation::COMMIT_REPLICA: return "COMMIT_REPLICA";
    case ControlOperation::AUTHORIZE_REPLICATION: return "AUTHORIZE_REPLICATION";
    case ControlOperation::AUTHORIZE_MIGRATION: return "AUTHORIZE_MIGRATION";
    case ControlOperation::COMMIT_OPERATION: return "COMMIT_OPERATION";
    case ControlOperation::CANCEL_OPERATION: return "CANCEL_OPERATION";
    case ControlOperation::EVALUATE_REUSE: return "EVALUATE_REUSE";
    case ControlOperation::RECONCILE_REPLICA_SET: return "RECONCILE_REPLICA_SET";
    case ControlOperation::AWAIT_OPERATION: return "AWAIT_OPERATION";
    case ControlOperation::QUERY_OPERATION: return "QUERY_OPERATION";
    case ControlOperation::LIST_REPLICAS: return "LIST_REPLICAS";
    case ControlOperation::MARK_CLUSTER_UNREACHABLE: return "MARK_CLUSTER_UNREACHABLE";
    case ControlOperation::REVALIDATE_REPLICA: return "REVALIDATE_REPLICA";
    case ControlOperation::QUERY_REPLICA: return "QUERY_REPLICA";
    case ControlOperation::PLAN_REPLICATION: return "PLAN_REPLICATION";
    case ControlOperation::EXPLAIN_REPLICA_SET: return "EXPLAIN_REPLICA_SET";
    case ControlOperation::RETIRE_CLUSTER: return "RETIRE_CLUSTER";
    case ControlOperation::PUBLISH_COMPATIBILITY: return "PUBLISH_COMPATIBILITY";
    case ControlOperation::QUERY_AUTHORIZATION: return "QUERY_AUTHORIZATION";
    case ControlOperation::LIST_OPERATIONS: return "LIST_OPERATIONS";
    case ControlOperation::PUBLISH_REPLICATION_POLICY: return "PUBLISH_REPLICATION_POLICY";
    case ControlOperation::PUBLISH_PLACEMENT_POLICY: return "PUBLISH_PLACEMENT_POLICY";
    case ControlOperation::TRANSFER_PROGRESS: return "TRANSFER_PROGRESS";
    case ControlOperation::REINCARNATE_CLUSTER: return "REINCARNATE_CLUSTER";
    case ControlOperation::SHUTDOWN: return "SHUTDOWN";
  }
  return "UNRECOGNIZED_CONTROL_OPERATION";
}

bool is_valid(ControlOperation operation) noexcept {
  return static_cast<std::uint16_t>(operation) <=
         static_cast<std::uint16_t>(ControlOperation::SHUTDOWN);
}

struct Coordinator::Session {
  std::unique_ptr<TcpConnection> connection;
  std::mutex send_mutex;
  ClusterId cluster_id;
  ClusterIncarnationId incarnation;
  WorkerBootId worker_boot_id;
  std::string host;
  std::uint16_t data_port{0};
  bool is_agent{false};
};

struct Coordinator::Shared {
  /// Guards the operation-wait condition variable only.
  std::mutex mutex;
  std::condition_variable condition;
  std::mutex agents_mutex;
  std::map<ClusterId, std::shared_ptr<Session>> agents;
  std::mutex sessions_mutex;
  std::vector<std::shared_ptr<Session>> sessions;
  std::atomic<bool> stopping{false};
  std::atomic<bool> shutdown_requested{false};
  std::mutex workers_mutex;
  std::vector<std::thread> workers;
};

Coordinator::~Coordinator() { request_stop(); }

Result<std::unique_ptr<Coordinator>> Coordinator::start(const CoordinatorConfig& config) {
  std::unique_ptr<Coordinator> coordinator(new Coordinator());
  coordinator->config_ = config;
  coordinator->sockets_ = std::make_unique<SocketRuntime>();
  coordinator->shared_ = std::make_shared<Shared>();

  FabricConfig fabric_config;
  fabric_config.coordinator_id = config.coordinator_id;
  fabric_config.home_cluster = config.home_cluster;
  fabric_config.persistence_directory = config.persistence_directory;
  fabric_config.require_durable_commits = config.require_durable_commits;
  fabric_config.maximum_payload_bytes = config.maximum_payload_bytes;
  fabric_config.default_placement_policy = config.placement_policy;
  fabric_config.default_replication_policy = config.replication_policy;
  auto fabric = Fabric::open(fabric_config);
  if (!fabric) {
    return fabric.error();
  }
  coordinator->fabric_ = std::move(fabric.value());

  auto listener = TcpListener::bind(config.bind_address, config.port, 16);
  if (!listener) {
    return listener.error();
  }
  coordinator->listener_ = std::move(listener.value());
  if (config.trace) {
    config.trace("coordinator " + config.coordinator_id.str() + " listening on port " +
                 std::to_string(coordinator->listener_->port()) + " at epoch " +
                 coordinator->fabric_->coordinator_epoch().str());
  }
  return coordinator;
}

std::uint16_t Coordinator::port() const { return listener_ == nullptr ? 0 : listener_->port(); }
CoordinatorEpoch Coordinator::epoch() const { return fabric_->coordinator_epoch(); }
FabricStatus Coordinator::status() const { return fabric_->status(); }

void Coordinator::request_stop() {
  if (shared_ != nullptr) {
    shared_->stopping = true;
  }
  if (listener_ != nullptr) {
    listener_->close();
  }
  if (shared_ != nullptr) {
    std::lock_guard<std::mutex> guard(shared_->mutex);
    for (const std::shared_ptr<Session>& session : shared_->sessions) {
      // A controller connection is closed by its own handler once its final
      // response has been written, so a shutdown acknowledgement is never lost.
      if (session->is_agent && session->connection != nullptr) {
        session->connection->close();
      }
    }
  }
}

Frame Coordinator::make_response(bool ok, ErrorCode code, const std::string& detail,
                                 const std::vector<std::byte>& body) {
  ByteWriter writer;
  writer.u32(ok ? 1U : 0U);
  writer.u16(static_cast<std::uint16_t>(code));
  writer.str(detail);
  writer.blob(ByteSpan(body.data(), body.size()));
  Frame frame;
  frame.header.message_type = MessageType::CONTROL_RESPONSE;
  frame.header.coordinator_epoch = fabric_->coordinator_epoch();
  frame.payload = writer.data();
  return frame;
}

Result<void> Coordinator::handle_agent_hello(std::shared_ptr<Session> session,
                                             const Frame& frame) {
  ByteReader reader(frame.payload_span());
  HelloPayload hello;
  if (!decode(reader, &hello)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed HELLO");
  }
  ClusterRegistration registration;
  registration.cluster_id = hello.cluster_id;
  registration.region = hello.region;
  registration.site = hello.site;
  registration.failure_domain = hello.failure_domain;
  registration.location_descriptor = hello.location_descriptor;
  registration.worker_id = hello.worker_id;
  registration.worker_boot_id = hello.worker_boot_id;
  registration.capabilities = hello.capabilities;
  registration.capacity = hello.capacity;
  registration.health = ClusterHealthState::HEALTHY;
  registration.expected_coordinator_epoch = hello.expected_coordinator_epoch;

  auto registered = fabric_->register_cluster(registration);
  HelloAckPayload ack;
  ack.cluster_id = hello.cluster_id;
  ack.coordinator_id = config_.coordinator_id;
  ack.coordinator_epoch = fabric_->coordinator_epoch();
  if (!registered) {
    ack.accepted = 0;
    ack.detail = registered.error().to_string();
  } else {
    ack.accepted = 1;
    ack.incarnation = registered.value().incarnation;
    ack.worker_id = registered.value().worker_id;
    ack.worker_boot_id = registered.value().worker_boot_id;
    ack.detail = "registered";
    session->cluster_id = hello.cluster_id;
    session->incarnation = registered.value().incarnation;
    session->worker_boot_id = registered.value().worker_boot_id;
    session->host = host_of(session->connection->peer());
    session->data_port = static_cast<std::uint16_t>(hello.listen_port);
    session->is_agent = true;
    {
      std::lock_guard<std::mutex> guard(shared_->agents_mutex);
      shared_->agents[hello.cluster_id] = session;
    }
  }
  ByteWriter writer;
  encode(writer, ack);
  Frame reply;
  reply.header.message_type = MessageType::HELLO_ACK;
  reply.header.coordinator_epoch = ack.coordinator_epoch;
  reply.payload = writer.data();
  std::lock_guard<std::mutex> guard(session->send_mutex);
  return session->connection->send_frame(reply);
}

Result<void> Coordinator::handle_agent_frame(std::shared_ptr<Session> session,
                                             const Frame& frame) {
  const MessageType type = frame.header.message_type;
  SimplePayload ack;
  ack.ok = 1;
  MessageType reply_type = MessageType::ERROR_REPORT;

  if (type == MessageType::EVIDENCE) {
    ByteReader reader(frame.payload_span());
    EvidencePayload evidence;
    if (!decode(reader, &evidence)) {
      return make_error(ErrorCode::FRAME_CORRUPT, "malformed EVIDENCE");
    }
    ClusterEvidenceUpdate update;
    update.cluster_id = evidence.cluster_id;
    update.incarnation = evidence.cluster_incarnation;
    update.worker_boot_id = evidence.worker_boot_id;
    update.health = evidence.health;
    update.capabilities = evidence.capabilities;
    update.capacity = evidence.capacity;
    update.has_capabilities = true;
    update.has_capacity = true;
    update.expected_coordinator_epoch = evidence.expected_coordinator_epoch;
    auto published = fabric_->publish_cluster_evidence(update);
    if (!published) {
      ack.ok = 0;
      ack.code = published.error().code;
      ack.detail = published.error().to_string();
    } else {
      ack.detail = "evidence accepted";
    }
    reply_type = MessageType::EVIDENCE_ACK;
  } else if (type == MessageType::TRANSFER_PROGRESS) {
    ByteReader reader(frame.payload_span());
    TransferReportPayload report;
    if (!decode(reader, &report)) {
      return make_error(ErrorCode::FRAME_CORRUPT, "malformed TRANSFER_PROGRESS");
    }
    TransferProgress progress;
    progress.transfer_id = report.transfer_id;
    progress.transfer_generation = report.transfer_generation;
    progress.replica_id = report.replica_id;
    progress.replica_generation = report.replica_generation;
    progress.operation_id = report.operation_id;
    progress.operation_generation = report.operation_generation;
    progress.cluster_id = session->cluster_id;
    progress.cluster_incarnation = report.cluster_incarnation;
    progress.worker_boot_id = report.worker_boot_id;
    progress.expected_coordinator_epoch = fabric_->coordinator_epoch();
    progress.bytes_transferred = report.bytes_transferred;
    auto recorded = fabric_->record_transfer_progress(progress);
    if (!recorded) {
      ack.ok = 0;
      ack.code = recorded.error().code;
      ack.detail = recorded.error().to_string();
    } else {
      ack.detail = "progress accepted";
    }
    reply_type = MessageType::TRANSFER_PROGRESS_ACK;
  } else if (type == MessageType::TRANSFER_RESULT) {
    ByteReader reader(frame.payload_span());
    TransferReportPayload report;
    if (!decode(reader, &report)) {
      return make_error(ErrorCode::FRAME_CORRUPT, "malformed TRANSFER_RESULT");
    }
    if (report.failed != 0U) {
      TransferProgress progress;
      progress.transfer_id = report.transfer_id;
      progress.transfer_generation = report.transfer_generation;
      progress.replica_id = report.replica_id;
      progress.replica_generation = report.replica_generation;
      progress.operation_id = report.operation_id;
      progress.operation_generation = report.operation_generation;
      progress.cluster_id = session->cluster_id;
      progress.cluster_incarnation = report.cluster_incarnation;
      progress.worker_boot_id = report.worker_boot_id;
      progress.expected_coordinator_epoch = fabric_->coordinator_epoch();
      progress.failed = true;
      progress.failure_reason = report.detail;
      auto recorded = fabric_->record_transfer_progress(progress);
      if (!recorded) {
        ack.ok = 0;
        ack.code = recorded.error().code;
        ack.detail = recorded.error().to_string();
      } else {
        ack.detail = "failure recorded";
      }
    } else {
      TransferCompletion completion;
      completion.transfer_id = report.transfer_id;
      completion.transfer_generation = report.transfer_generation;
      completion.replica_id = report.replica_id;
      completion.replica_generation = report.replica_generation;
      completion.operation_id = report.operation_id;
      completion.operation_generation = report.operation_generation;
      completion.cluster_id = session->cluster_id;
      completion.cluster_incarnation = report.cluster_incarnation;
      completion.worker_boot_id = report.worker_boot_id;
      completion.expected_coordinator_epoch = fabric_->coordinator_epoch();
      completion.bytes_transferred = report.bytes_transferred;
      completion.measured_digest = report.measured_digest;
      completion.transfer_complete = true;
      auto recorded = fabric_->record_transfer_completion(completion);
      if (!recorded) {
        ack.ok = 0;
        ack.code = recorded.error().code;
        ack.detail = recorded.error().to_string();
      } else {
        ack.detail = "transfer recorded";
      }
    }
    reply_type = MessageType::TRANSFER_RESULT_ACK;
  } else if (type == MessageType::VERIFY_RESULT) {
    ByteReader reader(frame.payload_span());
    VerifyReportPayload report;
    if (!decode(reader, &report)) {
      return make_error(ErrorCode::FRAME_CORRUPT, "malformed VERIFY_RESULT");
    }
    ReplicaVerification verification;
    verification.verification_id = report.verification_id;
    verification.replica_id = report.replica_id;
    verification.replica_generation = report.replica_generation;
    verification.operation_id = report.operation_id;
    verification.operation_generation = report.operation_generation;
    verification.cluster_id = session->cluster_id;
    verification.cluster_incarnation = report.cluster_incarnation;
    verification.worker_boot_id = report.worker_boot_id;
    verification.expected_coordinator_epoch = fabric_->coordinator_epoch();
    verification.integrity_verified = report.integrity_verified != 0U;
    verification.measured_digest = report.measured_digest;
    verification.compatibility_verified = report.compatibility_verified != 0U;
    verification.verified_bytes = report.verified_bytes;
    verification.established_durability = static_cast<DurabilityClass>(report.durability);
    verification.detail = report.detail;
    auto recorded = fabric_->verify_replica(verification);
    if (!recorded) {
      ack.ok = 0;
      ack.code = recorded.error().code;
      ack.detail = recorded.error().to_string();
    } else {
      ack.detail = "verification recorded";
    }
    reply_type = MessageType::VERIFY_RESULT_ACK;
  } else if (type == MessageType::TRANSFER_INTENT_ACK ||
             type == MessageType::TRANSFER_PROGRESS_ACK ||
             type == MessageType::TRANSFER_RESULT_ACK ||
             type == MessageType::VERIFY_RESULT_ACK || type == MessageType::SHUTDOWN_ACK ||
             type == MessageType::HEARTBEAT_ACK) {
    // Acknowledgements carry no authority and need no reply. A session is never
    // torn down because an agent acknowledged something: losing the session
    // would lose track of a live cluster incarnation.
    return Status{};
  } else if (type == MessageType::HEARTBEAT) {
    SimplePayload heartbeat;
    heartbeat.ok = 1;
    heartbeat.detail = "alive";
    ByteWriter writer;
    encode(writer, heartbeat);
    Frame reply;
    reply.header.message_type = MessageType::HEARTBEAT_ACK;
    reply.header.coordinator_epoch = fabric_->coordinator_epoch();
    reply.payload = writer.data();
    std::lock_guard<std::mutex> guard(session->send_mutex);
    Status sent = session->connection->send_frame(reply);
    shared_->condition.notify_all();
    return sent;
  } else {
    return make_error(ErrorCode::PROTOCOL_UNSUPPORTED, "unexpected agent message type");
  }

  ByteWriter writer;
  encode(writer, ack);
  Frame reply;
  reply.header.message_type = reply_type;
  reply.header.coordinator_epoch = fabric_->coordinator_epoch();
  reply.payload = writer.data();
  {
    std::lock_guard<std::mutex> guard(session->send_mutex);
    Status sent = session->connection->send_frame(reply);
    if (!sent) {
      return sent.error();
    }
  }
  if (ack.ok == 0U) {
    // A fencing rejection is reported back as an error so that an agent acting on
    // stale authority learns that it has been fenced instead of waiting for a
    // normal acknowledgement.
    reply_type = MessageType::ERROR_REPORT;
    ack.detail = std::string(is_fencing_error(ack.code) ? "fenced: " : "rejected: ") + ack.detail;
  }
  if (config_.trace) {
    config_.trace(std::string("agent frame ") + to_string(type) + " -> " + to_string(reply_type) +
                  ": " + ack.detail);
  }
  shared_->condition.notify_all();
  return Status{};
}

Result<std::vector<std::byte>> Coordinator::dispatch(ControlOperation operation,
                                                     ByteSpan arguments) {
  ByteReader reader(arguments);
  ByteWriter body;
  switch (operation) {
    case ControlOperation::STATUS: {
      encode_status(body, status());
      break;
    }
    case ControlOperation::REGISTER_STATE: {
      StateGenerationSpec spec;
      if (!detail::decode(reader, &spec)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed REGISTER_STATE arguments");
      }
      auto result = fabric_->register_state(spec);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::COMMIT_STATE: {
      StateId state_id;
      StateGeneration generation;
      CommitId commit_id;
      if (!detail::decode_id(reader, &state_id) || !detail::decode_id(reader, &generation) ||
          !detail::decode_id(reader, &commit_id)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed COMMIT_STATE arguments");
      }
      auto result = fabric_->commit_state_generation(state_id, generation, commit_id);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::QUERY_STATE: {
      StateId state_id;
      StateGeneration generation;
      if (!detail::decode_id(reader, &state_id) || !detail::decode_id(reader, &generation)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed QUERY_STATE arguments");
      }
      auto result = fabric_->query_state(state_id, generation);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::LIST_CLUSTERS: {
      auto result = fabric_->list_clusters();
      if (!result) {
        return result.error();
      }
      body.u32(static_cast<std::uint32_t>(result.value().size()));
      for (const ClusterRecord& record : result.value()) {
        detail::encode(body, record);
      }
      break;
    }
    case ControlOperation::REGISTER_REPLICA: {
      ReplicaRegistration registration;
      if (!detail::decode(reader, &registration)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed REGISTER_REPLICA arguments");
      }
      auto result = fabric_->register_replica(registration);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::TRANSFER_COMPLETION: {
      TransferCompletion completion;
      if (!detail::decode(reader, &completion)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed TRANSFER_COMPLETION arguments");
      }
      auto result = fabric_->record_transfer_completion(completion);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::VERIFY_REPLICA: {
      ReplicaVerification verification;
      if (!detail::decode(reader, &verification)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed VERIFY_REPLICA arguments");
      }
      auto result = fabric_->verify_replica(verification);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::COMMIT_REPLICA: {
      ReplicaCommit commit;
      if (!detail::decode(reader, &commit)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed COMMIT_REPLICA arguments");
      }
      auto result = fabric_->commit_replica(commit);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::QUERY_REPLICA: {
      ReplicaId replica_id;
      if (!detail::decode_id(reader, &replica_id)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed QUERY_REPLICA arguments");
      }
      auto result = fabric_->query_replica(replica_id);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::LIST_REPLICAS: {
      StateId state_id;
      StateGeneration generation;
      if (!detail::decode_id(reader, &state_id) || !detail::decode_id(reader, &generation)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed LIST_REPLICAS arguments");
      }
      auto result = fabric_->list_replicas(state_id, generation);
      if (!result) {
        return result.error();
      }
      body.u32(static_cast<std::uint32_t>(result.value().size()));
      for (const ReplicaRecord& record : result.value()) {
        detail::encode(body, record);
      }
      break;
    }
    case ControlOperation::AUTHORIZE_REPLICATION:
    case ControlOperation::AUTHORIZE_MIGRATION: {
      Result<TransferAuthorization> authorized =
          make_error(ErrorCode::INVALID_ARGUMENT, "unset");
      if (operation == ControlOperation::AUTHORIZE_REPLICATION) {
        ReplicationRequest request;
        if (!detail::decode(reader, &request)) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "malformed replication request");
        }
        authorized = fabric_->authorize_replication(request);
      } else {
        MigrationRequest request;
        if (!detail::decode(reader, &request)) {
          return make_error(ErrorCode::INVALID_ARGUMENT, "malformed migration request");
        }
        authorized = fabric_->authorize_migration(request);
      }
      if (!authorized) {
        return authorized.error();
      }
      const TransferAuthorization& auth = authorized.value();
      std::shared_ptr<Session> destination;
      std::shared_ptr<Session> source;
      {
        std::lock_guard<std::mutex> guard(shared_->agents_mutex);
        auto destination_it = shared_->agents.find(auth.destination_cluster);
        auto source_it = shared_->agents.find(auth.source_cluster);
        if (destination_it != shared_->agents.end()) {
          destination = destination_it->second;
        }
        if (source_it != shared_->agents.end()) {
          source = source_it->second;
        }
      }
      if (destination == nullptr || source == nullptr) {
        return make_error(ErrorCode::UNKNOWN_CLUSTER,
                          "the source or destination cluster agent is not connected");
      }
      TransferIntentPayload intent;
      intent.transfer_id = auth.transfer_id;
      intent.transfer_generation = auth.transfer_generation;
      intent.operation_id = auth.operation_id;
      intent.operation_generation = auth.operation_generation;
      intent.migration_id = auth.migration_id;
      intent.migration_generation = auth.migration_generation;
      intent.reservation_id = auth.reservation_id;
      intent.reservation_generation = auth.reservation_generation;
      intent.state_id = auth.state_id;
      intent.state_generation = auth.state_generation;
      intent.destination_replica = auth.destination_replica;
      intent.destination_replica_generation = auth.destination_replica_generation;
      intent.destination_cluster = auth.destination_cluster;
      intent.destination_incarnation = auth.destination_incarnation;
      intent.source_cluster = auth.source_cluster;
      intent.source_incarnation = auth.source_incarnation;
      intent.source_replica = auth.source_replica;
      intent.payload_bytes = auth.transfer_bytes.known() ? auth.transfer_bytes.bytes : 0U;
      intent.expected_digest = auth.expected_digest;
      intent.payload_seed = auth.transfer_id.value();
      intent.coordinator_epoch = auth.coordinator_epoch;
      intent.reservation_expires_at = auth.reservation_expires_at;
      intent.source_host = source->host;
      intent.source_data_port = source->data_port;
      intent.destination_data_port = destination->data_port;
      ByteWriter intent_writer;
      encode(intent_writer, intent);
      Frame intent_frame;
      intent_frame.header.message_type = MessageType::TRANSFER_INTENT;
      intent_frame.header.coordinator_epoch = fabric_->coordinator_epoch();
      intent_frame.header.cluster_id = auth.destination_cluster;
      intent_frame.header.cluster_incarnation = auth.destination_incarnation;
      intent_frame.header.operation_generation = auth.operation_generation;
      intent_frame.payload = intent_writer.data();
      if (config_.trace) {
        config_.trace("dispatching to cluster " + auth.destination_cluster.str() +
                      " pulling from cluster " + auth.source_cluster.str() + ":" +
                      std::to_string(source->data_port));
      }
      {
        std::lock_guard<std::mutex> guard(destination->send_mutex);
        Status sent = destination->connection->send_frame(intent_frame);
        if (!sent) {
          return sent.error();
        }
      }
      detail::encode(body, auth);
      break;
    }
    case ControlOperation::COMMIT_OPERATION: {
      OperationCommit commit;
      if (!detail::decode(reader, &commit)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed COMMIT_OPERATION arguments");
      }
      auto result = fabric_->commit_operation(commit);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      shared_->condition.notify_all();
      break;
    }
    case ControlOperation::CANCEL_OPERATION: {
      OperationCancellation cancellation;
      if (!detail::decode(reader, &cancellation)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed CANCEL_OPERATION arguments");
      }
      auto result = fabric_->cancel_operation(cancellation);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      shared_->condition.notify_all();
      break;
    }
    case ControlOperation::QUERY_OPERATION:
    case ControlOperation::AWAIT_OPERATION: {
      OperationId operation_id;
      if (!detail::decode_id(reader, &operation_id)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed operation identity");
      }
      if (operation == ControlOperation::QUERY_OPERATION) {
        auto result = fabric_->query_operation(operation_id);
        if (!result) {
          return result.error();
        }
        detail::encode(body, result.value());
        break;
      }
      std::unique_lock<std::mutex> guard(shared_->mutex);
      for (;;) {
        auto result = fabric_->query_operation(operation_id);
        if (!result) {
          return result.error();
        }
        const OperationRecord& record = result.value();
        if (record.state == OperationState::PREPARED || record.is_terminal()) {
          detail::encode(body, record);
          break;
        }
        shared_->condition.wait(guard);
      }
      break;
    }
    case ControlOperation::LIST_OPERATIONS: {
      auto result = fabric_->list_operations();
      if (!result) {
        return result.error();
      }
      body.u32(static_cast<std::uint32_t>(result.value().size()));
      for (const OperationRecord& record : result.value()) {
        detail::encode(body, record);
      }
      break;
    }
    case ControlOperation::QUERY_AUTHORIZATION: {
      OperationId operation_id;
      if (!detail::decode_id(reader, &operation_id)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed operation identity");
      }
      auto result = fabric_->query_authorization(operation_id);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::EVALUATE_REUSE: {
      StateId state_id;
      StateGeneration generation;
      ClusterId cluster_id;
      if (!detail::decode_id(reader, &state_id) || !detail::decode_id(reader, &generation) ||
          !detail::decode_id(reader, &cluster_id)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed EVALUATE_REUSE arguments");
      }
      auto result = fabric_->evaluate_reuse(state_id, generation, cluster_id);
      if (!result) {
        return result.error();
      }
      encode_reuse(body, result.value());
      break;
    }
    case ControlOperation::RECONCILE_REPLICA_SET: {
      StateId state_id;
      StateGeneration generation;
      if (!detail::decode_id(reader, &state_id) || !detail::decode_id(reader, &generation)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed reconcile arguments");
      }
      auto result = fabric_->reconcile_replica_set(state_id, generation);
      if (!result) {
        return result.error();
      }
      encode_reconciliation(body, result.value());
      break;
    }
    case ControlOperation::PLAN_REPLICATION: {
      ReplicationRequest request;
      if (!detail::decode(reader, &request)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed replication request");
      }
      auto result = fabric_->plan_replication(request);
      if (!result) {
        return result.error();
      }
      encode_placement(body, result.value());
      break;
    }
    case ControlOperation::EXPLAIN_REPLICA_SET: {
      StateId state_id;
      StateGeneration generation;
      if (!detail::decode_id(reader, &state_id) || !detail::decode_id(reader, &generation)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed explanation arguments");
      }
      auto result = fabric_->explain_replica_set_state(state_id, generation);
      if (!result) {
        return result.error();
      }
      encode_explanation(body, result.value());
      break;
    }
    case ControlOperation::MARK_CLUSTER_UNREACHABLE: {
      ClusterId cluster_id;
      ClusterIncarnationId incarnation;
      std::string reason;
      if (!detail::decode_id(reader, &cluster_id) ||
          !detail::decode_id(reader, &incarnation) ||
          !reader.str(kMaxReasonBytes, &reason)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed unreachable arguments");
      }
      auto result = fabric_->mark_cluster_unreachable(cluster_id, incarnation, reason);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::RETIRE_CLUSTER: {
      ClusterId cluster_id;
      ClusterIncarnationId incarnation;
      if (!detail::decode_id(reader, &cluster_id) || !detail::decode_id(reader, &incarnation)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed retire arguments");
      }
      auto result = fabric_->retire_cluster(cluster_id, incarnation);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::REINCARNATE_CLUSTER: {
      ClusterRegistration registration;
      if (!detail::decode(reader, &registration)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed reincarnation arguments");
      }
      auto result = fabric_->register_cluster(registration);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::REVALIDATE_REPLICA: {
      ReplicaTransition transition;
      if (!detail::decode(reader, &transition)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed revalidation arguments");
      }
      auto result = fabric_->revalidate_replica(transition);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::PUBLISH_COMPATIBILITY: {
      CompatibilityRecord record;
      if (!detail::decode(reader, &record)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed compatibility record");
      }
      auto result = fabric_->publish_compatibility(record);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::PUBLISH_REPLICATION_POLICY: {
      ReplicationPolicyRecord policy;
      if (!detail::decode(reader, &policy)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed replication policy");
      }
      auto result = fabric_->publish_replication_policy(policy);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::PUBLISH_PLACEMENT_POLICY: {
      PlacementPolicyRecord policy;
      if (!detail::decode(reader, &policy)) {
        return make_error(ErrorCode::INVALID_ARGUMENT, "malformed placement policy");
      }
      auto result = fabric_->publish_placement_policy(policy);
      if (!result) {
        return result.error();
      }
      detail::encode(body, result.value());
      break;
    }
    case ControlOperation::SHUTDOWN: {
      body.u32(1);
      // The acknowledgement is sent by the connection handler, which then stops
      // the coordinator. Closing every control connection here would discard the
      // acknowledgement the controller is waiting for.
      shared_->shutdown_requested = true;
      break;
    }
    case ControlOperation::INVALID:
    case ControlOperation::TRANSFER_PROGRESS:
    default:
      return make_error(ErrorCode::PROTOCOL_UNSUPPORTED, "unsupported control operation");
  }
  return body.data();
}

void Coordinator::release_session(const std::shared_ptr<Session>& session) {
  if (session->connection != nullptr) {
    session->connection->close();
  }
  std::lock_guard<std::mutex> guard(shared_->mutex);
  shared_->sessions.erase(
      std::remove(shared_->sessions.begin(), shared_->sessions.end(), session),
      shared_->sessions.end());
}

Result<void> Coordinator::handle_connection(std::unique_ptr<TcpConnection> connection) {
  auto session = std::make_shared<Session>();
  session->connection = std::move(connection);
  {
    std::lock_guard<std::mutex> guard(shared_->mutex);
    shared_->sessions.push_back(session);
  }

  auto first = session->connection->receive_frame(config_.maximum_payload_bytes);
  if (!first) {
    (void)release_session(session);
    return first.error();
  }
  if (first.value().header.message_type == MessageType::HELLO) {
    auto hello = handle_agent_hello(session, first.value());
    if (!hello) {
      return hello.error();
    }
    for (;;) {
      auto frame = session->connection->receive_frame(config_.maximum_payload_bytes);
      if (!frame) {
        break;
      }
      auto handled = handle_agent_frame(session, frame.value());
      if (!handled) {
        break;
      }
    }
    if (session->cluster_id.valid()) {
      std::lock_guard<std::mutex> guard(shared_->agents_mutex);
      shared_->agents.erase(session->cluster_id);
    }
    release_session(session);
    return Status{};
  }
  if (first.value().header.message_type != MessageType::CONTROL_REQUEST) {
    return make_error(ErrorCode::PROTOCOL_UNSUPPORTED,
                      "the first frame must be HELLO or a control request");
  }

  Frame frame = std::move(first.value());
  for (;;) {
    ByteReader reader(frame.payload_span());
    std::uint16_t raw_operation = 0;
    std::vector<std::byte> arguments;
    if (!reader.u16(&raw_operation) || !reader.blob(config_.maximum_payload_bytes, &arguments)) {
      Frame failure = make_response(
          false, ErrorCode::FRAME_CORRUPT,
          "malformed control request: payload " + std::to_string(frame.payload.size()) +
              " bytes, " + reader.error().to_string(),
          {});
      std::lock_guard<std::mutex> guard(session->send_mutex);
      (void)session->connection->send_frame(failure);
    } else {
      const auto operation = static_cast<ControlOperation>(raw_operation);
      if (!is_valid(operation)) {
        Frame failure = make_response(false, ErrorCode::PROTOCOL_UNSUPPORTED,
                                      "unknown control operation", {});
        std::lock_guard<std::mutex> guard(session->send_mutex);
        (void)session->connection->send_frame(failure);
      } else {
        auto result = dispatch(operation, ByteSpan(arguments.data(), arguments.size()));
        Frame response;
        if (result) {
          response = make_response(true, ErrorCode::OK, "ok", result.value());
        } else {
          response = make_response(false, result.error().code, result.error().to_string(), {});
        }
        std::lock_guard<std::mutex> guard(session->send_mutex);
        Status sent = session->connection->send_frame(response);
        if (!sent) {
          break;
        }
      }
      if (shared_->shutdown_requested) {
        request_stop();
        break;
      }
    }
    auto next = session->connection->receive_frame(config_.maximum_payload_bytes);
    if (!next) {
      break;
    }
    frame = std::move(next.value());
  }
  release_session(session);
  return Status{};
}

Result<void> Coordinator::serve() {
  for (;;) {
    auto accepted = listener_->accept();
    if (!accepted) {
      break;
    }
    std::unique_ptr<TcpConnection> connection = std::move(accepted.value());
    std::shared_ptr<Shared> shared = shared_;
    std::lock_guard<std::mutex> guard(shared->workers_mutex);
    shared->workers.emplace_back([this, connection = std::move(connection)]() mutable {
      (void)handle_connection(std::move(connection));
    });
  }
  {
    std::lock_guard<std::mutex> guard(shared_->workers_mutex);
    for (std::thread& worker : shared_->workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    shared_->workers.clear();
  }
  return Status{};
}

// --- controller client ------------------------------------------------------

CoordinatorClient::~CoordinatorClient() = default;

Result<std::unique_ptr<CoordinatorClient>> CoordinatorClient::connect(const std::string& host,
                                                                     std::uint16_t port) {
  std::unique_ptr<CoordinatorClient> client(new CoordinatorClient());
  client->sockets_ = std::make_unique<SocketRuntime>();
  auto connection = TcpConnection::connect(host, port);
  if (!connection) {
    return connection.error();
  }
  client->connection_ = std::move(connection.value());
  return client;
}

Result<std::vector<std::byte>> CoordinatorClient::call(ControlOperation operation,
                                                       const std::vector<std::byte>& arguments) {
  if (!is_valid(operation)) {
    return make_error(ErrorCode::INVALID_ARGUMENT, "unknown control operation");
  }
  ByteWriter writer;
  writer.u16(static_cast<std::uint16_t>(operation));
  writer.blob(ByteSpan(arguments.data(), arguments.size()));
  Frame frame;
  frame.header.message_type = MessageType::CONTROL_REQUEST;
  frame.payload = writer.data();
  Status sent = connection_->send_frame(frame);
  if (!sent) {
    return sent.error();
  }
  auto reply = connection_->receive_frame(kMaxFramePayloadBytes);
  if (!reply) {
    return reply.error();
  }
  if (reply.value().header.message_type != MessageType::CONTROL_RESPONSE) {
    return make_error(ErrorCode::PROTOCOL_UNSUPPORTED, "unexpected control response type");
  }
  ByteReader reader(reply.value().payload_span());
  std::uint32_t ok = 0;
  std::uint16_t code = 0;
  std::string detail;
  std::vector<std::byte> body;
  if (!reader.u32(&ok) || !reader.u16(&code) || !reader.str(kMaxReasonBytes, &detail) ||
      !reader.blob(kMaxFramePayloadBytes, &body)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed control response");
  }
  if (ok == 0U) {
    return Error(static_cast<ErrorCode>(code), detail);
  }
  return body;
}

namespace {

std::vector<std::byte> empty_arguments() { return {}; }

}  // namespace

Result<FabricStatus> CoordinatorClient::status() {
  auto body = call(ControlOperation::STATUS, empty_arguments());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  FabricStatus status;
  if (!decode_status(reader, &status)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed status response");
  }
  return status;
}

Result<StateGenerationRecord> CoordinatorClient::register_state(const StateGenerationSpec& spec) {
  ByteWriter writer;
  detail::encode(writer, spec);
  auto body = call(ControlOperation::REGISTER_STATE, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  StateGenerationRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed state response");
  }
  return record;
}

Result<StateGenerationRecord> CoordinatorClient::commit_state(StateId state_id,
                                                              StateGeneration generation,
                                                              CommitId commit_id) {
  ByteWriter writer;
  detail::encode_id(writer, state_id);
  detail::encode_id(writer, generation);
  detail::encode_id(writer, commit_id);
  auto body = call(ControlOperation::COMMIT_STATE, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  StateGenerationRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed state response");
  }
  return record;
}

Result<StateGenerationRecord> CoordinatorClient::query_state(StateId state_id,
                                                             StateGeneration generation) {
  ByteWriter writer;
  detail::encode_id(writer, state_id);
  detail::encode_id(writer, generation);
  auto body = call(ControlOperation::QUERY_STATE, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  StateGenerationRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed state response");
  }
  return record;
}

Result<std::vector<ClusterRecord>> CoordinatorClient::list_clusters() {
  auto body = call(ControlOperation::LIST_CLUSTERS, empty_arguments());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  std::uint32_t count = 0;
  if (!reader.u32(&count) || count > 1024U) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed cluster list");
  }
  std::vector<ClusterRecord> records;
  for (std::uint32_t index = 0; index < count; ++index) {
    ClusterRecord record;
    if (!detail::decode(reader, &record)) {
      return make_error(ErrorCode::FRAME_CORRUPT, "malformed cluster record");
    }
    records.push_back(std::move(record));
  }
  return records;
}

Result<ReplicaRecord> CoordinatorClient::register_replica(const ReplicaRegistration& registration) {
  ByteWriter writer;
  detail::encode(writer, registration);
  auto body = call(ControlOperation::REGISTER_REPLICA, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicaRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica response");
  }
  return record;
}

Result<ReplicaRecord> CoordinatorClient::record_transfer_progress(const TransferProgress& progress) {
  ByteWriter writer;
  detail::encode(writer, progress);
  auto body = call(ControlOperation::TRANSFER_PROGRESS, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicaRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica response");
  }
  return record;
}

Result<ReplicaRecord> CoordinatorClient::record_transfer_completion(
    const TransferCompletion& completion) {
  ByteWriter writer;
  detail::encode(writer, completion);
  auto body = call(ControlOperation::TRANSFER_COMPLETION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicaRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica response");
  }
  return record;
}

Result<ReplicaRecord> CoordinatorClient::verify_replica(const ReplicaVerification& verification) {
  ByteWriter writer;
  detail::encode(writer, verification);
  auto body = call(ControlOperation::VERIFY_REPLICA, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicaRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica response");
  }
  return record;
}

Result<ReplicaRecord> CoordinatorClient::commit_replica(const ReplicaCommit& commit) {
  ByteWriter writer;
  detail::encode(writer, commit);
  auto body = call(ControlOperation::COMMIT_REPLICA, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicaRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica response");
  }
  return record;
}

Result<ReplicaRecord> CoordinatorClient::query_replica(ReplicaId replica_id) {
  ByteWriter writer;
  detail::encode_id(writer, replica_id);
  auto body = call(ControlOperation::QUERY_REPLICA, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicaRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica response");
  }
  return record;
}

Result<std::vector<ReplicaRecord>> CoordinatorClient::list_replicas(StateId state_id,
                                                                    StateGeneration generation) {
  ByteWriter writer;
  detail::encode_id(writer, state_id);
  detail::encode_id(writer, generation);
  auto body = call(ControlOperation::LIST_REPLICAS, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  std::uint32_t count = 0;
  if (!reader.u32(&count) || count > 4096U) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica list");
  }
  std::vector<ReplicaRecord> records;
  for (std::uint32_t index = 0; index < count; ++index) {
    ReplicaRecord record;
    if (!detail::decode(reader, &record)) {
      return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica record");
    }
    records.push_back(std::move(record));
  }
  return records;
}

Result<TransferAuthorization> CoordinatorClient::authorize_replication(
    const ReplicationRequest& request) {
  ByteWriter writer;
  detail::encode(writer, request);
  auto body = call(ControlOperation::AUTHORIZE_REPLICATION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  TransferAuthorization authorization;
  if (!detail::decode(reader, &authorization)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed authorization response");
  }
  return authorization;
}

Result<TransferAuthorization> CoordinatorClient::authorize_migration(
    const MigrationRequest& request) {
  ByteWriter writer;
  detail::encode(writer, request);
  auto body = call(ControlOperation::AUTHORIZE_MIGRATION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  TransferAuthorization authorization;
  if (!detail::decode(reader, &authorization)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed authorization response");
  }
  return authorization;
}

Result<TransferAuthorization> CoordinatorClient::query_authorization(OperationId operation_id) {
  ByteWriter writer;
  detail::encode_id(writer, operation_id);
  auto body = call(ControlOperation::QUERY_AUTHORIZATION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  TransferAuthorization authorization;
  if (!detail::decode(reader, &authorization)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed authorization response");
  }
  return authorization;
}

Result<OperationRecord> CoordinatorClient::commit_operation(const OperationCommit& commit) {
  ByteWriter writer;
  detail::encode(writer, commit);
  auto body = call(ControlOperation::COMMIT_OPERATION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  OperationRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed operation response");
  }
  return record;
}

Result<OperationRecord> CoordinatorClient::cancel_operation(
    const OperationCancellation& cancellation) {
  ByteWriter writer;
  detail::encode(writer, cancellation);
  auto body = call(ControlOperation::CANCEL_OPERATION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  OperationRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed operation response");
  }
  return record;
}

Result<OperationRecord> CoordinatorClient::query_operation(OperationId operation_id) {
  ByteWriter writer;
  detail::encode_id(writer, operation_id);
  auto body = call(ControlOperation::QUERY_OPERATION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  OperationRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed operation response");
  }
  return record;
}

Result<OperationRecord> CoordinatorClient::await_operation(OperationId operation_id) {
  ByteWriter writer;
  detail::encode_id(writer, operation_id);
  auto body = call(ControlOperation::AWAIT_OPERATION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  OperationRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed operation response");
  }
  return record;
}

Result<std::vector<OperationRecord>> CoordinatorClient::list_operations() {
  auto body = call(ControlOperation::LIST_OPERATIONS, empty_arguments());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  std::uint32_t count = 0;
  if (!reader.u32(&count) || count > 4096U) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed operation list");
  }
  std::vector<OperationRecord> records;
  for (std::uint32_t index = 0; index < count; ++index) {
    OperationRecord record;
    if (!detail::decode(reader, &record)) {
      return make_error(ErrorCode::FRAME_CORRUPT, "malformed operation record");
    }
    records.push_back(std::move(record));
  }
  return records;
}

Result<ReuseEvaluation> CoordinatorClient::evaluate_reuse(StateId state_id,
                                                          StateGeneration generation,
                                                          ClusterId cluster_id) {
  ByteWriter writer;
  detail::encode_id(writer, state_id);
  detail::encode_id(writer, generation);
  detail::encode_id(writer, cluster_id);
  auto body = call(ControlOperation::EVALUATE_REUSE, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReuseEvaluation evaluation;
  if (!decode_reuse(reader, &evaluation)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed reuse response");
  }
  return evaluation;
}

Result<ReplicaSetReconciliation> CoordinatorClient::reconcile_replica_set(
    StateId state_id, StateGeneration generation) {
  ByteWriter writer;
  detail::encode_id(writer, state_id);
  detail::encode_id(writer, generation);
  auto body = call(ControlOperation::RECONCILE_REPLICA_SET, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicaSetReconciliation reconciliation;
  if (!decode_reconciliation(reader, &reconciliation)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed reconciliation response");
  }
  return reconciliation;
}

Result<PlacementEvaluation> CoordinatorClient::plan_replication(const ReplicationRequest& request) {
  ByteWriter writer;
  detail::encode(writer, request);
  auto body = call(ControlOperation::PLAN_REPLICATION, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  PlacementEvaluation evaluation;
  if (!decode_placement(reader, &evaluation)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed placement response");
  }
  return evaluation;
}

Result<ClusterRecord> CoordinatorClient::mark_cluster_unreachable(
    ClusterId cluster_id, ClusterIncarnationId incarnation, const std::string& reason) {
  ByteWriter writer;
  detail::encode_id(writer, cluster_id);
  detail::encode_id(writer, incarnation);
  writer.str(reason);
  auto body = call(ControlOperation::MARK_CLUSTER_UNREACHABLE, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ClusterRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed cluster response");
  }
  return record;
}

Result<ClusterRecord> CoordinatorClient::retire_cluster(ClusterId cluster_id,
                                                        ClusterIncarnationId incarnation) {
  ByteWriter writer;
  detail::encode_id(writer, cluster_id);
  detail::encode_id(writer, incarnation);
  auto body = call(ControlOperation::RETIRE_CLUSTER, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ClusterRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed cluster response");
  }
  return record;
}

Result<ClusterRecord> CoordinatorClient::reincarnate_cluster(
    const ClusterRegistration& registration) {
  ByteWriter writer;
  detail::encode(writer, registration);
  auto body = call(ControlOperation::REINCARNATE_CLUSTER, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ClusterRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed cluster response");
  }
  return record;
}

Result<ReplicaRecord> CoordinatorClient::revalidate_replica(const ReplicaTransition& transition) {
  ByteWriter writer;
  detail::encode(writer, transition);
  auto body = call(ControlOperation::REVALIDATE_REPLICA, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicaRecord record;
  if (!detail::decode(reader, &record)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed replica response");
  }
  return record;
}

Result<CompatibilityRecord> CoordinatorClient::publish_compatibility(
    const CompatibilityRecord& record) {
  ByteWriter writer;
  detail::encode(writer, record);
  auto body = call(ControlOperation::PUBLISH_COMPATIBILITY, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  CompatibilityRecord published;
  if (!detail::decode(reader, &published)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed compatibility response");
  }
  return published;
}

Result<ReplicationPolicyRecord> CoordinatorClient::publish_replication_policy(
    const ReplicationPolicyRecord& policy) {
  ByteWriter writer;
  detail::encode(writer, policy);
  auto body = call(ControlOperation::PUBLISH_REPLICATION_POLICY, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  ReplicationPolicyRecord published;
  if (!detail::decode(reader, &published)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed policy response");
  }
  return published;
}

Result<PlacementPolicyRecord> CoordinatorClient::publish_placement_policy(
    const PlacementPolicyRecord& policy) {
  ByteWriter writer;
  detail::encode(writer, policy);
  auto body = call(ControlOperation::PUBLISH_PLACEMENT_POLICY, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  PlacementPolicyRecord published;
  if (!detail::decode(reader, &published)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed policy response");
  }
  return published;
}

Result<Explanation> CoordinatorClient::explain_replica_set(StateId state_id,
                                                           StateGeneration generation) {
  ByteWriter writer;
  detail::encode_id(writer, state_id);
  detail::encode_id(writer, generation);
  auto body = call(ControlOperation::EXPLAIN_REPLICA_SET, writer.data());
  if (!body) {
    return body.error();
  }
  ByteReader reader(ByteSpan(body.value().data(), body.value().size()));
  Explanation explanation;
  if (!decode_explanation(reader, &explanation)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed explanation response");
  }
  return explanation;
}

Result<void> CoordinatorClient::shutdown_coordinator() {
  auto body = call(ControlOperation::SHUTDOWN, empty_arguments());
  if (!body) {
    return body.error();
  }
  return Status{};
}

}  // namespace ccsf
