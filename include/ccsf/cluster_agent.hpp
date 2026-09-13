// Cross-Cluster State Fabric - cluster agent.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A cluster agent is an independent operating system process that:
//   * registers a durable ClusterId and mints a fresh ClusterIncarnationId per
//     process start;
//   * publishes capability, capacity and health evidence;
//   * hosts replica bytes in a local store;
//   * serves and pulls real bytes over a framed TCP data plane;
//   * executes transfer and verification intents under explicit authority.
//
// The agent never decides authority: it reports evidence and executes intents.

#ifndef CCSF_CLUSTER_AGENT_HPP
#define CCSF_CLUSTER_AGENT_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ccsf/protocol.hpp"
#include "ccsf/result.hpp"
#include "ccsf/transport.hpp"

namespace ccsf {

struct ClusterAgentConfig {
  std::string coordinator_host{"127.0.0.1"};
  std::uint16_t coordinator_port{0};
  std::string data_bind_address{"127.0.0.1"};
  ClusterId cluster_id;
  RegionId region;
  SiteId site;
  FailureDomainId failure_domain;
  std::string location_descriptor;
  ClusterCapabilities capabilities;
  ClusterCapacity capacity;
  std::size_t maximum_payload_bytes{kMaxFramePayloadBytes};
  std::size_t maximum_data_connections{4};
  CoordinatorEpoch expected_coordinator_epoch;

  /// Optional observability hook. When set, the agent reports every
  /// authority-bearing step it takes. It is never used for control flow.
  std::function<void(const std::string&)> trace;
};

/// The agent's local replica byte store.
class ReplicaStore {
 public:
  /// Stores bytes for a replica, replacing any previous value.
  void put(ReplicaId replica, StateId state_id, StateGeneration generation,
           std::vector<std::byte> bytes, Digest digest);

  [[nodiscard]] bool contains(ReplicaId replica) const;
  [[nodiscard]] bool read(ReplicaId replica, std::uint64_t offset, std::uint64_t length,
                          std::vector<std::byte>* out) const;
  [[nodiscard]] std::uint64_t size_of(ReplicaId replica) const;
  [[nodiscard]] Digest digest_of(ReplicaId replica) const;
  [[nodiscard]] std::size_t count() const;
  void erase(ReplicaId replica);
  [[nodiscard]] std::vector<ReplicaId> replica_ids() const;

 private:
  struct Entry {
    StateId state_id;
    StateGeneration generation;
    std::vector<std::byte> bytes;
    Digest digest;
  };
  mutable std::mutex mutex_;
  std::map<ReplicaId, Entry> entries_;
};

class ClusterAgent {
 public:
  ~ClusterAgent();
  ClusterAgent(const ClusterAgent&) = delete;
  ClusterAgent& operator=(const ClusterAgent&) = delete;

  /// Binds the data plane, connects to the coordinator, registers and publishes
  /// initial evidence.
  static Result<std::unique_ptr<ClusterAgent>> start(const ClusterAgentConfig& config);

  /// Runs the control loop until the coordinator sends SHUTDOWN or the link
  /// closes. Transfer intents are executed inline: the loop is the agent's
  /// serialization point for authority-bearing work.
  Result<void> serve();

  /// Asks the agent to stop serving. Safe to call from another thread.
  void request_stop();

  [[nodiscard]] ClusterIncarnationId incarnation() const;
  [[nodiscard]] WorkerBootId worker_boot_id() const;
  [[nodiscard]] WorkerId worker_id() const;
  [[nodiscard]] std::uint16_t data_port() const;
  [[nodiscard]] bool registered() const;
  [[nodiscard]] std::string last_error() const;
  [[nodiscard]] const ReplicaStore& store() const { return store_; }

  /// Number of data-plane connections served, used by the resource audits.
  [[nodiscard]] std::uint64_t data_connections() const;

 private:
  ClusterAgent() = default;

  Result<void> connect_and_register();
  void data_loop();
  void handle_data_connection(std::unique_ptr<TcpConnection> connection);
  Result<void> handle_transfer_intent(const Frame& frame);
  /// Reads frames until the expected acknowledgement arrives. Bounded: an
  /// unexpected flood is a protocol violation, not a reason to block forever.
  Result<Frame> await_acknowledgement(MessageType expected);
  Result<std::vector<std::byte>> pull_from_peer(const TransferIntentPayload& intent, Digest* digest);

  ClusterAgentConfig config_;
  std::unique_ptr<TcpListener> listener_;
  std::unique_ptr<TcpConnection> control_;
  std::unique_ptr<SocketRuntime> sockets_;
  ClusterIncarnationId incarnation_;
  WorkerId worker_id_;
  WorkerBootId worker_boot_id_;
  CoordinatorEpoch coordinator_epoch_;
  bool registered_{false};
  bool stopping_{false};

  ReplicaStore store_;
  mutable std::mutex error_mutex_;
  std::string last_error_;
  std::mutex send_mutex_;
  mutable std::mutex thread_mutex_;
  std::uintptr_t data_thread_{0};
  std::uint64_t data_connections_{0};
  std::size_t active_data_connections_{0};
};

}  // namespace ccsf

#endif  // CCSF_CLUSTER_AGENT_HPP
