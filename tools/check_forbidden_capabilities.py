"""Reject live capabilities from M3/M4 paths.

Owner-local production paths additionally reject blocking work and execution handoffs.
"""

from __future__ import annotations

import argparse
import io
import re
import sys
import tokenize
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

# General M3 paths are explicit so future connectivity milestones do not weaken this proof. Build
# files are checked too because a linked capability can bypass source-level include inspection.
M3_GENERAL_DIRECTORY_PREFIXES = (
    Path("include/aegis/risk"),
    Path("src/aegis/risk"),
    Path("tests/unit/risk"),
)
M3_GENERAL_FILE_PATTERNS = (
    "include/aegis/model/domain_error.hpp",
    "include/aegis/model/identifier.hpp",
    "include/aegis/model/time.hpp",
    "include/aegis/execution/order_request.hpp",
    "include/aegis/execution/order_submission.hpp",
    "include/aegis/execution/order_validation.hpp",
    "include/aegis/execution/execution_route.hpp",
    "include/aegis/execution/submission_route.hpp",
    "include/aegis/execution/submit_result.hpp",
    "include/aegis/execution/submission_policy.hpp",
    "include/aegis/oms/outbound_oms.hpp",
    "include/aegis/execution/fake_order_encoder.hpp",
    "include/aegis/execution/fake_transport_initiator.hpp",
    "include/aegis/runtime/fake_submission_runtime.hpp",
    "src/aegis/runtime/submission_coordinator.hpp",
    "include/aegis/execution/submission_measurement_clock.hpp",
    "include/aegis/risk/risk_scope.hpp",
    "src/aegis/model/identifier.cpp",
    "src/aegis/execution/order_validation.cpp",
    "src/aegis/execution/execution_route.cpp",
    "src/aegis/execution/submission_route.cpp",
    "src/aegis/execution/submission_policy.cpp",
    "src/aegis/oms/outbound_oms.cpp",
    "src/aegis/execution/fake_order_encoder.cpp",
    "src/aegis/execution/fake_transport_initiator.cpp",
    "src/aegis/runtime/submission_coordinator.cpp",
    "src/aegis/execution/submission_measurement_clock.cpp",
    "include/aegis/runtime/submission_diagnostics.hpp",
    "src/aegis/runtime/submission_diagnostics.cpp",
    "include/aegis/trace/submission_trace.hpp",
    "src/aegis/trace/submission_trace.cpp",
    "include/aegis/runtime/bot_runtime.hpp",
    "src/aegis/runtime/bot_runtime.cpp",
    "include/aegis/runtime/market_runtime.hpp",
    "src/aegis/runtime/market_runtime.cpp",
    "tests/unit/model/identifier_test.cpp",
    "tests/unit/execution/execution_route_test.cpp",
    "tests/unit/execution/fake_order_encoder_test.cpp",
    "tests/unit/execution/fake_transport_initiator_test.cpp",
    "tests/unit/execution/order_request_test.cpp",
    "tests/unit/execution/route_authorization_test.cpp",
    "tests/unit/runtime/submission_coordinator_test.cpp",
    "tests/unit/execution/submission_measurement_clock_test.cpp",
    "tests/unit/oms/outbound_oms_test.cpp",
    "tests/unit/runtime/bot_runtime_test.cpp",
    "tests/unit/runtime/market_runtime_test.cpp",
    "tests/unit/runtime/submission_diagnostics_test.cpp",
    "tests/unit/trace/submission_trace_test.cpp",
    "tests/support/reference_configuration.cpp",
    "tests/support/reference_configuration.hpp",
    "tests/deterministic_scenarios/m3_reference_scenario_test.cpp",
    "tests/tooling/forbidden_capabilities_test.py",
    "tests/tooling/m3_benchmark_evidence_test.py",
    "tools/check_forbidden_capabilities.py",
    "tools/run_benchmarks.py",
    "tools/validate_benchmark_evidence.py",
    "benchmarks/support/allocation_tracking.cpp",
    "benchmarks/support/allocation_tracking.hpp",
    "benchmarks/support/distribution_metrics.cpp",
    "benchmarks/support/distribution_metrics.hpp",
    "benchmarks/m3_submission_benchmark.cpp",
)

