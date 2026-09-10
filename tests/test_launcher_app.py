import ast
import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = (ROOT / "src/apps/solar_os_launcher.c").read_text(encoding="utf-8")
REGISTRY = (ROOT / "src/apps/solar_os_app_registry.c").read_text(encoding="utf-8")
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")


class LauncherAppTest(unittest.TestCase):
    def test_config_describes_grid_items_and_commands(self):
        for field in (
            '\"layout\"',
            '\"columns\"',
            '\"rows\"',
            '\"items\"',
            '\"name\"',
            '\"icon\"',
            '\"command\"',
            '\"column\"',
            '\"row\"',
        ):
            self.assertIn(field, LAUNCHER)
        self.assertIn("solar_os_launcher_layout_valid", LAUNCHER)

    def test_default_config_includes_writer(self):
        self.assertIn(
            '\\"name\\": \\"Writer\\", \\"icon\\": 163, '
            '\\"command\\": \\"writer\\", \\"column\\": 2, \\"row\\": 1',
            LAUNCHER,
        )

    def test_embedded_default_is_valid_json(self):
        start = LAUNCHER.index("static const char launcher_default_config[]")
        end = LAUNCHER.index(";", start)
        literals = re.findall(r'^\s*(".*")$', LAUNCHER[start:end], re.MULTILINE)
        document = "".join(ast.literal_eval(literal) for literal in literals)
        parsed = json.loads(document)
        self.assertEqual(parsed["layout"], {"columns": 3, "rows": 2})
        self.assertEqual(len(parsed["items"]), 6)

    def test_default_config_is_verified_and_atomically_replaced(self):
        self.assertIn("solar_os_storage_sync_file(file)", LAUNCHER)
        self.assertIn("memcmp(verify, launcher_default_config, length)", LAUNCHER)
        self.assertIn("solar_os_storage_replace_file(temporary, path, backup)", LAUNCHER)

    def test_keyboard_pointer_and_child_return_are_wired(self):
        for key in (
            "SOLAR_OS_KEY_LEFT",
            "SOLAR_OS_KEY_RIGHT",
            "SOLAR_OS_KEY_UP",
            "SOLAR_OS_KEY_DOWN",
            "SOLAR_OS_KEY_ENTER",
        ):
            self.assertIn(key, LAUNCHER)
        self.assertIn("SOLAR_OS_EVENT_POINTER", LAUNCHER)
        self.assertIn("solar_os_launcher_layout_hit", LAUNCHER)
        self.assertIn("solar_os_context_request_launch_ex", LAUNCHER)
        self.assertIn("solar_os_shell_run_script", LAUNCHER)
        self.assertIn("solar_os_shell_execute_command", LAUNCHER)
        self.assertIn("SOLAR_OS_LAUNCH_CHILD_RETURN", LAUNCHER)

    def test_launcher_does_not_submit_into_a_suspended_shell_session(self):
        self.assertNotIn("solar_os_shell_session_submit_command", LAUNCHER)
        self.assertNotIn("solar_os_sessions_context_shell_session", LAUNCHER)

    def test_every_item_gets_a_title_when_its_cell_can_show_one(self):
        self.assertIn(
            "const bool show_title = cell_width >= 24 && cell_height >= 24;",
            LAUNCHER,
        )

    def test_launcher_is_registered_as_a_graphics_package(self):
        self.assertIn('APP_ENTRY("launcher"', REGISTRY)
        self.assertIn("SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY", REGISTRY)
        self.assertIn("[packages.app_launcher]", PACKAGES)
        self.assertIn('capabilities = ["gfx"]', PACKAGES)


if __name__ == "__main__":
    unittest.main()
