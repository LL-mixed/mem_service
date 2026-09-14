import pathlib
import shutil
import subprocess
import tempfile
import unittest


class ReferenceCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("requires a native C compiler")
        cls.root = pathlib.Path(__file__).resolve().parents[1]
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "reference-catalog"
        component = cls.root / "components/mem_service"
        result = subprocess.run([
            compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O2",
            "-Wall", "-Wextra", "-Werror", "-I", str(component),
            str(cls.root / "tests/mem_service_reference_catalog.c"),
            *[str(component / name) for name in (
                "mem_service_allocation.c", "mem_service_records.c", "mem_service_object_refs.c")],
            "-o", str(cls.binary),
        ], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def test_publication_identity_and_holder_lifecycle(self):
        result = subprocess.run([str(self.binary), "--self-test"],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("identity_mutations=12 publication_cycles=100 scope=core-metadata", result.stdout)

    def test_cli_rejects_unknown_action_and_extra_arguments(self):
        for args in ([], ["--unknown"], ["--self-test", "extra"]):
            with self.subTest(args=args):
                result = subprocess.run([str(self.binary), *args],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
