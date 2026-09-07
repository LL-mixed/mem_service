"""Run the real terminal reader against independently stale imported bytes."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "mem_service"


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class TerminalTokenVisibilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mem_service_token_")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "terminal_token_visibility"
        command = [
            "cc", "-O1", "-Wall", "-Wextra", "-ffunction-sections",
            "-fdata-sections",
            "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
        ]
        for include in (ROOT, COMPONENT, ROOT / "vendor/obmm/src/libobmm"):
            command.extend(["-I", str(include)])
        command.extend([
            str(ROOT / "tests/mem_service_terminal_token_visibility.c"),
            str(COMPONENT / "mem_service_model_terminal_token_flow.c"),
            str(COMPONENT / "mem_service_cluster_read.c"),
            "-o", str(cls.binary),
        ])
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def test_remote_token_visibility(self):
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("SIM_", "MEM_SERVICE_"))
        }
        for case in (
            "stale_import", "local", "checksum", "metadata_sync_failure",
            "payload_sync_failure", "torn_publication", "changed_publication",
            "record_bounds", "metadata_bounds", "address_overflow",
        ):
            with self.subTest(case=case):
                result = subprocess.run(
                    [str(self.binary), case], env=env, capture_output=True,
                    text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"case={case} status=ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
