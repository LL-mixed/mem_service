from __future__ import annotations

import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT
SERVICE_DIR = ROOT / "components" / "mem_service"
CLI_SOURCE = ROOT / "apps" / "mem_service" / "mem_service.c"

WIRE_MAGIC = 0x4D535643
WIRE_VERSION = 1
WIRE_HEADER_LEN = 48
WIRE_MAX_PAYLOAD_LEN = 4096

OP_PUT_OBJECT = 16
OP_ALLOCATE_OBJECT = 0x70
OP_ACQUIRE_OBJECT = 0x71
OP_RELEASE_OBJECT = 0x72
OP_RETIRE_OBJECT = 0x73
OP_INSPECT_ALLOCATION = 0x74
OP_ALLOCATION_STATS = 0x75
OP_UNKNOWN = 0xFF

STATUS_OK = 0
STATUS_NOT_FOUND = 1
STATUS_CHECKSUM_MISMATCH = 3
STATUS_VERSION_CONFLICT = 4
STATUS_INVALID_SESSION = 6
STATUS_UNSUPPORTED = 9
STATUS_INTERNAL = 10

ALLOCATION_STATS_FIELDS = (
    "backing_registered",
    "live_objects",
    "backing_allocated_bytes",
    "address_reserved_bytes",
    "export_mappings",
    "import_mappings",
    "live_refs",
    "in_flight",
    "quarantined_objects",
    "quarantined_bytes",
    "allocate_ok_count",
    "acquire_ok_count",
    "release_ok_count",
    "retire_ok_count",
    "allocate_rejected_count",
    "acquire_rejected_count",
    "release_rejected_count",
    "retire_rejected_count",
    "quarantine_events",
)

IO_TIMEOUT_MS = 500


def _tmp_parent() -> Path:
    private_tmp = Path("/private/tmp")
    if private_tmp.exists():
        return private_tmp
    return Path(tempfile.gettempdir())


def _fnv1a32(data: bytes) -> int:
    if not data:
        return 0
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def _wire_header(request_id: int, operation: int, payload: bytes,
                 checksum: int | None = None) -> bytes:
    return struct.pack(
        "<IHHQIIIIIIQ",
        WIRE_MAGIC,
        WIRE_VERSION,
        WIRE_HEADER_LEN,
        request_id,
        operation,
        0,
        len(payload),
        _fnv1a32(payload) if checksum is None else checksum,
        0,
        0,
        0,
    )


def _wire_exchange_unix(
    socket_path: Path,
    operation: int,
    payload: bytes = b"",
    request_id: int = 1,
    timeout: float = 5.0,
    checksum: int | None = None,
) -> tuple[int, bytes, int]:
    """One RPC over one Unix connection; returns (status, payload, request_id)."""
    request = _wire_header(request_id, operation, payload, checksum) + payload
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        sock.connect(str(socket_path))
        sock.sendall(request)
        header = b""
        while len(header) < WIRE_HEADER_LEN:
            chunk = sock.recv(WIRE_HEADER_LEN - len(header))
            if not chunk:
                raise ConnectionError("daemon closed connection before response header")
            header += chunk
        (magic, version, header_len, response_id, response_op, _flags,
         payload_len, payload_checksum, status, _error_code, _server_time) = struct.unpack(
            "<IHHQIIIIIIQ", header
        )
        assert magic == WIRE_MAGIC and version == WIRE_VERSION
        assert header_len == WIRE_HEADER_LEN
        assert response_op == operation
        assert payload_len <= WIRE_MAX_PAYLOAD_LEN
        body = b""
        while len(body) < payload_len:
            chunk = sock.recv(payload_len - len(body))
            if not chunk:
                raise ConnectionError("daemon closed connection before response payload")
            body += chunk
        assert _fnv1a32(body) == payload_checksum
        return status, body, response_id


