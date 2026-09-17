import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "mem_service"
PROVIDERS = COMPONENT / "providers"
APP_DIR = ROOT / "apps" / "mem_service"
CONFORMANCE_SOURCE = ROOT / "tests" / "mem_service_obmm_provider_conformance.c"
OBMM_SUBMODULE = ROOT / "vendor" / "obmm"
LIBOBMM_INCLUDE_FLAGS = [
    "-D__EXPORTED_HEADERS__",
    "-I",
    str(OBMM_SUBMODULE / "src" / "libobmm"),
    "-I",
    str(ROOT / "kernel_ub" / "include" / "uapi"),
    "-I",
    str(ROOT / "kernel_ub" / "include"),
]
LIBOBMM_SRCS = [
    str(OBMM_SUBMODULE / "src" / "libobmm" / "libobmm.c"),
    str(ROOT / "common" / "obmm_vendor_adaptor_sim.c"),
]


class MemServiceObmmProviderTest(unittest.TestCase):
    def test_gsva_import_and_visibility_boundaries(self):
        native = platform.system() == "Linux" and platform.machine() == "aarch64"
        compiler = shutil.which("cc" if native else "aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("requires AArch64 compiler")
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "gsva-import-test"
            result = subprocess.run(
                [compiler, "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", str(ROOT), *LIBOBMM_INCLUDE_FLAGS,
                 str(PROVIDERS / "mem_service_provider_obmm_test.c"),
                 str(COMPONENT / "mem_service_provider.c"), *LIBOBMM_SRCS,
                 "-Wl,--wrap=obmm_import", "-Wl,--wrap=ioctl",
                 "-Wl,--wrap=obmm_unimport", "-Wl,--wrap=obmm_unexport",
                 "-Wl,--wrap=obmm_export",
                 "-Wl,--wrap=open", "-Wl,--wrap=close", "-Wl,--wrap=mmap",
                 "-Wl,--wrap=munmap", "-o", str(binary)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stderr)
            if native:
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("gsva_import_dual_token=pass", result.stdout)
                self.assertIn("obmm_fixed_subrange=pass views=4 rejected=10 "
                              "file_alias=1 readonly=1 resources=0", result.stdout)
                self.assertIn("obmm_cleanup_ownership=pass", result.stdout)
                self.assertIn("obmm_cached_visibility=pass readonly=1 readwrite=1 "
                              "bounds=1 ioctl_failure=1", result.stdout)
                self.assertIn("obmm_compute_mapping_pins=pass no_alias=1 deferred_cleanup=1",
                              result.stdout)
                self.assertIn("retained_handle_conflict=pass", result.stdout)
                self.assertIn("retained_descriptor_probe=pass checks=24 device_operations=0",
                              result.stdout)
                self.assertIn("obmm_import_pa_fallback=pass candidates=5 "
                              "first_conflict=retried", result.stdout)
                self.assertEqual(result.stderr.count("result=pass checks=24 source=retained_handle"), 2)
                self.assertRegex(result.stderr, r"obmm-map: result=failed stage=mmap "
                                 r"fixed_va=0x[0-9a-f]+ len=[0-9]+ errno=17\b")
                focused = subprocess.run([str(binary), "--fixed-subrange"],
                                         capture_output=True, text=True, timeout=10)
                self.assertEqual(focused.returncode, 0, focused.stderr)
                self.assertIn("obmm_fixed_subrange=pass views=4 rejected=10", focused.stdout)
                cached = subprocess.run([str(binary), "--cached-visibility"],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(cached.returncode, 0, cached.stderr)
                self.assertIn("obmm_cached_visibility=pass", cached.stdout)

    def _compile(self, compiler: str, output: pathlib.Path, linux_backend=None) -> None:
        if linux_backend is None:
            linux_backend = platform.system() == "Linux"
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
                *(LIBOBMM_INCLUDE_FLAGS if linux_backend else []),
                str(PROVIDERS / "mem_service_provider_obmm_cli.c"),
                str(PROVIDERS / "mem_service_provider_obmm.c"),
                str(COMPONENT / "mem_service_provider.c"),
                *(LIBOBMM_SRCS if linux_backend else []),
                "-pthread",
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

    def test_holder_rejoin_is_explicit_and_fail_closed(self):
        cli = (PROVIDERS / "mem_service_provider_obmm_cli.c").read_text()
        worker = (PROVIDERS / "mem_service_provider_obmm_worker.c").read_text()
        provider_readme = (PROVIDERS / "README.md").read_text()

        self.assertIn("prepare-holder-rejoin", cli)
        self.assertIn("mem_service_provider_obmm_prepare_holder_rejoin", cli)
        self.assertIn('before.st_size != WORKER_LEDGER_FRAME_BYTES', worker)
        self.assertIn('reason = "old-home-obligations-remain"', worker)
        self.assertIn('reason = "old-holder-obligations-remain"', worker)
        self.assertIn(".holder-fenced-", worker)
        self.assertIn("原子归档", provider_readme)
        self.assertIn("禁止使用该命令跳过资源对账", provider_readme)

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
            self.assertIn("gsva_descriptor_version=2", fixture.stdout)
            self.assertIn("gsva_identity=checked", fixture.stdout)
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
        if compiler is None and platform.system() == "Linux" and platform.machine() == "aarch64":
            compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("aarch64-linux-gnu-gcc is unavailable")
        with tempfile.TemporaryDirectory() as temp_dir:
            output = pathlib.Path(temp_dir) / "linqu_mem_service_provider_obmm"
            self._compile(compiler, output, linux_backend=True)
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
                    "-pthread",
                    "-I",
                    str(ROOT),
                    "-I",
                    str(COMPONENT),
                    "-I",
                    str(PROVIDERS),
                    *LIBOBMM_INCLUDE_FLAGS,
                    str(CONFORMANCE_SOURCE),
                    str(PROVIDERS / "mem_service_provider_obmm.c"),
                    str(COMPONENT / "mem_service_provider.c"),
                    *LIBOBMM_SRCS,
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