# Direct-path rules use a separate exact production manifest. Test, scenario, benchmark, tooling,
# build, and MarketRuntime setup code remain under the general live-capability proof without
# becoming false owner-hop findings when they legitimately drive the existing M2 executor.
M3_DIRECT_PATH_FILE_PATTERNS = (
    "include/aegis/configuration/configuration_provenance.hpp",
    "include/aegis/model/domain_error.hpp",
    "include/aegis/model/fixed_point.hpp",
    "include/aegis/model/identifier.hpp",
    "include/aegis/model/instrument_metadata.hpp",
    "include/aegis/model/integer_input.hpp",
    "include/aegis/model/order_id.hpp",
    "include/aegis/model/result.hpp",
    "include/aegis/model/sha256.hpp",
    "include/aegis/model/time.hpp",
    "include/aegis/execution/order_request.hpp",
    "include/aegis/execution/order_submission.hpp",
    "include/aegis/execution/order_validation.hpp",
    "include/aegis/execution/execution_route.hpp",
    "include/aegis/execution/submission_route.hpp",
    "include/aegis/execution/submit_result.hpp",
    "include/aegis/execution/submission_policy.hpp",
    "include/aegis/execution/fake_order_encoder.hpp",
    "include/aegis/execution/fake_transport_initiator.hpp",
    "include/aegis/runtime/fake_submission_runtime.hpp",
    "src/aegis/runtime/submission_coordinator.hpp",
    "include/aegis/execution/submission_measurement_clock.hpp",
    "include/aegis/oms/outbound_oms.hpp",
    "include/aegis/risk/exposure.hpp",
    "include/aegis/risk/reservation_ledger.hpp",
    "include/aegis/risk/risk_policy.hpp",
    "include/aegis/risk/risk_scope.hpp",
    "include/aegis/runtime/bot_runtime.hpp",
    "include/aegis/runtime/submission_diagnostics.hpp",
    "include/aegis/trace/submission_trace.hpp",
    "include/aegis/organization/organization.hpp",
    "src/aegis/model/fixed_point.cpp",
    "src/aegis/model/identifier.cpp",
    "src/aegis/model/instrument_metadata.cpp",
    "src/aegis/model/order_id.cpp",
    "src/aegis/model/sha256.cpp",
    "src/aegis/execution/order_validation.cpp",
    "src/aegis/execution/execution_route.cpp",
    "src/aegis/execution/submission_route.cpp",
    "src/aegis/execution/submission_policy.cpp",
    "src/aegis/execution/fake_order_encoder.cpp",
    "src/aegis/execution/fake_transport_initiator.cpp",
    "src/aegis/runtime/submission_coordinator.cpp",
    "src/aegis/execution/submission_measurement_clock.cpp",
    "src/aegis/oms/outbound_oms.cpp",
    "src/aegis/risk/exposure.cpp",
    "src/aegis/risk/reservation_ledger.cpp",
    "src/aegis/risk/risk_policy.cpp",
    "src/aegis/runtime/bot_runtime.cpp",
    "src/aegis/runtime/submission_diagnostics.cpp",
    "src/aegis/trace/submission_trace.cpp",
)

# M4 paths remain an explicit credential-free/offline manifest so later connectivity milestones
# cannot silently make the OMS, inventory, recovery foundation, tests, or evidence helpers live.
M4_GENERAL_FILE_PATTERNS = (
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
    "tests/support/m4_private_event_fixture.hpp",
    "tests/support/m4_test_authority.cpp",
    "tests/support/m4_test_authority.hpp",
    "tests/unit/model/m4_identity_test.cpp",
    "tests/unit/oms/private_order_event_test.cpp",
    "tests/unit/runtime/m4_policy_test.cpp",
    "tests/unit/runtime/m4_provenance_resolver_test.cpp",
)

# These current M4 production files can execute during private owner admission/normalization and
# therefore inherit the M3 direct path's no-file, no-handoff, no-blocking rule set.
M4_OWNER_PATH_FILE_PATTERNS = (
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
)
BUILD_FILES = (
    Path("CMakeLists.txt"),
    Path("tests/CMakeLists.txt"),
    Path("benchmarks/CMakeLists.txt"),
    Path("cmake/ProjectOptions.cmake"),
    Path("CMakePresets.json"),
    Path(".github/workflows/ci.yml"),
)
BUILD_DIRECTORY_PREFIXES = (Path("cmake"),)

# Two scanner-owned Python files embed malicious snippets as inert self-test data. Their executable
# Python tokens remain scanned, but embedded strings are excluded from the default URL pass so those
# deliberate negative fixtures cannot make the live repository fail its own proof.
EMBEDDED_SCANNER_FIXTURE_PATHS = {
    Path("tools/check_forbidden_capabilities.py"),
    Path("tests/tooling/forbidden_capabilities_test.py"),
}

# Exact dependency-host exceptions are build inputs, never fake transport capabilities. The complete
# token sequences bind each allowed FetchContent name to one immutable archive, SHA-256, and option
# vocabulary; any revision, checksum, acquisition method, or option drift fails closed.
CATCH2_FETCHCONTENT_URL = (
    "https://github.com/catchorg/Catch2/archive/644821ce28cb25d7992a4d0375b1d83214392592.tar.gz"
)
CATCH2_FETCHCONTENT_HASH = "SHA256=5536f5e936466e3d0ef6350630d17d976fee2dee3cc44940e77a769cafab1904"
GOOGLE_BENCHMARK_FETCHCONTENT_URL = (
    "https://github.com/google/benchmark/archive/192ef10025eb2c4cdd392bc502f0c852196baa48.tar.gz"
)
GOOGLE_BENCHMARK_FETCHCONTENT_HASH = (
    "SHA256=f82705a2726d8f6cdcda274b841f6314dbfc6f731cdda06c946f310ec1cc3ad9"
)
PINNED_FETCHCONTENT_DECLARATIONS: dict[str, tuple[str, ...]] = {
    "Catch2": (
        "Catch2",
        "URL",
        CATCH2_FETCHCONTENT_URL,
        "URL_HASH",
        CATCH2_FETCHCONTENT_HASH,
        "DOWNLOAD_EXTRACT_TIMESTAMP",
        "TRUE",
        "SYSTEM",
    ),
    "google_benchmark": (
        "google_benchmark",
        "URL",
        GOOGLE_BENCHMARK_FETCHCONTENT_URL,
        "URL_HASH",
        GOOGLE_BENCHMARK_FETCHCONTENT_HASH,
        "DOWNLOAD_EXTRACT_TIMESTAMP",
        "TRUE",
        "SYSTEM",
    ),
}
ALLOWED_BUILD_URLS = frozenset({CATCH2_FETCHCONTENT_URL, GOOGLE_BENCHMARK_FETCHCONTENT_URL})

