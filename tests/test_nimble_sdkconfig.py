from pathlib import Path
import subprocess
import tempfile
import unittest


GUARD = Path(__file__).resolve().parents[1] / "patches/nimble/config_guard.cmake"


class NimbleSdkconfigTest(unittest.TestCase):
    def run_guard(self, contents, name="sdkconfig.test", explicit=True):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = root / name
            if contents is not None:
                config.write_text(contents)
            script = root / "guard.cmake"
            script.write_text(
                (f'set(SDKCONFIG "{config.as_posix()}")\n' if explicit else "")
                + f'include("{GUARD.as_posix()}")\n'
                + "solar_os_regenerate_stale_sdkconfig()\n"
                + "solar_os_regenerate_stale_sdkconfig()\n"
            )
            result = subprocess.run(
                ["cmake", "-P", str(script)], cwd=root,
                capture_output=True, text=True,
            )
            remaining = config.read_text() if config.exists() else None
            self.assertEqual(sorted(p.name for p in root.iterdir()),
                             sorted(["guard.cmake"] + ([name] if config.exists() else [])))
            return result, remaining

    def test_stale_host_is_removed_once_with_notice(self):
        result, remaining = self.run_guard(
            "CONFIG_BT_ENABLED=y\nCONFIG_BT_BLUEDROID_ENABLED=y\n"
            "# CONFIG_BT_NIMBLE_ENABLED is not set\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(remaining)
        self.assertEqual(result.stderr.count("regenerating"), 1)

    def test_current_disabled_and_missing_configs_are_unchanged(self):
        for content in (None, "# CONFIG_BT_ENABLED is not set\n",
                        "CONFIG_BT_ENABLED=y\nCONFIG_BT_NIMBLE_ENABLED=y\n"):
            with self.subTest(content=content):
                result, remaining = self.run_guard(content)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(remaining, content)
                self.assertEqual(result.stdout, "")

    def test_default_sdkconfig_path(self):
        result, remaining = self.run_guard("CONFIG_BT_ENABLED=y\n", "sdkconfig", False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIsNone(remaining)

    def test_defaults_are_never_removed(self):
        for name in ("sdkconfig.defaults", "sdkconfig.defaults.board"):
            with self.subTest(name=name):
                content = "CONFIG_BT_ENABLED=y\n"
                result, remaining = self.run_guard(content, name)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(remaining, content)


if __name__ == "__main__":
    unittest.main()
