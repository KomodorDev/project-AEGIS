#!/usr/bin/env python3

"""Reject incomplete or internally inconsistent M0, M2, or M3 benchmark-evidence bundles."""

# Interesting syntax: postponed annotations let the script use modern type hints on every supported
# Python version without evaluating those hints while the module is imported.
from __future__ import annotations

# Only the standard library is used so CI can validate evidence without another runtime dependency.
import argparse
import hashlib
import json
import math
import re
import subprocess
from collections.abc import Sequence
from pathlib import Path
from typing import Any

# Reuse the runner's byte-exact algorithm so generation and validation cannot interpret a dirty
# working tree differently.
from run_benchmarks import (
    CONTROLLED_HOST_ISOLATION,
    CONTROLLED_POWER_MODE,
    CONTROLLED_THERMAL_STATE,
    M0_BENCHMARK_FILTER,
    M0_RUN_NAME,
    M0_WORKLOAD_ID,
    M2_BENCHMARK_FILTER,
    M2_RUN_NAMES,
    M2_SAMPLE_COUNT,
    M2_WORKLOAD_IDS,
    M3_BENCHMARK_FILTER,
    M3_PROVENANCE_KEYS,
    M3_PROVENANCE_LABEL_PATTERN,
    M3_RUN_NAMES,
    M3_SAMPLE_COUNT,
    M3_WORKLOAD_IDS,
    worktree_fingerprint,
)

EXPECTED_AGGREGATES = {"mean", "median", "stddev", "cv"}

# Each exact M2 run has one published time unit and a workload-specific counter contract. Counter
# field names encode their units because Google Benchmark's JSON schema has no per-counter unit key.
M2_RECORD_CONTRACTS = {
    M2_RUN_NAMES[0]: {
        "time_unit": "ns",
        "counters": ("ns_per_turn", "turns_per_second", "allocations_per_turn"),
        "allocation_counter": "allocations_per_turn",
    },
    M2_RUN_NAMES[1]: {
        "time_unit": "us",
        "counters": (
            "p50_us",
            "p99_us",
            "p99_9_us",
            "callbacks_per_second",
            "allocations_per_callback",
        ),
        "allocation_counter": "allocations_per_callback",
    },
    M2_RUN_NAMES[2]: {
        "time_unit": "us",
        "counters": (
            "p50_us",
            "p99_us",
            "p99_9_us",
            "events_per_second",
            "allocations_per_event",
        ),
        "allocation_counter": "allocations_per_event",
    },
}
VALID_M2_FINGERPRINT_LABEL = re.compile(
    r"configuration_fingerprint_sha256=([0-9a-f]{64});"
    r"runtime_policy_fingerprint_sha256=([0-9a-f]{64})"
)

# Each M3 workload reports one common distribution contract and its exact workload-specific rate
# and allocation names. Both use the internal submit-path duration in microseconds.
M3_RECORD_CONTRACTS = {
    M3_RUN_NAMES[0]: {
        "rate_counter": "orders_per_second",
        "allocation_counter": "allocations_per_order",
    },
    M3_RUN_NAMES[1]: {
        "rate_counter": "rejections_per_second",
        "allocation_counter": "allocations_per_request",
    },
}


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
# Extracts a required finite nonnegative numeric observation.
def require_nonnegative_number(document: dict[str, Any], key: str, *, location: str) -> float:
    """Return one finite nonnegative number without accepting booleans as integers."""

    value = document.get(key)
    require(
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
        and value >= 0,
        f"{location} {key} must be a finite nonnegative number",
    )
    assert isinstance(value, (int, float)) and not isinstance(value, bool)
    return float(value)


