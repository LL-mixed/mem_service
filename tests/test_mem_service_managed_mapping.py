import subprocess
from pathlib import Path
import platform
import shutil
import tempfile
import time
import unittest

from tests import test_mem_service_object_session as session_fixture


class MemServiceManagedMappingFaultTests(unittest.TestCase):
    def test_sdk_reconciles_lost_responses_and_provider_cleanup_failures(self):
        root = Path(__file__).resolve().parents[1]
        component = root / "components" / "mem_service"
        native = platform.system() == "Linux"
        compiler = shutil.which("cc" if native else "aarch64-linux-gnu-gcc")
        if compiler is None:
            self.skipTest("requires native Linux or an AArch64 cross compiler")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "managed-mapping"
            result = subprocess.run(
                [compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(root),
                 str(root / "tests" / "mem_service_managed_mapping.c"),
                 str(component / "mem_service_client.c"),
                 str(component / "mem_service_wire_client.c"),
                 str(component / "mem_service_provider.c"),
                 "-Wl,--wrap=mem_service_send_request_with_options", "-o", str(binary)],
                capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            if not native:
                return
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("managed_mapping_faults=pass scenarios=11", result.stdout)
            self.assertIn("reference_mapping_faults=pass scenarios=18 exact_subrange=1 "
                          "holder_identity=1 cleanup=confirmed",
                          result.stdout)
            self.assertIn("managed_binding_integrity=pass mutations=108 preflight_failures=2",
                          result.stdout)


class MemServiceManagedMappingTests(unittest.TestCase):
    def setUp(self):
        self.fixture = session_fixture.MemServiceObjectSessionTests()
        self.fixture.setUp()

    def tearDown(self):
        self.fixture.tearDown()

    def test_live_process_mapping_is_counted_and_blocks_external_release(self):
        self._live_process_mapping(capacity_pressure=False)

    def test_live_process_mapping_cleans_up_at_idempotency_capacity(self):
        self._live_process_mapping(capacity_pressure=True)

    def _live_process_mapping(self, capacity_pressure):
        fixture = self.fixture
        daemon = fixture._start_active_object()
        process = None
        output = fixture.root / "mapping-owner.log"
        config = fixture._write_session(
            "mapping-owner.conf", fixture._connect,
            [
                "acquire key=obj-1 idempotency_key=owner-acquire expected_generation=1",
                "map key=obj-1 flags=readwrite",
                "write key=obj-1 offset=0 len=16 seed=7",
                "read key=obj-1 offset=0 len=16 seed=7",
                "wait_state key=obj-1 state=retiring timeout_ms=20000 poll_ms=10",
                "unmap key=obj-1",
                "release key=obj-1 idempotency_key=owner-release expected_generation=1",
            ], session_id="mapping-owner", header_extra="provider=session-loopback",
        )
        try:
            with output.open("w") as stream:
                process = subprocess.Popen(
                    [str(fixture.binary), "object-session", "--config", str(config)],
                    stdout=stream, stderr=subprocess.STDOUT, text=True,
                )
                deadline = time.monotonic() + 10
                marker = "action=read key=obj-1 status=ok"
                while marker not in output.read_text():
                    self.assertIsNone(process.poll(), output.read_text())
                    self.assertLess(time.monotonic(), deadline, output.read_text())
                    time.sleep(0.01)
                # A real child process has mapped and written the object; no
                # diagnostic mapping-transition calls manufacture this count.
                stats = fixture._allocation_stats()
                self.assertEqual(stats["live_refs"], "1", stats)
                self.assertEqual(stats["import_mappings"], "1", stats)
                release = fixture._run_client(
                    "release-object", "--connect", fixture._connect,
                    "--key", "obj-1", "--session-id", "mapping-owner",
                    "--expected-generation", "1", "--idempotency-key", "early-release",
                )
                self.assertNotEqual(release.returncode, 0, release.stdout + release.stderr)
                self.assertEqual(fixture._allocation_stats()["live_refs"], "1")
                if capacity_pressure:
                    for attempt in range(80):
                        pressure = fixture._run_client(
                            "retire-object", "--connect", fixture._connect,
                            "--key", "missing-capacity-object", "--expected-generation", "1",
                            "--idempotency-key", f"capacity-pressure-{attempt}",
                        )
                        self.assertNotEqual(pressure.returncode, 0,
                                            pressure.stdout + pressure.stderr)
                        reply = session_fixture._parse_kv(pressure.stdout)
                        if reply.get("status") == "capacity_exceeded":
                            break
                        self.assertEqual(reply.get("status"), "not_found", reply)
                    else:
                        self.fail("bounded pressure never reached idempotency capacity")
                retire = fixture._write_session(
                    "retire-observer.conf", fixture._connect,
                    ["retire key=obj-1 idempotency_key=observer-retire expected_generation=1"],
                )
                result = fixture._run_session(retire)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(process.wait(timeout=10), 0, output.read_text())
            stats = fixture._allocation_stats()
            self.assertEqual(stats["import_mappings"], "0", stats)
            self.assertEqual(stats["live_refs"], "0", stats)
            if capacity_pressure:
                self.assertEqual(stats["idempotency_cleanup_reserved"], "0", stats)
                self.assertEqual(stats["idempotency_reservation_deficit"], "0", stats)
                self.assertEqual(stats["idempotency_used"], stats["idempotency_capacity"], stats)
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            fixture._stop_server(daemon)
