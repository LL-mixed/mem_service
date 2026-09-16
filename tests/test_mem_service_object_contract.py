import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "components/mem_service/mem_service_object_contract.h"


class MemServiceObjectContractTest(unittest.TestCase):
    def test_terminal_result_has_distinct_stable_kind(self):
        source = CONTRACT.read_text()
        self.assertIn(
            "#define MEM_SERVICE_OBMM_KIND_MODEL_TOKEN_RESULT 6U", source)
        self.assertIn(
            "#define MEM_SERVICE_OBMM_KIND_MODEL_TERMINAL_RESULT 13U", source)


if __name__ == "__main__":
    unittest.main()
