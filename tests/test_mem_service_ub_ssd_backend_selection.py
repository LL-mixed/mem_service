import shutil
import subprocess
import tempfile
import unittest
import platform
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"


@unittest.skipUnless(shutil.which("cc"), "host cc is required")
class MemServiceUbSsdBackendSelectionTests(unittest.TestCase):
    def test_secondary_attachment_does_not_replace_primary_payload(self):
        with tempfile.TemporaryDirectory(prefix="mem_service_ub_ssd_") as temp:
            temp_dir = Path(temp)
            source = temp_dir / "backend_selection.c"
            binary = temp_dir / "backend_selection"
            source.write_text(
                r'''
#include "components/mem_service/mem_service_ub_ssd_gsva_backend.h"

#include <string.h>

int main(void)
{
    struct mem_service_record record;
    const struct mem_service_ub_ssd_gsva_block_ref block = {
        .block_hi = 1,
        .block_lo = 2,
        .version = 3,
        .offset = 4,
        .bytes = 4096,
        .checksum64 = 0x12345678,
    };

    memset(&record, 0, sizeof(record));
    record.object_payload_kind = 17;
    record.object_backing_offset = 8192;
    record.object_backing_len = block.bytes;
    record.object_payload_checksum = block.checksum64;

    if (!mem_service_ub_ssd_gsva_block_ref_matches_payload(
            &block, block.bytes, block.checksum64) ||
        mem_service_ub_ssd_gsva_block_ref_matches_payload(
            &block, block.bytes + 1, block.checksum64) ||
        mem_service_ub_ssd_gsva_block_ref_matches_payload(
            &block, block.bytes, block.checksum64 + 1)) {
        return 1;
    }
    if (mem_service_record_attach_ub_ssd_gsva_backend_ref(
            &record, 1, 2, 0, &block, false) != 0 ||
        mem_service_record_uses_ub_ssd_gsva_primary_payload(&record) ||
        record.object_payload_kind != 17 ||
        record.object_backing_offset != 8192 ||
        record.object_payload_checksum != block.checksum64) {
        return 2;
    }
    if (mem_service_record_attach_ub_ssd_gsva_backend_ref(
            &record, 1, 2, 0, &block, true) != 0 ||
        !mem_service_record_uses_ub_ssd_gsva_primary_payload(&record) ||
        record.object_payload_kind !=
            MEM_SERVICE_PAYLOAD_KIND_UB_SSD_GSVA_BLOCK ||
        record.object_backing_offset != block.offset ||
        record.object_backing_len != block.bytes) {
        return 3;
    }
    return 0;
}
'''
            )
            compile_command = [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                f"-I{ROOT}",
                str(source),
                str(SERVICE_DIR / "mem_service_ub_ssd_gsva_backend.c"),
                "-o",
                str(binary),
            ]
            command_line_tools_sdk = Path(
                "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
            )
            command_line_tools_cc = Path(
                "/Library/Developer/CommandLineTools/usr/bin/clang"
            )
            if platform.system() == "Darwin" and command_line_tools_sdk.exists():
                if command_line_tools_cc.exists():
                    compile_command[0] = str(command_line_tools_cc)
                compile_command[1:1] = ["-isysroot", str(command_line_tools_sdk)]
            compiled = subprocess.run(
                compile_command,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                compiled.returncode,
                0,
                compiled.stdout + compiled.stderr,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
