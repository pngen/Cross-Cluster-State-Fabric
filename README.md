# Cross-Cluster State Fabric

An open-source, vendor-neutral C++20 runtime for governing authoritative placement, replication,
migration, reuse, durability, integrity, compatibility and economic movement of reusable machine
state across independent clusters and regions.

## The question this runtime answers

> Where should reusable machine state live across clusters or regions, how many authoritative
> replicas should exist, when is migration or replication preferable to reconstruction, which copy
> may be reused now, and how does authority remain correct as clusters fail, reconnect, diverge, or
> advance generation?

The defining distinction of this project is:

> A copy exists in another cluster.

versus:

> This exact state generation has a verified, compatible, current, authoritative replica in that
> cluster under current replica-set, placement, cluster-incarnation, coordinator-epoch and policy
> authority.

Everything in the repository exists to make the second statement provable and the first statement
unable to masquerade as it.

## Systems boundary

Cross-Cluster State Fabric owns:

- cross-cluster state identity, generations and lineage;
- placement authority, deterministic placement ranking and placement policy;
- replica identity, replica generations, replica role, replica lifecycle and replica-set membership;
- replication policy, replication-factor enforcement and replica-set reconciliation;
- migration planning, migration authority and migration lifecycle;
- cross-cluster reuse eligibility, compatibility gating and integrity gating;
- stale replica fencing, stale cluster-incarnation fencing and coordinator-epoch fencing;
- ownership transfer, source and destination selection, durability classification;
- degraded replica-set state, split-brain prevention and authoritative commit boundaries;
- transfer-versus-reconstruction economics;
- conservative crash recovery and persistence of durable control state;
- deterministic explanations and historical placement/replication/migration audit.

It does not own, and deliberately does not absorb:

- raw accelerator-memory tiering, arbitrary in-process allocation, KV or tensor-cache internals;
- checkpoint creation semantics, storage-engine or object-store implementation;
- transport implementation (the framed TCP transport here exists only for the reference deployment);
- RDMA, GPUDirect, CXL, generic topology discovery, network routing or region provisioning;
- generic cluster scheduling, generic workload placement, generic failover or recovery planning;
- generic database replication, shared-memory coherence, model serving or resource brokerage.

It consumes evidence from adjacent systems and emits typed intents to them. Adjacent systems publish
capability, capacity, health, topology, cost and compatibility evidence; this runtime decides
authority. No adjacent repository is required to build or use it.

## Architecture

    +----------------------+        framed TCP control plane        +-----------------------+
    |  controller / CLI    | <------------------------------------> |  coordinator process  |
    |  (test driver, ops)  |                                      |  owns the Fabric      |
    +----------------------+                                      |  owns durable state   |
                                                                  +-----------------------+
                                                                       ^   ^   ^
                                                     framed TCP control|   |   |
                                                                       |   |   |
                                              +------------------------+   |   +-------------+
                                              |                            |                 |
                                     +-----------------+          +-----------------+  +-----------------+
                                     | cluster agent A |          | cluster agent B |  | cluster agent C |
                                     | local byte store|          | local byte store|  | local byte store|
                                     +-----------------+          +-----------------+  +-----------------+
                                              ^                            ^
                                              |   framed TCP data plane    |
                                              +----------------------------+
                                                 (real bytes, real digests)

- **Core library** (`cross_cluster_state_fabric`): the runtime. No I/O beyond the durable control
  plane; no dependency on the reference deployment.
- **Coordinator process** (`ccsf_coordinator`): owns exactly one `Fabric`, the durable control
  plane, and the control plane that controllers and cluster agents speak.
- **Cluster agent processes** (`ccsf_cluster_agent`): register a durable `ClusterId`, mint a fresh
  `ClusterIncarnationId` per process, publish evidence, host replica bytes, and execute transfer and
  verification intents under explicit authority. An agent never decides authority.
- **Controller** (tests, CLI, operator tooling): issues typed control requests. All mutation flows
  through the same authoritative runtime paths the library exposes.

## State model

A managed state object is not a blob. Every state generation carries:

