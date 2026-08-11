import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "mem_service"
PROVIDERS = COMPONENT / "providers"
APP_DIR = ROOT / "apps" / "mem_service"
CONFORMANCE_SOURCE = ROOT / "tests" / "mem_service_obmm_provider_conformance.c"


class MemServiceObmmProviderTest(unittest.TestCase):
    def _compile(self, compiler: str, output: pathlib.Path) -> None:
        subprocess.run(
            [
                compiler,
                "-O2",
                "-Wall",
                "-Wextra",
                "-I",
                str(ROOT),
                "-I",
                str(COMPONENT),
                "-I",
                str(PROVIDERS),
                str(PROVIDERS / "mem_service_provider_obmm_cli.c"),
                str(PROVIDERS / "mem_service_provider_obmm.c"),
                str(COMPONENT / "mem_service_provider.c"),
                "-o",
                str(output),
            ],
            check=True,
            cwd=ROOT,
            capture_output=True,
            text=True,
        )

    def test_neutral_contract_exposes_peer_mapping_operations(self):
        header = (COMPONENT / "mem_service_provider.h").read_text()

        self.assertIn("MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING", header)
        self.assertIn("MEM_SERVICE_PROVIDER_CAP_DATA_PLANE_MASK", header)
        self.assertIn("(*map_remote_region)", header)
        self.assertIn("(*unmap_remote_region)", header)
        self.assertIn("(*publish_range)", header)
        self.assertIn("(*invalidate_range)", header)
        self.assertIn("(*wait_range_visible)", header)

    def test_obmm_provider_is_independent_from_urma(self):
        source = (PROVIDERS / "mem_service_provider_obmm.c").read_text().lower()
        provider_readme = (PROVIDERS / "README.md").read_text().lower()

        self.assertNotIn("#include <urma", source)
        self.assertNotIn("uburma", source)
        self.assertIn("obmm remote mappings use the sim_dec/gva/gsva", provider_readme)
        self.assertIn("of urma", provider_readme)
        self.assertIn("mapping_path=sim-dec", source)
        self.assertIn("map_osync = context->force_osync ||", source)

    def test_makefile_exposes_obmm_provider_smoke(self):
        makefile = (APP_DIR / "Makefile").read_text()

        self.assertIn("linqu_mem_service_provider_obmm:", makefile)
        self.assertIn("obmm-provider-smoke:", makefile)
        self.assertIn("MEM_SERVICE_PROVIDER_OBMM", makefile)

    def test_qemu_conformance_uses_real_provider_through_neutral_channel(self):
        source = CONFORMANCE_SOURCE.read_text()

        self.assertIn("stage=pre-canary readiness=degraded", source)
        self.assertIn("mem_service_provider_registry_data_plane_ready", source)
        self.assertIn("mem_service_provider_channel_register_region", source)
        self.assertIn("mem_service_provider_channel_map_remote_region", source)
        self.assertIn("mem_service_provider_channel_publish_range", source)
        self.assertIn("mem_service_provider_channel_invalidate_range", source)
        self.assertIn("mem_service_provider_channel_wait_range_visible", source)
        self.assertIn("mem_service_provider_channel_unmap_remote_region", source)
        self.assertIn("fail-closed-corrupt-descriptor", source)
        self.assertIn("fail-closed-checksum", source)

    def test_host_protocol_fixture_and_fail_closed_status(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("cc is unavailable")
        with tempfile.TemporaryDirectory() as temp_dir:
            binary = pathlib.Path(temp_dir) / "linqu_mem_service_provider_obmm"
            self._compile(compiler, binary)
            fixture = subprocess.run(
                [str(binary), "protocol-fixtures"],
                check=True,
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertIn("status=ok", fixture.stdout)
            self.assertIn("node_local_id_collision=fail-closed", fixture.stdout)
            self.assertIn("mapping_path=sim-dec", fixture.stdout)
            self.assertIn("urma_dependency=none", fixture.stdout)

            status = subprocess.run(
                [
                    str(binary),
                    "status",
                    "--device",
                    "/definitely/missing-obmm",
                    "--cna-path",
                    "/definitely/missing-cna",
                ],
                check=False,
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertEqual(1, status.returncode)
            self.assertIn("status=unavailable", status.stderr)
            self.assertIn("data_plane_ready=0", status.stderr)

    def test_linux_backend_cross_compiles_when_toolchain_is_available(self):
        compiler = shutil.which("aarch64-linux-gnu-gcc")
        if compiler is None:
            self.skipTest("aarch64-linux-gnu-gcc is unavailable")
        with tempfile.TemporaryDirectory() as temp_dir:
            output = pathlib.Path(temp_dir) / "linqu_mem_service_provider_obmm"
            self._compile(compiler, output)
            self.assertTrue(output.exists())

            conformance = pathlib.Path(temp_dir) / "obmm_conformance"
            subprocess.run(
                [
                    compiler,
                    "-static",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT),
                    "-I",
                    str(COMPONENT),
                    "-I",
                    str(PROVIDERS),
                    str(CONFORMANCE_SOURCE),
                    str(PROVIDERS / "mem_service_provider_obmm.c"),
                    str(COMPONENT / "mem_service_provider.c"),
                    "-o",
                    str(conformance),
                ],
                check=True,
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            self.assertTrue(conformance.exists())


if __name__ == "__main__":
    unittest.main()
