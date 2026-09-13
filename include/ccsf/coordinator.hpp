// Cross-Cluster State Fabric - coordinator process and control plane client.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The coordinator process owns the authoritative Fabric runtime and the durable
// control plane. Cluster agents hold long-lived control connections to it; a
// controller (test driver, CLI, or any operator tool) issues typed control
// requests over its own connection. The coordinator is the only component that
// decides placement, authority and commit.

#ifndef CCSF_COORDINATOR_HPP
#define CCSF_COORDINATOR_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ccsf/fabric.hpp"
#include "ccsf/protocol.hpp"
#include "ccsf/result.hpp"
#include "ccsf/transport.hpp"

namespace ccsf {

/// Control-plane operations available to a controller.
enum class ControlOperation : std::uint16_t {
  INVALID = 0,
  STATUS = 1,
  REGISTER_STATE = 2,
  COMMIT_STATE = 3,
  QUERY_STATE = 4,
  LIST_CLUSTERS = 5,
  REGISTER_REPLICA = 6,
  TRANSFER_COMPLETION = 7,
  VERIFY_REPLICA = 8,
  COMMIT_REPLICA = 9,
  AUTHORIZE_REPLICATION = 10,
  AUTHORIZE_MIGRATION = 11,
  COMMIT_OPERATION = 12,
  CANCEL_OPERATION = 13,
  EVALUATE_REUSE = 14,
  RECONCILE_REPLICA_SET = 15,
  AWAIT_OPERATION = 16,
  QUERY_OPERATION = 17,
  LIST_REPLICAS = 18,
  MARK_CLUSTER_UNREACHABLE = 19,
  REVALIDATE_REPLICA = 20,
  QUERY_REPLICA = 21,
  PLAN_REPLICATION = 22,
  EXPLAIN_REPLICA_SET = 23,
  RETIRE_CLUSTER = 24,
  PUBLISH_COMPATIBILITY = 25,
  QUERY_AUTHORIZATION = 26,
  LIST_OPERATIONS = 27,
  PUBLISH_REPLICATION_POLICY = 28,
  PUBLISH_PLACEMENT_POLICY = 29,
  TRANSFER_PROGRESS = 30,
  REINCARNATE_CLUSTER = 31,
  SHUTDOWN = 32
};

const char* to_string(ControlOperation operation) noexcept;
bool is_valid(ControlOperation operation) noexcept;

struct CoordinatorConfig {
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{0};
  CoordinatorId coordinator_id;
  ClusterId home_cluster;
  std::string persistence_directory;
  bool require_durable_commits{true};
  std::size_t maximum_payload_bytes{kMaxFramePayloadBytes};
  PlacementPolicyRecord placement_policy;
  ReplicationPolicyRecord replication_policy;

  /// Optional observability hook for authority-bearing coordinator steps.
  std::function<void(const std::string&)> trace;
};

/// The coordinator process's server side.
class Coordinator {
 public:
  ~Coordinator();
  Coordinator(const Coordinator&) = delete;
  Coordinator& operator=(const Coordinator&) = delete;

  static Result<std::unique_ptr<Coordinator>> start(const CoordinatorConfig& config);

  /// Accepts and serves connections until request_stop() is called.
  Result<void> serve();

  void request_stop();

  [[nodiscard]] std::uint16_t port() const;
  [[nodiscard]] CoordinatorEpoch epoch() const;
  [[nodiscard]] FabricStatus status() const;
  [[nodiscard]] Fabric& fabric() { return *fabric_; }

 private:
  Coordinator() = default;

  struct Session;
  Result<void> handle_connection(std::unique_ptr<TcpConnection> connection);
  void release_session(const std::shared_ptr<Session>& session);
  Result<void> handle_agent_hello(std::shared_ptr<Session> session, const Frame& frame);
  Result<void> handle_agent_frame(std::shared_ptr<Session> session, const Frame& frame);
  Result<std::vector<std::byte>> dispatch(ControlOperation operation, ByteSpan arguments);
  Frame make_response(bool ok, ErrorCode code, const std::string& detail,
                      const std::vector<std::byte>& body);