| Field | Meaning |
| --- | --- |
| `StateId`, `StateGeneration`, `LineageId` | typed identity of exactly one generation of one lineage |
| `StateClass` | `OPAQUE_STATE`, `TENSOR_STATE`, `KV_STATE`, `PREFIX_STATE`, `CHECKPOINT_STATE`, `COMPILED_ARTIFACT`, `MODEL_COMPONENT`, `EXECUTION_SNAPSHOT`, `CACHE_OBJECT`, `INTERMEDIATE_RESULT`, `CUSTOM` |
| `ConsistencyMode` | `IMMUTABLE`, `APPEND_ONLY`, `SINGLE_WRITER`, `SNAPSHOT`, `OPAQUE_EXTERNAL_CONSISTENCY` |
| logical and physical size | physical size may be UNKNOWN; UNKNOWN is not zero |
| content digest | SHA-256 over the state bytes; may be UNKNOWN at registration, never at commit |
| compatibility identity | `CompatibilityId` + generation + a canonical attribute requirement |
| reconstruction identity | how the state could be rebuilt instead of transferred, with its own cost evidence |
| durability and reuse class | durability obligation; how broadly the generation may be reused |
| placement constraints and replication requirements | hard constraints and replica-count/diversity floors |
| provenance | producer, origin cluster and incarnation, source generation, source replica, operation, worker boot, coordinator epoch, commit, verification |
| expected reuse value, transfer cost, reconstruction cost | each carrying `MEASURED`, `POLICY`, `ESTIMATED` or `UNKNOWN` provenance |
| current lifecycle and current authority | see below |

The core is type-agnostic: it never special-cases a state class.

### State generation lifecycle

`STAGED` → `CURRENT` → `HISTORICAL` (with `SUPERSEDED`, `INVALID`, `CORRUPT`,
`REVALIDATION_REQUIRED`, `RETIRED` available).

Registration never makes a generation current. Only an explicit authoritative commit
(`commit_state_generation` with a `CommitId`) does, and that commit deterministically supersedes the
prior current generation of the same lineage: the superseded generation becomes `HISTORICAL`
(durable truth that stays in the audit trail), and its replicas become `SUPERSEDED` with
`HISTORICAL` authority and stop counting toward the replication factor.

Legal branching is explicit. A generation may declare `branch = true` together with the generation
it branches from; only then may two generations of one lineage be current at once. Committing a
non-branched generation while another non-branched generation of the same lineage is current is
refused with `SPLIT_BRAIN_RISK`.

## Replica model

A replica is modeled along independent dimensions rather than one enum:

- identity: `ReplicaId`, `ReplicaGeneration`, state generation, `ReplicaSetId`, `ReplicaSetGeneration`;
- location: `ClusterId`, `ClusterIncarnationId`, `RegionId`, `SiteId`, `FailureDomainId`, descriptor;
- integrity state: `UNKNOWN`, `UNVERIFIED`, `VERIFYING`, `VERIFIED`, `MISMATCH`, `CORRUPT`;
- compatibility state: `UNKNOWN`, `INCOMPATIBLE`, `COMPATIBLE`, `REVALIDATION_REQUIRED`;
- durability state: `UNKNOWN`, `VOLATILE`, `STANDARD`, `DURABLE`, `ARCHIVAL`;
- availability state: `UNKNOWN`, `PRESENT`, `ABSENT`, `UNREACHABLE`, `DRAINING`;
- authority state: `NONE`, `HISTORICAL`, `CANDIDATE`, `AUTHORITATIVE_FOR_REUSE`,
  `AUTHORITATIVE_SOURCE`, `AUTHORITATIVE_OWNER`, `FENCED`;
- lifecycle: `PLANNED`, `RESERVED`, `STAGING`, `TRANSFERRING`, `TRANSFERRED`, `VERIFYING`,
  `PREPARED`, `AUTHORITATIVE`, `DEGRADED`, `STALE`, `REVALIDATION_REQUIRED`, `SUPERSEDED`,
  `QUARANTINED`, `CORRUPT`, `RETIRING`, `RETIRED`, `ABANDONED`;
- evidence: integrity, compatibility, evidence, capability and health generations, verification
  identity, verification tick, coordinator epoch, revalidation obligation;
- provenance: where the copy came from and which operation, worker boot and cluster incarnation
  produced it.

## Authority model

Presence is one fact. Authority is a separate conclusion, evaluated field by field:

