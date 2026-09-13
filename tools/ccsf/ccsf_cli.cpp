// Cross-Cluster State Fabric - inspection CLI.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The CLI never implements its own control logic. Online it talks to a running
// coordinator over the same typed control plane a controller uses; offline it
// opens the durable control plane through the public library API and answers
// the same queries. Mutating commands are refused offline.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "ccsf/coordinator.hpp"
#include "ccsf/fabric.hpp"

namespace {

using namespace ccsf;

void usage() {
  std::printf(
      "ccsf <command> [arguments]\n"
      "\n"
      "Connection:\n"
      "  --connect <host:port>      query a running coordinator (default 127.0.0.1:7700)\n"
      "  --persistence <dir>        inspect a durable control plane offline\n"
      "\n"
      "Commands:\n"
      "  status\n"
      "  clusters\n"
      "  states\n"
      "  state <state-id> [generation]\n"
      "  replicas <state-id> <generation>\n"
      "  replica <replica-id>\n"
      "  reuse <state-id> <generation> <cluster-id>\n"
      "  reconcile <state-id> <generation>\n"
      "  plan-replication <state-id> <generation>\n"
      "  operations\n"
      "  operation <operation-id>\n"
      "  snapshot-info\n");
}

std::string join(const std::vector<std::string>& values, std::size_t from) {
  std::string text;
  for (std::size_t index = from; index < values.size(); ++index) {
    if (!text.empty()) {
      text += ' ';
    }
    text += values[index];
  }
  return text;
}

std::uint64_t parse_u64(const std::string& text) {
  return std::strtoull(text.c_str(), nullptr, 10);
}

/// Offline reader: everything goes through the public library API.
class OfflineReader {
 public:
  explicit OfflineReader(const std::string& directory) {
    FabricConfig config;
    config.coordinator_id = CoordinatorId::from_value(1);
    config.persistence_directory = directory;
    config.require_durable_commits = true;
    auto opened = Fabric::open(config);
    if (!opened) {
      error_ = opened.error().to_string();
      return;
    }
    fabric_ = std::move(opened.value());
  }

  [[nodiscard]] bool ok() const { return fabric_ != nullptr; }
  [[nodiscard]] const std::string& error() const { return error_; }
  Fabric& fabric() { return *fabric_; }

 private:
  std::unique_ptr<Fabric> fabric_;
  std::string error_;
};

int report(const Error& error) {
  std::fprintf(stderr, "error: %s\n", error.to_string().c_str());
  return 1;
}

void print_state(const StateGenerationRecord& record) {
  std::printf("%s g%s class=%s consistency=%s lifecycle=%s size=%s digest=%s commit=%s\n",
              record.state_id.str().c_str(), record.generation.str().c_str(),
              to_string(record.state_class), to_string(record.consistency),
              to_string(record.lifecycle), describe(record.logical_size).c_str(),
              record.content_digest.valid() ? record.content_digest.hex().c_str() : "UNKNOWN",
              record.commit.valid() ? record.commit.str().c_str() : "none");
}

void print_replica(const ReplicaRecord& record) {
  std::printf(
      "replica %s g%s state=%s g%s cluster=%s incarnation=%s lifecycle=%s integrity=%s "
      "compatibility=%s authority=%s counts=%s digest=%s\n",
      record.replica_id.str().c_str(), record.replica_generation.str().c_str(),
      record.state_id.str().c_str(), record.state_generation.str().c_str(),
      record.location.cluster_id.str().c_str(),
      record.location.cluster_incarnation.str().c_str(), to_string(record.lifecycle),
      to_string(record.integrity), to_string(record.compatibility), to_string(record.authority),
      record.counts_toward_factor ? "yes" : "no",
      record.measured_digest.valid() ? record.measured_digest.hex().c_str() : "UNKNOWN");
}

void print_operation(const OperationRecord& record) {
  std::printf("operation %s g%s kind=%s state=%s state-gen=%s dest-cluster=%s verified=%s "
              "committed=%s source-retired=%s\n",
              record.operation_id.str().c_str(), record.operation_generation.str().c_str(),
              to_string(record.kind), to_string(record.state),
              state_generation_label(record.state_id, record.state_generation).c_str(),
              record.destination_cluster.str().c_str(), record.destination_verified ? "yes" : "no",
              record.destination_committed ? "yes" : "no", record.source_retired ? "yes" : "no");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace ccsf;
  std::vector<std::string> arguments;
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  if (arguments.empty() || arguments.front() == "--help" || arguments.front() == "help") {
    usage();
    return arguments.empty() ? 2 : 0;
  }

  std::string connect = "127.0.0.1:7700";
  std::string persistence;
  std::vector<std::string> rest;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (arguments[index] == "--connect" && index + 1U < arguments.size()) {
      connect = arguments[++index];
    } else if (arguments[index] == "--persistence" && index + 1U < arguments.size()) {
      persistence = arguments[++index];
    } else {
      rest.push_back(arguments[index]);
    }
  }
  if (rest.empty()) {
    usage();
    return 2;
  }
  const std::string command = rest.front();