# General include rules reject communication, authentication, and database dependencies from every
# selected M3/M4 production, build, and benchmark path even when no call site exists yet.
GENERAL_FORBIDDEN_INCLUDE = re.compile(
    r"^\s*#\s*include\s*[<\"](?:"
    r"arpa/(?:inet|nameser(?:_compat)?)\.h|asio(?:\.hpp|/)|boost/asio(?:\.hpp|/)|curl/|"
    r"event2/|event\.h|ev\.h|grpc(?:pp)?/|httplib\.h|ifaddrs\.h|iphlpapi\.h|mstcpip\.h|"
    r"mysql/|net/if\.h|netdb\.h|netinet/|openssl/|poll\.h|pqxx/|resolv\.h|sqlite3\.h|"
    r"sys/(?:epoll|event|poll|select|socket|un)\.h|uv\.h|websocket|winsock2?\.h|ws2tcpip\.h"
    r")",
    re.MULTILINE | re.IGNORECASE,
)

# The direct path additionally forbids every blocking/handoff-capable standard-library dependency.
DIRECT_PATH_FORBIDDEN_INCLUDE = re.compile(
    r"^\s*#\s*include\s*[<\"](?:condition_variable|coroutine|fstream|future|mutex|queue)[>\"]",
    re.MULTILINE | re.IGNORECASE,
)

# General identifier rules catch live calls, linked clients, address surfaces, and secret surfaces.
# Word boundaries avoid unrelated spellings such as authorization or viewport.
GENERAL_FORBIDDEN_IDENTIFIERS: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "network API",
        re.compile(
            r"\b(?:accept4?|bind|connect|getpeername|getsockname|getsockopt|hton[ls]|"
            r"inet_(?:addr|aton|ntoa|ntop|pton)|listen|ntoh[ls]|"
            r"recv|recvfrom|recvmmsg|recvmsg|send|sendmmsg|sendmsg|sendto|setsockopt|shutdown|"
            r"socket|socketpair|WSAAccept|WSACleanup|WSAConnect|WSARecv|WSASend|WSASocket|"
            r"WSAStartup|curl_easy_init|curl_multi_init|SSL_connect|BIO_new_connect|WebSocket|"
            r"grpc_channel)\b"
        ),
    ),
    (
        "DNS API",
        re.compile(
            r"\b(?:dn_expand|endhostent|freeaddrinfo|gai_strerror|getaddrinfo|gethostbyaddr|"
            r"gethostbyname2?|gethostent|getnameinfo|res_init|res_query|res_search|res_send|"
            r"sethostent)\b"
        ),
    ),
    (
        "event API",
        re.compile(
            r"\b(?:epoll_create1?|epoll_ctl|epoll_pwait|epoll_wait|event_add|event_base_dispatch|"
            r"event_base_loop|event_base_new|event_del|event_free|event_new|ev_io_init|ev_run|"
            r"kevent|kqueue|poll|ppoll|pselect|select|uv_loop_init|uv_poll_init|uv_run|"
            r"uv_tcp_init)\b"
        ),
    ),
    (
        "network namespace",
        re.compile(r"\b(?:boost::asio|asio::|grpc::|httplib::|websocketpp::)"),
    ),
    (
        "database API",
        re.compile(r"\b(?:sqlite3_open|PQconnectdb|mysql_real_connect|pqxx::connection)\b"),
    ),
    (
        "live-address surface",
        re.compile(
            r"\b(?:endpoint|hostname|host_name|port_number|socket_handle|transport_handle|"
            r"http_url|https_url|websocket_url|ws_url)\b"
        ),
    ),
    (
        "credential surface",
        re.compile(
            r"\b(?:credential|credentials|password|api_key|api_secret|access_token|auth_token|"
            r"private_key)\b"
        ),
    ),
)

# Python package names require language-aware matching because ordinary C++ fixtures legitimately
# use plural variables such as requests. Imports and qualified calls still prove capability access.
PYTHON_FORBIDDEN_IDENTIFIERS: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "network namespace",
        re.compile(
            r"\b(?:requests|httpx|aiohttp|websockets|socket|asyncio)\s*\.|"
            r"\b(?:urllib\.request|selectors)\b|\b(?:from|import)\s+"
            r"(?:requests|httpx|aiohttp|websockets|socket|asyncio|selectors|urllib\.request)\b"
        ),
    ),
)

