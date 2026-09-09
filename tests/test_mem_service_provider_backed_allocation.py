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

HOME_NODE = "node-a"
HOME_INCARNATION = 7


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
class MemServiceProviderBackedAllocationTests(unittest.TestCase):
    """M1.2 wire-level behavior for provider-backed managed allocations.

    Two real processes per test: the daemon holds the managed table and
    provider directory, the CLI drives allocate/publish/reclaim over a
    unix endpoint. Covered end to end here: home-provider binding at
    allocate time, ALLOCATING persistence until publish, descriptor and
    address round-trip, publish replay/conflict/caller validation,
    reclaim-confirmed retirement, reclaim-unconfirmed quarantine,
    abandon of an unpublished intent, generation freshness on
    re-allocate, stats counters, config validation, and the regression
    for transient gate failures never being recorded as idempotent
    outcomes. Pure state-machine edge detail stays in the C
    `allocation-fixtures` self check.
    """

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="msvc_pba_", dir=str(_tmp_parent())))
        self.binary = self.root / "linqu_mem_service"
        self.socket = self.root / "pba.sock"
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

    def _start_daemon(self, config_path: Path) -> subprocess.Popen:
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
            health = self._run_client("health", "--connect", f"unix:{self.socket}")
            if health.returncode == 0 and "status=ok" in health.stdout:
                return proc
            time.sleep(0.05)
        self._stop_server(proc)
        self.fail("mem_service daemon did not become ready")

    def _start_home_daemon(self, extra_config: str = "") -> subprocess.Popen:
        lines = [
            f"required_provider={HOME_NODE}",
            "provider_lease_ms=30000",
            f"allocation_home_provider={HOME_NODE}",
        ]
        if extra_config:
            lines.append(extra_config)
        config = self._write_config("pba.conf", self._unix_config("\n".join(lines)))
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

    def _register_home(self, incarnation: int = HOME_INCARNATION) -> subprocess.CompletedProcess:
        return self._run_client(
            "provider-register",
            "--node-id", HOME_NODE,
            "--incarnation", str(incarnation),
            "--readiness-generation", "1",
            "--capabilities", "1",
            "--connect", self._connect,
        )

    def _allocate(self, key: str, idempotency_key: str) -> subprocess.CompletedProcess:
        return self._run_client(
            "allocate-object",
            "--key", key,
            "--idempotency-key", idempotency_key,
            "--session-id", "session-a",
            "--size-bytes", "4096",
            "--capabilities", "1",
            "--connect", self._connect,
        )

    def _publish(self, key: str, generation: int, descriptor_hex: str = "deadbeefcafe",
                 node_id: str = HOME_NODE, incarnation: int = HOME_INCARNATION,
                 address: int = 4096, address_len: int = 4096) -> subprocess.CompletedProcess:
        return self._run_client(
            "publish-allocation",
            "--key", key,
            "--node-id", node_id,
            "--incarnation", str(incarnation),
            "--generation", str(generation),
            "--descriptor-hex", descriptor_hex,
            "--address", str(address),
            "--address-len", str(address_len),
            "--connect", self._connect,
        )

    def _reclaim(self, key: str, generation: int, confirmed: int,
                 node_id: str = HOME_NODE,
                 incarnation: int = HOME_INCARNATION) -> subprocess.CompletedProcess:
        return self._run_client(
            "reclaim-allocation",
            "--key", key,
            "--node-id", node_id,
            "--incarnation", str(incarnation),
            "--generation", str(generation),
            "--confirmed", str(confirmed),
            "--connect", self._connect,
        )

    def _inspect(self, key: str) -> dict[str, str]:
        result = self._run_client(
            "inspect-allocation", "--key", key, "--connect", self._connect
        )
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("status=ok", result.stdout)
        return _parse_kv(result.stdout)

    def _stats(self) -> dict[str, str]:
        result = self._run_client("allocation-stats", "--connect", self._connect)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        return _parse_kv(result.stdout)

    def _allocate_bound(self, key: str, idempotency_key: str) -> int:
        """Allocate against a registered home provider; returns generation."""
        result = self._allocate(key, idempotency_key)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        view = _parse_kv(result.stdout)
        self.assertEqual(view.get("status"), "ok", result.stdout)
        self.assertEqual(view.get("state"), "allocating", result.stdout)
        self.assertEqual(view.get("home_node"), HOME_NODE, result.stdout)
        self.assertEqual(
            view.get("provider_incarnation"), str(HOME_INCARNATION), result.stdout
        )
        self.assertEqual(view.get("provider_backed"), "0", result.stdout)
        return int(view["generation"])

    # The bound home provider publishes the reserved descriptor; the
    # allocation stays ALLOCATING until then and the descriptor/address
    # round-trips through inspect.
    def test_publish_activates_with_descriptor_roundtrip(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("obj-1", "idem-pub-1")

            published = self._publish("obj-1", generation)
            self.assertEqual(published.returncode, 0, published.stderr + published.stdout)
            view = _parse_kv(published.stdout)
            self.assertEqual(view.get("status"), "ok", published.stdout)
            self.assertEqual(view.get("state"), "active", published.stdout)
            self.assertEqual(view.get("provider_backed"), "1", published.stdout)
            self.assertEqual(view.get("descriptor_hex"), "deadbeefcafe", published.stdout)
            self.assertEqual(view.get("descriptor_len"), "6", published.stdout)
            self.assertEqual(view.get("address"), "4096", published.stdout)
            self.assertEqual(view.get("address_len"), "4096", published.stdout)

            inspected = self._inspect("obj-1")
            self.assertEqual(inspected.get("state"), "active")
            self.assertEqual(inspected.get("provider_backed"), "1")
            self.assertEqual(inspected.get("descriptor_hex"), "deadbeefcafe")
            self.assertEqual(inspected.get("home_node"), HOME_NODE)
        finally:
            self._stop_server(proc)

    # Identical publish replayed by the home provider is idempotent; a
    # diverging descriptor for an ACTIVE object is a state conflict.
    def test_publish_idempotent_replay_and_descriptor_conflict(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("obj-2", "idem-pub-2")
            self.assertEqual(self._publish("obj-2", generation).returncode, 0)

            replay = self._publish("obj-2", generation)
            self.assertEqual(replay.returncode, 0, replay.stderr + replay.stdout)
            self.assertIn("state=active", replay.stdout)

            conflict = self._publish("obj-2", generation, descriptor_hex="00112233")
            self.assertIn("reason=state_conflict", conflict.stdout)
            stats = self._stats()
            self.assertEqual(stats.get("publish_ok_count"), "2")
            self.assertEqual(stats.get("publish_rejected_count"), "1")
        finally:
            self._stop_server(proc)

    # Only the bound home provider's active incarnation may publish:
    # foreign nodes, stale incarnations and stale generations are all
    # rejected without touching the allocation.
    def test_publish_caller_and_generation_validation(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("obj-3", "idem-pub-3")

            foreign = self._publish("obj-3", generation, node_id="node-b")
            self.assertIn("reason=provider_unavailable", foreign.stdout)

            # node-a is active but under a different incarnation, so the
            # caller check itself reports the divergence before the
            # managed table is touched.
            stale_incarnation = self._publish("obj-3", generation, incarnation=99)
            self.assertIn("reason=provider_mismatch", stale_incarnation.stdout)

            stale_generation = self._publish("obj-3", generation + 1)
            self.assertIn("reason=stale_generation", stale_generation.stdout)

            view = self._inspect("obj-3")
            self.assertEqual(view.get("state"), "allocating")
            self.assertEqual(view.get("provider_backed"), "0")
            # Only the stale-generation rejection reached the managed
            # table; caller-check rejections are pre-table auth failures.
            stats = self._stats()
            self.assertEqual(stats.get("publish_ok_count"), "0")
            self.assertEqual(stats.get("publish_rejected_count"), "1")
        finally:
            self._stop_server(proc)

    # A drained provider-backed object waits in RETIRING for the home
    # provider's reclaim confirmation; confirmed=1 lands RETIRED.
    def test_reclaim_confirmed_retires_after_holder_drain(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("obj-4", "idem-rec-1")
            self.assertEqual(self._publish("obj-4", generation).returncode, 0)

            acquired = self._run_client(
                "acquire-object", "--key", "obj-4", "--idempotency-key", "acq-4",
                "--session-id", "session-b", "--connect", self._connect,
            )
            self.assertEqual(acquired.returncode, 0, acquired.stderr + acquired.stdout)

            retired = self._run_client(
                "retire-object", "--key", "obj-4", "--idempotency-key", "ret-4",
                "--connect", self._connect,
            )
            self.assertIn("state=retiring", retired.stdout)

            released = self._run_client(
                "release-object", "--key", "obj-4", "--idempotency-key", "rel-4",
                "--session-id", "session-b", "--connect", self._connect,
            )
            self.assertEqual(released.returncode, 0, released.stderr + released.stdout)
            self.assertIn(
                "state=retiring",
                released.stdout,
                "provider-backed object must wait for reclaim confirmation",
            )

            reclaim = self._reclaim("obj-4", generation, confirmed=1)
            self.assertEqual(reclaim.returncode, 0, reclaim.stderr + reclaim.stdout)
            self.assertIn("state=retired", reclaim.stdout)
            self.assertEqual(self._inspect("obj-4").get("state"), "retired")
            stats = self._stats()
            self.assertEqual(stats.get("reclaim_ok_count"), "1")
            self.assertEqual(stats.get("quarantined_objects"), "0")
        finally:
            self._stop_server(proc)

    # confirmed=0 means the provider could not confirm the backing
    # release: the object is quarantined and counted separately.
    def test_reclaim_unconfirmed_quarantines(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("obj-5", "idem-rec-2")
            self.assertEqual(self._publish("obj-5", generation).returncode, 0)
            retired = self._run_client(
                "retire-object", "--key", "obj-5", "--idempotency-key", "ret-5",
                "--connect", self._connect,
            )
            self.assertIn("state=retiring", retired.stdout)

            reclaim = self._reclaim("obj-5", generation, confirmed=0)
            self.assertEqual(reclaim.returncode, 0, reclaim.stderr + reclaim.stdout)
            self.assertIn("state=quarantined", reclaim.stdout)
            stats = self._stats()
            self.assertEqual(stats.get("quarantined_objects"), "1")
            self.assertEqual(stats.get("quarantined_bytes"), "4096")
            self.assertEqual(stats.get("quarantine_events"), "1")
        finally:
            self._stop_server(proc)

    # Reclaim rejects foreign callers, stale generations and objects
    # that are not waiting for confirmation.
    def test_reclaim_validation(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("obj-6", "idem-rec-3")

            early = self._reclaim("obj-6", generation, confirmed=1)
            self.assertIn("reason=state_conflict", early.stdout)

            self.assertEqual(self._publish("obj-6", generation).returncode, 0)
            self._run_client(
                "retire-object", "--key", "obj-6", "--idempotency-key", "ret-6",
                "--connect", self._connect,
            )

            foreign = self._reclaim("obj-6", generation, confirmed=1, node_id="node-b")
            self.assertIn("reason=provider_unavailable", foreign.stdout)

            stale = self._reclaim("obj-6", generation + 1, confirmed=1)
            self.assertIn("reason=stale_generation", stale.stdout)

            # The foreign caller is rejected before reaching the managed
            # table, so only the two table-level rejections are counted.
            stats = self._stats()
            self.assertEqual(stats.get("reclaim_ok_count"), "0")
            self.assertEqual(stats.get("reclaim_rejected_count"), "2")
            self.assertEqual(self._inspect("obj-6").get("state"), "retiring")
        finally:
            self._stop_server(proc)

    # Retiring an unpublished ALLOCATING intent abandons it straight to
    # RETIRED: no backing was reserved, so no reclaim is involved.
    def test_retire_on_allocating_abandons_without_reclaim(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("obj-7", "idem-abd-1")

            retired = self._run_client(
                "retire-object", "--key", "obj-7", "--idempotency-key", "abd-7",
                "--connect", self._connect,
            )
            self.assertEqual(retired.returncode, 0, retired.stderr + retired.stdout)
            self.assertIn("state=retired", retired.stdout)

            late_publish = self._publish("obj-7", generation)
            self.assertIn("reason=state_conflict", late_publish.stdout)
            stats = self._stats()
            self.assertEqual(stats.get("reclaim_ok_count"), "0")
            self.assertEqual(stats.get("quarantined_objects"), "0")
        finally:
            self._stop_server(proc)

    # Regression: a transient data_plane_not_ready failure must not be
    # recorded as the idempotent outcome; retrying the same idempotency
    # key after the gate opens executes the operation fresh.
    def test_gated_allocate_retry_with_same_idempotency_key_executes_fresh(self):
        proc = self._start_home_daemon()
        try:
            gated = self._allocate("obj-8", "idem-gate-1")
            self.assertIn("reason=data_plane_not_ready", gated.stdout)

            self.assertEqual(self._register_home().returncode, 0)
            retried = self._allocate("obj-8", "idem-gate-1")
            self.assertEqual(retried.returncode, 0, retried.stderr + retried.stdout)
            view = _parse_kv(retried.stdout)
            self.assertEqual(view.get("status"), "ok", retried.stdout)
            self.assertEqual(view.get("state"), "allocating", retried.stdout)
            self.assertEqual(view.get("home_node"), HOME_NODE, retried.stdout)
        finally:
            self._stop_server(proc)

    # Re-allocating a retired key creates a fresh identity with a new
    # generation; stale generations stay rejected afterwards.
    def test_reallocate_after_reclaim_gets_fresh_generation(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            first = self._allocate_bound("obj-9", "idem-gen-1")
            self.assertEqual(self._publish("obj-9", first).returncode, 0)
            self._run_client(
                "retire-object", "--key", "obj-9", "--idempotency-key", "ret-9",
                "--connect", self._connect,
            )
            self.assertEqual(self._reclaim("obj-9", first, confirmed=1).returncode, 0)

            second = self._allocate_bound("obj-9", "idem-gen-2")
            self.assertNotEqual(first, second)

            stale = self._publish("obj-9", first)
            self.assertIn("reason=stale_generation", stale.stdout)
            self.assertEqual(self._publish("obj-9", second).returncode, 0)
        finally:
            self._stop_server(proc)

    # Without a configured home provider the daemon keeps its M1.1
    # fail-closed behavior (backing_unavailable) and publish finds no
    # active provider binding.
    def test_legacy_allocate_without_home_provider_stays_fail_closed(self):
        config = self._write_config("legacy.conf", self._unix_config())
        proc = self._start_daemon(config)
        try:
            allocated = self._allocate("obj-10", "idem-leg-1")
            self.assertIn("reason=backing_unavailable", allocated.stdout)

            published = self._publish("obj-10", 1)
            self.assertIn("reason=provider_unavailable", published.stdout)
        finally:
            self._stop_server(proc)

    # Config validation: an empty or over-long home provider node id
    # fails startup instead of being silently ignored.
    def test_config_validation_rejects_invalid_home_provider(self):
        empty = self._write_config(
            "empty.conf", self._unix_config("allocation_home_provider=")
        )
        result = subprocess.run(
            [str(self.binary), "serve", "--config", str(empty)],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("allocation_home_provider", result.stderr)

        too_long = self._write_config(
            "long.conf",
            self._unix_config(f"allocation_home_provider={'n' * 64}"),
        )
        result = subprocess.run(
            [str(self.binary), "serve", "--config", str(too_long)],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("allocation_home_provider", result.stderr)

    # CLI argument validation for the new provider-facing commands.
    def test_publish_and_reclaim_cli_argument_validation(self):
        proc = self._start_home_daemon()
        try:
            missing_descriptor = self._run_client(
                "publish-allocation",
                "--key", "obj-11",
                "--node-id", HOME_NODE,
                "--incarnation", str(HOME_INCARNATION),
                "--generation", "1",
                "--address", "4096",
                "--address-len", "4096",
                "--connect", self._connect,
            )
            self.assertEqual(missing_descriptor.returncode, 2)
            self.assertIn("--descriptor-hex", missing_descriptor.stderr)

            missing_confirmed = self._run_client(
                "reclaim-allocation",
                "--key", "obj-11",
                "--node-id", HOME_NODE,
                "--incarnation", str(HOME_INCARNATION),
                "--generation", "1",
                "--connect", self._connect,
            )
            self.assertEqual(missing_confirmed.returncode, 2)
            self.assertIn("--confirmed", missing_confirmed.stderr)

            bad_hex = self._publish("obj-11", 1, descriptor_hex="zz")
            self.assertIn("field=descriptor_hex", bad_hex.stdout)
        finally:
            self._stop_server(proc)


if __name__ == "__main__":
    unittest.main()
