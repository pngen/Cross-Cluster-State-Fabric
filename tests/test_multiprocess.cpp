// Cross-Cluster State Fabric - real multiprocess proof obligations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case in this suite drives independent operating system processes over
// real TCP: one coordinator process, three cluster agent processes, and this
// test binary acting as the controller. Synchronization is by explicit
// readiness lines, acknowledgements and state barriers. Nothing here polls with
// sleeps, and nothing runs under a timeout.

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ccsf/cluster_agent.hpp"
#include "ccsf/coordinator.hpp"
#include "ccsf/process.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

namespace {

std::string field_value(const std::string& line, const std::string& key) {
  const std::size_t start = line.find(key);
  if (start == std::string::npos) {
    return std::string();
  }
  const std::size_t value_start = start + key.size();
  const std::size_t end = line.find(' ', value_start);
  return line.substr(value_start, end == std::string::npos ? std::string::npos
                                                           : end - value_start);
}

std::string executable_path(const char* macro_path) {
  std::error_code code;
  if (std::filesystem::exists(macro_path, code)) {
    return macro_path;
  }
  return std::string();
}

struct AgentHandle {
  std::unique_ptr<ChildProcess> process;
  ClusterId cluster;
  ClusterIncarnationId incarnation;
  WorkerBootId worker_boot;
  std::uint16_t data_port{0};
};

struct Deployment {
  std::unique_ptr<ChildProcess> coordinator;
  std::uint16_t coordinator_port{0};
  std::unique_ptr<CoordinatorClient> client;
  std::vector<std::unique_ptr<AgentHandle>> agents;

  ~Deployment() {
    for (std::unique_ptr<AgentHandle>& agent : agents) {
      if (agent->process != nullptr && agent->process->running()) {
        (void)agent->process->kill();
      }
      if (agent->process != nullptr) {
        (void)agent->process->wait();
      }
    }
    if (coordinator != nullptr && coordinator->running()) {
      (void)coordinator->kill();
    }
    if (coordinator != nullptr) {
      (void)coordinator->wait();
    }
  }