def _parse_kv(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class MemServiceAllocationOpsTests(unittest.TestCase):
    """M1.1 wire-level behavior for the managed allocation control set.

    The stock daemon registers no managed backing, so allocate is
    fail-closed (backing_unavailable) while schema validation, network
    allowlisting, daemon idempotency and the stats shape are exercised
    end to end between two real processes. State-machine semantics with
    a stub backing (idempotent replay, key conflict, stale generation,
    quarantine) are covered by the C `allocation-fixtures` self check;
    stubs stay out of these wire tests by design.
    """

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="msvc_alloc_", dir=str(_tmp_parent())))
        self.binary = self.root / "linqu_mem_service"
        self.socket = self.root / "alloc.sock"
        self._compile_host_binary()

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def _compile_host_binary(self):
        cmd = [
            "cc",
            "-O2",
            "-Wall",
            "-Wextra",
            f"-I{ROOT}",
            f"-I{ROOT / 'libs' / 'obmm_queue'}",
            str(CLI_SOURCE),
            str(SERVICE_DIR / "mem_service_daemon.c"),
            str(SERVICE_DIR / "mem_service_replay_history.c"),
            str(SERVICE_DIR / "mem_service_client.c"),
            str(SERVICE_DIR / "mem_service_wire_client.c"),
            str(SERVICE_DIR / "mem_service_metadata.c"),
            str(SERVICE_DIR / "mem_service_provider.c"),
            str(SERVICE_DIR / "mem_service_keys.c"),
            str(SERVICE_DIR / "mem_service_object_refs.c"),
            str(SERVICE_DIR / "mem_service_ub_ssd_gsva_backend.c"),
            str(SERVICE_DIR / "mem_service_ub_ssd_gsva_io.c"),
            str(SERVICE_DIR / "mem_service_records.c"),
            str(SERVICE_DIR / "mem_service_allocation.c"),
            str(SERVICE_DIR / "mem_service_provider_directory.c"),
            "-lm",
            "-o",
            str(self.binary),
        ]
        subprocess.run(cmd, cwd=REPO_ROOT, check=True, capture_output=True, text=True)

    def _run_client(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.binary), *args],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def _start_unix_daemon(self) -> subprocess.Popen:
        proc = subprocess.Popen(
            [
                str(self.binary),
                "serve",
                "--listen",
                f"unix:{self.socket}",
                "--store",
                str(self.root / "alloc.store"),
            ],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate(timeout=1)
                if "Operation not permitted" in stderr:
                    raise unittest.SkipTest("sandbox forbids socket bind in subprocess")
                self.fail(f"unix daemon exited rc={proc.returncode} {stderr}{stdout}")
            health = self._run_client("health", "--connect", f"unix:{self.socket}")
            if health.returncode == 0 and "status=ok" in health.stdout:
                return proc
            time.sleep(0.05)
        self._stop_server(proc)
        self.fail("mem_service unix daemon did not become ready")

    def _stop_server(self, proc: subprocess.Popen) -> tuple[str, str]:
        proc.terminate()
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
        return stdout, stderr

    def _allocation_stats(self) -> dict[str, str]:
        result = self._run_client(
            "allocation-stats", "--connect", f"unix:{self.socket}"
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("status=ok", result.stdout)
        return _parse_kv(result.stdout)

    # Stats shape: every managed field is exported and starts at zero on a
    # fresh daemon with no backing registered.
    def test_allocation_stats_shape_over_unix(self):
        proc = self._start_unix_daemon()
        try:
            stats = self._allocation_stats()
            for field in ALLOCATION_STATS_FIELDS:
                self.assertIn(field, stats, f"missing stats field {field}")
                self.assertEqual(
                    stats[field],
                    "0",
                    f"stats field {field} should start at 0, got {stats[field]}",
                )
        finally:
            self._stop_server(proc)

    # Zero backing: allocate is fail-closed with a deterministic reason and
    # only the rejected counter moves.
    def test_allocate_fail_closed_without_backing_over_unix(self):
        proc = self._start_unix_daemon()
        try:
            result = self._run_client(
                "allocate-object",
                "--key",
                "obj-a",
                "--idempotency-key",
                "alloc-1",
                "--session-id",
                "session-a",
                "--size-bytes",
                "4096",
                "--capabilities",
                "1",
                "--connect",
                f"unix:{self.socket}",
            )
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("status=internal", result.stdout)
            self.assertIn("reason=backing_unavailable", result.stdout)

            stats = self._allocation_stats()
            self.assertEqual(stats["allocate_ok_count"], "0")
            self.assertEqual(stats["allocate_rejected_count"], "1")
            self.assertEqual(stats["live_objects"], "0")
            self.assertEqual(stats["backing_registered"], "0")
        finally:
            self._stop_server(proc)

    # Holder/lifecycle operations on an unknown key are deterministic
    # not_found rejections tracked by their rejected counters.
    def test_holder_ops_and_inspect_unknown_key_over_unix(self):
        proc = self._start_unix_daemon()
        try:
            for command, extra in (
                ("acquire-object", ("--session-id", "session-a")),
                ("release-object", ("--session-id", "session-a")),
                ("retire-object", ()),
            ):
                result = self._run_client(
                    command,
                    "--key",
                    "ghost",
                    "--idempotency-key",
                    f"{command}-1",
                    *extra,
                    "--connect",
                    f"unix:{self.socket}",
                )
                self.assertEqual(result.returncode, 1, command + result.stderr)
                self.assertIn("status=not_found", result.stdout, command)
                self.assertIn("reason=not_found", result.stdout, command)

            result = self._run_client(
                "inspect-allocation",
                "--key",
                "ghost",
                "--connect",
                f"unix:{self.socket}",
            )
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("status=not_found", result.stdout)
            self.assertIn("reason=not_found", result.stdout)

            stats = self._allocation_stats()
            self.assertEqual(stats["acquire_rejected_count"], "1")
            self.assertEqual(stats["release_rejected_count"], "1")
            self.assertEqual(stats["retire_rejected_count"], "1")
            self.assertEqual(stats["acquire_ok_count"], "0")
            self.assertEqual(stats["release_ok_count"], "0")
            self.assertEqual(stats["retire_ok_count"], "0")
        finally:
            self._stop_server(proc)

    # Idempotent transaction over the wire: a retry with the same
    # operation/idempotency identity replays the recorded outcome exactly
    # once (the handler is not re-executed); the same identity carrying a
    # different request is a version conflict, never a silent second change.
    def test_allocate_idempotent_replay_and_conflict_over_unix(self):
        proc = self._start_unix_daemon()
        try:
            payload = (
                b"key=obj-a\n"
                b"idempotency_key=alloc-1\n"
                b"session_id=session-a\n"
                b"size_bytes=4096\n"
                b"capabilities=1\n"
            )
            status_first, body_first, response_id_first = _wire_exchange_unix(
                self.socket, OP_ALLOCATE_OBJECT, payload, request_id=11
            )
            self.assertEqual(status_first, STATUS_INTERNAL)
            self.assertEqual(response_id_first, 11)
            self.assertIn(b"reason=backing_unavailable", body_first)

            status_replay, body_replay, response_id_replay = _wire_exchange_unix(
                self.socket, OP_ALLOCATE_OBJECT, payload, request_id=12
            )
            self.assertEqual(status_replay, STATUS_INTERNAL)
            self.assertEqual(response_id_replay, 12)
            self.assertEqual(body_replay, body_first)

            stats = self._allocation_stats()
            self.assertEqual(
                stats["allocate_rejected_count"],
                "1",
                "idempotent replay must not re-execute the mutation",
            )
            self.assertEqual(stats["allocate_ok_count"], "0")
            self.assertEqual(stats["live_objects"], "0")

            conflict_payload = payload.replace(b"size_bytes=4096", b"size_bytes=8192")
            status_conflict, body_conflict, _ = _wire_exchange_unix(
                self.socket, OP_ALLOCATE_OBJECT, conflict_payload, request_id=13
            )
            self.assertEqual(status_conflict, STATUS_VERSION_CONFLICT)
            self.assertIn(b"status=version_conflict", body_conflict)

            stats = self._allocation_stats()
            self.assertEqual(stats["allocate_rejected_count"], "1")
            self.assertEqual(stats["live_objects"], "0")
        finally:
            self._stop_server(proc)

    # Corrupted or semantically invalid allocate requests are rejected
    # fail-closed with invalid_request and never create an object.
    def test_allocate_rejects_corrupted_payloads_over_unix(self):
        proc = self._start_unix_daemon()
        try:
            valid = {
                "key": "obj-a",
                "idempotency_key": "alloc-corrupt",
                "size_bytes": "4096",
                "capabilities": "1",
            }

            def payload(**overrides) -> bytes:
                fields = dict(valid)
                for name, value in overrides.items():
                    if value is None:
                        fields.pop(name, None)
                    else:
                        fields[name] = value
                return "".join(f"{k}={v}\n" for k, v in fields.items()).encode()

            cases = (
                ("missing key", payload(key=None)),
                ("missing idempotency_key", payload(idempotency_key=None)),
                ("missing size_bytes", payload(size_bytes=None)),
                ("missing capabilities", payload(capabilities=None)),
                ("zero size_bytes", payload(size_bytes="0")),
                ("zero capabilities", payload(capabilities="0")),
                ("capabilities outside valid mask", payload(capabilities="4")),
                ("non power-of-two alignment", payload(alignment_bytes="3")),
                ("non-numeric size_bytes", payload(size_bytes="abc")),
            )
            for index, (name, body) in enumerate(cases):
                with self.subTest(case=name):
                    # Each case uses a distinct idempotency identity so the
                    # daemon ledger never short-circuits schema validation.
                    body = body.replace(b"alloc-corrupt", f"alloc-corrupt-{index}".encode())
                    status, response, _ = _wire_exchange_unix(
                        self.socket,
                        OP_ALLOCATE_OBJECT,
                        body,
                        request_id=100 + index,
                    )
                    self.assertEqual(status, STATUS_INVALID_SESSION, name)
                    self.assertIn(b"reason=invalid_request", response, name)

            stats = self._allocation_stats()
            self.assertEqual(stats["allocate_ok_count"], "0")
            self.assertEqual(stats["live_objects"], "0")
            # Schema-level rejections (missing required fields, non-numeric
            # u64) fail before the managed table runs; only the four
            # semantically invalid but schema-valid requests reach the
            # managed table and move its rejected counter.
            self.assertEqual(stats["allocate_rejected_count"], "4")
        finally:
            self._stop_server(proc)

    # Wire hygiene on the local endpoint: unknown operations get a
    # deterministic UNSUPPORTED, payload corruption is caught by the
    # checksum before any handler runs.
    def test_unknown_op_and_bad_checksum_over_unix(self):
        proc = self._start_unix_daemon()
        try:
            status, _, _ = _wire_exchange_unix(
                self.socket, OP_UNKNOWN, b"", request_id=7
            )
            self.assertEqual(status, STATUS_UNSUPPORTED)

            payload = b"key=obj-a\nidempotency_key=alloc-cs\nsize_bytes=4096\ncapabilities=1\n"
            status, body, _ = _wire_exchange_unix(
                self.socket,
                OP_ALLOCATE_OBJECT,
                payload,
                request_id=8,
                checksum=_fnv1a32(payload) ^ 0xFF,
            )
            self.assertEqual(status, STATUS_CHECKSUM_MISMATCH)
            self.assertIn(b"checksum_mismatch", body)

            stats = self._allocation_stats()
            self.assertEqual(stats["allocate_rejected_count"], "0")
            self.assertEqual(stats["live_objects"], "0")
        finally:
            self._stop_server(proc)

    # Trusted-guest-network endpoint: the managed allocation control set is
    # in the network allowlist and reaches the (fail-closed) managed table,
    # while legacy local file/admin operations return UNSUPPORTED.
    def test_network_endpoint_managed_ops_allowed_legacy_blocked(self):
        port_file = self.root / "port"
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.bind(("127.0.0.1", 0))
        port = int(sock.getsockname()[1])
        sock.close()
        port_file.write_text(str(port))

        config = self.root / "network.conf"
        config.write_text(
            f"listen=tcp:127.0.0.1:{port}\n"
            "auth_mode=trusted-guest-network\n"
            "node_id=node-a\n"
            "network_peer=node-a@127.0.0.1\n"
            f"network_io_timeout_ms={IO_TIMEOUT_MS}\n"
        )
        proc = subprocess.Popen(
            [str(self.binary), "serve", "--config", str(config)],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        deadline = time.time() + 5.0
        while time.time() < deadline:
            if proc.poll() is not None:
                stdout, stderr = proc.communicate(timeout=1)
                if "Operation not permitted" in stderr:
                    raise unittest.SkipTest("sandbox forbids socket bind in subprocess")
                self.fail(f"tcp daemon exited rc={proc.returncode} {stderr}{stdout}")
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            self._stop_server(proc)
            self.fail("mem_service tcp daemon did not start listening")

        try:
            # One RPC per connection (the daemon closes after each response).
            def one_rpc(operation: int, payload: bytes, request_id: int):
                with socket.create_connection(("127.0.0.1", port), timeout=5.0) as rpc:
                    rpc.settimeout(5.0)
                    request = _wire_header(request_id, operation, payload) + payload
                    rpc.sendall(request)
                    header = b""
                    while len(header) < WIRE_HEADER_LEN:
                        chunk = rpc.recv(WIRE_HEADER_LEN - len(header))
                        if not chunk:
                            raise ConnectionError("daemon closed before response header")
                        header += chunk
                    (magic, version, header_len, response_id, response_op, _f,
                     payload_len, payload_checksum, status, _e, _t) = struct.unpack(
                        "<IHHQIIIIIIQ", header
                    )
                    assert magic == WIRE_MAGIC and version == WIRE_VERSION
                    assert response_op == operation and response_id == request_id
                    body = b""
                    while len(body) < payload_len:
                        chunk = rpc.recv(payload_len - len(body))
                        if not chunk:
                            raise ConnectionError("daemon closed before response payload")
                        body += chunk
                    assert _fnv1a32(body) == payload_checksum
                    return status, body

            # Legacy local file operation is not in the network allowlist.
            put_payload = b"key=legacy\nidempotency_key=p1\nbacking_len=4\ndata=aBcD\n"
            status, body = one_rpc(OP_PUT_OBJECT, put_payload, request_id=21)
            self.assertEqual(status, STATUS_UNSUPPORTED)
            self.assertIn(b"reason=network_operation_not_allowed", body)

            # Managed allocate passes the allowlist and reaches the managed
            # table, which stays fail-closed with zero backing registered.
            alloc_payload = (
                b"key=obj-net\n"
                b"idempotency_key=alloc-net-1\n"
                b"size_bytes=4096\n"
                b"capabilities=1\n"
            )
            status, body = one_rpc(OP_ALLOCATE_OBJECT, alloc_payload, request_id=22)
            self.assertEqual(status, STATUS_INTERNAL)
            self.assertIn(b"reason=backing_unavailable", body)

            # Unknown keys are deterministic not_found on the network too.
            status, body = one_rpc(
                OP_INSPECT_ALLOCATION, b"key=obj-net\n", request_id=23
            )
            self.assertEqual(status, STATUS_NOT_FOUND)
            self.assertIn(b"reason=not_found", body)

            # Stats query is allowed and keeps its full shape over TCP.
            result = self._run_client(
                "allocation-stats", "--connect", f"tcp:127.0.0.1:{port}"
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            stats = _parse_kv(result.stdout)
            for field in ALLOCATION_STATS_FIELDS:
                self.assertIn(field, stats, f"missing stats field {field}")
            self.assertEqual(stats["backing_registered"], "0")
            self.assertEqual(stats["allocate_rejected_count"], "1")
            self.assertEqual(stats["live_objects"], "0")
        finally:
            self._stop_server(proc)


if __name__ == "__main__":
    unittest.main()
