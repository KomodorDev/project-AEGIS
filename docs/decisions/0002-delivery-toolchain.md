# ADR-0002: Adopt the Initial Delivery Toolchain

- **Status:** Accepted
- **Date:** 2026-08-18
- **Scope:** M0 development, build, test, formatting, and measurement baseline
- **Related:** [Implementation roadmap](../implementation-roadmap.md),
  [repository layout](../architecture/repository-layout.md)

## Context

AEGIS needs a repeatable baseline before domain or exchange behavior is added. A build that varies by
developer machine would make later determinism and latency evidence untrustworthy. The baseline must
also avoid selecting networking, asynchronous-I/O, persistence, or production deployment technology
before a concrete milestone needs it.

## Decision

### Language and supported toolchains

AEGIS v1 uses C++20 with compiler extensions disabled. C++20 has sufficiently mature support across
the initial Linux and macOS compilers; C++23 library support is not yet consistent enough across that
matrix to justify raising the floor.

The initial supported matrix is:

| Platform | Compiler floor | Role |
|---|---:|---|
| macOS 15 | AppleClang 16 | Primary Apple Silicon development platform and macOS CI |
| Ubuntu 24.04 | GCC 13 | Linux build/test CI |
| Ubuntu 24.04 | Clang 18 | Linux build/test, sanitizer, and benchmark CI |

Unsupported compilers fail during configuration instead of silently compiling with a weaker policy.
Windows, other operating systems, and older compiler versions are outside the M0 support promise.

### Build and dependency acquisition

CMake 3.25 or newer is the build-system interface and Ninja is the reference generator. Python 3.11
or newer runs development-tool installation and benchmark evidence collection; it is not a runtime
dependency of AEGIS. CI pins CMake 3.31.10 and Ninja 1.13.0. `CMakePresets.json` is the canonical set
of configuration, build, test, and workflow commands. `cmake --workflow --preset verify` is the local
and CI build/test entry point.

M0 has no runtime third-party dependency. Test-only dependencies use CMake `FetchContent` with an
immutable upstream commit archive and a SHA-256 checksum:

| Dependency | Selected version | Purpose |
|---|---:|---|
| Catch2 | 3.9.1 | Unit-test assertions and discovery through CTest |
| Google Benchmark | 1.9.5 | Minimal benchmark runner and machine-readable output |

An upgrade must change both the immutable source reference and checksum in a reviewed task. A
project package manager is deferred until the first runtime dependency creates a real need.

Headers consumed by more than one repository target begin under `include/aegis`; implementation-only
files remain under `src/aegis`. This is a source-build boundary, not a promise of an installed API or
stable ABI. That public packaging boundary remains deferred until a real consumer needs it.

### Quality policy

- Strict warning flags apply only to AEGIS-owned targets, and warnings are errors by default.
- clang-format 18.1.8 defines the C++ formatting gate; Ruff 0.16.3 formats and lints Python tooling.
- CI exercises AppleClang, GCC, and Clang builds.
- AddressSanitizer and UndefinedBehaviorSanitizer run together in one job; ThreadSanitizer runs in an
  isolated job because it is incompatible with AddressSanitizer.
- Benchmarks are built in every normal verification and smoke-run in a release profile. Shared CI
  timing is retained as evidence but is not an absolute performance gate.
- GitHub Actions and source dependencies are pinned to immutable revisions or content hashes.

## Consequences

- A developer needs a supported compiler, CMake, and Ninja before the one-command workflow can run.
- The first configure requires internet access to acquire checksum-verified test dependencies; later
  builds can reuse the ignored build-tree cache.
- C++23-only features require either a later accepted revision or a compatibility implementation.
- The build baseline remains small and does not prejudge networking, serialization, storage, or
  deployment libraries.
- Every new AEGIS target must opt into the common language, warning, and sanitizer interface.

## Revisit triggers

Revisit this decision when a required production dependency cannot be managed safely with the chosen
approach, a supported platform requires a different generator, or all required toolchains provide a
material C++23 capability whose benefit outweighs the migration cost.
