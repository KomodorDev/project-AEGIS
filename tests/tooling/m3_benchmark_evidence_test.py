"""Prove the M3 benchmark validator rejects malformed smoke and qualification evidence."""

from __future__ import annotations

import copy
import sys
import unittest
from pathlib import Path
from typing import Any

TOOLS_DIRECTORY = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS_DIRECTORY))

import validate_benchmark_evidence as validator  # noqa: E402


# --------------------------------------------------------
# Encode one exact raw-label fixture using the accepted ordered M3 provenance grammar.
def provenance_label(workload_id: str, risk_fingerprint: str, submission_fingerprint: str) -> str:
    """Return one exact semicolon-delimited M3 benchmark label."""

    return (
        f"workload_id={workload_id};"
        f"configuration_fingerprint_sha256={'1' * 64};"
        "configuration_revision=1;"
        "organization_revision=1;"
        f"runtime_policy_fingerprint_sha256={'2' * 64};"
        f"risk_policy_fingerprint_sha256={risk_fingerprint};"
        "risk_policy_revision=1;"
        f"submission_policy_fingerprint_sha256={submission_fingerprint};"
        "route_id=route.deribit-testnet-btc-perpetual;"
        "route_revision=1;"
        "account_id=account.deribit-testnet-primary;"
        "venue_id=deribit;"
        "instrument_id=BTC-USD-PERPETUAL;"
        "metadata_revision=1;"
        f"order_namespace_hex={'5' * 32}"
    )


# --------------------------------------------------------


# --------------------------------------------------------
# Build one complete raw iteration record with the workload-specific counter vocabulary.
def benchmark_record(index: int, *, p50: float = 4.0, p99: float = 8.0, p99_9: float = 10.0):
    """Return one valid M3 success or rejection benchmark record."""

    run_name = validator.M3_RUN_NAMES[index]
    workload_id = validator.M3_WORKLOAD_IDS[index]
    record: dict[str, Any] = {
        "name": run_name,
        "run_name": run_name,
        "run_type": "iteration",
        "repetitions": 1,
        "iterations": validator.M3_SAMPLE_COUNT,
        "threads": 1,
        "time_unit": "us",
        "real_time": p99,
        "cpu_time": p99,
        "p50_us": p50,
        "p99_us": p99,
        "p99_9_us": p99_9,
        "sample_count": validator.M3_SAMPLE_COUNT,
        "label": provenance_label(
            workload_id,
            "3" * 64 if index == 0 else "6" * 64,
            "4" * 64 if index == 0 else "7" * 64,
        ),
    }
    contract = validator.M3_RECORD_CONTRACTS[run_name]
    record[str(contract["rate_counter"])] = 100_000.0
    record[str(contract["allocation_counter"])] = 0.0
    return record


# --------------------------------------------------------


# --------------------------------------------------------
# Assemble one smoke bundle whose manifest repeats both raw provenance labels exactly.
def smoke_fixture() -> tuple[dict[str, Any], dict[str, Any], list[str]]:
    """Return a valid raw result, smoke manifest, and anchored benchmark command."""

    raw = {"benchmarks": [benchmark_record(0), benchmark_record(1)]}
    provenance = [
        validator.m3_provenance(record, location=f"fixture {index}")
        for index, record in enumerate(raw["benchmarks"])
    ]
    manifest = {
        "benchmark_suite": "m3",
        "workload_ids": list(validator.M3_WORKLOAD_IDS),
        "sample_count": validator.M3_SAMPLE_COUNT,
        "workload_provenance": provenance,
        "evidence_classification": "smoke",
    }
    command = ["aegis_benchmarks", f"--benchmark_filter={validator.M3_BENCHMARK_FILTER}"]
    return raw, manifest, command


# --------------------------------------------------------


# --------------------------------------------------------
# Upgrade one smoke manifest to the exact controlled reference-host qualification shape.
def qualify(manifest: dict[str, Any], raw: dict[str, Any]) -> None:
    """Mutate a fixture manifest into a valid two-claim REF-MAC-01 qualification."""

    manifest.update(
        {
            "evidence_classification": "qualification",
            "operating_system": "macOS-15.7.0-arm64",
            "host_model": "MacBookPro18,2",
            "machine": "arm64",
            "cpu_model": "Apple M1 Max",
            "total_memory_bytes": 32 * 1024 * 1024 * 1024,
            "logical_core_count": 10,
            "physical_core_count": 10,
            "compiler": "Apple clang version 16.0.0",
            "power_mode": validator.CONTROLLED_POWER_MODE,
            "host_isolation": validator.CONTROLLED_HOST_ISOLATION,
            "thermal_state": validator.CONTROLLED_THERMAL_STATE,
            "qualification_reference": "REF-MAC-01",
            "threshold_claims": [
                {
                    "workload_id": validator.M3_WORKLOAD_IDS[0],
                    "metric": "p99_us",
                    "operator": "<=",
                    "limit": 50.0,
                    "observed": raw["benchmarks"][0]["p99_us"],
                    "passed": True,
                },
                {
                    "workload_id": validator.M3_WORKLOAD_IDS[1],
                    "metric": "p99_us",
                    "operator": "<=",
                    "limit": 25.0,
                    "observed": raw["benchmarks"][1]["p99_us"],
                    "passed": True,
                },
            ],
        }
    )


