// Cross-Cluster State Fabric - property and randomized invariant proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every randomized case is driven by an explicit seed which is printed on
// failure together with the reproduction parameters. Invariants are checked
// after every single operation, never only at the end.

#include <cstdint>
#include <string>
#include <vector>

#include "ccsf/fabric.hpp"
#include "framework.hpp"
#include "harness.hpp"

using namespace ccsf;
using namespace ccsf::test;

namespace {

/// Canonical identity encoding for the reproducibility digest. Identities are
/// written as raw 64-bit values; the runtime's wire format does the same.
template <class Tag>
void encode_id(ByteWriter& writer, const StrongId<Tag>& id) {
  writer.u64(id.value());
}

/// Deterministic xorshift generator. The runtime never uses randomness; this is
/// test input generation only.
class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  std::uint64_t next() {
    state_ ^= state_ << 13U;
    state_ ^= state_ >> 7U;
    state_ ^= state_ << 17U;
    return state_;
  }

  std::uint32_t below(std::uint32_t bound) {
    return bound == 0U ? 0U : static_cast<std::uint32_t>(next() % bound);
  }

  bool coin() { return (next() & 1U) != 0U; }

 private:
  std::uint64_t state_;
};

struct InvariantReport {
  bool ok{true};
  std::string detail;
};

/// Checks the model invariants that must hold at every observable instant.
InvariantReport check_invariants(Fabric& fabric, const FabricSnapshot& snapshot) {
  InvariantReport report;

  // One current non-branched generation per (state, lineage).
  for (const StateGenerationRecord& state : snapshot.states) {
    if (!state.is_current() || state.branch) {
      continue;
    }
    std::uint32_t currents = 0;
    for (const StateGenerationRecord& other : snapshot.states) {
      if (other.state_id == state.state_id && other.lineage == state.lineage &&
          other.is_current() && !other.branch) {
        ++currents;
      }
    }
    if (currents != 1) {
      report.ok = false;
      report.detail = "state " + state.state_id.str() + " has " + std::to_string(currents) +
                      " current non-branched generations";
      return report;
    }
  }

  // Every replica references a registered state generation and cluster.
  for (const ReplicaRecord& replica : snapshot.replicas) {
    if (snapshot.find_state(replica.state_id, replica.state_generation) == nullptr) {
      report.ok = false;
      report.detail = "replica " + replica.replica_id.str() + " references an unknown generation";
      return report;
    }
    if (snapshot.find_cluster(replica.location.cluster_id) == nullptr) {
      report.ok = false;
      report.detail = "replica " + replica.replica_id.str() + " references an unknown cluster";
      return report;
    }
    if (replica_counts_toward_replication_factor(replica)) {
      if (replica.integrity != ReplicaIntegrityState::VERIFIED) {
        report.ok = false;
        report.detail = "replica " + replica.replica_id.str() + " counts without verified integrity";
        return report;
      }
      if (replica.compatibility != ReplicaCompatibilityState::COMPATIBLE) {
        report.ok = false;
        report.detail =
            "replica " + replica.replica_id.str() + " counts without verified compatibility";
        return report;
      }
      if (!replica.commit_recorded) {
        report.ok = false;
        report.detail = "replica " + replica.replica_id.str() + " counts without a commit record";
        return report;
      }
    }
  }

  // Derived membership agrees with the replicas it describes.
  for (const ReplicaSetRecord& set : snapshot.replica_sets) {
    std::uint32_t counted = 0;
    for (const ReplicaRecord& replica : snapshot.replicas) {
      if (replica.state_id == set.state_id && replica.state_generation == set.state_generation &&
          replica_counts_toward_replication_factor(replica)) {
        ++counted;
      }
    }
    if (counted != set.authoritative_count) {
      report.ok = false;
      report.detail = "replica set " + set.replica_set_id.str() + " reports " +
                      std::to_string(set.authoritative_count) + " authoritative replicas but " +
                      std::to_string(counted) + " replicas count";
      return report;
    }
    for (const ReplicaSetMember& member : set.authoritative_members) {
      if (snapshot.find_replica(member.replica_id) == nullptr) {
        report.ok = false;
        report.detail = "replica set member " + member.replica_id.str() + " does not exist";
        return report;
      }
    }
  }

  // Active operation accounting agrees with the operation records.
  std::uint64_t active = 0;
  for (const OperationRecord& operation : snapshot.operations) {
    if (!operation.is_terminal()) {
      ++active;
    }
  }
  if (active != snapshot.counters.active_operations) {
    report.ok = false;
    report.detail = "active operation accounting drifted: " + std::to_string(active) +
                    " records versus " + std::to_string(snapshot.counters.active_operations) +
                    " counted";
    return report;
  }

  // A single-writer generation never has two active owners.
  for (const StateGenerationRecord& state : snapshot.states) {
    if (!requires_single_authoritative_owner(state.consistency)) {
      continue;
    }
    std::uint32_t owners = 0;
    for (const ReplicaRecord& replica : snapshot.replicas) {
      if (replica.state_id == state.state_id &&
          replica.state_generation == state.generation &&
          replica.authority == ReplicaAuthorityState::AUTHORITATIVE_OWNER &&
          !replica.is_terminal()) {
        ++owners;
      }
    }
    if (owners > 1) {
      report.ok = false;
      report.detail = "state " + state.state_id.str() + " has " + std::to_string(owners) +
                      " simultaneous authoritative owners";
      return report;
    }
  }
  (void)fabric;
  return report;
}

