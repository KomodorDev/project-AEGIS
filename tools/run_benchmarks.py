#!/usr/bin/env python3

"""Run the M0 calibration benchmark and record enough context to reproduce its result."""

# Interesting syntax: postponed annotations allow modern type hints without eager evaluation.
from __future__ import annotations

# Standard-library modules cover CLI parsing, hashing, host discovery,
# subprocesses, and JSON output.
import argparse
import hashlib
import json
import os
import platform
import subprocess
from collections.abc import Sequence
from datetime import UTC, datetime
from pathlib import Path


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


def cache_value(cache_path: Path, name: str) -> str:
    """Read one named value from CMake's `NAME:TYPE=value` cache representation."""

    # Match the full cache-key prefix, then split only once so values may themselves contain `=`.
    prefix = f"{name}:"
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1]
    raise RuntimeError(f"{name} is absent from {cache_path}")


def sysctl_value(name: str) -> str | None:
    """Return a macOS sysctl value, or None when the key/command is unavailable."""

    # Host metadata is best-effort: unsupported probes must not invalidate benchmark results.
    try:
        return command_output(["sysctl", "-n", name])
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None


def cpu_model() -> str:
    """Return the most specific CPU model string available on macOS or Linux."""

    # Apple exposes CPU identity through sysctl rather than Linux's /proc virtual filesystem.
    if platform.system() == "Darwin":
        return sysctl_value("machdep.cpu.brand_string") or sysctl_value("hw.model") or "unknown"

    # Linux usually records a human-readable model once per logical processor in /proc/cpuinfo.
    cpu_info = Path("/proc/cpuinfo")
    if cpu_info.exists():
        for line in cpu_info.read_text(encoding="utf-8").splitlines():
            if line.startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    # Fall back to Python's portable probe and an explicit sentinel on other operating systems.
    return platform.processor() or "unknown"


def physical_core_count() -> int | None:
    """Count physical CPU cores when the host exposes topology without extra dependencies."""

    # macOS reports the total directly.
    if platform.system() == "Darwin":
        value = sysctl_value("hw.physicalcpu")
        return int(value) if value else None

    # Linux lscpu emits CORE,SOCKET pairs; unique pairs distinguish cores across CPU sockets.
    try:
        rows = command_output(["lscpu", "--parse=CORE,SOCKET"])
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None

    # Interesting syntax: this set comprehension both filters comments and removes duplicates.
    cores = {
        line for line in rows.splitlines() if line and not line.startswith("#") and line != "-,-"
    }
    return len(cores) or None


def parse_arguments() -> argparse.Namespace:
    """Parse the optional evidence directory while defaulting it inside the repository."""

    # Resolving from __file__ makes the default independent of the caller's current directory.
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=repository / "benchmark-results",
    )
    return parser.parse_args()


def main() -> int:
    """Validate inputs, run the benchmark, and write raw results plus a context manifest."""

    # Derive all fixed inputs from the repository and the release preset's stable output paths.
    arguments = parse_arguments()
    repository = Path(__file__).resolve().parents[1]
    binary = (repository / "build/release/benchmarks/aegis_benchmarks").resolve()
    results_dir = arguments.results_dir.resolve()
    cache_path = repository / "build/release/CMakeCache.txt"

    # Fail before creating evidence when the required optimized build is absent or incomplete.
    if not binary.is_file():
        raise FileNotFoundError(f"benchmark executable not found: {binary}")
    if not cache_path.is_file():
        raise FileNotFoundError(f"release configuration not found: {cache_path}")

    # Keep machine-readable timing data and its provenance manifest beside one another.
    results_dir.mkdir(parents=True, exist_ok=True)
    benchmark_path = results_dir / "m0-harness.json"
    context_path = results_dir / "m0-context.json"

    # Use short repeated samples for M0 pipeline calibration, not a product-performance claim.
    benchmark_command = [
        str(binary),
        "--benchmark_min_time=0.01s",
        "--benchmark_repetitions=3",
        "--benchmark_report_aggregates_only=true",
        f"--benchmark_out={benchmark_path}",
        "--benchmark_out_format=json",
    ]
    subprocess.run(benchmark_command, cwd=repository, check=True)

    # Read tool locations from the actual release cache and fingerprint repository/result state.
    compiler = cache_value(cache_path, "CMAKE_CXX_COMPILER")
    cmake = cache_value(cache_path, "CMAKE_COMMAND")
    git_status = command_output(["git", "status", "--porcelain"], cwd=repository)
    benchmark_hash = hashlib.sha256(benchmark_path.read_bytes()).hexdigest()

    # Capture the fields required to interpret and reproduce a benchmark result responsibly.
    context = {
        "schema_version": 1,
        "workload_id": "BENCH-M0-HARNESS-001",
        "generated_at_utc": datetime.now(UTC).isoformat(),
        "git_revision": command_output(["git", "rev-parse", "HEAD"], cwd=repository),
        "git_worktree_dirty": bool(git_status),
        "build_preset": "release",
        "build_type": cache_value(cache_path, "CMAKE_BUILD_TYPE"),
        "compiler": command_output([compiler, "--version"]).splitlines()[0],
        "cmake": command_output([cmake, "--version"]).splitlines()[0],
        "operating_system": platform.platform(),
        "machine": platform.machine(),
        "cpu_model": cpu_model(),
        "logical_core_count": os.cpu_count(),
        "physical_core_count": physical_core_count(),
        "power_mode": os.environ.get("AEGIS_BENCHMARK_POWER_MODE", "uncontrolled"),
        "host_isolation": os.environ.get("AEGIS_BENCHMARK_HOST_ISOLATION", "uncontrolled"),
        "thermal_state": os.environ.get("AEGIS_BENCHMARK_THERMAL_STATE", "uncontrolled"),
        "sample_count": 3,
        "warm_up_policy": (
            "Google Benchmark adaptive iteration calibration; no separate warm-up; "
            "minimum measured time 0.01 seconds per repetition"
        ),
        "benchmark_command": benchmark_command,
        "benchmark_result": benchmark_path.name,
        "benchmark_result_sha256": benchmark_hash,
    }

    # Pretty, newline-terminated JSON is both machine-readable and review-friendly.
    context_path.write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    return 0


# Interesting syntax: this guard runs main only as a script, not when helpers are imported in tests.
if __name__ == "__main__":
    raise SystemExit(main())
