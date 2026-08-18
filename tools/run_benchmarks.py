#!/usr/bin/env python3

"""Run the M0 benchmark and retain the context required by the quality budget."""

from __future__ import annotations

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
    completed = subprocess.run(
        arguments,
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def cache_value(cache_path: Path, name: str) -> str:
    prefix = f"{name}:"
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and "=" in line:
            return line.split("=", 1)[1]
    raise RuntimeError(f"{name} is absent from {cache_path}")


def sysctl_value(name: str) -> str | None:
    try:
        return command_output(["sysctl", "-n", name])
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None


def cpu_model() -> str:
    if platform.system() == "Darwin":
        return sysctl_value("machdep.cpu.brand_string") or sysctl_value("hw.model") or "unknown"

    cpu_info = Path("/proc/cpuinfo")
    if cpu_info.exists():
        for line in cpu_info.read_text(encoding="utf-8").splitlines():
            if line.startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def physical_core_count() -> int | None:
    if platform.system() == "Darwin":
        value = sysctl_value("hw.physicalcpu")
        return int(value) if value else None

    try:
        rows = command_output(["lscpu", "--parse=CORE,SOCKET"])
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None

    cores = {
        line for line in rows.splitlines() if line and not line.startswith("#") and line != "-,-"
    }
    return len(cores) or None


def parse_arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=repository / "benchmark-results",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    repository = Path(__file__).resolve().parents[1]
    binary = (repository / "build/release/benchmarks/aegis_benchmarks").resolve()
    results_dir = arguments.results_dir.resolve()
    cache_path = repository / "build/release/CMakeCache.txt"

    if not binary.is_file():
        raise FileNotFoundError(f"benchmark executable not found: {binary}")
    if not cache_path.is_file():
        raise FileNotFoundError(f"release configuration not found: {cache_path}")

    results_dir.mkdir(parents=True, exist_ok=True)
    benchmark_path = results_dir / "m0-harness.json"
    context_path = results_dir / "m0-context.json"

    benchmark_command = [
        str(binary),
        "--benchmark_min_time=0.01s",
        "--benchmark_repetitions=3",
        "--benchmark_report_aggregates_only=true",
        f"--benchmark_out={benchmark_path}",
        "--benchmark_out_format=json",
    ]
    subprocess.run(benchmark_command, cwd=repository, check=True)

    compiler = cache_value(cache_path, "CMAKE_CXX_COMPILER")
    cmake = cache_value(cache_path, "CMAKE_COMMAND")
    git_status = command_output(["git", "status", "--porcelain"], cwd=repository)
    benchmark_hash = hashlib.sha256(benchmark_path.read_bytes()).hexdigest()
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
    context_path.write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
