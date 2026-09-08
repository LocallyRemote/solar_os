from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BleSleepDeferTest(unittest.TestCase):
    def test_ble_connect_timeout_keeps_deferred_sleep_bounded(self):
        sdkconfig = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")

        self.assertIn("CONFIG_BT_BLE_ESTAB_LINK_CONN_TOUT=3", sdkconfig)

    def test_active_reconnect_defers_sleep_without_changing_pairing(self):
        source = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        start = source.index(
            "esp_err_t solar_os_ble_keyboard_prepare_sleep(uint32_t timeout_ms)\n{"
        )
        end = source.index(
            "\nbool solar_os_ble_keyboard_sleep_prepare_ready(void)", start
        )
        prepare = source[start:end]

        self.assertIn('stop_reconnect_task("sleep", timeout_ms)', prepare)
        self.assertIn("return ESP_ERR_NOT_FINISHED;", prepare)
        self.assertNotIn("start_pairing", prepare)
        self.assertNotIn("solar_os_ble_keyboard_forget", prepare)

    def test_main_retries_only_after_ble_worker_stops(self):
        source = (ROOT / "src/main.c").read_text(encoding="utf-8")
        start = source.index("static void maybe_enter_deferred_sleep(void)\n{")
        end = source.index("\nstatic void handle_key_short_press(void)", start)
        retry = source[start:end]

        self.assertIn("solar_os_ble_keyboard_sleep_prepare_ready()", retry)
        self.assertIn("enter_light_sleep(reason);", retry)
        self.assertLess(
            retry.index("solar_os_ble_keyboard_sleep_prepare_ready()"),
            retry.index("enter_light_sleep(reason);"),
        )


if __name__ == "__main__":
    unittest.main()
