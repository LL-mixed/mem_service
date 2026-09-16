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
SECOND_HOME_NODE = "node-b"
SECOND_HOME_INCARNATION = 11


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

    def test_small_store_restart_preserves_managed_obligations(self):
        """Restart must retain resource identity even without replay archiving."""
        store = self.root / "managed.snapshot"
        proc = self._start_home_daemon(f"store={store}")
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("restart-held", "restart-allocate")
            self.assertEqual(self._publish("restart-held", generation).returncode, 0)
            acquired = self._run_client(
                "acquire-object", "--key", "restart-held",
                "--session-id", "session-a", "--expected-generation", str(generation),
                "--idempotency-key", "restart-acquire", "--connect", self._connect)
            self.assertEqual(acquired.returncode, 0, acquired.stdout + acquired.stderr)
            self._allocate_bound("restart-pending", "restart-pending-allocate")

            def mapping(action, mapping_id):
                result = self._run_client(
                    "mapping-transition", "--key", "restart-held",
                    "--session-id", "session-a", "--generation", str(generation),
                    "--mapping-id", str(mapping_id), "--action", action,
                    "--idempotency-key", "restart-map-" + action,
                    "--connect", self._connect)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return _parse_kv(result.stdout)

            mapping_id = int(mapping("begin", 0)["mapping_id"])
            mapping("confirm", mapping_id)
            original = self._inspect("restart-held")
            lost = self._run_client(
                "provider-deregister", "--node-id", HOME_NODE,
                "--incarnation", str(HOME_INCARNATION), "--connect", self._connect)
            self.assertEqual(lost.returncode, 0, lost.stdout + lost.stderr)
            before = self._stats()
            self.assertEqual(before["quarantined_objects"], "2")
            self.assertEqual(before["import_mappings"], "1")
            self.assertEqual(before["managed_recovery_required"], "1")
            for restart in range(2):
                self._stop_server(proc)
                proc = None
                proc = self._start_home_daemon(f"store={store}")
                after = self._stats()
                for field in ("quarantined_objects", "quarantined_bytes",
                              "live_refs", "export_mappings", "import_mappings",
                              "managed_recovery_required"):
                    self.assertEqual(after[field], before[field], (restart, field))
                inspected = self._run_client(
                    "inspect-allocation", "--key", "restart-held",
                    "--connect", self._connect)
                self.assertEqual(inspected.returncode, 0, inspected.stdout)
                self.assertEqual(_parse_kv(inspected.stdout)["generation"], str(generation))
                current = _parse_kv(inspected.stdout)
                for field in ("descriptor_hex", "address", "address_len",
                              "home_node", "provider_incarnation", "live_refs"):
                    self.assertEqual(current[field], original[field], field)
                self.assertEqual(mapping("inspect", mapping_id)["mapping_id"], str(mapping_id))
                self.assertEqual(self._register_home(8 + restart).returncode, 0)
                new = self._allocate("restart-new", "restart-new-allocate")
                self.assertNotEqual(new.returncode, 0, new.stdout)
                self.assertIn("managed_reconciliation_required", new.stdout + new.stderr)
        finally:
            if proc is not None:
                self._stop_server(proc)

    def test_checkpoint_keeps_unpublished_intent_and_rejects_damage(self):
        store = self.root / "intent.snapshot"
        proc = self._start_home_daemon(f"store={store}")
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("pending", "pending-allocate")
            # The acknowledged intent must survive without graceful shutdown.
            proc.kill()
            proc.wait(timeout=10)
        finally:
            self._stop_server(proc)
        saved = store.read_bytes()
        self.assertTrue(saved.startswith(b"mem_service_store_managed_v2\n"))
        self.assertTrue(saved.endswith(b"managed_store_complete=1\n"))
        proc = self._start_home_daemon(f"store={store}")
        try:
            view = self._inspect("pending")
            self.assertEqual(view["generation"], str(generation))
            self.assertEqual(view["state"], "quarantined")
            stats = self._stats()
            self.assertEqual(stats["in_flight"], "1")
            self.assertEqual(stats["quarantined_objects"], "1")
            self.assertEqual(stats["quarantined_bytes"], "0")
        finally:
            self._stop_server(proc)
        corrupted = bytearray(saved)
        position = saved.index(b"\n", saved.index(b"\n") + 1) + 1
        corrupted[position] = ord("1") if corrupted[position] == ord("0") else ord("0")
        for name, data in (("checksum", bytes(corrupted)),
                           ("truncated", saved[:-25]),
                           ("trailing", saved + b"managed_store_complete=1\n")):
            with self.subTest(damage=name):
                store.write_bytes(data)
                run = subprocess.run(
                    [str(self.binary), "serve", "--config", str(self.root / "pba.conf")],
                    capture_output=True, text=True, timeout=10)
                self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("store load failed", run.stderr)
                self.assertEqual(store.read_bytes(), data)

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

    def _register_home(self, incarnation: int = HOME_INCARNATION,
                       node: str = HOME_NODE) -> subprocess.CompletedProcess:
        return self._run_client(
            "provider-register",
            "--node-id", node,
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

    def _recover(self, key: str, generation: int, current_incarnation: int,
                 fenced_incarnation: int, backing_gone: int,
                 node_id: str = HOME_NODE) -> subprocess.CompletedProcess:
        return self._run_client(
            "recover-allocation",
            "--key", key,
            "--node-id", node_id,
            "--incarnation", str(current_incarnation),
            "--generation", str(generation),
            "--fenced-incarnation", str(fenced_incarnation),
            "--backing-gone", str(backing_gone),
            "--connect", self._connect,
        )

    def _fence_holder(self, key: str, generation: int,
                      current_incarnation: int, fenced_incarnation: int,
                      idempotency_key: str,
                      node_id: str = SECOND_HOME_NODE) -> subprocess.CompletedProcess:
        return self._run_client(
            "fence-allocation-holder",
            "--key", key,
            "--node-id", node_id,
            "--incarnation", str(current_incarnation),
            "--generation", str(generation),
            "--fenced-incarnation", str(fenced_incarnation),
            "--idempotency-key", idempotency_key,
            "--connect", self._connect,
        )

    def _poll_recovery(self, current_incarnation: int,
                       fenced_incarnation: int,
                       after_generation: int = 0,
                       node_id: str = HOME_NODE) -> subprocess.CompletedProcess:
        return self._run_client(
            "poll-recovery",
            "--node-id", node_id,
            "--incarnation", str(current_incarnation),
            "--fenced-incarnation", str(fenced_incarnation),
            "--after-generation", str(after_generation),
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

    def _poll(self, after=0, incarnation=HOME_INCARNATION, node=HOME_NODE):
        return self._run_client(
            "poll-allocation", "--node-id", node,
            "--incarnation", str(incarnation),
            "--after-generation", str(after), "--connect", self._connect,
        )

    def test_allocate_can_select_each_registered_home(self):
        config = self._write_config(
            "multi-home.conf",
            self._unix_config(
                f"required_provider={HOME_NODE}\n"
                f"required_provider={SECOND_HOME_NODE}\n"
                "provider_lease_ms=30000\n"
                f"allocation_home_provider={HOME_NODE}"
            ),
        )
        proc = self._start_daemon(config)
        try:
            first = self._register_home()
            second = self._register_home(
                SECOND_HOME_INCARNATION, node=SECOND_HOME_NODE)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)

            selected = self._run_client(
                "allocate-object",
                "--key", "placed-on-b",
                "--idempotency-key", "placed-on-b-allocate",
                "--session-id", "session-b",
                "--home-node", SECOND_HOME_NODE,
                "--size-bytes", "4096",
                "--capabilities", "1",
                "--connect", self._connect,
            )
            self.assertEqual(selected.returncode, 0, selected.stdout + selected.stderr)
            selected_view = _parse_kv(selected.stdout)
            self.assertEqual(selected_view["home_node"], SECOND_HOME_NODE)
            self.assertEqual(
                selected_view["provider_incarnation"],
                str(SECOND_HOME_INCARNATION),
            )
            polled = self._poll(
                node=SECOND_HOME_NODE,
                incarnation=SECOND_HOME_INCARNATION,
            )
            self.assertEqual(_parse_kv(polled.stdout)["key"], "placed-on-b")
            self.assertIn("status=not_found", self._poll(node=HOME_NODE).stdout)

            defaulted = self._allocate("placed-on-a", "placed-on-a-allocate")
            self.assertEqual(defaulted.returncode, 0, defaulted.stdout + defaulted.stderr)
            self.assertEqual(_parse_kv(defaulted.stdout)["home_node"], HOME_NODE)

            unavailable = self._run_client(
                "allocate-object",
                "--key", "placed-on-missing",
                "--idempotency-key", "placed-on-missing-allocate",
                "--home-node", "node-missing",
                "--size-bytes", "4096",
                "--capabilities", "1",
                "--connect", self._connect,
            )
            self.assertNotEqual(unavailable.returncode, 0, unavailable.stdout)
            self.assertIn("reason=provider_unavailable", unavailable.stdout)

            oversized = self._run_client(
                "allocate-object",
                "--key", "oversized-home",
                "--idempotency-key", "oversized-home-allocate",
                "--home-node", "n" * 64,
                "--size-bytes", "4096",
                "--capabilities", "1",
                "--connect", self._connect,
            )
            self.assertNotEqual(oversized.returncode, 0, oversized.stdout)
            self.assertIn("status=invalid_session", oversized.stdout)
            self.assertIn("field=home_node", oversized.stdout)

            conflicting = self._run_client(
                "allocate-object",
                "--key", "placed-on-b",
                "--idempotency-key", "placed-on-b-conflicting-home",
                "--home-node", HOME_NODE,
                "--size-bytes", "4096",
                "--capabilities", "1",
                "--connect", self._connect,
            )
            self.assertNotEqual(conflicting.returncode, 0, conflicting.stdout)
            self.assertIn("reason=key_conflict", conflicting.stdout)
            self.assertEqual(
                self._inspect("placed-on-b")["home_node"], SECOND_HOME_NODE)
        finally:
            self._stop_server(proc)

    def test_holder_node_identity_scopes_loss_and_survives_restart(self):
        store = self.root / "holder-identity.store"
        config = self._write_config(
            "holder-identity.conf",
            self._unix_config(
                f"store={store}\n"
                f"required_provider={HOME_NODE}\n"
                f"required_provider={SECOND_HOME_NODE}\n"
                "provider_lease_ms=30000\n"
                f"allocation_home_provider={HOME_NODE}"
            ),
        )
        proc = self._start_daemon(config)
        try:
            self.assertEqual(self._register_home().returncode, 0)
            self.assertEqual(self._register_home(
                SECOND_HOME_INCARNATION, SECOND_HOME_NODE).returncode, 0)
            generations = {}
            for key in ("held-by-a", "held-by-b"):
                generation = self._allocate_bound(key, f"{key}-allocate")
                self.assertEqual(self._publish(key, generation).returncode, 0)
                generations[key] = generation

            stale_holder = self._run_client(
                "acquire-object", "--key", "held-by-a",
                "--session-id", "stale-holder-session",
                "--idempotency-key", "stale-holder-acquire",
                "--expected-generation", str(generations["held-by-a"]),
                "--holder-node-id", HOME_NODE,
                "--holder-provider-incarnation", str(HOME_INCARNATION + 1),
                "--connect", self._connect,
            )
            self.assertNotEqual(stale_holder.returncode, 0)
            self.assertIn("reason=holder_provider_mismatch", stale_holder.stdout)
            self.assertEqual(self._inspect("held-by-a")["live_refs"], "0")

            for key, node, incarnation in (
                ("held-by-a", HOME_NODE, HOME_INCARNATION),
                ("held-by-b", SECOND_HOME_NODE, SECOND_HOME_INCARNATION),
            ):
                acquired = self._run_client(
                    "acquire-object", "--key", key,
                    "--session-id", f"{key}-session",
                    "--idempotency-key", f"{key}-acquire",
                    "--expected-generation", str(generations[key]),
                    "--holder-node-id", node,
                    "--holder-provider-incarnation", str(incarnation),
                    "--connect", self._connect,
                )
                self.assertEqual(acquired.returncode, 0,
                                 acquired.stdout + acquired.stderr)
                view = self._inspect(key)
                self.assertEqual(view["holder.0.node_id"], node)
                self.assertEqual(view["holder.0.provider_incarnation"],
                                 str(incarnation))

            lost = self._run_client(
                "provider-deregister", "--node-id", SECOND_HOME_NODE,
                "--incarnation", str(SECOND_HOME_INCARNATION),
                "--connect", self._connect,
            )
            self.assertEqual(lost.returncode, 0, lost.stdout + lost.stderr)
            self.assertEqual(self._inspect("held-by-a")["state"], "active")
            self.assertEqual(self._inspect("held-by-b")["state"], "quarantined")
            self.assertEqual(self._stats()["managed_recovery_required"], "1")

            self._stop_server(proc)
            proc = self._start_daemon(config)
            for key, node, incarnation in (
                ("held-by-a", HOME_NODE, HOME_INCARNATION),
                ("held-by-b", SECOND_HOME_NODE, SECOND_HOME_INCARNATION),
            ):
                view = self._inspect(key)
                self.assertEqual(view["state"], "quarantined")
                self.assertEqual(view["holder.0.node_id"], node)
                self.assertEqual(view["holder.0.provider_incarnation"],
                                 str(incarnation))
            self.assertEqual(store.read_text().splitlines()[0],
                             "mem_service_store_managed_v2")
        finally:
            self._stop_server(proc)

    def test_v1_checkpoint_without_holder_bindings_remains_loadable(self):
        store = self.root / "managed-v1.store"
        proc = self._start_home_daemon(extra_config=f"store={store}")
        try:
            self.assertEqual(self._register_home().returncode, 0)
            self._allocate_bound("legacy-pending", "legacy-pending-allocate")
            self._stop_server(proc)
            proc = None
            data = store.read_bytes()
            self.assertTrue(data.startswith(b"mem_service_store_managed_v2\n"))
            store.write_bytes(data.replace(
                b"mem_service_store_managed_v2\n",
                b"mem_service_store_managed_v1\n", 1))
            proc = self._start_home_daemon(extra_config=f"store={store}")
            view = self._inspect("legacy-pending")
            self.assertEqual(view["state"], "quarantined")
            self.assertEqual(view["live_refs"], "0")
            self.assertEqual(self._stats()["managed_recovery_required"], "1")
        finally:
            if proc is not None:
                self._stop_server(proc)

    def test_provider_loss_preserves_resources_and_blocks_reactivation(self):
        for loss in ("home-deregister", "home-replace", "peer-deregister"):
            with self.subTest(loss=loss):
                proc = self._start_home_daemon()
                try:
                    self.assertEqual(self._register_home().returncode, 0)
                    generation = self._allocate_bound("held", "held-allocate")
                    self.assertEqual(self._publish("held", generation).returncode, 0)
                    self._allocate_bound("pending", "pending-allocate")
                    acquire_args = ("acquire-object", "--key", "held",
                        "--session-id", "session-a", "--idempotency-key", "held-acquire",
                        "--expected-generation", str(generation), "--connect", self._connect)
                    self.assertEqual(self._run_client(*acquire_args).returncode, 0)

                    def transition(action, mapping_id):
                        return self._run_client("mapping-transition", "--key", "held",
                            "--session-id", "session-a", "--generation", str(generation),
                            "--mapping-id", str(mapping_id), "--action", action,
                            "--idempotency-key", "held-" + action, "--connect", self._connect)

                    begun = transition("begin", 0)
                    self.assertEqual(begun.returncode, 0, begun.stdout + begun.stderr)
                    mapping_id = int(_parse_kv(begun.stdout)["mapping_id"])
                    self.assertEqual(transition("confirm", mapping_id).returncode, 0)
                    before = self._inspect("held")
                    wrong = self._run_client("provider-deregister", "--node-id", HOME_NODE,
                        "--incarnation", "99", "--connect", self._connect)
                    self.assertNotEqual(wrong.returncode, 0)
                    self.assertEqual(self._stats()["managed_recovery_required"], "0")

                    node = HOME_NODE
                    if loss == "peer-deregister":
                        node = "node-b"
                        peer = self._run_client("provider-register", "--node-id", node,
                            "--incarnation", "7", "--readiness-generation", "1",
                            "--capabilities", "1", "--connect", self._connect)
                        self.assertEqual(peer.returncode, 0)
                    if loss == "home-replace":
                        lost = self._register_home(8)
                    else:
                        lost = self._run_client("provider-deregister", "--node-id", node,
                            "--incarnation", "7", "--connect", self._connect)
                    self.assertEqual(lost.returncode, 0, lost.stdout + lost.stderr)
                    after = self._inspect("held")
                    self.assertEqual(after["state"], "quarantined")
                    for field in ("generation", "provider_incarnation", "descriptor_hex",
                                  "address", "address_len", "live_refs"):
                        self.assertEqual(after[field], before[field], field)
                    stats = self._stats()
                    self.assertEqual(stats["managed_recovery_required"], "1")
                    self.assertEqual(stats["quarantined_bytes"], "4096")
                    self.assertEqual(stats["quarantined_objects"],
                                     "1" if loss == "peer-deregister" else "2")
                    self.assertEqual(stats["live_refs"], "1")
                    self.assertEqual(stats["import_mappings"], "1")
                    self.assertEqual(stats["export_mappings"], "1")
                    self.assertEqual(stats["in_flight"], "1")
                    release_args = ("release-object", "--key", "held",
                        "--session-id", "session-a", "--expected-generation", str(generation),
                        "--idempotency-key", "held-release", "--connect", self._connect)
                    self.assertIn("reason=state_conflict", self._run_client(*release_args).stdout)
                    self.assertEqual(transition("close", mapping_id).returncode, 0)
                    self.assertEqual(transition("finish", mapping_id).returncode, 0)
                    # A fresh operation ID follows the earlier rejected release.
                    release_args = tuple("held-release-final" if x == "held-release" else x
                                         for x in release_args)
                    self.assertEqual(self._run_client(*release_args).returncode, 0)
                    self.assertEqual(self._register_home().returncode, 0)
                    for rejected in (self._allocate("fresh", "fresh-allocate"),
                                     self._run_client(*acquire_args)):
                        self.assertIn("reason=managed_reconciliation_required", rejected.stdout)
                    # The typed mapping CLI exposes the stable status, not the
                    # daemon's free-text reason; verify its actual contract.
                    rejected = transition("confirm", mapping_id)
                    self.assertNotEqual(rejected.returncode, 0)
                    self.assertIn("status=internal", rejected.stdout)
                    self.assertIn("state=quarantined", self._reclaim("held", generation, 1).stdout)
                    self.assertEqual(self._stats()["live_refs"], "0")
                    self.assertEqual(self._stats()["import_mappings"], "0")
                    self.assertEqual(self._stats()["quarantined_bytes"], "4096")
                finally:
                    self._stop_server(proc)

    def test_recovery_rejects_live_holder_then_drains_after_rejoin(self):
        store = self.root / "recovery.store"
        config = self._write_config(
            "recovery.conf",
            self._unix_config(
                f"required_provider={HOME_NODE}\n"
                f"required_provider={SECOND_HOME_NODE}\n"
                "provider_lease_ms=30000\n"
                f"store={store}\n"
                f"allocation_home_provider={HOME_NODE}"
            ),
        )
        proc = self._start_daemon(config)
        try:
            self.assertEqual(self._register_home().returncode, 0)
            self.assertEqual(self._register_home(
                SECOND_HOME_INCARNATION, SECOND_HOME_NODE).returncode, 0)
            generation = self._allocate_bound("recover-held", "recover-allocate")
            self.assertEqual(self._publish("recover-held", generation).returncode, 0)
            acquired = self._run_client(
                "acquire-object", "--key", "recover-held",
                "--session-id", "recover-session",
                "--idempotency-key", "recover-acquire",
                "--expected-generation", str(generation),
                "--holder-node-id", SECOND_HOME_NODE,
                "--holder-provider-incarnation", str(SECOND_HOME_INCARNATION),
                "--connect", self._connect,
            )
            self.assertEqual(acquired.returncode, 0, acquired.stdout + acquired.stderr)
            begun = self._run_client(
                "mapping-transition", "--key", "recover-held",
                "--session-id", "recover-session",
                "--generation", str(generation), "--mapping-id", "0",
                "--action", "begin", "--idempotency-key", "recover-map-begin",
                "--connect", self._connect,
            )
            self.assertEqual(begun.returncode, 0, begun.stdout + begun.stderr)
            mapping_id = _parse_kv(begun.stdout)["mapping_id"]
            confirmed = self._run_client(
                "mapping-transition", "--key", "recover-held",
                "--session-id", "recover-session",
                "--generation", str(generation), "--mapping-id", mapping_id,
                "--action", "confirm", "--idempotency-key", "recover-map-confirm",
                "--connect", self._connect,
            )
            self.assertEqual(confirmed.returncode, 0,
                             confirmed.stdout + confirmed.stderr)

            lost = self._run_client(
                "provider-deregister", "--node-id", SECOND_HOME_NODE,
                "--incarnation", str(SECOND_HOME_INCARNATION),
                "--connect", self._connect,
            )
            self.assertEqual(lost.returncode, 0, lost.stdout + lost.stderr)
            self.assertEqual(self._inspect("recover-held")["state"], "quarantined")
            self.assertEqual(self._register_home(
                SECOND_HOME_INCARNATION, SECOND_HOME_NODE).returncode, 0)
            denied = self._recover("recover-held", generation,
                                   HOME_INCARNATION, HOME_INCARNATION, 0)
            self.assertNotEqual(denied.returncode, 0)
            self.assertIn("reason=holder_fence_receipt_required", denied.stdout)
            self.assertEqual(self._stats()["import_mappings"], "1")

            replacement = SECOND_HOME_INCARNATION + 1
            self.assertEqual(self._register_home(
                replacement, SECOND_HOME_NODE).returncode, 0)
            still_denied = self._recover("recover-held", generation,
                                         HOME_INCARNATION,
                                         HOME_INCARNATION, 0)
            self.assertNotEqual(still_denied.returncode, 0)
            self.assertIn("reason=holder_fence_receipt_required",
                          still_denied.stdout)
            wrong = self._fence_holder("recover-held", generation,
                                       replacement,
                                       SECOND_HOME_INCARNATION + 99,
                                       "recover-fence-wrong")
            self.assertNotEqual(wrong.returncode, 0)
            self.assertIn("reason=not_holder", wrong.stdout)
            fenced = self._fence_holder("recover-held", generation,
                                        replacement,
                                        SECOND_HOME_INCARNATION,
                                        "recover-fence")
            self.assertEqual(fenced.returncode, 0,
                             fenced.stdout + fenced.stderr)
            fenced_view = _parse_kv(fenced.stdout)
            self.assertEqual(fenced_view["fenced_holder_count"], "1")
            self.assertEqual(fenced_view["fenced_mapping_count"], "1")
            self.assertEqual(fenced_view["live_refs"], "0")
            self.assertEqual(self._stats()["import_mappings"], "0")

            self._stop_server(proc)
            proc = self._start_daemon(config)
            self.assertEqual(self._register_home().returncode, 0)
            self.assertEqual(self._register_home(
                replacement, SECOND_HOME_NODE).returncode, 0)
            replay = self._fence_holder("recover-held", generation,
                                        replacement,
                                        SECOND_HOME_INCARNATION,
                                        "recover-fence")
            self.assertEqual(replay.returncode, 0,
                             replay.stdout + replay.stderr)
            self.assertEqual(_parse_kv(replay.stdout)["fenced_holder_count"],
                             "1")
            self.assertEqual(self._inspect("recover-held")["live_refs"], "0")
            prepared = self._recover("recover-held", generation,
                                     HOME_INCARNATION, HOME_INCARNATION, 0)
            self.assertEqual(prepared.returncode, 0,
                             prepared.stdout + prepared.stderr)
            self.assertEqual(_parse_kv(prepared.stdout)["state"], "retiring")
            stats = self._stats()
            self.assertEqual(stats["live_refs"], "0")
            self.assertEqual(stats["import_mappings"], "0")
            self.assertEqual(stats["managed_recovery_required"], "1")
            reclaimed = self._reclaim("recover-held", generation, 1)
            self.assertEqual(reclaimed.returncode, 0,
                             reclaimed.stdout + reclaimed.stderr)
            self.assertEqual(_parse_kv(reclaimed.stdout)["state"], "retired")
            self.assertEqual(self._stats()["managed_recovery_required"], "0")
            fresh = self._allocate("recover-fresh", "recover-fresh-allocate")
            self.assertEqual(fresh.returncode, 0, fresh.stdout + fresh.stderr)
        finally:
            self._stop_server(proc)

    def test_replaced_home_can_retire_confirmed_lost_backing(self):
        store = self.root / "recover-home.store"
        proc = self._start_home_daemon(f"store={store}")
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("recover-home", "recover-home-allocate")
            self.assertEqual(self._publish("recover-home", generation).returncode, 0)
            self._stop_server(proc)
            proc = self._start_home_daemon(f"store={store}")
            self.assertEqual(self._inspect("recover-home")["state"], "quarantined")
            replacement = HOME_INCARNATION + 1
            self.assertEqual(self._register_home(replacement).returncode, 0)
            pending = self._poll_recovery(replacement, HOME_INCARNATION)
            self.assertEqual(pending.returncode, 0,
                             pending.stdout + pending.stderr)
            pending_view = _parse_kv(pending.stdout)
            self.assertEqual(pending_view["key"], "recover-home")
            self.assertEqual(int(pending_view["generation"]), generation)
            self.assertEqual(pending_view["state"], "quarantined")
            exhausted = self._poll_recovery(replacement, HOME_INCARNATION,
                                            generation)
            self.assertNotEqual(exhausted.returncode, 0)
            self.assertIn("status=not_found", exhausted.stdout)
            wrong = self._recover("recover-home", generation, replacement,
                                  HOME_INCARNATION, 0)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertIn("reason=invalid_backing_proof", wrong.stdout)
            recovered = self._recover("recover-home", generation, replacement,
                                      HOME_INCARNATION, 1)
            self.assertEqual(recovered.returncode, 0,
                             recovered.stdout + recovered.stderr)
            view = _parse_kv(recovered.stdout)
            self.assertEqual(view["state"], "retired")
            self.assertEqual(view["provider_backed"], "0")
            self.assertEqual(view["address_len"], "0")
            self.assertEqual(self._stats()["managed_recovery_required"], "0")
            drained = self._poll_recovery(replacement, HOME_INCARNATION)
            self.assertNotEqual(drained.returncode, 0)
            self.assertIn("status=not_found", drained.stdout)
        finally:
            self._stop_server(proc)

    def test_expired_registration_cannot_revive_pending_allocation(self):
        config = self._write_config("expiry.conf", self._unix_config(
            f"required_provider={HOME_NODE}\nprovider_lease_ms=1000\n"
            f"allocation_home_provider={HOME_NODE}"))
        proc = self._start_daemon(config)
        try:
            self.assertEqual(self._register_home().returncode, 0)
            self._allocate_bound("pending", "expiry-allocate")
            time.sleep(1.1)
            # First request after expiry is registration, with no preceding status poll.
            self.assertEqual(self._register_home().returncode, 0)
            self.assertEqual(self._inspect("pending")["state"], "quarantined")
            stats = self._stats()
            self.assertEqual(stats["quarantined_objects"], "1")
            self.assertEqual(stats["quarantined_bytes"], "0")
            self.assertEqual(stats["in_flight"], "1")
            self.assertEqual(stats["managed_recovery_required"], "1")
            self.assertIn("reason=managed_reconciliation_required",
                          self._allocate("pending", "expiry-allocate").stdout)
        finally:
            self._stop_server(proc)

    def test_provider_loss_after_drain_does_not_latch_recovery(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("done", "done-allocate")
            self.assertEqual(self._publish("done", generation).returncode, 0)
            retired = self._run_client("retire-object", "--key", "done",
                "--idempotency-key", "done-retire", "--connect", self._connect)
            self.assertEqual(retired.returncode, 0)
            self.assertEqual(self._reclaim("done", generation, 1).returncode, 0)
            lost = self._run_client("provider-deregister", "--node-id", HOME_NODE,
                "--incarnation", "7", "--connect", self._connect)
            self.assertEqual(lost.returncode, 0)
            self.assertEqual(self._stats()["managed_recovery_required"], "0")
            self.assertEqual(self._register_home().returncode, 0)
            self.assertEqual(self._allocate("fresh", "fresh-allocate").returncode, 0)
        finally:
            self._stop_server(proc)

    def test_poll_enumerates_pending_without_consuming_work(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            self.assertIn("status=not_found", self._poll().stdout)
            first = self._allocate_bound("poll-a", "poll-idem-a")
            second = self._allocate_bound("poll-b", "poll-idem-b")
            for _ in range(2):
                result = self._poll()
                self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
                view = _parse_kv(result.stdout)
                self.assertEqual(view["key"], "poll-a")
                self.assertEqual(int(view["generation"]), first)
                self.assertEqual(view["state"], "allocating")
            self.assertEqual(_parse_kv(self._poll(first).stdout)["key"], "poll-b")
            self.assertIn("status=not_found", self._poll(second).stdout)
            self.assertEqual(self._publish("poll-a", first).returncode, 0)
            self.assertEqual(_parse_kv(self._poll().stdout)["key"], "poll-b")
        finally:
            self._stop_server(proc)

    def test_poll_rejects_stale_or_unregistered_provider(self):
        proc = self._start_home_daemon()
        try:
            self.assertIn("reason=provider_unavailable", self._poll().stdout)
            self.assertEqual(self._register_home().returncode, 0)
            self._allocate_bound("poll-bound", "poll-bound-idem")
            self.assertIn("reason=provider_mismatch",
                          self._poll(incarnation=HOME_INCARNATION + 1).stdout)
            self.assertIn("reason=provider_unavailable", self._poll(node="foreign").stdout)
            self.assertIn("status=invalid_session", self._poll(node="n" * 256).stdout)
            self.assertIn("status=invalid_session", self._poll(after=-1).stdout)
            self.assertIn("status=invalid_session", self._poll(after=1 << 64).stdout)
            self.assertIn("status=invalid_session", self._poll(incarnation=0).stdout)
            self.assertEqual(self._inspect("poll-bound")["state"], "allocating")
        finally:
            self._stop_server(proc)

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

    def test_rounded_backing_is_counted_separately_from_logical_size(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("padded", "padded-allocate")
            published = self._publish("padded", generation, address_len=2097152)
            self.assertEqual(published.returncode, 0, published.stdout + published.stderr)
            self.assertEqual(self._inspect("padded").get("size_bytes"), "4096")
            stats = self._stats()
            self.assertEqual(stats.get("backing_allocated_bytes"), "2097152")
            self.assertEqual(stats.get("address_reserved_bytes"), "2097152")
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
            self.assertIn("status=not_found", self._poll().stdout)

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

            work = _parse_kv(self._poll().stdout)
            self.assertEqual(work["key"], "obj-4")
            self.assertEqual(work["state"], "retiring")
            self.assertEqual(work["live_refs"], "0")
            reclaim = self._reclaim("obj-4", generation, confirmed=1)
            self.assertEqual(reclaim.returncode, 0, reclaim.stderr + reclaim.stdout)
            self.assertIn("state=retired", reclaim.stdout)
            self.assertEqual(self._inspect("obj-4").get("state"), "retired")
            self.assertIn("status=not_found", self._poll().stdout)
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

    # Cancellation keeps the identity until the home confirms cleanup;
    # a late publish records ownership without reopening acquisitions.
    def test_retire_on_allocating_waits_for_cancel_confirmation(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("obj-7", "idem-abd-1")

            retired = self._run_client(
                "retire-object", "--key", "obj-7", "--idempotency-key", "abd-7",
                "--connect", self._connect,
            )
            self.assertEqual(retired.returncode, 0, retired.stderr + retired.stdout)
            self.assertIn("state=retiring", retired.stdout)

            late_publish = self._publish("obj-7", generation)
            self.assertEqual(late_publish.returncode, 0, late_publish.stdout + late_publish.stderr)
            self.assertIn("state=retiring", late_publish.stdout)
            replay = self._publish("obj-7", generation)
            self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
            self.assertIn("state=retiring", replay.stdout)
            self.assertEqual(self._reclaim("obj-7", generation, 1).returncode, 0)
            self.assertEqual(self._inspect("obj-7").get("state"), "retired")
            self.assertIn("reason=state_conflict", self._publish("obj-7", generation).stdout)
            stats = self._stats()
            self.assertEqual(stats.get("reclaim_ok_count"), "1")
            self.assertEqual(stats.get("quarantined_objects"), "0")
        finally:
            self._stop_server(proc)

    def test_cancel_before_reservation_requires_explicit_home_ack(self):
        proc = self._start_home_daemon()
        try:
            self.assertEqual(self._register_home().returncode, 0)
            generation = self._allocate_bound("empty", "empty-allocate")
            for attempt in range(2):
                result = self._run_client("retire-object", "--key", "empty",
                    "--idempotency-key", f"empty-cancel-{attempt}", "--connect", self._connect)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("state=retiring", result.stdout)
            stats = self._stats()
            self.assertEqual(stats.get("backing_allocated_bytes"), "0")
            self.assertEqual(stats.get("address_reserved_bytes"), "0")
            self.assertEqual(stats.get("in_flight"), "1")
            self.assertEqual(self._reclaim("empty", generation, 1).returncode, 0)
            self.assertEqual(self._inspect("empty").get("state"), "retired")
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