# Build files cannot acquire a forbidden client indirectly through package discovery, target links,
# raw linker flags, presets, or workflow configuration. Exact current dependencies (Threads,
# Python3, Catch2, and Google Benchmark) contain none of these capability signatures and remain
# accepted; adding any remote/network/database/event dependency requires a deliberate rule review.
BUILD_FORBIDDEN_DEPENDENCIES: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "network build dependency",
        re.compile(
            r"(?<![A-Za-z0-9_])(?:-l)?(?:asio|boost|curl|libcurl|grpc|httplib|libevent|libev|libuv|"
            r"openssl|websocketpp|winhttp|wininet|winsock|ws2_32|mswsock|iphlpapi|resolv|nsl)"
            r"(?=$|[^A-Za-z0-9_])",
            re.IGNORECASE,
        ),
    ),
    (
        "database build dependency",
        re.compile(
            r"(?<![A-Za-z0-9_])(?:-l)?(?:libpq|mariadb|mysql|odbc|postgresql|pqxx|sqlite3?)"
            r"(?=$|[^A-Za-z0-9_])",
            re.IGNORECASE,
        ),
    ),
    (
        "remote-service build dependency",
        re.compile(
            r"(?<![A-Za-z0-9_])(?:-l)?(?:aws-sdk|azure-sdk|google-cloud|protobuf)"
            r"(?=$|[^A-Za-z0-9_])",
            re.IGNORECASE,
        ),
    ),
    (
        "unapproved linker flag",
        re.compile(r"(?<![-A-Za-z0-9_])-l[A-Za-z0-9_+.-]+"),
    ),
)

# CMake dependency-acquisition and module-loading commands are deny-by-default. These names are the
# complete current repository vocabulary; a new dependency must update this proof deliberately.
ALLOWED_CMAKE_COMMAND_INPUTS: dict[str, frozenset[str]] = {
    "find_package": frozenset({"Python3", "Threads"}),
    "fetchcontent_declare": frozenset({"Catch2", "google_benchmark"}),
    "fetchcontent_makeavailable": frozenset({"Catch2", "google_benchmark"}),
    "include": frozenset({"Catch", "CTest", "FetchContent", "ProjectOptions"}),
    "add_subdirectory": frozenset({"benchmarks", "tests"}),
}
CMAKE_NAMED_INPUT_COMMAND = re.compile(
    r"\b(?P<command>add_subdirectory|FetchContent_Declare|FetchContent_MakeAvailable|"
    r"find_package|include)\s*\(\s*(?P<input>[^\s()]+)",
    re.IGNORECASE,
)
CMAKE_FETCHCONTENT_DECLARE = re.compile(
    r"\bFetchContent_Declare\s*\((?P<body>[^)]*)\)", re.IGNORECASE
)
CMAKE_UNAPPROVED_ACQUISITION_COMMAND = re.compile(
    r"\b(?:CPMAddPackage|ExternalProject_Add|find_library|pkg_check_modules)\s*\(",
    re.IGNORECASE,
)
CMAKE_UNAPPROVED_LINK_SURFACE = re.compile(
    r"\b(?:add_link_options|link_directories|link_libraries|target_link_directories)\s*\(|"
    r"\b(?:CMAKE_[A-Za-z0-9_]*LINKER_FLAGS|INTERFACE_LINK_LIBRARIES|LINK_LIBRARIES)\b",
    re.IGNORECASE,
)

# Every current link edge is local or one of the four approved build dependencies. Rejecting any
# other token prevents an innocuously named imported client from evading keyword signatures.
ALLOWED_CMAKE_LINK_INPUTS = frozenset(
    {
        "${AEGIS_BENCHMARK_REFERENCE_TARGET}",
        "Catch2::Catch2WithMain",
        "Threads::Threads",
        "aegis::aegis",
        "aegis_project_options",
        "aegis_project_warnings",
        "aegis_test_support",
        "benchmark::benchmark",
    }
)
CMAKE_LINK_COMMAND = re.compile(r"\btarget_link_libraries\s*\((?P<body>[^)]*)\)", re.IGNORECASE)
CMAKE_LINK_OPTIONS_COMMAND = re.compile(
    r"\btarget_link_options\s*\((?P<body>[^)]*)\)", re.IGNORECASE
)
CMAKE_ARGUMENT = re.compile(r'"[^"]*"|[^\s]+')
ALLOWED_CMAKE_LINK_OPTIONS = frozenset({"-fsanitize=${sanitizer_list}"})

# Compile-time requires-expressions are the one safe way M3/M4 tests prove forbidden members are
# absent. Only the exact one-member probe grammar is masked; executable calls and declarations
# remain visible.
NEGATIVE_CAPABILITY_PROBE = re.compile(
    r"\bconcept\s+(?P<concept>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r"requires\s*\([^)]*\b(?P<object>[A-Za-z_][A-Za-z0-9_]*)\s*\)\s*"
    r"\{\s*(?P=object)\.(?P<identifier>endpoint|hostname|host_name|port_number|socket|"
    r"socket_handle|transport_handle|accept|bind|connect|listen|recv|send|credential|credentials|"
    r"password|api_key|api_secret|"
    r"access_token|auth_token|private_key|http_url|https_url|websocket_url|ws_url)"
    r"\s*(?:\(\s*\))?\s*;\s*\}"
)

# These exact spellings are non-capability uses whose values prove their safe meaning. Each pattern
# masks only the named identifier, so a second or enabled credential setting is still rejected.
APPROVED_NON_CAPABILITY_IDENTIFIERS: dict[Path, tuple[re.Pattern[str], ...]] = {
    Path(".github/workflows/ci.yml"): (
        re.compile(r"\bpersist-(?P<identifier>credentials)\s*:\s*false\b"),
    ),
}