  AgentHandle* agent(ClusterId cluster) {
    for (std::unique_ptr<AgentHandle>& handle : agents) {
      if (handle->cluster == cluster) {
        return handle.get();
      }
    }
    return nullptr;
  }
};

/// Waits for a readiness marker. A child that reports a startup failure, or that
/// stops before becoming ready, must never leave the controller blocked: that is
/// how a hang would hide a real defect.
Result<std::string> wait_for_marker(ChildProcess& child, const std::string& marker) {
  auto line = child.wait_for_line([&marker](const std::string& text) {
    return text.rfind(marker, 0) == 0 ||
           text.find("failed to start") != std::string::npos ||
           text.rfind("AGENT STOPPED", 0) == 0 || text.rfind("COORDINATOR STOPPED", 0) == 0;
  });
  if (!line) {
    return line.error();
  }
  if (line.value().rfind(marker, 0) != 0) {
    return make_error(ErrorCode::INTERNAL_ERROR,
                      "the child process did not become ready: " + line.value());
  }
  return line.value();
}

std::string coordinator_executable() {
  const std::string path = executable_path(CCSF_TEST_COORDINATOR_PATH);
  return path;
}

std::string agent_executable() {
  const std::string path = executable_path(CCSF_TEST_AGENT_PATH);
  return path;
}

/// Starts a coordinator process and blocks until it announces readiness.
Result<std::pair<std::unique_ptr<ChildProcess>, std::uint16_t>> start_coordinator(
    const std::string& persistence, std::uint64_t coordinator_id, std::uint32_t replicas) {
  ProcessOptions options;
  options.executable = coordinator_executable();
  if (options.executable.empty()) {
    return make_error(ErrorCode::NOT_SUPPORTED, "the coordinator executable was not found");
  }
  options.arguments = {"--port",
                       "0",
                       "--coordinator-id",
                       std::to_string(coordinator_id),
                       "--replication-factor",
                       std::to_string(replicas),
                       "--persistence",
                       persistence,
                       "--log",
                       persistence + "/coordinator-trace.log"};
  auto child = ChildProcess::spawn(options);
  if (!child) {
    return child.error();
  }
  auto ready = wait_for_marker(*child.value(), "COORDINATOR READY");
  if (!ready) {
    return ready.error();
  }
  const std::uint16_t port =
      static_cast<std::uint16_t>(std::strtoul(field_value(ready.value(), "port=").c_str(),
                                              nullptr, 10));
  if (port == 0) {
    return make_error(ErrorCode::INTERNAL_ERROR, "the coordinator reported no port");
  }
  return std::make_pair(std::move(child.value()), port);
}

/// Starts a cluster agent process and blocks until it announces readiness.
Result<std::unique_ptr<AgentHandle>> start_agent(std::uint16_t coordinator_port,
                                                 std::uint64_t cluster, std::uint64_t region,
                                                 std::uint64_t failure_domain,
                                                 const std::string& tag) {
  ProcessOptions options;
  options.executable = agent_executable();
  if (options.executable.empty()) {
    return make_error(ErrorCode::NOT_SUPPORTED, "the cluster agent executable was not found");
  }
  options.arguments = {"--coordinator-port", std::to_string(coordinator_port), "--cluster",
                       std::to_string(cluster), "--region", std::to_string(region),
                       "--failure-domain", std::to_string(failure_domain), "--descriptor",
                       "reference-site-" + std::to_string(cluster), "--tag", tag, "--accelerator",
                       "sm_120", "--storage", "nvme", "--log",
                       std::string("runtime/agent-") + std::to_string(cluster) + "-" +
                           std::to_string(process_id()) + ".log"};
  auto child = ChildProcess::spawn(options);
  if (!child) {
    return child.error();
  }
  auto ready = wait_for_marker(*child.value(), "AGENT READY");
  if (!ready) {
    return ready.error();
  }
  auto handle = std::make_unique<AgentHandle>();
  handle->process = std::move(child.value());
  handle->cluster = ClusterId::from_value(cluster);
  auto incarnation = ClusterIncarnationId::parse(field_value(ready.value(), "incarnation="));
  auto boot = WorkerBootId::parse(field_value(ready.value(), "worker_boot="));
  if (!incarnation || !boot) {
    return make_error(ErrorCode::INTERNAL_ERROR, "the agent reported malformed identities");
  }
  handle->incarnation = incarnation.value();
  handle->worker_boot = boot.value();
  handle->data_port = static_cast<std::uint16_t>(
      std::strtoul(field_value(ready.value(), "data_port=").c_str(), nullptr, 10));
  return handle;
}

/// Pushes real bytes into an agent's local store over the framed data plane.
Result<Digest> host_payload(const AgentHandle& agent, StateId state, StateGeneration generation,
                            ReplicaId replica, ReplicaGeneration replica_generation,
                            const std::vector<std::byte>& payload) {
  auto connection = TcpConnection::connect("127.0.0.1", agent.data_port);
  if (!connection) {
    return connection.error();
  }
  ByteWriter writer;
  encode(writer, HostStatePayload{});
  HostStatePayload host;
  host.state_id = state;
  host.state_generation = generation;
  host.replica_id = replica;
  host.replica_generation = replica_generation;
  host.expected_digest = sha256(ByteSpan(payload.data(), payload.size()));
  host.bytes = payload;
  ByteWriter payload_writer;
  encode(payload_writer, host);
  Frame frame;
  frame.header.message_type = MessageType::HOST_STATE;
  frame.header.cluster_id = agent.cluster;
  frame.header.cluster_incarnation = agent.incarnation;
  frame.payload = payload_writer.data();
  Status sent = connection.value()->send_frame(frame);
  if (!sent) {
    return sent.error();
  }
  auto reply = connection.value()->receive_frame(kMaxFramePayloadBytes);
  if (!reply) {
    return reply.error();
  }
  ByteReader reader(reply.value().payload_span());
  SimplePayload ack;
  if (!decode(reader, &ack)) {
    return make_error(ErrorCode::FRAME_CORRUPT, "malformed host acknowledgement");
  }
  if (ack.ok == 0U) {
    return Error(ack.code, ack.detail);
  }
  return host.expected_digest;
}

/// Registers and commits one authoritative replica hosted by an agent.
Result<ReplicaId> establish_replica(CoordinatorClient& client, StateId state,
                                    StateGeneration generation, const ClusterRecord& cluster,
                                    const StateGenerationRecord& record, ReplicaId replica_id,
                                    const Digest& digest) {
  ReplicaRegistration registration;
  registration.replica_id = replica_id;
  registration.state_id = state;
  registration.state_generation = generation;
  registration.location.cluster_id = cluster.cluster_id;
  registration.location.cluster_incarnation = cluster.incarnation;
  registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
  registration.content_digest = digest;
  registration.expected_coordinator_epoch = record.committed_epoch;
  auto registered = client.register_replica(registration);
  if (!registered) {
    return registered.error();
  }
  TransferCompletion completion;
  completion.replica_id = registered.value().replica_id;
  completion.replica_generation = registered.value().replica_generation;
  completion.cluster_id = cluster.cluster_id;
  completion.cluster_incarnation = cluster.incarnation;
  completion.expected_coordinator_epoch = record.committed_epoch;
  completion.bytes_transferred = record.logical_size.bytes;
  completion.measured_digest = digest;
  auto transferred = client.record_transfer_completion(completion);
  if (!transferred) {
    return transferred.error();
  }
  ReplicaVerification verification;
  verification.replica_id = registered.value().replica_id;
  verification.replica_generation = registered.value().replica_generation;
  verification.cluster_id = cluster.cluster_id;
  verification.cluster_incarnation = cluster.incarnation;
  verification.expected_coordinator_epoch = record.committed_epoch;
  verification.integrity_verified = true;
  verification.measured_digest = digest;
  verification.verified_bytes = record.logical_size.bytes;
  verification.established_durability = DurabilityClass::DURABLE;
  auto verified = client.verify_replica(verification);
  if (!verified) {
    return verified.error();
  }
  ReplicaCommit commit;
  commit.replica_id = registered.value().replica_id;
  commit.commit_id = CommitId::from_value(900000 + replica_id.value());
  commit.expected_coordinator_epoch = record.committed_epoch;
  auto committed = client.commit_replica(commit);
  if (!committed) {
    return committed.error();
  }
  return registered.value().replica_id;
}

StateGenerationSpec make_spec(StateId state, StateGeneration generation, std::uint64_t size,
                              const Digest& digest, CompatibilityDescriptor required,
                              std::uint32_t replicas) {
  StateGenerationSpec spec;
  spec.state_id = state;
  spec.generation = generation;
  spec.lineage = LineageId::from_value(state.value());
  spec.state_class = StateClass::OPAQUE_STATE;
  spec.consistency = ConsistencyMode::IMMUTABLE;
  spec.logical_size = SizeValue::of(size);
  spec.physical_size = SizeValue::of(size);
  spec.content_digest = digest;
  spec.compatibility_id = CompatibilityId::from_value(state.value() * 10U + 3U);
  spec.compatibility_generation = CompatibilityGeneration::from_value(1);
  spec.compatibility_required = required;
  spec.durability = DurabilityClass::DURABLE;
  spec.reuse_class = ReuseClass::SHARED_READ;
  spec.replication_requirements =
      make_replication_policy(ReplicationPolicyId::from_value(1),
                              ReplicationPolicyGeneration::from_value(1),
                              ReplicationPolicyForm::N_REPLICAS, replicas)
          .requirements;
  spec.provenance.producer = "multiprocess-controller";
  return spec;
}

constexpr std::uint64_t kPayloadBytes = 1ULL << 20U;  // one mebibyte of real bytes

}  // namespace