`physically_present`, `transfer_complete`, `integrity_verified`, `compatibility_verified`,
`current_for_state_generation`, `member_of_current_replica_set_generation`,
`cluster_incarnation_current`, `coordinator_epoch_current`, `evidence_current`,
`authoritative_for_reuse`, `authoritative_as_replication_source`,
`authoritative_for_migration_source_retirement`, `historical_only`, `fenced`.

A denied assessment always carries a specific structured code — `STALE_CLUSTER_INCARNATION`,
`STALE_EPOCH`, `STALE_STATE_GENERATION`, `INTEGRITY_UNKNOWN`, `INTEGRITY_MISMATCH`,
`INCOMPATIBLE`, `COMPATIBILITY_UNKNOWN`, `REVALIDATION_REQUIRED`, `FENCED`,
`AUTHORITY_NOT_ESTABLISHED` — never a bare false.

Counting is derived, never declared. `replica_counts_toward_replication_factor` is a pure function
of the replica record: committed, member of the current replica set, integrity `VERIFIED`,
compatibility `COMPATIBLE`, lifecycle `AUTHORITATIVE` or `DEGRADED`. Transient availability is
deliberately *not* part of it: a committed replica that is momentarily unreachable is still a
committed member. Explicit fencing events (cluster loss, reincarnation, quarantine, retirement)
move the replica out of the counting lifecycles instead.

## Replica-set model

A replica set is a first-class object with a stable `ReplicaSetId` and a versioned
`ReplicaSetGeneration`. Its authoritative and degraded membership is *derived* from the replica
records on every read, so a replica set can never disagree with the replicas it describes and an
authoritative count can never drift.

States: `HEALTHY`, `DEGRADED`, `UNDER_REPLICATED`, `OVER_REPLICATED`,
`REVALIDATION_REQUIRED`, `SPLIT_BRAIN_RISK`, `NO_AUTHORITATIVE_REPLICA`, `CORRUPT`,
`REBUILDING`, `RETIRING`.

The replica set generation advances only when the authoritative membership actually changes: a
commit, a retirement, a quarantine, a fence, a revalidation that restores authority, or an
authorized ownership handover. Staging, transfer progress and verification never advance it, so an
operation is never fenced by its own progress. Every operation captures the replica-set generation
it was authorized against and refuses to commit if membership moved on
(`STALE_REPLICA_SET_GENERATION`).

`STAGING`, `TRANSFERRING`, `VERIFYING`, `STALE`, `CORRUPT` and `REVALIDATION_REQUIRED`
replicas never count toward the authoritative replication factor.

## Placement semantics

Hard constraints are evaluated first and are absolute; no score can rescue a candidate that failed
one. The constraint set includes legal and prohibited clusters and regions, required locality,
destination eligibility rules, destination health, required health floor, required capabilities,
storage classes, data-sovereignty tags, encryption-at-rest and attestation evidence, compatibility
(`INCOMPATIBLE` and `UNKNOWN` are both refused), integrity evidence availability, achievable
durability, free capacity, evidence freshness, cluster/region/failure-domain diversity floors and a
policy cost ceiling.

Ranking then uses exact integer fixed-point arithmetic over named factors, in a fixed order:
`expected_reuse_value`, `transfer_cost`, `reconstruction_cost`, `available_bandwidth`,
`predicted_transfer_duration`, `storage_cost`, `compute_cost`, `region_affinity`,
`workload_affinity`, `existing_replica_proximity`, `failure_domain_diversity`,
`expected_future_demand`, `source_health`, `destination_health`, `pressure`, `utilization`.

Absent evidence is scored pessimistically: an UNKNOWN factor takes the worst value the policy's
weight sign could prefer, so missing evidence can never look best. Tie-breaking is total: score,
then failure-domain diversity, then region affinity, then the lower cluster identity. The same
canonical state and the same policy always produce the same decision and the same explanation text.

## Replication semantics

A replication transaction is explicit and preserves source authority:

    PLAN → RESERVE → TRANSFER → VERIFY → PREPARE → ADD_TO_REPLICA_SET → COMMIT → CLOSE

Authorization resolves the source replica, the policy, and the destination, reserves identifiers
and destination capacity, and creates the destination replica in `RESERVED`. Transfer completion
records the measured digest but changes no authority. Verification gates integrity and
compatibility and prepares the destination. Commit adds it to the replica set and advances the
replica-set generation. A destination with complete bytes and failed verification is quarantined and
can never become authoritative.

