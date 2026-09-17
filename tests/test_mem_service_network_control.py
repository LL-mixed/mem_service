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
DAEMON_SOURCE = SERVICE_DIR / "mem_service_daemon.c"
CLI_SOURCE = ROOT / "apps" / "mem_service" / "mem_service.c"
CONFIG_SCHEMA = ROOT / "apps" / "mem_service" / "configs" / "mem_service.conf.schema"
EXAMPLE_CONFIG = ROOT / "apps" / "mem_service" / "configs" / "mem_service.example.conf"

WIRE_MAGIC = 0x4D535643
WIRE_VERSION = 1
WIRE_HEADER_LEN = 48
WIRE_MAX_PAYLOAD_LEN = 4096

OP_HEALTH = 1
OP_READY = 2
OP_STATUS = 3
OP_PUT_OBJECT = 16

STATUS_OK = 0
STATUS_CHECKSUM_MISMATCH = 3
STATUS_INVALID_SESSION = 6
STATUS_CAPACITY_EXCEEDED = 8
STATUS_UNSUPPORTED = 9

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


def _wire_header(request_id: int, operation: int, payload: bytes) -> bytes:
    return struct.pack(
        "<IHHQIIIIIIQ",
        WIRE_MAGIC,
        WIRE_VERSION,
        WIRE_HEADER_LEN,
        request_id,
        operation,
        0,
        len(payload),
        _fnv1a32(payload),
        0,
        0,
        0,
    )


def _wire_exchange(
    port: int,
    operation: int,
    payload: bytes = b"",
    request_id: int = 1,
    timeout: float = 5.0,
    chunk_bytes: int | None = None,
) -> tuple[int, bytes, int]:
    """One RPC over one TCP connection; returns (status, payload, request_id)."""
    request = _wire_header(request_id, operation, payload) + payload
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        if chunk_bytes is None:
            sock.sendall(request)
        else:
            for offset in range(0, len(request), chunk_bytes):
                sock.sendall(request[offset : offset + chunk_bytes])
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