# --------------------------------------------------------


# ########################################################################
# Exercise the complete smoke/qualification contract through public validator entry points.
class M3BenchmarkEvidenceTest(unittest.TestCase):
    """Reject identity, metric, provenance, and threshold-claim drift deterministically."""

    # --------------------------------------------------------
    # Accept exact smoke and controlled-host qualification fixtures.
    def test_accepts_complete_smoke_and_qualification(self) -> None:
        """Accept only the two explicitly supported evidence classifications."""

        raw, manifest, command = smoke_fixture()
        raw["benchmarks"][0].update(
            {
                "error_occurred": False,
                "skipped": False,
                "error_message": "",
                "skip_message": "",
            }
        )
        self.assertEqual(validator.validate_m3(raw, manifest, command), "smoke")
        qualify(manifest, raw)
        self.assertEqual(validator.validate_m3(raw, manifest, command), "qualification")

    # --------------------------------------------------------
    # Reject malformed raw records, filters, counters, sample counts, and provenance bindings.
    def test_rejects_malformed_smoke_records(self) -> None:
        """Prove every stable workload and distribution invariant is enforced."""

        mutations = {
            "duplicate record": lambda raw, manifest, command: raw["benchmarks"].__setitem__(
                1, copy.deepcopy(raw["benchmarks"][0])
            ),
            "wrong filter": lambda raw, manifest, command: command.__setitem__(
                1, "--benchmark_filter=.*"
            ),
            "wrong samples": lambda raw, manifest, command: raw["benchmarks"][0].__setitem__(
                "sample_count", 9999
            ),
            "wrong unit": lambda raw, manifest, command: raw["benchmarks"][0].__setitem__(
                "time_unit", "ns"
            ),
            "nonmonotonic percentiles": lambda raw, manifest, command: raw["benchmarks"][0].update(
                {"p50_us": 9.0, "p99_us": 8.0}
            ),
            "error status": lambda raw, manifest, command: raw["benchmarks"][0].__setitem__(
                "error_occurred", True
            ),
            "skip status": lambda raw, manifest, command: raw["benchmarks"][0].__setitem__(
                "skipped", True
            ),
            "error message": lambda raw, manifest, command: raw["benchmarks"][0].__setitem__(
                "error_message", "benchmark failed"
            ),
            "skip message": lambda raw, manifest, command: raw["benchmarks"][0].__setitem__(
                "skip_message", "benchmark skipped"
            ),
            "manifest provenance mismatch": lambda raw, manifest, command: manifest[
                "workload_provenance"
            ][0].__setitem__("route_id", "route.foreign"),
            "same risk policy": lambda raw, manifest, command: raw["benchmarks"][1].__setitem__(
                "label",
                provenance_label(validator.M3_WORKLOAD_IDS[1], "3" * 64, "7" * 64),
            ),
            "smoke claim": lambda raw, manifest, command: manifest.__setitem__(
                "threshold_claims", []
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                raw, manifest, command = smoke_fixture()
                mutate(raw, manifest, command)
                with self.assertRaises(ValueError):
                    validator.validate_m3(raw, manifest, command)

    # --------------------------------------------------------
    # Reject missing, extra, reordered, mismatched, and independently failing qualification claims.
    def test_rejects_invalid_qualification_claims(self) -> None:
        """Bind both provisional thresholds to their exact ordered raw p99 observations."""

        mutations = {
            "missing claim": lambda raw, manifest: manifest["threshold_claims"].pop(),
            "extra claim": lambda raw, manifest: manifest["threshold_claims"].append(
                copy.deepcopy(manifest["threshold_claims"][0])
            ),
            "reordered claims": lambda raw, manifest: manifest["threshold_claims"].reverse(),
            "mismatched observed": lambda raw, manifest: manifest["threshold_claims"][
                0
            ].__setitem__("observed", 9.0),
            "success threshold failure": lambda raw, manifest: (
                raw["benchmarks"][0].__setitem__("p99_us", 51.0),
                raw["benchmarks"][0].__setitem__("p99_9_us", 52.0),
                manifest["threshold_claims"][0].update({"observed": 51.0, "passed": False}),
            ),
            "rejection threshold failure": lambda raw, manifest: (
                raw["benchmarks"][1].__setitem__("p99_us", 26.0),
                raw["benchmarks"][1].__setitem__("p99_9_us", 27.0),
                manifest["threshold_claims"][1].update({"observed": 26.0, "passed": False}),
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                raw, manifest, command = smoke_fixture()
                qualify(manifest, raw)
                mutate(raw, manifest)
                with self.assertRaises(ValueError):
                    validator.validate_m3(raw, manifest, command)

    # --------------------------------------------------------


# ########################################################################


# --------------------------------------------------------
# Run the fixture directly under Python while CTest imports the same unittest cases.
if __name__ == "__main__":
    unittest.main()


# --------------------------------------------------------