Concurrency control is generation-based: every step revalidates the generations captured at
authorization time, and a step that observes a superseded generation fails closed instead of
committing.

## Migration semantics

A migration moves authority and, when the request asks for it, retires the source:

    PLAN → RESERVE → STAGE_DESTINATION → TRANSFER → VERIFY_BYTES → VERIFY_COMPATIBILITY
         → PREPARE_AUTHORITY → COMMIT_DESTINATION → optionally RETIRE_SOURCE → CLOSE

The source is never retired before the destination commit is durable. Retirement happens inside the
same transaction as the destination commit, so the two cannot be separated by a crash. The
authoritative owner of mutable state cannot be retired in place: ownership must be handed over
first. A destination that has complete bytes but failed verification never becomes authoritative,
and a source is never retired on an ambiguous commit.

### Ownership transfer

Mutable state (`SINGLE_WRITER`, `SNAPSHOT`, `APPEND_ONLY`) has exactly one authoritative owner
at a time, recorded as an `OwnershipRecord` with the old and new owner, the replica and placement
generations, the coordinator epoch and the handoff commit. A second simultaneous ownership claim is
refused with `SPLIT_BRAIN_RISK`; a handoff is idempotent by commit identity. Immutable state does
not require an owner and may hold several authoritative replicas at once. Multi-writer mutable state
is not modeled and is not claimed.

## Consistency

Consistency semantics are explicit and conservative. For immutable state, replicas of one verified
digest are independently reusable once authority conditions hold. For mutable or snapshot state,
generation authority determines which replica is current, and conflicting current generations fail
closed. General strong consistency across arbitrary mutable state is not claimed.

## Transfer-versus-reconstruction economics

Economics decide *how* to move state, never *whether* moving it is legal. Hard preconditions are
checked first: an unhealthy destination, an unhealthy source or insufficient capacity rejects
outright regardless of price. A predicted transfer duration beyond the policy horizon, an unknown
expected reuse value, or a total absence of price evidence defers rather than guessing.

Cost values carry provenance (`MEASURED`, `POLICY`, `ESTIMATED`, `UNKNOWN`) and are exact
integer micro-units; no floating-point value can influence a decision. UNKNOWN is not zero and
absorbs: adding or scaling an UNKNOWN cost yields UNKNOWN, and comparing an UNKNOWN cost against a
known one treats UNKNOWN as strictly worse. When reconstruction is unavailable, transfer is chosen;
when one side has no price evidence, the priced side wins; when both are known, the cheaper legal
path is chosen and ties preserve the exact state bytes.

## Integrity and compatibility

SHA-256 is implemented in-tree (FIPS 180-4) and covered by RFC vectors, streaming/chunked
equivalence tests and boundary-length vectors at 0, 1, 2, 3, 31, 32, 33, 54, 55, 56, 57, 58, 63,
64, 65, 66, 119, 120, 127, 128, 129, 191, 192, 193, 255, 256, 257 and 1000 bytes, so padding
boundaries are exercised directly. Frame and record integrity use CRC-32C (Castagnoli) with its
published check value. No first-party code silently downgrades an algorithm.

Compatibility is a hard gate, not a ranking factor. A missing candidate is `UNKNOWN`; a candidate
whose evidence generation is behind the requirement, or that is flagged for revalidation, is
`REVALIDATION_REQUIRED`; a required attribute the candidate does not carry is `UNKNOWN`; only a
differing concrete value is `INCOMPATIBLE`. UNKNOWN never becomes compatible. Clusters publish
compatibility evidence through capability tags of the form `compat:<key>=<value>` plus their
accelerator, storage and platform attributes; the runtime consumes external compatibility identities
rather than duplicating a compatibility registry.

## Split-brain prevention

Authority depends on the current `CoordinatorEpoch`, `StateGeneration`, `ReplicaSetGeneration`,
`ClusterIncarnationId`, worker boot identity and required evidence generations. Every request
carries the authority it was minted under and is refused when that authority is stale:

- `STALE_EPOCH` — coordinator restart advanced the epoch;
- `STALE_WORKER` — a different worker boot identity is acting;
- `STALE_CLUSTER_INCARNATION` — the cluster reincarnated or is not the live incarnation;
- `STALE_STATE_GENERATION`, `STALE_REPLICA_GENERATION`, `STALE_REPLICA_SET_GENERATION`,
  `STALE_TRANSFER_GENERATION`, `STALE_OPERATION_GENERATION`, `STALE_EVIDENCE`.

