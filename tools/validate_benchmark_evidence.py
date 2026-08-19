#!/usr/bin/env python3

"""Reject incomplete or internally inconsistent M0 benchmark-evidence bundles."""

# Interesting syntax: postponed annotations let the script use modern type hints on every supported
# Python version without evaluating those hints while the module is imported.
from __future__ import annotations

# Only the standard library is used so CI can validate evidence without another runtime dependency.
import argparse
import hashlib
import json
import subprocess
from collections.abc import Sequence
from pathlib import Path
from typing import Any

# Reuse the runner's byte-exact algorithm so generation and validation cannot interpret a dirty
# working tree differently.
from run_benchmarks import worktree_fingerprint

WORKLOAD_ID = "BENCH-M0-HARNESS-001"
RUN_NAME = f"{WORKLOAD_ID}/harness.noop"
EXPECTED_AGGREGATES = {"mean", "median", "stddev", "cv"}


# --------------------------------------------------------
# Runs one repository-inspection command with strict failure propagation.
def command_output(arguments: Sequence[str], *, cwd: Path) -> str:
    """Run a repository-inspection command and return trimmed standard output."""

    # check=True makes a broken Git checkout fail validation instead of producing partial evidence.
    completed = subprocess.run(
        arguments,
        cwd=cwd,
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


# --------------------------------------------------------
# Computes the canonical lowercase SHA-256 digest for one file.
def sha256(path: Path) -> str:
    """Return the lowercase SHA-256 digest of one evidence or executable file."""

    return hashlib.sha256(path.read_bytes()).hexdigest()


# --------------------------------------------------------
# Loads evidence JSON while enforcing the object-shaped document contract.
def read_json_object(path: Path) -> dict[str, Any]:
    """Load a JSON document and require an object at its root."""

    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return document


# --------------------------------------------------------
# Converts failed evidence invariants into one consistent validation error.
def require(condition: bool, message: str) -> None:
    """Raise one consistent validation error when an evidence invariant is false."""

    if not condition:
        raise ValueError(message)


# --------------------------------------------------------
# Extracts one required nonempty string from an evidence object.
def require_nonempty_string(document: dict[str, Any], key: str) -> str:
    """Return a required nonempty string field from a JSON object."""

    value = document.get(key)
    require(isinstance(value, str) and bool(value), f"{key} must be a nonempty string")
    # The assertion narrows the static type after the runtime check above.
    assert isinstance(value, str)
    return value


# --------------------------------------------------------
# Extracts a required nonempty list whose elements are nonempty strings.
def require_string_list(document: dict[str, Any], key: str) -> list[str]:
    """Return a required nonempty list containing only nonempty strings."""

    value = document.get(key)
    require(
        isinstance(value, list)
        and bool(value)
        and all(isinstance(item, str) and item for item in value),
        f"{key} must be a nonempty list of strings",
    )
    # Interesting syntax: assert communicates the already-proven element type to type checkers.
    assert isinstance(value, list) and all(isinstance(item, str) for item in value)
    return value


# --------------------------------------------------------
# Defines the command-line contract and repository-relative result default.
def parse_arguments() -> argparse.Namespace:
    """Accept an alternate result directory while defaulting to the repository output path."""

    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=repository / "benchmark-results",
    )
    return parser.parse_args()