/// Canonical digest of the observable control plane, used for reproducibility.
std::string canonical_digest(const FabricSnapshot& snapshot) {
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(snapshot.states.size()));
  for (const StateGenerationRecord& state : snapshot.states) {
    encode_id(writer, state.state_id);
    encode_id(writer, state.generation);
    writer.u16(static_cast<std::uint16_t>(state.lifecycle));
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.replicas.size()));
  for (const ReplicaRecord& replica : snapshot.replicas) {
    encode_id(writer, replica.replica_id);
    encode_id(writer, replica.state_id);
    encode_id(writer, replica.state_generation);
    encode_id(writer, replica.location.cluster_id);
    writer.u16(static_cast<std::uint16_t>(replica.lifecycle));
    writer.u16(static_cast<std::uint16_t>(replica.authority));
    writer.u16(static_cast<std::uint16_t>(replica.integrity));
    writer.boolean(replica.counts_toward_factor);
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.replica_sets.size()));
  for (const ReplicaSetRecord& set : snapshot.replica_sets) {
    encode_id(writer, set.replica_set_id);
    encode_id(writer, set.generation);
    writer.u32(set.authoritative_count);
    writer.u16(static_cast<std::uint16_t>(set.state));
  }
  return sha256(writer.data()).hex();
}

struct PropertyWorld {
  std::unique_ptr<World> world;
  std::vector<ClusterId> clusters;
  std::vector<StateId> states;
};

