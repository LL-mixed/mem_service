import hashlib
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RepositoryContractTests(unittest.TestCase):
    def test_version_file_matches_runtime_and_package_contracts(self):
        version = (ROOT / "VERSION").read_text().strip()
        self.assertRegex(version, r"^[0-9]+\.[0-9]+\.[0-9]+$")

        cli_source = (ROOT / "apps" / "mem_service" / "mem_service.c").read_text()
        makefile = (ROOT / "apps" / "mem_service" / "Makefile").read_text()
        self.assertIn(f'#define MEM_SERVICE_RELEASE_VERSION "{version}"', cli_source)
        self.assertIn(f"MEM_SERVICE_DEB_VERSION ?= {version}-1", makefile)
        self.assertIn(f"MEM_SERVICE_RPM_VERSION ?= {version}", makefile)

    def test_vendored_files_match_recorded_checksums(self):
        provenance = (ROOT / "VENDORED.md").read_text()
        expected_files = {
            "common/obmm_common.h":
                "95b9645e04e685039e68d49cefcf5c1d62ee17d029c1c58d1d3433d9c03e4635",
            "kernel_ub/include/uapi/ub/gsva.h":
                "8986cec72ff5d252b729c44b746fcb6252cfe6d27e9d23e1e2a1a76190045236",
            "kernel_ub/include/uapi/ub/obmm.h":
                "50b1b9b081a030345bcc692a264c9593fa288dd30335b1370db354aa3421960c",
        }

        for relative_path, expected in expected_files.items():
            with self.subTest(path=relative_path):
                actual = hashlib.sha256((ROOT / relative_path).read_bytes()).hexdigest()
                self.assertEqual(actual, expected)
                self.assertIn(expected, provenance)

        queue_files = sorted(
            path
            for path in (ROOT / "libs" / "obmm_queue").iterdir()
            if path.is_file()
        )
        checksum_lines = "".join(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  "
            f"libs/obmm_queue/{path.name}\n"
            for path in queue_files
        )
        aggregate = hashlib.sha256(checksum_lines.encode()).hexdigest()
        recorded = re.search(
            r"`libs/obmm_queue/`.*?Aggregate SHA-256:\s+`([0-9a-f]{64})`",
            provenance,
            re.DOTALL,
        )
        self.assertIsNotNone(recorded)
        self.assertEqual(aggregate, recorded.group(1))


if __name__ == "__main__":
    unittest.main()
