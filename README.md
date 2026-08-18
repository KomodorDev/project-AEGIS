# AEGIS

> **Purpose:** Give a new contributor the shortest safe path to understand, configure, verify, and
> benchmark the M0 repository.

AEGIS (Asynchronous Exchange Gateway and Inventory System) is a deterministic trading and risk
engine under development. The initial delivery slice is deliberately narrow: one Deribit testnet
account, one BTC inverse perpetual, one bot, and no production connectivity.

The accepted system design is in [the architecture overview](docs/architecture.md). Delivery is
organized by capability gates in [the implementation roadmap](docs/implementation-roadmap.md).

## M0 quick start

The supported baseline is C++20 with CMake 3.25 or newer and Ninja. Python 3.11 or newer runs the
developer bootstrap and benchmark-evidence tooling. CI pins CMake 3.31.10 and Ninja 1.13.0. The
complete platform/compiler policy is recorded in
[ADR-0002](docs/decisions/0002-delivery-toolchain.md).

An optional isolated installation of the pinned developer tools is:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.txt
source .venv/bin/activate
```

The tracked VS Code workspace setting points CMake Tools at this `.venv` and prepends its `bin`
directory so the matching Ninja is found. Create the environment before using CMake Tools; command-line
users may instead provide any supported CMake and Ninja installation through `PATH`.

From a fresh checkout, the one verification command is:

```sh
cmake --workflow --preset verify
```

That command configures, builds, and runs the unit tests with strict warnings treated as errors.
The first configure downloads immutable, checksum-verified Catch2 and Google Benchmark source
archives into the ignored build tree. It does not contact an exchange or require credentials.

The `--workflow --preset` form selects an ordered configure/build/test workflow. A plain `--preset`
selects one configure preset, while `--build --preset` selects a previously configured build preset.

Other quality commands are:

```sh
cmake --preset format
cmake --build --preset format --target format-check
cmake --workflow --preset verify-asan-ubsan
cmake --workflow --preset verify-tsan
cmake --workflow --preset benchmark
python3 tools/run_benchmarks.py
python3 tools/validate_benchmark_evidence.py
```

The benchmark runner first rebuilds its Release target, then writes raw timing output and a context
manifest. The validator independently checks their workload identity, build mode, hashes, and Git
provenance; neither command is a claim of production latency.

The default build contains no exchange session, order-transmission capability, credential lookup,
or production endpoint. The first market and account assumptions are described in
[the reference scenario](docs/reference-scenario.md).

## M0 records

- [Delivery and toolchain decision](docs/decisions/0002-delivery-toolchain.md)
- [Dedicated testnet account policy](docs/decisions/0003-dedicated-testnet-account.md)
- [Deribit BTC perpetual reference scenario](docs/reference-scenario.md)
- [Correctness and performance budgets](docs/quality-budgets.md)
- [Deribit public-protocol spike](docs/protocol-spikes/deribit-btc-perpetual.md)
- [M0 exit evidence](docs/milestones/m0-exit-evidence.md)