# --------------------------------------------------------
# Parses one exact M2 configuration-attribution label.
def m2_fingerprint_pair(record: dict[str, Any], *, location: str) -> tuple[str, str]:
    """Return the two lowercase SHA-256 fingerprints encoded in one exact M2 record label."""

    label = record.get("label")
    require(isinstance(label, str), f"{location} label must be a string")
    assert isinstance(label, str)
    match = VALID_M2_FINGERPRINT_LABEL.fullmatch(label)
    require(match is not None, f"{location} label has invalid fingerprint grammar")
    assert match is not None
    return match.group(1), match.group(2)


# --------------------------------------------------------
# Parses one exact M3 provenance label into the same typed object stored in the manifest.
def m3_provenance(record: dict[str, Any], *, location: str) -> dict[str, object]:
    """Return every canonical M3 benchmark-provenance field from one anchored label."""

    label = record.get("label")
    require(isinstance(label, str), f"{location} label must be a string")
    assert isinstance(label, str)
    match = M3_PROVENANCE_LABEL_PATTERN.fullmatch(label)
    require(match is not None, f"{location} label has invalid M3 provenance grammar")
    assert match is not None
    numeric_keys = {
        "configuration_revision",
        "organization_revision",
        "risk_policy_revision",
        "route_revision",
        "metadata_revision",
    }
    parsed: dict[str, object] = {}
    for key, value in zip(M3_PROVENANCE_KEYS, match.groups(), strict=True):
        parsed[key] = int(value) if key in numeric_keys else value
    return parsed


# --------------------------------------------------------
# Independently verifies every published REF-MAC-01 qualification condition.
def require_ref_mac_01_context(manifest: dict[str, Any]) -> None:
    """Reject a qualification label unless all hardware, toolchain, and control facts are exact."""

    # Qualification is conjunctive: unknown, approximate, or free-form conditions remain smoke.
    require(
        str(manifest.get("operating_system", "")).startswith("macOS-15."),
        "qualification operating_system must identify macOS 15",
    )
    require(manifest.get("host_model") == "MacBookPro18,2", "qualification host_model mismatch")
    require(manifest.get("machine") == "arm64", "qualification machine must equal arm64")
    require(manifest.get("cpu_model") == "Apple M1 Max", "qualification cpu_model mismatch")
    require(
        manifest.get("total_memory_bytes") == 32 * 1024 * 1024 * 1024,
        "qualification memory must equal 32 GiB",
    )
    require(
        manifest.get("logical_core_count") == 10,
        "qualification logical_core_count must equal 10",
    )
    require(
        manifest.get("physical_core_count") == 10,
        "qualification physical_core_count must equal 10",
    )
    require(
        str(manifest.get("compiler", "")).startswith("Apple clang version 16."),
        "qualification compiler must identify AppleClang 16",
    )
    require(
        manifest.get("power_mode") == CONTROLLED_POWER_MODE,
        f"qualification power_mode must equal {CONTROLLED_POWER_MODE}",
    )
    require(
        manifest.get("host_isolation") == CONTROLLED_HOST_ISOLATION,
        f"qualification host_isolation must equal {CONTROLLED_HOST_ISOLATION}",
    )
    require(
        manifest.get("thermal_state") == CONTROLLED_THERMAL_STATE,
        f"qualification thermal_state must equal {CONTROLLED_THERMAL_STATE}",
    )


# --------------------------------------------------------
# Defines the M0/M2/M3 suite selector and repository-relative result default.
def parse_arguments() -> argparse.Namespace:
    """Select M0 by default or one explicit suite and repository-relative evidence directory."""

    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--suite",
        choices=("m0", "m2", "m3"),
        default="m0",
        help="benchmark evidence suite to validate (default: m0)",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=repository / "benchmark-results",
    )
    return parser.parse_args()


