from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class MemServiceMappingCleanupTests(unittest.TestCase):
    def test_failed_mapping_cleanup_retains_ownership_and_revokes_access(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "native C compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "mapping-cleanup"
            component = ROOT / "components" / "mem_service"
            result = subprocess.run(
                [compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT),
                 str(ROOT / "tests" / "mem_service_mapping_cleanup.c"),
                 str(component / "mem_service_client.c"),
                 str(component / "mem_service_wire_client.c"),
                 str(component / "mem_service_provider.c"), "-o", str(binary)],
                capture_output=True, text=True, timeout=120,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("mapping_cleanup_regression=pass", result.stdout)
