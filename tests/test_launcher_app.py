from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = (ROOT / "src/apps/solar_os_launcher.c").read_text(encoding="utf-8")
REGISTRY = (ROOT / "src/apps/solar_os_app_registry.c").read_text(encoding="utf-8")
SESSIONS = (ROOT / "src/services/solar_os_sessions.c").read_text(encoding="utf-8")
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
        self.assertIn("solar_os_sessions_context_shell_session", LAUNCHER)
        self.assertIn("solar_os_shell_session_submit_command", LAUNCHER)
        self.assertIn("SOLAR_OS_LAUNCH_CHILD_RETURN", LAUNCHER)

    def test_launcher_resolves_the_shell_behind_its_display_session(self):
        self.assertIn("session_return_shell(current)", SESSIONS)
        self.assertIn("session_uses_same_display(current, candidate)", SESSIONS)

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
