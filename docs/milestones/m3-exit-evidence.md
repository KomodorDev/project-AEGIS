# M3 Exit Evidence

> **Purpose:** Record the canonical-submission implementation, deterministic exit proof, named
> workload smoke evidence, forbidden-capability audit, publication, and completed integration.

**Status:** M3 was verified on `codex/m3-canonical-submission` at clean implementation and
benchmark producer `27087d4da423546041295de43e7fa2fb31425b63`, with tree
`2e339a5d97bdc9b6bd4522e754c6052783cb01b4`. The branch starts from the integrated M2 baseline
`d7733bb16a52d5ec954338f861d798d8c6620dad` and reconciles the later comment-only CI `dev` tip
`94598c38a750d40f5e10030ebf33706f459bff17` before publication. Normal, formatting, ASan+UBSan,
TSan, Release, deterministic replay, forbidden-capability, and M0/M2/M3 benchmark-evidence checks
passed locally as recorded below. [PR #10](https://github.com/KomodorDev/project-AEGIS/pull/10)
later merged the final feature head `2d5ea9e5b7fc28234789dc7c97ec4fc7bb71ef01` unchanged into
`dev` as `962eb8602c13c1930a74c59232f96920482edb2b` on 2026-08-23.

Publication, integration, and producer evidence remain distinct facts. The completed merge records
repository integration but does not replace or reinterpret the revision-specific local record
below. The recorded M3 performance bundle is valid smoke evidence from uncontrolled host
conditions; it makes no `REF-MAC-01` qualification or provisional-threshold claim.

## Dependency and revision record

The table preserves the exact M2 base, M3 producer, publication, and merge identities needed to
interpret the evidence below.

| Field | Recorded value |
|---|---|
| M2 integration baseline | `d7733bb16a52d5ec954338f861d798d8c6620dad` |
| M2 pull request | [PR #8](https://github.com/KomodorDev/project-AEGIS/pull/8), targeting `dev` |
| `dev` tip reconciled before M3 publication | `94598c38a750d40f5e10030ebf33706f459bff17` from comment-only CI [PR #9](https://github.com/KomodorDev/project-AEGIS/pull/9) |
| M3 branch | `codex/m3-canonical-submission` |
| M3 pull request | [PR #10](https://github.com/KomodorDev/project-AEGIS/pull/10), targeting `dev` |
| Final M3 feature head | `2d5ea9e5b7fc28234789dc7c97ec4fc7bb71ef01` |
| Final M3 feature tree | `f58410737c473bc3992d7cfd7c934d41db1d11cf` |
| M3 implementation range | `d7733bb16a52d5ec954338f861d798d8c6620dad..27087d4da423546041295de43e7fa2fb31425b63` |
| Clean implementation and benchmark producer | `27087d4da423546041295de43e7fa2fb31425b63` |
| Producer tree | `2e339a5d97bdc9b6bd4522e754c6052783cb01b4` |
| Clean worktree fingerprint | `64d17613312d21071e25a1816ad9abe920d87ef98ba1596dad8d2c469b7ed886` |
| Evidence captured | 2026-08-22 |
| Publication state | Branch pushed; PR #10 published against `dev` |
| Integration merge | `962eb8602c13c1930a74c59232f96920482edb2b` on 2026-08-23 |
| Integration parents | `94598c38a750d40f5e10030ebf33706f459bff17` and `2d5ea9e5b7fc28234789dc7c97ec4fc7bb71ef01` |
| Integration tree | `f58410737c473bc3992d7cfd7c934d41db1d11cf`; identical to the final feature tree |
| Integration state | Merged into `dev`; integrated capability baseline is M3 |

The evidence is revision-specific. A later documentation-only commit may describe this producer but
does not move its source, test, executable, or benchmark identity.

## Accepted M3 contracts

These adopted decisions define the canonical-submission and conservative fake-initiation promises
against which M3 is evaluated.

- [ADR-0008](../decisions/0008-canonical-submission-and-fixed-risk.md) fixes the bot-bound
  capability, explicit `RouteId`, limit/GTC/no-flags vocabulary, canonical validation order,
  immutable fixed risk policy, exact conservative exposure, seven scopes, atomic reservation, and
  stable submission outcomes.
- [ADR-0009](../decisions/0009-outbound-oms-and-fake-initiation.md) fixes outbound OMS admission,
  exact `AEGISFOE` bytes, the accepted-slot uncertainty boundary, exact-once definitive rollback,
  retained uncertain exposure, bounded `AEGISSTS` evidence, deterministic fake scripts, and the two
  named quality workloads.
- M1 `AEGISCFG`/`AEGISTRS` and M2 `AEGISRTS` remain compatibility anchors. M3 adds separately
  fingerprinted `AEGISRSP`, `AEGISSUP`, `AEGISFOE`, and `AEGISSTS` schemas instead of changing the
  older schemas.

## Implemented scope

The table maps each M3 scope area to the concrete local implementation and tests that prove it.

| Scope | Local implementation evidence |
|---|---|
| Canonical request and result | `order_request.hpp` and `submit_result.hpp`, collected by the `order_submission.hpp` aggregate include, define a bounded-field limit/GTC request and closed local result/reason/stage vocabulary. The request contains no caller-supplied organization, account, venue, or order identity, and the result has no exchange-acknowledgement disposition. |
| Route authorization | `submission_route.*` installs a canonical owner-local projection of sealed startup routes. `RouteId` selects the bot-owned account, venue, instrument, metadata, and provenance; subscriptions grant no execution authority. |
| Fixed risk | `risk_policy.*` canonically validates and fingerprints one immutable startup policy for bot, desk, firm, account, route, instrument, and venue scopes. `exposure.*` computes inverse quote face notional as quantity × contract multiplier at the policy scale with conservative `AwayFromZero` rounding; invalid scale or overflow rejects. `reservation_ledger.*` checks every proposed count/quantity/notional cell before one atomic commit. |
| Local identity and OMS | The canonical 24-byte `OrderId` is a 16-byte restart namespace plus unsigned 64-bit counter. The closed deterministic source emits its configured final counter exactly once, then maps permanent exhaustion to `OrderIdentityExhausted` before risk, reservation, or OMS mutation. A closed scripted source permits deliberate repeats for proof; OMS admission never retries them, gives duplicate identity precedence over bounded capacity, and records only the minimal M3 local lifecycle. The fail-closed production provider obtains its restart namespace from operating-system entropy but is not accepted by the fake direct-path variant. |
| Offline fake boundary | `fake_order_encoder.*` preserves the approved economics in exact `AEGISFOE` bytes. `fake_transport_initiator.*` is a final, fixed-capacity, in-memory accepted-slot analogue with no endpoint, credential, socket, or general transport interface. |
| Bot-bound coordinator | The runtime-private submission coordinator performs capability/evidence checks, route authorization, canonical validation, identity generation, risk, OMS, encoding, and fake initiation in one active owner turn. A single-use rollback guard distinguishes armed, released, and retained exposure. |
| Evidence and diagnostics | `submission_trace.*` owns bounded canonical `AEGISSTS`; `submission_diagnostics.*` owns a separate bounded noncanonical accepted prefix. Quiescent runtime evidence copies OMS rows, reservations, scope exposure, traces with terminal-result fields, diagnostics, and fake writes; the scenario strategy separately captures each synchronous returned `SubmitResult`. |
| Deterministic composition | `MarketRuntime::create_with_fake_submission` composes only closed in-memory M3 values. The original observation-only factory remains available and fails submission closed before identity or evidence mutation. |

## Exit-gate evidence

Each row names one required externally visible result or safety invariant. The complete scenario
drives the ordinary result matrix; focused unit, property, tooling, and benchmark tests prove the
construction and boundary cases that do not belong in one replay.

| Gate | Required proof | Local evidence |
|---|---|---|
| `M3-E01` | Unauthorized route rejection | Scenario attempt 1 returns `RouteNotOwned`. Route tests also cover not found, disabled, foreign bot/firm, instrument mismatch, canonical precedence, and multi-firm isolation. No risk, identity, OMS, encoder, initiator, or reservation mutation follows rejection. |
| `M3-E02` | Invalid canonical order rejection | Scenario attempts 2 and 3 return `PriceTickMismatch` and `QuantityScaleExceeded`. Request tests cover the complete limit/GTC/no-flags vocabulary, positive values, metadata precision, minimum quantity, lot step, and price tick—the complete M3 venue-filter set—without rounding or reinterpretation. Economics validation finishes before identity generation and risk. |
| `M3-E03` | Risk rejection | Scenario attempt 4 returns bot-scope `SingleOrderQuantityExceeded` before reservation or OMS. Ledger tests exercise all seven scopes and all six limit positions with atomic no-partial-mutation snapshots. |
| `M3-E04` | Duplicate local identity rejection | Scenario attempt 9 repeats an already admitted scripted identity and returns `DuplicateOrderIdentity`. The original OMS row and held reservation remain unchanged; no automatic identity retry occurs. |
| `M3-E05` | OMS non-admission | Scenario attempt 10 returns `OmsCapacityExceeded`. Focused tests pin duplicate-before-capacity precedence and prove release after the bounded admission decision. |
| `M3-E06` | Encoding failure | Scenario attempt 5 returns `EncodingFailed`, records a local-failure OMS row, performs no fake initiation, and releases its reservation exactly once. Encoder tests independently pin exact bytes and scripted-failure behavior. |
| `M3-E07` | Definitive fake-initiation failure | Scenario attempt 6 returns `InitiationDefinitelyFailed`. The initiator copies no accepted slot, the OMS records local failure, and the reservation is released exactly once. |
| `M3-E08` | Ambiguous post-initiation failure | Scenario attempt 7 copies one accepted slot and returns `SubmissionUnknown` with `InitiationOutcomeUnknown`. It is invoked once, is never retried, remains reconciliation-required, and retains quantity `3` and quote face notional `30`. |
| `M3-E09` | Successful fake initiation | Scenario attempt 8 returns only local `WriteInitiated`, retains quantity `4` and quote face notional `40`, and records one accepted fake write. No result, OMS state, or trace represents exchange acknowledgement or exchange acceptance; “accepted” in fake evidence means only that the in-memory slot was copied. |
| `M3-E10` | No reservation leak and exact-once release on definitive failures | Route/canonical/risk failures reserve nothing. Attempts 5, 6, 9, and 10 produce exactly four release transitions and four release diagnostics. Ledger and coordinator tests prove the single-use rollback right is consumed before the lower-level release call; a reservation cannot be released twice or partially subtracted. |
| `M3-E11` | Retained conservative exposure for uncertain outcomes | The final scenario holds only attempts 7 and 8: open count `2`, gross/worst quote notional `70`, and worst quantity `7` at all seven baseline-firm scopes. Every peer-firm scope remains zero. |
| `M3-E12` | Invalid or missing policy fails closed | Risk-policy construction rejects incomplete, duplicate, zero, inconsistent, stale-metadata, and forged-metadata inputs. Submission-capable runtime construction rejects absent/invalid capabilities or capacity allocation failure. Observation-only submission returns `SubmissionCapabilityUnavailable` without attempt, identity, or owner-local evidence mutation. |
| `M3-E13` | Worst-case exposure is never understated | Property/model tests use an independent 500-point inverse-contract ceiling grid, a 20-order alternating directional grid, same-currency two-instrument aggregation/release cases, every 7×6 limit position, and checked overflow cases. They prove conservative rounding, monotonic aggregation, and atomic rejection. |
| `M3-E14` | Direct path contains none of the forbidden hops or operations | Final fake and identity/clock types are closed; compile-time capability probes deny raw/live access; same-thread/same-turn tests prove the capability/evidence → route → validation → identity → risk → OMS → encode → fake-initiate order; the product link graph has no networking/database client; and a fail-closed manifest scanner audits the complete direct dependency closure plus tests, benchmarks, tools, CMake, presets, and CI. |
| `M3-E15` | Local success is never an exchange acknowledgement | The closed `SubmitResult` has only `LocallyRejected`, `WriteInitiated`, and `SubmissionUnknown`; compile-time probes prove there is no acknowledgement member. M4 owns acknowledgements and other exchange events. |
| `M3-E16` | `BENCH-M3-SUBMIT-001` reports the required metrics | The exact 10,000-sample record reports p50/p99/p99.9 and orders/s from the first bot-bound submit operation through the accepted fake-initiation endpoint. `allocations_per_order` separately covers the complete `context.submit_order()` call, including post-endpoint finalization, and is zero. The clean run is smoke only; no p99 qualification claim is made. |
| `M3-E17` | `BENCH-M3-SUBMIT-002` reports the required metrics | Workload 002 shares workload 001's value-identical request, configuration/organization, route/account/metadata, runtime policy, namespace, capacities, and scripts. Only its risk policy lowers the bot single-order-quantity limit to 1, changing the risk/submission fingerprints and rejecting before reservation/OMS. The exact 10,000-sample record reports p50/p99/p99.9, rejections/s, and allocations/request. The clean run is smoke only. |
| `M3-E18` | Timing excludes network and acknowledgement work and labels uncontrolled evidence honestly | The internal monotonic duration begins at `BotContext::submit_order` entry. Success ends immediately after accepted-slot copy; risk rejection ends at its completed local result. Fixture construction, preallocation, bootstrap, initial Ready callback, later OMS/trace finalization after the success endpoint, all network round trips, and acknowledgement timing are excluded. The manifest records all three host controls as `uncontrolled`, sets `evidence_classification` to `smoke`, and contains neither `qualification_reference` nor `threshold_claims`. |
| `M3-E19` | Context gates, bounded attempts, evidence preflight, and re-entry fail before unsafe mutation | Capability-absent, inactive/post-callback, and wrong-owner calls return before attempt or evidence mutation. The configured final `SubmissionAttemptId` is consumed once; the next outer call returns `SubmissionAttemptExhausted` with no new attempt, OrderId, duration, trace, or diagnostic. An owner-valid attempt preflights the fixed 11-record maximum before local OrderId generation or reservation; exhaustion returns `EvidenceCapacityExceeded` with a bounded noncanonical diagnostic and no reservation. The first nested submit consumes the outer attempt's reserved `ReentryRejected` slot; repeats coalesce into a saturating diagnostic count without creating a second attempt. Source-private, fixed-enum, one-shot probes prove that failed first re-entry evidence halts before local identity; failed `RiskReserved` evidence releases exactly once before OMS; and failed post-acceptance `WriteInitiated` or completion evidence returns `SubmissionUnknown`, retains exposure and the accepted fake write, moves authoritative OMS to `SubmissionUnknown`, and preserves the already accepted trace prefix without invention. Every later submit stops at the runtime latch without mutation. Focused coordinator, OMS, trace, diagnostic, and BotContext tests prove each branch. |

## Deterministic reference replay

`tests/deterministic_scenarios/m3_reference_scenario_test.cpp` executes the ten ordered attempts
above twice with a manual owner and once with the dedicated owner. All three runs compare the
complete callback, result, trace, diagnostic, OMS, reservation, risk-scope, encoded-write, clock,
identity, configuration, and policy evidence. The scenario executes 997 assertions and consumes
exactly 20 injected measurement-clock readings: entry and endpoint for each attempt, with a pinned
25-nanosecond local duration per attempt.

Pinned structural and canonical values are:

| Evidence | Expected value |
|---|---|
| Callback observations | 3: `Synchronizing` state, `Ready` state, then Ready market callback |
| Unrelated-bot callbacks | 0 |
| Submit results | 10: `RouteNotOwned`, `PriceTickMismatch`, `QuantityScaleExceeded`, `SingleOrderQuantityExceeded`, `EncodingFailed`, `InitiationDefinitelyFailed`, `SubmissionUnknown`, `WriteInitiated`, `DuplicateOrderIdentity`, `OmsCapacityExceeded` |
| M3 scenario `AEGISRTS` | 6 records; 2,494 bytes; SHA-256 `4d62d2c42bce7eeea8901b1c28baf4059190a21a46747f7fd70ef8fc67d71ed7` |
| `AEGISSTS` | 67 records; 35,616 bytes; SHA-256 `ace82ff42d02074d512f743f9c3b5d8dc040911e57031e1d438db2715780943d` |
| Enabled M3 configuration | `442dbeb26f2a1251f8badb9cff75e020940ad63d743e8b29175b50749793e908` |
| Scenario runtime policy | `78b64db91f7fc64914f8441cb5b883993c89b3f7e5b7105f3c0d3b02c74db2d5` |
| Scenario risk policy | `6fc0d0121e6b51fc103be03318d8b09ce1aba2f8c7da3b6afaa4765ba958ce9e` |
| Scenario submission policy | `eea132818dc108b9e0aafdcf5befc2dbc14c7332a6370b5ee22e80bae5aec737` |
| Accepted fake write 1 | 442 bytes; SHA-256 `4e9eae436a7db7e779a560ab2242e63c7816080376d8a59affbd21b2be789674` |
| Accepted fake write 2 | 442 bytes; SHA-256 `b19ac0749c2aaa1a55ee14c5b088e282cf83aa24a5379f3c270533af2ad79165` |

The accepted M1 disabled-route fixture and fingerprint
`e869459e338687fe372c4ee1c490a147e3c88261d3c2b89af4520cf990e35310` remain unchanged. The M3
scenario derives a separately fingerprinted enabled two-firm fixture solely for submission and
isolation proof; it does not rewrite the M1/M2 baseline or the standalone M2 replay golden.

## Named quality workload smoke evidence

The clean producer generated `benchmark-results/m3-submission.json` and
`benchmark-results/m3-context.json` with the exact anchored two-record filter. Both records use one
thread, one repetition, 10,000 manual-time samples, and microsecond units.

| Workload | Smoke observation at the clean producer |
|---|---|
| `BENCH-M3-SUBMIT-001/submission.authorized-limit-fake-initiation/iterations:10000/manual_time` | p50 19.291 µs; p99 27.459 µs; p99.9 67.625 µs; 51,143.569 orders/s; 0 allocations/order |
| `BENCH-M3-SUBMIT-002/submission.inline-risk-rejection/iterations:10000/manual_time` | p50 3.583 µs; p99 6.625 µs; p99.9 18.375 µs; 269,480.409 rejections/s; 0 allocations/request |

These numbers are observations, not qualification results. The context records `uncontrolled` for
power mode, host isolation, and thermal state. Therefore the validator correctly emits no claim
against the provisional 50-microsecond and 25-microsecond p99 limits, even where an observed number
is numerically below a limit. CI benchmark runs are also always smoke.

The clean context records Release/Ninja, AppleClang 16, CMake 3.31.10, Ninja 1.13.0, Google
Benchmark v1.9.5, macOS 15.7.4, MacBookPro18,2, Apple M1 Max, 32 GiB memory, and ten
logical/physical cores. Its revision and integrity values are:

| Artifact | SHA-256 or identity |
|---|---|
| Benchmark executable | `b145519f6063bc5c5265378993576be141aa87291f948f08cbac42ef522fbbc4` |
| `m3-submission.json` | `0630b40b720ca609bce81adb4270b7c3facf18b10415ea7be2e78b70c4816b03` |
| `m3-context.json` | `3f8763f034b15e8202d24c9cc60ed522c20f367e44c29b7ec1f70313a2a1683c` |
| Configuration fingerprint | `442dbeb26f2a1251f8badb9cff75e020940ad63d743e8b29175b50749793e908` |
| Runtime-policy fingerprint | `c0a346eb14ab4a5523fc9590313fb155bf933f69e8864e57fd8241aec056deee` |
| Success risk/submission policies | `6fc0d0121e6b51fc103be03318d8b09ce1aba2f8c7da3b6afaa4765ba958ce9e` / `89b38a5b7bdcfe0bb2078cd88c0f44b82efd266208246b9e141a365298f9d372` |
| Rejection risk/submission policies | `bd3a62561f7f68c2798d43dca54208e5e6b9abbfb90c4873d2b946823976e5a7` / `f8458d2dc5ba8bf994077da8a575d22dfee33fe12e362f894888ae7585d85e37` |
| Configuration / organization / route / metadata / risk revisions | `1` / `1` / `1` / `1` / `1` for both workloads |
| Route / account | `route.deribit-testnet-btc-perpetual` / `account.deribit-testnet-aegis` |
| Venue / normalized instrument | `deribit` / `BTC-USD-PERPETUAL` |
| Order namespace | `000102030405060708090a0b0c0d0e0f` |

The two fixtures share the value-identical quantity-2 request, configuration, M2 runtime policy,
route/account/venue/instrument metadata, order namespace, capacities, and fake scripts. Only the
risk and derived submission-policy fingerprints differ: the rejection policy lowers the bot
`SingleOrderQuantity` limit to 1, making it the first and only failing check before OMS.

## Verification record

The clean producer passed the following local matrix:

| Check | Result |
|---|---|
| Normal warning-as-error configure, build, and CTest | 237/237 |
| Catch2 unit suite | 230 cases; 7,065 assertions |
| Deterministic scenarios | 4 CTest cases: M1 has 2 cases/70 assertions; M2 has 1/471; M3 has 1/997 |
| Tooling/forbidden CTest entries | 3/3 |
| ASan+UBSan workflow | 237/237; no finding |
| TSan workflow | 237/237; no finding |
| Release benchmark workflow | 237/237 |
| clang-format 18.1.8 check and `git diff --check` | Pass |
| Forbidden-capability live scan | Pass with 0 findings |
| Forbidden-capability scanner self-tests | 18/18 |
| M3 benchmark-evidence tooling tests | 3/3 |
| M0, M2, and M3 collectors plus independent validators | Pass; M0 is calibration smoke by policy, while the M2 and M3 manifests explicitly classify their bundles as smoke |

The reproducible commands are:

```sh
cmake --workflow --preset verify
cmake --preset format
cmake --build --preset format --target format-check
cmake --workflow --preset verify-asan-ubsan
cmake --workflow --preset verify-tsan
cmake --workflow --preset benchmark
python3 tools/check_forbidden_capabilities.py
python3 -m unittest tests/tooling/forbidden_capabilities_test.py
python3 -m unittest tests/tooling/m3_benchmark_evidence_test.py
python3 tools/run_benchmarks.py --suite m0
python3 tools/validate_benchmark_evidence.py --suite m0
python3 tools/run_benchmarks.py --suite m2
python3 tools/validate_benchmark_evidence.py --suite m2
python3 tools/run_benchmarks.py --suite m3
python3 tools/validate_benchmark_evidence.py --suite m3
git diff --check
```

## Forbidden-capability proof

No single check can prove the offline boundary by itself, so M3 uses five mutually reinforcing
layers:

1. `DeterministicFakeOrderEncoder`, `DeterministicFakeWriteInitiator`, both submission measurement
   clocks, and both deterministic identity sources are closed repository-owned types. The fake
   stack interfaces and `FakeSubmissionRuntimeParams` accept no executable callback, endpoint,
   session, credential, or transport type; ordinary registered strategy callbacks remain outside
   those fake component interfaces.
2. Compile-time probes reject endpoint, credential, socket, send/connect/retry members on the fakes;
   raw route/account/encoder/initiator/socket/credential access on `BotContext`; caller
   firm/bot/account/venue/order identity on `OrderRequest`; and exchange acknowledgement on
   `SubmitResult`.
3. The product link graph has no networking, database, remote-service, or external serialization
   dependency. The build scan allow-lists exact checksum-pinned Catch2 and Google Benchmark archive
   acquisitions for tests and rejects revision/hash drift, unapproved packages, subdirectories,
   link inputs, link options, and raw library flags.
4. Coordinator and integrated runtime tests prove the required route → validation → identity →
   risk → OMS → encode → fake-initiate order in one active thread/turn with no owner admission or
   queue change.
   The direct-path scan rejects executor `InlineCommandWorkItem`/`try_admit`, queues, futures,
   coroutines,
   blocking synchronization, files, databases, networking, DNS, and remote calls across the complete
   synchronous dependency closure.
5. The manifest-backed scanner covers M3 production, unit tests, support fixtures, deterministic
   scenario, benchmark code, Python tooling, CMake, presets, and CI. It fails closed when an assigned
   path or root is missing, and its 18 self-tests exercise signatures, scope, exact negative probes,
   pinned exceptions, dependency drift, and manifest completeness.

No credential, DNS lookup, socket, HTTP/WebSocket call, live venue connection, authentication,
private session, or real order transmission is present in M3.

## Deferred capabilities

The table makes later ownership explicit so the M3 evidence cannot be mistaken for networking,
private-event, recovery, or operator capability.

| Owner | Capability that remains deferred |
|---|---|
| M4 | Exchange acknowledgements, rejections, fills and cancellations; private-event OMS reconciliation; confirmed inventory/positions; complete reservation lifecycle; fake crash/replay and recovery contracts; and the longitudinal one-shot reference-intent driver. |
| M5 | Dynamic risk allocation/publication, authority epochs and freshness, `Normal`/`ReduceOnly`/`Halted`, market-readiness enforcement, and risk control-plane observations/backpressure. |
| M6 | Real public networking, public venue protocol, and public adapter lifecycle. |
| M7 | Credentials, authenticated private observation/reconciliation sessions, and authoritative account identity/query mapping. |
| M8 | Venue-native order encoding, real sandbox transmission, session-local write sequencing, rate limits, cancel priority, and explicit transmission arming. |
| M9 | Durable journal, snapshots, audit storage, and venue-backed restart/recovery. |
| M10 | Load, backlog, fault, and soak qualification. |
| M11 | Operator plane, dynamic configuration adoption, deployment, and runbooks. |
| M12 | Shadow/paper qualification and deterministic P&L/reporting semantics. |
| M13 | Second venue and cross-venue behavior. |
| M14 | Combined production qualification only; any live pilot still requires a separate audited human authorization. |
| Later explicit decisions | Market/trigger orders, more TIFs, venue flags, collars, linear-contract risk, more venue filters, cross-currency aggregation, and cross-firm limits. |

## Publication and integration status

These facts distinguish the feature branch, reviewed producer, pull request, merge revision, and
historical M3 evidence boundary.

- M3 was created once from the verified M2 `dev` merge baseline and is not stacked on the old M2
  feature branch.
- The old `codex/m2-deterministic-runtime` branch and its commits were not modified.
- The M3 branch contains multiple coherent, independently understandable commits, with the clean
  implementation and benchmark producer recorded above.
- Publication was explicitly authorized on 2026-08-22; the branch is pushed and PR #10 targets
  `dev`, never `main`.
- Remote CI and review status are tracked on PR #10 and do not rewrite the local producer evidence.
- PR #10 merged final feature head `2d5ea9e5b7fc28234789dc7c97ec4fc7bb71ef01` into `dev` as
  `962eb8602c13c1930a74c59232f96920482edb2b` on 2026-08-23.
- The merge tree exactly matches the final feature tree, so integration introduced no content
  change. The repository's integrated capability baseline is M3.
