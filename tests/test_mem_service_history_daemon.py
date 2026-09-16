"""Real daemon/CLI processes; control lifecycle only, no guest payload mapping."""
import subprocess
import unittest

from tests import test_mem_service_object_session as fixtures


class HistoryDaemonTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.MemServiceObjectSessionTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.tearDown)
        self.store = self.fixture.root / "history.store"
        self.history = self.fixture.root / "history.store.replay-history"
        self.daemon = self.fixture._start_active_object(extra_config=f"store={self.store}")
        self.addCleanup(self.stop)

    def stop(self):
        if self.daemon is not None:
            result = self.fixture._stop_server(self.daemon)
            self.daemon = None
            return result

    def holder(self, action, nonce, success=True):
        result = self.fixture._run_client(
            f"{action}-object", "--key", "obj-1", "--session-id", "reader",
            "--expected-generation", "1", "--idempotency-key", nonce,
            "--connect", self.fixture._connect)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result.stdout

    def mapping(self, action, nonce, mapping_id=0):
        result = self.fixture._run_client(
            "mapping-transition", "--key", "obj-1", "--session-id", "reader",
            "--generation", "1", "--mapping-id", str(mapping_id),
            "--action", action, "--idempotency-key", nonce,
            "--connect", self.fixture._connect)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return fixtures._parse_kv(result.stdout)

    def cycles(self, count, mapping=False):
        first = None
        for index in range(count):
            if index % 5 == 0:
                self.fixture._register_home()
            self.holder("acquire", f"acquire-{index}")
            if mapping:
                begin = self.mapping("begin", f"begin-{index}")
                if first is None:
                    first = begin
                token = int(begin["mapping_id"])
                for action in ("confirm", "close", "finish"):
                    self.mapping(action, f"{action}-{index}", token)
            self.holder("release", f"release-{index}")
            stats = self.fixture._allocation_stats()
            self.assertEqual(stats["live_refs"], "0", stats)
            self.assertEqual(stats["import_mappings"], "0", stats)
            self.assertEqual(stats["idempotency_reservation_deficit"], "0", stats)
            self.assertEqual(stats["idempotency_capacity"], "64", stats)
        return first

    def assert_archived(self):
        stats = self.fixture._allocation_stats()
        self.assertEqual(stats["idempotency_history_enabled"], "1", stats)
        self.assertGreaterEqual(int(stats["idempotency_history_records"]), 60, stats)
        self.assertLess(int(stats["idempotency_used"]), 64, stats)
        self.assertEqual(stats["idempotency_history_failed"], "0", stats)
        self.assertEqual(self.store.read_text().splitlines()[0], "mem_service_store_managed_v2")
        self.assertIn("replay_history_enabled=1\n", self.store.read_text())
        return stats

    def test_hundred_mapping_lifecycles_keep_original_replies_and_cleanup(self):
        first = self.cycles(100, mapping=True)
        before = self.assert_archived()
        self.assertGreaterEqual(int(before["idempotency_history_records"]), 500)
        self.holder("acquire", "new-reader")
        self.assertEqual(self.mapping("begin", "begin-0"), first)
        # Historical BEGIN and ACQUIRE must not create a fresh mapping/holder.
        self.holder("acquire", "acquire-0")
        after = self.fixture._allocation_stats()
        self.assertEqual(after["import_mappings"], "0", after)
        self.assertEqual(after["live_refs"], "1", after)
        conflict = self.holder("release", "acquire-0", success=False)
        self.assertIn("status=version_conflict", conflict)
        self.holder("release", "new-reader-release")
        replay = self.fixture._run_session(self.fixture.root / "producer.conf")
        self.assertEqual(replay.returncode, 0, replay.stdout + replay.stderr)
        self.assertEqual(self.fixture._allocation_stats()["allocate_ok_count"], before["allocate_ok_count"])

    def test_restart_preserves_old_cleanup_reply_and_requires_resource_reconciliation(self):
        self.cycles(40)
        self.assert_archived()
        reply = self.holder("release", "release-0")
        self.stop()
        self.daemon = self.fixture._start_home_daemon(extra_config=f"store={self.store}")
        self.fixture._register_home()
        self.assertEqual(self.holder("release", "release-0"), reply)
        stats = self.fixture._allocation_stats()
        self.assertEqual(stats["managed_recovery_required"], "1", stats)
        status = self.fixture._run_client("status", "--connect", self.fixture._connect)
        self.assertIn("data_plane_ready=0", status.stdout)
        self.assertIn("managed_reconciliation_required",
                      self.holder("acquire", "fresh-after-restart", success=False))
        self.assertEqual(self.fixture._allocation_stats()["live_refs"], "0")

    def test_failed_checkpoint_does_not_evict_cache_or_acquire_a_holder(self):
        self.cycles(31)
        before = self.fixture._allocation_stats()
        self.assertEqual(before["idempotency_available"], "0", before)
        saved = self.store.with_suffix(".saved")
        self.store.rename(saved)
        self.store.mkdir()
        reply = self.holder("acquire", "after-checkpoint", success=False)
        self.assertIn("managed_store_write_uncertain", reply)
        after = self.fixture._allocation_stats()
        self.assertEqual(after["idempotency_used"], before["idempotency_used"])
        self.assertEqual(after["live_refs"], "0")
        self.store.rmdir()
        saved.rename(self.store)
        # Restoring the path cannot make an uncertain durable transition safe.
        self.assertIn("managed_store_write_uncertain",
                      self.holder("acquire", "after-checkpoint", success=False))
        self.assertEqual(self.fixture._allocation_stats()["live_refs"], "0")

    def drained_restart(self, archived):
        self.cycles(40 if archived else 1, mapping=True)
        if archived:
            self.assert_archived()
        cleanup_reply = self.holder("release", "release-0")

        def retire(generation, incarnation):
            config = self.fixture._write_session(
                "drain.conf", self.fixture._connect, [
                    f"retire key=obj-1 idempotency_key=drain-{generation} "
                    f"expected_generation={generation}",
                    f"reclaim key=obj-1 node_id={fixtures.HOME_NODE} "
                    f"incarnation={incarnation} generation={generation} confirmed=1",
                ])
            run = self.fixture._run_session(config)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            stats = self.fixture._allocation_stats()
            for field in ("live_objects", "live_refs", "in_flight", "import_mappings",
                          "backing_allocated_bytes", "address_reserved_bytes"):
                self.assertEqual(stats[field], "0", stats)

        def reject_old_grants():
            old_allocate = self.fixture._run_session(self.fixture.root / "producer.conf")
            self.assertNotEqual(old_allocate.returncode, 0, old_allocate.stdout)
            self.assertIn("status=version_conflict", old_allocate.stdout)
            old_acquire = self.holder("acquire", "acquire-0", success=False)
            self.assertIn("managed_generation_retired", old_acquire)
            self.assertEqual(self.holder("release", "release-0"), cleanup_reply)

        retire(1, fixtures.HOME_INCARNATION)
        reject_old_grants()
        for generation in (2, 3):
            self.stop()
            self.daemon = self.fixture._start_home_daemon(extra_config=f"store={self.store}")
            stats = self.fixture._allocation_stats()
            self.assertEqual(stats["managed_recovery_required"], "0", stats)
            not_ready = self.fixture._run_client("status", "--connect", self.fixture._connect)
            self.assertIn("data_plane_ready=0", not_ready.stdout)
            self.assertIn("provider_active_count=0", not_ready.stdout)
            incarnation = fixtures.HOME_INCARNATION + generation
            self.fixture._register_home(incarnation=incarnation)
            reject_old_grants()
            allocate = self.fixture._run_client(
                "allocate-object", "--key", "obj-1", "--session-id", "producer",
                "--size-bytes", "4096", "--capabilities", "1",
                "--idempotency-key", f"new-allocation-{generation}",
                "--connect", self.fixture._connect)
            self.assertEqual(allocate.returncode, 0, allocate.stdout + allocate.stderr)
            value = fixtures._parse_kv(allocate.stdout)
            self.assertEqual(value["generation"], str(generation), value)
            self.assertEqual(value["state"], "allocating", value)
            self.assertEqual(value["provider_incarnation"], str(incarnation), value)
            # Reusing the key must not make an archived generation current.
            reject_old_grants()
            retire(generation, incarnation)

    def test_drained_checkpoint_restarts_with_fresh_generations_and_rejects_old_cache_grants(self):
        self.drained_restart(archived=False)

    def test_drained_checkpoint_restarts_with_fresh_generations_and_rejects_archived_grants(self):
        self.drained_restart(archived=True)

    def test_reclaimed_checkpoint_does_not_require_live_resource_recovery(self):
        config = self.fixture._write_session("retired-only.conf", self.fixture._connect, [
            "retire key=obj-1 idempotency_key=retired-only expected_generation=1",
            f"reclaim key=obj-1 node_id={fixtures.HOME_NODE} "
            f"incarnation={fixtures.HOME_INCARNATION} generation=1 confirmed=1",
        ])
        run = self.fixture._run_session(config)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.stop()
        self.daemon = self.fixture._start_home_daemon(extra_config=f"store={self.store}")
        stats = self.fixture._allocation_stats()
        self.assertEqual(stats["managed_recovery_required"], "0", stats)
        self.assertEqual(stats["quarantined_objects"], "0", stats)
        self.assertEqual(stats["live_objects"], "0", stats)

    def assert_restart_rejected(self):
        self.stop()
        run = subprocess.run([
            str(self.fixture.binary), "serve", "--config",
            str(self.fixture.root / "daemon.conf"),
        ], capture_output=True, text=True, timeout=10)
        self.assertNotEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("store load failed", run.stderr)

    def test_corrupt_history_rejects_new_work_and_restart(self):
        self.cycles(40)
        self.assert_archived()
        data = bytearray(self.history.read_bytes())
        data[70] ^= 1
        self.history.write_bytes(data)
        self.assertIn("replay_history_unavailable",
                      self.holder("acquire", "after-corruption", success=False))
        self.assertEqual(self.fixture._allocation_stats()["live_refs"], "0")
        self.assert_restart_rejected()
        self.assertEqual(self.history.read_bytes(), data)

    def test_missing_history_never_becomes_an_empty_replay_namespace(self):
        self.cycles(40)
        self.assert_archived()
        self.history.rename(self.history.with_suffix(".saved"))
        self.assert_restart_rejected()
        self.assertFalse(self.history.exists())

    def test_snapshot_export_and_restore_preserve_archived_history(self):
        self.cycles(40)
        before = self.assert_archived()
        export = self.fixture._run_client("export-snapshot", "--connect", self.fixture._connect)
        self.assertNotEqual(export.returncode, 0)
        self.assertIn("external_replay_history_required", export.stdout)
        self.stop()
        self.daemon = self.fixture._start_home_daemon(extra_config=f"store={self.store}")
        # Restored quarantined allocations and replay history both retain
        # their identities; neither full nor paged restore may replace them.
        for padding in (0, 5000):
            snapshot = self.fixture._write_config(
                f"restore-{padding}.snapshot",
                "mem_service_store_v1\nstore_schema_version=1\nrecord_count=0\n" + "\n" * padding)
            restore = self.fixture._run_client(
                "restore-snapshot", "--from", str(snapshot), "--connect", self.fixture._connect)
            self.assertNotEqual(restore.returncode, 0, restore.stdout + restore.stderr)
            self.assertIn("managed_history_in_use", restore.stdout)
        self.assertEqual(self.fixture._allocation_stats()["idempotency_history_records"],
                         before["idempotency_history_records"])


if __name__ == "__main__":
    unittest.main()