CCSF_CASE(multiprocess, real_transfer_commit_and_cluster_death) {
  ctx.check(!coordinator_executable().empty(), "the coordinator executable must be built");
  ctx.check(!agent_executable().empty(), "the cluster agent executable must be built");

  const std::filesystem::path directory =
      std::filesystem::absolute(std::filesystem::path(fresh_directory("multiprocess")));
  Deployment deployment;

  ctx.note("phase REGISTER: starting the coordinator process");
  auto coordinator = start_coordinator(directory.string(), 77, 2);
  ctx.check(coordinator.has_value(), "the coordinator process starts");
  deployment.coordinator = std::move(coordinator.value().first);
  deployment.coordinator_port = coordinator.value().second;

  ctx.note("phase CONNECT: controller connects");
  auto client = CoordinatorClient::connect("127.0.0.1", deployment.coordinator_port);
  ctx.check(client.has_value(), "the controller connects to the coordinator");
  deployment.client = std::move(client.value());
  ctx.note("phase CONNECT: controller connected");

  const ClusterId cluster_a = ClusterId::from_value(4101);
  const ClusterId cluster_b = ClusterId::from_value(4102);
  const ClusterId cluster_c = ClusterId::from_value(4103);
  ctx.note("phase REGISTER: starting cluster agent A");
  auto agent_a = start_agent(deployment.coordinator_port, cluster_a.value(), 1, 1, "gpu:sm_120");
  ctx.note("phase REGISTER: cluster agent A is ready");
  auto agent_b = start_agent(deployment.coordinator_port, cluster_b.value(), 2, 2, "gpu:sm_120");
  ctx.note("phase REGISTER: cluster agent B is ready");
  auto agent_c = start_agent(deployment.coordinator_port, cluster_c.value(), 3, 3, "gpu:sm_120");
  ctx.note("phase REGISTER: cluster agent C is ready");
  ctx.check(agent_a.has_value(), "cluster agent A registers");
  ctx.check(agent_b.has_value(), "cluster agent B registers");
  ctx.check(agent_c.has_value(), "cluster agent C registers");
  const WorkerBootId boot_b = agent_b.value()->worker_boot;
  const ClusterIncarnationId incarnation_b = agent_b.value()->incarnation;
  deployment.agents.push_back(std::move(agent_a.value()));
  deployment.agents.push_back(std::move(agent_b.value()));
  deployment.agents.push_back(std::move(agent_c.value()));

  ctx.note("phase REGISTER: querying cluster records");
  auto clusters = deployment.client->list_clusters();
  ctx.check(clusters.has_value(),
            std::string("the cluster list is queryable: ") +
                (clusters ? std::string("ok") : clusters.error().to_string()));
  ctx.check(clusters.value().size() == 3,
            "three clusters are registered with the coordinator (observed " +
                std::to_string(clusters.value().size()) + ")");
  ctx.note("phase REGISTER: cluster records retrieved");
  ClusterRecord record_a;
  ClusterRecord record_b;
  ClusterRecord record_c;
  for (const ClusterRecord& record : clusters.value()) {
    if (record.cluster_id == cluster_a) record_a = record;
    if (record.cluster_id == cluster_b) record_b = record;
    if (record.cluster_id == cluster_c) record_c = record;
  }
  ctx.check(record_a.incarnation.valid() && record_b.incarnation.valid() &&
                record_c.incarnation.valid(),
            "every cluster holds a distinct incarnation");
  ctx.check(record_a.incarnation != record_b.incarnation,
            "cluster incarnations are distinct per process");
  ctx.check(record_a.live_authority && record_b.live_authority && record_c.live_authority,
            "every cluster holds live authority after registration");
  ctx.check(record_a.evidence.present && record_b.evidence.present,
            "every cluster published current evidence");
  ctx.check(record_a.worker_boot_id == deployment.agents[0]->worker_boot,
            "the coordinator recorded the agent's worker boot identity");

  ctx.note("phase PUBLISH: creating state S generation 1 in cluster A");
  const StateId state = StateId::from_value(8801);
  const StateGeneration generation = StateGeneration::from_value(1);
  const std::vector<std::byte> payload = pattern_bytes(kPayloadBytes);
  const Digest digest = sha256(ByteSpan(payload.data(), payload.size()));
  auto created = deployment.client->register_state(
      make_spec(state, generation, kPayloadBytes, digest, CompatibilityDescriptor{}, 2));
  ctx.check(created.has_value(), "the state generation is registered through the coordinator");
  auto committed_state = deployment.client->commit_state(
      state, generation, CommitId::from_value(5001));
  ctx.check(committed_state.has_value(), "the state generation is committed");
  ctx.check(committed_state.value().lifecycle == StateGenerationLifecycle::CURRENT,
            "the generation becomes current");

  ctx.note("phase HOST: moving real bytes into cluster agent A");
  const ReplicaId replica_a = ReplicaId::from_value(9101);
  auto hosted = host_payload(*deployment.agents[0], state, generation, replica_a,
                             ReplicaGeneration::from_value(1), payload);
  ctx.check(hosted.has_value(), "cluster agent A accepts the payload over TCP");
  ctx.check(hosted.value() == digest, "the agent's measured digest matches the controller's");
  ctx.check(deployment.agents[0]->process->running(), "cluster agent A is still running");

  auto established = establish_replica(*deployment.client, state, generation, record_a,
                                       committed_state.value(), replica_a, digest);
  ctx.check(established.has_value(), "cluster A holds a committed authoritative replica");
  auto authoritative_before =
      deployment.client->reconcile_replica_set(state, generation);
  ctx.check(authoritative_before.value().replica_set.authoritative_count == 1,
            "the replica set starts with exactly one authoritative replica");
  const ReplicaSetGeneration first_generation =
      authoritative_before.value().replica_set.generation;

  ctx.note("phase PLAN and RESERVE: authorizing replication from A to B");
  ReplicationRequest request;
  request.state_id = state;
  request.state_generation = generation;
  request.destination_cluster = cluster_b;
  request.expected_coordinator_epoch = deployment.client->status().value().coordinator_epoch;
  auto authorization = deployment.client->authorize_replication(request);
  ctx.check(authorization.has_value(), "the coordinator authorizes replication to cluster B");
  const TransferAuthorization auth = authorization.value();
  ctx.check(auth.expected_digest == digest, "the authorization pins the content digest");
  ctx.check(auth.transfer_bytes.known() && auth.transfer_bytes.bytes == kPayloadBytes,
            "the authorization pins the exact byte count");

  ctx.note("phase TRANSFER and VERIFY: the destination pulls real bytes from the source");
  auto prepared = deployment.client->await_operation(auth.operation_id);
  ctx.check(prepared.has_value(), "the transfer reaches a stable state");
  ctx.check(prepared.value().destination_verified,
            "the destination verified the transferred bytes");
  ctx.check(prepared.value().state == OperationState::PREPARED,
            "the destination is prepared but not yet authoritative");

  auto replica_b_record = deployment.client->query_replica(auth.destination_replica);
  ctx.check(replica_b_record.has_value(), "the destination replica record exists");
  ctx.check(replica_b_record.value().measured_digest == digest,
            "the destination independently measured the same digest");
  ctx.check(replica_b_record.value().transferred_bytes.bytes == kPayloadBytes,
            "the exact byte count crossed the process boundary");
  ctx.check(replica_b_record.value().location.cluster_incarnation == record_b.incarnation,
            "the destination replica is bound to cluster B's live incarnation");

  const auto count_before_commit = deployment.client->reconcile_replica_set(state, generation);
  ctx.check(count_before_commit.value().replica_set.authoritative_count == 1,
            "a transferred but uncommitted copy does not count toward the replication factor");

  ctx.note("phase COMMIT: committing the destination into the replica set");
  OperationCommit commit;
  commit.operation_id = auth.operation_id;
  commit.operation_generation = auth.operation_generation;
  commit.commit_id = CommitId::from_value(5002);
  commit.expected_coordinator_epoch = deployment.client->status().value().coordinator_epoch;
  auto committed = deployment.client->commit_operation(commit);
  ctx.check(committed.has_value(), "the destination commits transactionally");
  ctx.check(committed.value().state == OperationState::COMPLETE, "the operation completes");

  const auto count_after_commit = deployment.client->reconcile_replica_set(state, generation);
  ctx.check(count_after_commit.value().replica_set.authoritative_count == 2,
            "the committed destination now counts toward the replication factor");
  ctx.check(count_after_commit.value().replica_set.generation > first_generation,
            "the replica set generation advanced");
  ctx.check(count_after_commit.value().replica_set.distinct_clusters == 2,
            "the replica set spans two clusters");

  auto reuse_b = deployment.client->evaluate_reuse(state, generation, cluster_b);
  ctx.check(reuse_b.has_value() && reuse_b.value().assessment.authoritative_for_reuse,
            "cluster B may now serve reuse");

  ctx.note("phase KILL: killing cluster agent B while it hosts a current replica");
  ctx.check(deployment.agents[1]->process->kill().has_value(), "cluster agent B is killed hard");
  const int exit_code = deployment.agents[1]->process->wait().value_or(-1);
  ctx.check(exit_code != 0, "the killed agent did not exit cleanly");

  auto fenced = deployment.client->mark_cluster_unreachable(cluster_b, record_b.incarnation,
                                                            "agent process died");
  ctx.check(fenced.has_value(), "the coordinator marks cluster B unreachable");
  ctx.check(!fenced.value().live_authority, "cluster B no longer holds live authority");

  auto fenced_replica = deployment.client->query_replica(auth.destination_replica);
  ctx.check(fenced_replica.value().lifecycle == ReplicaLifecycle::STALE ||
                fenced_replica.value().lifecycle == ReplicaLifecycle::REVALIDATION_REQUIRED,
            "the B-hosted replica is fenced");
  ctx.check(!fenced_replica.value().counts_toward_factor,
            "the fenced replica no longer counts toward the replication factor");
  ctx.check(fenced_replica.value().commit_recorded,
            "the committed history of the B-hosted replica is preserved");

  auto reuse_after_death = deployment.client->evaluate_reuse(state, generation, cluster_b);
  ctx.check(reuse_after_death.has_value() &&
                reuse_after_death.value().decision == DecisionKind::REJECT,
            "cluster B may not serve reuse after its incarnation died");

  auto reuse_a = deployment.client->evaluate_reuse(state, generation, cluster_a);
  ctx.check(reuse_a.has_value() && reuse_a.value().assessment.authoritative_for_reuse,
            "cluster A remains authoritative");

  ctx.note("phase STALE: rejecting traffic from the dead incarnation");
  auto retired = deployment.client->retire_cluster(cluster_b, incarnation_b);
  ctx.check(retired.has_value(), "the dead incarnation can be retired explicitly");
  auto already_retired = deployment.client->retire_cluster(cluster_b, incarnation_b);
  ctx.check(already_retired.has_value(),
            "retiring the same incarnation twice is idempotent");
  const ClusterIncarnationId unknown_incarnation =
      ClusterIncarnationId::from_value(incarnation_b.value() + 4242U);
  auto stale_claim = deployment.client->retire_cluster(cluster_b, unknown_incarnation);
  ctx.check(!stale_claim.has_value(), "a claim from an unknown incarnation is rejected");
  ctx.check(stale_claim.error().code == ErrorCode::STALE_CLUSTER_INCARNATION,
            "a stale incarnation claim is rejected as STALE_CLUSTER_INCARNATION");
  auto retired_history = deployment.client->query_replica(auth.destination_replica);
  ctx.check(retired_history.value().commit_recorded,
            "retiring an incarnation preserves committed replica history");

  ctx.note("phase RESTART: reincarnating cluster B as B prime");
  auto agent_b_prime =
      start_agent(deployment.coordinator_port, cluster_b.value(), 2, 2, "gpu:sm_120");
  ctx.check(agent_b_prime.has_value(), "cluster B prime registers with a fresh incarnation");
  ctx.check(agent_b_prime.value()->incarnation != incarnation_b,
            "the restarted agent mints a different cluster incarnation");
  ctx.check(agent_b_prime.value()->worker_boot != boot_b,
            "the restarted agent mints a different worker boot identity");
  deployment.agents[1] = std::move(agent_b_prime.value());

  auto reincarnated = deployment.client->list_clusters();
  ClusterRecord record_b_prime;
  for (const ClusterRecord& record : reincarnated.value()) {
    if (record.cluster_id == cluster_b) {
      record_b_prime = record;
    }
  }
  ctx.check(record_b_prime.incarnation != incarnation_b,
            "the coordinator advanced the cluster incarnation");
  ctx.check(record_b_prime.reincarnation_count >= 1,
            "the reincarnation is recorded on the cluster record");

  auto inherited = deployment.client->query_replica(auth.destination_replica);
  ctx.check(inherited.value().location.cluster_incarnation == incarnation_b,
            "the durable replica metadata still records the incarnation that wrote it");
  ctx.check(!inherited.value().counts_toward_factor,
            "B prime does not inherit the authoritative status of its predecessor");
  auto reuse_prime = deployment.client->evaluate_reuse(state, generation, cluster_b);
  ctx.check(reuse_prime.value().decision == DecisionKind::REJECT,
            "B prime may not serve reuse before revalidation");

  ctx.note("phase REVALIDATE: restoring authority only after explicit revalidation");
  ReplicaTransition revalidate;
  revalidate.replica_id = auth.destination_replica;
  revalidate.expected_coordinator_epoch = deployment.client->status().value().coordinator_epoch;
  auto revalidated = deployment.client->revalidate_replica(revalidate);
  ctx.check(revalidated.has_value(), "the B-hosted replica can be revalidated explicitly");
  ctx.check(revalidated.value().location.cluster_incarnation == record_b_prime.incarnation,
            "revalidation rebinds the replica to the current incarnation");
  ctx.check(revalidated.value().evidence.revalidation_required == false,
            "the replica has fresh evidence after revalidation");

  auto reuse_restored = deployment.client->evaluate_reuse(state, generation, cluster_b);
  ctx.check(reuse_restored.value().decision != DecisionKind::REJECT,
            "cluster B serves reuse again only after revalidation");
  const auto count_after_revalidation =
      deployment.client->reconcile_replica_set(state, generation);
  ctx.check(count_after_revalidation.value().replica_set.authoritative_count == 2,
            "the replica set returns to a valid two-replica state");

  ctx.note("phase SHUTDOWN: the controller stops the coordinator");
  auto stopped = deployment.client->shutdown_coordinator();
  ctx.check(stopped.has_value(), "the coordinator accepts a shutdown request");
  const int coordinator_exit = deployment.coordinator->wait().value_or(-1);
  ctx.check(coordinator_exit == 0, "the coordinator exits cleanly");
  for (std::unique_ptr<AgentHandle>& handle : deployment.agents) {
    if (handle->process->running()) {
      (void)handle->process->kill();
    }
    (void)handle->process->wait();
  }
}

