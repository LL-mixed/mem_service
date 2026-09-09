"""Execute the real KV allocator and reservation validation on host memory."""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components/mem_service"


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class KvReservedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mem_service_kv_reserved_")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "kv_reserved"
        command = [
            "cc", "-O1", "-Wall", "-Wextra", "-ffunction-sections", "-fdata-sections",
            "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
        ]
        for include in (ROOT, COMPONENT, ROOT / "vendor/obmm/src/libobmm",
                        ROOT / "kernel_ub/include/uapi"):
            command.extend(["-I", str(include)])
        command.extend([
            str(ROOT / "tests/mem_service_kv_reserved.c"),
            str(COMPONENT / "mem_service_qwen3_runtime.c"),
            str(COMPONENT / "mem_service_obmm_objects.c"),
            "-o", str(cls.binary),
        ])
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def test_reservations(self):
        for case in (
            "valid", "multi_block", "wrong_pointer", "unaligned", "before_arena",
            "unreserved_padding", "slot_padding", "past_arena", "past_slot",
            "length_overflow", "address_overflow", "empty", "bad_node",
            "too_many_nodes", "null_mapping", "null_payload",
        ):
            with self.subTest(case=case):
                result = subprocess.run(
                    [str(self.binary), case], capture_output=True, text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"case={case} status=ok", result.stdout)


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class KvPublicationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="mem_service_kv_publish_")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.binary = Path(cls.tmp.name) / "kv_publish"
        command = [
            "cc", "-O1", "-Wall", "-Wextra", "-ffunction-sections", "-fdata-sections",
            "-fno-builtin", "-Dmemcpy=mem_service_test_memcpy",
            "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
        ]
        for include in (ROOT, COMPONENT, ROOT / "vendor/obmm/src/libobmm",
                        ROOT / "kernel_ub/include/uapi"):
            command.extend(["-I", str(include)])
        command.append(str(ROOT / "tests/mem_service_kv_publish.c"))
        for source in (
            "mem_service_model_range_publish_flow.c", "mem_service_qwen3_runtime.c",
            "mem_service_obmm_objects.c", "mem_service_records.c",
            "mem_service_object_refs.c", "mem_service_ub_ssd_gsva_backend.c",
        ):
            command.append(str(COMPONENT / source))
        command.extend(["-o", str(cls.binary)])
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    def test_publication(self):
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("SIM_", "MEM_SERVICE_"))}
        for case in (
            "in_place", "copy", "wrong_pointer", "invalid_pointer", "checksum",
            "unreserved_padding", "visibility_failure",
        ):
            with self.subTest(case=case):
                result = subprocess.run(
                    [str(self.binary), case], env=env, capture_output=True,
                    text=True, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(f"case={case} status=ok", result.stdout)
                if case == "in_place":
                    self.assertIn("payload_mode=in_place publication_copy_bytes=0", result.stdout)
                elif case == "copy":
                    self.assertIn("payload_mode=copy publication_copy_bytes=80", result.stdout)


if __name__ == "__main__":
    unittest.main()
