import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ScriptBleBindingsTest(unittest.TestCase):
    def test_managed_client_is_separate_from_keyboard(self):
        descriptor = (ROOT / "src/apps/solar_os_script_api.inc").read_text()
        ble = descriptor.split("#if SOLAR_OS_PACKAGE_SERVICE_BLE\n", 1)[1].split("#endif", 1)[0]
        self.assertIn("SOLAR_OS_SCRIPT_API_FUNCTION(ble, read, read);", ble)
        for method in ("connect", "disconnect", "status", "services", "characteristics", "read", "write"):
            self.assertIn(f"SOLAR_OS_SCRIPT_API_SUBMODULE_FUNCTION(ble, gatt, {method}, {method});", ble)

    def test_both_runtimes_release_on_all_teardown_paths(self):
        for language, prefix, teardown in (("python", "python", "mp_embed_deinit();"),
                                            ("lua", "solua", "lua_close(L);")):
            source = (ROOT / f"src/apps/solar_os_{language}.c").read_text()
            binding = (ROOT / f"src/apps/solar_os_{language}_ble.inc").read_text()
            self.assertEqual(source.count(f"{prefix}_ble_destroy();"), 3)
            for part in source.split(teardown)[:-1]:
                self.assertIn(f"{prefix}_ble_destroy();", part)
            self.assertIn("solar_os_ble_session_close", binding)
            self.assertIn("solar_os_ble_session_set_cancel_check", binding)
            self.assertIn(f"{prefix}_should_cancel", binding)
            self.assertNotRegex(binding, r"solar_os_ble_gatt_(connect|read|write|disconnect)\(")
            self.assertNotIn("esp_ble_", binding)

    def test_return_fields_have_language_parity(self):
        python = (ROOT / "src/apps/solar_os_python_ble.inc").read_text()
        lua = (ROOT / "src/apps/solar_os_lua_ble.inc").read_text()
        py_fields = set(re.findall(r'python_dict_store_\w+\([^,]+, "(\w+)"', python))
        lua_fields = set(re.findall(r'solua_set_\w+\(L, -1, "(\w+)"', lua))
        self.assertEqual(py_fields, lua_fields)
        self.assertTrue({"index", "mtu", "retiring", "properties", "max_value_bytes"} <= py_fields)
        self.assertIn("mp_obj_new_bytes(value, len)", python)
        self.assertIn("lua_pushlstring(L, (const char *)value, len)", lua)

    def test_language_specific_buffers_and_bounded_arguments(self):
        for language in ("python", "lua"):
            binding = (ROOT / f"src/apps/solar_os_{language}_ble.inc").read_text()
            self.assertIn("60000", binding)
            self.assertIn("handle < 1 || handle > UINT16_MAX", binding)
            self.assertIn("solar_os_ble_parse_address(address, len, bda)", binding)
            self.assertIn("BLE operation cancelled", binding)
            self.assertIn("SOLAR_OS_BLE_GATT_VALUE_MAX", binding)


if __name__ == "__main__":
    unittest.main()
