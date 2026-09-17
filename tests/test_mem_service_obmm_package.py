import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOL = ROOT / "scripts" / "build_mem_service_obmm_package.py"
CONFIG = (
    ROOT
    / "apps"
    / "mem_service"
    / "configs"
    / "providers"
    / "obmm"
    / "worker.example.conf"
)


class MemServiceObmmPackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.core = self.root / "linqu_mem_service"
        self.provider = self.root / "linqu_mem_service_provider_obmm"
        self.package = self.root / "obmm.tar"
        self.core.write_bytes(b"core-binary")
        self.provider.write_bytes(b"provider-binary")

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *args, check=True):
        return subprocess.run(
            [sys.executable, str(TOOL), *map(str, args)],
            check=check,
            capture_output=True,
            text=True,
        )

    def build(self):
        return self.run_tool(
            "build",
            "--core",
            self.core,
            "--provider",
            self.provider,
            "--config",
            CONFIG,
            "--output",
            self.package,
            "--service-revision",
            "0123456789abcdef",
            "--platform-revision",
            "fedcba9876543210",
            "--uapi-version",
            "obmm-uapi-v2",
        )

    def test_builds_and_verifies_source_free_layout(self):
        first = self.build()
        self.assertIn("status=pass action=build", first.stdout)
        original = self.package.read_bytes()
        second = self.build()
        self.assertEqual(original, self.package.read_bytes())
        self.assertIn("status=pass action=build", second.stdout)
        verify = self.run_tool("verify", "--package", self.package)
        self.assertIn("status=pass action=verify", verify.stdout)
        with tarfile.open(self.package) as archive:
            self.assertEqual(
                set(archive.getnames()),
                {
                    "usr/bin/linqu_mem_service",
                    "usr/libexec/lingqu/mem_service/linqu_mem_service_provider_obmm",
                    "etc/lingqu/mem_service/providers/obmm/worker.conf",
                    "usr/share/lingqu/mem_service/obmm-package-manifest.txt",
                },
            )
            manifest = archive.extractfile(
                "usr/share/lingqu/mem_service/obmm-package-manifest.txt"
            ).read().decode()
        self.assertIn("platform_linkage=static\n", manifest)
        self.assertIn("source_repo_required=0\n", manifest)
        self.assertIn("infer_required=0\n", manifest)
        self.assertIn("model_required=0\n", manifest)

    def test_rejects_invalid_provider_config(self):
        bad_config = self.root / "bad.conf"
        bad_config.write_text("connect=unix:/tmp/service.sock\n")
        result = self.run_tool(
            "build",
            "--core",
            self.core,
            "--provider",
            self.provider,
            "--config",
            bad_config,
            "--output",
            self.package,
            "--service-revision",
            "service",
            "--platform-revision",
            "platform",
            "--uapi-version",
            "uapi",
            check=False,
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("provider config keys mismatch", result.stdout)

    def test_rejects_tampered_payload(self):
        self.build()
        data = bytearray(self.package.read_bytes())
        data[1024] ^= 1
        self.package.write_bytes(data)
        result = self.run_tool(
            "verify", "--package", self.package, check=False
        )
        self.assertEqual(result.returncode, 1)
        self.assertIn("status=fail", result.stdout)


if __name__ == "__main__":
    unittest.main()
