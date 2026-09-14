"""Internal durable-history boundary tests; no daemon or guest acceptance."""
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


class ReplayHistoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("requires a native C compiler")
        root = Path(__file__).resolve().parents[1]
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "replay-history"
        run = subprocess.run([
            compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Dfsync=history_test_fsync", "-Dwrite=history_test_write",
            "-I", str(root / "components/mem_service"),
            str(root / "tests/mem_service_replay_history.c"),
            str(root / "components/mem_service/mem_service_replay_history.c"),
            "-o", str(cls.binary),
        ], capture_output=True, text=True, timeout=30)
        if run.returncode:
            raise AssertionError(run.stdout + run.stderr)

    def run_case(self, name):
        with tempfile.TemporaryDirectory() as directory:
            run = subprocess.run([
                str(self.binary), "--case", name, str(Path(directory) / "history"),
            ], capture_output=True, text=True, timeout=30)
            data = (Path(directory) / "history").read_bytes() if name == "roundtrip" else None
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(f"replay_history=pass case={name}", run.stdout)
        self.assertIn("scope=storage-boundary-fixture", run.stdout)
        return data

    def test_old_results_survive_three_hundred_records_and_reopen(self):
        data = self.run_case("roundtrip")
        self.assertEqual(data[:16], b"MSREPLAY0000001\n")
        offset, previous = 16, 0
        for index in range(300):
            fields = struct.unpack_from("<QQIIIIIIQ", data, offset)
            sequence, chained, operation, request, status, key_len, response_len, reserved, tail = fields
            self.assertEqual((sequence, chained, operation, request, status, reserved, tail),
                             (index + 1, previous, 0x71, index * 177 + 1, index % 3, 0, 0))
            body = offset + 48
            self.assertEqual(data[body:body + key_len], f"operation-{index}".encode())
            expected = f"status={index % 3}\ngeneration={index}\nexact-tail".encode()
            self.assertEqual(data[body + key_len:body + key_len + response_len], expected)
            end = body + key_len + response_len
            checksum = 14695981039346656037
            for value in data[offset:end]:
                checksum = ((checksum ^ value) * 1099511628211) & ((1 << 64) - 1)
            self.assertEqual(struct.unpack_from("<Q", data, end)[0], checksum)
            previous, offset = checksum, end + 8
        self.assertEqual(offset, len(data))

    def test_exact_bytes_and_record_bounds(self):
        self.run_case("bounds")

    def test_complete_frame_truncation_rejects_checkpoint(self):
        self.run_case("truncate")

    def test_torn_tail_is_preserved_and_rejected(self):
        self.run_case("torn")

    def test_corrupt_suffix_cannot_hide_behind_an_earlier_hit(self):
        self.run_case("corrupt")

    def test_exclusive_owner_missing_file_and_path_replacement(self):
        self.run_case("ownership")

    def test_partial_write_keeps_cache_checkpoint_and_poisoned_handle(self):
        self.run_case("write-failure")

    def test_sync_failure_keeps_cache_checkpoint_and_requires_reopen(self):
        self.run_case("sync-failure")

    def test_cli_rejects_missing_extra_and_unknown_arguments(self):
        for args in ([], ["--self-test"], ["--case", "unknown", "unused"],
                     ["--case", "roundtrip", "unused", "extra"]):
            run = subprocess.run([str(self.binary), *args],
                                 capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 2, run.stdout + run.stderr)
            self.assertIn("usage: replay-history --case", run.stderr)


if __name__ == "__main__":
    unittest.main()