# --------------------------------------------------------
# Validates the complete benchmark bundle and its repository provenance.
def main() -> int:
    """Validate schema, hashes, workload identity, build mode, and repository provenance."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Resolves fixed bundle names from the same repository-relative default used by the runner.
    arguments = parse_arguments()
    repository = Path(__file__).resolve().parents[1]
    results_dir = arguments.results_dir.resolve()
    raw_path = results_dir / "m0-harness.json"
    manifest_path = results_dir / "m0-context.json"
    raw = read_json_object(raw_path)
    manifest = read_json_object(manifest_path)

    # ++++++++++++++++++++++++++++++++++++++++
    # Requires the stable M0 identity and optimized build properties promised by the docs.
    require(manifest.get("schema_version") == 1, "schema_version must equal 1")
    require(manifest.get("workload_id") == WORKLOAD_ID, "unexpected workload_id")
    require(manifest.get("build_preset") == "release", "build_preset must equal release")
    require(manifest.get("build_type") == "Release", "build_type must equal Release")
    require(manifest.get("build_system") == "Ninja", "build_system must equal Ninja")
    require(manifest.get("sample_count") == 3, "sample_count must equal 3")
    require(type(manifest.get("git_worktree_dirty")) is bool, "git_worktree_dirty must be boolean")

    # ++++++++++++++++++++++++++++++++++++++++
    # Verifies required descriptive fields together with numeric host topology and memory.
    for key in (
        "generated_at_utc",
        "git_revision",
        "git_head_tree",
        "git_worktree_fingerprint_sha256",
        "compiler",
        "cmake",
        "build_tool",
        "google_benchmark_library_version",
        "operating_system",
        "host_model",
        "machine",
        "cpu_model",
        "power_mode",
        "host_isolation",
        "thermal_state",
        "warm_up_policy",
        "benchmark_executable",
        "benchmark_executable_sha256",
        "benchmark_result",
        "benchmark_result_sha256",
    ):
        require_nonempty_string(manifest, key)
    for key in ("logical_core_count", "total_memory_bytes"):
        value = manifest.get(key)
        require(type(value) is int and value > 0, f"{key} must be a positive integer")
    physical_cores = manifest.get("physical_core_count")
    require(
        physical_cores is None or (type(physical_cores) is int and physical_cores > 0),
        "physical_core_count must be null or a positive integer",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Cross-checks the raw file, recorded commands, and executable against recorded hashes.
    require(manifest["benchmark_result"] == raw_path.name, "benchmark_result names the wrong file")
    require(manifest["benchmark_result_sha256"] == sha256(raw_path), "raw result SHA-256 mismatch")
    executable = Path(manifest["benchmark_executable"]).resolve()
    require(executable.is_file(), f"benchmark executable is missing: {executable}")
    require(
        manifest["benchmark_executable_sha256"] == sha256(executable),
        "benchmark executable SHA-256 mismatch",
    )
    build_command = require_string_list(manifest, "build_command")
    benchmark_command = require_string_list(manifest, "benchmark_command")
    require(
        build_command[1:] == ["--build", "--preset", "release", "--target", "aegis_benchmarks"],
        "build_command does not rebuild the release benchmark target",
    )
    require(
        Path(benchmark_command[0]).resolve() == executable,
        "benchmark_command uses another executable",
    )
    require(
        f"--benchmark_out={raw_path}" in benchmark_command,
        "benchmark_command writes to another result file",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Inspects producer context so manifest claims cannot drift from the raw benchmark output.
    raw_context = raw.get("context")
    require(isinstance(raw_context, dict), "raw benchmark context must be an object")
    assert isinstance(raw_context, dict)
    library_build_type = raw_context.get("library_build_type")
    require(
        isinstance(library_build_type, str) and library_build_type.lower() == "release",
        "raw library build is not release",
    )
    require(
        raw_context.get("library_version") == manifest["google_benchmark_library_version"],
        "Google Benchmark library version mismatch",
    )
    require(
        Path(str(raw_context.get("executable"))).resolve() == executable,
        "raw result names another executable",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Requires every aggregate to represent the named workload and configured repetition count.
    benchmarks = raw.get("benchmarks")
    require(isinstance(benchmarks, list) and bool(benchmarks), "raw benchmarks must be nonempty")
    assert isinstance(benchmarks, list)
    aggregates: set[str] = set()
    for index, benchmark in enumerate(benchmarks):
        require(isinstance(benchmark, dict), f"benchmark record {index} must be an object")
        assert isinstance(benchmark, dict)
        require(
            benchmark.get("run_name") == RUN_NAME, f"benchmark record {index} has wrong run_name"
        )
        require(
            benchmark.get("repetitions") == 3, f"benchmark record {index} has wrong repetitions"
        )
        name = benchmark.get("name")
        require(
            isinstance(name, str) and name.startswith(f"{RUN_NAME}_"),
            f"benchmark record {index} has wrong name",
        )
        aggregate = benchmark.get("aggregate_name")
        require(isinstance(aggregate, str), f"benchmark record {index} has no aggregate_name")
        aggregates.add(aggregate)
    require(EXPECTED_AGGREGATES <= aggregates, "raw result is missing required aggregates")

    # ++++++++++++++++++++++++++++++++++++++++
    # Proves the manifest describes the exact repository state used for validation.
    require(
        manifest["git_revision"] == command_output(["git", "rev-parse", "HEAD"], cwd=repository),
        "git_revision does not match the current checkout",
    )
    require(
        manifest["git_head_tree"]
        == command_output(["git", "rev-parse", "HEAD^{tree}"], cwd=repository),
        "git_head_tree does not match the current checkout",
    )
    dirty = bool(command_output(["git", "status", "--porcelain"], cwd=repository))
    require(
        manifest["git_worktree_dirty"] == dirty, "git_worktree_dirty no longer matches checkout"
    )
    require(
        manifest["git_worktree_fingerprint_sha256"] == worktree_fingerprint(repository),
        "git_worktree_fingerprint_sha256 no longer matches checkout",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Reports success only after every independent evidence invariant has passed.
    print(f"validated {WORKLOAD_ID} evidence in {results_dir}")
    return 0

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------

# Interesting syntax: raising SystemExit forwards main's status code to shells and CI runners.
if __name__ == "__main__":
    raise SystemExit(main())
