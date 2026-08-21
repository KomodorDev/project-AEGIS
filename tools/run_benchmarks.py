#!/usr/bin/env python3

"""Run one named benchmark suite and record enough context to reproduce its result."""

# Interesting syntax: postponed annotations allow modern type hints without eager evaluation.
from __future__ import annotations

# Standard-library modules cover CLI parsing, hashing, host discovery,
# subprocesses, and JSON output.
import argparse
import hashlib
import json
import math
import os
import platform
import re
import subprocess
from collections.abc import Sequence
from datetime import UTC, datetime
from functools import cache
from pathlib import Path

# Stable suite identities and exact filters prevent future benchmark registrations from leaking into
# an evidence bundle owned by another milestone.
M0_WORKLOAD_ID = "BENCH-M0-HARNESS-001"
M0_RUN_NAME = f"{M0_WORKLOAD_ID}/harness.noop"
M0_BENCHMARK_FILTER = r"^BENCH-M0-HARNESS-001/harness\.noop$"
M2_SAMPLE_COUNT = 10_000
M2_BASE_RUN_NAMES = (
    "BENCH-M2-EXEC-001/executor.admit-and-run-noop",
    "BENCH-M2-CALLBACK-001/strategy.reference-ready-view",
    "BENCH-M2-MD-001/market.delta-20x20-and-callback",
)
M2_WORKLOAD_IDS = tuple(run_name.split("/", 1)[0] for run_name in M2_BASE_RUN_NAMES)
M2_RUN_NAMES = tuple(
    f"{run_name}/iterations:{M2_SAMPLE_COUNT}/manual_time" for run_name in M2_BASE_RUN_NAMES
)
M2_BENCHMARK_FILTER = (
    "^(" + "|".join(run_name.replace(".", r"\.") for run_name in M2_RUN_NAMES) + ")$"
)

# Only these exact operator assertions, together with exact REF-MAC-01 hardware/tool facts,
# authorize an absolute callback-latency qualification. Free-form descriptions remain smoke.
CONTROLLED_POWER_MODE = "ac-low-power-mode-disabled"
CONTROLLED_HOST_ISOLATION = "no-concurrent-build-or-test-load"
CONTROLLED_THERMAL_STATE = "no-thermal-pressure"
M2_FINGERPRINT_LABEL_PATTERN = re.compile(
    r"configuration_fingerprint_sha256=([0-9a-f]{64});"
    r"runtime_policy_fingerprint_sha256=([0-9a-f]{64})"
)


# --------------------------------------------------------
# Runs one required subprocess and returns normalized standard output.
def command_output(arguments: Sequence[str], *, cwd: Path | None = None) -> str:
    """Run a command successfully and return its standard output without surrounding whitespace."""

    # check=True converts a nonzero exit into an exception; capture_output keeps evidence concise.
    completed = subprocess.run(
        arguments,
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


# --------------------------------------------------------
# Extracts one required configuration value from a CMake cache.
def cache_value(cache_path: Path, name: str) -> str:
    """Read one named value from CMake's `NAME:TYPE=value` cache representation."""

    # Match the full cache-key prefix, then split only once so values may themselves contain `=`.
    prefix = f"{name}:"
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1]
    raise RuntimeError(f"{name} is absent from {cache_path}")