  CoordinatorConfig config_;
  std::unique_ptr<Fabric> fabric_;
  std::unique_ptr<TcpListener> listener_;
  std::unique_ptr<SocketRuntime> sockets_;

  struct Shared;
  std::shared_ptr<Shared> shared_;
};

/// A controller-side client of the coordinator's control plane.
class CoordinatorClient {
 public:
  ~CoordinatorClient();
  CoordinatorClient(const CoordinatorClient&) = delete;
  CoordinatorClient& operator=(const CoordinatorClient&) = delete;

  static Result<std::unique_ptr<CoordinatorClient>> connect(const std::string& host,
                                                            std::uint16_t port);

  /// Raw control request. Returns the decoded body on success.
  Result<std::vector<std::byte>> call(ControlOperation operation,
                                      const std::vector<std::byte>& arguments);

  // Typed helpers used by the reference deployment, the CLI and the tests.
  Result<FabricStatus> status();
  Result<StateGenerationRecord> register_state(const StateGenerationSpec& spec);
  Result<StateGenerationRecord> commit_state(StateId state_id, StateGeneration generation,
                                             CommitId commit_id);
  Result<StateGenerationRecord> query_state(StateId state_id, StateGeneration generation);
  Result<std::vector<ClusterRecord>> list_clusters();
  Result<ReplicaRecord> register_replica(const ReplicaRegistration& registration);
  Result<ReplicaRecord> record_transfer_progress(const TransferProgress& progress);
  Result<ReplicaRecord> record_transfer_completion(const TransferCompletion& completion);
  Result<ReplicaRecord> verify_replica(const ReplicaVerification& verification);
  Result<ReplicaRecord> commit_replica(const ReplicaCommit& commit);
  Result<ReplicaRecord> query_replica(ReplicaId replica_id);
  Result<std::vector<ReplicaRecord>> list_replicas(StateId state_id, StateGeneration generation);
  Result<TransferAuthorization> authorize_replication(const ReplicationRequest& request);
  Result<TransferAuthorization> authorize_migration(const MigrationRequest& request);
  Result<TransferAuthorization> query_authorization(OperationId operation_id);
  Result<OperationRecord> commit_operation(const OperationCommit& commit);
  Result<OperationRecord> cancel_operation(const OperationCancellation& cancellation);
  Result<OperationRecord> query_operation(OperationId operation_id);
  Result<OperationRecord> await_operation(OperationId operation_id);
  Result<std::vector<OperationRecord>> list_operations();
  Result<ReuseEvaluation> evaluate_reuse(StateId state_id, StateGeneration generation,
                                         ClusterId cluster_id);
  Result<ReplicaSetReconciliation> reconcile_replica_set(StateId state_id,
                                                         StateGeneration generation);
  Result<ClusterRecord> mark_cluster_unreachable(ClusterId cluster_id,
                                                 ClusterIncarnationId incarnation,
                                                 const std::string& reason);
  Result<ClusterRecord> retire_cluster(ClusterId cluster_id, ClusterIncarnationId incarnation);
  Result<ClusterRecord> reincarnate_cluster(const ClusterRegistration& registration);
  Result<ReplicaRecord> revalidate_replica(const ReplicaTransition& transition);
  Result<CompatibilityRecord> publish_compatibility(const CompatibilityRecord& record);
  Result<ReplicationPolicyRecord> publish_replication_policy(const ReplicationPolicyRecord& policy);
  Result<PlacementPolicyRecord> publish_placement_policy(const PlacementPolicyRecord& policy);
  Result<Explanation> explain_replica_set(StateId state_id, StateGeneration generation);
  Result<PlacementEvaluation> plan_replication(const ReplicationRequest& request);
  Result<void> shutdown_coordinator();

 private:
  CoordinatorClient() = default;
  std::unique_ptr<TcpConnection> connection_;
  std::unique_ptr<SocketRuntime> sockets_;
};

}  // namespace ccsf

#endif  // CCSF_COORDINATOR_HPP