# Direct-path identifier rules reject blocking operations and every known owner/executor handoff.
DIRECT_PATH_FORBIDDEN_IDENTIFIERS: tuple[tuple[str, re.Pattern[str]], ...] = (
    (
        "blocking file API",
        re.compile(r"\b(?:fopen|freopen|std::ifstream|std::ofstream|std::fstream)\b"),
    ),
    (
        "coroutine or future",
        re.compile(r"\b(?:co_await|co_yield|std::async|std::future|std::promise)\b"),
    ),
    (
        "blocking synchronization",
        re.compile(
            r"\b(?:std::condition_variable|std::mutex|std::recursive_mutex|std::shared_mutex|"
            r"std::this_thread::sleep_for|std::this_thread::sleep_until)\b"
        ),
    ),
    (
        "general-purpose queue",
        re.compile(r"\bstd::(?:priority_)?queue\b"),
    ),
    (
        "executor handoff",
        re.compile(r"\b(?:SerializedExecutor|WorkItem|try_admit)\b"),
    ),
)


# ########################################################################
# One stable scanner finding carries repository-relative location and an assigned rule label.
@dataclass(frozen=True, order=True)
class Violation:
    """Describe one deterministic forbidden-capability match."""

    path: str
    line: int
    column: int
    rule: str
    token: str

    # --------------------------------------------------------


# ########################################################################


# --------------------------------------------------------
# Strip C/C++ comments while retaining string literals and exact line positions for diagnostics.
def strip_cpp_comments(text: str) -> str:
    """Replace comment bytes with spaces while preserving newlines and string literals."""

    output: list[str] = []
    index = 0
    state = "code"
    quote = ""

    while index < len(text):
        character = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""

        if state == "line_comment":
            if character == "\n":
                output.append(character)
                state = "code"
            else:
                output.append(" ")
            index += 1
            continue

        if state == "block_comment":
            if character == "*" and following == "/":
                output.extend((" ", " "))
                index += 2
                state = "code"
            else:
                output.append("\n" if character == "\n" else " ")
                index += 1
            continue

        if state == "string":
            output.append(character)
            if character == "\\" and following:
                output.append(following)
                index += 2
                continue
            if character == quote:
                state = "code"
            index += 1
            continue

        if character == "/" and following == "/":
            output.extend((" ", " "))
            index += 2
            state = "line_comment"
            continue
        if character == "/" and following == "*":
            output.extend((" ", " "))
            index += 2
            state = "block_comment"
            continue
        # C++ digit separators also use apostrophes (for example 1'000'000U), so only double
        # quotes enter this lightweight string state. Character literals cannot acquire a
        # forbidden capability and remain subject to ordinary comment stripping.
        if character == '"':
            quote = character
            state = "string"
        output.append(character)
        index += 1

    return "".join(output)


# --------------------------------------------------------
# Strip inert C++ string literals while preserving quoted include paths for dependency inspection.
def strip_cpp_string_literals(text: str) -> str:
    """Mask ordinary C++ strings without hiding quoted preprocessor includes."""

    spans: list[tuple[int, int]] = []
    index = 0
    line_start = 0
    while index < len(text):
        character = text[index]
        if character == "\n":
            line_start = index + 1
            index += 1
            continue
        if character != '"':
            index += 1
            continue

        prefix = text[line_start:index]
        preserve = re.fullmatch(r"\s*#\s*include\s*", prefix) is not None
        start = index
        index += 1
        while index < len(text):
            if text[index] == "\\" and index + 1 < len(text):
                index += 2
                continue
            if text[index] == '"':
                index += 1
                break
            index += 1
        if not preserve:
            spans.append((start, index))
    return mask_spans(text, spans)


# --------------------------------------------------------
# Strip CMake/Python-style comments without treating a hash inside a quoted URL as a comment.
def strip_hash_comments(text: str) -> str:
    """Replace unquoted hash comments with spaces while preserving source locations."""

    lines: list[str] = []
    for line in text.splitlines(keepends=True):
        quote = ""
        escaped = False
        result: list[str] = []
        for character in line:
            if escaped:
                result.append(character)
                escaped = False
                continue
            if character == "\\" and quote:
                result.append(character)
                escaped = True
                continue
            if character in ('"', "'"):
                if not quote:
                    quote = character
                elif quote == character:
                    quote = ""
                result.append(character)
                continue
            if character == "#" and not quote:
                result.extend(" " for _ in range(len(line) - len(result)))
                break
            result.append(character)
        lines.append("".join(result))
    return "".join(lines)


# --------------------------------------------------------
# Replace selected spans with spaces while preserving every newline and diagnostic coordinate.
def mask_spans(text: str, spans: Iterable[tuple[int, int]]) -> str:
    """Mask exact source spans without changing text length or line structure."""

    output = list(text)
    for start, end in spans:
        for index in range(start, end):
            if output[index] not in {"\n", "\r"}:
                output[index] = " "
    return "".join(output)


# --------------------------------------------------------
# Strip inert Python strings and comments so scanner fixtures cannot be mistaken for executable API.
def strip_python_literals_and_comments(text: str) -> str:
    """Mask Python STRING and COMMENT tokens while retaining exact source coordinates."""

    line_offsets = [0]
    line_offsets.extend(match.end() for match in re.finditer(r"\n", text))
    spans: list[tuple[int, int]] = []
    for token_info in tokenize.generate_tokens(io.StringIO(text).readline):
        if token_info.type not in {tokenize.STRING, tokenize.COMMENT}:
            continue
        start_line, start_column = token_info.start
        end_line, end_column = token_info.end
        spans.append(
            (
                line_offsets[start_line - 1] + start_column,
                line_offsets[end_line - 1] + end_column,
            )
        )
    return mask_spans(text, spans)