# --------------------------------------------------------
# Produces a deterministic digest of every tracked and relevant untracked change.
def worktree_fingerprint(repository: Path) -> str:
    """Hash tracked changes plus every non-ignored untracked path and its current content."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Git's binary diff covers staged and unstaged tracked changes while disabling user diff helpers
    # that could make the byte stream depend on local configuration.
    tracked = subprocess.run(
        [
            "git",
            "diff",
            "--binary",
            "--no-color",
            "--no-ext-diff",
            "--no-textconv",
            "HEAD",
            "--",
        ],
        cwd=repository,
        check=True,
        capture_output=True,
    ).stdout

    # ++++++++++++++++++++++++++++++++++++++++
    # Git supplies NUL-delimited raw path bytes so unusual but valid filenames remain unambiguous.
    untracked_output = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard", "-z"],
        cwd=repository,
        check=True,
        capture_output=True,
    ).stdout
    untracked_paths = sorted(path for path in untracked_output.split(b"\0") if path)

    # ++++++++++++++++++++++++++++++++++++++++
    # Length prefixes keep path/content boundaries unambiguous even for arbitrary file bytes.
    digest = hashlib.sha256()
    digest.update(b"AEGIS worktree fingerprint v1\0")
    digest.update(len(tracked).to_bytes(8, byteorder="big"))
    digest.update(tracked)
    for raw_path in untracked_paths:
        path = repository / os.fsdecode(raw_path)
        if path.is_symlink():
            kind = b"symlink"
            content = os.fsencode(os.readlink(path))
        elif path.is_file():
            kind = b"executable" if path.stat().st_mode & 0o111 else b"file"
            content = path.read_bytes()
        else:
            raise RuntimeError(f"unsupported untracked path type: {path}")
        for field in (raw_path, kind, content):
            digest.update(len(field).to_bytes(8, byteorder="big"))
            digest.update(field)
    return digest.hexdigest()

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Reads optional macOS host metadata without making its absence fatal.
def sysctl_value(name: str) -> str | None:
    """Return a macOS sysctl value, or None when the key/command is unavailable."""

    # Host metadata is best-effort: unsupported probes must not invalidate benchmark results.
    try:
        return command_output(["sysctl", "-n", name])
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None


# --------------------------------------------------------
# Reads only non-sensitive hardware facts from macOS's fallback profiler.
@cache
def system_profiler_hardware() -> dict[str, str]:
    """Return model, chip, and core-count fields without retaining hardware identifiers."""

    # Sandboxed macOS processes may be denied sysctl while the minimal profiler remains available.
    # The allowlist deliberately excludes serial numbers, provisioning IDs, and hardware UUIDs.
    try:
        output = command_output(["system_profiler", "SPHardwareDataType", "-detailLevel", "mini"])
    except (FileNotFoundError, subprocess.CalledProcessError):
        return {}
    allowed_fields = {"Model Identifier", "Chip", "Total Number of Cores"}
    fields: dict[str, str] = {}
    for line in output.splitlines():
        key, separator, value = line.strip().partition(":")
        if separator and key in allowed_fields and value.strip():
            fields[key] = value.strip()
    return fields


# --------------------------------------------------------
# Selects the most specific CPU identity exposed by the current operating system.
def cpu_model() -> str:
    """Return the most specific CPU model string available on macOS or Linux."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Apple exposes CPU identity through sysctl rather than Linux's /proc virtual filesystem.
    if platform.system() == "Darwin":
        return (
            sysctl_value("machdep.cpu.brand_string")
            or system_profiler_hardware().get("Chip")
            or sysctl_value("hw.model")
            or "unknown"
        )

    # ++++++++++++++++++++++++++++++++++++++++
    # Linux usually records a human-readable model once per logical processor in /proc/cpuinfo.
    cpu_info = Path("/proc/cpuinfo")
    if cpu_info.exists():
        for line in cpu_info.read_text(encoding="utf-8").splitlines():
            if line.startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()

    # ++++++++++++++++++++++++++++++++++++++++
    # Fall back to Python's portable probe and an explicit sentinel on other operating systems.
    return platform.processor() or "unknown"

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Resolves a hardware model independently of the machine's user-assigned name.
def host_model() -> str:
    """Return the host's hardware model rather than its user-assigned network name."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Apple publishes the machine model directly through sysctl.
    if platform.system() == "Darwin":
        return sysctl_value("hw.model") or system_profiler_hardware().get(
            "Model Identifier", "unknown"
        )

    # ++++++++++++++++++++++++++++++++++++++++
    # Probe Linux hardware-description files in their preferred order.
    for model_path in (
        Path("/sys/devices/virtual/dmi/id/product_name"),
        Path("/sys/firmware/devicetree/base/model"),
    ):
        try:
            model = model_path.read_text(encoding="utf-8").strip("\x00\n ")
        except (FileNotFoundError, OSError, UnicodeError):
            continue
        if model:
            return model

    # ++++++++++++++++++++++++++++++++++++++++
    # Fall back to the portable node name when no hardware model is available.
    return platform.node() or "unknown"

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Determines total physical memory through the current platform's native facilities.
def total_memory_bytes() -> int:
    """Return installed/visible physical memory in bytes using operating-system facilities."""

    # ++++++++++++++++++++++++++++++++++++++++
    # macOS exposes installed physical memory directly through sysctl.
    if platform.system() == "Darwin":
        value = sysctl_value("hw.memsize")
        if value:
            return int(value)

    # ++++++++++++++++++++++++++++++++++++++++
    # Use portable POSIX page metrics and fail explicitly when the host exposes neither source.
    try:
        return int(os.sysconf("SC_PHYS_PAGES")) * int(os.sysconf("SC_PAGE_SIZE"))
    except (OSError, ValueError):
        pass
    raise RuntimeError("unable to determine total physical memory")

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Counts physical processor cores when platform topology data is available.
def physical_core_count() -> int | None:
    """Count physical CPU cores when the host exposes topology without extra dependencies."""

    # ++++++++++++++++++++++++++++++++++++++++
    # macOS reports the total directly.
    if platform.system() == "Darwin":
        value = sysctl_value("hw.physicalcpu")
        if value:
            return int(value)
        profiler_value = system_profiler_hardware().get("Total Number of Cores")
        if profiler_value:
            try:
                return int(profiler_value.split("(", 1)[0].strip())
            except ValueError:
                pass
        return None

    # ++++++++++++++++++++++++++++++++++++++++
    # Linux lscpu emits CORE,SOCKET pairs; unique pairs distinguish cores across CPU sockets.
    try:
        rows = command_output(["lscpu", "--parse=CORE,SOCKET"])
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None

    # ++++++++++++++++++++++++++++++++++++++++
    # Interesting syntax: this set comprehension both filters comments and removes duplicates.
    cores = {
        line for line in rows.splitlines() if line and not line.startswith("#") and line != "-,-"
    }
    return len(cores) or None

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Determines whether recorded facts exactly identify the controlled reference host.
def qualifies_as_ref_mac_01(context: dict[str, object]) -> bool:
    """Return whether context truthfully satisfies every controlled REF-MAC-01 condition."""

    # A free-form approximation never authorizes a threshold claim: every hardware, toolchain, and
    # operator-controlled condition must equal the published reference contract.
    return (
        str(context.get("operating_system", "")).startswith("macOS-15.")
        and context.get("host_model") == "MacBookPro18,2"
        and context.get("machine") == "arm64"
        and context.get("cpu_model") == "Apple M1 Max"
        and context.get("total_memory_bytes") == 32 * 1024 * 1024 * 1024
        and context.get("logical_core_count") == 10
        and context.get("physical_core_count") == 10
        and str(context.get("compiler", "")).startswith("Apple clang version 16.")
        and context.get("power_mode") == CONTROLLED_POWER_MODE
        and context.get("host_isolation") == CONTROLLED_HOST_ISOLATION
        and context.get("thermal_state") == CONTROLLED_THERMAL_STATE
    )


# --------------------------------------------------------
# Extracts the sole threshold-bearing observation from a raw M2 result.
def callback_p99_microseconds(raw_result: dict[str, object]) -> float:
    """Return the finite nonnegative callback p99 counter needed for a qualification claim."""

    # The validator performs the complete schema audit; generation needs only one exact record and
    # one numeric counter to create an eligible controlled-host claim.
    benchmarks = raw_result.get("benchmarks")
    if not isinstance(benchmarks, list):
        raise RuntimeError("M2 benchmark result has no benchmark list")
    callback_run_name = M2_RUN_NAMES[1]
    matches = [
        record
        for record in benchmarks
        if isinstance(record, dict) and record.get("run_name") == callback_run_name
    ]
    if len(matches) != 1:
        raise RuntimeError("M2 benchmark result has no unique callback record")
    value = matches[0].get("p99_us")
    if (
        not isinstance(value, (int, float))
        or isinstance(value, bool)
        or not math.isfinite(value)
        or value < 0
    ):
        raise RuntimeError("M2 callback record has no nonnegative p99_us counter")
    return float(value)


# --------------------------------------------------------
# Extracts and cross-checks the immutable configuration identities emitted by every M2 workload.
def m2_fingerprints(raw_result: dict[str, object]) -> tuple[str, str]:
    """Return one identical valid configuration/runtime-policy fingerprint pair from all M2 runs."""

    # Each result is independently attributable; requiring equality prevents a mixed raw bundle
    # from borrowing provenance from a different runtime fixture.
    benchmarks = raw_result.get("benchmarks")
    if not isinstance(benchmarks, list):
        raise RuntimeError("M2 benchmark result has no benchmark list")
    observed: list[tuple[str, str]] = []
    for record in benchmarks:
        if not isinstance(record, dict) or record.get("run_name") not in M2_RUN_NAMES:
            continue
        label = record.get("label")
        if not isinstance(label, str):
            raise RuntimeError("M2 benchmark record has no configuration fingerprint label")
        match = M2_FINGERPRINT_LABEL_PATTERN.fullmatch(label)
        if match is None:
            raise RuntimeError("M2 benchmark record has an invalid configuration fingerprint label")
        observed.append((match.group(1), match.group(2)))
    if len(observed) != len(M2_RUN_NAMES) or len(set(observed)) != 1:
        raise RuntimeError("M2 benchmark records do not share one fingerprint pair")
    return observed[0]


# --------------------------------------------------------
# Parses the suite and output destination while retaining the no-argument M0 contract.
def parse_arguments() -> argparse.Namespace:
    """Select M0 by default or one explicit suite and repository-relative evidence directory."""

    # Resolving from __file__ makes the default independent of the caller's current directory.
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--suite",
        choices=("m0", "m2"),
        default="m0",
        help="benchmark evidence suite to run (default: m0)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=repository / "benchmark-results",
    )
    return parser.parse_args()


# --------------------------------------------------------
# Builds, runs, and records the selected benchmark suite and its provenance.
def main() -> int:
    """Build first, run exactly one suite, and write raw results plus a context manifest."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Derive all fixed inputs from the repository and the release preset's stable output paths.
    arguments = parse_arguments()
    suite = arguments.suite
    repository = Path(__file__).resolve().parents[1]
    binary = (repository / "build/release/benchmarks/aegis_benchmarks").resolve()
    results_dir = arguments.results_dir.resolve()
    cache_path = repository / "build/release/CMakeCache.txt"

    # ++++++++++++++++++++++++++++++++++++++++
    # The release cache is the source of truth for the actual generator, tools, and source tree.
    if not cache_path.is_file():
        raise FileNotFoundError(f"release configuration not found: {cache_path}")
    build_type = cache_value(cache_path, "CMAKE_BUILD_TYPE")
    if build_type != "Release":
        raise RuntimeError(f"release preset cache has unexpected build type: {build_type}")
    configured_source = Path(cache_value(cache_path, "CMAKE_HOME_DIRECTORY")).resolve()
    if configured_source != repository:
        raise RuntimeError(
            f"release cache belongs to {configured_source}, not this repository: {repository}"
        )

    # ++++++++++++++++++++++++++++++++++++++++
    # Rebuild the exact benchmark target before measuring so stale or debug executables cannot be
    # mislabeled as evidence for the current source tree. The cache supplies the configured CMake.
    cmake = cache_value(cache_path, "CMAKE_COMMAND")
    build_command = [cmake, "--build", "--preset", "release", "--target", "aegis_benchmarks"]
    subprocess.run(build_command, cwd=repository, check=True)
    if not binary.is_file():
        raise FileNotFoundError(f"benchmark executable not found after release build: {binary}")

    # ++++++++++++++++++++++++++++++++++++++++
    # Keep suite-specific machine-readable timing data and its provenance manifest beside one
    # another without changing the established no-argument M0 filenames.
    results_dir.mkdir(parents=True, exist_ok=True)
    if suite == "m0":
        benchmark_path = results_dir / "m0-harness.json"
        context_path = results_dir / "m0-context.json"
    else:
        benchmark_path = results_dir / "m2-runtime.json"
        context_path = results_dir / "m2-context.json"

    # ++++++++++++++++++++++++++++++++++++++++
    # Give each suite an exact anchored filter. M0 retains its short repeated calibration; M2 owns
    # fixed 10,000-iteration workloads so each tail percentile has enough observations.
    if suite == "m0":
        benchmark_command = [
            str(binary),
            f"--benchmark_filter={M0_BENCHMARK_FILTER}",
            "--benchmark_min_time=0.01s",
            "--benchmark_repetitions=3",
            "--benchmark_report_aggregates_only=true",
            f"--benchmark_out={benchmark_path}",
            "--benchmark_out_format=json",
        ]
    else:
        benchmark_command = [
            str(binary),
            f"--benchmark_filter={M2_BENCHMARK_FILTER}",
            f"--benchmark_out={benchmark_path}",
            "--benchmark_out_format=json",
        ]
    subprocess.run(benchmark_command, cwd=repository, check=True)

    # ++++++++++++++++++++++++++++++++++++++++
    # Read tool locations and raw library metadata from the build/result rather than assumptions.
    compiler = cache_value(cache_path, "CMAKE_CXX_COMPILER")
    build_tool = cache_value(cache_path, "CMAKE_MAKE_PROGRAM")
    raw_result = json.loads(benchmark_path.read_text(encoding="utf-8"))
    raw_context = raw_result.get("context")
    if not isinstance(raw_context, dict):
        raise RuntimeError("benchmark result has no JSON context object")
    library_version = raw_context.get("library_version")
    library_build_type = raw_context.get("library_build_type")
    if not isinstance(library_version, str) or not library_version:
        raise RuntimeError("benchmark result has no library version")
    if not isinstance(library_build_type, str) or library_build_type.lower() != "release":
        raise RuntimeError(f"benchmark library is not a release build: {library_build_type}")

    # ++++++++++++++++++++++++++++++++++++++++
    # Fingerprint both the repository view and the produced files after the synchronized build.
    git_status = command_output(["git", "status", "--porcelain"], cwd=repository)
    benchmark_hash = hashlib.sha256(benchmark_path.read_bytes()).hexdigest()
    executable_hash = hashlib.sha256(binary.read_bytes()).hexdigest()

    # ++++++++++++++++++++++++++++++++++++++++
    # Capture the common fields required to interpret and reproduce either benchmark result.
    context = {
        "schema_version": 1,
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "git_revision": command_output(["git", "rev-parse", "HEAD"], cwd=repository),
        "git_head_tree": command_output(["git", "rev-parse", "HEAD^{tree}"], cwd=repository),
        "git_worktree_dirty": bool(git_status),
        "git_worktree_fingerprint_sha256": worktree_fingerprint(repository),
        "build_preset": "release",
        "build_type": build_type,
        "build_system": cache_value(cache_path, "CMAKE_GENERATOR"),
        "compiler": command_output([compiler, "--version"]).splitlines()[0],
        "cmake": command_output([cmake, "--version"]).splitlines()[0],
        "build_tool": command_output([build_tool, "--version"]).splitlines()[0],
        "google_benchmark_library_version": library_version,
        "operating_system": platform.platform(),
        "host_model": host_model(),
        "machine": platform.machine(),
        "cpu_model": cpu_model(),
        "total_memory_bytes": total_memory_bytes(),
        "logical_core_count": os.cpu_count(),
        "physical_core_count": physical_core_count(),
        "power_mode": os.environ.get("AEGIS_BENCHMARK_POWER_MODE", "uncontrolled"),
        "host_isolation": os.environ.get("AEGIS_BENCHMARK_HOST_ISOLATION", "uncontrolled"),
        "thermal_state": os.environ.get("AEGIS_BENCHMARK_THERMAL_STATE", "uncontrolled"),
        "build_command": build_command,
        "benchmark_command": benchmark_command,
        "benchmark_executable": str(binary),
        "benchmark_executable_sha256": executable_hash,
        "benchmark_result": benchmark_path.name,
        "benchmark_result_sha256": benchmark_hash,
    }

    # ++++++++++++++++++++++++++++++++++++++++
    # Preserve schema-one M0 fields exactly while adding an explicit M2 suite identity and fixed
    # sample/warm-up contract only when that suite was requested.
    if suite == "m0":
        context.update(
            {
                "workload_id": M0_WORKLOAD_ID,
                "sample_count": 3,
                "warm_up_policy": (
                    "Google Benchmark adaptive iteration calibration; no separate warm-up; "
                    "minimum measured time 0.01 seconds per repetition"
                ),
            }
        )
    else:
        configuration_fingerprint, runtime_policy_fingerprint = m2_fingerprints(raw_result)
        context.update(
            {
                "benchmark_suite": "m2",
                "workload_ids": list(M2_WORKLOAD_IDS),
                "configuration_fingerprint_sha256": configuration_fingerprint,
                "runtime_policy_fingerprint_sha256": runtime_policy_fingerprint,
                "sample_count": M2_SAMPLE_COUNT,
                "warm_up_policy": (
                    "Fixed 10,000 measured iterations per workload; fixture construction, "
                    "preallocation, bootstrap, and initial snapshot are outside measured intervals"
                ),
                "evidence_classification": "smoke",
            }
        )
        if qualifies_as_ref_mac_01(context):
            observed_p99 = callback_p99_microseconds(raw_result)
            callback_limit = 100.0
            context["evidence_classification"] = "qualification"
            context["qualification_reference"] = "REF-MAC-01"
            context["threshold_claims"] = [
                {
                    "workload_id": M2_WORKLOAD_IDS[1],
                    "metric": "p99_us",
                    "operator": "<=",
                    "limit": callback_limit,
                    "observed": observed_p99,
                    "passed": observed_p99 <= callback_limit,
                }
            ]

    # ++++++++++++++++++++++++++++++++++++++++
    # Pretty, newline-terminated JSON is both machine-readable and review-friendly.
    context_path.write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    return 0

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------

# Interesting syntax: this guard runs main only as a script, not when helpers are imported in tests.
if __name__ == "__main__":
    raise SystemExit(main())