CCSF_CASE(multiprocess, coordinator_crash_and_durable_recovery) {
  ctx.check(!coordinator_executable().empty(), "the coordinator executable must be built");
  ctx.check(!agent_executable().empty(), "the cluster agent executable must be built");

  const std::filesystem::path directory =
      std::filesystem::absolute(std::filesystem::path(fresh_directory("multiprocess-crash")));
  const ClusterId cluster_a = ClusterId::from_value(4201);
  const ClusterId cluster_b = ClusterId::from_value(4202);
  const StateId state = StateId::from_value(8802);
  const StateGeneration generation = StateGeneration::from_value(1);
  const std::vector<std::byte> payload = pattern_bytes(kPayloadBytes);
  const Digest digest = sha256(ByteSpan(payload.data(), payload.size()));

  CoordinatorEpoch first_epoch;
  ReplicaId replica_a = ReplicaId::from_value(9201);
  OperationId in_flight;
  ClusterIncarnationId incarnation_b;

  ctx.note("phase SETUP: establishing committed replicas and one in-flight transfer");
  {
    Deployment deployment;
    auto coordinator = start_coordinator(directory.string(), 78, 2);
    ctx.check(coordinator.has_value(), "the coordinator process starts");
    deployment.coordinator = std::move(coordinator.value().first);
    deployment.coordinator_port = coordinator.value().second;
    auto client = CoordinatorClient::connect("127.0.0.1", deployment.coordinator_port);
    ctx.check(client.has_value(), "the controller connects");
    deployment.client = std::move(client.value());

    auto agent_a = start_agent(deployment.coordinator_port, cluster_a.value(), 1, 1, "gpu:sm_120");
    auto agent_b = start_agent(deployment.coordinator_port, cluster_b.value(), 2, 2, "gpu:sm_120");
    ctx.check(agent_a.has_value() && agent_b.has_value(), "both agents register");
    incarnation_b = agent_b.value()->incarnation;
    deployment.agents.push_back(std::move(agent_a.value()));
    deployment.agents.push_back(std::move(agent_b.value()));

    auto clusters = deployment.client->list_clusters();
    ClusterRecord record_a;
    for (const ClusterRecord& record : clusters.value()) {
      if (record.cluster_id == cluster_a) {
        record_a = record;
      }
    }
    first_epoch = deployment.client->status().value().coordinator_epoch;
    auto created = deployment.client->register_state(
        make_spec(state, generation, kPayloadBytes, digest, CompatibilityDescriptor{}, 2));
    ctx.check(created.has_value(), "state registered");
    auto committed_state =
        deployment.client->commit_state(state, generation, CommitId::from_value(5101));
    ctx.check(committed_state.has_value(), "state committed");

    auto hosted = host_payload(*deployment.agents[0], state, generation, replica_a,
                               ReplicaGeneration::from_value(1), payload);
    ctx.check(hosted.has_value(), "cluster A hosts the bytes");
    ctx.check(establish_replica(*deployment.client, state, generation, record_a,
                                committed_state.value(), replica_a, digest)
                  .has_value(),
              "cluster A holds a committed authoritative replica");

    ReplicationRequest request;
    request.state_id = state;
    request.state_generation = generation;
    request.destination_cluster = cluster_b;
    request.expected_coordinator_epoch = first_epoch;
    auto authorization = deployment.client->authorize_replication(request);
    ctx.check(authorization.has_value(), "a replication is authorized");
    in_flight = authorization.value().operation_id;
    auto prepared = deployment.client->await_operation(in_flight);
    ctx.check(prepared.has_value(), "the transfer completes at the destination");
    ctx.check(prepared.value().state == OperationState::PREPARED,
              "the operation is prepared and not yet committed");

    ctx.note("phase KILL: killing the coordinator without graceful shutdown");
    ctx.check(deployment.coordinator->kill().has_value(), "the coordinator is killed hard");
    const int exit_code = deployment.coordinator->wait().value_or(0);
    ctx.check(exit_code != 0, "the killed coordinator did not exit cleanly");
    for (std::unique_ptr<AgentHandle>& handle : deployment.agents) {
      if (handle->process->running()) {
        (void)handle->process->kill();
      }
      (void)handle->process->wait();
    }
  }

  ctx.note("phase RESTART: starting a fresh coordinator against the durable directory");
  {
    Deployment deployment;
    auto coordinator = start_coordinator(directory.string(), 78, 2);
    ctx.check(coordinator.has_value(), "the coordinator restarts from the durable directory");
    deployment.coordinator = std::move(coordinator.value().first);
    deployment.coordinator_port = coordinator.value().second;
    auto client = CoordinatorClient::connect("127.0.0.1", deployment.coordinator_port);
    ctx.check(client.has_value(), "the controller reconnects");
    deployment.client = std::move(client.value());

    const FabricStatus status = deployment.client->status().value();
    ctx.check(status.recovered_from_durable_state, "the coordinator recovered durable state");
    ctx.check(status.coordinator_epoch > first_epoch,
              "the coordinator epoch advanced across the crash");

    auto recovered_state = deployment.client->query_state(state, generation);
    ctx.check(recovered_state.has_value(), "the committed state generation survived the crash");
    ctx.check(recovered_state.value().lifecycle == StateGenerationLifecycle::CURRENT,
              "the generation is still current");
    ctx.check(recovered_state.value().content_digest == digest, "the content identity survived");

    auto reconciliation = deployment.client->reconcile_replica_set(state, generation);
    ctx.check(reconciliation.has_value(), "the replica set survived the crash");
    ctx.check(reconciliation.value().replica_set.authoritative_count == 1,
              "the committed authoritative replica count survived the crash");

    ctx.note("phase REVALIDATE: old-epoch traffic and ambiguous work are fenced");
    auto operation = deployment.client->query_operation(in_flight);
    ctx.check(operation.has_value(), "the in-flight operation survived as history");
    ctx.check(operation.value().state == OperationState::OUTCOME_UNKNOWN,
              "an ambiguous in-flight operation is classified as OUTCOME_UNKNOWN");
    ctx.check(!operation.value().destination_committed,
              "the ambiguous operation never claims a committed destination");
    ctx.check(operation.value().recovery_required,
              "the ambiguous operation is flagged as requiring recovery");

    auto ambiguous_replica = deployment.client->list_replicas(state, generation);
    ctx.check(ambiguous_replica.has_value() && ambiguous_replica.value().size() == 2,
              "both replica records survived");
    bool saw_quarantine = false;
    for (const ReplicaRecord& record : ambiguous_replica.value()) {
      if (record.lifecycle == ReplicaLifecycle::QUARANTINED) {
        saw_quarantine = true;
      }
    }
    ctx.check(saw_quarantine,
              "the ambiguous destination copy is quarantined rather than trusted");

    auto clusters = deployment.client->list_clusters();
    bool saw_revalidation = false;
    for (const ClusterRecord& record : clusters.value()) {
      if (record.evidence.revalidation_required) {
        saw_revalidation = true;
      }
    }
    ctx.check(saw_revalidation, "recovered dynamic evidence requires revalidation");

    ctx.note("phase RECONNECT: agents re-register against the new epoch");
    auto agent_a = start_agent(deployment.coordinator_port, cluster_a.value(), 1, 1, "gpu:sm_120");
    auto agent_b = start_agent(deployment.coordinator_port, cluster_b.value(), 2, 2, "gpu:sm_120");
    ctx.check(agent_a.has_value() && agent_b.has_value(), "the agents reconnect");
    deployment.agents.push_back(std::move(agent_a.value()));
    deployment.agents.push_back(std::move(agent_b.value()));
    ctx.check(deployment.agents[0]->incarnation != incarnation_b,
              "the reconnecting agents hold fresh incarnations");

    auto clusters_after = deployment.client->list_clusters();
    ClusterRecord record_b_after;
    for (const ClusterRecord& record : clusters_after.value()) {
      if (record.cluster_id == cluster_b) {
        record_b_after = record;
      }
    }
    ctx.check(!record_b_after.evidence.revalidation_required,
              "a cluster that re-publishes evidence is current again");

    ctx.note("phase REVALIDATE: restoring authority for the committed replica");
    // The restarted agent's local store is empty: durable bytes must be restored
    // to it before it can serve as a replication source again.
    auto restored = host_payload(*deployment.agents[0], state, generation, replica_a,
                                 ReplicaGeneration::from_value(1), payload);
    ctx.check(restored.has_value(), "the restarted source agent accepts the durable payload");
    ctx.check(restored.value() == digest, "the restored payload matches the recorded digest");
    ReplicaTransition revalidate_a;
    revalidate_a.replica_id = replica_a;
    revalidate_a.expected_coordinator_epoch =
        deployment.client->status().value().coordinator_epoch;
    auto revalidated_a = deployment.client->revalidate_replica(revalidate_a);
    ctx.check(revalidated_a.has_value(),
              std::string("the committed replica is revalidated under the new epoch: ") +
                  (revalidated_a ? std::string("ok")
                                 : revalidated_a.error().to_string()));
    auto reuse_a = deployment.client->evaluate_reuse(state, generation, cluster_a);
    ctx.check(reuse_a.has_value() && reuse_a.value().assessment.authoritative_for_reuse,
              "cluster A is authoritative again only after revalidation");
    auto still_one = deployment.client->reconcile_replica_set(state, generation);
    ctx.check(still_one.value().replica_set.authoritative_count == 1,
              "the quarantined ambiguous copy still does not count");

    ctx.note("phase COMMIT: the recovered replica set accepts a fresh legal commit");
    ReplicaId replica_b = ReplicaId::from_value(9202);
    auto hosted = host_payload(*deployment.agents[1], state, generation, replica_b,
                               ReplicaGeneration::from_value(1), payload);
    ctx.check(hosted.has_value(), "cluster B prime accepts the payload");
    ReplicationRequest request;
    request.state_id = state;
    request.state_generation = generation;
    request.destination_cluster = cluster_b;
    request.expected_coordinator_epoch = deployment.client->status().value().coordinator_epoch;
    auto authorization = deployment.client->authorize_replication(request);
    ctx.check(authorization.has_value(), "a fresh replication is authorized under the new epoch");
    auto prepared = deployment.client->await_operation(authorization.value().operation_id);
    ctx.check(prepared.has_value(), "the fresh transfer completes");
    OperationCommit commit;
    commit.operation_id = authorization.value().operation_id;
    commit.operation_generation = authorization.value().operation_generation;
    commit.commit_id = CommitId::from_value(5102);
    commit.expected_coordinator_epoch = deployment.client->status().value().coordinator_epoch;
    ctx.check(deployment.client->commit_operation(commit).has_value(),
              "the fresh replication commits");

    auto final_state = deployment.client->reconcile_replica_set(state, generation);
    ctx.check(final_state.value().replica_set.authoritative_count == 2,
              "the replica set returns to a valid two-replica state after revalidation");

    (void)deployment.client->shutdown_coordinator();
    (void)deployment.coordinator->wait();
    for (std::unique_ptr<AgentHandle>& handle : deployment.agents) {
      if (handle->process->running()) {
        (void)handle->process->kill();
      }
      (void)handle->process->wait();
    }
  }
}
