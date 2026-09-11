from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class BleConfigTest(unittest.TestCase):
    def test_nimble_defaults_enable_incoming_att_handlers(self):
        checked = []
        for path in sorted(ROOT.glob("sdkconfig.defaults*")):
            lines = set(path.read_text(encoding="utf-8").splitlines())
            if "CONFIG_BT_NIMBLE_ENABLED=y" not in lines:
                continue
            with self.subTest(config=path.name):
                # ESP-IDF gates GATT-server support on the peripheral role.
                # Central connections must also answer peer ATT requests.
                self.assertIn("CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y", lines)
                self.assertIn("CONFIG_BT_NIMBLE_GATT_SERVER=y", lines)
                self.assertIn(
                    "# CONFIG_BT_NIMBLE_ROLE_BROADCASTER is not set", lines
                )
            checked.append(path.name)
        self.assertIn("sdkconfig.defaults", checked)


if __name__ == "__main__":
    unittest.main()