# --------------------------------------------------------
# Mask exact one-member compile-time probes only in unit tests that prove capabilities stay absent.
def strip_negative_capability_probes(text: str, relative: Path) -> str:
    """Exclude inert unit-test requires probes without hiding executable forbidden identifiers."""

    if relative.parts[:2] != ("tests", "unit"):
        return text
    spans: list[tuple[int, int]] = []
    for match in NEGATIVE_CAPABILITY_PROBE.finditer(text):
        concept = re.escape(match.group("concept"))
        negative_assertion = re.compile(rf"\bstatic_assert\s*\(\s*!\s*{concept}\s*<")
        if negative_assertion.search(text):
            spans.append(match.span("identifier"))
    return mask_spans(text, spans)


# --------------------------------------------------------
# Mask narrowly approved safe meanings while leaving every other matching identifier scan-visible.
def strip_approved_non_capability_identifiers(text: str, relative: Path) -> str:
    """Exclude exact disabled-credential identifiers from live findings."""

    patterns = APPROVED_NON_CAPABILITY_IDENTIFIERS.get(relative, ())
    spans = (match.span("identifier") for pattern in patterns for match in pattern.finditer(text))
    return mask_spans(text, spans)


# --------------------------------------------------------
# Convert an absolute character offset into stable one-based line and column coordinates.
def source_position(text: str, offset: int) -> tuple[int, int]:
    """Return the one-based line and column for an offset into source text."""

    line = text.count("\n", 0, offset) + 1
    line_start = text.rfind("\n", 0, offset)
    return line, offset - line_start


# --------------------------------------------------------
# Report whether one selected path can execute in the M3 submit or M4 private owner stack.
def is_direct_path(relative: Path) -> bool:
    """Classify exact owner-local production files independently from fixtures and drivers."""

    path = relative.as_posix()
    return path in M3_DIRECT_PATH_FILE_PATTERNS or path in M4_OWNER_PATH_FILE_PATTERNS


# --------------------------------------------------------
# Report whether a path can add compile, link, dependency, preset, or CI capability to M3 or M4.
def is_build_path(relative: Path) -> bool:
    """Classify every explicit or recursively discovered repository build-control file."""

    return (
        relative in BUILD_FILES
        or relative.name == "CMakeLists.txt"
        or relative.suffix.lower() == ".cmake"
    )


# --------------------------------------------------------
# Report whether a build path uses CMake command syntax rather than JSON or workflow YAML.
def is_cmake_path(relative: Path) -> bool:
    """Classify root/subdirectory lists and recursively discovered CMake modules."""

    return relative.name == "CMakeLists.txt" or relative.suffix.lower() == ".cmake"