At most the modeled number of authoritative owners may exist: exactly one current authoritative
ownership chain for the default single-authority model. Fencing is validated before idempotency, so
a caller holding a stale view is never told that its stale request succeeded.

## Cluster incarnation and coordinator epoch

A cluster identity is durable; a cluster incarnation is not. Every cluster agent process start mints
a fresh `ClusterIncarnationId` and a fresh `WorkerBootId`. When a cluster re-registers with a new
boot identity, the coordinator advances the incarnation, fences every replica written under the old
one, preserves their committed history and requires revalidation before their state may be reused.
A cluster that disappears and later returns does not reclaim authority merely because old bytes
still exist. Revalidation explicitly rebinds a surviving replica to the incarnation that is current
now, records the rebinding, and only then restores counting.

A coordinator restart advances `CoordinatorEpoch`, marks every recovered cluster's dynamic evidence
as requiring revalidation, preserves committed replica-set membership and history, and classifies
in-flight operations conservatively:

| In-flight state at crash | Recovery classification |
| --- | --- |
| `PLANNED`, `RESERVED`, `STAGING_DESTINATION`, `TRANSFERRING` | `ABANDONED`; the destination copy carries no authority |
| `TRANSFERRED`, `VERIFYING`, `PREPARED` | `OUTCOME_UNKNOWN`; the destination is quarantined and requires explicit revalidation |
| `COMMITTED`, `ADDED_TO_REPLICA_SET` | `COMPLETE`; the commit was already durable |
| `SOURCE_RETIRING` | `OUTCOME_UNKNOWN`; the source is preserved because retirement is destructive |

## Persistence

The durable control plane is a versioned, integrity-checked snapshot plus a checksummed
append-only journal of redo effects:

- `control.snapshot` — magic `CCSS`, format version, sequence, declared payload length, payload
  CRC-32C and a header CRC computed over the header with its own checksum field canonically zeroed;
- `control.journal` — magic `CCSJ`, format version, header CRC, then `REC1` records of
  `length | CRC-32C | payload`.

