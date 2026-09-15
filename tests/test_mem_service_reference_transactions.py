"""Real CLI/SDK/daemon reference lifecycles; loopback payloads are synthetic."""
import shutil
import socket
import struct
import subprocess
import time
import unittest

from tests import test_mem_service_object_session as fixtures

ALLOCATION_BYTES = 8192


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class ReferenceTransactionTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.MemServiceObjectSessionTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.daemon = self.fixture._start_active_object(logical_size=ALLOCATION_BYTES,
            extra_config="record_retention=latest:1")
        self.addCleanup(lambda: self.fixture._stop_server(self.daemon))
        self.serial = 0
        self.connect = self.fixture._connect
        self._holder("acquire", "producer")

    def _nonce(self):
        self.serial += 1
        return f"reference-{self.serial}"

    def test_restart_preserves_full_v2_view_without_reactivating_payload(self):
        store = self.fixture.root / "references.store"
        self.fixture._stop_server(self.daemon)
        self.daemon = self.fixture._start_active_object(logical_size=ALLOCATION_BYTES,
            extra_config=f"store={store}")
        self._holder("acquire", "producer")
        self._publish()
        expected = self._reference()

        def saved_reference():
            self.assertIn("response_line=reference_hex=" + expected + "\n",
                          store.read_text())
            fields = fixtures._parse_kv(store.read_text())
            return fields["managed_reference_part0"] + fields["managed_reference_part1"]

        self.assertEqual(saved_reference(), expected)
        self.fixture._stop_server(self.daemon)
        self.daemon = self.fixture._start_home_daemon(extra_config=f"store={store}")
        self._holder("release", "producer")  # rewrites the restored checkpoint
        self.assertEqual(saved_reference(), expected)
        self.fixture._stop_server(self.daemon)
        self.daemon = self.fixture._start_home_daemon(extra_config=f"store={store}")
        self.assertEqual(self.fixture._allocation_stats()["quarantined_objects"], "1")
        self.assertEqual(self.fixture._allocation_stats()["live_refs"], "0")
        self._transition("resolve", success=False)
        self.fixture._stop_server(self.daemon)
        damaged = store.read_text().replace("managed_reference_part0=", "managed_reference_part0=f", 1)
        store.write_text(damaged)
        run = subprocess.run(
            [str(self.fixture.binary), "serve", "--config", str(self.fixture.root / "daemon.conf")],
            capture_output=True, text=True, timeout=10)
        self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("store load failed", run.stderr)
        self.assertEqual(store.read_text(), damaged)

    def test_drained_restart_keeps_historical_view_but_does_not_restore_its_payload(self):
        store = self.fixture.root / "drained-references.store"
        self.fixture._stop_server(self.daemon)
        self.daemon = self.fixture._start_active_object(logical_size=ALLOCATION_BYTES,
            extra_config=f"store={store}")
        self._holder("acquire", "producer")
        self._publish()
        expected = self._reference()
        self._holder("release", "producer")
        config = self.fixture._write_session("drain-reference.conf", self.connect, [
            "retire key=obj-1 idempotency_key=drain-reference expected_generation=1",
            f"reclaim key=obj-1 node_id={fixtures.HOME_NODE} "
            f"incarnation={fixtures.HOME_INCARNATION} generation=1 confirmed=1",
        ])
        run = self.fixture._run_session(config)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        for attempt in range(2):
            self.fixture._stop_server(self.daemon)
            self.daemon = self.fixture._start_home_daemon(extra_config=f"store={store}")
            stats = self.fixture._allocation_stats()
            self.assertEqual(stats["managed_recovery_required"], "0", stats)
            self.assertEqual(stats["live_objects"], "0", stats)
            self.fixture._register_home(incarnation=fixtures.HOME_INCARNATION + attempt + 1)
            self._transition("resolve", success=False)
            saved = fixtures._parse_kv(store.read_text())
            self.assertEqual(saved["managed_reference_part0"] +
                             saved["managed_reference_part1"], expected)

    def _holder(self, action, session):
        result = self.fixture._run_client(
            f"{action}-object", "--key", "obj-1", "--session-id", session,
            "--expected-generation", "1", "--idempotency-key", self._nonce(),
            "--connect", self.connect)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def _reference(self, version=2, checksum=42, **changes):
        values = dict(key=b"obj-1", home=fixtures.HOME_NODE.encode(), generation=1,
                      incarnation=fixtures.HOME_INCARNATION, size=ALLOCATION_BYTES, access=1,
                      offset=128, length=512, kind=5, owner=1, producer=2)
        values.update(changes)
        v = values
        return struct.pack(
            "<QHHHHIIQQQQQIIQQQ96s64s", 0x514F424D4D524546,
            2, v["kind"], 2, 0, v["owner"], v["producer"], version,
            fixtures._fnv1a64(v["key"]), v["offset"], v["length"], checksum,
            256, v["access"], v["generation"], v["incarnation"], v["size"],
            v["key"], v["home"]).hex()

    def _transition(self, action, key=None, version=2, reference=None,
                    session="producer", nonce=None, success=True, extra=()):
        key = key or ("logical/a" if action in ("stage", "resolve") else "obj-1")
        args = ["reference-transition", "--action", action, "--key", key]
        if action != "resolve":
            args += ["--session-id", session, "--idempotency-key", nonce or self._nonce()]
        if action in ("begin", "seal"):
            args += ["--generation", "1", "--version", str(version)]
        if action in ("stage", "acquire", "map-begin"):
            args += ["--reference-hex", reference or self._reference(version)]
        if action in ("acquire", "map-begin"):
            args += ["--access", "1"]
        result = self.fixture._run_client(*args, *extra, "--connect", self.connect)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return fixtures._parse_kv(result.stdout)

    def _publish(self):
        self._transition("begin", version=1)
        self._transition("stage")
        self._transition("seal")

    def _mapping(self, action, mapping_id, session="reader"):
        result = self.fixture._run_client(
            "mapping-transition", "--action", action, "--mapping-id", str(mapping_id),
            "--key", "obj-1", "--session-id", session, "--generation", "1",
            "--idempotency-key", self._nonce(), "--connect", self.connect)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return fixtures._parse_kv(result.stdout)

    def test_reference_mapping_requires_holder_and_pending_replay(self):
        self._publish()
        self._transition("map-begin", session="reader", success=False)
        self._transition("acquire", session="reader")
        for mutation in (dict(generation=2), dict(incarnation=8), dict(home=b"other"),
                         dict(size=16384), dict(offset=129), dict(length=511),
                         dict(kind=6), dict(owner=3), dict(producer=4), dict(access=3)):
            self._transition("map-begin", session="reader", reference=self._reference(**mutation),
                             success=False)
        nonce = self._nonce()
        pending = self._transition("map-begin", session="reader", nonce=nonce)
        mapping_id = pending["mapping_id"]
        self.assertEqual(pending["mapping_state"], "1")
        self.assertEqual(self._transition("map-begin", session="reader", nonce=nonce), pending)
        self.assertEqual(self.fixture._allocation_stats()["in_flight"], "1")
        release = self.fixture._run_client(
            "release-object", "--key", "obj-1", "--session-id", "reader",
            "--expected-generation", "1", "--idempotency-key", self._nonce(),
            "--connect", self.connect)
        self.assertNotEqual(release.returncode, 0)
        self._mapping("confirm", mapping_id)
        self._transition("map-begin", session="reader", nonce=nonce, success=False)
        self._mapping("close", mapping_id)
        self._mapping("finish", mapping_id)
        self._transition("map-begin", session="reader", nonce=nonce, success=False)
        self._holder("release", "reader")
        self.assertEqual(self.fixture._allocation_stats()["import_mappings"], "0")

    def test_reference_mapping_pins_version_and_reserves_cleanup_capacity(self):
        self._publish()
        nonce = self._nonce()
        pending = self._transition("map-begin", nonce=nonce)
        mapping_id = pending["mapping_id"]
        self._transition("begin", version=2, success=False)
        removed = self.fixture._run_client(
            "provider-deregister", "--node-id", fixtures.HOME_NODE,
            "--incarnation", str(fixtures.HOME_INCARNATION), "--connect", self.connect)
        self.assertEqual(removed.returncode, 0, removed.stdout + removed.stderr)
        self._transition("map-begin", nonce=nonce, success=False)
        self._mapping("cancel", mapping_id, session="producer")
        self.fixture._register_home()
        self._transition("map-begin", nonce=nonce, success=False)
        self._transition("map-begin", success=False)
        self._holder("release", "producer")
        self.assertEqual(self.fixture._allocation_stats()["managed_recovery_required"], "1")
        self.assertEqual(self.fixture._allocation_stats()["idempotency_reservation_deficit"], "0")

    def test_reference_mapping_retains_cleanup_capacity_without_provider_loss(self):
        self._publish()
        pending = self._transition("map-begin")
        for _ in range(80):
            result = self.fixture._run_client(
                "retire-object", "--key", "missing", "--expected-generation", "1",
                "--idempotency-key", self._nonce(), "--connect", self.connect)
            if "status=capacity_exceeded" in result.stdout:
                break
        else:
            self.fail("expected bounded idempotency capacity")
        self._transition("map-begin", success=False)
        for action in ("confirm", "close", "finish"):
            self._mapping(action, pending["mapping_id"], session="producer")
        self._holder("release", "producer")
        self.assertEqual(self.fixture._allocation_stats()["idempotency_reservation_deficit"], "0")

    def test_session_maps_registered_subrange_readonly(self):
        checksum = fixtures._fnv1a64(bytes(512))
        self._transition("begin", version=1)
        self._transition("stage", reference=self._reference(checksum=checksum))
        self._transition("stage", key="logical/b",
                         reference=self._reference(checksum=checksum, offset=4224))
        self._transition("seal")
        config = self.fixture._write_session(
            "reference-reader.conf", self.connect, [
                "acquire_reference key=logical/a idempotency_key=reader-acquire expected_generation=1",
                "map_reference key=obj-1",
                f"wait_visible key=obj-1 offset=0 len=512 expect_checksum={checksum}",
                f"read key=obj-1 offset=0 len=512 expect_checksum={checksum}",
                "read key=obj-1 offset=512 len=1 seed=0 expect_status=capacity_exceeded",
                "write key=obj-1 offset=0 len=1 seed=7 expect_status=unsupported",
                "probe_readonly key=obj-1",
                "unmap key=obj-1",
                "release key=obj-1 idempotency_key=reader-release expected_generation=1",
                "acquire_reference key=logical/b idempotency_key=reader-acquire-b expected_generation=1",
                "map_reference key=obj-1",
                f"wait_visible key=obj-1 offset=0 len=512 expect_checksum={checksum}",
                f"read key=obj-1 offset=0 len=512 expect_checksum={checksum}",
                "probe_readonly key=obj-1",
                "unmap key=obj-1",
                "release key=obj-1 idempotency_key=reader-release-b expected_generation=1",
            ], session_id="reader", header_extra="provider=session-loopback")
        result = self.fixture._run_session(config)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("action=map_reference key=obj-1 status=ok", result.stdout)
        self.assertIn(f"base=0x{fixtures.DATA_MAP_ADDRESS + 128:016x} len=512", result.stdout)
        self.assertIn(f"base=0x{fixtures.DATA_MAP_ADDRESS + 4224:016x} len=512", result.stdout)
        self.assertIn("len=512", result.stdout)
        self.assertIn("cpu_fault_probe=probe_readonly", result.stdout)
        stats = self.fixture._allocation_stats()
        self.assertEqual(stats["live_refs"], "1")
        self.assertEqual(stats["import_mappings"], "0")
        self.assertEqual(stats["in_flight"], "0")

    def test_session_publishes_reference_from_written_bytes(self):
        self._holder("release", "producer")
        publish = "publish_reference key=logical/a idempotency_key=writer-stage offset=128 len=512 kind=5 owner=1 producer=2"
        config = self.fixture._write_session(
            "reference-writer.conf", self.connect, [
                "acquire key=obj-1 idempotency_key=writer-acquire expected_generation=1",
                "begin_reference key=obj-1 idempotency_key=writer-begin generation=1 version=1",
                "map key=obj-1 flags=readwrite",
                "write key=obj-1 offset=128 len=512 seed=17",
                "write key=obj-1 offset=4224 len=512 seed=39",
                publish,
                publish,
                publish.replace("len=512", "len=511") + " expect_status=version_conflict",
                "write key=obj-1 offset=128 len=1 seed=99 expect_status=unsupported",
                "begin_reference key=obj-1 idempotency_key=writer-begin generation=1 version=1",
                "write key=obj-1 offset=128 len=1 seed=99 expect_status=unsupported",
                "publish_reference key=logical/b idempotency_key=writer-stage-b offset=4224 len=512 kind=5 owner=1 producer=2",
                "read key=obj-1 offset=128 len=512 seed=17",
                "read key=obj-1 offset=4224 len=512 seed=39",
                "unmap key=obj-1",
                "seal_reference key=obj-1 idempotency_key=writer-seal generation=1 version=2",
                "release key=obj-1 idempotency_key=writer-release expected_generation=1",
            ], session_id="producer", header_extra="provider=session-loopback")
        result = self.fixture._run_session(config)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name, seed, offset in (("logical/a", 17, 128), ("logical/b", 39, 4224)):
            expected = self._reference(checksum=fixtures._fnv1a64(fixtures._pattern(seed, 512)),
                                       offset=offset)
            self.assertIn(f"reference_key={name} reference_hex={expected}", result.stdout)
            self.assertEqual(self._transition("resolve", key=name)["reference_hex"], expected)
        self.assertIn("reference_payload_frozen", result.stdout)
        stats = self.fixture._allocation_stats()
        for field in ("live_refs", "import_mappings", "in_flight"):
            self.assertEqual(stats[field], "0")

    def test_publication_roundtrip_and_old_replay_rejection(self):
        nonce = self._nonce()
        first = self._transition("begin", version=1, nonce=nonce)
        self.assertEqual(first["version"], "2")
        self.assertEqual(self._transition("begin", version=1, nonce=nonce), first)
        self._transition("stage", reference=self._reference().upper())
        self._transition("stage", key="logical/b", reference=self._reference(checksum=43))
        self._transition("resolve", success=False)
        self._transition("seal")
        self.assertEqual(self._transition("resolve")["reference_hex"], self._reference())
        self.assertEqual(self._transition("resolve", key="logical/b")["reference_hex"],
                         self._reference(checksum=43))
        self._transition("begin", version=1, nonce=nonce, success=False)
        grant = self._nonce()
        acquired = self._transition("acquire", session="reader", nonce=grant)
        self.assertEqual(self._transition("acquire", session="reader", nonce=grant), acquired)
        self.assertEqual(self.fixture._allocation_stats()["live_refs"], "2")
        self._transition("begin", version=2, success=False)
        self._holder("release", "reader")
        self._transition("acquire", session="reader", nonce=grant, success=False)
        self.assertEqual(self.fixture._allocation_stats()["live_refs"], "1")
        self._transition("begin", version=2)
        self._transition("resolve", success=False)
        self._transition("acquire", session="reader", success=False)
        self._transition("stage", version=3)
        self._transition("seal", version=3)
        self.assertEqual(self._transition("resolve")["version"], "3")

    def test_identity_matrix_and_home_readiness_precede_replay(self):
        self._publish()
        for mutation in (dict(generation=2), dict(incarnation=8), dict(home=b"other"),
                         dict(size=16384), dict(offset=129), dict(length=511),
                         dict(kind=6), dict(owner=3), dict(producer=4), dict(access=3)):
            with self.subTest(mutation=mutation):
                self._transition("acquire", reference=self._reference(**mutation),
                                 session="reader", success=False)
        self._transition("acquire", reference=self._reference(checksum=43), success=False)
        nonce = self._nonce()
        self._transition("acquire", session="reader", nonce=nonce)
        removed = self.fixture._run_client(
            "provider-deregister", "--node-id", fixtures.HOME_NODE,
            "--incarnation", str(fixtures.HOME_INCARNATION), "--connect", self.fixture._connect)
        self.assertEqual(removed.returncode, 0, removed.stdout + removed.stderr)
        self._transition("acquire", session="reader", nonce=nonce, success=False)
        retry = self._nonce()
        self._transition("acquire", session="reader2", nonce=retry, success=False)
        self._transition("resolve", success=False)
        self.fixture._register_home()
        self._transition("acquire", session="reader", nonce=nonce, success=False)
        self._transition("acquire", session="reader2", nonce=retry, success=False)
        self.assertEqual(self.fixture._allocation_stats()["live_refs"], "2")
        self.assertEqual(self.fixture._allocation_stats()["managed_recovery_required"], "1")
        self.fixture._register_home(incarnation=fixtures.HOME_INCARNATION + 1)
        self._transition("resolve", success=False)
        self._transition("acquire", session="reader", nonce=nonce, success=False)

    def test_first_stage_and_seal_keep_reserved_capacity(self):
        self._transition("begin", version=1)
        for _ in range(80):
            result = self.fixture._run_client(
                "retire-object", "--key", "missing", "--expected-generation", "1",
                "--idempotency-key", self._nonce(), "--connect", self.fixture._connect)
            self.assertNotEqual(result.returncode, 0)
            if "status=capacity_exceeded" in result.stdout:
                break
        else:
            self.fail("idempotency capacity was never reached")
        self._transition("seal", success=False)
        self._transition("stage", session="other", success=False)
        self._transition("stage")
        self._transition("stage", key="logical/b", success=False)
        self._transition("stage", reference=self._reference(checksum=99), success=False)
        self._transition("seal")
        self._transition("resolve")
        self._holder("release", "producer")
        result = self.fixture._run_client(
            "retire-object", "--key", "obj-1", "--expected-generation", "1",
            "--idempotency-key", self._nonce(), "--connect", self.fixture._connect)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        stats = self.fixture._allocation_stats()
        self.assertEqual(stats["idempotency_cleanup_reserved"], "0", stats)
        self.assertEqual(stats["idempotency_reservation_deficit"], "0", stats)

    def test_legacy_overwrite_and_retention_cannot_remove_reference(self):
        self._publish()
        rejected = self.fixture._run_client(
            "put-object", "--key", "logical/a", "--version", "3", "--checksum", "9",
            "--backing-len", "16", "--connect", self.fixture._connect)
        self.assertNotEqual(rejected.returncode, 0, rejected.stdout + rejected.stderr)
        for index in range(3):
            result = self.fixture._run_client(
                "put-object", "--key", f"legacy-{index}", "--version", "1", "--checksum", "9",
                "--backing-len", "16", "--connect", self.fixture._connect)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self._transition("resolve")["reference_hex"], self._reference())

    def test_cli_rejects_invalid_fields_before_dispatch(self):
        for extra in (("--unknown", "1"), ("--version", "2"),
                      ("--key", "duplicate"), ("--access", "1"),
                      ("--reference-hex", self._reference()), ("unexpected",)):
            with self.subTest(extra=extra):
                self._transition("resolve", extra=extra, success=False)
        for value in ("-1", "+1", " 1", "18446744073709551616", "1\naccess=1"):
            self._transition("begin", version=value, success=False)
        self._transition("begin", version=1)
        self._transition("stage", extra=("--generation", "1"), success=False)
        self._transition("stage")
        self._transition("seal")

    def test_mapping_quiescence_blocks_seal_and_unbound_reader_mapping(self):
        self._transition("begin", version=1)
        self._transition("stage")
        mapping_id = "0"
        for action in ("begin", "confirm", "close", "finish"):
            result = self.fixture._run_client(
                "mapping-transition", "--action", action, "--key", "obj-1",
                "--session-id", "producer", "--generation", "1", "--mapping-id", mapping_id,
                "--idempotency-key", self._nonce(), "--connect", self.fixture._connect)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            mapping_id = fixtures._parse_kv(result.stdout)["mapping_id"]
            if action != "finish":
                self._transition("seal", success=False)
        self._transition("seal")
        self._transition("acquire", session="reader")
        result = self.fixture._run_client(
            "mapping-transition", "--action", "begin", "--key", "obj-1",
            "--session-id", "reader", "--generation", "1", "--mapping-id", "0",
            "--idempotency-key", self._nonce(), "--connect", self.fixture._connect)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self._holder("release", "reader")

    def test_reference_roundtrip_on_trusted_network_control_endpoint(self):
        port = fixtures._free_tcp_port()
        self.connect = f"tcp:127.0.0.1:{port}"
        config = self.fixture._write_config("reference-network.conf", "\n".join([
            f"listen={self.connect}", "auth_mode=trusted-guest-network",
            f"node_id={fixtures.HOME_NODE}", f"network_peer={fixtures.HOME_NODE}@127.0.0.1",
            f"required_provider={fixtures.HOME_NODE}", "provider_lease_ms=30000",
            f"allocation_home_provider={fixtures.HOME_NODE}", "",
        ]))
        daemon = subprocess.Popen(
            [str(self.fixture.binary), "serve", "--config", str(config)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.fixture._stop_server, daemon)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.assertIsNone(daemon.poll(), "network test daemon exited before readiness")
            health = self.fixture._run_client("health", "--connect", self.connect)
            if health.returncode == 0:
                break
            time.sleep(0.05)
        else:
            self.fail("network control endpoint did not become ready")
        self.fixture._register_home(connect=self.connect)
        result = self.fixture._run_client(
            "allocate-object", "--key", "obj-1", "--session-id", "producer",
            "--size-bytes", str(ALLOCATION_BYTES), "--capabilities", "1",
            "--idempotency-key", self._nonce(),
            "--connect", self.connect)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.fixture._run_client(
            "publish-allocation", "--key", "obj-1", "--node-id", fixtures.HOME_NODE,
            "--incarnation", "7", "--generation", "1", "--descriptor-hex", "deadbeef",
            "--address", fixtures.DATA_MAP_ADDRESS_DEC, "--address-len", str(fixtures.DATA_MAP_LEN),
            "--connect", self.connect)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self._holder("acquire", "producer")
        self._publish()
        self.assertEqual(self._transition("resolve")["reference_hex"], self._reference())
        self._transition("acquire", session="reader")
        self._holder("release", "reader")
        self._holder("release", "producer")

    def test_wire_embedded_nul_cannot_hide_duplicate_fields(self):
        nonce = self._nonce()
        valid = ("action=1\nkey=obj-1\nsession_id=producer\ngeneration=1\nversion=1\n"
                 f"idempotency_key={nonce}\n").encode()
        payload = valid + b"\0generation=2\n"
        checksum = 2166136261
        for byte in payload:
            checksum = ((checksum ^ byte) * 16777619) & 0xFFFFFFFF
        header = struct.pack("<IHHQIIIIIIQ", 0x4D535643, 1, 48, 1, 0x7e, 0,
                             len(payload), checksum, 0, 0, 0)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
            connection.settimeout(5)
            connection.connect(str(self.fixture.socket))
            connection.sendall(header + payload)
            response = b""
            while len(response) < 48:
                chunk = connection.recv(48 - len(response))
                self.assertTrue(chunk, "truncated wire response")
                response += chunk
            decoded = struct.unpack("<IHHQIIIIIIQ", response)
            self.assertEqual(decoded[8], 6, decoded)
            body = b""
            while len(body) < decoded[6]:
                chunk = connection.recv(decoded[6] - len(body))
                self.assertTrue(chunk, "truncated response payload")
                body += chunk
            self.assertIn(b"reason=embedded_nul\n", body)
        # The rejected frame neither starts publication nor records the nonce.
        self._transition("begin", version=1, nonce=nonce)
        self._transition("stage")
        self._transition("seal")


if __name__ == "__main__":
    unittest.main()
