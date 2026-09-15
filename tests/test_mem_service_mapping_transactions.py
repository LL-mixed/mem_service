"""Managed mapping control transactions; actual provider mapping is tested separately."""
import shutil
import unittest

from tests import test_mem_service_object_session as session_fixtures


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class MemServiceMappingTransactionTests(unittest.TestCase):
    def setUp(self):
        self.fixture = session_fixtures.MemServiceObjectSessionTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.daemon = self.fixture._start_active_object(extra_config="record_retention=latest:1")
        self.addCleanup(self.fixture._stop_server, self.daemon)
        self.serial = 0
        self._holder("acquire", "owner")

    def _nonce(self):
        self.serial += 1
        return f"mapping-test-{self.serial}"

    def _holder(self, action, owner, success=True):
        result = self.fixture._run_client(
            f"{action}-object", "--key", "obj-1", "--session-id", owner,
            "--expected-generation", "1", "--idempotency-key", self._nonce(),
            "--connect", self.fixture._connect)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def _mapping(self, action, mapping_id=0, owner="owner", generation=1,
                 nonce=None, success=True):
        result = self.fixture._run_client(
            "mapping-transition", "--key", "obj-1", "--session-id", owner,
            "--generation", str(generation), "--mapping-id", str(mapping_id),
            "--action", action, "--idempotency-key", nonce or self._nonce(),
            "--connect", self.fixture._connect)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return session_fixtures._parse_kv(result.stdout)

    def _counts(self, imports, inflight, refs=1):
        stats = self.fixture._allocation_stats()
        self.assertEqual(int(stats["import_mappings"]), imports, stats)
        self.assertEqual(int(stats["in_flight"]), inflight, stats)
        self.assertEqual(int(stats["live_refs"]), refs, stats)

    def _exhaust_unreserved_records(self):
        for attempt in range(80):
            result = self.fixture._run_client(
                "retire-object", "--key", "missing-capacity-object",
                "--expected-generation", "1", "--idempotency-key", self._nonce(),
                "--connect", self.fixture._connect)
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            reply = session_fixtures._parse_kv(result.stdout)
            if reply.get("status") == "capacity_exceeded":
                return
            self.assertEqual(reply.get("status"), "not_found", reply)
        self.fail("bounded request sequence never reached idempotency capacity")

    def _capacity_cleanup(self, state):
        nonce = self._nonce()
        first = self._mapping("begin", nonce=nonce)
        token = int(first["mapping_id"])
        if state not in ("pending", "pending_confirm"):
            self._mapping("confirm", token)
        if state == "closing":
            self._mapping("close", token)
        self._exhaust_unreserved_records()
        self._mapping("begin", success=False)
        # Invalid cleanup must not steal the slot reserved for its owner.
        self._mapping("finish", token, owner="missing", success=False)
        if state == "pending":
            self._mapping("cancel", token)
        else:
            if state == "pending_confirm":
                self._mapping("confirm", token)
            if state in ("active", "pending_confirm"):
                self._mapping("close", token)
            self._mapping("finish", token)
        self.assertEqual(self._mapping("begin", nonce=nonce), first)
        self._mapping("inspect", token, success=False)
        self._counts(0, 0)
        self._holder("release", "owner")
        retired = self.fixture._run_client(
            "retire-object", "--key", "obj-1", "--expected-generation", "1",
            "--idempotency-key", self._nonce(), "--connect", self.fixture._connect)
        self.assertEqual(retired.returncode, 0, retired.stdout + retired.stderr)
        config = self.fixture._write_session(
            "capacity-reclaim.conf", self.fixture._connect,
            [f"reclaim key=obj-1 node_id={session_fixtures.HOME_NODE} "
             f"incarnation={session_fixtures.HOME_INCARNATION} "
             "generation=1 confirmed=1"],
        )
        reclaimed = self.fixture._run_session(config)
        self.assertEqual(reclaimed.returncode, 0, reclaimed.stdout + reclaimed.stderr)
        self._counts(0, 0, 0)
        stats = self.fixture._allocation_stats()
        for field in ("live_objects", "backing_allocated_bytes", "address_reserved_bytes",
                      "idempotency_cleanup_reserved", "idempotency_reservation_deficit"):
            self.assertEqual(stats[field], "0", stats)
        self.assertLessEqual(int(stats["idempotency_used"]),
                             int(stats["idempotency_capacity"]), stats)

    def test_pending_mapping_can_cancel_at_idempotency_capacity(self):
        self._capacity_cleanup("pending")

    def test_active_mapping_can_unmap_at_idempotency_capacity(self):
        self._capacity_cleanup("active")

    def test_closing_mapping_can_finish_at_idempotency_capacity(self):
        self._capacity_cleanup("closing")

    def test_pending_mapping_can_confirm_and_finish_at_capacity(self):
        self._capacity_cleanup("pending_confirm")

    def test_snapshot_restore_preserves_managed_replay_history(self):
        nonce = self._nonce()
        first = self._mapping("begin", nonce=nonce)
        token = int(first["mapping_id"])
        self._mapping("cancel", token)
        for padding in (0, 5000):
            with self.subTest(paged=bool(padding)):
                # An empty metadata snapshot is valid for the legacy store.
                # Padding exercises the CLI's paged restore path as well.
                snapshot = self.fixture._write_config(
                    f"capacity-restore-{padding}.snapshot",
                    "mem_service_store_v1\nstore_schema_version=1\nrecord_count=0\n" +
                    "\n" * padding,
                )
                result = self.fixture._run_client(
                    "restore-snapshot", "--from", str(snapshot),
                    "--connect", self.fixture._connect,
                )
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("reason=managed_history_in_use", result.stdout)
                self.assertEqual(self._mapping("begin", nonce=nonce), first)
                self._mapping("inspect", token, success=False)
                self._counts(0, 0)

    def test_record_retention_preserves_managed_replay_for_same_key(self):
        nonce = self._nonce()
        first = self._mapping("begin", nonce=nonce)
        token = int(first["mapping_id"])
        self._mapping("cancel", token)
        for index, key in enumerate(("obj-1", "metadata-only-object"), start=1):
            result = self.fixture._run_client(
                "put-object", "--key", key, "--version", str(index),
                "--checksum", "123", "--backing-len", "16",
                "--idempotency-key", f"metadata-{index}",
                "--connect", self.fixture._connect,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        missing = self.fixture._run_client("get-object", "--key", "obj-1",
                                           "--connect", self.fixture._connect)
        self.assertNotEqual(missing.returncode, 0, missing.stdout + missing.stderr)
        self.assertIn("status=not_found", missing.stdout)
        self.assertEqual(self._mapping("begin", nonce=nonce), first)
        self._mapping("inspect", token, success=False)
        self._counts(0, 0)

    def test_pending_active_closing_all_block_holder_release(self):
        mapping = self._mapping("begin")
        token = int(mapping["mapping_id"])
        self._counts(0, 1)
        self._holder("release", "owner", success=False)
        self._mapping("finish", token, success=False)
        self._mapping("confirm", token)
        self._counts(1, 0)
        self._holder("release", "owner", success=False)
        self._mapping("cancel", token, success=False)
        self._mapping("close", token)
        self._counts(1, 1)
        self._holder("release", "owner", success=False)
        self._mapping("finish", token)
        self._counts(0, 0)
        self._holder("release", "owner")
        self._counts(0, 0, 0)

    def test_pending_cancel_and_old_replays_do_not_recreate_mapping(self):
        nonce = self._nonce()
        first = self._mapping("begin", nonce=nonce)
        token = int(first["mapping_id"])
        self.assertEqual(self._mapping("begin", nonce=nonce), first)
        self._counts(0, 1)
        self._mapping("cancel", token)
        self._counts(0, 0)
        self.assertEqual(self._mapping("begin", nonce=nonce), first)
        self._mapping("inspect", token, success=False)
        self._mapping("confirm", token, success=False)
        second = int(self._mapping("begin")["mapping_id"])
        self.assertGreater(second, token)
        self._mapping("finish", token, success=False)
        self._counts(0, 1)
        self._mapping("cancel", second)
        self._holder("release", "owner")

    def test_owner_generation_and_multiple_mapping_isolation(self):
        self._holder("acquire", "other")
        first = int(self._mapping("begin")["mapping_id"])
        second = int(self._mapping("begin")["mapping_id"])
        self._mapping("confirm", first, owner="other", success=False)
        self._mapping("confirm", first, generation=2, success=False)
        self._mapping("confirm", first, owner="missing", success=False)
        self._holder("release", "other")
        self._mapping("confirm", first)
        self._counts(1, 1)
        self._mapping("cancel", second)
        self._counts(1, 0)
        self._holder("release", "owner", success=False)
        self._mapping("close", first)
        self._mapping("finish", first)
        self._holder("release", "owner")

    def test_retire_rejects_new_and_late_mapping_confirmation(self):
        token = int(self._mapping("begin")["mapping_id"])
        retired = self.fixture._run_client(
            "retire-object", "--key", "obj-1", "--expected-generation", "1",
            "--idempotency-key", self._nonce(), "--connect", self.fixture._connect)
        self.assertEqual(retired.returncode, 0, retired.stdout + retired.stderr)
        self._mapping("begin", success=False)
        self._mapping("confirm", token, success=False)
        self._holder("release", "owner", success=False)
        self._counts(0, 2)
        self._mapping("cancel", token)
        self._holder("release", "owner")
        self._counts(0, 1, 0)

    def test_readiness_loss_blocks_admission_but_allows_teardown(self):
        token = int(self._mapping("begin")["mapping_id"])
        self._mapping("confirm", token)
        removed = self.fixture._run_client(
            "provider-deregister", "--node-id", session_fixtures.HOME_NODE,
            "--incarnation", str(session_fixtures.HOME_INCARNATION),
            "--connect", self.fixture._connect)
        self.assertEqual(removed.returncode, 0, removed.stdout + removed.stderr)
        nonce = self._nonce()
        self._mapping("begin", nonce=nonce, success=False)
        self.assertEqual(self._mapping("inspect", token)["mapping_state"], "2")
        self._mapping("close", token)
        self._mapping("finish", token)
        self._counts(0, 0)
        self.fixture._register_home()
        self._mapping("begin", nonce=nonce, success=False)
        self._mapping("begin", success=False)
        self.assertEqual(self.fixture._allocation_stats()["managed_recovery_required"], "1")
        self._holder("release", "owner")

    def test_release_manifest_lists_every_schema_operation(self):
        release = self.fixture._run_client("release-manifest")
        schema = self.fixture._run_client("wire-schema")
        self.assertEqual(release.returncode, 0, release.stdout + release.stderr)
        self.assertEqual(schema.returncode, 0, schema.stdout + schema.stderr)
        declared = [line.split()[0] for line in schema.stdout.splitlines()
                    if line.startswith("operation=")]
        exported = [line for line in release.stdout.splitlines()
                    if line.startswith("operation=")]
        self.assertEqual(len(exported), len(set(exported)))
        self.assertEqual(set(exported), set(declared))

    def test_replaced_home_rejects_old_allocation_mapping_admission(self):
        nonce = self._nonce()
        token = int(self._mapping("begin", nonce=nonce)["mapping_id"])
        self.fixture._register_home(incarnation=session_fixtures.HOME_INCARNATION + 1)
        self._mapping("begin", success=False)
        self._mapping("begin", nonce=nonce, success=False)
        self._mapping("confirm", token, success=False)
        self.assertEqual(self._mapping("inspect", token)["mapping_state"], "1")
        self._mapping("cancel", token)
        self._counts(0, 0)


if __name__ == "__main__":
    unittest.main()
