from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BleKeyboardCacheTest(unittest.TestCase):
    def test_forget_cleans_gatt_cache_before_removing_bond(self):
        source = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        start = source.index("static esp_err_t remove_deferred_bonds(void)\n{")
        end = source.index("\nstatic esp_err_t complete_deferred_bond_forget(void)", start)
        forget = source[start:end]

        cache_clean = "esp_ble_gattc_cache_clean(bda)"
        remove_bond = "esp_ble_remove_bond_device(bda)"
        self.assertIn(cache_clean, forget)
        self.assertIn(remove_bond, forget)
        self.assertLess(forget.index(cache_clean), forget.index(remove_bond))

    def test_remembered_state_is_not_cleared_when_forget_is_requested(self):
        source = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        start = source.index("static esp_err_t forget_remembered_keyboard(void)\n{")
        end = source.index("\nesp_err_t solar_os_ble_keyboard_forget(void)", start)
        request = source[start:end]

        self.assertNotIn("clear_remembered_peers()", request)
        self.assertIn("complete_deferred_bond_forget()", request)

    def test_matching_gap_completion_finishes_forget(self):
        source = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        start = source.index("static void gap_callback(")
        end = source.index("\nstatic bool gatt_event_is_for_hid", start)
        callback = source[start:end]

        self.assertIn("ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT", callback)
        self.assertIn("memcmp(bond_remove_bda,", callback)
        self.assertIn("complete_bond_forget(", callback)

    def test_completion_clears_remembered_state_only_after_bond_is_absent(self):
        source = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        start = source.index(
            "static esp_err_t complete_bond_forget(",
            source.index("static esp_err_t bonded_peer_is_present"),
        )
        end = source.index("\nstatic esp_err_t remove_deferred_bonds(void)", start)
        completion = source[start:end]

        absent_check = "if (query_ret == ESP_OK && !still_bonded)"
        clear_state = (
            "if (bond_removed) {\n"
            "        const esp_err_t clear_ret = clear_remembered_peers();"
        )
        self.assertIn(absent_check, completion)
        self.assertIn(clear_state, completion)
        self.assertLess(completion.index(absent_check), completion.index(clear_state))

    def test_nvs_commit_precedes_runtime_state_clear(self):
        source = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        start = source.index("static esp_err_t clear_remembered_peers(void)\n{")
        end = source.index("\nstatic bool hidh_conn_params_ready", start)
        clear = source[start:end]

        commit = "ret = nvs_commit(nvs);"
        runtime_clear = "memset(remembered_peers, 0, sizeof(remembered_peers));"
        self.assertIn(commit, clear)
        self.assertGreater(clear.rindex(runtime_clear), clear.index(commit))


if __name__ == "__main__":
    unittest.main()