  std::unique_ptr<CoordinatorClient> client;
  std::unique_ptr<OfflineReader> offline;
  if (persistence.empty()) {
    const std::size_t separator = connect.find(':');
    const std::string host = separator == std::string::npos ? connect : connect.substr(0, separator);
    const std::uint16_t port = static_cast<std::uint16_t>(
        std::strtoul((separator == std::string::npos ? "7700" : connect.substr(separator + 1U))
                         .c_str(),
                     nullptr, 10));
    auto connected = CoordinatorClient::connect(host, port);
    if (!connected) {
      return report(connected.error());
    }
    client = std::move(connected.value());
  } else {
    offline = std::make_unique<OfflineReader>(persistence);
    if (!offline->ok()) {
      std::fprintf(stderr, "error: %s\n", offline->error().c_str());
      return 1;
    }
  }

  if (command == "status") {
    if (client) {
      auto status = client->status();
      if (!status) {
        return report(status.error());
      }
      const FabricStatus& value = status.value();
      std::printf("coordinator=%s epoch=%s tick=%llu persistence=%s healthy=%s recovered=%s\n",
                  value.coordinator_id.str().c_str(), value.coordinator_epoch.str().c_str(),
                  static_cast<unsigned long long>(value.logical_time),
                  value.persistence_enabled ? "enabled" : "disabled",
                  value.persistence_healthy ? "yes" : "no",
                  value.recovered_from_durable_state ? "yes" : "no");
      std::printf("states=%zu clusters=%zu replicas=%zu replica-sets=%zu operations=%zu\n",
                  value.state_generations, value.clusters, value.replicas, value.replica_sets,
                  value.operations);
      std::printf("commits=%llu committed-replicas=%llu retired=%llu quarantined=%llu "
                  "integrity-failures=%llu active-operations=%llu reserved-bytes=%llu\n",
                  static_cast<unsigned long long>(value.counters.durable_commits),
                  static_cast<unsigned long long>(value.counters.replicas_committed),
                  static_cast<unsigned long long>(value.counters.replicas_retired),
                  static_cast<unsigned long long>(value.counters.replicas_quarantined),
                  static_cast<unsigned long long>(value.counters.integrity_failures),
                  static_cast<unsigned long long>(value.counters.active_operations),
                  static_cast<unsigned long long>(value.counters.reserved_bytes));
      if (!value.persistence_detail.empty()) {
        std::printf("persistence-detail=%s\n", value.persistence_detail.c_str());
      }
    } else {
      const FabricStatus value = offline->fabric().status();
      std::printf("offline coordinator=%s epoch=%s tick=%llu states=%zu replicas=%zu\n",
                  value.coordinator_id.str().c_str(), value.coordinator_epoch.str().c_str(),
                  static_cast<unsigned long long>(value.logical_time), value.state_generations,
                  value.replicas);
    }
    return 0;
  }

  if (command == "clusters") {
    std::vector<ClusterRecord> clusters;
    if (client) {
      auto listed = client->list_clusters();
      if (!listed) {
        return report(listed.error());
      }
      clusters = listed.value();
    } else {
      auto listed = offline->fabric().list_clusters();
      if (!listed) {
        return report(listed.error());
      }
      clusters = listed.value();
    }
    for (const ClusterRecord& cluster : clusters) {
      std::printf("cluster %s incarnation=%s region=%s site=%s domain=%s lifecycle=%s health=%s "
                  "live=%s evidence=%s revalidation=%s hosted=%u reincarnations=%u\n",
                  cluster.cluster_id.str().c_str(), cluster.incarnation.str().c_str(),
                  cluster.location.region.str().c_str(), cluster.location.site.str().c_str(),
                  cluster.location.failure_domain.str().c_str(), to_string(cluster.lifecycle),
                  to_string(cluster.health), cluster.live_authority ? "yes" : "no",
                  cluster.evidence.present ? "present" : "absent",
                  cluster.evidence.revalidation_required ? "required" : "current",
                  cluster.hosted_replica_count, cluster.reincarnation_count);
    }
    return 0;
  }

