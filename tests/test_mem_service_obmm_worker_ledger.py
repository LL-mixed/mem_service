import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == 'Linux', 'requires Linux UAPI headers')
class ObmmWorkerLedgerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('requires C compiler')
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = pathlib.Path(cls.directory.name) / 'worker-ledger-test'
        source = ROOT / 'components/mem_service/providers/mem_service_provider_obmm_worker_ledger_test.c'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-D__EXPORTED_HEADERS__', '-I', str(ROOT / 'kernel_ub/include/uapi'),
                        str(source), '-o', str(cls.binary)], check=True,
                       capture_output=True, text=True, timeout=30)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), '--case', name],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f'worker_ledger_{name}=pass', result.stdout)

    def test_roundtrip_complete_identity(self):
        self.run_case('roundtrip')

    def test_every_byte_corruption_rejected(self):
        self.run_case('corruption')

    def test_zero_birth_identity_and_old_version_rejected(self):
        self.run_case('birth-identity')

    def test_noncanonical_frames_and_process_pointers_rejected(self):
        self.run_case('canonical')
