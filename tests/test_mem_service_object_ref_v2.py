import pathlib
import shutil
import subprocess
import tempfile
import unittest


class ObjectRefV2Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("requires a native C compiler")
        cls.root = pathlib.Path(__file__).resolve().parents[1]
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / "object-ref-v2"
        built = subprocess.run([
            compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(cls.root / "components/mem_service"),
            str(cls.root / "tests/mem_service_object_ref_v2.c"), "-o", str(cls.binary),
        ], capture_output=True, text=True, timeout=30)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def test_canonical_wire_bounds_failures_and_v1_compatibility(self):
        result = subprocess.run([str(self.binary), "--self-test"], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("v1_bytes=64 truncations=256 mutations=16 scope=codec-only", result.stdout)

    def test_cli_rejects_invalid_hex_and_arguments(self):
        for args, code in (([], 2), (["--unknown"], 2), (["--self-test", "extra"], 2),
                           (["--decode-hex", "0" * 512], 1),
                           (["--decode-hex", "0" * 511], 1),
                           (["--decode-hex", "x" * 512], 1)):
            with self.subTest(args=args):
                result = subprocess.run([str(self.binary), *args], capture_output=True,
                                        text=True, timeout=5)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)

    def test_cli_decodes_frozen_little_endian_reference(self):
        golden = (
            "4645524d4d424f51020005000200000001000000020000000300000000000000"
            "aaae337b0b4a94e8800f0000000000000001000000000000a8a7a6a5a4a3a2a1"
            "0001000003000000080706050403020118171615141312110000010000000000"
            "6f626a6563742f766965772d3100000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000"
            "67756573742d6e6f64652d310000000000000000000000000000000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000"
        )
        for encoded in (golden, golden.upper()):
            result = subprocess.run([str(self.binary), "--decode-hex", encoded],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("key=object/view-1 home=guest-node-1 access=3", result.stdout)
            self.assertIn("generation=72623859790382856 incarnation=1230066625199609624", result.stdout)
            self.assertIn("version=3 offset=3968 bytes=256 allocation_bytes=65536 scope=codec-only", result.stdout)

    def test_header_compiles_as_cpp_without_link_dependencies(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("requires a C++ compiler")
        result = subprocess.run([
            compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-x", "c++",
            "-fsyntax-only", "-I", str(self.root / "components/mem_service"), "-",
        ], input='#include "lingqu_object_service.h"\nint main() { return 0; }\n',
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
