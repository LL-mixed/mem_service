import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class MemServiceMappingOwnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("requires a native C compiler")
        root = Path(__file__).resolve().parents[1]
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "mapping-owner"
        result = subprocess.run(
            [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-I", str(root / "components" / "mem_service"),
             str(root / "tests" / "mem_service_mapping_owner.c"),
             str(root / "components" / "mem_service" / "mem_service_mapping_owner.c"),
             "-o", str(cls.binary)], capture_output=True, text=True, timeout=30,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_threaded_ownership_and_retryable_cleanup(self):
        result = subprocess.run([str(self.binary), "--self-test"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("mapping_owner_native=pass threads=8 owners=2 callbacks=800", result.stdout)
        self.assertIn("scope=boundary-fixture", result.stdout)
        self.assertIn("mapping_owner_unmap=pass holder_retained=1 retry_idempotent=1 "
                      "admission_closed=1", result.stdout)

    def test_cli_rejects_unknown_arguments(self):
        for arguments in ([], ["--unknown"], ["--self-test", "extra"]):
            with self.subTest(arguments=arguments):
                result = subprocess.run([str(self.binary), *arguments],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn("usage: mapping-owner --self-test", result.stderr)


if __name__ == "__main__":
    unittest.main()