A mutation is serialized, appended to the journal and flushed to stable storage **before** it is
applied in memory and before it is acknowledged. A checkpoint serializes the state, writes a
temporary file, flushes it, atomically replaces the snapshot (`MoveFileExW` with
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` on Windows, `rename` plus directory `fsync`
elsewhere) and only then resets the journal. Checkpoints take the same commit lock as mutations, so
a checkpoint can never reset the journal while a mutation is appending to it.

Replay is idempotent by construction: effects are whole-record puts keyed by identity, and applying
the same effect twice is a no-op. A crash between the snapshot replacement and the journal reset
therefore converges. A single truncated journal tail record is discarded as an unacknowledged
mutation and reported in `FabricStatus::journal_tail_discarded`; corruption inside a complete
record, unknown versions, trailing garbage, impossible references, duplicate identities and absurd
declared sizes are hard failures.

## Transport

The reference deployment uses a versioned framed TCP transport with an 80-byte big-endian header:
magic, protocol version, message type, flags, payload length, payload CRC-32C, coordinator epoch,
cluster id, cluster incarnation, worker id, worker boot id, operation generation, sequence and a
header CRC computed with the checksum field zeroed. The maximum frame size is bounded. Reads and
writes loop until complete; a peer that closes mid-frame is reported as `FRAME_TRUNCATED`, never
accepted as a short frame. Bad magic, unknown mandatory version, unknown message type, oversized
declared payload, checksum mismatch, truncated header or payload are all rejected with specific
codes. The checksum never covers itself.

The data plane is separate: a destination cluster agent opens a real TCP connection to the source
agent's data port, requests a hosted replica and receives bounded `DATA_CHUNK` frames, hashing
while it receives. The transfer is real bytes between independent processes with a real SHA-256 at
each end.

## Concurrency model

Every public `Fabric` method is safe to call concurrently.

- Queries take a shared lock and return value copies; they never observe a partially mutated plane.
- Mutations take `commit_mutex_`, then the state lock. The lock order is always
  `commit_mutex_` → `checkpoint_mutex_` → state lock, and never the reverse.
- A mutation validates and computes its effects while holding the state lock, releases the state
  lock before touching the durable log, and re-acquires it to apply.
- No lock is ever held across file I/O, network I/O, backend calls or callbacks.
- Mutation bodies receive a *const* control plane and express every change as an ordered list of
  durable effects. Nothing mutates runtime state directly, so live execution and journal replay are
  the same function of the same effects, and a rejected request cannot leave a partial mutation
  behind.
- Replica-set membership, operation accounting and derived counters are recomputed on read rather
  than maintained in parallel, so they cannot drift.

## Crash and process-death behavior

The multiprocess proof exercises real process death, not simulated failure:

- killing a cluster agent removes its live authority, fences its replicas, marks the replica set
  under-replicated and rejects its stale frames;
- restarting it mints a new incarnation, and its durable copies must be revalidated before reuse;
- killing the coordinator without a graceful shutdown and restarting it against the same durable
  directory advances the epoch, preserves committed membership and history, classifies ambiguous
  in-flight work honestly, and rejects old-epoch traffic.

## Build

Requirements: CMake 3.25 or newer, a C++20 compiler. Windows with MSVC is first-class and the whole
first-party tree builds with `/W4 /WX /permissive-` and zero warnings.

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release --parallel

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `CROSS_CLUSTER_STATE_BUILD_TESTS` | `ON` | build the test suites |
| `CROSS_CLUSTER_STATE_BUILD_EXAMPLES` | `ON` | build the runnable examples |
| `CROSS_CLUSTER_STATE_BUILD_BENCHMARKS` | `ON` | build the benchmarks |
| `CROSS_CLUSTER_STATE_BUILD_CLI` | `ON` | build the inspection CLI |
| `CROSS_CLUSTER_STATE_ENABLE_CUDA` | `OFF` | build the CUDA-backed state example |
| `CROSS_CLUSTER_STATE_WARNINGS_AS_ERRORS` | `ON` | treat first-party warnings as errors |
| `CROSS_CLUSTER_STATE_ENABLE_ASAN` | `OFF` | instrument first-party code with AddressSanitizer |

The core library builds without CUDA and without any other Summon Software Labs repository.

## Tests

Every suite exposes stable case identifiers (`<suite>::<case>`) and can be filtered:

    build/tests/Release/ccsf_tests.exe                     # everything
    build/tests/Release/ccsf_tests.exe --case replication  # one suite
    build/tests/Release/ccsf_tests.exe --case migration::source_is_not_retired_before_commit
    build/tests/Release/ccsf_tests.exe --list

Cases print `BEGIN <suite>::<case>` and `PASS`/`FAIL <suite>::<case>: <reason>` immediately and
unbuffered. No case runs under a timeout of any kind; a hanging case is a defect to diagnose.

Suites:

| Suite | What it proves |
| --- | --- |
| `sha256`, `crc32c`, `bytes`, `ids`, `taxonomy`, `cost` | digests, padding boundaries, checked codecs, typed identities, enum validation, cost algebra |
| `compatibility`, `policy`, `economics`, `replica_set` | hard gating, canonical fingerprints, deterministic economic decisions, derived membership |
| `state` | staged registration, explicit commit, deterministic supersede, duplicates, lineage and explicit branching |
| `authority` | presence is not authority, integrity and compatibility fencing, quarantine, historical-only copies |
| `replication` | pre-commit copies do not count, transactional commit, idempotent and conflicting duplicates, over-replication bound |
| `migration` | ownership handoff, source not retired before commit, stale completion fences, cancellation semantics |
| `placement` | hard constraints dominate ranking, bit-identical repeated decisions, pessimistic UNKNOWN scoring, diversity floors, capacity gates |
| `persistence` | durable reopen, journal-only reconstruction, durability before acknowledgement, corruption, truncation, trailing garbage, unknown version, truncated tail, conservative recovery |
| `protocol`, `transport` | frame round trip, magic/version/type/checksum/size/truncation rejection, absurd length rejection, loopback exchange, peer close |
| `splitbrain` | epoch, worker boot, replica/set/transfer generations, one current generation per lineage, cluster loss with preserved history |
| `property` | seeded randomized sequences (12 sequences × 24 operations) with invariants checked after **every** operation, plus reproducibility of the canonical control plane |
| `adversarial` | source death mid-transfer, conflicting registration and publication, compatibility change before commit, resource bounds, concurrent migrations, repeated restart/retire cycles returning to baseline |
| `concurrency` | readers never observe a torn plane, parallel mutation of distinct states, serialized checkpoints, idempotent shutdown |
| `multiprocess` | the real multiprocess proof: coordinator + three agents, real byte transfer, hard kill, reincarnation, coordinator crash and durable recovery |
| `cuda` | real device allocation, kernel, synchronization, copy-back, digest replication and memory return (only with `CROSS_CLUSTER_STATE_ENABLE_CUDA`) |

### AddressSanitizer

    cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DCROSS_CLUSTER_STATE_ENABLE_ASAN=ON
    cmake --build build-asan --config RelWithDebInfo --parallel
    build-asan/tests/RelWithDebInfo/ccsf_sanitizer_self_test.exe --expect-report   # must report
    build-asan/tests/RelWithDebInfo/ccsf_tests.exe                                 # must be clean

The self test performs a deliberate heap-buffer-overflow and must produce a sanitizer report with a
non-zero exit code; it is proof that instrumentation is real rather than assumed. MSVC
AddressSanitizer does not implement LeakSanitizer on this platform, so leak checking is covered by
explicit accounting tests instead (`active_operations`, `reserved_bytes`,
`reserved_transfer_slots` return to baseline, asserted in `adversarial`).

## Installation and downstream use

    cmake --install build --config Release --prefix /path/to/prefix

This installs the library, the public headers, the exported targets and
`CrossClusterStateFabricConfig.cmake` / `CrossClusterStateFabricConfigVersion.cmake`.

A downstream project uses only the installed package:

    find_package(CrossClusterStateFabric CONFIG REQUIRED)
    target_link_libraries(my_target PRIVATE CrossClusterStateFabric::cross_cluster_state_fabric)

`tests/consumer/` is exactly that project, built and run outside this repository's own build tree.

## CLI

    ccsf_cli --connect 127.0.0.1:7700 status
    ccsf_cli --connect 127.0.0.1:7700 clusters
    ccsf_cli --connect 127.0.0.1:7700 replicas <state-id> <generation>
    ccsf_cli --connect 127.0.0.1:7700 reuse <state-id> <generation> <cluster-id>
    ccsf_cli --connect 127.0.0.1:7700 reconcile <state-id> <generation>
    ccsf_cli --connect 127.0.0.1:7700 plan-replication <state-id> <generation>
    ccsf_cli --connect 127.0.0.1:7700 operations
    ccsf_cli --connect 127.0.0.1:7700 operation <operation-id>

    ccsf_cli --persistence /path/to/control-plane snapshot-info
    ccsf_cli --persistence /path/to/control-plane clusters
    ccsf_cli --persistence /path/to/control-plane state <state-id> <generation>

Online the CLI speaks the same typed control plane a controller or an operator tool uses, so it can
never see state the runtime would not report. Offline it opens the durable control plane through the
public library API and answers the same queries; it offers no fake controls and refuses mutating
commands offline.

## Examples

Ten runnable examples compile against the public API only:

| Example | Shows |
| --- | --- |
| `ex_basic_replication` | a full replication transaction and the authority it produces |
| `ex_migration` | migration with explicit ownership handoff and source retirement |
| `ex_transfer_vs_reconstruction` | the economic decision, including UNKNOWN and illegal-destination cases |
| `ex_incompatible_destination` | a hard compatibility rejection and the placement explanation for it |
| `ex_stale_replica` | a fenced replica, preserved history and under-replication |
| `ex_cluster_reincarnation` | incarnation change, fencing and revalidation |
| `ex_under_replicated_repair` | reconciliation and repair of a replica set |
| `ex_coordinator_restart` | durable restart, epoch advance and revalidation |
| `ex_deterministic_explanation` | byte-identical explanations on repetition |
| `ex_cuda_state` | a device-produced state payload governed and replicated by the runtime |

## Benchmarks

`ccsf_benchmarks` measures completed work, never enqueue time. Representative Release results on
the development machine (Windows, MSVC 19.44, x64):

    state registration                           1000 ops     0.007 s        6.6 us/op
    state generation commit                      1000 ops     0.006 s        5.7 us/op
    replica registration                         1000 ops     0.007 s        6.5 us/op
    placement evaluation                         5000 ops     0.011 s        2.3 us/op
    replica lookup                              20000 ops     0.002 s        0.1 us/op
    reuse eligibility                            5000 ops     0.045 s        9.0 us/op
    replica set reconciliation                   5000 ops     0.025 s        5.0 us/op
    replication planning                         2000 ops     0.084 s       42.1 us/op
    deterministic explanation                    2000 ops     0.012 s        6.1 us/op
    integrity verification (1 MiB)                512 ops     1.259 s     2459.9 us/op
    verified throughput                                                   406.5 MiB/s
    durable mutation (journal append)            1000 ops     1.003 s     1003.4 us/op
    10000 states registered                     10000 ops     0.380 s       38.0 us/op

The SHA-256 implementation is a correctness-first in-tree construction, not an optimized one; its
throughput is reported as measured. Durable mutation cost is dominated by the flush to stable
storage that durability requires.

## CUDA proof

When `CROSS_CLUSTER_STATE_ENABLE_CUDA=ON` and a CUDA compiler is present, `ccsf_cuda_proof` and
the `cuda` test case allocate device memory, copy inputs host-to-device, launch a kernel,
synchronize, copy the device-produced result back, register those exact bytes as `TENSOR_STATE`
content, replicate them, verify digest parity at the destination, retire the source and free every
device allocation, checking that device free memory returns to its baseline.

Measured on the development machine:

    device: NVIDIA GeForce RTX 5090 sm_120 total=32579 MiB runtime=12090 driver=13040
    device state: 32768 bytes digest=23fe9561c46fa5daf8e54df90a4b86b948fee4d2fc082972d7a9b1010ee9a3fb cpu_parity=pass
    device memory restored: yes

## REAL / SYNTHETIC / UNSUPPORTED

**REAL** — what is actually executed and observed in this repository:

- independent operating system processes (coordinator, three cluster agents, controller);
- real TCP with partial reads, partial writes and bounded framing;
- real byte transfer between two independent processes with real SHA-256 at both ends;
- real hard process termination (`TerminateProcess`) and real coordinator crash/restart;
- real transactional persistence with flush and atomic replacement on Windows;
- real SHA-256 and CRC-32C with published vectors and boundary coverage;
- real CUDA device work on an NVIDIA RTX 5090 (sm_120): `cudaMalloc`, H2D copy, kernel launch,
  `cudaDeviceSynchronize`, D2H copy, `cudaFree` and a device free-memory baseline check;
- real MSVC x64 AddressSanitizer instrumentation, proven by a self test that must report.

**SYNTHETIC** — modeled, not physically realized:

- the three "clusters" are cluster agent processes on a single physical host; placement, regions,
  failure domains, capacity, bandwidth and WAN-like cost are policy and evidence inputs, not
  physical geography;
- transfer cost, egress cost, storage cost, compute cost, reuse value and demand are synthetic
  numbers supplied through policy, labelled `POLICY` or `ESTIMATED` in every explanation;
- the reference transport is loopback TCP, standing in for a real inter-site link;
- the coordinator is a single process: it is a single-writer authority, not a replicated control
  plane, and quorum semantics are not implemented or claimed.

**UNSUPPORTED** — not implemented, not tested, and not claimed anywhere in this repository:

- real multi-site WAN behaviour, real cloud egress, real region provisioning;
- RDMA, GPUDirect, CXL, NVLink-across-hosts, multi-GPU migration, real cluster storage appliances;
- cross-machine GPU state movement (the CUDA proof's device work is real; its "second cluster" is a
  logical cluster on the same host);
- quorum-based coordinator replication, multi-writer mutable state, and general strong consistency
  across arbitrary mutable state;
- POSIX process management: `ChildProcess` is implemented for Windows in this release and returns
  `NOT_SUPPORTED` elsewhere. The durable file primitives do have a POSIX branch, which is
  implemented but was not validated in this release environment.

## Repository layout

    include/ccsf/      public headers (the entire supported surface)
    src/               library implementation, including src/detail/
    apps/              reference deployment: coordinator and cluster agent processes
    tools/ccsf/        inspection CLI
    examples/          ten runnable examples against the public API
    benchmarks/        completed-work benchmarks
    tests/             test suites, harness and the independent find_package consumer
    cuda/              optional CUDA-backed state example
    cmake/             package configuration template

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