# --------------------------------------------------------
# Require every explicitly assigned M3/M4/build artifact and discovery root before defaults.
def validate_required_manifest(repository: Path) -> None:
    """Fail closed when an assigned file or directory silently disappears from default discovery."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Require the explicit general, direct, and build file union without silently dropping entries.
    required_files = {
        Path(relative)
        for relative in (
            *M3_GENERAL_FILE_PATTERNS,
            *M3_DIRECT_PATH_FILE_PATTERNS,
            *M4_GENERAL_FILE_PATTERNS,
            *M4_OWNER_PATH_FILE_PATTERNS,
        )
    }
    required_files.update(BUILD_FILES)
    for relative in sorted(required_files, key=Path.as_posix):
        path = repository / relative
        if not path.is_file():
            raise FileNotFoundError(f"required scanner manifest file is missing: {relative}")

    # ++++++++++++++++++++++++++++++++++++++++
    # Require recursive roots separately so an accidentally removed subtree cannot appear empty.
    required_directories = {*M3_GENERAL_DIRECTORY_PREFIXES, *BUILD_DIRECTORY_PREFIXES}
    for relative in sorted(required_directories, key=Path.as_posix):
        path = repository / relative
        if not path.is_dir():
            raise FileNotFoundError(f"required scanner manifest directory is missing: {relative}")

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Discover every assigned M3/M4 source, test, fixture, tool, benchmark, and build artifact.
def discover_default_paths(repository: Path) -> list[Path]:
    """Return the validated explicit/directory union in deterministic repository-relative order."""

    # ++++++++++++++++++++++++++++++++++++++++
    # Reject incomplete authority before collecting any best-effort directory contents.
    validate_required_manifest(repository)

    # ++++++++++++++++++++++++++++++++++++++++
    # Union recursive M3/CMake files with every explicit M3/M4 and build assignment.
    paths: set[Path] = set()
    for prefix in M3_GENERAL_DIRECTORY_PREFIXES:
        directory = repository / prefix
        paths.update(path for path in directory.rglob("*") if path.is_file())
    for prefix in BUILD_DIRECTORY_PREFIXES:
        directory = repository / prefix
        paths.update(path for path in directory.rglob("*.cmake") if path.is_file())
    for relative in (
        *M3_GENERAL_FILE_PATTERNS,
        *M3_DIRECT_PATH_FILE_PATTERNS,
        *M4_GENERAL_FILE_PATTERNS,
        *M4_OWNER_PATH_FILE_PATTERNS,
        *BUILD_FILES,
    ):
        paths.add(repository / relative)
    return sorted(paths, key=lambda path: path.relative_to(repository).as_posix())

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Report a match once with repository-relative source coordinates and the assigned capability rule.
def violation(
    repository: Path, path: Path, text: str, match: re.Match[str], rule: str
) -> Violation:
    """Build one canonical finding from a regex match."""

    line, column = source_position(text, match.start())
    return Violation(path.relative_to(repository).as_posix(), line, column, rule, match.group(0))


# --------------------------------------------------------
# Report a caller-selected source span when command parsing identifies a forbidden build token.
def violation_at(
    repository: Path, path: Path, text: str, offset: int, rule: str, token: str
) -> Violation:
    """Build one canonical finding from an offset rather than a regular-expression match."""

    line, column = source_position(text, offset)
    return Violation(path.relative_to(repository).as_posix(), line, column, rule, token)


# --------------------------------------------------------
# Require the full immutable URL/hash/option grammar for both approved FetchContent dependencies.
def scan_pinned_fetchcontent_declarations(
    repository: Path, path: Path, text: str
) -> list[Violation]:
    """Reject every approved-name declaration that differs from its exact pinned token sequence."""

    findings: list[Violation] = []
    for match in CMAKE_FETCHCONTENT_DECLARE.finditer(text):
        arguments = list(CMAKE_ARGUMENT.finditer(match.group("body")))
        if not arguments:
            findings.append(
                violation_at(
                    repository,
                    path,
                    text,
                    match.start(),
                    "unapproved CMake dependency",
                    match.group(0),
                )
            )
            continue

        tokens = tuple(argument.group(0).strip('"') for argument in arguments)
        expected = PINNED_FETCHCONTENT_DECLARATIONS.get(tokens[0])
        if expected is not None and tokens != expected:
            findings.append(
                violation_at(
                    repository,
                    path,
                    text,
                    match.start("body") + arguments[0].start(),
                    "unapproved CMake dependency",
                    tokens[0],
                )
            )
    return findings


# --------------------------------------------------------
# Enforce exact CMake acquisition, module, subdirectory, and target-link dependency allow-sets.
def scan_cmake_dependencies(repository: Path, path: Path, text: str) -> list[Violation]:
    """Return deny-by-default CMake command and link-edge findings."""

    findings: list[Violation] = []

    # ++++++++++++++++++++++++++++++++++++++++
    # Bind approved FetchContent names to their exact immutable archive and checksum declarations.
    findings.extend(scan_pinned_fetchcontent_declarations(repository, path, text))

    # ++++++++++++++++++++++++++++++++++++++++
    # Permit only the assigned package, module, and owned-subdirectory command inputs.
    for match in CMAKE_NAMED_INPUT_COMMAND.finditer(text):
        command = match.group("command").lower()
        input_name = match.group("input").strip('"')
        if input_name not in ALLOWED_CMAKE_COMMAND_INPUTS[command]:
            findings.append(
                violation_at(
                    repository,
                    path,
                    text,
                    match.start("input"),
                    "unapproved CMake dependency",
                    input_name,
                )
            )

    # ++++++++++++++++++++++++++++++++++++++++
    # Reject acquisition commands for which M3/M4 has no accepted input vocabulary at all.
    for match in CMAKE_UNAPPROVED_ACQUISITION_COMMAND.finditer(text):
        findings.append(
            violation_at(
                repository,
                path,
                text,
                match.start(),
                "unapproved CMake dependency",
                match.group(0),
            )
        )

    # ++++++++++++++++++++++++++++++++++++++++
    # Reject global/property/directory link surfaces for which M3/M4 has no accepted use.
    for match in CMAKE_UNAPPROVED_LINK_SURFACE.finditer(text):
        findings.append(
            violation_at(
                repository,
                path,
                text,
                match.start(),
                "unapproved CMake link",
                match.group(0),
            )
        )

    # ++++++++++++++++++++++++++++++++++++++++
    # Permit only local targets and the pinned test/benchmark/thread link dependency vocabulary.
    for match in CMAKE_LINK_COMMAND.finditer(text):
        arguments = list(CMAKE_ARGUMENT.finditer(match.group("body")))
        for argument in arguments[1:]:
            link_input = argument.group(0).strip('"')
            if link_input.upper() in {"INTERFACE", "PRIVATE", "PUBLIC"}:
                continue
            if link_input in ALLOWED_CMAKE_LINK_INPUTS:
                continue
            findings.append(
                violation_at(
                    repository,
                    path,
                    text,
                    match.start("body") + argument.start(),
                    "unapproved CMake link",
                    link_input,
                )
            )

    # ++++++++++++++++++++++++++++++++++++++++
    # Permit only the sanitizer runtime option assembled by the accepted project-options module.
    for match in CMAKE_LINK_OPTIONS_COMMAND.finditer(text):
        arguments = list(CMAKE_ARGUMENT.finditer(match.group("body")))
        for argument in arguments[1:]:
            link_option = argument.group(0).strip('"')
            if link_option.upper() in {"BEFORE", "INTERFACE", "PRIVATE", "PUBLIC"}:
                continue
            if link_option in ALLOWED_CMAKE_LINK_OPTIONS:
                continue
            findings.append(
                violation_at(
                    repository,
                    path,
                    text,
                    match.start("body") + argument.start(),
                    "unapproved CMake link",
                    link_option,
                )
            )
    return findings

    # ++++++++++++++++++++++++++++++++++++++++


# --------------------------------------------------------
# Inspect one source/build path under general rules and add direct-path rules only when classified.
def scan_file(repository: Path, path: Path) -> list[Violation]:
    """Return general live findings plus production-only blocking or owner-hop findings."""

    raw_text = path.read_text(encoding="utf-8")
    relative = path.relative_to(repository)
    is_build_file = is_build_path(relative)
    is_python = path.suffix.lower() == ".py"

    # Python strings are inert data for API/include checks. C++ and build strings remain visible
    # because they can spell includes, linked surfaces, or configuration keys.
    if is_python:
        text = strip_python_literals_and_comments(raw_text)
        include_text = text
    elif is_build_file:
        text = strip_hash_comments(raw_text)
        include_text = text
    else:
        include_text = strip_cpp_comments(raw_text)
        text = strip_cpp_string_literals(include_text)
        text = strip_negative_capability_probes(text, relative)
    text = strip_approved_non_capability_identifiers(text, relative)
    findings: list[Violation] = []

    for match in GENERAL_FORBIDDEN_INCLUDE.finditer(include_text):
        findings.append(violation(repository, path, include_text, match, "forbidden include"))
    for rule, pattern in GENERAL_FORBIDDEN_IDENTIFIERS:
        for match in pattern.finditer(text):
            findings.append(violation(repository, path, text, match, rule))
    if is_python:
        for rule, pattern in PYTHON_FORBIDDEN_IDENTIFIERS:
            for match in pattern.finditer(text):
                findings.append(violation(repository, path, text, match, rule))
    if is_build_file:
        for rule, pattern in BUILD_FORBIDDEN_DEPENDENCIES:
            for match in pattern.finditer(text):
                findings.append(violation(repository, path, text, match, rule))
    if is_cmake_path(relative):
        findings.extend(scan_cmake_dependencies(repository, path, text))

    if is_direct_path(relative):
        for match in DIRECT_PATH_FORBIDDEN_INCLUDE.finditer(include_text):
            findings.append(violation(repository, path, include_text, match, "forbidden include"))
        for rule, pattern in DIRECT_PATH_FORBIDDEN_IDENTIFIERS:
            for match in pattern.finditer(text):
                findings.append(violation(repository, path, text, match, rule))

    # Real Python URL literals remain evidence except in the two scanner-owned files whose strings
    # are deliberately malicious fixtures. Other languages reuse the API-scanning representation.
    if is_python and relative not in EMBEDDED_SCANNER_FIXTURE_PATHS:
        url_text = strip_hash_comments(raw_text)
    elif not is_python and not is_build_file:
        url_text = include_text
    else:
        url_text = text
    for match in re.finditer(r"https?://[^\s\"')]+", url_text):
        if is_build_file and match.group(0) in ALLOWED_BUILD_URLS:
            continue
        findings.append(violation(repository, path, url_text, match, "live URL"))

    return findings


# --------------------------------------------------------
# Validate explicit or discovered files and return findings in deterministic path/location order.
def scan_paths(repository: Path, paths: Iterable[Path] | None = None) -> list[Violation]:
    """Scan M3/M4 defaults or caller-selected files beneath one repository root."""

    repository = repository.resolve()
    selected = discover_default_paths(repository) if paths is None else list(paths)
    resolved: list[Path] = []
    for path in selected:
        candidate = path if path.is_absolute() else repository / path
        candidate = candidate.resolve()
        try:
            candidate.relative_to(repository)
        except ValueError as error:
            raise ValueError(f"scan path escapes repository: {candidate}") from error
        if not candidate.is_file():
            raise FileNotFoundError(f"scan path is not a file: {candidate}")
        resolved.append(candidate)

    findings = [finding for path in resolved for finding in scan_file(repository, path)]
    return sorted(findings)


# --------------------------------------------------------
# Parse the repository root and optional explicit file list used by CI and focused self-tests.
def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    """Parse deterministic scanner inputs without accepting implicit external paths."""

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (default: parent of tools)",
    )
    parser.add_argument(
        "paths",
        nargs="*",
        type=Path,
        help="optional repository-relative files; omit to scan the fixed M3/M4-owned set",
    )
    return parser.parse_args(arguments)


# --------------------------------------------------------
# Print stable machine-readable findings and fail exactly when a forbidden capability is present.
def main(arguments: Sequence[str] | None = None) -> int:
    """Run the scanner and return a conventional process status."""

    options = parse_arguments(arguments)
    selected = options.paths if options.paths else None
    try:
        findings = scan_paths(options.root, selected)
    except (FileNotFoundError, UnicodeDecodeError, ValueError, tokenize.TokenError) as error:
        print(f"forbidden-capability scanner error: {error}", file=sys.stderr)
        return 2

    for finding in findings:
        print(
            f"{finding.path}:{finding.line}:{finding.column}: {finding.rule}: {finding.token}",
            file=sys.stderr,
        )
    return 1 if findings else 0


# --------------------------------------------------------


if __name__ == "__main__":
    raise SystemExit(main())