# --------------------------------------------------------
# Validates the build, host, hash, and raw-producer fields shared by every suite.
def validate_common_bundle(
    raw: dict[str, Any], manifest: dict[str, Any], raw_path: Path
) -> tuple[Path, list[str]]:
    """Validate common manifest context and return its resolved executable and benchmark command."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Require one optimized Ninja build and an explicit dirty-state observation.
    require(manifest.get("schema_version") == 1, "schema_version must equal 1")
    require(manifest.get("build_preset") == "release", "build_preset must equal release")
    require(manifest.get("build_type") == "Release", "build_type must equal Release")
    require(manifest.get("build_system") == "Ninja", "build_system must equal Ninja")
    require(type(manifest.get("git_worktree_dirty")) is bool, "git_worktree_dirty must be boolean")

    # ++++++++++++++++++++++++++++++++++++++++
    # Verify required descriptive fields together with numeric host topology and memory.
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
    # Cross-check the raw file, recorded commands, and executable against recorded hashes.
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
    require(
        "--benchmark_out_format=json" in benchmark_command,
        "benchmark_command does not request JSON output",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Inspect producer context so manifest claims cannot drift from the raw benchmark output.
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
    return executable, benchmark_command

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Preserves the established M0 aggregate and command contract while accepting its new exact filter.
def validate_m0(
    raw: dict[str, Any], manifest: dict[str, Any], benchmark_command: list[str]
) -> None:
    """Validate the M0 calibration identity, repetitions, aggregates, and optional legacy filter."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Retain every original schema-one M0 identity and sample requirement.
    require(manifest.get("workload_id") == M0_WORKLOAD_ID, "unexpected workload_id")
    require(manifest.get("sample_count") == 3, "sample_count must equal 3")

    # ++++++++++++++++++++++++++++++++++++++++
    # New runs must carry the exact anchored M0 filter; an absent filter remains accepted so the
    # validator stays backward-compatible with already-retained schema-one M0 bundles.
    filters = [
        argument for argument in benchmark_command if argument.startswith("--benchmark_filter=")
    ]
    require(
        filters in ([], [f"--benchmark_filter={M0_BENCHMARK_FILTER}"]),
        "M0 benchmark_command has a wrong or ambiguous filter",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Require every raw record to remain an aggregate of the sole calibration workload.
    benchmarks = raw.get("benchmarks")
    require(isinstance(benchmarks, list) and bool(benchmarks), "raw benchmarks must be nonempty")
    assert isinstance(benchmarks, list)
    aggregates: set[str] = set()
    for index, benchmark in enumerate(benchmarks):
        require(isinstance(benchmark, dict), f"benchmark record {index} must be an object")
        assert isinstance(benchmark, dict)
        require(
            benchmark.get("run_name") == M0_RUN_NAME,
            f"benchmark record {index} has wrong run_name",
        )
        require(
            benchmark.get("repetitions") == 3,
            f"benchmark record {index} has wrong repetitions",
        )
        name = benchmark.get("name")
        require(
            isinstance(name, str) and name.startswith(f"{M0_RUN_NAME}_"),
            f"benchmark record {index} has wrong name",
        )
        aggregate = benchmark.get("aggregate_name")
        require(isinstance(aggregate, str), f"benchmark record {index} has no aggregate_name")
        aggregates.add(aggregate)
    require(EXPECTED_AGGREGATES <= aggregates, "raw result is missing required aggregates")

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Validates exact M2 identities, metrics, attribution, and controlled-host threshold eligibility.
def validate_m2(raw: dict[str, Any], manifest: dict[str, Any], benchmark_command: list[str]) -> str:
    """Validate all three fixed M2 workloads and return their evidence classification."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Require one exact suite identity, ordered workload list, and fixed sample count per workload.
    require(manifest.get("benchmark_suite") == "m2", "benchmark_suite must equal m2")
    require(manifest.get("workload_ids") == list(M2_WORKLOAD_IDS), "unexpected M2 workload_ids")
    require(
        manifest.get("sample_count") == M2_SAMPLE_COUNT,
        f"sample_count must equal {M2_SAMPLE_COUNT}",
    )
    filters = [
        argument for argument in benchmark_command if argument.startswith("--benchmark_filter=")
    ]
    require(
        filters == [f"--benchmark_filter={M2_BENCHMARK_FILTER}"],
        "M2 benchmark_command must carry the exact anchored suite filter",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Index exactly three non-aggregate raw records by their complete registered run identities.
    benchmarks = raw.get("benchmarks")
    require(
        isinstance(benchmarks, list) and len(benchmarks) == len(M2_RUN_NAMES),
        "raw M2 benchmarks must contain exactly three records",
    )
    assert isinstance(benchmarks, list)
    records: dict[str, dict[str, Any]] = {}
    fingerprint_pairs: set[tuple[str, str]] = set()
    for index, record in enumerate(benchmarks):
        location = f"benchmark record {index}"
        require(isinstance(record, dict), f"{location} must be an object")
        assert isinstance(record, dict)
        run_name = record.get("run_name")
        require(run_name in M2_RECORD_CONTRACTS, f"{location} has an unexpected run_name")
        assert isinstance(run_name, str)
        require(run_name not in records, f"{location} duplicates {run_name}")
        require(record.get("name") == run_name, f"{location} name must equal run_name")
        require(record.get("run_type") == "iteration", f"{location} must be an iteration record")
        require(record.get("repetitions") == 1, f"{location} repetitions must equal 1")
        records[run_name] = record
        fingerprint_pairs.add(m2_fingerprint_pair(record, location=location))
    require(set(records) == set(M2_RUN_NAMES), "raw M2 records have wrong identities")
    require(len(fingerprint_pairs) == 1, "M2 records do not share one fingerprint pair")
    configuration_fingerprint, runtime_policy_fingerprint = next(iter(fingerprint_pairs))
    require(
        manifest.get("configuration_fingerprint_sha256") == configuration_fingerprint,
        "configuration fingerprint does not match raw M2 labels",
    )
    require(
        manifest.get("runtime_policy_fingerprint_sha256") == runtime_policy_fingerprint,
        "runtime-policy fingerprint does not match raw M2 labels",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Validate units, fixed samples, general timings, and every workload-specific counter.
    for run_name in M2_RUN_NAMES:
        record = records[run_name]
        contract = M2_RECORD_CONTRACTS[run_name]
        location = run_name
        require(
            record.get("iterations") == M2_SAMPLE_COUNT, f"{location} iterations must equal 10000"
        )
        require(record.get("threads") == 1, f"{location} threads must equal 1")
        require(record.get("time_unit") == contract["time_unit"], f"{location} has wrong time_unit")
        require_nonnegative_number(record, "real_time", location=location)
        require_nonnegative_number(record, "cpu_time", location=location)
        sample_count = require_nonnegative_number(record, "sample_count", location=location)
        require(sample_count == M2_SAMPLE_COUNT, f"{location} sample_count must equal 10000")
        for counter in contract["counters"]:
            assert isinstance(counter, str)
            require_nonnegative_number(record, counter, location=location)
        allocation_counter = contract["allocation_counter"]
        assert isinstance(allocation_counter, str)
        require_nonnegative_number(record, allocation_counter, location=location)

    # ++++++++++++++++++++++++++++++++++++++++
    # Tail counters must form a monotonic distribution for both percentile-bearing workloads.
    for run_name in M2_RUN_NAMES[1:]:
        record = records[run_name]
        p50 = require_nonnegative_number(record, "p50_us", location=run_name)
        p99 = require_nonnegative_number(record, "p99_us", location=run_name)
        p99_9 = require_nonnegative_number(record, "p99_9_us", location=run_name)
        require(p50 <= p99 <= p99_9, f"{run_name} percentile counters are not monotonic")

    # ++++++++++++++++++++++++++++++++++++++++
    # Smoke evidence must contain no threshold claim. Only exact controlled REF-MAC-01 facts may
    # authorize the published M2 callback p99 qualification and cause its threshold to be tested.
    classification = manifest.get("evidence_classification")
    require(classification in ("smoke", "qualification"), "invalid evidence_classification")
    callback_p99 = require_nonnegative_number(
        records[M2_RUN_NAMES[1]], "p99_us", location=M2_RUN_NAMES[1]
    )
    if classification == "smoke":
        require(
            "qualification_reference" not in manifest and "threshold_claims" not in manifest,
            "smoke evidence must not contain a threshold claim",
        )
        return "smoke"

    require_ref_mac_01_context(manifest)
    require(
        manifest.get("qualification_reference") == "REF-MAC-01",
        "qualification_reference must equal REF-MAC-01",
    )
    claims = manifest.get("threshold_claims")
    require(
        isinstance(claims, list) and len(claims) == 1, "qualification needs one threshold claim"
    )
    assert isinstance(claims, list)
    require(isinstance(claims[0], dict), "qualification threshold claim must be an object")
    assert isinstance(claims[0], dict)
    claim = claims[0]
    require(
        claim.get("workload_id") == M2_WORKLOAD_IDS[1]
        and claim.get("metric") == "p99_us"
        and claim.get("operator") == "<="
        and claim.get("limit") == 100.0
        and claim.get("observed") == callback_p99
        and type(claim.get("passed")) is bool
        and claim.get("passed") == (callback_p99 <= 100.0),
        "callback qualification claim does not match the raw p99 observation",
    )
    require(callback_p99 <= 100.0, "BENCH-M2-CALLBACK-001 p99 exceeds 100 microseconds")
    return "qualification"

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Validates exact M3 identities, metrics, provenance, and two-claim qualification policy.
def validate_m3(raw: dict[str, Any], manifest: dict[str, Any], benchmark_command: list[str]) -> str:
    """Validate both fixed M3 submit workloads and return their evidence classification."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Require the exact suite identity, workload order, sample count, and anchored binary filter.
    require(manifest.get("benchmark_suite") == "m3", "benchmark_suite must equal m3")
    require(manifest.get("workload_ids") == list(M3_WORKLOAD_IDS), "unexpected M3 workload_ids")
    require(
        manifest.get("sample_count") == M3_SAMPLE_COUNT,
        f"sample_count must equal {M3_SAMPLE_COUNT}",
    )
    filters = [
        argument for argument in benchmark_command if argument.startswith("--benchmark_filter=")
    ]
    require(
        filters == [f"--benchmark_filter={M3_BENCHMARK_FILTER}"],
        "M3 benchmark_command must carry the exact anchored suite filter",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Index exactly two nonaggregate records and parse each workload's immutable fixture identity.
    benchmarks = raw.get("benchmarks")
    require(
        isinstance(benchmarks, list) and len(benchmarks) == len(M3_RUN_NAMES),
        "raw M3 benchmarks must contain exactly two records",
    )
    assert isinstance(benchmarks, list)
    records: dict[str, dict[str, Any]] = {}
    provenance_by_run: dict[str, dict[str, object]] = {}
    for index, record in enumerate(benchmarks):
        location = f"benchmark record {index}"
        require(isinstance(record, dict), f"{location} must be an object")
        assert isinstance(record, dict)
        run_name = record.get("run_name")
        require(run_name in M3_RECORD_CONTRACTS, f"{location} has an unexpected run_name")
        assert isinstance(run_name, str)
        require(run_name not in records, f"{location} duplicates {run_name}")
        require(record.get("name") == run_name, f"{location} name must equal run_name")
        require(record.get("run_type") == "iteration", f"{location} must be an iteration record")
        require(record.get("repetitions") == 1, f"{location} repetitions must equal 1")
        # Google Benchmark can attach failure or skip status to otherwise complete-looking metrics.
        for status_key in ("error_occurred", "skipped"):
            if status_key in record:
                require(
                    record.get(status_key) is False,
                    f"{location} {status_key} must be false when present",
                )
        for message_key in ("error_message", "skip_message"):
            if message_key in record:
                require(
                    record.get(message_key) == "",
                    f"{location} {message_key} must be empty when present",
                )
        records[run_name] = record
        provenance_by_run[run_name] = m3_provenance(record, location=location)
    require(set(records) == set(M3_RUN_NAMES), "raw M3 records have wrong identities")

    # ++++++++++++++++++++++++++++++++++++++++
    # The manifest repeats the ordered raw labels exactly, preventing attribution substitution.
    ordered_provenance = [provenance_by_run[run_name] for run_name in M3_RUN_NAMES]
    require(
        manifest.get("workload_provenance") == ordered_provenance,
        "workload_provenance does not match ordered raw M3 labels",
    )
    for index, expected_workload in enumerate(M3_WORKLOAD_IDS):
        require(
            ordered_provenance[index].get("workload_id") == expected_workload,
            f"M3 provenance row {index} names another workload",
        )

    # ++++++++++++++++++++++++++++++++++++++++
    # Both workloads use the value-identical request/runtime fixture; only risk and consequently
    # submission fingerprints differ so the rejection branch cannot borrow success provenance.
    left, right = ordered_provenance
    differing_keys = {
        "workload_id",
        "risk_policy_fingerprint_sha256",
        "submission_policy_fingerprint_sha256",
    }
    for key in M3_PROVENANCE_KEYS:
        if key not in differing_keys:
            require(left.get(key) == right.get(key), f"M3 shared provenance differs at {key}")
    require(
        left.get("risk_policy_fingerprint_sha256") != right.get("risk_policy_fingerprint_sha256"),
        "M3 workload risk-policy fingerprints must differ",
    )
    require(
        left.get("submission_policy_fingerprint_sha256")
        != right.get("submission_policy_fingerprint_sha256"),
        "M3 workload submission-policy fingerprints must differ",
    )

    # ++++++++++++++++++++++++++++++++++++++++
    # Enforce one thread, one repetition, fixed samples, monotonic tails, and exact rate/allocation
    # names for both internally timed submission paths.
    p99_by_workload: dict[str, float] = {}
    for run_name, workload_id in zip(M3_RUN_NAMES, M3_WORKLOAD_IDS, strict=True):
        record = records[run_name]
        contract = M3_RECORD_CONTRACTS[run_name]
        require(
            record.get("iterations") == M3_SAMPLE_COUNT,
            f"{run_name} iterations must equal {M3_SAMPLE_COUNT}",
        )
        require(record.get("threads") == 1, f"{run_name} threads must equal 1")
        require(record.get("time_unit") == "us", f"{run_name} time_unit must equal us")
        require_nonnegative_number(record, "real_time", location=run_name)
        require_nonnegative_number(record, "cpu_time", location=run_name)
        sample_count = require_nonnegative_number(record, "sample_count", location=run_name)
        require(sample_count == M3_SAMPLE_COUNT, f"{run_name} sample_count must equal 10000")
        p50 = require_nonnegative_number(record, "p50_us", location=run_name)
        p99 = require_nonnegative_number(record, "p99_us", location=run_name)
        p99_9 = require_nonnegative_number(record, "p99_9_us", location=run_name)
        require(p50 <= p99 <= p99_9, f"{run_name} percentile counters are not monotonic")
        rate_counter = contract["rate_counter"]
        allocation_counter = contract["allocation_counter"]
        assert isinstance(rate_counter, str) and isinstance(allocation_counter, str)
        require_nonnegative_number(record, rate_counter, location=run_name)
        require_nonnegative_number(record, allocation_counter, location=run_name)
        p99_by_workload[workload_id] = p99

    # ++++++++++++++++++++++++++++++++++++++++
    # Smoke runs make no threshold statement. Qualification requires exactly the two ordered,
    # raw-bound REF-MAC-01 claims and both provisional p99 limits must pass.
    classification = manifest.get("evidence_classification")
    require(classification in ("smoke", "qualification"), "invalid evidence_classification")
    if classification == "smoke":
        require(
            "qualification_reference" not in manifest and "threshold_claims" not in manifest,
            "smoke evidence must not contain threshold claims",
        )
        return "smoke"

    require_ref_mac_01_context(manifest)
    require(
        manifest.get("qualification_reference") == "REF-MAC-01",
        "qualification_reference must equal REF-MAC-01",
    )
    claims = manifest.get("threshold_claims")
    require(
        isinstance(claims, list) and len(claims) == 2,
        "M3 qualification needs exactly two threshold claims",
    )
    assert isinstance(claims, list)
    limits = (50.0, 25.0)
    for index, (workload_id, limit) in enumerate(zip(M3_WORKLOAD_IDS, limits, strict=True)):
        claim = claims[index]
        require(isinstance(claim, dict), f"M3 threshold claim {index} must be an object")
        assert isinstance(claim, dict)
        observed = p99_by_workload[workload_id]
        require(
            claim.get("workload_id") == workload_id
            and claim.get("metric") == "p99_us"
            and claim.get("operator") == "<="
            and claim.get("limit") == limit
            and claim.get("observed") == observed
            and type(claim.get("passed")) is bool
            and claim.get("passed") == (observed <= limit),
            f"M3 threshold claim {index} does not match its raw p99 observation",
        )
        require(observed <= limit, f"{workload_id} p99 exceeds {limit} microseconds")
    return "qualification"

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Proves that a manifest still describes the exact checkout being validated.
def validate_repository_provenance(manifest: dict[str, Any], repository: Path) -> None:
    """Cross-check Git revision, tree, dirty state, and byte-exact worktree fingerprint."""

    # A committed revision alone is insufficient because benchmarkable source may be dirty.
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
        manifest["git_worktree_dirty"] == dirty,
        "git_worktree_dirty no longer matches checkout",
    )
    require(
        manifest["git_worktree_fingerprint_sha256"] == worktree_fingerprint(repository),
        "git_worktree_fingerprint_sha256 no longer matches checkout",
    )


# --------------------------------------------------------
# Validates the selected complete benchmark bundle and its current repository provenance.
def main() -> int:
    """Validate schema, hashes, workload metrics, qualification policy, and Git provenance."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Resolve suite-specific bundle names from the same default used by the runner.
    arguments = parse_arguments()
    suite = arguments.suite
    repository = Path(__file__).resolve().parents[1]
    results_dir = arguments.results_dir.resolve()
    if suite == "m0":
        raw_name, manifest_name = "m0-harness.json", "m0-context.json"
    elif suite == "m2":
        raw_name, manifest_name = "m2-runtime.json", "m2-context.json"
    else:
        raw_name, manifest_name = "m3-submission.json", "m3-context.json"
    raw_path = results_dir / raw_name
    manifest_path = results_dir / manifest_name
    raw = read_json_object(raw_path)
    manifest = read_json_object(manifest_path)

    # ++++++++++++++++++++++++++++++++++++++++
    # Validate common evidence first, then apply only the selected milestone's record contract.
    _, benchmark_command = validate_common_bundle(raw, manifest, raw_path)
    if suite == "m0":
        validate_m0(raw, manifest, benchmark_command)
        summary = M0_WORKLOAD_ID
    elif suite == "m2":
        classification = validate_m2(raw, manifest, benchmark_command)
        summary = f"M2 runtime {classification}"
    else:
        classification = validate_m3(raw, manifest, benchmark_command)
        summary = f"M3 submission {classification}"
    validate_repository_provenance(manifest, repository)

    # ++++++++++++++++++++++++++++++++++++++++
    # Report a smoke classification without asserting that its observed p99 meets a host threshold.
    print(f"validated {summary} evidence in {results_dir}")
    return 0

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------

# Interesting syntax: raising SystemExit forwards main's status code to shells and CI runners.
if __name__ == "__main__":
    raise SystemExit(main())
