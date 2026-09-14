import shutil
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT
SERVICE_DIR = ROOT / "components" / "mem_service"
CLI_SOURCE = ROOT / "apps" / "mem_service" / "mem_service.c"

DIRECTORY_STATS_FIELDS = (
    "register_ok_count",
    "register_replace_count",
    "refresh_ok_count",
    "deregister_ok_count",
    "register_rejected_count",
    "refresh_rejected_count",
    "deregister_rejected_count",
    "incarnation_conflict_count",
    "expired_count",
)

IO_TIMEOUT_MS = 500


def _tmp_parent() -> Path:
    private_tmp = Path("/private/tmp")
    if private_tmp.exists():
        return private_tmp
    return Path(tempfile.gettempdir())


def _parse_kv(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class MemServiceProviderDirectoryTests(unittest.TestCase):
    """M1.2 wire-level behavior for the control-plane provider directory.

    Two real processes per test: the daemon holds the directory, the CLI
    drives register/refresh/status/deregister over unix or TCP endpoints.
    Covered end to end here: registration lifecycle, data-plane gating on
    required-provider readiness, incarnation replace/conflict, lease
    expiry revoking readiness, idempotent replay, payload and config
    validation, and the network allowlist for the 0x76 segment. Pure
    state-machine edge detail (capacity, first_missing, per-entry
    timestamps) stays in the C `provider-directory-fixtures` self check.
    """

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="msvc_pdir_", dir=str(_tmp_parent())))
        self.binary = self.root / "linqu_mem_service"
        self.socket = self.root / "pdir.sock"
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

    def _write_config(self, name: str, text: str) -> Path:
        path = self.root / name
        path.write_text(text)
        return path

    def _unix_config(self, extra: str = "") -> str:
        lines = [f"listen=unix:{self.socket}"]
        if extra:
            lines.append(extra)
        return "\n".join(lines) + "\n"

    def _start_daemon(self, config_path: Path, connect: str | None = None,
                      tcp_port: int | None = None) -> subprocess.Popen:
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
            if tcp_port is not None:
                try:
                    with socket.create_connection(("127.0.0.1", tcp_port), timeout=0.2):
                        return proc
                except OSError:
                    pass
            else:
                health = self._run_client(
                    "health", "--connect", connect or f"unix:{self.socket}"
                )
                if health.returncode == 0 and "status=ok" in health.stdout:
                    return proc
            time.sleep(0.05)
        self._stop_server(proc)
        self.fail("mem_service daemon did not become ready")

    def _start_unix_daemon(self, extra_config: str = "") -> subprocess.Popen:
        config = self._write_config("pdir.conf", self._unix_config(extra_config))
        return self._start_daemon(config)

    def _stop_server(self, proc: subprocess.Popen) -> tuple[str, str]:
        proc.terminate()
        try:
            stdout, stderr = proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=5)
        return stdout, stderr

    @property
    def _connect(self) -> str:
        return f"unix:{self.socket}"

    def _register(self, node_id: str, incarnation: int, generation: int = 1,
                  capabilities: int = 1) -> subprocess.CompletedProcess:
        return self._run_client(
            "provider-register",
            "--node-id", node_id,
            "--incarnation", str(incarnation),
            "--readiness-generation", str(generation),
            "--capabilities", str(capabilities),
            "--connect", self._connect,
        )

    def _refresh(self, node_id: str, incarnation: int,
                 generation: int = 1) -> subprocess.CompletedProcess:
        return self._run_client(
            "provider-refresh",
            "--node-id", node_id,
            "--incarnation", str(incarnation),
            "--readiness-generation", str(generation),
            "--connect", self._connect,
        )

    def _deregister(self, node_id: str, incarnation: int) -> subprocess.CompletedProcess:
        return self._run_client(
            "provider-deregister",
            "--node-id", node_id,
            "--incarnation", str(incarnation),
            "--connect", self._connect,
        )

    def _directory_status(self) -> dict[str, str]:
        result = self._run_client(
            "provider-directory-status", "--connect", self._connect
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("status=ok", result.stdout)
        return _parse_kv(result.stdout)

    def _allocate_once(self, idempotency_key: str) -> subprocess.CompletedProcess:
        return self._run_client(
            "allocate-object",
            "--key", "gate-probe",
            "--idempotency-key", idempotency_key,
            "--session-id", "session-a",
            "--size-bytes", "4096",
            "--capabilities", "1",
            "--connect", self._connect,
        )

    # Fresh daemon without directory config: legacy behavior, the gate is
    # open (zero required providers) and every counter starts at zero.
    def test_directory_status_shape_on_fresh_daemon(self):
        proc = self._start_unix_daemon()
        try:
            status = self._directory_status()
            self.assertEqual(status["directory_epoch"], "0")
            self.assertEqual(status["lease_ms"], "5000")
            self.assertEqual(status["provider_required_count"], "0")
            self.assertEqual(status["provider_active_count"], "0")
            self.assertEqual(status["provider_directory_ready"], "1")
            for field in DIRECTORY_STATS_FIELDS:
                self.assertIn(field, status, f"missing stats field {field}")
                self.assertEqual(
                    status[field], "0",
                    f"stats field {field} should start at 0, got {status[field]}",
                )
        finally:
            self._stop_server(proc)

    # Full lifecycle: two required nodes register, the directory flips
    # ready only when both are active; deregister drops readiness and
    # later refresh/deregister of the removed node is not_found.
    def test_register_refresh_deregister_lifecycle(self):
        proc = self._start_unix_daemon(
            "required_provider=node-a\nrequired_provider=node-b\n"
        )
        try:
            status = self._directory_status()
            self.assertEqual(status["provider_required_count"], "2")
            self.assertEqual(status["provider_active_count"], "0")
            self.assertEqual(status["provider_directory_ready"], "0")

            result = self._register("node-a", 1)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("status=ok", result.stdout)
            self.assertIn("replaced=0", result.stdout)
            self.assertIn("provider_directory_ready=0", result.stdout)

            result = self._register("node-b", 1, capabilities=3)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("provider_active_count=2", result.stdout)
            self.assertIn("provider_directory_ready=1", result.stdout)

            result = self._refresh("node-a", 1, generation=2)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("provider_directory_ready=1", result.stdout)

            result = self._run_client(
                "provider-directory-status", "--connect", self._connect
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("provider=node-a", result.stdout)
            self.assertIn("readiness_generation=2", result.stdout)

            result = self._deregister("node-b", 1)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("provider_active_count=1", result.stdout)
            self.assertIn("provider_directory_ready=0", result.stdout)

            result = self._refresh("node-b", 1)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("status=not_found", result.stdout)
            self.assertIn("reason=not_found", result.stdout)

            result = self._deregister("node-b", 1)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("status=not_found", result.stdout)

            status = self._directory_status()
            self.assertEqual(status["register_ok_count"], "2")
            self.assertEqual(status["refresh_ok_count"], "1")
            self.assertEqual(status["deregister_ok_count"], "1")
            self.assertEqual(status["refresh_rejected_count"], "1")
            self.assertEqual(status["deregister_rejected_count"], "1")
        finally:
            self._stop_server(proc)

    # Data-plane gate: managed data operations stay fail-closed with
    # reason=data_plane_not_ready until every required provider holds a
    # fresh registration, and close again when one leaves.
    def test_data_plane_gate_tracks_directory_readiness(self):
        proc = self._start_unix_daemon("required_provider=node-a\n")
        try:
            result = self._allocate_once("gate-1")
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("status=internal", result.stdout)
            self.assertIn("reason=data_plane_not_ready", result.stdout)
            self.assertNotIn("backing_unavailable", result.stdout)

            result = self._register("node-a", 1)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

            # Gate open: allocate now reaches the managed table and fails
            # only because no backing is registered on the stock daemon.
            result = self._allocate_once("gate-2")
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("reason=backing_unavailable", result.stdout)
            self.assertNotIn("data_plane_not_ready", result.stdout)

            result = self._deregister("node-a", 1)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

            result = self._allocate_once("gate-3")
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("reason=data_plane_not_ready", result.stdout)
        finally:
            self._stop_server(proc)

    # Reboot replace: a new incarnation of the same node replaces the
    # stale entry atomically; the old incarnation can neither refresh nor
    # deregister (stale_ref / incarnation_conflict) while the active one
    # keeps working.
    def test_incarnation_replace_rejects_stale_writer(self):
        proc = self._start_unix_daemon("required_provider=node-a\n")
        try:
            result = self._register("node-a", 1)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            epoch_boot1 = self._directory_status()["directory_epoch"]

            result = self._register("node-a", 2, generation=7)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("replaced=1", result.stdout)
            self.assertIn("provider_active_count=1", result.stdout)
            self.assertIn("provider_directory_ready=1", result.stdout)
            epoch_boot2 = self._directory_status()["directory_epoch"]
            self.assertNotEqual(epoch_boot2, epoch_boot1)

            result = self._refresh("node-a", 1)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("status=stale_ref", result.stdout)
            self.assertIn("reason=incarnation_conflict", result.stdout)

            result = self._deregister("node-a", 1)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("status=stale_ref", result.stdout)
            self.assertIn("reason=incarnation_conflict", result.stdout)

            result = self._refresh("node-a", 2, generation=8)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

            status = self._directory_status()
            self.assertEqual(status["register_ok_count"], "2")
            self.assertEqual(status["register_replace_count"], "1")
            self.assertEqual(status["incarnation_conflict_count"], "2")
            self.assertEqual(status["refresh_ok_count"], "1")
            self.assertEqual(status["provider_active_count"], "1")
            self.assertEqual(status["provider_directory_ready"], "1")
        finally:
            self._stop_server(proc)

    # Lease expiry (失联撤销): an entry that misses its lease is expired
    # on poll, readiness is revoked and the data-plane gate closes; a
    # re-registration restores both.
    def test_lease_expiry_revokes_readiness(self):
        proc = self._start_unix_daemon(
            "required_provider=node-a\nprovider_lease_ms=300\n"
        )
        try:
            result = self._register("node-a", 1)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("lease_ms=300", result.stdout)
            self.assertIn("provider_directory_ready=1", result.stdout)

            time.sleep(0.6)

            status = self._directory_status()
            self.assertEqual(status["expired_count"], "1")
            self.assertEqual(status["provider_active_count"], "0")
            self.assertEqual(status["provider_directory_ready"], "0")

            result = self._allocate_once("gate-expired")
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("reason=data_plane_not_ready", result.stdout)

            result = self._register("node-a", 2)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("provider_directory_ready=1", result.stdout)

            status = self._directory_status()
            self.assertEqual(status["provider_active_count"], "1")
            self.assertEqual(status["provider_directory_ready"], "1")
        finally:
            self._stop_server(proc)

    # Idempotent replay: re-registering the same (node_id, incarnation)
    # reports the current state without mutating the directory; the
    # directory epoch does not move and no replace is counted.
    def test_register_idempotent_replay(self):
        proc = self._start_unix_daemon("required_provider=node-a\n")
        try:
            result = self._register("node-a", 1, generation=3)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            epoch_first = self._directory_status()["directory_epoch"]

            result = self._register("node-a", 1, generation=3)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("replaced=0", result.stdout)
            self.assertIn(f"directory_epoch={epoch_first}", result.stdout)

            status = self._directory_status()
            self.assertEqual(status["directory_epoch"], epoch_first)
            self.assertEqual(status["register_ok_count"], "2")
            self.assertEqual(status["register_replace_count"], "0")
            self.assertEqual(status["provider_active_count"], "1")
            self.assertEqual(status["provider_directory_ready"], "1")
        finally:
            self._stop_server(proc)

    # Register validation: zero incarnation, zero capabilities and
    # capability bits outside the valid mask are invalid_request
    # rejections; the directory stays empty and fail-closed.
    def test_register_validation_failures(self):
        proc = self._start_unix_daemon("required_provider=node-a\n")
        try:
            for incarnation, capabilities in ((1, 0), (1, 4), (0, 1)):
                result = self._register("node-a", incarnation,
                                        capabilities=capabilities)
                self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
                self.assertIn("status=invalid_session", result.stdout)
                self.assertIn("reason=invalid_request", result.stdout)

            result = self._refresh("ghost", 1)
            self.assertEqual(result.returncode, 1, result.stderr + result.stdout)
            self.assertIn("status=not_found", result.stdout)

            status = self._directory_status()
            self.assertEqual(status["register_ok_count"], "0")
            self.assertEqual(status["register_rejected_count"], "3")
            self.assertEqual(status["provider_active_count"], "0")
            self.assertEqual(status["provider_directory_ready"], "0")
        finally:
            self._stop_server(proc)

    # Config validation: duplicate required providers and out-of-bounds
    # leases are rejected at config load, before the daemon ever listens.
    def test_config_validation_rejects_bad_directory_config(self):
        for name, extra in (
            ("dup", "required_provider=node-a\nrequired_provider=node-a\n"),
            ("lease_low", "provider_lease_ms=50\n"),
            ("lease_high", "provider_lease_ms=600001\n"),
        ):
            config = self._write_config(
                f"reject_{name}.conf", self._unix_config(extra)
            )
            result = self._run_client("serve", "--config", str(config))
            self.assertEqual(result.returncode, 2, name + result.stdout + result.stderr)
            self.assertNotIn("status=ready", result.stdout)

    # Readiness and metrics aggregation: the status op payload reflects
    # the directory counters and the metrics op exports the provider
    # operation counters alongside the allocation set. (The ready op
    # stays a shallow ok/internal probe by design.)
    def test_status_and_metrics_reflect_directory(self):
        proc = self._start_unix_daemon("required_provider=node-a\n")
        try:
            result = self._run_client("status", "--connect", self._connect)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            status = _parse_kv(result.stdout)
            self.assertEqual(status["provider_required_count"], "1")
            self.assertEqual(status["provider_active_count"], "0")
            self.assertEqual(status["provider_directory_ready"], "0")

            result = self._register("node-a", 1)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

            result = self._run_client("status", "--connect", self._connect)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            status = _parse_kv(result.stdout)
            self.assertEqual(status["provider_required_count"], "1")
            self.assertEqual(status["provider_active_count"], "1")
            self.assertEqual(status["provider_directory_ready"], "1")

            self._directory_status()

            result = self._run_client("metrics", "--connect", self._connect)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            metrics = _parse_kv(result.stdout)
            self.assertEqual(metrics["provider_register_count"], "1")
            self.assertEqual(metrics["provider_refresh_count"], "0")
            self.assertEqual(metrics["provider_deregister_count"], "0")
            self.assertGreaterEqual(int(metrics["provider_status_count"]), 1)
        finally:
            self._stop_server(proc)

    # Network allowlist: the 0x76 provider segment is exposed on
    # trusted-guest-network TCP endpoints so per-node provider processes
    # can report readiness to the control plane.
    def test_provider_ops_over_tcp_trusted_guest_network(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            listener.bind(("127.0.0.1", 0))
            port = int(listener.getsockname()[1])
        finally:
            listener.close()

        config = self._write_config(
            "pdir_tcp.conf",
            "\n".join(
                [
                    f"listen=tcp:127.0.0.1:{port}",
                    "auth_mode=trusted-guest-network",
                    "node_id=node-a",
                    "network_peer=node-a@127.0.0.1",
                    f"network_io_timeout_ms={IO_TIMEOUT_MS}",
                    "required_provider=node-a",
                ]
            )
            + "\n",
        )
        proc = self._start_daemon(config, tcp_port=port)
        connect = f"tcp:127.0.0.1:{port}"
        try:
            result = self._run_client(
                "provider-register",
                "--node-id", "node-a",
                "--incarnation", "1",
                "--readiness-generation", "1",
                "--capabilities", "1",
                "--connect", connect,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("status=ok", result.stdout)
            self.assertIn("provider_directory_ready=1", result.stdout)

            result = self._run_client(
                "provider-directory-status", "--connect", connect
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("provider=node-a", result.stdout)
            self.assertIn("provider_directory_ready=1", result.stdout)

            result = self._run_client(
                "provider-deregister",
                "--node-id", "node-a",
                "--incarnation", "1",
                "--connect", connect,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn("provider_directory_ready=0", result.stdout)
        finally:
            self._stop_server(proc)


if __name__ == "__main__":
    unittest.main()
