import pathlib
import shutil
import subprocess
import tempfile
import unittest


class ReferenceProtocolTests(unittest.TestCase):
    def test_sdk_rejects_malformed_responses_without_exposing_output(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires a native C compiler")
        root = pathlib.Path(__file__).resolve().parents[1]
        component = root / "components/mem_service"
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "reference-client"
            build = subprocess.run([
                compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O2",
                "-Wall", "-Wextra", "-Werror", "-I", str(component),
                str(root / "tests/mem_service_reference_client.c"),
                str(component / "mem_service_client.c"), str(component / "mem_service_provider.c"),
                "-o", str(binary),
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary), "--self-test"], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("malformed_replies=17 output_preserved=1 scope=mock-wire", run.stdout)

    def test_strict_roundtrip_and_unchanged_failure_outputs(self):
        compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("requires a native C compiler")
        root = pathlib.Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "reference-protocol"
            build = subprocess.run([
                compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O2",
                "-Wall", "-Wextra", "-Werror", "-I", str(root / "components/mem_service"),
                str(root / "tests/mem_service_reference_protocol.c"), "-o", str(binary),
            ], capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary), "--self-test"], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("actions=5 invalid_requests=16 hex_truncations=512 scope=codec-only", run.stdout)
            for args in ([], ["--unknown"], ["--self-test", "extra"]):
                run = subprocess.run([str(binary), *args], capture_output=True, timeout=5)
                self.assertEqual(run.returncode, 2)


if __name__ == "__main__":
    unittest.main()