/// Runs one randomized sequence and reports the first invariant violation.
InvariantReport run_sequence(std::uint64_t seed, std::uint32_t operations, std::string* digest) {
  Random random(seed);
  auto world = make_world(base_config(CoordinatorId::from_value(700), ClusterId::from_value(100)));
  if (world == nullptr) {
    return InvariantReport{false, "the test world failed to open"};
  }
  std::vector<ClusterId> clusters = {world->cluster_a, world->cluster_b, world->cluster_c};
  std::vector<StateId> states;
  std::vector<std::pair<StateId, StateGeneration>> generations;
  std::uint64_t next_id = 900000;

  for (std::uint32_t step = 0; step < operations; ++step) {
    const std::uint32_t choice = random.below(10U);
    if (choice == 0U) {
      const StateId state = StateId::from_value(800000U + random.below(4U));
      StateGenerationSpec spec;
      spec.state_id = state;
      spec.generation = StateGeneration::from_value(1);
      spec.lineage = LineageId::from_value(state.value());
      spec.logical_size = SizeValue::of(1024);
      spec.content_digest = digest_of("state-" + state.str());
      spec.compatibility_id = CompatibilityId::from_value(state.value() * 3U + 1U);
      spec.compatibility_generation = CompatibilityGeneration::from_value(1);
      spec.replication_requirements =
          make_replication_policy(ReplicationPolicyId::from_value(1),
                                  ReplicationPolicyGeneration::from_value(1),
                                  ReplicationPolicyForm::N_REPLICAS, 2)
              .requirements;
      auto registered = world->fabric->register_state(spec);
      if (registered) {
        states.push_back(state);
        generations.emplace_back(state, spec.generation);
      }
    } else if (choice == 1U && !generations.empty()) {
      const auto target = generations[random.below(static_cast<std::uint32_t>(generations.size()))];
      (void)world->fabric->commit_state_generation(target.first, target.second,
                                                   CommitId::from_value(next_id++));
    } else if (choice == 2U && !generations.empty()) {
      const auto target = generations[random.below(static_cast<std::uint32_t>(generations.size()))];
      const ClusterId cluster = clusters[random.below(static_cast<std::uint32_t>(clusters.size()))];
      auto current = world->fabric->query_state(target.first, target.second);
      if (!current) {
        continue;
      }
      ReplicaRegistration registration;
      registration.state_id = target.first;
      registration.state_generation = target.second;
      registration.location.cluster_id = cluster;
      registration.lifecycle = ReplicaLifecycle::TRANSFERRING;
      registration.content_digest = current.value().content_digest;
      registration.expected_coordinator_epoch = world->epoch();
      auto registered = world->fabric->register_replica(registration);
      if (!registered) {
        continue;
      }
      TransferCompletion completion;
      completion.replica_id = registered.value().replica_id;
      completion.replica_generation = registered.value().replica_generation;
      completion.cluster_id = cluster;
      completion.expected_coordinator_epoch = world->epoch();
      completion.bytes_transferred = current.value().logical_size.bytes;
      completion.measured_digest = current.value().content_digest;
      if (!world->fabric->record_transfer_completion(completion)) {
        continue;
      }
      ReplicaVerification verification;
      verification.replica_id = registered.value().replica_id;
      verification.replica_generation = registered.value().replica_generation;
      verification.cluster_id = cluster;
      verification.expected_coordinator_epoch = world->epoch();
      verification.integrity_verified = true;
      verification.measured_digest = current.value().content_digest;
      if (!world->fabric->verify_replica(verification)) {
        continue;
      }
      ReplicaCommit commit;
      commit.replica_id = registered.value().replica_id;
      commit.commit_id = CommitId::from_value(next_id++);
      commit.expected_coordinator_epoch = world->epoch();
      commit.make_authoritative_owner =
          requires_single_authoritative_owner(current.value().consistency);
      (void)world->fabric->commit_replica(commit);
    } else if (choice == 3U && !generations.empty()) {
      const auto target = generations[random.below(static_cast<std::uint32_t>(generations.size()))];
      ReplicationRequest request;
      request.state_id = target.first;
      request.state_generation = target.second;
      request.expected_coordinator_epoch = world->epoch();
      (void)world->fabric->authorize_replication(request);
    } else if (choice == 4U) {
      const ClusterId cluster = clusters[random.below(static_cast<std::uint32_t>(clusters.size()))];
      auto record = world->fabric->query_cluster(cluster);
      if (record && record.value().live_authority && random.coin()) {
        (void)world->fabric->mark_cluster_unreachable(cluster, record.value().incarnation,
                                                      "randomized partition");
      } else if (record) {
        ClusterEvidenceUpdate update;
        update.cluster_id = cluster;
        update.incarnation = record.value().incarnation;
        update.expected_coordinator_epoch = world->epoch();
        update.has_capabilities = true;
        update.capabilities.tags = {"gpu:sm_120"};
        (void)world->fabric->publish_cluster_evidence(update);
      }
    } else if (choice == 5U) {
      auto operations_list = world->fabric->list_operations();
      if (operations_list) {
        for (const OperationRecord& operation : operations_list.value()) {
          if (operation.state == OperationState::RESERVED ||
              operation.state == OperationState::TRANSFERRING) {
            OperationCancellation cancellation;
            cancellation.operation_id = operation.operation_id;
            cancellation.operation_generation = operation.operation_generation;
            cancellation.expected_coordinator_epoch = world->epoch();
            cancellation.reason = "randomized cancellation";
            (void)world->fabric->cancel_operation(cancellation);
            break;
          }
        }
      }
    } else if (choice == 6U) {
      const ClusterId cluster = clusters[random.below(static_cast<std::uint32_t>(clusters.size()))];
      ClusterRegistration registration = make_cluster_registration(
          world->ids, cluster, world->region_one, FailureDomainId::from_value(cluster.value()),
          WorkerBootId::from_value(next_id++), ClusterHealthState::HEALTHY, {"gpu:sm_120"});
      (void)world->fabric->register_cluster(registration);
    } else if (choice == 7U && !generations.empty()) {
      const auto target = generations[random.below(static_cast<std::uint32_t>(generations.size()))];
      auto replicas = world->fabric->list_replicas(target.first, target.second);
      if (replicas) {
        if (!replicas.value().empty()) {
          ReplicaTransition transition;
          transition.replica_id = replicas.value().front().replica_id;
          transition.expected_coordinator_epoch = world->epoch();
          transition.reason = "randomized retirement";
          if (random.coin()) {
            (void)world->fabric->retire_replica(transition);
          } else {
            (void)world->fabric->quarantine_replica(transition);
          }
        }
      }
    } else if (choice == 8U) {
      (void)world->fabric->advance_time(1U + random.below(4U));
    } else {
      auto reconcilable = world->fabric->list_replica_sets();
      if (reconcilable && !reconcilable.value().empty()) {
        const ReplicaSetRecord& set = reconcilable.value().front();
        (void)world->fabric->reconcile_replica_set(set.state_id, set.state_generation);
      }
    }

    const FabricSnapshot snapshot = world->fabric->snapshot();
    const InvariantReport report = check_invariants(*world->fabric, snapshot);
    if (!report.ok) {
      return report;
    }
  }

  *digest = canonical_digest(world->fabric->snapshot());
  return InvariantReport{};
}

}  // namespace

CCSF_CASE(property, randomized_sequences_preserve_invariants) {
  const std::uint64_t base_seed = ctx.seed();
  const std::uint32_t sequences = 12;
  for (std::uint32_t index = 0; index < sequences; ++index) {
    const std::uint64_t seed = base_seed * 2654435761ULL + index;
    std::string digest;
    const InvariantReport report = run_sequence(seed, 24U, &digest);
    if (!report.ok) {
      ctx.check(false, "seed " + std::to_string(seed) + " sequence " + std::to_string(index) +
                           " violated an invariant: " + report.detail);
    }
  }
}

CCSF_CASE(property, randomized_sequences_are_reproducible) {
  const std::uint64_t seed = ctx.seed() * 6364136223846793005ULL + 17U;
  std::string first;
  std::string second;
  const InvariantReport first_report = run_sequence(seed, 20U, &first);
  const InvariantReport second_report = run_sequence(seed, 20U, &second);
  ctx.check(first_report.ok && second_report.ok,
            "the reproducibility sequence must satisfy every invariant: " + first_report.detail +
                second_report.detail);
  ctx.check(first == second,
            "the same seed must produce the same canonical control plane (seed " +
                std::to_string(seed) + ")");
}