def _primary_ipv4() -> str | None:
    """Best-effort primary interface IPv4 without sending packets."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("192.0.2.1", 80))
        address = sock.getsockname()[0]
    except OSError:
        return None
    finally:
        sock.close()
    if address.startswith("127."):
        return None
    return address


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class MemServiceNetworkControlTests(unittest.TestCase):
    """T-control: wire-over-TCP control channel behavior (plan section 6.2)."""

    def test_tcp_listener_backlog_covers_multi_node_client_bursts(self):
        daemon = DAEMON_SOURCE.read_text()

        self.assertIn("#define MEM_SERVICE_TCP_LISTEN_BACKLOG 128", daemon)
        tcp_runtime = daemon.split(
            "int mem_service_run_daemon_with_runtime(", 1
        )[1]
        self.assertIn(
            "listen(server_fd, MEM_SERVICE_TCP_LISTEN_BACKLOG)", tcp_runtime
        )

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="msvc_netctl_", dir=str(_tmp_parent())))
        self.binary = self.root / "linqu_mem_service"
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

    def _free_tcp_port(self) -> int:
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            listener.bind(("127.0.0.1", 0))
            return int(listener.getsockname()[1])
        finally:
            listener.close()

    def _run_client(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [str(self.binary), *args],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def _write_config(self, name: str, text: str) -> Path:
        path = self.root / name
        path.write_text(text)
        return path

    def _network_config(
        self,
        listen: str,
        node_id: str | None = "node-a",
        peers: tuple[str, ...] = ("node-a@127.0.0.1",),
        auth_mode: str | None = "trusted-guest-network",
        extra: str = "",
    ) -> str:
        lines = [f"listen={listen}"]
        if auth_mode is not None:
            lines.append(f"auth_mode={auth_mode}")
        if node_id is not None:
            lines.append(f"node_id={node_id}")
        for peer in peers:
            lines.append(f"network_peer={peer}")
        lines.append(f"network_io_timeout_ms={IO_TIMEOUT_MS}")
        if extra:
            lines.append(extra)
        return "\n".join(lines) + "\n"

    def _start_server(self, config_path: Path, probe_host: str = "127.0.0.1",
                      probe_port: int | None = None) -> subprocess.Popen:
        proc = subprocess.Popen(
            [str(self.binary), "serve", "--config", str(config_path)],
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
                self.fail(
                    f"mem_service daemon exited rc={proc.returncode}\n"
                    f"stdout={stdout}\nstderr={stderr}"
                )
            if probe_port is not None:
                try:
                    with socket.create_connection((probe_host, probe_port), timeout=0.2):
                        return proc
                except OSError:
                    pass
            time.sleep(0.05)
        self._stop_server(proc)
        self.fail("mem_service daemon did not start listening")

    def _expect_serve_rejected(self, config_text: str, reason: str):
        config = self._write_config(f"reject_{reason}.conf", config_text)
        result = self._run_client("serve", "--config", str(config))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertNotIn("status=ready", result.stdout)

    def _stop_server(self, proc: subprocess.Popen) -> tuple[str, str]:
        proc.terminate()
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
        return stdout, stderr

    # T-control row 1: unix compatibility and TCP two-process round trip.
    def test_unix_compat_and_tcp_two_process_round_trip(self):
        port = self._free_tcp_port()
        config = self._write_config(
            "round_trip.conf",
            self._network_config(f"tcp:127.0.0.1:{port}"),
        )
        proc = self._start_server(config, probe_port=port)
        try:
            for command in ("health", "ready", "status"):
                result = self._run_client(command, "--connect", f"tcp:127.0.0.1:{port}")
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                self.assertIn("status=ok", result.stdout)
                # The response must be a wire text-kv payload, never a
                # metrics HTTP response leaking onto the control channel.
                self.assertNotIn("HTTP/1.1", result.stdout)

            status, body, response_id = _wire_exchange(
                port, OP_STATUS, request_id=0xDEADBEEF
            )
            self.assertEqual(status, STATUS_OK)
            self.assertEqual(response_id, 0xDEADBEEF)
            self.assertIn(b"record_count=0", body)
        finally:
            self._stop_server(proc)

        # Legacy unix endpoint keeps serving the full local operation set.
        socket_path = self.root / "compat.sock"
        unix_proc = subprocess.Popen(
            [
                str(self.binary),
                "serve",
                "--listen",
                f"unix:{socket_path}",
                "--store",
                str(self.root / "compat.store"),
            ],
            cwd=REPO_ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            deadline = time.time() + 5.0
            while time.time() < deadline:
                if unix_proc.poll() is not None:
                    stdout, stderr = unix_proc.communicate(timeout=1)
                    if "Operation not permitted" in stderr:
                        raise unittest.SkipTest("sandbox forbids socket bind")
                    self.fail(f"unix daemon exited rc={unix_proc.returncode} {stderr}")
                health = self._run_client("health", "--connect", f"unix:{socket_path}")
                if health.returncode == 0 and "status=ok" in health.stdout:
                    break
                time.sleep(0.05)
            else:
                self.fail("unix daemon did not become ready")
            listed = self._run_client("list-records", "--connect", f"unix:{socket_path}")
            self.assertEqual(listed.returncode, 0, listed.stderr + listed.stdout)
            self.assertIn("status=ok", listed.stdout)
        finally:
            self._stop_server(unix_proc)

    # T-control row 2: configuration and source rejection.
    def test_config_and_source_rejection(self):
        port = self._free_tcp_port()
        self._expect_serve_rejected(
            f"listen=tcp:127.0.0.1:{port}\n",
            "none_tcp",
        )
        self._expect_serve_rejected(
            f"listen=tcp:127.0.0.1:{port}\n"
            "auth_mode=none\n"
            "network_peer=node-a@127.0.0.1\n",
            "none_with_peer",
        )
        self._expect_serve_rejected(
            self._network_config(f"tcp:127.0.0.1:{port}", peers=()),
            "missing_allowlist",
        )
        self._expect_serve_rejected(
            self._network_config(f"tcp:127.0.0.1:{port}", node_id=None),
            "missing_node_id",
        )
        self._expect_serve_rejected(
            self._network_config(f"tcp:0.0.0.0:{port}"),
            "wildcard_bind",
        )
        self._expect_serve_rejected(
            self._network_config("tcp:255.255.255.255:9700"),
            "broadcast_bind",
        )
        self._expect_serve_rejected(
            self._network_config("tcp:224.0.0.1:9700"),
            "multicast_bind",
        )
        self._expect_serve_rejected(
            self._network_config("tcp:127.0.0.1:0"),
            "invalid_port",
        )
        self._expect_serve_rejected(
            self._network_config(
                f"tcp:127.0.0.1:{port}",
                peers=("node-a@127.0.0.1", "node-b@127.0.0.1"),
            ),
            "duplicate_address",
        )
        self._expect_serve_rejected(
            self._network_config(
                f"tcp:127.0.0.1:{port}",
                peers=("node-a@127.0.0.1", "node-a@127.0.0.2"),
            ),
            "duplicate_node_id",
        )
        self._expect_serve_rejected(
            self._network_config(f"tcp:127.0.0.1:{port}", peers=("node-a@10.0.0.0/8",)),
            "cidr_allowlist",
        )
        self._expect_serve_rejected(
            f"listen=rdma:127.0.0.1:{port}\n",
            "unknown_listen_scheme",
        )

        unknown_scheme = self._run_client("health", "--connect", f"rdma:127.0.0.1:{port}")
        self.assertEqual(unknown_scheme.returncode, 2)

        # Source address outside the allowlist is closed without service.
        denied_port = self._free_tcp_port()
        denied_config = self._write_config(
            "denied_source.conf",
            self._network_config(
                f"tcp:127.0.0.1:{denied_port}",
                peers=("node-b@192.0.2.1",),
            ),
        )
        proc = self._start_server(denied_config, probe_port=denied_port)
        try:
            with self.assertRaises((ConnectionError, socket.timeout)):
                _wire_exchange(denied_port, OP_HEALTH, timeout=3.0)
        finally:
            stdout, stderr = self._stop_server(proc)
        self.assertIn("peer_not_allowed", stderr)

        # A declared node identity that does not match the connection's
        # allowlisted identity is a spoofing attempt and is not executed.
        port = self._free_tcp_port()
        config = self._write_config(
            "spoof.conf",
            self._network_config(f"tcp:127.0.0.1:{port}"),
        )
        proc = self._start_server(config, probe_port=port)
        try:
            status, body, _ = _wire_exchange(port, OP_STATUS, b"node_id=node-b\n")
            self.assertEqual(status, STATUS_INVALID_SESSION)
            self.assertIn(b"node_id_mismatch", body)
            status, _, _ = _wire_exchange(port, OP_STATUS, b"node_id=node-a\n")
            self.assertEqual(status, STATUS_OK)
        finally:
            stdout, stderr = self._stop_server(proc)
        self.assertIn("node_id_mismatch", stderr)

    # T-control row 3: independent listener policy for control vs metrics.
    def test_independent_listener_policy(self):
        primary = _primary_ipv4()
        if primary is None:
            raise unittest.SkipTest("no non-loopback IPv4 interface available")
        port = self._free_tcp_port()
        config = self._write_config(
            "primary.conf",
            self._network_config(
                f"tcp:{primary}:{port}",
                peers=(f"node-a@{primary}",),
            ),
        )
        proc = self._start_server(config, probe_host=primary, probe_port=port)
        try:
            result = self._run_client("health", "--connect", f"tcp:{primary}:{port}")
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("status=ok", result.stdout)
            # The exact bind never collapses into a wildcard bind: the same
            # port stays unreachable on loopback, so no host port is exposed
            # beyond the configured interface.
            with self.assertRaises(OSError):
                socket.create_connection(("127.0.0.1", port), timeout=0.5)
        finally:
            self._stop_server(proc)

        # The metrics listener keeps its own loopback-only rule even when the
        # control channel serves a non-loopback address.
        self._expect_serve_rejected(
            self._network_config(
                f"tcp:{primary}:{self._free_tcp_port()}",
                peers=(f"node-a@{primary}",),
                extra=f"metrics_listen=tcp:{primary}:9901",
            ),
            "non_loopback_metrics",
        )

    # T-control row 4: framing and compatibility errors.
    def test_framing_and_compatibility_errors(self):
        port = self._free_tcp_port()
        config = self._write_config(
            "framing.conf",
            self._network_config(f"tcp:127.0.0.1:{port}"),
        )
        proc = self._start_server(config, probe_port=port)
        try:
            # Byte-by-byte fragmented header and body still complete.
            status, body, response_id = _wire_exchange(
                port, OP_STATUS, b"node_id=node-a\n", request_id=77, chunk_bytes=1
            )
            self.assertEqual(status, STATUS_OK)
            self.assertEqual(response_id, 77)

            # Oversized declared payload is bounded.
            oversize = struct.pack(
                "<IHHQIIIIIIQ",
                WIRE_MAGIC, WIRE_VERSION, WIRE_HEADER_LEN,
                1, OP_STATUS, 0, WIRE_MAX_PAYLOAD_LEN + 1, 0, 0, 0, 0,
            )
            with socket.create_connection(("127.0.0.1", port), timeout=3.0) as sock:
                sock.settimeout(3.0)
                sock.sendall(oversize)
                header = sock.recv(WIRE_HEADER_LEN)
                self.assertEqual(len(header), WIRE_HEADER_LEN)
                status = struct.unpack("<IHHQIIIIIIQ", header)[8]
                self.assertEqual(status, STATUS_CAPACITY_EXCEEDED)

            # Corrupted payload checksum is rejected deterministically.
            with socket.create_connection(("127.0.0.1", port), timeout=3.0) as sock:
                sock.settimeout(3.0)
                bad = struct.pack(
                    "<IHHQIIIIIIQ",
                    WIRE_MAGIC, WIRE_VERSION, WIRE_HEADER_LEN,
                    2, OP_STATUS, 0, 4, 0xFFFFFFFF, 0, 0, 0,
                )
                sock.sendall(bad + b"junk")
                header = sock.recv(WIRE_HEADER_LEN)
                self.assertEqual(len(header), WIRE_HEADER_LEN)
                status = struct.unpack("<IHHQIIIIIIQ", header)[8]
                self.assertEqual(status, STATUS_CHECKSUM_MISMATCH)

            # Wrong protocol version closes the connection without service.
            with socket.create_connection(("127.0.0.1", port), timeout=3.0) as sock:
                sock.settimeout(3.0)
                bad_version = struct.pack(
                    "<IHHQIIIIIIQ",
                    WIRE_MAGIC, 99, WIRE_HEADER_LEN,
                    3, OP_STATUS, 0, 0, 0, 0, 0, 0,
                )
                sock.sendall(bad_version)
                self.assertEqual(sock.recv(WIRE_HEADER_LEN), b"")

            # Unknown operation gets a deterministic UNSUPPORTED error, never
            # a fallback to a legacy payload path.
            status, body, _ = _wire_exchange(port, 999)
            self.assertEqual(status, STATUS_UNSUPPORTED)
            self.assertIn(b"network_operation_not_allowed", body)

            # The daemon survived every malformed peer above.
            status, _, _ = _wire_exchange(port, OP_HEALTH)
            self.assertEqual(status, STATUS_OK)
        finally:
            self._stop_server(proc)

    # T-control row 5: timeout and resource bounds.
    def test_timeout_and_resource_bounds(self):
        refused = self._run_client(
            "health", "--connect", f"tcp:127.0.0.1:{self._free_tcp_port()}",
            "--timeout-ms", "500",
        )
        self.assertNotEqual(refused.returncode, 0)

        port = self._free_tcp_port()
        config = self._write_config(
            "timeout.conf",
            self._network_config(f"tcp:127.0.0.1:{port}"),
        )
        proc = self._start_server(config, probe_port=port)
        try:
            # A peer that stops mid-header is closed within the configured
            # I/O budget instead of holding the connection forever.
            stalled = socket.create_connection(("127.0.0.1", port), timeout=3.0)
            stalled.settimeout(3.0)
            stalled.sendall(_wire_header(1, OP_STATUS, b"")[:10])
            started = time.monotonic()
            remainder = stalled.recv(WIRE_HEADER_LEN)
            stalled.close()
            self.assertEqual(remainder, b"")
            self.assertLess(time.monotonic() - started, 3.0)

            # A slow drip that would exceed the budget is also cut off.
            drip = socket.create_connection(("127.0.0.1", port), timeout=5.0)
            drip.settimeout(5.0)
            header = _wire_header(2, OP_STATUS, b"")
            started = time.monotonic()
            for byte in header[:20]:
                drip.sendall(bytes([byte]))
                time.sleep(0.05)
            self.assertEqual(drip.recv(WIRE_HEADER_LEN), b"")
            drip.close()
            self.assertLess(time.monotonic() - started, 5.0)

            # While one peer stalls, another peer's readiness query still
            # completes within a bounded time (single budget per connection).
            blocked = socket.create_connection(("127.0.0.1", port), timeout=3.0)
            blocked.sendall(_wire_header(3, OP_STATUS, b"")[:8])
            started = time.monotonic()
            status, _, _ = _wire_exchange(port, OP_READY, timeout=5.0)
            elapsed = time.monotonic() - started
            blocked.close()
            self.assertEqual(status, STATUS_OK)
            self.assertLess(elapsed, 4.0)

            # Repeated connect/close cycles do not wedge the listener.
            for _ in range(50):
                with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                    pass
            status, _, _ = _wire_exchange(port, OP_HEALTH)
            self.assertEqual(status, STATUS_OK)
        finally:
            self._stop_server(proc)

    # T-control row 6: reply loss, retries and unsupported local operations.
    def test_reply_loss_and_idempotent_retries(self):
        port = self._free_tcp_port()
        config = self._write_config(
            "reply_loss.conf",
            self._network_config(f"tcp:127.0.0.1:{port}"),
        )
        proc = self._start_server(config, probe_port=port)
        try:
            # A client that vanishes before reading the reply does not damage
            # the service; the next request is served normally.
            for _ in range(5):
                sock = socket.create_connection(("127.0.0.1", port), timeout=2.0)
                sock.sendall(_wire_header(1, OP_HEALTH, b""))
                sock.close()
            status, _, _ = _wire_exchange(port, OP_HEALTH)
            self.assertEqual(status, STATUS_OK)

            # Query retries with the bounded-attempt options stay safe.
            retried = self._run_client(
                "health",
                "--connect", f"tcp:127.0.0.1:{port}",
                "--max-attempts", "3",
                "--retry-backoff-ms", "10",
                "--retry-timeouts",
            )
            self.assertEqual(retried.returncode, 0, retried.stderr + retried.stdout)

            # Legacy local file/admin operations stay rejected on the network
            # endpoint with UNSUPPORTED instead of executing.
            for args in (
                ("put-object", "--key", "k1", "--payload-file", "/dev/null"),
                ("list-records",),
                ("materialize-object", "--key", "k1", "--to", str(self.root / "m.bin")),
                ("export-snapshot",),
            ):
                result = self._run_client(*args, "--connect", f"tcp:127.0.0.1:{port}")
                self.assertNotEqual(result.returncode, 0, args)
                self.assertIn("unsupported", result.stdout, args)

            # Control connections carry metadata only; record state is
            # unchanged by the disconnect churn above.
            status, body, _ = _wire_exchange(port, OP_STATUS)
            self.assertEqual(status, STATUS_OK)
            self.assertIn(b"record_count=0", body)
        finally:
            stdout, stderr = self._stop_server(proc)
        self.assertIn("status=stopped", stdout)

    # T-control config coverage: schema and example document the new mode.
    def test_config_schema_documents_trusted_guest_network(self):
        schema = CONFIG_SCHEMA.read_text()
        self.assertIn("trusted-guest-network", schema)
        self.assertIn("network_peer", schema)
        self.assertIn("network_io_timeout_ms", schema)
        example = EXAMPLE_CONFIG.read_text()
        self.assertIn("trusted-guest-network", example)
        self.assertIn("network_peer=", example)

        # The shipped example stays a local-only config: every network access
        # line is commented out, so auth_mode resolves to none by default.
        for line in example.splitlines():
            stripped = line.strip()
            if "trusted-guest-network" in stripped or stripped.startswith("network_peer"):
                self.assertTrue(stripped.startswith("#"), stripped)


if __name__ == "__main__":
    unittest.main()
