"""Prove the deterministic M3/M4 scanner accepts only offline owner-local code."""

from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

# Load the repository-owned scanner without turning tools into an importable product package.
REPOSITORY = Path(__file__).resolve().parents[2]
SCANNER_PATH = REPOSITORY / "tools/check_forbidden_capabilities.py"
SPEC = importlib.util.spec_from_file_location("aegis_forbidden_capabilities", SCANNER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("unable to load forbidden-capability scanner")
scanner = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = scanner
SPEC.loader.exec_module(scanner)


# ########################################################################
# Temporary repositories make positive and negative capability proofs independent from live files.
class ForbiddenCapabilitiesTest(unittest.TestCase):
    """Exercise source, build, scope, ordering, and command-status contracts."""

    # --------------------------------------------------------
    # Create one UTF-8 file beneath a temporary repository, including absent parent directories.
    def write(self, repository: Path, relative: str, text: str) -> Path:
        """Write one scanner fixture and return its absolute path."""

        path = repository / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    # --------------------------------------------------------
    # Comments may explain guardrails while local deterministic value code remains acceptable.
    def test_accepts_offline_code_and_ignores_comments(self) -> None:
        """Keep documentation words from becoming false capability findings."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            clean = self.write(
                repository,
                "include/aegis/execution/fake_order_encoder.hpp",
                "// No socket, credential, endpoint, or coroutine is permitted.\n"
                "struct EncodedFakeOrder { unsigned size; };\n",
            )
            self.assertEqual(scanner.scan_paths(repository, [clean]), [])
            self.assertEqual(scanner.main(["--root", str(repository), str(clean)]), 0)

    # --------------------------------------------------------
    # Includes and call sites independently prove that merely acquiring a capability is forbidden.
    def test_rejects_network_database_file_and_owner_hop_capabilities(self) -> None:
        """Detect representative prohibited includes and executable identifiers."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = self.write(
                repository,
                "src/aegis/runtime/submission_coordinator.cpp",
                "#include <sys/socket.h>\n"
                "#include <fstream>\n"
                "void violate() {\n"
                "  socket(0, 0, 0);\n"
                '  sqlite3_open("state", nullptr);\n'
                "  SerializedExecutor executor;\n"
                "}\n",
            )
            findings = scanner.scan_paths(repository, [source])
            rules = {finding.rule for finding in findings}
            self.assertIn("forbidden include", rules)
            self.assertIn("network API", rules)
            self.assertIn("database API", rules)
            self.assertIn("executor handoff", rules)
            errors = io.StringIO()
            with redirect_stderr(errors):
                self.assertEqual(scanner.main(["--root", str(repository), str(source)]), 1)
            self.assertIn("forbidden include", errors.getvalue())

    # --------------------------------------------------------
    # Every assigned socket, address, DNS, and event header family must independently fail closed.
    def test_rejects_complete_network_header_signature_table(self) -> None:
        """Exercise each platform and event-library branch in the forbidden include grammar."""

        headers = (
            "arpa/inet.h",
            "arpa/nameser.h",
            "asio.hpp",
            "boost/asio/ip/tcp.hpp",
            "curl/curl.h",
            "event.h",
            "event2/event.h",
            "ev.h",
            "grpcpp/grpcpp.h",
            "httplib.h",
            "ifaddrs.h",
            "iphlpapi.h",
            "mstcpip.h",
            "mysql/mysql.h",
            "net/if.h",
            "netdb.h",
            "netinet/tcp.h",
            "openssl/ssl.h",
            "poll.h",
            "pqxx/pqxx",
            "resolv.h",
            "sqlite3.h",
            "sys/epoll.h",
            "sys/event.h",
            "sys/poll.h",
            "sys/select.h",
            "sys/socket.h",
            "sys/un.h",
            "uv.h",
            "websocketpp/client.hpp",
            "winsock2.h",
            "ws2tcpip.h",
        )
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = self.write(repository, "include/aegis/execution/order_request.hpp", "")
            for header in headers:
                with self.subTest(header=header):
                    source.write_text(f"#include <{header}>\n", encoding="utf-8")
                    rules = {finding.rule for finding in scanner.scan_paths(repository, [source])}
                    self.assertIn("forbidden include", rules)

    # --------------------------------------------------------
    # Call signatures cover socket lifecycle, data transfer, DNS, readiness, and event-loop APIs.
    def test_rejects_complete_network_api_signature_table(self) -> None:
        """Exercise each API-family branch without relying on a corresponding include."""

        signatures = {
            "network API": (
                "socket",
                "socketpair",
                "connect",
                "bind",
                "listen",
                "accept",
                "accept4",
                "shutdown",
                "send",
                "sendto",
                "sendmsg",
                "sendmmsg",
                "recv",
                "recvfrom",
                "recvmsg",
                "recvmmsg",
                "getsockname",
                "getpeername",
                "getsockopt",
                "setsockopt",
                "inet_pton",
                "htonl",
                "WSAStartup",
                "WSASend",
                "curl_easy_init",
                "SSL_connect",
            ),
            "DNS API": (
                "getaddrinfo",
                "freeaddrinfo",
                "getnameinfo",
                "gethostbyname",
                "gethostbyname2",
                "gethostbyaddr",
                "gethostent",
                "sethostent",
                "endhostent",
                "res_init",
                "res_query",
                "res_search",
                "res_send",
                "dn_expand",
            ),
            "event API": (
                "select",
                "pselect",
                "poll",
                "ppoll",
                "epoll_create",
                "epoll_create1",
                "epoll_ctl",
                "epoll_wait",
                "epoll_pwait",
                "kqueue",
                "kevent",
                "event_base_new",
                "event_base_dispatch",
                "event_new",
                "event_add",
                "event_del",
                "event_free",
                "ev_io_init",
                "ev_run",
                "uv_loop_init",
                "uv_run",
                "uv_tcp_init",
                "uv_poll_init",
            ),
        }
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = self.write(repository, "include/aegis/execution/order_request.hpp", "")
            for expected_rule, names in signatures.items():
                for name in names:
                    with self.subTest(rule=expected_rule, name=name):
                        source.write_text(f"void violate() {{ {name}(); }}\n", encoding="utf-8")
                        rules = [
                            finding.rule for finding in scanner.scan_paths(repository, [source])
                        ]
                        self.assertEqual(rules, [expected_rule])

    # --------------------------------------------------------
    # Test and benchmark setup may drive M2 ingress, while every scanned scope remains offline.
    def test_scopes_owner_hops_to_production_but_network_rules_to_both(self) -> None:
        """Distinguish fixture admission from a direct-path hop without weakening live checks."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            coordinator = self.write(
                repository,
                "src/aegis/runtime/submission_coordinator.cpp",
                "void production() { try_admit(); socket(0, 0, 0); int credential = 0; }\n",
            )
            benchmark = self.write(
                repository,
                "benchmarks/m3_submission_benchmark.cpp",
                "void fixture() { try_admit(); socket(0, 0, 0); }\n",
            )
            unit_test = self.write(
                repository,
                "tests/unit/runtime/submission_coordinator_test.cpp",
                "void fixture() { try_admit(); socket(0, 0, 0); int credential = 0; }\n",
            )

            production_rules = [
                finding.rule for finding in scanner.scan_paths(repository, [coordinator])
            ]
            benchmark_rules = [
                finding.rule for finding in scanner.scan_paths(repository, [benchmark])
            ]
            unit_test_rules = [
                finding.rule for finding in scanner.scan_paths(repository, [unit_test])
            ]
            self.assertEqual(
                production_rules,
                ["executor handoff", "network API", "credential surface"],
            )
            self.assertEqual(benchmark_rules, ["network API"])
            self.assertEqual(unit_test_rules, ["network API", "credential surface"])

            benchmark.write_text("void fixture() { try_admit(); }\n", encoding="utf-8")
            self.assertEqual(scanner.scan_paths(repository, [benchmark]), [])

    # --------------------------------------------------------
    # Shared arithmetic/model implementations execute inline and therefore receive all direct rules.
    def test_applies_every_direct_rule_to_an_audited_shared_dependency(self) -> None:
        """Prove direct manifest expansion rejects every blocking, queue, and handoff family."""

        includes = ("condition_variable", "coroutine", "fstream", "future", "mutex", "queue")
        identifiers = {
            "fopen()": "blocking file API",
            "co_await value": "coroutine or future",
            "std::future<int> value": "coroutine or future",
            "std::mutex value": "blocking synchronization",
            "std::queue<int> value": "general-purpose queue",
            "try_admit()": "executor handoff",
        }
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = self.write(repository, "src/aegis/model/fixed_point.cpp", "")
            for header in includes:
                with self.subTest(header=header):
                    source.write_text(f"#include <{header}>\n", encoding="utf-8")
                    rules = [finding.rule for finding in scanner.scan_paths(repository, [source])]
                    self.assertEqual(rules, ["forbidden include"])
            for text, expected_rule in identifiers.items():
                with self.subTest(text=text, expected_rule=expected_rule):
                    source.write_text(f"void violate() {{ {text}; }}\n", encoding="utf-8")
                    rules = [finding.rule for finding in scanner.scan_paths(repository, [source])]
                    self.assertEqual(rules, [expected_rule])

    # --------------------------------------------------------
    # Exact requires-expressions may prove a member absent, but executable use remains forbidden.
    def test_allows_negative_member_probes_without_hiding_executable_network_calls(self) -> None:
        """Limit the negative-probe exception to one inert compile-time grammar."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = self.write(
                repository,
                "tests/unit/execution/fake_transport_initiator_test.cpp",
                "template <typename Value>\n"
                "concept HasSocket = requires(Value value) { value.socket; };\n"
                "static_assert(!HasSocket<int>);\n",
            )
            self.assertEqual(scanner.scan_paths(repository, [source]), [])

            source.write_text(
                "template <typename Value>\n"
                "concept HasSocket = requires(Value value) { value.socket; };\n",
                encoding="utf-8",
            )
            findings = scanner.scan_paths(repository, [source])
            self.assertEqual([finding.rule for finding in findings], ["network API"])

            source.write_text(
                "template <typename Value>\n"
                "concept HasSocket = requires(Value value) { value.socket; };\n"
                "static_assert(!HasSocket<int>);\n"
                "template <typename Value>\n"
                "concept HasSend = requires(Value value) { value.send(); };\n"
                "static_assert(!HasSend<int>);\n"
                "template <typename Value>\n"
                "concept HasConnect = requires(Value value) { value.connect(); };\n"
                "static_assert(!HasConnect<int>);\n"
                "void violate() { socket(0, 0, 0); }\n",
                encoding="utf-8",
            )
            findings = scanner.scan_paths(repository, [source])
            self.assertEqual([finding.rule for finding in findings], ["network API"])

    # --------------------------------------------------------
    # Only exact immutable Catch2 and Google Benchmark archive URLs are accepted in build files.
    def test_allows_pinned_dependency_urls_and_rejects_other_urls(self) -> None:
        """Keep the narrow build-dependency exception exact and host-specific."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            allowed = self.write(
                repository,
                "CMakeLists.txt",
                'set(CATCH_URL "https://github.com/catchorg/Catch2/archive/'
                '644821ce28cb25d7992a4d0375b1d83214392592.tar.gz")\n',
            )
            self.assertEqual(scanner.scan_paths(repository, [allowed]), [])

            denied = self.write(
                repository,
                "benchmarks/CMakeLists.txt",
                'set(LIVE_URL "https://exchange.example/orders")\n',
            )
            findings = scanner.scan_paths(repository, [denied])
            self.assertEqual(len(findings), 1)
            self.assertEqual(findings[0].rule, "live URL")

    # --------------------------------------------------------
    # Approved dependency names cannot switch revision, checksum, source, or acquisition mechanism.
    def test_rejects_fetchcontent_pin_and_acquisition_drift(self) -> None:
        """Fail closed on alternate or incomplete Catch2 and Google Benchmark declarations."""

        catch_url = (
            "https://github.com/catchorg/Catch2/archive/"
            "644821ce28cb25d7992a4d0375b1d83214392592.tar.gz"
        )
        catch_hash = "SHA256=5536f5e936466e3d0ef6350630d17d976fee2dee3cc44940e77a769cafab1904"
        benchmark_url = (
            "https://github.com/google/benchmark/archive/"
            "192ef10025eb2c4cdd392bc502f0c852196baa48.tar.gz"
        )
        benchmark_hash = "SHA256=f82705a2726d8f6cdcda274b841f6314dbfc6f731cdda06c946f310ec1cc3ad9"
        violations = {
            "alternate Catch2 revision": (
                "FetchContent_Declare(Catch2 URL "
                "https://github.com/catchorg/Catch2/archive/"
                f"{'a' * 40}.tar.gz URL_HASH {catch_hash} "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n"
            ),
            "alternate Catch2 hash": (
                f"FetchContent_Declare(Catch2 URL {catch_url} "
                f"URL_HASH SHA256={'b' * 64} "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n"
            ),
            "missing Catch2 hash": (
                f"FetchContent_Declare(Catch2 URL {catch_url} "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n"
            ),
            "Catch2 Git acquisition": (
                "FetchContent_Declare(Catch2 GIT_REPOSITORY "
                "https://github.com/catchorg/Catch2.git GIT_TAG "
                "644821ce28cb25d7992a4d0375b1d83214392592)\n"
            ),
            "Catch2 Git tag option": (
                f"FetchContent_Declare(Catch2 URL {catch_url} "
                f"URL_HASH {catch_hash} GIT_TAG "
                "644821ce28cb25d7992a4d0375b1d83214392592 "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n"
            ),
            "Catch2 source directory": ("FetchContent_Declare(Catch2 SOURCE_DIR /tmp/catch2)\n"),
            "alternate benchmark revision": (
                "FetchContent_Declare(google_benchmark URL "
                "https://github.com/google/benchmark/archive/"
                f"{'c' * 40}.tar.gz URL_HASH {benchmark_hash} "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n"
            ),
            "alternate benchmark hash": (
                f"FetchContent_Declare(google_benchmark URL {benchmark_url} "
                f"URL_HASH SHA256={'d' * 64} "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n"
            ),
            "missing benchmark hash": (
                f"FetchContent_Declare(google_benchmark URL {benchmark_url} "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n"
            ),
        }
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            cmake = self.write(repository, "CMakeLists.txt", "")
            for name, text in violations.items():
                with self.subTest(name=name):
                    cmake.write_text(text, encoding="utf-8")
                    rules = [finding.rule for finding in scanner.scan_paths(repository, [cmake])]
                    self.assertIn("unapproved CMake dependency", rules)

    # --------------------------------------------------------
    # Build acquisition and link surfaces are scanned even when they contain no source-level API.
    def test_rejects_build_dependencies_links_flags_modules_and_presets(self) -> None:
        """Block representative network, database, event, and remote-service build acquisition."""

        violations = {
            "find_package(CURL REQUIRED)\n": "network build dependency",
            "target_link_libraries(aegis PRIVATE OpenSSL::SSL)\n": "network build dependency",
            'set(CMAKE_EXE_LINKER_FLAGS "-lresolv")\n': "network build dependency",
            "find_package(SQLite3 REQUIRED)\n": "database build dependency",
            "target_link_libraries(aegis PRIVATE pqxx)\n": "database build dependency",
            "find_package(Protobuf REQUIRED)\n": "remote-service build dependency",
            "find_package(UnreviewedClient REQUIRED)\n": "unapproved CMake dependency",
            "target_link_libraries(aegis PRIVATE innocuous_name)\n": "unapproved CMake link",
            "target_link_options(aegis PRIVATE -levil)\n": "unapproved CMake link",
            "link_libraries(innocuous_name)\n": "unapproved CMake link",
            "ExternalProject_Add(vendor_client)\n": "unapproved CMake dependency",
        }
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            cmake = self.write(repository, "cmake/RemoteDependency.cmake", "")
            preset = self.write(repository, "CMakePresets.json", "{}\n")
            for text, expected_rule in violations.items():
                with self.subTest(text=text.strip(), expected_rule=expected_rule):
                    cmake.write_text(text, encoding="utf-8")
                    rules = [finding.rule for finding in scanner.scan_paths(repository, [cmake])]
                    self.assertIn(expected_rule, rules)

            preset.write_text(
                '{"configurePresets":[{"cacheVariables":{"CMAKE_CXX_FLAGS":"-lcurl"}}]}\n',
                encoding="utf-8",
            )
            rules = [finding.rule for finding in scanner.scan_paths(repository, [preset])]
            self.assertEqual(rules, ["network build dependency", "unapproved linker flag"])

    # --------------------------------------------------------
    # The repository's immutable source tools and local runtime/tooling dependencies stay accepted.
    def test_accepts_current_pinned_build_dependency_vocabulary(self) -> None:
        """Keep Threads, Python3, Catch2, and Google Benchmark inside the build allow-set."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            cmake = self.write(
                repository,
                "CMakeLists.txt",
                "find_package(Threads REQUIRED)\n"
                "find_package(Python3 REQUIRED COMPONENTS Interpreter)\n"
                "target_link_libraries(aegis PRIVATE Threads::Threads)\n"
                "target_link_libraries(tests PRIVATE Catch2::Catch2WithMain)\n"
                "target_link_libraries(benchmarks PRIVATE benchmark::benchmark)\n"
                "FetchContent_Declare(Catch2 URL "
                "https://github.com/catchorg/Catch2/archive/"
                "644821ce28cb25d7992a4d0375b1d83214392592.tar.gz URL_HASH "
                "SHA256=5536f5e936466e3d0ef6350630d17d976fee2dee3cc44940e77a769cafab1904 "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n"
                "FetchContent_Declare(google_benchmark URL "
                "https://github.com/google/benchmark/archive/"
                "192ef10025eb2c4cdd392bc502f0c852196baa48.tar.gz URL_HASH "
                "SHA256=f82705a2726d8f6cdcda274b841f6314dbfc6f731cdda06c946f310ec1cc3ad9 "
                "DOWNLOAD_EXTRACT_TIMESTAMP TRUE SYSTEM)\n",
            )
            self.assertEqual(scanner.scan_paths(repository, [cmake]), [])

    # --------------------------------------------------------
    # Python import/qualified-call detection is language-aware and ignores inert fixture strings.
    def test_rejects_python_network_namespaces_but_ignores_literal_data(self) -> None:
        """Prevent Python tooling from importing communication libraries without fixture leakage."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = self.write(
                repository,
                "tools/probe.py",
                'TEXT = "import socket; socket.connect()"\nimport requests\n',
            )
            findings = scanner.scan_paths(repository, [source])
            self.assertEqual([finding.rule for finding in findings], ["network namespace"])

            source.write_text("from urllib.request import urlopen\n", encoding="utf-8")
            findings = scanner.scan_paths(repository, [source])
            self.assertEqual([finding.rule for finding in findings], ["network namespace"])

    # --------------------------------------------------------
    # The sole safe credential spelling masks only checkout's explicitly disabled YAML key.
    def test_credential_exception_is_exact_and_cannot_mask_neighboring_capabilities(self) -> None:
        """Accept disabled checkout persistence while rejecting other secret surfaces."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            workflow = self.write(
                repository,
                ".github/workflows/ci.yml",
                "persist-credentials: false\n",
            )
            self.assertEqual(scanner.scan_paths(repository, [workflow]), [])

            workflow.write_text("persist-credentials: true\n", encoding="utf-8")
            findings = scanner.scan_paths(repository, [workflow])
            self.assertEqual([finding.rule for finding in findings], ["credential surface"])

            workflow.write_text(
                "persist-credentials: false\ncredentials: enabled\n", encoding="utf-8"
            )
            findings = scanner.scan_paths(repository, [workflow])
            self.assertEqual([finding.rule for finding in findings], ["credential surface"])

    # --------------------------------------------------------
    # Address vocabulary receives no general timing-variable exception in production source.
    def test_rejects_endpoint_even_when_typed_like_a_measurement(self) -> None:
        """Prove a plausible timing declaration cannot hide an endpoint surface."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = self.write(
                repository,
                "src/aegis/runtime/submission_coordinator.cpp",
                "std::optional<std::uint64_t> endpoint;\n",
            )
            findings = scanner.scan_paths(repository, [source])
            self.assertEqual([finding.rule for finding in findings], ["live-address surface"])

    # --------------------------------------------------------
    # Default discovery covers each M3 artifact class and reports a stable canonical path order.
    def test_default_scope_is_bounded_and_findings_are_sorted(self) -> None:
        """Cover the explicit general/direct/build union without unrelated future-milestone code."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            self.write(
                repository,
                "src/aegis/risk/z.cpp",
                "void z() { socket(0, 0, 0); }\n",
            )
            self.write(
                repository,
                "src/aegis/oms/outbound_oms.cpp",
                'void oms() { sqlite3_open("state", nullptr); }\n',
            )
            self.write(repository, "src/aegis/model/fixed_point.cpp", "void fixed_point() {}\n")
            self.write(
                repository,
                "include/aegis/risk/a.hpp",
                "void a() { connect(0, 0, 0); }\n",
            )
            self.write(
                repository,
                "src/aegis/public_connectivity/later.cpp",
                "void allowed_later() { connect(0, 0, 0); }\n",
            )
            self.write(
                repository,
                "tests/unit/runtime/submission_coordinator_test.cpp",
                "void unit_contract() {}\n",
            )
            self.write(
                repository,
                "tests/support/reference_configuration.cpp",
                "void reference_configuration() {}\n",
            )
            self.write(
                repository,
                "tests/deterministic_scenarios/m3_reference_scenario_test.cpp",
                "void deterministic_scenario() {}\n",
            )
            self.write(
                repository,
                "tests/tooling/m3_benchmark_evidence_test.py",
                "VALUE = 1\n",
            )
            self.write(repository, "CMakeLists.txt", "find_package(Threads REQUIRED)\n")
            self.write(
                repository,
                "CMakePresets.json",
                '{"configurePresets":[{"cacheVariables":{"CMAKE_CXX_FLAGS":"-lcurl"}}]}\n',
            )
            self.write(repository, "cmake/ProjectOptions.cmake", "include_guard(GLOBAL)\n")
            self.write(repository, "cmake/nested/Extra.cmake", "include(UnreviewedModule)\n")

            manifest = {
                "M3_GENERAL_DIRECTORY_PREFIXES": (
                    Path("include/aegis/risk"),
                    Path("src/aegis/risk"),
                ),
                "M3_GENERAL_FILE_PATTERNS": (
                    "src/aegis/oms/outbound_oms.cpp",
                    "src/aegis/model/fixed_point.cpp",
                    "tests/unit/runtime/submission_coordinator_test.cpp",
                    "tests/support/reference_configuration.cpp",
                    "tests/deterministic_scenarios/m3_reference_scenario_test.cpp",
                    "tests/tooling/m3_benchmark_evidence_test.py",
                ),
                "M3_DIRECT_PATH_FILE_PATTERNS": (
                    "src/aegis/model/fixed_point.cpp",
                    "src/aegis/oms/outbound_oms.cpp",
                ),
                "M4_GENERAL_FILE_PATTERNS": (),
                "M4_OWNER_PATH_FILE_PATTERNS": (),
                "BUILD_FILES": (
                    Path("CMakeLists.txt"),
                    Path("CMakePresets.json"),
                    Path("cmake/ProjectOptions.cmake"),
                ),
                "BUILD_DIRECTORY_PREFIXES": (Path("cmake"),),
            }
            with mock.patch.multiple(scanner, **manifest):
                discovered_once = [
                    path.relative_to(repository).as_posix()
                    for path in scanner.discover_default_paths(repository)
                ]
                discovered_twice = [
                    path.relative_to(repository).as_posix()
                    for path in scanner.discover_default_paths(repository)
                ]
                self.assertEqual(
                    discovered_once,
                    [
                        "CMakeLists.txt",
                        "CMakePresets.json",
                        "cmake/ProjectOptions.cmake",
                        "cmake/nested/Extra.cmake",
                        "include/aegis/risk/a.hpp",
                        "src/aegis/model/fixed_point.cpp",
                        "src/aegis/oms/outbound_oms.cpp",
                        "src/aegis/risk/z.cpp",
                        "tests/deterministic_scenarios/m3_reference_scenario_test.cpp",
                        "tests/support/reference_configuration.cpp",
                        "tests/tooling/m3_benchmark_evidence_test.py",
                        "tests/unit/runtime/submission_coordinator_test.cpp",
                    ],
                )
                self.assertEqual(discovered_twice, discovered_once)

                findings = scanner.scan_paths(repository)
                self.assertEqual(
                    [finding.path for finding in findings],
                    [
                        "CMakePresets.json",
                        "CMakePresets.json",
                        "cmake/nested/Extra.cmake",
                        "include/aegis/risk/a.hpp",
                        "src/aegis/oms/outbound_oms.cpp",
                        "src/aegis/risk/z.cpp",
                    ],
                )

    # --------------------------------------------------------
    # The exact submit-time dependency closure remains assigned to direct-path blocking/hop rules.
    def test_direct_manifest_covers_every_audited_submit_time_dependency(self) -> None:
        """Pin the shared model/configuration/organization paths that execute during submit."""

        required = {
            "include/aegis/configuration/configuration_provenance.hpp",
            "include/aegis/model/fixed_point.hpp",
            "src/aegis/model/fixed_point.cpp",
            "include/aegis/model/instrument_metadata.hpp",
            "src/aegis/model/instrument_metadata.cpp",
            "include/aegis/model/integer_input.hpp",
            "include/aegis/model/order_id.hpp",
            "src/aegis/model/order_id.cpp",
            "include/aegis/model/result.hpp",
            "include/aegis/model/sha256.hpp",
            "src/aegis/model/sha256.cpp",
            "include/aegis/organization/organization.hpp",
        }
        direct = set(scanner.M3_DIRECT_PATH_FILE_PATTERNS)
        self.assertTrue(required <= direct)
        discovered = {
            path.relative_to(REPOSITORY).as_posix()
            for path in scanner.discover_default_paths(REPOSITORY)
        }
        self.assertTrue(direct <= discovered)

    # --------------------------------------------------------
    # The explicit M4 manifest covers every current foundation file and applies owner-hop rules to
    # production normalization without widening those rules to test drivers.
    def test_m4_manifest_covers_foundation_and_owner_paths(self) -> None:
        """Pin current offline files and prove M4 production receives direct-path checks."""

        required = {
            "include/aegis/model/m4_provenance.hpp",
            "include/aegis/oms/private_order_event.hpp",
            "include/aegis/oms/private_order_identity.hpp",
            "include/aegis/recovery/recovery_identity.hpp",
            "include/aegis/runtime/m4_policy.hpp",
            "src/aegis/oms/private_order_event.cpp",
            "src/aegis/runtime/m4_policy.cpp",
            "src/aegis/runtime/m4_provenance_resolver.cpp",
            "src/aegis/runtime/m4_provenance_resolver.hpp",
            "src/aegis/runtime/private_order_event_factory.cpp",
            "src/aegis/runtime/private_order_event_factory.hpp",
            "tests/unit/model/m4_identity_test.cpp",
            "tests/unit/oms/private_order_event_test.cpp",
            "tests/unit/runtime/m4_policy_test.cpp",
            "tests/unit/runtime/m4_provenance_resolver_test.cpp",
        }
        general = set(scanner.M4_GENERAL_FILE_PATTERNS)
        owner = set(scanner.M4_OWNER_PATH_FILE_PATTERNS)
        self.assertTrue(required <= general)
        self.assertTrue(owner <= general)
        discovered = {
            path.relative_to(REPOSITORY).as_posix()
            for path in scanner.discover_default_paths(REPOSITORY)
        }
        self.assertTrue(general <= discovered)

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = self.write(
                repository,
                "src/aegis/runtime/private_order_event_factory.cpp",
                "#include <future>\nvoid owner_path() { SerializedExecutor executor; }\n",
            )
            findings = scanner.scan_paths(repository, [source])
            self.assertEqual(
                [finding.rule for finding in findings],
                ["forbidden include", "executor handoff"],
            )

    # --------------------------------------------------------
    # Default scans must not silently succeed after an explicitly assigned artifact disappears.
    def test_default_scan_fails_closed_for_missing_manifest_file(self) -> None:
        """Return scanner-error status two for a missing direct file."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            manifest = {
                "M3_GENERAL_DIRECTORY_PREFIXES": (),
                "M3_GENERAL_FILE_PATTERNS": ("present.cpp",),
                "M3_DIRECT_PATH_FILE_PATTERNS": ("missing-direct.cpp",),
                "M4_GENERAL_FILE_PATTERNS": (),
                "M4_OWNER_PATH_FILE_PATTERNS": (),
                "BUILD_FILES": (),
                "BUILD_DIRECTORY_PREFIXES": (),
            }
            present = self.write(repository, "present.cpp", "void clean() {}\n")
            with mock.patch.multiple(scanner, **manifest):
                with self.assertRaisesRegex(
                    FileNotFoundError, "required scanner manifest file is missing"
                ):
                    scanner.discover_default_paths(repository)

                errors = io.StringIO()
                with redirect_stderr(errors):
                    self.assertEqual(scanner.main(["--root", str(repository)]), 2)
                self.assertIn("missing-direct.cpp", errors.getvalue())

                # A focused explicit scan intentionally bypasses the repository-default manifest.
                self.assertEqual(scanner.scan_paths(repository, [present]), [])

    # --------------------------------------------------------
    # Directory-backed discovery also fails closed rather than treating a removed root as empty.
    def test_default_scan_fails_closed_for_missing_manifest_directory(self) -> None:
        """Require both M3 recursive roots and the recursive CMake-module root to exist."""

        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            manifest = {
                "M3_GENERAL_DIRECTORY_PREFIXES": (Path("src/aegis/risk"),),
                "M3_GENERAL_FILE_PATTERNS": (),
                "M3_DIRECT_PATH_FILE_PATTERNS": (),
                "M4_GENERAL_FILE_PATTERNS": (),
                "M4_OWNER_PATH_FILE_PATTERNS": (),
                "BUILD_FILES": (),
                "BUILD_DIRECTORY_PREFIXES": (),
            }
            with mock.patch.multiple(scanner, **manifest):
                with self.assertRaisesRegex(
                    FileNotFoundError, "required scanner manifest directory is missing"
                ):
                    scanner.discover_default_paths(repository)

    # --------------------------------------------------------


# ########################################################################


if __name__ == "__main__":
    unittest.main()