  if (command == "states") {
    std::vector<StateGenerationRecord> states;
    if (offline) {
      auto listed = offline->fabric().list_states();
      if (!listed) {
        return report(listed.error());
      }
      states = listed.value();
    } else {
      return report(make_error(ErrorCode::NOT_SUPPORTED,
                               "listing every state requires --persistence"));
    }
    for (const StateGenerationRecord& state : states) {
      print_state(state);
    }
    return 0;
  }

  if (command == "state") {
    if (rest.size() < 2U) {
      usage();
      return 2;
    }
    const StateId state = StateId::from_value(parse_u64(rest[1]));
    const StateGeneration generation =
        rest.size() > 2U ? StateGeneration::from_value(parse_u64(rest[2])) : StateGeneration{};
    if (client) {
      if (generation.valid()) {
        auto record = client->query_state(state, generation);
        if (!record) {
          return report(record.error());
        }
        print_state(record.value());
        return 0;
      }
      return report(make_error(ErrorCode::INVALID_ARGUMENT,
                               "a generation is required for online queries"));
    }
    auto record = generation.valid() ? offline->fabric().query_state(state, generation)
                                     : offline->fabric().current_state(state);
    if (!record) {
      return report(record.error());
    }
    print_state(record.value());
    return 0;
  }

  if (command == "replicas") {
    if (rest.size() < 3U) {
      usage();
      return 2;
    }
    const StateId state = StateId::from_value(parse_u64(rest[1]));
    const StateGeneration generation = StateGeneration::from_value(parse_u64(rest[2]));
    std::vector<ReplicaRecord> replicas;
    if (client) {
      auto listed = client->list_replicas(state, generation);
      if (!listed) {
        return report(listed.error());
      }
      replicas = listed.value();
    } else {
      auto listed = offline->fabric().list_replicas(state, generation);
      if (!listed) {
        return report(listed.error());
      }
      replicas = listed.value();
    }
    for (const ReplicaRecord& replica : replicas) {
      print_replica(replica);
    }
    return 0;
  }

  if (command == "replica") {
    if (rest.size() < 2U) {
      usage();
      return 2;
    }
    const ReplicaId replica = ReplicaId::from_value(parse_u64(rest[1]));
    auto record = client ? client->query_replica(replica)
                         : offline->fabric().query_replica(replica);
    if (!record) {
      return report(record.error());
    }
    print_replica(record.value());
    return 0;
  }

  if (command == "reuse") {
    if (rest.size() < 4U) {
      usage();
      return 2;
    }
    const StateId state = StateId::from_value(parse_u64(rest[1]));
    const StateGeneration generation = StateGeneration::from_value(parse_u64(rest[2]));
    const ClusterId cluster = ClusterId::from_value(parse_u64(rest[3]));
    auto evaluation = client ? client->evaluate_reuse(state, generation, cluster)
                             : offline->fabric().evaluate_reuse(state, generation, cluster);
    if (!evaluation) {
      return report(evaluation.error());
    }
    std::printf("decision=%s code=%s replica=%s reason=%s\n",
                to_string(evaluation.value().decision), to_string(evaluation.value().code),
                evaluation.value().replica.valid() ? evaluation.value().replica.str().c_str()
                                                   : "none",
                evaluation.value().reason.c_str());
    for (const std::string& finding : evaluation.value().findings) {
      std::printf("  %s\n", finding.c_str());
    }
    return 0;
  }

