// Cross-Cluster State Fabric - coordinator session and stop proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These cases run a coordinator inside the test process on a loopback port and
// drive it through real control connections. A case never leaves a thread
// behind: every wait is bounded and a coordinator that fails to react is
// released by other means so that the failure is reported instead of hanging.

#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ccsf/cluster_agent.hpp"
#include "ccsf/coordinator.hpp"
#include "ccsf/protocol.hpp"
#include "ccsf/transport.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

namespace {

constexpr std::chrono::seconds kReactionBound{5};

CoordinatorConfig coordinator_config(std::uint64_t id) {
  CoordinatorConfig config;
  config.coordinator_id = CoordinatorId::from_value(id);
  config.home_cluster = ClusterId::from_value(100);
  config.require_durable_commits = false;
  config.placement_policy = default_placement_policy(1);
  config.replication_policy = default_replication_policy(2, 2);
  return config;
}

/// Runs serve() on its own thread and reports when it has returned.
class Served {
 public:
  explicit Served(Coordinator& coordinator)
      : coordinator_(coordinator), done_(finished_.get_future()), thread_([this]() {
          (void)coordinator_.serve();
          finished_.set_value();
        }) {}
  ~Served() {
    coordinator_.request_stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  Served(const Served&) = delete;
  Served& operator=(const Served&) = delete;

  [[nodiscard]] bool returned_within(std::chrono::seconds bound) {
    return done_.wait_for(bound) == std::future_status::ready;
  }

 private:
  Coordinator& coordinator_;
  std::promise<void> finished_;
  std::future<void> done_;
  std::thread thread_;
};

using Client = std::unique_ptr<CoordinatorClient>;

Client connect_client(const Coordinator& coordinator) {
  auto client = CoordinatorClient::connect("127.0.0.1", coordinator.port());
  if (!client) {
    return nullptr;
  }
  return std::move(client.value());
}

struct Deployment {
  IdSource ids;
  ClusterId source{ClusterId::from_value(201)};
  ClusterId destination{ClusterId::from_value(202)};
  StateId state{StateId::from_value(301)};
  StateGeneration generation{StateGeneration::from_value(1)};
};

ClusterRegistration cluster_shape(Deployment& deployment, ClusterId cluster) {
  return make_cluster_registration(deployment.ids, cluster, RegionId::from_value(1),
                                   FailureDomainId::from_value(cluster.value()),
                                   WorkerBootId::from_value(cluster.value() + 5000U),
                                   ClusterHealthState::HEALTHY, {"gpu:sm_120"});
}

/// Registers a cluster through the control plane, without an agent session.
bool register_cluster(CoordinatorClient& client, Deployment& deployment, ClusterId cluster) {
  return client.reincarnate_cluster(cluster_shape(deployment, cluster)).has_value();
}

/// Registers a cluster the way an agent does: a session that says HELLO.
std::unique_ptr<TcpConnection> register_agent(const Coordinator& coordinator,
                                              Deployment& deployment, ClusterId cluster) {
  auto connection = TcpConnection::connect("127.0.0.1", coordinator.port());
  if (!connection) {
    return nullptr;
  }
  const ClusterRegistration shape = cluster_shape(deployment, cluster);
  HelloPayload hello;
  hello.cluster_id = cluster;
  hello.region = shape.region;
  hello.site = shape.site;
  hello.failure_domain = shape.failure_domain;
  hello.worker_id = WorkerId::from_value(cluster.value());
  hello.worker_boot_id = shape.worker_boot_id;
  hello.location_descriptor = shape.location_descriptor;
  hello.capabilities = shape.capabilities;
  hello.capabilities.canonicalize();
  hello.capacity = shape.capacity;
  hello.listen_port = 1;
  ByteWriter writer;
  encode(writer, hello);
  Frame frame;
  frame.header.message_type = MessageType::HELLO;
  frame.payload = writer.data();
  if (!connection.value()->send_frame(frame)) {
    return nullptr;
  }
  auto reply = connection.value()->receive_frame(kMaxFramePayloadBytes);
  if (!reply || reply.value().header.message_type != MessageType::HELLO_ACK) {
    return nullptr;
  }
  ByteReader reader(reply.value().payload_span());
  HelloAckPayload ack;
  if (!decode(reader, &ack) || ack.accepted == 0U) {
    return nullptr;
  }
  return std::move(connection.value());
}

/// Commits one state generation and an authoritative replica of it in the
/// source cluster, entirely through control requests.
bool seed_source_replica(CoordinatorClient& client, Deployment& deployment) {
  const Digest digest = digest_of("payload");
  StateGenerationSpec spec;
  spec.state_id = deployment.state;
  spec.generation = deployment.generation;
  spec.lineage = LineageId::from_value(deployment.state.value());
  spec.state_class = StateClass::OPAQUE_STATE;
  spec.consistency = ConsistencyMode::IMMUTABLE;
  spec.logical_size = SizeValue::of(2048);
  spec.physical_size = SizeValue::of(2048);
  spec.content_digest = digest;
  spec.compatibility_id = CompatibilityId::from_value(deployment.ids.take());
  spec.compatibility_generation = CompatibilityGeneration::from_value(1);
  spec.durability = DurabilityClass::DURABLE;
  spec.reuse_class = ReuseClass::SHARED_READ;
  spec.expected_reuse = ReuseValue::estimated(14U, 800U);
  spec.replication_requirements =
      make_replication_policy(ReplicationPolicyId::from_value(1),
                              ReplicationPolicyGeneration::from_value(1),
                              ReplicationPolicyForm::N_REPLICAS, 2)
          .requirements;
  spec.provenance.producer = "test-harness";
  if (!client.register_state(spec) ||
      !client.commit_state(deployment.state, deployment.generation,
                           CommitId::from_value(deployment.ids.take()))) {
    return false;
  }

  ReplicaRegistration registration;
  registration.state_id = deployment.state;
  registration.state_generation = deployment.generation;
  registration.location.cluster_id = deployment.source;
  registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
  registration.content_digest = digest;
  auto registered = client.register_replica(registration);
  if (!registered) {
    return false;
  }
  TransferCompletion completion;
  completion.replica_id = registered.value().replica_id;
  completion.replica_generation = registered.value().replica_generation;
  completion.cluster_id = deployment.source;
  completion.bytes_transferred = 2048;
  completion.measured_digest = digest;
  if (!client.record_transfer_completion(completion)) {
    return false;
  }
  ReplicaVerification verification;
  verification.replica_id = registered.value().replica_id;
  verification.replica_generation = registered.value().replica_generation;
  verification.cluster_id = deployment.source;
  verification.integrity_verified = true;
  verification.measured_digest = digest;
  verification.compatibility_verified = true;
  verification.established_durability = DurabilityClass::DURABLE;
  verification.verified_bytes = 2048;
  if (!client.verify_replica(verification)) {
    return false;
  }
  ReplicaCommit commit;
  commit.replica_id = registered.value().replica_id;
  commit.replica_generation = registered.value().replica_generation;
  commit.commit_id = CommitId::from_value(deployment.ids.take());
  return client.commit_replica(commit).has_value();
}

ReplicationRequest replicate_to_destination(const Deployment& deployment) {
  ReplicationRequest request;
  request.state_id = deployment.state;
  request.state_generation = deployment.generation;
  request.destination_cluster = deployment.destination;
  return request;
}

}  // namespace

CCSF_CASE(coordinator, stop_does_not_wait_for_an_idle_controller) {
  auto coordinator = Coordinator::start(coordinator_config(70));
  ctx.check(coordinator.has_value(), "the coordinator starts");
  bool connected = false;
  bool acknowledged = false;
  bool returned = false;
  {
    Served served(*coordinator.value());
    Client idle = connect_client(*coordinator.value());
    Client stopper = connect_client(*coordinator.value());
    connected = idle != nullptr && stopper != nullptr && idle->status().has_value();
    if (connected) {
      acknowledged = stopper->shutdown_coordinator().has_value();
    }
    returned = served.returned_within(kReactionBound);
    // Dropping the idle connection releases a coordinator that is still waiting
    // for it, so the defect is reported rather than hanging the suite.
    idle.reset();
  }
  ctx.check(connected, "two controllers connect");
  ctx.check(acknowledged, "the shutdown request is acknowledged");
  ctx.check(returned,
            "serve() returns after a stop although another controller is still connected");
}

CCSF_CASE(coordinator, stop_releases_a_controller_awaiting_an_operation) {
  auto coordinator = Coordinator::start(coordinator_config(71));
  ctx.check(coordinator.has_value(), "the coordinator starts");
  bool prepared = false;
  bool authorized = false;
  bool released = false;
  bool returned = false;
  {
    Served served(*coordinator.value());
    Client control = connect_client(*coordinator.value());
    Client waiting = connect_client(*coordinator.value());
    Client rescue = connect_client(*coordinator.value());
    Deployment deployment;
    prepared = control != nullptr && waiting != nullptr && rescue != nullptr &&
               register_cluster(*control, deployment, deployment.source) &&
               register_cluster(*control, deployment, deployment.destination) &&
               seed_source_replica(*control, deployment) && rescue->status().has_value();
    OperationId operation;
    if (prepared) {
      // Authorized on the runtime directly: no agent will ever move this
      // operation, so a controller awaiting it waits until something ends it.
      auto authorization =
          coordinator.value()->fabric().authorize_replication(replicate_to_destination(deployment));
      authorized = authorization.has_value();
      if (authorized) {
        operation = authorization.value().operation_id;
      }
    }
    if (authorized) {
      std::promise<void> answered;
      std::future<void> answer = answered.get_future();
      std::thread awaiting([&waiting, &answered, operation]() {
        (void)waiting->await_operation(operation);
        answered.set_value();
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      coordinator.value()->request_stop();
      released = answer.wait_for(kReactionBound) == std::future_status::ready;
      if (!released) {
        // Ending the operation wakes a coordinator that left the controller
        // waiting, so the defect is reported rather than hanging the suite.
        OperationCancellation cancellation;
        cancellation.operation_id = operation;
        cancellation.reason = "released by the test";
        (void)rescue->cancel_operation(cancellation);
      }
      awaiting.join();
    }
    control.reset();
    waiting.reset();
    rescue.reset();
    returned = served.returned_within(kReactionBound);
  }
  ctx.check(prepared, "the deployment is prepared through the control plane");
  ctx.check(authorized, "an operation is in flight");
  ctx.check(released, "a stop releases the controller that awaits an operation");
  ctx.check(returned, "serve() returns after the stop");
}

CCSF_CASE(coordinator, an_undeliverable_transfer_intent_leaves_no_operation_in_flight) {
  auto coordinator = Coordinator::start(coordinator_config(72));
  ctx.check(coordinator.has_value(), "the coordinator starts");
  bool prepared = false;
  bool refused = false;
  bool listed = false;
  std::size_t in_flight = 0;
  {
    Served served(*coordinator.value());
    Client control = connect_client(*coordinator.value());
    Deployment deployment;
    prepared = control != nullptr &&
               register_cluster(*control, deployment, deployment.source) &&
               register_cluster(*control, deployment, deployment.destination) &&
               seed_source_replica(*control, deployment);
    if (prepared) {
      // Both clusters are registered but neither has an agent session, so the
      // transfer intent cannot be delivered.
      refused = !control->authorize_replication(replicate_to_destination(deployment)).has_value();
      auto operations = control->list_operations();
      listed = operations.has_value();
      if (listed) {
        for (const OperationRecord& operation : operations.value()) {
          if (!operation.is_terminal()) {
            ++in_flight;
          }
        }
      }
    }
    control.reset();
  }
  ctx.check(prepared, "the deployment is prepared through the control plane");
  ctx.check(refused, "a replication whose intent cannot be delivered is refused");
  ctx.check(listed, "operations can be listed");
  ctx.check(in_flight == 0U, "the refused replication leaves no operation in flight");
}

CCSF_CASE(coordinator, a_reconnected_agent_is_not_dropped_when_its_old_session_ends) {
  auto coordinator = Coordinator::start(coordinator_config(73));
  ctx.check(coordinator.has_value(), "the coordinator starts");
  bool prepared = false;
  bool old_session_ended = false;
  bool authorized = false;
  bool delivered = false;
  {
    Served served(*coordinator.value());
    Client control = connect_client(*coordinator.value());
    Deployment deployment;
    auto source = register_agent(*coordinator.value(), deployment, deployment.source);
    auto old_session = register_agent(*coordinator.value(), deployment, deployment.destination);
    auto new_session = register_agent(*coordinator.value(), deployment, deployment.destination);
    prepared = control != nullptr && source != nullptr && old_session != nullptr &&
               new_session != nullptr && seed_source_replica(*control, deployment);
    if (prepared) {
      // A frame no agent may send ends the old session; the coordinator closing
      // the connection shows that it has finished tearing the session down.
      Frame unexpected;
      unexpected.header.message_type = MessageType::DATA_REQUEST;
      old_session_ended = old_session->send_frame(unexpected).has_value() &&
                          !old_session->receive_frame(kMaxFramePayloadBytes).has_value();
    }
    if (old_session_ended) {
      authorized = control->authorize_replication(replicate_to_destination(deployment)).has_value();
    }
    if (authorized) {
      auto intent = new_session->receive_frame(kMaxFramePayloadBytes);
      delivered = intent.has_value() &&
                  intent.value().header.message_type == MessageType::TRANSFER_INTENT;
    }
    control.reset();
  }
  ctx.check(prepared, "two sessions register the destination cluster");
  ctx.check(old_session_ended, "the older session is ended");
  ctx.check(authorized, "the cluster is still reachable through its newer session");
  ctx.check(delivered, "the transfer intent is delivered on the newer session");
}

CCSF_CASE(coordinator, a_connection_that_opens_with_an_illegal_frame_is_closed) {
  auto coordinator = Coordinator::start(coordinator_config(74));
  ctx.check(coordinator.has_value(), "the coordinator starts");
  bool sent = false;
  bool closed = false;
  {
    auto served = std::make_unique<Served>(*coordinator.value());
    auto connection = TcpConnection::connect("127.0.0.1", coordinator.value()->port());
    if (connection.has_value()) {
      Frame unexpected;
      unexpected.header.message_type = MessageType::DATA_REQUEST;
      sent = connection.value()->send_frame(unexpected).has_value();
    }
    if (sent) {
      std::promise<bool> finished;
      std::future<bool> outcome = finished.get_future();
      std::thread reading([&connection, &finished]() {
        finished.set_value(!connection.value()->receive_frame(kMaxFramePayloadBytes).has_value());
      });
      const bool reacted = outcome.wait_for(kReactionBound) == std::future_status::ready;
      if (!reacted) {
        // A coordinator that kept the connection gives it up only when it is
        // destroyed, so the defect is reported rather than hanging the suite.
        served.reset();
        coordinator.value().reset();
      }
      reading.join();
      closed = reacted && outcome.get();
    }
    served.reset();
  }
  ctx.check(sent, "the frame is sent");
  ctx.check(closed, "a connection whose first frame is neither HELLO nor a control request is closed");
}

CCSF_CASE(coordinator, a_stopped_agent_has_ended_its_data_connections) {
  auto coordinator = Coordinator::start(coordinator_config(75));
  ctx.check(coordinator.has_value(), "the coordinator starts");
  bool started = false;
  bool hosted = false;
  bool stopped = false;
  bool ended = false;
  {
    Served served(*coordinator.value());
    Deployment deployment;
    const ClusterRegistration shape = cluster_shape(deployment, deployment.source);
    ClusterAgentConfig config;
    config.coordinator_port = coordinator.value()->port();
    config.cluster_id = deployment.source;
    config.region = shape.region;
    config.site = shape.site;
    config.failure_domain = shape.failure_domain;
    config.location_descriptor = shape.location_descriptor;
    config.capabilities = shape.capabilities;
    config.capacity = shape.capacity;
    auto agent = ClusterAgent::start(config);
    started = agent.has_value();
    if (started) {
      std::promise<void> finished;
      std::future<void> done = finished.get_future();
      std::thread serving([&agent, &finished]() {
        (void)agent.value()->serve();
        finished.set_value();
      });
      auto data = TcpConnection::connect("127.0.0.1", agent.value()->data_port());
      if (data.has_value()) {
        // Hosting bytes over the data plane proves that a thread of the agent is
        // serving this connection, and it keeps serving it afterwards.
        HostStatePayload host;
        host.state_id = deployment.state;
        host.state_generation = deployment.generation;
        host.replica_id = ReplicaId::from_value(901);
        host.replica_generation = ReplicaGeneration::from_value(1);
        host.bytes = pattern_bytes(64);
        host.expected_digest = payload_digest(64);
        ByteWriter writer;
        encode(writer, host);
        Frame frame;
        frame.header.message_type = MessageType::HOST_STATE;
        frame.payload = writer.data();
        if (data.value()->send_frame(frame).has_value()) {
          auto reply = data.value()->receive_frame(kMaxFramePayloadBytes);
          hosted = reply.has_value() &&
                   reply.value().header.message_type == MessageType::HOST_STATE_ACK;
        }
      }
      agent.value()->request_stop();
      stopped = done.wait_for(kReactionBound) == std::future_status::ready;
      // Once serve() has returned the agent may be destroyed, so no thread of it
      // may still be serving a data connection.
      ended = stopped && agent.value()->data_connections() == 1U;
      if (data.has_value()) {
        data.value()->close();
      }
      if (!ended) {
        // A data thread that outlived serve() needs a moment to leave the agent
        // before the agent is destroyed.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      }
      serving.join();
    }
  }
  ctx.check(started, "the agent registers");
  ctx.check(hosted, "the agent serves a data connection");
  ctx.check(stopped, "serve() returns after a stop");
  ctx.check(ended, "the data connection has ended by the time serve() returns");
}