  if (command == "reconcile") {
    if (rest.size() < 3U) {
      usage();
      return 2;
    }
    const StateId state = StateId::from_value(parse_u64(rest[1]));
    const StateGeneration generation = StateGeneration::from_value(parse_u64(rest[2]));
    auto reconciliation = client ? client->reconcile_replica_set(state, generation)
                                 : offline->fabric().reconcile_replica_set(state, generation);
    if (!reconciliation) {
      return report(reconciliation.error());
    }
    const ReplicaSetReconciliation& value = reconciliation.value();
    std::printf("set=%s g%s state=%s authoritative=%u degraded=%u clusters=%u regions=%u "
                "under=%s over=%s revalidation=%s\n",
                value.replica_set.replica_set_id.str().c_str(),
                value.replica_set.generation.str().c_str(), to_string(value.replica_set.state),
                value.replica_set.authoritative_count, value.replica_set.degraded_count,
                value.replica_set.distinct_clusters, value.replica_set.distinct_regions,
                value.under_replicated ? "yes" : "no", value.over_replicated ? "yes" : "no",
                value.replica_set.revalidation_required ? "required" : "current");
    for (const std::string& finding : value.findings) {
      std::printf("  %s\n", finding.c_str());
    }
    return 0;
  }

  if (command == "plan-replication") {
    if (rest.size() < 3U) {
      usage();
      return 2;
    }
    if (!client) {
      return report(make_error(ErrorCode::NOT_SUPPORTED,
                               "planning requires a coordinator: use --connect"));
    }
    ReplicationRequest request;
    request.state_id = StateId::from_value(parse_u64(rest[1]));
    request.state_generation = StateGeneration::from_value(parse_u64(rest[2]));
    auto plan = client->plan_replication(request);
    if (!plan) {
      return report(plan.error());
    }
    std::printf("decision=%s destination=%s score=%lld\n", to_string(plan.value().decision),
                plan.value().chosen_cluster.valid() ? plan.value().chosen_cluster.str().c_str()
                                                    : "none",
                static_cast<long long>(plan.value().chosen_score));
    for (const std::string& reason : plan.value().reasons) {
      std::printf("  %s\n", reason.c_str());
    }
    return 0;
  }

  if (command == "operations") {
    if (!client) {
      return report(make_error(ErrorCode::NOT_SUPPORTED,
                               "operation history requires a coordinator: use --connect"));
    }
    auto operations = client->list_operations();
    if (!operations) {
      return report(operations.error());
    }
    for (const OperationRecord& operation : operations.value()) {
      print_operation(operation);
    }
    return 0;
  }

  if (command == "operation") {
    if (rest.size() < 2U) {
      usage();
      return 2;
    }
    if (!client) {
      return report(make_error(ErrorCode::NOT_SUPPORTED,
                               "operation history requires a coordinator: use --connect"));
    }
    auto record = client->query_operation(OperationId::from_value(parse_u64(rest[1])));
    if (!record) {
      return report(record.error());
    }
    print_operation(record.value());
    for (const std::string& line : explain_operation(record.value())) {
      std::printf("  %s\n", line.c_str());
    }
    return 0;
  }

  if (command == "snapshot-info") {
    if (!offline) {
      return report(make_error(ErrorCode::NOT_SUPPORTED,
                               "snapshot inspection requires --persistence"));
    }
    const FabricSnapshot snapshot = offline->fabric().snapshot();
    std::printf("coordinator=%s epoch=%s tick=%llu sequence=%llu replay=%llu\n",
                snapshot.coordinator_id.str().c_str(), snapshot.coordinator_epoch.str().c_str(),
                static_cast<unsigned long long>(snapshot.logical_time),
                static_cast<unsigned long long>(snapshot.durable_sequence),
                static_cast<unsigned long long>(snapshot.replay_generation));
    std::printf("states=%zu clusters=%zu replicas=%zu replica-sets=%zu operations=%zu "
                "ownership=%zu authorizations=%zu\n",
                snapshot.states.size(), snapshot.clusters.size(), snapshot.replicas.size(),
                snapshot.replica_sets.size(), snapshot.operations.size(),
                snapshot.ownership.size(), snapshot.active_authorizations.size());
    for (const ReplicaSetRecord& set : snapshot.replica_sets) {
      std::printf("  %s g%s state=%s authoritative=%u\n",
                  state_generation_label(set.state_id, set.state_generation).c_str(),
                  set.generation.str().c_str(), to_string(set.state), set.authoritative_count);
    }
    return 0;
  }

  std::fprintf(stderr, "unknown command: %s\n", command.c_str());
  std::fprintf(stderr, "arguments: %s\n", join(rest, 0).c_str());
  usage();
  return 2;
}
